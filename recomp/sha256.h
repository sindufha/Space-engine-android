#ifndef SE_SHA256_H
#define SE_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Streaming SHA-256 hash context (RFC 6234). */
typedef struct se_sha256_ctx {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
} se_sha256_ctx_t;

/** Initialize a SHA-256 context. */
void se_sha256_init(se_sha256_ctx_t *ctx);

/** Feed bytes into an active SHA-256 context. */
void se_sha256_update(se_sha256_ctx_t *ctx, const void *data, size_t len);

/** Finalize SHA-256 computation into a 32-byte digest. */
void se_sha256_final(se_sha256_ctx_t *ctx, uint8_t digest[32]);

/** Format a 32-byte digest as a lowercase 64-character hex string + NUL. */
void se_sha256_to_hex(const uint8_t digest[32], char hex_out[65]);

#ifdef __cplusplus
}
#endif

#endif /* SE_SHA256_H */
