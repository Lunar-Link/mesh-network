/* Flash log for the storage slots: a ring of pages, each opened with a copy of every slot, records appended after. */
#include "storage.h"

#include <stdbool.h>
#include <string.h>

#include "board.h"

/* Top 128 KB of bank 2; platformio.ini caps the program size below it. */
#ifndef STORAGE_ADDR
#define STORAGE_ADDR 0x080E0000u
#endif
#ifndef STORAGE_MEM
#define STORAGE_MEM ((const uint8_t *)STORAGE_ADDR)
#endif
#define BANK2_ADDR 0x08080000u
#define BANK2_END 0x08100000u
#define PAGES 64u
#define PAGE 2048u
#define SLOTS 3u
#define SLOT_MAX 640u
#define PAGE_MAGIC 0x484d4c53u
#define RECORD_MARK 0xa5u
#define CRC_INIT 0xffffu

#define PAD8(n) (((n) + 7u) & ~7u)

_Static_assert(STORAGE_ADDR >= BANK2_ADDR && (STORAGE_ADDR - BANK2_ADDR) % PAGE == 0 && STORAGE_ADDR + PAGES * PAGE <= BANK2_END,
               "the log must be whole pages inside bank 2");
_Static_assert(8u + SLOTS * (8u + SLOT_MAX) <= PAGE, "a fresh page must hold a copy of every slot");
_Static_assert(PAGES >= 3u, "an interrupted page start must never erase the last complete page");

typedef struct cached {
    bool valid;
    uint16_t len;
    uint8_t data[SLOT_MAX];
} cached;

static cached cache[SLOTS];
static int cur_page;
static uint32_t cur_seq;
static uint32_t cur_off;
static volatile bool ecc_error;

static uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000u) ? (uint16_t)(crc << 1 ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Copies from flash; false if the read hit an ECC error. */
static bool flash_read(uint32_t off, void *out, size_t n)
{
    ecc_error = false;
    memcpy(out, STORAGE_MEM + off, n);
    return !ecc_error;
}

static bool flash_erase(unsigned page)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t bad = 0;
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks = FLASH_BANK_2;
    e.Page = (STORAGE_ADDR - BANK2_ADDR) / PAGE + page;
    e.NbPages = 1;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    bool ok = HAL_FLASHEx_Erase(&e, &bad) == HAL_OK;
    HAL_FLASH_Lock();
    return ok;
}

static bool flash_program(uint32_t off, const uint8_t *data, size_t n)
{
    bool ok = true;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    for (size_t i = 0; ok && i < n; i += 8u) {
        uint64_t v;
        memcpy(&v, data + i, 8);
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, STORAGE_ADDR + off + (uint32_t)i, v) == HAL_OK;
    }
    HAL_FLASH_Lock();
    return ok;
}

/* Record: mark, slot, length, data CRC, header CRC; then the data padded to a doubleword. */
static bool append(uint8_t slot)
{
    const cached *c = &cache[slot];
    uint8_t rec[8u + PAD8(SLOT_MAX)];
    size_t n = 8u + PAD8(c->len);
    memset(rec, 0xff, n);
    rec[0] = RECORD_MARK;
    rec[1] = slot;
    rec[2] = (uint8_t)c->len;
    rec[3] = (uint8_t)(c->len >> 8);
    uint16_t dcrc = crc16(c->data, c->len, CRC_INIT);
    rec[4] = (uint8_t)dcrc;
    rec[5] = (uint8_t)(dcrc >> 8);
    uint16_t hcrc = crc16(rec, 6, CRC_INIT);
    rec[6] = (uint8_t)hcrc;
    rec[7] = (uint8_t)(hcrc >> 8);
    memcpy(rec + 8, c->data, c->len);
    if (!flash_program((uint32_t)cur_page * PAGE + cur_off, rec, n)) {
        cur_off = PAGE;
        return false;
    }
    cur_off += (uint32_t)n;
    return true;
}

