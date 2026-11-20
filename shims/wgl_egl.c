/*
 * wgl_egl.c - Phase 4 WGL + OpenGL 4.6 / EGL / Mesa Zink / GLES3 bridge.
 *
 * Implements all 50 OPENGL32.dll IAT imports plus dynamic wglGetProcAddress()
 * resolution (typed Core 2.0-4.6 / WGL_ARB / WGL_EXT handlers backed by
 * runtime libGL.so (Mesa Zink) when present or Android NDK EGL/GLESv3, with
 * clean host-mode GL state tracking for test_phase4_host).
 *
 * Also handles:
 *  - Shared worker GL contexts (wglCreateContext + wglShareLists +
 *    wglCreateContextAttribsARB) with lazy EGL context materialization so
 *    worker threads sharing textures/shaders with the main context work cleanly.
 *  - Stage 7 + first wglSwapLayerBuffers synchronization to invoke
 *    on_planetarium_ready() exactly once.
 */
#if defined(__ANDROID__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "hle_bridge.h"
#include "pe_loader.h"
#include "engine_host.h"
#include "shader_es3.h"

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "gl46_core_direct.inc"
#define WGL_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineWGL", fmt, ##__VA_ARGS__)
#else
#define WGL_LOG(fmt, ...) ((void)0)
typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef unsigned int   GLuint;
typedef int            GLsizei;
typedef float          GLfloat;
typedef double         GLdouble;
typedef unsigned char  GLubyte;
#endif

/* ========================================================================= */
/* OpenGL / WGL Constants                                                    */
/* ========================================================================= */

#ifndef GL_NO_ERROR
#define GL_NO_ERROR 0u
#endif
#define SE_GL_VENDOR                         0x1F00u
#define SE_GL_RENDERER                       0x1F01u
#define SE_GL_VERSION                        0x1F02u
#define SE_GL_EXTENSIONS                     0x1F03u
#define SE_GL_SHADING_LANGUAGE_VERSION       0x8B8Cu

#define SE_GL_MAJOR_VERSION                  0x821Bu
#define SE_GL_MINOR_VERSION                  0x821Cu
#define SE_GL_NUM_EXTENSIONS                 0x821Du
#define SE_GL_CONTEXT_FLAGS                  0x821Eu
#define SE_GL_CONTEXT_PROFILE_MASK           0x9126u
#define SE_GL_CONTEXT_CORE_PROFILE_BIT       0x00000001u

#define SE_GL_VIEWPORT                       0x0BA2u
#define SE_GL_DEPTH_WRITEMASK                0x0B72u
#define SE_GL_MAX_TEXTURE_SIZE               0x0D33u
#define SE_GL_MAX_3D_TEXTURE_SIZE            0x8073u
#define SE_GL_MAX_CUBE_MAP_TEXTURE_SIZE      0x851Cu
#define SE_GL_MAX_TEXTURE_IMAGE_UNITS        0x8872u
#define SE_GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4Cu
#define SE_GL_MAX_COMBINED_TEX_UNITS         0x8B4Du
#define SE_GL_MAX_COLOR_ATTACHMENTS          0x8CDFu
#define SE_GL_MAX_DRAW_BUFFERS               0x8824u
#define SE_GL_MAX_SAMPLES                    0x8D57u
#define SE_GL_MAX_UNIFORM_BLOCK_SIZE         0x8A30u
#define SE_GL_MAX_UNIFORM_BUFFER_BINDINGS    0x8A2Fu
#define SE_GL_MAX_SSBO_BLOCK_SIZE            0x90DEu
#define SE_GL_MAX_COMPUTE_WORK_GROUP_INVOC   0x90EBu
#define SE_GL_MAX_VERTEX_ATTRIBS             0x8869u
#define SE_GL_MAX_VIEWPORTS                  0x825Bu
#define SE_GL_MAX_ARRAY_TEXTURE_LAYERS       0x88FFu
#define SE_GL_MAX_TEXTURE_MAX_ANISOTROPY     0x84FFu
#define SE_GL_FRAMEBUFFER_BINDING            0x8CA6u
#define SE_GL_TEXTURE_BINDING_2D             0x8069u
#define SE_GL_ARRAY_BUFFER_BINDING           0x8894u
#define SE_GL_CURRENT_PROGRAM                0x8B8Du

#define SE_GL_MAX_VIEWPORT_DIMS              0x0D3Au
#define SE_GL_TEXTURE_1D                     0x0DE0u
#define SE_GL_TEXTURE_2D                     0x0DE1u
#define SE_GL_TEXTURE_WIDTH                  0x1000u
#define SE_GL_TEXTURE_HEIGHT                 0x1001u
#define SE_GL_TEXTURE_INTERNAL_FORMAT        0x1003u
#define SE_GL_TEXTURE_WRAP_S                 0x2802u
#define SE_GL_TEXTURE_WRAP_T                 0x2803u
#define SE_GL_TEXTURE_WRAP_R                 0x8072u
#define SE_GL_CLAMP                          0x2900u
#define SE_GL_CLAMP_TO_EDGE                  0x812Fu
#define SE_GL_UNPACK_ALIGNMENT               0x0CF5u
#define SE_GL_BYTE                           0x1400u
#define SE_GL_UNSIGNED_BYTE                  0x1401u
#define SE_GL_SHORT                          0x1402u
#define SE_GL_UNSIGNED_SHORT                 0x1403u
#define SE_GL_INT                            0x1404u
#define SE_GL_UNSIGNED_INT                   0x1405u
#define SE_GL_FLOAT                          0x1406u
#define SE_GL_HALF_FLOAT                     0x140Bu
#define SE_GL_UNSIGNED_INT_8_8_8_8_REV       0x8367u
#define SE_GL_RED                            0x1903u
#define SE_GL_ALPHA                          0x1906u
#define SE_GL_RGB                            0x1907u
#define SE_GL_RGBA                           0x1908u
#define SE_GL_LUMINANCE                      0x1909u
#define SE_GL_LUMINANCE_ALPHA                0x190Au
#define SE_GL_BGR                            0x80E0u
#define SE_GL_BGRA                           0x80E1u
#define SE_GL_RG                             0x8227u
#define SE_GL_RGB8                           0x8051u
#define SE_GL_RGBA8                          0x8058u
#define SE_GL_R8                             0x8229u
#define SE_GL_RG8                            0x822Bu
#define SE_GL_R16F                           0x822Du
#define SE_GL_RG16F                          0x822Fu
#define SE_GL_RGBA16F                        0x881Au
#define SE_GL_RGB16F                         0x881Bu
#define SE_GL_R8_SNORM                       0x8F94u
#define SE_GL_RG8_SNORM                      0x8F95u
#define SE_GL_SRGB8                          0x8C41u
#define SE_GL_SRGB8_ALPHA8                   0x8C43u
#define SE_GL_DEPTH_COMPONENT                0x1902u
#define SE_GL_DEPTH_COMPONENT24              0x81A6u
#define SE_GL_DEPTH_COMPONENT32              0x81A7u
#define SE_GL_DEPTH_COMPONENT32F             0x8CACu
#define SE_GL_COMPRESSED_RGB_S3TC_DXT1_EXT   0x83F0u
#define SE_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1u
#define SE_GL_COMPRESSED_RGBA_S3TC_DXT3_EXT  0x83F2u
#define SE_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3u
#define SE_GL_COMPRESSED_SRGB_S3TC_DXT1_EXT  0x8C4Cu
#define SE_GL_COMPRESSED_SRGB_ALPHA_DXT1_EXT 0x8C4Du
#define SE_GL_COMPRESSED_SRGB_ALPHA_DXT3_EXT 0x8C4Eu
#define SE_GL_COMPRESSED_SRGB_ALPHA_DXT5_EXT 0x8C4Fu
#define SE_GL_COMPRESSED_RED_RGTC1           0x8DBBu
#define SE_GL_COMPRESSED_SIGNED_RED_RGTC1    0x8DBCu
#define SE_GL_COMPRESSED_RG_RGTC2            0x8DBDu
#define SE_GL_COMPRESSED_SIGNED_RG_RGTC2     0x8DBEu
#define SE_GL_MAP_PERSISTENT_BIT             0x0040u
#define SE_GL_MAP_COHERENT_BIT               0x0080u
#define SE_GL_DYNAMIC_DRAW                   0x88E8u
#define SE_GL_FRAMEBUFFER_COMPLETE           0x8CD5u
#define SE_GL_FRONT                          0x0404u
#define SE_GL_BACK                           0x0405u
#define SE_GL_NONE                           0u
#define SE_GL_COLOR_ATTACHMENT0              0x8CE0u

#define SE_WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define SE_WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define SE_WGL_CONTEXT_FLAGS_ARB             0x2094
#define SE_WGL_CONTEXT_PROFILE_MASK_ARB      0x9126

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#ifndef EGL_CONTEXT_MAJOR_VERSION
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#endif
#ifndef EGL_CONTEXT_MINOR_VERSION
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#endif

static const char *const k_gl_extensions[] = {
    "GL_ARB_direct_state_access",
    "GL_ARB_buffer_storage",
    "GL_ARB_clip_control",
    "GL_ARB_blend_func_extended",
    "GL_ARB_uniform_buffer_object",
    "GL_ARB_shader_storage_buffer_object",
    "GL_ARB_compute_shader",
    "GL_ARB_texture_storage",
    "GL_ARB_texture_compression",
    "GL_ARB_texture_compression_rgtc",
    "GL_ARB_texture_float",
    "GL_ARB_texture_multisample",
    "GL_ARB_framebuffer_object",
    "GL_ARB_vertex_array_object",
    "GL_ARB_vertex_buffer_object",
    "GL_ARB_get_program_binary",
    "GL_ARB_shader_image_load_store",
    "GL_ARB_shader_objects",
    "GL_ARB_sync",
    "GL_ARB_instanced_arrays",
    "GL_ARB_draw_instanced",
    "GL_ARB_draw_buffers",
    "GL_ARB_sampler_objects",
    "GL_ARB_multisample",
    "GL_ARB_debug_output",
    "GL_ARB_internalformat_query",
    "GL_ARB_internalformat_query2",
    "GL_ARB_invalidate_subdata",
    "GL_ARB_map_buffer_range",
    "GL_ARB_parallel_shader_compile",
    "GL_ARB_program_interface_query",
    "GL_ARB_timer_query",
    "GL_ARB_depth_buffer_float",
    "GL_ARB_color_buffer_float",
    "GL_ARB_half_float_vertex",
    "GL_EXT_direct_state_access",
    "GL_EXT_framebuffer_object",
    "GL_EXT_framebuffer_multisample",
    "GL_EXT_texture3D",
    "GL_EXT_texture_array",
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_filter_anisotropic",
    "GL_KHR_debug",
    "GL_KHR_robustness",
    "GL_NV_depth_buffer_float",
    "WGL_ARB_pixel_format",
    "WGL_ARB_create_context",
    "WGL_ARB_create_context_profile",
    "WGL_ARB_extensions_string",
    "WGL_EXT_extensions_string",
    "WGL_EXT_swap_control",
};

#define NUM_GL_EXTENSIONS ((uint32_t)(sizeof(k_gl_extensions) / sizeof(k_gl_extensions[0])))

/* ========================================================================= */
/* Context & Bridge State                                                    */
/* ========================================================================= */

#define MAX_WGL_CONTEXTS 32

typedef struct {
    bool     used;
    uint64_t handle;
    uint64_t hdc;
    uint64_t share_master;
    int      major_ver;
    int      minor_ver;
    int      profile_mask;
    int      flags;
    bool     is_main;
#if defined(__ANDROID__)
    EGLContext egl_ctx;
    EGLSurface pbuffer_surf;
    bool       materialized;
#endif
} se_wgl_ctx_t;

static pthread_mutex_t g_wgl_mu = PTHREAD_MUTEX_INITIALIZER;
static se_wgl_ctx_t    g_ctxs[MAX_WGL_CONTEXTS];
static uint64_t        g_next_ctx_handle = 0x00030001ull;
static uint32_t        g_next_id = 1u;
static int32_t         g_swap_interval = 1;
static int32_t         g_viewport[4] = { 0, 0, 2400, 1080 };
static bool            g_depth_mask = true;
static uint32_t        g_swap_count = 0u;
static bool            g_planetarium_loaded = false;
static bool            g_ready_fired = false;
static void          (*g_ready_cb)(void *) = NULL;
static void           *g_ready_user = NULL;

#if defined(__ANDROID__)
static bool            g_egl_desktop_gl = false;
#else
static bool            g_egl_desktop_gl = true;
#endif
static bool            g_es3_caps_probed = false;
static bool            g_es3_has_s3tc = false;
static bool            g_es3_has_rgtc = false;
static bool            g_es3_has_clip_control = false;
static bool            g_es3_has_buffer_storage = false;
static bool            g_es3_has_blend_func_extended = false;

static uint64_t        g_ext_str_guest = 0ull;
static uint64_t        g_ext_item_guest[NUM_GL_EXTENSIONS];
static const char     *g_active_exts[NUM_GL_EXTENSIONS];
static uint32_t        g_active_ext_count = 0u;
static bool            g_ext_cache_dirty = true;
static void           *g_dyn_native_procs[SE_HLE_TOTAL_SLOTS];

#define SE_MAX_SHADER_TYPES 4096
typedef struct {
    bool     used;
    uint32_t id;
    uint32_t stage;
} se_shader_slot_t;
static se_shader_slot_t g_shader_slots[SE_MAX_SHADER_TYPES];
static void (*g_shader_capture_cb)(uint32_t shader, uint32_t stage, const char *translated, void *user) = NULL;
static void *g_shader_capture_user = NULL;

#if defined(_MSC_VER) && !defined(__clang__)
static __declspec(thread) uint64_t tl_cur_hglrc = 0ull;
static __declspec(thread) uint64_t tl_cur_hdc = 0ull;
#else
static _Thread_local uint64_t tl_cur_hglrc = 0ull;
static _Thread_local uint64_t tl_cur_hdc = 0ull;
#endif

#if defined(__ANDROID__)
static EGLDisplay     g_egl_dpy = EGL_NO_DISPLAY;
static EGLConfig      g_egl_cfg = NULL;
static EGLSurface     g_egl_win_surf = EGL_NO_SURFACE;
static void          *g_egl_bound_anw = NULL;
static bool           g_egl_init_done = false;
static void          *g_libegl = NULL;
static void          *g_libglapi = NULL;
static void          *g_libglesv3 = NULL;

typedef struct {
    EGLDisplay (*p_eglGetDisplay)(EGLNativeDisplayType);
    EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*p_eglBindAPI)(EGLenum);
    EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLSurface (*p_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
    EGLSurface (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
    EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*p_eglSwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*p_eglSwapInterval)(EGLDisplay, EGLint);
    EGLint     (*p_eglGetError)(void);
    __eglMustCastToProperFunctionPointerType (*p_eglGetProcAddress)(const char *);
} se_egl_dispatch_t;

static se_egl_dispatch_t g_egl;

