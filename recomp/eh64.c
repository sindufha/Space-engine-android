/*
 * eh64.c - Guest Win64 virtual unwind, C++ EH (FH3 + FH4), and simulated TEB.
 *
 * Adapted from pcrecomp runtime/recomp64_cpu/eh64.c for SpaceEngine Android
 * (arm64-v8a NDK Clang -std=c11) and Win64/POSIX host tests.
 */

#include "cpu64.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if !defined(_WIN32)
#include <pthread.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define SE_TLS __declspec(thread)
#else
#define SE_TLS _Thread_local
#endif

uint64_t es3_eh_image_base = 0;
uint32_t es3_eh_image_size = 0x01abd000u;
int      g_eh_trace = 0;

extern void dispatch(CPU *c, uint64_t target);

/* ---- Shadow call depth & EH frame stack (C11 per-thread) ----------------- */

static SE_TLS int tl_cs_depth = 0;
static SE_TLS es3_ehframe_t *eh_top = NULL;

int es3_cs_depth_get(void) { return tl_cs_depth; }
void es3_cs_depth_set(int d) { tl_cs_depth = d; }

void es3_eh_push(es3_ehframe_t *f, uint64_t func, jmp_buf *jb)
{
    f->prev = eh_top;
    f->jb = jb;
    f->func = func;
    f->resume = 0;
    f->cs_depth = tl_cs_depth;
    eh_top = f;
}

void es3_eh_pop(es3_ehframe_t *f)
{
    eh_top = f->prev;
}

/* ---- RECOMP_TODO trap ----------------------------------------------------- */

void se_recomp_todo_trap(uint64_t va, const char *text)
{
    const char *msg = text ? text : "(unknown)";
    fprintf(stderr, "[RECOMP_TODO] unimplemented guest instruction at %#llx: %s\n",
            (unsigned long long)va, msg);
    fflush(stderr);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_FATAL, "SpaceEngineRecomp",
                        "[RECOMP_TODO] at %#llx: %s",
                        (unsigned long long)va, msg);
#endif
    abort();
}

/* ---- Simulated Win64 PEB & TEB + PE TLS slot 0 (gs:[0x58]) ---------------- */

#define SE_TEB_BYTES            0x600u
#define SE_DEFAULT_TLS_RVA      0x6e64a0u
#define SE_DEFAULT_TLS_SIZE     0xaa30u

typedef struct {
    uint8_t  bytes[SE_TEB_BYTES];
    uint64_t tls_array[8];
    uint8_t *tls_slot0;
    uint32_t tls_slot0_cap;
    uint32_t tid;
    uint32_t last_error;
    int      initialized;
    int      tls_populated;
} SE_SimTeb;

static SE_TLS SE_SimTeb tl_teb;
static uint8_t g_sim_peb[0x400];
static uint8_t g_sim_process_params[0x200];
static _Atomic int g_sim_peb_init = 0;
static _Atomic uint32_t g_next_tid = 1000u;

static void se_ensure_sim_peb(void)
{
    int st = atomic_load_explicit(&g_sim_peb_init, memory_order_acquire);
    if (st == 2) return;
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_sim_peb_init, &expected, 1)) {
        memset(g_sim_peb, 0, sizeof(g_sim_peb));
        memset(g_sim_process_params, 0, sizeof(g_sim_process_params));
        /* RTL_USER_PROCESS_PARAMETERS64: MaximumLength, Length, Flags = NORMALIZED (1). */
        uint32_t pp_len = (uint32_t)sizeof(g_sim_process_params);
        uint32_t pp_flags = 1u;
        memcpy(g_sim_process_params + 0x00u, &pp_len, 4);
        memcpy(g_sim_process_params + 0x04u, &pp_len, 4);
        memcpy(g_sim_process_params + 0x08u, &pp_flags, 4);
        uint64_t img_base = 0x140000000ull;
        uint64_t pp_ptr = (uint64_t)(uintptr_t)g_sim_process_params;
        memcpy(g_sim_peb + 0x10u, &img_base, 8);
        memcpy(g_sim_peb + 0x20u, &pp_ptr, 8);
        atomic_store_explicit(&g_sim_peb_init, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&g_sim_peb_init, memory_order_acquire) != 2) {
        }
    }
}

static const uint8_t *g_tls_tmpl = NULL;
static uint32_t       g_tls_raw_size = 0;
static uint32_t       g_tls_zero_size = 0;

#if !defined(_WIN32)
static pthread_key_t  g_tls_cleanup_key;
static pthread_once_t g_tls_cleanup_once = PTHREAD_ONCE_INIT;

static void se_tls_slot0_dtor(void *ptr)
{
    free(ptr);
}

static void se_tls_cleanup_init(void)
{
    pthread_key_create(&g_tls_cleanup_key, se_tls_slot0_dtor);
}
#endif

