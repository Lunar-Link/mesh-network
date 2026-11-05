/* Unit tests for the RYLR498 driver on a fake module that answers AT commands as REYAX's AT command guide describes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "console.h"
#include "radio.h"
#include "serial.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(cond)) {                                                                        \
            failures++;                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                     \
    } while (0)

/* A 120-byte frame at SF9, 125 kHz, CR 4/5, preamble 12: 160 characters plus 8 on air, 878 ms, plus 15 ms of UART and
   the 20 ms margin. A 30-byte frame: 40 characters plus 8, 325 ms, plus 5 ms and 20 ms. */
#define AIR_120_MS 913u
#define AIR_30_MS 350u

#define LOG_MAX 64
#define TEXT_MAX 400

UART_HandleTypeDef board_radio_uart;

static uint32_t now_ms;

/* The fake module: its settings in flash and in use, the commands it was sent, and the bytes it has to say. */
static struct {
    bool in_reset;
    bool silent;
    bool ready_on_reset;
    bool has_store; /* firmware 1.2.0 and later accept ",M" on AT+BAND */
    const char *send_reply;
    uint32_t band;
    uint32_t band_flash;
    char parameter[32];
    int crfop;
    int network;
    int mode;
    int flash_writes;
    char in[TEXT_MAX];
    size_t in_len;
    char out[4096];
    size_t out_head;
    size_t out_tail;
    char log[LOG_MAX][TEXT_MAX];
    int log_n;
    char sent[TEXT_MAX];
} mod;

static char last_line[300];
static int ready_lines;
static int error_lines;

static uint8_t got_frame[256];
static size_t got_len;
static int16_t got_rssi;
static int8_t got_snr;
static int rx_calls;
static int tx_ok_calls;
static int tx_fail_calls;
static lmesh_node node;

/* The driver's calls into the library land here, compiled in by a -D rename of the driver file only. */
void driver_radio_rx(lmesh_node *n, const uint8_t *frame, size_t len, int16_t rssi, int8_t snr);
void driver_radio_tx_done(lmesh_node *n, bool ok);

void driver_radio_rx(lmesh_node *n, const uint8_t *frame, size_t len, int16_t rssi, int8_t snr)
{
    (void)n;
    memcpy(got_frame, frame, len);
    got_len = len;
    got_rssi = rssi;
    got_snr = snr;
    rx_calls++;
}

void driver_radio_tx_done(lmesh_node *n, bool ok)
{
    (void)n;
    if (ok) {
        tx_ok_calls++;
    } else {
        tx_fail_calls++;
    }
}

uint32_t HAL_GetTick(void)
{
    return now_ms;
}

void console_line(const char *text)
{
    snprintf(last_line, sizeof last_line, "%s", text);
    if (strstr(text, "\"state\":\"ready\"")) {
        ready_lines++;
    }
    if (strstr(text, "\"state\":\"error\"")) {
        error_lines++;
    }
}

static void say(const char *text)
{
    for (const char *p = text; *p; p++) {
        mod.out[mod.out_head++ % sizeof mod.out] = *p;
    }
    mod.out[mod.out_head++ % sizeof mod.out] = '\r';
    mod.out[mod.out_head++ % sizeof mod.out] = '\n';
}

void board_radio_reset(bool asserted)
{
    if (asserted) {
        mod.in_reset = true;
        mod.in_len = 0;
        return;
    }
    if (mod.in_reset) {
        mod.in_reset = false;
        mod.band = mod.band_flash;
        if (mod.ready_on_reset) {
            say("+READY");
        }
    }
}

