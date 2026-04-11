/* Jim Tcl GPIO Extension for ESP32
 *
 * Provides Tcl commands for GPIO pin control:
 *
 *   gpio mode <pin> input|output|input_output
 *   gpio read <pin>
 *   gpio write <pin> 0|1
 *   gpio pullup <pin> 0|1
 *   gpio pulldown <pin> 0|1
 *   gpio interrupt <pin> -edge rising|falling|both|low|high -callback {proc task}
 *   gpio interrupt <pin> -remove
 *   gpio interrupt list
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "jim-esp32-task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "jim-gpio";

/* ---------------------------------------------------------------------------
 * GPIO interrupt → message system
 *
 * ISR posts {pin, level} to a queue. A background dispatcher task drains
 * the queue and delivers messages to Tcl task VMs via task_send_to_name.
 * ---------------------------------------------------------------------------*/

#define GPIO_ISR_QUEUE_LEN    16
#define GPIO_MAX_INTERRUPTS   8

typedef struct {
    gpio_num_t pin;
    int level;
} gpio_isr_event_t;

typedef struct {
    int active;
    gpio_num_t pin;
    gpio_int_type_t edge;
    char callback_proc[64];
    char callback_target[16];
} gpio_interrupt_t;

static gpio_interrupt_t gpio_interrupts[GPIO_MAX_INTERRUPTS] = { 0 };
static QueueHandle_t gpio_isr_queue = NULL;
static TaskHandle_t gpio_dispatcher_task = NULL;
static int gpio_isr_service_installed = 0;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    gpio_num_t pin = (gpio_num_t)(intptr_t)arg;
    gpio_isr_event_t evt = { .pin = pin, .level = gpio_get_level(pin) };
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(gpio_isr_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void gpio_dispatcher(void *param)
{
    gpio_isr_event_t evt;
    while (1) {
        if (xQueueReceive(gpio_isr_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Find the interrupt registration for this pin */
        for (int i = 0; i < GPIO_MAX_INTERRUPTS; i++) {
            gpio_interrupt_t *intr = &gpio_interrupts[i];
            if (!intr->active || intr->pin != evt.pin) continue;

            /* Determine edge description */
            const char *edge_str;
            switch (intr->edge) {
                case GPIO_INTR_POSEDGE:  edge_str = "rising"; break;
                case GPIO_INTR_NEGEDGE:  edge_str = "falling"; break;
                case GPIO_INTR_ANYEDGE:  edge_str = evt.level ? "rising" : "falling"; break;
                case GPIO_INTR_LOW_LEVEL:  edge_str = "low"; break;
                case GPIO_INTR_HIGH_LEVEL: edge_str = "high"; break;
                default: edge_str = "unknown"; break;
            }

            /* Build: {callback} {pin} {edge} {level} */
            char script[128];
            snprintf(script, sizeof(script), "%s %d %s %d",
                     intr->callback_proc, (int)evt.pin, edge_str, evt.level);

            if (task_send_to_name(intr->callback_target, script) != 0) {
                ESP_LOGW(TAG, "GPIO interrupt delivery failed: pin %d -> task '%s'",
                         (int)evt.pin, intr->callback_target);
            }
            break;
        }
    }
}

static void ensure_isr_infrastructure(void)
{
    if (!gpio_isr_queue) {
        gpio_isr_queue = xQueueCreate(GPIO_ISR_QUEUE_LEN, sizeof(gpio_isr_event_t));
    }
    if (!gpio_dispatcher_task) {
        xTaskCreate(gpio_dispatcher, "gpio_dispatch", 4096, NULL, 6, &gpio_dispatcher_task);
    }
    if (!gpio_isr_service_installed) {
        gpio_install_isr_service(0);
        gpio_isr_service_installed = 1;
    }
}

static int gpio_get_pin(Jim_Interp *interp, Jim_Obj *obj, gpio_num_t *pin)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 0 || val >= GPIO_NUM_MAX) {
        Jim_SetResultFormatted(interp, "invalid GPIO pin number: %ld", val);
        return JIM_ERR;
    }
    *pin = (gpio_num_t)val;
    return JIM_OK;
}

static int gpio_cmd_mode(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    const char *mode_str = Jim_String(argv[1]);
    gpio_mode_t mode;

    if (strcmp(mode_str, "input") == 0) {
        mode = GPIO_MODE_INPUT;
    }
    else if (strcmp(mode_str, "output") == 0) {
        mode = GPIO_MODE_OUTPUT;
    }
    else if (strcmp(mode_str, "input_output") == 0) {
        mode = GPIO_MODE_INPUT_OUTPUT;
    }
    else if (strcmp(mode_str, "disable") == 0) {
        mode = GPIO_MODE_DISABLE;
    }
    else {
        Jim_SetResultFormatted(interp, "bad mode \"%s\": should be input, output, input_output, or disable", mode_str);
        return JIM_ERR;
    }

    esp_err_t err = gpio_set_direction(pin, mode);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio_set_direction failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Also reset the pin to a known state */
    gpio_reset_pin(pin);
    gpio_set_direction(pin, mode);

    ESP_LOGD(TAG, "GPIO %d set to mode %s", pin, mode_str);
    return JIM_OK;
}

static int gpio_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    int level = gpio_get_level(pin);
    Jim_SetResultInt(interp, level);
    return JIM_OK;
}

