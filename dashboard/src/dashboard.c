
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common.h"
#include "fault.h"

#define SERVICE_NAME       "dashboard"
#define ABS_NAME           "abs"
#define TRACTION_NAME      "traction_control"
#define WHEEL_SPEED_NAME   "wheel_speed"
#define POLL_INTERVAL_US   500000

static int poll_service(const char *service_name,
                        int *coid,
                        double *out_value)
{
    if (*coid == -1) {
        *coid = name_open(service_name, 0);

        if (*coid == -1) {
            return -1;
        }
    }

    ServiceMsg req = {0};
    ServiceMsg reply = {0};

    req.type = MSG_TYPE_REQUEST;
    snprintf(req.sender, MAX_NAME_LEN, "%s", SERVICE_NAME);

    if (MsgSend(*coid,
                &req, sizeof(req),
                &reply, sizeof(reply)) == -1) {
        name_close(*coid);
        *coid = -1;
        return -1;
    }

    *out_value = reply.value;
    return 0;
}

int main(void)
{
    int abs_coid = -1;
    int traction_coid = -1;
    int wheel_coid = -1;

    uint64_t *progress_counter = NULL;

    /* Initialize fault injection before entering the main loop. */
    svc_init(SERVICE_NAME);

    /* Create and map the shared-memory progress counter. */
    char shm_name[64];

    int name_len = snprintf(shm_name, sizeof(shm_name),
                            "%s%s",
                            COUNTER_SHM_PREFIX,
                            SERVICE_NAME);

    if (name_len < 0 || (size_t)name_len >= sizeof(shm_name)) {
        fprintf(stderr, "dashboard: shared-memory name too long\n");
        return EXIT_FAILURE;
    }

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);

    if (shm_fd == -1) {
        perror("dashboard: shm_open");
        return EXIT_FAILURE;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(uint64_t)) == -1) {
        perror("dashboard: ftruncate");
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
        perror("dashboard: mmap");
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

    LOG_EVENT(SERVICE_NAME, "STARTED",
              "polling wheel_speed, abs and traction_control");

    for (;;) {
        /* Process any injected fault before the next polling cycle. */
        fault_hook(NULL);

        double abs_value = 0.0;
        double traction_value = 0.0;
        double wheel_value = 0.0;
        char log_msg[128];

        if (poll_service(WHEEL_SPEED_NAME,
                         &wheel_coid, &wheel_value) == 0) {
            snprintf(log_msg, sizeof(log_msg),
                     "Speed: %.1f km/h", wheel_value);
            LOG_EVENT(SERVICE_NAME, "DISPLAY", log_msg);
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR",
                      "wheel_speed unreachable");
        }

        if (poll_service(ABS_NAME,
                         &abs_coid, &abs_value) == 0) {
            snprintf(log_msg, sizeof(log_msg),
                     "ABS status: %s (%.0f)",
                     abs_value == 1.0 ? "BRAKING ACTIVE" : "normal",
                     abs_value);
            LOG_EVENT(SERVICE_NAME, "DISPLAY", log_msg);
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR", "abs unreachable");
        }

        if (poll_service(TRACTION_NAME,
                         &traction_coid, &traction_value) == 0) {
            snprintf(log_msg, sizeof(log_msg),
                     "Traction status: %s (%.0f)",
                     traction_value == 1.0
                         ? "TRACTION CONTROL ENGAGED"
                         : "normal",
                     traction_value);
            LOG_EVENT(SERVICE_NAME, "DISPLAY", log_msg);
        } else {
            LOG_EVENT(SERVICE_NAME, "ERROR",
                      "traction_control unreachable");
        }

        /* Report progress after completing the polling cycle. */
        __sync_fetch_and_add(progress_counter, 1ULL);

        usleep(POLL_INTERVAL_US);
    }

    /* Unreachable during normal operation. */
    if (wheel_coid != -1) {
        name_close(wheel_coid);
    }

    if (abs_coid != -1) {
        name_close(abs_coid);
    }

    if (traction_coid != -1) {
        name_close(traction_coid);
    }

    munmap(progress_counter, sizeof(uint64_t));

    return EXIT_SUCCESS;
}