static bool starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void module_command(const char *c)
{
    char reply[TEXT_MAX];
    if (mod.log_n < LOG_MAX) {
        snprintf(mod.log[mod.log_n++], TEXT_MAX, "%s", c);
    }
    if (mod.silent) {
        return;
    }
    if (strcmp(c, "AT") == 0) {
        say("+OK");
    } else if (strcmp(c, "AT+VER?") == 0) {
        say("+VER=RYLR498_V1.2.3");
    } else if (strcmp(c, "AT+NETWORKID?") == 0) {
        snprintf(reply, sizeof reply, "+NETWORK=%d", mod.network); /* the guide's spelling of the reply */
        say(reply);
    } else if (starts(c, "AT+NETWORKID=")) {
        mod.network = atoi(c + 13);
        mod.flash_writes++;
        say("+OK");
    } else if (strcmp(c, "AT+BAND?") == 0) {
        snprintf(reply, sizeof reply, "+BAND=%lu", (unsigned long)mod.band);
        say(reply);
    } else if (starts(c, "AT+BAND=")) {
        const char *m = strchr(c, ',');
        if (m && (!mod.has_store || strcmp(m, ",M") != 0)) {
            say("+ERR=4");
            return;
        }
        mod.band = (uint32_t)strtoul(c + 8, NULL, 10);
        if (m) {
            mod.band_flash = mod.band;
            mod.flash_writes++;
        }
        say("+OK");
    } else if (strcmp(c, "AT+PARAMETER?") == 0) {
        snprintf(reply, sizeof reply, "+PARAMETER=%s", mod.parameter);
        say(reply);
    } else if (starts(c, "AT+PARAMETER=")) {
        snprintf(mod.parameter, sizeof mod.parameter, "%.31s", c + 13);
        mod.flash_writes++;
        say("+OK");
    } else if (strcmp(c, "AT+CRFOP?") == 0) {
        snprintf(reply, sizeof reply, "+CRFOP=%02d", mod.crfop);
        say(reply);
    } else if (starts(c, "AT+CRFOP=")) {
        mod.crfop = atoi(c + 9);
        mod.flash_writes++;
        say("+OK");
    } else if (strcmp(c, "AT+MODE?") == 0) {
        snprintf(reply, sizeof reply, "+MODE=%d", mod.mode);
        say(reply);
    } else if (starts(c, "AT+MODE=")) {
        mod.mode = atoi(c + 8);
        say("+OK");
    } else if (starts(c, "AT+SEND=")) {
        const char *len_text = strchr(c, ',') + 1;
        const char *data = strchr(len_text, ',') + 1;
        if ((size_t)atoi(len_text) != strlen(data)) {
            say("+ERR=5");
            return;
        }
        snprintf(mod.sent, sizeof mod.sent, "%s", data);
        say(mod.send_reply);
    } else {
        say("+ERR=4");
    }
}

void serial_init(serial *s, UART_HandleTypeDef *uart, uint8_t *rx, size_t rx_size, uint8_t *tx, size_t tx_size)
{
    (void)s;
    (void)uart;
    (void)rx;
    (void)rx_size;
    (void)tx;
    (void)tx_size;
}

int serial_getc(serial *s)
{
    (void)s;
    if (mod.out_tail == mod.out_head) {
        return -1;
    }
    return (unsigned char)mod.out[mod.out_tail++ % sizeof mod.out];
}

bool serial_write(serial *s, const void *data, size_t len)
{
    (void)s;
    const char *p = data;
    for (size_t i = 0; i < len; i++) {
        if (mod.in_reset) {
            continue;
        }
        if (p[i] == '\n' && mod.in_len > 0 && mod.in[mod.in_len - 1u] == '\r') {
            mod.in[mod.in_len - 1u] = '\0';
            module_command(mod.in);
            mod.in_len = 0;
        } else if (mod.in_len < sizeof mod.in - 1u) {
            mod.in[mod.in_len++] = p[i];
        }
    }
    return true;
}

static void run(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        radio_poll(&node);
        now_ms++;
    }
    radio_poll(&node);
}

static void factory_module(void)
{
    memset(&mod, 0, sizeof mod);
    mod.ready_on_reset = true;
    mod.has_store = true;
    mod.send_reply = "+OK";
    mod.band = mod.band_flash = 915000000u;
    snprintf(mod.parameter, sizeof mod.parameter, "9,7,1,12");
    mod.crfop = 22;
    mod.network = 18;
}

