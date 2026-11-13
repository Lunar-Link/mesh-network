/* Non-volatile slots for the library (SPEC.md Section 12) in a wear-leveled log at the top of flash. */
#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

/* Scans the log into RAM; call once before lmesh_init. */
void storage_init(void);
/* 0 if the slot holds exactly len bytes, copied to buf. */
int storage_read(uint8_t slot, void *buf, size_t len);
/* 0 once the value is in flash; an unchanged value is not written again. */
int storage_write(uint8_t slot, const void *buf, size_t len);

#endif
