/* Controlled flood with duplicate cache and hop limit. SPEC.md Section 6.4. */
#ifndef LMESH_FLOOD_H
#define LMESH_FLOOD_H

#include "frame.h"

lmesh_err lmesh_flood_originate(lmesh_node *node, uint8_t type, const uint8_t *body, size_t len);
bool lmesh_flood_accept(lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, size_t len);

#endif
