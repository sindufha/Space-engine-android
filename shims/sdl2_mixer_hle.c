/*
 * sdl2_mixer_hle.c - Phase 5 HLE shims for SDL2.dll (24) and SDL2_mixer_ext.dll (27).
 *
 * Bridges SpaceEngine.exe Win64 IAT imports directly to native SDL2 2.30.x +
 * SDL-Mixer-X 2.5.0 (WAV + stb_vorbis OGG decoding, Mix_*MusicStream multi-stream
 * API, VFS path resolution via se_vfs_resolve_path(), and safe Windows system
 * DLL stubbing in SDL_LoadObject for SHCORE.DLL / USER32.DLL DPI queries).
 */
#include "hle_bridge.h"

#include "SDL.h"
#include "SDL_mixer.h"

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define SDL_HLE_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineSDL", fmt, ##__VA_ARGS__)
#else
#define SDL_HLE_LOG(fmt, ...) ((void)0)
#endif

static bool is_write_mode(const char *mode) {
    if (!mode) return false;
    return (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL || strchr(mode, '+') != NULL);
}

static bool is_windows_sys_dll(const char *name) {
    if (!name) return false;
    const char *base = name;
    for (const char *p = name; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    static const char *const k_win_dlls[] = {
        "shcore.dll", "user32.dll", "kernel32.dll", "gdi32.dll",
        "advapi32.dll", "shell32.dll", "ole32.dll", "dwmapi.dll",
        "ntdll.dll", "version.dll", "winmm.dll"
    };
    for (size_t i = 0; i < sizeof(k_win_dlls) / sizeof(k_win_dlls[0]); ++i) {
        const char *a = base;
        const char *b = k_win_dlls[i];
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') return true;
    }
    return false;
}

/* ========================================================================= */
/* SDL2.dll (24 imports)                                                     */
/* ========================================================================= */

static void hle_SDL_Init(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG0(c);
    SDL_SetMainReady();
    int r = SDL_Init(flags);
#if !defined(__ANDROID__)
    if (r != 0 && (flags & SDL_INIT_AUDIO)) {
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
        r = SDL_Init(flags);
    }
#endif
    SDL_HLE_LOG("SDL_Init(0x%x) -> %d", flags, r);
    HLE_RET32(c, (uint32_t)r);
}

static void hle_SDL_Quit(CPU *c) {
    (void)c;
    SDL_Quit();
}

static void hle_SDL_GetError(CPU *c) {
    const char *err = SDL_GetError();
    HLE_RET64(c, err ? se_hle_intern_ascii(err) : 0ull);
}

static void hle_SDL_GetNumAudioDevices(CPU *c) {
    int iscapture = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    int r = SDL_GetNumAudioDevices(iscapture);
    HLE_RET32(c, (uint32_t)r);
}

static void hle_SDL_GetAudioDeviceName(CPU *c) {
    int index = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    int iscapture = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    const char *name = SDL_GetAudioDeviceName(index, iscapture);
    HLE_RET64(c, name ? se_hle_intern_ascii(name) : 0ull);
}

static void hle_SDL_RWFromFile(CPU *c) {
    const char *file = (const char *)(uintptr_t)HLE_ARG0(c);
    const char *mode = (const char *)(uintptr_t)HLE_ARG1(c);
    if (!file || !mode) {
        HLE_RET64(c, 0ull);
        return;
    }
    char host_path[1024];
    if (!se_vfs_resolve_path(file, is_write_mode(mode), host_path, sizeof(host_path))) {
        SDL_SetError("SDL_RWFromFile: failed to resolve path %s", file);
        HLE_RET64(c, 0ull);
        return;
    }
    SDL_RWops *rw = SDL_RWFromFile(host_path, mode);
    HLE_RET_PTR(c, rw);
}

static void hle_SDL_LoadObject(CPU *c) {
    const char *sofile = (const char *)(uintptr_t)HLE_ARG0(c);
    if (!sofile || !sofile[0] || is_windows_sys_dll(sofile)) {
        HLE_RET64(c, 0ull);
        return;
    }
    void *obj = SDL_LoadObject(sofile);
    HLE_RET_PTR(c, obj);
}

static void hle_SDL_LoadFunction(CPU *c) {
    void *handle = (void *)(uintptr_t)HLE_ARG0(c);
    const char *name = (const char *)(uintptr_t)HLE_ARG1(c);
    if (!handle || !name) {
        HLE_RET64(c, 0ull);
        return;
    }
    void *fn = SDL_LoadFunction(handle, name);
    HLE_RET_PTR(c, fn);
}

static void hle_SDL_PollEvent(CPU *c) {
    uint64_t ev_va = HLE_ARG0(c);
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    int has = SDL_PollEvent(ev_va ? &ev : NULL);
    if (has && ev_va) {
        /* SDL_Event is identically sized/aligned (56 bytes) across Win64 & ARM64 for joystick/controller events. */
        uint8_t *dst = (uint8_t *)(uintptr_t)ev_va;
        memcpy(dst, &ev, sizeof(ev));
    }
    HLE_RET32(c, (uint32_t)has);
}

static void hle_SDL_NumJoysticks(CPU *c) {
    HLE_RET32(c, (uint32_t)SDL_NumJoysticks());
}

static void hle_SDL_JoystickOpen(CPU *c) {
    int idx = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET_PTR(c, SDL_JoystickOpen(idx));
}

static void hle_SDL_JoystickClose(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    if (j) SDL_JoystickClose(j);
}

static void hle_SDL_JoystickInstanceID(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    SDL_JoystickID id = j ? SDL_JoystickInstanceID(j) : -1;
    HLE_RET32(c, (uint32_t)id);
}

static void hle_SDL_JoystickName(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    const char *n = j ? SDL_JoystickName(j) : NULL;
    HLE_RET64(c, n ? se_hle_intern_ascii(n) : 0ull);
}

static void hle_SDL_JoystickNumAxes(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)(j ? SDL_JoystickNumAxes(j) : 0));
}

