/*
 * win32_user32_gdi32.c - Phase 4L Landscape-enforced USER32 (58) & GDI32 (20) shims
 * with live splash-window rendering, GDI software DIB/BitBlt/DrawTextW rasterizer,
 * guest WNDPROC dispatch, and touch/key MSG ring buffer.
 */
#include "hle_bridge.h"
#include "engine_host.h"

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <android/native_window.h>
#define U32_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineU32", fmt, ##__VA_ARGS__)
#else
#define U32_LOG(fmt, ...) ((void)0)
#endif

/* Well-known Win64 HWND / DC sentinels */
#define SE_HWND_SPLASH 0x0000000000000001ull
#define SE_HWND_MAIN   0x0000000000000002ull
#define SE_HWND_AUX0   0x0000000000010010ull
#define SE_MAIN_HDC    0x0000000000020010ull
#define SE_SPLASH_HDC  0x0000000000020020ull
#define SE_HMONITOR0   0x0000000000010001ull

/* Win32 message constants */
#define WM_CREATE      0x0001u
#define WM_DESTROY     0x0002u
#define WM_SIZE        0x0005u
#define WM_ACTIVATE    0x0006u
#define WM_SETFOCUS    0x0007u
#define WM_PAINT       0x000Fu
#define WM_CLOSE       0x0010u
#define WM_QUIT        0x0012u
#define WM_ERASEBKGND  0x0014u
#define WM_NCCREATE    0x0081u
#define WM_KEYDOWN     0x0100u
#define WM_KEYUP       0x0101u
#define WM_CHAR        0x0102u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP   0x0208u
#define WM_MOUSEWHEEL  0x020Au

#define PM_REMOVE      0x0001u

/* GDI handle spaces */
#define GDI_HDC_DYN_BASE   0x00020100ull
#define GDI_OBJ_DYN_BASE   0x00040100ull
#define GDI_STOCK_OBJ_BASE 0x0004F000ull

/* ========================================================================= */
/* Window Class & Window Registry                                            */
/* ========================================================================= */

#define MAX_WND_CLASSES 32
#define MAX_WND_HANDLES 16
#define MAX_GDI_OBJECTS 128
#define MAX_GDI_DCS     32

typedef enum {
    GDI_OBJ_NONE = 0,
    GDI_OBJ_BITMAP,
    GDI_OBJ_BRUSH,
    GDI_OBJ_PEN,
    GDI_OBJ_FONT
} gdi_obj_kind_t;

typedef struct {
    bool           used;
    uint64_t       handle;
    gdi_obj_kind_t kind;
    int32_t        width;
    int32_t        height;
    bool           top_down;
    uint32_t      *pixels;     /* top-down or bottom-up 32-bit BGRA */
    uint32_t       color_xrgb; /* 0xFFRRGGBB */
    bool           hollow;
    int32_t        pen_width;
    int32_t        font_height;
} gdi_obj_t;

typedef struct {
    bool     used;
    uint64_t hdc;
    bool     is_window_dc;
    uint64_t hwnd;
    uint64_t sel_bitmap;
    uint64_t sel_brush;
    uint64_t sel_pen;
    uint64_t sel_font;
    uint32_t text_color_xrgb;
    uint32_t text_color_cref;
    int32_t  bk_mode;
    bool     pixel_format_set;
    int32_t  pixel_format;
} gdi_dc_t;

typedef struct {
    bool     used;
    uint16_t atom;
    char     name[128];
    uint64_t wndproc_va;
    uint32_t style;
} se_wndclass_t;

typedef struct {
    bool      used;
    uint64_t  hwnd;
    uint64_t  wndproc_va;
    int64_t   userdata;
    int32_t   style;
    int32_t   exstyle;
    int32_t   x;
    int32_t   y;
    int32_t   width;
    int32_t   height;
    bool      visible;
    bool      is_splash;
    bool      is_main;
    bool      pixel_format_set;
    int32_t   pixel_format;
    uint64_t  dc_handle;
    uint32_t *fb_pixels; /* top-down 0xFFRRGGBB */
    int32_t   fb_w;
    int32_t   fb_h;
    bool      fb_dirty;
    char      cls_name[128];
    char      title[128];
} se_wnd_t;

typedef struct {
    uint64_t hwnd;
    uint32_t msg;
    uint64_t wParam;
    uint64_t lParam;
    uint32_t time;
    int32_t  pt_x;
    int32_t  pt_y;
} se_msg_item_t;

#define MSG_RING_CAP 256u

static pthread_mutex_t g_u32_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_msg_cv = PTHREAD_COND_INITIALIZER;

static se_wndclass_t   g_classes[MAX_WND_CLASSES];
static uint16_t        g_next_atom = 0xC001u;
static se_wnd_t        g_wnds[MAX_WND_HANDLES];
static uint64_t        g_next_aux_hwnd = SE_HWND_AUX0;

static gdi_obj_t       g_gdi_objs[MAX_GDI_OBJECTS];
static uint64_t        g_next_gdi_obj = GDI_OBJ_DYN_BASE;
static gdi_dc_t        g_gdi_dcs[MAX_GDI_DCS];
static uint64_t        g_next_gdi_dc = GDI_HDC_DYN_BASE;

static pthread_mutex_t g_splash_mu = PTHREAD_MUTEX_INITIALIZER;
static void           *g_splash_window = NULL;
static int             g_splash_surf_w = 0;
static int             g_splash_surf_h = 0;
static uint32_t       *g_last_splash_fb = NULL;
static int             g_last_splash_w = 0;
static int             g_last_splash_h = 0;

static void (*g_on_splash_vis_cb)(int visible, void *user) = NULL;
static void (*g_on_game_vis_cb)(void *user) = NULL;
static void  *g_ui_cb_user = NULL;

static se_msg_item_t   g_msg_ring[MSG_RING_CAP];
static uint32_t        g_msg_head = 0u;
static uint32_t        g_msg_tail = 0u;
static bool            g_quit_posted = false;
static uint32_t        g_quit_code = 0u;

static int32_t         g_cursor_x = 0;
static int32_t         g_cursor_y = 0;
static bool            g_lbtn_down = false;
static bool            g_rbtn_down = false;
static uint8_t         g_key_down[256];
static void           *g_native_window = NULL;

static uint32_t u32_tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L));
}

static int u32_strcasecmp_ascii(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : (unsigned char)*a;
        int cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : (unsigned char)*b;
        if (ca != cb) return ca - cb;
        ++a;
        ++b;
    }
    int ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : (unsigned char)*a;
    int cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : (unsigned char)*b;
    return ca - cb;
}

static void query_landscape_dims(uint32_t *out_w, uint32_t *out_h) {
    int w = 0, h = 0;
    se_host_get_surface_size(&w, &h);
    if (w <= 0 || h <= 0) {
        w = 2400;
        h = 1080;
    }
    if (w < h) {
        int tmp = w;
        w = h;
        h = tmp;
    }
    if (out_w) *out_w = (uint32_t)w;
    if (out_h) *out_h = (uint32_t)h;
}

static se_wnd_t *find_wnd_locked(uint64_t hwnd) {
    for (int i = 0; i < MAX_WND_HANDLES; ++i) {
        if (g_wnds[i].used && g_wnds[i].hwnd == hwnd) return &g_wnds[i];
    }
    return NULL;
}

static se_wndclass_t *find_class_locked(uint64_t name_or_atom) {
    if (!name_or_atom) return NULL;
    if ((name_or_atom >> 16) == 0ull) {
        uint16_t atom = (uint16_t)(name_or_atom & 0xFFFFu);
        for (int i = 0; i < MAX_WND_CLASSES; ++i) {
            if (g_classes[i].used && g_classes[i].atom == atom) return &g_classes[i];
        }
        return NULL;
    }
    char name[128];
    se_utf16_to_utf8((const uint16_t *)(uintptr_t)name_or_atom, -1, name, (int)sizeof(name));
    for (int i = 0; i < MAX_WND_CLASSES; ++i) {
        if (g_classes[i].used && u32_strcasecmp_ascii(g_classes[i].name, name) == 0) {
            return &g_classes[i];
        }
    }
    return NULL;
}

static void push_msg_locked(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int32_t x, int32_t y) {
    if (msg == WM_MOUSEMOVE && g_msg_head != g_msg_tail) {
        uint32_t prev = (g_msg_tail + MSG_RING_CAP - 1u) % MSG_RING_CAP;
        if (g_msg_ring[prev].hwnd == hwnd &&
            g_msg_ring[prev].msg == WM_MOUSEMOVE &&
            g_msg_ring[prev].wParam == wp) {
            g_msg_ring[prev].lParam = lp;
            g_msg_ring[prev].time = u32_tick_ms();
            g_msg_ring[prev].pt_x = x;
            g_msg_ring[prev].pt_y = y;
            pthread_cond_broadcast(&g_msg_cv);
            return;
        }
    }
    uint32_t next = (g_msg_tail + 1u) % MSG_RING_CAP;
    if (next == g_msg_head) {
        g_msg_head = (g_msg_head + 1u) % MSG_RING_CAP;
    }
    g_msg_ring[g_msg_tail].hwnd = hwnd;
    g_msg_ring[g_msg_tail].msg = msg;
    g_msg_ring[g_msg_tail].wParam = wp;
    g_msg_ring[g_msg_tail].lParam = lp;
    g_msg_ring[g_msg_tail].time = u32_tick_ms();
    g_msg_ring[g_msg_tail].pt_x = x;
    g_msg_ring[g_msg_tail].pt_y = y;
    g_msg_tail = next;
    pthread_cond_broadcast(&g_msg_cv);
}

