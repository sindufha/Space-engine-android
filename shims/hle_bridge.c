/*
 * hle_bridge.c - HLE sentinel dispatch table, UTF-16LE/VFS helpers, and guest call bridge.
 */
#include "hle_bridge.h"
#include "pe_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define HLE_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineHLE", fmt, ##__VA_ARGS__)
#else
#define HLE_LOG(fmt, ...) ((void)0)
#endif

typedef struct {
    char dll[64];
    char name[128];
    se_hle_fn_t fn;
} HleSlot;

static HleSlot g_slots[SE_HLE_TOTAL_SLOTS];
static uint32_t g_dyn_count = HLE_COUNT;
static pthread_mutex_t g_hle_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_hle_initialized = 0;
static int g_android_hooks_enabled = 0;

static char g_game_dir[1024] = ".";
static char g_internal_dir[1024] = ".";
static char g_exe_path[1024] = "system/SpaceEngine.exe";

static se_hle_stage_cb_t g_stage_cb = NULL;
static void *g_stage_user = NULL;
static int g_auto_planetarium_enabled = 0;
static int g_planetarium_entered = 0;
static int g_last_polled_stage = -1;

#if defined(_MSC_VER) && !defined(__clang__)
static __declspec(thread) int tl_suppress_ret_pop = 0;
static __declspec(thread) uint32_t tl_active_slot_idx = 0;
static __declspec(thread) int tl_in_mainframe_hook = 0;
#else
static _Thread_local int tl_suppress_ret_pop = 0;
static _Thread_local uint32_t tl_active_slot_idx = 0;
static _Thread_local int tl_in_mainframe_hook = 0;
#endif

static int strcasecmp_ascii(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
        ++a;
        ++b;
    }
}

static void hle_unimplemented_stub(CPU *c) {
    (void)c;
    HLE_RET64(c, 0);
}

static void init_table_locked(void) {
    if (g_hle_initialized) return;
    memset(g_slots, 0, sizeof(g_slots));
    for (uint32_t i = 0; i < HLE_COUNT; ++i) {
        snprintf(g_slots[i].dll, sizeof(g_slots[i].dll), "%s", recomp_imports[i].dll);
        snprintf(g_slots[i].name, sizeof(g_slots[i].name), "%s", recomp_imports[i].name);
        g_slots[i].fn = hle_unimplemented_stub;
    }
    g_dyn_count = HLE_COUNT;
#if defined(SE_RECOMP_FULL)
    extern void se_init_dispatch_hash_table(void);
    se_init_dispatch_hash_table();
#endif
    g_hle_initialized = 1;
}

void se_hle_init(void) {
    pthread_mutex_lock(&g_hle_lock);
    int need_reg = !g_hle_initialized;
    if (need_reg) {
        init_table_locked();
    }
    pthread_mutex_unlock(&g_hle_lock);

    if (need_reg) {
        se_u32_reset_state();
        se_wgl_reset_state();
        se_register_png_tiff_shims();
        se_register_sdl2_mixer_shims();
        se_register_misc_win32_shims();
        se_register_kernel32_shims();
        se_register_minizip_shims();
        se_register_user32_gdi32_shims();
        se_register_wgl_egl_shims();
    }
}

void se_hle_set_paths(const char *game_dir, const char *internal_dir, const char *exe_path) {
    pthread_mutex_lock(&g_hle_lock);
    if (game_dir && game_dir[0]) snprintf(g_game_dir, sizeof(g_game_dir), "%s", game_dir);
    if (internal_dir && internal_dir[0]) snprintf(g_internal_dir, sizeof(g_internal_dir), "%s", internal_dir);
    if (exe_path && exe_path[0]) snprintf(g_exe_path, sizeof(g_exe_path), "%s", exe_path);
    pthread_mutex_unlock(&g_hle_lock);
}

const char *se_hle_get_game_dir(void) { return g_game_dir; }
const char *se_hle_get_internal_dir(void) { return g_internal_dir; }
const char *se_hle_get_exe_path(void) { return g_exe_path; }