static void hle_SDL_JoystickNumBalls(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)(j ? SDL_JoystickNumBalls(j) : 0));
}

static void hle_SDL_JoystickNumHats(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)(j ? SDL_JoystickNumHats(j) : 0));
}

static void hle_SDL_JoystickNumButtons(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)(j ? SDL_JoystickNumButtons(j) : 0));
}

static void hle_SDL_JoystickGetAxis(CPU *c) {
    SDL_Joystick *j = (SDL_Joystick *)(uintptr_t)HLE_ARG0(c);
    int axis = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    Sint16 v = j ? SDL_JoystickGetAxis(j, axis) : 0;
    HLE_RET32(c, (uint32_t)(int32_t)v);
}

static void hle_SDL_IsGameController(CPU *c) {
    int idx = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)SDL_IsGameController(idx));
}

static void hle_SDL_GameControllerOpen(CPU *c) {
    int idx = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET_PTR(c, SDL_GameControllerOpen(idx));
}

static void hle_SDL_GameControllerClose(CPU *c) {
    SDL_GameController *gc = (SDL_GameController *)(uintptr_t)HLE_ARG0(c);
    if (gc) SDL_GameControllerClose(gc);
}

static void hle_SDL_GameControllerName(CPU *c) {
    SDL_GameController *gc = (SDL_GameController *)(uintptr_t)HLE_ARG0(c);
    const char *n = gc ? SDL_GameControllerName(gc) : NULL;
    HLE_RET64(c, n ? se_hle_intern_ascii(n) : 0ull);
}

static void hle_SDL_GameControllerGetAxis(CPU *c) {
    SDL_GameController *gc = (SDL_GameController *)(uintptr_t)HLE_ARG0(c);
    SDL_GameControllerAxis axis = (SDL_GameControllerAxis)(int32_t)(uint32_t)HLE_ARG1(c);
    Sint16 v = gc ? SDL_GameControllerGetAxis(gc, axis) : 0;
    HLE_RET32(c, (uint32_t)(int32_t)v);
}

