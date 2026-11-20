/*
 * png_tiff_hle.c - Phase 5 HLE shims for libpng16.dll (19) and tiff.dll (11).
 *
 * Bridges SpaceEngine.exe Win64 IAT imports directly to native libpng 1.6.44
 * and libtiff 4.7.0, safely dispatching guest I/O, allocator, and error callbacks
 * via se_hle_call_guest(), resolving virtual paths for TIFFOpen(), and mapping
 * Win64 variadic TIFFGetField / TIFFGetFieldDefaulted / TIFFSetField arguments
 * cleanly across ARM64 / Win64 ABI boundaries.
 */
#include "hle_bridge.h"

#include "png.h"
#include "tiffio.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define PT_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineImg", fmt, ##__VA_ARGS__)
#else
#define PT_LOG(fmt, ...) ((void)0)
#endif

/* ========================================================================= */
/* Lightweight C11 spinlock for context registration tables                  */
/* ========================================================================= */

static atomic_flag g_png_lock = ATOMIC_FLAG_INIT;
static atomic_flag g_tiff_lock = ATOMIC_FLAG_INIT;

static inline void spin_lock(atomic_flag *f) {
    while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) {
    }
}

static inline void spin_unlock(atomic_flag *f) {
    atomic_flag_clear_explicit(f, memory_order_release);
}

/* ========================================================================= */
/* libpng16.dll (19 imports)                                                */
/* ========================================================================= */

#define SE_PNG_CTX_MAGIC 0x5345504E47435458ull /* "SEPNGCTX" */

typedef struct se_png_ctx {
    uint64_t magic;
    png_structp png_ptr;
    uint64_t guest_err_ptr;
    uint64_t guest_err_fn;
    uint64_t guest_warn_fn;
    uint64_t guest_mem_ptr;
    uint64_t guest_malloc_fn;
    uint64_t guest_free_fn;
    uint64_t guest_io_ptr;
    uint64_t guest_read_fn;
    uint64_t guest_write_fn;
    uint64_t guest_flush_fn;
    struct se_png_ctx *next;
} se_png_ctx_t;

static se_png_ctx_t *g_png_list = NULL;
static _Thread_local CPU *tls_png_cpu = NULL;

static void se_png_track(se_png_ctx_t *ctx) {
    spin_lock(&g_png_lock);
    ctx->next = g_png_list;
    g_png_list = ctx;
    spin_unlock(&g_png_lock);
}

static se_png_ctx_t *se_png_find(png_const_structrp png_ptr) {
    if (!png_ptr) return NULL;
    spin_lock(&g_png_lock);
    se_png_ctx_t *cur = g_png_list;
    while (cur) {
        if (cur->png_ptr == png_ptr) {
            spin_unlock(&g_png_lock);
            return cur;
        }
        cur = cur->next;
    }
    spin_unlock(&g_png_lock);
    return NULL;
}

static se_png_ctx_t *se_png_untrack(png_const_structrp png_ptr) {
    if (!png_ptr) return NULL;
    spin_lock(&g_png_lock);
    se_png_ctx_t **pp = &g_png_list;
    while (*pp) {
        if ((*pp)->png_ptr == png_ptr) {
            se_png_ctx_t *found = *pp;
            *pp = found->next;
            found->next = NULL;
            spin_unlock(&g_png_lock);
            return found;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&g_png_lock);
    return NULL;
}

static void se_png_error_cb(png_structp png_ptr, png_const_charp msg) {
    PT_LOG("png error: %s", msg ? msg : "(null)");
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_error_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_err_fn && tls_png_cpu) {
        se_hle_call_guest(tls_png_cpu, ctx->guest_err_fn, 2,
                          (uint64_t)(uintptr_t)png_ptr,
                          (uint64_t)(uintptr_t)msg, 0, 0);
    }
    /* Return cleanly so libpng proceeds to png_longjmp(png_ptr, 1). */
}

static void se_png_warn_cb(png_structp png_ptr, png_const_charp msg) {
    PT_LOG("png warn: %s", msg ? msg : "(null)");
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_error_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_warn_fn && tls_png_cpu) {
        se_hle_call_guest(tls_png_cpu, ctx->guest_warn_fn, 2,
                          (uint64_t)(uintptr_t)png_ptr,
                          (uint64_t)(uintptr_t)msg, 0, 0);
    }
}

static void se_png_read_cb(png_structp png_ptr, png_bytep data, size_t length) {
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_io_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_read_fn && tls_png_cpu) {
        se_hle_call_guest(tls_png_cpu, ctx->guest_read_fn, 3,
                          (uint64_t)(uintptr_t)png_ptr,
                          (uint64_t)(uintptr_t)data,
                          (uint64_t)length, 0);
    } else {
        png_error(png_ptr, "PNG read callback missing");
    }
}