void se_hle_set_android_hooks(int enabled) {
    g_android_hooks_enabled = enabled;
}

void se_hle_register_import(const char *dll, const char *name, se_hle_fn_t fn) {
    if (!dll || !name || !fn) return;
    pthread_mutex_lock(&g_hle_lock);
    init_table_locked();
    for (uint32_t i = 0; i < HLE_COUNT; ++i) {
        if (strcasecmp_ascii(g_slots[i].dll, dll) == 0 && strcmp(g_slots[i].name, name) == 0) {
            g_slots[i].fn = fn;
            pthread_mutex_unlock(&g_hle_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_hle_lock);
    (void)se_hle_register_dynamic(dll, name, fn);
}

uint64_t se_hle_find_sentinel(const char *dll, const char *name) {
    if (!name) return 0;
    pthread_mutex_lock(&g_hle_lock);
    init_table_locked();
    for (uint32_t i = 0; i < g_dyn_count; ++i) {
        if ((!dll || strcasecmp_ascii(g_slots[i].dll, dll) == 0) &&
            strcmp(g_slots[i].name, name) == 0) {
            uint64_t s = HLE_SENTINEL(i);
            pthread_mutex_unlock(&g_hle_lock);
            return s;
        }
    }
    pthread_mutex_unlock(&g_hle_lock);
    return 0;
}

uint32_t se_hle_unregistered_count(void) {
    pthread_mutex_lock(&g_hle_lock);
    init_table_locked();
    uint32_t missing = 0;
    for (uint32_t i = 0; i < HLE_COUNT; ++i) {
        if (g_slots[i].fn == hle_unimplemented_stub || g_slots[i].fn == NULL) {
            missing++;
        }
    }
    pthread_mutex_unlock(&g_hle_lock);
    return missing;
}

uint64_t se_hle_register_dynamic(const char *dll, const char *name, se_hle_fn_t fn) {
    if (!name || !fn) return 0;
    const char *d = (dll && dll[0]) ? dll : "DYNAMIC";
    pthread_mutex_lock(&g_hle_lock);
    init_table_locked();
    for (uint32_t i = 0; i < g_dyn_count; ++i) {
        if (strcasecmp_ascii(g_slots[i].dll, d) == 0 && strcmp(g_slots[i].name, name) == 0) {
            g_slots[i].fn = fn;
            uint64_t s = HLE_SENTINEL(i);
            pthread_mutex_unlock(&g_hle_lock);
            return s;
        }
    }
    if (g_dyn_count >= SE_HLE_TOTAL_SLOTS) {
        pthread_mutex_unlock(&g_hle_lock);
        return 0;
    }
    uint32_t idx = g_dyn_count++;
    snprintf(g_slots[idx].dll, sizeof(g_slots[idx].dll), "%s", d);
    snprintf(g_slots[idx].name, sizeof(g_slots[idx].name), "%s", name);
    g_slots[idx].fn = fn;
    uint64_t s = HLE_SENTINEL(idx);
    pthread_mutex_unlock(&g_hle_lock);
    return s;
}

void se_hle_suppress_ret_pop_once(void) {
    tl_suppress_ret_pop = 1;
}

uint32_t se_hle_active_slot_index(void) {
    return tl_active_slot_idx;
}

void se_hle_set_stage_callback(se_hle_stage_cb_t cb, void *user) {
    pthread_mutex_lock(&g_hle_lock);
    g_stage_cb = cb;
    g_stage_user = user;
    pthread_mutex_unlock(&g_hle_lock);
}

void se_hle_set_auto_planetarium(bool enabled) {
    pthread_mutex_lock(&g_hle_lock);
    g_auto_planetarium_enabled = enabled ? 1 : 0;
    pthread_mutex_unlock(&g_hle_lock);
}

void se_hle_reset_boot_state(void) {
    pthread_mutex_lock(&g_hle_lock);
    g_planetarium_entered = 0;
    g_last_polled_stage = -1;
    pthread_mutex_unlock(&g_hle_lock);
    tl_in_mainframe_hook = 0;
}

void se_hle_poll_stage(void) {
    uint64_t base = se_pe_active_base();
    uint32_t sz = se_pe_active_size();
    if (!base || sz < 0x780000u) return;

    uint32_t stage = rd32(GVA(0x14077B250ull));
    if (stage == 0u || stage > 7u) return;

    se_hle_stage_cb_t cb = NULL;
    void *user = NULL;
    int emit_stage = -1;

    pthread_mutex_lock(&g_hle_lock);
    if ((int)stage != g_last_polled_stage) {
        g_last_polled_stage = (int)stage;
        emit_stage = (int)stage;
        cb = g_stage_cb;
        user = g_stage_user;
    }
    pthread_mutex_unlock(&g_hle_lock);

    if (emit_stage == 7) {
        se_wgl_set_planetarium_loaded(true);
    }
    if (cb && emit_stage >= 1) {
        char msg[64];
        if (emit_stage <= 6) {
            snprintf(msg, sizeof(msg), "Loading Planetarium (%d/6)...", emit_stage);
        } else {
            snprintf(msg, sizeof(msg), "Entering live planetarium...");
        }
        cb(emit_stage, msg, user);
    }
}

bool se_hle_invoke_sentinel(CPU *c, uint64_t target) {
    if (target < HLE_BASE) return false;
    uint64_t off = target - HLE_BASE;
    if ((off % HLE_STRIDE) != 0) return false;
    uint64_t idx = off / HLE_STRIDE;
    if (idx >= SE_HLE_TOTAL_SLOTS) return false;

    if (!g_hle_initialized) {
        se_hle_init();
    }

    se_hle_fn_t fn = g_slots[idx].fn;
    if (!fn) {
        HLE_LOG("Unregistered HLE sentinel #%llu (%s!%s)",
                (unsigned long long)idx, g_slots[idx].dll, g_slots[idx].name);
        HLE_RET64(c, 0);
        c->rsp += 8;
        return true;
    }

    tl_suppress_ret_pop = 0;
    tl_active_slot_idx = (uint32_t)idx;
    fn(c);
    if (tl_suppress_ret_pop) {
        tl_suppress_ret_pop = 0;
    } else {
        c->rsp += 8;
    }
    return true;
}

#if defined(SE_RECOMP_FULL)
#define SE_DISPATCH_HASH_BITS 16u
#define SE_DISPATCH_HASH_SIZE (1u << SE_DISPATCH_HASH_BITS)
#define SE_DISPATCH_HASH_MASK (SE_DISPATCH_HASH_SIZE - 1u)

static recomp_entry_t g_dispatch_hash[SE_DISPATCH_HASH_SIZE];
static uint32_t g_dispatch_hash_ready = 0u;

static inline uint32_t se_hash_va(uint64_t va) {
    uint64_t x = va;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x = x ^ (x >> 31);
    return (uint32_t)x & SE_DISPATCH_HASH_MASK;
}

void se_init_dispatch_hash_table(void) {
    if (__atomic_load_n(&g_dispatch_hash_ready, __ATOMIC_ACQUIRE) != 0u) {
        return;
    }
    if (recomp_dispatch_count >= (SE_DISPATCH_HASH_SIZE * 3u) / 4u) {
        return;
    }
    memset(g_dispatch_hash, 0, sizeof(g_dispatch_hash));
    for (uint32_t i = 0u; i < recomp_dispatch_count; ++i) {
        uint64_t va = recomp_dispatch_table[i].va;
        if (!va) continue;
        uint32_t slot = se_hash_va(va);
        while (g_dispatch_hash[slot].va != 0ull) {
            slot = (slot + 1u) & SE_DISPATCH_HASH_MASK;
        }
        g_dispatch_hash[slot] = recomp_dispatch_table[i];
    }
    __atomic_store_n(&g_dispatch_hash_ready, 1u, __ATOMIC_RELEASE);
}

static inline const recomp_entry_t *se_find_recomp_entry(uint64_t canonical_va) {
    if (__builtin_expect(__atomic_load_n(&g_dispatch_hash_ready, __ATOMIC_ACQUIRE) != 0u, 1)) {
        uint32_t slot = se_hash_va(canonical_va);
        for (;;) {
            const recomp_entry_t *e = &g_dispatch_hash[slot];
            if (__builtin_expect(e->va == canonical_va, 1)) {
                return e;
            }
            if (e->va == 0ull) {
                return NULL;
            }
            slot = (slot + 1u) & SE_DISPATCH_HASH_MASK;
        }
    }
    uint32_t lo = 0u;
    uint32_t hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1u);
        uint64_t va = recomp_dispatch_table[mid].va;
        if (canonical_va < va) {
            hi = mid;
        } else if (canonical_va > va) {
            lo = mid + 1u;
        } else {
            return &recomp_dispatch_table[mid];
        }
    }
    return NULL;
}

