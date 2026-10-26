/* Unit tests for host: command parser, table dumps, event formatting, escaping, truncation. */
#include <stdio.h>
#include <string.h>

#include "lmesh/lmesh.h"

static int failures;
static int checks;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(cond)) {                                                                        \
            failures++;                                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
        }                                                                                     \
    } while (0)

static uint32_t clock_ms;

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return clock_ms;
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    return 4;
}

static int radio_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    (void)frame;
    (void)len;
    return 0;
}

static uint32_t airtime_ms(void *ctx, size_t len)
{
    (void)ctx;
    return 15u + (uint32_t)len * 2u;
}

static lmesh_node node;
static char out[8192];

static void fresh(void)
{
    lmesh_identity ident = {.id = 7, .network_id = 1, .station_id_len = 6, .station_id = "KD2ABC"};
    lmesh_port port = {.now_ms = now_ms, .random = rnd, .radio_send = radio_send, .airtime_ms = airtime_ms};
    lmesh_callbacks cb = {0};
    clock_ms = 1000;
    if (lmesh_init(&node, &ident, &port, &cb) != LMESH_OK) {
        failures++;
        printf("FAIL init\n");
    }
}

static void check_text(const char *got, size_t n, const char *want, int line)
{
    checks++;
    if (n != strlen(got) || strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s:%d\n  want: %s\n  got:  %s\n  len %lu\n", __FILE__, line, want, got, (unsigned long)n);
    }
}

static void expect_cmd(const char *line, const char *want, int lineno)
{
    size_t n = lmesh_host_command(&node, line, out, sizeof out);
    check_text(out, n, want, lineno);
}

static void expect_ev(const lmesh_event *ev, const char *want, int lineno)
{
    size_t n = lmesh_host_format_event(ev, out, sizeof out);
    check_text(out, n, want, lineno);
}

/* Accepts either outcome while the message and status modules are stubs. */
static void expect_cmd_either(const char *line, const char *prefix_ok, const char *alt, int lineno)
{
    size_t n = lmesh_host_command(&node, line, out, sizeof out);
    checks++;
    bool good = n == strlen(out) && (strncmp(out, prefix_ok, strlen(prefix_ok)) == 0 || strcmp(out, alt) == 0);
    if (!good) {
        failures++;
        printf("FAIL %s:%d\n  want: %s... or %s\n  got:  %s\n", __FILE__, lineno, prefix_ok, alt, out);
    }
}

#define EXPECT_CMD(line, want) expect_cmd((line), (want), __LINE__)
#define EXPECT_EV(ev, want) expect_ev((ev), (want), __LINE__)
#define FAILS(reason) "{\"ok\":false,\"reason\":\"" reason "\"}\n"
#define UNIMPL FAILS("unimplemented")
#define EXPECT_SUBMIT(line) expect_cmd_either((line), "{\"ok\":true,\"origin\":7,\"id\":", UNIMPL, __LINE__)

static bool tail_untouched(const char *buf, size_t from, size_t size)
{
    for (size_t i = from; i < size; i++) {
        if (buf[i] != 'Z') {
            return false;
        }
    }
    return true;
}

static void test_basic_commands(void)
{
    fresh();
    EXPECT_CMD("id", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"KD2ABC\"}\n");
    EXPECT_CMD("id\r\n", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"KD2ABC\"}\n");
    EXPECT_CMD("id\n", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"KD2ABC\"}\n");
    EXPECT_CMD("  id  ", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"KD2ABC\"}\n");
    EXPECT_CMD("id x", FAILS("unexpected_argument"));
    EXPECT_CMD("", FAILS("empty"));
    EXPECT_CMD("\r\n", FAILS("empty"));
    EXPECT_CMD("   ", FAILS("empty"));
    EXPECT_CMD("bogus", FAILS("unknown_command"));
    EXPECT_CMD("ID", FAILS("unknown_command"));
    EXPECT_CMD("ids", FAILS("unknown_command"));
    EXPECT_CMD("\tid", FAILS("unknown_command"));

    node.ident.station_id_len = 0;
    EXPECT_CMD("id", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"\"}\n");
    memcpy(node.ident.station_id, "a\"b\\c\x01\xe9\x7f", 8);
    node.ident.station_id_len = 8;
    EXPECT_CMD("id", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"a\\\"b\\\\c\\u0001\\u00e9\x7f\"}\n");
    node.ident.station_id_len = 200;
    EXPECT_CMD("id", "{\"ok\":true,\"id\":7,\"net\":1,\"station\":\"a\\\"b\\\\c\\u0001\\u00e9\x7f\"}\n");

    CHECK(lmesh_host_command(NULL, "id", out, sizeof out) == strlen(FAILS("invalid_argument")));
    CHECK(strcmp(out, FAILS("invalid_argument")) == 0);
    CHECK(lmesh_host_command(&node, NULL, out, sizeof out) == strlen(FAILS("invalid_argument")));

    expect_cmd_either("status", "{\"ok\":true}\n", UNIMPL, __LINE__);
    expect_cmd_either("status\r\n", "{\"ok\":true}\n", UNIMPL, __LINE__);
    EXPECT_CMD("status now", FAILS("unexpected_argument"));
}