static void se_sim_teb_write64(SE_SimTeb *t, uint32_t off, uint64_t val)
{
    memcpy(t->bytes + off, &val, 8);
}

static void se_sim_teb_write32(SE_SimTeb *t, uint32_t off, uint32_t val)
{
    memcpy(t->bytes + off, &val, 4);
}

void se_win64_teb_set_tls_template(const void *raw_data, uint32_t raw_size, uint32_t zero_size)
{
    g_tls_tmpl = (const uint8_t *)raw_data;
    g_tls_raw_size = raw_size;
    g_tls_zero_size = zero_size;
}

static void se_ensure_tls_slot0(SE_SimTeb *t)
{
    const uint8_t *src = g_tls_tmpl;
    uint32_t raw_sz = g_tls_raw_size;
    uint32_t zero_sz = g_tls_zero_size;

    if (!src && es3_eh_image_base && es3_eh_image_size >= (SE_DEFAULT_TLS_RVA + SE_DEFAULT_TLS_SIZE)) {
        src = (const uint8_t *)(uintptr_t)(es3_eh_image_base + SE_DEFAULT_TLS_RVA);
        raw_sz = SE_DEFAULT_TLS_SIZE;
        zero_sz = 0;
    }

    uint32_t total = raw_sz + zero_sz;
    if (total == 0) total = 4096u;

    if (!t->tls_slot0 || t->tls_slot0_cap < total) {
        uint8_t *buf = (uint8_t *)calloc(1u, total);
        if (!buf) abort();
        t->tls_slot0 = buf;
        t->tls_slot0_cap = total;
        t->tls_populated = 0;
#if !defined(_WIN32)
        pthread_once(&g_tls_cleanup_once, se_tls_cleanup_init);
        pthread_setspecific(g_tls_cleanup_key, buf);
#endif
    }

    if (!t->tls_populated) {
        if (src && raw_sz > 0) {
            memcpy(t->tls_slot0, src, raw_sz);
        }
        if (zero_sz > 0) {
            memset(t->tls_slot0 + raw_sz, 0, zero_sz);
        }
        t->tls_populated = (src != NULL);
    }

    t->tls_array[0] = (uint64_t)(uintptr_t)t->tls_slot0;
}

uint8_t *se_win64_teb_ptr(void)
{
    se_ensure_sim_peb();
    SE_SimTeb *t = &tl_teb;
    if (!t->initialized) {
        memset(t->bytes, 0, sizeof(t->bytes));
        memset(t->tls_array, 0, sizeof(t->tls_array));
        t->tid = atomic_fetch_add(&g_next_tid, 1u);
        if (t->tid == 0) t->tid = 1000u;

        /* Default stack bounds updated per worker thread via se_win64_teb_set_stack. */
        uint64_t approx_sp = (uint64_t)(uintptr_t)&approx_sp;
        se_sim_teb_write64(t, 0x08u, approx_sp + 0x100000ull);
        se_sim_teb_write64(t, 0x10u, approx_sp > 0x100000ull ? approx_sp - 0x100000ull : 0x1000ull);
        se_sim_teb_write64(t, 0x30u, (uint64_t)(uintptr_t)t->bytes);  /* NtTib.Self */
        se_sim_teb_write64(t, 0x40u, 1ull);                           /* ClientId.UniqueProcess */
        se_sim_teb_write64(t, 0x48u, (uint64_t)t->tid);               /* ClientId.UniqueThread */
        se_sim_teb_write64(t, 0x58u, (uint64_t)(uintptr_t)t->tls_array);
        se_sim_teb_write64(t, 0x60u, (uint64_t)(uintptr_t)g_sim_peb); /* PEB */
        se_sim_teb_write32(t, 0x68u, t->last_error);
        t->initialized = 1;
    }
    if (!t->tls_populated) {
        se_ensure_tls_slot0(t);
    }
    return t->bytes;
}

void se_win64_teb_set_stack(uint64_t stack_base, uint64_t stack_limit)
{
    SE_SimTeb *t = &tl_teb;
    (void)se_win64_teb_ptr();
    se_sim_teb_write64(t, 0x08u, stack_base);
    se_sim_teb_write64(t, 0x10u, stack_limit);
}

uint32_t se_win64_teb_get_tid(void)
{
    (void)se_win64_teb_ptr();
    return tl_teb.tid;
}

void se_win64_teb_set_last_error(uint32_t err)
{
    SE_SimTeb *t = &tl_teb;
    (void)se_win64_teb_ptr();
    t->last_error = err;
    se_sim_teb_write32(t, 0x68u, err);
}

uint32_t se_win64_teb_get_last_error(void)
{
    SE_SimTeb *t = &tl_teb;
    (void)se_win64_teb_ptr();
    uint32_t v = 0;
    memcpy(&v, t->bytes + 0x68u, 4);
    return v;
}