void dispatch(CPU *c, uint64_t target) {
    if (!c) {
        abort();
    }
    if (se_hle_invoke_sentinel(c, target)) {
        return;
    }
    uint64_t canonical = se_hle_normalize_target(target);
    if (se_hle_try_intercept(c, canonical)) {
        return;
    }
    const recomp_entry_t *entry = se_find_recomp_entry(canonical);
    if (entry && entry->fn) {
        entry->fn(c);
        return;
    }
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_FATAL, "SpaceEngineRecomp",
                        "[dispatch] unresolved target=0x%llx canonical=0x%llx rip=0x%llx",
                        (unsigned long long)target,
                        (unsigned long long)canonical,
                        (unsigned long long)c->rip);
#else
    fprintf(stderr,
            "[dispatch] unresolved target=0x%llx canonical=0x%llx rip=0x%llx\n",
            (unsigned long long)target,
            (unsigned long long)canonical,
            (unsigned long long)c->rip);
#endif
    abort();
}

void dispatch_jmp(CPU *c, uint64_t target) {
    dispatch(c, target);
}
#endif /* SE_RECOMP_FULL */

uint64_t se_hle_normalize_target(uint64_t target) {
    uint64_t base = se_pe_active_base();
    uint32_t size = se_pe_active_size();
    if (base && size && target >= base && target < base + (uint64_t)size) {
        return SE_EXPECTED_IMAGE_BASE + (target - base);
    }
    return target;
}

