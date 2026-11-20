/*
 * minizip_ord.c - Clean-room Win64 zlibwapi.dll 13-ordinal MiniZip + ZipCrypto layer.
 *
 * Implements exact Win64 LLP64 ABI for the 13 MiniZip ordinals imported by
 * SpaceEngine.exe from zlibwapi.dll:
 *   #61  unzOpen
 *   #62  unzClose
 *   #63  unzGetGlobalInfo
 *   #64  unzGetCurrentFileInfo
 *   #65  unzGoToFirstFile
 *   #66  unzGoToNextFile
 *   #67  unzOpenCurrentFile
 *   #68  unzReadCurrentFile
 *   #70  unztell
 *   #72  unzCloseCurrentFile
 *   #79  unzOpenCurrentFilePassword
 *   #100 unzGetFilePos
 *   #101 unzGoToFilePos
 *
 * Uses system zlib (-lz) on Android NDK / POSIX when <zlib.h> is available,
 * with a clean-room streaming RFC-1951 DEFLATE fallback for host test toolchains
 * lacking zlib headers (e.g. MinGW64). Always preserves PKWARE ZipCrypto
 * decryption so data/shaders/Shaders.pak reads transparently.
 */
#include "hle_bridge.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include(<zlib.h>)
#    define SE_MINIZIP_HAVE_ZLIB 1
#    include <zlib.h>
#  else
#    define SE_MINIZIP_HAVE_ZLIB 0
#  endif
#else
#  define SE_MINIZIP_HAVE_ZLIB 0
#endif

/* Standard MiniZip status codes */
#define UNZ_OK                  (0)
#define UNZ_EOF                 (0)
#define UNZ_END_OF_LIST_OF_FILE (-100)
#define UNZ_ERRNO               (-1)
#define UNZ_PARAMERROR          (-102)
#define UNZ_BADZIPFILE          (-103)
#define UNZ_INTERNALERROR       (-104)
#define UNZ_CRCERROR            (-105)

/* Exact Win64 LLP64 structs (MSVC unsigned long = 32-bit) */
typedef struct {
    uint32_t number_entry;
    uint32_t size_comment;
} win64_unz_global_info_t;

typedef struct {
    uint32_t tm_sec;
    uint32_t tm_min;
    uint32_t tm_hour;
    uint32_t tm_mday;
    uint32_t tm_mon;
    uint32_t tm_year;
} win64_tm_unz_t;

typedef struct {
    uint32_t version;
    uint32_t version_needed;
    uint32_t flag;
    uint32_t compression_method;
    uint32_t dosDate;
    uint32_t crc;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t size_filename;
    uint32_t size_file_extra;
    uint32_t size_file_comment;
    uint32_t disk_num_start;
    uint32_t internal_fa;
    uint32_t external_fa;
    win64_tm_unz_t tmu_date;
} win64_unz_file_info_t;

typedef struct {
    uint32_t pos_in_zip_directory;
    uint32_t num_of_file;
} win64_unz_file_pos_t;

typedef struct {
    uint32_t cd_offset;
    uint32_t num_of_file;
    uint16_t version;
    uint16_t version_needed;
    uint16_t flag;
    uint16_t method;
    uint32_t dos_date;
    uint32_t crc32;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint64_t local_hdr_offset;
    uint32_t disk_start;
    uint16_t internal_fa;
    uint32_t external_fa;
    uint16_t fname_len;
    uint16_t extra_len;
    uint16_t comment_len;
    uint32_t fname_pool_off;
    uint32_t extra_pool_off;
    uint32_t comment_pool_off;
} zip_entry_t;

#if !SE_MINIZIP_HAVE_ZLIB
/* ========================================================================= */
/* Clean-room streaming RFC-1951 DEFLATE decompressor (STORE + FIXED + DYN)  */
/* ========================================================================= */

#define INF_MAXBITS 15
#define INF_WIN_SIZE 32768u
#define INF_WIN_MASK (INF_WIN_SIZE - 1u)

typedef struct {
    int16_t counts[INF_MAXBITS + 1];
    int16_t symbols[320];
} inf_huff_t;

typedef enum {
    INF_BLOCK_HDR = 0,
    INF_STORED_HDR,
    INF_STORED_COPY,
    INF_DECODE_LITLEN,
    INF_COPY_MATCH,
    INF_DONE,
    INF_ERROR
} inf_state_t;

typedef struct {
    inf_state_t state;
    bool bfinal;
    uint32_t bitbuf;
    uint32_t bitcnt;

    /* Stored block state */
    uint32_t stored_len;

    /* Dynamic/Fixed trees */
    inf_huff_t litlen_tree;
    inf_huff_t dist_tree;

    /* Active match copy */
    uint32_t match_len;
    uint32_t match_dist;

    /* Sliding 32 KB window */
    uint8_t window[INF_WIN_SIZE];
    uint32_t win_pos;
} raw_inflate_t;

static void inf_build_huff(inf_huff_t *h, const uint8_t *lengths, int num_syms) {
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < num_syms; ++i) {
        if (lengths[i] <= INF_MAXBITS) h->counts[lengths[i]]++;
    }
    int16_t offsets[INF_MAXBITS + 1];
    int16_t acc = 0;
    offsets[0] = 0;
    for (int bits = 1; bits <= INF_MAXBITS; ++bits) {
        offsets[bits] = acc;
        acc = (int16_t)(acc + h->counts[bits]);
    }
    for (int sym = 0; sym < num_syms; ++sym) {
        uint8_t len = lengths[sym];
        if (len != 0 && len <= INF_MAXBITS) {
            h->symbols[offsets[len]++] = (int16_t)sym;
        }
    }
}

static bool inf_ensure_bits(raw_inflate_t *s, const uint8_t **in_p, const uint8_t *in_end, uint32_t n) {
    while (s->bitcnt < n) {
        if (*in_p >= in_end) return false;
        s->bitbuf |= (uint32_t)(**in_p) << s->bitcnt;
        (*in_p)++;
        s->bitcnt += 8u;
    }
    return true;
}