static bool logged(int i, const char *text)
{
    return i < mod.log_n && strcmp(mod.log[i], text) == 0;
}

/* From factory settings only the band differs, so it is the one write; a second start writes nothing. */
static void test_bring_up(void)
{
    factory_module();
    radio_init();
    CHECK(mod.in_reset);
    run(100);
    CHECK(ready_lines == 1);
    CHECK(mod.log_n == 8);
    CHECK(logged(0, "AT"));
    CHECK(logged(1, "AT+VER?"));
    CHECK(logged(2, "AT+NETWORKID?"));
    CHECK(logged(3, "AT+BAND?"));
    CHECK(logged(4, "AT+BAND=433000000,M"));
    CHECK(logged(5, "AT+PARAMETER?"));
    CHECK(logged(6, "AT+CRFOP?"));
    CHECK(logged(7, "AT+MODE?"));
    CHECK(mod.flash_writes == 1);
    CHECK(mod.band_flash == 433000000u);
    CHECK(strstr(last_line, "\"driver\":\"rylr498\"") != NULL);
    CHECK(strstr(last_line, "\"version\":\"RYLR498_V1.2.3\"") != NULL);
    CHECK(strstr(last_line, "\"band_stored\":true") != NULL);

    /* The module restarts on its own: it says +READY, and the driver checks again without writing. */
    mod.log_n = 0;
    say("+READY");
    run(100);
    CHECK(ready_lines == 2);
    CHECK(mod.log_n == 7);
    CHECK(mod.flash_writes == 1);
}

/* Firmware before 1.2.0 rejects ",M": the band goes in without it, and again after each reset. */
static void test_old_firmware(void)
{
    factory_module();
    mod.has_store = false;
    radio_init();
    run(100);
    CHECK(ready_lines == 3);
    CHECK(error_lines == 0);
    CHECK(logged(4, "AT+BAND=433000000,M"));
    CHECK(logged(5, "AT+BAND=433000000"));
    CHECK(mod.band == 433000000u);
    CHECK(mod.flash_writes == 0);
    CHECK(strstr(last_line, "\"band_stored\":false") != NULL);
}

/* A frame goes out as base64url, done no earlier than the estimate; a second send waits until then. */
static void test_send(void)
{
    uint8_t frame[LMESH_MAX_FRAME];
    for (size_t i = 0; i < sizeof frame; i++) {
        frame[i] = (uint8_t)(i * 37u + 5u);
    }
    CHECK(radio_airtime_ms(LMESH_MAX_FRAME) == AIR_120_MS);
    CHECK(radio_airtime_ms(30) == AIR_30_MS);
    CHECK(radio_send(frame, 0) != 0);
    CHECK(radio_send(frame, LMESH_MAX_FRAME + 1u) != 0);

    uint32_t start = now_ms;
    CHECK(radio_send(frame, sizeof frame) == 0);
    CHECK(strlen(mod.sent) == 160u);
    CHECK(radio_send(frame, sizeof frame) != 0);
    run(AIR_120_MS - 2u);
    CHECK(tx_ok_calls == 0);
    run(2);
    CHECK(tx_ok_calls == 1);
    CHECK(now_ms - start == AIR_120_MS);
    CHECK(radio_send(frame, 30) == 0);
    CHECK(strlen(mod.sent) == 40u);
    run(AIR_30_MS);
    CHECK(tx_ok_calls == 2);
    CHECK(tx_fail_calls == 0);
}

/* Every frame length survives encoding by radio_send and decoding of the same text from +RCV. */
static void test_round_trip(void)
{
    uint8_t frame[LMESH_MAX_FRAME];
    char text[TEXT_MAX + 32];
    int bad = 0;
    for (size_t len = 1; len <= LMESH_MAX_FRAME; len++) {
        for (size_t i = 0; i < len; i++) {
            frame[i] = (uint8_t)(len * 131u + i * 29u);
        }
        if (radio_send(frame, len) != 0) {
            bad++;
            continue;
        }
        run(radio_airtime_ms(len));
        snprintf(text, sizeof text, "+RCV=7,%u,%s,-99,40", (unsigned)strlen(mod.sent), mod.sent);
        got_len = 0;
        say(text);
        run(1);
        if (got_len != len || memcmp(got_frame, frame, len) != 0 || got_rssi != -99 || got_snr != 40) {
            bad++;
        }
    }
    CHECK(bad == 0);
}

