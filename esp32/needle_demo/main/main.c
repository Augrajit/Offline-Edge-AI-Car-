/* Modified by <YOUR NAME>, 2026: replaced set_led tool with move/turn for
 * a two-board robot car; added ESP-NOW link to Board B, wired-UART fallback,
 * plan formatting, and increased MAX_NEW to 160. */
/* Needle 2 on ESP32-S3: speech-shaped requests -> onboard RGB LED.
 *
 * The 13.1 MB model lives in its own flash partition and is memory-mapped, so
 * the engine reads weights in place and never copies them into RAM (there is
 * nowhere near enough). Activations and the KV cache go to octal PSRAM via
 * ND_ALLOC.
 *
 * Line protocol on the console UART, matching tools/needle_tui.py:
 *   in : one request per line
 *   out: EVT / TOK / CONF / ACT / ERR lines
 */
#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include <ctype.h>
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nd_cact.h"
#include "nd_grammar.h"
#include "nd_model.h"
#include "nd_sample.h"
#include "nd_tokenizer.h"
#include "nd_quant.h"
#include "freertos/semphr.h"

#define ONBOARD_GPIO   48
#define RMT_RESOLUTION 10000000      /* 10 MHz -> 100 ns per tick */
#define MAX_NEW        160
#define DEFAULT_SECS   2.0f
#define ND_LINE_MAX    256   /* not LINE_MAX: that is taken by limits.h */


#include "tools_schema.h"

/* The schema is generated from tools/led.json at build time; see
 * main/CMakeLists.txt. Edit that JSON, not this file. */
static const char TOOLS_RAW[] = ND_TOOLS_JSON;

/* Compacted at boot: the model was trained on whitespace-free schemas, and an
 * indented one degrades it silently (it starts naming tools that were never
 * declared). Doing this here means the JSON file's formatting is free. */
static char TOOLS_JSON[sizeof(TOOLS_RAW)];

static const struct { const char *name; uint8_t r, g, b; } PALETTE[] = {
    { "red",    64,  0,  0 },
    { "green",   0, 64,  0 },
    { "blue",    0,  0, 64 },
    { "yellow", 48, 40,  0 },
    { "purple", 40,  0, 56 },
    { "white",  40, 40, 40 },
};

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static nd_model             s_model;
static nd_grammar           s_grammar;

/* ------------------------------------------------- second-core GEMV worker
 *
 * Every matvec is independent across output rows, so the row range is split
 * in half and core 1 runs one half while core 0 runs the other. Both halves
 * read disjoint weight slices and write disjoint outputs, so no locking is
 * needed beyond the start/done handshake. */

static SemaphoreHandle_t s_go, s_done;
static nd_row_fn         s_fn;
static void             *s_ctx;
static uint32_t          s_r0, s_r1;

static void worker_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        s_fn(s_ctx, s_r0, s_r1);
        xSemaphoreGive(s_done);
    }
}

static void rows_dual_core(nd_row_fn fn, void *ctx, uint32_t nrows)
{
    uint32_t half = nrows / 2;

    /* Below this the handshake costs more than the work it saves. Attention
     * splits only 8 heads at a time, but each head is ~10K MACs, far above the
     * ~15 us handshake. */
    if (half < 2 || !s_go) {
        fn(ctx, 0, nrows);
        return;
    }
    s_fn  = fn;
    s_ctx = ctx;
    s_r0  = half;
    s_r1  = nrows;
    xSemaphoreGive(s_go);
    fn(ctx, 0, half);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static void worker_start(void)
{
    s_go   = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(worker_task, "nd_worker", 4096, NULL,
                            configMAX_PRIORITIES - 2, NULL, 1);
    nd_parallel_rows = rows_dual_core;
}

/* ------------------------------------------------------------------ WS2812 */

static void led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = ONBOARD_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz     = RMT_RESOLUTION,
        .trans_queue_depth = 4,
    };
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0  = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1  = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags = { .msb_first = 1 },
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&chan_cfg, &s_chan));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_chan));
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };          /* WS2812 wire order */
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx);
    rmt_tx_wait_all_done(s_chan, portMAX_DELAY);
}