static uint32_t inf_take_bits(raw_inflate_t *s, uint32_t n) {
    uint32_t v = s->bitbuf & ((1u << n) - 1u);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

/* Streaming canonical Huffman decode; commits bits only on full symbol match. */
static int inf_decode_sym(raw_inflate_t *s, const inf_huff_t *h,
                          const uint8_t **in_p, const uint8_t *in_end, int *out_sym) {
    uint32_t bitbuf = s->bitbuf;
    uint32_t bitcnt = s->bitcnt;
    int code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= INF_MAXBITS; ++len) {
        if (bitcnt < (uint32_t)len) {
            if (*in_p >= in_end) {
                s->bitbuf = bitbuf;
                s->bitcnt = bitcnt;
                return 0; /* Need more input bytes */
            }
            bitbuf |= (uint32_t)(**in_p) << bitcnt;
            (*in_p)++;
            bitcnt += 8u;
        }
        code |= (int)((bitbuf >> (len - 1)) & 1u);
        int count = h->counts[len];
        if (code - count < first) {
            s->bitbuf = bitbuf >> len;
            s->bitcnt = bitcnt - (uint32_t)len;
            *out_sym = h->symbols[index + (code - first)];
            return 1;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->bitbuf = bitbuf;
    s->bitcnt = bitcnt;
    return -1;
}

static void inf_build_fixed(inf_huff_t *litlen, inf_huff_t *dist) {
    uint8_t ll[288];
    uint8_t dl[32];
    for (int i = 0; i <= 143; ++i) ll[i] = 8;
    for (int i = 144; i <= 255; ++i) ll[i] = 9;
    for (int i = 256; i <= 279; ++i) ll[i] = 7;
    for (int i = 280; i <= 287; ++i) ll[i] = 8;
    for (int i = 0; i < 32; ++i) dl[i] = 5;
    inf_build_huff(litlen, ll, 288);
    inf_build_huff(dist, dl, 32);
}

static void inf_init(raw_inflate_t *s) {
    memset(s, 0, sizeof(*s));
    s->state = INF_BLOCK_HDR;
}

typedef struct {
    int state; /* 0 = need blockhdr, 1 = reading hlit/hdist/hclen, 2 = clens, 3 = codes, 4 = extra */
    uint32_t hlit;
    uint32_t hdist;
    uint32_t hclen;
    uint32_t idx;
    uint8_t clen_lens[19];
    inf_huff_t clen_tree;
    uint8_t all_lens[288 + 32];
    int pending_sym;
} dyn_hdr_state_t;

static int inf_read_dyn_trees(raw_inflate_t *s, dyn_hdr_state_t *dh,
                              const uint8_t **in_p, const uint8_t *in_end) {
    static const uint8_t kClenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    while (1) {
        if (dh->state == 0) {
            if (!inf_ensure_bits(s, in_p, in_end, 14u)) return 0;
            dh->hlit = inf_take_bits(s, 5u) + 257u;
            dh->hdist = inf_take_bits(s, 5u) + 1u;
            dh->hclen = inf_take_bits(s, 4u) + 4u;
            if (dh->hlit > 286u || dh->hdist > 30u) return -1;
            memset(dh->clen_lens, 0, sizeof(dh->clen_lens));
            memset(dh->all_lens, 0, sizeof(dh->all_lens));
            dh->idx = 0;
            dh->state = 1;
        }
        if (dh->state == 1) {
            while (dh->idx < dh->hclen) {
                if (!inf_ensure_bits(s, in_p, in_end, 3u)) return 0;
                dh->clen_lens[kClenOrder[dh->idx++]] = (uint8_t)inf_take_bits(s, 3u);
            }
            inf_build_huff(&dh->clen_tree, dh->clen_lens, 19);
            dh->idx = 0;
            dh->state = 2;
        }
        if (dh->state == 2) {
            uint32_t total = dh->hlit + dh->hdist;
            while (dh->idx < total) {
                int sym = 0;
                int rc = inf_decode_sym(s, &dh->clen_tree, in_p, in_end, &sym);
                if (rc == 0) return 0;
                if (rc < 0) return -1;
                if (sym < 16) {
                    dh->all_lens[dh->idx++] = (uint8_t)sym;
                } else {
                    dh->pending_sym = sym;
                    dh->state = 3;
                    break;
                }
            }
            if (dh->state == 2) {
                inf_build_huff(&s->litlen_tree, dh->all_lens, (int)dh->hlit);
                inf_build_huff(&s->dist_tree, dh->all_lens + dh->hlit, (int)dh->hdist);
                dh->state = 0;
                return 1;
            }
        }
        if (dh->state == 3) {
            uint32_t extra_bits = (dh->pending_sym == 16) ? 2u : (dh->pending_sym == 17 ? 3u : 7u);
            if (!inf_ensure_bits(s, in_p, in_end, extra_bits)) return 0;
            uint32_t rep = inf_take_bits(s, extra_bits);
            uint32_t count = rep + ((dh->pending_sym == 18) ? 11u : 3u);
            uint8_t val = 0;
            if (dh->pending_sym == 16) {
                if (dh->idx == 0) return -1;
                val = dh->all_lens[dh->idx - 1u];
            }
            if (dh->idx + count > dh->hlit + dh->hdist) return -1;
            while (count-- > 0) {
                dh->all_lens[dh->idx++] = val;
            }
            dh->state = 2;
        }
    }
}

typedef struct {
    raw_inflate_t inf;
    dyn_hdr_state_t dyn;
    bool reading_dyn_hdr;
    uint32_t pending_dist_sym;
    uint32_t pending_len_sym;
    int sub_step; /* for litlen/match extra bits re-entrancy */
} inf_stream_ctx_t;

static int inf_step(inf_stream_ctx_t *ctx,
                    const uint8_t **in_p, const uint8_t *in_end,
                    uint8_t **out_p, uint8_t *out_end) {
    static const uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    raw_inflate_t *s = &ctx->inf;
    while (1) {
        if (s->state == INF_DONE) return 1;
        if (s->state == INF_ERROR) return -1;

        if (s->state == INF_BLOCK_HDR) {
            if (ctx->reading_dyn_hdr) {
                int rc = inf_read_dyn_trees(s, &ctx->dyn, in_p, in_end);
                if (rc == 0) return 0;
                if (rc < 0) { s->state = INF_ERROR; return -1; }
                ctx->reading_dyn_hdr = false;
                ctx->sub_step = 0;
                s->state = INF_DECODE_LITLEN;
                continue;
            }
            if (!inf_ensure_bits(s, in_p, in_end, 3u)) return 0;
            uint32_t hdr = inf_take_bits(s, 3u);
            s->bfinal = (hdr & 1u) != 0;
            uint32_t btype = hdr >> 1;
            if (btype == 0u) {
                /* Align to byte boundary */
                uint32_t discard = s->bitcnt & 7u;
                (void)inf_take_bits(s, discard);
                s->state = INF_STORED_HDR;
            } else if (btype == 1u) {
                inf_build_fixed(&s->litlen_tree, &s->dist_tree);
                ctx->sub_step = 0;
                s->state = INF_DECODE_LITLEN;
            } else if (btype == 2u) {
                memset(&ctx->dyn, 0, sizeof(ctx->dyn));
                ctx->reading_dyn_hdr = true;
            } else {
                s->state = INF_ERROR;
                return -1;
            }
        } else if (s->state == INF_STORED_HDR) {
            if (!inf_ensure_bits(s, in_p, in_end, 32u)) return 0;
            uint32_t len = inf_take_bits(s, 16u);
            uint32_t nlen = inf_take_bits(s, 16u);
            if ((len ^ 0xFFFFu) != nlen) { s->state = INF_ERROR; return -1; }
            s->stored_len = len;
            s->state = (len == 0u) ? (s->bfinal ? INF_DONE : INF_BLOCK_HDR) : INF_STORED_COPY;
        } else if (s->state == INF_STORED_COPY) {
            if (*in_p >= in_end || *out_p >= out_end) return 0;
            uint32_t avail_in = (uint32_t)(in_end - *in_p);
            uint32_t avail_out = (uint32_t)(out_end - *out_p);
            uint32_t chunk = s->stored_len;
            if (chunk > avail_in) chunk = avail_in;
            if (chunk > avail_out) chunk = avail_out;
            for (uint32_t i = 0; i < chunk; ++i) {
                uint8_t b = (*in_p)[i];
                (*out_p)[i] = b;
                s->window[s->win_pos] = b;
                s->win_pos = (s->win_pos + 1u) & INF_WIN_MASK;
            }
            *in_p += chunk;
            *out_p += chunk;
            s->stored_len -= chunk;
            if (s->stored_len == 0u) {
                s->state = s->bfinal ? INF_DONE : INF_BLOCK_HDR;
            }
        } else if (s->state == INF_DECODE_LITLEN) {
            if (ctx->sub_step == 0) {
                int sym = 0;
                int rc = inf_decode_sym(s, &s->litlen_tree, in_p, in_end, &sym);
                if (rc == 0) return 0;
                if (rc < 0) { s->state = INF_ERROR; return -1; }
                if (sym < 256) {
                    if (*out_p >= out_end) {
                        /* Push single byte via length-1 deferred copy? No, simply require out space:
                         * wait, sym already consumed bits! Stage in match_len=1/literal or check before!
                         * Actually check out_end BEFORE consuming, or use 1-byte literal hold! */
                        ctx->pending_len_sym = (uint32_t)sym;
                        ctx->sub_step = 10;
                        return 0;
                    }
                    uint8_t b = (uint8_t)sym;
                    *(*out_p)++ = b;
                    s->window[s->win_pos] = b;
                    s->win_pos = (s->win_pos + 1u) & INF_WIN_MASK;
                    continue;
                }
                if (sym == 256) {
                    s->state = s->bfinal ? INF_DONE : INF_BLOCK_HDR;
                    continue;
                }
                if (sym > 285) { s->state = INF_ERROR; return -1; }
                ctx->pending_len_sym = (uint32_t)(sym - 257);
                ctx->sub_step = 1;
            }
            if (ctx->sub_step == 10) {
                if (*out_p >= out_end) return 0;
                uint8_t b = (uint8_t)ctx->pending_len_sym;
                *(*out_p)++ = b;
                s->window[s->win_pos] = b;
                s->win_pos = (s->win_pos + 1u) & INF_WIN_MASK;
                ctx->sub_step = 0;
                continue;
            }
            if (ctx->sub_step == 1) {
                uint8_t eb = kLenExtra[ctx->pending_len_sym];
                if (eb > 0 && !inf_ensure_bits(s, in_p, in_end, eb)) return 0;
                uint32_t ex = (eb > 0) ? inf_take_bits(s, eb) : 0u;
                s->match_len = kLenBase[ctx->pending_len_sym] + ex;
                ctx->sub_step = 2;
            }
            if (ctx->sub_step == 2) {
                int dsym = 0;
                int rc = inf_decode_sym(s, &s->dist_tree, in_p, in_end, &dsym);
                if (rc == 0) return 0;
                if (rc < 0 || dsym >= 30) { s->state = INF_ERROR; return -1; }
                ctx->pending_dist_sym = (uint32_t)dsym;
                ctx->sub_step = 3;
            }
            if (ctx->sub_step == 3) {
                uint8_t eb = kDistExtra[ctx->pending_dist_sym];
                if (eb > 0 && !inf_ensure_bits(s, in_p, in_end, eb)) return 0;
                uint32_t ex = (eb > 0) ? inf_take_bits(s, eb) : 0u;
                s->match_dist = kDistBase[ctx->pending_dist_sym] + ex;
                if (s->match_dist == 0u || s->match_dist > INF_WIN_SIZE) {
                    s->state = INF_ERROR;
                    return -1;
                }
                ctx->sub_step = 0;
                s->state = INF_COPY_MATCH;
            }
        } else if (s->state == INF_COPY_MATCH) {
            if (*out_p >= out_end) return 0;
            while (s->match_len > 0u && *out_p < out_end) {
                uint32_t src_pos = (s->win_pos - s->match_dist) & INF_WIN_MASK;
                uint8_t b = s->window[src_pos];
                *(*out_p)++ = b;
                s->window[s->win_pos] = b;
                s->win_pos = (s->win_pos + 1u) & INF_WIN_MASK;
                s->match_len--;
            }
            if (s->match_len == 0u) {
                s->state = INF_DECODE_LITLEN;
            }
        }
    }
}
#endif

/* ========================================================================= */
/* CRC-32 & PKWARE ZipCrypto                                                 */
/* ========================================================================= */

static uint32_t g_crc_table[256];
static bool g_crc_init = false;

static void ensure_crc_table(void) {
    if (g_crc_init) return;
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_init = true;
}

static uint32_t se_crc32(uint32_t crc, const uint8_t *buf, size_t len) {
    ensure_crc_table();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = g_crc_table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t crc32_byte(uint32_t old_crc, uint8_t val) {
    return g_crc_table[(old_crc ^ val) & 0xFFu] ^ (old_crc >> 8);
}

typedef struct {
    uint32_t keys[3];
} zipcrypto_t;

static uint8_t zc_decrypt_byte(const zipcrypto_t *zc) {
    uint16_t temp = (uint16_t)((zc->keys[2] & 0xFFFFu) | 2u);
    return (uint8_t)(((uint32_t)temp * (uint32_t)(temp ^ 1u)) >> 8);
}

static void zc_update_keys(zipcrypto_t *zc, uint8_t c) {
    zc->keys[0] = crc32_byte(zc->keys[0], c);
    zc->keys[1] += zc->keys[0] & 0xFFu;
    zc->keys[1] = zc->keys[1] * 134775813u + 1u;
    zc->keys[2] = crc32_byte(zc->keys[2], (uint8_t)(zc->keys[1] >> 24));
}

static void zc_init_keys(zipcrypto_t *zc, const char *password) {
    ensure_crc_table();
    zc->keys[0] = 305419896u;
    zc->keys[1] = 591751049u;
    zc->keys[2] = 878082192u;
    if (password) {
        for (const char *p = password; *p; ++p) {
            zc_update_keys(zc, (uint8_t)*p);
        }
    }
}

static void zc_decrypt_buf(zipcrypto_t *zc, uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = (uint8_t)(buf[i] ^ zc_decrypt_byte(zc));
        zc_update_keys(zc, c);
        buf[i] = c;
    }
}

/* ========================================================================= */
/* ZIP Archive State                                                         */
/* ========================================================================= */

#define MZ_READ_CHUNK 16384u

typedef struct {
    FILE        *fp;
    uint64_t     file_size;
    zip_entry_t *entries;
    uint32_t     num_entries;
    uint32_t     comment_len;
    uint32_t     cur_idx;
    bool         has_cur;

    /* Currently open entry stream state */
    bool         entry_open;
    uint64_t     comp_rem;
    uint64_t     uncomp_pos;
    uint64_t     uncomp_size;
    uint32_t     expected_crc;
    uint32_t     running_crc;
    uint16_t     method;
    bool         encrypted;
    zipcrypto_t  zc;
    bool         stream_eof;
    bool         crc_checked;

    uint8_t      in_buf[MZ_READ_CHUNK];
    uint32_t     in_pos;
    uint32_t     in_len;

#if SE_MINIZIP_HAVE_ZLIB
    z_stream     zs;
    bool         zs_init;
#else
    inf_stream_ctx_t inf;
    bool             inf_init;
#endif

    char        *str_pool;
    uint32_t     str_pool_size;
    uint32_t     str_pool_cap;
} mz_archive_t;

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

static bool fseek64(FILE *fp, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(fp, (int64_t)offset, SEEK_SET) == 0;
#else
    return fseeko(fp, (off_t)offset, SEEK_SET) == 0;
#endif
}

static uint64_t ftell64_end(FILE *fp) {
#if defined(_WIN32)
    if (_fseeki64(fp, 0, SEEK_END) != 0) return 0;
    int64_t pos = _ftelli64(fp);
    return (pos > 0) ? (uint64_t)pos : 0ull;
#else
    if (fseeko(fp, 0, SEEK_END) != 0) return 0;
    off_t pos = ftello(fp);
    return (pos > 0) ? (uint64_t)pos : 0ull;
#endif
}

static uint32_t pool_append(mz_archive_t *a, const char *src, uint32_t len) {
    if (a->str_pool_size + len + 1u > a->str_pool_cap) {
        uint32_t ncap = a->str_pool_cap ? a->str_pool_cap * 2u : 4096u;
        while (ncap < a->str_pool_size + len + 1u) ncap *= 2u;
        char *nbuf = (char *)realloc(a->str_pool, ncap);
        if (!nbuf) return 0;
        a->str_pool = nbuf;
        a->str_pool_cap = ncap;
    }
    uint32_t off = a->str_pool_size;
    if (len > 0 && src) memcpy(a->str_pool + off, src, len);
    a->str_pool[off + len] = '\0';
    a->str_pool_size += len + 1u;
    return off;
}

static void dos_date_to_tm(uint32_t dos, win64_tm_unz_t *tm) {
    uint32_t d = dos >> 16;
    tm->tm_sec = (dos & 0x1Fu) * 2u;
    tm->tm_min = (dos >> 5) & 0x3Fu;
    tm->tm_hour = (dos >> 11) & 0x1Fu;
    tm->tm_mday = d & 0x1Fu;
    tm->tm_mon = ((d >> 5) & 0x0Fu);
    if (tm->tm_mon > 0u) tm->tm_mon -= 1u;
    tm->tm_year = ((d >> 9) & 0x7Fu) + 1980u;
}

static void close_current_entry_state(mz_archive_t *a) {
    if (!a) return;
#if SE_MINIZIP_HAVE_ZLIB
    if (a->zs_init) {
        inflateEnd(&a->zs);
        a->zs_init = false;
    }
#else
    a->inf_init = false;
#endif
    a->entry_open = false;
    a->comp_rem = 0;
    a->in_pos = 0;
    a->in_len = 0;
}

static bool parse_zip_directory(mz_archive_t *a) {
    if (a->file_size < 22u) return false;
    uint64_t scan_size = a->file_size < 65558u ? a->file_size : 65558u;
    uint64_t scan_start = a->file_size - scan_size;
    uint8_t *tail = (uint8_t *)malloc((size_t)scan_size);
    if (!tail) return false;

    if (!fseek64(a->fp, scan_start) ||
        fread(tail, 1, (size_t)scan_size, a->fp) != (size_t)scan_size) {
        free(tail);
        return false;
    }

    int64_t eocd_pos = -1;
    for (int64_t i = (int64_t)scan_size - 22; i >= 0; --i) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4B &&
            tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            uint16_t clen = rd_le16(tail + i + 20);
            if ((uint64_t)i + 22u + clen == scan_size) {
                eocd_pos = i;
                break;
            }
        }
    }
    if (eocd_pos < 0) {
        free(tail);
        return false;
    }

    const uint8_t *eocd = tail + eocd_pos;
    uint64_t total_entries = rd_le16(eocd + 10);
    uint64_t cd_size = rd_le32(eocd + 12);
    uint64_t cd_offset = rd_le32(eocd + 16);
    a->comment_len = rd_le16(eocd + 20);
    free(tail);

    /* ZIP64 EOCD locator check right before EOCD */
    if ((cd_offset == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu || total_entries == 0xFFFFu) &&
        scan_start + (uint64_t)eocd_pos >= 20u) {
        uint8_t loc64[20];
        if (fseek64(a->fp, scan_start + (uint64_t)eocd_pos - 20u) &&
            fread(loc64, 1, 20, a->fp) == 20 &&
            rd_le32(loc64) == 0x07064B50u) {
            uint64_t eocd64_off = rd_le64(loc64 + 8);
            uint8_t eocd64[56];
            if (eocd64_off + 56u <= a->file_size &&
                fseek64(a->fp, eocd64_off) &&
                fread(eocd64, 1, 56, a->fp) == 56 &&
                rd_le32(eocd64) == 0x06064B50u) {
                total_entries = rd_le64(eocd64 + 32);
                cd_size = rd_le64(eocd64 + 40);
                cd_offset = rd_le64(eocd64 + 48);
            }
        }
    }

    if (cd_offset + cd_size > a->file_size || total_entries > 2000000u) return false;

    if (total_entries == 0u) {
        a->entries = NULL;
        a->num_entries = 0u;
        a->cur_idx = 0u;
        a->has_cur = false;
        return true;
    }

    uint8_t *cd = (uint8_t *)malloc((size_t)cd_size);
    if (!cd) return false;
    if (!fseek64(a->fp, cd_offset) ||
        fread(cd, 1, (size_t)cd_size, a->fp) != (size_t)cd_size) {
        free(cd);
        return false;
    }

    a->entries = (zip_entry_t *)calloc((size_t)total_entries, sizeof(zip_entry_t));
    if (!a->entries) {
        free(cd);
        return false;
    }
    pool_append(a, "", 0); /* reserve offset 0 as empty string */

    uint64_t p = 0;
    uint32_t parsed = 0;
    while (parsed < (uint32_t)total_entries && p + 46u <= cd_size) {
        if (rd_le32(cd + p) != 0x02014B50u) break;
        zip_entry_t *e = &a->entries[parsed];
        e->cd_offset = (uint32_t)p;
        e->num_of_file = parsed;
        e->version = rd_le16(cd + p + 4);
        e->version_needed = rd_le16(cd + p + 6);
        e->flag = rd_le16(cd + p + 8);
        e->method = rd_le16(cd + p + 10);
        e->dos_date = rd_le32(cd + p + 12);
        e->crc32 = rd_le32(cd + p + 16);
        e->comp_size = rd_le32(cd + p + 20);
        e->uncomp_size = rd_le32(cd + p + 24);
        e->fname_len = rd_le16(cd + p + 28);
        e->extra_len = rd_le16(cd + p + 30);
        e->comment_len = rd_le16(cd + p + 32);
        e->disk_start = rd_le16(cd + p + 34);
        e->internal_fa = rd_le16(cd + p + 36);
        e->external_fa = rd_le32(cd + p + 38);
        e->local_hdr_offset = rd_le32(cd + p + 42);

        uint64_t rec_len = 46u + e->fname_len + e->extra_len + e->comment_len;
        if (p + rec_len > cd_size) break;

        e->fname_pool_off = pool_append(a, (const char *)(cd + p + 46u), e->fname_len);
        e->extra_pool_off = e->extra_len
            ? pool_append(a, (const char *)(cd + p + 46u + e->fname_len), e->extra_len)
            : 0u;
        e->comment_pool_off = e->comment_len
            ? pool_append(a, (const char *)(cd + p + 46u + e->fname_len + e->extra_len), e->comment_len)
            : 0u;

        /* ZIP64 extra field (0x0001) override */
        const uint8_t *ex = cd + p + 46u + e->fname_len;
        uint32_t ex_off = 0;
        while (ex_off + 4u <= e->extra_len) {
            uint16_t hid = rd_le16(ex + ex_off);
            uint16_t hsz = rd_le16(ex + ex_off + 2u);
            if (hid == 0x0001u && ex_off + 4u + hsz <= e->extra_len) {
                uint32_t f = 0;
                const uint8_t *d = ex + ex_off + 4u;
                if (e->uncomp_size == 0xFFFFFFFFu && f + 8u <= hsz) {
                    e->uncomp_size = rd_le64(d + f); f += 8u;
                }
                if (e->comp_size == 0xFFFFFFFFu && f + 8u <= hsz) {
                    e->comp_size = rd_le64(d + f); f += 8u;
                }
                if (e->local_hdr_offset == 0xFFFFFFFFu && f + 8u <= hsz) {
                    e->local_hdr_offset = rd_le64(d + f); f += 8u;
                }
                if (e->disk_start == 0xFFFFu && f + 4u <= hsz) {
                    e->disk_start = rd_le32(d + f);
                }
                break;
            }
            ex_off += 4u + hsz;
        }

        p += rec_len;
        parsed++;
    }

    free(cd);
    a->num_entries = parsed;
    a->cur_idx = 0;
    a->has_cur = (parsed > 0u);
    return true;
}

