/* REYAX RYLR999 driver: AT commands on the module's LoRa UART (UART4), frames carried as base64url text. */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "console.h"
#include "radio.h"
#include "serial.h"

/* RF settings, each overridable with a -D flag; every module in the network must match. */
#ifndef RYLR999_BAND_HZ
#define RYLR999_BAND_HZ 915000000
#endif
#ifndef RYLR999_SF
#define RYLR999_SF 8 /* the module guide recommends SF8 for payloads over 100 bytes */
#endif
#ifndef RYLR999_BW
#define RYLR999_BW 7 /* 7: 125 kHz, 8: 250 kHz, 9: 500 kHz */
#endif
#ifndef RYLR999_CR
#define RYLR999_CR 1 /* coding rate 4/(4+CR) */
#endif
#ifndef RYLR999_PREAMBLE
#define RYLR999_PREAMBLE 12
#endif
#ifndef RYLR999_NETWORK
#define RYLR999_NETWORK 18 /* the module's default, and the only network id that allows a preamble other than 12 */
#endif
#ifndef RYLR999_POWER_DBM
#define RYLR999_POWER_DBM 10 /* 30 dBm draws 950 mA at 5 V, more than the Nucleo's USB supply gives */
#endif
#ifndef RYLR999_TX_MARGIN_MS
#define RYLR999_TX_MARGIN_MS 20
#endif

_Static_assert(RYLR999_SF >= 7 && RYLR999_SF <= RYLR999_BW + 2, "the module allows SF7-9 at 125 kHz, SF7-10 at 250, SF7-11 at 500");
_Static_assert(RYLR999_BW >= 7 && RYLR999_BW <= 9, "bandwidth code is 7, 8 or 9");
_Static_assert(RYLR999_CR >= 1 && RYLR999_CR <= 4, "coding rate is 1 to 4");
_Static_assert(RYLR999_PREAMBLE == 12 || (RYLR999_NETWORK == 18 && RYLR999_PREAMBLE >= 4 && RYLR999_PREAMBLE <= 24),
               "a preamble other than 12 needs network 18 and 4 to 24");
_Static_assert(RYLR999_POWER_DBM >= 0 && RYLR999_POWER_DBM <= 30, "power is 0 to 30 dBm");

#define STR_(x) #x
#define STR(x) STR_(x)

#define B64_LEN(n) (((n) * 4u + 2u) / 3u)
#define MAX_TEXT 240u
#define PAYLOAD_OVERHEAD 8u /* the module adds 8 bytes to every payload on air */
#define BW_HZ (125000u << (RYLR999_BW - 7))

_Static_assert(B64_LEN(LMESH_MAX_FRAME) <= MAX_TEXT, "an encoded frame must fit one AT+SEND");

#define RESET_MS 20u
#define BOOT_MS 3000u
#define CMD_MS 2000u
#define SEND_GRACE_MS 2000u
#define RETRY_MS 2000u
#define LINE_MAX 320u

typedef struct setting {
    const char *name;
    const char *value;
    const char *store; /* suffix that makes the module keep the value in flash, if it needs one */
} setting;

/* Each is queried first and written only if it differs, since the module stores them in flash (10k cycles). */
static const setting settings[] = {
    {"NETWORKID", STR(RYLR999_NETWORK), ""},
    {"BAND", STR(RYLR999_BAND_HZ), ",M"},
    {"PARAMETER", STR(RYLR999_SF) "," STR(RYLR999_BW) "," STR(RYLR999_CR) "," STR(RYLR999_PREAMBLE), ""},
    {"CRFOP", STR(RYLR999_POWER_DBM), ""},
    {"MODE", "0", ""},
};

#define N_SETTINGS (sizeof settings / sizeof settings[0])
/* Bring-up steps: AT, AT+VER?, then a query and a write per setting */
#define STEP_AT 0u
#define STEP_VER 1u
#define STEP_SETTINGS 2u
#define STEP_DONE (STEP_SETTINGS + 2u * N_SETTINGS)

typedef enum state { S_RESET, S_BOOT, S_CMD, S_PAUSE, S_IDLE, S_SEND, S_ON_AIR } state;

static serial port;
static uint8_t rx_buf[1024];
static uint8_t tx_buf[512];
static char line[LINE_MAX];
static size_t line_len;
static bool line_overflow;

