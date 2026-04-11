/* Jim Tcl I2C Extension for ESP32
 *
 * Provides Tcl commands for I2C bus master communication:
 *
 *   i2c init <port> -sda <pin> -scl <pin> ?-freq <hz>?
 *   i2c deinit <port>
 *   i2c write <port> <addr> <data_bytes>
 *   i2c read <port> <addr> <length>
 *   i2c detect <port>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "jim-i2c";

/* Track bus handles per port (ESP32 has 2 I2C ports) */
#define I2C_MAX_PORTS 2

static i2c_master_bus_handle_t bus_handles[I2C_MAX_PORTS] = { NULL, NULL };
static long bus_freq[I2C_MAX_PORTS] = { 100000, 100000 };

static int i2c_get_port(Jim_Interp *interp, Jim_Obj *obj, int *port)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 0 || val >= I2C_MAX_PORTS) {
        Jim_SetResultFormatted(interp, "invalid I2C port: %ld (must be 0-%d)", val, I2C_MAX_PORTS - 1);
        return JIM_ERR;
    }
    *port = (int)val;
    return JIM_OK;
}

static int i2c_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int port;
    if (i2c_get_port(interp, argv[0], &port) != JIM_OK) {
        return JIM_ERR;
    }

    /* Parse keyword arguments: -sda <pin> -scl <pin> ?-freq <hz>? */
    int sda = -1, scl = -1;
    long freq = 100000;  /* Default 100 kHz */
    int i;

    for (i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) {
            return JIM_ERR;
        }
        if (strcmp(opt, "-sda") == 0) {
            sda = (int)val;
        } else if (strcmp(opt, "-scl") == 0) {
            scl = (int)val;
        } else if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -sda, -scl, or -freq", opt);
            return JIM_ERR;
        }
    }

    if (sda < 0 || scl < 0) {
        Jim_SetResultString(interp, "must specify both -sda and -scl pins", -1);
        return JIM_ERR;
    }

    if (freq <= 0 || freq > 1000000) {
        Jim_SetResultString(interp, "frequency must be 1-1000000 Hz", -1);
        return JIM_ERR;
    }

    /* Deinit if already initialized */
    if (bus_handles[port] != NULL) {
        i2c_del_master_bus(bus_handles[port]);
        bus_handles[port] = NULL;
    }

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handles[port]);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "i2c init failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    bus_freq[port] = freq;
    ESP_LOGI(TAG, "I2C port %d initialized: SDA=%d SCL=%d freq=%ld", port, sda, scl, freq);
    return JIM_OK;
}

static int i2c_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int port;
    if (i2c_get_port(interp, argv[0], &port) != JIM_OK) {
        return JIM_ERR;
    }

    if (bus_handles[port] != NULL) {
        i2c_del_master_bus(bus_handles[port]);
        bus_handles[port] = NULL;
    }

    return JIM_OK;
}

static int i2c_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int port;
    if (i2c_get_port(interp, argv[0], &port) != JIM_OK) {
        return JIM_ERR;
    }
    if (bus_handles[port] == NULL) {
        Jim_SetResultString(interp, "I2C port not initialized", -1);
        return JIM_ERR;
    }

    long addr;
    if (Jim_GetLong(interp, argv[1], &addr) != JIM_OK) {
        return JIM_ERR;
    }

    /* Data is a list of byte values */
    int data_len = Jim_ListLength(interp, argv[2]);
    if (data_len <= 0) {
        Jim_SetResultString(interp, "empty data list", -1);
        return JIM_ERR;
    }

    uint8_t *data = malloc(data_len);
    if (!data) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    for (int i = 0; i < data_len; i++) {
        Jim_Obj *elem = Jim_ListGetIndex(interp, argv[2], i);
        long byte_val;
        if (Jim_GetLong(interp, elem, &byte_val) != JIM_OK) {
            free(data);
            return JIM_ERR;
        }
        data[i] = (uint8_t)(byte_val & 0xFF);
    }

    /* Create a temporary device handle for this transaction */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = (uint32_t)bus_freq[port],
    };
    i2c_master_dev_handle_t dev_handle;
    esp_err_t err = i2c_master_bus_add_device(bus_handles[port], &dev_cfg, &dev_handle);
    if (err != ESP_OK) {
        free(data);
        Jim_SetResultFormatted(interp, "i2c add device failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = i2c_master_transmit(dev_handle, data, data_len, 1000);
    free(data);
    i2c_master_bus_rm_device(dev_handle);

    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "i2c write failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, data_len);
    return JIM_OK;
}

static int i2c_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int port;
    if (i2c_get_port(interp, argv[0], &port) != JIM_OK) {
        return JIM_ERR;
    }
    if (bus_handles[port] == NULL) {
        Jim_SetResultString(interp, "I2C port not initialized", -1);
        return JIM_ERR;
    }

    long addr, length;
    if (Jim_GetLong(interp, argv[1], &addr) != JIM_OK) {
        return JIM_ERR;
    }
    if (Jim_GetLong(interp, argv[2], &length) != JIM_OK) {
        return JIM_ERR;
    }
    if (length <= 0 || length > 256) {
        Jim_SetResultString(interp, "read length must be 1-256", -1);
        return JIM_ERR;
    }

    uint8_t *buf = malloc(length);
    if (!buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = (uint32_t)bus_freq[port],
    };
    i2c_master_dev_handle_t dev_handle;
    esp_err_t err = i2c_master_bus_add_device(bus_handles[port], &dev_cfg, &dev_handle);
    if (err != ESP_OK) {
        free(buf);
        Jim_SetResultFormatted(interp, "i2c add device failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = i2c_master_receive(dev_handle, buf, length, 1000);
    i2c_master_bus_rm_device(dev_handle);

    if (err != ESP_OK) {
        free(buf);
        Jim_SetResultFormatted(interp, "i2c read failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Return as list of byte values */
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < length; i++) {
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, buf[i]));
    }
    free(buf);
    Jim_SetResult(interp, result);
    return JIM_OK;
}

static int i2c_cmd_detect(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int port;
    if (i2c_get_port(interp, argv[0], &port) != JIM_OK) {
        return JIM_ERR;
    }
    if (bus_handles[port] == NULL) {
        Jim_SetResultString(interp, "I2C port not initialized", -1);
        return JIM_ERR;
    }

    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);

    /* Probe addresses 0x08 to 0x77 (valid 7-bit range) */
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(bus_handles[port], addr, 50);
        if (err == ESP_OK) {
            Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, addr));
        }
    }

    Jim_SetResult(interp, result);
    return JIM_OK;
}

static const jim_subcmd_type i2c_command_table[] = {
    {   "init",
        "port -sda pin -scl pin ?-freq hz?",
        i2c_cmd_init,
        3,
        -1,
        /* Description: Initialize I2C master bus */
    },
    {   "deinit",
        "port",
        i2c_cmd_deinit,
        1,
        1,
        /* Description: Deinitialize I2C master bus */
    },
    {   "write",
        "port addr data_list",
        i2c_cmd_write,
        3,
        3,
        /* Description: Write bytes to an I2C device */
    },
    {   "read",
        "port addr length",
        i2c_cmd_read,
        3,
        3,
        /* Description: Read bytes from an I2C device */
    },
    {   "detect",
        "port",
        i2c_cmd_detect,
        1,
        1,
        /* Description: Scan bus and return list of detected device addresses */
    },
    { NULL }
};

int Jim_i2cInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "i2c");
    Jim_RegisterSubCmd(interp, "i2c", i2c_command_table, NULL);
    return JIM_OK;
}
