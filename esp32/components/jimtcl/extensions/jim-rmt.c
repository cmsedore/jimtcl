/* Jim Tcl RMT Extension for ESP32
 *
 * Provides Tcl commands for the RMT (Remote Control Transceiver) peripheral:
 *
 *   rmt tx init <gpio> ?-freq hz? ?-mem_symbols n?
 *   rmt tx send <handle> <symbol_list>           ;# list of {level duration_us} pairs
 *   rmt tx deinit <handle>
 *
 *   rmt rx init <gpio> ?-freq hz? ?-idle_threshold us?
 *   rmt rx start <handle> ?-callback {proc task}?
 *   rmt rx stop <handle>
 *   rmt rx deinit <handle>
 *
 *   rmt ws2812 init <gpio> <num_leds>
 *   rmt ws2812 set <handle> <led_index> <r> <g> <b>
 *   rmt ws2812 fill <handle> <r> <g> <b>
 *   rmt ws2812 show <handle>
 *   rmt ws2812 clear <handle>
 *   rmt ws2812 deinit <handle>
 */

#include <string.h>
#include <stdlib.h>
#include "jim.h"
#include "jim-subcmd.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "jim-esp32-task.h"

static const char *TAG = "jim-rmt";

#define RMT_MAX_CHANNELS 4
#define RMT_MAX_WS2812   4
#define RMT_RX_BUF_SIZE  256

/* WS2812 timing (in RMT ticks at 10MHz resolution = 100ns per tick) */
#define WS2812_T0H_TICKS  3   /* 300ns ~ 350ns */
#define WS2812_T0L_TICKS  9   /* 900ns */
#define WS2812_T1H_TICKS  9   /* 900ns */
#define WS2812_T1L_TICKS  3   /* 300ns ~ 350ns */
#define WS2812_RESET_TICKS 500 /* 50us = 500 * 100ns */

/* ----- TX channel state ----- */
typedef struct {
    int in_use;
    int gpio;
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;  /* copy encoder for raw symbols */
    uint32_t resolution_hz;
} rmt_tx_state_t;

static rmt_tx_state_t rmt_tx_channels[RMT_MAX_CHANNELS] = { 0 };

/* ----- RX channel state ----- */
typedef struct {
    int in_use;
    int gpio;
    rmt_channel_handle_t channel;
    uint32_t resolution_hz;
    rmt_symbol_word_t rx_buffer[RMT_RX_BUF_SIZE];
    int running;
    char callback_proc[64];
    char callback_task[16];
} rmt_rx_state_t;

static rmt_rx_state_t rmt_rx_channels[RMT_MAX_CHANNELS] = { 0 };

/* ----- WS2812 state ----- */
typedef struct {
    int in_use;
    int gpio;
    int num_leds;
    uint8_t *pixel_buf;         /* RGB data: 3 bytes per LED */
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder; /* bytes encoder for WS2812 */
} rmt_ws2812_state_t;

static rmt_ws2812_state_t rmt_ws2812[RMT_MAX_WS2812] = { 0 };

/* ----- WS2812 bytes encoder ----- */

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size,
                            rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws_enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws_enc->state) {
    case 0: /* encode pixel data */
        encoded_symbols += ws_enc->bytes_encoder->encode(ws_enc->bytes_encoder, channel,
                                                          primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_enc->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            *ret_state = state;
            return encoded_symbols;
        }
        /* fall through */
    case 1: /* encode reset code */
        encoded_symbols += ws_enc->copy_encoder->encode(ws_enc->copy_encoder, channel,
                                                         &ws_enc->reset_code,
                                                         sizeof(rmt_symbol_word_t),
                                                         &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws_enc->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
        }
        break;
    }
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws_enc->bytes_encoder);
    rmt_encoder_reset(ws_enc->copy_encoder);
    ws_enc->state = 0;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws_enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws_enc->bytes_encoder);
    rmt_del_encoder(ws_enc->copy_encoder);
    free(ws_enc);
    return ESP_OK;
}

