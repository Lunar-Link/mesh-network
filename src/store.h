/* Fixed message store: records with checksum, lifetime countdown, eviction. SPEC.md Sections 9.3, 9.5, 9.7. */
#ifndef LMESH_STORE_H
#define LMESH_STORE_H

#include "internal.h"

void lmesh_store_init(lmesh_node *node);
lmesh_record *lmesh_store_alloc(lmesh_node *node, lmesh_prio arriving, uint8_t role);
lmesh_record *lmesh_store_find(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t role_mask);
void lmesh_store_release(lmesh_node *node, lmesh_record *r);
void lmesh_store_retire(lmesh_node *node, lmesh_record *r, lmesh_msg_state state, lmesh_fail_reason reason);
uint16_t lmesh_store_next_msg_id(lmesh_node *node);
uint8_t lmesh_store_depth(const lmesh_node *node);
uint32_t lmesh_record_remaining_s(const lmesh_node *node, const lmesh_record *r);
uint16_t lmesh_record_crc(const lmesh_record *r);
void lmesh_record_seal(lmesh_record *r);
bool lmesh_record_valid(const lmesh_record *r);
uint16_t lmesh_crc16(const uint8_t *p, size_t len, uint16_t crc);
void lmesh_store_tick(lmesh_node *node);

#define LMESH_ROLE_MASK(role) ((uint8_t)(1u << (role)))
#define LMESH_CRC16_INIT 0xFFFFu

static inline lmesh_prio lmesh_record_prio(const lmesh_record *r)
{
    return (lmesh_prio)((r->type_prio >> 4) & 0x03u);
}

static inline lmesh_msg_type lmesh_record_type(const lmesh_record *r)
{
    return (lmesh_msg_type)(r->type_prio & 0x0Fu);
}

#endif