uint64_t se_win64_peb_ptr(void)
{
    return (uint64_t)(uintptr_t)g_sim_peb;
}

/* ---- Image access & address normalization -------------------------------- */

typedef struct { uint32_t begin, end, unwind; } rfunc_t;

static rfunc_t  *pdata = NULL;
static uint32_t  pdata_n = 0;
static uint64_t  pdata_init_base = 0;

static uint32_t g_fh4_handlers[8];
static int      g_fh4_handler_n = 0;

void es3_eh_register_fh4_handler(uint32_t handler_rva)
{
    for (int i = 0; i < g_fh4_handler_n; i++) {
        if (g_fh4_handlers[i] == handler_rva) return;
    }
    if (g_fh4_handler_n < 8) {
        g_fh4_handlers[g_fh4_handler_n++] = handler_rva;
    }
}

static int is_fh4_handler_rva(uint32_t rva)
{
    for (int i = 0; i < g_fh4_handler_n; i++) {
        if (g_fh4_handlers[i] == rva) return 1;
    }
    return 0;
}

void es3_eh_set_image(uint64_t image_base, uint32_t image_size)
{
    es3_eh_image_base = image_base;
    es3_eh_image_size = image_size ? image_size : 0x01abd000u;
    pdata = NULL;
    pdata_n = 0;
    pdata_init_base = 0;
}

static uint8_t *img(uint32_t rva)
{
    return (uint8_t *)(uintptr_t)(es3_eh_image_base + rva);
}

static uint32_t ru32(uint32_t rva)
{
    uint32_t v = 0;
    memcpy(&v, img(rva), 4);
    return v;
}

static uint16_t ru16(uint32_t rva)
{
    uint16_t v = 0;
    memcpy(&v, img(rva), 2);
    return v;
}

static int32_t ri32(uint32_t rva)
{
    int32_t v = 0;
    memcpy(&v, img(rva), 4);
    return v;
}

static uint32_t va_to_rva(uint64_t va)
{
    if (es3_eh_image_base &&
        va >= es3_eh_image_base &&
        va < es3_eh_image_base + (uint64_t)es3_eh_image_size) {
        return (uint32_t)(va - es3_eh_image_base);
    }
    if (va >= 0x140000000ull &&
        va < 0x140000000ull + (uint64_t)es3_eh_image_size) {
        return (uint32_t)(va - 0x140000000ull);
    }
    return 0xffffffffu;
}

static uint64_t va_to_canonical(uint64_t va)
{
    uint32_t rva = va_to_rva(va);
    if (rva != 0xffffffffu) {
        return 0x140000000ull + (uint64_t)rva;
    }
    return va;
}

static void eh_init(void)
{
    uint32_t pe, dd, pdata_rva, pdata_sz;
    if (!es3_eh_image_base) return;
    if (pdata && pdata_init_base == es3_eh_image_base) return;

    pe = ru32(0x3c);
    if (pe + 0x18 + 0x70 + 3 * 8 + 8 > es3_eh_image_size) return;
    dd = pe + 0x18 + 0x70 + 3 * 8;
    pdata_rva = ru32(dd);
    pdata_sz = ru32(dd + 4);
    if (!pdata_rva || pdata_rva + pdata_sz > es3_eh_image_size) return;

    pdata = (rfunc_t *)img(pdata_rva);
    pdata_n = pdata_sz / 12u;
    pdata_init_base = es3_eh_image_base;
}

static rfunc_t *pdata_find(uint32_t rva)
{
    int lo = 0, hi = (int)pdata_n - 1;
    while (lo <= hi) {
        int m = lo + (hi - lo) / 2;
        if (rva < pdata[m].begin) hi = m - 1;
        else if (rva >= pdata[m].end) lo = m + 1;
        else return &pdata[m];
    }
    return NULL;
}

/* ---- Virtual unwind over Win64 .pdata / .xdata --------------------------- */

static uint64_t *gpr(CPU *c, int n) { return &((uint64_t *)c)[n & 15]; }

static uint64_t frame_establisher(CPU *c, const rfunc_t *rf)
{
    uint32_t u;
    if (!rf) return c->rsp;
    u = rf->unwind;
    for (;;) {
        uint8_t ver_flags = *img(u);
        uint8_t ncodes = *img(u + 2);
        uint8_t fr = *img(u + 3);
        uint8_t freg = fr & 0xfu;
        uint8_t foff = fr >> 4;
        if (freg) return *gpr(c, freg) - (uint64_t)foff * 16ull;
        if (!(ver_flags & 0x20u)) break; /* UNW_FLAG_CHAININFO */
        u = ru32(u + 4u + (((uint32_t)ncodes + 1u) & ~1u) * 2u + 8u);
    }
    return c->rsp;
}