static esp_err_t ws2812_encoder_new(rmt_encoder_handle_t *ret_encoder)
{
    ws2812_encoder_t *ws_enc = calloc(1, sizeof(ws2812_encoder_t));
    if (!ws_enc) return ESP_ERR_NO_MEM;

    ws_enc->base.encode = ws2812_encode;
    ws_enc->base.reset = ws2812_encoder_reset;
    ws_enc->base.del = ws2812_encoder_del;

    /* Bytes encoder: maps each bit to an RMT symbol */
    rmt_bytes_encoder_config_t bytes_cfg = {
        .bit0 = {
            .level0 = 1,
            .duration0 = WS2812_T0H_TICKS,
            .level1 = 0,
            .duration1 = WS2812_T0L_TICKS,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = WS2812_T1H_TICKS,
            .level1 = 0,
            .duration1 = WS2812_T1L_TICKS,
        },
        .flags.msb_first = 1,
    };
    esp_err_t err = rmt_new_bytes_encoder(&bytes_cfg, &ws_enc->bytes_encoder);
    if (err != ESP_OK) {
        free(ws_enc);
        return err;
    }

    /* Copy encoder for the reset signal */
    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &ws_enc->copy_encoder);
    if (err != ESP_OK) {
        rmt_del_encoder(ws_enc->bytes_encoder);
        free(ws_enc);
        return err;
    }

    ws_enc->reset_code = (rmt_symbol_word_t){
        .level0 = 0,
        .duration0 = WS2812_RESET_TICKS,
        .level1 = 0,
        .duration1 = WS2812_RESET_TICKS,
    };

    *ret_encoder = &ws_enc->base;
    return ESP_OK;
}

/* ----- Helper: find free slot ----- */

static int find_free_tx(void)
{
    for (int i = 0; i < RMT_MAX_CHANNELS; i++) {
        if (!rmt_tx_channels[i].in_use) return i;
    }
    return -1;
}

static int find_free_rx(void)
{
    for (int i = 0; i < RMT_MAX_CHANNELS; i++) {
        if (!rmt_rx_channels[i].in_use) return i;
    }
    return -1;
}

static int find_free_ws2812(void)
{
    for (int i = 0; i < RMT_MAX_WS2812; i++) {
        if (!rmt_ws2812[i].in_use) return i;
    }
    return -1;
}

/* ----- RX done callback ----- */

static bool IRAM_ATTR rmt_rx_done_callback(rmt_channel_handle_t channel,
                                            const rmt_rx_done_event_data_t *edata,
                                            void *user_ctx)
{
    rmt_rx_state_t *rx = (rmt_rx_state_t *)user_ctx;
    if (rx->callback_proc[0] == '\0') return false;

    /* Build a Tcl script with the received symbols as a list */
    size_t num_symbols = edata->num_symbols;
    /* Estimate: each symbol ~20 chars + overhead */
    size_t buf_size = strlen(rx->callback_proc) + 2 + num_symbols * 24 + 32;
    char *script = malloc(buf_size);
    if (!script) return false;

    int pos = snprintf(script, buf_size, "%s {", rx->callback_proc);
    for (size_t i = 0; i < num_symbols && pos < (int)buf_size - 32; i++) {
        rmt_symbol_word_t sym = edata->received_symbols[i];
        pos += snprintf(script + pos, buf_size - pos, "{%d %d %d %d}",
                        sym.level0, sym.duration0, sym.level1, sym.duration1);
        if (i + 1 < num_symbols) {
            script[pos++] = ' ';
        }
    }
    pos += snprintf(script + pos, buf_size - pos, "}");

    task_send_to_name(rx->callback_task, script);
    free(script);
    return false;
}

/* ===== TX subcommands ===== */

