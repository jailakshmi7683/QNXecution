/* monitor.c - watchdog + health monitor + recovery manager + CLI (single process)
 *
 * The monitor OWNS the services: it spawns them in dependency order, keeps the pid
 * spawnl() returned and reaps them itself (instant crash detection, no pid-reuse risk).
 *
 * Per-service state machine (non-blocking, 100 ms tick):
 *   INIT -> VERIFYING -> HEALTHY -> FAULTED -> RESTART_PENDING -> VERIFYING -> ...
 *                                        \-> DEGRADED (retries exhausted / dependency lost)
 *
 * CLI (type in the terminal running the monitor):
 *   help | status | timeline [n|all] | metrics [reset] | inject <svc> <fault> [persist] [ms]
 *   reset <svc|all> | demo | bench <n> | quit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <process.h>
#include <sys/wait.h>
#ifdef __QNXNTO__
#include <sched.h>

#endif
#include "common.h"
#include "fault.h"

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */
#define TICK_MS           100
#define CORRELATION_MS    2000   /* STUCK waits this long for a culprit upstream to be flagged */
#define ANCESTOR_QUIET_MS 1000   /* upstream silent this long may be about to be flagged too   */
#define STABLE_MS         20000  /* continuous health before the retry budget resets            */
#define KILL_WAIT_MS      1500
#define SETTLE_MS         500    /* upstream must be up this long before dependents spawn       */
#define MONITOR_PRIO      40     /* monitor above services (best effort, QNX only)              */
#define DEFAULT_BIN_DIR "$HOME/ecu"

typedef struct {
    int      tier;         /* 1 = safety-critical ... 3 = non-critical                     */
    uint32_t stall_ms;     /* no progress for this long => STUCK                           */
    int      max_retries;
    uint32_t verify_ms;    /* after spawn, first progress must arrive within this          */
    uint32_t budget_ms;    /* recovery-time budget, reported MET / MISSED                  */
} Policy;

/* SAME ORDER as dependency_graph[] in common.h */
static const Policy policy[NUM_SERVICES] = {
    /* wheel_speed      */ { 1, 5000, 3, 8000, 3000 },
    /* abs              */ { 1, 5000, 3, 8000, 3000 },
    /* traction_control */ { 2, 5000, 3, 8000, 5000 },
    /* dashboard        */ { 3, 5000, 2, 8000, 8000 },
};
_Static_assert(sizeof(policy) / sizeof(policy[0]) == NUM_SERVICES,
               "policy[] must have one entry per service in dependency_graph[]");

static const uint32_t backoff_table_ms[] = { 100, 500, 1500, 3000 };

/* ------------------------------------------------------------------ */
/* Types and state                                                     */
/* ------------------------------------------------------------------ */
typedef enum { ST_INIT, ST_VERIFYING, ST_HEALTHY, ST_FAULTED,
               ST_RESTART_PENDING, ST_DEGRADED } SvcState;
static const char *state_name[] = { "INIT", "VERIFYING", "HEALTHY", "FAULTED",
                                    "RESTART_PENDING", "DEGRADED" };
typedef enum { F_NONE = 0, F_DEAD, F_STUCK } FaultType;

typedef struct {
    SvcState  state;
    FaultType fault;
    pid_t     pid;               /* 0 = no live child */
    uint64_t *counter;
    uint64_t  last_value;
    uint64_t  last_change_ms;
    uint64_t  fault_ms;
    uint64_t  incident_ms;       /* first detection of this incident (0 = none) */
    uint64_t  spawn_ms;
    uint64_t  restart_at_ms;
    uint64_t  kill_sent_ms;
    uint64_t  healthy_since_ms;
    int       attempts;
    int       symptom_logged;
    int       inc_ref;           /* 1 + index into incidents[], 0 = none */
} Svc;

static Svc  svc[NUM_SERVICES];
static int  order[NUM_SERVICES];
static const char *g_bin_dir = DEFAULT_BIN_DIR;
static char g_svc_log[160];
static char g_csv_path[160];
static volatile sig_atomic_t g_stop = 0;
static uint64_t g_t0_ms;

/* ---- event ring + CSV ---- */
#define RING_N 300
typedef struct { uint64_t t_ms; char event[24]; char text[200]; } Ev;
static Ev   ring[RING_N];
static int  ring_next = 0, ring_count = 0;
static FILE *g_csv = NULL;

/* ---- incidents (for metrics) ---- */
#define MAX_INC 512
enum { OUT_OPEN = 0, OUT_RECOVERED, OUT_DEGRADED, OUT_IGNORED };
typedef struct {
    int      svc;
    FaultType ft;
    long     detect_lat_ms;      /* injection activation -> detection, -1 unknown */
    uint64_t recovery_ms;        /* detection -> verified progress */
    int      attempts;
    int      outcome;
    int      met;
} Incident;
static Incident incidents[MAX_INC];
static int n_inc = 0;