static state st;
static uint32_t deadline;
static uint32_t done_ms;
static unsigned step;
static char last_cmd[64];
static char version[32];

/* Counters for the debugger */
static uint32_t rx_frames;
static uint32_t rx_bad;
static uint32_t tx_errors;
static uint32_t resets;

static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    size_t i = 0;
    for (; i + 2u < n; i += 3u) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1u] << 8 | in[i + 2u];
        out[o++] = b64_chars[v >> 18 & 63u];
        out[o++] = b64_chars[v >> 12 & 63u];
        out[o++] = b64_chars[v >> 6 & 63u];
        out[o++] = b64_chars[v & 63u];
    }
    if (n - i == 1u) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = b64_chars[v >> 18 & 63u];
        out[o++] = b64_chars[v >> 12 & 63u];
    } else if (n - i == 2u) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1u] << 8;
        out[o++] = b64_chars[v >> 18 & 63u];
        out[o++] = b64_chars[v >> 12 & 63u];
        out[o++] = b64_chars[v >> 6 & 63u];
    }
    return o;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '-') {
        return 62;
    }
    if (c == '_') {
        return 63;
    }
    return -1;
}

/* Returns the decoded length, or 0 if the text is not valid unpadded base64url or does not fit. */
static size_t b64_decode(const char *in, size_t n, uint8_t *out, size_t max)
{
    if (n % 4u == 1u || n * 3u / 4u > max) {
        return 0;
    }
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64_value(in[i]);
        if (v < 0) {
            return 0;
        }
        acc = acc << 6 | (uint32_t)v;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (acc & ((1u << bits) - 1u)) == 0 ? o : 0;
}

/* Semtech's time-on-air formula for SF7 and up, explicit header, CRC on. */
static uint32_t lora_airtime_us(size_t payload)
{
    const uint32_t sf = RYLR999_SF;
    const uint32_t tsym_us = (1u << sf) * 1000000u / BW_HZ;
    const uint32_t de = tsym_us > 16000u ? 1u : 0u;
    int32_t num = 8 * (int32_t)payload - 4 * (int32_t)sf + 28 + 16;
    int32_t den = 4 * (int32_t)(sf - 2u * de);
    uint32_t blocks = num > 0 ? (uint32_t)((num + den - 1) / den) : 0u;
    uint32_t symbols = 8u + blocks * (RYLR999_CR + 4u);
    return (4u * RYLR999_PREAMBLE + 17u) * tsym_us / 4u + symbols * tsym_us;
}

/* Copies text into a JSON string body, replacing anything that would need escaping. */
static void json_safe(char *out, size_t max, const char *in)
{
    size_t o = 0;
    for (; *in && o + 1u < max; in++) {
        char c = *in;
        out[o++] = (c < 0x20 || c > 0x7e || c == '"' || c == '\\') ? '?' : c;
    }
    out[o] = '\0';
}

static void report_ready(void)
{
    char v[sizeof version];
    char text[200];
    json_safe(v, sizeof v, version);
    snprintf(text, sizeof text,
             "{\"ev\":\"radio\",\"driver\":\"rylr999\",\"state\":\"ready\",\"version\":\"%s\","
             "\"band_hz\":" STR(RYLR999_BAND_HZ) ",\"sf\":" STR(RYLR999_SF) ",\"bw\":" STR(RYLR999_BW)
             ",\"cr\":" STR(RYLR999_CR) ",\"preamble\":" STR(RYLR999_PREAMBLE)
             ",\"power_dbm\":" STR(RYLR999_POWER_DBM) "}",
             v);
    console_line(text);
}

static void report_error(const char *reply)
{
    char c[sizeof last_cmd];
    char r[48];
    char text[200];
    json_safe(c, sizeof c, last_cmd);
    json_safe(r, sizeof r, reply);
    snprintf(text, sizeof text, "{\"ev\":\"radio\",\"driver\":\"rylr999\",\"state\":\"error\",\"cmd\":\"%s\",\"reply\":\"%s\"}",
             c, r);
    console_line(text);
}

static void command(const char *cmd)
{
    snprintf(last_cmd, sizeof last_cmd, "%s", cmd);
    serial_write(&port, cmd, strlen(cmd));
    serial_write(&port, "\r\n", 2);
    st = S_CMD;
    deadline = HAL_GetTick() + CMD_MS;
}

