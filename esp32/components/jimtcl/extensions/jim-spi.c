/* Jim Tcl SPI Extension for ESP32
 *
 * Provides Tcl commands for SPI master communication:
 *
 *   spi init <host> -mosi <pin> -miso <pin> -sclk <pin> ?-cs <pin>? ?-freq hz?
 *   spi transfer <host> <tx_data_bytes> ?-rxlen n?
 *   spi write <host> <data_bytes>
 *   spi read <host> <length>
 *   spi deinit <host>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "jim-spi";

/* Track available SPI hosts (SPI1 is reserved for flash).
 * SOC_SPI_PERIPH_NUM includes SPI1, so usable hosts = total - 1. */
#include "soc/soc_caps.h"
#define SPI_MAX_HOSTS (SOC_SPI_PERIPH_NUM - 1)

typedef struct {
    int initialized;
    spi_device_handle_t dev_handle;
} spi_host_state_t;

static spi_host_state_t spi_hosts[SPI_MAX_HOSTS] = { {0}, {0} };

static int spi_get_host(Jim_Interp *interp, Jim_Obj *obj, int *index, spi_host_device_t *host)
{
    long val;
    if (Jim_GetLong(interp, obj, &val) != JIM_OK) {
        return JIM_ERR;
    }
    if (val < 2 || val > 2 + SPI_MAX_HOSTS - 1) {
        Jim_SetResultFormatted(interp, "invalid SPI host: %ld (must be 2-%d)", val, 2 + SPI_MAX_HOSTS - 1);
        return JIM_ERR;
    }
    *index = (int)(val - 2);
    *host = (spi_host_device_t)val;
    return JIM_OK;
}

/* Parse a Tcl list of byte values into a uint8_t buffer.
 * Caller must free the returned buffer. */
static uint8_t *parse_byte_list(Jim_Interp *interp, Jim_Obj *listObj, int *out_len)
{
    int len = Jim_ListLength(interp, listObj);
    if (len <= 0) {
        Jim_SetResultString(interp, "empty data list", -1);
        return NULL;
    }

    uint8_t *buf = malloc(len);
    if (!buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return NULL;
    }

    for (int i = 0; i < len; i++) {
        Jim_Obj *elem = Jim_ListGetIndex(interp, listObj, i);
        long byte_val;
        if (Jim_GetLong(interp, elem, &byte_val) != JIM_OK) {
            free(buf);
            return NULL;
        }
        buf[i] = (uint8_t)(byte_val & 0xFF);
    }

    *out_len = len;
    return buf;
}

/* Return a Tcl list of byte values from a buffer. */
static Jim_Obj *bytes_to_list(Jim_Interp *interp, const uint8_t *buf, int len)
{
    Jim_Obj *result = Jim_NewListObj(interp, NULL, 0);
    for (int i = 0; i < len; i++) {
        Jim_ListAppendElement(interp, result, Jim_NewIntObj(interp, buf[i]));
    }
    return result;
}