static int minizip_open_current(mz_archive_t *a, const char *password) {
    if (!a || !a->has_cur || a->cur_idx >= a->num_entries) return UNZ_PARAMERROR;
    close_current_entry_state(a);

    const zip_entry_t *e = &a->entries[a->cur_idx];
    if (e->method != 0u && e->method != 8u) return UNZ_PARAMERROR;
    if ((e->flag & 1u) != 0u && !password) return UNZ_PARAMERROR;

    uint8_t lfh[30];
    if (e->local_hdr_offset + 30u > a->file_size ||
        !fseek64(a->fp, e->local_hdr_offset) ||
        fread(lfh, 1, 30, a->fp) != 30 ||
        rd_le32(lfh) != 0x04034B50u) {
        return UNZ_BADZIPFILE;
    }

    uint16_t lfname_len = rd_le16(lfh + 26);
    uint16_t lextra_len = rd_le16(lfh + 28);
    uint64_t data_off = e->local_hdr_offset + 30u + lfname_len + lextra_len;
    if (data_off + e->comp_size > a->file_size || !fseek64(a->fp, data_off)) {
        return UNZ_BADZIPFILE;
    }

    a->method = e->method;
    a->comp_rem = e->comp_size;
    a->uncomp_pos = 0;
    a->uncomp_size = e->uncomp_size;
    a->expected_crc = e->crc32;
    a->running_crc = 0;
    a->encrypted = (e->flag & 1u) != 0u;
    a->stream_eof = (e->uncomp_size == 0u && e->comp_size == 0u);
    a->crc_checked = false;

    if (a->encrypted) {
        if (a->comp_rem < 12u) return UNZ_BADZIPFILE;
        uint8_t hdr12[12];
        if (fread(hdr12, 1, 12, a->fp) != 12) return UNZ_ERRNO;
        a->comp_rem -= 12u;
        zc_init_keys(&a->zc, password);
        zc_decrypt_buf(&a->zc, hdr12, 12u);

        uint8_t expected = (e->flag & 0x0008u)
            ? (uint8_t)((e->dos_date >> 8) & 0xFFu)
            : (uint8_t)((e->crc32 >> 24) & 0xFFu);
        if (hdr12[11] != expected) {
            return UNZ_BADZIPFILE;
        }
    }

    if (a->method == 8u) {
#if SE_MINIZIP_HAVE_ZLIB
        memset(&a->zs, 0, sizeof(a->zs));
        if (inflateInit2(&a->zs, -MAX_WBITS) != Z_OK) {
            return UNZ_INTERNALERROR;
        }
        a->zs_init = true;
#else
        memset(&a->inf, 0, sizeof(a->inf));
        inf_init(&a->inf.inf);
        a->inf_init = true;
#endif
    }

    a->entry_open = true;
    return UNZ_OK;
}

