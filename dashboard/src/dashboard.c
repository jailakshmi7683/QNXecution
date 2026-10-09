#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/types.h>
#include "common.h"

#define SERVICE_NAME "dashboard"
#define ABS_NAME "abs"
#define TRACTION_NAME "traction_control"
#define WHEEL_SPEED_NAME "wheel_speed"

/* Helper: attempt to pull the latest value from a named service.
   Returns 0 on success (fills *out_value), -1 on failure. */
static int poll_service(const char *service_name, int *coid, double *out_value) {
    if (*coid == -1) {
        *coid = name_open(service_name, 0);
        if (*coid == -1) {
            return -1;
        }
        LOG_EVENT(SERVICE_NAME, "RECOVERED", service_name);
    }

    ServiceMsg req = {0}, reply;
    req.type = MSG_TYPE_REQUEST;
    snprintf(req.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);

    if (MsgSend(*coid, &req, sizeof(req), &reply, sizeof(reply)) == -1) {
        name_close(*coid);
        *coid = -1;
        return -1;
    }

    *out_value = reply.value;
    return 0;
}

int main(void) {
    int abs_coid = -1;
    int traction_coid = -1;
    int wheel_coid = -1;

    LOG_EVENT(SERVICE_NAME, "STARTED", "polling wheel_speed, abs and traction_control");
    FILE *pidf = fopen("/tmp/" SERVICE_NAME ".pid", "w");
    if (pidf) {
        fprintf(pidf, "%d", getpid());
        fclose(pidf);
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

    for (;;) {
        double abs_value = 0.0, traction_value = 0.0, wheel_value = 0.0;
        char log_msg[128];

        if (poll_service(WHEEL_SPEED_NAME, &wheel_coid, &wheel_value) == 0) {
            snprintf(log_msg, sizeof(log_msg), "Speed: %.1f km/h", wheel_value);
            LOG_EVENT(SERVICE_NAME, "DISPLAY", log_msg);
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR", "wheel_speed unreachable");
        }

        if (poll_service(ABS_NAME, &abs_coid, &abs_value) == 0) {
            snprintf(log_msg, sizeof(log_msg), "ABS status: %s (%.0f)",
                     abs_value == 1.0 ? "BRAKING ACTIVE" : "normal", abs_value);
            LOG_EVENT(SERVICE_NAME, "DISPLAY", log_msg);
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR", "abs unreachable");
        }

        if (poll_service(TRACTION_NAME, &traction_coid, &traction_value) == 0) {
            snprintf(log_msg, sizeof(log_msg), "Traction status: %s (%.0f)",
                     traction_value == 1.0 ? "TRACTION CONTROL ENGAGED" : "normal", traction_value);
            LOG_EVENT(SERVICE_NAME, "DISPLAY", log_msg);
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR", "traction_control unreachable");
        }

        (*progress_counter)++;
        usleep(2000000);
    }

    return EXIT_SUCCESS;
}