static void start_reset(void)
{
    resets++;
    board_radio_reset(true);
    st = S_RESET;
    deadline = HAL_GetTick() + RESET_MS;
}

/* Fails any frame on the air, reports, and starts over after a pause. */
static void fail(lmesh_node *node, const char *reply)
{
    if (st == S_SEND || st == S_ON_AIR) {
        tx_errors++;
        lmesh_radio_tx_done(node, false);
    }
    report_error(reply);
    st = S_PAUSE;
    deadline = HAL_GetTick() + RETRY_MS;
}

static void run_step(void)
{
    char cmd[64];
    if (step == STEP_AT) {
        command("AT");
    } else if (step == STEP_VER) {
        command("AT+VER?");
    } else if (step < STEP_DONE) {
        const setting *s = &settings[(step - STEP_SETTINGS) / 2u];
        if ((step - STEP_SETTINGS) % 2u == 0) {
            snprintf(cmd, sizeof cmd, "AT+%s?", s->name);
        } else {
            snprintf(cmd, sizeof cmd, "AT+%s=%s%s", s->name, s->value, s->store);
        }
        command(cmd);
    } else {
        st = S_IDLE;
        report_ready();
    }
}

/* Reads the next comma-separated integer: 1 with a value, 0 at the end, -1 on anything else. */
static int next_int(const char **p, int32_t *out)
{
    const char *s = *p;
    while (*s == ' ' || *s == ',') {
        s++;
    }
    if (*s == '\0') {
        *p = s;
        return 0;
    }
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') {
        s++;
    }
    if (*s < '0' || *s > '9') {
        return -1;
    }
    int32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    if (*s != '\0' && *s != ',' && *s != ' ') {
        return -1;
    }
    *out = neg ? -v : v;
    *p = s;
    return 1;
}

static bool same_values(const char *a, const char *b)
{
    for (;;) {
        int32_t x = 0;
        int32_t y = 0;
        int ra = next_int(&a, &x);
        int rb = next_int(&b, &y);
        if (ra < 0 || rb < 0 || ra != rb || x != y) {
            return false;
        }
        if (ra == 0) {
            return true;
        }
    }
}