/* ========================================================================= */
/* Win64 HLE Handlers (#61..#101)                                            */
/* ========================================================================= */

static void hle_unzOpen(CPU *c) {
    const char *win_path = (const char *)(uintptr_t)HLE_ARG0(c);
    if (!win_path || !win_path[0]) {
        HLE_RET64(c, 0ull);
        return;
    }

    char host_path[1024];
    FILE *fp = NULL;
    if (se_vfs_resolve_path(win_path, false, host_path, sizeof(host_path))) {
        fp = fopen(host_path, "rb");
    }
    if (!fp) {
        fp = fopen(win_path, "rb");
    }
    if (!fp) {
        HLE_RET64(c, 0ull);
        return;
    }

    mz_archive_t *a = (mz_archive_t *)calloc(1u, sizeof(mz_archive_t));
    if (!a) {
        fclose(fp);
        HLE_RET64(c, 0ull);
        return;
    }
    a->fp = fp;
    a->file_size = ftell64_end(fp);
    if (!parse_zip_directory(a)) {
        free(a->str_pool);
        free(a->entries);
        fclose(fp);
        free(a);
        HLE_RET64(c, 0ull);
        return;
    }
    HLE_RET_PTR(c, a);
}

static void hle_unzClose(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    if (!a) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    close_current_entry_state(a);
    if (a->fp) fclose(a->fp);
    free(a->entries);
    free(a->str_pool);
    free(a);
    HLE_RET32(c, (uint32_t)UNZ_OK);
}