/* Erases the next page in the ring and opens it with every slot, so older pages are never needed again. */
static bool start_page(void)
{
    unsigned next = cur_page < 0 ? 0u : ((unsigned)cur_page + 1u) % PAGES;
    uint8_t head[8];
    cur_page = (int)next;
    cur_off = PAGE;
    cur_seq++;
    if (!flash_erase(next)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        head[i] = (uint8_t)(PAGE_MAGIC >> (8 * i));
        head[4 + i] = (uint8_t)(cur_seq >> (8 * i));
    }
    if (!flash_program((uint32_t)next * PAGE, head, sizeof head)) {
        return false;
    }
    cur_off = 8;
    for (uint8_t s = 0; s < SLOTS; s++) {
        if (cache[s].valid && !append(s)) {
            return false;
        }
    }
    return true;
}

/* Replays one page's records into the cache; returns the offset where it ends, PAGE if anything was damaged. */
static uint32_t replay(unsigned page, unsigned *seen)
{
    static const uint8_t erased[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static uint8_t data[PAD8(SLOT_MAX)];
    uint32_t base = (uint32_t)page * PAGE;
    uint32_t off = 8;
    while (off + 8u <= PAGE) {
        uint8_t h[8];
        if (!flash_read(base + off, h, sizeof h)) {
            return PAGE;
        }
        if (memcmp(h, erased, sizeof h) == 0) {
            return off;
        }
        uint16_t len = get16(h + 2);
        if (h[0] != RECORD_MARK || h[1] >= SLOTS || len == 0 || len > SLOT_MAX ||
            get16(h + 6) != crc16(h, 6, CRC_INIT) || off + 8u + PAD8(len) > PAGE) {
            return PAGE;
        }
        if (!flash_read(base + off + 8u, data, len) || crc16(data, len, CRC_INIT) != get16(h + 4)) {
            return PAGE;
        }
        cached *c = &cache[h[1]];
        c->valid = true;
        c->len = len;
        memcpy(c->data, data, len);
        *seen |= 1u << h[1];
        off += 8u + PAD8(len);
    }
    return off;
}

void storage_init(void)
{
    uint32_t seq[PAGES];
    bool valid[PAGES];
    memset(cache, 0, sizeof cache);
    cur_page = -1;
    cur_seq = 0;
    cur_off = PAGE;

    for (unsigned p = 0; p < PAGES; p++) {
        uint8_t head[8];
        valid[p] = flash_read((uint32_t)p * PAGE, head, sizeof head) && get32(head) == PAGE_MAGIC;
        seq[p] = valid[p] ? get32(head + 4) : 0;
    }
    /* Oldest page first, so later records override earlier ones */
    for (;;) {
        int pick = -1;
        for (unsigned p = 0; p < PAGES; p++) {
            if (valid[p] && (pick < 0 || seq[p] < seq[pick])) {
                pick = (int)p;
            }
        }
        if (pick < 0) {
            break;
        }
        valid[pick] = false;
        unsigned seen = 0;
        uint32_t end = replay((unsigned)pick, &seen);
        cur_page = pick;
        cur_seq = seq[pick];
        cur_off = end;
        unsigned live = 0;
        for (unsigned s = 0; s < SLOTS; s++) {
            live |= cache[s].valid ? 1u << s : 0u;
        }
        /* A page whose opening copy was cut short must not take further records. */
        if (seen != live) {
            cur_off = PAGE;
        }
    }
}

int storage_read(uint8_t slot, void *buf, size_t len)
{
    if (slot >= SLOTS || !cache[slot].valid || cache[slot].len != len) {
        return -1;
    }
    memcpy(buf, cache[slot].data, len);
    return 0;
}

int storage_write(uint8_t slot, const void *buf, size_t len)
{
    if (slot >= SLOTS || len == 0 || len > SLOT_MAX) {
        return -1;
    }
    cached *c = &cache[slot];
    if (c->valid && c->len == len && memcmp(c->data, buf, len) == 0) {
        return 0;
    }
    c->valid = true;
    c->len = (uint16_t)len;
    memcpy(c->data, buf, len);
    if (cur_page >= 0 && cur_off + 8u + PAD8(len) <= PAGE) {
        return append(slot) ? 0 : -1;
    }
    return start_page() ? 0 : -1;
}

void storage_ecc_error(void)
{
    ecc_error = true;
}