static int rmt_tx_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long gpio;
    if (Jim_GetLong(interp, argv[0], &gpio) != JIM_OK) return JIM_ERR;
    if (gpio < 0 || gpio > 48) {
        Jim_SetResultString(interp, "invalid GPIO pin", -1);
        return JIM_ERR;
    }

    long freq = 10000000;  /* 10MHz default */
    long mem_symbols = 64;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) return JIM_ERR;

        if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else if (strcmp(opt, "-mem_symbols") == 0) {
            mem_symbols = val;
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -freq or -mem_symbols", opt);
            return JIM_ERR;
        }
    }

    int slot = find_free_tx();
    if (slot < 0) {
        Jim_SetResultString(interp, "no free RMT TX channels", -1);
        return JIM_ERR;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = (int)gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = (uint32_t)freq,
        .mem_block_symbols = (size_t)mem_symbols,
        .trans_queue_depth = 4,
    };

    rmt_channel_handle_t ch = NULL;
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &ch);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Create a copy encoder for raw symbol transmission */
    rmt_copy_encoder_config_t copy_cfg = {};
    rmt_encoder_handle_t enc = NULL;
    err = rmt_new_copy_encoder(&copy_cfg, &enc);
    if (err != ESP_OK) {
        rmt_del_channel(ch);
        Jim_SetResultFormatted(interp, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = rmt_enable(ch);
    if (err != ESP_OK) {
        rmt_del_encoder(enc);
        rmt_del_channel(ch);
        Jim_SetResultFormatted(interp, "rmt_enable failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    rmt_tx_channels[slot].in_use = 1;
    rmt_tx_channels[slot].gpio = (int)gpio;
    rmt_tx_channels[slot].channel = ch;
    rmt_tx_channels[slot].encoder = enc;
    rmt_tx_channels[slot].resolution_hz = (uint32_t)freq;

    ESP_LOGI(TAG, "TX channel %d: gpio=%ld freq=%ld mem=%ld", slot, gpio, freq, mem_symbols);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int rmt_tx_cmd_send(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_CHANNELS || !rmt_tx_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid TX handle", -1);
        return JIM_ERR;
    }

    /* Parse symbol list: list of {level duration_us} pairs */
    int list_len = Jim_ListLength(interp, argv[1]);
    if (list_len <= 0) {
        Jim_SetResultString(interp, "empty symbol list", -1);
        return JIM_ERR;
    }

    rmt_symbol_word_t *symbols = calloc(list_len, sizeof(rmt_symbol_word_t));
    if (!symbols) {
        Jim_SetResultString(interp, "out of memory", -1);
        return JIM_ERR;
    }

    rmt_tx_state_t *tx = &rmt_tx_channels[handle];
    /* Convert microseconds to ticks: ticks = us * (resolution / 1000000) */
    double ticks_per_us = (double)tx->resolution_hz / 1000000.0;

    for (int i = 0; i < list_len; i++) {
        Jim_Obj *pair = Jim_ListGetIndex(interp, argv[1], i);
        int pair_len = Jim_ListLength(interp, pair);
        if (pair_len != 2 && pair_len != 4) {
            free(symbols);
            Jim_SetResultFormatted(interp,
                "symbol %d: expected {level duration_us} or {level0 dur0 level1 dur1}", i);
            return JIM_ERR;
        }

        if (pair_len == 2) {
            long level, dur_us;
            if (Jim_GetLong(interp, Jim_ListGetIndex(interp, pair, 0), &level) != JIM_OK ||
                Jim_GetLong(interp, Jim_ListGetIndex(interp, pair, 1), &dur_us) != JIM_OK) {
                free(symbols);
                return JIM_ERR;
            }
            uint32_t ticks = (uint32_t)(dur_us * ticks_per_us);
            symbols[i].level0 = level ? 1 : 0;
            symbols[i].duration0 = ticks;
            symbols[i].level1 = 0;
            symbols[i].duration1 = 0;
        } else {
            long l0, d0, l1, d1;
            if (Jim_GetLong(interp, Jim_ListGetIndex(interp, pair, 0), &l0) != JIM_OK ||
                Jim_GetLong(interp, Jim_ListGetIndex(interp, pair, 1), &d0) != JIM_OK ||
                Jim_GetLong(interp, Jim_ListGetIndex(interp, pair, 2), &l1) != JIM_OK ||
                Jim_GetLong(interp, Jim_ListGetIndex(interp, pair, 3), &d1) != JIM_OK) {
                free(symbols);
                return JIM_ERR;
            }
            symbols[i].level0 = l0 ? 1 : 0;
            symbols[i].duration0 = (uint32_t)(d0 * ticks_per_us);
            symbols[i].level1 = l1 ? 1 : 0;
            symbols[i].duration1 = (uint32_t)(d1 * ticks_per_us);
        }
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(tx->channel, tx->encoder,
                                  symbols, list_len * sizeof(rmt_symbol_word_t),
                                  &tx_config);
    if (err != ESP_OK) {
        free(symbols);
        Jim_SetResultFormatted(interp, "rmt_transmit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Wait for transmission to complete */
    err = rmt_tx_wait_all_done(tx->channel, 1000);
    free(symbols);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_tx_wait_all_done failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    return JIM_OK;
}

static int rmt_tx_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_CHANNELS || !rmt_tx_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid TX handle", -1);
        return JIM_ERR;
    }

    rmt_tx_state_t *tx = &rmt_tx_channels[handle];
    rmt_disable(tx->channel);
    rmt_del_encoder(tx->encoder);
    rmt_del_channel(tx->channel);
    memset(tx, 0, sizeof(*tx));

    ESP_LOGI(TAG, "TX channel %ld deinitialized", handle);
    return JIM_OK;
}

/* ===== RX subcommands ===== */

static int rmt_rx_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long gpio;
    if (Jim_GetLong(interp, argv[0], &gpio) != JIM_OK) return JIM_ERR;
    if (gpio < 0 || gpio > 48) {
        Jim_SetResultString(interp, "invalid GPIO pin", -1);
        return JIM_ERR;
    }

    long freq = 10000000;  /* 10MHz default */
    long idle_threshold = 10000;  /* 10ms default idle threshold in ticks */

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        long val;
        if (Jim_GetLong(interp, argv[i + 1], &val) != JIM_OK) return JIM_ERR;

        if (strcmp(opt, "-freq") == 0) {
            freq = val;
        } else if (strcmp(opt, "-idle_threshold") == 0) {
            idle_threshold = val;
        } else {
            Jim_SetResultFormatted(interp,
                "unknown option \"%s\": should be -freq or -idle_threshold", opt);
            return JIM_ERR;
        }
    }

    int slot = find_free_rx();
    if (slot < 0) {
        Jim_SetResultString(interp, "no free RMT RX channels", -1);
        return JIM_ERR;
    }

    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = (int)gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = (uint32_t)freq,
        .mem_block_symbols = 64,
    };

    rmt_channel_handle_t ch = NULL;
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &ch);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    rmt_rx_state_t *rx = &rmt_rx_channels[slot];
    rx->in_use = 1;
    rx->gpio = (int)gpio;
    rx->channel = ch;
    rx->resolution_hz = (uint32_t)freq;
    rx->running = 0;
    rx->callback_proc[0] = '\0';
    rx->callback_task[0] = '\0';

    ESP_LOGI(TAG, "RX channel %d: gpio=%ld freq=%ld idle=%ld", slot, gpio, freq, idle_threshold);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int rmt_rx_cmd_start(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_CHANNELS || !rmt_rx_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid RX handle", -1);
        return JIM_ERR;
    }

    rmt_rx_state_t *rx = &rmt_rx_channels[handle];

    /* Parse optional -callback {proc task} */
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            Jim_SetResultString(interp, "missing value for option", -1);
            return JIM_ERR;
        }
        const char *opt = Jim_String(argv[i]);
        if (strcmp(opt, "-callback") == 0) {
            int cb_len = Jim_ListLength(interp, argv[i + 1]);
            if (cb_len != 2) {
                Jim_SetResultString(interp, "-callback requires {proc task}", -1);
                return JIM_ERR;
            }
            const char *proc = Jim_String(Jim_ListGetIndex(interp, argv[i + 1], 0));
            const char *task = Jim_String(Jim_ListGetIndex(interp, argv[i + 1], 1));
            snprintf(rx->callback_proc, sizeof(rx->callback_proc), "%s", proc);
            snprintf(rx->callback_task, sizeof(rx->callback_task), "%s", task);
        } else {
            Jim_SetResultFormatted(interp, "unknown option \"%s\": should be -callback", opt);
            return JIM_ERR;
        }
    }

    /* Register RX done callback */
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_rx_done_callback,
    };
    esp_err_t err = rmt_rx_register_event_callbacks(rx->channel, &cbs, rx);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_rx_register_event_callbacks failed: %s",
                               esp_err_to_name(err));
        return JIM_ERR;
    }

    err = rmt_enable(rx->channel);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_enable failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    rmt_receive_config_t recv_cfg = {
        .signal_range_min_ns = 1250,
        .signal_range_max_ns = 12000000,
    };
    err = rmt_receive(rx->channel, rx->rx_buffer, sizeof(rx->rx_buffer), &recv_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_receive failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    rx->running = 1;
    return JIM_OK;
}

