/* Waveshare Core1121-LF driver (Semtech LR1121): SPI commands with BUSY handling, raw LoRa frames, CAD channel sensing. */
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "console.h"
#include "lr1121.h"
#include "radio.h"

/* RF settings, each overridable with a -D flag; every module in the network must match. */
#ifndef LR1121_HF
#define LR1121_HF 0 /* 0: sub-GHz on the LORA_ANT connector, 1: S-band or 2.4 GHz on the 2.4G_ANT connector */
#endif

/* One block per RF path: frequency limits, PA, image calibration and RSSI calibration. */
#if LR1121_HF
#ifndef LR1121_BAND_HZ
#define LR1121_BAND_HZ 2425000000 /* inside both the 2.4 GHz ISM band and the 13 cm amateur band */
#endif
#ifndef LR1121_POWER_DBM
#define LR1121_POWER_DBM 13
#endif
#ifndef LR1121_PA_DUTY
#define LR1121_PA_DUTY 0
#endif
#ifndef LR1121_PA_HP_SEL
#define LR1121_PA_HP_SEL 0
#endif
#define PA_SEL 2u    /* the HF PA on RFIO_HF */
#define PA_SUPPLY 0u /* internal regulator */
#define BAND_OK ((LR1121_BAND_HZ >= 1900000000 && LR1121_BAND_HZ <= 2100000000) || \
                 (LR1121_BAND_HZ >= 2400000000 && LR1121_BAND_HZ <= 2500000000))
#define POWER_OK (LR1121_POWER_DBM >= -18 && LR1121_POWER_DBM <= 13)
#define PA_OK (LR1121_PA_DUTY <= 4 && LR1121_PA_HP_SEL == 0)
#if LR1121_BAND_HZ > 2000000000
#define RSSI_TUNES 0x76, 0x46, 0x43, 0xCE, 0xCE, 0xCC, 0x8C, 0x98, 0x09, 0x07, 0xEE
#else
#define RSSI_TUNES 0x22, 0x32, 0x43, 0x45, 0x64, 0x55, 0x66, 0x76, 0x06, 0x00, 0x00
#endif
#else
#ifndef LR1121_BAND_HZ
#define LR1121_BAND_HZ 433000000
#endif
#ifndef LR1121_POWER_DBM
#define LR1121_POWER_DBM 22
#endif
#ifndef LR1121_PA_DUTY
#define LR1121_PA_DUTY 4
#endif
#ifndef LR1121_PA_HP_SEL
#define LR1121_PA_HP_SEL 7
#endif
#ifndef LR1121_CAL_FREQ1
#define LR1121_CAL_FREQ1 0x6B /* image calibration from this frequency, in 4 MHz steps */
#endif
#ifndef LR1121_CAL_FREQ2
#define LR1121_CAL_FREQ2 0x6E /* to this one */
#endif
#define PA_SEL 1u    /* the HP PA on RFO_HP_LF, the path the module populates */
#define PA_SUPPLY 1u /* VBAT, required above +14 dBm */
#define BAND_OK (LR1121_BAND_HZ >= 410000000 && LR1121_BAND_HZ <= 510000000)
#define POWER_OK (LR1121_POWER_DBM >= -9 && LR1121_POWER_DBM <= 22)
#define PA_OK (LR1121_PA_DUTY <= (LR1121_PA_HP_SEL >= 5 ? 4 : 7) && LR1121_PA_HP_SEL <= 7)
#define RSSI_TUNES 0xCC, 0x0E, 0x31, 0x44, 0x63, 0x66, 0x66, 0x66, 0x06, 0x00, 0x00
#endif

