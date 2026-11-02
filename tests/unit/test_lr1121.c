/* Unit tests for the LR1121 driver on a fake chip: command bytes against the user manual, BUSY, CAD through the real link layer. */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "link.h"
#include "lr1121.h"
#include "radio.h"

#ifndef TEST_HF
#define TEST_HF 0
#endif
#ifndef TEST_CW_MS
#define TEST_CW_MS 0
#endif

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

#define IRQ_TX_DONE (1ul << 2)
#define IRQ_RX_DONE (1ul << 3)
#define IRQ_PREAMBLE (1ul << 4)
#define IRQ_HEADER_VALID (1ul << 5)
#define IRQ_HEADER_ERROR (1ul << 6)
#define IRQ_CRC_ERROR (1ul << 7)
#define IRQ_CAD_DONE (1ul << 8)
#define IRQ_CAD_DETECTED (1ul << 9)

#define MODE_SLEEP 0
#define MODE_RC 1
#define MODE_XOSC 2
#define MODE_RX 4
#define MODE_TX 5

#define LOG_MAX 512
#define LOG_BYTES 16

/* The 30-byte and 120-byte airtimes from the time-on-air formula at SF9, 125 kHz, CR 4/5, preamble 12, rounded up. */
#define AIR_30_MS 243u
#define AIR_120_MS 653u
#define MARGIN_MS 10u

GPIO_TypeDef fake_port;
SPI_HandleTypeDef board_radio_spi;

static struct {
    uint32_t us;
    int mode;
    bool in_reset;
    uint32_t busy_until_us;
    bool hold_busy;
    uint32_t irq;
    uint32_t irq_mask;
    uint32_t tx_done_us;
    uint32_t cad_done_us;
    bool cad_detect;
    bool cad_hang;
    uint32_t irq_on_cad;
    bool cw;
    uint8_t reply[256];
    size_t reply_len;
    uint8_t cmd[300];
    size_t cmd_len;
    bool nss_low;
    bool read_phase;
    uint8_t tx_buf[256];
    size_t tx_len;
    uint8_t rx_buf[256];
    uint8_t rx_len;
    uint8_t rx_start;
    uint8_t rssi;
    uint8_t snr;
    uint16_t errors;
    uint16_t inject_errors;
    uint8_t use_case;
    uint8_t packet_len;
    bool packet_type_set;
    bool pa_set;
    bool tcxo_set;
    bool calibrated;
    int spi_timeout;
} chip;

static struct {
    uint16_t op;
    uint8_t bytes[LOG_BYTES];
    size_t len;
    int mode;
} cmd_log[LOG_MAX];
static int log_n;

static int nss_while_busy;
static int read_not_zero;
static int wrong_mode;
static int order_errors;
static char last_line[240];
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
static bool link_active;

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
    if (ok) {
        tx_ok_calls++;
    } else {
        tx_fail_calls++;
    }
    if (link_active) {
        lmesh_link_tx_done(n, ok);
    }
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

static void chip_step(void)
{
    if (chip.tx_done_us != 0 && (int32_t)(chip.us - chip.tx_done_us) >= 0) {
        chip.tx_done_us = 0;
        chip.irq |= IRQ_TX_DONE;
        chip.mode = MODE_XOSC;
    }
    if (chip.cad_done_us != 0 && (int32_t)(chip.us - chip.cad_done_us) >= 0) {
        chip.cad_done_us = 0;
        chip.irq |= IRQ_CAD_DONE | (chip.cad_detect ? IRQ_CAD_DETECTED : 0ul);
        chip.mode = MODE_RC;
    }
}

uint32_t HAL_GetTick(void)
{
    chip.us += 50u;
    chip_step();
    return chip.us / 1000u;
}

static void advance_ms(uint32_t ms)
{
    chip.us += ms * 1000u;
    chip_step();
}

