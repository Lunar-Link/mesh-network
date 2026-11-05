/* Storage log flash on the L476: 2 KB pages in bank 2, programmed a doubleword at a time, with ECC on reads. */
#include <string.h>

#include "storage_flash.h"

#ifndef STORAGE_MEM
#define STORAGE_MEM ((const uint8_t *)STORAGE_ADDR)
#endif
#define BANK2_ADDR 0x08080000u
#define BANK2_END 0x08100000u

_Static_assert(STORAGE_PAGE == 2048u && STORAGE_ADDR >= BANK2_ADDR && (STORAGE_ADDR - BANK2_ADDR) % STORAGE_PAGE == 0 &&
                   STORAGE_ADDR + STORAGE_PAGES * STORAGE_PAGE <= BANK2_END,
               "the log must be whole pages inside bank 2");

static volatile bool ecc_error;

/* A write cut off by power loss can leave a doubleword whose ECC fails; reading it raises NMI, caught below. */
bool storage_flash_read(uint32_t off, void *out, size_t n)
{
    ecc_error = false;
    memcpy(out, STORAGE_MEM + off, n);
    return !ecc_error;
}

bool storage_flash_erase(unsigned page)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t bad = 0;
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks = FLASH_BANK_2;
    e.Page = (STORAGE_ADDR - BANK2_ADDR) / STORAGE_PAGE + page;
    e.NbPages = 1;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    bool ok = HAL_FLASHEx_Erase(&e, &bad) == HAL_OK;
    HAL_FLASH_Lock();
    return ok;
}

bool storage_flash_program(uint32_t off, const uint8_t *data, size_t n)
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

/* A flash double ECC error raises NMI; the storage scan treats the read as failed and carries on. */
void NMI_Handler(void)
{
    if (__HAL_FLASH_GET_FLAG(FLASH_FLAG_ECCD)) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ECCD);
        ecc_error = true;
        return;
    }
    for (;;) {
    }
}
