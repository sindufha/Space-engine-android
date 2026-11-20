/*
 * crt_intercepts.c - Clean native C intercepts for MSVC CRT/UCRT hotpaths and
 * baseline SSE2 ISA enforcement (recomp/CRT_TAGS.json).
 */
#include "hle_bridge.h"
#include "pe_loader.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static void safe_wr32_gva(uint64_t canonical_va, uint32_t val) {
    uint64_t base = se_pe_active_base();
    uint32_t size = se_pe_active_size();
    if (!base || canonical_va < SE_EXPECTED_IMAGE_BASE) return;
    uint64_t rva = canonical_va - SE_EXPECTED_IMAGE_BASE;
    if (rva + 4u <= size) {
        wr32(GVA(canonical_va), val);
    }
}

static void crt_isa_init(CPU *c) {
    safe_wr32_gva(0x140773928ull, 0u); /* __isa_available = 0 (SSE2 baseline) */
    safe_wr32_gva(0x14077392Cull, 2u); /* __isa_enabled = SSE2 only */
    safe_wr32_gva(0x14081FA64ull, 0u); /* __favor = 0 */
    HLE_RET32(c, 0u);
    c->rsp += 8;
}

static void crt_ucrt_math_isa_init(CPU *c) {
    safe_wr32_gva(0x140820724ull, 0u); /* ucrt math __isa_available */
    safe_wr32_gva(0x140820728ull, 0u); /* ucrt math __isa_enabled */
    HLE_RET32(c, 0u);
    c->rsp += 8;
}

static void crt_memmove(CPU *c) {
    void *dst = (void *)(uintptr_t)c->rcx;
    const void *src = (const void *)(uintptr_t)c->rdx;
    size_t len = (size_t)c->r8;
    if (len > 0 && dst && src) {
        memmove(dst, src, len);
    }
    HLE_RET64(c, c->rcx);
    c->rsp += 8;
}

static void crt_memset(CPU *c) {
    void *dst = (void *)(uintptr_t)c->rcx;
    int val = (int)(uint8_t)c->rdx;
    size_t len = (size_t)c->r8;
    if (len > 0 && dst) {
        memset(dst, val, len);
    }
    HLE_RET64(c, c->rcx);
    c->rsp += 8;
}

static void crt_strrchr(CPU *c) {
    const char *s = (const char *)(uintptr_t)c->rcx;
    int ch = (int)(uint8_t)c->rdx;
    const char *res = s ? strrchr(s, ch) : NULL;
    HLE_RET_PTR(c, res);
    c->rsp += 8;
}

static void crt_strstr(CPU *c) {
    const char *hay = (const char *)(uintptr_t)c->rcx;
    const char *needle = (const char *)(uintptr_t)c->rdx;
    const char *res = (hay && needle) ? (needle[0] ? strstr(hay, needle) : hay) : NULL;
    HLE_RET_PTR(c, res);
    c->rsp += 8;
}

static void crt_wcsrchr(CPU *c) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)c->rcx;
    uint16_t ch = (uint16_t)c->rdx;
    const uint16_t *match = NULL;
    if (s) {
        for (;;) {
            if (*s == ch) match = s;
            if (*s == 0) break;
            ++s;
        }
    }
    HLE_RET_PTR(c, match);
    c->rsp += 8;
}

static void crt_wcsstr(CPU *c) {
    const uint16_t *hay = (const uint16_t *)(uintptr_t)c->rcx;
    const uint16_t *needle = (const uint16_t *)(uintptr_t)c->rdx;
    const uint16_t *res = NULL;
    if (hay && needle) {
        if (needle[0] == 0) {
            res = hay;
        } else {
            size_t nlen = se_utf16_len(needle);
            for (const uint16_t *p = hay; *p != 0; ++p) {
                if (*p == needle[0] && memcmp(p, needle, nlen * sizeof(uint16_t)) == 0) {
                    res = p;
                    break;
                }
            }
        }
    }
    HLE_RET_PTR(c, res);
    c->rsp += 8;
}

static void crt_fmod(CPU *c) {
    double x = c->xmm[0].f64[0];
    double y = c->xmm[1].f64[0];
    HLE_RET_F64(c, fmod(x, y));
    c->rsp += 8;
}