static void led_run(const char *color, const char *mode, float secs)
{
    uint8_t r = 0, g = 0, b = 0;
    size_t  i;

    for (i = 0; i < sizeof(PALETTE) / sizeof(PALETTE[0]); i++) {
        if (!strcmp(PALETTE[i].name, color)) {
            r = PALETTE[i].r; g = PALETTE[i].g; b = PALETTE[i].b;
            break;
        }
    }
    if (secs <= 0.0f)
        secs = DEFAULT_SECS;

    if (!strcmp(mode, "flash")) {
        int cycles = (int)(secs / 0.4f);
        if (cycles < 1)
            cycles = 1;
        for (i = 0; i < (size_t)cycles; i++) {
            led_set(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    } else {
        led_set(r, g, b);
        vTaskDelay(pdMS_TO_TICKS((int)(secs * 1000.0f)));
        led_set(0, 0, 0);
    }
}

/* ------------------------------------------------------------ argument access
 *
 * The call is grammar-constrained, so it is already well-formed and contains
 * only declared keys - a full JSON parser would be dead weight. These read a
 * field out of the arguments object by name.
 *
 * Tool handlers below use these; they are the only JSON you need to touch when
 * adding a tool. */

static int arg_str(const char *json, const char *key, char *out, size_t cap)
{
    char        quoted[ND_GR_STRLEN + 4];
    const char *p;
    size_t      w = 0;

    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    p = strstr(json, quoted);
    if (!p)
        return 0;
    p = strchr(p + strlen(quoted), ':');
    if (!p)
        return 0;
    p = strchr(p, '"');
    if (!p)
        return 0;
    p++;
    while (*p && *p != '"' && w + 1 < cap)
        out[w++] = *p++;
    out[w] = '\0';
    return w > 0;
}

static float arg_num(const char *json, const char *key, float dflt)
{
    char        quoted[ND_GR_STRLEN + 4];
    const char *p;

    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    p = strstr(json, quoted);
    if (!p)
        return dflt;
    p = strchr(p + strlen(quoted), ':');
    if (!p)
        return dflt;
    return (float)atof(p + 1);
}

__attribute__((unused))    /* provided for handlers with boolean fields */
static int arg_bool(const char *json, const char *key, int dflt)
{
    char        quoted[ND_GR_STRLEN + 4];
    const char *p;

    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    p = strstr(json, quoted);
    if (!p)
        return dflt;
    p = strchr(p + strlen(quoted), ':');
    if (!p)
        return dflt;
    return strncmp(p + 1, "true", 4) == 0 || strncmp(p + 2, "true", 4) == 0;
}

/* ===========================================================================
 *                        YOUR TOOLS START HERE
 *
 * To use this project for something else:
 *   1. edit tools/led.json          - the schema the model is constrained to
 *   2. write a handler below        - receives the call's arguments object
 *   3. add it to the TOOL_TABLE     - matched by the schema's "name"
 *   4. print one ACT line           - the TUI renders it; format is free-form
 *                                     "ACT <what> k=v k=v", or "ACT none"
 * Nothing in engine/ needs to change.
 * ======================================================================== */

/* ---- link to the car (board B) ---------------------------------------- */
#define LINK_ESPNOW 1            /* 1 = ESP-NOW radio, 0 = 3-wire UART */

#define BOT_UART    UART_NUM_1
#define BOT_TX_GPIO 17
#define BOT_RX_GPIO 18

/* Replace with the MAC address printed by your car at boot. */
static const uint8_t CAR_MAC[6] = { 0x2C, 0xBC, 0xBB, 0x0C, 0x31, 0x24 };

static char              s_plan[240];
static int               s_plan_n, s_plan_over;
static unsigned          s_plan_id;
static volatile unsigned s_ack_id;
static volatile char     s_ack_kind;
static int               s_link_up;

static void bot_plan_reset(void)
{
    s_plan[0]   = '\0';
    s_plan_n    = 0;
    s_plan_over = 0;
}

#if LINK_ESPNOW
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    char b[16];
    int  n = len < 15 ? len : 15;

    (void)info;
    if (n < 2)
        return;
    memcpy(b, data, (size_t)n);
    b[n] = '\0';
    if (b[0] == 'A' || b[0] == 'N' || b[0] == 'B') {
        s_ack_id   = (unsigned)atoi(b + 1);
        s_ack_kind = b[0];
    }
}

static void bot_link_init(void)
{
    esp_err_t           e   = nvs_flash_init();
    wifi_init_config_t  cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_now_peer_info_t peer;

    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, CAR_MAC, 6);
    peer.channel = 1;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    printf("EVT radio_up internal_free=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    fflush(stdout);
    s_link_up = 1;
}
#else
static void bot_link_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    ESP_ERROR_CHECK(uart_driver_install(BOT_UART, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BOT_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BOT_UART, BOT_TX_GPIO, BOT_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
                                    s_link_up = 1;
}
#endif

/* Handlers call this once per step. Radio mode collects the steps; they are
 * sent together as one packet by bot_flush(). */
static void bot_send(const char *line)
{
#if LINK_ESPNOW
    size_t used = strlen(s_plan);
    size_t i    = 0;

    if (s_plan_n >= 8 || used + 20 >= sizeof(s_plan)) {
        s_plan_over = 1;
        return;
    }
    while (line[i] && line[i] != '\n')
        s_plan[used++] = line[i++];
    s_plan[used++] = ';';
    s_plan[used]   = '\0';
    s_plan_n++;
#else
    uart_write_bytes(BOT_UART, line, strlen(line));
#endif
}

static void bot_flush(void)
{
#if LINK_ESPNOW
    char pkt[250];
    int  n;

    if (s_plan_n == 0)
        return;
    if (!s_link_up) {
        printf("ERR car_link_down\n");
        fflush(stdout);
        bot_plan_reset();
        return;
    }
    if (s_plan_over) {
        printf("ERR plan_too_long (max 8 steps)\n");
        fflush(stdout);
        bot_plan_reset();
        return;
    }
    s_plan_id++;
    n = snprintf(pkt, sizeof(pkt), "P%u,%d|%s", s_plan_id, s_plan_n, s_plan);
    if (esp_now_send(CAR_MAC, (const uint8_t *)pkt, (size_t)n) == ESP_OK)
            printf("EVT car sent id=%u steps=%d internal_free=%u min=%u\n",
               s_plan_id, s_plan_n,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    else
        printf("ERR car_send_failed\n");
    fflush(stdout);
    bot_plan_reset();
#endif
}

static void bot_report_ack(void)
{
    char k = s_ack_kind;

    if (!k)
        return;
    printf("EVT car %s id=%u\n",
           k == 'A' ? "accepted" : k == 'N' ? "rejected" : "busy", s_ack_id);
    fflush(stdout);
    s_ack_kind = 0;
}

typedef void (*tool_fn)(const char *args);

static void tool_set_led(const char *args)
{
    char  color[32] = "", mode[16] = "";
    float secs;

    if (!arg_str(args, "color", color, sizeof(color)) ||
        !arg_str(args, "mode", mode, sizeof(mode))) {
        printf("ACT none\n");
        fflush(stdout);
        return;
    }
    secs = arg_num(args, "duration_seconds", DEFAULT_SECS);

    printf("ACT led color=%s mode=%s duration=%.1f\n", color, mode, secs);
    fflush(stdout);
    led_run(color, mode, secs);
}


static void tool_move(const char *args)
{
    char  dir[16] = "";
    char  line[32];
    float secs = arg_num(args, "seconds", 2.0f);

    if (!arg_str(args, "direction", dir, sizeof(dir)))
        strcpy(dir, "forward");               /* "go straight" */
    if (secs < 0.5f)  secs = 0.5f;
    if (secs > 10.0f) secs = 10.0f;
    snprintf(line, sizeof(line), "M %c %.1f\n", dir[0] == 'b' ? 'B' : 'F', secs);
    printf("ACT move dir=%s seconds=%.1f\n", dir, secs);
    fflush(stdout);
    bot_send(line);
}

static void tool_turn(const char *args)
{
    char dir[16] = "";
    char line[32];
    int  deg = (int)arg_num(args, "degrees", 90);

    if (!arg_str(args, "direction", dir, sizeof(dir))) {
        printf("ACT none\n");                 /* ambiguous: refuse, don't guess */
        fflush(stdout);
        return;
    }
    if (deg < 1)   deg = 1;
    if (deg > 360) deg = 360;
    snprintf(line, sizeof(line), "T %c %d\n", dir[0] == 'l' ? 'L' : 'R', deg);
    printf("ACT turn dir=%s degrees=%d\n", dir, deg);
    fflush(stdout);
    bot_send(line);
}

static const struct {
    const char *name;
    tool_fn     fn;
} TOOL_TABLE[] = {
    { "set_led", tool_set_led },
    { "move", tool_move },
    { "turn", tool_turn },
};


/* ===========================================================================
 *                         YOUR TOOLS END HERE
 * ======================================================================== */

/* Route a generated tool call to its handler. The grammar guarantees the name
 * is one the schema declared, so an unmatched name means the table and the
 * schema have drifted apart. */
/* The request text, lower-cased, kept so calls can be checked against it. */
static char s_query[ND_LINE_MAX];

/* True if 'dir' ("left" or "right") appears in the request as a direction.
 * "right away" and "right now" do not count. */
static int dir_in_query(const char *dir)
{
    size_t      n = strlen(dir);
    const char *p = s_query;

    while ((p = strstr(p, dir)) != NULL) {
        int before_ok = (p == s_query) || !isalpha((unsigned char)p[-1]);
        int after_ok  = !isalpha((unsigned char)p[n]);

        if (before_ok && after_ok) {
            int not_dir = !strcmp(dir, "right") &&
                          (!strncmp(p + n, " away", 5) || !strncmp(p + n, " now", 4));
            if (!not_dir)
                return 1;
        }
        p += n;
    }
    return 0;
}

/* Return the '}' that closes the object starting at p (p points at '{'),
 * or NULL if the text ends first. Braces inside strings are skipped. */
static const char *obj_end(const char *p)
{
    int depth = 0, in_str = 0;

    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == '"')
                in_str = 0;
        } else if (*p == '"') {
            in_str = 1;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}' && --depth == 0) {
            return p;
        }
    }
    return NULL;
}

