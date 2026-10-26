/* Message-layer observation: per-message lifecycle, receipts, handovers, and the custody audit of every store. */
#ifndef LMESH_SIM_MSG_H
#define LMESH_SIM_MSG_H

#include "sim_net.h"

#define SIM_TRACK_MAX 1024
#define SIM_TRACK_STATES 12
#define SIM_RECEIPT_MAX 1024
#define SIM_RECEIPT_DATA 8
#define SIM_MARKS 64
#define SIM_SEQS 256
#define SIM_MSG_TYPE_MASK 0x0Fu
#define SIM_MSG_PRIO_SHIFT 4
#define SIM_MSG_PRIO_MASK 0x30u
#define SIM_MSG_RESEND 0x40u
#define SIM_FLAG_RETRY 0x02u
#define SIM_ACK_ACCEPTED 0u
#define SIM_ACK_DUPLICATE 1u
#define SIM_ACK_REFUSED_INVALID 3u
#define SIM_ROLE_ORIGIN_COPY 1u
#define SIM_ROLE_ORIGIN_SHADOW 2u
#define SIM_ROLE_RELAY 3u

typedef struct sim_data_view {
    lmesh_node_id tx;
    lmesh_node_id rx;
    uint8_t seq;
    bool retry;
    lmesh_msg_ref ref;
    lmesh_node_id dest;
    uint8_t type;
    uint8_t prio;
    bool resend;
    uint32_t lifetime_s;
    uint8_t hops;
    const uint8_t *content;
    uint8_t content_len;
} sim_data_view;

typedef struct sim_track {
    lmesh_msg_ref ref;
    int origin;
    lmesh_node_id dest;
    uint8_t type;
    uint8_t prio;
    uint8_t n_states;
    uint8_t states[SIM_TRACK_STATES];
    uint8_t reasons[SIM_TRACK_STATES];
    uint8_t terminal;
    uint8_t delivered;
    uint8_t received;
    uint8_t rx_hops;
    uint8_t final_state;
    uint8_t final_reason;
    uint8_t result_code;
    bool tx_seen;
    uint16_t resend_frames;
    uint32_t handover_index;
    uint32_t submit_ms;
    uint32_t first_tx_ms;
    uint32_t terminal_ms;
    uint32_t delivered_ms;
    uint32_t received_ms;
} sim_track;

typedef struct sim_receipt {
    lmesh_msg_ref ref;
    lmesh_node_id dest;
    uint16_t orig_id;
    uint8_t code;
    uint8_t len;
    uint8_t data[SIM_RECEIPT_DATA];
    uint32_t first_ms;
} sim_receipt;

typedef struct sim_held {
    lmesh_msg_ref ref;
    uint8_t role;
} sim_held;

typedef struct sim_mark {
    bool used;
    lmesh_msg_ref ref;
    uint8_t status;
} sim_mark;

typedef struct sim_txmap {
    bool data;
    lmesh_msg_ref ref;
    lmesh_node_id to;
} sim_txmap;

typedef struct sim_audit {
    bool primed[SIM_MAX_NODES];
    uint8_t held_n[SIM_MAX_NODES];
    sim_held held[SIM_MAX_NODES][LMESH_STORE_SIZE];
    uint32_t stat_expired[SIM_MAX_NODES];
    uint32_t stat_evicted[SIM_MAX_NODES];
    uint32_t origin_counted[SIM_MAX_NODES];
    sim_mark marks[SIM_MAX_NODES][SIM_MARKS];
    uint8_t mark_next[SIM_MAX_NODES];
    sim_txmap txmap[SIM_MAX_NODES][SIM_SEQS];
    uint8_t depth_max[SIM_MAX_NODES];
    uint8_t depth_final[SIM_MAX_NODES];
    uint32_t store_events[SIM_MAX_NODES];
    uint16_t store_ev_evicted[SIM_MAX_NODES];
    uint16_t store_ev_expired[SIM_MAX_NODES];
    size_t listed_max;
    uint32_t samples;
    uint32_t custody;
    uint32_t refused;
    uint32_t by_counter;
    uint32_t origin_terminal;
    uint32_t death_lost;
    uint32_t unexplained;
    char first[192];
} sim_audit;

typedef struct sim_msgobs {
    uint32_t count;
    uint32_t overflow;
    sim_track t[SIM_TRACK_MAX];
    uint32_t receipt_count;
    sim_receipt receipts[SIM_RECEIPT_MAX];
    uint32_t handovers[SIM_MAX_NODES];
    uint32_t data_first;
    uint32_t data_retries;
    uint32_t resend_frames;
    uint32_t violations;
    char first[192];
    sim_audit audit;
} sim_msgobs;

bool sim_parse_data(const uint8_t *frame, size_t len, sim_data_view *v);
void msgobs_event(sim_msgobs *mo, const sim_net *net, int node, const lmesh_event *ev);
void msgobs_frame(sim_msgobs *mo, const sim_net *net, int from, int to, const uint8_t *frame, size_t len);
void msgobs_sample(sim_msgobs *mo, const sim_net *net);
sim_track *msgobs_find(sim_msgobs *mo, lmesh_msg_ref ref);
bool msgobs_all_terminal(const sim_msgobs *mo);
uint32_t msgobs_count_state(const sim_msgobs *mo, lmesh_msg_state state);

#endif