#ifndef LR1121_SF
#define LR1121_SF 9
#endif
#ifndef LR1121_BW
#define LR1121_BW 4 /* 3: 62.5 kHz, 4: 125, 5: 250, 6: 500; 2.4 GHz only: 13: 203, 14: 406, 15: 812 */
#endif
#ifndef LR1121_CR
#define LR1121_CR 1 /* coding rate 4/(4+CR) */
#endif
#ifndef LR1121_PREAMBLE
#define LR1121_PREAMBLE 12
#endif
#ifndef LR1121_SYNC_WORD
#define LR1121_SYNC_WORD 0x12
#endif
#ifndef LR1121_RAMP
#define LR1121_RAMP 2 /* 48 us, the manual's trade-off between rise time and spurious emissions */
#endif
#ifndef LR1121_RX_BOOST
#define LR1121_RX_BOOST 1
#endif
#ifndef LR1121_REG_MODE
#define LR1121_REG_MODE 1 /* DC-DC: the module fits the inductor on DCC_SW */
#endif
#ifndef LR1121_TCXO_TUNE
#define LR1121_TCXO_TUNE 6 /* 3.0 V, from Waveshare's demos; the schematic does not name the part */
#endif
#ifndef LR1121_TCXO_DELAY
#define LR1121_TCXO_DELAY 300 /* in 30.52 us steps, from Waveshare's demos */
#endif
#ifndef LR1121_ERROR_MASK
#define LR1121_ERROR_MASK 0x01BFu /* every GetErrors flag except the unused 32 kHz crystal */
#endif

/* RF switch: DIO5 and DIO6 drive the PE4259; bit 0 is DIO5 and selects receive, bit 1 is DIO6 and selects the HP PA */
#ifndef LR1121_RFSW_ENABLE
#define LR1121_RFSW_ENABLE 0x03
#endif
#ifndef LR1121_RFSW_STANDBY
#define LR1121_RFSW_STANDBY 0x00
#endif
#ifndef LR1121_RFSW_RX
#define LR1121_RFSW_RX 0x01
#endif
#ifndef LR1121_RFSW_TX
#define LR1121_RFSW_TX 0x02
#endif
#ifndef LR1121_RFSW_TX_HP
#define LR1121_RFSW_TX_HP 0x02
#endif
#ifndef LR1121_RFSW_TX_HF
#define LR1121_RFSW_TX_HF 0x00
#endif

/* Channel sensing before queued frames; set LR1121_CAD to 0 to send without it. */
#ifndef LR1121_CAD
#define LR1121_CAD 1
#endif
#ifndef LR1121_CAD_SYMBOLS
#define LR1121_CAD_SYMBOLS 4
#endif
#ifndef LR1121_CAD_PEAK
#if LR1121_SF == 9 && LR1121_BW == 4 && LR1121_CAD_SYMBOLS == 4
#define LR1121_CAD_PEAK 53 /* Semtech's best measured value for these settings, from its CAD example */
#elif LR1121_CAD
#error "set LR1121_CAD_PEAK for this SF, bandwidth and symbol count"
#else
#define LR1121_CAD_PEAK 0
#endif
#endif
#ifndef LR1121_CAD_MIN
#define LR1121_CAD_MIN 10
#endif

#ifndef LR1121_TX_MARGIN_MS
#define LR1121_TX_MARGIN_MS 10 /* covers the TCXO start before the first symbol */
#endif
#ifndef LR1121_TEST_CW_MS
#define LR1121_TEST_CW_MS 0 /* above 0: unmodulated carrier for this long after bring-up, for the power check */
#endif

_Static_assert(LR1121_HF == 0 || LR1121_HF == 1, "LR1121_HF is 0 or 1");
_Static_assert(BAND_OK, "the Core1121-LF covers 410-510 MHz on the sub-GHz path, 1900-2100 and 2400-2500 MHz on the HF path");
_Static_assert(POWER_OK, "the module allows -9 to +22 dBm on the sub-GHz path and -18 to +13 dBm on the HF path");
_Static_assert(PA_OK, "PA duty and size outside the manual's ranges for this path");
_Static_assert(LR1121_SF >= 7 && LR1121_SF <= 12, "spreading factor is 7 to 12");
_Static_assert((LR1121_BW >= 3 && LR1121_BW <= 6) || (LR1121_BW >= 13 && LR1121_BW <= 15), "bandwidth code is 3 to 6 or 13 to 15");
_Static_assert(LR1121_BW <= 6 || (LR1121_HF && LR1121_BAND_HZ >= 2400000000), "bandwidth codes 13 to 15 are for 2.4 GHz only");
_Static_assert(LR1121_CR >= 1 && LR1121_CR <= 4, "coding rate is 1 to 4");
_Static_assert(LR1121_PREAMBLE >= 8 && LR1121_PREAMBLE <= 65535, "preamble is 8 to 65535 symbols");
_Static_assert(LR1121_RAMP >= 0 && LR1121_RAMP <= 15, "ramp code is 0 to 15");
_Static_assert(LR1121_RX_BOOST == 0 || LR1121_RX_BOOST == 1, "RX boost is 0 or 1");
_Static_assert(LR1121_REG_MODE == 0 || LR1121_REG_MODE == 1, "regulator mode is 0 or 1");
_Static_assert(LR1121_TCXO_TUNE >= 0 && LR1121_TCXO_TUNE <= 7, "TCXO tune code is 0 to 7");
_Static_assert(LR1121_TCXO_DELAY >= 1 && LR1121_TCXO_DELAY <= 0xFFFFFF, "TCXO delay is 24 bits and not 0");
_Static_assert(LR1121_RFSW_ENABLE <= 0x1F, "only five DIOs can drive an RF switch");
_Static_assert(((LR1121_RFSW_STANDBY | LR1121_RFSW_RX | LR1121_RFSW_TX | LR1121_RFSW_TX_HP | LR1121_RFSW_TX_HF) &
                ~LR1121_RFSW_ENABLE) == 0,
               "an RF switch state uses a DIO that is not enabled");