static void test_send(void)
{
    fresh();
    EXPECT_SUBMIT("send 3 n 600 hello world");
    EXPECT_SUBMIT("send 3 u 1 x");
    EXPECT_SUBMIT("send 3 r 4294967295 routine text");
    EXPECT_SUBMIT("send 3 n 600 hi\r\n");
    EXPECT_SUBMIT("send   3   n   600   spaced  out  ");
    EXPECT_SUBMIT("send 254 n 600 hi");
    EXPECT_SUBMIT("send 003 n 0600 hi");

    EXPECT_CMD("send", FAILS("missing_argument"));
    EXPECT_CMD("send 3", FAILS("missing_argument"));
    EXPECT_CMD("send 3 n", FAILS("missing_argument"));
    EXPECT_CMD("send 3 n 600", FAILS("missing_argument"));
    EXPECT_CMD("send 3 n 600 ", FAILS("missing_argument"));
    EXPECT_CMD("send 3 n 600\r\n", FAILS("missing_argument"));

    EXPECT_CMD("send x n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send 0 n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send 255 n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send 300 n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send -1 n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send +3 n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send 3x n 600 hi", FAILS("bad_dest"));
    EXPECT_CMD("send 99999999999 n 600 hi", FAILS("bad_dest"));

    EXPECT_CMD("send 3 x 600 hi", FAILS("bad_prio"));
    EXPECT_CMD("send 3 U 600 hi", FAILS("bad_prio"));
    EXPECT_CMD("send 3 un 600 hi", FAILS("bad_prio"));
    EXPECT_CMD("send 3 urgent 600 hi", FAILS("bad_prio"));
    EXPECT_CMD("send 3 600 hi", FAILS("bad_prio"));

    EXPECT_CMD("send 3 n 6o0 hi", FAILS("bad_lifetime"));
    EXPECT_CMD("send 3 n 0 hi", FAILS("bad_lifetime"));
    EXPECT_CMD("send 3 n -5 hi", FAILS("bad_lifetime"));
    EXPECT_CMD("send 3 n 4294967296 hi", FAILS("bad_lifetime"));
    EXPECT_CMD("send 3 n 1.5 hi", FAILS("bad_lifetime"));

    EXPECT_CMD("send 7 n 600 to myself", FAILS("invalid_argument"));

    char line[256];
    int head = snprintf(line, sizeof line, "send 3 n 600 ");
    memset(line + head, 'x', LMESH_MAX_CONTENT + 1);
    line[(size_t)head + LMESH_MAX_CONTENT + 1] = '\0';
    EXPECT_CMD(line, FAILS("too_long"));
    line[(size_t)head + LMESH_MAX_CONTENT] = '\0';
    EXPECT_SUBMIT(line);
}

static void test_cmd_tel(void)
{
    fresh();
    EXPECT_SUBMIT("cmd 3 120 reboot now");
    EXPECT_SUBMIT("cmd 3 120 reboot\r\n");
    EXPECT_CMD("cmd", FAILS("missing_argument"));
    EXPECT_CMD("cmd 3", FAILS("missing_argument"));
    EXPECT_CMD("cmd 3 120", FAILS("missing_argument"));
    EXPECT_CMD("cmd 3 abc reboot", FAILS("bad_lifetime"));
    EXPECT_CMD("cmd 3 n 120 reboot", FAILS("bad_lifetime"));
    EXPECT_CMD("cmd 0 120 reboot", FAILS("bad_dest"));
    EXPECT_CMD("cmd 7 120 reboot", FAILS("invalid_argument"));

    EXPECT_SUBMIT("tel 3 600 0a0B");
    EXPECT_SUBMIT("tel 3 600 deadBEEF\r\n");
    EXPECT_SUBMIT("tel 3 600 00  ");
    EXPECT_CMD("tel", FAILS("missing_argument"));
    EXPECT_CMD("tel 3", FAILS("missing_argument"));
    EXPECT_CMD("tel 3 600", FAILS("missing_argument"));
    EXPECT_CMD("tel 3 600 \r\n", FAILS("missing_argument"));
    EXPECT_CMD("tel 3 600 abc", FAILS("bad_hex"));
    EXPECT_CMD("tel 3 600 a", FAILS("bad_hex"));
    EXPECT_CMD("tel 3 600 zz", FAILS("bad_hex"));
    EXPECT_CMD("tel 3 600 0x12", FAILS("bad_hex"));
    EXPECT_CMD("tel 3 600 0a 0b", FAILS("unexpected_argument"));
    EXPECT_CMD("tel 3 x 0a", FAILS("bad_lifetime"));
    EXPECT_CMD("tel 256 600 0a", FAILS("bad_dest"));

    char line[512];
    int head = snprintf(line, sizeof line, "tel 3 600 ");
    memset(line + head, 'a', 2 * (LMESH_MAX_CONTENT + 1));
    line[(size_t)head + 2 * (LMESH_MAX_CONTENT + 1)] = '\0';
    EXPECT_CMD(line, FAILS("too_long"));
    line[(size_t)head + 2 * LMESH_MAX_CONTENT] = '\0';
    EXPECT_SUBMIT(line);
}