static uint32_t gdi_colorref_to_xrgb(uint32_t cref) {
    uint32_t r = cref & 0xFFu;
    uint32_t g = (cref >> 8) & 0xFFu;
    uint32_t b = (cref >> 16) & 0xFFu;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static bool has_pending_paint_locked(uint64_t hwnd) {
    for (uint32_t idx = g_msg_head; idx != g_msg_tail; idx = (idx + 1u) % MSG_RING_CAP) {
        if (g_msg_ring[idx].hwnd == hwnd && g_msg_ring[idx].msg == WM_PAINT) {
            return true;
        }
    }
    return false;
}

static gdi_obj_t *find_gdi_obj_locked(uint64_t h) {
    if (h >= GDI_STOCK_OBJ_BASE && h < GDI_STOCK_OBJ_BASE + 16ull) {
        static gdi_obj_t stock_objs[16];
        static bool stock_init = false;
        if (!stock_init) {
            memset(stock_objs, 0, sizeof(stock_objs));
            for (int i = 0; i < 16; ++i) {
                stock_objs[i].used = true;
                stock_objs[i].handle = GDI_STOCK_OBJ_BASE + (uint64_t)i;
                stock_objs[i].kind = GDI_OBJ_FONT;
                stock_objs[i].font_height = 16;
            }
            stock_objs[0].kind = GDI_OBJ_BRUSH; stock_objs[0].color_xrgb = 0xFFFFFFFFu;
            stock_objs[1].kind = GDI_OBJ_BRUSH; stock_objs[1].color_xrgb = 0xFFC0C0C0u;
            stock_objs[2].kind = GDI_OBJ_BRUSH; stock_objs[2].color_xrgb = 0xFF404854u;
            stock_objs[3].kind = GDI_OBJ_BRUSH; stock_objs[3].color_xrgb = 0xFF20242Au;
            stock_objs[4].kind = GDI_OBJ_BRUSH; stock_objs[4].color_xrgb = 0xFF000000u;
            stock_objs[5].kind = GDI_OBJ_BRUSH; stock_objs[5].hollow = true;
            stock_objs[6].kind = GDI_OBJ_PEN;   stock_objs[6].color_xrgb = 0xFFFFFFFFu; stock_objs[6].pen_width = 1;
            stock_objs[7].kind = GDI_OBJ_PEN;   stock_objs[7].color_xrgb = 0xFF000000u; stock_objs[7].pen_width = 1;
            stock_objs[8].kind = GDI_OBJ_PEN;   stock_objs[8].hollow = true;
            stock_init = true;
        }
        return &stock_objs[h - GDI_STOCK_OBJ_BASE];
    }
    for (int i = 0; i < MAX_GDI_OBJECTS; ++i) {
        if (g_gdi_objs[i].used && g_gdi_objs[i].handle == h) {
            return &g_gdi_objs[i];
        }
    }
    return NULL;
}

static gdi_obj_t *alloc_gdi_obj_locked(gdi_obj_kind_t kind) {
    for (int i = 0; i < MAX_GDI_OBJECTS; ++i) {
        if (!g_gdi_objs[i].used) {
            memset(&g_gdi_objs[i], 0, sizeof(g_gdi_objs[i]));
            g_gdi_objs[i].used = true;
            g_gdi_objs[i].handle = g_next_gdi_obj++;
            g_gdi_objs[i].kind = kind;
            return &g_gdi_objs[i];
        }
    }
    return NULL;
}

static gdi_dc_t *find_gdi_dc_locked(uint64_t hdc) {
    for (int i = 0; i < MAX_GDI_DCS; ++i) {
        if (g_gdi_dcs[i].used && g_gdi_dcs[i].hdc == hdc) {
            return &g_gdi_dcs[i];
        }
    }
    return NULL;
}

static gdi_dc_t *alloc_gdi_dc_locked(uint64_t fixed_hdc, bool is_window_dc, uint64_t hwnd) {
    if (fixed_hdc) {
        gdi_dc_t *existing = find_gdi_dc_locked(fixed_hdc);
        if (existing) return existing;
    }
    for (int i = 0; i < MAX_GDI_DCS; ++i) {
        if (!g_gdi_dcs[i].used) {
            memset(&g_gdi_dcs[i], 0, sizeof(g_gdi_dcs[i]));
            g_gdi_dcs[i].used = true;
            g_gdi_dcs[i].hdc = fixed_hdc ? fixed_hdc : (g_next_gdi_dc++);
            g_gdi_dcs[i].is_window_dc = is_window_dc;
            g_gdi_dcs[i].hwnd = hwnd;
            g_gdi_dcs[i].sel_brush = GDI_STOCK_OBJ_BASE + 4ull; /* BLACK_BRUSH */
            g_gdi_dcs[i].sel_pen = GDI_STOCK_OBJ_BASE + 7ull;   /* BLACK_PEN */
            g_gdi_dcs[i].sel_font = GDI_STOCK_OBJ_BASE + 10ull;
            g_gdi_dcs[i].text_color_xrgb = 0xFFFFFFFFu;
            g_gdi_dcs[i].text_color_cref = 0x00FFFFFFu;
            g_gdi_dcs[i].bk_mode = 1; /* TRANSPARENT */
            return &g_gdi_dcs[i];
        }
    }
    return NULL;
}

static void ensure_wnd_fb_locked(se_wnd_t *w) {
    if (!w) return;
    int32_t tw = (w->width > 0) ? w->width : 640;
    int32_t th = (w->height > 0) ? w->height : 360;
    if (tw > 4096) tw = 4096;
    if (th > 4096) th = 4096;
    if (w->fb_pixels && w->fb_w == tw && w->fb_h == th) return;
    uint32_t *new_px = (uint32_t *)calloc((size_t)tw * (size_t)th, sizeof(uint32_t));
    if (!new_px) return;
    for (size_t i = 0; i < (size_t)tw * (size_t)th; ++i) {
        new_px[i] = 0xFF000000u;
    }
    free(w->fb_pixels);
    w->fb_pixels = new_px;
    w->fb_w = tw;
    w->fb_h = th;
}

static bool resolve_dc_pixels_locked(uint64_t hdc, uint32_t **out_px, int32_t *out_w,
                                     int32_t *out_h, bool *out_top_down, se_wnd_t **out_wnd) {
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    if (!dc) {
        if (hdc == SE_MAIN_HDC || hdc == SE_SPLASH_HDC) {
            dc = alloc_gdi_dc_locked(hdc, true, (hdc == SE_SPLASH_HDC) ? SE_HWND_SPLASH : SE_HWND_MAIN);
        }
        if (!dc) return false;
    }
    if (!dc->is_window_dc && dc->sel_bitmap) {
        gdi_obj_t *bmp = find_gdi_obj_locked(dc->sel_bitmap);
        if (bmp && bmp->kind == GDI_OBJ_BITMAP && bmp->pixels && bmp->width > 0 && bmp->height > 0) {
            if (out_px) *out_px = bmp->pixels;
            if (out_w) *out_w = bmp->width;
            if (out_h) *out_h = bmp->height;
            if (out_top_down) *out_top_down = bmp->top_down;
            if (out_wnd) *out_wnd = NULL;
            return true;
        }
    }
    if (dc->is_window_dc) {
        se_wnd_t *w = find_wnd_locked(dc->hwnd);
        if (w) {
            ensure_wnd_fb_locked(w);
            if (w->fb_pixels) {
                if (out_px) *out_px = w->fb_pixels;
                if (out_w) *out_w = w->fb_w;
                if (out_h) *out_h = w->fb_h;
                if (out_top_down) *out_top_down = true;
                if (out_wnd) *out_wnd = w;
                return true;
            }
        }
    }
    return false;
}

static void blit_to_splash_anw_locked(void) {
#if defined(__ANDROID__)
    ANativeWindow *anw = (ANativeWindow *)g_splash_window;
    const uint32_t *src = g_last_splash_fb;
    int sw = g_last_splash_w;
    int sh = g_last_splash_h;
    if (!anw || !src || sw <= 0 || sh <= 0) return;

    int32_t win_w = ANativeWindow_getWidth(anw);
    int32_t win_h = ANativeWindow_getHeight(anw);
    if (win_w <= 0) win_w = (g_splash_surf_w > 0) ? g_splash_surf_w : 2400;
    if (win_h <= 0) win_h = (g_splash_surf_h > 0) ? g_splash_surf_h : 1080;

    if (ANativeWindow_setBuffersGeometry(anw, win_w, win_h, WINDOW_FORMAT_RGBX_8888) != 0) {
        return;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(anw, &buf, NULL) != 0 || !buf.bits) {
        return;
    }

    int W = buf.width;
    int H = buf.height;
    int stride = buf.stride;
    uint32_t *dst = (uint32_t *)buf.bits;

    for (int y = 0; y < H; ++y) {
        uint32_t *row = dst + (size_t)y * (size_t)stride;
        for (int x = 0; x < W; ++x) {
            row[x] = 0xFF000000u;
        }
    }

    int dst_w, dst_h;
    if ((int64_t)sw * (int64_t)H >= (int64_t)W * (int64_t)sh) {
        dst_w = W;
        dst_h = (int)(((int64_t)W * (int64_t)sh) / sw);
    } else {
        dst_h = H;
        dst_w = (int)(((int64_t)H * (int64_t)sw) / sh);
    }
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (dst_w > W) dst_w = W;
    if (dst_h > H) dst_h = H;
    int dst_x = (W - dst_w) / 2;
    int dst_y = (H - dst_h) / 2;

    for (int y = 0; y < dst_h; ++y) {
        int sy = (int)(((int64_t)y * (int64_t)sh) / dst_h);
        if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + (size_t)sy * (size_t)sw;
        uint32_t *drow = dst + (size_t)(dst_y + y) * (size_t)stride + dst_x;
        for (int x = 0; x < dst_w; ++x) {
            int sx = (int)(((int64_t)x * (int64_t)sw) / dst_w);
            if (sx >= sw) sx = sw - 1;
            uint32_t bgra = srow[sx];
            uint32_t r = (bgra >> 16) & 0xFFu;
            uint32_t g = (bgra >> 8) & 0xFFu;
            uint32_t b = bgra & 0xFFu;
            drow[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
    ANativeWindow_unlockAndPost(anw);
#else
    (void)g_splash_window;
    (void)g_splash_surf_w;
    (void)g_splash_surf_h;
#endif
}

static void present_splash_fb_from_wnd(uint64_t hwnd) {
    uint32_t *copy = NULL;
    int w = 0, h = 0;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *wnd = find_wnd_locked(hwnd);
    if (wnd && wnd->is_splash && wnd->fb_pixels && wnd->fb_w > 0 && wnd->fb_h > 0) {
        w = wnd->fb_w;
        h = wnd->fb_h;
        size_t n = (size_t)w * (size_t)h;
        copy = (uint32_t *)malloc(n * sizeof(uint32_t));
        if (copy) {
            memcpy(copy, wnd->fb_pixels, n * sizeof(uint32_t));
            wnd->fb_dirty = false;
        }
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (!copy) return;
    pthread_mutex_lock(&g_splash_mu);
    free(g_last_splash_fb);
    g_last_splash_fb = copy;
    g_last_splash_w = w;
    g_last_splash_h = h;
    blit_to_splash_anw_locked();
    pthread_mutex_unlock(&g_splash_mu);
}

/* ========================================================================= */
/* Host Input / Window Wiring (called from engine_host.c)                    */
/* ========================================================================= */

void se_u32_reset_state(void) {
    pthread_mutex_lock(&g_u32_mu);
    for (int i = 0; i < MAX_WND_HANDLES; ++i) {
        free(g_wnds[i].fb_pixels);
    }
    for (int i = 0; i < MAX_GDI_OBJECTS; ++i) {
        if (g_gdi_objs[i].used && g_gdi_objs[i].kind == GDI_OBJ_BITMAP) {
            free(g_gdi_objs[i].pixels);
        }
    }
    memset(g_classes, 0, sizeof(g_classes));
    memset(g_wnds, 0, sizeof(g_wnds));
    memset(g_gdi_objs, 0, sizeof(g_gdi_objs));
    memset(g_gdi_dcs, 0, sizeof(g_gdi_dcs));
    memset(g_key_down, 0, sizeof(g_key_down));
    g_next_atom = 0xC001u;
    g_next_aux_hwnd = SE_HWND_AUX0;
    g_next_gdi_obj = GDI_OBJ_DYN_BASE;
    g_next_gdi_dc = GDI_HDC_DYN_BASE;
    g_msg_head = 0u;
    g_msg_tail = 0u;
    g_quit_posted = false;
    g_quit_code = 0u;
    g_lbtn_down = false;
    g_rbtn_down = false;
    pthread_mutex_unlock(&g_u32_mu);

    pthread_mutex_lock(&g_splash_mu);
    free(g_last_splash_fb);
    g_last_splash_fb = NULL;
    g_last_splash_w = 0;
    g_last_splash_h = 0;
    pthread_mutex_unlock(&g_splash_mu);
}

void se_u32_set_native_window(void *anw) {
    pthread_mutex_lock(&g_u32_mu);
    g_native_window = anw;
    if (anw) {
        uint32_t sw = 0, sh = 0;
        query_landscape_dims(&sw, &sh);
        se_wnd_t *main_w = find_wnd_locked(SE_HWND_MAIN);
        if (main_w) {
            main_w->width = (int32_t)sw;
            main_w->height = (int32_t)sh;
        }
        push_msg_locked(SE_HWND_MAIN, WM_SIZE, 0ull,
                        (uint64_t)(((uint32_t)sh << 16) | (sw & 0xFFFFu)),
                        (int32_t)(sw / 2u), (int32_t)(sh / 2u));
    }
    pthread_mutex_unlock(&g_u32_mu);
}

void *se_u32_get_native_window(void) {
    pthread_mutex_lock(&g_u32_mu);
    void *anw = g_native_window;
    pthread_mutex_unlock(&g_u32_mu);
    return anw;
}

void se_u32_set_splash_window(void *anw, int width, int height) {
    pthread_mutex_lock(&g_splash_mu);
    g_splash_window = anw;
    g_splash_surf_w = width;
    g_splash_surf_h = height;
    if (anw && g_last_splash_fb) {
        blit_to_splash_anw_locked();
    }
    pthread_mutex_unlock(&g_splash_mu);
}

void se_u32_set_ui_callbacks(
    void (*on_splash_vis)(int visible, void *user),
    void (*on_game_vis)(void *user),
    void *user) {
    pthread_mutex_lock(&g_u32_mu);
    g_on_splash_vis_cb = on_splash_vis;
    g_on_game_vis_cb = on_game_vis;
    g_ui_cb_user = user;
    pthread_mutex_unlock(&g_u32_mu);
}

uint64_t se_u32_get_main_hwnd(void) {
    return SE_HWND_MAIN;
}

uint64_t se_u32_get_splash_hwnd(void) {
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(SE_HWND_SPLASH);
    uint64_t res = (w && w->used) ? SE_HWND_SPLASH : 0ull;
    pthread_mutex_unlock(&g_u32_mu);
    return res;
}

const uint32_t *se_u32_get_splash_framebuffer(int *out_w, int *out_h) {
    pthread_mutex_lock(&g_splash_mu);
    if (out_w) *out_w = g_last_splash_w;
    if (out_h) *out_h = g_last_splash_h;
    const uint32_t *px = g_last_splash_fb;
    pthread_mutex_unlock(&g_splash_mu);
    return px;
}

/* action: 0=DOWN, 1=UP, 2=MOVE, 5/6=PINCH/ZOOM */
void se_u32_push_touch(int action, float x, float y, int pointer_count, float pinch_delta) {
    uint32_t sw = 0, sh = 0;
    query_landscape_dims(&sw, &sh);

    int32_t ix = (int32_t)x;
    int32_t iy = (int32_t)y;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if ((uint32_t)ix >= sw) ix = (int32_t)(sw - 1u);
    if ((uint32_t)iy >= sh) iy = (int32_t)(sh - 1u);

    uint64_t lparam = (uint64_t)(((uint32_t)(uint16_t)iy << 16) | (uint32_t)(uint16_t)ix);

    pthread_mutex_lock(&g_u32_mu);
    g_cursor_x = ix;
    g_cursor_y = iy;

    if (pointer_count >= 2 || (action == 2 && (pinch_delta > 0.001f || pinch_delta < -0.001f))) {
        int32_t wheel = (int32_t)(pinch_delta * 120.0f);
        if (wheel != 0) {
            uint64_t wp = (uint64_t)((uint32_t)((int16_t)wheel) << 16);
            push_msg_locked(SE_HWND_MAIN, WM_MOUSEWHEEL, wp, lparam, ix, iy);
        }
    }

    if (action == 0) {
        push_msg_locked(SE_HWND_MAIN, WM_MOUSEMOVE, 0ull, lparam, ix, iy);
        g_lbtn_down = true;
        push_msg_locked(SE_HWND_MAIN, WM_LBUTTONDOWN, 0x0001ull /* MK_LBUTTON */, lparam, ix, iy);
    } else if (action == 1) {
        g_lbtn_down = false;
        push_msg_locked(SE_HWND_MAIN, WM_LBUTTONUP, 0ull, lparam, ix, iy);
    } else if (action == 2) {
        uint64_t wp = (g_lbtn_down ? 0x0001ull : 0ull) | (g_rbtn_down ? 0x0002ull : 0ull);
        push_msg_locked(SE_HWND_MAIN, WM_MOUSEMOVE, wp, lparam, ix, iy);
    } else if (action == 3) {
        push_msg_locked(SE_HWND_MAIN, WM_MOUSEMOVE, 0ull, lparam, ix, iy);
        g_rbtn_down = true;
        push_msg_locked(SE_HWND_MAIN, WM_RBUTTONDOWN, 0x0002ull /* MK_RBUTTON */, lparam, ix, iy);
    } else if (action == 4) {
        g_rbtn_down = false;
        push_msg_locked(SE_HWND_MAIN, WM_RBUTTONUP, 0ull, lparam, ix, iy);
    }
    pthread_mutex_unlock(&g_u32_mu);
}

void se_u32_push_key(int key_code, bool down) {
    uint32_t vk = (uint32_t)key_code & 0xFFu;
    pthread_mutex_lock(&g_u32_mu);
    g_key_down[vk] = down ? 1u : 0u;
    uint32_t msg = down ? WM_KEYDOWN : WM_KEYUP;
    uint64_t lp = down ? 0x00000001ull : 0xC0000001ull;
    push_msg_locked(SE_HWND_MAIN, msg, (uint64_t)vk, lp, g_cursor_x, g_cursor_y);
    pthread_mutex_unlock(&g_u32_mu);
}

/* Deliver a typed Unicode character (soft keyboard) as a real WM_CHAR. */
void se_u32_push_char(uint32_t codepoint) {
    if (codepoint == 0u || codepoint > 0xFFFFu) return;
    pthread_mutex_lock(&g_u32_mu);
    push_msg_locked(SE_HWND_MAIN, WM_CHAR, (uint64_t)codepoint, 1ull, g_cursor_x, g_cursor_y);
    pthread_mutex_unlock(&g_u32_mu);
}

/* ========================================================================= */
/* 1. USER32.dll (58 imports)                                                */
/* ========================================================================= */

static void hle_u32_ret0(CPU *c) { HLE_RET64(c, 0ull); }
static void hle_u32_ret1(CPU *c) { HLE_RET64(c, 1ull); }

static void hle_u32_RegisterClass(CPU *c, bool is_ex) {
    uint64_t lpWndClass = HLE_ARG0(c);
    if (!lpWndClass) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t style = is_ex ? rd32(lpWndClass + 4ull) : rd32(lpWndClass + 0ull);
    uint64_t wndproc = rd64(lpWndClass + 8ull);
    uint64_t cls_ptr = is_ex ? rd64(lpWndClass + 64ull) : rd64(lpWndClass + 64ull);

    char cls_name[128];
    cls_name[0] = '\0';
    if (cls_ptr && (cls_ptr >> 16) != 0ull) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)cls_ptr, -1, cls_name, (int)sizeof(cls_name));
    }

    pthread_mutex_lock(&g_u32_mu);
    uint16_t atom = g_next_atom++;
    se_wndclass_t *slot = NULL;
    for (int i = 0; i < MAX_WND_CLASSES; ++i) {
        if (g_classes[i].used && cls_name[0] &&
            u32_strcasecmp_ascii(g_classes[i].name, cls_name) == 0) {
            slot = &g_classes[i];
            atom = slot->atom;
            break;
        }
        if (!g_classes[i].used && !slot) {
            slot = &g_classes[i];
        }
    }
    if (slot) {
        slot->used = true;
        slot->atom = atom;
        slot->wndproc_va = wndproc;
        slot->style = style;
        snprintf(slot->name, sizeof(slot->name), "%s", cls_name);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, (uint32_t)atom);
}

static void hle_u32_RegisterClassW(CPU *c) {
    hle_u32_RegisterClass(c, false);
}

static void hle_u32_RegisterClassExW(CPU *c) {
    hle_u32_RegisterClass(c, true);
}

static void hle_u32_UnregisterClassW(CPU *c) {
    uint64_t cls_arg = HLE_ARG0(c);
    pthread_mutex_lock(&g_u32_mu);
    se_wndclass_t *cls = find_class_locked(cls_arg);
    if (cls) cls->used = false;
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void fire_splash_visibility(int visible) {
    void (*cb)(int, void *) = NULL;
    void *user = NULL;
    pthread_mutex_lock(&g_u32_mu);
    cb = g_on_splash_vis_cb;
    user = g_ui_cb_user;
    pthread_mutex_unlock(&g_u32_mu);
    if (cb) cb(visible, user);
}

static void fire_game_surface_visible(void) {
    void (*cb)(void *) = NULL;
    void *user = NULL;
    pthread_mutex_lock(&g_u32_mu);
    cb = g_on_game_vis_cb;
    user = g_ui_cb_user;
    pthread_mutex_unlock(&g_u32_mu);
    if (cb) cb(user);
}

/* Notify guest WndProc that hwnd is active and focused so power-save throttling stays off. */
static void dispatch_window_activate_focus(CPU *c, uint64_t hwnd, uint64_t wndproc) {
    if (wndproc >= 0x140000000ull && wndproc < 0x142000000ull) {
        (void)se_hle_call_guest(c, wndproc, 4, hwnd, WM_ACTIVATE, 1ull, 0ull);
        (void)se_hle_call_guest(c, wndproc, 4, hwnd, WM_SETFOCUS, 0ull, 0ull);
    }
}

static void hle_u32_CreateWindowExW(CPU *c) {
    uint32_t exstyle = (uint32_t)HLE_ARG0(c);
    uint64_t cls_arg = HLE_ARG1(c);
    uint64_t title_arg = HLE_ARG2(c);
    uint32_t style = (uint32_t)HLE_ARG3(c);
    int32_t arg_x = (int32_t)(uint32_t)hle_stack_arg(c, 4u);
    int32_t arg_y = (int32_t)(uint32_t)hle_stack_arg(c, 5u);
    int32_t arg_w = (int32_t)(uint32_t)hle_stack_arg(c, 6u);
    int32_t arg_h = (int32_t)(uint32_t)hle_stack_arg(c, 7u);
    uint64_t hwnd_parent = hle_stack_arg(c, 8u);
    uint64_t hmenu = hle_stack_arg(c, 9u);
    uint64_t hinst = hle_stack_arg(c, 10u);
    uint64_t lp_param = hle_stack_arg(c, 11u);

    char cls_name[128];
    char title[128];
    cls_name[0] = '\0';
    title[0] = '\0';
    if (cls_arg && (cls_arg >> 16) != 0ull) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)cls_arg, -1, cls_name, (int)sizeof(cls_name));
    }
    if (title_arg) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)title_arg, -1, title, (int)sizeof(title));
    }

    bool is_splash = (u32_strcasecmp_ascii(cls_name, "SE Splash") == 0) ||
                     (u32_strcasecmp_ascii(title, "Banner") == 0);

    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);

    int32_t wx = (arg_x == (int32_t)0x80000000u) ? 0 : arg_x;
    int32_t wy = (arg_y == (int32_t)0x80000000u) ? 0 : arg_y;
    int32_t ww = (arg_w <= 0 || arg_w == (int32_t)0x80000000u) ? (is_splash ? 640 : (int32_t)lw) : arg_w;
    int32_t wh = (arg_h <= 0 || arg_h == (int32_t)0x80000000u) ? (is_splash ? 360 : (int32_t)lh) : arg_h;

    pthread_mutex_lock(&g_u32_mu);
    se_wndclass_t *cls = find_class_locked(cls_arg);
    uint64_t wndproc = cls ? cls->wndproc_va : 0ull;

    uint64_t hwnd = 0ull;
    if (is_splash) {
        hwnd = SE_HWND_SPLASH;
    } else if (!find_wnd_locked(SE_HWND_MAIN)) {
        hwnd = SE_HWND_MAIN;
    } else {
        hwnd = g_next_aux_hwnd++;
    }

    bool is_main = (hwnd == SE_HWND_MAIN);
    if (is_main) {
        wx = 0;
        wy = 0;
        ww = (int32_t)lw;
        wh = (int32_t)lh;
    }

    se_wnd_t *slot = find_wnd_locked(hwnd);
    if (!slot) {
        for (int i = 0; i < MAX_WND_HANDLES; ++i) {
            if (!g_wnds[i].used) {
                slot = &g_wnds[i];
                break;
            }
        }
    }
    if (slot) {
        free(slot->fb_pixels);
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->hwnd = hwnd;
        slot->wndproc_va = wndproc;
        slot->userdata = 0;
        slot->style = (int32_t)style;
        slot->exstyle = (int32_t)exstyle;
        slot->x = wx;
        slot->y = wy;
        slot->width = ww;
        slot->height = wh;
        slot->visible = ((style & 0x10000000u) != 0u);
        slot->is_splash = is_splash;
        slot->is_main = is_main;
        slot->dc_handle = is_splash ? SE_SPLASH_HDC : (is_main ? SE_MAIN_HDC : 0ull);
        if (slot->dc_handle) {
            alloc_gdi_dc_locked(slot->dc_handle, true, hwnd);
        }
        snprintf(slot->cls_name, sizeof(slot->cls_name), "%s", cls_name);
        snprintf(slot->title, sizeof(slot->title), "%s", title);
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (wndproc >= 0x140000000ull && wndproc < 0x142000000ull) {
        uint8_t *cs = (uint8_t *)calloc(1u, 80u);
        if (cs) {
            uint64_t cs_va = (uint64_t)(uintptr_t)cs;
            wr64(cs_va + 0ull,  lp_param);
            wr64(cs_va + 8ull,  hinst);
            wr64(cs_va + 16ull, hmenu);
            wr64(cs_va + 24ull, hwnd_parent);
            wr32(cs_va + 32ull, (uint32_t)wh);
            wr32(cs_va + 36ull, (uint32_t)ww);
            wr32(cs_va + 40ull, (uint32_t)wy);
            wr32(cs_va + 44ull, (uint32_t)wx);
            wr32(cs_va + 48ull, style);
            wr64(cs_va + 56ull, title_arg);
            wr64(cs_va + 64ull, cls_arg);
            wr32(cs_va + 72ull, exstyle);

            (void)se_hle_call_guest(c, wndproc, 4, hwnd, WM_NCCREATE, 0ull, cs_va);
            (void)se_hle_call_guest(c, wndproc, 4, hwnd, WM_CREATE, 0ull, cs_va);
            free(cs);
        }
        if (is_main) {
            (void)se_hle_call_guest(c, wndproc, 4, hwnd, WM_SIZE, 0ull,
                                    (uint64_t)(((uint32_t)lh << 16) | (lw & 0xFFFFu)));
            dispatch_window_activate_focus(c, hwnd, wndproc);
        }
    }

    if ((style & 0x10000000u) != 0u) {
        if (is_splash) fire_splash_visibility(1);
        else if (is_main) fire_game_surface_visible();
    }

    HLE_RET64(c, hwnd);
}