static int unwind_one(CPU *c, uint64_t *pc, uint64_t *establisher)
{
    uint32_t rva = va_to_rva(*pc);
    rfunc_t *rf = (rva != 0xffffffffu) ? pdata_find(rva) : NULL;
    uint32_t u, off;
    uint64_t frame_base;

    if (!rf) {
        if (establisher) *establisher = c->rsp;
        *pc = rd64(c->rsp);
        c->rsp += 8;
        return *pc != 0;
    }

    u = rf->unwind;
    off = (rva >= rf->begin) ? (rva - rf->begin) : 0u;
    frame_base = c->rsp;

    for (;;) {
        uint8_t ver_flags = *img(u);
        uint8_t ncodes = *img(u + 2);
        uint8_t fr = *img(u + 3);
        uint8_t freg = fr & 0xfu;
        uint8_t foff = fr >> 4;
        uint32_t codes = u + 4u;
        int i;

        if (freg) {
            frame_base = *gpr(c, freg) - (uint64_t)foff * 16ull;
        }

        for (i = 0; i < (int)ncodes; ) {
            uint8_t coff = *img(codes + (uint32_t)i * 2u);
            uint8_t opinfo = *img(codes + (uint32_t)i * 2u + 1u);
            uint8_t op = opinfo & 0xfu;
            uint8_t info = opinfo >> 4;
            int slots = 1;
            int apply = coff <= off;

            switch (op) {
            case 0: /* PUSH_NONVOL */
                if (apply) {
                    *gpr(c, info) = rd64(c->rsp);
                    c->rsp += 8;
                }
                break;
            case 1: /* ALLOC_LARGE */
                if (info == 0) {
                    slots = 2;
                    if (apply) c->rsp += (uint64_t)ru16(codes + (uint32_t)(i + 1) * 2u) * 8ull;
                } else {
                    slots = 3;
                    if (apply) c->rsp += ru32(codes + (uint32_t)(i + 1) * 2u);
                }
                break;
            case 2: /* ALLOC_SMALL */
                if (apply) c->rsp += (uint64_t)info * 8ull + 8ull;
                break;
            case 3: /* SET_FPREG */
                if (apply && freg) c->rsp = *gpr(c, freg) - (uint64_t)foff * 16ull;
                break;
            case 4: /* SAVE_NONVOL */
                slots = 2;
                if (apply) {
                    *gpr(c, info) =
                        rd64(frame_base + (uint64_t)ru16(codes + (uint32_t)(i + 1) * 2u) * 8ull);
                }
                break;
            case 5: /* SAVE_NONVOL_FAR */
                slots = 3;
                if (apply) {
                    *gpr(c, info) =
                        rd64(frame_base + (uint64_t)ru32(codes + (uint32_t)(i + 1) * 2u));
                }
                break;
            case 6: /* UWOP_EPILOG (version 2 metadata) */
                slots = 1;
                break;
            case 8: /* SAVE_XMM128 */
                slots = 2;
                break;
            case 9: /* SAVE_XMM128_FAR */
                slots = 3;
                break;
            default: /* PUSH_MACHFRAME */
                break;
            }
            i += slots;
        }
        if (ver_flags & 0x20u) { /* UNW_FLAG_CHAININFO */
            uint32_t chain = u + 4u + (((uint32_t)ncodes + 1u) & ~1u) * 2u;
            u = ru32(chain + 8u);
            off = 0xffffffffu;
            continue;
        }
        break;
    }

    if (establisher) *establisher = c->rsp;
    *pc = rd64(c->rsp);
    c->rsp += 8;
    return *pc != 0;
}

/* ---- ThrowInfo / CatchableType / TypeDescriptor matching ---------------- */

static const char *td_name(uint32_t td_rva)
{
    return td_rva ? (const char *)img(td_rva + 16u) : NULL;
}

static int catch_matches(uint32_t catch_td, uint32_t ti, uint32_t *out_size)
{
    uint32_t cta, n, i;
    const char *want;

    if (!catch_td) {
        if (out_size) *out_size = 8u;
        return 1; /* catch (...) */
    }
    want = td_name(catch_td);
    cta = (uint32_t)ri32(ti + 0x0cu);
    if (!cta || !want) return 0;
    n = ru32(cta);
    for (i = 0; i < n && i < 64u; i++) {
        uint32_t ct = ru32(cta + 4u + 4u * i);
        const char *have = td_name((uint32_t)ri32(ct + 4u));
        if (have && strcmp(have, want) == 0) {
            if (out_size) *out_size = ru32(ct + 0x14u);
            return 1;
        }
    }
    return 0;
}

