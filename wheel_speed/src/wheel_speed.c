#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/types.h>
#include "common.h"

#define SERVICE_NAME "wheel_speed"

int main(void) {
    name_attach_t *attach;
    ServiceMsg msg;
    double t = 0.0;

    /* Register this process under a well-known name so clients can find it */
    attach = name_attach(NULL, SERVICE_NAME, 0);
    if (attach == NULL) {
        perror("name_attach failed");
        exit(EXIT_FAILURE);
    }

    /* Set up shared-memory progress counter */
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%s", COUNTER_SHM_PREFIX, SERVICE_NAME);

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }
    ftruncate(shm_fd, sizeof(uint64_t));

    uint64_t *progress_counter = mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE,
                                    MAP_SHARED, shm_fd, 0);
    if (progress_counter == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    *progress_counter = 0;

    /* Write PID file immediately, before any test delay, so process-state
       check can correctly see this process as alive throughout */
    FILE *pidf = fopen("/tmp/" SERVICE_NAME ".pid", "w");
    if (pidf) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
    }

//    /* ===== TEMPORARY TEST: simulate a stuck/hung service ===== */
//    /* Comment out this #define to disable the test and restore normal operation */
//    #define TEST_SIMULATE_STUCK
//
//    #ifdef TEST_SIMULATE_STUCK
//    LOG_EVENT(SERVICE_NAME, "TEST", "simulating stuck state - entering long sleep");
//    sleep(60); /* process stays alive (PID valid, responds to kill(pid,0))
//                  but does zero work and never touches progress_counter */
//    #endif
//    /* ===== END TEMPORARY TEST ===== */

    LOG_EVENT(SERVICE_NAME, "STARTED", "waiting for requests");

    for (;;) {
        /* Block until a client sends a request */
        int rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);

        if (rcvid < 0) {
            /* Error receiving — log and keep looping */
            LOG_EVENT(SERVICE_NAME, "ERROR", "MsgReceive failed");
            continue;
        }

        if (rcvid == 0) {
            /* This was a pulse, not a message — ignore for now */
            continue;
        }

        if (msg.type == MSG_TYPE_REQUEST) {
            /* Generate a fake wheel speed value: a slowly varying number */
            t += 0.1;
            double fake_speed = 60.0 + 10.0 * sin(t); /* oscillates around 60 km/h */
            (*progress_counter)++;

            ServiceMsg reply;
            reply.type = MSG_TYPE_REPLY;
            snprintf(reply.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);
            reply.value = fake_speed;
            reply.timestamp = (uint64_t)ClockCycles();

            MsgReply(rcvid, EOK, &reply, sizeof(reply));

            char log_msg[100];
            snprintf(log_msg, sizeof(log_msg),
                     "sent speed value = %.2f",
                     fake_speed);

            LOG_EVENT(SERVICE_NAME, "REPLIED", log_msg);
        } else {
            /* Unknown message type — reply with an error */
            MsgError(rcvid, EBADMSG);
        }
    }

    name_detach(attach, 0);
    return EXIT_SUCCESS;
}