static void hle_u32_DestroyWindow(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t wndproc = 0;
    bool was_splash = false;
    uint64_t dc_h = 0;
    uint32_t *fb_to_free = NULL;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        wndproc = w->wndproc_va;
        was_splash = w->is_splash;
        dc_h = w->dc_handle;
        fb_to_free = w->fb_pixels;
        w->fb_pixels = NULL;
        w->used = false;
    }
    if (dc_h) {
        gdi_dc_t *dc = find_gdi_dc_locked(dc_h);
        if (dc) dc->used = false;
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (was_splash) {
        fire_splash_visibility(0);
    }
    if (wndproc >= 0x140000000ull && wndproc < 0x142000000ull) {
        (void)se_hle_call_guest(c, wndproc, 4, hwnd, WM_DESTROY, 0ull, 0ull);
    }
    free(fb_to_free);
    HLE_RET32(c, 1u);
}

static void hle_u32_FindWindowW(CPU *c) {
    uint64_t cls_arg = HLE_ARG0(c);
    uint64_t title_arg = HLE_ARG1(c);
    char cls_name[128], title[128];
    cls_name[0] = '\0';
    title[0] = '\0';
    if (cls_arg && (cls_arg >> 16) != 0ull) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)cls_arg, -1, cls_name, (int)sizeof(cls_name));
    }
    if (title_arg) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)title_arg, -1, title, (int)sizeof(title));
    }
    uint64_t found = 0ull;
    pthread_mutex_lock(&g_u32_mu);
    for (int i = 0; i < MAX_WND_HANDLES; ++i) {
        if (!g_wnds[i].used) continue;
        bool cls_ok = !cls_name[0] || u32_strcasecmp_ascii(g_wnds[i].cls_name, cls_name) == 0;
        bool tit_ok = !title[0] || u32_strcasecmp_ascii(g_wnds[i].title, title) == 0;
        if (cls_ok && tit_ok) {
            found = g_wnds[i].hwnd;
            break;
        }
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, found);
}