static void test_cancel(void)
{
    fresh();
    expect_cmd_either("cancel 5", "{\"ok\":false,\"reason\":\"not_found\"}\n", UNIMPL, __LINE__);
    expect_cmd_either("cancel 65535\r\n", "{\"ok\":false,\"reason\":\"not_found\"}\n", UNIMPL, __LINE__);
    EXPECT_CMD("cancel", FAILS("missing_argument"));
    EXPECT_CMD("cancel x", FAILS("bad_id"));
    EXPECT_CMD("cancel -1", FAILS("bad_id"));
    EXPECT_CMD("cancel 65536", FAILS("bad_id"));
    EXPECT_CMD("cancel 1 2", FAILS("unexpected_argument"));
}

static void fill_links(void)
{
    lmesh_neighbor *a = &node.nbr.table[0];
    a->used = true;
    a->up = true;
    a->bidirectional = true;
    a->id = 2;
    a->rx_quality = 200;
    a->tx_quality = 180;
    a->etx8 = 10;
    a->rssi = -91;
    a->snr = -7;
    a->last_heard_ms = 12345;
    lmesh_neighbor *b = &node.nbr.table[2];
    b->used = true;
    b->id = 5;
    b->etx8 = 255;
    b->rssi = -120;
    b->snr = 12;
    b->last_heard_ms = 400;
}

#define LINK_ROW1 "{\"id\":2,\"up\":true,\"bidir\":true,\"etx\":1.25,\"rxq\":200,\"txq\":180,\"rssi\":-91,\"snr\":-7,\"heard_ms\":12345}\n"
#define LINK_ROW2 "{\"id\":5,\"up\":false,\"bidir\":false,\"etx\":\"inf\",\"rxq\":0,\"txq\":0,\"rssi\":-120,\"snr\":12,\"heard_ms\":400}\n"
#define LINK_END "{\"end\":\"links\"}\n"
#define LINK_END_T "{\"end\":\"links\",\"truncated\":true}\n"

