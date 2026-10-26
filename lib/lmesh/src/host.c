/* Line protocol for the local interface: commands in, JSON lines out. See SPEC.md Section 11. */
#include <string.h>

#include "internal.h"

typedef struct jw {
    char *buf;
    size_t cap;
    size_t len;
    bool full;
} jw;

static void jw_init(jw *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = buf ? cap : 0;
    w->len = 0;
    w->full = false;
    if (w->cap > 0) {
        w->buf[0] = '\0';
    }
}

static void jw_raw(jw *w, const char *s, size_t n)
{
    if (w->cap == 0) {
        w->full = w->full || n > 0;
        return;
    }
    size_t room = w->cap - 1 - w->len;
    if (n > room) {
        n = room;
        w->full = true;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void jw_ch(jw *w, char c)
{
    jw_raw(w, &c, 1);
}

static void jw_str(jw *w, const char *s)
{
    jw_raw(w, s, strlen(s));
}

static void jw_u32(jw *w, uint32_t v)
{
    char tmp[10];
    size_t i = sizeof tmp;
    do {
        tmp[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    jw_raw(w, tmp + i, sizeof tmp - i);
}

static void jw_i32(jw *w, int32_t v)
{
    if (v < 0) {
        jw_ch(w, '-');
        jw_u32(w, 0u - (uint32_t)v);
    } else {
        jw_u32(w, (uint32_t)v);
    }
}

static void jw_bool(jw *w, bool v)
{
    jw_str(w, v ? "true" : "false");
}

static void jw_etx(jw *w, uint8_t v)
{
    if (v == LMESH_METRIC_INF) {
        jw_str(w, "\"inf\"");
        return;
    }
    uint32_t frac = ((uint32_t)(v % 8u) * 100u + 4u) / 8u;
    jw_u32(w, (uint32_t)(v / 8u));
    jw_ch(w, '.');
    jw_ch(w, (char)('0' + frac / 10u));
    jw_ch(w, (char)('0' + frac % 10u));
}

static const char hex_digits[] = "0123456789abcdef";

static void jw_hex(jw *w, const uint8_t *data, size_t n)
{
    jw_ch(w, '"');
    for (size_t i = 0; i < n; i++) {
        char pair[2] = {hex_digits[data[i] >> 4], hex_digits[data[i] & 0x0Fu]};
        jw_raw(w, pair, 2);
    }
    jw_ch(w, '"');
}

static void jw_esc(jw *w, const uint8_t *s, size_t n, bool ascii_only)
{
    jw_ch(w, '"');
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            jw_raw(w, e, 2);
        } else if (c < 0x20u || (ascii_only && c >= 0x80u)) {
            char e[6] = {'\\', 'u', '0', '0', hex_digits[c >> 4], hex_digits[c & 0x0Fu]};
            jw_raw(w, e, 6);
        } else {
            jw_ch(w, (char)c);
        }
    }
    jw_ch(w, '"');
}

static void jw_key(jw *w, const char *key)
{
    jw_str(w, ",\"");
    jw_str(w, key);
    jw_str(w, "\":");
}

static void jw_ku(jw *w, const char *key, uint32_t v)
{
    jw_key(w, key);
    jw_u32(w, v);
}

static void jw_ks(jw *w, const char *key, const char *name)
{
    jw_key(w, key);
    jw_ch(w, '"');
    jw_str(w, name);
    jw_ch(w, '"');
}

static void jw_array(jw *w, const uint32_t *v, size_t n)
{
    jw_ch(w, '[');
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            jw_ch(w, ',');
        }
        jw_u32(w, v[i]);
    }
    jw_ch(w, ']');
}

