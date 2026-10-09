/*
 * fault.h - fault injection and service start-up helpers for QNX on Raspberry Pi 4.
 * Target: QNX SDP 8.0, Raspberry Pi 4 Model B, AArch64 little-endian (aarch64le).
 * This is QNX code, not a Linux port.
 */

#ifndef FAULT_H
#define FAULT_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifdef __QNXNTO__
#include <sched.h>
#endif

#include "common.h"

#define INJECT_SHM_PREFIX "/inject_"
#define SERVICE_PRIO      10

enum {
    INJ_NONE = 0,
    INJ_CRASH,
    INJ_STUCK,
    INJ_OVERRUN,
    INJ_DEADLOCK
};

typedef struct {
    volatile uint32_t cmd;       /* publish last */
    volatile uint32_t persist;   /* 1 = fault repeats after restart */
    volatile uint32_t arg_ms;    /* simulated overrun duration */
    volatile uint32_t pad;
    volatile uint64_t ts_ms;     /* CLOCK_MONOTONIC injection timestamp */
    volatile uint64_t act_ms;    /* CLOCK_MONOTONIC activation timestamp */
} InjectShm;

static InjectShm *g_inject = NULL;
static const char *g_inject_name = "?";

static inline uint64_t inject_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL
         + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Map "/inject_<service>". create=0 requires the object to exist already. */
static inline InjectShm *inject_map(const char *service, int create)
{
    char name[64];
    int fd;
    struct stat st;
    void *mapped;
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;

    if (service == NULL || service[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    if (snprintf(name, sizeof(name), "%s%s",
                 INJECT_SHM_PREFIX, service) >= (int)sizeof(name)) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    fd = shm_open(name, flags, 0666);
    if (fd == -1) {
        return NULL;
    }

    if (create && ftruncate(fd, (off_t)sizeof(InjectShm)) == -1) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }

    if (fstat(fd, &st) == -1 ||
        st.st_size < (off_t)sizeof(InjectShm)) {
        int saved_errno = (errno != 0) ? errno : EINVAL;
        close(fd);
        errno = saved_errno;
        return NULL;
    }

    mapped = mmap(NULL, sizeof(InjectShm),
                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }

    if (mapped == MAP_FAILED) {
        return NULL;
    }

    return (InjectShm *)mapped;
}

/* Monitor/injector side. */
static inline int fault_send(const char *service, uint32_t cmd,
                             uint32_t persist, uint32_t arg_ms)
{
    InjectShm *f = inject_map(service, 1);

    if (f == NULL) {
        return -1;
    }

    f->persist = persist;
    f->arg_ms = arg_ms;
    f->act_ms = 0;
    f->ts_ms = inject_now_ms();
    __sync_synchronize();         /* publish arguments before command */
    f->cmd = cmd;

    if (munmap(f, sizeof(*f)) == -1) {
        return -1;
    }

    return 0;
}

/* Return activation-to-observation latency in ms, or -1 if unavailable. */
static inline long fault_take_latency(const char *service, uint64_t now)
{
    InjectShm *f = inject_map(service, 0);
    long latency = -1;
    uint64_t activated;

    if (f == NULL) {
        return -1;
    }

    activated = f->act_ms;
    if (activated != 0 && activated <= now &&
        now - activated < 120000ULL) {
        latency = (long)(now - activated);
        f->act_ms = 0;
    }

    (void)munmap(f, sizeof(*f));
    return latency;
}

/* Service side: call once during service initialization. */
static inline void fault_init(const char *service_name)
{
    g_inject_name = service_name;
    g_inject = inject_map(service_name, 1);

    if (g_inject == NULL) {
        perror("fault_init: inject_map");
    }
}

/*
 * Call once in main(), before creating worker threads.
 * QNX may reject the requested priority if the process lacks permission or
 * the current scheduling policy does not permit it.
 */
static inline void svc_init(const char *service_name)
{
#ifdef __QNXNTO__
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    param.sched_priority = SERVICE_PRIO;

    if (sched_setparam(0, &param) == -1) {
        perror("svc_init: sched_setparam");
    }
#endif
    fault_init(service_name);
}

/* Call from the service work loop to activate injected faults. */
static inline void fault_hook(pthread_mutex_t *lock)
{
    InjectShm *f = g_inject;
    uint32_t cmd;
    uint32_t arg;

    if (f == NULL) {
        return;
    }

    cmd = f->cmd;
    if (cmd == INJ_NONE) {
        return;
    }

    __sync_synchronize();
    arg = f->arg_ms;

    if (!f->persist) {
        f->cmd = INJ_NONE;        /* one-shot: clear before acting */
    }
    f->act_ms = inject_now_ms();

    switch (cmd) {
    case INJ_CRASH:
        LOG_EVENT(g_inject_name, "INJECTED", "CRASH - aborting now");
        abort();
        break;

    case INJ_STUCK:
        LOG_EVENT(g_inject_name, "INJECTED",
                  "STUCK - process remains alive without progress");
        for (;;) {
            sleep(1);
        }
        break;

    case INJ_OVERRUN: {
        char message[80];
        struct timespec delay;

        (void)snprintf(message, sizeof(message),
                       "OVERRUN - cycle blocked for %u ms",
                       (unsigned int)arg);
        LOG_EVENT(g_inject_name, "INJECTED", message);

        delay.tv_sec = (time_t)(arg / 1000U);
        delay.tv_nsec = (long)(arg % 1000U) * 1000000L;
        while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
            /* Continue remaining delay if interrupted by a signal. */
        }
        break;
    }

    case INJ_DEADLOCK:
        LOG_EVENT(g_inject_name, "INJECTED",
                  lock != NULL
                      ? "DEADLOCK - worker locks mutex and stops"
                      : "DEADLOCK - no mutex supplied; behaving as STUCK");
        if (lock != NULL) {
            (void)pthread_mutex_lock(lock); /* intentionally never released */
        }
        for (;;) {
            sleep(1);
        }
        break;

    case INJ_NONE:
    default:
        break;
    }
}

#endif /* FAULT_H */