static bool busy_level(void)
{
    return chip.in_reset || chip.hold_busy || chip.mode == MODE_SLEEP || (int32_t)(chip.busy_until_us - chip.us) > 0;
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    (void)port;
    chip_step();
    if (pin == RADIO_BUSY_PIN) {
        return busy_level() ? GPIO_PIN_SET : GPIO_PIN_RESET;
    }
    return (chip.irq & chip.irq_mask) != 0 ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

static void reply_with(const uint8_t *data, size_t n)
{
    memcpy(chip.reply, data, n);
    chip.reply_len = n;
}

static bool in_standby(void)
{
    return chip.mode == MODE_RC || chip.mode == MODE_XOSC;
}

static void run_command(void)
{
    const uint8_t *c = chip.cmd;
    uint16_t op = (uint16_t)(c[0] << 8 | c[1]);
    uint32_t busy_us = 100u;

    if (log_n < LOG_MAX) {
        cmd_log[log_n].op = op;
        cmd_log[log_n].len = chip.cmd_len;
        cmd_log[log_n].mode = chip.mode;
        memcpy(cmd_log[log_n].bytes, c, chip.cmd_len < LOG_BYTES ? chip.cmd_len : LOG_BYTES);
        log_n++;
    }
    switch (op) {
    case 0x0101: {
        const uint8_t v[4] = {0x22, chip.use_case, 0x01, 0x03};
        reply_with(v, sizeof v);
        break;
    }
    case 0x0109:
        chip.tx_len = chip.cmd_len - 2u;
        memcpy(chip.tx_buf, c + 2, chip.tx_len);
        break;
    case 0x010A:
        reply_with(&chip.rx_buf[c[2]], c[3]);
        break;
    case 0x010D: {
        const uint8_t e[2] = {(uint8_t)(chip.errors >> 8), (uint8_t)chip.errors};
        reply_with(e, sizeof e);
        break;
    }
    case 0x010E:
        chip.errors = 0;
        break;
    case 0x010F:
        order_errors += chip.tcxo_set ? 0 : 1;
        chip.calibrated = true;
        chip.errors = chip.inject_errors;
        chip.mode = MODE_RC;
        busy_us = 5000u;
        break;
    case 0x0110:
    case 0x0112:
        wrong_mode += chip.mode == MODE_RC ? 0 : 1;
        break;
    case 0x0111:
        order_errors += chip.calibrated ? 0 : 1;
        chip.mode = MODE_RC;
        busy_us = 3000u;
        break;
    case 0x0113:
        chip.irq_mask = (uint32_t)c[2] << 24 | (uint32_t)c[3] << 16 | (uint32_t)c[4] << 8 | c[5];
        break;
    case 0x0114:
        chip.irq &= ~((uint32_t)c[2] << 24 | (uint32_t)c[3] << 16 | (uint32_t)c[4] << 8 | c[5]);
        break;
    case 0x0117:
        wrong_mode += chip.mode == MODE_RC ? 0 : 1;
        chip.tcxo_set = true;
        break;
    case 0x011B:
        chip.mode = MODE_SLEEP;
        break;
    case 0x011C:
        chip.mode = c[2] == 0x01 ? MODE_XOSC : MODE_RC;
        chip.cad_done_us = 0;
        break;
    case 0x0203: {
        const uint8_t r[2] = {chip.rx_len, chip.rx_start};
        reply_with(r, sizeof r);
        break;
    }
    case 0x0204: {
        const uint8_t r[3] = {chip.rssi, chip.snr, chip.rssi};
        reply_with(r, sizeof r);
        break;
    }
    case 0x0209:
        chip.mode = MODE_RX;
        break;
    case 0x020A:
        order_errors += chip.packet_len == chip.tx_len ? 0 : 1;
        chip.mode = MODE_TX;
        chip.tx_done_us = chip.us + 50000u;
        break;
    case 0x020E:
        wrong_mode += in_standby() ? 0 : 1;
        chip.packet_type_set = true;
        break;
    case 0x020F:
    case 0x020B:
        order_errors += chip.packet_type_set ? 0 : 1;
        break;
    case 0x0210:
        order_errors += chip.packet_type_set ? 0 : 1;
        chip.packet_len = c[5];
        break;
    case 0x0211:
        order_errors += chip.pa_set ? 0 : 1;
        break;
    case 0x0215:
        chip.pa_set = true;
        break;
    case 0x0218:
        wrong_mode += in_standby() ? 0 : 1;
        chip.mode = MODE_RX;
        chip.irq |= chip.irq_on_cad;
        if (!chip.cad_hang) {
            chip.cad_done_us = chip.us + 17000u;
        }
        break;
    case 0x0219:
        chip.mode = MODE_TX;
        chip.cw = true;
        break;
    default:
        break;
    }
    chip.busy_until_us = chip.us + busy_us;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    (void)port;
    if (pin != RADIO_NSS_PIN) {
        return;
    }
    if (state == GPIO_PIN_RESET) {
        nss_while_busy += busy_level() ? 1 : 0;
        chip.nss_low = true;
        chip.read_phase = chip.reply_len > 0;
        chip.cmd_len = 0;
    } else {
        chip.nss_low = false;
        if (chip.read_phase) {
            chip.reply_len = 0;
        } else {
            run_command();
        }
    }
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *h, uint8_t *tx, uint8_t *rx, uint16_t n, uint32_t timeout)
{
    (void)h;
    (void)timeout;
    if (chip.spi_timeout > 0) {
        chip.spi_timeout--;
        return HAL_ERROR;
    }
    if (chip.read_phase) {
        rx[0] = 0x04;
        for (size_t i = 1; i < n; i++) {
            rx[i] = chip.reply[i - 1u];
            read_not_zero += tx[i] == 0 ? 0 : 1;
        }
        return HAL_OK;
    }
    const uint8_t status[6] = {0x04, (uint8_t)(chip.mode << 1 | 1), (uint8_t)(chip.irq >> 24), (uint8_t)(chip.irq >> 16),
                               (uint8_t)(chip.irq >> 8), (uint8_t)chip.irq};
    for (size_t i = 0; i < n; i++) {
        rx[i] = i < sizeof status ? status[i] : 0;
    }
    memcpy(chip.cmd, tx, n);
    chip.cmd_len = n;
    return HAL_OK;
}

void board_radio_reset(bool asserted)
{
    if (asserted) {
        uint32_t now = chip.us;
        uint8_t use_case = chip.use_case;
        uint16_t inject = chip.inject_errors;
        bool hang = chip.cad_hang;
        memset(&chip, 0, sizeof chip);
        chip.us = now;
        chip.use_case = use_case;
        chip.inject_errors = inject;
        chip.cad_hang = hang;
        chip.errors = 0x0020;
        chip.in_reset = true;
    } else {
        chip.in_reset = false;
        chip.mode = MODE_RC;
        chip.busy_until_us = chip.us + 30000u;
    }
}

static int find_op(uint16_t op, int from)
{
    for (int i = from; i < log_n; i++) {
        if (cmd_log[i].op == op) {
            return i;
        }
    }
    return -1;
}

static int count_op(uint16_t op, int from)
{
    int n = 0;
    for (int i = from; i < log_n; i++) {
        n += cmd_log[i].op == op ? 1 : 0;
    }
    return n;
}

static bool sent(const uint8_t *bytes, size_t n)
{
    for (int i = 0; i < log_n; i++) {
        if (cmd_log[i].len == n && memcmp(cmd_log[i].bytes, bytes, n) == 0) {
            return true;
        }
    }
    return false;
}

#define SENT(...)                                         \
    do {                                                  \
        const uint8_t expected_[] = {__VA_ARGS__};        \
        CHECK(sent(expected_, sizeof expected_));         \
    } while (0)

static void poll_for(uint32_t ms)
{
    uint32_t end = HAL_GetTick() + ms;
    while ((int32_t)(HAL_GetTick() - end) < 0) {
        (void)radio_poll(&node);
        advance_ms(1);
    }
}

static void start_fresh(void)
{
    uint32_t us = chip.us;
    memset(&chip, 0, sizeof chip);
    chip.us = us;
    chip.use_case = 0x03;
    log_n = 0;
    nss_while_busy = 0;
    read_not_zero = 0;
    wrong_mode = 0;
    order_errors = 0;
    ready_lines = 0;
    error_lines = 0;
    rx_calls = 0;
    tx_ok_calls = 0;
    tx_fail_calls = 0;
    got_len = 0;
    link_active = false;
    last_line[0] = '\0';
}

static void bring_up(void)
{
    start_fresh();
    radio_init();
    poll_for(300u + TEST_CW_MS);
    CHECK(ready_lines == 1);
    CHECK(chip.mode == MODE_RX);
}

static void clear_log(void)
{
    log_n = 0;
}

static void test_bring_up_bytes(void)
{
    bring_up();

    SENT(0x01, 0x10, 0x01);
    SENT(0x01, 0x12, 0x03, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00);
    SENT(0x01, 0x17, 0x06, 0x00, 0x01, 0x2C);
    SENT(0x01, 0x0F, 0x3F);
    SENT(0x02, 0x0E, 0x02);
    SENT(0x02, 0x0F, 0x09, 0x04, 0x01, 0x00);
    SENT(0x02, 0x10, 0x00, 0x0C, 0x00, LMESH_MAX_FRAME, 0x01, 0x00);
    SENT(0x02, 0x13, 0x02);
    SENT(0x02, 0x27, 0x01);
    SENT(0x02, 0x2B, 0x12);
    SENT(0x02, 0x0D, 0x04, 0x35, 0x0A, 0x00, 0x00, 0x00, 0x00);
    SENT(0x02, 0x09, 0xFF, 0xFF, 0xFF);
#if TEST_HF
    SENT(0x02, 0x0B, 0x90, 0x8A, 0x90, 0x40);
    SENT(0x02, 0x15, 0x02, 0x00, 0x00, 0x00);
    SENT(0x02, 0x11, 0x0D, 0x02);
    SENT(0x02, 0x29, 0x76, 0x46, 0x43, 0xCE, 0xCE, 0xCC, 0x8C, 0x98, 0x09, 0x07, 0xEE);
    CHECK(find_op(0x0111, 0) < 0);
#else
    SENT(0x02, 0x0B, 0x19, 0xCF, 0x0E, 0x40);
    SENT(0x02, 0x15, 0x01, 0x01, 0x04, 0x07);
    SENT(0x02, 0x11, 0x16, 0x02);
    SENT(0x02, 0x29, 0xCC, 0x0E, 0x31, 0x44, 0x63, 0x66, 0x66, 0x66, 0x06, 0x00, 0x00);
    SENT(0x01, 0x11, 0x6B, 0x6E);
    CHECK(find_op(0x010F, 0) < find_op(0x0111, 0));
#endif

    CHECK(find_op(0x0101, 0) == 0);
    CHECK(find_op(0x0117, 0) >= 0 && find_op(0x0117, 0) < find_op(0x010F, 0));
    CHECK(find_op(0x0112, 0) < find_op(0x0117, 0));
    CHECK(find_op(0x010E, 0) < find_op(0x010F, 0));
    CHECK(find_op(0x010F, 0) < find_op(0x010D, 0));
    CHECK(find_op(0x020E, 0) < find_op(0x020F, 0));
    CHECK(find_op(0x020E, 0) < find_op(0x0210, 0));
    CHECK(find_op(0x0215, 0) < find_op(0x0211, 0));
    CHECK(find_op(0x0113, 0) < find_op(0x0209, 0));

    CHECK(nss_while_busy == 0);
    CHECK(read_not_zero == 0);
    CHECK(wrong_mode == 0);
    CHECK(order_errors == 0);
    CHECK(strstr(last_line, "\"driver\":\"lr1121\"") != NULL);
    CHECK(strstr(last_line, "\"bw\":7") != NULL);
    CHECK(strstr(last_line, "\"sf\":9") != NULL);
    CHECK(strstr(last_line, "\"version\":\"22.03.01.03\"") != NULL);
}

static void test_irq_mask(void)
{
    bring_up();
    int i = find_op(0x0113, 0);
    CHECK(i >= 0);
    const uint8_t *b = cmd_log[i].bytes;
    uint32_t irq1 = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5];
    CHECK(cmd_log[i].len == 10);
    CHECK((irq1 & (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_PREAMBLE | IRQ_HEADER_VALID | IRQ_HEADER_ERROR | IRQ_CRC_ERROR |
                   IRQ_CAD_DONE | IRQ_CAD_DETECTED)) == (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_PREAMBLE | IRQ_HEADER_VALID |
                                                         IRQ_HEADER_ERROR | IRQ_CRC_ERROR | IRQ_CAD_DONE | IRQ_CAD_DETECTED));
    for (size_t k = 6; k < 10; k++) {
        CHECK(b[k] == 0);
    }
}

