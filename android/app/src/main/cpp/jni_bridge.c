#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_host.h"
#include "pe_loader.h"

#define LOG_TAG "SpaceEngineJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM *g_jvm = NULL;
static pthread_mutex_t g_jni_lock = PTHREAD_MUTEX_INITIALIZER;
static jobject g_bridge_ref = NULL;
static jmethodID g_mid_stage = NULL;
static jmethodID g_mid_ready = NULL;
static jmethodID g_mid_error = NULL;
static jmethodID g_mid_splash = NULL;
static jmethodID g_mid_game_vis = NULL;
static ANativeWindow *g_native_window = NULL;
static ANativeWindow *g_splash_window = NULL;

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static JNIEnv *acquire_jni_env(bool *out_attached) {
    *out_attached = false;
    if (!g_jvm) {
        return NULL;
    }
    JNIEnv *env = NULL;
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        return env;
    }
    if (rc == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == JNI_OK) {
            *out_attached = true;
            return env;
        }
    }
    return NULL;
}

static void release_jni_env(bool attached) {
    if (attached && g_jvm) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

static void jni_on_stage_changed(int stage, const char *message, void *user) {
    (void)user;
    bool attached = false;
    JNIEnv *env = acquire_jni_env(&attached);
    if (!env) {
        return;
    }

    pthread_mutex_lock(&g_jni_lock);
    jobject target = g_bridge_ref ? (*env)->NewLocalRef(env, g_bridge_ref) : NULL;
    jmethodID mid = g_mid_stage;
    pthread_mutex_unlock(&g_jni_lock);

    if (target && mid) {
        jstring jmsg = (*env)->NewStringUTF(env, message ? message : "");
        (*env)->CallVoidMethod(env, target, mid, (jint)stage, jmsg);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        if (jmsg) {
            (*env)->DeleteLocalRef(env, jmsg);
        }
    }
    if (target) {
        (*env)->DeleteLocalRef(env, target);
    }
    release_jni_env(attached);
}

static void jni_on_planetarium_ready(void *user) {
    (void)user;
    bool attached = false;
    JNIEnv *env = acquire_jni_env(&attached);
    if (!env) {
        return;
    }

    pthread_mutex_lock(&g_jni_lock);
    jobject target = g_bridge_ref ? (*env)->NewLocalRef(env, g_bridge_ref) : NULL;
    jmethodID mid = g_mid_ready;
    pthread_mutex_unlock(&g_jni_lock);

    if (target && mid) {
        (*env)->CallVoidMethod(env, target, mid);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
    }
    if (target) {
        (*env)->DeleteLocalRef(env, target);
    }
    release_jni_env(attached);
}

static void jni_on_engine_error(const char *message, void *user) {
    (void)user;
    bool attached = false;
    JNIEnv *env = acquire_jni_env(&attached);
    if (!env) {
        return;
    }

    pthread_mutex_lock(&g_jni_lock);
    jobject target = g_bridge_ref ? (*env)->NewLocalRef(env, g_bridge_ref) : NULL;
    jmethodID mid = g_mid_error;
    pthread_mutex_unlock(&g_jni_lock);

    if (target && mid) {
        jstring jmsg = (*env)->NewStringUTF(env, message ? message : "Unknown native error");
        (*env)->CallVoidMethod(env, target, mid, jmsg);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
        if (jmsg) {
            (*env)->DeleteLocalRef(env, jmsg);
        }
    }
    if (target) {
        (*env)->DeleteLocalRef(env, target);
    }
    release_jni_env(attached);
}

static void jni_on_splash_visibility(int visible, void *user) {
    (void)user;
    bool attached = false;
    JNIEnv *env = acquire_jni_env(&attached);
    if (!env) {
        return;
    }

    pthread_mutex_lock(&g_jni_lock);
    jobject target = g_bridge_ref ? (*env)->NewLocalRef(env, g_bridge_ref) : NULL;
    jmethodID mid = g_mid_splash;
    pthread_mutex_unlock(&g_jni_lock);

    if (target && mid) {
        (*env)->CallVoidMethod(env, target, mid, visible ? JNI_TRUE : JNI_FALSE);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
    }
    if (target) {
        (*env)->DeleteLocalRef(env, target);
    }
    release_jni_env(attached);
}

static void jni_on_game_surface_visible(void *user) {
    (void)user;
    bool attached = false;
    JNIEnv *env = acquire_jni_env(&attached);
    if (!env) {
        return;
    }

    pthread_mutex_lock(&g_jni_lock);
    jobject target = g_bridge_ref ? (*env)->NewLocalRef(env, g_bridge_ref) : NULL;
    jmethodID mid = g_mid_game_vis;
    pthread_mutex_unlock(&g_jni_lock);

    if (target && mid) {
        (*env)->CallVoidMethod(env, target, mid);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        }
    }
    if (target) {
        (*env)->DeleteLocalRef(env, target);
    }
    release_jni_env(attached);
}

