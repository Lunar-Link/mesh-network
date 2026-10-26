/* Identifiers, enumerations, events, and query rows shared by the library, the platform, and the application. */
#ifndef LMESH_TYPES_H
#define LMESH_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lmesh_config.h"

typedef uint8_t lmesh_node_id;
#define LMESH_ID_NONE 0u
#define LMESH_ID_ALL 0xFFu
#define LMESH_METRIC_INF 0xFFu

typedef struct lmesh_node lmesh_node;

typedef enum lmesh_err {
    LMESH_OK = 0,
    LMESH_ERR_ARG,
    LMESH_ERR_FULL,
    LMESH_ERR_TOO_LONG,
    LMESH_ERR_NOT_FOUND,
    LMESH_ERR_STATE,
    LMESH_ERR_UNIMPLEMENTED
} lmesh_err;

typedef enum lmesh_prio {
    LMESH_PRIO_URGENT = 0,
    LMESH_PRIO_NORMAL = 1,
    LMESH_PRIO_ROUTINE = 2,
    LMESH_PRIO_COUNT = 3
} lmesh_prio;

typedef enum lmesh_msg_type {
    LMESH_MSG_TEXT = 0,
    LMESH_MSG_COMMAND = 1,
    LMESH_MSG_TELEMETRY = 2,
    LMESH_MSG_RECEIPT = 3
} lmesh_msg_type;

typedef enum lmesh_msg_state {
    LMESH_STATE_QUEUED = 0,
    LMESH_STATE_TRANSMITTED,
    LMESH_STATE_DELIVERED,
    LMESH_STATE_EXPIRED,
    LMESH_STATE_FAILED
} lmesh_msg_state;

typedef enum lmesh_result_code {
    LMESH_RESULT_DELIVERED = 0,
    LMESH_RESULT_EXECUTED = 1,
    LMESH_RESULT_REJECTED = 2,
    LMESH_RESULT_UNSUPPORTED = 3
} lmesh_result_code;

typedef enum lmesh_fail_reason {
    LMESH_REASON_NONE = 0,
    LMESH_REASON_HOP_ACCEPTED,
    LMESH_REASON_RECEIPT,
    LMESH_REASON_LIFETIME,
    LMESH_REASON_EVICTED,
    LMESH_REASON_INVALID,
    LMESH_REASON_REJECTED,
    LMESH_REASON_CANCELED,
    LMESH_REASON_RESEND
} lmesh_fail_reason;

typedef struct lmesh_msg_ref {
    lmesh_node_id origin;
    uint16_t id;
} lmesh_msg_ref;

typedef struct lmesh_cmd_result {
    uint8_t code;
    uint8_t len;
    uint8_t data[LMESH_RESULT_MAX];
} lmesh_cmd_result;

typedef struct lmesh_identity {
    lmesh_node_id id;
    uint8_t network_id;
    uint8_t key[16];
    uint8_t station_id_len;
    char station_id[LMESH_STATION_ID_MAX];
} lmesh_identity;

typedef enum lmesh_event_type {
    LMESH_EV_BOOT = 0,
    LMESH_EV_NEIGHBOR_UP,
    LMESH_EV_NEIGHBOR_DOWN,
    LMESH_EV_LINK_QUALITY,
    LMESH_EV_ROUTE_CHANGED,
    LMESH_EV_ROUTE_LOST,
    LMESH_EV_MSG_STATE,
    LMESH_EV_MSG_RECEIVED,
    LMESH_EV_STATUS_RECEIVED,
    LMESH_EV_STORE
} lmesh_event_type;

typedef struct lmesh_event {
    lmesh_event_type type;
    uint32_t t_ms;
    union {
        struct {
            lmesh_node_id id;
            uint8_t etx8;
            int16_t rssi;
            int8_t snr;
        } neighbor;
        struct {
            lmesh_node_id dest;
            lmesh_node_id next_hop;
            uint8_t metric8;
            uint16_t seqno;
        } route;
        struct {
            lmesh_msg_ref ref;
            lmesh_node_id dest;
            lmesh_msg_type msg_type;
            lmesh_prio prio;
            lmesh_msg_state state;
            uint8_t reason;
            uint8_t result_code;
        } msg_state;
        struct {
            lmesh_msg_ref ref;
            lmesh_msg_type msg_type;
            lmesh_prio prio;
            uint8_t hops;
            uint8_t len;
            const uint8_t *data;
        } msg_received;
        struct {
            lmesh_node_id id;
        } status;
        struct {
            uint8_t depth;
            uint16_t evicted;
            uint16_t expired;
        } store;
    } u;
} lmesh_event;

typedef struct lmesh_neighbor_info {
    lmesh_node_id id;
    bool up;
    bool bidirectional;
    uint8_t etx8;
    uint8_t rx_quality;
    uint8_t tx_quality;
    int16_t rssi;
    int8_t snr;
    uint32_t last_heard_ms;
} lmesh_neighbor_info;

typedef struct lmesh_route_info {
    lmesh_node_id dest;
    lmesh_node_id next_hop;
    uint8_t metric8;
    uint16_t seqno;
    bool retracted;
} lmesh_route_info;

typedef struct lmesh_msg_info {
    lmesh_msg_ref ref;
    lmesh_node_id dest;
    lmesh_msg_type type;
    lmesh_prio prio;
    lmesh_msg_state state;
    uint8_t role;
    uint32_t lifetime_s;
    uint8_t attempts;
    uint8_t hops;
} lmesh_msg_info;

typedef struct lmesh_node_info {
    lmesh_node_id id;
    bool stale;
    uint16_t battery_mv;
    uint8_t flags;
    uint32_t uptime_s;
    uint8_t store_depth;
    uint8_t lost;
    uint8_t station_id_len;
    char station_id[LMESH_STATION_ID_MAX];
    uint8_t neighbor_count;
    lmesh_node_id neighbors[LMESH_MAX_NEIGHBORS];
    uint8_t neighbor_etx8[LMESH_MAX_NEIGHBORS];
    uint32_t last_heard_ms;
} lmesh_node_info;

typedef struct lmesh_stats {
    uint32_t tx_frames[16];
    uint32_t rx_frames[16];
    uint32_t rx_bad_tag;
    uint32_t rx_foreign;
    uint32_t rx_malformed;
    uint32_t ack_timeouts;
    uint32_t link_failures;
    uint32_t dup_hop;
    uint32_t dup_relay;
    uint32_t dup_dest;
    uint32_t evicted;
    uint32_t expired;
    uint32_t delivered;
    uint32_t hop_limit_drops;
    uint32_t record_crc_errors;
    uint32_t airtime_ms;
    uint32_t dedup_outside;
} lmesh_stats;

#endif