static void hle_unzGetGlobalInfo(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    win64_unz_global_info_t *gi = (win64_unz_global_info_t *)(uintptr_t)HLE_ARG1(c);
    if (!a || !gi) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    gi->number_entry = a->num_entries;
    gi->size_comment = a->comment_len;
    HLE_RET32(c, (uint32_t)UNZ_OK);
}

static void hle_unzGetCurrentFileInfo(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    win64_unz_file_info_t *fi = (win64_unz_file_info_t *)(uintptr_t)HLE_ARG1(c);
    char *szFileName = (char *)(uintptr_t)HLE_ARG2(c);
    uint32_t fileNameBufSize = (uint32_t)HLE_ARG3(c);
    void *extraField = (void *)(uintptr_t)HLE_ARG4(c);
    uint32_t extraBufSize = (uint32_t)HLE_ARG5(c);
    char *szComment = (char *)(uintptr_t)HLE_ARG6(c);
    uint32_t commentBufSize = (uint32_t)HLE_ARG7(c);

    if (!a || !a->has_cur || a->cur_idx >= a->num_entries) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    const zip_entry_t *e = &a->entries[a->cur_idx];

    if (fi) {
        memset(fi, 0, sizeof(*fi));
        fi->version = e->version;
        fi->version_needed = e->version_needed;
        fi->flag = e->flag;
        fi->compression_method = e->method;
        fi->dosDate = e->dos_date;
        fi->crc = e->crc32;
        fi->compressed_size = (e->comp_size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)e->comp_size;
        fi->uncompressed_size = (e->uncomp_size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)e->uncomp_size;
        fi->size_filename = e->fname_len;
        fi->size_file_extra = e->extra_len;
        fi->size_file_comment = e->comment_len;
        fi->disk_num_start = e->disk_start;
        fi->internal_fa = e->internal_fa;
        fi->external_fa = e->external_fa;
        dos_date_to_tm(e->dos_date, &fi->tmu_date);
    }

    if (szFileName && fileNameBufSize > 0u) {
        uint32_t copy_len = e->fname_len;
        if (copy_len > fileNameBufSize) copy_len = fileNameBufSize;
        if (copy_len > 0u) {
            memcpy(szFileName, a->str_pool + e->fname_pool_off, copy_len);
        }
        if (copy_len < fileNameBufSize) {
            szFileName[copy_len] = '\0';
        }
    }
    if (extraField && extraBufSize > 0u && e->extra_len > 0u) {
        uint32_t copy_len = (e->extra_len < extraBufSize) ? e->extra_len : extraBufSize;
        memcpy(extraField, a->str_pool + e->extra_pool_off, copy_len);
    }
    if (szComment && commentBufSize > 0u) {
        uint32_t copy_len = e->comment_len;
        if (copy_len > commentBufSize) copy_len = commentBufSize;
        if (copy_len > 0u) {
            memcpy(szComment, a->str_pool + e->comment_pool_off, copy_len);
        }
        if (copy_len < commentBufSize) {
            szComment[copy_len] = '\0';
        }
    }

    HLE_RET32(c, (uint32_t)UNZ_OK);
}