/* +RCV=<address>,<length>,<data>,<rssi>,<snr>; base64url has no commas, so splitting on them is safe. */
static void receive(lmesh_node *node, char *s)
{
    char *field[5];
    size_t n = 0;
    field[n++] = s;
    for (char *p = s; *p; p++) {
        if (*p == ',') {
            *p = '\0';
            if (n == 5) {
                rx_bad++;
                return;
            }
            field[n++] = p + 1;
        }
    }
    if (n != 5) {
        rx_bad++;
        return;
    }
    const char *len_text = field[1];
    const char *rssi_text = field[3];
    const char *snr_text = field[4];
    int32_t len = 0;
    int32_t rssi = 0;
    int32_t snr = 0;
    char *data = field[2];
    while (*data == ' ') {
        data++;
    }
    size_t dlen = strlen(data);
    while (dlen > 0 && data[dlen - 1u] == ' ') {
        data[--dlen] = '\0';
    }
    if (next_int(&len_text, &len) != 1 || next_int(&rssi_text, &rssi) != 1 || next_int(&snr_text, &snr) != 1 ||
        (size_t)len != dlen) {
        rx_bad++;
        return;
    }
    uint8_t frame[LMESH_MAX_FRAME];
    size_t flen = b64_decode(data, dlen, frame, sizeof frame);
    if (flen == 0) {
        rx_bad++;
        return;
    }
    rx_frames++;
    lmesh_radio_rx(node, frame, flen, (int16_t)rssi, (int8_t)snr);
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void config_reply(lmesh_node *node, const char *s)
{
    bool query = step == STEP_VER || (step >= STEP_SETTINGS && (step - STEP_SETTINGS) % 2u == 0);
    const char *eq = strchr(s, '=');
    if (starts(s, "+ERR=")) {
        fail(node, s);
    } else if (!query && strcmp(s, "+OK") == 0) {
        step = step >= STEP_SETTINGS ? step + 1u : STEP_VER;
        run_step();
    } else if (query && s[0] == '+' && eq) {
        if (step == STEP_VER) {
            snprintf(version, sizeof version, "%s", eq + 1);
            step = STEP_SETTINGS;
        } else {
            const setting *set = &settings[(step - STEP_SETTINGS) / 2u];
            step += same_values(eq + 1, set->value) ? 2u : 1u;
        }
        run_step();
    }
}

static void on_line(lmesh_node *node, char *s)
{
    if (starts(s, "+RCV=")) {
        receive(node, s + 5);
        return;
    }
    if (strcmp(s, "+READY") == 0) {
        if (st == S_SEND || st == S_ON_AIR) {
            fail(node, s);
        }
        step = STEP_AT;
        run_step();
        return;
    }
    if (strcmp(s, "+HITEMP") == 0) {
        report_error(s);
        return;
    }
    switch (st) {
    case S_CMD:
        config_reply(node, s);
        break;
    case S_SEND:
        if (strcmp(s, "+OK") == 0) {
            st = S_ON_AIR;
        } else if (starts(s, "+ERR=")) {
            tx_errors++;
            st = S_IDLE;
            lmesh_radio_tx_done(node, false);
        }
        break;
    default:
        break;
    }
}

void radio_init(void)
{
    serial_init(&port, &board_radio_uart, rx_buf, sizeof rx_buf, tx_buf, sizeof tx_buf);
    start_reset();
}

uint32_t radio_airtime_ms(size_t len)
{
    size_t text = B64_LEN(len);
    size_t cmd = sizeof "AT+SEND=0,240,\r\n" - 1u + text;
    uint32_t uart_us = (uint32_t)cmd * 10u * 1000000u / RADIO_UART_BAUD;
    uint32_t us = uart_us + lora_airtime_us(text + PAYLOAD_OVERHEAD);
    return (us + 999u) / 1000u + RYLR999_TX_MARGIN_MS;
}

int radio_send(const uint8_t *frame, size_t len)
{
    char cmd[sizeof "AT+SEND=0,240,\r\n" + MAX_TEXT];
    if (st != S_IDLE || len == 0 || len > LMESH_MAX_FRAME) {
        return -1;
    }
    size_t text = B64_LEN(len);
    int n = snprintf(cmd, sizeof cmd, "AT+SEND=0,%u,", (unsigned)text);
    size_t total = (size_t)n + b64_encode(frame, len, cmd + n);
    cmd[total++] = '\r';
    cmd[total++] = '\n';
    if (!serial_write(&port, cmd, total)) {
        return -1;
    }
    snprintf(last_cmd, sizeof last_cmd, "AT+SEND=0,%u,...", (unsigned)text);
    st = S_SEND;
    done_ms = HAL_GetTick() + radio_airtime_ms(len);
    deadline = done_ms + SEND_GRACE_MS;
    return 0;
}

bool radio_poll(lmesh_node *node)
{
    bool did = false;
    int c;
    while ((c = serial_getc(&port)) >= 0) {
        if (c == '\n') {
            if (!line_overflow && line_len > 0) {
                line[line_len] = '\0';
                on_line(node, line);
                did = true;
            }
            line_len = 0;
            line_overflow = false;
        } else if (c == '\r') {
            continue;
        } else if (line_len < LINE_MAX - 1u) {
            line[line_len++] = (char)c;
        } else {
            line_overflow = true;
        }
    }

    uint32_t now = HAL_GetTick();
    bool due = (int32_t)(now - deadline) >= 0;
    switch (st) {
    case S_RESET:
        if (due) {
            board_radio_reset(false);
            st = S_BOOT;
            deadline = now + BOOT_MS;
        }
        break;
    case S_BOOT:
        /* No +READY: the reset line may not be wired, so try the module anyway. */
        if (due) {
            step = STEP_AT;
            run_step();
        }
        break;
    case S_CMD:
        if (due) {
            fail(node, "timeout");
        }
        break;
    case S_PAUSE:
        if (due) {
            start_reset();
        }
        break;
    case S_SEND:
        if (due) {
            fail(node, "timeout");
            did = true;
        }
        break;
    case S_ON_AIR:
        if ((int32_t)(now - done_ms) >= 0) {
            st = S_IDLE;
            lmesh_radio_tx_done(node, true);
            did = true;
        }
        break;
    case S_IDLE:
        break;
    }
    return did;
}