static bool text_printable(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80u) {
            if (c < 0x20u && c != '\t') {
                return false;
            }
            i++;
            continue;
        }
        size_t extra;
        uint32_t cp;
        uint32_t min;
        if ((c & 0xE0u) == 0xC0u) {
            extra = 1;
            cp = c & 0x1Fu;
            min = 0x80u;
        } else if ((c & 0xF0u) == 0xE0u) {
            extra = 2;
            cp = c & 0x0Fu;
            min = 0x800u;
        } else if ((c & 0xF8u) == 0xF0u) {
            extra = 3;
            cp = c & 0x07u;
            min = 0x10000u;
        } else {
            return false;
        }
        if (n - i <= extra) {
            return false;
        }
        for (size_t k = 1; k <= extra; k++) {
            uint8_t b = s[i + k];
            if ((b & 0xC0u) != 0x80u) {
                return false;
            }
            cp = (cp << 6) | (uint32_t)(b & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

static const char *const state_names[] = {"queued", "transmitted", "delivered", "expired", "failed"};
static const char *const type_names[] = {"text", "command", "telemetry", "receipt"};
static const char *const prio_names[] = {"urgent", "normal", "routine"};
static const char *const reason_names[] = {"none",    "hop_accepted", "receipt",  "lifetime", "evicted",
                                           "invalid", "rejected",     "canceled", "resend"};
static const char *const result_names[] = {"delivered", "executed", "rejected", "unsupported"};
static const char *const role_names[] = {"free", "origin_copy", "origin_shadow", "relay", "history"};

static const char *name_of(const char *const *names, size_t count, unsigned v)
{
    return v < count ? names[v] : "unknown";
}

#define NAME(tbl, v) name_of((tbl), sizeof(tbl) / sizeof((tbl)[0]), (unsigned)(v))

static const char *event_name(lmesh_event_type type)
{
    switch (type) {
    case LMESH_EV_BOOT:
        return "boot";
    case LMESH_EV_NEIGHBOR_UP:
    case LMESH_EV_NEIGHBOR_DOWN:
        return "neighbor";
    case LMESH_EV_LINK_QUALITY:
        return "link";
    case LMESH_EV_ROUTE_CHANGED:
    case LMESH_EV_ROUTE_LOST:
        return "route";
    case LMESH_EV_MSG_STATE:
        return "msg";
    case LMESH_EV_MSG_RECEIVED:
        return "rx";
    case LMESH_EV_STATUS_RECEIVED:
        return "status";
    case LMESH_EV_STORE:
        return "store";
    }
    return "unknown";
}

size_t lmesh_host_format_event(const lmesh_event *ev, char *out, size_t out_max)
{
    jw w;
    jw_init(&w, out, out_max);
    if (!ev) {
        return 0;
    }

    jw_str(&w, "{\"ev\":\"");
    jw_str(&w, event_name(ev->type));
    jw_ch(&w, '"');
    jw_ku(&w, "t", ev->t_ms);

    switch (ev->type) {
    case LMESH_EV_BOOT:
        break;
    case LMESH_EV_NEIGHBOR_UP:
    case LMESH_EV_NEIGHBOR_DOWN:
        jw_ku(&w, "id", ev->u.neighbor.id);
        jw_ks(&w, "state", ev->type == LMESH_EV_NEIGHBOR_UP ? "up" : "down");
        jw_key(&w, "etx");
        jw_etx(&w, ev->u.neighbor.etx8);
        break;
    case LMESH_EV_LINK_QUALITY:
        jw_ku(&w, "id", ev->u.neighbor.id);
        jw_key(&w, "etx");
        jw_etx(&w, ev->u.neighbor.etx8);
        jw_key(&w, "rssi");
        jw_i32(&w, ev->u.neighbor.rssi);
        jw_key(&w, "snr");
        jw_i32(&w, ev->u.neighbor.snr);
        break;
    case LMESH_EV_ROUTE_CHANGED:
    case LMESH_EV_ROUTE_LOST:
        jw_ku(&w, "dest", ev->u.route.dest);
        jw_ku(&w, "next", ev->type == LMESH_EV_ROUTE_LOST ? 0u : ev->u.route.next_hop);
        jw_key(&w, "metric");
        jw_etx(&w, ev->u.route.metric8);
        jw_ku(&w, "seq", ev->u.route.seqno);
        break;
    case LMESH_EV_MSG_STATE:
        jw_ku(&w, "origin", ev->u.msg_state.ref.origin);
        jw_ku(&w, "id", ev->u.msg_state.ref.id);
        jw_ku(&w, "dest", ev->u.msg_state.dest);
        jw_ks(&w, "state", NAME(state_names, ev->u.msg_state.state));
        jw_ks(&w, "reason", NAME(reason_names, ev->u.msg_state.reason));
        jw_ks(&w, "type", NAME(type_names, ev->u.msg_state.msg_type));
        jw_ks(&w, "prio", NAME(prio_names, ev->u.msg_state.prio));
        if (ev->u.msg_state.state == LMESH_STATE_DELIVERED) {
            jw_ks(&w, "result", NAME(result_names, ev->u.msg_state.result_code));
        }
        break;
    case LMESH_EV_MSG_RECEIVED: {
        const uint8_t *data = ev->u.msg_received.data;
        size_t len = data ? ev->u.msg_received.len : 0;
        jw_ku(&w, "origin", ev->u.msg_received.ref.origin);
        jw_ku(&w, "id", ev->u.msg_received.ref.id);
        jw_ks(&w, "type", NAME(type_names, ev->u.msg_received.msg_type));
        jw_ks(&w, "prio", NAME(prio_names, ev->u.msg_received.prio));
        jw_ku(&w, "hops", ev->u.msg_received.hops);
        if (ev->u.msg_received.msg_type == LMESH_MSG_TEXT && text_printable(data, len)) {
            jw_key(&w, "text");
            jw_esc(&w, data, len, false);
        } else {
            jw_key(&w, "hex");
            jw_hex(&w, data, len);
        }
        break;
    }
    case LMESH_EV_STATUS_RECEIVED:
        jw_ku(&w, "node", ev->u.status.id);
        break;
    case LMESH_EV_STORE:
        jw_ku(&w, "depth", ev->u.store.depth);
        jw_ku(&w, "evicted", ev->u.store.evicted);
        jw_ku(&w, "expired", ev->u.store.expired);
        break;
    default:
        jw_ku(&w, "type", (uint32_t)ev->type);
        break;
    }

    jw_str(&w, "}\n");
    return w.len;
}

/* Command parsing */

typedef struct cursor {
    const char *p;
    const char *end;
} cursor;

typedef struct token {
    const char *s;
    size_t len;
} token;

static void skip_spaces(cursor *c)
{
    while (c->p < c->end && *c->p == ' ') {
        c->p++;
    }
}

static bool next_token(cursor *c, token *t)
{
    skip_spaces(c);
    t->s = c->p;
    while (c->p < c->end && *c->p != ' ') {
        c->p++;
    }
    t->len = (size_t)(c->p - t->s);
    return t->len > 0;
}

static bool at_end(cursor *c)
{
    skip_spaces(c);
    return c->p == c->end;
}

static bool tok_is(const token *t, const char *word)
{
    size_t n = strlen(word);
    return t->len == n && memcmp(t->s, word, n) == 0;
}

static bool parse_u32(const token *t, uint32_t *out)
{
    if (t->len == 0) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < t->len; i++) {
        char ch = t->s[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        uint32_t d = (uint32_t)(ch - '0');
        if (v > (UINT32_MAX - d) / 10u) {
            return false;
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static const char *err_reason(lmesh_err err)
{
    switch (err) {
    case LMESH_OK:
        return "ok";
    case LMESH_ERR_ARG:
        return "invalid_argument";
    case LMESH_ERR_FULL:
        return "store_full";
    case LMESH_ERR_TOO_LONG:
        return "too_long";
    case LMESH_ERR_NOT_FOUND:
        return "not_found";
    case LMESH_ERR_STATE:
        return "wrong_state";
    case LMESH_ERR_UNIMPLEMENTED:
        return "unimplemented";
    }
    return "error";
}

static void reply_fail(jw *w, const char *reason)
{
    jw_str(w, "{\"ok\":false,\"reason\":\"");
    jw_str(w, reason);
    jw_str(w, "\"}\n");
}

static void reply_ref(jw *w, lmesh_err err, lmesh_msg_ref ref)
{
    if (err != LMESH_OK) {
        reply_fail(w, err_reason(err));
        return;
    }
    jw_str(w, "{\"ok\":true");
    jw_ku(w, "origin", ref.origin);
    jw_ku(w, "id", ref.id);
    jw_str(w, "}\n");
}

static const char *parse_dest(cursor *c, lmesh_node_id *dest)
{
    token t;
    uint32_t v;
    if (!next_token(c, &t)) {
        return "missing_argument";
    }
    if (!parse_u32(&t, &v) || v == LMESH_ID_NONE || v >= LMESH_ID_ALL) {
        return "bad_dest";
    }
    *dest = (lmesh_node_id)v;
    return NULL;
}

static const char *parse_prio(cursor *c, lmesh_prio *prio)
{
    token t;
    if (!next_token(c, &t)) {
        return "missing_argument";
    }
    if (tok_is(&t, "u")) {
        *prio = LMESH_PRIO_URGENT;
    } else if (tok_is(&t, "n")) {
        *prio = LMESH_PRIO_NORMAL;
    } else if (tok_is(&t, "r")) {
        *prio = LMESH_PRIO_ROUTINE;
    } else {
        return "bad_prio";
    }
    return NULL;
}

static const char *parse_lifetime(cursor *c, uint32_t *lifetime_s)
{
    token t;
    if (!next_token(c, &t)) {
        return "missing_argument";
    }
    if (!parse_u32(&t, lifetime_s) || *lifetime_s == 0) {
        return "bad_lifetime";
    }
    return NULL;
}

static const char *cmd_send(lmesh_node *node, jw *w, cursor *c, lmesh_msg_type type)
{
    lmesh_node_id dest = LMESH_ID_NONE;
    lmesh_prio prio = LMESH_PRIO_NORMAL;
    uint32_t lifetime_s = 0;
    lmesh_msg_ref ref = {0, 0};
    const char *err = parse_dest(c, &dest);
    if (!err && type == LMESH_MSG_TEXT) {
        err = parse_prio(c, &prio);
    }
    if (!err) {
        err = parse_lifetime(c, &lifetime_s);
    }
    if (err) {
        return err;
    }

    if (type == LMESH_MSG_TELEMETRY) {
        token t;
        uint8_t content[LMESH_MAX_CONTENT];
        if (!next_token(c, &t)) {
            return "missing_argument";
        }
        if (!at_end(c)) {
            return "unexpected_argument";
        }
        if (t.len % 2u != 0) {
            return "bad_hex";
        }
        if (t.len / 2u > sizeof content) {
            return "too_long";
        }
        for (size_t i = 0; i < t.len / 2u; i++) {
            int hi = hex_value(t.s[2 * i]);
            int lo = hex_value(t.s[2 * i + 1]);
            if (hi < 0 || lo < 0) {
                return "bad_hex";
            }
            content[i] = (uint8_t)((hi << 4) | lo);
        }
        reply_ref(w, lmesh_send(node, dest, type, prio, lifetime_s, content, t.len / 2u, &ref), ref);
        return NULL;
    }

    skip_spaces(c);
    if (c->p == c->end) {
        return "missing_argument";
    }
    const uint8_t *text = (const uint8_t *)c->p;
    size_t len = (size_t)(c->end - c->p);
    reply_ref(w, lmesh_send(node, dest, type, prio, lifetime_s, text, len, &ref), ref);
    return NULL;
}

static const char *cmd_cancel(lmesh_node *node, jw *w, cursor *c)
{
    token t;
    uint32_t v;
    if (!next_token(c, &t)) {
        return "missing_argument";
    }
    if (!parse_u32(&t, &v) || v > UINT16_MAX) {
        return "bad_id";
    }
    if (!at_end(c)) {
        return "unexpected_argument";
    }
    lmesh_msg_ref ref = {node->ident.id, (uint16_t)v};
    reply_ref(w, lmesh_cancel(node, ref), ref);
    return NULL;
}

static void cmd_id(lmesh_node *node, jw *w)
{
    size_t slen = node->ident.station_id_len;
    if (slen > LMESH_STATION_ID_MAX) {
        slen = LMESH_STATION_ID_MAX;
    }
    jw_str(w, "{\"ok\":true");
    jw_ku(w, "id", node->ident.id);
    jw_ku(w, "net", node->ident.network_id);
    jw_key(w, "station");
    jw_esc(w, (const uint8_t *)node->ident.station_id, slen, true);
    jw_str(w, "}\n");
}

static void cmd_status(lmesh_node *node, jw *w)
{
    lmesh_err err = lmesh_status_now(node);
    if (err == LMESH_OK) {
        jw_str(w, "{\"ok\":true}\n");
    } else {
        reply_fail(w, err_reason(err));
    }
}

/* Table dumps: whole rows only, keeping room for the end line */

typedef struct dump {
    jw *w;
    const char *name;
    size_t reserve_last;
    size_t reserve_more;
    size_t mark;
    bool truncated;
} dump;

static void dump_init(dump *d, jw *w, const char *name)
{
    d->w = w;
    d->name = name;
    d->reserve_last = strlen("{\"end\":\"\"}\n") + strlen(name);
    d->reserve_more = d->reserve_last + strlen(",\"truncated\":true");
    d->mark = 0;
    d->truncated = false;
}

static void row_begin(dump *d)
{
    d->mark = d->w->len;
}

static void row_finish(dump *d, bool last)
{
    jw *w = d->w;
    size_t reserve = last ? d->reserve_last : d->reserve_more;
    if (w->full || w->len + reserve + 1 > w->cap) {
        w->len = d->mark;
        if (w->cap > 0) {
            w->buf[w->len] = '\0';
        }
        w->full = false;
        d->truncated = true;
    }
}

static void dump_end(dump *d)
{
    jw_str(d->w, "{\"end\":\"");
    jw_str(d->w, d->name);
    jw_ch(d->w, '"');
    if (d->truncated) {
        jw_str(d->w, ",\"truncated\":true");
    }
    jw_str(d->w, "}\n");
}

static void dump_links(lmesh_node *node, jw *w)
{
    lmesh_neighbor_info rows[LMESH_MAX_NEIGHBORS];
    size_t n = lmesh_neighbors(node, rows, LMESH_MAX_NEIGHBORS);
    dump d;
    dump_init(&d, w, "links");
    for (size_t i = 0; i < n && !d.truncated; i++) {
        const lmesh_neighbor_info *r = &rows[i];
        row_begin(&d);
        jw_str(w, "{\"id\":");
        jw_u32(w, r->id);
        jw_key(w, "up");
        jw_bool(w, r->up);
        jw_key(w, "bidir");
        jw_bool(w, r->bidirectional);
        jw_key(w, "etx");
        jw_etx(w, r->etx8);
        jw_ku(w, "rxq", r->rx_quality);
        jw_ku(w, "txq", r->tx_quality);
        jw_key(w, "rssi");
        jw_i32(w, r->rssi);
        jw_key(w, "snr");
        jw_i32(w, r->snr);
        jw_ku(w, "heard_ms", r->last_heard_ms);
        jw_str(w, "}\n");
        row_finish(&d, i + 1 == n);
    }
    dump_end(&d);
}

static void dump_routes(lmesh_node *node, jw *w)
{
    lmesh_route_info rows[LMESH_MAX_NODES];
    size_t n = lmesh_routes(node, rows, LMESH_MAX_NODES);
    dump d;
    dump_init(&d, w, "routes");
    for (size_t i = 0; i < n && !d.truncated; i++) {
        const lmesh_route_info *r = &rows[i];
        row_begin(&d);
        jw_str(w, "{\"dest\":");
        jw_u32(w, r->dest);
        jw_ku(w, "next", r->next_hop);
        jw_key(w, "metric");
        jw_etx(w, r->metric8);
        jw_ku(w, "seq", r->seqno);
        jw_key(w, "retracted");
        jw_bool(w, r->retracted);
        jw_str(w, "}\n");
        row_finish(&d, i + 1 == n);
    }
    dump_end(&d);
}

static void dump_nodes(lmesh_node *node, jw *w)
{
    lmesh_node_info rows[LMESH_MAX_NODES];
    size_t n = lmesh_nodes(node, rows, LMESH_MAX_NODES);
    dump d;
    dump_init(&d, w, "nodes");
    for (size_t i = 0; i < n && !d.truncated; i++) {
        const lmesh_node_info *r = &rows[i];
        size_t slen = r->station_id_len > LMESH_STATION_ID_MAX ? LMESH_STATION_ID_MAX : r->station_id_len;
        size_t nbrs = r->neighbor_count > LMESH_MAX_NEIGHBORS ? LMESH_MAX_NEIGHBORS : r->neighbor_count;
        row_begin(&d);
        jw_str(w, "{\"node\":");
        jw_u32(w, r->id);
        jw_ku(w, "batt_mv", r->battery_mv);
        jw_ku(w, "flags", r->flags);
        jw_ku(w, "uptime_s", r->uptime_s);
        jw_ku(w, "store", r->store_depth);
        jw_ku(w, "lost", r->lost);
        jw_key(w, "station");
        jw_esc(w, (const uint8_t *)r->station_id, slen, true);
        jw_key(w, "stale");
        jw_bool(w, r->stale);
        jw_ku(w, "heard_ms", r->last_heard_ms);
        jw_str(w, ",\"neighbors\":[");
        for (size_t k = 0; k < nbrs; k++) {
            jw_str(w, k == 0 ? "[" : ",[");
            jw_u32(w, r->neighbors[k]);
            jw_ch(w, ',');
            jw_etx(w, r->neighbor_etx8[k]);
            jw_ch(w, ']');
        }
        jw_str(w, "]}\n");
        row_finish(&d, i + 1 == n);
    }
    dump_end(&d);
}

static void dump_msgs(lmesh_node *node, jw *w)
{
    lmesh_msg_info rows[LMESH_STORE_SIZE];
    size_t n = lmesh_messages(node, rows, LMESH_STORE_SIZE);
    dump d;
    dump_init(&d, w, "msgs");
    for (size_t i = 0; i < n && !d.truncated; i++) {
        const lmesh_msg_info *r = &rows[i];
        row_begin(&d);
        jw_str(w, "{\"origin\":");
        jw_u32(w, r->ref.origin);
        jw_ku(w, "id", r->ref.id);
        jw_ku(w, "dest", r->dest);
        jw_ks(w, "type", NAME(type_names, r->type));
        jw_ks(w, "prio", NAME(prio_names, r->prio));
        jw_ks(w, "state", NAME(state_names, r->state));
        jw_ks(w, "role", NAME(role_names, r->role));
        jw_ku(w, "lifetime_s", r->lifetime_s);
        jw_ku(w, "attempts", r->attempts);
        jw_ku(w, "hops", r->hops);
        jw_str(w, "}\n");
        row_finish(&d, i + 1 == n);
    }
    dump_end(&d);
}

static void dump_stats(lmesh_node *node, jw *w)
{
    lmesh_stats s;
    lmesh_get_stats(node, &s);
    dump d;
    dump_init(&d, w, "stats");
    row_begin(&d);
    jw_str(w, "{\"tx_frames\":");
    jw_array(w, s.tx_frames, 16);
    jw_key(w, "rx_frames");
    jw_array(w, s.rx_frames, 16);
    jw_ku(w, "rx_bad_tag", s.rx_bad_tag);
    jw_ku(w, "rx_foreign", s.rx_foreign);
    jw_ku(w, "rx_malformed", s.rx_malformed);
    jw_ku(w, "ack_timeouts", s.ack_timeouts);
    jw_ku(w, "link_failures", s.link_failures);
    jw_ku(w, "dup_hop", s.dup_hop);
    jw_ku(w, "dup_relay", s.dup_relay);
    jw_ku(w, "dup_dest", s.dup_dest);
    jw_ku(w, "dedup_outside", s.dedup_outside);
    jw_ku(w, "evicted", s.evicted);
    jw_ku(w, "expired", s.expired);
    jw_ku(w, "delivered", s.delivered);
    jw_ku(w, "hop_limit_drops", s.hop_limit_drops);
    jw_ku(w, "record_crc_errors", s.record_crc_errors);
    jw_ku(w, "airtime_ms", s.airtime_ms);
    jw_str(w, "}\n");
    row_finish(&d, true);
    dump_end(&d);
}

static const char *run_command(lmesh_node *node, jw *w, cursor *c)
{
    token cmd;
    if (!next_token(c, &cmd)) {
        return "empty";
    }
    if (tok_is(&cmd, "send")) {
        return cmd_send(node, w, c, LMESH_MSG_TEXT);
    }
    if (tok_is(&cmd, "cmd")) {
        return cmd_send(node, w, c, LMESH_MSG_COMMAND);
    }
    if (tok_is(&cmd, "tel")) {
        return cmd_send(node, w, c, LMESH_MSG_TELEMETRY);
    }
    if (tok_is(&cmd, "cancel")) {
        return cmd_cancel(node, w, c);
    }

    void (*simple)(lmesh_node *, jw *) = NULL;
    if (tok_is(&cmd, "id")) {
        simple = cmd_id;
    } else if (tok_is(&cmd, "nodes")) {
        simple = dump_nodes;
    } else if (tok_is(&cmd, "links")) {
        simple = dump_links;
    } else if (tok_is(&cmd, "routes")) {
        simple = dump_routes;
    } else if (tok_is(&cmd, "msgs")) {
        simple = dump_msgs;
    } else if (tok_is(&cmd, "stats")) {
        simple = dump_stats;
    } else if (tok_is(&cmd, "status")) {
        simple = cmd_status;
    } else {
        return "unknown_command";
    }
    if (!at_end(c)) {
        return "unexpected_argument";
    }
    simple(node, w);
    return NULL;
}

size_t lmesh_host_command(lmesh_node *node, const char *line, char *out, size_t out_max)
{
    jw w;
    jw_init(&w, out, out_max);
    if (!node || !line) {
        reply_fail(&w, "invalid_argument");
        return w.len;
    }
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) {
        n--;
    }
    cursor c = {line, line + n};
    const char *err = run_command(node, &w, &c);
    if (err) {
        reply_fail(&w, err);
    }
    return w.len;
}
