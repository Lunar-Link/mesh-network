/* Frame header codec and integrity tag. SPEC.md Section 5. */
#ifndef LMESH_FRAME_H
#define LMESH_FRAME_H

#include "internal.h"

#define LMESH_FRAME_VERSION 1

enum lmesh_frame_type {
    LMESH_FT_HELLO = 0,
    LMESH_FT_DATA = 1,
    LMESH_FT_HOP_ACK = 2,
    LMESH_FT_STATUS = 3,
    LMESH_FT_ROUTE_REQ = 4
};

enum lmesh_frame_flag {
    LMESH_FLAG_ACK_REQ = 0x01,
    LMESH_FLAG_RETRY = 0x02
};

enum lmesh_ack_status {
    LMESH_ACK_ACCEPTED = 0,
    LMESH_ACK_DUPLICATE = 1,
    LMESH_ACK_REFUSED_FULL = 2,
    LMESH_ACK_REFUSED_INVALID = 3,
    LMESH_ACK_TIMEOUT = 4
};

typedef struct lmesh_frame_hdr {
    uint8_t version;
    uint8_t type;
    uint8_t network_id;
    lmesh_node_id tx;
    lmesh_node_id rx;
    uint8_t seq;
    uint8_t flags;
    uint8_t payload_len;
} lmesh_frame_hdr;

size_t lmesh_frame_encode(const lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, uint8_t *out,
                          size_t out_max);
lmesh_err lmesh_frame_decode(lmesh_node *node, const uint8_t *in, size_t len, lmesh_frame_hdr *hdr,
                             const uint8_t **payload);
void lmesh_frame_retag(const lmesh_node *node, uint8_t *buf, size_t len);
void lmesh_frame_tag(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t tag[LMESH_TAG_LEN]);
uint32_t lmesh_crc32(const uint8_t *data, size_t len);
void lmesh_cmac128(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16]);

#endif
