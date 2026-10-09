#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define COUNTER_SHM_PREFIX "/progress_"  /* e.g. /progress_wheel_speed */

#define MAX_DEPS 4
#define MAX_NAME_LEN 32
#define NUM_SERVICES 4

/* Message type constants */
#define MSG_TYPE_REQUEST 1
#define MSG_TYPE_REPLY   2

typedef struct {
    char name[MAX_NAME_LEN];
    char depends_on[MAX_DEPS][MAX_NAME_LEN]; // empty string = no more deps
} ServiceNode;

static const ServiceNode dependency_graph[NUM_SERVICES] = {
    {"wheel_speed",      {"", "", "", ""}},
    {"abs",              {"wheel_speed", "", "", ""}},
    {"traction_control", {"wheel_speed", "", "", ""}},
    {"dashboard",        {"abs", "traction_control", "wheel_speed", ""}}
};

typedef struct {
    uint16_t type;              // MSG_TYPE_REQUEST or MSG_TYPE_REPLY
    char sender[MAX_NAME_LEN];
    double value;                // generic payload slot — reused for speed, decision, etc.
    uint64_t timestamp;          // set with ClockCycles() or clock_gettime()
} ServiceMsg;


#define LOG_EVENT(service, event, message)                         \
    do {                                                           \
        struct timespec ts;                                        \
        struct tm time_info;                                       \
        clock_gettime(CLOCK_REALTIME, &ts);                        \
        localtime_r(&ts.tv_sec, &time_info);                       \
                                                                   \
        printf("[%04d-%02d-%02d %02d:%02d:%02d.%03ld] "            \
               "%s | %s | %s\n",                                   \
               time_info.tm_year + 1900,                            \
               time_info.tm_mon + 1,                               \
               time_info.tm_mday,                                  \
               time_info.tm_hour,                                  \
               time_info.tm_min,                                   \
               time_info.tm_sec,                                   \
               ts.tv_nsec / 1000000L,                              \
               service,                                             \
               event,                                               \
               message);                                            \
        fflush(stdout);                                             \
    } while (0)

#endif // COMMON_H
