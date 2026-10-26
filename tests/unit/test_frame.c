/* Unit tests for the frame codec, CRC-32, AES-128, and the CMAC integrity tag. */
#include <stdio.h>
#include <string.h>

#include "aes.h"
#include "frame.h"

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

static lmesh_node node;

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    while (s[0] && s[1]) {
        unsigned v = 0;
        if (sscanf(s, "%2x", &v) != 1) {
            break;
        }
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void reset_node(void)
{
    memset(&node, 0, sizeof node);
    node.ident.id = 3;
    node.ident.network_id = 0x5A;
    for (unsigned i = 0; i < 16; i++) {
        node.ident.key[i] = (uint8_t)(0xA0u + i);
    }
}

static bool stats_zero(void)
{
    return node.stats.rx_malformed == 0 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0;
}

static void test_crc32(void)
{
    CHECK(lmesh_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(lmesh_crc32(NULL, 0) == 0u);
}

static void test_aes(void)
{
    uint8_t key[16], pt[16], ct[16], out[16];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    unhex("69c4e0d86a7b0430d8cdb78070b4c55a", ct);
    lmesh_aes128 aes;
    lmesh_aes128_init(&aes, key);
    lmesh_aes128_encrypt_block(&aes, pt, out);
    CHECK(memcmp(out, ct, 16) == 0);
}

static void test_cmac(void)
{
    static const char *const msg_hex = "6bc1bee22e409f96e93d7e117393172a"
                                       "ae2d8a571e03ac9c9eb76fac45af8e51"
                                       "30c81c46a35ce411e5fbc1191a0a52ef"
                                       "f69f2445df4f9b17ad2b417be66c3710";
    static const struct {
        size_t len;
        const char *mac;
    } vec[] = {
        {0, "bb1d6929e95937287fa37d129b756746"},
        {16, "070a16b46b4d4144f79bdd9dd04a287c"},
        {40, "dfa66747de9ae63030ca32611497c827"},
        {64, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    uint8_t key[16], msg[64], want[16], mac[16];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    CHECK(unhex(msg_hex, msg) == 64);

    for (size_t v = 0; v < sizeof vec / sizeof vec[0]; v++) {
        unhex(vec[v].mac, want);
        lmesh_cmac128(key, vec[v].len ? msg : NULL, vec[v].len, mac);
        CHECK(memcmp(mac, want, 16) == 0);
    }
}

static void test_tag_mode(void)
{
    uint8_t data[40], tag[LMESH_TAG_LEN], want[LMESH_TAG_LEN];
    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 7u);
    }
    reset_node();
    lmesh_frame_tag(node.ident.key, data, sizeof data, tag);
#if LMESH_TAG_CMAC
    uint8_t mac[16];
    lmesh_cmac128(node.ident.key, data, sizeof data, mac);
    memcpy(want, mac, LMESH_TAG_LEN);
#else
    lmesh_put32(want, lmesh_crc32(data, sizeof data));
#endif
    CHECK(memcmp(tag, want, LMESH_TAG_LEN) == 0);
}

static lmesh_frame_hdr make_hdr(uint8_t type, uint8_t len)
{
    lmesh_frame_hdr h = {0};
    h.type = type;
    h.tx = 3;
    h.rx = (uint8_t)(type == LMESH_FT_DATA ? 9u : LMESH_ID_ALL);
    h.seq = (uint8_t)(200u + type);
    h.flags = type == LMESH_FT_DATA ? (uint8_t)LMESH_FLAG_ACK_REQ : 0u;
    h.payload_len = len;
    return h;
}

static void test_round_trip(void)
{
    uint8_t payload[LMESH_MAX_PAYLOAD];
    uint8_t buf[LMESH_MAX_FRAME];
    for (unsigned i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i ^ 0x3Cu);
    }

    for (uint8_t type = 0; type <= 15; type++) {
        const uint8_t lens[2] = {0, LMESH_MAX_PAYLOAD};
        for (unsigned k = 0; k < 2; k++) {
            reset_node();
            lmesh_frame_hdr h = make_hdr(type, lens[k]);
            size_t n = lmesh_frame_encode(&node, &h, lens[k] ? payload : NULL, buf, sizeof buf);
            CHECK(n == (size_t)LMESH_HEADER_LEN + lens[k] + LMESH_TAG_LEN);
            CHECK(buf[0] == (uint8_t)((LMESH_FRAME_VERSION << 4) | type));
            CHECK(buf[1] == 0x5A);
            CHECK(buf[2] == h.tx && buf[3] == h.rx && buf[4] == h.seq && buf[5] == h.flags);
            CHECK(buf[6] == lens[k] && buf[7] == 0);

            lmesh_frame_hdr d;
            const uint8_t *p = NULL;
            CHECK(lmesh_frame_decode(&node, buf, n, &d, &p) == LMESH_OK);
            CHECK(stats_zero());
            CHECK(d.version == LMESH_FRAME_VERSION && d.type == type && d.network_id == 0x5A);
            CHECK(d.tx == h.tx && d.rx == h.rx && d.seq == h.seq && d.flags == h.flags);
            CHECK(d.payload_len == lens[k]);
            CHECK(p == &buf[LMESH_HEADER_LEN]);
            CHECK(memcmp(p, payload, lens[k]) == 0);
        }
    }
    CHECK(LMESH_MAX_PAYLOAD == LMESH_MAX_FRAME - 12);
}

static void test_encode_rejects(void)
{
    uint8_t payload[LMESH_MAX_PAYLOAD + 1] = {0};
    uint8_t buf[LMESH_MAX_FRAME + 8];
    reset_node();

    lmesh_frame_hdr h = make_hdr(LMESH_FT_DATA, 10);
    h.type = 16;
    CHECK(lmesh_frame_encode(&node, &h, payload, buf, sizeof buf) == 0);
    h = make_hdr(LMESH_FT_DATA, LMESH_MAX_PAYLOAD + 1);
    CHECK(lmesh_frame_encode(&node, &h, payload, buf, sizeof buf) == 0);
    h = make_hdr(LMESH_FT_DATA, 10);
    CHECK(lmesh_frame_encode(&node, &h, NULL, buf, sizeof buf) == 0);
    CHECK(lmesh_frame_encode(&node, &h, payload, buf, 10 + 11) == 0);
    CHECK(lmesh_frame_encode(&node, &h, payload, buf, 10 + 12) == 22);
    CHECK(lmesh_frame_encode(&node, NULL, payload, buf, sizeof buf) == 0);
    CHECK(lmesh_frame_encode(&node, &h, payload, NULL, sizeof buf) == 0);
}

static lmesh_err decode(const uint8_t *in, size_t len)
{
    lmesh_frame_hdr d;
    const uint8_t *p;
    return lmesh_frame_decode(&node, in, len, &d, &p);
}

static size_t good_frame(uint8_t *buf, uint8_t len)
{
    uint8_t payload[LMESH_MAX_PAYLOAD];
    for (unsigned i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i + 1u);
    }
    lmesh_frame_hdr h = make_hdr(LMESH_FT_DATA, len);
    return lmesh_frame_encode(&node, &h, payload, buf, LMESH_MAX_FRAME);
}

