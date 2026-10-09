
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
#include "fault.h"

#define SERVICE_NAME "traction_control"
#define UPSTREAM_NAME "wheel_speed"
#define RECONNECT_DELAY_US 1000000
#define WORKER_DELAY_US    3000000

static pthread_mutex_t decision_lock = PTHREAD_MUTEX_INITIALIZER;
static double latest_decision = 0.0;
static uint64_t *progress_counter = NULL;

/*
 * Background worker:
 * 1. Checks for injected faults.
 * 2. Requests wheel speed.
 * 3. Computes the traction-control decision.
 * 4. Updates the shared progress counter.
 */
static void *worker_thread(void *arg)
{
    (void)arg;

    int wheel_coid = -1;

    /* Wait until wheel_speed becomes available. */
    while (wheel_coid == -1) {
        fault_hook(&decision_lock);

        wheel_coid = name_open(UPSTREAM_NAME, 0);

        if (wheel_coid == -1) {
            LOG_EVENT(SERVICE_NAME, "WAITING",
                      "wheel_speed not available yet");
            usleep(RECONNECT_DELAY_US);
        }
    }

    for (;;) {
        ServiceMsg req = {0};
        ServiceMsg wheel_reply = {0};

        /* Activate any pending injected fault. */
        fault_hook(&decision_lock);

        req.type = MSG_TYPE_REQUEST;
        snprintf(req.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);

        if (MsgSend(wheel_coid,
                    &req, sizeof(req),
                    &wheel_reply, sizeof(wheel_reply)) != -1) {

            /* Engage traction control above the configured threshold. */
            double decision = (wheel_reply.value > 67.0) ? 1.0 : 0.0;

            pthread_mutex_lock(&decision_lock);
            latest_decision = decision;
            pthread_mutex_unlock(&decision_lock);

            /* Record worker progress after processing a valid reply. */
            if (progress_counter != NULL) {
                __sync_fetch_and_add(progress_counter, 1ULL);
            }

            char log_msg[128];

            snprintf(log_msg, sizeof(log_msg),
                     "Computed Traction Decision = %.2f "
                     "for wheel speed = %.2f",
                     decision, wheel_reply.value);

            LOG_EVENT(SERVICE_NAME, "PROCESSED", log_msg);

            usleep(WORKER_DELAY_US);

        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR",
                      "MsgSend to wheel_speed failed; reconnecting");

            name_close(wheel_coid);
            wheel_coid = -1;

            usleep(RECONNECT_DELAY_US);

            wheel_coid = name_open(UPSTREAM_NAME, 0);

            if (wheel_coid == -1) {
                LOG_EVENT(SERVICE_NAME, "ERROR",
                          "wheel_speed still unavailable");
            } else {
                LOG_EVENT(SERVICE_NAME, "RECOVERED",
                          "reconnected to wheel_speed");
            }
        }
    }

    return NULL;
}

int main(void)
{
    /*
     * Initialize the fault-injection interface before starting
     * any worker thread.
     */
    svc_init(SERVICE_NAME);

    /*
     * Publish the service name so other QNX processes can connect.
     */
    name_attach_t *attach = name_attach(NULL, SERVICE_NAME, 0);

    if (attach == NULL) {
        perror("traction_control: name_attach");
        return EXIT_FAILURE;
    }

    /* Save the PID for diagnostic purposes. */
    FILE *pidf = fopen("/tmp/" SERVICE_NAME ".pid", "w");

    if (pidf != NULL) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
    }

    /*
     * Create and map the shared-memory progress counter.
     * The monitor uses this counter to detect a stalled worker.
     */
    char shm_name[64];

    int name_len = snprintf(shm_name, sizeof(shm_name),
                            "%s%s",
                            COUNTER_SHM_PREFIX,
                            SERVICE_NAME);

    if (name_len < 0 || (size_t)name_len >= sizeof(shm_name)) {
        fprintf(stderr, "traction_control: shared-memory name too long\n");
        return EXIT_FAILURE;
    }

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);

    if (shm_fd == -1) {
        perror("traction_control: shm_open");
        return EXIT_FAILURE;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(uint64_t)) == -1) {
        perror("traction_control: ftruncate");
        close(shm_fd);
        return EXIT_FAILURE;
    }

    void *mapped = mmap(NULL,
                        sizeof(uint64_t),
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        shm_fd,
                        0);

    close(shm_fd);

    if (mapped == MAP_FAILED) {
        perror("traction_control: mmap");
        return EXIT_FAILURE;
    }

    progress_counter = (uint64_t *)mapped;

    /*
     * Reset progress for this service instance.
     * The monitor should observe subsequent increments.
     */
    __sync_lock_test_and_set(progress_counter, 0ULL);

    /*
     * Start the background worker only after all shared state
     * has been initialized.
     */
    pthread_t tid;

    int thread_rc = pthread_create(&tid, NULL, worker_thread, NULL);

    if (thread_rc != 0) {
        fprintf(stderr, "traction_control: pthread_create: %s\n",
                strerror(thread_rc));
        munmap(progress_counter, sizeof(uint64_t));
        return EXIT_FAILURE;
    }

    LOG_EVENT(SERVICE_NAME, "STARTED",
              "worker thread running, serving clients");

    /*
     * Main thread: handle client requests independently of
     * the worker's wheel_speed requests.
     */
    for (;;) {
        ServiceMsg client_msg = {0};

        int rcvid = MsgReceive(attach->chid,
                               &client_msg,
                               sizeof(client_msg),
                               NULL);

        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("traction_control: MsgReceive");
            continue;
        }

        if (rcvid == 0) {
            /* Pulse received; no message reply is required. */
            continue;
        }

        if (client_msg.type == MSG_TYPE_REQUEST) {
            ServiceMsg reply = {0};

            reply.type = MSG_TYPE_REPLY;

            snprintf(reply.sender, MAX_NAME_LEN,
                     "%s", SERVICE_NAME);

            pthread_mutex_lock(&decision_lock);
            reply.value = latest_decision;
            pthread_mutex_unlock(&decision_lock);

            reply.timestamp = (uint64_t)ClockCycles();

            if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1) {
                LOG_EVENT(SERVICE_NAME, "ERROR",
                          "MsgReply failed");
            } else {
                LOG_EVENT(SERVICE_NAME, "REPLIED",
                          "sent decision to client");
            }
        } else {
            MsgError(rcvid, EBADMSG);
        }
    }

    /* The service normally runs indefinitely. */
    return EXIT_SUCCESS;
}