static void bind_catch_object(uint64_t establisher, uint32_t obj_off,
                              uint32_t adjectives, uint64_t obj, uint32_t obj_size)
{
    if (!obj_off) return;
    if (adjectives & 0x08u) {
        /* HT_IsReference: catch (const T& e) binds pointer to thrown object */
        memcpy((void *)(uintptr_t)(establisher + obj_off), &obj, 8);
    } else {
        uint32_t copy_sz = obj_size ? obj_size : 8u;
        if (copy_sz > 64u) copy_sz = 64u;
        memcpy((void *)(uintptr_t)(establisher + obj_off),
               (const void *)(uintptr_t)obj, copy_sz);
    }
}

/* ---- FH3 (__CxxFrameHandler3) -------------------------------------------- */

typedef struct {
    uint32_t magic;
    int32_t  max_state;
    int32_t  unwind_map;
    int32_t  n_try;
    int32_t  try_map;
    int32_t  n_ip;
    int32_t  ip_map;
    int32_t  unwind_help;
} funcinfo3_t;

static int funcinfo_at_fh3(uint32_t unwind, funcinfo3_t *fi)
{
    uint8_t ver_flags, ncodes;
    uint32_t hd, rva;

    if (!unwind) return 0;
    ver_flags = *img(unwind);
    if (!(ver_flags & 0x18u)) return 0; /* UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER */
    if (ver_flags & 0x20u) return 0;
    ncodes = *img(unwind + 2u);
    hd = unwind + 4u + (((uint32_t)ncodes + 1u) & ~1u) * 2u;
    rva = ru32(hd + 4u);
    if (!rva || rva + sizeof(*fi) > es3_eh_image_size) return 0;
    memcpy(fi, img(rva), sizeof(*fi));
    if ((fi->magic & 0xfffffff0u) != 0x19930520u) return 0;
    return 1;
}

static int fh3_state_at(const funcinfo3_t *fi, uint32_t rva)
{
    int i, st = -1;
    if (fi->n_ip <= 0 || !fi->ip_map) return -1;
    for (i = 0; i < fi->n_ip; i++) {
        if ((uint32_t)ri32((uint32_t)fi->ip_map + 8u * (uint32_t)i) > rva) break;
        st = ri32((uint32_t)fi->ip_map + 8u * (uint32_t)i + 4u);
    }
    return st;
}

static int fh3_find_catch(const funcinfo3_t *fi, int st, uint32_t ti,
                          int *out_try_lo, uint32_t *out_handler,
                          uint32_t *out_obj_off, uint32_t *out_adj, uint32_t *out_sz)
{
    int t, best_low = -1, found = 0;
    if (fi->n_try <= 0 || !fi->try_map || st < 0) return 0;
    for (t = 0; t < fi->n_try; t++) {
        uint32_t entry = (uint32_t)fi->try_map + 20u * (uint32_t)t;
        int32_t lo = ri32(entry);
        int32_t hi = ri32(entry + 4u);
        int32_t nc = ri32(entry + 12u);
        uint32_t ha = (uint32_t)ri32(entry + 16u);
        int k;
        if (st < lo || st > hi || lo <= best_low) continue;
        for (k = 0; k < nc; k++) {
            uint32_t h_rva = ha + 20u * (uint32_t)k;
            uint32_t adj = ru32(h_rva);
            uint32_t td = (uint32_t)ri32(h_rva + 4u);
            uint32_t sz = 8u;
            if (!catch_matches(td, ti, &sz)) continue;
            best_low = lo;
            *out_try_lo = lo;
            *out_handler = (uint32_t)ri32(h_rva + 12u);
            *out_obj_off = (uint32_t)ri32(h_rva + 8u);
            *out_adj = adj;
            *out_sz = sz;
            found = 1;
            break;
        }
    }
    return found;
}

static void run_cleanup_funclet(CPU *c, uint64_t establisher, uint32_t action_rva)
{
    CPU tmp = *c;
    tmp.rcx = 0;
    tmp.rdx = establisher;
    tmp.rsp = (establisher - 0x200ull) & ~(uint64_t)15ull;
    push64(&tmp, 0);
    dispatch(&tmp, 0x140000000ull + (uint64_t)action_rva);
}

static void fh3_unwind_to_state(CPU *c, uint64_t establisher, const funcinfo3_t *fi,
                                int cur_state, int target_state)
{
    int steps = 0;
    if (!fi->unwind_map || fi->max_state <= 0) return;
    while (cur_state > target_state && cur_state >= 0 && cur_state < fi->max_state && steps < 256) {
        uint32_t u_rva = (uint32_t)fi->unwind_map + 8u * (uint32_t)cur_state;
        int32_t  next_state = ri32(u_rva);
        uint32_t action = (uint32_t)ri32(u_rva + 4u);
        cur_state = next_state;
        if (action) {
            run_cleanup_funclet(c, establisher, action);
        }
        steps++;
    }
}