static void se_png_write_cb(png_structp png_ptr, png_bytep data, size_t length) {
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_io_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_write_fn && tls_png_cpu) {
        se_hle_call_guest(tls_png_cpu, ctx->guest_write_fn, 3,
                          (uint64_t)(uintptr_t)png_ptr,
                          (uint64_t)(uintptr_t)data,
                          (uint64_t)length, 0);
    } else {
        png_error(png_ptr, "PNG write callback missing");
    }
}

static void se_png_flush_cb(png_structp png_ptr) {
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_io_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_flush_fn && tls_png_cpu) {
        se_hle_call_guest(tls_png_cpu, ctx->guest_flush_fn, 1,
                          (uint64_t)(uintptr_t)png_ptr, 0, 0, 0);
    }
}

static png_voidp se_png_malloc_cb(png_structp png_ptr, png_alloc_size_t size) {
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_mem_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_malloc_fn && tls_png_cpu) {
        uint64_t p = se_hle_call_guest(tls_png_cpu, ctx->guest_malloc_fn, 2,
                                       (uint64_t)(uintptr_t)png_ptr,
                                       (uint64_t)size, 0, 0);
        return (png_voidp)(uintptr_t)p;
    }
    return malloc((size_t)size);
}

static void se_png_free_cb(png_structp png_ptr, png_voidp ptr) {
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_mem_ptr(png_ptr);
    if ((!ctx || ctx->magic != SE_PNG_CTX_MAGIC) && png_ptr) {
        ctx = se_png_find(png_ptr);
    }
    if (ctx && ctx->guest_free_fn && tls_png_cpu) {
        se_hle_call_guest(tls_png_cpu, ctx->guest_free_fn, 2,
                          (uint64_t)(uintptr_t)png_ptr,
                          (uint64_t)(uintptr_t)ptr, 0, 0);
        return;
    }
    free(ptr);
}

static void hle_png_sig_cmp(CPU *c) {
    png_const_bytep sig = (png_const_bytep)(uintptr_t)HLE_ARG0(c);
    size_t start = (size_t)HLE_ARG1(c);
    size_t num_to_check = (size_t)HLE_ARG2(c);
    int r = sig ? png_sig_cmp(sig, start, num_to_check) : -1;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_png_set_sig_bytes(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    int num_bytes = (int)(int32_t)HLE_ARG1(c);
    if (!png_ptr) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_set_sig_bytes(png_ptr, num_bytes);
    }
    tls_png_cpu = prev;
}

static void hle_png_create_struct_2_common(CPU *c, bool is_write) {
    png_const_charp ver = (png_const_charp)(uintptr_t)HLE_ARG0(c);
    uint64_t guest_err_ptr = HLE_ARG1(c);
    uint64_t guest_err_fn = HLE_ARG2(c);
    uint64_t guest_warn_fn = HLE_ARG3(c);
    uint64_t guest_mem_ptr = hle_stack_arg(c, 4u);
    uint64_t guest_malloc_fn = hle_stack_arg(c, 5u);
    uint64_t guest_free_fn = hle_stack_arg(c, 6u);

    se_png_ctx_t *ctx = (se_png_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        HLE_RET64(c, 0);
        return;
    }
    ctx->magic = SE_PNG_CTX_MAGIC;
    ctx->guest_err_ptr = guest_err_ptr;
    ctx->guest_err_fn = guest_err_fn;
    ctx->guest_warn_fn = guest_warn_fn;
    ctx->guest_mem_ptr = guest_mem_ptr;
    ctx->guest_malloc_fn = guest_malloc_fn;
    ctx->guest_free_fn = guest_free_fn;

    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    bool use_guest_mem = (guest_malloc_fn != 0 && guest_free_fn != 0);
    png_structp png_ptr = is_write
        ? png_create_write_struct_2(
              ver,
              ctx, se_png_error_cb, se_png_warn_cb,
              use_guest_mem ? ctx : NULL,
              use_guest_mem ? se_png_malloc_cb : NULL,
              use_guest_mem ? se_png_free_cb : NULL)
        : png_create_read_struct_2(
              ver,
              ctx, se_png_error_cb, se_png_warn_cb,
              use_guest_mem ? ctx : NULL,
              use_guest_mem ? se_png_malloc_cb : NULL,
              use_guest_mem ? se_png_free_cb : NULL);
    tls_png_cpu = prev;

    if (!png_ptr) {
        free(ctx);
        HLE_RET64(c, 0);
        return;
    }
    ctx->png_ptr = png_ptr;
    se_png_track(ctx);
    HLE_RET_PTR(c, png_ptr);
}

