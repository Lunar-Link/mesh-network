/* Frame header codec, CRC-32 and AES-128-CMAC integrity tag, network-id filter. SPEC.md Section 5. */
#include "frame.h"

#include "aes.h"

uint32_t lmesh_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static void cmac_double(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = (uint8_t)(in[0] >> 7);

    for (unsigned i = 0; i < 15; i++) {
        out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
    }
    out[15] = (uint8_t)((in[15] << 1) ^ (carry ? 0x87u : 0x00u));
}

void lmesh_cmac128(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16])
{
    lmesh_aes128 aes;
    uint8_t k1[16];
    uint8_t k2[16];
    uint8_t x[16] = {0};
    uint8_t last[16];

    lmesh_aes128_init(&aes, key);
    lmesh_aes128_encrypt_block(&aes, x, k1);
    cmac_double(k1, k1);
    cmac_double(k1, k2);

    size_t blocks = (len + 15u) / 16u;
    bool complete = blocks > 0 && len % 16u == 0;
    if (blocks == 0) {
        blocks = 1;
    }

    for (size_t b = 0; b + 1 < blocks; b++) {
        for (unsigned i = 0; i < 16; i++) {
            x[i] = (uint8_t)(x[i] ^ data[16 * b + i]);
        }
        lmesh_aes128_encrypt_block(&aes, x, x);
    }

    size_t off = 16u * (blocks - 1);
    size_t rem = len - off;
    for (unsigned i = 0; i < 16; i++) {
        if (complete) {
            last[i] = (uint8_t)(data[off + i] ^ k1[i]);
        } else {
            uint8_t m = (uint8_t)(i < rem ? data[off + i] : (i == rem ? 0x80u : 0x00u));
            last[i] = (uint8_t)(m ^ k2[i]);
        }
        x[i] = (uint8_t)(x[i] ^ last[i]);
    }
    lmesh_aes128_encrypt_block(&aes, x, mac);
}

void lmesh_frame_tag(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t tag[LMESH_TAG_LEN])
{
#if LMESH_TAG_CMAC
    uint8_t mac[16];

    lmesh_cmac128(key, data, len, mac);
    for (unsigned i = 0; i < LMESH_TAG_LEN; i++) {
        tag[i] = mac[i];
    }
#else
    (void)key;
    lmesh_put32(tag, lmesh_crc32(data, len));
#endif
}

size_t lmesh_frame_encode(const lmesh_node *node, const lmesh_frame_hdr *hdr, const uint8_t *payload, uint8_t *out,
                          size_t out_max)
{
    if (node == NULL || hdr == NULL || out == NULL || hdr->type > 15u || hdr->payload_len > LMESH_MAX_PAYLOAD ||
        (hdr->payload_len > 0 && payload == NULL)) {
        return 0;
    }
    size_t body = (size_t)LMESH_HEADER_LEN + hdr->payload_len;
    size_t total = body + LMESH_TAG_LEN;
    if (out_max < total) {
        return 0;
    }

    out[0] = (uint8_t)((LMESH_FRAME_VERSION << 4) | hdr->type);
    out[1] = node->ident.network_id;
    out[2] = hdr->tx;
    out[3] = hdr->rx;
    out[4] = hdr->seq;
    out[5] = hdr->flags;
    out[6] = hdr->payload_len;
    out[7] = 0;
    for (size_t i = 0; i < hdr->payload_len; i++) {
        out[LMESH_HEADER_LEN + i] = payload[i];
    }
    lmesh_frame_tag(node->ident.key, out, body, &out[body]);
    return total;
}

lmesh_err lmesh_frame_decode(lmesh_node *node, const uint8_t *in, size_t len, lmesh_frame_hdr *hdr,
                             const uint8_t **payload)
{
    if (in == NULL || len < LMESH_HEADER_LEN + LMESH_TAG_LEN || len > LMESH_MAX_FRAME ||
        (in[0] >> 4) != LMESH_FRAME_VERSION) {
        node->stats.rx_malformed++;
        return LMESH_ERR_ARG;
    }
    if (in[1] != node->ident.network_id) {
        node->stats.rx_foreign++;
        return LMESH_ERR_ARG;
    }
    size_t body = (size_t)LMESH_HEADER_LEN + in[6];
    if (body + LMESH_TAG_LEN != len) {
        node->stats.rx_malformed++;
        return LMESH_ERR_ARG;
    }

    uint8_t tag[LMESH_TAG_LEN];
    uint8_t diff = 0;
    lmesh_frame_tag(node->ident.key, in, body, tag);
    for (unsigned i = 0; i < LMESH_TAG_LEN; i++) {
        diff = (uint8_t)(diff | (tag[i] ^ in[body + i]));
    }
    if (diff != 0) {
        node->stats.rx_bad_tag++;
        return LMESH_ERR_ARG;
    }

    hdr->version = (uint8_t)(in[0] >> 4);
    hdr->type = (uint8_t)(in[0] & 0x0Fu);
    hdr->network_id = in[1];
    hdr->tx = in[2];
    hdr->rx = in[3];
    hdr->seq = in[4];
    hdr->flags = in[5];
    hdr->payload_len = in[6];
    *payload = &in[LMESH_HEADER_LEN];
    return LMESH_OK;
}

void lmesh_frame_retag(const lmesh_node *node, uint8_t *buf, size_t len)
{
    if (node == NULL || buf == NULL || len < LMESH_HEADER_LEN + LMESH_TAG_LEN) {
        return;
    }
    size_t body = len - LMESH_TAG_LEN;
    lmesh_frame_tag(node->ident.key, buf, body, &buf[body]);
}