/* Parse the next call at *pp. Returns 1 and fills name/args, or 0 when no
 * calls are left. */
static int next_call(const char **pp, char *name, size_t ncap,
                     char *args, size_t acap)
{
    const char *p = *pp, *a, *end;
    size_t      alen;

    name[0] = '\0';
    if (!arg_str(p, "name", name, ncap))
        return 0;
    a = strstr(p, "\"arguments\"");
    if (!a || !(a = strchr(a, '{')))
        return 0;
    end = obj_end(a);
    if (!end) {
        printf("ERR truncated_call\n");
        fflush(stdout);
        return 0;
    }
    alen = (size_t)(end - a) + 1;
    if (alen >= acap)
        alen = acap - 1;
    memcpy(args, a, alen);
    args[alen] = '\0';
    *pp = end + 1;
    return 1;
}

/* Pass 1 checks every call and runs nothing. Pass 2 runs them in order. */
static void dispatch_call(const char *generated)
{
    static char argbuf[256];
    char        name[ND_GR_STRLEN];
    const char *start = strstr(generated, "<tool_call>");
    const char *p;
    size_t      i;
    int         ran = 0;

    if (!start) {
        printf("ACT none\n");
        fflush(stdout);
        return;
    }
    start += strlen("<tool_call>");
    bot_plan_reset();

    p = start;
    while (next_call(&p, name, sizeof(name), argbuf, sizeof(argbuf))) {
        if (!strcmp(name, "turn")) {
            char dir[16] = "";
            if (!arg_str(argbuf, "direction", dir, sizeof(dir)) ||
                !dir_in_query(dir)) {
                printf("EVT guard turn direction not in request\n");
                printf("ACT none\n");
                fflush(stdout);
                return;
            }
        }
    }

    p = start;
    while (next_call(&p, name, sizeof(name), argbuf, sizeof(argbuf))) {
        int found = 0;

        for (i = 0; i < sizeof(TOOL_TABLE) / sizeof(TOOL_TABLE[0]); i++) {
            if (!strcmp(TOOL_TABLE[i].name, name)) {
                TOOL_TABLE[i].fn(argbuf);
                found = 1;
                ran++;
                break;
            }
        }
        if (!found) {
            printf("ERR no_handler_for %s\n", name);
            fflush(stdout);
        }
    }
      bot_flush();

    if (!ran) {
        printf("ACT none\n");
        fflush(stdout);
    }
}