/* The guide's example puts spaces after the commas; a bad length, character or field count is dropped. */
static void test_receive(void)
{
    rx_calls = 0;
    say("+RCV=50, 6, AAECAw, -110, -7");
    run(1);
    CHECK(rx_calls == 1);
    CHECK(got_len == 4u && got_frame[0] == 0 && got_frame[1] == 1 && got_frame[2] == 2 && got_frame[3] == 3);
    CHECK(got_rssi == -110 && got_snr == -7);
    say("+RCV=50,5,AAECAw,-110,-7");
    say("+RCV=50,6,AAEC+w,-110,-7");
    say("+RCV=50,6,AAECAw,-110");
    say("+RCV=50,6,AAECAw,-110,-7,1");
    say("+RCV=50,5,AAECA,-110,-7");
    run(1);
    CHECK(rx_calls == 1);
}

/* +ERR on a send fails that frame only; no reply at all fails it and restarts the module. */
static void test_send_errors(void)
{
    uint8_t frame[30] = {1, 2, 3};
    int ready = ready_lines;
    mod.send_reply = "+ERR=17";
    CHECK(radio_send(frame, sizeof frame) == 0);
    run(1);
    CHECK(tx_fail_calls == 1);
    CHECK(error_lines == 0);
    mod.send_reply = "+OK";
    CHECK(radio_send(frame, sizeof frame) == 0);
    run(AIR_30_MS);
    CHECK(tx_ok_calls >= 3);

    mod.silent = true;
    CHECK(radio_send(frame, sizeof frame) == 0);
    run(AIR_30_MS + 2000u);
    CHECK(tx_fail_calls == 2);
    CHECK(error_lines == 1);
    CHECK(strstr(last_line, "\"reply\":\"timeout\"") != NULL);
    CHECK(radio_send(frame, sizeof frame) != 0);
    mod.silent = false;
    run(2000u + 100u);
    CHECK(ready_lines == ready + 1);
    CHECK(radio_send(frame, sizeof frame) == 0);
    run(AIR_30_MS);
}

/* A module that restarts while a frame is on the air fails that frame and is set up again. */
static void test_ready_mid_send(void)
{
    uint8_t frame[30] = {9};
    int fails = tx_fail_calls;
    int ready = ready_lines;
    CHECK(radio_send(frame, sizeof frame) == 0);
    run(1);
    say("+READY");
    run(100);
    CHECK(tx_fail_calls == fails + 1);
    CHECK(ready_lines == ready + 1);
}

/* With no +READY after reset (reset line not wired), the driver tries the module after the boot time. */
static void test_no_ready(void)
{
    factory_module();
    mod.ready_on_reset = false;
    int ready = ready_lines;
    radio_init();
    run(2900);
    CHECK(mod.log_n == 0);
    run(200);
    CHECK(ready_lines == ready + 1);
}

/* A configuration error is reported and the driver retries from reset. */
static void test_config_error(void)
{
    factory_module();
    snprintf(mod.parameter, sizeof mod.parameter, "7,9,4,12");
    int ready = ready_lines;
    int errors = error_lines;
    radio_init();
    run(1);
    mod.silent = true;
    run(2100);
    CHECK(error_lines == errors + 1);
    mod.silent = false;
    run(2200);
    CHECK(ready_lines == ready + 1);
    CHECK(strcmp(mod.parameter, "9,7,1,12") == 0);
}

int main(void)
{
    test_bring_up();
    test_old_firmware();
    test_send();
    test_round_trip();
    test_receive();
    test_send_errors();
    test_ready_mid_send();
    test_no_ready();
    test_config_error();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