static void hle_u32_GetClientRect(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t lpRect = HLE_ARG1(c);
    if (!lpRect) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    uint32_t rw = lw, rh = lh;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w && !w->is_main && w->width > 0 && w->height > 0) {
        rw = (uint32_t)w->width;
        rh = (uint32_t)w->height;
    }
    pthread_mutex_unlock(&g_u32_mu);
    wr32(lpRect + 0ull, 0u);
    wr32(lpRect + 4ull, 0u);
    wr32(lpRect + 8ull, rw);
    wr32(lpRect + 12ull, rh);
    HLE_RET32(c, 1u);
}

static void hle_u32_GetWindowRect(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t lpRect = HLE_ARG1(c);
    if (!lpRect) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    int32_t rx = 0, ry = 0;
    int32_t rw = (int32_t)lw, rh = (int32_t)lh;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w && !w->is_main && w->width > 0 && w->height > 0) {
        rx = w->x;
        ry = w->y;
        rw = w->width;
        rh = w->height;
    }
    pthread_mutex_unlock(&g_u32_mu);
    wr32(lpRect + 0ull, (uint32_t)rx);
    wr32(lpRect + 4ull, (uint32_t)ry);
    wr32(lpRect + 8ull, (uint32_t)(rx + rw));
    wr32(lpRect + 12ull, (uint32_t)(ry + rh));
    HLE_RET32(c, 1u);
}

static void hle_u32_EnumDisplaySettingsA(CPU *c) {
    uint64_t lpDevMode = HLE_ARG2(c);
    if (!lpDevMode) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    wr32(lpDevMode + 40ull, 0x005C0000u); /* DM_BITSPERPEL|DM_PELSWIDTH|DM_PELSHEIGHT|DM_DISPLAYFREQUENCY */
    wr32(lpDevMode + 104ull, 32u);
    wr32(lpDevMode + 108ull, lw);
    wr32(lpDevMode + 112ull, lh);
    wr32(lpDevMode + 120ull, 60u);
    HLE_RET32(c, 1u);
}

static void hle_u32_GetMonitorInfoA(CPU *c) {
    uint64_t lpmi = HLE_ARG1(c);
    if (!lpmi || rd32(lpmi) < 40u) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    wr32(lpmi + 4ull, 0u);
    wr32(lpmi + 8ull, 0u);
    wr32(lpmi + 12ull, lw);
    wr32(lpmi + 16ull, lh);
    wr32(lpmi + 20ull, 0u);
    wr32(lpmi + 24ull, 0u);
    wr32(lpmi + 28ull, lw);
    wr32(lpmi + 32ull, lh);
    wr32(lpmi + 36ull, 1u); /* MONITORINFOF_PRIMARY */
    HLE_RET32(c, 1u);
}

static void hle_u32_EnumDisplayMonitors(CPU *c) {
    uint64_t lpfnEnum = HLE_ARG2(c);
    uint64_t dwData = HLE_ARG3(c);
    if (lpfnEnum) {
        uint32_t lw = 0, lh = 0;
        query_landscape_dims(&lw, &lh);
        static uint32_t g_mon_rect[4];
        g_mon_rect[0] = 0u;
        g_mon_rect[1] = 0u;
        g_mon_rect[2] = lw;
        g_mon_rect[3] = lh;
        (void)se_hle_call_guest(c, lpfnEnum, 4,
                                SE_HMONITOR0, 0ull,
                                (uint64_t)(uintptr_t)g_mon_rect, dwData);
    }
    HLE_RET32(c, 1u);
}

static void hle_u32_MonitorFromWindow(CPU *c) {
    (void)c;
    HLE_RET64(c, SE_HMONITOR0);
}

static void hle_u32_GetWindowPlacement(CPU *c) {
    uint64_t lpwndpl = HLE_ARG1(c);
    if (!lpwndpl) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    uint32_t len = rd32(lpwndpl);
    if (len == 0u) len = 44u;
    memset((void *)(uintptr_t)lpwndpl, 0, len < 64u ? len : 64u);
    wr32(lpwndpl + 0ull, len);
    wr32(lpwndpl + 8ull, 1u); /* SW_SHOWNORMAL */
    if (len >= 44u) {
        wr32(lpwndpl + 36ull, lw);
        wr32(lpwndpl + 40ull, lh);
    }
    HLE_RET32(c, 1u);
}

static void hle_u32_AdjustWindowRectExForDpi(CPU *c) {
    (void)c;
    /* Borderless fullscreen on Android Landscape: keep rect unchanged. */
    HLE_RET32(c, 1u);
}

static void hle_u32_GetWindowLongPtrW(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    int32_t idx = (int32_t)(uint32_t)HLE_ARG1(c);
    int64_t prev = 0;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        if (idx == -21) prev = w->userdata;
        else if (idx == -16) prev = w->style;
        else if (idx == -20) prev = w->exstyle;
        else if (idx == -4) prev = (int64_t)w->wndproc_va;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, (uint64_t)prev);
}

static void hle_u32_SetWindowLongPtrW(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    int32_t idx = (int32_t)(uint32_t)HLE_ARG1(c);
    int64_t val = (int64_t)HLE_ARG2(c);
    int64_t prev = 0;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        if (idx == -21) { prev = w->userdata; w->userdata = val; }
        else if (idx == -16) { prev = w->style; w->style = (int32_t)val; }
        else if (idx == -20) { prev = w->exstyle; w->exstyle = (int32_t)val; }
        else if (idx == -4) { prev = (int64_t)w->wndproc_va; w->wndproc_va = (uint64_t)val; }
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, (uint64_t)prev);
}

static void hle_u32_SetWindowTextW(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t txt = HLE_ARG1(c);
    if (txt) {
        char buf[128];
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)txt, -1, buf, (int)sizeof(buf));
        pthread_mutex_lock(&g_u32_mu);
        se_wnd_t *w = find_wnd_locked(hwnd);
        if (w) snprintf(w->title, sizeof(w->title), "%s", buf);
        pthread_mutex_unlock(&g_u32_mu);
    }
    HLE_RET32(c, 1u);
}