static int spi_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int idx;
    spi_host_device_t host;
    if (spi_get_host(interp, argv[0], &idx, &host) != JIM_OK) {
        return JIM_ERR;
    }

    /* Parse keyword arguments */
    int mosi = -1, miso = -1, sclk = -1, cs = -1;
    long freq = 1000000; /* Default 1 MHz */

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) {
            return JIM_ERR;
        }
        if (strcmp(opt, "-mosi") == 0) {
            mosi = (int)val;
        } else if (strcmp(opt, "-miso") == 0) {
            miso = (int)val;
        } else if (strcmp(opt, "-sclk") == 0) {
            sclk = (int)val;
        } else if (strcmp(opt, "-cs") == 0) {
            cs = (int)val;
        } else if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -mosi, -miso, -sclk, -cs, or -freq", opt);
            return JIM_ERR;
        }
    }

    if (mosi < 0 || miso < 0 || sclk < 0) {
        Jim_SetResultString(interp, "must specify -mosi, -miso, and -sclk pins", -1);
        return JIM_ERR;
    }

    if (freq <= 0 || freq > 80000000) {
        Jim_SetResultString(interp, "frequency must be 1-80000000 Hz", -1);
        return JIM_ERR;
    }

    /* Deinit if already initialized */
    if (spi_hosts[idx].initialized) {
        spi_bus_remove_device(spi_hosts[idx].dev_handle);
        spi_bus_free(host);
        spi_hosts[idx].initialized = 0;
        spi_hosts[idx].dev_handle = NULL;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi,
        .miso_io_num = miso,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (int)freq,
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 1,
    };

    err = spi_bus_add_device(host, &dev_cfg, &spi_hosts[idx].dev_handle);
    if (err != ESP_OK) {
        spi_bus_free(host);
        Jim_SetResultFormatted(interp, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    spi_hosts[idx].initialized = 1;
    ESP_LOGI(TAG, "SPI%d initialized: MOSI=%d MISO=%d SCLK=%d CS=%d freq=%ld",
             (int)host + 1, mosi, miso, sclk, cs, freq);
    return JIM_OK;
}

static int spi_cmd_transfer(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int idx;
    spi_host_device_t host;
    if (spi_get_host(interp, argv[0], &idx, &host) != JIM_OK) {
        return JIM_ERR;
    }
    if (!spi_hosts[idx].initialized) {
        Jim_SetResultFormatted(interp, "SPI host %d not initialized", (int)(idx + 2));
        return JIM_ERR;
    }

    /* Parse tx data */
    int tx_len = 0;
    uint8_t *tx_buf = parse_byte_list(interp, argv[1], &tx_len);
    if (!tx_buf) {
        return JIM_ERR;
    }

    /* Optional -rxlen */
    long rx_len = tx_len;
    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) {
            free(tx_buf);
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-rxlen") == 0) {
            if (Jim_GetLong(interp, argv[i + 1], &rx_len) != JIM_OK) {
                free(tx_buf);
                return JIM_ERR;
            }
        } else {
            free(tx_buf);
            Jim_SetResultFormatted(interp, "unknown option \"%s\"", opt);
            return JIM_ERR;
        }
    }

    if (rx_len <= 0 || rx_len > 4096) {
        free(tx_buf);
        Jim_SetResultString(interp, "rxlen must be 1-4096", -1);
        return JIM_ERR;
    }

    /* Determine the larger of tx and rx for the transaction length */
    int trans_len = (tx_len > (int)rx_len) ? tx_len : (int)rx_len;

    uint8_t *rx_buf = calloc(trans_len, 1);
    if (!rx_buf) {
        free(tx_buf);
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    /* Pad tx buffer with zeros if shorter than transaction length */
    uint8_t *tx_padded = tx_buf;
    if (tx_len < trans_len) {
        tx_padded = calloc(trans_len, 1);
        if (!tx_padded) {
            free(tx_buf);
            free(rx_buf);
            Jim_SetResultString(interp, "out of memory", -1);
            return JIM_ERR;
        }
        memcpy(tx_padded, tx_buf, tx_len);
        free(tx_buf);
    }

    spi_transaction_t trans = {
        .length = trans_len * 8,
        .tx_buffer = tx_padded,
        .rx_buffer = rx_buf,
    };

    esp_err_t err = spi_device_transmit(spi_hosts[idx].dev_handle, &trans);
    free(tx_padded);

    if (err != ESP_OK) {
        free(rx_buf);
        Jim_SetResultFormatted(interp, "spi transfer failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResult(interp, bytes_to_list(interp, rx_buf, (int)rx_len));
    free(rx_buf);
    return JIM_OK;
}

static int spi_cmd_write(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int idx;
    spi_host_device_t host;
    if (spi_get_host(interp, argv[0], &idx, &host) != JIM_OK) {
        return JIM_ERR;
    }
    if (!spi_hosts[idx].initialized) {
        Jim_SetResultFormatted(interp, "SPI host %d not initialized", (int)(idx + 2));
        return JIM_ERR;
    }

    int tx_len = 0;
    uint8_t *tx_buf = parse_byte_list(interp, argv[1], &tx_len);
    if (!tx_buf) {
        return JIM_ERR;
    }

    spi_transaction_t trans = {
        .length = tx_len * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = NULL,
    };

    esp_err_t err = spi_device_transmit(spi_hosts[idx].dev_handle, &trans);
    free(tx_buf);

    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "spi write failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResultInt(interp, tx_len);
    return JIM_OK;
}

static int spi_cmd_read(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int idx;
    spi_host_device_t host;
    if (spi_get_host(interp, argv[0], &idx, &host) != JIM_OK) {
        return JIM_ERR;
    }
    if (!spi_hosts[idx].initialized) {
        Jim_SetResultFormatted(interp, "SPI host %d not initialized", (int)(idx + 2));
        return JIM_ERR;
    }

    long length;
    if (Jim_GetLong(interp, argv[1], &length) != JIM_OK) {
        return JIM_ERR;
    }
    if (length <= 0 || length > 4096) {
        Jim_SetResultString(interp, "read length must be 1-4096", -1);
        return JIM_ERR;
    }

    uint8_t *rx_buf = calloc(length, 1);
    if (!rx_buf) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    spi_transaction_t trans = {
        .length = length * 8,
        .rxlength = length * 8,
        .tx_buffer = NULL,
        .rx_buffer = rx_buf,
    };

    esp_err_t err = spi_device_transmit(spi_hosts[idx].dev_handle, &trans);
    if (err != ESP_OK) {
        free(rx_buf);
        Jim_SetResultFormatted(interp, "spi read failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    Jim_SetResult(interp, bytes_to_list(interp, rx_buf, (int)length));
    free(rx_buf);
    return JIM_OK;
}

static int spi_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    int idx;
    spi_host_device_t host;
    if (spi_get_host(interp, argv[0], &idx, &host) != JIM_OK) {
        return JIM_ERR;
    }

    if (spi_hosts[idx].initialized) {
        spi_bus_remove_device(spi_hosts[idx].dev_handle);
        spi_bus_free(host);
        spi_hosts[idx].initialized = 0;
        spi_hosts[idx].dev_handle = NULL;
        ESP_LOGI(TAG, "SPI%d deinitialized", (int)host + 1);
    }

    return JIM_OK;
}

static const jim_subcmd_type spi_command_table[] = {
    {   "init",
        "host -mosi pin -miso pin -sclk pin ?-cs pin? ?-freq hz?",
        spi_cmd_init,
        5,
        -1,
        /* Description: Initialize SPI bus and add device */
    },
    {   "transfer",
        "host tx_data_bytes ?-rxlen n?",
        spi_cmd_transfer,
        2,
        -1,
        /* Description: Full-duplex SPI transfer, returns rx bytes */
    },
    {   "write",
        "host data_bytes",
        spi_cmd_write,
        2,
        2,
        /* Description: Write-only SPI transfer */
    },
    {   "read",
        "host length",
        spi_cmd_read,
        2,
        2,
        /* Description: Read-only SPI transfer */
    },
    {   "deinit",
        "host",
        spi_cmd_deinit,
        1,
        1,
        /* Description: Remove SPI device and free bus */
    },
    { NULL }
};

int Jim_spiInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "spi");
    Jim_RegisterSubCmd(interp, "spi", spi_command_table, NULL);
    return JIM_OK;
}