static void test_busy_respected(void)
{
    start_fresh();
    radio_init();
    poll_for(15);
    CHECK(log_n == 0);
    poll_for(300);
    CHECK(ready_lines == 1);
    CHECK(nss_while_busy == 0);

    clear_log();
    chip.busy_until_us = chip.us + 5000u;
    uint8_t frame[3] = {1, 2, 3};
    int before = nss_while_busy;
    CHECK(radio_send(frame, sizeof frame) == 0);
    CHECK(nss_while_busy == before);
    CHECK(chip.mode == MODE_TX);

    start_fresh();
    radio_init();
    chip.use_case = 0x03;
    poll_for(60);
    int logged = log_n;
    chip.hold_busy = true;
    poll_for(200);
    CHECK(log_n == logged);
    CHECK(nss_while_busy == 0);
    poll_for(1000);
    CHECK(error_lines == 1);
    CHECK(strstr(last_line, "busy timeout") != NULL);
    chip.hold_busy = false;
    poll_for(2300);
    CHECK(ready_lines == 1);
}

static void test_bad_chip(void)
{
    start_fresh();
    chip.use_case = 0xDF;
    radio_init();
    poll_for(100);
    CHECK(error_lines == 1);
    CHECK(strstr(last_line, "not an LR1121") != NULL);
    CHECK(radio_send((const uint8_t *)"x", 1) == -1);
    CHECK(find_op(0x0117, 0) < 0);

    start_fresh();
    chip.inject_errors = 0x0020;
    radio_init();
    poll_for(100);
    CHECK(error_lines == 1);
    CHECK(strstr(last_line, "errors 0x0020") != NULL);
    CHECK(ready_lines == 0);
    CHECK(find_op(0x020E, 0) < 0);

    start_fresh();
    chip.inject_errors = 0x0040;
    radio_init();
    poll_for(300);
    CHECK(ready_lines == 1);
}