static void hle_u32_PeekMessageW(CPU *c) {
    uint64_t lpMsg = HLE_ARG0(c);
    uint64_t hWnd = HLE_ARG1(c);
    uint32_t wMsgFilterMin = (uint32_t)HLE_ARG2(c);
    uint32_t wMsgFilterMax = (uint32_t)HLE_ARG3(c);
    uint32_t wRemoveMsg = (uint32_t)HLE_ARG4(c);

    if (!lpMsg) {
        HLE_RET32(c, 0u);
        return;
    }

    se_msg_item_t item;
    bool got = false;
    pthread_mutex_lock(&g_u32_mu);
    for (uint32_t idx = g_msg_head; idx != g_msg_tail; idx = (idx + 1u) % MSG_RING_CAP) {
        se_msg_item_t cand = g_msg_ring[idx];
        bool hwnd_ok = (hWnd == 0ull || cand.hwnd == hWnd);
        bool range_ok = (wMsgFilterMin == 0u && wMsgFilterMax == 0u) ||
                        (cand.msg >= wMsgFilterMin && cand.msg <= wMsgFilterMax);
        if (hwnd_ok && range_ok) {
            item = cand;
            got = true;
            if (wRemoveMsg & PM_REMOVE) {
                uint32_t cur = idx;
                while (cur != g_msg_head) {
                    uint32_t prev = (cur == 0u) ? (MSG_RING_CAP - 1u) : (cur - 1u);
                    g_msg_ring[cur] = g_msg_ring[prev];
                    cur = prev;
                }
                g_msg_head = (g_msg_head + 1u) % MSG_RING_CAP;
            }
            break;
        }
    }
    if (!got && g_quit_posted) {
        item.hwnd = hWnd ? hWnd : SE_HWND_MAIN;
        item.msg = WM_QUIT;
        item.wParam = g_quit_code;
        item.lParam = 0ull;
        item.time = u32_tick_ms();
        item.pt_x = g_cursor_x;
        item.pt_y = g_cursor_y;
        got = true;
        if (wRemoveMsg & PM_REMOVE) {
            g_quit_posted = false;
        }
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (!got) {
        HLE_RET32(c, 0u);
        return;
    }

    wr64(lpMsg + 0ull, item.hwnd);
    wr32(lpMsg + 8ull, item.msg);
    wr32(lpMsg + 12ull, 0u);
    wr64(lpMsg + 16ull, item.wParam);
    wr64(lpMsg + 24ull, item.lParam);
    wr32(lpMsg + 32ull, item.time);
    wr32(lpMsg + 36ull, (uint32_t)item.pt_x);
    wr32(lpMsg + 40ull, (uint32_t)item.pt_y);
    wr32(lpMsg + 44ull, 0u);
    HLE_RET32(c, 1u);
}

static void hle_u32_TranslateMessage(CPU *c) {
    uint64_t lpMsg = HLE_ARG0(c);
    if (!lpMsg) {
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t msg = rd32(lpMsg + 8ull);
    if (msg == WM_KEYDOWN) {
        uint64_t vk = rd64(lpMsg + 16ull);
        if (vk >= 0x20ull && vk <= 0x7Eull) {
            pthread_mutex_lock(&g_u32_mu);
            push_msg_locked(rd64(lpMsg + 0ull), WM_CHAR, vk, 1ull,
                            (int32_t)rd32(lpMsg + 36ull), (int32_t)rd32(lpMsg + 40ull));
            pthread_mutex_unlock(&g_u32_mu);
            HLE_RET32(c, 1u);
            return;
        }
    }
    HLE_RET32(c, 0u);
}

static void hle_u32_DispatchMessageW(CPU *c) {
    uint64_t lpMsg = HLE_ARG0(c);
    if (!lpMsg) {
        HLE_RET64(c, 0ull);
        return;
    }
    uint64_t hwnd = rd64(lpMsg + 0ull);
    uint32_t msg = rd32(lpMsg + 8ull);
    uint64_t wp = rd64(lpMsg + 16ull);
    uint64_t lp = rd64(lpMsg + 24ull);

    uint64_t wndproc = 0ull;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) wndproc = w->wndproc_va;
    pthread_mutex_unlock(&g_u32_mu);

    uint64_t res = 0ull;
    if (wndproc >= 0x140000000ull && wndproc < 0x142000000ull) {
        res = se_hle_call_guest(c, wndproc, 4, hwnd, (uint64_t)msg, wp, lp);
    }
    HLE_RET64(c, res);
}

static void hle_u32_SendMessageW(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint32_t msg = (uint32_t)HLE_ARG1(c);
    uint64_t wp = HLE_ARG2(c);
    uint64_t lp = HLE_ARG3(c);

    uint64_t wndproc = 0ull;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) wndproc = w->wndproc_va;
    pthread_mutex_unlock(&g_u32_mu);

    uint64_t res = 0ull;
    if (wndproc >= 0x140000000ull && wndproc < 0x142000000ull) {
        res = se_hle_call_guest(c, wndproc, 4, hwnd, (uint64_t)msg, wp, lp);
    }
    HLE_RET64(c, res);
}

static void hle_u32_DefWindowProcW(CPU *c) {
    uint32_t msg = (uint32_t)HLE_ARG1(c);
    if (msg == WM_NCCREATE || msg == WM_ERASEBKGND) {
        HLE_RET64(c, 1ull);
        return;
    }
    if (msg == WM_CLOSE) {
        HLE_RET64(c, 0ull);
        return;
    }
    HLE_RET64(c, 0ull);
}

static void hle_u32_WaitMessage(CPU *c) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 16000000L; /* wake at least at ~60 Hz so frame/state polling stays live */
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_u32_mu);
    if (g_msg_head == g_msg_tail && !g_quit_posted) {
        pthread_cond_timedwait(&g_msg_cv, &g_u32_mu, &ts);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_u32_PostQuitMessage(CPU *c) {
    uint32_t code = (uint32_t)HLE_ARG0(c);
    pthread_mutex_lock(&g_u32_mu);
    g_quit_posted = true;
    g_quit_code = code;
    pthread_cond_broadcast(&g_msg_cv);
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, 0ull);
}

static void hle_u32_ShowWindow(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    int32_t cmd = (int32_t)(uint32_t)HLE_ARG1(c);
    bool vis = (cmd != 0);
    bool prev_vis = false;
    bool is_splash = false;
    bool is_main = false;
    uint64_t wndproc = 0ull;

    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        prev_vis = w->visible;
        w->visible = vis;
        is_splash = w->is_splash;
        is_main = w->is_main;
        wndproc = w->wndproc_va;
        if (vis && !has_pending_paint_locked(hwnd)) {
            push_msg_locked(hwnd, WM_PAINT, 0ull, 0ull, 0, 0);
        }
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (is_splash) {
        fire_splash_visibility(vis ? 1 : 0);
        if (vis) present_splash_fb_from_wnd(hwnd);
    } else if (is_main && vis) {
        fire_game_surface_visible();
        dispatch_window_activate_focus(c, hwnd, wndproc);
    }
    HLE_RET32(c, prev_vis ? 1u : 0u);
}

static void hle_u32_SetWindowPos(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    int32_t x = (int32_t)(uint32_t)HLE_ARG2(c);
    int32_t y = (int32_t)(uint32_t)HLE_ARG3(c);
    int32_t cx = (int32_t)(uint32_t)hle_stack_arg(c, 4u);
    int32_t cy = (int32_t)(uint32_t)hle_stack_arg(c, 5u);
    uint32_t flags = (uint32_t)hle_stack_arg(c, 6u);

    bool fire_splash = false;
    int splash_vis = 0;
    bool fire_main = false;

    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        if (!w->is_main) {
            if ((flags & 0x0002u) == 0u) {
                w->x = x;
                w->y = y;
            }
            if ((flags & 0x0001u) == 0u && cx > 0 && cy > 0) {
                w->width = cx;
                w->height = cy;
                if (w->fb_pixels) ensure_wnd_fb_locked(w);
            }
        }
        if (flags & 0x0040u) {
            w->visible = true;
            if (w->is_splash) { fire_splash = true; splash_vis = 1; }
            else if (w->is_main) { fire_main = true; }
        } else if (flags & 0x0080u) {
            w->visible = false;
            if (w->is_splash) { fire_splash = true; splash_vis = 0; }
        }
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (fire_splash) fire_splash_visibility(splash_vis);
    if (fire_main) fire_game_surface_visible();
    HLE_RET32(c, 1u);
}

static void hle_u32_InvalidateRect(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w && !has_pending_paint_locked(hwnd)) {
        push_msg_locked(hwnd, WM_PAINT, 0ull, 0ull, 0, 0);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_u32_GetDC(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t dc_h = SE_MAIN_HDC;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        if (!w->dc_handle) {
            gdi_dc_t *dc = alloc_gdi_dc_locked(0ull, true, hwnd);
            if (dc) w->dc_handle = dc->hdc;
        }
        if (w->dc_handle) dc_h = w->dc_handle;
        ensure_wnd_fb_locked(w);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, dc_h);
}

static void hle_u32_ReleaseDC(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t hdc = HLE_ARG1(c);
    bool should_present = false;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (!w && hdc) {
        gdi_dc_t *dc = find_gdi_dc_locked(hdc);
        if (dc && dc->is_window_dc) {
            w = find_wnd_locked(dc->hwnd);
            if (w) hwnd = w->hwnd;
        }
    }
    if (w && w->is_splash && w->fb_dirty) {
        should_present = true;
    }
    pthread_mutex_unlock(&g_u32_mu);
    if (should_present) {
        present_splash_fb_from_wnd(hwnd);
    }
    HLE_RET32(c, 1u);
}

static void hle_u32_BeginPaint(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t lpPaint = HLE_ARG1(c);
    uint64_t dc_h = SE_MAIN_HDC;
    int32_t pw = 0, ph = 0;

    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) {
        if (!w->dc_handle) {
            gdi_dc_t *dc = alloc_gdi_dc_locked(0ull, true, hwnd);
            if (dc) w->dc_handle = dc->hdc;
        }
        if (w->dc_handle) dc_h = w->dc_handle;
        ensure_wnd_fb_locked(w);
        pw = w->fb_w;
        ph = w->fb_h;
    } else {
        uint32_t lw = 0, lh = 0;
        query_landscape_dims(&lw, &lh);
        pw = (int32_t)lw;
        ph = (int32_t)lh;
    }
    pthread_mutex_unlock(&g_u32_mu);

    if (lpPaint) {
        memset((void *)(uintptr_t)lpPaint, 0, 72u);
        wr64(lpPaint + 0ull, dc_h);
        wr32(lpPaint + 8ull, 1u);
        wr32(lpPaint + 12ull, 0u);
        wr32(lpPaint + 16ull, 0u);
        wr32(lpPaint + 20ull, (uint32_t)pw);
        wr32(lpPaint + 24ull, (uint32_t)ph);
    }
    HLE_RET64(c, dc_h);
}

static void hle_u32_EndPaint(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    bool is_splash = false;
    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w && w->is_splash) is_splash = true;
    pthread_mutex_unlock(&g_u32_mu);
    if (is_splash) {
        present_splash_fb_from_wnd(hwnd);
    }
    HLE_RET32(c, 1u);
}

static void hle_u32_GetWindowThreadProcessId(CPU *c) {
    uint64_t lpdwProcessId = HLE_ARG1(c);
    if (lpdwProcessId) wr32(lpdwProcessId, 1000u);
    HLE_RET32(c, se_win64_teb_get_tid());
}

static void hle_u32_GetSystemMetrics(CPU *c) {
    int32_t idx = (int32_t)(uint32_t)HLE_ARG0(c);
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    if (idx == 0) HLE_RET32(c, lw);      /* SM_CXSCREEN */
    else if (idx == 1) HLE_RET32(c, lh); /* SM_CYSCREEN */
    else HLE_RET32(c, 0u);
}

static void hle_u32_GetCursorPos(CPU *c) {
    uint64_t lpPoint = HLE_ARG0(c);
    if (!lpPoint) {
        HLE_RET32(c, 0u);
        return;
    }
    pthread_mutex_lock(&g_u32_mu);
    int32_t x = g_cursor_x;
    int32_t y = g_cursor_y;
    pthread_mutex_unlock(&g_u32_mu);
    wr32(lpPoint + 0ull, (uint32_t)x);
    wr32(lpPoint + 4ull, (uint32_t)y);
    HLE_RET32(c, 1u);
}