static void hle_png_create_read_struct_2(CPU *c) {
    hle_png_create_struct_2_common(c, false);
}

static void hle_png_create_write_struct_2(CPU *c) {
    hle_png_create_struct_2_common(c, true);
}

static void hle_png_create_info_struct(CPU *c) {
    png_const_structrp png_ptr = (png_const_structrp)(uintptr_t)HLE_ARG0(c);
    if (!png_ptr) {
        HLE_RET64(c, 0);
        return;
    }
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    png_infop volatile info = NULL;
    if (setjmp(png_jmpbuf((png_structp)png_ptr)) == 0) {
        info = png_create_info_struct(png_ptr);
    }
    tls_png_cpu = prev;
    HLE_RET_PTR(c, info);
}

static void hle_png_destroy_read_struct(CPU *c) {
    uint64_t *pptr_va = (uint64_t *)(uintptr_t)HLE_ARG0(c);
    uint64_t *iptr_va = (uint64_t *)(uintptr_t)HLE_ARG1(c);
    uint64_t *eptr_va = (uint64_t *)(uintptr_t)HLE_ARG2(c);

    png_structp png_ptr = pptr_va ? (png_structp)(uintptr_t)*pptr_va : NULL;
    png_infop info_ptr = iptr_va ? (png_infop)(uintptr_t)*iptr_va : NULL;
    png_infop end_ptr = eptr_va ? (png_infop)(uintptr_t)*eptr_va : NULL;
    se_png_ctx_t *ctx = se_png_untrack(png_ptr);

    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    png_destroy_read_struct(pptr_va ? &png_ptr : NULL,
                            iptr_va ? &info_ptr : NULL,
                            eptr_va ? &end_ptr : NULL);
    tls_png_cpu = prev;

    if (pptr_va) *pptr_va = (uint64_t)(uintptr_t)png_ptr;
    if (iptr_va) *iptr_va = (uint64_t)(uintptr_t)info_ptr;
    if (eptr_va) *eptr_va = (uint64_t)(uintptr_t)end_ptr;
    free(ctx);
}

static void hle_png_destroy_write_struct(CPU *c) {
    uint64_t *pptr_va = (uint64_t *)(uintptr_t)HLE_ARG0(c);
    uint64_t *iptr_va = (uint64_t *)(uintptr_t)HLE_ARG1(c);

    png_structp png_ptr = pptr_va ? (png_structp)(uintptr_t)*pptr_va : NULL;
    png_infop info_ptr = iptr_va ? (png_infop)(uintptr_t)*iptr_va : NULL;
    se_png_ctx_t *ctx = se_png_untrack(png_ptr);

    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    png_destroy_write_struct(pptr_va ? &png_ptr : NULL,
                             iptr_va ? &info_ptr : NULL);
    tls_png_cpu = prev;

    if (pptr_va) *pptr_va = (uint64_t)(uintptr_t)png_ptr;
    if (iptr_va) *iptr_va = (uint64_t)(uintptr_t)info_ptr;
    free(ctx);
}

static void hle_png_set_read_fn(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    uint64_t io_ptr = HLE_ARG1(c);
    uint64_t read_fn = HLE_ARG2(c);
    if (!png_ptr) return;

    se_png_ctx_t *ctx = se_png_find(png_ptr);
    if (!ctx) {
        ctx = (se_png_ctx_t *)calloc(1, sizeof(*ctx));
        if (!ctx) return;
        ctx->magic = SE_PNG_CTX_MAGIC;
        ctx->png_ptr = png_ptr;
        se_png_track(ctx);
    }
    ctx->guest_io_ptr = io_ptr;
    ctx->guest_read_fn = read_fn;
    png_set_read_fn(png_ptr, ctx, read_fn ? se_png_read_cb : NULL);
}

static void hle_png_set_write_fn(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    uint64_t io_ptr = HLE_ARG1(c);
    uint64_t write_fn = HLE_ARG2(c);
    uint64_t flush_fn = HLE_ARG3(c);
    if (!png_ptr) return;

    se_png_ctx_t *ctx = se_png_find(png_ptr);
    if (!ctx) {
        ctx = (se_png_ctx_t *)calloc(1, sizeof(*ctx));
        if (!ctx) return;
        ctx->magic = SE_PNG_CTX_MAGIC;
        ctx->png_ptr = png_ptr;
        se_png_track(ctx);
    }
    ctx->guest_io_ptr = io_ptr;
    ctx->guest_write_fn = write_fn;
    ctx->guest_flush_fn = flush_fn;
    png_set_write_fn(png_ptr, ctx,
                     write_fn ? se_png_write_cb : NULL,
                     se_png_flush_cb);
}

