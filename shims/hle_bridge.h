/*
 * hle_bridge.h - Win64-to-native HLE dispatch bridge, argument helpers, and VFS.
 */
#ifndef SE_SHIMS_HLE_BRIDGE_H
#define SE_SHIMS_HLE_BRIDGE_H

#include "cpu64.h"
#include "recomp_dispatch.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SE_HLE_TOTAL_SLOTS 4096u

typedef void (*se_hle_fn_t)(CPU *c);

/* Win64 ABI argument helpers (valid on entry to HLE handler, before popping ret_pc). */
#define HLE_ARG0(c) ((c)->rcx)
#define HLE_ARG1(c) ((c)->rdx)
#define HLE_ARG2(c) ((c)->r8)
#define HLE_ARG3(c) ((c)->r9)

static inline uint64_t hle_stack_arg(const CPU *c, uint32_t zero_based_idx) {
    /* [rsp+0] = ret_pc; [rsp+8..+0x20] = shadow space; [rsp+0x28] = arg index 4 */
    return rd64(c->rsp + 8ull + (uint64_t)zero_based_idx * 8ull);
}

#define HLE_ARG4(c) hle_stack_arg((c), 4u)
#define HLE_ARG5(c) hle_stack_arg((c), 5u)
#define HLE_ARG6(c) hle_stack_arg((c), 6u)
#define HLE_ARG7(c) hle_stack_arg((c), 7u)

#define HLE_ARG_F32(c, idx) ((c)->xmm[(idx) & 15].f32[0])
#define HLE_ARG_F64(c, idx) ((c)->xmm[(idx) & 15].f64[0])

#define HLE_RET64(c, v) SET64((c)->rax, (uint64_t)(v))
#define HLE_RET32(c, v) SET32((c)->rax, (uint32_t)(v))
#define HLE_RET_PTR(c, p) SET64((c)->rax, (uint64_t)(uintptr_t)(p))
#define HLE_RET_F32(c, f) do { (c)->xmm[0].u64[0] = 0; (c)->xmm[0].u64[1] = 0; (c)->xmm[0].f32[0] = (float)(f); } while (0)
#define HLE_RET_F64(c, d) do { (c)->xmm[0].u64[1] = 0; (c)->xmm[0].f64[0] = (double)(d); } while (0)

/* Initialize the HLE registry, bind all 420 static IAT slots, and register subsystem shims. */
void se_hle_init(void);

/* Configure runtime directories for Win32/VFS path translation. */
void se_hle_set_paths(const char *game_dir, const char *internal_dir, const char *exe_path);
const char *se_hle_get_game_dir(void);
const char *se_hle_get_internal_dir(void);
const char *se_hle_get_exe_path(void);

/* Configure optional Phase 4 startup hooks (disabled by default in unit tests). */
void se_hle_set_android_hooks(int enabled);

/* Register an HLE handler for a Win64 import (by exact DLL name and export/ordinal name). */
void se_hle_register_import(const char *dll, const char *name, se_hle_fn_t fn);

/* Allocate or lookup a dynamic HLE sentinel (indices 420..2047) for GetProcAddress/wglGetProcAddress. */
uint64_t se_hle_register_dynamic(const char *dll, const char *name, se_hle_fn_t fn);
uint64_t se_hle_find_sentinel(const char *dll, const char *name);
uint32_t se_hle_unregistered_count(void);

/* Invoke a static or dynamic sentinel from dispatch()/dispatch_jmp(). Returns true if handled. */
bool se_hle_invoke_sentinel(CPU *c, uint64_t target);

/* Address normalization & CRT/hook intercept check used by dispatch()/dispatch_jmp(). */
uint64_t se_hle_normalize_target(uint64_t target);
bool se_hle_try_intercept(CPU *c, uint64_t canonical_target);