static int rmt_rx_cmd_stop(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_CHANNELS || !rmt_rx_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid RX handle", -1);
        return JIM_ERR;
    }

    rmt_rx_state_t *rx = &rmt_rx_channels[handle];
    if (rx->running) {
        rmt_disable(rx->channel);
        rx->running = 0;
    }
    return JIM_OK;
}

static int rmt_rx_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_CHANNELS || !rmt_rx_channels[handle].in_use) {
        Jim_SetResultString(interp, "invalid RX handle", -1);
        return JIM_ERR;
    }

    rmt_rx_state_t *rx = &rmt_rx_channels[handle];
    if (rx->running) {
        rmt_disable(rx->channel);
    }
    rmt_del_channel(rx->channel);
    memset(rx, 0, sizeof(*rx));

    ESP_LOGI(TAG, "RX channel %ld deinitialized", handle);
    return JIM_OK;
}

/* ===== WS2812 subcommands ===== */

static int rmt_ws2812_cmd_init(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long gpio, num_leds;
    if (Jim_GetLong(interp, argv[0], &gpio) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[1], &num_leds) != JIM_OK) return JIM_ERR;

    if (gpio < 0 || gpio > 48) {
        Jim_SetResultString(interp, "invalid GPIO pin", -1);
        return JIM_ERR;
    }
    if (num_leds <= 0 || num_leds > 1024) {
        Jim_SetResultString(interp, "num_leds must be 1-1024", -1);
        return JIM_ERR;
    }

    int slot = find_free_ws2812();
    if (slot < 0) {
        Jim_SetResultString(interp, "no free WS2812 slots", -1);
        return JIM_ERR;
    }

    /* Create TX channel at 10MHz for WS2812 timing */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = (int)gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,  /* 10MHz = 100ns per tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };

    rmt_channel_handle_t ch = NULL;
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &ch);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Create WS2812 encoder */
    rmt_encoder_handle_t enc = NULL;
    err = ws2812_encoder_new(&enc);
    if (err != ESP_OK) {
        rmt_del_channel(ch);
        Jim_SetResultFormatted(interp, "ws2812_encoder_new failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = rmt_enable(ch);
    if (err != ESP_OK) {
        rmt_del_encoder(enc);
        rmt_del_channel(ch);
        Jim_SetResultFormatted(interp, "rmt_enable failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    /* Allocate pixel buffer (GRB order for WS2812) */
    uint8_t *buf = calloc(num_leds * 3, 1);
    if (!buf) {
        rmt_disable(ch);
        rmt_del_encoder(enc);
        rmt_del_channel(ch);
        Jim_SetResultString(interp, "out of memory for pixel buffer", -1);
        return JIM_ERR;
    }

    rmt_ws2812[slot].in_use = 1;
    rmt_ws2812[slot].gpio = (int)gpio;
    rmt_ws2812[slot].num_leds = (int)num_leds;
    rmt_ws2812[slot].pixel_buf = buf;
    rmt_ws2812[slot].channel = ch;
    rmt_ws2812[slot].encoder = enc;

    ESP_LOGI(TAG, "WS2812 slot %d: gpio=%ld leds=%ld", slot, gpio, num_leds);
    Jim_SetResultInt(interp, slot);
    return JIM_OK;
}

static int rmt_ws2812_cmd_set(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle, idx, r, g, b;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_WS2812 || !rmt_ws2812[handle].in_use) {
        Jim_SetResultString(interp, "invalid WS2812 handle", -1);
        return JIM_ERR;
    }
    if (Jim_GetLong(interp, argv[1], &idx) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &r) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[3], &g) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[4], &b) != JIM_OK) return JIM_ERR;

    rmt_ws2812_state_t *ws = &rmt_ws2812[handle];
    if (idx < 0 || idx >= ws->num_leds) {
        Jim_SetResultFormatted(interp, "LED index %ld out of range (0-%d)", idx, ws->num_leds - 1);
        return JIM_ERR;
    }

    /* WS2812 uses GRB byte order */
    ws->pixel_buf[idx * 3 + 0] = (uint8_t)(g & 0xFF);
    ws->pixel_buf[idx * 3 + 1] = (uint8_t)(r & 0xFF);
    ws->pixel_buf[idx * 3 + 2] = (uint8_t)(b & 0xFF);
    return JIM_OK;
}