/* ---- FH4 (__CxxFrameHandler4 compressed metadata) ------------------------ */

static uint32_t fh4_read_uint(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint8_t b0 = p[0];
    uint32_t v;

    if ((b0 & 0x01u) == 0u) {
        *pp = p + 1;
        return (uint32_t)(b0 >> 1);
    }
    if ((b0 & 0x03u) == 0x01u) {
        v = (uint32_t)b0 | ((uint32_t)p[1] << 8);
        *pp = p + 2;
        return v >> 2;
    }
    if ((b0 & 0x07u) == 0x03u) {
        v = (uint32_t)b0 | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        *pp = p + 3;
        return v >> 3;
    }
    if ((b0 & 0x0fu) == 0x07u) {
        v = (uint32_t)b0 | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        *pp = p + 4;
        return v >> 4;
    }
    /* (b0 & 0x0fu) == 0x0fu: next 4 bytes LE */
    memcpy(&v, p + 1, 4);
    *pp = p + 5;
    return v;
}

typedef struct {
    uint8_t  header;
    int32_t  unwind_map_rva;
    int32_t  try_map_rva;
    int32_t  ip_map_rva;
    uint32_t func_begin_rva;
} funcinfo4_t;

static int funcinfo_at_fh4(uint32_t unwind, uint32_t func_begin_rva, funcinfo4_t *fi)
{
    uint8_t ver_flags, ncodes, hdr;
    uint32_t hd, handler_rva, fi_rva;
    const uint8_t *p;

    if (!unwind) return 0;
    ver_flags = *img(unwind);
    if (!(ver_flags & 0x18u) || (ver_flags & 0x20u)) return 0;
    ncodes = *img(unwind + 2u);
    hd = unwind + 4u + (((uint32_t)ncodes + 1u) & ~1u) * 2u;
    handler_rva = ru32(hd);
    fi_rva = ru32(hd + 4u);
    if (!fi_rva || fi_rva >= es3_eh_image_size) return 0;
    if (!is_fh4_handler_rva(handler_rva)) return 0;

    memset(fi, 0, sizeof(*fi));
    fi->func_begin_rva = func_begin_rva;
    p = img(fi_rva);
    hdr = *p++;
    fi->header = hdr;
    if (hdr & 0x04u) {
        (void)fh4_read_uint(&p); /* BBT flags */
    }
    if (hdr & 0x08u) {
        memcpy(&fi->unwind_map_rva, p, 4);
        p += 4;
    }
    if (hdr & 0x10u) {
        memcpy(&fi->try_map_rva, p, 4);
        p += 4;
    }
    memcpy(&fi->ip_map_rva, p, 4);
    p += 4;

    if (hdr & 0x02u) {
        /* Separated segments: ip_map_rva points to list of (funcRva, ipMapRva) */
        const uint8_t *sp = img((uint32_t)fi->ip_map_rva);
        uint32_t nseg = fh4_read_uint(&sp);
        int32_t matched = 0;
        for (uint32_t s = 0; s < nseg; s++) {
            int32_t frva = 0, mrva = 0;
            memcpy(&frva, sp, 4);
            memcpy(&mrva, sp + 4, 4);
            sp += 8;
            if ((uint32_t)frva == func_begin_rva) {
                matched = mrva;
                break;
            }
        }
        fi->ip_map_rva = matched;
    }
    return 1;
}

static int fh4_state_at(const funcinfo4_t *fi, uint32_t rva)
{
    const uint8_t *p;
    uint32_t n, cur_rva = fi->func_begin_rva;
    int st = -1;

    if (!fi->ip_map_rva) return -1;
    p = img((uint32_t)fi->ip_map_rva);
    n = fh4_read_uint(&p);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t delta = fh4_read_uint(&p);
        uint32_t enc_st = fh4_read_uint(&p);
        cur_rva += delta;
        if (cur_rva > rva) break;
        st = (int)enc_st - 1;
    }
    return st;
}