_Static_assert(LR1121_CAD_SYMBOLS == 2 || LR1121_CAD_SYMBOLS == 4 || LR1121_CAD_SYMBOLS == 8 || LR1121_CAD_SYMBOLS == 16,
               "CAD runs over 2, 4, 8 or 16 symbols");
_Static_assert(LMESH_MAX_FRAME <= 255, "an LR1121 packet carries at most 255 bytes");
#if !LR1121_HF
_Static_assert(LR1121_BAND_HZ >= LR1121_CAL_FREQ1 * 4000000 && LR1121_BAND_HZ <= LR1121_CAL_FREQ2 * 4000000,
               "the image calibration range must cover the band");
#endif

#define OP_GET_STATUS 0x0100u
#define OP_GET_VERSION 0x0101u
#define OP_WRITE_BUFFER 0x0109u
#define OP_READ_BUFFER 0x010Au
#define OP_GET_ERRORS 0x010Du
#define OP_CLEAR_ERRORS 0x010Eu
#define OP_CALIBRATE 0x010Fu
#define OP_SET_REG_MODE 0x0110u
#define OP_CALIB_IMAGE 0x0111u
#define OP_SET_RF_SWITCH 0x0112u
#define OP_SET_DIO_IRQ 0x0113u
#define OP_CLEAR_IRQ 0x0114u
#define OP_SET_TCXO 0x0117u
#define OP_SET_SLEEP 0x011Bu
#define OP_SET_STANDBY 0x011Cu
#define OP_GET_RX_BUFFER 0x0203u
#define OP_GET_PACKET_STATUS 0x0204u
#define OP_SET_RX 0x0209u
#define OP_SET_TX 0x020Au
#define OP_SET_FREQUENCY 0x020Bu
#define OP_SET_CAD_PARAMS 0x020Du
#define OP_SET_PACKET_TYPE 0x020Eu
#define OP_SET_MODULATION 0x020Fu
#define OP_SET_PACKET_PARAMS 0x0210u
#define OP_SET_TX_PARAMS 0x0211u
#define OP_SET_FALLBACK 0x0213u
#define OP_SET_PA_CONFIG 0x0215u
#define OP_SET_CAD 0x0218u
#define OP_SET_TX_CW 0x0219u
#define OP_SET_RX_BOOST 0x0227u
#define OP_SET_RSSI_CAL 0x0229u
#define OP_SET_SYNC_WORD 0x022Bu

#define OP(x) (uint8_t)((x) >> 8), (uint8_t)((x) & 0xFFu)
#define BYTE(x, n) (uint8_t)(((x) >> (n)) & 0xFF)

#define IRQ_TX_DONE (1ul << 2)
#define IRQ_RX_DONE (1ul << 3)
#define IRQ_PREAMBLE (1ul << 4)
#define IRQ_HEADER_VALID (1ul << 5)
#define IRQ_HEADER_ERROR (1ul << 6)
#define IRQ_CRC_ERROR (1ul << 7)
#define IRQ_CAD_DONE (1ul << 8)
#define IRQ_CAD_DETECTED (1ul << 9)
#define IRQ_TIMEOUT (1ul << 10)
#define IRQ_CMD_ERROR (1ul << 22)
#define IRQ_ERROR (1ul << 23)
#define IRQ_MASK (IRQ_TX_DONE | IRQ_RX_DONE | IRQ_PREAMBLE | IRQ_HEADER_VALID | IRQ_HEADER_ERROR | IRQ_CRC_ERROR | \
                  IRQ_CAD_DONE | IRQ_CAD_DETECTED | IRQ_TIMEOUT | IRQ_CMD_ERROR | IRQ_ERROR)