/* ========================================================================= */
/* SDL2_mixer_ext.dll (27 imports)                                           */
/* ========================================================================= */

static void hle_Mix_OpenAudio(CPU *c) {
    int freq = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    Uint16 format = (Uint16)HLE_ARG1(c);
    int channels = (int)(int32_t)(uint32_t)HLE_ARG2(c);
    int chunksize = (int)(int32_t)(uint32_t)HLE_ARG3(c);
    int r = Mix_OpenAudio(freq, format, channels, chunksize);
    SDL_HLE_LOG("Mix_OpenAudio(%d, 0x%x, %d, %d) -> %d", freq, format, channels, chunksize, r);
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_OpenAudioDevice(CPU *c) {
    int freq = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    Uint16 format = (Uint16)HLE_ARG1(c);
    int channels = (int)(int32_t)(uint32_t)HLE_ARG2(c);
    int chunksize = (int)(int32_t)(uint32_t)HLE_ARG3(c);
    const char *device = (const char *)(uintptr_t)hle_stack_arg(c, 4u);
    int allowed = (int)(int32_t)(uint32_t)hle_stack_arg(c, 5u);
    int r = Mix_OpenAudioDevice(freq, format, channels, chunksize, device, allowed);
    SDL_HLE_LOG("Mix_OpenAudioDevice(%d, 0x%x, %d, %d, %s, %d) -> %d",
                freq, format, channels, chunksize, device ? device : "(default)", allowed, r);
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_CloseAudio(CPU *c) {
    (void)c;
    Mix_CloseAudio();
}

static void hle_Mix_AllocateChannels(CPU *c) {
    int numchans = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)Mix_AllocateChannels(numchans));
}

static void hle_Mix_ReserveChannels(CPU *c) {
    int num = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)Mix_ReserveChannels(num));
}

static void hle_Mix_LoadWAV_RW(CPU *c) {
    SDL_RWops *src = (SDL_RWops *)(uintptr_t)HLE_ARG0(c);
    int freesrc = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    Mix_Chunk *chunk = src ? Mix_LoadWAV_RW(src, freesrc) : NULL;
    HLE_RET_PTR(c, chunk);
}

static void hle_Mix_FreeChunk(CPU *c) {
    Mix_Chunk *chunk = (Mix_Chunk *)(uintptr_t)HLE_ARG0(c);
    if (chunk) Mix_FreeChunk(chunk);
}

static void hle_Mix_LoadMUS(CPU *c) {
    const char *file = (const char *)(uintptr_t)HLE_ARG0(c);
    if (!file || !file[0]) {
        HLE_RET64(c, 0ull);
        return;
    }
    char host_path[1024];
    if (!se_vfs_resolve_path(file, false, host_path, sizeof(host_path))) {
        Mix_SetError("Mix_LoadMUS: cannot resolve %s", file);
        HLE_RET64(c, 0ull);
        return;
    }
    Mix_Music *mus = Mix_LoadMUS(host_path);
    HLE_RET_PTR(c, mus);
}

static void hle_Mix_FreeMusic(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    if (mus) Mix_FreeMusic(mus);
}

static void hle_Mix_Volume(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    int vol = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    HLE_RET32(c, (uint32_t)Mix_Volume(ch, vol));
}