static int rmt_ws2812_cmd_fill(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle, r, g, b;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_WS2812 || !rmt_ws2812[handle].in_use) {
        Jim_SetResultString(interp, "invalid WS2812 handle", -1);
        return JIM_ERR;
    }
    if (Jim_GetLong(interp, argv[1], &r) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[2], &g) != JIM_OK) return JIM_ERR;
    if (Jim_GetLong(interp, argv[3], &b) != JIM_OK) return JIM_ERR;

    rmt_ws2812_state_t *ws = &rmt_ws2812[handle];
    for (int i = 0; i < ws->num_leds; i++) {
        ws->pixel_buf[i * 3 + 0] = (uint8_t)(g & 0xFF);
        ws->pixel_buf[i * 3 + 1] = (uint8_t)(r & 0xFF);
        ws->pixel_buf[i * 3 + 2] = (uint8_t)(b & 0xFF);
    }
    return JIM_OK;
}

static int rmt_ws2812_cmd_show(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_WS2812 || !rmt_ws2812[handle].in_use) {
        Jim_SetResultString(interp, "invalid WS2812 handle", -1);
        return JIM_ERR;
    }

    rmt_ws2812_state_t *ws = &rmt_ws2812[handle];
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(ws->channel, ws->encoder,
                                  ws->pixel_buf, ws->num_leds * 3,
                                  &tx_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_transmit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }

    err = rmt_tx_wait_all_done(ws->channel, 1000);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_tx_wait_all_done failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    return JIM_OK;
}