static void hle_png_get_io_ptr(CPU *c) {
    png_const_structrp png_ptr = (png_const_structrp)(uintptr_t)HLE_ARG0(c);
    if (!png_ptr) {
        HLE_RET64(c, 0);
        return;
    }
    se_png_ctx_t *ctx = (se_png_ctx_t *)png_get_io_ptr(png_ptr);
    if (ctx && ctx->magic == SE_PNG_CTX_MAGIC && ctx->png_ptr == png_ptr) {
        HLE_RET64(c, ctx->guest_io_ptr);
        return;
    }
    ctx = se_png_find(png_ptr);
    if (ctx) {
        HLE_RET64(c, ctx->guest_io_ptr);
        return;
    }
    HLE_RET_PTR(c, png_get_io_ptr(png_ptr));
}

static void hle_png_read_info(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    png_inforp info_ptr = (png_inforp)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr || !info_ptr) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_read_info(png_ptr, info_ptr);
    }
    tls_png_cpu = prev;
}

static void hle_png_get_IHDR(CPU *c) {
    png_const_structrp png_ptr = (png_const_structrp)(uintptr_t)HLE_ARG0(c);
    png_const_inforp info_ptr = (png_const_inforp)(uintptr_t)HLE_ARG1(c);
    png_uint_32 *w = (png_uint_32 *)(uintptr_t)HLE_ARG2(c);
    png_uint_32 *h = (png_uint_32 *)(uintptr_t)HLE_ARG3(c);
    int *bd = (int *)(uintptr_t)hle_stack_arg(c, 4u);
    int *ct = (int *)(uintptr_t)hle_stack_arg(c, 5u);
    int *im = (int *)(uintptr_t)hle_stack_arg(c, 6u);
    int *cm = (int *)(uintptr_t)hle_stack_arg(c, 7u);
    int *fm = (int *)(uintptr_t)hle_stack_arg(c, 8u);
    if (!png_ptr || !info_ptr) {
        HLE_RET32(c, 0);
        return;
    }
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    png_uint_32 volatile r = 0;
    if (setjmp(png_jmpbuf((png_structp)png_ptr)) == 0) {
        r = png_get_IHDR(png_ptr, info_ptr, w, h, bd, ct, im, cm, fm);
    }
    tls_png_cpu = prev;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_png_set_IHDR(CPU *c) {
    png_const_structrp png_ptr = (png_const_structrp)(uintptr_t)HLE_ARG0(c);
    png_inforp info_ptr = (png_inforp)(uintptr_t)HLE_ARG1(c);
    png_uint_32 w = (png_uint_32)HLE_ARG2(c);
    png_uint_32 h = (png_uint_32)HLE_ARG3(c);
    int bd = (int)(int32_t)hle_stack_arg(c, 4u);
    int ct = (int)(int32_t)hle_stack_arg(c, 5u);
    int im = (int)(int32_t)hle_stack_arg(c, 6u);
    int cm = (int)(int32_t)hle_stack_arg(c, 7u);
    int fm = (int)(int32_t)hle_stack_arg(c, 8u);
    if (!png_ptr || !info_ptr) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf((png_structp)png_ptr)) == 0) {
        png_set_IHDR(png_ptr, info_ptr, w, h, bd, ct, im, cm, fm);
    }
    tls_png_cpu = prev;
}

static void hle_png_get_channels(CPU *c) {
    png_const_structrp png_ptr = (png_const_structrp)(uintptr_t)HLE_ARG0(c);
    png_const_inforp info_ptr = (png_const_inforp)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr || !info_ptr) {
        HLE_RET32(c, 0);
        return;
    }
    HLE_RET32(c, (uint32_t)png_get_channels(png_ptr, info_ptr));
}

static void hle_png_read_image(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    png_bytepp image = (png_bytepp)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr || !image) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_read_image(png_ptr, image);
    }
    tls_png_cpu = prev;
}

static void hle_png_read_end(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    png_inforp info_ptr = (png_inforp)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_read_end(png_ptr, info_ptr);
    }
    tls_png_cpu = prev;
}

static void hle_png_write_info(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    png_const_inforp info_ptr = (png_const_inforp)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr || !info_ptr) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_write_info(png_ptr, info_ptr);
    }
    tls_png_cpu = prev;
}

static void hle_png_write_row(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    png_const_bytep row = (png_const_bytep)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr || !row) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_write_row(png_ptr, row);
    }
    tls_png_cpu = prev;
}

