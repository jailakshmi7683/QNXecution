
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
#include "fault.h"

#define SERVICE_NAME "wheel_speed"
#define HEARTBEAT_NS 500000000ULL

int main(void)
{
    name_attach_t *attach;
    ServiceMsg msg;
    double t = 0.0;
    uint64_t *progress_counter = NULL;

    /* Initialize fault injection and service scheduling settings. */
    svc_init(SERVICE_NAME);

    /* Register the service with QNX name-based message passing. */
    attach = name_attach(NULL, SERVICE_NAME, 0);

    if (attach == NULL) {
        perror("wheel_speed: name_attach");
        return EXIT_FAILURE;
    }

    /* Create and map the shared-memory progress counter. */
    char shm_name[64];

    int name_len = snprintf(shm_name, sizeof(shm_name),
                            "%s%s",
                            COUNTER_SHM_PREFIX,
                            SERVICE_NAME);

    if (name_len < 0 || (size_t)name_len >= sizeof(shm_name)) {
        fprintf(stderr, "wheel_speed: shared-memory name too long\n");
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);

    if (shm_fd == -1) {
        perror("wheel_speed: shm_open");
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(uint64_t)) == -1) {
        perror("wheel_speed: ftruncate");
        close(shm_fd);
        name_detach(attach, 0);
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
        perror("wheel_speed: mmap");
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }

    progress_counter = (uint64_t *)mapped;
    __sync_lock_test_and_set(progress_counter, 0ULL);

    /* Write PID file for monitor diagnostics. */
    FILE *pidf = fopen("/tmp/" SERVICE_NAME ".pid", "w");

    if (pidf != NULL) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
    }

    LOG_EVENT(SERVICE_NAME, "STARTED", "waiting for requests");

    for (;;) {
        /*
         * Check for injected faults before waiting for a message.
         * INJ_STUCK blocks here, preventing further progress.
         */
        fault_hook(NULL);

        uint64_t timeout_ns = HEARTBEAT_NS;

        TimerTimeout(CLOCK_MONOTONIC,
                     _NTO_TIMEOUT_RECEIVE,
                     NULL,
                     &timeout_ns,
                     NULL);

        int rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);

        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                /* Report idle liveness only when no fault is active. */
                __sync_fetch_and_add(progress_counter, 1ULL);
                continue;
            }

            if (errno == EINTR) {
                continue;
            }

            LOG_EVENT(SERVICE_NAME, "ERROR", "MsgReceive failed");
            continue;
        }

        if (rcvid == 0) {
            /* Pulse received; no client message to process. */
            continue;
        }

        if (msg.type == MSG_TYPE_REQUEST) {
            t += 0.1;

            double fake_speed = 60.0 + 10.0 * sin(t);

            ServiceMsg reply = {0};

            reply.type = MSG_TYPE_REPLY;
            snprintf(reply.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);
            reply.value = fake_speed;
            reply.timestamp = (uint64_t)ClockCycles();

            if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1) {
                LOG_EVENT(SERVICE_NAME, "ERROR", "MsgReply failed");
                continue;
            }

            __sync_fetch_and_add(progress_counter, 1ULL);

            char log_msg[100];

            snprintf(log_msg, sizeof(log_msg),
                     "sent speed value = %.2f", fake_speed);

            LOG_EVENT(SERVICE_NAME, "REPLIED", log_msg);
        } else {
            MsgError(rcvid, EBADMSG);
        }
    }

    /* Unreachable during normal operation. */
    munmap(progress_counter, sizeof(uint64_t));
    name_detach(attach, 0);

    return EXIT_SUCCESS;
}