#define USE_CASE_LR1121 0x03u
#define PACKET_TYPE_LORA 0x02u
#define CALIBRATE_ALL 0x3Fu
#define STANDBY_RC 0x00u
#define STANDBY_XOSC 0x01u
#define FALLBACK_XOSC 0x02u
#define SLEEP_POWER_DOWN 0x00u
#define CAD_ONLY 0x00u

#define BW_HZ (LR1121_BW == 13 ? 203125u : LR1121_BW == 14 ? 406250u : LR1121_BW == 15 ? 812500u : 62500u << (LR1121_BW - 3))
#define TSYM_US ((1u << LR1121_SF) * 1000000u / BW_HZ)
#define LDRO (TSYM_US > 16000u ? 1u : 0u)

#define RESET_MS 10u
#define BOOT_SETTLE_MS 10u
#define BOOT_MS 500u
#define STEP_MS 1000u
#define READY_MS 20u
#define SPI_TIMEOUT_MS 10u
#define SEND_GRACE_MS 2000u
#define RETRY_MS 2000u
#define CAD_WAIT_MS (LR1121_CAD_SYMBOLS * TSYM_US / 1000u + 20u)
#define PREAMBLE_HOLD_MS ((LR1121_PREAMBLE + 13u) * TSYM_US / 1000u)
#define SPI_MAX (2u + LMESH_MAX_FRAME)

typedef struct step {
    uint8_t len;
    uint8_t bytes[13];
} step;

/* Bring-up runs in this order: version check, these steps, the error check, then the radio steps. */
static const step setup_steps[] = {
    {3, {OP(OP_SET_STANDBY), STANDBY_RC}},
    {3, {OP(OP_SET_REG_MODE), LR1121_REG_MODE}},
    {10,
     {OP(OP_SET_RF_SWITCH), LR1121_RFSW_ENABLE, LR1121_RFSW_STANDBY, LR1121_RFSW_RX, LR1121_RFSW_TX, LR1121_RFSW_TX_HP,
      LR1121_RFSW_TX_HF, 0, 0}},
    {6, {OP(OP_SET_TCXO), LR1121_TCXO_TUNE, BYTE(LR1121_TCXO_DELAY, 16), BYTE(LR1121_TCXO_DELAY, 8), BYTE(LR1121_TCXO_DELAY, 0)}},
    {2, {OP(OP_CLEAR_ERRORS)}},
    {3, {OP(OP_CALIBRATE), CALIBRATE_ALL}},
#if !LR1121_HF
    {4, {OP(OP_CALIB_IMAGE), LR1121_CAL_FREQ1, LR1121_CAL_FREQ2}},
#endif
};

static const step radio_steps[] = {
    {3, {OP(OP_SET_PACKET_TYPE), PACKET_TYPE_LORA}},
    {6, {OP(OP_SET_FREQUENCY), BYTE(LR1121_BAND_HZ, 24), BYTE(LR1121_BAND_HZ, 16), BYTE(LR1121_BAND_HZ, 8), BYTE(LR1121_BAND_HZ, 0)}},
    {13, {OP(OP_SET_RSSI_CAL), RSSI_TUNES}},
    {6, {OP(OP_SET_PA_CONFIG), PA_SEL, PA_SUPPLY, LR1121_PA_DUTY, LR1121_PA_HP_SEL}},
    {4, {OP(OP_SET_TX_PARAMS), BYTE(LR1121_POWER_DBM, 0), LR1121_RAMP}},
    {3, {OP(OP_SET_FALLBACK), FALLBACK_XOSC}},
    {3, {OP(OP_SET_RX_BOOST), LR1121_RX_BOOST}},
    {6, {OP(OP_SET_MODULATION), LR1121_SF, LR1121_BW, LR1121_CR, LDRO}},
    {3, {OP(OP_SET_SYNC_WORD), LR1121_SYNC_WORD}},
    {9, {OP(OP_SET_CAD_PARAMS), LR1121_CAD_SYMBOLS, LR1121_CAD_PEAK, LR1121_CAD_MIN, CAD_ONLY, 0, 0, 0}},
    {10, {OP(OP_SET_DIO_IRQ), BYTE(IRQ_MASK, 24), BYTE(IRQ_MASK, 16), BYTE(IRQ_MASK, 8), BYTE(IRQ_MASK, 0), 0, 0, 0, 0}},
    {6, {OP(OP_CLEAR_IRQ), 0xFF, 0xFF, 0xFF, 0xFF}},
};