static void hle_png_write_end(CPU *c) {
    png_structrp png_ptr = (png_structrp)(uintptr_t)HLE_ARG0(c);
    png_inforp info_ptr = (png_inforp)(uintptr_t)HLE_ARG1(c);
    if (!png_ptr) return;
    CPU * volatile prev = tls_png_cpu;
    tls_png_cpu = c;
    if (setjmp(png_jmpbuf(png_ptr)) == 0) {
        png_write_end(png_ptr, info_ptr);
    }
    tls_png_cpu = prev;
}

/* ========================================================================= */
/* tiff.dll (11 imports)                                                     */
/* ========================================================================= */

typedef struct se_tiff_ctx {
    TIFF *tif;
    uint64_t guest_clientdata;
    uint64_t readproc;
    uint64_t writeproc;
    uint64_t seekproc;
    uint64_t closeproc;
    uint64_t sizeproc;
    uint64_t mapproc;
    uint64_t unmapproc;
    struct se_tiff_ctx *next;
} se_tiff_ctx_t;

static se_tiff_ctx_t *g_tiff_list = NULL;
static uint64_t g_tiff_guest_error_handler = 0;
static _Thread_local CPU *tls_tiff_cpu = NULL;

static void se_tiff_track(se_tiff_ctx_t *ctx) {
    spin_lock(&g_tiff_lock);
    ctx->next = g_tiff_list;
    g_tiff_list = ctx;
    spin_unlock(&g_tiff_lock);
}

static se_tiff_ctx_t *se_tiff_untrack(TIFF *tif) {
    if (!tif) return NULL;
    spin_lock(&g_tiff_lock);
    se_tiff_ctx_t **pp = &g_tiff_list;
    while (*pp) {
        if ((*pp)->tif == tif) {
            se_tiff_ctx_t *found = *pp;
            *pp = found->next;
            found->next = NULL;
            spin_unlock(&g_tiff_lock);
            return found;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&g_tiff_lock);
    return NULL;
}

static void se_tiff_error_cb(const char *module, const char *fmt, va_list ap) {
    char formatted[512];
    formatted[0] = '\0';
    if (fmt) {
        vsnprintf(formatted, sizeof(formatted), fmt, ap);
    }
    PT_LOG("tiff error [%s]: %s", module ? module : "", formatted);

    uint64_t fn = g_tiff_guest_error_handler;
    if (fn && tls_tiff_cpu) {
        /* SpaceEngine's error handler reads (module, fmt, win64_va_list) and
         * converts %s -> %S before calling wide vswprintf_s. Passing fmt="%s"
         * with a 1-slot Win64 va_list eliminates ARM64 __va_list ABI skew. */
        uint64_t win64_va[1] = { (uint64_t)(uintptr_t)formatted };
        const char *pass_fmt = "%s";
        se_hle_call_guest(tls_tiff_cpu, fn, 3,
                          (uint64_t)(uintptr_t)module,
                          (uint64_t)(uintptr_t)pass_fmt,
                          (uint64_t)(uintptr_t)win64_va, 0);
    }
}

static tmsize_t se_tiff_read_cb(thandle_t fd, void *buf, tmsize_t size) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->readproc || !tls_tiff_cpu) return -1;
    uint64_t r = se_hle_call_guest(tls_tiff_cpu, ctx->readproc, 3,
                                   ctx->guest_clientdata,
                                   (uint64_t)(uintptr_t)buf,
                                   (uint64_t)size, 0);
    return (tmsize_t)(int64_t)r;
}

static tmsize_t se_tiff_write_cb(thandle_t fd, void *buf, tmsize_t size) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->writeproc || !tls_tiff_cpu) return -1;
    uint64_t r = se_hle_call_guest(tls_tiff_cpu, ctx->writeproc, 3,
                                   ctx->guest_clientdata,
                                   (uint64_t)(uintptr_t)buf,
                                   (uint64_t)size, 0);
    return (tmsize_t)(int64_t)r;
}

static toff_t se_tiff_seek_cb(thandle_t fd, toff_t off, int whence) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->seekproc || !tls_tiff_cpu) return (toff_t)-1;
    uint64_t r = se_hle_call_guest(tls_tiff_cpu, ctx->seekproc, 3,
                                   ctx->guest_clientdata,
                                   (uint64_t)off,
                                   (uint64_t)(uint32_t)whence, 0);
    return (toff_t)r;
}

static int se_tiff_close_cb(thandle_t fd) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->closeproc || !tls_tiff_cpu) return 0;
    uint64_t r = se_hle_call_guest(tls_tiff_cpu, ctx->closeproc, 1,
                                   ctx->guest_clientdata, 0, 0, 0);
    return (int)(int32_t)r;
}