static bool resolve_egl_dispatch(void *h) {
    memset(&g_egl, 0, sizeof(g_egl));
    if (!h) return false;
    *(void **)(&g_egl.p_eglGetProcAddress) = dlsym(h, "eglGetProcAddress");
#define LOAD_EGL(fn) do { \
    void *_sym = dlsym(h, #fn); \
    if (!_sym && g_egl.p_eglGetProcAddress) _sym = (void *)g_egl.p_eglGetProcAddress(#fn); \
    *(void **)(&g_egl.p_##fn) = _sym; \
} while (0)
    LOAD_EGL(eglGetDisplay);
    LOAD_EGL(eglInitialize);
    LOAD_EGL(eglBindAPI);
    LOAD_EGL(eglChooseConfig);
    LOAD_EGL(eglCreateWindowSurface);
    LOAD_EGL(eglCreatePbufferSurface);
    LOAD_EGL(eglDestroySurface);
    LOAD_EGL(eglCreateContext);
    LOAD_EGL(eglDestroyContext);
    LOAD_EGL(eglMakeCurrent);
    LOAD_EGL(eglSwapBuffers);
    LOAD_EGL(eglSwapInterval);
    LOAD_EGL(eglGetError);
#undef LOAD_EGL
    return (g_egl.p_eglGetDisplay && g_egl.p_eglInitialize &&
            g_egl.p_eglChooseConfig && g_egl.p_eglCreateContext &&
            g_egl.p_eglMakeCurrent && g_egl.p_eglSwapBuffers &&
            g_egl.p_eglGetProcAddress);
}

#define eglGetDisplay           (g_egl.p_eglGetDisplay)
#define eglInitialize           (g_egl.p_eglInitialize)
#define eglBindAPI              (g_egl.p_eglBindAPI)
#define eglChooseConfig         (g_egl.p_eglChooseConfig)
#define eglCreateWindowSurface  (g_egl.p_eglCreateWindowSurface)
#define eglCreatePbufferSurface (g_egl.p_eglCreatePbufferSurface)
#define eglDestroySurface       (g_egl.p_eglDestroySurface)
#define eglCreateContext        (g_egl.p_eglCreateContext)
#define eglDestroyContext       (g_egl.p_eglDestroyContext)
#define eglMakeCurrent          (g_egl.p_eglMakeCurrent)
#define eglSwapBuffers          (g_egl.p_eglSwapBuffers)
#define eglSwapInterval         (g_egl.p_eglSwapInterval)
#define eglGetError             (g_egl.p_eglGetError)
#define eglGetProcAddress       (g_egl.p_eglGetProcAddress)

static const GLubyte *(*g_native_glGetStringi)(GLenum, GLuint) = NULL;
static void (*g_native_glPolygonMode)(GLenum, GLenum) = NULL;
static void (*g_native_glTexImage1D)(GLenum, GLint, GLint, GLsizei, GLint, GLenum, GLenum, const void *) = NULL;
static void (*g_native_glTexSubImage1D)(GLenum, GLint, GLint, GLsizei, GLenum, GLenum, const void *) = NULL;
static void (*g_native_glGetTexImage)(GLenum, GLint, GLenum, GLenum, void *) = NULL;
static void (*g_native_glGetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *) = NULL;
static void (*g_native_glClearDepth)(double) = NULL;
static void (*g_native_glDepthRange)(double, double) = NULL;
static void (*g_native_glDrawBuffer)(GLenum) = NULL;
static void (*g_native_glClipControl)(GLenum, GLenum) = NULL;
static void (*g_native_glBufferStorage)(GLenum, ptrdiff_t, const void *, GLbitfield) = NULL;
static void (*g_native_glCreateTextures)(GLenum, GLsizei, GLuint *) = NULL;
static void (*g_native_glTextureStorage2D)(GLuint, GLsizei, GLenum, GLsizei, GLsizei) = NULL;
static void (*g_native_glTexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = NULL;
static void (*g_native_glTexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei) = NULL;
static void (*g_native_glBindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum) = NULL;
static void (*g_native_glMemoryBarrier)(GLbitfield) = NULL;
static void (*g_native_glDispatchCompute)(GLuint, GLuint, GLuint) = NULL;
static void (*g_native_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = NULL;
static void (*g_native_glBlendEquation)(GLenum) = NULL;
static void (*g_native_glBlendEquationSeparate)(GLenum, GLenum) = NULL;
static void (*g_native_glProgramParameteri)(GLuint, GLenum, GLint) = NULL;
static void (*g_native_glGetProgramBinary)(GLuint, GLsizei, GLsizei *, GLenum *, void *) = NULL;
static void (*g_native_glProgramBinary)(GLuint, GLenum, const void *, GLsizei) = NULL;
static void (*g_native_glVertexAttribDivisor)(GLuint, GLuint) = NULL;
static void (*g_native_glDrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei) = NULL;
static void (*g_native_glDrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei) = NULL;
static void (*g_native_glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = NULL;
static void (*g_native_glRenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei) = NULL;
static void (*g_native_glTexImage2DMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean) = NULL;
static void (*g_native_glDebugMessageCallback)(const void *, const void *) = NULL;
static void (*g_native_glTexParameterfv)(GLenum, GLenum, const GLfloat *) = NULL;
static void (*g_native_glSamplerParameteriv)(GLuint, GLenum, const GLint *) = NULL;
static void (*g_native_glObjectLabel)(GLenum, GLuint, GLsizei, const char *) = NULL;
static void (*g_native_glPushDebugGroup)(GLenum, GLuint, GLsizei, const char *) = NULL;
static void (*g_native_glPopDebugGroup)(void) = NULL;

#if defined(__ANDROID__)
static void *get_es3_unimpl_stub(void) {
    static bool probed = false;
    static void *unimpl_stub = NULL;
    pthread_mutex_lock(&g_wgl_mu);
    if (!probed) {
        if (eglGetProcAddress) {
            unimpl_stub = (void *)eglGetProcAddress("gl__se_nonexistent_api_probe_");
        }
        probed = true;
    }
    pthread_mutex_unlock(&g_wgl_mu);
    return unimpl_stub;
}

static void *filter_es3_unimpl(void *p) {
    if (!p) return NULL;
    void *unimpl = get_es3_unimpl_stub();
    return (unimpl && p == unimpl) ? NULL : p;
}

static void *lookup_native_gl_proc(const char *name) {
    if (!name || !name[0]) return NULL;
    void *p = NULL;
    if (g_egl_desktop_gl) {
        if (eglGetProcAddress) {
            p = (void *)eglGetProcAddress(name);
        }
        if (!p && g_libglapi) {
            p = dlsym(g_libglapi, name);
        }
    } else {
        if (g_libglesv3) {
            p = dlsym(g_libglesv3, name);
        }
        if (!p && eglGetProcAddress) {
            p = filter_es3_unimpl((void *)eglGetProcAddress(name));
        }
        static const char *const k_es3_suffixes[] = { "EXT", "KHR", "OES" };
        size_t nlen = strlen(name);
        bool has_suffix =
            (nlen >= 3u && (strcmp(name + nlen - 3u, "EXT") == 0 ||
                            strcmp(name + nlen - 3u, "KHR") == 0 ||
                            strcmp(name + nlen - 3u, "OES") == 0 ||
                            strcmp(name + nlen - 3u, "ARB") == 0));
        if (!p && !has_suffix && nlen + 4u < 128u) {
            char alt[128];
            for (size_t i = 0; i < sizeof(k_es3_suffixes) / sizeof(k_es3_suffixes[0]) && !p; ++i) {
                snprintf(alt, sizeof(alt), "%s%s", name, k_es3_suffixes[i]);
                if (g_libglesv3) p = dlsym(g_libglesv3, alt);
                if (!p && eglGetProcAddress) p = filter_es3_unimpl((void *)eglGetProcAddress(alt));
            }
        }
    }
    return p;
}

static void *resolve_opt_gl_proc(void **slot, uint32_t *state,
                                 const char *desktop_name,
                                 const char *es3_name,
                                 const char *es3_req_ver_substr,
                                 const char *es3_req_ext_substr) {
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2u) {
        pthread_mutex_lock(&g_wgl_mu);
        if (*state != 2u) {
            void *p = NULL;
            if (g_egl_desktop_gl) {
                p = desktop_name ? lookup_native_gl_proc(desktop_name) : NULL;
            } else if (es3_name) {
                bool ok = (!es3_req_ver_substr && !es3_req_ext_substr);
                if (!ok && es3_req_ver_substr && es3_req_ver_substr[0]) {
                    const char *ver = (const char *)glGetString((GLenum)SE_GL_VERSION);
                    if (ver && strstr(ver, es3_req_ver_substr) != NULL) ok = true;
                }
                if (!ok && es3_req_ext_substr && es3_req_ext_substr[0]) {
                    const char *exts = (const char *)glGetString((GLenum)SE_GL_EXTENSIONS);
                    if (exts && strstr(exts, es3_req_ext_substr) != NULL) ok = true;
                }
                if (ok) {
                    p = lookup_native_gl_proc(es3_name);
                }
            }
            *slot = p;
            __atomic_store_n(state, 2u, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_wgl_mu);
    }
    return *slot;
}
#endif

static void ensure_mesa_zink_env(void) {
    if (!getenv("LIBGL_DRIVERS_PATH")) {
        Dl_info dli;
        if (dladdr((const void *)ensure_mesa_zink_env, &dli) && dli.dli_fname) {
            const char *slash = strrchr(dli.dli_fname, '/');
            if (slash && (size_t)(slash - dli.dli_fname) < 512) {
                char libdir[512];
                size_t len = (size_t)(slash - dli.dli_fname);
                memcpy(libdir, dli.dli_fname, len);
                libdir[len] = '\0';
                setenv("LIBGL_DRIVERS_PATH", libdir, 0);
            }
        }
    }
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
    setenv("GALLIUM_DRIVER", "zink", 0);
    setenv("EGL_PLATFORM", "android", 0);
}

static bool init_egl_backend_locked(void) {
    if (g_egl_init_done) return (g_egl_dpy != EGL_NO_DISPLAY && g_egl_cfg != NULL);
    g_egl_init_done = true;

    ensure_mesa_zink_env();

    /* 1. Try Mesa Zink Kopper (libEGL_mesa.so -> libzink_dri.so over Vulkan) */
    g_libegl = dlopen("libEGL_mesa.so", RTLD_NOW | RTLD_LOCAL);
    if (g_libegl && resolve_egl_dispatch(g_libegl)) {
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint maj = 0, min = 0;
        if (dpy != EGL_NO_DISPLAY && eglInitialize(dpy, &maj, &min) &&
            eglBindAPI && eglBindAPI(EGL_OPENGL_API)) {
            const EGLint desktop_attribs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 24,
                EGL_NONE
            };
            EGLConfig cfg = NULL;
            EGLint ncfg = 0;
            if (!eglChooseConfig(dpy, desktop_attribs, &cfg, 1, &ncfg) || ncfg == 0 || !cfg) {
                const EGLint window_only_attribs[] = {
                    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                    EGL_RED_SIZE, 8,
                    EGL_GREEN_SIZE, 8,
                    EGL_BLUE_SIZE, 8,
                    EGL_ALPHA_SIZE, 8,
                    EGL_DEPTH_SIZE, 24,
                    EGL_NONE
                };
                eglChooseConfig(dpy, window_only_attribs, &cfg, 1, &ncfg);
            }
            if (ncfg > 0 && cfg) {
                g_egl_dpy = dpy;
                g_egl_cfg = cfg;
                g_egl_desktop_gl = true;
                g_libglapi = dlopen("libglapi.so", RTLD_LAZY | RTLD_LOCAL);
                WGL_LOG("EGL initialized with Mesa Zink Kopper EGL_OPENGL_API (EGL %d.%d)", maj, min);
            } else {
                WGL_LOG("Mesa Zink eglChooseConfig returned %d configs (err=0x%x)",
                        ncfg, eglGetError ? eglGetError() : 0);
            }
        } else {
            WGL_LOG("Mesa Zink dpy/init/bind failed (dpy=%p err=0x%x)",
                    (void *)dpy, eglGetError ? eglGetError() : 0);
        }
        if (!g_egl_desktop_gl) {
            WGL_LOG("Mesa Zink EGL_OPENGL_API unavailable (err=0x%x); falling back to system EGL/GLES3",
                    eglGetError ? eglGetError() : 0);
            memset(&g_egl, 0, sizeof(g_egl));
            dlclose(g_libegl);
            g_libegl = NULL;
        }
    } else if (g_libegl) {
        WGL_LOG("libEGL_mesa.so missing expected EGL entrypoints; falling back to system EGL");
        dlclose(g_libegl);
        g_libegl = NULL;
        memset(&g_egl, 0, sizeof(g_egl));
    }

    /* 2. Fall back cleanly to Android system libEGL.so + libGLESv3.so */
    if (!g_egl_desktop_gl) {
        g_libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
        g_libglesv3 = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_libegl || !resolve_egl_dispatch(g_libegl)) {
            WGL_LOG("System libEGL.so dlopen/resolve failed (%s)", dlerror());
            return false;
        }
        g_egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_egl_dpy == EGL_NO_DISPLAY) {
            WGL_LOG("eglGetDisplay failed");
            return false;
        }
        EGLint maj = 0, min = 0;
        if (!eglInitialize(g_egl_dpy, &maj, &min)) {
            WGL_LOG("eglInitialize failed (err=0x%x)", eglGetError ? eglGetError() : 0);
            g_egl_dpy = EGL_NO_DISPLAY;
            return false;
        }
        if (eglBindAPI) eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint es3_attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 24,
            EGL_NONE
        };
        EGLint ncfg = 0;
        if (!eglChooseConfig(g_egl_dpy, es3_attribs, &g_egl_cfg, 1, &ncfg) || ncfg == 0) {
            const EGLint fallback_attribs[] = {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_NONE
            };
            eglChooseConfig(g_egl_dpy, fallback_attribs, &g_egl_cfg, 1, &ncfg);
        }
        g_egl_desktop_gl = false;
        WGL_LOG("EGL initialized with EGL_OPENGL_ES_API (GLES3 fallback, EGL %d.%d)", maj, min);
    }

    se_resolve_direct_gl_procs(lookup_native_gl_proc);

    /* Cache optional extended GL / GLES3 entrypoints */
    g_native_glGetStringi = (const GLubyte *(*)(GLenum, GLuint))lookup_native_gl_proc("glGetStringi");
    g_native_glPolygonMode = (void (*)(GLenum, GLenum))lookup_native_gl_proc("glPolygonMode");
    g_native_glTexImage1D = (void (*)(GLenum, GLint, GLint, GLsizei, GLint, GLenum, GLenum, const void *))lookup_native_gl_proc("glTexImage1D");
    g_native_glTexSubImage1D = (void (*)(GLenum, GLint, GLint, GLsizei, GLenum, GLenum, const void *))lookup_native_gl_proc("glTexSubImage1D");
    g_native_glGetTexImage = (void (*)(GLenum, GLint, GLenum, GLenum, void *))lookup_native_gl_proc("glGetTexImage");
    g_native_glGetTexLevelParameteriv = (void (*)(GLenum, GLint, GLenum, GLint *))lookup_native_gl_proc("glGetTexLevelParameteriv");
    g_native_glClearDepth = (void (*)(double))lookup_native_gl_proc("glClearDepth");
    g_native_glDepthRange = (void (*)(double, double))lookup_native_gl_proc("glDepthRange");
    g_native_glDrawBuffer = (void (*)(GLenum))lookup_native_gl_proc("glDrawBuffer");
    g_native_glClipControl = (void (*)(GLenum, GLenum))lookup_native_gl_proc("glClipControl");
    g_native_glBufferStorage = (void (*)(GLenum, ptrdiff_t, const void *, GLbitfield))lookup_native_gl_proc("glBufferStorage");
    g_native_glCreateTextures = (void (*)(GLenum, GLsizei, GLuint *))lookup_native_gl_proc("glCreateTextures");
    g_native_glTextureStorage2D = (void (*)(GLuint, GLsizei, GLenum, GLsizei, GLsizei))lookup_native_gl_proc("glTextureStorage2D");
    g_native_glTexStorage2D = (void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei))lookup_native_gl_proc("glTexStorage2D");
    g_native_glTexStorage3D = (void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei))lookup_native_gl_proc("glTexStorage3D");
    g_native_glBindImageTexture = (void (*)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum))lookup_native_gl_proc("glBindImageTexture");
    g_native_glMemoryBarrier = (void (*)(GLbitfield))lookup_native_gl_proc("glMemoryBarrier");
    g_native_glDispatchCompute = (void (*)(GLuint, GLuint, GLuint))lookup_native_gl_proc("glDispatchCompute");
    g_native_glBlendFuncSeparate = (void (*)(GLenum, GLenum, GLenum, GLenum))lookup_native_gl_proc("glBlendFuncSeparate");
    g_native_glBlendEquation = (void (*)(GLenum))lookup_native_gl_proc("glBlendEquation");
    g_native_glBlendEquationSeparate = (void (*)(GLenum, GLenum))lookup_native_gl_proc("glBlendEquationSeparate");
    g_native_glProgramParameteri = (void (*)(GLuint, GLenum, GLint))lookup_native_gl_proc("glProgramParameteri");
    g_native_glGetProgramBinary = (void (*)(GLuint, GLsizei, GLsizei *, GLenum *, void *))lookup_native_gl_proc("glGetProgramBinary");
    g_native_glProgramBinary = (void (*)(GLuint, GLenum, const void *, GLsizei))lookup_native_gl_proc("glProgramBinary");
    g_native_glVertexAttribDivisor = (void (*)(GLuint, GLuint))lookup_native_gl_proc("glVertexAttribDivisor");
    g_native_glDrawArraysInstanced = (void (*)(GLenum, GLint, GLsizei, GLsizei))lookup_native_gl_proc("glDrawArraysInstanced");
    g_native_glDrawElementsInstanced = (void (*)(GLenum, GLsizei, GLenum, const void *, GLsizei))lookup_native_gl_proc("glDrawElementsInstanced");
    g_native_glBlitFramebuffer = (void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))lookup_native_gl_proc("glBlitFramebuffer");
    g_native_glRenderbufferStorageMultisample = (void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei))lookup_native_gl_proc("glRenderbufferStorageMultisample");
    g_native_glTexImage2DMultisample = (void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean))lookup_native_gl_proc("glTexImage2DMultisample");
    g_native_glDebugMessageCallback = (void (*)(const void *, const void *))lookup_native_gl_proc("glDebugMessageCallback");
    g_native_glObjectLabel = (void (*)(GLenum, GLuint, GLsizei, const char *))lookup_native_gl_proc("glObjectLabel");
    g_native_glPushDebugGroup = (void (*)(GLenum, GLuint, GLsizei, const char *))lookup_native_gl_proc("glPushDebugGroup");
    g_native_glPopDebugGroup = (void (*)(void))lookup_native_gl_proc("glPopDebugGroup");
    g_native_glTexParameterfv = (void (*)(GLenum, GLenum, const GLfloat *))lookup_native_gl_proc("glTexParameterfv");
    g_native_glSamplerParameteriv = (void (*)(GLuint, GLenum, const GLint *))lookup_native_gl_proc("glSamplerParameteriv");

    return (g_egl_cfg != NULL);
}

static se_wgl_ctx_t *find_ctx_locked(uint64_t handle);

static bool ensure_egl_window_surface_locked(void) {
    if (!init_egl_backend_locked()) return false;
    void *anw = se_u32_get_native_window();
    if (!anw) {
        if (g_egl_win_surf != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl_dpy, g_egl_win_surf);
            g_egl_win_surf = EGL_NO_SURFACE;
            g_egl_bound_anw = NULL;
        }
        return false;
    }
    if (anw != g_egl_bound_anw) {
        if (g_egl_win_surf != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl_dpy, g_egl_win_surf);
            g_egl_win_surf = EGL_NO_SURFACE;
        }
        g_egl_win_surf = eglCreateWindowSurface(g_egl_dpy, g_egl_cfg, (EGLNativeWindowType)anw, NULL);
        g_egl_bound_anw = anw;
    }
    return (g_egl_win_surf != EGL_NO_SURFACE);
}

static bool materialize_egl_ctx_locked(se_wgl_ctx_t *ctx) {
    if (!ctx || !init_egl_backend_locked()) return false;
    if (ctx->materialized && ctx->egl_ctx != EGL_NO_CONTEXT) return true;

    EGLContext share = EGL_NO_CONTEXT;
    if (ctx->share_master) {
        se_wgl_ctx_t *m = find_ctx_locked(ctx->share_master);
        if (m && materialize_egl_ctx_locked(m)) {
            share = m->egl_ctx;
        }
    }

    if (g_egl_desktop_gl) {
        eglBindAPI(EGL_OPENGL_API);
        const EGLint gl_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION, (ctx->major_ver > 0) ? ctx->major_ver : 4,
            EGL_CONTEXT_MINOR_VERSION, (ctx->major_ver > 0) ? ctx->minor_ver : 6,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        ctx->egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, share, gl_attrs);
        if (ctx->egl_ctx == EGL_NO_CONTEXT) {
            const EGLint simple_attrs[] = { EGL_NONE };
            ctx->egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, share, simple_attrs);
        }
    } else {
        eglBindAPI(EGL_OPENGL_ES_API);
        static const EGLint es32_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 2,
            EGL_NONE
        };
        static const EGLint es30_attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 0,
            EGL_NONE
        };
        static const EGLint es3_attrs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
        };
        static const EGLint es2_attrs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        const EGLint *const attempts[] = { es32_attrs, es30_attrs, es3_attrs, es2_attrs };
        for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]) && ctx->egl_ctx == EGL_NO_CONTEXT; ++i) {
            ctx->egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, share, attempts[i]);
        }
    }

    if (ctx->pbuffer_surf == EGL_NO_SURFACE) {
        const EGLint pb_attrs[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        ctx->pbuffer_surf = eglCreatePbufferSurface(g_egl_dpy, g_egl_cfg, pb_attrs);
    }

    ctx->materialized = (ctx->egl_ctx != EGL_NO_CONTEXT);
    return ctx->materialized;
}
#endif /* __ANDROID__ */

static se_wgl_ctx_t *find_ctx_locked(uint64_t handle) {
    for (int i = 0; i < MAX_WGL_CONTEXTS; ++i) {
        if (g_ctxs[i].used && g_ctxs[i].handle == handle) return &g_ctxs[i];
    }
    return NULL;
}

static se_wgl_ctx_t *alloc_ctx_locked(uint64_t hdc, uint64_t share_master,
                                      int maj, int min, int profile, int flags) {
    for (int i = 0; i < MAX_WGL_CONTEXTS; ++i) {
        if (!g_ctxs[i].used) {
            memset(&g_ctxs[i], 0, sizeof(g_ctxs[i]));
            g_ctxs[i].used = true;
            g_ctxs[i].handle = g_next_ctx_handle++;
            g_ctxs[i].hdc = hdc;
            g_ctxs[i].share_master = share_master;
            g_ctxs[i].major_ver = (maj > 0) ? maj : 4;
            g_ctxs[i].minor_ver = (maj > 0) ? min : 6;
            g_ctxs[i].profile_mask = profile ? profile : (int)SE_GL_CONTEXT_CORE_PROFILE_BIT;
            g_ctxs[i].flags = flags;
            /* Unshared main-window contexts (temp + core) bind the onscreen ANativeWindow surface. */
            g_ctxs[i].is_main = (share_master == 0ull) &&
                                (hdc == 0ull || hdc == 0x0000000000020010ull /* SE_MAIN_HDC */);
#if defined(__ANDROID__)
            g_ctxs[i].egl_ctx = EGL_NO_CONTEXT;
            g_ctxs[i].pbuffer_surf = EGL_NO_SURFACE;
            g_ctxs[i].materialized = false;
#endif
            return &g_ctxs[i];
        }
    }
    return NULL;
}

static bool ext_enabled_for_current_mode_locked(const char *ext) {
    if (g_egl_desktop_gl) return true;
    if (strcmp(ext, "GL_ARB_direct_state_access") == 0) return false;
    if (strcmp(ext, "GL_EXT_direct_state_access") == 0) return false;
    if (strcmp(ext, "GL_NV_depth_buffer_float") == 0) return false;
    /* L_0001401F7ED0/0x1401f6d9d call glQueryCounter/glGetQueryObjectui64v unconditionally; keep GL_ARB_timer_query enabled so GLEW populates them via our EXT/no-op shims. */
    if (strcmp(ext, "GL_ARB_timer_query") == 0) return true;
    if (strcmp(ext, "GL_ARB_internalformat_query2") == 0) return false;
    if (strcmp(ext, "GL_ARB_parallel_shader_compile") == 0) return false;
    /* 0x140197a3e/0x1401aedd3 call glBufferStorage without checking __GLEW_ARB_buffer_storage; hle_gl_BufferStorage falls back to glBufferData when g_es3_has_buffer_storage is false. */
    if (strcmp(ext, "GL_ARB_buffer_storage") == 0) return true;
    if (strcmp(ext, "GL_ARB_clip_control") == 0) return g_es3_has_clip_control;
    if (strcmp(ext, "GL_ARB_blend_func_extended") == 0) return g_es3_has_blend_func_extended;
    return true;
}

static void invalidate_ext_cache_locked(void) {
    g_ext_str_guest = 0ull;
    memset(g_ext_item_guest, 0, sizeof(g_ext_item_guest));
    g_active_ext_count = 0u;
    g_ext_cache_dirty = true;
}

static void rebuild_active_exts_locked(void) {
    if (!g_ext_cache_dirty) return;
    g_active_ext_count = 0u;
    for (uint32_t i = 0; i < NUM_GL_EXTENSIONS; ++i) {
        if (ext_enabled_for_current_mode_locked(k_gl_extensions[i])) {
            g_active_exts[g_active_ext_count++] = k_gl_extensions[i];
        }
    }
    g_ext_cache_dirty = false;
}

static uint64_t get_ext_string_guest(void) {
    pthread_mutex_lock(&g_wgl_mu);
    rebuild_active_exts_locked();
    if (!g_ext_str_guest) {
        char buf[2048];
        size_t pos = 0;
        for (uint32_t i = 0; i < g_active_ext_count; ++i) {
            size_t len = strlen(g_active_exts[i]);
            if (pos + len + 2 >= sizeof(buf)) break;
            if (pos > 0) buf[pos++] = ' ';
            memcpy(buf + pos, g_active_exts[i], len);
            pos += len;
        }
        buf[pos] = '\0';
        g_ext_str_guest = se_hle_intern_ascii(buf);
    }
    uint64_t r = g_ext_str_guest;
    pthread_mutex_unlock(&g_wgl_mu);
    return r;
}

void se_wgl_set_desktop_gl_mode(bool desktop) {
    pthread_mutex_lock(&g_wgl_mu);
    g_egl_desktop_gl = desktop;
    if (desktop) {
        g_es3_caps_probed = false;
        g_es3_has_s3tc = false;
        g_es3_has_rgtc = false;
        g_es3_has_clip_control = false;
        g_es3_has_buffer_storage = false;
        g_es3_has_blend_func_extended = false;
    }
    invalidate_ext_cache_locked();
    pthread_mutex_unlock(&g_wgl_mu);
}

bool se_wgl_is_desktop_gl(void) {
    pthread_mutex_lock(&g_wgl_mu);
    bool v = g_egl_desktop_gl;
    pthread_mutex_unlock(&g_wgl_mu);
    return v;
}

void se_wgl_set_es3_driver_caps(bool s3tc, bool rgtc, bool clip_control,
                                bool buffer_storage, bool blend_func_extended) {
    pthread_mutex_lock(&g_wgl_mu);
    g_es3_has_s3tc = s3tc;
    g_es3_has_rgtc = rgtc;
    g_es3_has_clip_control = clip_control;
    g_es3_has_buffer_storage = buffer_storage;
    g_es3_has_blend_func_extended = blend_func_extended;
    g_es3_caps_probed = true;
    invalidate_ext_cache_locked();
    pthread_mutex_unlock(&g_wgl_mu);
}

