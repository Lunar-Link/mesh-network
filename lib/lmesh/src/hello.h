/* HELLO encode and decode, periodic and triggered transmission. SPEC.md Sections 7.1 and 8.5. */
#ifndef LMESH_HELLO_H
#define LMESH_HELLO_H

#include "frame.h"

enum lmesh_hello_flag {
    LMESH_HELLO_TRIGGERED = 0x01,
    LMESH_HELLO_MORE = 0x02
};

void lmesh_hello_init(lmesh_node *node);
void lmesh_hello_tick(lmesh_node *node);
void lmesh_hello_trigger(lmesh_node *node);
size_t lmesh_hello_encode(lmesh_node *node, uint8_t *out, size_t out_max);
void lmesh_hello_rx(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len, int16_t rssi,
                    int8_t snr);

#endif