static int gpio_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long level;
    if (Jim_GetLong(interp, argv[1], &level) != JIM_OK) {
        return JIM_ERR;
    }

    esp_err_t err = gpio_set_level(pin, level ? 1 : 0);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio_set_level failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int gpio_cmd_pullup(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long enable;
    if (Jim_GetLong(interp, argv[1], &enable) != JIM_OK) {
        return JIM_ERR;
    }

    esp_err_t err;
    if (enable) {
        err = gpio_pullup_en(pin);
    } else {
        err = gpio_pullup_dis(pin);
    }
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio pullup failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int gpio_cmd_pulldown(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) {
        return JIM_ERR;
    }

    long enable;
    if (Jim_GetLong(interp, argv[1], &enable) != JIM_OK) {
        return JIM_ERR;
    }

    esp_err_t err;
    if (enable) {
        err = gpio_pulldown_en(pin);
    } else {
        err = gpio_pulldown_dis(pin);
    }
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio pulldown failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int gpio_cmd_interrupt(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    /* gpio interrupt list */
    if (argc >= 1 && strcmp(Jim_String(argv[0]), "list") == 0) {
        Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
        for (int i = 0; i < GPIO_MAX_INTERRUPTS; i++) {
            gpio_interrupt_t *intr = &gpio_interrupts[i];
            if (!intr->active) continue;
            Jim_Obj *entry = Jim_NewListObj(interp, NULL, 0);
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "pin", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewIntObj(interp, intr->pin));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "callback", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, intr->callback_proc, -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, "target", -1));
            Jim_ListAppendElement(interp, entry, Jim_NewStringObj(interp, intr->callback_target, -1));
            Jim_ListAppendElement(interp, result, entry);
        }
        Jim_SetResult(interp, result);
        return JIM_OK;
    }

    /* Need at least pin + one option */
    if (argc < 2) {
        Jim_SetResultString(interp,
            "wrong # args: should be \"gpio interrupt pin -edge type -callback {proc task}\" "
            "or \"gpio interrupt pin -remove\" or \"gpio interrupt list\"", -1);
        return JIM_ERR;
    }

    gpio_num_t pin;
    if (gpio_get_pin(interp, argv[0], &pin) != JIM_OK) return JIM_ERR;

    /* gpio interrupt <pin> -remove */
    if (argc == 2 && strcmp(Jim_String(argv[1]), "-remove") == 0) {
        gpio_isr_handler_remove(pin);
        gpio_intr_disable(pin);
        for (int i = 0; i < GPIO_MAX_INTERRUPTS; i++) {
            if (gpio_interrupts[i].active && gpio_interrupts[i].pin == pin) {
                gpio_interrupts[i].active = 0;
                ESP_LOGI(TAG, "Interrupt removed from GPIO %d", (int)pin);
                break;
            }
        }
        return JIM_OK;
    }

    /* Parse -edge and -callback */
    const char *edge_str = NULL;
    const char *cb_proc = NULL;
    const char *cb_target = NULL;

    for (int i = 1; i < argc; i++) {
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-edge") == 0 && i + 1 < argc) {
            edge_str = Jim_String(argv[++i]);
        } else if (strcmp(opt, "-callback") == 0 && i + 1 < argc) {
            Jim_Obj *cbObj = argv[++i];
            if (Jim_ListLength(interp, cbObj) != 2) {
                Jim_SetResultString(interp, "-callback requires {procname target_task}", -1);
                return JIM_ERR;
            }
            cb_proc = Jim_String(Jim_ListGetIndex(interp, cbObj, 0));
            cb_target = Jim_String(Jim_ListGetIndex(interp, cbObj, 1));
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (!edge_str || !cb_proc || !cb_target) {
        Jim_SetResultString(interp, "must specify both -edge and -callback", -1);
        return JIM_ERR;
    }

    gpio_int_type_t edge;
    if (strcmp(edge_str, "rising") == 0) edge = GPIO_INTR_POSEDGE;
    else if (strcmp(edge_str, "falling") == 0) edge = GPIO_INTR_NEGEDGE;
    else if (strcmp(edge_str, "both") == 0) edge = GPIO_INTR_ANYEDGE;
    else if (strcmp(edge_str, "low") == 0) edge = GPIO_INTR_LOW_LEVEL;
    else if (strcmp(edge_str, "high") == 0) edge = GPIO_INTR_HIGH_LEVEL;
    else {
        Jim_SetResultFormatted(interp,
            "bad edge type \"%s\": should be rising, falling, both, low, or high", edge_str);
        return JIM_ERR;
    }

    /* Find or allocate a slot */
    int slot = -1;
    for (int i = 0; i < GPIO_MAX_INTERRUPTS; i++) {
        if (gpio_interrupts[i].active && gpio_interrupts[i].pin == pin) {
            slot = i;  /* Re-use existing slot for this pin */
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < GPIO_MAX_INTERRUPTS; i++) {
            if (!gpio_interrupts[i].active) { slot = i; break; }
        }
    }
    if (slot < 0) {
        Jim_SetResultString(interp, "maximum GPIO interrupts reached", -1);
        return JIM_ERR;
    }

    ensure_isr_infrastructure();

    /* Remove existing handler for this pin if re-registering */
    if (gpio_interrupts[slot].active) {
        gpio_isr_handler_remove(pin);
    }

    /* Configure the pin for interrupt */
    gpio_set_intr_type(pin, edge);
    esp_err_t err = gpio_isr_handler_add(pin, gpio_isr_handler, (void *)(intptr_t)pin);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    gpio_intr_enable(pin);

    /* Store registration */
    gpio_interrupt_t *intr = &gpio_interrupts[slot];
    intr->active = 1;
    intr->pin = pin;
    intr->edge = edge;
    strncpy(intr->callback_proc, cb_proc, sizeof(intr->callback_proc) - 1);
    intr->callback_proc[sizeof(intr->callback_proc) - 1] = '\0';
    strncpy(intr->callback_target, cb_target, sizeof(intr->callback_target) - 1);
    intr->callback_target[sizeof(intr->callback_target) - 1] = '\0';

    ESP_LOGI(TAG, "Interrupt on GPIO %d (%s) -> %s in task '%s'",
             (int)pin, edge_str, cb_proc, cb_target);
    return JIM_OK;
}