static int fh4_find_catch(const funcinfo4_t *fi, int st, uint32_t ti,
                          int *out_try_lo, uint32_t *out_handler,
                          uint32_t *out_obj_off, uint32_t *out_adj, uint32_t *out_sz)
{
    const uint8_t *tp;
    uint32_t n_try;
    int best_low = -1, found = 0;

    if (!(fi->header & 0x10u) || !fi->try_map_rva || st < 0) return 0;
    tp = img((uint32_t)fi->try_map_rva);
    n_try = fh4_read_uint(&tp);

    for (uint32_t t = 0; t < n_try; t++) {
        int32_t lo = (int32_t)fh4_read_uint(&tp);
        int32_t hi = (int32_t)fh4_read_uint(&tp);
        (void)fh4_read_uint(&tp); /* catchHigh */
        int32_t ha_rva = 0;
        memcpy(&ha_rva, tp, 4);
        tp += 4;

        if (st < lo || st > hi || lo <= best_low || !ha_rva) continue;
        const uint8_t *hp = img((uint32_t)ha_rva);
        uint32_t nc = fh4_read_uint(&hp);

        for (uint32_t k = 0; k < nc; k++) {
            uint8_t h_hdr = *hp++;
            uint32_t adj = (h_hdr & 0x01u) ? fh4_read_uint(&hp) : 0u;
            uint32_t td = 0;
            if (h_hdr & 0x02u) {
                int32_t td_rva = 0;
                memcpy(&td_rva, hp, 4);
                hp += 4;
                td = (uint32_t)td_rva;
            }
            uint32_t obj_off = (h_hdr & 0x04u) ? fh4_read_uint(&hp) : 0u;
            int32_t disp_handler = 0;
            memcpy(&disp_handler, hp, 4);
            hp += 4;

            uint32_t cont_cnt = (uint32_t)((h_hdr >> 4) & 0x03u);
            for (uint32_t cidx = 0; cidx < cont_cnt; cidx++) {
                if (h_hdr & 0x08u) {
                    hp += 4; /* contIsRVA */
                } else {
                    (void)fh4_read_uint(&hp);
                }
            }

            uint32_t sz = 8u;
            if (!catch_matches(td, ti, &sz)) continue;
            best_low = lo;
            *out_try_lo = lo;
            *out_handler = (uint32_t)disp_handler;
            *out_obj_off = obj_off;
            *out_adj = adj;
            *out_sz = sz;
            found = 1;
            break;
        }
    }
    return found;
}

static void fh4_unwind_to_state(CPU *c, uint64_t establisher, const funcinfo4_t *fi,
                                int cur_state, int target_state)
{
    typedef struct { uint32_t type; int32_t to_state; uint32_t action; } uw4_t;
    uw4_t entries[128];
    uint32_t n;
    const uint8_t *p;
    int steps = 0;

    if (!(fi->header & 0x08u) || !fi->unwind_map_rva || cur_state <= target_state) return;
    p = img((uint32_t)fi->unwind_map_rva);
    n = fh4_read_uint(&p);
    if (n > 128u) n = 128u;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t next_and_type = fh4_read_uint(&p);
        uint32_t type = next_and_type & 0x03u;
        uint32_t delta = next_and_type >> 2;
        uint32_t action = 0;
        if (type == 1u || type == 2u) {
            int32_t a = 0;
            memcpy(&a, p, 4);
            p += 4;
            action = (uint32_t)a;
            (void)fh4_read_uint(&p); /* object offset */
        } else if (type == 3u) {
            int32_t a = 0;
            memcpy(&a, p, 4);
            p += 4;
            action = (uint32_t)a;
        }
        entries[i].type = type;
        entries[i].to_state = (int32_t)i - (int32_t)delta;
        entries[i].action = action;
    }

    while (cur_state > target_state && cur_state >= 0 && (uint32_t)cur_state < n && steps < 128) {
        uw4_t e = entries[cur_state];
        cur_state = (e.to_state < cur_state) ? e.to_state : (cur_state - 1);
        if (e.action && (e.type == 1u || e.type == 2u || e.type == 3u)) {
            run_cleanup_funclet(c, establisher, e.action);
        }
        steps++;
    }
}

/* ---- Two-pass C++ throw dispatcher --------------------------------------- */

