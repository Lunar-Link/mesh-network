/* Storage log flash on the F103: two 1 KB flash pages per log page, programmed a word at a time. */
#include <string.h>

#include "storage_flash.h"

#ifndef STORAGE_MEM
#define STORAGE_MEM ((const uint8_t *)STORAGE_ADDR)
#endif
#define FLASH_TOP 0x08010000u /* 64 KB, the size the C8 is sold with */
#define FLASH_ERRORS (FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR)

_Static_assert(STORAGE_PAGE % FLASH_PAGE_SIZE == 0 && (STORAGE_ADDR - FLASH_BASE) % FLASH_PAGE_SIZE == 0 &&
                   STORAGE_ADDR + STORAGE_PAGES * STORAGE_PAGE <= FLASH_TOP,
               "the log must be whole flash pages inside the 64 KB");

/* The F103 has no flash ECC, so a write cut off by power loss reads back as wrong bits, which the record CRCs catch. */
bool storage_flash_read(uint32_t off, void *out, size_t n)
{
    memcpy(out, STORAGE_MEM + off, n);
    return true;
}

/* A power cut between the flash pages leaves the log page without its header, so it is ignored and erased again. */
bool storage_flash_erase(unsigned page)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t bad = 0;
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.PageAddress = STORAGE_ADDR + page * STORAGE_PAGE;
    e.NbPages = STORAGE_PAGE / FLASH_PAGE_SIZE;
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