static void test_dumps(void)
{
    fresh();
    EXPECT_CMD("links", LINK_END);
    EXPECT_CMD("routes", "{\"end\":\"routes\"}\n");
    EXPECT_CMD("nodes", "{\"end\":\"nodes\"}\n");
    EXPECT_CMD("msgs", "{\"end\":\"msgs\"}\n");
    EXPECT_CMD("links extra", FAILS("unexpected_argument"));
    EXPECT_CMD("stats 1", FAILS("unexpected_argument"));

    fill_links();
    EXPECT_CMD("links", LINK_ROW1 LINK_ROW2 LINK_END);
    EXPECT_CMD("links\r\n", LINK_ROW1 LINK_ROW2 LINK_END);

    lmesh_route *r = &node.route.table[0];
    r->used = true;
    r->dest = 4;
    r->next_hop = 2;
    r->metric8 = 20;
    r->seqno = 17;
    r = &node.route.table[3];
    r->used = true;
    r->dest = 5;
    r->next_hop = 0;
    r->metric8 = 255;
    r->seqno = 65535;
    EXPECT_CMD("routes", "{\"dest\":4,\"next\":2,\"metric\":2.50,\"seq\":17,\"retracted\":false}\n"
                         "{\"dest\":5,\"next\":0,\"metric\":\"inf\",\"seq\":65535,\"retracted\":true}\n"
                         "{\"end\":\"routes\"}\n");

    lmesh_node_entry *e = &node.status.table[0];
    e->used = true;
    e->id = 3;
    e->battery_mv = 3700;
    e->flags = 0x05;
    e->uptime_s = 86400;
    e->store_depth = 2;
    e->lost = 1;
    e->station_id_len = 3;
    memcpy(e->station_id, "N0X", 3);
    e->neighbor_count = 2;
    e->neighbors[0] = 7;
    e->neighbor_etx8[0] = 8;
    e->neighbors[1] = 4;
    e->neighbor_etx8[1] = 255;
    e->last_heard_ms = 100000;
    e = &node.status.table[5];
    e->used = true;
    e->id = 4;
    e->last_heard_ms = 0;
    node.now_ms = 200000;
    EXPECT_CMD("nodes", "{\"node\":3,\"batt_mv\":3700,\"flags\":5,\"uptime_s\":86400,\"store\":2,\"lost\":1,"
                        "\"station\":\"N0X\",\"stale\":false,\"heard_ms\":100000,\"neighbors\":[[7,1.00],[4,\"inf\"]]}\n"
                        "{\"node\":4,\"batt_mv\":0,\"flags\":0,\"uptime_s\":0,\"store\":0,\"lost\":0,"
                        "\"station\":\"\",\"stale\":true,\"heard_ms\":0,\"neighbors\":[]}\n"
                        "{\"end\":\"nodes\"}\n");

    lmesh_record *m = &node.store.records[0];
    m->role = LMESH_ROLE_ORIGIN_COPY;
    m->origin = 7;
    m->dest = 3;
    m->id = 12;
    m->type_prio = (uint8_t)(LMESH_MSG_TEXT | (LMESH_PRIO_NORMAL << 4));
    m->state = LMESH_STATE_QUEUED;
    m->acquired_ms = node.now_ms;
    m->lifetime_s = 600;
    m = &node.store.records[5];
    m->role = LMESH_ROLE_RELAY;
    m->origin = 2;
    m->dest = 9;
    m->id = 65535;
    m->type_prio = (uint8_t)(LMESH_MSG_COMMAND | (LMESH_PRIO_URGENT << 4));
    m->state = LMESH_STATE_QUEUED;
    m->acquired_ms = node.now_ms;
    m->lifetime_s = 42;
    m->attempts = 2;
    m->hops = 3;
    m = &node.store.records[6];
    m->role = LMESH_ROLE_HISTORY;
    m->origin = 7;
    m->dest = 4;
    m->id = 3;
    m->type_prio = (uint8_t)(LMESH_MSG_TELEMETRY | (LMESH_PRIO_ROUTINE << 4));
    m->state = LMESH_STATE_DELIVERED;
    m->attempts = 1;
    m = &node.store.records[7];
    m->role = LMESH_ROLE_ORIGIN_SHADOW;
    m->origin = 7;
    m->dest = 4;
    m->id = 4;
    m->type_prio = (uint8_t)(LMESH_MSG_RECEIPT | (3u << 4));
    m->state = LMESH_STATE_TRANSMITTED;
    m->acquired_ms = node.now_ms;
    m->lifetime_s = 4294967295u;
    EXPECT_CMD("msgs", "{\"origin\":7,\"id\":12,\"dest\":3,\"type\":\"text\",\"prio\":\"normal\",\"state\":\"queued\","
                       "\"role\":\"origin_copy\",\"lifetime_s\":600,\"attempts\":0,\"hops\":0}\n"
                       "{\"origin\":2,\"id\":65535,\"dest\":9,\"type\":\"command\",\"prio\":\"urgent\",\"state\":\"queued\","
                       "\"role\":\"relay\",\"lifetime_s\":42,\"attempts\":2,\"hops\":3}\n"
                       "{\"origin\":7,\"id\":3,\"dest\":4,\"type\":\"telemetry\",\"prio\":\"routine\",\"state\":\"delivered\","
                       "\"role\":\"history\",\"lifetime_s\":0,\"attempts\":1,\"hops\":0}\n"
                       "{\"origin\":7,\"id\":4,\"dest\":4,\"type\":\"receipt\",\"prio\":\"unknown\",\"state\":\"transmitted\","
                       "\"role\":\"origin_shadow\",\"lifetime_s\":4294967295,\"attempts\":0,\"hops\":0}\n"
                       "{\"end\":\"msgs\"}\n");

    EXPECT_CMD("stats", "{\"tx_frames\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],\"rx_frames\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],"
                        "\"rx_bad_tag\":0,\"rx_foreign\":0,\"rx_malformed\":0,\"ack_timeouts\":0,\"link_failures\":0,"
                        "\"dup_hop\":0,\"dup_relay\":0,\"dup_dest\":0,\"dedup_outside\":0,\"evicted\":0,\"expired\":0,\"delivered\":0,"
                        "\"hop_limit_drops\":0,\"record_crc_errors\":0,\"airtime_ms\":0}\n{\"end\":\"stats\"}\n");
    node.stats.tx_frames[0] = 5;
    node.stats.tx_frames[15] = 1;
    node.stats.rx_frames[2] = 4294967295u;
    node.stats.rx_bad_tag = 1;
    node.stats.rx_foreign = 2;
    node.stats.rx_malformed = 3;
    node.stats.ack_timeouts = 4;
    node.stats.link_failures = 5;
    node.stats.dup_hop = 6;
    node.stats.dup_relay = 7;
    node.stats.dup_dest = 8;
    node.stats.dedup_outside = 14;
    node.stats.evicted = 9;
    node.stats.expired = 10;
    node.stats.delivered = 11;
    node.stats.hop_limit_drops = 12;
    node.stats.record_crc_errors = 13;
    node.stats.airtime_ms = 123456;
    EXPECT_CMD("stats", "{\"tx_frames\":[5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1],\"rx_frames\":[0,0,4294967295,0,0,0,0,0,0,0,0,0,0,0,0,0],"
                        "\"rx_bad_tag\":1,\"rx_foreign\":2,\"rx_malformed\":3,\"ack_timeouts\":4,\"link_failures\":5,"
                        "\"dup_hop\":6,\"dup_relay\":7,\"dup_dest\":8,\"dedup_outside\":14,\"evicted\":9,\"expired\":10,\"delivered\":11,"
                        "\"hop_limit_drops\":12,\"record_crc_errors\":13,\"airtime_ms\":123456}\n{\"end\":\"stats\"}\n");
}