/* Invoke a Win64 guest function pointer/callback from native HLE code. */
uint64_t se_hle_call_guest(CPU *c, uint64_t fn_va, int argc,
                           uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

/* RaiseException handler signal bit: allows RaiseException to leave RSP pre-popped on throw. */
void se_hle_suppress_ret_pop_once(void);
uint32_t se_hle_active_slot_index(void);

/* Phase 4 stage polling & auto-planetarium transition controls. */
typedef void (*se_hle_stage_cb_t)(int stage, const char *message, void *user);
void se_hle_set_stage_callback(se_hle_stage_cb_t cb, void *user);
void se_hle_set_auto_planetarium(bool enabled);
void se_hle_poll_stage(void);
void se_hle_reset_boot_state(void);

/* UTF-16LE <-> UTF-8 conversion helpers (Android Bionic wchar_t is 32-bit; guest WCHAR is 16-bit). */
int se_utf16_to_utf8(const uint16_t *wstr, int wlen, char *out_utf8, int out_cap);
int se_utf8_to_utf16(const char *utf8, int utf8_len, uint16_t *out_w, int out_wcap);
size_t se_utf16_len(const uint16_t *wstr);

/* Normalize Windows paths and resolve case-insensitively on POSIX/Android under game/internal dir. */
bool se_vfs_resolve_path(const char *win_path, bool for_write, char *out_host_path, size_t out_cap);
bool se_vfs_resolve_wpath(const uint16_t *win_wpath, bool for_write, char *out_host_path, size_t out_cap);

/* Scratch guest-accessible static strings/buffers allocator (process lifetime). */
uint64_t se_hle_intern_ascii(const char *s);
uint64_t se_hle_intern_utf16(const char *s);

/* Subsystem registration entry points implemented in shims */
void se_register_kernel32_shims(void);
void se_register_misc_win32_shims(void);
void se_register_minizip_shims(void);
void se_register_user32_gdi32_shims(void);
void se_register_wgl_egl_shims(void);
void se_register_png_tiff_shims(void);
void se_register_sdl2_mixer_shims(void);

/* Phase 4/4L USER32/GDI32/ANativeWindow, splash & input controls */
void se_u32_reset_state(void);
void se_u32_set_native_window(void *anw);
void *se_u32_get_native_window(void);
void se_u32_set_splash_window(void *anw, int width, int height);
void se_u32_set_ui_callbacks(
    void (*on_splash_vis)(int visible, void *user),
    void (*on_game_vis)(void *user),
    void *user);
uint64_t se_u32_get_main_hwnd(void);
uint64_t se_u32_get_splash_hwnd(void);
const uint32_t *se_u32_get_splash_framebuffer(int *out_w, int *out_h);
void se_u32_push_touch(int action, float x, float y, int pointer_count, float pinch_delta);
void se_u32_push_key(int key_code, bool down);
void se_u32_push_char(uint32_t codepoint);

/* Phase 4 WGL/EGL/Zink & first-frame-ready controls */
void se_wgl_reset_state(void);
void se_wgl_set_ready_callback(void (*cb)(void *), void *user);
void se_wgl_set_planetarium_loaded(bool loaded);
bool se_wgl_is_planetarium_loaded(void);
bool se_wgl_was_ready_fired(void);
uint64_t se_wgl_get_current_context(void);
uint32_t se_wgl_get_swap_count(void);
#if !defined(__ANDROID__)
void se_wgl_set_mock_gl_resolver(void *(*fn)(const char *));
#endif
void se_wgl_set_desktop_gl_mode(bool desktop);
bool se_wgl_is_desktop_gl(void);
void se_wgl_set_es3_driver_caps(bool s3tc, bool rgtc, bool clip_control,
                                bool buffer_storage, bool blend_func_extended);
void se_wgl_set_shader_capture_cb(
    void (*cb)(uint32_t shader, uint32_t stage, const char *translated, void *user),
    void *user);
uint32_t se_wgl_normalize_es3_target(uint32_t target);
uint32_t se_wgl_normalize_es3_format(uint32_t format);
int32_t se_wgl_normalize_es3_tex_param_i(uint32_t pname, int32_t param);
float se_wgl_normalize_es3_tex_param_f(uint32_t pname, float param);
int32_t se_wgl_normalize_es3_internalformat(int32_t internalformat, uint32_t format,
                                            uint32_t type, bool for_storage);
uint8_t *se_wgl_alloc_es3_uncompressed_pixels(uint32_t format, uint32_t type,
                                              int32_t width, int32_t height, int32_t depth,
                                              const void *pixels,
                                              uint32_t *out_format, uint32_t *out_type);
bool se_wgl_compressed_format_needs_cpu_decode(uint32_t ifmt);
uint8_t *se_wgl_decode_bc_to_uncompressed(uint32_t internalformat, int32_t width, int32_t height,
                                          int32_t image_size, const void *data,
                                          uint32_t *out_internalformat, uint32_t *out_format,
                                          uint32_t *out_type);
uint32_t se_wgl_map_access_to_es3_range_bits(uint32_t access);

/* Runtime check invoked inside CRT intercepts. */
bool se_crt_try_intercept(CPU *c, uint64_t canonical_target);

#ifdef __cplusplus
}
#endif

#endif /* SE_SHIMS_HLE_BRIDGE_H */