static void crt_powf(CPU *c) {
    float x = c->xmm[0].f32[0];
    float y = c->xmm[1].f32[0];
    HLE_RET_F32(c, powf(x, y));
    c->rsp += 8;
}

static void crt_setjmp(CPU *c) {
    uint64_t buf = c->rcx;
    if (buf) {
        wr64(buf + 0x00ull, c->rdx);
        wr64(buf + 0x08ull, c->rbx);
        wr64(buf + 0x10ull, c->rsp + 8ull);
        wr64(buf + 0x18ull, c->rbp);
        wr64(buf + 0x20ull, c->rsi);
        wr64(buf + 0x28ull, c->rdi);
        wr64(buf + 0x30ull, c->r12);
        wr64(buf + 0x38ull, c->r13);
        wr64(buf + 0x40ull, c->r14);
        wr64(buf + 0x48ull, c->r15);
        wr64(buf + 0x50ull, rd64(c->rsp));
        wr32(buf + 0x58ull, c->mxcsr);
        wr16(buf + 0x5Cull, 0x027Fu); /* default x87 CW */
        for (int i = 0; i < 10; ++i) {
            wr128(buf + 0x60ull + (uint64_t)i * 16ull, &c->xmm[6 + i]);
        }
    }
    HLE_RET32(c, 0u);
    c->rsp += 8;
}

static void crt_longjmp(CPU *c) {
    uint64_t buf = c->rcx;
    uint32_t val = (uint32_t)c->rdx;
    if (val == 0u) val = 1u;
    fprintf(stderr, "[crt_longjmp] invoked with buf=%#llx val=%u from rip=%#llx\n",
            (unsigned long long)buf, val, (unsigned long long)c->rip);
    if (buf) {
        c->rbx = rd64(buf + 0x08ull);
        c->rbp = rd64(buf + 0x18ull);
        c->rsi = rd64(buf + 0x20ull);
        c->rdi = rd64(buf + 0x28ull);
        c->r12 = rd64(buf + 0x30ull);
        c->r13 = rd64(buf + 0x38ull);
        c->r14 = rd64(buf + 0x40ull);
        c->r15 = rd64(buf + 0x48ull);
        c->mxcsr = rd32(buf + 0x58ull);
        for (int i = 0; i < 10; ++i) {
            rd128(buf + 0x60ull + (uint64_t)i * 16ull, &c->xmm[6 + i]);
        }
        c->rsp = rd64(buf + 0x10ull);
        c->rip = rd64(buf + 0x50ull);
    }
    HLE_RET32(c, val);
    /* No native host setjmp frame exists in static non-EH wrappers; abort cleanly rather than corrupting C stack. */
    abort();
}

bool se_crt_try_intercept(CPU *c, uint64_t canonical_target) {
    switch (canonical_target) {
    case 0x140076CC0ull: /* _guard_check_icall_nop */
        c->rsp += 8;
        return true;
    case 0x1405D76D0ull: /* _guard_dispatch_icall_nop (jmp rax) */
        dispatch_jmp(c, c->rax);
        return true;
    case 0x1400CABB8ull: /* __isa_available_init */
        crt_isa_init(c);
        return true;
    case 0x1401698F0ull: /* __isa_available_init (UCRT math) */
        crt_ucrt_math_isa_init(c);
        return true;
    case 0x1401470C0ull: /* memcpy / memmove */
        crt_memmove(c);
        return true;
    case 0x140147520ull: /* memset */
        crt_memset(c);
        return true;
    case 0x140146C88ull: /* strrchr */
        crt_strrchr(c);
        return true;
    case 0x140146DD4ull: /* wcsrchr */
        crt_wcsrchr(c);
        return true;
    case 0x140147828ull: /* wcsstr */
        crt_wcsstr(c);
        return true;
    case 0x140147CCCull: /* strstr */
        crt_strstr(c);
        return true;
    case 0x1405D15B0ull: /* fmod */
        crt_fmod(c);
        return true;
    case 0x14015F520ull: /* powf (eliminates AVX2 + rcr paths) */
        crt_powf(c);
        return true;
    case 0x1405D7810ull: /* _setjmp / __setjmpex */
    case 0x1405D78B0ull:
        crt_setjmp(c);
        return true;
    case 0x1405D76F0ull: /* longjmp */
    case 0x140147ECCull: /* _longjmpex */
        crt_longjmp(c);
        return true;
    default:
        return false;
    }
}