static void hle_u32_SetCursorPos(CPU *c) {
    int32_t x = (int32_t)(uint32_t)HLE_ARG0(c);
    int32_t y = (int32_t)(uint32_t)HLE_ARG1(c);
    pthread_mutex_lock(&g_u32_mu);
    g_cursor_x = x;
    g_cursor_y = y;
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_u32_GetKeyState(CPU *c) {
    uint32_t vk = (uint32_t)HLE_ARG0(c) & 0xFFu;
    uint16_t state = 0u;
    pthread_mutex_lock(&g_u32_mu);
    if (g_key_down[vk]) state = 0x8001u;
    else if (vk == 0x01u && g_lbtn_down) state = 0x8001u;
    else if (vk == 0x02u && g_rbtn_down) state = 0x8001u;
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, (uint32_t)state);
}

static void hle_u32_LoadHandle(CPU *c) {
    (void)c;
    HLE_RET64(c, 0x00030001ull);
}

static void hle_u32_MessageBoxW(CPU *c) {
    char text[512], cap[256];
    text[0] = '\0';
    cap[0] = '\0';
    if (HLE_ARG1(c)) se_utf16_to_utf8((const uint16_t *)(uintptr_t)HLE_ARG1(c), -1, text, (int)sizeof(text));
    if (HLE_ARG2(c)) se_utf16_to_utf8((const uint16_t *)(uintptr_t)HLE_ARG2(c), -1, cap, (int)sizeof(cap));
    uint32_t uType = (uint32_t)HLE_ARG3(c) & 0x0Fu;
    /* Confirm MB_YESNO / MB_YESNOCANCEL prompts (e.g. "Run anyway?") with IDYES (6). */
    uint32_t ret = (uType == 4u || uType == 3u) ? 6u : 1u;
    U32_LOG("MessageBoxW [uType=0x%x -> %u] [%s]: %s", (uint32_t)HLE_ARG3(c), ret, cap, text);
    HLE_RET32(c, ret);
}

static uint64_t u32_va_arg(const CPU *c, int arg_idx) {
    if (arg_idx == 2) return c->r8;
    if (arg_idx == 3) return c->r9;
    return hle_stack_arg(c, (uint32_t)arg_idx);
}

static void hle_u32_wsprintfW(CPU *c) {
    uint64_t lpOut = HLE_ARG0(c);
    uint64_t lpFmt = HLE_ARG1(c);
    if (!lpOut || !lpFmt) {
        HLE_RET32(c, 0u);
        return;
    }
    char fmt[512];
    se_utf16_to_utf8((const uint16_t *)(uintptr_t)lpFmt, -1, fmt, (int)sizeof(fmt));

    char out[1024];
    size_t pos = 0;
    int argi = 2;
    for (size_t i = 0; fmt[i] && pos + 2 < sizeof(out); ++i) {
        if (fmt[i] != '%') {
            out[pos++] = fmt[i];
            continue;
        }
        ++i;
        if (fmt[i] == '\0') break;
        if (fmt[i] == '%') {
            out[pos++] = '%';
            continue;
        }
        char spec_buf[32];
        size_t s = 0;
        spec_buf[s++] = '%';
        while (fmt[i] && s + 4 < sizeof(spec_buf) &&
               !((fmt[i] >= 'a' && fmt[i] <= 'z') || (fmt[i] >= 'A' && fmt[i] <= 'Z'))) {
            spec_buf[s++] = fmt[i++];
        }
        char conv = fmt[i];
        if (!conv) break;

        uint64_t val = u32_va_arg(c, argi++);
        char tmp[256];
        tmp[0] = '\0';
        if (conv == 's' || conv == 'S') {
            if (conv == 's') {
                if (val) se_utf16_to_utf8((const uint16_t *)(uintptr_t)val, -1, tmp, (int)sizeof(tmp));
            } else {
                if (val) snprintf(tmp, sizeof(tmp), "%s", (const char *)(uintptr_t)val);
            }
        } else if (conv == 'd' || conv == 'i') {
            spec_buf[s++] = conv;
            spec_buf[s] = '\0';
            snprintf(tmp, sizeof(tmp), spec_buf, (int32_t)(uint32_t)val);
        } else if (conv == 'u' || conv == 'x' || conv == 'X') {
            spec_buf[s++] = conv;
            spec_buf[s] = '\0';
            snprintf(tmp, sizeof(tmp), spec_buf, (uint32_t)val);
        } else if (conv == 'p') {
            snprintf(tmp, sizeof(tmp), "%llx", (unsigned long long)val);
        } else if (conv == 'c' || conv == 'C') {
            tmp[0] = (char)(val & 0x7Fu);
            tmp[1] = '\0';
        }
        for (size_t k = 0; tmp[k] && pos + 2 < sizeof(out); ++k) {
            out[pos++] = tmp[k];
        }
    }
    out[pos] = '\0';

    int wchars = se_utf8_to_utf16(out, -1, (uint16_t *)(uintptr_t)lpOut, 1024);
    HLE_RET32(c, (wchars > 0) ? (uint32_t)(wchars - 1) : 0u);
}

static const uint8_t SE_FONT5X7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}, {0x00,0x00,0x00,0x00,0x00}
};

static inline void fb_put_px(uint32_t *px, int32_t w, int32_t h, bool top_down, int32_t x, int32_t y, uint32_t xrgb) {
    if (!px || x < 0 || y < 0 || x >= w || y >= h) return;
    int32_t row = top_down ? y : (h - 1 - y);
    px[(size_t)row * (size_t)w + (size_t)x] = xrgb;
}

static inline uint32_t fb_get_px(const uint32_t *px, int32_t w, int32_t h, bool top_down, int32_t x, int32_t y) {
    if (!px || x < 0 || y < 0 || x >= w || y >= h) return 0xFF000000u;
    int32_t row = top_down ? y : (h - 1 - y);
    return px[(size_t)row * (size_t)w + (size_t)x];
}