#define N_SETUP (sizeof setup_steps / sizeof setup_steps[0])
#define N_RADIO (sizeof radio_steps / sizeof radio_steps[0])
#define STEP_VERSION 0u
#define STEP_SETUP 1u
#define STEP_ERRORS (STEP_SETUP + N_SETUP)
#define STEP_RADIO (STEP_ERRORS + 1u)
#define STEP_DONE (STEP_RADIO + N_RADIO)

typedef enum state { S_RESET, S_BOOT, S_CONFIG, S_PAUSE, S_CW, S_IDLE, S_TX, S_SLEEP } state;

static uint8_t out_buf[SPI_MAX];
static uint8_t in_buf[SPI_MAX];

static state st;
static uint32_t deadline;
static unsigned step_index;
static char version[16];
static bool rx_on;
static bool rx_hold;
static uint32_t rx_until;

/* Counters for the debugger */
static uint32_t rx_frames;
static uint32_t rx_bad;
static uint32_t tx_errors;
static uint32_t resets;
static uint32_t chip_errors;
#if LR1121_CAD
static uint32_t cad_runs;
static uint32_t cad_busy;
#endif

static bool busy_high(void)
{
    return HAL_GPIO_ReadPin(RADIO_BUSY_PORT, RADIO_BUSY_PIN) == GPIO_PIN_SET;
}

static bool irq_high(void)
{
    return HAL_GPIO_ReadPin(RADIO_DIO0_PORT, RADIO_DIO0_PIN) == GPIO_PIN_SET;
}

static bool wait_ready(void)
{
    uint32_t start = HAL_GetTick();
    while (busy_high()) {
        if (HAL_GetTick() - start >= READY_MS) {
            return false;
        }
    }
    return true;
}