static toff_t se_tiff_size_cb(thandle_t fd) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->sizeproc || !tls_tiff_cpu) return 0;
    uint64_t r = se_hle_call_guest(tls_tiff_cpu, ctx->sizeproc, 1,
                                   ctx->guest_clientdata, 0, 0, 0);
    return (toff_t)r;
}

static int se_tiff_map_cb(thandle_t fd, void **pbase, toff_t *psize) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->mapproc || !tls_tiff_cpu) return 0;
    uint64_t r = se_hle_call_guest(tls_tiff_cpu, ctx->mapproc, 3,
                                   ctx->guest_clientdata,
                                   (uint64_t)(uintptr_t)pbase,
                                   (uint64_t)(uintptr_t)psize, 0);
    return (int)(int32_t)r;
}

static void se_tiff_unmap_cb(thandle_t fd, void *base, toff_t size) {
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)fd;
    if (!ctx || !ctx->unmapproc || !tls_tiff_cpu) return;
    se_hle_call_guest(tls_tiff_cpu, ctx->unmapproc, 3,
                      ctx->guest_clientdata,
                      (uint64_t)(uintptr_t)base,
                      (uint64_t)size, 0);
}

static void hle_TIFFSetErrorHandler(CPU *c) {
    uint64_t new_fn = HLE_ARG0(c);
    uint64_t old_fn = g_tiff_guest_error_handler;
    g_tiff_guest_error_handler = new_fn;
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    TIFFSetErrorHandler(se_tiff_error_cb);
    tls_tiff_cpu = prev;
    HLE_RET64(c, old_fn);
}

