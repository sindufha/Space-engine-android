#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "engine_host.h"
#include "pe_loader.h"
#include "cpu64.h"
#include "eh64.h"
#include "recomp_dispatch.h"
#include "hle_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

typedef struct se_host_state {
    se_pe_arena_t arena;
    void *native_window;
    void *splash_window;
    int surface_w;
    int surface_h;
    int splash_w;
    int splash_h;
    bool running;
    char exe_path[1024];
    char game_dir[1024];
    char internal_dir[1024];
    se_host_callbacks_t callbacks;
} se_host_state_t;

static se_host_state_t g_host = {
    .surface_w = 2400,
    .surface_h = 1080,
    .splash_w = 2400,
    .splash_h = 1080,
    .running = false,
};

#if !defined(_WIN32)
static pthread_mutex_t g_host_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_engine_thread;
static bool g_has_thread = false;
#endif

static void lock_host(void) {
#if !defined(_WIN32)
    pthread_mutex_lock(&g_host_lock);
#endif
}

static void unlock_host(void) {
#if !defined(_WIN32)
    pthread_mutex_unlock(&g_host_lock);
#endif
}

static void normalize_landscape_dims(int w, int h, int *out_w, int *out_h) {
    if (w <= 0 || h <= 0) {
        w = 2400;
        h = 1080;
    }
    if (w < h) {
        int tmp = w;
        w = h;
        h = tmp;
    }
    *out_w = w;
    *out_h = h;
}

static void notify_stage(const se_host_callbacks_t *cb, int stage, const char *msg) {
    if (cb && cb->on_stage_changed) {
        cb->on_stage_changed(stage, msg, cb->user);
    }
}

static void host_stage_bridge(int stage, const char *message, void *user) {
    (void)user;
    se_host_callbacks_t cb;
    lock_host();
    cb = g_host.callbacks;
    unlock_host();
    if (cb.on_stage_changed) {
        cb.on_stage_changed(stage, message, cb.user);
    }
}

static void host_planetarium_ready_bridge(void *user) {
    (void)user;
    se_host_callbacks_t cb;
    lock_host();
    cb = g_host.callbacks;
    unlock_host();
    if (cb.on_planetarium_ready) {
        cb.on_planetarium_ready(cb.user);
    }
}

static void host_splash_vis_bridge(int visible, void *user) {
    (void)user;
    se_host_callbacks_t cb;
    lock_host();
    cb = g_host.callbacks;
    unlock_host();
    if (cb.on_splash_visibility) {
        cb.on_splash_visibility(visible, cb.user);
    }
}

static void host_game_surface_vis_bridge(void *user) {
    (void)user;
    se_host_callbacks_t cb;
    lock_host();
    cb = g_host.callbacks;
    unlock_host();
    if (cb.on_game_surface_visible) {
        cb.on_game_surface_visible(cb.user);
    }
}

bool se_host_is_recomp_linked(void) {
#if defined(SE_RECOMP_FULL)
    return true;
#else
    return false;
#endif
}

void se_host_set_window(void *native_window, int width, int height) {
    int sw = 0;
    int sh = 0;
    normalize_landscape_dims(width, height, &sw, &sh);
    lock_host();
    g_host.native_window = native_window;
    if (width > 0 && height > 0) {
        g_host.surface_w = sw;
        g_host.surface_h = sh;
    }
    unlock_host();
    se_u32_set_native_window(native_window);
}

void se_host_set_splash_window(void *native_window, int width, int height) {
    int sw = 0;
    int sh = 0;
    if (width > 0 && height > 0) {
        normalize_landscape_dims(width, height, &sw, &sh);
    }
    lock_host();
    g_host.splash_window = native_window;
    if (sw > 0 && sh > 0) {
        g_host.splash_w = sw;
        g_host.splash_h = sh;
    }
    unlock_host();
    se_u32_set_splash_window(native_window, sw, sh);
}

void se_host_get_surface_size(int *out_width, int *out_height) {
    lock_host();
    if (out_width) {
        *out_width = g_host.surface_w;
    }
    if (out_height) {
        *out_height = g_host.surface_h;
    }
    unlock_host();
}