JNIEXPORT jboolean JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeIsRecompLinked(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    return se_host_is_recomp_linked() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeValidateExe(
    JNIEnv *env,
    jclass clazz,
    jstring jpath,
    jint fd) {
    (void)clazz;
    const char *path = jpath ? (*env)->GetStringUTFChars(env, jpath, NULL) : NULL;
    se_pe_info_t info;
    char err[512] = {0};

    bool ok = false;
    if (fd >= 0) {
        ok = se_pe_validate_exe_fd((int)fd, &info, err, sizeof(err));
    } else if (path) {
        ok = se_pe_validate_exe_path(path, &info, err, sizeof(err));
    } else {
        snprintf(err, sizeof(err), "No executable path or file descriptor supplied");
    }

    if (path) {
        (*env)->ReleaseStringUTFChars(env, jpath, path);
    }

    char result[768];
    if (ok) {
        snprintf(
            result,
            sizeof(result),
            "OK|PE32+ x86-64 (%u sections, Entry 0x%x) | SHA256 %.12s...",
            (unsigned)info.num_sections,
            info.entry_rva,
            info.sha256_hex);
        LOGI("Validated SpaceEngine.exe cleanly");
    } else {
        snprintf(result, sizeof(result), "ERR|%s", err[0] ? err : "PE validation failed");
        LOGE("SpaceEngine.exe validation failed: %s", err);
    }
    return (*env)->NewStringUTF(env, result);
}

#include <unistd.h>

static int g_stdio_pipe[2] = { -1, -1 };
static pthread_once_t g_stdio_once = PTHREAD_ONCE_INIT;

static void *se_stdio_logcat_thread(void *arg) {
    (void)arg;
    char line[1024];
    size_t pos = 0;
    char ch = 0;
    while (read(g_stdio_pipe[0], &ch, 1) == 1) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n' || pos >= sizeof(line) - 1) {
            line[pos] = '\0';
            if (pos > 0) {
                __android_log_write(ANDROID_LOG_INFO, "SpaceEngineStdio", line);
            }
            pos = 0;
            if (ch != '\n') {
                line[pos++] = ch;
            }
        } else {
            line[pos++] = ch;
        }
    }
    if (pos > 0) {
        line[pos] = '\0';
        __android_log_write(ANDROID_LOG_INFO, "SpaceEngineStdio", line);
    }
    return NULL;
}