static void expect_small(const char *line, size_t cap, const char *want, int lineno)
{
    char buf[1024];
    memset(buf, 'Z', sizeof buf);
    size_t n = lmesh_host_command(&node, line, buf, cap);
    if (cap > 0) {
        check_text(buf, n, want, lineno);
    } else {
        checks++;
        if (n != 0 || buf[0] != 'Z') {
            failures++;
            printf("FAIL %s:%d: zero-size buffer written\n", __FILE__, lineno);
        }
    }
    checks++;
    if (!tail_untouched(buf, cap, sizeof buf)) {
        failures++;
        printf("FAIL %s:%d: wrote past %lu\n", __FILE__, lineno, (unsigned long)cap);
    }
}

#define EXPECT_SMALL(line, cap, want) expect_small((line), (cap), (want), __LINE__)

static void test_dump_truncation(void)
{
    fresh();
    fill_links();
    size_t l1 = strlen(LINK_ROW1);
    size_t l2 = strlen(LINK_ROW2);
    size_t e = strlen(LINK_END);
    size_t et = strlen(LINK_END_T);

    EXPECT_SMALL("links", l1 + l2 + e + 1, LINK_ROW1 LINK_ROW2 LINK_END);
    EXPECT_SMALL("links", l1 + l2 + e, LINK_ROW1 LINK_END_T);
    EXPECT_SMALL("links", l1 + et + 1, LINK_ROW1 LINK_END_T);
    EXPECT_SMALL("links", l1 + et, LINK_END_T);
    EXPECT_SMALL("links", et + 1, LINK_END_T);
    EXPECT_SMALL("links", et, "{\"end\":\"links\",\"truncated\":true}");
    EXPECT_SMALL("links", 5, "{\"en");
    EXPECT_SMALL("links", 1, "");
    EXPECT_SMALL("links", 0, "");

    EXPECT_SMALL("stats", 100, "{\"end\":\"stats\",\"truncated\":true}\n");
    EXPECT_SMALL("msgs", 1, "");
    EXPECT_SMALL("nodes", 0, "");
    EXPECT_SMALL("routes", 0, "");

    EXPECT_SMALL("id", 10, "{\"ok\":tru");
    EXPECT_SMALL("id", 1, "");
    EXPECT_SMALL("id", 0, "");
    EXPECT_SMALL("bogus", 1, "");
    EXPECT_SMALL("bogus", 0, "");
    EXPECT_SMALL("send 3 n 60 hi", 1, "");
    EXPECT_SMALL("send 3 n 60 hi", 0, "");

    CHECK(lmesh_host_command(&node, "id", NULL, 0) == 0);
    CHECK(lmesh_host_command(&node, "links", NULL, 100) == 0);
}

