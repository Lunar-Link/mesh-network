/* AES-128 forward cipher for the CMAC integrity tag. */
#ifndef LMESH_AES_H
#define LMESH_AES_H

#include <stdint.h>

typedef struct lmesh_aes128 {
    uint8_t round_keys[176];
} lmesh_aes128;

void lmesh_aes128_init(lmesh_aes128 *ctx, const uint8_t key[16]);
void lmesh_aes128_encrypt_block(const lmesh_aes128 *ctx, const uint8_t in[16], uint8_t out[16]);

#endif