static void hle_unzGoToFirstFile(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    if (!a) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    if (a->num_entries == 0u) {
        a->has_cur = false;
        HLE_RET32(c, (uint32_t)UNZ_END_OF_LIST_OF_FILE);
        return;
    }
    a->cur_idx = 0u;
    a->has_cur = true;
    HLE_RET32(c, (uint32_t)UNZ_OK);
}

static void hle_unzGoToNextFile(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    if (!a || !a->has_cur) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    if (a->cur_idx + 1u >= a->num_entries) {
        HLE_RET32(c, (uint32_t)UNZ_END_OF_LIST_OF_FILE);
        return;
    }
    a->cur_idx++;
    HLE_RET32(c, (uint32_t)UNZ_OK);
}

static void hle_unzOpenCurrentFile(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)minizip_open_current(a, NULL));
}

static void hle_unzOpenCurrentFilePassword(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    const char *pw = (const char *)(uintptr_t)HLE_ARG1(c);
    HLE_RET32(c, (uint32_t)minizip_open_current(a, pw));
}

static void hle_unzReadCurrentFile(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    uint8_t *out_buf = (uint8_t *)(uintptr_t)HLE_ARG1(c);
    uint32_t len = (uint32_t)HLE_ARG2(c);

    if (!a || !a->entry_open || (!out_buf && len > 0u)) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    if (len == 0u || a->stream_eof || a->uncomp_pos >= a->uncomp_size) {
        HLE_RET32(c, 0u);
        return;
    }

    uint64_t rem_uncomp = a->uncomp_size - a->uncomp_pos;
    uint32_t want = (rem_uncomp < (uint64_t)len) ? (uint32_t)rem_uncomp : len;
    uint32_t copied = 0u;

    if (a->method == 0u) {
        while (copied < want && a->comp_rem > 0u) {
            uint32_t chunk = want - copied;
            if ((uint64_t)chunk > a->comp_rem) chunk = (uint32_t)a->comp_rem;
            size_t got = fread(out_buf + copied, 1, chunk, a->fp);
            if (got == 0u) {
                HLE_RET32(c, (uint32_t)UNZ_ERRNO);
                return;
            }
            if (a->encrypted) zc_decrypt_buf(&a->zc, out_buf + copied, got);
            a->running_crc = se_crc32(a->running_crc, out_buf + copied, got);
            copied += (uint32_t)got;
            a->comp_rem -= got;
            a->uncomp_pos += got;
        }
        if (a->uncomp_pos >= a->uncomp_size) a->stream_eof = true;
    } else {
        uint8_t *out_cur = out_buf;
        uint8_t *out_end = out_buf + want;
        while (out_cur < out_end && !a->stream_eof) {
            if (a->in_pos >= a->in_len) {
                if (a->comp_rem > 0u) {
                    uint32_t to_read = (a->comp_rem < MZ_READ_CHUNK) ? (uint32_t)a->comp_rem : MZ_READ_CHUNK;
                    size_t got = fread(a->in_buf, 1, to_read, a->fp);
                    if (got == 0u) {
                        HLE_RET32(c, (uint32_t)UNZ_ERRNO);
                        return;
                    }
                    if (a->encrypted) zc_decrypt_buf(&a->zc, a->in_buf, got);
                    a->comp_rem -= got;
                    a->in_pos = 0;
                    a->in_len = (uint32_t)got;
                }
            }

#if SE_MINIZIP_HAVE_ZLIB
            a->zs.next_in = a->in_buf + a->in_pos;
            a->zs.avail_in = a->in_len - a->in_pos;
            a->zs.next_out = out_cur;
            a->zs.avail_out = (uInt)(out_end - out_cur);
            if (a->zs.avail_in == 0 && a->comp_rem == 0u) {
                break;
            }
            int zrc = inflate(&a->zs, Z_NO_FLUSH);
            uint32_t consumed = (uint32_t)(a->zs.next_in - (a->in_buf + a->in_pos));
            uint32_t produced = (uint32_t)(a->zs.next_out - out_cur);
            a->in_pos += consumed;
            if (produced > 0u) {
                a->running_crc = se_crc32(a->running_crc, out_cur, produced);
                a->uncomp_pos += produced;
                out_cur += produced;
                copied += produced;
            }
            if (zrc == Z_STREAM_END) {
                a->stream_eof = true;
                break;
            }
            if (zrc != Z_OK && zrc != Z_BUF_ERROR) {
                HLE_RET32(c, (uint32_t)UNZ_BADZIPFILE);
                return;
            }
            if (consumed == 0u && produced == 0u && a->comp_rem == 0u) {
                break;
            }
#else
            if (a->in_pos >= a->in_len && a->comp_rem == 0u && a->inf.inf.state != INF_COPY_MATCH) {
                break;
            }
            const uint8_t *in_p = a->in_buf + a->in_pos;
            const uint8_t *in_end = a->in_buf + a->in_len;
            uint8_t *before_out = out_cur;
            int irc = inf_step(&a->inf, &in_p, in_end, &out_cur, out_end);
            uint32_t consumed = (uint32_t)(in_p - (a->in_buf + a->in_pos));
            uint32_t produced = (uint32_t)(out_cur - before_out);
            a->in_pos += consumed;
            if (produced > 0u) {
                a->running_crc = se_crc32(a->running_crc, before_out, produced);
                a->uncomp_pos += produced;
                copied += produced;
            }
            if (irc == 1) {
                a->stream_eof = true;
                break;
            }
            if (irc < 0) {
                HLE_RET32(c, (uint32_t)UNZ_BADZIPFILE);
                return;
            }
            if (consumed == 0u && produced == 0u && a->in_pos >= a->in_len && a->comp_rem == 0u) {
                break;
            }
#endif
        }
    }

    if (a->uncomp_pos >= a->uncomp_size && !a->crc_checked) {
        a->crc_checked = true;
        if (a->running_crc != a->expected_crc) {
            HLE_RET32(c, (uint32_t)UNZ_CRCERROR);
            return;
        }
    }

    HLE_RET32(c, copied);
}