static void test_metric_format(void)
{
    static const struct {
        uint8_t v;
        const char *text;
    } cases[] = {
        {8, "1.00"}, {10, "1.25"}, {12, "1.50"}, {254, "31.75"}, {255, "\"inf\""},
        {0, "0.00"}, {9, "1.13"},  {15, "1.88"}, {16, "2.00"},   {127, "15.88"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        lmesh_event ev = {.type = LMESH_EV_ROUTE_CHANGED, .t_ms = 1};
        ev.u.route.dest = 4;
        ev.u.route.next_hop = 2;
        ev.u.route.metric8 = cases[i].v;
        ev.u.route.seqno = 9;
        char want[128];
        snprintf(want, sizeof want, "{\"ev\":\"route\",\"t\":1,\"dest\":4,\"next\":2,\"metric\":%s,\"seq\":9}\n",
                 cases[i].text);
        EXPECT_EV(&ev, want);
    }
}

static void test_events(void)
{
    lmesh_event ev;

    ev = (lmesh_event){.type = LMESH_EV_BOOT, .t_ms = 1000};
    EXPECT_EV(&ev, "{\"ev\":\"boot\",\"t\":1000}\n");
    ev.t_ms = 4294967295u;
    EXPECT_EV(&ev, "{\"ev\":\"boot\",\"t\":4294967295}\n");

    ev = (lmesh_event){.type = LMESH_EV_NEIGHBOR_UP, .t_ms = 2000, .u.neighbor = {.id = 2, .etx8 = 8}};
    EXPECT_EV(&ev, "{\"ev\":\"neighbor\",\"t\":2000,\"id\":2,\"state\":\"up\",\"etx\":1.00}\n");
    ev = (lmesh_event){.type = LMESH_EV_NEIGHBOR_DOWN, .t_ms = 2100, .u.neighbor = {.id = 2, .etx8 = 255}};
    EXPECT_EV(&ev, "{\"ev\":\"neighbor\",\"t\":2100,\"id\":2,\"state\":\"down\",\"etx\":\"inf\"}\n");

    ev = (lmesh_event){.type = LMESH_EV_LINK_QUALITY, .t_ms = 3000,
                       .u.neighbor = {.id = 3, .etx8 = 10, .rssi = -97, .snr = -12}};
    EXPECT_EV(&ev, "{\"ev\":\"link\",\"t\":3000,\"id\":3,\"etx\":1.25,\"rssi\":-97,\"snr\":-12}\n");
    ev = (lmesh_event){.type = LMESH_EV_LINK_QUALITY, .t_ms = 3001,
                       .u.neighbor = {.id = 254, .etx8 = 12, .rssi = -32768, .snr = 127}};
    EXPECT_EV(&ev, "{\"ev\":\"link\",\"t\":3001,\"id\":254,\"etx\":1.50,\"rssi\":-32768,\"snr\":127}\n");
    ev.u.neighbor.rssi = 0;
    ev.u.neighbor.snr = -128;
    EXPECT_EV(&ev, "{\"ev\":\"link\",\"t\":3001,\"id\":254,\"etx\":1.50,\"rssi\":0,\"snr\":-128}\n");

    ev = (lmesh_event){.type = LMESH_EV_ROUTE_CHANGED, .t_ms = 4000,
                       .u.route = {.dest = 4, .next_hop = 2, .metric8 = 20, .seqno = 300}};
    EXPECT_EV(&ev, "{\"ev\":\"route\",\"t\":4000,\"dest\":4,\"next\":2,\"metric\":2.50,\"seq\":300}\n");
    ev = (lmesh_event){.type = LMESH_EV_ROUTE_LOST, .t_ms = 4100,
                       .u.route = {.dest = 4, .next_hop = 2, .metric8 = 255, .seqno = 301}};
    EXPECT_EV(&ev, "{\"ev\":\"route\",\"t\":4100,\"dest\":4,\"next\":0,\"metric\":\"inf\",\"seq\":301}\n");

    ev = (lmesh_event){.type = LMESH_EV_MSG_STATE, .t_ms = 5000};
    ev.u.msg_state.ref.origin = 7;
    ev.u.msg_state.ref.id = 12;
    ev.u.msg_state.dest = 3;
    ev.u.msg_state.msg_type = LMESH_MSG_COMMAND;
    ev.u.msg_state.prio = LMESH_PRIO_URGENT;
    ev.u.msg_state.state = LMESH_STATE_DELIVERED;
    ev.u.msg_state.reason = LMESH_REASON_RECEIPT;
    ev.u.msg_state.result_code = LMESH_RESULT_EXECUTED;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":12,\"dest\":3,\"state\":\"delivered\","
                   "\"reason\":\"receipt\",\"type\":\"command\",\"prio\":\"urgent\",\"result\":\"executed\"}\n");
    ev.u.msg_state.msg_type = LMESH_MSG_TEXT;
    ev.u.msg_state.prio = LMESH_PRIO_NORMAL;
    ev.u.msg_state.result_code = LMESH_RESULT_DELIVERED;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":12,\"dest\":3,\"state\":\"delivered\","
                   "\"reason\":\"receipt\",\"type\":\"text\",\"prio\":\"normal\",\"result\":\"delivered\"}\n");
    ev.u.msg_state.result_code = 9;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":12,\"dest\":3,\"state\":\"delivered\","
                   "\"reason\":\"receipt\",\"type\":\"text\",\"prio\":\"normal\",\"result\":\"unknown\"}\n");

    ev.u.msg_state.ref.id = 65535;
    ev.u.msg_state.msg_type = LMESH_MSG_TEXT;
    ev.u.msg_state.prio = LMESH_PRIO_ROUTINE;
    ev.u.msg_state.state = LMESH_STATE_FAILED;
    ev.u.msg_state.reason = LMESH_REASON_EVICTED;
    ev.u.msg_state.result_code = LMESH_RESULT_REJECTED;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":65535,\"dest\":3,\"state\":\"failed\","
                   "\"reason\":\"evicted\",\"type\":\"text\",\"prio\":\"routine\"}\n");
    ev.u.msg_state.msg_type = LMESH_MSG_TELEMETRY;
    ev.u.msg_state.state = LMESH_STATE_QUEUED;
    ev.u.msg_state.reason = LMESH_REASON_RESEND;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":65535,\"dest\":3,\"state\":\"queued\","
                   "\"reason\":\"resend\",\"type\":\"telemetry\",\"prio\":\"routine\"}\n");
    ev.u.msg_state.msg_type = LMESH_MSG_RECEIPT;
    ev.u.msg_state.state = LMESH_STATE_TRANSMITTED;
    ev.u.msg_state.reason = LMESH_REASON_HOP_ACCEPTED;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":65535,\"dest\":3,\"state\":\"transmitted\","
                   "\"reason\":\"hop_accepted\",\"type\":\"receipt\",\"prio\":\"routine\"}\n");
    ev.u.msg_state.state = LMESH_STATE_EXPIRED;
    ev.u.msg_state.reason = LMESH_REASON_LIFETIME;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":65535,\"dest\":3,\"state\":\"expired\","
                   "\"reason\":\"lifetime\",\"type\":\"receipt\",\"prio\":\"routine\"}\n");
    ev.u.msg_state.state = (lmesh_msg_state)9;
    ev.u.msg_state.reason = 99;
    ev.u.msg_state.msg_type = (lmesh_msg_type)7;
    ev.u.msg_state.prio = (lmesh_prio)5;
    EXPECT_EV(&ev, "{\"ev\":\"msg\",\"t\":5000,\"origin\":7,\"id\":65535,\"dest\":3,\"state\":\"unknown\","
                   "\"reason\":\"unknown\",\"type\":\"unknown\",\"prio\":\"unknown\"}\n");

    static const char *const reasons[] = {"none",    "hop_accepted", "receipt",  "lifetime", "evicted",
                                          "invalid", "rejected",     "canceled", "resend"};
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
        lmesh_event m = {.type = LMESH_EV_MSG_STATE, .t_ms = 1};
        m.u.msg_state.ref.origin = 7;
        m.u.msg_state.ref.id = 1;
        m.u.msg_state.dest = 2;
        m.u.msg_state.state = LMESH_STATE_FAILED;
        m.u.msg_state.reason = (uint8_t)i;
        char want[256];
        snprintf(want, sizeof want,
                 "{\"ev\":\"msg\",\"t\":1,\"origin\":7,\"id\":1,\"dest\":2,\"state\":\"failed\",\"reason\":\"%s\","
                 "\"type\":\"text\",\"prio\":\"urgent\"}\n",
                 reasons[i]);
        EXPECT_EV(&m, want);
    }

    ev = (lmesh_event){.type = LMESH_EV_STATUS_RECEIVED, .t_ms = 8000, .u.status = {.id = 5}};
    EXPECT_EV(&ev, "{\"ev\":\"status\",\"t\":8000,\"node\":5}\n");

    ev = (lmesh_event){.type = LMESH_EV_STORE, .t_ms = 9000, .u.store = {.depth = 12, .evicted = 3, .expired = 65535}};
    EXPECT_EV(&ev, "{\"ev\":\"store\",\"t\":9000,\"depth\":12,\"evicted\":3,\"expired\":65535}\n");

    ev = (lmesh_event){.type = (lmesh_event_type)42, .t_ms = 1};
    EXPECT_EV(&ev, "{\"ev\":\"unknown\",\"t\":1,\"type\":42}\n");

    CHECK(lmesh_host_format_event(NULL, out, sizeof out) == 0);
    CHECK(out[0] == '\0');
}