bool se_hle_try_intercept(CPU *c, uint64_t canonical_target) {
    if (se_crt_try_intercept(c, canonical_target)) {
        return true;
    }
    if (g_android_hooks_enabled) {
        if (canonical_target == 0x140577AB0ull) {
            /* Phase 4 hook: MainFrame tick -> poll stages 1..7 and auto-enter planetarium. */
            se_hle_poll_stage();
            uint64_t base = se_pe_active_base();
            uint32_t sz = se_pe_active_size();
            if (g_auto_planetarium_enabled && !g_planetarium_entered && !tl_in_mainframe_hook &&
                base != 0ull && sz >= 0x806000u) {
                uint64_t mgr = rd64(GVA(0x140778C00ull));
                uint32_t stage = rd32(GVA(0x14077B250ull));
                if (mgr >= 0x10000ull && rd32(mgr) == 0u && stage <= 1u) {
                    g_planetarium_entered = 1;
                    tl_in_mainframe_hook = 1;
                    uint32_t fbits = rd32(GVA(0x1406CCDFCull));
                    if (fbits == 0u) fbits = 0xBF800000u; /* -1.0f */
                    c->xmm[3].u32[0] = fbits;
                    HLE_LOG("Main menu ready -> auto-triggering planetarium entry (0x140222670)");
                    (void)se_hle_call_guest(c, 0x140222670ull, 4,
                                            GVA(0x140804D80ull), 1ull, 1ull, 0ull);
                    tl_in_mainframe_hook = 0;
                    se_hle_poll_stage();
                }
            }
            return false; /* Always proceed into real MainFrame */
        }
        if (canonical_target == 0x14057A620ull) {
            /* Loading planetarium (6): Loading complete hook */
            se_hle_poll_stage();
            return false;
        }
    }
    return false;
}