static void test_send_raw_frame(void)
{
    bring_up();
    uint8_t frame[LMESH_MAX_FRAME];
    for (size_t i = 0; i < sizeof frame; i++) {
        frame[i] = (uint8_t)(i * 7u + 0x80u);
    }

    clear_log();
    CHECK(radio_send(frame, 30) == 0);
    SENT(0x01, 0x1C, 0x01);
    SENT(0x02, 0x10, 0x00, 0x0C, 0x00, 30, 0x01, 0x00);
    SENT(0x02, 0x0A, 0x00, 0x00, 0x00);
    CHECK(chip.tx_len == 30);
    CHECK(memcmp(chip.tx_buf, frame, 30) == 0);
    CHECK(find_op(0x0109, 0) < find_op(0x020A, 0));
    CHECK(find_op(0x0210, 0) < find_op(0x020A, 0));
    CHECK(order_errors == 0);
    CHECK(radio_send(frame, 30) == -1);

    clear_log();
    poll_for(60);
    CHECK(tx_ok_calls == 1);
    CHECK(tx_fail_calls == 0);
    CHECK(chip.mode == MODE_RX);
    SENT(0x02, 0x10, 0x00, 0x0C, 0x00, LMESH_MAX_FRAME, 0x01, 0x00);
    SENT(0x02, 0x09, 0xFF, 0xFF, 0xFF);

    CHECK(radio_send(frame, sizeof frame) == 0);
    CHECK(chip.tx_len == sizeof frame);
    CHECK(memcmp(chip.tx_buf, frame, sizeof frame) == 0);
    poll_for(60);
    CHECK(tx_ok_calls == 2);

    CHECK(radio_send(frame, 0) == -1);
    CHECK(radio_send(frame, LMESH_MAX_FRAME + 1u) == -1);
}