static void rx_event(lmesh_event *ev, lmesh_msg_type type, const void *data, size_t len)
{
    *ev = (lmesh_event){.type = LMESH_EV_MSG_RECEIVED, .t_ms = 7000};
    ev->u.msg_received.ref.origin = 3;
    ev->u.msg_received.ref.id = 9;
    ev->u.msg_received.msg_type = type;
    ev->u.msg_received.prio = LMESH_PRIO_URGENT;
    ev->u.msg_received.hops = 2;
    ev->u.msg_received.len = (uint8_t)len;
    ev->u.msg_received.data = (const uint8_t *)data;
}

#define RX_HEAD(type) "{\"ev\":\"rx\",\"t\":7000,\"origin\":3,\"id\":9,\"type\":\"" type "\",\"prio\":\"urgent\",\"hops\":2,"

static void test_rx_events(void)
{
    lmesh_event ev;

    rx_event(&ev, LMESH_MSG_TEXT, "hi", 2);
    EXPECT_EV(&ev, RX_HEAD("text") "\"text\":\"hi\"}\n");

    const char quoted[] = "say \"hi\" C:\\dir\tend";
    rx_event(&ev, LMESH_MSG_TEXT, quoted, strlen(quoted));
    EXPECT_EV(&ev, RX_HEAD("text") "\"text\":\"say \\\"hi\\\" C:\\\\dir\\u0009end\"}\n");

    const char utf8[] = "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8c\x95 /\x7f";
    rx_event(&ev, LMESH_MSG_TEXT, utf8, strlen(utf8));
    EXPECT_EV(&ev, RX_HEAD("text") "\"text\":\"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8c\x95 /\x7f\"}\n");

    rx_event(&ev, LMESH_MSG_TEXT, "a\nb", 3);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"610a62\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "a\0b", 3);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"610062\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\x01", 1);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"01\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\xff", 1);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"ff\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\xc0\xaf", 2);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"c0af\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\xe2\x82", 2);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"e282\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\xed\xa0\x80", 3);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"eda080\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\xf4\x90\x80\x80", 4);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"f4908080\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\x80", 1);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"80\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, "\xc3(", 2);
    EXPECT_EV(&ev, RX_HEAD("text") "\"hex\":\"c328\"}\n");

    rx_event(&ev, LMESH_MSG_TEXT, "", 0);
    EXPECT_EV(&ev, RX_HEAD("text") "\"text\":\"\"}\n");
    rx_event(&ev, LMESH_MSG_TEXT, NULL, 5);
    EXPECT_EV(&ev, RX_HEAD("text") "\"text\":\"\"}\n");

    rx_event(&ev, LMESH_MSG_COMMAND, "reboot", 6);
    EXPECT_EV(&ev, RX_HEAD("command") "\"hex\":\"7265626f6f74\"}\n");
    const uint8_t tel[] = {0x0a, 0xff, 0x00, 0xB7};
    rx_event(&ev, LMESH_MSG_TELEMETRY, tel, sizeof tel);
    EXPECT_EV(&ev, RX_HEAD("telemetry") "\"hex\":\"0aff00b7\"}\n");
    rx_event(&ev, LMESH_MSG_TELEMETRY, tel, 0);
    EXPECT_EV(&ev, RX_HEAD("telemetry") "\"hex\":\"\"}\n");
    rx_event(&ev, LMESH_MSG_RECEIPT, tel, 2);
    EXPECT_EV(&ev, RX_HEAD("receipt") "\"hex\":\"0aff\"}\n");

    uint8_t big[255];
    for (size_t i = 0; i < sizeof big; i++) {
        big[i] = (uint8_t)i;
    }
    rx_event(&ev, LMESH_MSG_TELEMETRY, big, sizeof big);
    size_t n = lmesh_host_format_event(&ev, out, sizeof out);
    CHECK(n == strlen(RX_HEAD("telemetry") "\"hex\":\"\"}\n") + 2 * sizeof big);
    CHECK(strstr(out, "\"hex\":\"000102") != NULL);
    CHECK(strstr(out, "fcfdfe\"}\n") != NULL);
}