static void hle_unztell(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    if (!a || !a->entry_open) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    HLE_RET32(c, (uint32_t)a->uncomp_pos);
}

static void hle_unzCloseCurrentFile(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    if (!a) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    int err = UNZ_OK;
    if (a->entry_open && a->uncomp_pos == a->uncomp_size && a->running_crc != a->expected_crc) {
        err = UNZ_CRCERROR;
    }
    close_current_entry_state(a);
    HLE_RET32(c, (uint32_t)err);
}

static void hle_unzGetFilePos(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    win64_unz_file_pos_t *fp = (win64_unz_file_pos_t *)(uintptr_t)HLE_ARG1(c);
    if (!a || !a->has_cur || !fp || a->cur_idx >= a->num_entries) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    fp->pos_in_zip_directory = a->entries[a->cur_idx].cd_offset;
    fp->num_of_file = a->entries[a->cur_idx].num_of_file;
    HLE_RET32(c, (uint32_t)UNZ_OK);
}

static void hle_unzGoToFilePos(CPU *c) {
    mz_archive_t *a = (mz_archive_t *)(uintptr_t)HLE_ARG0(c);
    const win64_unz_file_pos_t *fp = (const win64_unz_file_pos_t *)(uintptr_t)HLE_ARG1(c);
    if (!a || !fp || fp->num_of_file >= a->num_entries) {
        HLE_RET32(c, (uint32_t)UNZ_PARAMERROR);
        return;
    }
    a->cur_idx = fp->num_of_file;
    a->has_cur = true;
    HLE_RET32(c, (uint32_t)UNZ_OK);
}

