/* Platform port: what the platform provides to the library and what the application provides. See SPEC.md Section 12. */
#ifndef LMESH_PORT_H
#define LMESH_PORT_H

#include "lmesh_types.h"

typedef struct lmesh_port {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*random)(void *ctx);
    int (*radio_send)(void *ctx, const uint8_t *frame, size_t len);
    bool (*radio_busy)(void *ctx);
    uint32_t (*airtime_ms)(void *ctx, size_t len);
    uint16_t (*battery_mv)(void *ctx);
    uint8_t (*health_flags)(void *ctx);
    int (*storage_read)(void *ctx, uint8_t slot, void *buf, size_t len);
    int (*storage_write)(void *ctx, uint8_t slot, const void *buf, size_t len);
} lmesh_port;

typedef struct lmesh_callbacks {
    void *ctx;
    void (*on_event)(void *ctx, const lmesh_event *ev);
    void (*on_command)(void *ctx, lmesh_node_id from, const uint8_t *data, size_t len, lmesh_cmd_result *result);
} lmesh_callbacks;

enum lmesh_storage_slot {
    LMESH_SLOT_MSGID = 0,
    LMESH_SLOT_SEQNO = 1,
    LMESH_SLOT_DEDUP = 2
};

#endif