static int rmt_ws2812_cmd_clear(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_WS2812 || !rmt_ws2812[handle].in_use) {
        Jim_SetResultString(interp, "invalid WS2812 handle", -1);
        return JIM_ERR;
    }

    rmt_ws2812_state_t *ws = &rmt_ws2812[handle];
    memset(ws->pixel_buf, 0, ws->num_leds * 3);

    /* Flush zeros to strip */
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    esp_err_t err = rmt_transmit(ws->channel, ws->encoder,
                                  ws->pixel_buf, ws->num_leds * 3,
                                  &tx_cfg);
    if (err != ESP_OK) {
        Jim_SetResultFormatted(interp, "rmt_transmit failed: %s", esp_err_to_name(err));
        return JIM_ERR;
    }
    rmt_tx_wait_all_done(ws->channel, 1000);
    return JIM_OK;
}

static int rmt_ws2812_cmd_deinit(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    long handle;
    if (Jim_GetLong(interp, argv[0], &handle) != JIM_OK) return JIM_ERR;
    if (handle < 0 || handle >= RMT_MAX_WS2812 || !rmt_ws2812[handle].in_use) {
        Jim_SetResultString(interp, "invalid WS2812 handle", -1);
        return JIM_ERR;
    }

    rmt_ws2812_state_t *ws = &rmt_ws2812[handle];
    rmt_disable(ws->channel);
    rmt_del_encoder(ws->encoder);
    rmt_del_channel(ws->channel);
    free(ws->pixel_buf);
    memset(ws, 0, sizeof(*ws));

    ESP_LOGI(TAG, "WS2812 slot %ld deinitialized", handle);
    return JIM_OK;
}

