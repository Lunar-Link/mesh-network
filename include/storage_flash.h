/* The flash under the storage log, one implementation per MCU in src/mcu/<chip>/flash.c. Offsets count from
   STORAGE_ADDR; a page (STORAGE_PAGE bytes, STORAGE_PAGES of them, both from mcu.h) is the smallest unit erased. */
#ifndef STORAGE_FLASH_H
#define STORAGE_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcu.h"

/* Copies from flash; false if the read hit an uncorrectable error (flash ECC on the L4; the F4 has none). */
bool storage_flash_read(uint32_t off, void *out, size_t n);
bool storage_flash_erase(unsigned page);
/* off and n are multiples of 8, the L4's doubleword programming unit. */
bool storage_flash_program(uint32_t off, const uint8_t *data, size_t n);

#endif