/* -------------------------------------------------------------- inference */

static int s_show_think = 1;   /* toggled from the host with "!think" */

/* Prefill the constant part of the prompt once and snapshot it. Every request
 * then resumes from here, so only the query and the assistant header are
 * prefilled per turn - ~13 tokens instead of ~171. */
static void prime_prefix(void)
{
    static uint32_t ids[512];
    static char     pre[1024];
    int64_t         t0 = esp_timer_get_time();
    int             n;
    int             i;

    snprintf(pre, sizeof(pre), "<|im_start|>user\n<tools>%s</tools>", TOOLS_JSON);

    nd_model_reset(&s_model);
    ids[0] = ND_BOS_ID;
    n = nd_tok_encode(&s_model.tok, pre, strlen(pre), ids + 1,
                      (uint32_t)(sizeof(ids) / sizeof(ids[0]) - 1));
    if (n < 0) {
        printf("ERR prefix_too_long\n");
        return;
    }
    n += 1;
    printf("EVT priming tokens=%d (one-time; every later request reuses this)\n", n);
    fflush(stdout);
    for (i = 0; i < n; i++) {
        nd_model_step_hidden(&s_model, (uint32_t)ids[i]);
        if ((i + 1) % 8 == 0 || i + 1 == n) {
            double el = (esp_timer_get_time() - t0) / 1000000.0;
            double eta = el / (i + 1) * (n - i - 1);
            printf("EVT priming %d/%d  %.0f%%  elapsed=%.0fs  eta=%.0fs\n",
                   i + 1, n, 100.0 * (i + 1) / n, el, eta);
            fflush(stdout);
        }
    }
    nd_model_snapshot(&s_model);
    printf("EVT prefix tokens=%d ms=%.0f sink=%u (cached for all requests)\n",
           n, (esp_timer_get_time() - t0) / 1000.0, (unsigned)s_model.n_sink);
    fflush(stdout);
}