void se_register_minizip_shims(void) {
    se_hle_register_import("zlibwapi.dll", "#61",  hle_unzOpen);
    se_hle_register_import("zlibwapi.dll", "#62",  hle_unzClose);
    se_hle_register_import("zlibwapi.dll", "#63",  hle_unzGetGlobalInfo);
    se_hle_register_import("zlibwapi.dll", "#64",  hle_unzGetCurrentFileInfo);
    se_hle_register_import("zlibwapi.dll", "#65",  hle_unzGoToFirstFile);
    se_hle_register_import("zlibwapi.dll", "#66",  hle_unzGoToNextFile);
    se_hle_register_import("zlibwapi.dll", "#67",  hle_unzOpenCurrentFile);
    se_hle_register_import("zlibwapi.dll", "#68",  hle_unzReadCurrentFile);
    se_hle_register_import("zlibwapi.dll", "#70",  hle_unztell);
    se_hle_register_import("zlibwapi.dll", "#72",  hle_unzCloseCurrentFile);
    se_hle_register_import("zlibwapi.dll", "#79",  hle_unzOpenCurrentFilePassword);
    se_hle_register_import("zlibwapi.dll", "#100", hle_unzGetFilePos);
    se_hle_register_import("zlibwapi.dll", "#101", hle_unzGoToFilePos);
}