static void retag(uint8_t *buf, size_t n)
{
    lmesh_frame_tag(node.ident.key, buf, n - LMESH_TAG_LEN, &buf[n - LMESH_TAG_LEN]);
}

static void test_decode_rejects(void)
{
    uint8_t buf[LMESH_MAX_FRAME + 8];
    size_t n;

    reset_node();
    good_frame(buf, 20);
    CHECK(decode(buf, LMESH_HEADER_LEN + LMESH_TAG_LEN - 1) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 1 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);
    CHECK(decode(buf, 0) != LMESH_OK);
    CHECK(decode(NULL, 0) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 3 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);

    reset_node();
    memset(buf, 0, sizeof buf);
    buf[0] = (uint8_t)((LMESH_FRAME_VERSION << 4) | LMESH_FT_DATA);
    buf[1] = 0x5A;
    buf[6] = LMESH_MAX_PAYLOAD + 1;
    retag(buf, LMESH_MAX_FRAME + 1);
    CHECK(decode(buf, LMESH_MAX_FRAME + 1) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 1 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);

    reset_node();
    n = good_frame(buf, 20);
    buf[0] = (uint8_t)(((LMESH_FRAME_VERSION + 1) << 4) | LMESH_FT_DATA);
    retag(buf, n);
    CHECK(decode(buf, n) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 1 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);

    reset_node();
    n = good_frame(buf, 20);
    buf[1] = 0x5B;
    CHECK(decode(buf, n) != LMESH_OK);
    CHECK(node.stats.rx_foreign == 1 && node.stats.rx_malformed == 0 && node.stats.rx_bad_tag == 0);

    reset_node();
    n = good_frame(buf, 20);
    buf[6] = 21;
    retag(buf, n);
    CHECK(decode(buf, n) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 1 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);
    buf[6] = 19;
    retag(buf, n);
    CHECK(decode(buf, n) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 2 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);

    reset_node();
    n = good_frame(buf, 20);
    CHECK(decode(buf, n - 1) != LMESH_OK);
    CHECK(decode(buf, n + 1) != LMESH_OK);
    CHECK(node.stats.rx_malformed == 2 && node.stats.rx_foreign == 0 && node.stats.rx_bad_tag == 0);

    reset_node();
    n = good_frame(buf, 20);
    buf[7] = 0xEE;
    retag(buf, n);
    CHECK(decode(buf, n) == LMESH_OK);
    CHECK(stats_zero());

    reset_node();
    n = good_frame(buf, 20);
    buf[n - 1] ^= 0x01u;
    CHECK(decode(buf, n) != LMESH_OK);
    CHECK(node.stats.rx_bad_tag == 1 && node.stats.rx_malformed == 0 && node.stats.rx_foreign == 0);

    reset_node();
    n = good_frame(buf, 20);
    node.ident.key[0] ^= 1u;
    retag(buf, n);
    node.ident.key[0] ^= 1u;
#if LMESH_TAG_CMAC
    CHECK(decode(buf, n) != LMESH_OK);
    CHECK(node.stats.rx_bad_tag == 1 && node.stats.rx_malformed == 0 && node.stats.rx_foreign == 0);
#else
    CHECK(decode(buf, n) == LMESH_OK);
    CHECK(stats_zero());
#endif
}