static void se_init_stdio_redirect_once(void) {
    if (pipe(g_stdio_pipe) != 0) {
        return;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(g_stdio_pipe[1], STDOUT_FILENO);
    dup2(g_stdio_pipe[1], STDERR_FILENO);
    close(g_stdio_pipe[1]);
    g_stdio_pipe[1] = -1;

    pthread_t tid;
    if (pthread_create(&tid, NULL, se_stdio_logcat_thread, NULL) == 0) {
        pthread_detach(tid);
    }
}

static void se_install_stdio_logcat_redirect(void) {
    pthread_once(&g_stdio_once, se_init_stdio_redirect_once);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeSetRuntimePaths(
    JNIEnv *env,
    jclass clazz,
    jstring jnative_lib_dir) {
    (void)clazz;
    se_install_stdio_logcat_redirect();
    if (!jnative_lib_dir) {
        return;
    }
    const char *lib_dir = (*env)->GetStringUTFChars(env, jnative_lib_dir, NULL);
    if (lib_dir && lib_dir[0]) {
        setenv("LIBGL_DRIVERS_PATH", lib_dir, 1);
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        setenv("GALLIUM_DRIVER", "zink", 1);
        setenv("EGL_PLATFORM", "android", 1);
        setenv("ZINK_DESCRIPTORS", "lazy", 0);
        setenv("LIBGL_DEBUG", "verbose", 0);
        setenv("EGL_LOG_LEVEL", "debug", 0);
        LOGI("Configured Mesa Zink Kopper runtime path: LIBGL_DRIVERS_PATH=%s", lib_dir);
    }
    if (lib_dir) {
        (*env)->ReleaseStringUTFChars(env, jnative_lib_dir, lib_dir);
    }
}

JNIEXPORT jstring JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeValidateGameDir(
    JNIEnv *env,
    jclass clazz,
    jstring jdir) {
    (void)clazz;
    const char *dir = jdir ? (*env)->GetStringUTFChars(env, jdir, NULL) : NULL;
    se_game_dir_info_t info;
    char err[512] = {0};

    bool ok = dir ? se_game_validate_dir(dir, &info, err, sizeof(err)) : false;
    if (dir) {
        (*env)->ReleaseStringUTFChars(env, jdir, dir);
    }

    char result[768];
    if (ok) {
        snprintf(
            result,
            sizeof(result),
            "OK|Verified data/, config/, system/folders.cfg (config %s)",
            info.config_writable ? "writable" : "read-only -> internal fallback");
        LOGI("Validated game directory cleanly");
    } else {
        snprintf(result, sizeof(result), "ERR|%s", err[0] ? err : "Invalid game directory");
        LOGE("Game directory validation failed: %s", err);
    }
    return (*env)->NewStringUTF(env, result);
}

JNIEXPORT jstring JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeStartEngine(
    JNIEnv *env,
    jclass clazz,
    jobject callback_bridge,
    jstring jexe_path,
    jstring jgame_dir,
    jstring jinternal_dir,
    jint surface_w,
    jint surface_h) {
    (void)clazz;

    pthread_mutex_lock(&g_jni_lock);
    if (g_bridge_ref) {
        (*env)->DeleteGlobalRef(env, g_bridge_ref);
        g_bridge_ref = NULL;
    }
    if (callback_bridge) {
        g_bridge_ref = (*env)->NewGlobalRef(env, callback_bridge);
        jclass cls = (*env)->GetObjectClass(env, callback_bridge);
        g_mid_stage = (*env)->GetMethodID(env, cls, "onEngineStageChanged", "(ILjava/lang/String;)V");
        g_mid_splash = (*env)->GetMethodID(env, cls, "onSplashVisibilityChanged", "(Z)V");
        g_mid_game_vis = (*env)->GetMethodID(env, cls, "onGameSurfaceVisible", "()V");
        g_mid_ready = (*env)->GetMethodID(env, cls, "onPlanetariumReady", "()V");
        g_mid_error = (*env)->GetMethodID(env, cls, "onEngineError", "(Ljava/lang/String;)V");
        (*env)->DeleteLocalRef(env, cls);
    }
    pthread_mutex_unlock(&g_jni_lock);

    const char *exe_path = jexe_path ? (*env)->GetStringUTFChars(env, jexe_path, NULL) : NULL;
    const char *game_dir = jgame_dir ? (*env)->GetStringUTFChars(env, jgame_dir, NULL) : NULL;
    const char *internal_dir = jinternal_dir ? (*env)->GetStringUTFChars(env, jinternal_dir, NULL) : NULL;

    se_host_callbacks_t cb = {
        .on_stage_changed = jni_on_stage_changed,
        .on_planetarium_ready = jni_on_planetarium_ready,
        .on_engine_error = jni_on_engine_error,
        .on_splash_visibility = jni_on_splash_visibility,
        .on_game_surface_visible = jni_on_game_surface_visible,
        .user = NULL,
    };

    char err[512] = {0};
    bool started = se_host_start_engine(
        exe_path,
        game_dir,
        internal_dir,
        (int)surface_w,
        (int)surface_h,
        &cb,
        err,
        sizeof(err));

    if (exe_path) {
        (*env)->ReleaseStringUTFChars(env, jexe_path, exe_path);
    }
    if (game_dir) {
        (*env)->ReleaseStringUTFChars(env, jgame_dir, game_dir);
    }
    if (internal_dir) {
        (*env)->ReleaseStringUTFChars(env, jinternal_dir, internal_dir);
    }

    if (started) {
        return (*env)->NewStringUTF(env, "OK|Started");
    }
    char out[600];
    snprintf(out, sizeof(out), "ERR|%s", err[0] ? err : "Failed starting native engine");
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeOnSurfaceCreated(
    JNIEnv *env,
    jclass clazz,
    jobject surface,
    jint width,
    jint height) {
    (void)clazz;
    ANativeWindow *new_win = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g_jni_lock);
    if (g_native_window) {
        ANativeWindow_release(g_native_window);
    }
    g_native_window = new_win;
    pthread_mutex_unlock(&g_jni_lock);
    se_host_set_window(new_win, (int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeOnSurfaceChanged(
    JNIEnv *env,
    jclass clazz,
    jint width,
    jint height) {
    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_jni_lock);
    ANativeWindow *win = g_native_window;
    pthread_mutex_unlock(&g_jni_lock);
    se_host_set_window(win, (int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeOnSurfaceDestroyed(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_jni_lock);
    if (g_native_window) {
        ANativeWindow_release(g_native_window);
        g_native_window = NULL;
    }
    pthread_mutex_unlock(&g_jni_lock);
    se_host_set_window(NULL, 0, 0);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeOnSplashSurfaceCreated(
    JNIEnv *env,
    jclass clazz,
    jobject surface,
    jint width,
    jint height) {
    (void)clazz;
    ANativeWindow *new_win = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g_jni_lock);
    if (g_splash_window) {
        ANativeWindow_release(g_splash_window);
    }
    g_splash_window = new_win;
    pthread_mutex_unlock(&g_jni_lock);
    se_host_set_splash_window(new_win, (int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeOnSplashSurfaceChanged(
    JNIEnv *env,
    jclass clazz,
    jobject surface,
    jint width,
    jint height) {
    (void)clazz;
    ANativeWindow *new_win = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    pthread_mutex_lock(&g_jni_lock);
    if (surface) {
        if (g_splash_window) {
            ANativeWindow_release(g_splash_window);
        }
        g_splash_window = new_win;
    }
    ANativeWindow *win = g_splash_window;
    pthread_mutex_unlock(&g_jni_lock);
    se_host_set_splash_window(win, (int)width, (int)height);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeOnSplashSurfaceDestroyed(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    pthread_mutex_lock(&g_jni_lock);
    if (g_splash_window) {
        ANativeWindow_release(g_splash_window);
        g_splash_window = NULL;
    }
    pthread_mutex_unlock(&g_jni_lock);
    se_host_set_splash_window(NULL, 0, 0);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeSendTouch(
    JNIEnv *env,
    jclass clazz,
    jint action,
    jfloat x,
    jfloat y,
    jint pointer_count,
    jfloat pinch_delta) {
    (void)env;
    (void)clazz;
    se_host_push_touch((int)action, (float)x, (float)y, (int)pointer_count, (float)pinch_delta);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeSendKey(
    JNIEnv *env,
    jclass clazz,
    jint key_code,
    jboolean down) {
    (void)env;
    (void)clazz;
    se_host_push_key((int)key_code, down == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeSendChar(
    JNIEnv *env,
    jclass clazz,
    jint codepoint) {
    (void)env;
    (void)clazz;
    se_host_push_char((unsigned int)codepoint);
}

JNIEXPORT void JNICALL
Java_org_spaceengine_port_NativeEngineBridge_nativeShutdown(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    se_host_shutdown();
}
