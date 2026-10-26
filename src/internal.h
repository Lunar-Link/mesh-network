/* Helpers shared by the library modules: byte order, wrap-safe time, events, randomness, airtime. */
#ifndef LMESH_INTERNAL_H
#define LMESH_INTERNAL_H

#include "lmesh/lmesh.h"

static inline void lmesh_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t lmesh_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline void lmesh_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t lmesh_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline bool lmesh_time_reached(uint32_t now_ms, uint32_t t_ms)
{
    return (int32_t)(now_ms - t_ms) >= 0;
}

static inline bool lmesh_seqno_newer(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}

void lmesh_emit(lmesh_node *node, const lmesh_event *ev);
void lmesh_wake_at(lmesh_node *node, uint32_t t_ms);
uint32_t lmesh_rand_range(lmesh_node *node, uint32_t lo, uint32_t hi);
uint32_t lmesh_airtime(const lmesh_node *node, size_t len);

#endif