static void hle_Mix_PlayChannelTimed(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    Mix_Chunk *chunk = (Mix_Chunk *)(uintptr_t)HLE_ARG1(c);
    int loops = (int)(int32_t)(uint32_t)HLE_ARG2(c);
    int ticks = (int)(int32_t)(uint32_t)HLE_ARG3(c);
    int r = chunk ? Mix_PlayChannelTimed(ch, chunk, loops, ticks) : -1;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_FadeInChannelTimed(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    Mix_Chunk *chunk = (Mix_Chunk *)(uintptr_t)HLE_ARG1(c);
    int loops = (int)(int32_t)(uint32_t)HLE_ARG2(c);
    int ms = (int)(int32_t)(uint32_t)HLE_ARG3(c);
    int ticks = (int)(int32_t)(uint32_t)hle_stack_arg(c, 4u);
    int r = chunk ? Mix_FadeInChannelTimed(ch, chunk, loops, ms, ticks) : -1;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_HaltChannel(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)Mix_HaltChannel(ch));
}

static void hle_Mix_FadeOutChannel(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    int ms = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    HLE_RET32(c, (uint32_t)Mix_FadeOutChannel(ch, ms));
}

static void hle_Mix_Playing(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)Mix_Playing(ch));
}

static void hle_Mix_FadingChannel(CPU *c) {
    int ch = (int)(int32_t)(uint32_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)Mix_FadingChannel(ch));
}

static void hle_Mix_PlayMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    int loops = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    int r = mus ? Mix_PlayMusicStream(mus, loops) : -1;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_FadeInMusicStreamPos(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    int loops = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    int ms = (int)(int32_t)(uint32_t)HLE_ARG2(c);
    double pos = HLE_ARG_F64(c, 3);
    int r = mus ? Mix_FadeInMusicStreamPos(mus, loops, ms, pos) : -1;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_VolumeMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    int vol = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    int r = mus ? Mix_VolumeMusicStream(mus, vol) : 0;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_GetVolumeMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    int r = mus ? Mix_GetVolumeMusicStream(mus) : 0;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_HaltMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    int r = mus ? Mix_HaltMusicStream(mus) : 0;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_FadeOutMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    int ms = (int)(int32_t)(uint32_t)HLE_ARG1(c);
    int r = mus ? Mix_FadeOutMusicStream(mus, ms) : 0;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_PlayingMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)(mus ? Mix_PlayingMusicStream(mus) : 0));
}

static void hle_Mix_FadingMusicStream(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    HLE_RET32(c, (uint32_t)(mus ? Mix_FadingMusicStream(mus) : MIX_NO_FADING));
}

static void hle_Mix_SetMusicStreamPosition(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    double pos = HLE_ARG_F64(c, 1);
    int r = mus ? Mix_SetMusicPositionStream(mus, pos) : -1;
    HLE_RET32(c, (uint32_t)r);
}

static void hle_Mix_GetMusicPosition(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    double r = mus ? Mix_GetMusicPosition(mus) : -1.0;
    HLE_RET_F64(c, r);
}

static void hle_Mix_MusicDuration(CPU *c) {
    Mix_Music *mus = (Mix_Music *)(uintptr_t)HLE_ARG0(c);
    double r = mus ? Mix_MusicDuration(mus) : -1.0;
    HLE_RET_F64(c, r);
}

/* ========================================================================= */
/* Registration Entrypoint                                                   */
/* ========================================================================= */