static void hle_u32_DrawTextW(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    uint64_t lpchText = HLE_ARG1(c);
    int32_t cchText = (int32_t)(uint32_t)HLE_ARG2(c);
    uint64_t lprc = HLE_ARG3(c);
    uint32_t format = (uint32_t)hle_stack_arg(c, 4u);

    if (!lpchText || !lprc) {
        HLE_RET32(c, 0u);
        return;
    }

    char utf8[512];
    int len = se_utf16_to_utf8((const uint16_t *)(uintptr_t)lpchText, cchText, utf8, (int)sizeof(utf8));
    if (len < 0) len = 0;

    int32_t left = (int32_t)rd32(lprc + 0ull);
    int32_t top = (int32_t)rd32(lprc + 4ull);
    int32_t right = (int32_t)rd32(lprc + 8ull);
    int32_t bottom = (int32_t)rd32(lprc + 12ull);

    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    int32_t fh = 16;
    uint32_t col = 0xFFFFFFFFu;
    if (dc) {
        col = dc->text_color_xrgb;
        gdi_obj_t *f = find_gdi_obj_locked(dc->sel_font);
        if (f && f->kind == GDI_OBJ_FONT && f->font_height > 0) {
            fh = f->font_height;
        }
    }
    int32_t scale = fh / 8;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    int32_t char_w = 6 * scale;
    int32_t char_h = 8 * scale;
    int32_t text_w = (int32_t)len * char_w;

    if (format & 0x0400u /* DT_CALCRECT */) {
        wr32(lprc + 8ull, (uint32_t)(left + text_w));
        wr32(lprc + 12ull, (uint32_t)(top + char_h));
        pthread_mutex_unlock(&g_u32_mu);
        HLE_RET32(c, (uint32_t)char_h);
        return;
    }

    uint32_t *px = NULL;
    int32_t dw = 0, dh = 0;
    bool top_down = true;
    se_wnd_t *wnd = NULL;
    if (resolve_dc_pixels_locked(hdc, &px, &dw, &dh, &top_down, &wnd)) {
        int32_t start_x = left;
        int32_t start_y = top;
        int32_t box_w = right - left;
        int32_t box_h = bottom - top;
        if (format & 0x0001u /* DT_CENTER */) start_x = left + (box_w - text_w) / 2;
        else if (format & 0x0002u /* DT_RIGHT */) start_x = right - text_w;
        if ((format & 0x0024u) == 0x0024u /* DT_VCENTER|DT_SINGLELINE */) {
            start_y = top + (box_h - char_h) / 2;
        }

        for (int i = 0; i < len; ++i) {
            unsigned char ch = (unsigned char)utf8[i];
            if (ch < 0x20u || ch > 0x7Fu) ch = '?';
            const uint8_t *glyph = SE_FONT5X7[ch - 0x20u];
            int32_t gx = start_x + i * char_w;
            for (int col_i = 0; col_i < 5; ++col_i) {
                uint8_t bits = glyph[col_i];
                for (int row_i = 0; row_i < 7; ++row_i) {
                    if (bits & (1u << row_i)) {
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                int32_t px_x = gx + col_i * scale + sx;
                                int32_t px_y = start_y + row_i * scale + sy;
                                if (px_x >= left && px_x < right && px_y >= top && px_y < bottom) {
                                    fb_put_px(px, dw, dh, top_down, px_x, px_y, col);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (wnd && wnd->is_splash) wnd->fb_dirty = true;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, (uint32_t)char_h);
}

/* ========================================================================= */
/* 2. GDI32.dll (20 imports)                                                 */
/* ========================================================================= */

static void hle_gdi_ChoosePixelFormat(CPU *c) {
    (void)c;
    HLE_RET32(c, 1u);
}

static void hle_gdi_SetPixelFormat(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    int32_t iPixelFormat = (int32_t)(uint32_t)HLE_ARG1(c);
    uint32_t res = 0u;

    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    uint64_t target_hwnd = 0ull;
    if (dc && dc->is_window_dc) {
        target_hwnd = dc->hwnd;
    } else if (hdc == SE_MAIN_HDC) {
        target_hwnd = SE_HWND_MAIN;
    } else if (hdc == SE_SPLASH_HDC) {
        target_hwnd = SE_HWND_SPLASH;
    }

    se_wnd_t *w = target_hwnd ? find_wnd_locked(target_hwnd) : NULL;
    if (w) {
        /* Win32 allows SetPixelFormat only once per window; a second call returns FALSE. */
        if (!w->pixel_format_set) {
            w->pixel_format_set = true;
            w->pixel_format = (iPixelFormat > 0) ? iPixelFormat : 1;
            res = 1u;
        }
    } else if (dc) {
        if (!dc->pixel_format_set) {
            dc->pixel_format_set = true;
            dc->pixel_format = (iPixelFormat > 0) ? iPixelFormat : 1;
            res = 1u;
        }
    }
    pthread_mutex_unlock(&g_u32_mu);

    HLE_RET32(c, res);
}

static void hle_gdi_DescribePixelFormat(CPU *c) {
    uint32_t nBytes = (uint32_t)HLE_ARG2(c);
    uint64_t ppfd = HLE_ARG3(c);
    if (ppfd && nBytes >= 40u) {
        memset((void *)(uintptr_t)ppfd, 0, 40u);
        wr16(ppfd + 0ull, 40u);
        wr16(ppfd + 2ull, 1u);
        wr32(ppfd + 4ull, 0x25u);
        wr8(ppfd + 8ull, 0u);
        wr8(ppfd + 9ull, 32u);
        wr8(ppfd + 23ull, 24u);
        wr8(ppfd + 24ull, 8u);
    }
    HLE_RET32(c, 1u);
}

static void hle_gdi_GetDeviceCaps(CPU *c) {
    int32_t idx = (int32_t)(uint32_t)HLE_ARG1(c);
    uint32_t lw = 0, lh = 0;
    query_landscape_dims(&lw, &lh);
    switch (idx) {
    case 8:  HLE_RET32(c, lw); break;  /* HORZRES */
    case 10: HLE_RET32(c, lh); break;  /* VERTRES */
    case 12: HLE_RET32(c, 32u); break; /* BITSPIXEL */
    case 88:
    case 90: HLE_RET32(c, 96u); break; /* LOGPIXELSX/Y */
    default: HLE_RET32(c, 1u); break;
    }
}

static void hle_gdi_SetBkMode(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    int32_t mode = (int32_t)(uint32_t)HLE_ARG1(c);
    int32_t prev = 1;
    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    if (dc) {
        prev = dc->bk_mode;
        dc->bk_mode = mode;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, (uint32_t)prev);
}

static void hle_gdi_SetTextColor(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    uint32_t cref = (uint32_t)HLE_ARG1(c);
    uint32_t prev = 0u;
    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    if (dc) {
        prev = dc->text_color_cref;
        dc->text_color_cref = cref;
        dc->text_color_xrgb = gdi_colorref_to_xrgb(cref);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, prev);
}

static void hle_gdi_GetStockObject(CPU *c) {
    int32_t id = (int32_t)(uint32_t)HLE_ARG0(c);
    if (id < 0 || id >= 16) id = 0;
    HLE_RET64(c, GDI_STOCK_OBJ_BASE + (uint64_t)(uint32_t)id);
}

static void hle_gdi_CreateCompatibleDC(CPU *c) {
    uint64_t hdc_ref = HLE_ARG0(c);
    (void)hdc_ref;
    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = alloc_gdi_dc_locked(0ull, false, 0ull);
    uint64_t res = dc ? dc->hdc : 0ull;
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, res);
}

static void hle_gdi_DeleteDC(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    if (dc && hdc >= GDI_HDC_DYN_BASE) {
        dc->used = false;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_gdi_SelectObject(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    uint64_t hobj = HLE_ARG1(c);
    uint64_t prev = 0ull;
    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    gdi_obj_t *obj = find_gdi_obj_locked(hobj);
    if (dc && obj) {
        switch (obj->kind) {
        case GDI_OBJ_BITMAP: prev = dc->sel_bitmap; dc->sel_bitmap = hobj; break;
        case GDI_OBJ_BRUSH:  prev = dc->sel_brush;  dc->sel_brush = hobj;  break;
        case GDI_OBJ_PEN:    prev = dc->sel_pen;    dc->sel_pen = hobj;    break;
        case GDI_OBJ_FONT:   prev = dc->sel_font;   dc->sel_font = hobj;   break;
        default: break;
        }
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, prev);
}

static void hle_gdi_CreateSolidBrush(CPU *c) {
    uint32_t cref = (uint32_t)HLE_ARG0(c);
    pthread_mutex_lock(&g_u32_mu);
    gdi_obj_t *obj = alloc_gdi_obj_locked(GDI_OBJ_BRUSH);
    uint64_t h = 0ull;
    if (obj) {
        obj->color_xrgb = gdi_colorref_to_xrgb(cref);
        h = obj->handle;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, h);
}

static void hle_gdi_CreatePen(CPU *c) {
    int32_t style = (int32_t)(uint32_t)HLE_ARG0(c);
    int32_t width = (int32_t)(uint32_t)HLE_ARG1(c);
    uint32_t cref = (uint32_t)HLE_ARG2(c);
    pthread_mutex_lock(&g_u32_mu);
    gdi_obj_t *obj = alloc_gdi_obj_locked(GDI_OBJ_PEN);
    uint64_t h = 0ull;
    if (obj) {
        obj->hollow = (style == 5 /* PS_NULL */);
        obj->pen_width = (width > 0) ? width : 1;
        obj->color_xrgb = gdi_colorref_to_xrgb(cref);
        h = obj->handle;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, h);
}

static void hle_gdi_CreateFontIndirectW(CPU *c) {
    uint64_t lplf = HLE_ARG0(c);
    int32_t h = 16;
    if (lplf) {
        int32_t lf_h = (int32_t)rd32(lplf + 0ull);
        if (lf_h < 0) lf_h = -lf_h;
        if (lf_h > 0 && lf_h <= 128) h = lf_h;
    }
    pthread_mutex_lock(&g_u32_mu);
    gdi_obj_t *obj = alloc_gdi_obj_locked(GDI_OBJ_FONT);
    uint64_t res = 0ull;
    if (obj) {
        obj->font_height = h;
        res = obj->handle;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, res);
}

static void hle_gdi_CreateBitmap(CPU *c) {
    int32_t w = (int32_t)(uint32_t)HLE_ARG0(c);
    int32_t h = (int32_t)(uint32_t)HLE_ARG1(c);
    uint32_t planes = (uint32_t)HLE_ARG2(c);
    uint32_t bpp = (uint32_t)HLE_ARG3(c);
    uint64_t lpvBits = hle_stack_arg(c, 4u);
    (void)planes;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;
    uint32_t *px = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (px && lpvBits && bpp == 32u) {
        memcpy(px, (const void *)(uintptr_t)lpvBits, (size_t)w * (size_t)h * 4u);
    }
    pthread_mutex_lock(&g_u32_mu);
    gdi_obj_t *obj = px ? alloc_gdi_obj_locked(GDI_OBJ_BITMAP) : NULL;
    uint64_t res = 0ull;
    if (obj) {
        obj->width = w;
        obj->height = h;
        obj->top_down = true;
        obj->pixels = px;
        res = obj->handle;
    } else {
        free(px);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, res);
}

static void hle_gdi_CreateDIBSection(CPU *c) {
    uint64_t lpbmi = HLE_ARG1(c);
    uint64_t ppvBits = HLE_ARG3(c);
    int32_t w = 32, h = 32;
    bool top_down = false;
    if (lpbmi) {
        int32_t bw = (int32_t)rd32(lpbmi + 4ull);
        int32_t bh = (int32_t)rd32(lpbmi + 8ull);
        if (bw < 0) bw = -bw;
        if (bh < 0) { top_down = true; bh = -bh; }
        if (bw > 0 && bw <= 4096) w = bw;
        if (bh > 0 && bh <= 4096) h = bh;
    }
    uint32_t *bits = (uint32_t *)calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (ppvBits) wr64(ppvBits, (uint64_t)(uintptr_t)bits);

    pthread_mutex_lock(&g_u32_mu);
    gdi_obj_t *obj = bits ? alloc_gdi_obj_locked(GDI_OBJ_BITMAP) : NULL;
    uint64_t res = 0ull;
    if (obj) {
        obj->width = w;
        obj->height = h;
        obj->top_down = top_down;
        obj->pixels = bits;
        res = obj->handle;
    } else {
        free(bits);
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET64(c, res);
}

static void hle_gdi_DeleteObject(CPU *c) {
    uint64_t hobj = HLE_ARG0(c);
    if (hobj >= GDI_OBJ_DYN_BASE && hobj < GDI_STOCK_OBJ_BASE) {
        uint32_t *px_free = NULL;
        pthread_mutex_lock(&g_u32_mu);
        gdi_obj_t *obj = find_gdi_obj_locked(hobj);
        if (obj) {
            if (obj->kind == GDI_OBJ_BITMAP) px_free = obj->pixels;
            obj->pixels = NULL;
            obj->used = false;
        }
        pthread_mutex_unlock(&g_u32_mu);
        free(px_free);
    }
    HLE_RET32(c, 1u);
}

static void hle_gdi_Rectangle(CPU *c) {
    uint64_t hdc = HLE_ARG0(c);
    int32_t left = (int32_t)(uint32_t)HLE_ARG1(c);
    int32_t top = (int32_t)(uint32_t)HLE_ARG2(c);
    int32_t right = (int32_t)(uint32_t)HLE_ARG3(c);
    int32_t bottom = (int32_t)(uint32_t)hle_stack_arg(c, 4u);

    if (left > right) { int32_t t = left; left = right; right = t; }
    if (top > bottom) { int32_t t = top; top = bottom; bottom = t; }

    pthread_mutex_lock(&g_u32_mu);
    gdi_dc_t *dc = find_gdi_dc_locked(hdc);
    uint32_t *px = NULL;
    int32_t dw = 0, dh = 0;
    bool top_down = true;
    se_wnd_t *wnd = NULL;
    if (dc && resolve_dc_pixels_locked(hdc, &px, &dw, &dh, &top_down, &wnd)) {
        gdi_obj_t *brush = find_gdi_obj_locked(dc->sel_brush);
        gdi_obj_t *pen = find_gdi_obj_locked(dc->sel_pen);
        if (brush && brush->kind == GDI_OBJ_BRUSH && !brush->hollow) {
            int32_t x0 = (left < 0) ? 0 : left;
            int32_t y0 = (top < 0) ? 0 : top;
            int32_t x1 = (right > dw) ? dw : right;
            int32_t y1 = (bottom > dh) ? dh : bottom;
            for (int32_t y = y0; y < y1; ++y) {
                for (int32_t x = x0; x < x1; ++x) {
                    fb_put_px(px, dw, dh, top_down, x, y, brush->color_xrgb);
                }
            }
        }
        if (pen && pen->kind == GDI_OBJ_PEN && !pen->hollow && right > left && bottom > top) {
            int32_t pw = (pen->pen_width > 0) ? pen->pen_width : 1;
            uint32_t pcol = pen->color_xrgb;
            for (int32_t t = 0; t < pw; ++t) {
                for (int32_t x = left; x < right; ++x) {
                    fb_put_px(px, dw, dh, top_down, x, top + t, pcol);
                    fb_put_px(px, dw, dh, top_down, x, bottom - 1 - t, pcol);
                }
                for (int32_t y = top; y < bottom; ++y) {
                    fb_put_px(px, dw, dh, top_down, left + t, y, pcol);
                    fb_put_px(px, dw, dh, top_down, right - 1 - t, y, pcol);
                }
            }
        }
        if (wnd && wnd->is_splash) wnd->fb_dirty = true;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_gdi_BitBlt(CPU *c) {
    uint64_t hdc_dst = HLE_ARG0(c);
    int32_t dx = (int32_t)(uint32_t)HLE_ARG1(c);
    int32_t dy = (int32_t)(uint32_t)HLE_ARG2(c);
    int32_t cx = (int32_t)(uint32_t)HLE_ARG3(c);
    int32_t cy = (int32_t)(uint32_t)hle_stack_arg(c, 4u);
    uint64_t hdc_src = hle_stack_arg(c, 5u);
    int32_t sx = (int32_t)(uint32_t)hle_stack_arg(c, 6u);
    int32_t sy = (int32_t)(uint32_t)hle_stack_arg(c, 7u);

    if (cx <= 0 || cy <= 0) {
        HLE_RET32(c, 1u);
        return;
    }

    pthread_mutex_lock(&g_u32_mu);
    uint32_t *dpx = NULL, *spx = NULL;
    int32_t dw = 0, dh = 0, sw = 0, sh = 0;
    bool d_td = true, s_td = true;
    se_wnd_t *dwnd = NULL, *swnd = NULL;
    if (resolve_dc_pixels_locked(hdc_dst, &dpx, &dw, &dh, &d_td, &dwnd) &&
        resolve_dc_pixels_locked(hdc_src, &spx, &sw, &sh, &s_td, &swnd)) {
        for (int32_t y = 0; y < cy; ++y) {
            int32_t ty = dy + y;
            int32_t fy = sy + y;
            if (ty < 0 || ty >= dh || fy < 0 || fy >= sh) continue;
            for (int32_t x = 0; x < cx; ++x) {
                int32_t tx = dx + x;
                int32_t fx = sx + x;
                if (tx < 0 || tx >= dw || fx < 0 || fx >= sw) continue;
                uint32_t val = fb_get_px(spx, sw, sh, s_td, fx, fy) | 0xFF000000u;
                fb_put_px(dpx, dw, dh, d_td, tx, ty, val);
            }
        }
        if (dwnd && dwnd->is_splash) dwnd->fb_dirty = true;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_gdi_StretchBlt(CPU *c) {
    uint64_t hdc_dst = HLE_ARG0(c);
    int32_t dx = (int32_t)(uint32_t)HLE_ARG1(c);
    int32_t dy = (int32_t)(uint32_t)HLE_ARG2(c);
    int32_t dw_box = (int32_t)(uint32_t)HLE_ARG3(c);
    int32_t dh_box = (int32_t)(uint32_t)hle_stack_arg(c, 4u);
    uint64_t hdc_src = hle_stack_arg(c, 5u);
    int32_t sx = (int32_t)(uint32_t)hle_stack_arg(c, 6u);
    int32_t sy = (int32_t)(uint32_t)hle_stack_arg(c, 7u);
    int32_t sw_box = (int32_t)(uint32_t)hle_stack_arg(c, 8u);
    int32_t sh_box = (int32_t)(uint32_t)hle_stack_arg(c, 9u);

    if (dw_box <= 0 || dh_box <= 0 || sw_box <= 0 || sh_box <= 0) {
        HLE_RET32(c, 1u);
        return;
    }

    pthread_mutex_lock(&g_u32_mu);
    uint32_t *dpx = NULL, *spx = NULL;
    int32_t dw = 0, dh = 0, sw = 0, sh = 0;
    bool d_td = true, s_td = true;
    se_wnd_t *dwnd = NULL, *swnd = NULL;
    if (resolve_dc_pixels_locked(hdc_dst, &dpx, &dw, &dh, &d_td, &dwnd) &&
        resolve_dc_pixels_locked(hdc_src, &spx, &sw, &sh, &s_td, &swnd)) {
        for (int32_t y = 0; y < dh_box; ++y) {
            int32_t ty = dy + y;
            if (ty < 0 || ty >= dh) continue;
            int32_t fy = sy + (int32_t)(((int64_t)y * (int64_t)sh_box) / dh_box);
            if (fy < 0) fy = 0;
            if (fy >= sh) fy = sh - 1;
            for (int32_t x = 0; x < dw_box; ++x) {
                int32_t tx = dx + x;
                if (tx < 0 || tx >= dw) continue;
                int32_t fx = sx + (int32_t)(((int64_t)x * (int64_t)sw_box) / dw_box);
                if (fx < 0) fx = 0;
                if (fx >= sw) fx = sw - 1;
                uint32_t val = fb_get_px(spx, sw, sh, s_td, fx, fy) | 0xFF000000u;
                fb_put_px(dpx, dw, dh, d_td, tx, ty, val);
            }
        }
        if (dwnd && dwnd->is_splash) dwnd->fb_dirty = true;
    }
    pthread_mutex_unlock(&g_u32_mu);
    HLE_RET32(c, 1u);
}

static void hle_u32_SetForegroundWindow(CPU *c) {
    uint64_t hwnd = HLE_ARG0(c);
    uint64_t wndproc = 0ull;

    pthread_mutex_lock(&g_u32_mu);
    se_wnd_t *w = find_wnd_locked(hwnd);
    if (w) wndproc = w->wndproc_va;
    pthread_mutex_unlock(&g_u32_mu);

    dispatch_window_activate_focus(c, hwnd, wndproc);
    HLE_RET32(c, 1u);
}

void se_register_user32_gdi32_shims(void) {
    /* USER32.dll (58) */
    se_hle_register_import("USER32.dll", "SetCursorPos",              hle_u32_SetCursorPos);
    se_hle_register_import("USER32.dll", "ClientToScreen",            hle_u32_ret1);
    se_hle_register_import("USER32.dll", "GetCursorPos",              hle_u32_GetCursorPos);
    se_hle_register_import("USER32.dll", "ScreenToClient",            hle_u32_ret1);
    se_hle_register_import("USER32.dll", "GetKeyState",               hle_u32_GetKeyState);
    se_hle_register_import("USER32.dll", "EnumDisplaySettingsA",      hle_u32_EnumDisplaySettingsA);
    se_hle_register_import("USER32.dll", "MessageBoxW",               hle_u32_MessageBoxW);
    se_hle_register_import("USER32.dll", "wsprintfW",                 hle_u32_wsprintfW);
    se_hle_register_import("USER32.dll", "DestroyCursor",             hle_u32_ret1);
    se_hle_register_import("USER32.dll", "GetMonitorInfoA",           hle_u32_GetMonitorInfoA);
    se_hle_register_import("USER32.dll", "EnumDisplayMonitors",       hle_u32_EnumDisplayMonitors);
    se_hle_register_import("USER32.dll", "SendMessageW",              hle_u32_SendMessageW);
    se_hle_register_import("USER32.dll", "SetWindowTextW",            hle_u32_SetWindowTextW);
    se_hle_register_import("USER32.dll", "GetWindowLongW",            hle_u32_GetWindowLongPtrW);
    se_hle_register_import("USER32.dll", "ShowWindow",                hle_u32_ShowWindow);
    se_hle_register_import("USER32.dll", "SetWindowLongW",            hle_u32_SetWindowLongPtrW);
    se_hle_register_import("USER32.dll", "AdjustWindowRectExForDpi",  hle_u32_AdjustWindowRectExForDpi);
    se_hle_register_import("USER32.dll", "SetWindowPos",              hle_u32_SetWindowPos);
    se_hle_register_import("USER32.dll", "GetClientRect",             hle_u32_GetClientRect);
    se_hle_register_import("USER32.dll", "GetWindowRect",             hle_u32_GetWindowRect);
    se_hle_register_import("USER32.dll", "ChangeDisplaySettingsExA",  hle_u32_ret0);
    se_hle_register_import("USER32.dll", "MonitorFromWindow",         hle_u32_MonitorFromWindow);
    se_hle_register_import("USER32.dll", "GetWindowPlacement",        hle_u32_GetWindowPlacement);
    se_hle_register_import("USER32.dll", "SetForegroundWindow",       hle_u32_SetForegroundWindow);
    se_hle_register_import("USER32.dll", "OpenClipboard",             hle_u32_ret1);
    se_hle_register_import("USER32.dll", "EmptyClipboard",            hle_u32_ret1);
    se_hle_register_import("USER32.dll", "SetClipboardData",          hle_u32_ret0);
    se_hle_register_import("USER32.dll", "CloseClipboard",            hle_u32_ret1);
    se_hle_register_import("USER32.dll", "CreateIconIndirect",        hle_u32_LoadHandle);
    se_hle_register_import("USER32.dll", "GetClipboardData",          hle_u32_ret0);
    se_hle_register_import("USER32.dll", "EnableNonClientDpiScaling", hle_u32_ret1);
    se_hle_register_import("USER32.dll", "DefWindowProcW",            hle_u32_DefWindowProcW);
    se_hle_register_import("USER32.dll", "PostQuitMessage",           hle_u32_PostQuitMessage);
    se_hle_register_import("USER32.dll", "FindWindowW",               hle_u32_FindWindowW);
    se_hle_register_import("USER32.dll", "LoadIconW",                 hle_u32_LoadHandle);
    se_hle_register_import("USER32.dll", "RegisterClassW",            hle_u32_RegisterClassW);
    se_hle_register_import("USER32.dll", "CreateWindowExW",           hle_u32_CreateWindowExW);
    se_hle_register_import("USER32.dll", "GetWindowThreadProcessId",  hle_u32_GetWindowThreadProcessId);
    se_hle_register_import("USER32.dll", "PeekMessageW",              hle_u32_PeekMessageW);
    se_hle_register_import("USER32.dll", "TranslateMessage",          hle_u32_TranslateMessage);
    se_hle_register_import("USER32.dll", "DispatchMessageW",          hle_u32_DispatchMessageW);
    se_hle_register_import("USER32.dll", "WaitMessage",               hle_u32_WaitMessage);
    se_hle_register_import("USER32.dll", "DestroyWindow",             hle_u32_DestroyWindow);
    se_hle_register_import("USER32.dll", "UnregisterClassW",          hle_u32_UnregisterClassW);
    se_hle_register_import("USER32.dll", "SetWindowLongPtrW",         hle_u32_SetWindowLongPtrW);
    se_hle_register_import("USER32.dll", "GetWindowLongPtrW",         hle_u32_GetWindowLongPtrW);
    se_hle_register_import("USER32.dll", "ReleaseDC",                 hle_u32_ReleaseDC);
    se_hle_register_import("USER32.dll", "BringWindowToTop",          hle_u32_ret1);
    se_hle_register_import("USER32.dll", "DrawTextW",                 hle_u32_DrawTextW);
    se_hle_register_import("USER32.dll", "LoadCursorW",               hle_u32_LoadHandle);
    se_hle_register_import("USER32.dll", "RegisterClassExW",          hle_u32_RegisterClassExW);
    se_hle_register_import("USER32.dll", "GetSystemMetrics",          hle_u32_GetSystemMetrics);
    se_hle_register_import("USER32.dll", "BeginPaint",                hle_u32_BeginPaint);
    se_hle_register_import("USER32.dll", "EndPaint",                  hle_u32_EndPaint);
    se_hle_register_import("USER32.dll", "InvalidateRect",            hle_u32_InvalidateRect);
    se_hle_register_import("USER32.dll", "SetCursor",                 hle_u32_LoadHandle);
    se_hle_register_import("USER32.dll", "GetDC",                     hle_u32_GetDC);
    se_hle_register_import("USER32.dll", "IsClipboardFormatAvailable", hle_u32_ret0);

    /* GDI32.dll (20) */
    se_hle_register_import("GDI32.dll", "SetBkMode",           hle_gdi_SetBkMode);
    se_hle_register_import("GDI32.dll", "Rectangle",           hle_gdi_Rectangle);
    se_hle_register_import("GDI32.dll", "BitBlt",              hle_gdi_BitBlt);
    se_hle_register_import("GDI32.dll", "StretchBlt",          hle_gdi_StretchBlt);
    se_hle_register_import("GDI32.dll", "SetStretchBltMode",   hle_u32_ret1);
    se_hle_register_import("GDI32.dll", "DeleteDC",            hle_gdi_DeleteDC);
    se_hle_register_import("GDI32.dll", "CreateCompatibleDC",  hle_gdi_CreateCompatibleDC);
    se_hle_register_import("GDI32.dll", "GetStockObject",      hle_gdi_GetStockObject);
    se_hle_register_import("GDI32.dll", "SetTextColor",        hle_gdi_SetTextColor);
    se_hle_register_import("GDI32.dll", "SelectObject",        hle_gdi_SelectObject);
    se_hle_register_import("GDI32.dll", "CreatePen",           hle_gdi_CreatePen);
    se_hle_register_import("GDI32.dll", "CreateFontIndirectW", hle_gdi_CreateFontIndirectW);
    se_hle_register_import("GDI32.dll", "CreateSolidBrush",    hle_gdi_CreateSolidBrush);
    se_hle_register_import("GDI32.dll", "DescribePixelFormat", hle_gdi_DescribePixelFormat);
    se_hle_register_import("GDI32.dll", "SetPixelFormat",      hle_gdi_SetPixelFormat);
    se_hle_register_import("GDI32.dll", "ChoosePixelFormat",   hle_gdi_ChoosePixelFormat);
    se_hle_register_import("GDI32.dll", "GetDeviceCaps",       hle_gdi_GetDeviceCaps);
    se_hle_register_import("GDI32.dll", "DeleteObject",        hle_gdi_DeleteObject);
    se_hle_register_import("GDI32.dll", "CreateBitmap",        hle_gdi_CreateBitmap);
    se_hle_register_import("GDI32.dll", "CreateDIBSection",    hle_gdi_CreateDIBSection);
}