static void test_event_truncation(void)
{
    lmesh_event ev = {.type = LMESH_EV_STORE, .t_ms = 9000, .u.store = {.depth = 12, .evicted = 3, .expired = 4}};
    const char *full = "{\"ev\":\"store\",\"t\":9000,\"depth\":12,\"evicted\":3,\"expired\":4}\n";
    size_t len = strlen(full);
    char buf[128];

    memset(buf, 'Z', sizeof buf);
    CHECK(lmesh_host_format_event(&ev, buf, len + 1) == len);
    CHECK(strcmp(buf, full) == 0);
    CHECK(tail_untouched(buf, len + 1, sizeof buf));

    memset(buf, 'Z', sizeof buf);
    CHECK(lmesh_host_format_event(&ev, buf, len) == len - 1);
    CHECK(strncmp(buf, full, len - 1) == 0 && buf[len - 1] == '\0');
    CHECK(tail_untouched(buf, len, sizeof buf));

    memset(buf, 'Z', sizeof buf);
    CHECK(lmesh_host_format_event(&ev, buf, 10) == 9);
    CHECK(strcmp(buf, "{\"ev\":\"st") == 0);
    CHECK(tail_untouched(buf, 10, sizeof buf));

    memset(buf, 'Z', sizeof buf);
    CHECK(lmesh_host_format_event(&ev, buf, 1) == 0);
    CHECK(buf[0] == '\0');
    CHECK(tail_untouched(buf, 1, sizeof buf));

    memset(buf, 'Z', sizeof buf);
    CHECK(lmesh_host_format_event(&ev, buf, 0) == 0);
    CHECK(tail_untouched(buf, 0, sizeof buf));
    CHECK(lmesh_host_format_event(&ev, NULL, 0) == 0);
    CHECK(lmesh_host_format_event(&ev, NULL, 64) == 0);

    const char text[] = "a\"b";
    lmesh_event rx;
    rx_event(&rx, LMESH_MSG_TEXT, text, 3);
    const char *rx_full = RX_HEAD("text") "\"text\":\"a\\\"b\"}\n";
    size_t cut = strlen(RX_HEAD("text") "\"text\":\"a\\");
    memset(buf, 'Z', sizeof buf);
    CHECK(lmesh_host_format_event(&rx, buf, cut + 1) == cut);
    CHECK(strncmp(buf, rx_full, cut) == 0 && buf[cut] == '\0');
    CHECK(tail_untouched(buf, cut + 1, sizeof buf));
}

int main(void)
{
    test_basic_commands();
    test_send();
    test_cmd_tel();
    test_cancel();
    test_dumps();
    test_dump_truncation();
    test_metric_format();
    test_events();
    test_rx_events();
    test_event_truncation();

    printf("%d checks, %d failures\n", checks, failures);
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