static void *engine_worker_main(void *arg) {
    (void)arg;
    se_host_callbacks_t cb;
    char exe_path[1024];
    char game_dir[1024];
    char internal_dir[1024];
    int sw = 0;
    int sh = 0;

    lock_host();
    cb = g_host.callbacks;
    snprintf(exe_path, sizeof(exe_path), "%s", g_host.exe_path);
    snprintf(game_dir, sizeof(game_dir), "%s", g_host.game_dir);
    snprintf(internal_dir, sizeof(internal_dir), "%s", g_host.internal_dir);
    sw = g_host.surface_w;
    sh = g_host.surface_h;
    unlock_host();

    char err[512] = {0};
    char cfg_written[1024] = {0};

    notify_stage(&cb, 0, "Generating mobile Landscape configuration...");
    if (!se_generate_mobile_config(game_dir, internal_dir, sw, sh, cfg_written, sizeof(cfg_written), err, sizeof(err))) {
        if (cb.on_engine_error) {
            cb.on_engine_error(err, cb.user);
        }
        lock_host();
        g_host.running = false;
        unlock_host();
        return NULL;
    }

    notify_stage(&cb, 1, "Mapping SpaceEngine.exe (.rdata/.data/.pdata) arena...");
    se_pe_arena_t arena;
    if (!se_pe_map_runtime_data(exe_path, &arena, err, sizeof(err))) {
        if (cb.on_engine_error) {
            cb.on_engine_error(err, cb.user);
        }
        lock_host();
        g_host.running = false;
        unlock_host();
        return NULL;
    }

    lock_host();
    se_pe_free_arena(&g_host.arena);
    g_host.arena = arena;
    unlock_host();

    notify_stage(&cb, 2, "Initializing Win64 HLE bridge, IAT sentinels & PE-TLS...");
    se_hle_set_paths(game_dir, internal_dir, exe_path);
    se_hle_set_android_hooks(1);
    se_hle_reset_boot_state();
    se_u32_reset_state();
    se_wgl_reset_state();
    se_hle_init();
    lock_host();
    void *anw_cur = g_host.native_window;
    void *splash_cur = g_host.splash_window;
    int spl_w = g_host.splash_w;
    int spl_h = g_host.splash_h;
    unlock_host();
    se_u32_set_native_window(anw_cur);
    se_u32_set_splash_window(splash_cur, splash_cur ? spl_w : 0, splash_cur ? spl_h : 0);
    se_u32_set_ui_callbacks(host_splash_vis_bridge, host_game_surface_vis_bridge, NULL);
    se_hle_set_stage_callback(host_stage_bridge, NULL);
    se_wgl_set_ready_callback(host_planetarium_ready_bridge, NULL);

    uint64_t slot_vas[HLE_COUNT];
    for (uint32_t i = 0; i < HLE_COUNT; ++i) {
        slot_vas[i] = recomp_imports[i].slot;
    }
    if (!se_pe_bind_iat(&arena, HLE_BASE, HLE_STRIDE, slot_vas, HLE_COUNT, err, sizeof(err))) {
        if (cb.on_engine_error) {
            cb.on_engine_error(err, cb.user);
        }
        lock_host();
        g_host.running = false;
        unlock_host();
        return NULL;
    }

    es3_eh_set_image((uint64_t)(uintptr_t)arena.base, arena.size);
    se_win64_teb_set_tls_template(
        arena.base + arena.tls_raw_rva,
        arena.tls_raw_size,
        arena.tls_zero_size);

    if (!se_host_is_recomp_linked()) {
        char ready_msg[384];
        snprintf(
            ready_msg,
            sizeof(ready_msg),
            "Phase 3 Ready: Mapped %.2f MB arena (reloc=%u, TLS=%u B), bound %u/420 HLE IAT slots & Landscape %dx%d config (%s).",
            (double)arena.size / (1024.0 * 1024.0),
            (unsigned)arena.reloc_applied,
            (unsigned)arena.tls_raw_size,
            (unsigned)HLE_COUNT,
            sw,
            sh,
            cfg_written);
        notify_stage(&cb, 100, ready_msg);
        lock_host();
        g_host.running = false;
        unlock_host();
        return NULL;
    }

#if defined(SE_RECOMP_FULL)
    notify_stage(&cb, 3, "Entering lifted SpaceEngine.exe entry (0x1400C9B40)...");
    {
        const size_t stack_bytes = 2u * 1024u * 1024u;
        uint8_t *guest_stack = (uint8_t *)calloc(1u, stack_bytes);
        if (!guest_stack) {
            if (cb.on_engine_error) {
                cb.on_engine_error("Failed allocating 2 MB guest Win64 stack", cb.user);
            }
        } else {
            uint64_t stack_limit = (uint64_t)(uintptr_t)guest_stack;
            uint64_t stack_base = (stack_limit + stack_bytes - 0x40ull) & ~(uint64_t)15ull;
            se_win64_teb_set_stack(stack_base, stack_limit);

            CPU cpu;
            memset(&cpu, 0, sizeof(cpu));
            cpu.rsp = stack_base;
            cpu.mxcsr = 0x1F80u;
            push64(&cpu, 0ull);
            dispatch(&cpu, SE_EXPECTED_IMAGE_BASE + SE_EXPECTED_ENTRY_RVA);
            free(guest_stack);
            notify_stage(&cb, 100, "Lifted guest entry returned cleanly.");
        }
    }
#endif

    lock_host();
    g_host.running = false;
    unlock_host();
    return NULL;
}