uint64_t se_hle_call_guest(CPU *c, uint64_t fn_va, int argc,
                           uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!c || !fn_va) return 0;
    CPU saved = *c;
    uint64_t sp = c->rsp & ~(uint64_t)15ull;
    sp -= 0x20ull; /* shadow space for 4 register args */
    c->rsp = sp;
    if (argc > 0) c->rcx = a0;
    if (argc > 1) c->rdx = a1;
    if (argc > 2) c->r8 = a2;
    if (argc > 3) c->r9 = a3;
    push64(c, 0ull);
    dispatch(c, fn_va);
    uint64_t ret = c->rax;
    *c = saved;
    return ret;
}

/* ---- UTF-16LE <-> UTF-8 conversions --------------------------------------- */

size_t se_utf16_len(const uint16_t *wstr) {
    if (!wstr) return 0;
    size_t n = 0;
    while (wstr[n] != 0) ++n;
    return n;
}

int se_utf16_to_utf8(const uint16_t *wstr, int wlen, char *out_utf8, int out_cap) {
    if (!wstr) return 0;
    size_t slen = (wlen < 0) ? se_utf16_len(wstr) : (size_t)wlen;
    size_t pos = 0;
    for (size_t i = 0; i < slen; ++i) {
        uint32_t cp = wstr[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && (i + 1) < slen) {
            uint32_t lo = wstr[i + 1];
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                ++i;
            }
        }
        uint8_t tmp[4];
        int nbytes = 0;
        if (cp < 0x80u) {
            tmp[0] = (uint8_t)cp;
            nbytes = 1;
        } else if (cp < 0x800u) {
            tmp[0] = (uint8_t)(0xC0u | (cp >> 6));
            tmp[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
            nbytes = 2;
        } else if (cp < 0x10000u) {
            tmp[0] = (uint8_t)(0xE0u | (cp >> 12));
            tmp[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
            tmp[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
            nbytes = 3;
        } else {
            tmp[0] = (uint8_t)(0xF0u | (cp >> 18));
            tmp[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
            tmp[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
            tmp[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
            nbytes = 4;
        }
        if (out_utf8 && out_cap > 0) {
            if ((int)(pos + (size_t)nbytes) >= out_cap) break;
            memcpy(out_utf8 + pos, tmp, (size_t)nbytes);
        }
        pos += (size_t)nbytes;
    }
    if (out_utf8 && out_cap > 0) {
        size_t nul_pos = (pos < (size_t)out_cap) ? pos : (size_t)(out_cap - 1);
        out_utf8[nul_pos] = '\0';
    }
    return (int)(pos + ((wlen < 0) ? 1u : 0u));
}

int se_utf8_to_utf16(const char *utf8, int utf8_len, uint16_t *out_w, int out_wcap) {
    if (!utf8) return 0;
    size_t slen = (utf8_len < 0) ? strlen(utf8) : (size_t)utf8_len;
    size_t i = 0;
    size_t wpos = 0;
    while (i < slen) {
        uint8_t b0 = (uint8_t)utf8[i++];
        uint32_t cp = b0;
        if (b0 < 0x80u) {
            cp = b0;
        } else if ((b0 & 0xE0u) == 0xC0u && i < slen) {
            uint8_t b1 = (uint8_t)utf8[i++];
            cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
        } else if ((b0 & 0xF0u) == 0xE0u && (i + 1) < slen) {
            uint8_t b1 = (uint8_t)utf8[i++];
            uint8_t b2 = (uint8_t)utf8[i++];
            cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(b1 & 0x3Fu) << 6) | (uint32_t)(b2 & 0x3Fu);
        } else if ((b0 & 0xF8u) == 0xF0u && (i + 2) < slen) {
            uint8_t b1 = (uint8_t)utf8[i++];
            uint8_t b2 = (uint8_t)utf8[i++];
            uint8_t b3 = (uint8_t)utf8[i++];
            cp = ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(b1 & 0x3Fu) << 12) |
                 ((uint32_t)(b2 & 0x3Fu) << 6) | (uint32_t)(b3 & 0x3Fu);
        }
        if (cp <= 0xFFFFu) {
            if (out_w && (int)wpos + 1 < out_wcap) out_w[wpos] = (uint16_t)cp;
            ++wpos;
        } else {
            cp -= 0x10000u;
            if (out_w && (int)wpos + 2 < out_wcap) {
                out_w[wpos] = (uint16_t)(0xD800u + (cp >> 10));
                out_w[wpos + 1] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
            }
            wpos += 2;
        }
    }
    if (out_w && out_wcap > 0) {
        size_t nul_pos = (wpos < (size_t)out_wcap) ? wpos : (size_t)(out_wcap - 1);
        out_w[nul_pos] = 0;
    }
    return (int)(wpos + ((utf8_len < 0) ? 1u : 0u));
}

/* ---- VFS & case-insensitive path resolution ------------------------------ */

static bool is_writable_subpath(const char *rel) {
    if (!rel) return false;
    while (*rel == '/') ++rel;
    static const char *const kWritablePrefixes[] = {
        "config/", "cache/", "export/", "screenshots/", "se.log", "shader.log"
    };
    for (size_t i = 0; i < sizeof(kWritablePrefixes) / sizeof(kWritablePrefixes[0]); ++i) {
        size_t len = strlen(kWritablePrefixes[i]);
        if (strncasecmp(rel, kWritablePrefixes[i], len) == 0 ||
            strcasecmp_ascii(rel, kWritablePrefixes[i]) == 0) {
            return true;
        }
    }
    if (strcasecmp_ascii(rel, "config") == 0 || strcasecmp_ascii(rel, "cache") == 0 ||
        strcasecmp_ascii(rel, "export") == 0 || strcasecmp_ascii(rel, "screenshots") == 0) {
        return true;
    }
    return false;
}

static void resolve_case_insensitive(const char *root, const char *rel, char *out, size_t cap) {
    char candidate[1024];
    snprintf(candidate, sizeof(candidate), "%s/%s", root, rel);
    struct stat st;
    if (stat(candidate, &st) == 0) {
        snprintf(out, cap, "%s", candidate);
        return;
    }

    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", root);
    char rel_copy[1024];
    snprintf(rel_copy, sizeof(rel_copy), "%s", rel);

    char *p = rel_copy;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        char *slash = strchr(p, '/');
        if (slash) *slash = '\0';

        char next[1024];
        size_t cur_len = strlen(cur);
        size_t p_len = strlen(p);
        if (cur_len + 1u + p_len >= sizeof(next)) break;
        memcpy(next, cur, cur_len);
        next[cur_len] = '/';
        memcpy(next + cur_len + 1u, p, p_len + 1u);

        if (stat(next, &st) == 0) {
            memcpy(cur, next, strlen(next) + 1u);
        } else {
            DIR *d = opendir(cur);
            bool matched = false;
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (strcasecmp_ascii(ent->d_name, p) == 0) {
                        size_t dlen = strlen(ent->d_name);
                        if (cur_len + 1u + dlen < sizeof(next)) {
                            memcpy(next, cur, cur_len);
                            next[cur_len] = '/';
                            memcpy(next + cur_len + 1u, ent->d_name, dlen + 1u);
                            memcpy(cur, next, cur_len + 1u + dlen + 1u);
                            matched = true;
                        }
                        break;
                    }
                }
                closedir(d);
            }
            if (!matched) {
                memcpy(cur, next, strlen(next) + 1u);
            }
        }
        if (!slash) break;
        p = slash + 1;
    }
    snprintf(out, cap, "%s", cur);
}