/* ---- scenarios (demo / bench) ---- */
typedef enum { SC_INJECT, SC_RESET_ALL, SC_METRICS } ScKind;
typedef struct { ScKind kind; int svc; uint32_t fault; uint32_t persist; } ScStep;
#define SC_MAX 130
static ScStep   sc[SC_MAX];
static int      sc_len = 0, sc_pos = 0, sc_phase = 0, sc_bench = 0;
static uint64_t sc_t = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *sname(int i) { return dependency_graph[i].name; }
static const char *fault_str(FaultType t) { return t == F_DEAD ? "DEAD" : "STUCK"; }

static void mlog(const char *event, const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    uint64_t t = now_ms() - g_t0_ms;
    Ev *e = &ring[ring_next];
    e->t_ms = t;
    snprintf(e->event, sizeof(e->event), "%s", event);
    snprintf(e->text, sizeof(e->text), "%s", buf);
    ring_next = (ring_next + 1) % RING_N;
    if (ring_count < RING_N) ring_count++;

    if (g_csv) {
        char q[200];
        snprintf(q, sizeof(q), "%s", buf);
        for (char *c = q; *c; c++) if (*c == '"' || *c == ',') *c = ';';
        fprintf(g_csv, "%llu,%s,%s\n", (unsigned long long)t, event, q);
        fflush(g_csv);
    }
    LOG_EVENT("monitor", event, buf);
}

static uint32_t backoff_ms(int attempts_done) {
    int n = (int)(sizeof(backoff_table_ms) / sizeof(backoff_table_ms[0]));
    if (attempts_done < 0) attempts_done = 0;
    return backoff_table_ms[attempts_done < n ? attempts_done : n - 1];
}

static void set_state(int i, SvcState ns) {
    if (svc[i].state == ns) return;
    mlog("STATE", "%s: %s -> %s", sname(i), state_name[svc[i].state], state_name[ns]);
    svc[i].state = ns;
}

static void service_path(int i, char *out, size_t n) {
    size_t len = strlen(g_bin_dir);
    snprintf(out, n, "%s%s%s", g_bin_dir, (len && g_bin_dir[len - 1] == '/') ? "" : "/", sname(i));
}

static int find_by_pid(pid_t p) {
    for (int i = 0; i < NUM_SERVICES; i++) if (svc[i].pid == p) return i;
    return -1;
}

static int service_index(const char *name) {
    for (int i = 0; i < NUM_SERVICES; i++)
        if (strcmp(dependency_graph[i].name, name) == 0) return i;
    return -1;
}

/* ---- progress counter (shared memory) ---- */
static void close_counter(Svc *s) {
    if (s->counter) { munmap(s->counter, sizeof(uint64_t)); s->counter = NULL; }
}