/* ===== Top-level "rmt" dispatch ===== */

static const jim_subcmd_type rmt_tx_table[] = {
    {   "init",
        "gpio ?-freq hz? ?-mem_symbols n?",
        rmt_tx_cmd_init,
        1, -1,
    },
    {   "send",
        "handle symbol_list",
        rmt_tx_cmd_send,
        2, 2,
    },
    {   "deinit",
        "handle",
        rmt_tx_cmd_deinit,
        1, 1,
    },
    { NULL }
};

static const jim_subcmd_type rmt_rx_table[] = {
    {   "init",
        "gpio ?-freq hz? ?-idle_threshold us?",
        rmt_rx_cmd_init,
        1, -1,
    },
    {   "start",
        "handle ?-callback {proc task}?",
        rmt_rx_cmd_start,
        1, -1,
    },
    {   "stop",
        "handle",
        rmt_rx_cmd_stop,
        1, 1,
    },
    {   "deinit",
        "handle",
        rmt_rx_cmd_deinit,
        1, 1,
    },
    { NULL }
};

static const jim_subcmd_type rmt_ws2812_table[] = {
    {   "init",
        "gpio num_leds",
        rmt_ws2812_cmd_init,
        2, 2,
    },
    {   "set",
        "handle led_index r g b",
        rmt_ws2812_cmd_set,
        5, 5,
    },
    {   "fill",
        "handle r g b",
        rmt_ws2812_cmd_fill,
        4, 4,
    },
    {   "show",
        "handle",
        rmt_ws2812_cmd_show,
        1, 1,
    },
    {   "clear",
        "handle",
        rmt_ws2812_cmd_clear,
        1, 1,
    },
    {   "deinit",
        "handle",
        rmt_ws2812_cmd_deinit,
        1, 1,
    },
    { NULL }
};

static int rmt_cmd(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
    if (argc < 2) {
        Jim_SetResultString(interp, "wrong # args: should be \"rmt tx|rx|ws2812 ...\"", -1);
        return JIM_ERR;
    }

    const char *subcmd = Jim_String(argv[1]);

    if (strcmp(subcmd, "tx") == 0) {
        const jim_subcmd_type *ct = Jim_ParseSubCmd(interp, rmt_tx_table, argc - 1, argv + 1);
        return Jim_CallSubCmd(interp, ct, argc - 1, argv + 1);
    } else if (strcmp(subcmd, "rx") == 0) {
        const jim_subcmd_type *ct = Jim_ParseSubCmd(interp, rmt_rx_table, argc - 1, argv + 1);
        return Jim_CallSubCmd(interp, ct, argc - 1, argv + 1);
    } else if (strcmp(subcmd, "ws2812") == 0) {
        const jim_subcmd_type *ct = Jim_ParseSubCmd(interp, rmt_ws2812_table, argc - 1, argv + 1);
        return Jim_CallSubCmd(interp, ct, argc - 1, argv + 1);
    } else {
        Jim_SetResultFormatted(interp, "unknown subcommand \"%s\": should be tx, rx, or ws2812",
                               subcmd);
        return JIM_ERR;
    }
}

int Jim_rmtInit(Jim_Interp *interp)
{
    Jim_PackageProvideCheck(interp, "rmt");
    Jim_CreateCommand(interp, "rmt", rmt_cmd, NULL, NULL);
    return JIM_OK;
}