bool se_vfs_resolve_path(const char *win_path, bool for_write, char *out_host_path, size_t out_cap) {
    if (!win_path || !out_host_path || out_cap == 0) return false;
    const char *src = win_path;
    if (strncmp(src, "\\\\?\\", 4) == 0) src += 4;
    if (isalpha((unsigned char)src[0]) && src[1] == ':') src += 2;

    char norm[1024];
    size_t n = 0;
    for (; src[n] && n + 1 < sizeof(norm); ++n) {
        norm[n] = (src[n] == '\\') ? '/' : src[n];
    }
    norm[n] = '\0';

    /* Strip leading "./" or "/" if relative to engine root. */
    const char *rel = norm;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;

    /* Already rooted in known host dir? Pass through with separator normalization. */
    size_t g_len = strlen(g_game_dir);
    size_t i_len = strlen(g_internal_dir);
    if ((g_len > 1 && strncmp(rel, g_game_dir, g_len) == 0) ||
        (i_len > 1 && strncmp(rel, g_internal_dir, i_len) == 0)) {
        snprintf(out_host_path, out_cap, "%s", rel);
        return true;
    }

    while (*rel == '/') ++rel;
    if (!rel[0]) {
        snprintf(out_host_path, out_cap, "%s", g_game_dir);
        return true;
    }

    bool internal_only =
        strcasecmp_ascii(rel, "cache/filesys64.dat") == 0 ||
        strcasecmp_ascii(rel, "filesys64.dat") == 0;
    bool prefer_internal = (for_write || internal_only || is_writable_subpath(rel)) &&
                           strcmp(g_internal_dir, g_game_dir) != 0;
    if (prefer_internal) {
        char internal_path[1024];
        resolve_case_insensitive(g_internal_dir, rel, internal_path, sizeof(internal_path));
        struct stat st;
        if (for_write || internal_only || stat(internal_path, &st) == 0) {
            snprintf(out_host_path, out_cap, "%s", internal_path);
            return true;
        }
    }

    resolve_case_insensitive(g_game_dir, rel, out_host_path, out_cap);
    return true;
}

