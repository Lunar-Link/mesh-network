/* Complete node state. Public so the application can allocate it statically; the contents are private to the library. */
#ifndef LMESH_STATE_H
#define LMESH_STATE_H

#include "lmesh_port.h"
#include "lmesh_types.h"

typedef struct lmesh_frame {
    uint8_t len;
    uint8_t buf[LMESH_MAX_FRAME];
} lmesh_frame;

/* Link layer */

typedef struct lmesh_txq_entry {
    bool used;
    bool want_ack;
    uint8_t band;
    uint8_t attempts;
    uint8_t owner;
    uint16_t owner_ref;
    uint32_t due_ms;
    lmesh_frame frame;
} lmesh_txq_entry;

typedef struct lmesh_link_state {
    lmesh_txq_entry queue[LMESH_TXQ_SIZE];
    int8_t in_flight;
    bool tx_active;
    bool awaiting_ack;
    uint32_t ack_deadline_ms;
    uint8_t frame_seq;
    uint8_t wrr_credit[LMESH_PRIO_COUNT];
    struct {
        lmesh_node_id id;
        uint8_t last_seq;
    } last_rx[LMESH_MAX_NEIGHBORS];
    bool ack_pending;
    bool ack_on_air;
    lmesh_node_id ack_to;
    uint8_t ack_seq;
    uint8_t ack_status;
    uint8_t ack_buf[LMESH_HEADER_LEN + 2 + LMESH_TAG_LEN];
} lmesh_link_state;

typedef struct lmesh_flood_entry {
    bool used;
    lmesh_node_id origin;
    uint8_t seq;
} lmesh_flood_entry;

typedef struct lmesh_flood_state {
    lmesh_flood_entry cache[LMESH_FLOOD_CACHE];
    uint8_t next;
    uint8_t own_seq;
} lmesh_flood_state;

/* Routing layer */

typedef struct lmesh_adv {
    bool valid;
    uint8_t metric8;
    uint16_t seqno;
    uint32_t rx_ms;
} lmesh_adv;

typedef struct lmesh_neighbor {
    bool used;
    bool up;
    bool bidirectional;
    lmesh_node_id id;
    uint8_t last_hello_seq;
    uint16_t rx_bitmap;
    uint8_t rx_quality;
    uint8_t tx_quality;
    uint8_t etx8;
    uint8_t fail_count;
    int16_t rssi;
    int8_t snr;
    uint32_t last_heard_ms;
    uint32_t last_hello_ms;
    uint32_t tx_quality_ms;
    lmesh_adv adv[LMESH_MAX_NODES];
    bool own_seqno_valid;
    uint16_t own_seqno;
} lmesh_neighbor;

typedef struct lmesh_neighbor_state {
    lmesh_neighbor table[LMESH_MAX_NEIGHBORS];
    uint8_t hello_seq;
} lmesh_neighbor_state;

typedef struct lmesh_hello_state {
    bool trigger_pending;
    uint8_t page;
    uint32_t next_ms;
    uint32_t trigger_at_ms;
    uint32_t last_triggered_ms;
} lmesh_hello_state;

typedef struct lmesh_route {
    bool used;
    bool fd_valid;
    lmesh_node_id dest;
    lmesh_node_id next_hop;
    uint8_t metric8;
    uint8_t fd_metric8;
    uint8_t retract_left;
    uint8_t better_count;
    uint16_t seqno;
    uint16_t fd_seqno;
    uint32_t refreshed_ms;
    uint32_t last_req_ms;
} lmesh_route;

typedef struct lmesh_route_state {
    lmesh_route table[LMESH_MAX_NODES];
    uint16_t own_seqno;
} lmesh_route_state;

/* Message layer */

typedef enum lmesh_record_role {
    LMESH_ROLE_FREE = 0,
    LMESH_ROLE_ORIGIN_COPY,
    LMESH_ROLE_ORIGIN_SHADOW,
    LMESH_ROLE_RELAY,
    LMESH_ROLE_HISTORY
} lmesh_record_role;

typedef struct lmesh_record {
    lmesh_node_id origin;
    lmesh_node_id dest;
    uint16_t id;
    uint8_t type_prio;
    uint8_t hops;
    uint32_t lifetime_s;
    uint8_t content_len;
    uint8_t content[LMESH_MAX_CONTENT];
    uint8_t role;
    uint8_t state;
    uint8_t attempts;
    uint8_t result_code;
    lmesh_node_id in_flight_to;
    uint32_t acquired_ms;
    uint32_t next_attempt_ms;
    uint32_t e2e_backoff_ms;
    uint32_t submitted_ms;
    uint16_t crc;
} lmesh_record;

typedef struct lmesh_store_state {
    lmesh_record records[LMESH_STORE_SIZE];
    uint16_t next_msg_id;
    uint16_t ids_since_save;
    uint16_t evicted;
    uint16_t expired;
    uint32_t last_sweep_ms;
} lmesh_store_state;

typedef struct lmesh_message_state {
    int8_t in_flight;
    uint8_t wrr_credit[LMESH_PRIO_COUNT];
} lmesh_message_state;

typedef struct lmesh_dedup_window {
    bool used;
    lmesh_node_id origin;
    uint16_t highest;
    uint32_t bitmap[LMESH_DEDUP_WINDOW_BITS / 32];
    uint16_t cmd_id[LMESH_DEDUP_CMD_HISTORY];
    uint8_t cmd_code[LMESH_DEDUP_CMD_HISTORY];
    uint8_t cmd_count;
    uint8_t cmd_next;
    uint32_t last_used_ms;
} lmesh_dedup_window;

typedef struct lmesh_fwd_entry {
    bool used;
    lmesh_node_id origin;
    uint16_t id;
    uint32_t t_ms;
} lmesh_fwd_entry;

typedef struct lmesh_dedup_state {
    lmesh_dedup_window windows[LMESH_MAX_NODES];
    lmesh_fwd_entry fwd[LMESH_FWD_CACHE];
    uint8_t fwd_next;
    bool dirty;
    uint32_t last_save_ms;
} lmesh_dedup_state;

typedef struct lmesh_node_entry {
    bool used;
    lmesh_node_id id;
    uint16_t battery_mv;
    uint8_t flags;
    uint8_t store_depth;
    uint8_t lost;
    uint8_t station_id_len;
    char station_id[LMESH_STATION_ID_MAX];
    uint8_t neighbor_count;
    lmesh_node_id neighbors[LMESH_MAX_NEIGHBORS];
    uint8_t neighbor_etx8[LMESH_MAX_NEIGHBORS];
    uint32_t uptime_s;
    uint32_t last_heard_ms;
} lmesh_node_entry;

typedef struct lmesh_status_state {
    lmesh_node_entry table[LMESH_MAX_NODES];
    bool reset_flag;
    uint8_t lost_since;
    uint16_t lost_seen;
    uint32_t next_ms;
    uint32_t boot_ms;
    uint32_t uptime_s;
    uint32_t uptime_mark_ms;
} lmesh_status_state;

struct lmesh_node {
    lmesh_identity ident;
    lmesh_port port;
    lmesh_callbacks cb;
    uint32_t now_ms;
    uint32_t next_wake_ms;
    lmesh_link_state link;
    lmesh_flood_state flood;
    lmesh_neighbor_state nbr;
    lmesh_hello_state hello;
    lmesh_route_state route;
    lmesh_store_state store;
    lmesh_message_state msg;
    lmesh_dedup_state dedup;
    lmesh_status_state status;
    lmesh_stats stats;
};

#endif