void se_wgl_set_shader_capture_cb(void (*cb)(uint32_t shader, uint32_t stage, const char *translated, void *user),
                                  void *user) {
    pthread_mutex_lock(&g_wgl_mu);
    g_shader_capture_cb = cb;
    g_shader_capture_user = user;
    pthread_mutex_unlock(&g_wgl_mu);
}

static void track_shader_stage(uint32_t id, uint32_t stage) {
    if (id == 0u) return;
    pthread_mutex_lock(&g_wgl_mu);
    int free_idx = -1;
    for (int i = 0; i < SE_MAX_SHADER_TYPES; ++i) {
        if (g_shader_slots[i].used && g_shader_slots[i].id == id) {
            g_shader_slots[i].stage = stage;
            pthread_mutex_unlock(&g_wgl_mu);
            return;
        }
        if (!g_shader_slots[i].used && free_idx < 0) free_idx = i;
    }
    if (free_idx >= 0) {
        g_shader_slots[free_idx].used = true;
        g_shader_slots[free_idx].id = id;
        g_shader_slots[free_idx].stage = stage;
    }
    pthread_mutex_unlock(&g_wgl_mu);
}

static uint32_t lookup_shader_stage(uint32_t id) {
    uint32_t stage = 0u;
    if (id == 0u) return 0u;
    pthread_mutex_lock(&g_wgl_mu);
    for (int i = 0; i < SE_MAX_SHADER_TYPES; ++i) {
        if (g_shader_slots[i].used && g_shader_slots[i].id == id) {
            stage = g_shader_slots[i].stage;
            break;
        }
    }
    pthread_mutex_unlock(&g_wgl_mu);
    return stage;
}

static void untrack_shader_stage(uint32_t id) {
    if (id == 0u) return;
    pthread_mutex_lock(&g_wgl_mu);
    for (int i = 0; i < SE_MAX_SHADER_TYPES; ++i) {
        if (g_shader_slots[i].used && g_shader_slots[i].id == id) {
            g_shader_slots[i].used = false;
            g_shader_slots[i].id = 0u;
            g_shader_slots[i].stage = 0u;
            break;
        }
    }
    pthread_mutex_unlock(&g_wgl_mu);
}

uint32_t se_wgl_normalize_es3_target(uint32_t target) {
    if (!se_wgl_is_desktop_gl() && target == SE_GL_TEXTURE_1D) {
        return SE_GL_TEXTURE_2D;
    }
    return target;
}

int32_t se_wgl_normalize_es3_tex_param_i(uint32_t pname, int32_t param) {
    if (!se_wgl_is_desktop_gl() &&
        (pname == SE_GL_TEXTURE_WRAP_S || pname == SE_GL_TEXTURE_WRAP_T || pname == SE_GL_TEXTURE_WRAP_R) &&
        (uint32_t)param == SE_GL_CLAMP) {
        return (int32_t)SE_GL_CLAMP_TO_EDGE;
    }
    return param;
}

float se_wgl_normalize_es3_tex_param_f(uint32_t pname, float param) {
    if (!se_wgl_is_desktop_gl() &&
        (pname == SE_GL_TEXTURE_WRAP_S || pname == SE_GL_TEXTURE_WRAP_T || pname == SE_GL_TEXTURE_WRAP_R) &&
        (param > 10495.5f && param < 10496.5f)) {
        return (float)SE_GL_CLAMP_TO_EDGE;
    }
    return param;
}

int32_t se_wgl_normalize_es3_internalformat(int32_t internalformat, uint32_t format, uint32_t type, bool for_storage) {
    (void)type;
    if (se_wgl_is_desktop_gl()) return internalformat;
    switch ((uint32_t)internalformat) {
    case 1u:
    case SE_GL_LUMINANCE:
    case SE_GL_ALPHA:
        return (int32_t)SE_GL_R8;
    case 2u:
    case SE_GL_LUMINANCE_ALPHA:
        return (int32_t)SE_GL_RG8;
    case 3u:
        return (int32_t)SE_GL_RGB8;
    case 4u:
        return (int32_t)SE_GL_RGBA8;
    case 0x8054u: /* GL_RGB16 */
        return (int32_t)SE_GL_RGB16F;
    case 0x805Bu: /* GL_RGBA16 */
        return (int32_t)SE_GL_RGBA16F;
    case 0x822Au: /* GL_R16 */
        return (int32_t)SE_GL_R16F;
    case 0x822Cu: /* GL_RG16 */
        return (int32_t)SE_GL_RG16F;
    case SE_GL_DEPTH_COMPONENT:
    case SE_GL_DEPTH_COMPONENT32:
        return (int32_t)SE_GL_DEPTH_COMPONENT24;
    case 0x84EDu: /* GL_COMPRESSED_RGB */
        return (int32_t)SE_GL_RGB8;
    case 0x84EEu: /* GL_COMPRESSED_RGBA */
        return (int32_t)SE_GL_RGBA8;
    case SE_GL_RGB:
        return for_storage ? (int32_t)SE_GL_RGB8 : internalformat;
    case SE_GL_RGBA:
        return for_storage ? (int32_t)SE_GL_RGBA8 : internalformat;
    case SE_GL_RED:
        return for_storage ? (int32_t)SE_GL_R8 : internalformat;
    case SE_GL_RG:
        return for_storage ? (int32_t)SE_GL_RG8 : internalformat;
    case SE_GL_BGR:
        return (int32_t)SE_GL_RGB8;
    case SE_GL_BGRA:
        return (int32_t)SE_GL_RGBA8;
    case SE_GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        return g_es3_has_s3tc ? internalformat : (int32_t)SE_GL_RGBA8;
    case SE_GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT1_EXT:
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT3_EXT:
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT5_EXT:
        return g_es3_has_s3tc ? internalformat : (int32_t)SE_GL_SRGB8_ALPHA8;
    case SE_GL_COMPRESSED_RED_RGTC1:
        return g_es3_has_rgtc ? internalformat : (int32_t)SE_GL_R8;
    case SE_GL_COMPRESSED_SIGNED_RED_RGTC1:
        return g_es3_has_rgtc ? internalformat : (int32_t)SE_GL_R8_SNORM;
    case SE_GL_COMPRESSED_RG_RGTC2:
        return g_es3_has_rgtc ? internalformat : (int32_t)SE_GL_RG8;
    case SE_GL_COMPRESSED_SIGNED_RG_RGTC2:
        return g_es3_has_rgtc ? internalformat : (int32_t)SE_GL_RG8_SNORM;
    default:
        break;
    }
    if (for_storage && format == SE_GL_RGBA && internalformat == (int32_t)SE_GL_RGBA) {
        return (int32_t)SE_GL_RGBA8;
    }
    return internalformat;
}

static void decode_rgb565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
    uint8_t g6 = (uint8_t)((c >> 5) & 0x3Fu);
    uint8_t b5 = (uint8_t)(c & 0x1Fu);
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static void decode_bc_color_block(const uint8_t *src, bool dxt1_mode, bool punchthrough_alpha, uint8_t out_rgba[16][4]) {
    uint16_t c0 = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
    uint16_t c1 = (uint16_t)(src[2] | ((uint16_t)src[3] << 8));
    uint32_t bits = (uint32_t)src[4] | ((uint32_t)src[5] << 8) | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
    uint8_t pal[4][4];
    decode_rgb565(c0, &pal[0][0], &pal[0][1], &pal[0][2]);
    decode_rgb565(c1, &pal[1][0], &pal[1][1], &pal[1][2]);
    pal[0][3] = 255u;
    pal[1][3] = 255u;
    if (!dxt1_mode || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = (uint8_t)((2u * pal[0][ch] + pal[1][ch] + 1u) / 3u);
            pal[3][ch] = (uint8_t)((pal[0][ch] + 2u * pal[1][ch] + 1u) / 3u);
        }
        pal[2][3] = 255u;
        pal[3][3] = 255u;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = (uint8_t)((pal[0][ch] + pal[1][ch]) >> 1);
            pal[3][ch] = 0u;
        }
        pal[2][3] = 255u;
        pal[3][3] = punchthrough_alpha ? 0u : 255u;
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t idx = (bits >> (i * 2)) & 0x3u;
        memcpy(out_rgba[i], pal[idx], 4u);
    }
}

static void decode_bc_u8_block(const uint8_t *src, uint8_t out_vals[16]) {
    uint8_t a0 = src[0], a1 = src[1];
    uint8_t pal[8];
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        for (int k = 0; k < 6; ++k) {
            pal[2 + k] = (uint8_t)(((6 - k) * (int32_t)a0 + (1 + k) * (int32_t)a1 + 3) / 7);
        }
    } else {
        for (int k = 0; k < 4; ++k) {
            pal[2 + k] = (uint8_t)(((4 - k) * (int32_t)a0 + (1 + k) * (int32_t)a1 + 2) / 5);
        }
        pal[6] = 0u;
        pal[7] = 255u;
    }
    uint64_t idx_bits = 0ull;
    for (int i = 0; i < 6; ++i) idx_bits |= ((uint64_t)src[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) {
        out_vals[i] = pal[(idx_bits >> (3 * i)) & 0x7ull];
    }
}

static void decode_bc_s8_block(const uint8_t *src, int8_t out_vals[16]) {
    int32_t a0 = (int8_t)src[0];
    int32_t a1 = (int8_t)src[1];
    int8_t pal[8];
    pal[0] = (int8_t)a0;
    pal[1] = (int8_t)a1;
    if (a0 > a1) {
        for (int k = 0; k < 6; ++k) {
            int32_t num = (6 - k) * a0 + (1 + k) * a1;
            pal[2 + k] = (int8_t)((num >= 0 ? (num + 3) : (num - 3)) / 7);
        }
    } else {
        for (int k = 0; k < 4; ++k) {
            int32_t num = (4 - k) * a0 + (1 + k) * a1;
            pal[2 + k] = (int8_t)((num >= 0 ? (num + 2) : (num - 2)) / 5);
        }
        pal[6] = (int8_t)-127;
        pal[7] = (int8_t)127;
    }
    uint64_t idx_bits = 0ull;
    for (int i = 0; i < 6; ++i) idx_bits |= ((uint64_t)src[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) {
        out_vals[i] = pal[(idx_bits >> (3 * i)) & 0x7ull];
    }
}

uint8_t *se_wgl_decode_bc_to_uncompressed(uint32_t internalformat, int32_t width, int32_t height,
                                          int32_t image_size, const void *data,
                                          uint32_t *out_internalformat, uint32_t *out_format,
                                          uint32_t *out_type) {
    if (!data || width <= 0 || height <= 0 || !out_internalformat || !out_format || !out_type) {
        return NULL;
    }
    uint32_t block_bytes = 0u;
    uint32_t bpp = 0u;
    uint32_t ifmt = 0u, fmt = 0u, tp = SE_GL_UNSIGNED_BYTE;
    bool is_bc1_rgb = false, is_bc1_rgba = false, is_bc2 = false, is_bc3 = false;
    bool is_bc4_u = false, is_bc4_s = false, is_bc5_u = false, is_bc5_s = false;

    switch (internalformat) {
    case SE_GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
        block_bytes = 8u; bpp = 4u; ifmt = SE_GL_RGBA8; fmt = SE_GL_RGBA; is_bc1_rgb = true; break;
    case SE_GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
        block_bytes = 8u; bpp = 4u; ifmt = SE_GL_SRGB8_ALPHA8; fmt = SE_GL_RGBA; is_bc1_rgb = true; break;
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
        block_bytes = 8u; bpp = 4u; ifmt = SE_GL_RGBA8; fmt = SE_GL_RGBA; is_bc1_rgba = true; break;
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT1_EXT:
        block_bytes = 8u; bpp = 4u; ifmt = SE_GL_SRGB8_ALPHA8; fmt = SE_GL_RGBA; is_bc1_rgba = true; break;
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
        block_bytes = 16u; bpp = 4u; ifmt = SE_GL_RGBA8; fmt = SE_GL_RGBA; is_bc2 = true; break;
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT3_EXT:
        block_bytes = 16u; bpp = 4u; ifmt = SE_GL_SRGB8_ALPHA8; fmt = SE_GL_RGBA; is_bc2 = true; break;
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
        block_bytes = 16u; bpp = 4u; ifmt = SE_GL_RGBA8; fmt = SE_GL_RGBA; is_bc3 = true; break;
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT5_EXT:
        block_bytes = 16u; bpp = 4u; ifmt = SE_GL_SRGB8_ALPHA8; fmt = SE_GL_RGBA; is_bc3 = true; break;
    case SE_GL_COMPRESSED_RED_RGTC1:
        block_bytes = 8u; bpp = 1u; ifmt = SE_GL_R8; fmt = SE_GL_RED; is_bc4_u = true; break;
    case SE_GL_COMPRESSED_SIGNED_RED_RGTC1:
        block_bytes = 8u; bpp = 1u; ifmt = SE_GL_R8_SNORM; fmt = SE_GL_RED; tp = SE_GL_BYTE; is_bc4_s = true; break;
    case SE_GL_COMPRESSED_RG_RGTC2:
        block_bytes = 16u; bpp = 2u; ifmt = SE_GL_RG8; fmt = SE_GL_RG; is_bc5_u = true; break;
    case SE_GL_COMPRESSED_SIGNED_RG_RGTC2:
        block_bytes = 16u; bpp = 2u; ifmt = SE_GL_RG8_SNORM; fmt = SE_GL_RG; tp = SE_GL_BYTE; is_bc5_s = true; break;
    default:
        return NULL;
    }

    size_t bw = ((size_t)width + 3u) / 4u;
    size_t bh = ((size_t)height + 3u) / 4u;
    size_t need_bytes = bw * bh * (size_t)block_bytes;
    if (image_size > 0 && (size_t)image_size < need_bytes) {
        return NULL;
    }

    size_t dst_size = (size_t)width * (size_t)height * (size_t)bpp;
    uint8_t *dst = (uint8_t *)malloc(dst_size ? dst_size : 1u);
    if (!dst) return NULL;

    const uint8_t *src = (const uint8_t *)data;
    for (size_t by = 0; by < bh; ++by) {
        for (size_t bx = 0; bx < bw; ++bx) {
            const uint8_t *blk = src + (by * bw + bx) * (size_t)block_bytes;
            uint8_t rgba[16][4];
            uint8_t u0[16], u1[16];
            int8_t s0[16], s1[16];

            if (is_bc1_rgb || is_bc1_rgba) {
                decode_bc_color_block(blk, true, is_bc1_rgba, rgba);
            } else if (is_bc2) {
                decode_bc_color_block(blk + 8, false, false, rgba);
                for (int i = 0; i < 8; ++i) {
                    uint8_t a_lo = (uint8_t)(blk[i] & 0x0Fu);
                    uint8_t a_hi = (uint8_t)((blk[i] >> 4) & 0x0Fu);
                    rgba[2 * i][3] = (uint8_t)((a_lo << 4) | a_lo);
                    rgba[2 * i + 1][3] = (uint8_t)((a_hi << 4) | a_hi);
                }
            } else if (is_bc3) {
                decode_bc_u8_block(blk, u0);
                decode_bc_color_block(blk + 8, false, false, rgba);
                for (int i = 0; i < 16; ++i) rgba[i][3] = u0[i];
            } else if (is_bc4_u) {
                decode_bc_u8_block(blk, u0);
            } else if (is_bc4_s) {
                decode_bc_s8_block(blk, s0);
            } else if (is_bc5_u) {
                decode_bc_u8_block(blk, u0);
                decode_bc_u8_block(blk + 8, u1);
            } else if (is_bc5_s) {
                decode_bc_s8_block(blk, s0);
                decode_bc_s8_block(blk + 8, s1);
            }

            for (size_t py = 0; py < 4u; ++py) {
                size_t y = by * 4u + py;
                if (y >= (size_t)height) continue;
                for (size_t px = 0; px < 4u; ++px) {
                    size_t x = bx * 4u + px;
                    if (x >= (size_t)width) continue;
                    size_t ti = py * 4u + px;
                    uint8_t *outp = dst + (y * (size_t)width + x) * (size_t)bpp;
                    if (bpp == 4u) {
                        memcpy(outp, rgba[ti], 4u);
                    } else if (is_bc4_u) {
                        outp[0] = u0[ti];
                    } else if (is_bc4_s) {
                        outp[0] = (uint8_t)s0[ti];
                    } else if (is_bc5_u) {
                        outp[0] = u0[ti];
                        outp[1] = u1[ti];
                    } else if (is_bc5_s) {
                        outp[0] = (uint8_t)s0[ti];
                        outp[1] = (uint8_t)s1[ti];
                    }
                }
            }
        }
    }

    *out_internalformat = ifmt;
    *out_format = fmt;
    *out_type = tp;
    return dst;
}

uint32_t se_wgl_normalize_es3_format(uint32_t format) {
    if (se_wgl_is_desktop_gl()) return format;
    switch (format) {
    case SE_GL_BGR:
        return SE_GL_RGB;
    case SE_GL_BGRA:
        return SE_GL_RGBA;
    case SE_GL_LUMINANCE:
    case SE_GL_ALPHA:
        return SE_GL_RED;
    case SE_GL_LUMINANCE_ALPHA:
        return SE_GL_RG;
    default:
        return format;
    }
}

uint8_t *se_wgl_alloc_es3_uncompressed_pixels(uint32_t format, uint32_t type,
                                              int32_t width, int32_t height, int32_t depth,
                                              const void *pixels,
                                              uint32_t *out_format, uint32_t *out_type) {
    uint32_t nfmt = se_wgl_normalize_es3_format(format);
    uint32_t ntype = type;
    bool is_rev_bgra = (format == SE_GL_BGRA && type == SE_GL_UNSIGNED_INT_8_8_8_8_REV);
    if (!se_wgl_is_desktop_gl() && is_rev_bgra) {
        ntype = SE_GL_UNSIGNED_BYTE;
    }
    if (out_format) *out_format = nfmt;
    if (out_type) *out_type = ntype;

    if (se_wgl_is_desktop_gl() || !pixels || width <= 0 || height <= 0 || depth <= 0) {
        return NULL;
    }
    if (format != SE_GL_BGR && format != SE_GL_BGRA) {
        return NULL;
    }

    size_t ch = (format == SE_GL_BGR) ? 3u : 4u;
    size_t ch_bytes = 0u;
    if (is_rev_bgra) {
        ch_bytes = 1u;
    } else {
        switch (type) {
        case SE_GL_BYTE:
        case SE_GL_UNSIGNED_BYTE:
            ch_bytes = 1u;
            break;
        case SE_GL_SHORT:
        case SE_GL_UNSIGNED_SHORT:
        case SE_GL_HALF_FLOAT:
            ch_bytes = 2u;
            break;
        case SE_GL_INT:
        case SE_GL_UNSIGNED_INT:
        case SE_GL_FLOAT:
            ch_bytes = 4u;
            break;
        default:
            return NULL;
        }
    }

    size_t n_pixels = (size_t)width * (size_t)height * (size_t)depth;
    size_t stride = ch * ch_bytes;
    if (n_pixels > ((size_t)-1) / stride) return NULL;
    uint8_t *dst = (uint8_t *)malloc(n_pixels * stride);
    if (!dst) return NULL;

    const uint8_t *src = (const uint8_t *)pixels;
    for (size_t p = 0; p < n_pixels; ++p) {
        const uint8_t *sp = src + p * stride;
        uint8_t *dp = dst + p * stride;
        memcpy(dp, sp + 2u * ch_bytes, ch_bytes);               /* R <- B(sp+2) */
        memcpy(dp + ch_bytes, sp + ch_bytes, ch_bytes);         /* G <- G */
        memcpy(dp + 2u * ch_bytes, sp, ch_bytes);               /* B <- R(sp+0) */
        if (ch == 4u) {
            memcpy(dp + 3u * ch_bytes, sp + 3u * ch_bytes, ch_bytes); /* A <- A */
        }
    }
    return dst;
}

static bool is_s3tc_internalformat(uint32_t ifmt) {
    switch (ifmt) {
    case SE_GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case SE_GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
    case SE_GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT1_EXT:
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT3_EXT:
    case SE_GL_COMPRESSED_SRGB_ALPHA_DXT5_EXT:
        return true;
    default:
        return false;
    }
}

static bool is_rgtc_internalformat(uint32_t ifmt) {
    switch (ifmt) {
    case SE_GL_COMPRESSED_RED_RGTC1:
    case SE_GL_COMPRESSED_SIGNED_RED_RGTC1:
    case SE_GL_COMPRESSED_RG_RGTC2:
    case SE_GL_COMPRESSED_SIGNED_RG_RGTC2:
        return true;
    default:
        return false;
    }
}

bool se_wgl_compressed_format_needs_cpu_decode(uint32_t ifmt) {
    if (se_wgl_is_desktop_gl()) return false;
    if (is_s3tc_internalformat(ifmt)) return !g_es3_has_s3tc;
    if (is_rgtc_internalformat(ifmt)) return !g_es3_has_rgtc;
    return false;
}

#if defined(__ANDROID__)
static GLint unpack_align_1_push(void) {
    GLint saved = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved);
    if (saved != 1) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    return saved;
}

static void unpack_align_1_pop(GLint saved) {
    if (saved != 1) glPixelStorei(GL_UNPACK_ALIGNMENT, saved);
}

