/* Duplicate suppression: per-origin sliding windows at the destination, recent-forward cache at relays. SPEC.md Section 9.6. */
#ifndef LMESH_DEDUP_H
#define LMESH_DEDUP_H

#include "internal.h"

#ifndef LMESH_DEDUP_SAVE_GAP_MS
#define LMESH_DEDUP_SAVE_GAP_MS 10000u
#endif
#define LMESH_DEDUP_WINDOW_BITS 32u

bool lmesh_dedup_dest_accept(lmesh_node *node, lmesh_node_id origin, uint16_t id);
void lmesh_dedup_remember_result(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t code);
bool lmesh_dedup_lookup_result(lmesh_node *node, lmesh_node_id origin, uint16_t id, uint8_t *code);
bool lmesh_dedup_relay_accept(lmesh_node *node, lmesh_node_id origin, uint16_t id);
lmesh_dedup_window *lmesh_dedup_window_for(lmesh_node *node, lmesh_node_id origin);
void lmesh_dedup_load(lmesh_node *node);
void lmesh_dedup_save(lmesh_node *node);
void lmesh_dedup_tick(lmesh_node *node);

#endif
