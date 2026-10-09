#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/types.h>
#include "common.h"
#include "fault.h"

#define SERVICE_NAME "abs"
#define UPSTREAM_NAME "wheel_speed"

static pthread_mutex_t decision_lock = PTHREAD_MUTEX_INITIALIZER;
static double latest_decision = 0.0;
static uint64_t *progress_counter = NULL;

/* Background thread: continuously pulls wheel_speed and updates the decision,
   independent of whether anyone is asking us for it right now. */
static void *worker_thread(void *arg) {
    (void)arg;
    int wheel_coid = -1;
    while (wheel_coid == -1) {
        wheel_coid = name_open(UPSTREAM_NAME, 0);
        if (wheel_coid == -1) {
            LOG_EVENT(SERVICE_NAME, "WAITING", "wheel_speed not available yet");
            usleep(1000000);
        }
    }

    for (;;) {

    	fault_hook(&decision_lock);

        ServiceMsg req = {0}, wheel_reply;
        req.type = MSG_TYPE_REQUEST;
        snprintf(req.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);

        if (MsgSend(wheel_coid, &req, sizeof(req), &wheel_reply, sizeof(wheel_reply)) != -1) {
            double decision = (wheel_reply.value < 55.0) ? 1.0 : 0.0;

            pthread_mutex_lock(&decision_lock);
            latest_decision = decision;
            pthread_mutex_unlock(&decision_lock);

            (*progress_counter)++;

            char log_msg[100];

            snprintf(log_msg, sizeof(log_msg),
                     "Computed Braking Decision = %.2f for wheel speed = %.2f",
                     decision, wheel_reply.value);

            LOG_EVENT(SERVICE_NAME, "PROCESSED", log_msg);
            usleep(3000000); //1sec
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR", "MsgSend to wheel_speed failed, reconnecting");

            /* The old connection is dead — close it and try to open a fresh one */
            name_close(wheel_coid);
            usleep(1000000);
            wheel_coid = name_open(UPSTREAM_NAME, 0);

            if (wheel_coid == -1) {
                LOG_EVENT(SERVICE_NAME, "ERROR", "wheel_speed still unavailable");
            } else {
                LOG_EVENT(SERVICE_NAME, "RECOVERED", "reconnected to wheel_speed");
            }
        }
    }

    return NULL;
}

int main(void) {
    name_attach_t *attach = name_attach(NULL, SERVICE_NAME, 0);
    if (attach == NULL) {
        perror("name_attach failed");
        exit(EXIT_FAILURE);
    }

    FILE *pidf = fopen("/tmp/" SERVICE_NAME ".pid", "w");
    if (pidf) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
    }

    /* Set up shared-memory progress counter BEFORE starting the worker thread,
       so it's never NULL by the time worker_thread tries to use it */
    char shm_name[64];
    snprintf(shm_name, sizeof(shm_name), "%s%s", COUNTER_SHM_PREFIX, SERVICE_NAME);

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }
    ftruncate(shm_fd, sizeof(uint64_t));

    progress_counter = mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_fd, 0);
    if (progress_counter == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }
    *progress_counter = 0;

    svc_init(SERVICE_NAME);

    /* NOW it's safe to start the worker thread */
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_thread, NULL) != 0) {
        perror("pthread_create failed");
        exit(EXIT_FAILURE);
    }

    LOG_EVENT(SERVICE_NAME, "STARTED", "worker thread running, serving clients");

    /* Main thread: only handles serving clients, always ready, never blocked on wheel_speed */
    for (;;) {
        ServiceMsg client_msg;
        int rcvid = MsgReceive(attach->chid, &client_msg, sizeof(client_msg), NULL);

        if (rcvid <= 0) continue;

        if (client_msg.type == MSG_TYPE_REQUEST) {
            ServiceMsg reply;
            reply.type = MSG_TYPE_REPLY;
            snprintf(reply.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);

            pthread_mutex_lock(&decision_lock);
            reply.value = latest_decision;
            pthread_mutex_unlock(&decision_lock);

            reply.timestamp = (uint64_t)ClockCycles();
            MsgReply(rcvid, EOK, &reply, sizeof(reply));
            LOG_EVENT(SERVICE_NAME, "REPLIED", "sent decision to client");
        } else {
            MsgError(rcvid, EBADMSG);
        }
    }

    return EXIT_SUCCESS;
}