static void android_tex_image_2d(GLenum target, GLint level, GLint internalformat,
                                 GLsizei width, GLsizei height, GLint border,
                                 GLenum format, GLenum type, const void *pixels) {
    GLenum nt = (GLenum)se_wgl_normalize_es3_target((uint32_t)target);
    GLint nif = (GLint)se_wgl_normalize_es3_internalformat(internalformat, (uint32_t)format, (uint32_t)type, false);
    GLint nborder = se_wgl_is_desktop_gl() ? border : 0;
    uint32_t nfmt = (uint32_t)format, ntype = (uint32_t)type;
    uint8_t *swz = se_wgl_alloc_es3_uncompressed_pixels((uint32_t)format, (uint32_t)type,
                                                        width, height, 1, pixels,
                                                        &nfmt, &ntype);
    if (swz) {
        GLint saved = unpack_align_1_push();
        glTexImage2D(nt, level, nif, width, height, nborder, (GLenum)nfmt, (GLenum)ntype, swz);
        unpack_align_1_pop(saved);
        free(swz);
    } else {
        glTexImage2D(nt, level, nif, width, height, nborder, (GLenum)nfmt, (GLenum)ntype, pixels);
    }
}

static void android_tex_subimage_2d(GLenum target, GLint level, GLint xoff, GLint yoff,
                                    GLsizei width, GLsizei height,
                                    GLenum format, GLenum type, const void *pixels) {
    GLenum nt = (GLenum)se_wgl_normalize_es3_target((uint32_t)target);
    uint32_t nfmt = (uint32_t)format, ntype = (uint32_t)type;
    uint8_t *swz = se_wgl_alloc_es3_uncompressed_pixels((uint32_t)format, (uint32_t)type,
                                                        width, height, 1, pixels,
                                                        &nfmt, &ntype);
    if (swz) {
        GLint saved = unpack_align_1_push();
        glTexSubImage2D(nt, level, xoff, yoff, width, height, (GLenum)nfmt, (GLenum)ntype, swz);
        unpack_align_1_pop(saved);
        free(swz);
    } else {
        glTexSubImage2D(nt, level, xoff, yoff, width, height, (GLenum)nfmt, (GLenum)ntype, pixels);
    }
}
#endif

static void maybe_fire_planetarium_ready(void);

#if !defined(__ANDROID__)
typedef void *(*se_wgl_mock_resolver_t)(const char *name);
static se_wgl_mock_resolver_t g_host_mock_resolver = NULL;
void se_wgl_set_mock_gl_resolver(se_wgl_mock_resolver_t fn) {
    g_host_mock_resolver = fn;
}
static void *lookup_native_gl_proc(const char *name) {
    if (g_host_mock_resolver && name && name[0]) {
        return g_host_mock_resolver(name);
    }
    return NULL;
}
#endif

static void maybe_fire_planetarium_ready(void) {
    se_hle_poll_stage();

    void (*cb)(void *) = NULL;
    void *user = NULL;

    pthread_mutex_lock(&g_wgl_mu);
    bool loaded = g_planetarium_loaded;
    if (!loaded && se_pe_active_base() != 0 && se_pe_active_size() >= 0x780000u) {
        if (rd32(GVA(0x14077B250ull)) == 7u) {
            loaded = true;
            g_planetarium_loaded = true;
        }
    }
    if (loaded && !g_ready_fired && g_ready_cb) {
        g_ready_fired = true;
        cb = g_ready_cb;
        user = g_ready_user;
    }
    pthread_mutex_unlock(&g_wgl_mu);

    if (cb) {
        WGL_LOG("First planetarium frame swapped after stage 7 -> firing on_planetarium_ready()");
        cb(user);
    }
}

void se_wgl_reset_state(void) {
    pthread_mutex_lock(&g_wgl_mu);
#if defined(__ANDROID__)
    if (g_egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        for (int i = 0; i < MAX_WGL_CONTEXTS; ++i) {
            if (g_ctxs[i].used) {
                if (g_ctxs[i].egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_egl_dpy, g_ctxs[i].egl_ctx);
                if (g_ctxs[i].pbuffer_surf != EGL_NO_SURFACE) eglDestroySurface(g_egl_dpy, g_ctxs[i].pbuffer_surf);
            }
        }
        if (g_egl_win_surf != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl_dpy, g_egl_win_surf);
            g_egl_win_surf = EGL_NO_SURFACE;
        }
        g_egl_bound_anw = NULL;
    }
#endif
    memset(g_ctxs, 0, sizeof(g_ctxs));
    g_next_ctx_handle = 0x00030001ull;
    g_next_id = 1u;
    g_swap_interval = 1;
    int sw = 2400, sh = 1080;
    se_host_get_surface_size(&sw, &sh);
    if (sw > 0 && sh > 0) {
        if (sw < sh) { int t = sw; sw = sh; sh = t; }
        g_viewport[0] = 0;
        g_viewport[1] = 0;
        g_viewport[2] = sw;
        g_viewport[3] = sh;
    }
    g_depth_mask = true;
    g_swap_count = 0u;
    g_planetarium_loaded = false;
    g_ready_fired = false;
    memset(g_shader_slots, 0, sizeof(g_shader_slots));
    g_shader_capture_cb = NULL;
    g_shader_capture_user = NULL;
    g_es3_caps_probed = false;
    g_es3_has_s3tc = false;
    g_es3_has_rgtc = false;
    g_es3_has_clip_control = false;
    g_es3_has_buffer_storage = false;
    g_es3_has_blend_func_extended = false;
#if defined(__ANDROID__)
    g_egl_desktop_gl = false;
#else
    g_egl_desktop_gl = true;
#endif
    invalidate_ext_cache_locked();
    tl_cur_hglrc = 0ull;
    tl_cur_hdc = 0ull;
    pthread_mutex_unlock(&g_wgl_mu);
}

void se_wgl_set_ready_callback(void (*cb)(void *), void *user) {
    pthread_mutex_lock(&g_wgl_mu);
    g_ready_cb = cb;
    g_ready_user = user;
    pthread_mutex_unlock(&g_wgl_mu);
}

void se_wgl_set_planetarium_loaded(bool loaded) {
    pthread_mutex_lock(&g_wgl_mu);
    g_planetarium_loaded = loaded;
    pthread_mutex_unlock(&g_wgl_mu);
}

bool se_wgl_is_planetarium_loaded(void) {
    pthread_mutex_lock(&g_wgl_mu);
    bool v = g_planetarium_loaded;
    pthread_mutex_unlock(&g_wgl_mu);
    return v;
}

bool se_wgl_was_ready_fired(void) {
    pthread_mutex_lock(&g_wgl_mu);
    bool v = g_ready_fired;
    pthread_mutex_unlock(&g_wgl_mu);
    return v;
}

uint64_t se_wgl_get_current_context(void) {
    return tl_cur_hglrc;
}

uint32_t se_wgl_get_swap_count(void) {
    pthread_mutex_lock(&g_wgl_mu);
    uint32_t n = g_swap_count;
    pthread_mutex_unlock(&g_wgl_mu);
    return n;
}

/* ========================================================================= */
/* 1. WGL Context & Surface Management                                       */
/* ========================================================================= */

static void hle_wgl_CreateContext(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    pthread_mutex_lock(&g_wgl_mu);
    se_wgl_ctx_t *ctx = alloc_ctx_locked(hdc, 0ull, 4, 6, (int)SE_GL_CONTEXT_CORE_PROFILE_BIT, 0);
    uint64_t h = ctx ? ctx->handle : 0ull;
#if defined(__ANDROID__)
    if (ctx && ctx->is_main) {
        materialize_egl_ctx_locked(ctx);
    }
#endif
    pthread_mutex_unlock(&g_wgl_mu);
    HLE_RET64(c, h);
}

static void hle_wgl_CreateContextAttribsARB(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    uint64_t hShareContext = HLE_ARG1(c);
    uint64_t attribList = HLE_ARG2(c);

    int maj = 4, min = 6, profile = (int)SE_GL_CONTEXT_CORE_PROFILE_BIT, flags = 0;
    if (attribList) {
        for (int i = 0; i < 64; i += 2) {
            int32_t k = (int32_t)rd32(attribList + (uint64_t)i * 4ull);
            if (k == 0) break;
            int32_t v = (int32_t)rd32(attribList + (uint64_t)(i + 1) * 4ull);
            if (k == SE_WGL_CONTEXT_MAJOR_VERSION_ARB) maj = v;
            else if (k == SE_WGL_CONTEXT_MINOR_VERSION_ARB) min = v;
            else if (k == SE_WGL_CONTEXT_PROFILE_MASK_ARB) profile = v;
            else if (k == SE_WGL_CONTEXT_FLAGS_ARB) flags = v;
        }
    }

    pthread_mutex_lock(&g_wgl_mu);
    se_wgl_ctx_t *ctx = alloc_ctx_locked(hdc, hShareContext, maj, min, profile, flags);
    uint64_t h = ctx ? ctx->handle : 0ull;
#if defined(__ANDROID__)
    if (ctx) {
        materialize_egl_ctx_locked(ctx);
    }
#endif
    pthread_mutex_unlock(&g_wgl_mu);
    HLE_RET64(c, h);
}

static void hle_wgl_ShareLists(CPU *c) {
    uint64_t hglrc1 = HLE_ARG0(c);
    uint64_t hglrc2 = HLE_ARG1(c);
    bool ok = false;

    pthread_mutex_lock(&g_wgl_mu);
    se_wgl_ctx_t *c1 = find_ctx_locked(hglrc1);
    se_wgl_ctx_t *c2 = find_ctx_locked(hglrc2);
    if (c1 && c2) {
        c2->share_master = c1->handle;
        c2->is_main = false;
#if defined(__ANDROID__)
        /* If c2 was already created without share context, re-create it shared with c1 */
        if (c2->materialized && c2->egl_ctx != EGL_NO_CONTEXT && g_egl_dpy != EGL_NO_DISPLAY) {
            eglDestroyContext(g_egl_dpy, c2->egl_ctx);
            c2->egl_ctx = EGL_NO_CONTEXT;
            c2->materialized = false;
        }
        ok = materialize_egl_ctx_locked(c2);
#else
        ok = true;
#endif
    }
    pthread_mutex_unlock(&g_wgl_mu);
    HLE_RET32(c, ok ? 1u : 0u);
}

#if defined(__ANDROID__)
static void probe_es3_driver_caps_once(void) {
    pthread_mutex_lock(&g_wgl_mu);
    if (g_egl_desktop_gl || g_es3_caps_probed) {
        pthread_mutex_unlock(&g_wgl_mu);
        return;
    }
    pthread_mutex_unlock(&g_wgl_mu);

    const char *exts = (glGetString != NULL) ? (const char *)glGetString((GLenum)SE_GL_EXTENSIONS) : NULL;
    bool s3tc = exts && (strstr(exts, "GL_EXT_texture_compression_s3tc") != NULL);
    bool rgtc = exts && (strstr(exts, "GL_EXT_texture_compression_rgtc") != NULL ||
                         strstr(exts, "GL_ARB_texture_compression_rgtc") != NULL);
    bool clip_ctrl = exts && (strstr(exts, "GL_EXT_clip_control") != NULL) && (g_native_glClipControl != NULL);
    bool buf_stor = exts && (strstr(exts, "GL_EXT_buffer_storage") != NULL) && (g_native_glBufferStorage != NULL);
    bool blend_ext = exts && (strstr(exts, "GL_EXT_blend_func_extended") != NULL);
    WGL_LOG("GLES3 driver caps: s3tc=%d rgtc=%d clip_ctrl=%d buf_stor=%d blend_ext=%d",
            s3tc ? 1 : 0, rgtc ? 1 : 0, clip_ctrl ? 1 : 0, buf_stor ? 1 : 0, blend_ext ? 1 : 0);
    se_wgl_set_es3_driver_caps(s3tc, rgtc, clip_ctrl, buf_stor, blend_ext);
}
#endif

static void hle_wgl_MakeCurrent(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    uint64_t hglrc = HLE_ARG1(c);

    if (!hglrc) {
#if defined(__ANDROID__)
        pthread_mutex_lock(&g_wgl_mu);
        if (g_egl_dpy != EGL_NO_DISPLAY) {
            eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        pthread_mutex_unlock(&g_wgl_mu);
#endif
        tl_cur_hglrc = 0ull;
        tl_cur_hdc = 0ull;
        HLE_RET32(c, 1u);
        return;
    }

    bool ok = false;
    uint64_t ctx_hdc = 0ull;
    pthread_mutex_lock(&g_wgl_mu);
    se_wgl_ctx_t *ctx = find_ctx_locked(hglrc);
    if (ctx) {
        if (hdc) ctx->hdc = hdc;
        ctx_hdc = ctx->hdc;
#if defined(__ANDROID__)
        if (materialize_egl_ctx_locked(ctx)) {
            EGLSurface surf = EGL_NO_SURFACE;
            if (ctx->is_main && ensure_egl_window_surface_locked()) {
                surf = g_egl_win_surf;
            } else {
                surf = ctx->pbuffer_surf;
            }
            if (eglMakeCurrent(g_egl_dpy, surf, surf, ctx->egl_ctx)) {
                ok = true;
            } else {
                WGL_LOG("eglMakeCurrent failed err=0x%x (is_main=%d surf=%p)",
                        eglGetError(), ctx->is_main ? 1 : 0, (void *)surf);
            }
        }
#else
        ok = true;
#endif
    }
    pthread_mutex_unlock(&g_wgl_mu);

    if (ok) {
        tl_cur_hglrc = hglrc;
        tl_cur_hdc = hdc ? hdc : ctx_hdc;
#if defined(__ANDROID__)
        probe_es3_driver_caps_once();
#endif
    }
    HLE_RET32(c, ok ? 1u : 0u);
}

static void hle_wgl_DeleteContext(CPU *c) {
    uint64_t hglrc = HLE_ARG0(c);
    pthread_mutex_lock(&g_wgl_mu);
    se_wgl_ctx_t *ctx = find_ctx_locked(hglrc);
    if (ctx) {
#if defined(__ANDROID__)
        if (g_egl_dpy != EGL_NO_DISPLAY) {
            if (tl_cur_hglrc == hglrc) {
                eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            if (ctx->egl_ctx != EGL_NO_CONTEXT) eglDestroyContext(g_egl_dpy, ctx->egl_ctx);
            if (ctx->pbuffer_surf != EGL_NO_SURFACE) eglDestroySurface(g_egl_dpy, ctx->pbuffer_surf);
        }
#endif
        ctx->used = false;
    }
    pthread_mutex_unlock(&g_wgl_mu);
    if (tl_cur_hglrc == hglrc) {
        tl_cur_hglrc = 0ull;
        tl_cur_hdc = 0ull;
    }
    HLE_RET32(c, 1u);
}

static void hle_wgl_GetCurrentDC(CPU *c) {
    (void)c;
    HLE_RET64(c, tl_cur_hdc);
}

static void hle_wgl_GetCurrentContext(CPU *c) {
    (void)c;
    HLE_RET64(c, tl_cur_hglrc);
}

static void hle_wgl_SwapLayerBuffers(CPU *c) {
    (void)HLE_ARG0(c);
    (void)HLE_ARG1(c);

#if defined(__ANDROID__)
    pthread_mutex_lock(&g_wgl_mu);
    if (ensure_egl_window_surface_locked()) {
        eglSwapBuffers(g_egl_dpy, g_egl_win_surf);
    }
    g_swap_count++;
    pthread_mutex_unlock(&g_wgl_mu);
#else
    pthread_mutex_lock(&g_wgl_mu);
    g_swap_count++;
    pthread_mutex_unlock(&g_wgl_mu);
#endif

    maybe_fire_planetarium_ready();
    HLE_RET32(c, 1u);
}

static void hle_wgl_SwapIntervalEXT(CPU *c) {
    int32_t interval = (int32_t)(uint32_t)HLE_ARG0(c);
#if defined(__ANDROID__)
    if (interval > 1) interval = 1;
    if (interval < 0) interval = 0;
#endif
    g_swap_interval = interval;
#if defined(__ANDROID__)
    pthread_mutex_lock(&g_wgl_mu);
    if (g_egl_dpy != EGL_NO_DISPLAY) {
        eglSwapInterval(g_egl_dpy, interval);
    }
    pthread_mutex_unlock(&g_wgl_mu);
#endif
    HLE_RET32(c, 1u);
}

static void hle_wgl_GetSwapIntervalEXT(CPU *c) {
    (void)c;
    HLE_RET32(c, (uint32_t)g_swap_interval);
}

static void hle_wgl_GetExtensionsStringARB(CPU *c) {
    (void)c;
    HLE_RET64(c, get_ext_string_guest());
}

static void hle_wgl_ChoosePixelFormatARB(CPU *c) {
    uint64_t piFormats = HLE_ARG4(c);
    uint64_t pNumFormats = HLE_ARG5(c);
    if (piFormats) wr32(piFormats, 1u);
    if (pNumFormats) wr32(pNumFormats, 1u);
    HLE_RET32(c, 1u);
}

static void hle_wgl_GetPixelFormatAttribivARB(CPU *c) {
    uint32_t nAttributes = (uint32_t)HLE_ARG3(c);
    uint64_t piAttributes = HLE_ARG4(c);
    uint64_t piValues = HLE_ARG5(c);
    if (piAttributes && piValues) {
        for (uint32_t i = 0; i < nAttributes && i < 64u; ++i) {
            uint32_t attr = rd32(piAttributes + (uint64_t)i * 4ull);
            uint32_t val = 1u;
            /* Common WGL_ARB_pixel_format attributes */
            switch (attr) {
            case 0x2014u: /* COLOR_BITS */ val = 32u; break;
            case 0x2015u: case 0x2017u: case 0x2019u: case 0x201Bu: val = 8u; break;
            case 0x2022u: /* DEPTH_BITS */ val = 24u; break;
            case 0x2023u: /* STENCIL_BITS */ val = 8u; break;
            case 0x2041u: /* SAMPLE_BUFFERS */ val = 0u; break;
            case 0x2042u: /* SAMPLES */ val = 0u; break;
            default: val = 1u; break;
            }
            wr32(piValues + (uint64_t)i * 4ull, val);
        }
    }
    HLE_RET32(c, 1u);
}

/* ========================================================================= */
/* 2. Static GL 1.1 Imports + Core Extension Implementations                 */
/* ========================================================================= */

static void hle_gl_noop(CPU *c) {
    (void)c;
}

static void hle_gl_ret1(CPU *c) {
    HLE_RET32(c, 1u);
}

static void hle_gl_ret0(CPU *c) {
    HLE_RET32(c, 0u);
}

static void hle_gl_GetError(CPU *c) {
#if defined(__ANDROID__)
    HLE_RET32(c, (uint32_t)glGetError());
#else
    (void)c;
    HLE_RET32(c, GL_NO_ERROR);
#endif
}

static void hle_gl_IsEnabled(CPU *c) {
#if defined(__ANDROID__)
    HLE_RET32(c, (uint32_t)glIsEnabled((GLenum)HLE_ARG0(c)));
#else
    typedef uint8_t (*fn_glIsEnabled_t)(uint32_t);
    fn_glIsEnabled_t fn = (fn_glIsEnabled_t)(uintptr_t)lookup_native_gl_proc("glIsEnabled");
    HLE_RET32(c, fn ? (uint32_t)fn((uint32_t)HLE_ARG0(c)) : 0u);
#endif
}

static void hle_gl_GetString(CPU *c) {
    uint32_t name = (uint32_t)HLE_ARG0(c);
#if defined(__ANDROID__)
    pthread_mutex_lock(&g_wgl_mu);
    bool desktop = g_egl_desktop_gl;
    pthread_mutex_unlock(&g_wgl_mu);
    if (desktop && tl_cur_hglrc && glGetString) {
        const GLubyte *s = glGetString((GLenum)name);
        if (s) {
            HLE_RET64(c, se_hle_intern_ascii((const char *)s));
            return;
        }
    }
#endif
    switch (name) {
    case SE_GL_VENDOR:
        HLE_RET64(c, se_hle_intern_ascii("SpaceEngine Android Port"));
        break;
    case SE_GL_RENDERER:
        HLE_RET64(c, se_hle_intern_ascii("Android EGL/Zink GLES3 Bridge"));
        break;
    case SE_GL_VERSION:
        HLE_RET64(c, se_hle_intern_ascii("4.6.0 SpaceEngine Android Bridge"));
        break;
    case SE_GL_SHADING_LANGUAGE_VERSION:
        HLE_RET64(c, se_hle_intern_ascii("4.60 SpaceEngine Android Bridge"));
        break;
    case SE_GL_EXTENSIONS:
        HLE_RET64(c, get_ext_string_guest());
        break;
    default:
        HLE_RET64(c, se_hle_intern_ascii(""));
        break;
    }
}

static void hle_gl_GetStringi(CPU *c) {
    uint32_t name = (uint32_t)HLE_ARG0(c);
    uint32_t idx = (uint32_t)HLE_ARG1(c);
    if (name == SE_GL_EXTENSIONS) {
#if defined(__ANDROID__)
        pthread_mutex_lock(&g_wgl_mu);
        bool desktop = g_egl_desktop_gl;
        pthread_mutex_unlock(&g_wgl_mu);
        if (desktop && g_native_glGetStringi) {
            const GLubyte *s = g_native_glGetStringi((GLenum)name, idx);
            if (s) {
                HLE_RET64(c, se_hle_intern_ascii((const char *)s));
                return;
            }
        }
#endif
        pthread_mutex_lock(&g_wgl_mu);
        rebuild_active_exts_locked();
        uint64_t r = 0ull;
        if (idx < g_active_ext_count) {
            if (!g_ext_item_guest[idx]) {
                g_ext_item_guest[idx] = se_hle_intern_ascii(g_active_exts[idx]);
            }
            r = g_ext_item_guest[idx];
        }
        pthread_mutex_unlock(&g_wgl_mu);
        HLE_RET64(c, r);
        return;
    }
    HLE_RET64(c, 0ull);
}

static void hle_gl_GetIntegerv(CPU *c) {
    uint32_t pname = (uint32_t)HLE_ARG0(c);
    uint64_t params = HLE_ARG1(c);
    if (!params) return;

    switch (pname) {
    case SE_GL_MAJOR_VERSION:               wr32(params, 4u); return;
    case SE_GL_MINOR_VERSION:               wr32(params, 6u); return;
    case SE_GL_NUM_EXTENSIONS:
#if defined(__ANDROID__)
        {
            pthread_mutex_lock(&g_wgl_mu);
            bool desktop = g_egl_desktop_gl;
            pthread_mutex_unlock(&g_wgl_mu);
            if (desktop && tl_cur_hglrc && glGetIntegerv) {
                GLint v = 0;
                glGetIntegerv((GLenum)pname, &v);
                if (v > 0) { wr32(params, (uint32_t)v); return; }
            }
        }
#endif
        pthread_mutex_lock(&g_wgl_mu);
        rebuild_active_exts_locked();
        uint32_t ext_cnt = g_active_ext_count;
        pthread_mutex_unlock(&g_wgl_mu);
        wr32(params, ext_cnt);
        return;
    case SE_GL_MAX_VIEWPORT_DIMS:
#if defined(__ANDROID__)
        if (tl_cur_hglrc && glGetIntegerv) {
            GLint dims[2] = { 0, 0 };
            glGetIntegerv((GLenum)pname, dims);
            if (dims[0] > 0 && dims[1] > 0) {
                wr32(params, (uint32_t)dims[0]);
                wr32(params + 4ull, (uint32_t)dims[1]);
                return;
            }
        }
#endif
        wr32(params, 8192u);
        wr32(params + 4ull, 8192u);
        return;
    case SE_GL_CONTEXT_PROFILE_MASK:        wr32(params, SE_GL_CONTEXT_CORE_PROFILE_BIT); return;
    case SE_GL_CONTEXT_FLAGS:               wr32(params, 0u); return;
    case SE_GL_VIEWPORT:
#if defined(__ANDROID__)
        if (tl_cur_hglrc) {
            GLint vp[4] = {0, 0, 0, 0};
            glGetIntegerv((GLenum)pname, vp);
            for (int i = 0; i < 4; ++i) wr32(params + (uint64_t)i * 4ull, (uint32_t)vp[i]);
            return;
        }
#endif
        for (int i = 0; i < 4; ++i) wr32(params + (uint64_t)i * 4ull, (uint32_t)g_viewport[i]);
        return;
    case SE_GL_DEPTH_WRITEMASK:             wr8(params, g_depth_mask ? 1u : 0u); return;
    case SE_GL_MAX_TEXTURE_SIZE:            wr32(params, 4096u); return;
    case SE_GL_MAX_3D_TEXTURE_SIZE:         wr32(params, 512u); return;
    case SE_GL_MAX_CUBE_MAP_TEXTURE_SIZE:   wr32(params, 4096u); return;
    case SE_GL_MAX_TEXTURE_IMAGE_UNITS:     wr32(params, 16u); return;
    case SE_GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: wr32(params, 16u); return;
    case SE_GL_MAX_COMBINED_TEX_UNITS:      wr32(params, 32u); return;
    case SE_GL_MAX_COLOR_ATTACHMENTS:       wr32(params, 8u); return;
    case SE_GL_MAX_DRAW_BUFFERS:            wr32(params, 8u); return;
    case SE_GL_MAX_SAMPLES:                 wr32(params, 4u); return;
    case SE_GL_MAX_UNIFORM_BLOCK_SIZE:      wr32(params, 65536u); return;
    case SE_GL_MAX_UNIFORM_BUFFER_BINDINGS: wr32(params, 36u); return;
    case SE_GL_MAX_SSBO_BLOCK_SIZE:         wr32(params, 134217728u); return;
    case SE_GL_MAX_COMPUTE_WORK_GROUP_INVOC: wr32(params, 128u); return;
    case SE_GL_MAX_VERTEX_ATTRIBS:          wr32(params, 16u); return;
    case SE_GL_MAX_VIEWPORTS:               wr32(params, 1u); return;
    case SE_GL_MAX_ARRAY_TEXTURE_LAYERS:    wr32(params, 256u); return;
    case SE_GL_MAX_TEXTURE_MAX_ANISOTROPY:  wr32(params, 16u); return;
    case 0x87FF: /* GL_PROGRAM_BINARY_FORMATS */
#if defined(__ANDROID__)
        if (tl_cur_hglrc) {
            GLint nfmt = 0;
            glGetIntegerv(0x87FE /* GL_NUM_PROGRAM_BINARY_FORMATS */, &nfmt);
            if (nfmt > 0) {
                if (nfmt > 16) nfmt = 16;
                GLint fmts[16];
                memset(fmts, 0, sizeof(fmts));
                glGetIntegerv((GLenum)pname, fmts);
                for (GLint i = 0; i < nfmt; ++i) {
                    wr32(params + (uint64_t)i * 4ull, (uint32_t)fmts[i]);
                }
            }
            return;
        }
#endif
        wr32(params, 0u);
        return;
    default:
#if defined(__ANDROID__)
        if (tl_cur_hglrc) {
            GLint v[4] = { 0, 0, 0, 0 };
            glGetIntegerv((GLenum)pname, v);
            wr32(params, (uint32_t)v[0]);
            return;
        }
#endif
        wr32(params, 0u);
        return;
    }
}

static void alloc_gl_ids(uint32_t n, uint64_t out_ptr) {
    if (!out_ptr || n == 0u || n > 65536u) return;
    pthread_mutex_lock(&g_wgl_mu);
    for (uint32_t i = 0; i < n; ++i) {
        wr32(out_ptr + (uint64_t)i * 4ull, g_next_id++);
    }
    pthread_mutex_unlock(&g_wgl_mu);
}

static void hle_gl_GenTextures(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t textures = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && textures && n > 0u) {
        glGenTextures((GLsizei)n, (GLuint *)(uintptr_t)textures);
        return;
    }
#endif
    alloc_gl_ids(n, textures);
}

static void hle_gl_DeleteTextures(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t textures = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && textures && n > 0u) {
        glDeleteTextures((GLsizei)n, (const GLuint *)(uintptr_t)textures);
    }
#else
    (void)n;
    (void)textures;
#endif
}