static void test_bit_flips(void)
{
    uint8_t buf[LMESH_MAX_FRAME];
    uint8_t copy[LMESH_MAX_FRAME];

    reset_node();
    size_t n = good_frame(buf, 30);
    CHECK(decode(buf, n) == LMESH_OK);

    uint32_t want_malformed = 0, want_foreign = 0, want_bad = 0;
    unsigned accepted = 0;
    for (size_t byte = 0; byte < n; byte++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            memcpy(copy, buf, n);
            copy[byte] = (uint8_t)(copy[byte] ^ (1u << bit));
            if (decode(copy, n) == LMESH_OK) {
                accepted++;
            }
            if ((byte == 0 && bit >= 4) || byte == 6) {
                want_malformed++;
            } else if (byte == 1) {
                want_foreign++;
            } else {
                want_bad++;
            }
        }
    }
    CHECK(accepted == 0);
    CHECK(node.stats.rx_malformed == want_malformed);
    CHECK(node.stats.rx_foreign == want_foreign);
    CHECK(node.stats.rx_bad_tag == want_bad);
}

int main(void)
{
    test_crc32();
    test_aes();
    test_cmac();
    test_tag_mode();
    test_round_trip();
    test_encode_rejects();
    test_decode_rejects();
    test_bit_flips();

    printf("%d checks, %d failures, tag %s\n", checks, failures, LMESH_TAG_CMAC ? "cmac" : "crc32");
    printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