void se_register_sdl2_mixer_shims(void) {
    /* SDL2.dll (24 imports) */
    se_hle_register_import("SDL2.dll", "SDL_Init",                   hle_SDL_Init);
    se_hle_register_import("SDL2.dll", "SDL_Quit",                   hle_SDL_Quit);
    se_hle_register_import("SDL2.dll", "SDL_GetError",               hle_SDL_GetError);
    se_hle_register_import("SDL2.dll", "SDL_GetNumAudioDevices",     hle_SDL_GetNumAudioDevices);
    se_hle_register_import("SDL2.dll", "SDL_GetAudioDeviceName",     hle_SDL_GetAudioDeviceName);
    se_hle_register_import("SDL2.dll", "SDL_RWFromFile",             hle_SDL_RWFromFile);
    se_hle_register_import("SDL2.dll", "SDL_LoadObject",             hle_SDL_LoadObject);
    se_hle_register_import("SDL2.dll", "SDL_LoadFunction",           hle_SDL_LoadFunction);
    se_hle_register_import("SDL2.dll", "SDL_PollEvent",              hle_SDL_PollEvent);
    se_hle_register_import("SDL2.dll", "SDL_NumJoysticks",           hle_SDL_NumJoysticks);
    se_hle_register_import("SDL2.dll", "SDL_JoystickOpen",           hle_SDL_JoystickOpen);
    se_hle_register_import("SDL2.dll", "SDL_JoystickClose",          hle_SDL_JoystickClose);
    se_hle_register_import("SDL2.dll", "SDL_JoystickInstanceID",     hle_SDL_JoystickInstanceID);
    se_hle_register_import("SDL2.dll", "SDL_JoystickName",           hle_SDL_JoystickName);
    se_hle_register_import("SDL2.dll", "SDL_JoystickNumAxes",        hle_SDL_JoystickNumAxes);
    se_hle_register_import("SDL2.dll", "SDL_JoystickNumBalls",       hle_SDL_JoystickNumBalls);
    se_hle_register_import("SDL2.dll", "SDL_JoystickNumHats",        hle_SDL_JoystickNumHats);
    se_hle_register_import("SDL2.dll", "SDL_JoystickNumButtons",     hle_SDL_JoystickNumButtons);
    se_hle_register_import("SDL2.dll", "SDL_JoystickGetAxis",        hle_SDL_JoystickGetAxis);
    se_hle_register_import("SDL2.dll", "SDL_IsGameController",       hle_SDL_IsGameController);
    se_hle_register_import("SDL2.dll", "SDL_GameControllerOpen",     hle_SDL_GameControllerOpen);
    se_hle_register_import("SDL2.dll", "SDL_GameControllerClose",    hle_SDL_GameControllerClose);
    se_hle_register_import("SDL2.dll", "SDL_GameControllerName",     hle_SDL_GameControllerName);
    se_hle_register_import("SDL2.dll", "SDL_GameControllerGetAxis",  hle_SDL_GameControllerGetAxis);

    /* SDL2_mixer_ext.dll (27 imports) */
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_OpenAudio",              hle_Mix_OpenAudio);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_OpenAudioDevice",        hle_Mix_OpenAudioDevice);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_CloseAudio",             hle_Mix_CloseAudio);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_AllocateChannels",       hle_Mix_AllocateChannels);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_ReserveChannels",        hle_Mix_ReserveChannels);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_LoadWAV_RW",             hle_Mix_LoadWAV_RW);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FreeChunk",              hle_Mix_FreeChunk);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_LoadMUS",                hle_Mix_LoadMUS);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FreeMusic",              hle_Mix_FreeMusic);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_Volume",                 hle_Mix_Volume);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_PlayChannelTimed",       hle_Mix_PlayChannelTimed);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FadeInChannelTimed",     hle_Mix_FadeInChannelTimed);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_HaltChannel",            hle_Mix_HaltChannel);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FadeOutChannel",         hle_Mix_FadeOutChannel);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_Playing",                hle_Mix_Playing);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FadingChannel",          hle_Mix_FadingChannel);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_PlayMusicStream",        hle_Mix_PlayMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FadeInMusicStreamPos",   hle_Mix_FadeInMusicStreamPos);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_VolumeMusicStream",      hle_Mix_VolumeMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_GetVolumeMusicStream",   hle_Mix_GetVolumeMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_HaltMusicStream",        hle_Mix_HaltMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FadeOutMusicStream",     hle_Mix_FadeOutMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_PlayingMusicStream",     hle_Mix_PlayingMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_FadingMusicStream",      hle_Mix_FadingMusicStream);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_SetMusicStreamPosition", hle_Mix_SetMusicStreamPosition);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_GetMusicPosition",       hle_Mix_GetMusicPosition);
    se_hle_register_import("SDL2_mixer_ext.dll", "Mix_MusicDuration",          hle_Mix_MusicDuration);
}