static void run_inference(const char *query)
{
    static uint32_t ids[512];
    static char     out[2048];
    static char     suf[512];
    nd_sampler      smp;
    const float    *lg = NULL;
    int             n;
    uint32_t        i, produced = 0, w = 0;
    int64_t         t0;
    double          pre_ms;

    {
        size_t qi;
        for (qi = 0; query[qi] && qi + 1 < sizeof(s_query); qi++)
            s_query[qi] = (query[qi] >= 'A' && query[qi] <= 'Z') ? query[qi] + 32 : query[qi];
        s_query[qi] = '\0';
    }

    snprintf(suf, sizeof(suf),
             "\n%s<|im_end|>\n<|im_start|>assistant\n", query);

    /* Resume from the cached prefix rather than re-running it. */
    nd_model_rewind(&s_model);
    nd_sampler_init(&smp, &s_model.tok, &s_grammar);

    n = nd_tok_encode_ex(&s_model.tok, suf, strlen(suf), ids,
                         (uint32_t)(sizeof(ids) / sizeof(ids[0])), 0);
    if (n < 0) {
        printf("ERR prompt_too_long\n");
        return;
    }

    t0 = esp_timer_get_time();
    for (i = 0; i < (uint32_t)n; i++) {
        lg = nd_model_step_hidden(&s_model, ids[i]);
        printf("EVT reading %u/%d\n", (unsigned)(i + 1), n);
        fflush(stdout);
    }
    pre_ms = (esp_timer_get_time() - t0) / 1000.0;

    printf("EVT prefill tokens=%d ms=%.0f tps=%.2f sink=%u\n",
           n, pre_ms, n / (pre_ms / 1000.0), (unsigned)s_model.n_sink);
    fflush(stdout);

    /* Optionally skip the reasoning block: force an empty <think></think> and
     * open the call directly. Verified to produce identical calls at roughly
     * half the generated tokens. */
    if (!s_show_think) {
        uint32_t forced[8], nf = 0, fi, tmp[4];
        int tn;
        forced[nf++] = ND_THINK_START_ID;
        forced[nf++] = ND_THINK_END_ID;
        tn = nd_tok_encode_ex(&s_model.tok, "\n", 1, tmp, 4, 0);
        for (fi = 0; fi < (uint32_t)tn; fi++)
            forced[nf++] = tmp[fi];
        forced[nf++] = ND_TOOL_CALL_START_ID;
        for (fi = 0; fi < nf; fi++) {
            char pc[128];
            nd_sample_accept(&smp, forced[fi]);
            nd_tok_decode_ex(&s_model.tok, &forced[fi], 1, pc, sizeof(pc), 0);
            if (w + strlen(pc) < sizeof(out) - 1) { strcpy(out + w, pc); w += strlen(pc); }
            lg = nd_model_step_hidden(&s_model, forced[fi]);
        }
    }

    t0 = esp_timer_get_time();
    for (i = 0; i < MAX_NEW; i++) {
        uint32_t id = nd_sample_hidden(&s_model, &smp, lg);
        char     piece[256];
        uint32_t k;

        if (id == (uint32_t)-1) {
            printf("ERR no_legal_token\n");
            break;
        }
        if (id == ND_EOS_ID || id == ND_IM_END_ID)
            break;
        nd_sample_accept(&smp, id);

        nd_tok_decode_ex(&s_model.tok, &id, 1, piece, sizeof(piece), 0);
        printf("TOK ");
        for (k = 0; piece[k]; k++) {
            if (piece[k] == '\n')      printf("\\n");
            else if (piece[k] != '\r') putchar(piece[k]);
        }
        printf("\n");
        fflush(stdout);

        if (w + strlen(piece) < sizeof(out) - 1) {
            strcpy(out + w, piece);
            w += strlen(piece);
        }
        produced++;
        lg = nd_model_step_hidden(&s_model, id);
    }
    out[w] = '\0';

    {
        double dec_ms = (esp_timer_get_time() - t0) / 1000.0;
        float  conf   = nd_model_confidence(&s_model);
        if (conf >= 0.0f)
            printf("CONF %.4f\n", conf);
        printf("EVT done tokens=%u ms=%.0f tps=%.2f\n", (unsigned)produced,
               dec_ms, produced ? produced / (dec_ms / 1000.0) : 0.0);
        fflush(stdout);
    }

    dispatch_call(out);
}