static bool transfer(size_t n)
{
    if (!wait_ready()) {
        return false;
    }
    HAL_GPIO_WritePin(RADIO_NSS_PORT, RADIO_NSS_PIN, GPIO_PIN_RESET);
    HAL_StatusTypeDef r = HAL_SPI_TransmitReceive(&board_radio_spi, out_buf, in_buf, (uint16_t)n, SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(RADIO_NSS_PORT, RADIO_NSS_PIN, GPIO_PIN_SET);
    return r == HAL_OK;
}

static bool write_cmd(const uint8_t *cmd, size_t n)
{
    memcpy(out_buf, cmd, n);
    return transfer(n);
}

/* The reply comes in a second transaction: Stat1, then n bytes at in_buf[1]. */
static bool read_cmd(const uint8_t *cmd, size_t n, size_t reply)
{
    if (!write_cmd(cmd, n)) {
        return false;
    }
    memset(out_buf, 0, reply + 1u);
    return transfer(reply + 1u);
}

static bool set_standby(uint8_t mode)
{
    const uint8_t cmd[] = {OP(OP_SET_STANDBY), mode};
    return write_cmd(cmd, sizeof cmd);
}

static bool set_packet(size_t payload)
{
    const uint8_t cmd[] = {OP(OP_SET_PACKET_PARAMS), BYTE(LR1121_PREAMBLE, 8), BYTE(LR1121_PREAMBLE, 0), 0, (uint8_t)payload, 1, 0};
    return write_cmd(cmd, sizeof cmd);
}

static bool start_rx(void)
{
    static const uint8_t cmd[] = {OP(OP_SET_RX), 0xFF, 0xFF, 0xFF};
    if (!set_packet(LMESH_MAX_FRAME) || !write_cmd(cmd, sizeof cmd)) {
        return false;
    }
    rx_on = true;
    return true;
}

/* Reads the interrupt flags and clears exactly those it read. */
static bool take_irq(uint32_t *irq)
{
    static const uint8_t get[] = {OP(OP_GET_STATUS), 0, 0, 0, 0};
    if (!write_cmd(get, sizeof get)) {
        return false;
    }
    uint32_t v = (uint32_t)in_buf[2] << 24 | (uint32_t)in_buf[3] << 16 | (uint32_t)in_buf[4] << 8 | in_buf[5];
    const uint8_t clear[] = {OP(OP_CLEAR_IRQ), BYTE(v, 24), BYTE(v, 16), BYTE(v, 8), BYTE(v, 0)};
    *irq = v;
    return v == 0 || write_cmd(clear, sizeof clear);
}

/* The manual's RX buffer is a ring, so a frame may wrap past offset 255. */
static bool read_buffer(uint8_t offset, uint8_t *dst, size_t n)
{
    while (n > 0) {
        size_t part = 256u - offset;
        if (part > n) {
            part = n;
        }
        const uint8_t cmd[] = {OP(OP_READ_BUFFER), offset, (uint8_t)part};
        if (!read_cmd(cmd, sizeof cmd, part)) {
            return false;
        }
        memcpy(dst, &in_buf[1], part);
        dst += part;
        n -= part;
        offset = (uint8_t)(offset + part);
    }
    return true;
}

/* Semtech's time-on-air formula for SF7 and up, explicit header, CRC on. */
static uint32_t lora_airtime_us(size_t payload)
{
    const uint32_t sf = LR1121_SF;
    int32_t num = 8 * (int32_t)payload - 4 * (int32_t)sf + 28 + 16;
    int32_t den = 4 * (int32_t)(sf - 2u * LDRO);
    uint32_t blocks = num > 0 ? (uint32_t)((num + den - 1) / den) : 0u;
    uint32_t symbols = 8u + blocks * (LR1121_CR + 4u);
    uint64_t quarters = 4u * LR1121_PREAMBLE + 17u + 4u * symbols;
    return (uint32_t)(quarters * (1u << sf) * 1000000u / (4u * BW_HZ));
}

static void report_ready(void)
{
    char text[200];
    snprintf(text, sizeof text,
             "{\"ev\":\"radio\",\"driver\":\"lr1121\",\"state\":\"ready\",\"version\":\"%s\",\"band_hz\":%lu,\"sf\":%d,\"bw\":%d,"
             "\"cr\":%d,\"preamble\":%d,\"power_dbm\":%d}",
             version, (unsigned long)LR1121_BAND_HZ, LR1121_SF, LR1121_BW <= 6 ? LR1121_BW + 3 : LR1121_BW, LR1121_CR,
             LR1121_PREAMBLE, LR1121_POWER_DBM);
    console_line(text);
}

static void report_error(const char *reply)
{
    char text[120];
    snprintf(text, sizeof text, "{\"ev\":\"radio\",\"driver\":\"lr1121\",\"state\":\"error\",\"cmd\":\"step %u\",\"reply\":\"%s\"}",
             step_index, reply);
    console_line(text);
}

static void start_reset(void)
{
    resets++;
    rx_on = false;
    rx_hold = false;
    board_radio_reset(true);
    st = S_RESET;
    deadline = HAL_GetTick() + RESET_MS;
}

/* Fails any frame on the air, reports, and starts over after a pause. */
static void fail(lmesh_node *node, const char *reply)
{
    if (st == S_TX) {
        tx_errors++;
        lmesh_radio_tx_done(node, false);
    }
    report_error(reply);
    rx_on = false;
    st = S_PAUSE;
    deadline = HAL_GetTick() + RETRY_MS;
}

static void check_version(lmesh_node *node)
{
    static const uint8_t cmd[] = {OP(OP_GET_VERSION)};
    if (!read_cmd(cmd, sizeof cmd, 4)) {
        fail(node, "spi");
    } else if (in_buf[2] != USE_CASE_LR1121) {
        fail(node, "not an LR1121 in transceiver mode");
    } else {
        snprintf(version, sizeof version, "%02x.%02x.%02x.%02x", in_buf[1], in_buf[2], in_buf[3], in_buf[4]);
        step_index++;
    }
}

static void check_errors(lmesh_node *node)
{
    static const uint8_t cmd[] = {OP(OP_GET_ERRORS)};
    if (!read_cmd(cmd, sizeof cmd, 2)) {
        fail(node, "spi");
        return;
    }
    uint32_t errors = (uint32_t)in_buf[1] << 8 | in_buf[2];
    if ((errors & LR1121_ERROR_MASK) != 0) {
        char reply[32];
        snprintf(reply, sizeof reply, "errors 0x%04lx", (unsigned long)errors);
        fail(node, reply);
    } else {
        step_index++;
    }
}

static void finish_bring_up(lmesh_node *node)
{
    report_ready();
#if LR1121_TEST_CW_MS > 0
    static const uint8_t cw[] = {OP(OP_SET_TX_CW)};
    if (write_cmd(cw, sizeof cw)) {
        st = S_CW;
        deadline = HAL_GetTick() + LR1121_TEST_CW_MS;
    } else {
        fail(node, "spi");
    }
#else
    if (start_rx()) {
        st = S_IDLE;
    } else {
        fail(node, "spi");
    }
#endif
}

static void run_step(lmesh_node *node)
{
    if (step_index == STEP_VERSION) {
        check_version(node);
    } else if (step_index < STEP_ERRORS) {
        const step *s = &setup_steps[step_index - STEP_SETUP];
        if (write_cmd(s->bytes, s->len)) {
            step_index++;
        } else {
            fail(node, "spi");
        }
    } else if (step_index == STEP_ERRORS) {
        check_errors(node);
    } else if (step_index < STEP_DONE) {
        const step *s = &radio_steps[step_index - STEP_RADIO];
        if (write_cmd(s->bytes, s->len)) {
            step_index++;
        } else {
            fail(node, "spi");
        }
    } else {
        finish_bring_up(node);
    }
}

static void receive(lmesh_node *node)
{
    static const uint8_t get_buffer[] = {OP(OP_GET_RX_BUFFER)};
    static const uint8_t get_status[] = {OP(OP_GET_PACKET_STATUS)};
    uint8_t frame[LMESH_MAX_FRAME];

    if (!read_cmd(get_buffer, sizeof get_buffer, 2)) {
        rx_bad++;
        return;
    }
    size_t len = in_buf[1];
    uint8_t start = in_buf[2];
    if (len == 0 || len > LMESH_MAX_FRAME || !read_cmd(get_status, sizeof get_status, 3)) {
        rx_bad++;
        return;
    }
    int16_t rssi = (int16_t)(-(int)in_buf[1] / 2);
    int8_t snr = (int8_t)((int8_t)in_buf[2] / 4);
    if (!read_buffer(start, frame, len)) {
        rx_bad++;
        return;
    }
    rx_frames++;
    lmesh_radio_rx(node, frame, len, rssi, snr);
}

/* Handles the pending interrupt flags; true if the library was told something. */
static bool service(lmesh_node *node)
{
    uint32_t irq = 0;
    if (!take_irq(&irq)) {
        fail(node, "spi");
        return true;
    }
    uint32_t now = HAL_GetTick();
    if (irq & (IRQ_CMD_ERROR | IRQ_ERROR)) {
        chip_errors++;
    }
    if (irq & IRQ_PREAMBLE) {
        rx_hold = true;
        rx_until = now + PREAMBLE_HOLD_MS;
    }
    if (irq & IRQ_HEADER_VALID) {
        rx_hold = true;
        rx_until = now + radio_airtime_ms(LMESH_MAX_FRAME);
    }
    if (irq & (IRQ_RX_DONE | IRQ_HEADER_ERROR | IRQ_CRC_ERROR)) {
        rx_hold = false;
    }
    if (st == S_TX) {
        if (irq & IRQ_TX_DONE) {
            st = S_IDLE;
            rx_on = false;
            (void)start_rx();
            lmesh_radio_tx_done(node, true);
            return true;
        }
        if (irq & IRQ_TIMEOUT) {
            fail(node, "tx timeout");
            return true;
        }
        return false;
    }
    if (irq & (IRQ_HEADER_ERROR | IRQ_CRC_ERROR)) {
        rx_bad++;
    } else if (irq & IRQ_RX_DONE) {
        receive(node);
        return true;
    }
    return false;
}

void radio_init(void)
{
    st = S_RESET;
    version[0] = '\0';
    step_index = STEP_VERSION;
    start_reset();
}

uint32_t radio_airtime_ms(size_t len)
{
    return (lora_airtime_us(len) + 999u) / 1000u + LR1121_TX_MARGIN_MS;
}

int radio_send(const uint8_t *frame, size_t len)
{
    static const uint8_t set_tx[] = {OP(OP_SET_TX), 0, 0, 0};
    static const uint8_t head[] = {OP(OP_WRITE_BUFFER)};
    if (st != S_IDLE || len == 0 || len > LMESH_MAX_FRAME) {
        return -1;
    }
    if (!set_standby(STANDBY_XOSC) || !set_packet(len)) {
        fail(NULL, "spi");
        return -1;
    }
    memcpy(out_buf, head, sizeof head);
    memcpy(&out_buf[sizeof head], frame, len);
    if (!transfer(sizeof head + len) || !write_cmd(set_tx, sizeof set_tx)) {
        fail(NULL, "spi");
        return -1;
    }
    rx_on = false;
    rx_hold = false;
    st = S_TX;
    deadline = HAL_GetTick() + radio_airtime_ms(len) + SEND_GRACE_MS;
    return 0;
}

#if LR1121_CAD
/* Runs one CAD and leaves the chip in standby if the channel is clear, ready for the radio_send that follows. */
bool radio_channel_busy(void)
{
    static const uint8_t set_cad[] = {OP(OP_SET_CAD)};
    if (st != S_IDLE) {
        return false;
    }
    uint32_t now = HAL_GetTick();
    if (irq_high() || (rx_hold && (int32_t)(rx_until - now) > 0)) {
        cad_busy++;
        return true;
    }
    rx_hold = false;
    cad_runs++;
    if (!set_standby(STANDBY_XOSC) || !write_cmd(set_cad, sizeof set_cad)) {
        fail(NULL, "spi");
        return false;
    }
    rx_on = false;
    uint32_t seen = 0;
    while ((seen & IRQ_CAD_DONE) == 0) {
        if (HAL_GetTick() - now >= CAD_WAIT_MS) {
            fail(NULL, "cad timeout");
            return false;
        }
        uint32_t irq = 0;
        if (irq_high() && !take_irq(&irq)) {
            fail(NULL, "spi");
            return false;
        }
        seen |= irq;
    }
    if (seen & IRQ_RX_DONE) {
        rx_bad++;
    }
    if (seen & (IRQ_CAD_DETECTED | IRQ_PREAMBLE | IRQ_HEADER_VALID)) {
        cad_busy++;
        (void)start_rx();
        return true;
    }
    return false;
}
#endif

void lr1121_sleep(void)
{
    const uint8_t cmd[] = {OP(OP_SET_SLEEP), SLEEP_POWER_DOWN, 0, 0, 0, 0};
    if (st == S_IDLE && write_cmd(cmd, sizeof cmd)) {
        rx_on = false;
        rx_hold = false;
        st = S_SLEEP;
    }
}

void lr1121_wake(void)
{
    if (st == S_SLEEP) {
        start_reset();
    }
}

bool radio_poll(lmesh_node *node)
{
    bool did = false;
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
        if ((int32_t)(now - (deadline - BOOT_MS + BOOT_SETTLE_MS)) >= 0 && !busy_high()) {
            step_index = STEP_VERSION;
            st = S_CONFIG;
            deadline = now + STEP_MS;
        } else if (due) {
            fail(node, "boot timeout");
        }
        break;
    case S_CONFIG:
        if (!busy_high()) {
            deadline = now + STEP_MS;
            run_step(node);
        } else if (due) {
            fail(node, "busy timeout");
        }
        break;
    case S_PAUSE:
        if (due) {
            start_reset();
        }
        break;
    case S_CW:
        if (due) {
            if (set_standby(STANDBY_XOSC) && start_rx()) {
                st = S_IDLE;
            } else {
                fail(node, "spi");
            }
        }
        break;
    case S_IDLE:
        if (!rx_on && !start_rx()) {
            fail(node, "spi");
            did = true;
        } else if (irq_high()) {
            did = service(node);
        }
        break;
    case S_TX:
        if (irq_high()) {
            did = service(node);
        } else if (due) {
            fail(node, "tx timeout");
            did = true;
        }
        break;
    case S_SLEEP:
        break;
    }
    return did;
}