int es3_eh_throw(CPU *c, uint64_t ret_pc, uint64_t obj, uint64_t throw_info)
{
    uint64_t pc;
    uint32_t ti;
    int depth;

    int      catch_depth = -1;
    int      catch_is_fh4 = 0;
    int      catch_try_lo = 0;
    int      catch_state = -1;
    uint32_t catch_handler = 0;
    uint32_t catch_obj_off = 0;
    uint32_t catch_adj = 0;
    uint32_t catch_sz = 8u;
    uint64_t catch_fn_canon = 0;
    es3_ehframe_t *catch_frame = NULL;

    if (!obj || !throw_info || !ret_pc) return 0;
    eh_init();
    if (!pdata) return 0;
    ti = va_to_rva(throw_info);
    if (ti == 0xffffffffu) return 0;

    if (g_eh_trace) {
        fprintf(stderr, "[eh] throw obj %#llx ti %#llx from %#llx, rsp %#llx\n",
                (unsigned long long)obj, (unsigned long long)throw_info,
                (unsigned long long)ret_pc, (unsigned long long)c->rsp);
    }

    /* Pass 1: probe stack non-destructively to find matching catch handler. */
    CPU probe = *c;
    pc = ret_pc;
    for (depth = 0; depth < 512 && pc; depth++) {
        uint32_t rva = va_to_rva(pc);
        rfunc_t *rf = (rva != 0xffffffffu) ? pdata_find(rva) : NULL;
        if (rf) {
            uint32_t lookup_rva = (rva > rf->begin) ? (rva - 1u) : rva;
            funcinfo3_t fi3;
            funcinfo4_t fi4;
            int found = 0;
            int is_fh4 = 0;
            int try_lo = 0, st = -1;
            uint32_t handler = 0, obj_off = 0, adj = 0, sz = 8u;

            if (funcinfo_at_fh3(rf->unwind, &fi3)) {
                st = fh3_state_at(&fi3, lookup_rva);
                found = fh3_find_catch(&fi3, st, ti, &try_lo, &handler, &obj_off, &adj, &sz);
            } else if (funcinfo_at_fh4(rf->unwind, rf->begin, &fi4)) {
                is_fh4 = 1;
                st = fh4_state_at(&fi4, lookup_rva);
                found = fh4_find_catch(&fi4, st, ti, &try_lo, &handler, &obj_off, &adj, &sz);
            }

            if (found) {
                uint64_t fn_canon = 0x140000000ull + (uint64_t)rf->begin;
                es3_ehframe_t *f;
                for (f = eh_top; f; f = f->prev) {
                    if (f->func == fn_canon) break;
                }
                if (!f) {
                    fprintf(stderr, "[eh] %#llx catches at state %d but has no landing pad\n",
                            (unsigned long long)fn_canon, st);
                    return 0;
                }
                catch_depth = depth;
                catch_is_fh4 = is_fh4;
                catch_try_lo = try_lo;
                catch_state = st;
                catch_handler = handler;
                catch_obj_off = obj_off;
                catch_adj = adj;
                catch_sz = sz;
                catch_fn_canon = fn_canon;
                catch_frame = f;
                break;
            }
        }
        if (!unwind_one(&probe, &pc, NULL)) break;
    }

    if (catch_depth < 0 || !catch_frame) {
        if (g_eh_trace) fprintf(stderr, "[eh] no guest frame caught the throw\n");
        return 0;
    }

    /* Pass 2: actually unwind frames 0..catch_depth, running RAII cleanup actions. */
    pc = ret_pc;
    for (depth = 0; depth <= catch_depth; depth++) {
        uint32_t rva = va_to_rva(pc);
        rfunc_t *rf = (rva != 0xffffffffu) ? pdata_find(rva) : NULL;
        uint64_t establisher = frame_establisher(c, rf);

        if (rf) {
            uint32_t lookup_rva = (rva > rf->begin) ? (rva - 1u) : rva;
            int target_st = (depth == catch_depth) ? (catch_try_lo - 1) : -1;
            funcinfo3_t fi3;
            funcinfo4_t fi4;

            if (depth == catch_depth && catch_is_fh4 &&
                funcinfo_at_fh4(rf->unwind, rf->begin, &fi4)) {
                fh4_unwind_to_state(c, establisher, &fi4, catch_state, target_st);
            } else if (funcinfo_at_fh3(rf->unwind, &fi3)) {
                int st = (depth == catch_depth) ? catch_state : fh3_state_at(&fi3, lookup_rva);
                fh3_unwind_to_state(c, establisher, &fi3, st, target_st);
            } else if (funcinfo_at_fh4(rf->unwind, rf->begin, &fi4)) {
                int st = fh4_state_at(&fi4, lookup_rva);
                fh4_unwind_to_state(c, establisher, &fi4, st, target_st);
            }
        }

        if (depth == catch_depth) {
            uint64_t saved_frame_rsp = c->rsp;
            uint64_t cont;

            if (g_eh_trace) {
                fprintf(stderr, "[eh] entering funclet %#llx frame %#llx catchobj +%#x size %u\n",
                        (unsigned long long)(es3_eh_image_base + catch_handler),
                        (unsigned long long)establisher, catch_obj_off, catch_sz);
            }

            bind_catch_object(establisher, catch_obj_off, catch_adj, obj, catch_sz);

            c->rdx = establisher;
            c->rcx = 0;
            c->rsp = (establisher - 0x200ull) & ~(uint64_t)15ull;
            push64(c, 0);
            dispatch(c, 0x140000000ull + (uint64_t)catch_handler);
            cont = va_to_canonical(c->rax);
            c->rsp = saved_frame_rsp;

            if (g_eh_trace) {
                fprintf(stderr, "[eh] caught in %#llx (state %d), resuming at %#llx\n",
                        (unsigned long long)catch_fn_canon, catch_state,
                        (unsigned long long)cont);
            }

            catch_frame->resume = cont;
            c->rip = cont;
            tl_cs_depth = catch_frame->cs_depth;
            eh_top = catch_frame;
            longjmp(*catch_frame->jb, 1);
        }

        if (!unwind_one(c, &pc, &establisher)) break;
    }

    return 0;
}