static void hle_gl_BindTexture(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glBindTexture((GLenum)se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c)), (GLuint)HLE_ARG1(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_Clear(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glClear((GLbitfield)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_ClearColor(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glClearColor(HLE_ARG_F32(c, 0), HLE_ARG_F32(c, 1),
                     HLE_ARG_F32(c, 2), HLE_ARG_F32(c, 3));
    }
#else
    (void)c;
#endif
}

static void hle_gl_ClearDepth(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        double d = HLE_ARG_F64(c, 0);
        if (g_native_glClearDepth) g_native_glClearDepth(d);
        else glClearDepthf((GLfloat)d);
    }
#else
    (void)c;
#endif
}

static void hle_gl_ClearStencil(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glClearStencil((GLint)(int32_t)(uint32_t)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_ColorMask(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glColorMask((GLboolean)(HLE_ARG0(c) != 0), (GLboolean)(HLE_ARG1(c) != 0),
                    (GLboolean)(HLE_ARG2(c) != 0), (GLboolean)(HLE_ARG3(c) != 0));
    }
#else
    (void)c;
#endif
}

static void hle_gl_CullFace(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glCullFace((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_FrontFace(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glFrontFace((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_BlendFunc(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBlendFunc((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_DepthFunc(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glDepthFunc((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DepthMask(CPU *c) {
    bool flag = (HLE_ARG0(c) & 0xFFu) != 0u;
    g_depth_mask = flag;
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glDepthMask(flag ? GL_TRUE : GL_FALSE);
#endif
}

static void hle_gl_DepthRange(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        double n = HLE_ARG_F64(c, 0);
        double f = HLE_ARG_F64(c, 1);
        if (g_native_glDepthRange) g_native_glDepthRange(n, f);
        else glDepthRangef((GLfloat)n, (GLfloat)f);
    }
#else
    (void)c;
#endif
}

static void hle_gl_Viewport(CPU *c) {
    int32_t x = (int32_t)(uint32_t)HLE_ARG0(c);
    int32_t y = (int32_t)(uint32_t)HLE_ARG1(c);
    int32_t w = (int32_t)(uint32_t)HLE_ARG2(c);
    int32_t h = (int32_t)(uint32_t)HLE_ARG3(c);
    g_viewport[0] = x;
    g_viewport[1] = y;
    g_viewport[2] = w;
    g_viewport[3] = h;
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glViewport(x, y, w, h);
#endif
}

static void hle_gl_Enable(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glEnable((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_Disable(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glDisable((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_Finish(CPU *c) {
    (void)c;
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glFinish();
#endif
}

static void hle_gl_Flush(CPU *c) {
    (void)c;
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glFlush();
#endif
}

static void hle_gl_Hint(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glHint((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_LineWidth(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glLineWidth(HLE_ARG_F32(c, 0));
#else
    (void)c;
#endif
}

static void hle_gl_PointSize(CPU *c) {
    (void)c;
}

static void hle_gl_PolygonOffset(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glPolygonOffset(HLE_ARG_F32(c, 0), HLE_ARG_F32(c, 1));
#else
    (void)c;
#endif
}

static void hle_gl_PolygonMode(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glPolygonMode) {
        g_native_glPolygonMode((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_PixelStorei(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glPixelStorei((GLenum)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_TexParameteri(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t target = se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        int32_t param = se_wgl_normalize_es3_tex_param_i(pname, (int32_t)(uint32_t)HLE_ARG2(c));
        glTexParameteri((GLenum)target, (GLenum)pname, (GLint)param);
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexParameteriv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t target = se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        const GLint *params = (const GLint *)(uintptr_t)HLE_ARG2(c);
        if (params && !se_wgl_is_desktop_gl() &&
            (pname == SE_GL_TEXTURE_WRAP_S || pname == SE_GL_TEXTURE_WRAP_T || pname == SE_GL_TEXTURE_WRAP_R)) {
            GLint norm = (GLint)se_wgl_normalize_es3_tex_param_i(pname, (int32_t)params[0]);
            glTexParameteriv((GLenum)target, (GLenum)pname, &norm);
        } else {
            glTexParameteriv((GLenum)target, (GLenum)pname, params);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexParameterf(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t target = se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        float param = se_wgl_normalize_es3_tex_param_f(pname, HLE_ARG_F32(c, 2));
        glTexParameterf((GLenum)target, (GLenum)pname, param);
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexParameterfv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t target = se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        const GLfloat *params = (const GLfloat *)(uintptr_t)HLE_ARG2(c);
        if (!params) return;
        if (!se_wgl_is_desktop_gl() &&
            (pname == SE_GL_TEXTURE_WRAP_S || pname == SE_GL_TEXTURE_WRAP_T || pname == SE_GL_TEXTURE_WRAP_R)) {
            GLfloat norm = se_wgl_normalize_es3_tex_param_f(pname, params[0]);
            if (g_native_glTexParameterfv) {
                g_native_glTexParameterfv((GLenum)target, (GLenum)pname, &norm);
            } else {
                glTexParameterf((GLenum)target, (GLenum)pname, norm);
            }
        } else if (g_native_glTexParameterfv) {
            g_native_glTexParameterfv((GLenum)target, (GLenum)pname, params);
        } else {
            glTexParameterf((GLenum)target, (GLenum)pname, params[0]);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexImage1D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLint internalformat = (GLint)(int32_t)(uint32_t)HLE_ARG2(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLint border = (GLint)(int32_t)(uint32_t)HLE_ARG4(c);
        GLenum format = (GLenum)HLE_ARG5(c);
        GLenum type = (GLenum)HLE_ARG6(c);
        const void *pixels = (const void *)(uintptr_t)HLE_ARG7(c);
        if (se_wgl_is_desktop_gl() && g_native_glTexImage1D) {
            g_native_glTexImage1D(target, level, internalformat, width, border, format, type, pixels);
        } else if (target == SE_GL_TEXTURE_1D) {
            android_tex_image_2d(GL_TEXTURE_2D, level, internalformat, width, 1, border, format, type, pixels);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexSubImage1D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLint xoff = (GLint)(int32_t)(uint32_t)HLE_ARG2(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLenum format = (GLenum)HLE_ARG4(c);
        GLenum type = (GLenum)HLE_ARG5(c);
        const void *pixels = (const void *)(uintptr_t)HLE_ARG6(c);
        if (se_wgl_is_desktop_gl() && g_native_glTexSubImage1D) {
            g_native_glTexSubImage1D(target, level, xoff, width, format, type, pixels);
        } else if (target == SE_GL_TEXTURE_1D) {
            android_tex_subimage_2d(GL_TEXTURE_2D, level, xoff, 0, width, 1, format, type, pixels);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexImage2D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        android_tex_image_2d((GLenum)HLE_ARG0(c),
                             (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                             (GLint)(int32_t)(uint32_t)HLE_ARG2(c),
                             (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c),
                             (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c),
                             (GLint)(int32_t)(uint32_t)HLE_ARG5(c),
                             (GLenum)HLE_ARG6(c),
                             (GLenum)HLE_ARG7(c),
                             (const void *)(uintptr_t)hle_stack_arg(c, 8u));
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexSubImage2D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        android_tex_subimage_2d((GLenum)HLE_ARG0(c),
                                (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                                (GLint)(int32_t)(uint32_t)HLE_ARG2(c),
                                (GLint)(int32_t)(uint32_t)HLE_ARG3(c),
                                (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c),
                                (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c),
                                (GLenum)HLE_ARG6(c),
                                (GLenum)HLE_ARG7(c),
                                (const void *)(uintptr_t)hle_stack_arg(c, 8u));
    }
#else
    (void)c;
#endif
}

static void hle_gl_GetTexImage(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glGetTexImage) {
        g_native_glGetTexImage((GLenum)HLE_ARG0(c),
                               (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                               (GLenum)HLE_ARG2(c),
                               (GLenum)HLE_ARG3(c),
                               (void *)(uintptr_t)HLE_ARG4(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_GetTexLevelParameteriv(CPU *c) {
    uint32_t target = se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
    uint32_t pname = (uint32_t)HLE_ARG2(c);
    uint64_t params = HLE_ARG3(c);
    if (!params) return;
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glGetTexLevelParameteriv) {
        g_native_glGetTexLevelParameteriv((GLenum)target,
                                          (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                                          (GLenum)pname,
                                          (GLint *)(uintptr_t)params);
        return;
    }
#else
    (void)target;
#endif
    if (pname == SE_GL_TEXTURE_WIDTH || pname == SE_GL_TEXTURE_HEIGHT) wr32(params, 256u);
    else if (pname == SE_GL_TEXTURE_INTERNAL_FORMAT) wr32(params, SE_GL_RGBA8);
    else wr32(params, 0u);
}

static void hle_gl_ReadBuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glReadBuffer((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DrawBuffer(CPU *c) {
    GLenum mode = (GLenum)HLE_ARG0(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        if (g_native_glDrawBuffer) {
            g_native_glDrawBuffer(mode);
        } else {
            GLint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            if (fbo == 0 && mode == SE_GL_FRONT) mode = SE_GL_BACK;
            glDrawBuffers(1, &mode);
        }
    }
#else
    (void)mode;
#endif
}

static void hle_gl_ReadPixels(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glReadPixels((GLint)(int32_t)(uint32_t)HLE_ARG0(c),
                     (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                     (GLsizei)(int32_t)(uint32_t)HLE_ARG2(c),
                     (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c),
                     (GLenum)HLE_ARG4(c),
                     (GLenum)HLE_ARG5(c),
                     (void *)(uintptr_t)HLE_ARG6(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_DrawArrays(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glDrawArrays((GLenum)HLE_ARG0(c),
                     (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                     (GLsizei)(int32_t)(uint32_t)HLE_ARG2(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_DrawElements(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glDrawElements((GLenum)HLE_ARG0(c),
                       (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                       (GLenum)HLE_ARG2(c),
                       (const void *)(uintptr_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

/* ========================================================================= */
/* 3. Typed OpenGL 2.0-4.6 Extension Shims Loaded via wglGetProcAddress      */
/* ========================================================================= */

static void hle_gl_ActiveTexture(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glActiveTexture((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_CreateShader(CPU *c) {
    uint32_t stage = (uint32_t)HLE_ARG0(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t id = (uint32_t)glCreateShader((GLenum)stage);
        track_shader_stage(id, stage);
        HLE_RET32(c, id);
        return;
    }
#endif
    pthread_mutex_lock(&g_wgl_mu);
    uint32_t id = g_next_id++;
    pthread_mutex_unlock(&g_wgl_mu);
    track_shader_stage(id, stage);
    HLE_RET32(c, id);
}

static void hle_gl_ShaderSource(CPU *c) {
    uint32_t shader = (uint32_t)HLE_ARG0(c);
    int32_t count = (int32_t)(uint32_t)HLE_ARG1(c);
    const char *const *strings = (const char *const *)(uintptr_t)HLE_ARG2(c);
    const int32_t *lengths = (const int32_t *)(uintptr_t)HLE_ARG3(c);

    if (!se_wgl_is_desktop_gl() && count > 0 && strings) {
        size_t total_len = 0u;
        for (int32_t i = 0; i < count; ++i) {
            const char *s = strings[i];
            if (!s) continue;
            size_t slen = (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(s);
            if (slen > ((size_t)-1) - total_len - 1u) return;
            total_len += slen;
        }
        char *joined = (char *)malloc(total_len + 1u);
        if (!joined) return;
        size_t pos = 0u;
        for (int32_t i = 0; i < count; ++i) {
            const char *s = strings[i];
            if (!s) continue;
            size_t slen = (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(s);
            if (slen > 0u) {
                memcpy(joined + pos, s, slen);
                pos += slen;
            }
        }
        joined[pos] = '\0';

        uint32_t stage = lookup_shader_stage(shader);
        char *translated = se_shader_translate_to_es3(joined, stage);
        const char *src_to_use = translated ? translated : joined;

        void (*cap_cb)(uint32_t, uint32_t, const char *, void *) = NULL;
        void *cap_user = NULL;
        pthread_mutex_lock(&g_wgl_mu);
        cap_cb = g_shader_capture_cb;
        cap_user = g_shader_capture_user;
        pthread_mutex_unlock(&g_wgl_mu);
        if (cap_cb) {
            cap_cb(shader, stage, src_to_use, cap_user);
        }

#if defined(__ANDROID__)
        if (tl_cur_hglrc) {
            const GLchar *arr[1] = { (const GLchar *)src_to_use };
            GLint len = (GLint)strlen(src_to_use);
            glShaderSource((GLuint)shader, 1, (void *)arr, &len);
        }
#endif
        se_shader_free_translated(translated);
        free(joined);
        return;
    }

#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glShaderSource((GLuint)shader,
                       (GLsizei)count,
                       (void *)(uintptr_t)strings,
                       (const GLint *)lengths);
    }
#endif
}

static void hle_gl_CompileShader(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glCompileShader((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DeleteShader(CPU *c) {
    uint32_t shader = (uint32_t)HLE_ARG0(c);
    untrack_shader_stage(shader);
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glDeleteShader((GLuint)shader);
#endif
}

static void hle_gl_GetShaderiv(CPU *c) {
    uint64_t params = HLE_ARG2(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && params) {
        glGetShaderiv((GLuint)HLE_ARG0(c), (GLenum)HLE_ARG1(c), (GLint *)(uintptr_t)params);
        return;
    }
#endif
    if (params) wr32(params, 1u);
}

static void hle_gl_GetShaderInfoLog(CPU *c) {
    uint64_t lenp = HLE_ARG2(c);
    uint64_t logp = HLE_ARG3(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && logp) {
        glGetShaderInfoLog((GLuint)HLE_ARG0(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                           (GLsizei *)(uintptr_t)lenp, (GLchar *)(uintptr_t)logp);
        return;
    }
#endif
    if (lenp) wr32(lenp, 0u);
    if (logp && HLE_ARG1(c) > 0) wr8(logp, 0u);
}

static void hle_gl_CreateProgram(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) { HLE_RET32(c, glCreateProgram()); return; }
#endif
    pthread_mutex_lock(&g_wgl_mu);
    uint32_t id = g_next_id++;
    pthread_mutex_unlock(&g_wgl_mu);
    HLE_RET32(c, id);
}

static void hle_gl_AttachShader(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glAttachShader((GLuint)HLE_ARG0(c), (GLuint)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_LinkProgram(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glLinkProgram((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_UseProgram(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glUseProgram((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DeleteProgram(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glDeleteProgram((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_GetProgramiv(CPU *c) {
    uint64_t params = HLE_ARG2(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && params) {
        glGetProgramiv((GLuint)HLE_ARG0(c), (GLenum)HLE_ARG1(c), (GLint *)(uintptr_t)params);
        return;
    }
#endif
    if (params) wr32(params, 1u);
}

static void hle_gl_GetProgramInfoLog(CPU *c) {
    uint64_t lenp = HLE_ARG2(c);
    uint64_t logp = HLE_ARG3(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && logp) {
        glGetProgramInfoLog((GLuint)HLE_ARG0(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                            (GLsizei *)(uintptr_t)lenp, (GLchar *)(uintptr_t)logp);
        return;
    }
#endif
    if (lenp) wr32(lenp, 0u);
    if (logp && HLE_ARG1(c) > 0) wr8(logp, 0u);
}

static void hle_gl_GetUniformLocation(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        HLE_RET32(c, (uint32_t)glGetUniformLocation((GLuint)HLE_ARG0(c),
                                                    (const GLchar *)(uintptr_t)HLE_ARG1(c)));
        return;
    }
#endif
    HLE_RET32(c, 0u);
}

static void hle_gl_GetAttribLocation(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        HLE_RET32(c, (uint32_t)glGetAttribLocation((GLuint)HLE_ARG0(c),
                                                   (const GLchar *)(uintptr_t)HLE_ARG1(c)));
        return;
    }
#endif
    HLE_RET32(c, 0u);
}

static void hle_gl_Uniform1i(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glUniform1i((GLint)(int32_t)(uint32_t)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_Uniform1f(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glUniform1f((GLint)(int32_t)(uint32_t)HLE_ARG0(c), HLE_ARG_F32(c, 1));
#else
    (void)c;
#endif
}

static void hle_gl_Uniform2f(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glUniform2f((GLint)(int32_t)(uint32_t)HLE_ARG0(c), HLE_ARG_F32(c, 1), HLE_ARG_F32(c, 2));
#else
    (void)c;
#endif
}

static void hle_gl_Uniform3f(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glUniform3f((GLint)(int32_t)(uint32_t)HLE_ARG0(c), HLE_ARG_F32(c, 1), HLE_ARG_F32(c, 2), HLE_ARG_F32(c, 3));
#else
    (void)c;
#endif
}

static void hle_gl_Uniform4f(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        union { uint32_t u; float f; } w4;
        w4.u = (uint32_t)HLE_ARG4(c);
        glUniform4f((GLint)(int32_t)(uint32_t)HLE_ARG0(c), HLE_ARG_F32(c, 1), HLE_ARG_F32(c, 2), HLE_ARG_F32(c, 3), w4.f);
    }
#else
    (void)c;
#endif
}

static void hle_gl_UniformMatrix4fv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glUniformMatrix4fv((GLint)(int32_t)(uint32_t)HLE_ARG0(c),
                           (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                           (GLboolean)(HLE_ARG2(c) != 0),
                           (const GLfloat *)(uintptr_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_GenBuffers(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t bufs = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && bufs && n > 0u) { glGenBuffers((GLsizei)n, (GLuint *)(uintptr_t)bufs); return; }
#endif
    alloc_gl_ids(n, bufs);
}

static void hle_gl_BindBuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBindBuffer((GLenum)HLE_ARG0(c), (GLuint)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_BufferData(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glBufferData((GLenum)HLE_ARG0(c), (GLsizeiptr)(int64_t)HLE_ARG1(c),
                     (const void *)(uintptr_t)HLE_ARG2(c), (GLenum)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_BufferSubData(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glBufferSubData((GLenum)HLE_ARG0(c), (GLintptr)(int64_t)HLE_ARG1(c),
                        (GLsizeiptr)(int64_t)HLE_ARG2(c), (const void *)(uintptr_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_DeleteBuffers(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG1(c)) glDeleteBuffers((GLsizei)(int32_t)(uint32_t)HLE_ARG0(c), (const GLuint *)(uintptr_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_GenVertexArrays(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t arr = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && arr && n > 0u) { glGenVertexArrays((GLsizei)n, (GLuint *)(uintptr_t)arr); return; }
#endif
    alloc_gl_ids(n, arr);
}

static void hle_gl_BindVertexArray(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBindVertexArray((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DeleteVertexArrays(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG1(c)) glDeleteVertexArrays((GLsizei)(int32_t)(uint32_t)HLE_ARG0(c), (const GLuint *)(uintptr_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_EnableVertexAttribArray(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glEnableVertexAttribArray((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DisableVertexAttribArray(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glDisableVertexAttribArray((GLuint)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_VertexAttribPointer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glVertexAttribPointer((GLuint)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                              (GLenum)HLE_ARG2(c), (GLboolean)(HLE_ARG3(c) != 0),
                              (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c),
                              (const void *)(uintptr_t)HLE_ARG5(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_GenFramebuffers(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t fbs = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && fbs && n > 0u) { glGenFramebuffers((GLsizei)n, (GLuint *)(uintptr_t)fbs); return; }
#endif
    alloc_gl_ids(n, fbs);
}

static void hle_gl_BindFramebuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBindFramebuffer((GLenum)HLE_ARG0(c), (GLuint)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_FramebufferTexture2D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum textarget = (GLenum)se_wgl_normalize_es3_target((uint32_t)HLE_ARG2(c));
        glFramebufferTexture2D((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c), textarget,
                               (GLuint)HLE_ARG3(c), (GLint)(int32_t)(uint32_t)HLE_ARG4(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_CheckFramebufferStatus(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) { HLE_RET32(c, (uint32_t)glCheckFramebufferStatus((GLenum)HLE_ARG0(c))); return; }
#endif
    HLE_RET32(c, SE_GL_FRAMEBUFFER_COMPLETE);
}

static void hle_gl_DeleteFramebuffers(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG1(c)) glDeleteFramebuffers((GLsizei)(int32_t)(uint32_t)HLE_ARG0(c), (const GLuint *)(uintptr_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_GenRenderbuffers(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t rbs = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && rbs && n > 0u) { glGenRenderbuffers((GLsizei)n, (GLuint *)(uintptr_t)rbs); return; }
#endif
    alloc_gl_ids(n, rbs);
}

static void hle_gl_BindRenderbuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBindRenderbuffer((GLenum)HLE_ARG0(c), (GLuint)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_RenderbufferStorage(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum ifmt = (GLenum)se_wgl_normalize_es3_internalformat(
            (int32_t)(uint32_t)HLE_ARG1(c), 0u, 0u, true);
        glRenderbufferStorage((GLenum)HLE_ARG0(c), ifmt,
                              (GLsizei)(int32_t)(uint32_t)HLE_ARG2(c),
                              (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_FramebufferRenderbuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glFramebufferRenderbuffer((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c),
                                  (GLenum)HLE_ARG2(c), (GLuint)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_DeleteRenderbuffers(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG1(c)) glDeleteRenderbuffers((GLsizei)(int32_t)(uint32_t)HLE_ARG0(c), (const GLuint *)(uintptr_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_DrawBuffers(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG1(c)) glDrawBuffers((GLsizei)(int32_t)(uint32_t)HLE_ARG0(c), (const GLenum *)(uintptr_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_TexImage3D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLint internalformat = (GLint)(int32_t)(uint32_t)HLE_ARG2(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLsizei height = (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c);
        GLsizei depth = (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c);
        GLint border = se_wgl_is_desktop_gl() ? (GLint)(int32_t)(uint32_t)HLE_ARG6(c) : 0;
        GLenum format = (GLenum)HLE_ARG7(c);
        GLenum type = (GLenum)hle_stack_arg(c, 8u);
        const void *pixels = (const void *)(uintptr_t)hle_stack_arg(c, 9u);
        GLint nif = (GLint)se_wgl_normalize_es3_internalformat(internalformat, (uint32_t)format, (uint32_t)type, false);
        uint32_t nfmt = (uint32_t)format, ntype = (uint32_t)type;
        uint8_t *swz = se_wgl_alloc_es3_uncompressed_pixels((uint32_t)format, (uint32_t)type,
                                                            width, height, depth, pixels,
                                                            &nfmt, &ntype);
        if (swz) {
            GLint saved = unpack_align_1_push();
            glTexImage3D(target, level, nif, width, height, depth, border, (GLenum)nfmt, (GLenum)ntype, swz);
            unpack_align_1_pop(saved);
            free(swz);
        } else {
            glTexImage3D(target, level, nif, width, height, depth, border, (GLenum)nfmt, (GLenum)ntype, pixels);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexSubImage3D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLint xoff = (GLint)(int32_t)(uint32_t)HLE_ARG2(c);
        GLint yoff = (GLint)(int32_t)(uint32_t)HLE_ARG3(c);
        GLint zoff = (GLint)(int32_t)(uint32_t)HLE_ARG4(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c);
        GLsizei height = (GLsizei)(int32_t)(uint32_t)HLE_ARG6(c);
        GLsizei depth = (GLsizei)(int32_t)(uint32_t)HLE_ARG7(c);
        GLenum format = (GLenum)hle_stack_arg(c, 8u);
        GLenum type = (GLenum)hle_stack_arg(c, 9u);
        const void *pixels = (const void *)(uintptr_t)hle_stack_arg(c, 10u);
        uint32_t nfmt = (uint32_t)format, ntype = (uint32_t)type;
        uint8_t *swz = se_wgl_alloc_es3_uncompressed_pixels((uint32_t)format, (uint32_t)type,
                                                            width, height, depth, pixels,
                                                            &nfmt, &ntype);
        if (swz) {
            GLint saved = unpack_align_1_push();
            glTexSubImage3D(target, level, xoff, yoff, zoff, width, height, depth, (GLenum)nfmt, (GLenum)ntype, swz);
            unpack_align_1_pop(saved);
            free(swz);
        } else {
            glTexSubImage3D(target, level, xoff, yoff, zoff, width, height, depth, (GLenum)nfmt, (GLenum)ntype, pixels);
        }
    }
#else
    (void)c;
#endif
}

#if defined(__ANDROID__)
static void compressed_fallback_uncompressed_fmt(uint32_t ifmt, GLint *out_nif,
                                                 GLenum *out_fmt, GLenum *out_tp) {
    GLint nif = (GLint)se_wgl_normalize_es3_internalformat((int32_t)ifmt, SE_GL_RGBA,
                                                           SE_GL_UNSIGNED_BYTE, false);
    *out_nif = nif;
    *out_fmt = (nif == (GLint)SE_GL_R8 || nif == (GLint)SE_GL_R8_SNORM) ? SE_GL_RED
             : ((nif == (GLint)SE_GL_RG8 || nif == (GLint)SE_GL_RG8_SNORM) ? SE_GL_RG : SE_GL_RGBA);
    *out_tp = (nif == (GLint)SE_GL_R8_SNORM || nif == (GLint)SE_GL_RG8_SNORM)
            ? SE_GL_BYTE : SE_GL_UNSIGNED_BYTE;
}

static void android_compressed_tex_image_2d(GLenum raw_target, GLint level, GLenum ifmt,
                                            GLsizei width, GLsizei height, GLint border,
                                            GLsizei image_size, const void *data) {
    GLenum target = (GLenum)se_wgl_normalize_es3_target((uint32_t)raw_target);
    GLint nborder = se_wgl_is_desktop_gl() ? border : 0;
    if (se_wgl_compressed_format_needs_cpu_decode((uint32_t)ifmt)) {
        uint32_t dec_ifmt = 0u, dec_fmt = 0u, dec_tp = 0u;
        uint8_t *dec = se_wgl_decode_bc_to_uncompressed((uint32_t)ifmt, width, height, image_size,
                                                         data, &dec_ifmt, &dec_fmt, &dec_tp);
        if (dec) {
            GLint saved = unpack_align_1_push();
            glTexImage2D(target, level, (GLint)dec_ifmt, width, height, 0,
                         (GLenum)dec_fmt, (GLenum)dec_tp, dec);
            unpack_align_1_pop(saved);
            free(dec);
        } else {
            GLint nif = 0;
            GLenum fmt = 0, tp = 0;
            compressed_fallback_uncompressed_fmt((uint32_t)ifmt, &nif, &fmt, &tp);
            glTexImage2D(target, level, nif, width, height, 0, fmt, tp, NULL);
        }
        return;
    }
    glCompressedTexImage2D(target, level, ifmt, width, height, nborder, image_size, data);
}

static void android_compressed_tex_subimage_2d(GLenum raw_target, GLint level, GLint xoff, GLint yoff,
                                               GLsizei width, GLsizei height, GLenum fmt,
                                               GLsizei image_size, const void *data) {
    GLenum target = (GLenum)se_wgl_normalize_es3_target((uint32_t)raw_target);
    if (se_wgl_compressed_format_needs_cpu_decode((uint32_t)fmt)) {
        uint32_t dec_ifmt = 0u, dec_fmt = 0u, dec_tp = 0u;
        uint8_t *dec = se_wgl_decode_bc_to_uncompressed((uint32_t)fmt, width, height, image_size,
                                                         data, &dec_ifmt, &dec_fmt, &dec_tp);
        if (dec) {
            GLint saved = unpack_align_1_push();
            glTexSubImage2D(target, level, xoff, yoff, width, height,
                            (GLenum)dec_fmt, (GLenum)dec_tp, dec);
            unpack_align_1_pop(saved);
            free(dec);
        }
        return;
    }
    glCompressedTexSubImage2D(target, level, xoff, yoff, width, height, fmt, image_size, data);
}

static void android_compressed_tex_3d_slices(bool sub_only, GLenum target, GLint level,
                                             GLint xoff, GLint yoff, GLint zoff,
                                             GLsizei width, GLsizei height, GLsizei depth,
                                             uint32_t ifmt, GLsizei image_size, const void *data) {
    if (depth <= 0) return;
    if (!data || image_size < depth) {
        if (!sub_only) {
            GLint nif = 0;
            GLenum fmt = 0, tp = 0;
            compressed_fallback_uncompressed_fmt(ifmt, &nif, &fmt, &tp);
            glTexImage3D(target, level, nif, width, height, depth, 0, fmt, tp, NULL);
        }
        return;
    }
    GLsizei slice_size = image_size / depth;
    bool alloc_done = sub_only;
    for (GLsizei z = 0; z < depth; ++z) {
        const uint8_t *sptr = (const uint8_t *)data + (size_t)z * (size_t)slice_size;
        uint32_t dec_ifmt = 0u, dec_fmt = 0u, dec_tp = 0u;
        uint8_t *dec = se_wgl_decode_bc_to_uncompressed(ifmt, width, height, slice_size,
                                                         sptr, &dec_ifmt, &dec_fmt, &dec_tp);
        if (!dec) {
            if (!alloc_done) {
                GLint nif = 0;
                GLenum fmt = 0, tp = 0;
                compressed_fallback_uncompressed_fmt(ifmt, &nif, &fmt, &tp);
                glTexImage3D(target, level, nif, width, height, depth, 0, fmt, tp, NULL);
            }
            break;
        }
        if (!alloc_done) {
            glTexImage3D(target, level, (GLint)dec_ifmt, width, height, depth, 0,
                         (GLenum)dec_fmt, (GLenum)dec_tp, NULL);
            alloc_done = true;
        }
        GLint saved = unpack_align_1_push();
        glTexSubImage3D(target, level, xoff, yoff, zoff + z, width, height, 1,
                        (GLenum)dec_fmt, (GLenum)dec_tp, dec);
        unpack_align_1_pop(saved);
        free(dec);
    }
}
#endif

static void hle_gl_CompressedTexImage1D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLenum ifmt = (GLenum)HLE_ARG2(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLint border = (GLint)(int32_t)(uint32_t)HLE_ARG4(c);
        GLsizei image_size = (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c);
        const void *data = (const void *)(uintptr_t)HLE_ARG6(c);
        typedef void (*pfn_t)(GLenum, GLint, GLenum, GLsizei, GLint, GLsizei, const void *);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_t fn = se_wgl_is_desktop_gl()
            ? (pfn_t)resolve_opt_gl_proc(&pfn, &state, "glCompressedTexImage1D", NULL, NULL, NULL)
            : NULL;
        if (fn) {
            fn(target, level, ifmt, width, border, image_size, data);
        } else if (target == SE_GL_TEXTURE_1D) {
            android_compressed_tex_image_2d(GL_TEXTURE_2D, level, ifmt, width, 1,
                                            border, image_size, data);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_CompressedTexImage2D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        android_compressed_tex_image_2d((GLenum)HLE_ARG0(c),
                                        (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                                        (GLenum)HLE_ARG2(c),
                                        (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c),
                                        (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c),
                                        (GLint)(int32_t)(uint32_t)HLE_ARG5(c),
                                        (GLsizei)(int32_t)(uint32_t)HLE_ARG6(c),
                                        (const void *)(uintptr_t)HLE_ARG7(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_CompressedTexImage3D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLenum ifmt = (GLenum)HLE_ARG2(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLsizei height = (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c);
        GLsizei depth = (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c);
        GLint border = se_wgl_is_desktop_gl() ? (GLint)(int32_t)(uint32_t)HLE_ARG6(c) : 0;
        GLsizei image_size = (GLsizei)(int32_t)(uint32_t)HLE_ARG7(c);
        const void *data = (const void *)(uintptr_t)hle_stack_arg(c, 8u);
        if (se_wgl_compressed_format_needs_cpu_decode((uint32_t)ifmt)) {
            android_compressed_tex_3d_slices(false, target, level, 0, 0, 0,
                                             width, height, depth, (uint32_t)ifmt, image_size, data);
            return;
        }
        typedef void (*pfn_c3d_t)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, const void *);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_c3d_t fn = (pfn_c3d_t)resolve_opt_gl_proc(&pfn, &state,
                                                       "glCompressedTexImage3D",
                                                       "glCompressedTexImage3D",
                                                       NULL, NULL);
        if (fn) fn(target, level, ifmt, width, height, depth, border, image_size, data);
    }
#else
    (void)c;
#endif
}

static void hle_gl_CompressedTexSubImage1D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLint xoff = (GLint)(int32_t)(uint32_t)HLE_ARG2(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLenum fmt = (GLenum)HLE_ARG4(c);
        GLsizei image_size = (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c);
        const void *data = (const void *)(uintptr_t)HLE_ARG6(c);
        typedef void (*pfn_t)(GLenum, GLint, GLint, GLsizei, GLenum, GLsizei, const void *);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_t fn = se_wgl_is_desktop_gl()
            ? (pfn_t)resolve_opt_gl_proc(&pfn, &state, "glCompressedTexSubImage1D", NULL, NULL, NULL)
            : NULL;
        if (fn) {
            fn(target, level, xoff, width, fmt, image_size, data);
        } else if (target == SE_GL_TEXTURE_1D) {
            android_compressed_tex_subimage_2d(GL_TEXTURE_2D, level, xoff, 0, width, 1,
                                               fmt, image_size, data);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_CompressedTexSubImage2D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        android_compressed_tex_subimage_2d((GLenum)HLE_ARG0(c),
                                           (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                                           (GLint)(int32_t)(uint32_t)HLE_ARG2(c),
                                           (GLint)(int32_t)(uint32_t)HLE_ARG3(c),
                                           (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c),
                                           (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c),
                                           (GLenum)HLE_ARG6(c),
                                           (GLsizei)(int32_t)(uint32_t)HLE_ARG7(c),
                                           (const void *)(uintptr_t)hle_stack_arg(c, 8u));
    }
#else
    (void)c;
#endif
}

static void hle_gl_CompressedTexSubImage3D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLint xoff = (GLint)(int32_t)(uint32_t)HLE_ARG2(c);
        GLint yoff = (GLint)(int32_t)(uint32_t)HLE_ARG3(c);
        GLint zoff = (GLint)(int32_t)(uint32_t)HLE_ARG4(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c);
        GLsizei height = (GLsizei)(int32_t)(uint32_t)HLE_ARG6(c);
        GLsizei depth = (GLsizei)(int32_t)(uint32_t)HLE_ARG7(c);
        GLenum fmt = (GLenum)hle_stack_arg(c, 8u);
        GLsizei image_size = (GLsizei)(int32_t)(uint32_t)hle_stack_arg(c, 9u);
        const void *data = (const void *)(uintptr_t)hle_stack_arg(c, 10u);
        if (se_wgl_compressed_format_needs_cpu_decode((uint32_t)fmt)) {
            android_compressed_tex_3d_slices(true, target, level, xoff, yoff, zoff,
                                             width, height, depth, (uint32_t)fmt, image_size, data);
            return;
        }
        typedef void (*pfn_cs3d_t)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                   GLenum, GLsizei, const void *);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_cs3d_t fn = (pfn_cs3d_t)resolve_opt_gl_proc(&pfn, &state,
                                                         "glCompressedTexSubImage3D",
                                                         "glCompressedTexSubImage3D",
                                                         NULL, NULL);
        if (fn) {
            fn(target, level, xoff, yoff, zoff, width, height, depth, fmt, image_size, data);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_ClipControl(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && (se_wgl_is_desktop_gl() || g_es3_has_clip_control) && g_native_glClipControl) {
        g_native_glClipControl((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_BufferStorage(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        if ((se_wgl_is_desktop_gl() || g_es3_has_buffer_storage) && g_native_glBufferStorage) {
            g_native_glBufferStorage((GLenum)HLE_ARG0(c), (ptrdiff_t)(int64_t)HLE_ARG1(c),
                                     (const void *)(uintptr_t)HLE_ARG2(c), (GLbitfield)HLE_ARG3(c));
        } else {
            glBufferData((GLenum)HLE_ARG0(c), (GLsizeiptr)(int64_t)HLE_ARG1(c),
                         (const void *)(uintptr_t)HLE_ARG2(c), GL_DYNAMIC_DRAW);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_CreateTextures(CPU *c) {
    uint32_t target = se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
    uint32_t n = (uint32_t)HLE_ARG1(c);
    uint64_t textures = HLE_ARG2(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && se_wgl_is_desktop_gl() && g_native_glCreateTextures && textures) {
        g_native_glCreateTextures((GLenum)target, (GLsizei)n, (GLuint *)(uintptr_t)textures);
        return;
    }
#else
    (void)target;
#endif
    alloc_gl_ids(n, textures);
}

static void hle_gl_TexStorage2D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c));
        GLsizei levels = (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c);
        GLenum ifmt = (GLenum)se_wgl_normalize_es3_internalformat((int32_t)(uint32_t)HLE_ARG2(c), 0u, 0u, true);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLsizei height = (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c);
        if (se_wgl_is_desktop_gl() && g_native_glTexStorage2D) {
            g_native_glTexStorage2D(target, levels, ifmt, width, height);
        } else {
            glTexStorage2D(target, levels, ifmt, width, height);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_TexStorage3D(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum ifmt = (GLenum)se_wgl_normalize_es3_internalformat((int32_t)(uint32_t)HLE_ARG2(c), 0u, 0u, true);
        glTexStorage3D((GLenum)HLE_ARG0(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                       ifmt, (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c),
                       (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG5(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_GenerateMipmap(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glGenerateMipmap((GLenum)se_wgl_normalize_es3_target((uint32_t)HLE_ARG0(c)));
#else
    (void)c;
#endif
}

static void hle_gl_UniformBlockBinding(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glUniformBlockBinding((GLuint)HLE_ARG0(c), (GLuint)HLE_ARG1(c), (GLuint)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_GetUniformBlockIndex(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        HLE_RET32(c, glGetUniformBlockIndex((GLuint)HLE_ARG0(c), (const GLchar *)(uintptr_t)HLE_ARG1(c)));
        return;
    }
#endif
    HLE_RET32(c, 0u);
}

static void hle_gl_BindBufferBase(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBindBufferBase((GLenum)HLE_ARG0(c), (GLuint)HLE_ARG1(c), (GLuint)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_BindBufferRange(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glBindBufferRange((GLenum)HLE_ARG0(c), (GLuint)HLE_ARG1(c), (GLuint)HLE_ARG2(c),
                          (GLintptr)(int64_t)HLE_ARG3(c), (GLsizeiptr)(int64_t)HLE_ARG4(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_FenceSync(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) { HLE_RET64(c, (uint64_t)(uintptr_t)glFenceSync((GLenum)HLE_ARG0(c), (GLbitfield)HLE_ARG1(c))); return; }
#endif
    HLE_RET64(c, 0x10001ull);
}

static void hle_gl_ClientWaitSync(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG0(c)) {
        HLE_RET32(c, (uint32_t)glClientWaitSync((GLsync)(uintptr_t)HLE_ARG0(c),
                                                (GLbitfield)HLE_ARG1(c), (GLuint64)HLE_ARG2(c)));
        return;
    }
#endif
    HLE_RET32(c, 0x911Au); /* GL_ALREADY_SIGNALED */
}

static void hle_gl_DeleteSync(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG0(c)) glDeleteSync((GLsync)(uintptr_t)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_GenSamplers(CPU *c) {
    uint32_t n = (uint32_t)HLE_ARG0(c);
    uint64_t out = HLE_ARG1(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && out && n > 0u) { glGenSamplers((GLsizei)n, (GLuint *)(uintptr_t)out); return; }
#endif
    alloc_gl_ids(n, out);
}

static void hle_gl_BindSampler(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBindSampler((GLuint)HLE_ARG0(c), (GLuint)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_DeleteSamplers(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && HLE_ARG1(c)) glDeleteSamplers((GLsizei)(int32_t)(uint32_t)HLE_ARG0(c), (const GLuint *)(uintptr_t)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_SamplerParameteri(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        int32_t param = se_wgl_normalize_es3_tex_param_i(pname, (int32_t)(uint32_t)HLE_ARG2(c));
        glSamplerParameteri((GLuint)HLE_ARG0(c), (GLenum)pname, (GLint)param);
    }
#else
    (void)c;
#endif
}

static void hle_gl_SamplerParameteriv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLuint sampler = (GLuint)HLE_ARG0(c);
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        const GLint *params = (const GLint *)(uintptr_t)HLE_ARG2(c);
        if (!params) return;
        if (!se_wgl_is_desktop_gl() &&
            (pname == SE_GL_TEXTURE_WRAP_S || pname == SE_GL_TEXTURE_WRAP_T || pname == SE_GL_TEXTURE_WRAP_R)) {
            GLint norm = (GLint)se_wgl_normalize_es3_tex_param_i(pname, (int32_t)params[0]);
            if (g_native_glSamplerParameteriv) {
                g_native_glSamplerParameteriv(sampler, (GLenum)pname, &norm);
            } else {
                glSamplerParameteri(sampler, (GLenum)pname, norm);
            }
        } else if (g_native_glSamplerParameteriv) {
            g_native_glSamplerParameteriv(sampler, (GLenum)pname, params);
        } else {
            glSamplerParameteri(sampler, (GLenum)pname, params[0]);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_SamplerParameterf(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t pname = (uint32_t)HLE_ARG1(c);
        float param = se_wgl_normalize_es3_tex_param_f(pname, HLE_ARG_F32(c, 2));
        glSamplerParameterf((GLuint)HLE_ARG0(c), (GLenum)pname, param);
    }
#else
    (void)c;
#endif
}

#ifndef SE_GL_READ_ONLY
#define SE_GL_READ_ONLY              0x88B8u
#endif
#ifndef SE_GL_WRITE_ONLY
#define SE_GL_WRITE_ONLY             0x88B9u
#endif
#ifndef SE_GL_READ_WRITE
#define SE_GL_READ_WRITE             0x88BAu
#endif
#ifndef SE_GL_MAP_READ_BIT
#define SE_GL_MAP_READ_BIT           0x0001u
#endif
#ifndef SE_GL_MAP_WRITE_BIT
#define SE_GL_MAP_WRITE_BIT          0x0002u
#endif
#ifndef SE_GL_BUFFER_SIZE
#define SE_GL_BUFFER_SIZE            0x8764u
#endif
#ifndef SE_GL_TEXTURE0
#define SE_GL_TEXTURE0               0x84C0u
#endif
#ifndef SE_GL_ACTIVE_TEXTURE
#define SE_GL_ACTIVE_TEXTURE         0x84E0u
#endif
#ifndef SE_GL_QUERY_RESULT
#define SE_GL_QUERY_RESULT           0x8866u
#endif
#ifndef SE_GL_QUERY_RESULT_AVAILABLE
#define SE_GL_QUERY_RESULT_AVAILABLE 0x8867u
#endif

uint32_t se_wgl_map_access_to_es3_range_bits(uint32_t access) {
    switch (access) {
    case SE_GL_READ_ONLY:  return SE_GL_MAP_READ_BIT;
    case SE_GL_WRITE_ONLY: return SE_GL_MAP_WRITE_BIT;
    case SE_GL_READ_WRITE: return SE_GL_MAP_READ_BIT | SE_GL_MAP_WRITE_BIT;
    default:               return SE_GL_MAP_READ_BIT | SE_GL_MAP_WRITE_BIT;
    }
}

static void hle_gl_MapBuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLenum access = (GLenum)HLE_ARG1(c);
        typedef void *(*pfn_map_t)(GLenum, GLenum);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_map_t fn = se_wgl_is_desktop_gl()
            ? (pfn_map_t)resolve_opt_gl_proc(&pfn, &state, "glMapBuffer", NULL, NULL, NULL)
            : NULL;
        if (fn) {
            HLE_RET64(c, (uint64_t)(uintptr_t)fn(target, access));
            return;
        }
        typedef void (*pfn_gbpi64_t)(GLenum, GLenum, GLint64 *);
        static void *pfn_sz = NULL;
        static uint32_t state_sz = 0u;
        pfn_gbpi64_t fn_sz = (pfn_gbpi64_t)resolve_opt_gl_proc(&pfn_sz, &state_sz,
                                                               "glGetBufferParameteri64v",
                                                               "glGetBufferParameteri64v",
                                                               NULL, NULL);
        GLint64 buf_size = 0;
        if (fn_sz) fn_sz(target, (GLenum)SE_GL_BUFFER_SIZE, &buf_size);
        if (buf_size <= 0) {
            HLE_RET64(c, 0ull);
            return;
        }
        GLbitfield bits = (GLbitfield)se_wgl_map_access_to_es3_range_bits((uint32_t)access);
        HLE_RET64(c, (uint64_t)(uintptr_t)glMapBufferRange(target, 0, (GLsizeiptr)buf_size, bits));
        return;
    }
#endif
    HLE_RET64(c, 0ull);
}

static void hle_gl_GetBufferSubData(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLintptr offset = (GLintptr)(int64_t)HLE_ARG1(c);
        GLsizeiptr size = (GLsizeiptr)(int64_t)HLE_ARG2(c);
        void *data = (void *)(uintptr_t)HLE_ARG3(c);
        if (!data || size <= 0) return;
        typedef void (*pfn_subdata_t)(GLenum, GLintptr, GLsizeiptr, void *);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_subdata_t fn = se_wgl_is_desktop_gl()
            ? (pfn_subdata_t)resolve_opt_gl_proc(&pfn, &state, "glGetBufferSubData", NULL, NULL, NULL)
            : NULL;
        if (fn) {
            fn(target, offset, size, data);
            return;
        }
        const void *src = glMapBufferRange(target, offset, size, (GLbitfield)SE_GL_MAP_READ_BIT);
        if (src) {
            memcpy(data, src, (size_t)size);
            glUnmapBuffer(target);
        } else {
            memset(data, 0, (size_t)size);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_BindMultiTextureEXT(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum texunit = (GLenum)HLE_ARG0(c);
        GLenum target = (GLenum)HLE_ARG1(c);
        GLuint texture = (GLuint)HLE_ARG2(c);
        typedef void (*pfn_bind_multi_t)(GLenum, GLenum, GLuint);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_bind_multi_t fn = se_wgl_is_desktop_gl()
            ? (pfn_bind_multi_t)resolve_opt_gl_proc(&pfn, &state, "glBindMultiTextureEXT", NULL, NULL, NULL)
            : NULL;
        if (fn) {
            fn(texunit, target, texture);
        } else {
            GLenum norm_target = (GLenum)se_wgl_normalize_es3_target((uint32_t)target);
            GLint prev_unit = (GLint)SE_GL_TEXTURE0;
            glGetIntegerv((GLenum)SE_GL_ACTIVE_TEXTURE, &prev_unit);
            if ((GLenum)prev_unit != texunit) glActiveTexture(texunit);
            glBindTexture(norm_target, texture);
            if ((GLenum)prev_unit != texunit) glActiveTexture((GLenum)prev_unit);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_DepthRangedNV(CPU *c) {
    hle_gl_DepthRange(c);
}

static void hle_gl_PrimitiveRestartIndex(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        typedef void (*pfn_pri_t)(GLuint);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_pri_t fn = se_wgl_is_desktop_gl()
            ? (pfn_pri_t)resolve_opt_gl_proc(&pfn, &state, "glPrimitiveRestartIndex", NULL, NULL, NULL)
            : NULL;
        if (fn) fn((GLuint)HLE_ARG0(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_FramebufferTexture(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum target = (GLenum)HLE_ARG0(c);
        GLenum attachment = (GLenum)HLE_ARG1(c);
        GLuint texture = (GLuint)HLE_ARG2(c);
        GLint level = (GLint)(int32_t)(uint32_t)HLE_ARG3(c);
        typedef void (*pfn_fbt_t)(GLenum, GLenum, GLuint, GLint);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_fbt_t fn = (pfn_fbt_t)resolve_opt_gl_proc(&pfn, &state,
                                                       "glFramebufferTexture",
                                                       "glFramebufferTexture",
                                                       "OpenGL ES 3.2",
                                                       "geometry_shader");
        if (fn) {
            fn(target, attachment, texture, level);
        } else if (texture == 0u) {
            glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0u, level);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_FramebufferTextureLayer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        typedef void (*pfn_fbtl_t)(GLenum, GLenum, GLuint, GLint, GLint);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_fbtl_t fn = (pfn_fbtl_t)resolve_opt_gl_proc(&pfn, &state,
                                                         "glFramebufferTextureLayer",
                                                         "glFramebufferTextureLayer",
                                                         NULL, NULL);
        if (fn) {
            fn((GLenum)HLE_ARG0(c),
               (GLenum)HLE_ARG1(c),
               (GLuint)HLE_ARG2(c),
               (GLint)(int32_t)(uint32_t)HLE_ARG3(c),
               (GLint)(int32_t)(uint32_t)hle_stack_arg(c, 4u));
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_InvalidateTexImage(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        typedef void (*pfn_inv_t)(GLuint, GLint);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_inv_t fn = se_wgl_is_desktop_gl()
            ? (pfn_inv_t)resolve_opt_gl_proc(&pfn, &state, "glInvalidateTexImage", NULL, NULL, NULL)
            : NULL;
        if (fn) fn((GLuint)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_MaxShaderCompilerThreadsARB(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        typedef void (*pfn_threads_t)(GLuint);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_threads_t fn = (pfn_threads_t)resolve_opt_gl_proc(&pfn, &state,
                                                               "glMaxShaderCompilerThreadsARB",
                                                               "glMaxShaderCompilerThreadsKHR",
                                                               NULL,
                                                               "GL_KHR_parallel_shader_compile");
        if (fn) fn((GLuint)HLE_ARG0(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_QueryCounter(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        typedef void (*pfn_qc_t)(GLuint, GLenum);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_qc_t fn = (pfn_qc_t)resolve_opt_gl_proc(&pfn, &state,
                                                     "glQueryCounter",
                                                     "glQueryCounterEXT",
                                                     NULL,
                                                     "GL_EXT_disjoint_timer_query");
        if (fn) fn((GLuint)HLE_ARG0(c), (GLenum)HLE_ARG1(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_get_query_object64(CPU *c, bool is_signed) {
    uint32_t pname = (uint32_t)HLE_ARG1(c);
    uint64_t params = HLE_ARG2(c);
#if defined(__ANDROID__)
    if (tl_cur_hglrc && params) {
        GLuint id = (GLuint)HLE_ARG0(c);
        typedef void (*pfn_gq64_t)(GLuint, GLenum, uint64_t *);
        static void *pfns[2] = { NULL, NULL };
        static uint32_t states[2] = { 0u, 0u };
        size_t slot = is_signed ? 1u : 0u;
        const char *primary = is_signed ? "glGetQueryObjecti64v" : "glGetQueryObjectui64v";
        const char *fallback = is_signed ? "glGetQueryObjecti64vEXT" : "glGetQueryObjectui64vEXT";
        pfn_gq64_t fn = (pfn_gq64_t)resolve_opt_gl_proc(&pfns[slot], &states[slot],
                                                         primary, fallback, NULL,
                                                         "GL_EXT_disjoint_timer_query");
        if (fn) {
            fn(id, (GLenum)pname, (uint64_t *)(uintptr_t)params);
            return;
        }
    }
#else
    (void)is_signed;
#endif
    if (params) {
        wr64(params, (pname == SE_GL_QUERY_RESULT_AVAILABLE) ? 1ull : 0ull);
    }
}

static void hle_gl_GetQueryObjecti64v(CPU *c) {
    hle_gl_get_query_object64(c, true);
}

static void hle_gl_GetQueryObjectui64v(CPU *c) {
    hle_gl_get_query_object64(c, false);
}

static void hle_gl_ReadnPixels(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLint x = (GLint)(int32_t)(uint32_t)HLE_ARG0(c);
        GLint y = (GLint)(int32_t)(uint32_t)HLE_ARG1(c);
        GLsizei width = (GLsizei)(int32_t)(uint32_t)HLE_ARG2(c);
        GLsizei height = (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c);
        GLenum format = (GLenum)HLE_ARG4(c);
        GLenum type = (GLenum)HLE_ARG5(c);
        GLsizei buf_size = (GLsizei)(int32_t)(uint32_t)HLE_ARG6(c);
        void *data = (void *)(uintptr_t)HLE_ARG7(c);
        typedef void (*pfn_rnp_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLsizei, void *);
        static void *pfn = NULL;
        static uint32_t state = 0u;
        pfn_rnp_t fn = (pfn_rnp_t)resolve_opt_gl_proc(&pfn, &state,
                                                       "glReadnPixels",
                                                       "glReadnPixels",
                                                       "OpenGL ES 3.2",
                                                       "robustness");
        if (fn) {
            fn(x, y, width, height, format, type, buf_size, data);
        } else {
            glReadPixels(x, y, width, height, format, type, data);
        }
    }
#else
    (void)c;
#endif
}

static void hle_gl_MapBufferRange(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        uint32_t access = (uint32_t)HLE_ARG3(c);
        if (!se_wgl_is_desktop_gl() && !g_es3_has_buffer_storage) {
            access &= ~(SE_GL_MAP_PERSISTENT_BIT | SE_GL_MAP_COHERENT_BIT);
        }
        HLE_RET64(c, (uint64_t)(uintptr_t)glMapBufferRange((GLenum)HLE_ARG0(c),
                                                          (GLintptr)(int64_t)HLE_ARG1(c),
                                                          (GLsizeiptr)(int64_t)HLE_ARG2(c),
                                                          (GLbitfield)access));
        return;
    }
#endif
    HLE_RET64(c, 0ull);
}

static void hle_gl_UnmapBuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) { HLE_RET32(c, (uint32_t)glUnmapBuffer((GLenum)HLE_ARG0(c))); return; }
#endif
    HLE_RET32(c, 1u);
}

static void hle_gl_FlushMappedBufferRange(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glFlushMappedBufferRange((GLenum)HLE_ARG0(c), (GLintptr)(int64_t)HLE_ARG1(c), (GLsizeiptr)(int64_t)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_BlendFuncSeparate(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBlendFuncSeparate((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c), (GLenum)HLE_ARG2(c), (GLenum)HLE_ARG3(c));
#else
    (void)c;
#endif
}

static void hle_gl_BlendEquation(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBlendEquation((GLenum)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_BlendEquationSeparate(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glBlendEquationSeparate((GLenum)HLE_ARG0(c), (GLenum)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_BlitFramebuffer(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glBlitFramebuffer((GLint)(int32_t)(uint32_t)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                          (GLint)(int32_t)(uint32_t)HLE_ARG2(c), (GLint)(int32_t)(uint32_t)HLE_ARG3(c),
                          (GLint)(int32_t)(uint32_t)HLE_ARG4(c), (GLint)(int32_t)(uint32_t)HLE_ARG5(c),
                          (GLint)(int32_t)(uint32_t)HLE_ARG6(c), (GLint)(int32_t)(uint32_t)HLE_ARG7(c),
                          (GLbitfield)hle_stack_arg(c, 8u), (GLenum)hle_stack_arg(c, 9u));
    }
#else
    (void)c;
#endif
}

static void hle_gl_RenderbufferStorageMultisample(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        GLenum ifmt = (GLenum)se_wgl_normalize_es3_internalformat(
            (int32_t)(uint32_t)HLE_ARG2(c), 0u, 0u, true);
        glRenderbufferStorageMultisample((GLenum)HLE_ARG0(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                                         ifmt, (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c),
                                         (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_DrawArraysInstanced(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glDrawArraysInstanced((GLenum)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                              (GLsizei)(int32_t)(uint32_t)HLE_ARG2(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_DrawElementsInstanced(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glDrawElementsInstanced((GLenum)HLE_ARG0(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                                (GLenum)HLE_ARG2(c), (const void *)(uintptr_t)HLE_ARG3(c),
                                (GLsizei)(int32_t)(uint32_t)HLE_ARG4(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_VertexAttribDivisor(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glVertexAttribDivisor((GLuint)HLE_ARG0(c), (GLuint)HLE_ARG1(c));
#else
    (void)c;
#endif
}

static void hle_gl_ProgramParameteri(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glProgramParameteri((GLuint)HLE_ARG0(c), (GLenum)HLE_ARG1(c), (GLint)(int32_t)(uint32_t)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_GetProgramBinary(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glGetProgramBinary) {
        g_native_glGetProgramBinary((GLuint)HLE_ARG0(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG1(c),
                                    (GLsizei *)(uintptr_t)HLE_ARG2(c), (GLenum *)(uintptr_t)HLE_ARG3(c),
                                    (void *)(uintptr_t)HLE_ARG4(c));
        return;
    }
#endif
    if (HLE_ARG2(c)) wr32(HLE_ARG2(c), 0u);
}

static void hle_gl_ProgramBinary(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glProgramBinary) {
        g_native_glProgramBinary((GLuint)HLE_ARG0(c), (GLenum)HLE_ARG1(c),
                                 (const void *)(uintptr_t)HLE_ARG2(c), (GLsizei)(int32_t)(uint32_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_ClearBufferfv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glClearBufferfv((GLenum)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c), (const GLfloat *)(uintptr_t)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_ClearBufferiv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glClearBufferiv((GLenum)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c), (const GLint *)(uintptr_t)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_ClearBufferuiv(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) glClearBufferuiv((GLenum)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c), (const GLuint *)(uintptr_t)HLE_ARG2(c));
#else
    (void)c;
#endif
}

static void hle_gl_ClearBufferfi(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc) {
        glClearBufferfi((GLenum)HLE_ARG0(c), (GLint)(int32_t)(uint32_t)HLE_ARG1(c),
                        HLE_ARG_F32(c, 2), (GLint)(int32_t)(uint32_t)HLE_ARG3(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_BindImageTexture(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glBindImageTexture) {
        g_native_glBindImageTexture((GLuint)HLE_ARG0(c), (GLuint)HLE_ARG1(c), (GLint)(int32_t)(uint32_t)HLE_ARG2(c),
                                    (GLboolean)(HLE_ARG3(c) != 0), (GLint)(int32_t)(uint32_t)HLE_ARG4(c),
                                    (GLenum)HLE_ARG5(c), (GLenum)HLE_ARG6(c));
    }
#else
    (void)c;
#endif
}

static void hle_gl_MemoryBarrier(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glMemoryBarrier) g_native_glMemoryBarrier((GLbitfield)HLE_ARG0(c));
#else
    (void)c;
#endif
}

static void hle_gl_DispatchCompute(CPU *c) {
#if defined(__ANDROID__)
    if (tl_cur_hglrc && g_native_glDispatchCompute) {
        g_native_glDispatchCompute((GLuint)HLE_ARG0(c), (GLuint)HLE_ARG1(c), (GLuint)HLE_ARG2(c));
    }
#else
    (void)c;
#endif
}

#include "gl46_procs_gen.inc"

static uint64_t g_debug_cb_va = 0ull;
static uint64_t g_debug_user_param = 0ull;

static void hle_gl_DebugMessageCallback(CPU *c) {
    /* Store guest VA safely; never pass raw x86-64 guest pointers to native AArch64 drivers. */
    g_debug_cb_va = HLE_ARG0(c);
    g_debug_user_param = HLE_ARG1(c);
    (void)g_debug_cb_va;
    (void)g_debug_user_param;
}

static void hle_wgl_dyn_trampoline(CPU *c) {
    uint32_t idx = se_hle_active_slot_index();
    void *fn = (idx < SE_HLE_TOTAL_SLOTS) ? g_dyn_native_procs[idx] : NULL;
    if (!fn) {
        HLE_RET64(c, 0ull);
        return;
    }
    typedef uint64_t (*generic_gl_fn_t)(
        uint64_t, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t, uint64_t, uint64_t);
    generic_gl_fn_t gf = (generic_gl_fn_t)(uintptr_t)fn;
    uint64_t r = gf(HLE_ARG0(c), HLE_ARG1(c), HLE_ARG2(c), HLE_ARG3(c),
                    HLE_ARG4(c), HLE_ARG5(c), HLE_ARG6(c), HLE_ARG7(c),
                    hle_stack_arg(c, 8u), hle_stack_arg(c, 9u),
                    hle_stack_arg(c, 10u), hle_stack_arg(c, 11u),
                    hle_stack_arg(c, 12u), hle_stack_arg(c, 13u),
                    hle_stack_arg(c, 14u), hle_stack_arg(c, 15u));
    HLE_RET64(c, r);
}

typedef struct {
    const char *name;
    se_hle_fn_t fn;
} wgl_proc_entry_t;

static const wgl_proc_entry_t k_wgl_priority_table[] = {
    { "wglChoosePixelFormatARB",      hle_wgl_ChoosePixelFormatARB },
    { "wglGetPixelFormatAttribivARB", hle_wgl_GetPixelFormatAttribivARB },
    { "wglCreateContextAttribsARB",   hle_wgl_CreateContextAttribsARB },
    { "wglSwapIntervalEXT",           hle_wgl_SwapIntervalEXT },
    { "wglGetSwapIntervalEXT",        hle_wgl_GetSwapIntervalEXT },
    { "wglGetExtensionsStringARB",    hle_wgl_GetExtensionsStringARB },
    { "wglGetExtensionsStringEXT",    hle_wgl_GetExtensionsStringARB },
    { "wglGetCurrentContext",         hle_wgl_GetCurrentContext },
    { "wglGetCurrentDC",              hle_wgl_GetCurrentDC },
    { "glGetStringi",                 hle_gl_GetStringi },
    { "glDebugMessageCallback",       hle_gl_DebugMessageCallback },
    { "glDebugMessageCallbackARB",    hle_gl_DebugMessageCallback },
    { "glDebugMessageCallbackAMD",    hle_gl_DebugMessageCallback },
    { NULL, NULL }
};

static const wgl_proc_entry_t k_wgl_proc_table[] = {
    { "glActiveTexture",              hle_gl_ActiveTexture },
    { "glClipControl",                hle_gl_ClipControl },
    { "glCreateShader",               hle_gl_CreateShader },
    { "glShaderSource",               hle_gl_ShaderSource },
    { "glCompileShader",              hle_gl_CompileShader },
    { "glDeleteShader",               hle_gl_DeleteShader },
    { "glGetShaderiv",                hle_gl_GetShaderiv },
    { "glGetShaderInfoLog",           hle_gl_GetShaderInfoLog },
    { "glCreateProgram",              hle_gl_CreateProgram },
    { "glAttachShader",               hle_gl_AttachShader },
    { "glLinkProgram",                hle_gl_LinkProgram },
    { "glUseProgram",                 hle_gl_UseProgram },
    { "glDeleteProgram",              hle_gl_DeleteProgram },
    { "glGetProgramiv",               hle_gl_GetProgramiv },
    { "glGetProgramInfoLog",          hle_gl_GetProgramInfoLog },
    { "glGetUniformLocation",         hle_gl_GetUniformLocation },
    { "glGetAttribLocation",          hle_gl_GetAttribLocation },
    { "glUniform1i",                  hle_gl_Uniform1i },
    { "glUniform1f",                  hle_gl_Uniform1f },
    { "glUniform2f",                  hle_gl_Uniform2f },
    { "glUniform3f",                  hle_gl_Uniform3f },
    { "glUniform4f",                  hle_gl_Uniform4f },
    { "glUniformMatrix4fv",           hle_gl_UniformMatrix4fv },
    { "glGenBuffers",                 hle_gl_GenBuffers },
    { "glBindBuffer",                 hle_gl_BindBuffer },
    { "glBufferData",                 hle_gl_BufferData },
    { "glBufferSubData",              hle_gl_BufferSubData },
    { "glBufferStorage",              hle_gl_BufferStorage },
    { "glDeleteBuffers",              hle_gl_DeleteBuffers },
    { "glGenVertexArrays",            hle_gl_GenVertexArrays },
    { "glBindVertexArray",            hle_gl_BindVertexArray },
    { "glDeleteVertexArrays",         hle_gl_DeleteVertexArrays },
    { "glEnableVertexAttribArray",    hle_gl_EnableVertexAttribArray },
    { "glDisableVertexAttribArray",   hle_gl_DisableVertexAttribArray },
    { "glVertexAttribPointer",        hle_gl_VertexAttribPointer },
    { "glGenFramebuffers",            hle_gl_GenFramebuffers },
    { "glBindFramebuffer",            hle_gl_BindFramebuffer },
    { "glFramebufferTexture2D",       hle_gl_FramebufferTexture2D },
    { "glFramebufferTexture",         hle_gl_FramebufferTexture },
    { "glFramebufferTextureLayer",    hle_gl_FramebufferTextureLayer },
    { "glCheckFramebufferStatus",     hle_gl_CheckFramebufferStatus },
    { "glDeleteFramebuffers",         hle_gl_DeleteFramebuffers },
    { "glGenRenderbuffers",           hle_gl_GenRenderbuffers },
    { "glBindRenderbuffer",           hle_gl_BindRenderbuffer },
    { "glRenderbufferStorage",        hle_gl_RenderbufferStorage },
    { "glRenderbufferStorageMultisample", hle_gl_RenderbufferStorageMultisample },
    { "glFramebufferRenderbuffer",    hle_gl_FramebufferRenderbuffer },
    { "glDeleteRenderbuffers",        hle_gl_DeleteRenderbuffers },
    { "glBlitFramebuffer",            hle_gl_BlitFramebuffer },
    { "glDrawBuffers",                hle_gl_DrawBuffers },
    { "glTexImage3D",                 hle_gl_TexImage3D },
    { "glTexSubImage3D",              hle_gl_TexSubImage3D },
    { "glCompressedTexImage1D",       hle_gl_CompressedTexImage1D },
    { "glCompressedTexImage2D",       hle_gl_CompressedTexImage2D },
    { "glCompressedTexImage3D",       hle_gl_CompressedTexImage3D },
    { "glCompressedTexSubImage1D",    hle_gl_CompressedTexSubImage1D },
    { "glCompressedTexSubImage2D",    hle_gl_CompressedTexSubImage2D },
    { "glCompressedTexSubImage3D",    hle_gl_CompressedTexSubImage3D },
    { "glCreateTextures",             hle_gl_CreateTextures },
    { "glTexStorage2D",               hle_gl_TexStorage2D },
    { "glTexStorage3D",               hle_gl_TexStorage3D },
    { "glGenerateMipmap",             hle_gl_GenerateMipmap },
    { "glUniformBlockBinding",        hle_gl_UniformBlockBinding },
    { "glGetUniformBlockIndex",       hle_gl_GetUniformBlockIndex },
    { "glBindBufferBase",             hle_gl_BindBufferBase },
    { "glBindBufferRange",            hle_gl_BindBufferRange },
    { "glFenceSync",                  hle_gl_FenceSync },
    { "glClientWaitSync",             hle_gl_ClientWaitSync },
    { "glDeleteSync",                 hle_gl_DeleteSync },
    { "glGenSamplers",                hle_gl_GenSamplers },
    { "glBindSampler",                hle_gl_BindSampler },
    { "glDeleteSamplers",             hle_gl_DeleteSamplers },
    { "glSamplerParameteri",          hle_gl_SamplerParameteri },
    { "glSamplerParameteriv",         hle_gl_SamplerParameteriv },
    { "glSamplerParameterf",          hle_gl_SamplerParameterf },
    { "glTexParameterfv",             hle_gl_TexParameterfv },
    { "glMapBuffer",                  hle_gl_MapBuffer },
    { "glGetBufferSubData",           hle_gl_GetBufferSubData },
    { "glMapBufferRange",             hle_gl_MapBufferRange },
    { "glUnmapBuffer",                hle_gl_UnmapBuffer },
    { "glFlushMappedBufferRange",     hle_gl_FlushMappedBufferRange },
    { "glBindMultiTextureEXT",        hle_gl_BindMultiTextureEXT },
    { "glDepthRangedNV",              hle_gl_DepthRangedNV },
    { "glPrimitiveRestartIndex",      hle_gl_PrimitiveRestartIndex },
    { "glInvalidateTexImage",         hle_gl_InvalidateTexImage },
    { "glMaxShaderCompilerThreadsARB", hle_gl_MaxShaderCompilerThreadsARB },
    { "glQueryCounter",               hle_gl_QueryCounter },
    { "glGetQueryObjecti64v",         hle_gl_GetQueryObjecti64v },
    { "glGetQueryObjectui64v",        hle_gl_GetQueryObjectui64v },
    { "glReadnPixels",                hle_gl_ReadnPixels },
    { "glBlendFuncSeparate",          hle_gl_BlendFuncSeparate },
    { "glBlendEquation",              hle_gl_BlendEquation },
    { "glBlendEquationSeparate",      hle_gl_BlendEquationSeparate },
    { "glDrawArraysInstanced",        hle_gl_DrawArraysInstanced },
    { "glDrawElementsInstanced",      hle_gl_DrawElementsInstanced },
    { "glVertexAttribDivisor",        hle_gl_VertexAttribDivisor },
    { "glProgramParameteri",          hle_gl_ProgramParameteri },
    { "glGetProgramBinary",           hle_gl_GetProgramBinary },
    { "glProgramBinary",              hle_gl_ProgramBinary },
    { "glClearBufferfv",              hle_gl_ClearBufferfv },
    { "glClearBufferiv",              hle_gl_ClearBufferiv },
    { "glClearBufferuiv",             hle_gl_ClearBufferuiv },
    { "glClearBufferfi",              hle_gl_ClearBufferfi },
    { "glBindImageTexture",           hle_gl_BindImageTexture },
    { "glMemoryBarrier",              hle_gl_MemoryBarrier },
    { "glDispatchCompute",            hle_gl_DispatchCompute },
    { NULL, NULL }
};

static uint64_t register_wgl_proc_table_slot(const char *name) {
    for (size_t i = 0; k_wgl_proc_table[i].name != NULL; ++i) {
        if (strcmp(k_wgl_proc_table[i].name, name) == 0) {
            return se_hle_register_dynamic("WGL_EXT", name, k_wgl_proc_table[i].fn);
        }
    }
    return 0ull;
}

static void hle_wgl_GetProcAddress(CPU *c) {
    uint64_t ptr = HLE_ARG0(c);
    if (!ptr) {
        HLE_RET64(c, 0ull);
        return;
    }
    const char *name = (const char *)(uintptr_t)ptr;

    /* 1. Check static OPENGL32 import slot first if present */
    uint64_t existing = se_hle_find_sentinel("OPENGL32.dll", name);
    if (existing) {
        HLE_RET64(c, existing);
        return;
    }

    /* 2. Check already-registered dynamic sentinel */
    existing = se_hle_find_sentinel("WGL_EXT", name);
    if (existing) {
        HLE_RET64(c, existing);
        return;
    }

    /* 3. Priority WGL extensions & safe callback overrides (never pass guest VA to driver) */
    for (size_t i = 0; k_wgl_priority_table[i].name != NULL; ++i) {
        if (strcmp(k_wgl_priority_table[i].name, name) == 0) {
            uint64_t s = se_hle_register_dynamic("WGL_EXT", name, k_wgl_priority_table[i].fn);
            HLE_RET64(c, s);
            return;
        }
    }

    /* 4. In GLES3 fallback mode, check ES3 compatibility table BEFORE raw driver procs
     *    so glShaderSource / textures / BC1-5 / ClipControl / BufferStorage get translated. */
    bool desktop = se_wgl_is_desktop_gl();
    if (!desktop) {
        uint64_t s = register_wgl_proc_table_slot(name);
        if (s) {
            HLE_RET64(c, s);
            return;
        }
    }

    /* 5. Resolve real native driver proc (Mesa Zink OpenGL 4.6 Core or unshimmed GLES3) */
    void *native_fn = lookup_native_gl_proc(name);
    if (native_fn) {
        se_hle_fn_t thunk = se_find_gl46_typed_thunk(name);
        if (!thunk) {
            thunk = hle_wgl_dyn_trampoline;
        }
        uint64_t s = se_hle_register_dynamic("WGL_EXT", name, thunk);
        if (s >= HLE_BASE) {
            uint64_t idx = (s - HLE_BASE) / HLE_STRIDE;
            if (idx < SE_HLE_TOTAL_SLOTS) {
                g_dyn_native_procs[idx] = native_fn;
            }
            HLE_RET64(c, s);
            return;
        }
    }

    /* 6. Desktop / host fallback implementation table */
    if (desktop) {
        uint64_t s = register_wgl_proc_table_slot(name);
        if (s) {
            HLE_RET64(c, s);
            return;
        }
    }

    /* 6. Allow core GL / ARB query fallbacks safely if queried by GLEW */
    if (strncmp(name, "gl", 2) == 0 && strstr(name, "NV") == NULL &&
        strstr(name, "AMD") == NULL && strstr(name, "ATI") == NULL) {
        se_hle_fn_t fallback = hle_gl_noop;
        if (strstr(name, "Create") || strncmp(name, "glGen", 5) == 0) {
            fallback = hle_gl_ret1;
        } else if (strncmp(name, "glIs", 4) == 0 || strncmp(name, "glCheck", 7) == 0) {
            fallback = hle_gl_ret0;
        }
        uint64_t s = se_hle_register_dynamic("WGL_EXT", name, fallback);
        HLE_RET64(c, s);
        return;
    }

    HLE_RET64(c, 0ull);
}

/* ========================================================================= */
/* 4. OPENGL32.dll (50 imports) Registration                                 */
/* ========================================================================= */

void se_register_wgl_egl_shims(void) {
    /* Reference helpers in case host/static build prunes unused */
    (void)hle_gl_ret0;
    (void)hle_wgl_dyn_trampoline;

    se_hle_register_import("OPENGL32.dll", "glClearStencil",           hle_gl_ClearStencil);
    se_hle_register_import("OPENGL32.dll", "glCullFace",               hle_gl_CullFace);
    se_hle_register_import("OPENGL32.dll", "wglGetCurrentDC",          hle_wgl_GetCurrentDC);
    se_hle_register_import("OPENGL32.dll", "glBlendFunc",              hle_gl_BlendFunc);
    se_hle_register_import("OPENGL32.dll", "glFinish",                 hle_gl_Finish);
    se_hle_register_import("OPENGL32.dll", "glGetTexImage",            hle_gl_GetTexImage);
    se_hle_register_import("OPENGL32.dll", "glPixelStorei",            hle_gl_PixelStorei);
    se_hle_register_import("OPENGL32.dll", "glLineWidth",              hle_gl_LineWidth);
    se_hle_register_import("OPENGL32.dll", "glGetTexLevelParameteriv", hle_gl_GetTexLevelParameteriv);
    se_hle_register_import("OPENGL32.dll", "glTexParameteri",          hle_gl_TexParameteri);
    se_hle_register_import("OPENGL32.dll", "glGetIntegerv",            hle_gl_GetIntegerv);
    se_hle_register_import("OPENGL32.dll", "glGenTextures",            hle_gl_GenTextures);
    se_hle_register_import("OPENGL32.dll", "glTexImage1D",             hle_gl_TexImage1D);
    se_hle_register_import("OPENGL32.dll", "glDeleteTextures",         hle_gl_DeleteTextures);
    se_hle_register_import("OPENGL32.dll", "glIsEnabled",              hle_gl_IsEnabled);
    se_hle_register_import("OPENGL32.dll", "glClearColor",             hle_gl_ClearColor);
    se_hle_register_import("OPENGL32.dll", "glGetString",              hle_gl_GetString);
    se_hle_register_import("OPENGL32.dll", "glDepthRange",             hle_gl_DepthRange);
    se_hle_register_import("OPENGL32.dll", "glClearDepth",             hle_gl_ClearDepth);
    se_hle_register_import("OPENGL32.dll", "glDepthFunc",              hle_gl_DepthFunc);
    se_hle_register_import("OPENGL32.dll", "glDrawArrays",             hle_gl_DrawArrays);
    se_hle_register_import("OPENGL32.dll", "glTexSubImage2D",          hle_gl_TexSubImage2D);
    se_hle_register_import("OPENGL32.dll", "glClear",                  hle_gl_Clear);
    se_hle_register_import("OPENGL32.dll", "glDrawBuffer",             hle_gl_DrawBuffer);
    se_hle_register_import("OPENGL32.dll", "glBindTexture",            hle_gl_BindTexture);
    se_hle_register_import("OPENGL32.dll", "glTexParameteriv",         hle_gl_TexParameteriv);
    se_hle_register_import("OPENGL32.dll", "glTexParameterf",          hle_gl_TexParameterf);
    se_hle_register_import("OPENGL32.dll", "glTexSubImage1D",          hle_gl_TexSubImage1D);
    se_hle_register_import("OPENGL32.dll", "glTexImage2D",             hle_gl_TexImage2D);
    se_hle_register_import("OPENGL32.dll", "glPolygonOffset",          hle_gl_PolygonOffset);
    se_hle_register_import("OPENGL32.dll", "glPolygonMode",            hle_gl_PolygonMode);
    se_hle_register_import("OPENGL32.dll", "glPointSize",              hle_gl_PointSize);
    se_hle_register_import("OPENGL32.dll", "glReadPixels",             hle_gl_ReadPixels);
    se_hle_register_import("OPENGL32.dll", "glReadBuffer",             hle_gl_ReadBuffer);
    se_hle_register_import("OPENGL32.dll", "glViewport",               hle_gl_Viewport);
    se_hle_register_import("OPENGL32.dll", "glDepthMask",              hle_gl_DepthMask);
    se_hle_register_import("OPENGL32.dll", "wglGetProcAddress",        hle_wgl_GetProcAddress);
    se_hle_register_import("OPENGL32.dll", "wglDeleteContext",         hle_wgl_DeleteContext);
    se_hle_register_import("OPENGL32.dll", "wglMakeCurrent",           hle_wgl_MakeCurrent);
    se_hle_register_import("OPENGL32.dll", "wglShareLists",            hle_wgl_ShareLists);
    se_hle_register_import("OPENGL32.dll", "wglCreateContext",         hle_wgl_CreateContext);
    se_hle_register_import("OPENGL32.dll", "glFlush",                  hle_gl_Flush);
    se_hle_register_import("OPENGL32.dll", "wglSwapLayerBuffers",      hle_wgl_SwapLayerBuffers);
    se_hle_register_import("OPENGL32.dll", "glColorMask",              hle_gl_ColorMask);
    se_hle_register_import("OPENGL32.dll", "glDrawElements",           hle_gl_DrawElements);
    se_hle_register_import("OPENGL32.dll", "glGetError",               hle_gl_GetError);
    se_hle_register_import("OPENGL32.dll", "glFrontFace",              hle_gl_FrontFace);
    se_hle_register_import("OPENGL32.dll", "glDisable",                hle_gl_Disable);
    se_hle_register_import("OPENGL32.dll", "glEnable",                 hle_gl_Enable);
    se_hle_register_import("OPENGL32.dll", "glHint",                   hle_gl_Hint);
}
