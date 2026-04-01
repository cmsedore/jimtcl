/* Jim Tcl ESP32 Sleep Manager - Shared Header
 *
 * Cross-VM sleep coordination: any VM can propose sleep, all registered
 * VMs vote (approve/veto), and wake sources are collected from all VMs
 * before the system enters a low-power state.
 *
 * This header is shared between jim-sleep.c and jim-esp-task.c so the
 * task system can deliver sleep consultation messages to VM tasks.
 */

#ifndef JIM_ESP32_SLEEP_H
#define JIM_ESP32_SLEEP_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum VMs that can participate in sleep voting */
#define SLEEP_MAX_VOTERS 10

/* Maximum wake sources that can be registered */
#define SLEEP_MAX_WAKE_SOURCES 8

/* Sleep modes (maps to ESP-IDF power modes) */
typedef enum {
    SLEEP_MODE_LIGHT,      /* esp_light_sleep_start() - CPU paused, RAM retained */
    SLEEP_MODE_DEEP,       /* esp_deep_sleep() - only RTC memory retained */
} sleep_mode_t;

/* Wake source types */
typedef enum {
    WAKE_SOURCE_TIMER,     /* Wake after N microseconds */
    WAKE_SOURCE_GPIO,      /* Wake on GPIO level */
    WAKE_SOURCE_UART,      /* Wake on UART activity */
    WAKE_SOURCE_TOUCH,     /* Wake on touch pad */
} wake_source_type_t;

/* A registered wake source */
typedef struct {
    int active;
    wake_source_type_t type;
    union {
        struct { uint64_t duration_us; } timer;
        struct { uint64_t pin_mask; int level; } gpio;
        struct { int uart_num; } uart;
        struct { int touch_pad; } touch;
    } config;
    int voter_id;              /* Which voter registered this */
} wake_source_t;

/* Vote result from a single VM */
typedef enum {
    SLEEP_VOTE_PENDING,
    SLEEP_VOTE_APPROVE,
    SLEEP_VOTE_VETO,
} sleep_vote_t;

/* A registered voter (VM) */
typedef struct {
    int active;
    char name[16];             /* VM name for logging */
    QueueHandle_t msg_queue;   /* The task's existing message queue (borrowed from task system) */
    QueueHandle_t vote_reply;  /* Dedicated reply queue for vote results */
    char callback_proc[64];    /* Name of Tcl proc to call for consultation */
    int is_main;               /* True if this is the main interpreter (not on a task) */
    sleep_vote_t last_vote;    /* Result of most recent consultation */
} sleep_voter_t;

/* Global sleep manager state */
typedef struct {
    SemaphoreHandle_t mutex;
    sleep_voter_t voters[SLEEP_MAX_VOTERS];
    wake_source_t wake_sources[SLEEP_MAX_WAKE_SOURCES];
    int sleep_pending;         /* A sleep request is in progress */
    sleep_mode_t pending_mode; /* The proposed sleep mode */
    int vote_count;            /* How many votes have been cast this round */
    int veto_count;            /* How many vetoes this round */
} sleep_manager_t;

/* Get the global sleep manager singleton */
sleep_manager_t *sleep_manager_get(void);

/* Initialize the sleep manager (idempotent) */
void sleep_manager_init(void);

/* Register a voter. Returns voter_id or -1 on failure.
 * msg_queue is the task's existing message queue (NULL for main interp).
 * callback_proc is the Tcl proc name to evaluate during consultation. */
int sleep_manager_register(const char *name, QueueHandle_t msg_queue,
                           const char *callback_proc, int is_main);

/* Unregister a voter */
void sleep_manager_unregister(int voter_id);

/* Update a voter's callback proc */
void sleep_manager_set_callback(int voter_id, const char *callback_proc);

/* Add a wake source. Returns source_id or -1. */
int sleep_manager_add_wake_source(int voter_id, wake_source_type_t type);

/* Remove a wake source */
void sleep_manager_remove_wake_source(int source_id);

/* Get a wake source by id for configuration */
wake_source_t *sleep_manager_get_wake_source(int source_id);

#ifdef __cplusplus
}
#endif

#endif /* JIM_ESP32_SLEEP_H */