static void test_tx_never_finishes(void)
{
    bring_up();
    uint8_t frame[4] = {9, 8, 7, 6};
    CHECK(radio_send(frame, sizeof frame) == 0);
    chip.tx_done_us = 0;
    poll_for(radio_airtime_ms(sizeof frame) + 1900u);
    CHECK(tx_fail_calls == 0);
    poll_for(300);
    CHECK(tx_fail_calls == 1);
    CHECK(error_lines == 1);
    CHECK(strstr(last_line, "tx timeout") != NULL);
    poll_for(2400);
    CHECK(ready_lines == 2);
}

static void load_rx(uint8_t start, uint8_t len, uint8_t rssi, uint8_t snr)
{
    for (unsigned i = 0; i < len; i++) {
        chip.rx_buf[(uint8_t)(start + i)] = (uint8_t)(i ^ 0xA5u);
    }
    chip.rx_start = start;
    chip.rx_len = len;
    chip.rssi = rssi;
    chip.snr = snr;
}

static bool frame_ok(uint8_t len)
{
    for (unsigned i = 0; i < len; i++) {
        if (got_frame[i] != (uint8_t)(i ^ 0xA5u)) {
            return false;
        }
    }
    return got_len == len;
}

static void test_receive(void)
{
    bring_up();
    load_rx(0, 40, 164, 0x18);
    chip.irq |= IRQ_RX_DONE;
    poll_for(3);
    CHECK(rx_calls == 1);
    CHECK(frame_ok(40));
    CHECK(got_rssi == -82);
    CHECK(got_snr == 6);
    CHECK(chip.irq == 0);
    SENT(0x01, 0x0A, 0x00, 40);

    load_rx(200, 100, 200, 0xE8);
    chip.irq |= IRQ_RX_DONE;
    poll_for(3);
    CHECK(rx_calls == 2);
    CHECK(frame_ok(100));
    CHECK(got_rssi == -100);
    CHECK(got_snr == -6);
    SENT(0x01, 0x0A, 200, 56);
    SENT(0x01, 0x0A, 0, 44);

    load_rx(0, 40, 164, 0x18);
    chip.irq |= IRQ_RX_DONE | IRQ_CRC_ERROR;
    poll_for(3);
    CHECK(rx_calls == 2);

    chip.irq |= IRQ_HEADER_ERROR;
    poll_for(3);
    CHECK(rx_calls == 2);

    load_rx(0, LMESH_MAX_FRAME + 1u, 164, 0x18);
    chip.irq |= IRQ_RX_DONE;
    poll_for(3);
    CHECK(rx_calls == 2);
    CHECK(read_not_zero == 0);
}

