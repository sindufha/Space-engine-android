#ifndef SE_ENGINE_HOST_H
#define SE_ENGINE_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callbacks delivered from the native engine host thread to UI/JNI layers. */
typedef struct se_host_callbacks {
    void (*on_stage_changed)(int stage, const char *message, void *user);
    void (*on_planetarium_ready)(void *user);
    void (*on_engine_error)(const char *message, void *user);
    void (*on_splash_visibility)(int visible, void *user);
    void (*on_game_surface_visible)(void *user);
    void *user;
} se_host_callbacks_t;

/** Query whether Phase 2-5 lifted x86-64 engine code is linked into libSpaceEngine.so. */
bool se_host_is_recomp_linked(void);

/** Update the active Android/UI window pointer and enforce landscape bounds. */
void se_host_set_window(void *native_window, int width, int height);

/** Update the active Android splash SurfaceView native window pointer and dimensions. */
void se_host_set_splash_window(void *native_window, int width, int height);

/** Retrieve the currently active landscape surface dimensions. */
void se_host_get_surface_size(int *out_width, int *out_height);

/**
 * Validate SpaceEngine.exe + game directory, generate landscape mobile config,
 * map PE non-code sections (.rdata/.data/.pdata/.reloc), and launch engine bring-up.
 */
bool se_host_start_engine(
    const char *exe_path,
    const char *game_dir,
    const char *internal_dir,
    int surface_w,
    int surface_h,
    const se_host_callbacks_t *callbacks,
    char *err_buf,
    size_t err_cap);

/** Forward a touch/pointer event to the native input queue (Phase 6 full wiring). */
void se_host_push_touch(int action, float x, float y, int pointer_count, float pinch_delta);

/** Forward a key press/release event to the native input queue (Phase 6 full wiring). */
void se_host_push_key(int key_code, bool down);
void se_host_push_char(unsigned int codepoint);

/** Request clean shutdown of any running engine thread and release arena memory. */
void se_host_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SE_ENGINE_HOST_H */