static int try_open_counter(int i, uint64_t now) {
    Svc *s = &svc[i];
    if (s->counter) return 1;
    char nm[64];
    snprintf(nm, sizeof(nm), "%s%s", COUNTER_SHM_PREFIX, sname(i));
    int fd = shm_open(nm, O_RDONLY, 0666);
    if (fd == -1) return 0;
    struct stat st;                                  /* service may not have sized it yet */
    if (fstat(fd, &st) == -1 || st.st_size < (off_t)sizeof(uint64_t)) { close(fd); return 0; }
    void *p = mmap(NULL, sizeof(uint64_t), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return 0;
    s->counter = (uint64_t *)p;
    s->last_value = *s->counter;
    s->last_change_ms = now;
    return 1;
}

/* ---- dependency graph ---- */
static int is_ancestor(int anc, int desc, int depth) {
    if (depth > NUM_SERVICES) return 0;
    for (int k = 0; k < MAX_DEPS; k++) {
        const char *dep = dependency_graph[desc].depends_on[k];
        if (dep[0] == '\0') break;
        int p = service_index(dep);
        if (p < 0) continue;
        if (p == anc) return 1;
        if (is_ancestor(anc, p, depth + 1)) return 1;
    }
    return 0;
}

static int unhealthy_ancestor(int i) {
    for (int j = 0; j < NUM_SERVICES; j++)
        if (j != i && is_ancestor(j, i, 0) && svc[j].state != ST_HEALTHY) return j;
    return -1;
}

static int deps_ready(int i, uint64_t now) {
    for (int k = 0; k < MAX_DEPS; k++) {
        const char *dep = dependency_graph[i].depends_on[k];
        if (dep[0] == '\0') break;
        int p = service_index(dep);
        if (p < 0) continue;
        const Svc *d = &svc[p];
        if (d->state == ST_HEALTHY) continue;
        if (d->state == ST_VERIFYING && d->pid > 0 && now - d->spawn_ms >= SETTLE_MS) continue;
        return 0;
    }
    return 1;
}

static int ancestor_may_still_fault(int i, uint64_t now) {
    for (int j = 0; j < NUM_SERVICES; j++)
        if (j != i && is_ancestor(j, i, 0) && svc[j].state == ST_HEALTHY &&
            now - svc[j].last_change_ms >= ANCESTOR_QUIET_MS) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Incident records                                                    */
/* ------------------------------------------------------------------ */
static void incident_open(int i, FaultType ft, uint64_t now) {
    if (n_inc >= MAX_INC) return;
    Incident *r = &incidents[n_inc];
    memset(r, 0, sizeof(*r));
    r->svc = i;
    r->ft = ft;
    r->detect_lat_ms = fault_take_latency(sname(i), now);
    r->outcome = OUT_OPEN;
    svc[i].inc_ref = ++n_inc;
}

static void incident_close(int i, int outcome, uint64_t recovery_ms, int met) {
    Svc *s = &svc[i];
    if (s->inc_ref > 0) {
        Incident *r = &incidents[s->inc_ref - 1];
        r->outcome = outcome;
        r->recovery_ms = recovery_ms;
        r->attempts = s->attempts;
        r->met = met;
    }
    s->inc_ref = 0;
}

/* ------------------------------------------------------------------ */
/* Fault bookkeeping and recovery actions                              */
/* ------------------------------------------------------------------ */
static void mark_fault(int i, FaultType ft, uint64_t now, const char *detail) {
    Svc *s = &svc[i];
    s->fault = ft;
    s->fault_ms = now;
    s->symptom_logged = 0;
    if (!s->incident_ms) { s->incident_ms = now; incident_open(i, ft, now); }
    set_state(i, ST_FAULTED);
    mlog("FAULT_DETECTED", "%s is %s (%s)", sname(i), fault_str(ft), detail);
}

static void enter_safe_state(int i) {
    /* Hook point: drive GPIO LED / send UART health frame here later. */
    switch (policy[i].tier) {
    case 1:  mlog("SAFE_STATE", "%s: SAFETY-CRITICAL service lost - driver warning raised, limp mode", sname(i)); break;
    case 2:  mlog("SAFE_STATE", "%s: function disabled - reduced vehicle capability", sname(i)); break;
    default: mlog("SAFE_STATE", "%s: non-critical service disabled - vehicle operation unaffected", sname(i)); break;
    }
}

static void degrade(int i, const char *why) {
    Svc *s = &svc[i];
    if (s->pid > 0) kill(s->pid, SIGKILL);
    incident_close(i, OUT_DEGRADED, 0, 0);
    set_state(i, ST_DEGRADED);
    mlog("ESCALATE", "%s: %s after %d attempt(s) - no further restarts", sname(i), why, s->attempts);
    enter_safe_state(i);
    for (int k = 0; k < NUM_SERVICES; k++) {
        if (k != i && is_ancestor(i, k, 0) && svc[k].state != ST_DEGRADED) {
            incident_close(k, OUT_IGNORED, 0, 0);
            svc[k].incident_ms = 0;
            set_state(k, ST_DEGRADED);
            mlog("DEPENDENCY_LOST", "%s: unavailable, upstream %s is DEGRADED", sname(k), sname(i));
        }
    }
}

static void recovered(int i, uint64_t now) {
    Svc *s = &svc[i];
    if (s->incident_ms) {
        uint64_t total = now - s->incident_ms;
        int met = total <= policy[i].budget_ms;
        mlog("RECOVERED",
             "%s healthy again: recovery_time=%llu ms, spawn_to_progress=%llu ms, attempts=%d/%d, budget=%u ms: %s",
             sname(i), (unsigned long long)total, (unsigned long long)(now - s->spawn_ms),
             s->attempts, policy[i].max_retries, (unsigned)policy[i].budget_ms, met ? "MET" : "MISSED");
        incident_close(i, OUT_RECOVERED, total, met);
    } else {
        mlog("READY", "%s up: first progress after %llu ms", sname(i), (unsigned long long)(now - s->spawn_ms));
    }
    s->fault = F_NONE;
    s->incident_ms = 0;
    s->symptom_logged = 0;
    s->healthy_since_ms = now;
    s->last_value = s->counter ? *s->counter : 0;
    s->last_change_ms = now;
    set_state(i, ST_HEALTHY);
    /* attempts reset only after STABLE_MS of continuous health (flap guard) */
}

/* Spawn with stdin=/dev/null and stdout/stderr -> services.log, so the terminal stays
   free for the CLI. Falls back to inheriting the terminal if the log cannot be opened. */
static pid_t spawn_redirected(const char *path) {
    int saved[3] = { -1, -1, -1 };
    int in  = open("/dev/null", O_RDONLY);
    int out = open(g_svc_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    int redirect = (in >= 0 && out >= 0);
    if (redirect) {
        for (int k = 0; k < 3; k++) {
            saved[k] = dup(k);
            if (saved[k] >= 0) fcntl(saved[k], F_SETFD, FD_CLOEXEC);
        }
        dup2(in, 0); dup2(out, 1); dup2(out, 2);
    }
    if (in >= 0)  close(in);
    if (out >= 0) close(out);

    pid_t p = spawnl(P_NOWAIT, path, path, (char *)NULL);

    if (redirect) {
        for (int k = 0; k < 3; k++)
            if (saved[k] >= 0) { dup2(saved[k], k); close(saved[k]); }
    }
    return p;
}

static int spawn_service(int i, uint64_t now) {
    Svc *s = &svc[i];
    char path[160], nm[64];
    service_path(i, path, sizeof(path));

    close_counter(s);                                  /* clean slate: no stale counter */
    snprintf(nm, sizeof(nm), "%s%s", COUNTER_SHM_PREFIX, sname(i));
    shm_unlink(nm);
    snprintf(nm, sizeof(nm), "/tmp/%s.pid", sname(i));
    unlink(nm);

    pid_t p = spawn_redirected(path);
    if (p == -1) {
        mlog("SPAWN_FAILED", "%s: cannot spawn %s (%s)", sname(i), path, strerror(errno));
        return -1;
    }
    s->pid = p;
    s->spawn_ms = now;
    s->last_value = 0;
    s->last_change_ms = now;
    mlog("SPAWNED", "%s started (pid=%d)", sname(i), (int)p);
    return 0;
}

static void start_or_fail(int i, uint64_t now) {
    Svc *s = &svc[i];
    if (spawn_service(i, now) == 0) { set_state(i, ST_VERIFYING); return; }
    if (s->state == ST_INIT) s->attempts++;
    if (s->attempts >= policy[i].max_retries) degrade(i, "cannot be spawned");
    else s->restart_at_ms = now + backoff_ms(s->attempts);
}

/* ------------------------------------------------------------------ */
/* Reaping (instant crash detection) and detection                     */
/* ------------------------------------------------------------------ */
static void reap_children(uint64_t now) {
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        int i = find_by_pid(p);
        if (i < 0) continue;
        Svc *s = &svc[i];
        char how[48];
        if (WIFSIGNALED(status)) snprintf(how, sizeof(how), "killed by signal %d", WTERMSIG(status));
        else                     snprintf(how, sizeof(how), "exited with code %d", WEXITSTATUS(status));
        s->pid = 0;

        switch (s->state) {
        case ST_HEALTHY:
        case ST_VERIFYING:
            mark_fault(i, F_DEAD, now, how);
            break;
        case ST_FAULTED:
            if (s->fault == F_STUCK) {
                s->fault = F_DEAD;
                mlog("FAULT_UPDATE", "%s: now DEAD (%s)", sname(i), how);
            }
            break;
        default:
            break;
        }
    }
}

static void detect(int i, uint64_t now) {
    Svc *s = &svc[i];
    const Policy *p = &policy[i];
    char detail[96];

    switch (s->state) {

    case ST_HEALTHY: {
        uint64_t cur = s->last_value;
        if (try_open_counter(i, now)) cur = *s->counter;
        if (cur != s->last_value) { s->last_value = cur; s->last_change_ms = now; }

        int up = unhealthy_ancestor(i);          /* upstream down: pause the stall clock */
        if (up >= 0) {
            s->last_change_ms = now;
            if (!s->symptom_logged) {
                mlog("SYMPTOM", "%s depends on %s (%s) - monitoring suspended, not restarting",
                     sname(i), sname(up), state_name[svc[up].state]);
                s->symptom_logged = 1;
            }
            break;
        }
        s->symptom_logged = 0;

        if (now - s->last_change_ms >= p->stall_ms) {
            snprintf(detail, sizeof(detail), "no progress for %llu ms, counter=%llu",
                     (unsigned long long)(now - s->last_change_ms), (unsigned long long)cur);
            mark_fault(i, F_STUCK, now, detail);
        } else if (s->attempts > 0 && now - s->healthy_since_ms >= STABLE_MS) {
            mlog("STABLE", "%s stable for %d s - retry budget reset", sname(i), STABLE_MS / 1000);
            s->attempts = 0;
        }
        break;
    }

    case ST_VERIFYING:
        if (try_open_counter(i, now) && *s->counter > 0) {
            recovered(i, now);
        } else if (now - s->spawn_ms >= p->verify_ms) {
            snprintf(detail, sizeof(detail), "no progress within %u ms of spawn", (unsigned)p->verify_ms);
            mark_fault(i, s->pid > 0 ? F_STUCK : F_DEAD, now, detail);
            s->fault_ms = now - CORRELATION_MS;      /* already waited: act immediately */
        }
        break;

    case ST_FAULTED:
        if (s->fault == F_STUCK && try_open_counter(i, now) && *s->counter != s->last_value) {
            s->last_value = *s->counter;             /* started moving again by itself */
            s->last_change_ms = now;
            s->fault = F_NONE;
            incident_close(i, OUT_IGNORED, 0, 0);
            s->incident_ms = 0;
            s->symptom_logged = 0;
            s->healthy_since_ms = now;
            mlog("PROGRESS_RESUMED", "%s progress resumed on its own (counter=%llu)",
                 sname(i), (unsigned long long)s->last_value);
            set_state(i, ST_HEALTHY);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Root-cause isolation + recovery decision                            */
/* ------------------------------------------------------------------ */
static void isolate_and_recover(uint64_t now) {
    for (int k = 0; k < NUM_SERVICES; k++) {          /* most critical tier first */
        int i = order[k];
        Svc *s = &svc[i];
        if (s->state != ST_FAULTED) continue;

        if (s->fault == F_STUCK) {
            int up = unhealthy_ancestor(i);
            if (up < 0 && now - s->fault_ms < CORRELATION_MS && ancestor_may_still_fault(i, now))
                continue;
            if (up >= 0) {
                if (!s->symptom_logged) {
                    mlog("SYMPTOM", "%s is a SYMPTOM of upstream %s (%s) - not restarting",
                         sname(i), sname(up), state_name[svc[up].state]);
                    s->symptom_logged = 1;
                }
                continue;
            }
            if (s->symptom_logged) {                  /* upstream recovered meanwhile */
                s->fault = F_NONE;
                s->symptom_logged = 0;
                s->last_change_ms = now;
                incident_close(i, OUT_IGNORED, 0, 0);
                s->incident_ms = 0;
                set_state(i, ST_HEALTHY);
                mlog("SYMPTOM_CLEARED", "%s: upstream recovered, monitoring resumed", sname(i));
                continue;
            }
        }

        if (s->attempts >= policy[i].max_retries) { degrade(i, "recovery failed"); continue; }

        mlog("ROOT_CAUSE", "%s is ROOT CAUSE (%s) - restart scheduled (attempt %d/%d)",
             sname(i), fault_str(s->fault), s->attempts + 1, policy[i].max_retries);
        if (s->pid > 0) { kill(s->pid, SIGKILL); s->kill_sent_ms = now; }
        s->restart_at_ms = now + backoff_ms(s->attempts);
        set_state(i, ST_RESTART_PENDING);
    }
}

static void drive_spawns(uint64_t now) {
    for (int k = 0; k < NUM_SERVICES; k++) {
        int i = order[k];
        Svc *s = &svc[i];

        if (s->state == ST_RESTART_PENDING) {
            if (s->pid > 0) {
                if (now - s->kill_sent_ms >= KILL_WAIT_MS) {
                    kill(s->pid, SIGKILL);
                    s->kill_sent_ms = now;
                    mlog("KILL_RETRY", "%s: pid %d still alive, re-sending SIGKILL", sname(i), (int)s->pid);
                }
                continue;
            }
            if (now < s->restart_at_ms || !deps_ready(i, now)) continue;
            s->attempts++;
            mlog("RESTARTING", "%s: restart attempt %d/%d", sname(i), s->attempts, policy[i].max_retries);
            start_or_fail(i, now);
        } else if (s->state == ST_INIT) {
            if (now < s->restart_at_ms || !deps_ready(i, now)) continue;
            start_or_fail(i, now);
        }
    }
}

/* Operator re-arm of a DEGRADED service (CLI "reset"). */
static void reset_service(int i, uint64_t now) {
    Svc *s = &svc[i];
    fault_send(sname(i), INJ_NONE, 0, 0);            /* cancel any persistent injected fault */
    if (s->pid > 0) { kill(s->pid, SIGKILL); s->kill_sent_ms = now; }
    incident_close(i, OUT_IGNORED, 0, 0);
    s->attempts = 0;
    s->fault = F_NONE;
    s->incident_ms = 0;
    s->symptom_logged = 0;
    s->restart_at_ms = now;
    mlog("RESET", "%s re-armed by operator", sname(i));
    set_state(i, ST_RESTART_PENDING);
}

/* ------------------------------------------------------------------ */
/* CLI: status / timeline / metrics                                    */
/* ------------------------------------------------------------------ */
static void cmd_status(uint64_t now) {
    printf("\n%-17s %-5s %-16s %-8s %-14s %s\n",
           "SERVICE", "TIER", "STATE", "PID", "LAST_PROGRESS", "RETRIES");
    for (int i = 0; i < NUM_SERVICES; i++) {
        Svc *s = &svc[i];
        char age[24] = "-";
        if (s->state == ST_HEALTHY) snprintf(age, sizeof(age), "%llu ms ago",
                                             (unsigned long long)(now - s->last_change_ms));
        printf("%-17s %-5d %-16s %-8d %-14s %d/%d\n", sname(i), policy[i].tier,
               state_name[s->state], (int)s->pid, age, s->attempts, policy[i].max_retries);
    }
    printf("\n");
}

static void cmd_timeline(int n, int all) {
    if (n <= 0) n = 20;
    int idx[RING_N], m = 0;
    for (int k = 0; k < ring_count; k++) {
        int p = (ring_next - ring_count + k + RING_N) % RING_N;
        if (!all && strcmp(ring[p].event, "STATE") == 0) continue;
        idx[m++] = p;
    }
    int start = m > n ? m - n : 0;
    printf("\n  T+sec    EVENT              DETAIL\n");
    for (int k = start; k < m; k++) {
        Ev *e = &ring[idx[k]];
        printf("%8.3f   %-18s %s\n", e->t_ms / 1000.0, e->event, e->text);
    }
    printf("\n");
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void print_metrics(void) {
    printf("\n=== RECOVERY METRICS (root-cause incidents only) ===\n");
    printf("%-6s %3s %3s %4s | recovery ms: %6s %6s %6s %6s | %-9s | detect avg | outage avg\n",
           "FAULT", "N", "OK", "DEGR", "min", "avg", "p95", "max", "budget");
    for (int ft = F_DEAD; ft <= F_STUCK; ft++) {
        uint64_t rec[MAX_INC];
        int ok = 0, deg = 0, met = 0, dn = 0, on = 0;
        double dsum = 0, osum = 0;
        for (int k = 0; k < n_inc; k++) {
            Incident *r = &incidents[k];
            if ((int)r->ft != ft) continue;
            if (r->outcome == OUT_RECOVERED) {
                rec[ok++] = r->recovery_ms;
                met += r->met;
                if (r->detect_lat_ms >= 0) {
                    dsum += r->detect_lat_ms; dn++;
                    osum += r->detect_lat_ms + (double)r->recovery_ms; on++;
                }
            } else if (r->outcome == OUT_DEGRADED) deg++;
        }
        if (ok + deg == 0) { printf("%-6s %3d\n", fault_str((FaultType)ft), 0); continue; }
        qsort(rec, ok, sizeof(rec[0]), cmp_u64);
        double avg = 0;
        for (int k = 0; k < ok; k++) avg += (double)rec[k];
        if (ok) avg /= ok;
        int p95 = ok ? (ok * 95 + 99) / 100 - 1 : 0;
        char det[24] = "n/a", out[24] = "n/a", bud[32] = "n/a";
        if (dn) snprintf(det, sizeof(det), "%.0f ms", dsum / dn);
        if (on) snprintf(out, sizeof(out), "%.0f ms", osum / on);
        if (ok) snprintf(bud, sizeof(bud), "%d/%d met", met, ok);
        printf("%-6s %3d %3d %4d | %-12s %6llu %6.0f %6llu %6llu | %-9s | %-10s | %s\n",
               fault_str((FaultType)ft), ok + deg, ok, deg, "",
               ok ? (unsigned long long)rec[0] : 0ULL, avg,
               ok ? (unsigned long long)rec[p95] : 0ULL,
               ok ? (unsigned long long)rec[ok - 1] : 0ULL, bud, det, out);
    }
    printf("(detect = fault activation -> detection; outage = detect + recovery; log: %s)\n\n",
           g_csv_path);
}

/* ------------------------------------------------------------------ */
/* CLI: injection and scenarios                                        */
/* ------------------------------------------------------------------ */
static int parse_fault(const char *w, uint32_t *cmd) {
    if      (!strcmp(w, "crash"))    *cmd = INJ_CRASH;
    else if (!strcmp(w, "stuck"))    *cmd = INJ_STUCK;
    else if (!strcmp(w, "overrun"))  *cmd = INJ_OVERRUN;
    else if (!strcmp(w, "deadlock")) *cmd = INJ_DEADLOCK;
    else if (!strcmp(w, "clear"))    *cmd = INJ_NONE;
    else return -1;
    return 0;
}

static const char *inj_name(uint32_t c) {
    switch (c) { case INJ_CRASH: return "crash"; case INJ_STUCK: return "stuck";
                 case INJ_OVERRUN: return "overrun"; case INJ_DEADLOCK: return "deadlock";
                 default: return "clear"; }
}

static int do_inject(int i, uint32_t cmd, uint32_t persist, uint32_t arg_ms) {
    if (fault_send(sname(i), cmd, persist, arg_ms) != 0) return -1;
    mlog("INJECT", "%s <- %s%s", sname(i), inj_name(cmd), persist ? " (persistent)" : "");
    return 0;
}

static int all_settled(void) {
    for (int i = 0; i < NUM_SERVICES; i++)
        if (svc[i].state != ST_HEALTHY && svc[i].state != ST_DEGRADED) return 0;
    return 1;
}

static void sc_add(ScKind k, int s, uint32_t f, uint32_t persist) {
    if (sc_len >= SC_MAX) return;
    sc[sc_len].kind = k; sc[sc_len].svc = s; sc[sc_len].fault = f; sc[sc_len].persist = persist;
    sc_len++;
}

static void scenario_tick(uint64_t now) {
    if (sc_len == 0) return;
    if (sc_pos >= sc_len) { mlog("SCENARIO", "finished (%d steps)", sc_len); sc_len = sc_pos = sc_phase = 0; return; }

    if (sc_phase == 0) {                               /* ready to fire the next step */
        if (now < sc_t || !all_settled()) return;
        ScStep *st = &sc[sc_pos];
        if (st->kind == SC_RESET_ALL) {
            for (int i = 0; i < NUM_SERVICES; i++) if (svc[i].state == ST_DEGRADED) reset_service(i, now);
            sc_pos++; sc_t = now + 1500; return;
        }
        if (st->kind == SC_METRICS) { print_metrics(); sc_pos++; return; }
        if (sc_bench) for (int i = 0; i < NUM_SERVICES; i++) svc[i].attempts = 0;  /* bench: no flap guard */
        if (svc[st->svc].state == ST_DEGRADED) { sc_pos++; return; }
        mlog("SCENARIO", "step %d/%d: inject %s into %s%s", sc_pos + 1, sc_len,
             inj_name(st->fault), sname(st->svc), st->persist ? " (persistent)" : "");
        do_inject(st->svc, st->fault, st->persist, 6000);
        sc_phase = 1;
        sc_t = now + 25000;                            /* give up waiting for a reaction */
    } else if (sc_phase == 1) {                        /* wait for the system to react */
        int disturbed = 0;
        for (int i = 0; i < NUM_SERVICES; i++) if (svc[i].state != ST_HEALTHY) disturbed = 1;
        if (disturbed || now >= sc_t) sc_phase = 2;
    } else {                                           /* wait until everything settled */
        if (all_settled()) { sc_pos++; sc_phase = 0; sc_t = now + 1500; }
    }
}

static void start_demo(void) {
    sc_len = sc_pos = sc_phase = 0; sc_bench = 0; sc_t = 0;
    sc_add(SC_INJECT, service_index("wheel_speed"),      INJ_CRASH,    0);
    sc_add(SC_INJECT, service_index("abs"),              INJ_STUCK,    0);
    sc_add(SC_INJECT, service_index("traction_control"), INJ_DEADLOCK, 0);
    sc_add(SC_INJECT, service_index("traction_control"), INJ_CRASH,    1);   /* never recovers */
    sc_add(SC_RESET_ALL, 0, 0, 0);
    sc_add(SC_METRICS, 0, 0, 0);
    mlog("SCENARIO", "demo started (%d steps)", sc_len);
}

static void start_bench(int n) {
    static const uint32_t kinds[] = { INJ_CRASH, INJ_STUCK, INJ_DEADLOCK };
    sc_len = sc_pos = sc_phase = 0; sc_bench = 1; sc_t = 0;
    if (n > SC_MAX - 2) n = SC_MAX - 2;
    for (int k = 0; k < n; k++)
        sc_add(SC_INJECT, rand() % NUM_SERVICES, kinds[rand() % 3], 0);
    sc_add(SC_METRICS, 0, 0, 0);
    mlog("SCENARIO", "bench started: %d random faults (hangs take ~5-8 s each)", n);
}

static void cmd_help(void) {
    printf("\n  status                         service states\n"
           "  timeline [n|all]               recent events (default 20)\n"
           "  metrics [reset]                detection / recovery statistics\n"
           "  inject <svc> <fault> [persist] [ms]\n"
           "                                 fault = crash|stuck|overrun|deadlock|clear\n"
           "  reset <svc|all>                re-arm DEGRADED services\n"
           "  demo                           scripted fault sequence\n"
           "  bench <n>                      n random faults, then metrics\n"
           "  quit\n\n");
}

static void handle_line(char *line, uint64_t now) {
    char t[5][64] = { "", "", "", "", "" };
    int n = sscanf(line, "%63s %63s %63s %63s %63s", t[0], t[1], t[2], t[3], t[4]);
    if (n <= 0) return;

    if (!strcmp(t[0], "help") || !strcmp(t[0], "?")) cmd_help();
    else if (!strcmp(t[0], "status")) cmd_status(now);
    else if (!strcmp(t[0], "timeline")) cmd_timeline(atoi(t[1]), !strcmp(t[1], "all"));
    else if (!strcmp(t[0], "metrics")) {
        if (!strcmp(t[1], "reset")) {
            n_inc = 0;
            for (int i = 0; i < NUM_SERVICES; i++) svc[i].inc_ref = 0;
            printf("metrics cleared\n");
        } else print_metrics();
    }
    else if (!strcmp(t[0], "inject")) {
        uint32_t cmd; int i = service_index(t[1]);
        if (n < 3 || i < 0 || parse_fault(t[2], &cmd) != 0) {
            printf("usage: inject <wheel_speed|abs|traction_control|dashboard> <crash|stuck|overrun|deadlock|clear> [persist] [ms]\n");
            return;
        }
        uint32_t persist = 0, arg = 6000;
        for (int k = 3; k < n; k++) { if (!strcmp(t[k], "persist")) persist = 1; else arg = (uint32_t)atoi(t[k]); }
        if (do_inject(i, cmd, persist, arg) != 0) printf("injection failed\n");
    }
    else if (!strcmp(t[0], "reset")) {
        if (!strcmp(t[1], "all")) {
            for (int i = 0; i < NUM_SERVICES; i++) if (svc[i].state == ST_DEGRADED) reset_service(i, now);
        } else {
            int i = service_index(t[1]);
            if (i < 0) printf("usage: reset <service|all>\n");
            else if (svc[i].state != ST_DEGRADED) printf("%s is not DEGRADED\n", sname(i));
            else reset_service(i, now);
        }
    }
    else if (!strcmp(t[0], "demo")) { if (sc_len) printf("a scenario is already running\n"); else start_demo(); }
    else if (!strcmp(t[0], "bench")) {
        int cnt = atoi(t[1]);
        if (sc_len) printf("a scenario is already running\n");
        else if (cnt <= 0) printf("usage: bench <n>\n");
        else start_bench(cnt);
    }
    else if (!strcmp(t[0], "quit") || !strcmp(t[0], "exit")) g_stop = 1;
    else printf("unknown command '%s' (try: help)\n", t[0]);
}

/* ------------------------------------------------------------------ */
/* Startup / shutdown                                                  */
/* ------------------------------------------------------------------ */
static void on_sigchld(int sig) { (void)sig; }
static void on_stop(int sig)    { (void)sig; g_stop = 1; }

static void build_order(void) {                        /* stable sort by tier */
    for (int i = 0; i < NUM_SERVICES; i++) order[i] = i;
    for (int a = 1; a < NUM_SERVICES; a++) {
        int key = order[a], b = a - 1;
        while (b >= 0 && policy[order[b]].tier > policy[key].tier) { order[b + 1] = order[b]; b--; }
        order[b + 1] = key;
    }
}

static void unlink_shared_objects(void) {
    for (int i = 0; i < NUM_SERVICES; i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "%s%s", COUNTER_SHM_PREFIX, sname(i)); shm_unlink(nm);
        snprintf(nm, sizeof(nm), "%s%s", INJECT_SHM_PREFIX, sname(i));  shm_unlink(nm);
    }
}

static void shutdown_all(void) {
    mlog("SHUTDOWN", "stopping all services");
    for (int i = 0; i < NUM_SERVICES; i++) if (svc[i].pid > 0) kill(svc[i].pid, SIGKILL);
    while (waitpid(-1, NULL, 0) > 0) { }
    for (int i = 0; i < NUM_SERVICES; i++) close_counter(&svc[i]);
    unlink_shared_objects();
    if (g_csv) fclose(g_csv);
}

int main(void) {
    const char *d = getenv("ECU_BIN_DIR");
    if (d && *d) g_bin_dir = d;
    g_t0_ms = now_ms();
    srand((unsigned)g_t0_ms);
    signal(SIGPIPE, SIG_IGN);   /* GUI link may drop; do not die on a closed stdout */

    size_t len = strlen(g_bin_dir);
    const char *sep = (len && g_bin_dir[len - 1] == '/') ? "" : "/";
    snprintf(g_svc_log, sizeof(g_svc_log), "%s%sservices.log", g_bin_dir, sep);
    snprintf(g_csv_path, sizeof(g_csv_path), "%s%sevents.csv", g_bin_dir, sep);
    g_csv = fopen(g_csv_path, "w");
    if (g_csv) {
        fcntl(fileno(g_csv), F_SETFD, FD_CLOEXEC);      /* children must not inherit it */
        fprintf(g_csv, "t_ms,event,detail\n");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sigchld;                          /* no SA_RESTART: poll() is interrupted */
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    build_order();
    unlink_shared_objects();                             /* drop leftovers from a previous run */


#ifdef __QNXNTO__
    struct sched_param monitor_param;
    monitor_param.sched_priority = MONITOR_PRIO;

    if (sched_setparam(0, &monitor_param) == -1) {
        mlog("PRIORITY", "sched_setparam failed: %s", strerror(errno));
    } else {
        mlog("PRIORITY", "monitor priority set to %d", MONITOR_PRIO);
    }
#endif


    for (int i = 0; i < NUM_SERVICES; i++) {
        char path[160];
        service_path(i, path, sizeof(path));
        if (access(path, X_OK) != 0)
            mlog("WARNING", "%s: binary not found or not executable: %s", sname(i), path);
    }
    mlog("STARTED", "bin_dir=%s, tick=%d ms, service output -> %s", g_bin_dir, TICK_MS, g_svc_log);
    printf("type 'help' for commands\n");

    int  cli_open = 1;
    char inbuf[256];
    int  inlen = 0;

    while (!g_stop) {
        uint64_t now = now_ms();

        reap_children(now);
        for (int i = 0; i < NUM_SERVICES; i++) detect(i, now);
        isolate_and_recover(now);
        drive_spawns(now);
        scenario_tick(now);

        struct pollfd pfd = { 0, POLLIN, 0 };
        int r = poll(&pfd, cli_open ? 1 : 0, TICK_MS);   /* doubles as the tick sleep */
        if (r > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            ssize_t got = read(0, inbuf + inlen, sizeof(inbuf) - 1 - inlen);
            if (got <= 0) { cli_open = 0; continue; }    /* stdin closed: run headless */
            inlen += (int)got;
            inbuf[inlen] = '\0';
            char *nl;
            while ((nl = strchr(inbuf, '\n')) != NULL) {
                *nl = '\0';
                handle_line(inbuf, now_ms());
                int rest = inlen - (int)(nl - inbuf) - 1;
                memmove(inbuf, nl + 1, rest + 1);
                inlen = rest;
            }
            if (inlen >= (int)sizeof(inbuf) - 1) inlen = 0;   /* overlong line: drop it */
            fflush(stdout);
        }
    }

    shutdown_all();
    return EXIT_SUCCESS;
}