bool se_host_start_engine(
    const char *exe_path,
    const char *game_dir,
    const char *internal_dir,
    int surface_w,
    int surface_h,
    const se_host_callbacks_t *callbacks,
    char *err_buf,
    size_t err_cap) {
    se_pe_info_t pe_info;
    if (!se_pe_validate_exe_path(exe_path, &pe_info, err_buf, err_cap)) {
        return false;
    }

    se_game_dir_info_t dir_info;
    if (!se_game_validate_dir(game_dir, &dir_info, err_buf, err_cap)) {
        return false;
    }

    int sw = 0;
    int sh = 0;
    normalize_landscape_dims(surface_w, surface_h, &sw, &sh);

    lock_host();
    if (g_host.running) {
        unlock_host();
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "Engine startup is already in progress");
        }
        return false;
    }
    snprintf(g_host.exe_path, sizeof(g_host.exe_path), "%s", exe_path);
    snprintf(g_host.game_dir, sizeof(g_host.game_dir), "%s", game_dir);
    snprintf(g_host.internal_dir, sizeof(g_host.internal_dir), "%s", internal_dir ? internal_dir : "");
    g_host.surface_w = sw;
    g_host.surface_h = sh;
    if (callbacks) {
        g_host.callbacks = *callbacks;
    } else {
        memset(&g_host.callbacks, 0, sizeof(g_host.callbacks));
    }
    g_host.running = true;
    unlock_host();

#if defined(_WIN32)
    engine_worker_main(NULL);
    return true;
#else
    if (g_has_thread) {
        pthread_join(g_engine_thread, NULL);
        g_has_thread = false;
    }
    if (pthread_create(&g_engine_thread, NULL, engine_worker_main, NULL) != 0) {
        lock_host();
        g_host.running = false;
        unlock_host();
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "Failed to spawn engine worker thread");
        }
        return false;
    }
    g_has_thread = true;
    return true;
#endif
}

void se_host_push_touch(int action, float x, float y, int pointer_count, float pinch_delta) {
    se_u32_push_touch(action, x, y, pointer_count, pinch_delta);
}

void se_host_push_char(unsigned int codepoint) {
    se_u32_push_char((uint32_t)codepoint);
}

void se_host_push_key(int key_code, bool down) {
    se_u32_push_key(key_code, down);
}

void se_host_shutdown(void) {
#if !defined(_WIN32)
    if (g_has_thread) {
        pthread_join(g_engine_thread, NULL);
        g_has_thread = false;
    }
#endif
    se_u32_set_ui_callbacks(NULL, NULL, NULL);
    se_hle_set_stage_callback(NULL, NULL);
    se_wgl_set_ready_callback(NULL, NULL);
    lock_host();
    g_host.running = false;
    se_pe_free_arena(&g_host.arena);
    unlock_host();
}