static void hle_TIFFOpen(CPU *c) {
    const char *win_path = (const char *)(uintptr_t)HLE_ARG0(c);
    const char *mode = (const char *)(uintptr_t)HLE_ARG1(c);
    if (!win_path || !mode) {
        HLE_RET64(c, 0);
        return;
    }
    bool for_write = (strchr(mode, 'w') != NULL ||
                      strchr(mode, 'a') != NULL ||
                      strchr(mode, '+') != NULL);
    char host_path[1024];
    const char *use_path = win_path;
    if (se_vfs_resolve_path(win_path, for_write, host_path, sizeof(host_path))) {
        use_path = host_path;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    TIFF *tif = TIFFOpen(use_path, mode);
    tls_tiff_cpu = prev;
    HLE_RET_PTR(c, tif);
}

static void hle_TIFFClientOpen(CPU *c) {
    const char *name = (const char *)(uintptr_t)HLE_ARG0(c);
    const char *mode = (const char *)(uintptr_t)HLE_ARG1(c);
    uint64_t clientdata = HLE_ARG2(c);
    uint64_t readproc = HLE_ARG3(c);
    uint64_t writeproc = hle_stack_arg(c, 4u);
    uint64_t seekproc = hle_stack_arg(c, 5u);
    uint64_t closeproc = hle_stack_arg(c, 6u);
    uint64_t sizeproc = hle_stack_arg(c, 7u);
    uint64_t mapproc = hle_stack_arg(c, 8u);
    uint64_t unmapproc = hle_stack_arg(c, 9u);

    if (!name || !mode) {
        HLE_RET64(c, 0);
        return;
    }
    se_tiff_ctx_t *ctx = (se_tiff_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        HLE_RET64(c, 0);
        return;
    }
    ctx->guest_clientdata = clientdata;
    ctx->readproc = readproc;
    ctx->writeproc = writeproc;
    ctx->seekproc = seekproc;
    ctx->closeproc = closeproc;
    ctx->sizeproc = sizeproc;
    ctx->mapproc = mapproc;
    ctx->unmapproc = unmapproc;

    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    TIFF *tif = TIFFClientOpen(
        name, mode, (thandle_t)ctx,
        se_tiff_read_cb, se_tiff_write_cb, se_tiff_seek_cb,
        se_tiff_close_cb, se_tiff_size_cb, se_tiff_map_cb, se_tiff_unmap_cb);
    tls_tiff_cpu = prev;

    if (!tif) {
        free(ctx);
        HLE_RET64(c, 0);
        return;
    }
    ctx->tif = tif;
    se_tiff_track(ctx);
    HLE_RET_PTR(c, tif);
}

static void hle_TIFFClose(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    if (!tif) return;
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    TIFFClose(tif);
    tls_tiff_cpu = prev;
    se_tiff_ctx_t *ctx = se_tiff_untrack(tif);
    free(ctx);
}

static int tiff_get_field_dispatch(TIFF *tif, uint32_t tag, CPU *c, bool defaulted) {
    void *p0 = (void *)(uintptr_t)HLE_ARG2(c);
    void *p1 = (void *)(uintptr_t)HLE_ARG3(c);
    void *p2 = (void *)(uintptr_t)hle_stack_arg(c, 4u);

    const TIFFField *fip = TIFFFindField(tif, tag, TIFF_ANY);
    int argc_ptr = 1;
    if (tag == TIFFTAG_COLORMAP || tag == TIFFTAG_TRANSFERFUNCTION) {
        argc_ptr = 3;
    } else if (tag == TIFFTAG_PAGENUMBER || tag == TIFFTAG_HALFTONEHINTS ||
               tag == TIFFTAG_YCBCRSUBSAMPLING || tag == TIFFTAG_DOTRANGE) {
        argc_ptr = 2;
    } else if (fip && TIFFFieldPassCount(fip)) {
        argc_ptr = 2;
    }

    if (argc_ptr == 3) {
        return defaulted ? TIFFGetFieldDefaulted(tif, tag, p0, p1, p2)
                         : TIFFGetField(tif, tag, p0, p1, p2);
    }
    if (argc_ptr == 2) {
        return defaulted ? TIFFGetFieldDefaulted(tif, tag, p0, p1)
                         : TIFFGetField(tif, tag, p0, p1);
    }
    return defaulted ? TIFFGetFieldDefaulted(tif, tag, p0)
                     : TIFFGetField(tif, tag, p0);
}

static void hle_TIFFGetField(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    uint32_t tag = (uint32_t)HLE_ARG1(c);
    if (!tif) {
        HLE_RET32(c, 0);
        return;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    int r = tiff_get_field_dispatch(tif, tag, c, false);
    tls_tiff_cpu = prev;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_TIFFGetFieldDefaulted(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    uint32_t tag = (uint32_t)HLE_ARG1(c);
    if (!tif) {
        HLE_RET32(c, 0);
        return;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    int r = tiff_get_field_dispatch(tif, tag, c, true);
    tls_tiff_cpu = prev;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_TIFFSetField(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    uint32_t tag = (uint32_t)HLE_ARG1(c);
    if (!tif) {
        HLE_RET32(c, 0);
        return;
    }

    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    const TIFFField *fip = TIFFFindField(tif, tag, TIFF_ANY);
    int r = 0;

    if (tag == TIFFTAG_COLORMAP || tag == TIFFTAG_TRANSFERFUNCTION) {
        r = TIFFSetField(tif, tag,
                         (void *)(uintptr_t)HLE_ARG2(c),
                         (void *)(uintptr_t)HLE_ARG3(c),
                         (void *)(uintptr_t)hle_stack_arg(c, 4u));
    } else if (tag == TIFFTAG_PAGENUMBER || tag == TIFFTAG_HALFTONEHINTS ||
               tag == TIFFTAG_YCBCRSUBSAMPLING || tag == TIFFTAG_DOTRANGE) {
        r = TIFFSetField(tif, tag,
                         (uint32_t)HLE_ARG2(c),
                         (uint32_t)HLE_ARG3(c));
    } else if (fip && TIFFFieldPassCount(fip)) {
        r = TIFFSetField(tif, tag,
                         (uint32_t)HLE_ARG2(c),
                         (void *)(uintptr_t)HLE_ARG3(c));
    } else if (fip) {
        TIFFDataType dt = TIFFFieldDataType(fip);
        int wc = TIFFFieldWriteCount(fip);
        if (wc != 1) {
            r = TIFFSetField(tif, tag, (void *)(uintptr_t)HLE_ARG2(c));
        } else if (dt == TIFF_FLOAT || dt == TIFF_DOUBLE ||
                   dt == TIFF_RATIONAL || dt == TIFF_SRATIONAL) {
            r = TIFFSetField(tif, tag, HLE_ARG_F64(c, 2));
        } else if (dt == TIFF_ASCII) {
            r = TIFFSetField(tif, tag, (const char *)(uintptr_t)HLE_ARG2(c));
        } else if (dt == TIFF_LONG8 || dt == TIFF_SLONG8 || dt == TIFF_IFD8) {
            r = TIFFSetField(tif, tag, (uint64_t)HLE_ARG2(c));
        } else {
            r = TIFFSetField(tif, tag, (uint32_t)HLE_ARG2(c));
        }
    } else {
        r = TIFFSetField(tif, tag, (uint32_t)HLE_ARG2(c));
    }

    tls_tiff_cpu = prev;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_TIFFScanlineSize(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    if (!tif) {
        HLE_RET64(c, 0);
        return;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    tmsize_t sz = TIFFScanlineSize(tif);
    tls_tiff_cpu = prev;
    HLE_RET64(c, (uint64_t)(int64_t)sz);
}

static void hle_TIFFDefaultStripSize(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    uint32_t request = (uint32_t)HLE_ARG1(c);
    if (!tif) {
        HLE_RET32(c, 0);
        return;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    uint32_t r = TIFFDefaultStripSize(tif, request);
    tls_tiff_cpu = prev;
    HLE_RET32(c, r);
}

static void hle_TIFFReadScanline(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    void *buf = (void *)(uintptr_t)HLE_ARG1(c);
    uint32_t row = (uint32_t)HLE_ARG2(c);
    uint16_t sample = (uint16_t)HLE_ARG3(c);
    if (!tif || !buf) {
        HLE_RET32(c, (uint32_t)-1);
        return;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    int r = TIFFReadScanline(tif, buf, row, sample);
    tls_tiff_cpu = prev;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_TIFFWriteScanline(CPU *c) {
    TIFF *tif = (TIFF *)(uintptr_t)HLE_ARG0(c);
    void *buf = (void *)(uintptr_t)HLE_ARG1(c);
    uint32_t row = (uint32_t)HLE_ARG2(c);
    uint16_t sample = (uint16_t)HLE_ARG3(c);
    if (!tif || !buf) {
        HLE_RET32(c, (uint32_t)-1);
        return;
    }
    CPU *prev = tls_tiff_cpu;
    tls_tiff_cpu = c;
    int r = TIFFWriteScanline(tif, buf, row, sample);
    tls_tiff_cpu = prev;
    HLE_RET32(c, (uint32_t)r);
}

void se_register_png_tiff_shims(void) {
    /* libpng16.dll (19) */
    se_hle_register_import("libpng16.dll", "png_sig_cmp", hle_png_sig_cmp);
    se_hle_register_import("libpng16.dll", "png_set_sig_bytes", hle_png_set_sig_bytes);
    se_hle_register_import("libpng16.dll", "png_create_read_struct_2", hle_png_create_read_struct_2);
    se_hle_register_import("libpng16.dll", "png_create_write_struct_2", hle_png_create_write_struct_2);
    se_hle_register_import("libpng16.dll", "png_create_info_struct", hle_png_create_info_struct);
    se_hle_register_import("libpng16.dll", "png_destroy_read_struct", hle_png_destroy_read_struct);
    se_hle_register_import("libpng16.dll", "png_destroy_write_struct", hle_png_destroy_write_struct);
    se_hle_register_import("libpng16.dll", "png_set_read_fn", hle_png_set_read_fn);
    se_hle_register_import("libpng16.dll", "png_set_write_fn", hle_png_set_write_fn);
    se_hle_register_import("libpng16.dll", "png_get_io_ptr", hle_png_get_io_ptr);
    se_hle_register_import("libpng16.dll", "png_read_info", hle_png_read_info);
    se_hle_register_import("libpng16.dll", "png_get_IHDR", hle_png_get_IHDR);
    se_hle_register_import("libpng16.dll", "png_set_IHDR", hle_png_set_IHDR);
    se_hle_register_import("libpng16.dll", "png_get_channels", hle_png_get_channels);
    se_hle_register_import("libpng16.dll", "png_read_image", hle_png_read_image);
    se_hle_register_import("libpng16.dll", "png_read_end", hle_png_read_end);
    se_hle_register_import("libpng16.dll", "png_write_info", hle_png_write_info);
    se_hle_register_import("libpng16.dll", "png_write_row", hle_png_write_row);
    se_hle_register_import("libpng16.dll", "png_write_end", hle_png_write_end);

    /* tiff.dll (11) */
    se_hle_register_import("tiff.dll", "TIFFSetErrorHandler", hle_TIFFSetErrorHandler);
    se_hle_register_import("tiff.dll", "TIFFOpen", hle_TIFFOpen);
    se_hle_register_import("tiff.dll", "TIFFClientOpen", hle_TIFFClientOpen);
    se_hle_register_import("tiff.dll", "TIFFClose", hle_TIFFClose);
    se_hle_register_import("tiff.dll", "TIFFGetField", hle_TIFFGetField);
    se_hle_register_import("tiff.dll", "TIFFGetFieldDefaulted", hle_TIFFGetFieldDefaulted);
    se_hle_register_import("tiff.dll", "TIFFSetField", hle_TIFFSetField);
    se_hle_register_import("tiff.dll", "TIFFScanlineSize", hle_TIFFScanlineSize);
    se_hle_register_import("tiff.dll", "TIFFDefaultStripSize", hle_TIFFDefaultStripSize);
    se_hle_register_import("tiff.dll", "TIFFReadScanline", hle_TIFFReadScanline);
    se_hle_register_import("tiff.dll", "TIFFWriteScanline", hle_TIFFWriteScanline);
}