bool se_vfs_resolve_wpath(const uint16_t *win_wpath, bool for_write, char *out_host_path, size_t out_cap) {
    char utf8[1024];
    if (!win_wpath) return false;
    se_utf16_to_utf8(win_wpath, -1, utf8, (int)sizeof(utf8));
    return se_vfs_resolve_path(utf8, for_write, out_host_path, out_cap);
}

/* ---- Guest-addressable interned string storage --------------------------- */

#define SE_INTERN_ARENA_SIZE (256u * 1024u)
static uint8_t g_intern_arena[SE_INTERN_ARENA_SIZE];
static size_t  g_intern_used = 0;

uint64_t se_hle_intern_ascii(const char *s) {
    if (!s) return 0;
    size_t len = strlen(s) + 1u;
    pthread_mutex_lock(&g_hle_lock);
    size_t aligned = (g_intern_used + 7u) & ~7u;
    if (aligned + len > SE_INTERN_ARENA_SIZE) {
        pthread_mutex_unlock(&g_hle_lock);
        return 0;
    }
    uint8_t *dst = g_intern_arena + aligned;
    memcpy(dst, s, len);
    g_intern_used = aligned + len;
    pthread_mutex_unlock(&g_hle_lock);
    return (uint64_t)(uintptr_t)dst;
}

uint64_t se_hle_intern_utf16(const char *s) {
    if (!s) return 0;
    int need = se_utf8_to_utf16(s, -1, NULL, 0);
    if (need <= 0) need = 1;
    size_t bytes = (size_t)need * 2u;
    pthread_mutex_lock(&g_hle_lock);
    size_t aligned = (g_intern_used + 7u) & ~7u;
    if (aligned + bytes > SE_INTERN_ARENA_SIZE) {
        pthread_mutex_unlock(&g_hle_lock);
        return 0;
    }
    uint16_t *dst = (uint16_t *)(g_intern_arena + aligned);
    se_utf8_to_utf16(s, -1, dst, need);
    g_intern_used = aligned + bytes;
    pthread_mutex_unlock(&g_hle_lock);
    return (uint64_t)(uintptr_t)dst;
}
