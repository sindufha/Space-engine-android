#include "sha256.h"

#include <string.h>

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

static const uint32_t SHA256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static void sha256_transform(se_sha256_ctx_t *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        int j = i * 4;
        w[i] = ((uint32_t)block[j] << 24) |
               ((uint32_t)block[j + 1] << 16) |
               ((uint32_t)block[j + 2] << 8) |
               ((uint32_t)block[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void se_sha256_init(se_sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bitcount = 0;
}

void se_sha256_update(se_sha256_ctx_t *ctx, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t buf_used = (size_t)((ctx->bitcount >> 3) & 63U);
    ctx->bitcount += (uint64_t)len * 8ULL;

    size_t offset = 0;
    if (buf_used > 0) {
        size_t take = 64U - buf_used;
        if (len < take) {
            memcpy(ctx->buffer + buf_used, bytes, len);
            return;
        }
        memcpy(ctx->buffer + buf_used, bytes, take);
        sha256_transform(ctx, ctx->buffer);
        offset += take;
    }

    while (offset + 64U <= len) {
        sha256_transform(ctx, bytes + offset);
        offset += 64U;
    }

    if (offset < len) {
        memcpy(ctx->buffer, bytes + offset, len - offset);
    }
}

void se_sha256_final(se_sha256_ctx_t *ctx, uint8_t digest[32]) {
    uint64_t total_bits = ctx->bitcount;
    uint8_t pad = 0x80U;
    se_sha256_update(ctx, &pad, 1);

    static const uint8_t zero_block[64] = {0};
    size_t buf_used = (size_t)((ctx->bitcount >> 3) & 63U);
    size_t pad_zeros = (buf_used <= 56U) ? (56U - buf_used) : (120U - buf_used);
    if (pad_zeros > 0) {
        se_sha256_update(ctx, zero_block, pad_zeros);
    }

    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) {
        len_be[7 - i] = (uint8_t)((total_bits >> (i * 8)) & 0xffU);
    }
    se_sha256_update(ctx, len_be, 8);

    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = (uint8_t)((ctx->state[i] >> 24) & 0xffU);
        digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16) & 0xffU);
        digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8) & 0xffU);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xffU);
    }
}

void se_sha256_to_hex(const uint8_t digest[32], char hex_out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        hex_out[i * 2 + 0] = hex[(digest[i] >> 4) & 0xfU];
        hex_out[i * 2 + 1] = hex[digest[i] & 0xfU];
    }
    hex_out[64] = '\0';
}