static void test_airtime(void)
{
    CHECK(radio_airtime_ms(30) == AIR_30_MS + MARGIN_MS);
    CHECK(radio_airtime_ms(120) == AIR_120_MS + MARGIN_MS);
}

static void test_cad_clear_then_send(void)
{
    bring_up();
    clear_log();
    CHECK(!radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 1);
    SENT(0x01, 0x1C, 0x01);
    SENT(0x02, 0x18);
    CHECK(find_op(0x011C, 0) < find_op(0x0218, 0));
    CHECK(wrong_mode == 0);
    CHECK(chip.irq == 0);
    CHECK(find_op(0x020A, 0) < 0);

    uint8_t frame[5] = {1, 2, 3, 4, 5};
    CHECK(radio_send(frame, sizeof frame) == 0);
    CHECK(chip.mode == MODE_TX);
    poll_for(60);
    CHECK(tx_ok_calls == 1);

    clear_log();
    CHECK(!radio_channel_busy());
    poll_for(3);
    CHECK(chip.mode == MODE_RX);
}

static void test_cad_detected(void)
{
    bring_up();
    clear_log();
    chip.cad_detect = true;
    CHECK(radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 1);
    CHECK(chip.mode == MODE_RX);
    CHECK(find_op(0x0218, 0) < find_op(0x0209, 0));
    CHECK(find_op(0x020A, 0) < 0);
    CHECK(chip.irq == 0);
}