static const jim_subcmd_type gpio_command_table[] = {
    {   "mode",
        "pin input|output|input_output|disable",
        gpio_cmd_mode,
        2,
        2,
        /* Description: Set GPIO pin direction */
    },
    {   "read",
        "pin",
        gpio_cmd_read,
        1,
        1,
        /* Description: Read GPIO pin level (returns 0 or 1) */
    },
    {   "write",
        "pin level",
        gpio_cmd_write,
        2,
        2,
        /* Description: Set GPIO pin output level (0 or 1) */
    },
    {   "pullup",
        "pin enable",
        gpio_cmd_pullup,
        2,
        2,
        /* Description: Enable or disable internal pull-up resistor */
    },
    {   "pulldown",
        "pin enable",
        gpio_cmd_pulldown,
        2,
        2,
        /* Description: Enable or disable internal pull-down resistor */
    },
    {   "interrupt",
        "pin -edge rising|falling|both|low|high -callback {proc task} | pin -remove | list",
        gpio_cmd_interrupt,
        1,
        -1,
        /* Description: Wire GPIO interrupt to a task message */
    },
    { NULL }
};

int Jim_gpioInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "gpio");
    Jim_RegisterSubCmd(interp, "gpio", gpio_command_table, NULL);
    return JIM_OK;
}
