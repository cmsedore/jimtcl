/* Jim Tcl ESP32 Task Extension - Shared Header
 *
 * Exposes the task slot structure and management APIs so other extensions
 * (watchdog, sleep) can interact with task VMs.
 */

#ifndef JIM_ESP32_TASK_H
#define JIM_ESP32_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TASK_MAX_SLOTS 8
#define TASK_MSG_QUEUE_LEN 4
#define TASK_DEFAULT_STACK 8192
#define TASK_DEFAULT_PRIORITY 5

/* Circuit breaker history depth */
#define CB_HISTORY_LEN 8

/* Circuit breaker defaults */
#define CB_DEFAULT_MAX_RESTARTS 3       /* Max restarts within window before breaker opens */
#define CB_DEFAULT_WINDOW_MS    60000   /* 60 second window */
#define CB_DEFAULT_COOLDOWN_MS  300000  /* 5 minute cooldown before half-open */

/* Message types sent to a task's interpreter */
typedef enum {
    TASK_MSG_EVAL,       /* Evaluate script, send result back */
    TASK_MSG_SEND,       /* Evaluate script, no result needed */
    TASK_MSG_SHUTDOWN,   /* Destroy interpreter and exit task */
} task_msg_type_t;

/* Message structure passed via queue */
typedef struct {
    task_msg_type_t type;
    char *script;                /* Heap-allocated script string (freed by receiver) */
    QueueHandle_t reply_queue;   /* For EVAL: queue to send result back on */
} task_msg_t;

/* Reply from a task eval */
typedef struct {
    int retcode;
    char *result;   /* Heap-allocated result string (freed by caller) */
} task_reply_t;

/* Task lifecycle state */
typedef enum {
    TASK_STATE_STOPPED,
    TASK_STATE_STARTING,
    TASK_STATE_RUNNING,
} task_state_t;

/* Circuit breaker state */
typedef enum {
    CB_CLOSED,     /* Normal operation — restarts allowed */
    CB_OPEN,       /* Too many failures — restarts blocked until cooldown */
    CB_HALF_OPEN,  /* Cooldown expired — allow one trial restart */
} cb_state_t;

/* Per-task slot */
typedef struct {
    int in_use;
    TaskHandle_t task_handle;
    QueueHandle_t msg_queue;
    QueueHandle_t reply_queue;          /* Persistent reply queue for sync eval */
    char name[16];
    struct Jim_Interp *interp;
    char *retained_script;          /* Kept for restart (heap-allocated) */
    uint32_t stacksize;
    UBaseType_t priority;
    task_state_t state;
    int64_t last_activity_us;       /* Timestamp of last message processed */

    /* Restart tracking */
    int restart_count;              /* Total restarts over lifetime */
    int restart_pending;            /* Set before kill to preserve retained_script */

    /* Circuit breaker */
    cb_state_t cb_state;
    int cb_max_restarts;            /* Threshold: max restarts in window */
    unsigned long cb_window_ms;     /* Time window for counting restarts */
    unsigned long cb_cooldown_ms;   /* Cooldown before half-open trial */
    int64_t cb_open_since_us;       /* When the breaker opened */
    int64_t restart_timestamps_us[CB_HISTORY_LEN];  /* Ring of restart times */
} task_slot_t;

/* Global task slot array and mutex (defined in jim-esp-task.c) */
extern task_slot_t task_slots[TASK_MAX_SLOTS];
extern SemaphoreHandle_t task_slots_mutex;

/* Force-kill a task (graceful then hard). Returns 0 on success. */
int task_force_kill(int slot_idx);

/* Restart a task using its retained config. Returns:
 *   0  = success
 *  -1  = general failure
 *  -2  = circuit breaker open */
int task_restart(int slot_idx);

#ifdef __cplusplus
}
#endif

#endif /* JIM_ESP32_TASK_H */