static void test_cad_during_reception(void)
{
    bring_up();
    clear_log();

    chip.irq |= IRQ_PREAMBLE;
    CHECK(radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 0);
    poll_for(3);
    CHECK(chip.irq == 0);
    CHECK(radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 0);
    CHECK(chip.mode == MODE_RX);

    chip.irq |= IRQ_HEADER_VALID;
    poll_for(3);
    poll_for(200);
    CHECK(radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 0);

    load_rx(0, 40, 164, 0x18);
    chip.irq |= IRQ_RX_DONE;
    poll_for(3);
    CHECK(rx_calls == 1);
    CHECK(!radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 1);

    bring_up();
    clear_log();
    chip.irq |= IRQ_PREAMBLE;
    poll_for(3);
    poll_for(120);
    CHECK(!radio_channel_busy());
    CHECK(count_op(0x0218, 0) == 1);
}

static void test_flag_during_cad_does_not_reset(void)
{
    bring_up();
    clear_log();
    chip.irq_on_cad = IRQ_PREAMBLE;
    CHECK(radio_channel_busy());
    CHECK(error_lines == 0);
    CHECK(chip.irq == 0);
    CHECK(chip.mode == MODE_RX);

    bring_up();
    clear_log();
    chip.irq_on_cad = IRQ_TX_DONE;
    CHECK(!radio_channel_busy());
    CHECK(chip.cad_done_us == 0);
    CHECK(error_lines == 0);
    CHECK(radio_send((const uint8_t *)"x", 1) == 0);

    bring_up();
    clear_log();
    chip.irq_on_cad = IRQ_TX_DONE;
    chip.cad_detect = true;
    CHECK(radio_channel_busy());
    CHECK(chip.cad_done_us == 0);
}

static void test_cad_hang(void)
{
    bring_up();
    chip.cad_hang = true;
    CHECK(!radio_channel_busy());
    CHECK(error_lines == 1);
    CHECK(strstr(last_line, "cad timeout") != NULL);
    CHECK(radio_send((const uint8_t *)"x", 1) == -1);
    CHECK(!radio_channel_busy());
    chip.cad_hang = false;
    poll_for(2400);
    CHECK(ready_lines == 2);
    CHECK(radio_send((const uint8_t *)"x", 1) == 0);
}

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

static uint32_t rnd(void *ctx)
{
    static uint32_t state = 12345;
    (void)ctx;
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

static int port_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return radio_send(frame, len);
}

static bool port_busy(void *ctx)
{
    (void)ctx;
    return radio_channel_busy();
}

static uint32_t port_airtime(void *ctx, size_t len)
{
    (void)ctx;
    return radio_airtime_ms(len);
}

static void link_setup(void)
{
    bring_up();
    memset(&node, 0, sizeof node);
    node.ident.id = 1;
    node.ident.network_id = 0x5A;
    node.port.now_ms = now_ms;
    node.port.random = rnd;
    node.port.radio_send = port_send;
    node.port.radio_busy = port_busy;
    node.port.airtime_ms = port_airtime;
    node.link.in_flight = -1;
    node.now_ms = HAL_GetTick();
    node.next_wake_ms = node.now_ms + LMESH_TICK_MAX_SLEEP_MS;
    link_active = true;
    clear_log();
}

static void link_tick(void)
{
    node.now_ms = HAL_GetTick();
    node.next_wake_ms = node.now_ms + LMESH_TICK_MAX_SLEEP_MS;
    lmesh_link_tick(&node);
}

static void test_link_defers_like_the_harness(void)
{
    uint32_t slot = radio_airtime_ms(LMESH_MAX_FRAME);
    bool seen[5] = {false, false, false, false, false};

    for (int run = 0; run < 64; run++) {
        link_setup();
        uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        CHECK(lmesh_link_send(&node, LMESH_FT_HELLO, LMESH_ID_ALL, payload, sizeof payload, 0, node.now_ms, false,
                              LMESH_OWNER_NONE, 0) == LMESH_OK);

        chip.cad_detect = true;
        link_tick();
        CHECK(count_op(0x0218, 0) == 1);
        CHECK(find_op(0x020A, 0) < 0);
        CHECK(!node.link.tx_active);
        CHECK(node.link.in_flight < 0);
        CHECK(chip.mode == MODE_RX);

        uint32_t wait = node.link.queue[0].due_ms - node.now_ms;
        uint32_t slots = wait / slot;
        CHECK(wait % slot == 0);
        CHECK(slots >= 1 && slots <= 4);
        if (slots >= 1 && slots <= 4) {
            seen[slots] = true;
        }

        chip.cad_detect = false;
        link_tick();
        CHECK(count_op(0x0218, 0) == 1);
        CHECK(find_op(0x020A, 0) < 0);

        advance_ms(wait + 1u);
        link_tick();
        CHECK(count_op(0x0218, 0) == 2);
        CHECK(find_op(0x0218, 0) < find_op(0x020A, 0));
        CHECK(node.link.tx_active);
        CHECK(chip.tx_len == node.link.queue[0].frame.len);
        CHECK(memcmp(chip.tx_buf, node.link.queue[0].frame.buf, chip.tx_len) == 0);

        poll_for(60);
        CHECK(!node.link.tx_active);
        CHECK(tx_ok_calls == 1);
    }
    for (int i = 1; i <= 4; i++) {
        CHECK(seen[i]);
    }
}

