/* Storage log flash on the F446: one 128 KB sector per log page, programmed a word at a time. */
#include <string.h>

#include "storage_flash.h"

#ifndef STORAGE_MEM
#define STORAGE_MEM ((const uint8_t *)STORAGE_ADDR)
#endif
#define SECTOR5_ADDR 0x08020000u /* sectors 5 to 7 are 128 KB each; 0 to 4 are smaller */
#define FLASH_END_ADDR 0x08080000u
#define FIRST_SECTOR (FLASH_SECTOR_5 + (STORAGE_ADDR - SECTOR5_ADDR) / STORAGE_PAGE)
#define FLASH_ERRORS (FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR)

_Static_assert(STORAGE_PAGE == 0x20000u && STORAGE_ADDR >= SECTOR5_ADDR &&
                   (STORAGE_ADDR - SECTOR5_ADDR) % STORAGE_PAGE == 0 &&
                   STORAGE_ADDR + STORAGE_PAGES * STORAGE_PAGE <= FLASH_END_ADDR,
               "the log must be whole 128 KB sectors");

/* The F446 has no flash ECC, so a write cut off by power loss reads back as wrong bits, which the record CRCs catch. */
bool storage_flash_read(uint32_t off, void *out, size_t n)
{
    memcpy(out, STORAGE_MEM + off, n);
    return true;
}

bool storage_flash_erase(unsigned page)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t bad = 0;
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Banks = FLASH_BANK_1;
    e.Sector = FIRST_SECTOR + page;
    e.NbSectors = 1;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* 2.7 to 3.6 V, 32-bit parallelism */
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_ERRORS);
    bool ok = HAL_FLASHEx_Erase(&e, &bad) == HAL_OK;
    HAL_FLASH_Lock();
    return ok;
}

bool storage_flash_program(uint32_t off, const uint8_t *data, size_t n)
{
    bool ok = true;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_ERRORS);
    for (size_t i = 0; ok && i < n; i += 4u) {
        uint32_t v;
        memcpy(&v, data + i, 4);
        ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, STORAGE_ADDR + off + (uint32_t)i, v) == HAL_OK;
    }
    HAL_FLASH_Lock();
    return ok;
}