/* ------------------------------------------------------------------- main */

void app_main(void)
{
    const esp_partition_t     *part;
    esp_partition_mmap_handle_t handle;
    const void                *blob = NULL;
    const char                *gerr = NULL;

    vTaskDelay(pdMS_TO_TICKS(500));
    led_init();
    worker_start();
    led_set(0, 0, 8);                 /* dim blue: booting */

    if (nd_json_compact(TOOLS_RAW, strlen(TOOLS_RAW),
                        TOOLS_JSON, sizeof(TOOLS_JSON)) < 0) {
        printf("ERR schema_too_large\n");
        return;
    }

    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "model");
    if (!part) {
        printf("ERR no_model_partition\n");
        return;
    }
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                           &blob, &handle) != ESP_OK) {
        printf("ERR mmap_failed size=%u\n", (unsigned)part->size);
        return;
    }
    printf("EVT mapped partition=%u bytes at %p\n", (unsigned)part->size, blob);

    if (nd_model_open(&s_model, blob, part->size) != 0) {
        printf("ERR model_open\n");
        return;
    }
    if (nd_grammar_compile(&s_grammar, TOOLS_JSON, strlen(TOOLS_JSON), &gerr) != 0) {
        printf("ERR grammar %s\n", gerr ? gerr : "?");
        return;
    }

    printf("EVT ready model=needle2 layers=%u d_model=%u vocab=%u window=%u "
           "tools=%u psram_free=%u internal_free=%u\n",
           (unsigned)s_model.n_layers, (unsigned)s_model.d_model,
           (unsigned)s_model.vocab, (unsigned)s_model.window,
           (unsigned)s_grammar.n_tools,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    fflush(stdout);
    prime_prefix();

    /* Startup benchmark: a fixed number of steps with the KV cache cold, so
     * kernel changes can be measured in seconds instead of running a whole
     * 9-minute request. */
    {
        const int N = 6;
        int64_t   t;
        int       i;
        nd_model_rewind(&s_model);
        t = esp_timer_get_time();
        for (i = 0; i < N; i++)
            nd_model_step_hidden(&s_model, (uint32_t)(100 + i));
        {
            double ms = (esp_timer_get_time() - t) / 1000.0;
            printf("EVT bench tokens=%d ms=%.0f ms_per_tok=%.0f tps=%.3f\n",
                   N, ms, ms / N, N / (ms / 1000.0));
#ifdef ND_PROFILE
            {   /* Per-phase breakdown; enable ND_PROFILE in the component's
                 * CMakeLists to get it. Off by default: the timers add work
                 * to every layer and the output is noise during a demo. */
                static const char *PN[ND_P_COUNT] = {
                    "proj2bit", "attention", "hadamard", "mhc_phi4",
                    "engram", "logits4", "prep+lut", "confpool" };
                int p;
                for (p = 0; p < ND_P_COUNT; p++)
                    printf("EVT prof %-10s %8.1f ms  %5.1f%%\n", PN[p],
                           nd_prof[p] / 1000.0 / N,
                           100.0 * nd_prof[p] / 1000.0 / ms);
            }
#endif
            fflush(stdout);
        }
        nd_model_rewind(&s_model);
    }
    nd_model_rewind(&s_model);
    nd_model_rewind(&s_model);
    bot_link_init();
    printf("\n");
    printf("\n");
    printf("EVT ==================================================\n");
    printf("EVT  READY - type a request and press enter\n");
    printf("EVT  e.g. \"flash red light for two seconds\"\n");
    printf("EVT  commands: !think (toggle reasoning)\n");
    printf("EVT ==================================================\n\n");
    fflush(stdout);

    /* Three green pulses: the board says ready even off-camera. */
    {
        int b;
        for (b = 0; b < 3; b++) {
            led_set(0, 24, 0);
            vTaskDelay(pdMS_TO_TICKS(120));
            led_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    for (;;) {
        char line[ND_LINE_MAX];
        int  len = 0;

        /* Read one request line from the console. */
        for (;;) {
            int c = getchar();
            if (c == EOF) {
                bot_report_ack();
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (c == '\r')
                continue;
            if (c == '\n')
                break;
            if (len < ND_LINE_MAX - 1)
                line[len++] = (char)c;
        }
        line[len] = '\0';
        if (len == 0)
            continue;

        /* Reasoning control. "!think 0" / "!think 1" set it explicitly so a
         * reconnecting client is never at the mercy of the current state;
         * bare "!think" still toggles for interactive use. */
        if (!strncmp(line, "!think", 6)) {
            if (line[6] == ' ')
                s_show_think = (line[7] != '0');
            else
                s_show_think = !s_show_think;
            printf("EVT think=%d\n", s_show_think);
            fflush(stdout);
            continue;
        }

        run_inference(line);
        printf("EVT turn_end\n");
        fflush(stdout);
    }
}