static void test_ack_skips_cad(void)
{
    link_setup();
    lmesh_link_send_ack(&node, 2, 7, LMESH_ACK_ACCEPTED);
    CHECK(count_op(0x0218, 0) == 0);
    CHECK(find_op(0x020A, 0) >= 0);
    CHECK(node.link.tx_active);
    poll_for(60);
    CHECK(!node.link.tx_active);
}

static void test_ack_goes_out_while_frame_deferred(void)
{
    link_setup();
    uint8_t payload[2] = {1, 2};
    CHECK(lmesh_link_send(&node, LMESH_FT_HELLO, LMESH_ID_ALL, payload, sizeof payload, 0, node.now_ms, false,
                          LMESH_OWNER_NONE, 0) == LMESH_OK);
    chip.cad_detect = true;
    link_tick();
    CHECK(find_op(0x020A, 0) < 0);

    lmesh_link_send_ack(&node, 2, 7, LMESH_ACK_ACCEPTED);
    CHECK(find_op(0x020A, 0) >= 0);
    CHECK(node.link.tx_active);
    CHECK(node.link.queue[0].used);
    CHECK(node.link.in_flight < 0);
}

static void test_sleep(void)
{
    bring_up();
    clear_log();
    lr1121_sleep();
    SENT(0x01, 0x1B, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(chip.mode == MODE_SLEEP);
    int logged = log_n;
    CHECK(radio_send((const uint8_t *)"x", 1) == -1);
    CHECK(!radio_channel_busy());
    poll_for(50);
    CHECK(log_n == logged);
    CHECK(nss_while_busy == 0);

    lr1121_wake();
    poll_for(300);
    CHECK(ready_lines == 2);
    CHECK(chip.mode == MODE_RX);
    CHECK(radio_send((const uint8_t *)"x", 1) == 0);
}

static void test_spi_failure(void)
{
    bring_up();
    chip.spi_timeout = 1;
    CHECK(radio_send((const uint8_t *)"x", 1) == -1);
    CHECK(error_lines == 1);
    poll_for(2400);
    CHECK(ready_lines == 2);
}

#if TEST_CW_MS > 0
static void test_cw(void)
{
    start_fresh();
    radio_init();
    for (int i = 0; i < 300 && !chip.cw; i++) {
        poll_for(1);
    }
    CHECK(ready_lines == 1);
    CHECK(chip.cw);
    CHECK(find_op(0x0219, 0) >= 0);
    CHECK(chip.mode == MODE_TX);
    CHECK(radio_send((const uint8_t *)"x", 1) == -1);
    poll_for(TEST_CW_MS + 50u);
    CHECK(chip.mode == MODE_RX);
    CHECK(radio_send((const uint8_t *)"x", 1) == 0);
}
#endif

int main(void)
{
    test_bring_up_bytes();
    test_irq_mask();
    test_airtime();
    test_busy_respected();
    test_bad_chip();
    test_send_raw_frame();
    test_tx_never_finishes();
    test_receive();
    test_cad_clear_then_send();
    test_cad_detected();
    test_cad_during_reception();
    test_flag_during_cad_does_not_reset();
    test_cad_hang();
    test_link_defers_like_the_harness();
    test_ack_skips_cad();
    test_ack_goes_out_while_frame_deferred();
    test_sleep();
    test_spi_failure();
#if TEST_CW_MS > 0
    test_cw();
#endif
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
