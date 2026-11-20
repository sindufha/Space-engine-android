/*
 * win32_kernel32.c - Clean Win64 KERNEL32.dll compatibility layer (all 166 imports)
 * targeting Android NDK POSIX/Bionic and Win64/MinGW host unit tests.
 */
#define _POSIX_C_SOURCE 200809L

#include "hle_bridge.h"
#include "pe_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if !defined(_WIN32)
#include <sys/mman.h>
#else
#include <direct.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#define K32_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineK32", fmt, ##__VA_ARGS__)
#else
#define K32_LOG(fmt, ...) ((void)0)
#endif

/* ---- Win32 Error Constants ----------------------------------------------- */
#define W32_ERROR_SUCCESS           0u
#define W32_ERROR_FILE_NOT_FOUND    2u
#define W32_ERROR_PATH_NOT_FOUND    3u
#define W32_ERROR_ACCESS_DENIED     5u
#define W32_ERROR_INVALID_HANDLE    6u
#define W32_ERROR_NOT_ENOUGH_MEMORY 8u
#define W32_ERROR_NO_MORE_FILES     18u
#define W32_ERROR_NOT_SUPPORTED     50u
#define W32_ERROR_FILE_EXISTS       80u
#define W32_ERROR_INVALID_PARAMETER 87u
#define W32_ERROR_INSUFFICIENT_BUF  122u
#define W32_ERROR_MOD_NOT_FOUND     126u
#define W32_ERROR_PROC_NOT_FOUND    127u
#define W32_ERROR_ALREADY_EXISTS    183u
#define W32_ERROR_MORE_DATA         234u
#define W32_WAIT_OBJECT_0           0u
#define W32_WAIT_TIMEOUT            0x00000102u
#define W32_WAIT_FAILED             0xFFFFFFFFu
#define W32_INFINITE                0xFFFFFFFFu
#define W32_INVALID_HANDLE          0xFFFFFFFFFFFFFFFFull
#define W32_FILETIME_EPOCH_DIFF     116444736000000000ull /* 100ns ticks 1601..1970 */

static uint64_t unix_to_filetime(time_t sec, long nsec) {
    return ((uint64_t)sec * 10000000ull) + (uint64_t)(nsec / 100L) + W32_FILETIME_EPOCH_DIFF;
}

static uint32_t errno_to_w32(int e) {
    switch (e) {
    case ENOENT: return W32_ERROR_FILE_NOT_FOUND;
    case EACCES:
    case EPERM:  return W32_ERROR_ACCESS_DENIED;
    case EEXIST: return W32_ERROR_ALREADY_EXISTS;
    case ENOMEM: return W32_ERROR_NOT_ENOUGH_MEMORY;
    case EINVAL: return W32_ERROR_INVALID_PARAMETER;
    default:     return 31u; /* ERROR_GEN_FAILURE */
    }
}

static void k32_set_err(uint32_t err) {
    se_win64_teb_set_last_error(err);
}

static uint32_t k32_get_err(void) {
    return se_win64_teb_get_last_error();
}

static int strcasecmp_local(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
        ++a;
        ++b;
    }
}

/* ---- Unified Win32 Handle Table (Files, Threads, Events, Semaphores, Find) */

typedef enum {
    HT_NONE = 0,
    HT_FILE,
    HT_STD,
    HT_EVENT,
    HT_SEMAPHORE,
    HT_THREAD,
    HT_FIND
} HandleType;

typedef struct FindEntry {
    char name[512];
    uint32_t attr;
    uint64_t size;
    uint64_t mtime_ft;
} FindEntry;

typedef struct {
    HandleType type;
    int ref_count;
    union {
        struct {
            int fd;
            bool close_fd;
            uint32_t access;
            uint32_t attr;
        } file;
        struct {
            pthread_mutex_t mu;
            pthread_cond_t cv;
            bool manual_reset;
            bool signaled;
            char name[128];
        } event;
        struct {
            pthread_mutex_t mu;
            pthread_cond_t cv;
            int32_t count;
            int32_t max_count;
            char name[128];
        } sem;
        struct {
            pthread_t pt;
            pthread_mutex_t mu;
            pthread_cond_t cv;
            uint32_t tid;
            uint32_t exit_code;
            bool finished;
            bool detached;
            uint64_t start_va;
            uint64_t param;
            size_t stack_size;
        } thread;
        struct {
            FindEntry *entries;
            size_t count;
            size_t index;
        } find;
    } u;
} K32Handle;

#define K32_MAX_HANDLES     4096u
#define K32_HANDLE_BASE     0x10000ull
#define K32_STD_INPUT       ((uint64_t)-10ll)
#define K32_STD_OUTPUT      ((uint64_t)-11ll)
#define K32_STD_ERROR       ((uint64_t)-12ll)
#define K32_CUR_PROCESS     ((uint64_t)-1ll)
#define K32_CUR_THREAD      ((uint64_t)-2ll)

static K32Handle *g_handles[K32_MAX_HANDLES];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_std_in_handle = K32_STD_INPUT;
static uint64_t g_std_out_handle = K32_STD_OUTPUT;
static uint64_t g_std_err_handle = K32_STD_ERROR;

static uint64_t alloc_handle(HandleType type, K32Handle **out_h) {
    K32Handle *h = (K32Handle *)calloc(1u, sizeof(K32Handle));
    if (!h) return 0;
    h->type = type;
    h->ref_count = 1;

    pthread_mutex_lock(&g_handle_lock);
    for (uint32_t i = 1; i < K32_MAX_HANDLES; ++i) {
        if (!g_handles[i]) {
            g_handles[i] = h;
            pthread_mutex_unlock(&g_handle_lock);
            if (out_h) *out_h = h;
            return K32_HANDLE_BASE + ((uint64_t)i * 4ull);
        }
    }
    pthread_mutex_unlock(&g_handle_lock);
    free(h);
    return 0;
}

static K32Handle *lookup_handle(uint64_t val, HandleType expected) {
    if (val < K32_HANDLE_BASE) return NULL;
    uint64_t off = val - K32_HANDLE_BASE;
    if ((off % 4ull) != 0) return NULL;
    uint64_t idx = off / 4ull;
    if (idx == 0 || idx >= K32_MAX_HANDLES) return NULL;

    pthread_mutex_lock(&g_handle_lock);
    K32Handle *h = g_handles[idx];
    if (h && expected != HT_NONE && h->type != expected) {
        h = NULL;
    }
    pthread_mutex_unlock(&g_handle_lock);
    return h;
}

static void destroy_handle(K32Handle *h) {
    if (!h) return;
    switch (h->type) {
    case HT_FILE:
        if (h->u.file.close_fd && h->u.file.fd >= 0) close(h->u.file.fd);
        break;
    case HT_EVENT:
        pthread_cond_destroy(&h->u.event.cv);
        pthread_mutex_destroy(&h->u.event.mu);
        break;
    case HT_SEMAPHORE:
        pthread_cond_destroy(&h->u.sem.cv);
        pthread_mutex_destroy(&h->u.sem.mu);
        break;
    case HT_THREAD:
        if (!h->u.thread.detached) pthread_detach(h->u.thread.pt);
        pthread_cond_destroy(&h->u.thread.cv);
        pthread_mutex_destroy(&h->u.thread.mu);
        break;
    case HT_FIND:
        free(h->u.find.entries);
        break;
    default:
        break;
    }
    free(h);
}

static bool close_k32_handle(uint64_t val) {
    if (val == K32_STD_INPUT || val == K32_STD_OUTPUT || val == K32_STD_ERROR ||
        val == K32_CUR_PROCESS || val == K32_CUR_THREAD) {
        return true;
    }
    if (val < K32_HANDLE_BASE || ((val - K32_HANDLE_BASE) % 4ull) != 0) return false;
    uint64_t idx = (val - K32_HANDLE_BASE) / 4ull;
    if (idx == 0 || idx >= K32_MAX_HANDLES) return false;

    pthread_mutex_lock(&g_handle_lock);
    K32Handle *h = g_handles[idx];
    if (!h) {
        pthread_mutex_unlock(&g_handle_lock);
        return false;
    }
    h->ref_count--;
    if (h->ref_count <= 0) {
        g_handles[idx] = NULL;
    } else {
        h = NULL;
    }
    pthread_mutex_unlock(&g_handle_lock);
    if (h) destroy_handle(h);
    return true;
}

static int resolve_std_fd(uint64_t h) {
    if (h == K32_STD_INPUT || h == g_std_in_handle) return STDIN_FILENO;
    if (h == K32_STD_OUTPUT || h == g_std_out_handle) return STDOUT_FILENO;
    if (h == K32_STD_ERROR || h == g_std_err_handle) return STDERR_FILENO;
    return -1;
}

/* ---- Heap / Global / Local / VirtualAlloc -------------------------------- */

#define HEAP_MAGIC 0x53454850424C4B31ull

typedef struct {
    uint64_t magic;
    uint64_t size;
} HeapHdr;

#define MAX_VALLOC_REGIONS 2048u
typedef struct {
    uint64_t addr;
    size_t size;
    bool in_use;
} VAllocRegion;

static VAllocRegion g_vallocs[MAX_VALLOC_REGIONS];
static pthread_mutex_t g_valloc_lock = PTHREAD_MUTEX_INITIALIZER;

static void k32_GetProcessHeap(CPU *c) {
    HLE_RET64(c, 0x0000534548454150ull);
}

static void k32_HeapAlloc(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG1(c);
    size_t sz = (size_t)HLE_ARG2(c);
    size_t alloc_sz = sz ? sz : 1u;
    HeapHdr *hdr = (HeapHdr *)malloc(sizeof(HeapHdr) + alloc_sz);
    if (!hdr) {
        k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY);
        HLE_RET64(c, 0);
        return;
    }
    hdr->magic = HEAP_MAGIC;
    hdr->size = (uint64_t)sz;
    uint8_t *usr = (uint8_t *)(hdr + 1);
    if (flags & 0x08u) { /* HEAP_ZERO_MEMORY */
        memset(usr, 0, alloc_sz);
    }
    HLE_RET_PTR(c, usr);
}

static void k32_HeapReAlloc(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG1(c);
    uint64_t ptr = HLE_ARG2(c);
    size_t new_sz = (size_t)HLE_ARG3(c);
    if (!ptr) {
        c->rcx = HLE_ARG0(c);
        c->rdx = flags;
        c->r8 = new_sz;
        k32_HeapAlloc(c);
        return;
    }
    HeapHdr *old_hdr = ((HeapHdr *)(uintptr_t)ptr) - 1;
    if (old_hdr->magic != HEAP_MAGIC) {
        void *np = realloc((void *)(uintptr_t)ptr, new_sz ? new_sz : 1u);
        HLE_RET_PTR(c, np);
        return;
    }
    uint64_t old_sz = old_hdr->size;
    size_t alloc_sz = new_sz ? new_sz : 1u;
    HeapHdr *new_hdr = (HeapHdr *)realloc(old_hdr, sizeof(HeapHdr) + alloc_sz);
    if (!new_hdr) {
        k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY);
        HLE_RET64(c, 0);
        return;
    }
    new_hdr->magic = HEAP_MAGIC;
    new_hdr->size = (uint64_t)new_sz;
    uint8_t *usr = (uint8_t *)(new_hdr + 1);
    if ((flags & 0x08u) && (uint64_t)new_sz > old_sz) {
        memset(usr + old_sz, 0, (size_t)((uint64_t)new_sz - old_sz));
    }
    HLE_RET_PTR(c, usr);
}

static void k32_HeapFree(CPU *c) {
    uint64_t ptr = HLE_ARG2(c);
    if (ptr) {
        HeapHdr *hdr = ((HeapHdr *)(uintptr_t)ptr) - 1;
        if (hdr->magic == HEAP_MAGIC) {
            hdr->magic = 0;
            free(hdr);
        } else {
            free((void *)(uintptr_t)ptr);
        }
    }
    HLE_RET32(c, 1u);
}

static void k32_HeapSize(CPU *c) {
    uint64_t ptr = HLE_ARG2(c);
    if (!ptr) {
        HLE_RET64(c, (uint64_t)-1ll);
        return;
    }
    HeapHdr *hdr = ((HeapHdr *)(uintptr_t)ptr) - 1;
    if (hdr->magic == HEAP_MAGIC) {
        HLE_RET64(c, hdr->size);
    } else {
        HLE_RET64(c, (uint64_t)-1ll);
    }
}

static void k32_HeapCompact(CPU *c) { HLE_RET64(c, 4096ull); }

static void k32_GlobalAlloc(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG0(c);
    size_t sz = (size_t)HLE_ARG1(c);
    void *p = (flags & 0x40u) ? calloc(1u, sz ? sz : 1u) : malloc(sz ? sz : 1u);
    HLE_RET_PTR(c, p);
}
static void k32_GlobalFree(CPU *c) { free((void *)(uintptr_t)HLE_ARG0(c)); HLE_RET64(c, 0); }
static void k32_GlobalLock(CPU *c) { HLE_RET64(c, HLE_ARG0(c)); }
static void k32_GlobalUnlock(CPU *c) { HLE_RET32(c, 1u); }

static void k32_LocalAlloc(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG0(c);
    size_t sz = (size_t)HLE_ARG1(c);
    void *p = (flags & 0x40u) ? calloc(1u, sz ? sz : 1u) : malloc(sz ? sz : 1u);
    HLE_RET_PTR(c, p);
}
static void k32_LocalFree(CPU *c) { free((void *)(uintptr_t)HLE_ARG0(c)); HLE_RET64(c, 0); }

static void k32_VirtualAlloc(CPU *c) {
    uint64_t addr = HLE_ARG0(c);
    size_t size = (size_t)HLE_ARG1(c);
    uint32_t flType = (uint32_t)HLE_ARG2(c);
    if (size == 0) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET64(c, 0);
        return;
    }
    size = (size + 4095u) & ~(size_t)4095u;

    /* MEM_COMMIT on an already reserved range inside a tracked region: succeed in-place. */
    if (addr != 0 && (flType & 0x1000u) && !(flType & 0x2000u)) {
        pthread_mutex_lock(&g_valloc_lock);
        for (size_t i = 0; i < MAX_VALLOC_REGIONS; ++i) {
            if (g_vallocs[i].in_use && addr >= g_vallocs[i].addr &&
                addr + size <= g_vallocs[i].addr + g_vallocs[i].size) {
                pthread_mutex_unlock(&g_valloc_lock);
                HLE_RET64(c, addr);
                return;
            }
        }
        pthread_mutex_unlock(&g_valloc_lock);
    }

    void *p = NULL;
#if !defined(_WIN32)
    p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED && addr != 0) {
        p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (p == MAP_FAILED) p = NULL;
#else
    p = calloc(1u, size);
#endif
    if (!p) {
        k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY);
        HLE_RET64(c, 0);
        return;
    }

    pthread_mutex_lock(&g_valloc_lock);
    for (size_t i = 0; i < MAX_VALLOC_REGIONS; ++i) {
        if (!g_vallocs[i].in_use) {
            g_vallocs[i].addr = (uint64_t)(uintptr_t)p;
            g_vallocs[i].size = size;
            g_vallocs[i].in_use = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_valloc_lock);
    HLE_RET_PTR(c, p);
}

static void k32_VirtualFree(CPU *c) {
    uint64_t addr = HLE_ARG0(c);
    size_t size = (size_t)HLE_ARG1(c);
    uint32_t flType = (uint32_t)HLE_ARG2(c);
    if (!addr) {
        HLE_RET32(c, 0);
        return;
    }
    if (flType & 0x8000u) { /* MEM_RELEASE */
        size_t tracked = size;
        pthread_mutex_lock(&g_valloc_lock);
        for (size_t i = 0; i < MAX_VALLOC_REGIONS; ++i) {
            if (g_vallocs[i].in_use && g_vallocs[i].addr == addr) {
                tracked = g_vallocs[i].size;
                g_vallocs[i].in_use = false;
                break;
            }
        }
        pthread_mutex_unlock(&g_valloc_lock);
#if !defined(_WIN32)
        if (tracked) munmap((void *)(uintptr_t)addr, tracked);
#else
        (void)tracked;
        free((void *)(uintptr_t)addr);
#endif
    }
    HLE_RET32(c, 1u);
}

static void k32_VirtualProtect(CPU *c) {
    uint64_t lpOld = HLE_ARG3(c);
    if (lpOld) wr32(lpOld, 0x04u); /* PAGE_READWRITE */
    HLE_RET32(c, 1u);
}

/* ---- File & Directory & Find I/O ----------------------------------------- */

static void ensure_parent_dirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
#if defined(_WIN32)
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/';
        }
    }
}

static void k32_CreateFileW(CPU *c) {
    const uint16_t *wname = (const uint16_t *)(uintptr_t)HLE_ARG0(c);
    uint32_t desired_access = (uint32_t)HLE_ARG1(c);
    uint32_t creation_disp = (uint32_t)HLE_ARG4(c);
    if (!wname) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET64(c, W32_INVALID_HANDLE);
        return;
    }

    char utf8[1024];
    se_utf16_to_utf8(wname, -1, utf8, (int)sizeof(utf8));
    bool for_write = (desired_access & 0x40000000u) != 0 ||
                     creation_disp == 1u || creation_disp == 2u || creation_disp == 4u || creation_disp == 5u;
    if (strcmp(utf8, "CONOUT$") == 0 || strcmp(utf8, "CONOUT") == 0) {
        HLE_RET64(c, K32_STD_OUTPUT);
        return;
    }
    if (strcmp(utf8, "CONIN$") == 0 || strcmp(utf8, "CONIN") == 0) {
        HLE_RET64(c, K32_STD_INPUT);
        return;
    }

    char host_path[1024];
    if (!se_vfs_resolve_path(utf8, for_write, host_path, sizeof(host_path))) {
        k32_set_err(W32_ERROR_PATH_NOT_FOUND);
        HLE_RET64(c, W32_INVALID_HANDLE);
        return;
    }

    int oflags = 0;
#ifdef O_BINARY
    oflags |= O_BINARY;
#endif
    bool want_read = (desired_access & 0x80000000u) != 0 || !for_write;
    bool want_write = (desired_access & 0x40000000u) != 0;
    if (want_read && want_write) oflags |= O_RDWR;
    else if (want_write) oflags |= O_WRONLY;
    else oflags |= O_RDONLY;

    switch (creation_disp) {
    case 1u: oflags |= O_CREAT | O_EXCL; break;  /* CREATE_NEW */
    case 2u: oflags |= O_CREAT | O_TRUNC; break; /* CREATE_ALWAYS */
    case 3u: break;                              /* OPEN_EXISTING */
    case 4u: oflags |= O_CREAT; break;           /* OPEN_ALWAYS */
    case 5u: oflags |= O_TRUNC; break;           /* TRUNCATE_EXISTING */
    default: break;
    }

    if (oflags & O_CREAT) ensure_parent_dirs(host_path);

    int fd = open(host_path, oflags, 0644);
    if (fd < 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET64(c, W32_INVALID_HANDLE);
        return;
    }

    K32Handle *h = NULL;
    uint64_t hv = alloc_handle(HT_FILE, &h);
    if (!hv) {
        close(fd);
        k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY);
        HLE_RET64(c, W32_INVALID_HANDLE);
        return;
    }
    h->u.file.fd = fd;
    h->u.file.close_fd = true;
    h->u.file.access = desired_access;
    h->u.file.attr = 0x80u;
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET64(c, hv);
}

static void k32_ReadFile(CPU *c) {
    uint64_t hv = HLE_ARG0(c);
    void *buf = (void *)(uintptr_t)HLE_ARG1(c);
    uint32_t to_read = (uint32_t)HLE_ARG2(c);
    uint64_t out_read_va = HLE_ARG3(c);

    int fd = resolve_std_fd(hv);
    if (fd < 0) {
        K32Handle *h = lookup_handle(hv, HT_FILE);
        if (!h) {
            k32_set_err(W32_ERROR_INVALID_HANDLE);
            HLE_RET32(c, 0);
            return;
        }
        fd = h->u.file.fd;
    }

    size_t done = 0;
    while (done < to_read) {
        ssize_t n = read(fd, (uint8_t *)buf + done, to_read - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            k32_set_err(errno_to_w32(errno));
            if (out_read_va) wr32(out_read_va, (uint32_t)done);
            HLE_RET32(c, 0);
            return;
        }
        if (n == 0) break;
        done += (size_t)n;
    }
    if (out_read_va) wr32(out_read_va, (uint32_t)done);
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET32(c, 1u);
}

static void k32_WriteFile(CPU *c) {
    uint64_t hv = HLE_ARG0(c);
    const void *buf = (const void *)(uintptr_t)HLE_ARG1(c);
    uint32_t to_write = (uint32_t)HLE_ARG2(c);
    uint64_t out_written_va = HLE_ARG3(c);

    int fd = resolve_std_fd(hv);
    if (fd < 0) {
        K32Handle *h = lookup_handle(hv, HT_FILE);
        if (!h) {
            k32_set_err(W32_ERROR_INVALID_HANDLE);
            HLE_RET32(c, 0);
            return;
        }
        fd = h->u.file.fd;
    }

    size_t done = 0;
    while (done < to_write) {
        ssize_t n = write(fd, (const uint8_t *)buf + done, to_write - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            k32_set_err(errno_to_w32(errno));
            if (out_written_va) wr32(out_written_va, (uint32_t)done);
            HLE_RET32(c, 0);
            return;
        }
        done += (size_t)n;
    }
    if (out_written_va) wr32(out_written_va, (uint32_t)done);
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET32(c, 1u);
}

static void k32_SetFilePointerEx(CPU *c) {
    uint64_t hv = HLE_ARG0(c);
    int64_t dist = (int64_t)HLE_ARG1(c);
    uint64_t out_new_va = HLE_ARG2(c);
    uint32_t method = (uint32_t)HLE_ARG3(c);

    K32Handle *h = lookup_handle(hv, HT_FILE);
    if (!h) {
        k32_set_err(W32_ERROR_INVALID_HANDLE);
        HLE_RET32(c, 0);
        return;
    }
    int whence = (method == 1u) ? SEEK_CUR : (method == 2u ? SEEK_END : SEEK_SET);
#if defined(_WIN32)
    __int64 pos = _lseeki64(h->u.file.fd, (__int64)dist, whence);
#else
    off_t pos = lseek(h->u.file.fd, (off_t)dist, whence);
#endif
    if (pos < 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET32(c, 0);
        return;
    }
    if (out_new_va) wr64(out_new_va, (uint64_t)pos);
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET32(c, 1u);
}

static void k32_GetFileSizeEx(CPU *c) {
    uint64_t hv = HLE_ARG0(c);
    uint64_t out_sz_va = HLE_ARG1(c);
    K32Handle *h = lookup_handle(hv, HT_FILE);
    if (!h) {
        k32_set_err(W32_ERROR_INVALID_HANDLE);
        HLE_RET32(c, 0);
        return;
    }
    struct stat st;
    if (fstat(h->u.file.fd, &st) != 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET32(c, 0);
        return;
    }
    if (out_sz_va) wr64(out_sz_va, (uint64_t)st.st_size);
    HLE_RET32(c, 1u);
}

static void k32_SetEndOfFile(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_FILE);
    if (!h) { HLE_RET32(c, 0); return; }
#if defined(_WIN32)
    __int64 cur = _lseeki64(h->u.file.fd, 0, SEEK_CUR);
    int rc = (cur >= 0) ? _chsize_s(h->u.file.fd, cur) : -1;
#else
    off_t cur = lseek(h->u.file.fd, 0, SEEK_CUR);
    int rc = (cur >= 0) ? ftruncate(h->u.file.fd, cur) : -1;
#endif
    HLE_RET32(c, rc == 0 ? 1u : 0u);
}

static void k32_FlushFileBuffers(CPU *c) { (void)c; HLE_RET32(c, 1u); }

static void k32_GetFileType(CPU *c) {
    uint64_t hv = HLE_ARG0(c);
    if (resolve_std_fd(hv) >= 0) { HLE_RET32(c, 2u); return; } /* FILE_TYPE_CHAR */
    K32Handle *h = lookup_handle(hv, HT_FILE);
    HLE_RET32(c, h ? 1u : 0u); /* FILE_TYPE_DISK */
}

static void k32_GetFileInformationByHandle(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_FILE);
    uint64_t out_va = HLE_ARG1(c);
    if (!h || !out_va) { HLE_RET32(c, 0); return; }
    struct stat st;
    if (fstat(h->u.file.fd, &st) != 0) { HLE_RET32(c, 0); return; }
    uint8_t *p = (uint8_t *)(uintptr_t)out_va;
    memset(p, 0, 52u);
    uint32_t attr = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
    uint64_t ft = unix_to_filetime(st.st_mtime, 0);
    uint64_t fsz = (uint64_t)st.st_size;
    memcpy(p + 0x00u, &attr, 4);
    memcpy(p + 0x04u, &ft, 8);
    memcpy(p + 0x0Cu, &ft, 8);
    memcpy(p + 0x14u, &ft, 8);
    uint32_t hi = (uint32_t)(fsz >> 32), lo = (uint32_t)fsz, links = 1u;
    memcpy(p + 0x20u, &hi, 4);
    memcpy(p + 0x24u, &lo, 4);
    memcpy(p + 0x28u, &links, 4);
    HLE_RET32(c, 1u);
}

static void k32_GetFileAttributesW(CPU *c) {
    char host_path[1024];
    if (!se_vfs_resolve_wpath((const uint16_t *)(uintptr_t)HLE_ARG0(c), false, host_path, sizeof(host_path))) {
        k32_set_err(W32_ERROR_PATH_NOT_FOUND);
        HLE_RET32(c, 0xFFFFFFFFu);
        return;
    }
    struct stat st;
    if (stat(host_path, &st) != 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET32(c, 0xFFFFFFFFu);
        return;
    }
    HLE_RET32(c, S_ISDIR(st.st_mode) ? 0x10u : 0x80u);
}

static void k32_GetFileAttributesExW(CPU *c) {
    const uint16_t *wp = (const uint16_t *)(uintptr_t)HLE_ARG0(c);
    uint64_t out_va = HLE_ARG2(c);
    char host_path[1024];
    if (!out_va || !se_vfs_resolve_wpath(wp, false, host_path, sizeof(host_path))) {
        k32_set_err(W32_ERROR_PATH_NOT_FOUND);
        HLE_RET32(c, 0);
        return;
    }
    struct stat st;
    if (stat(host_path, &st) != 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET32(c, 0);
        return;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)out_va;
    memset(p, 0, 36u);
    uint32_t attr = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
    uint64_t ft = unix_to_filetime(st.st_mtime, 0);
    uint64_t fsz = (uint64_t)st.st_size;
    uint32_t hi = (uint32_t)(fsz >> 32), lo = (uint32_t)fsz;
    memcpy(p + 0x00u, &attr, 4);
    memcpy(p + 0x04u, &ft, 8);
    memcpy(p + 0x0Cu, &ft, 8);
    memcpy(p + 0x14u, &ft, 8);
    memcpy(p + 0x1Cu, &hi, 4);
    memcpy(p + 0x20u, &lo, 4);
    HLE_RET32(c, 1u);
}

static void k32_CreateDirectoryW(CPU *c) {
    char host_path[1024];
    if (!se_vfs_resolve_wpath((const uint16_t *)(uintptr_t)HLE_ARG0(c), true, host_path, sizeof(host_path))) {
        k32_set_err(W32_ERROR_PATH_NOT_FOUND);
        HLE_RET32(c, 0);
        return;
    }
    ensure_parent_dirs(host_path);
#if defined(_WIN32)
    int rc = _mkdir(host_path);
#else
    int rc = mkdir(host_path, 0755);
#endif
    if (rc != 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET32(c, 0);
        return;
    }
    HLE_RET32(c, 1u);
}

static void k32_DeleteFileW(CPU *c) {
    char host_path[1024];
    if (!se_vfs_resolve_wpath((const uint16_t *)(uintptr_t)HLE_ARG0(c), true, host_path, sizeof(host_path))) {
        k32_set_err(W32_ERROR_FILE_NOT_FOUND);
        HLE_RET32(c, 0);
        return;
    }
    int rc = remove(host_path);
    if (rc != 0) k32_set_err(errno_to_w32(errno));
    HLE_RET32(c, rc == 0 ? 1u : 0u);
}

static void k32_MoveFileExW(CPU *c) {
    char src[1024], dst[1024];
    if (!se_vfs_resolve_wpath((const uint16_t *)(uintptr_t)HLE_ARG0(c), true, src, sizeof(src)) ||
        !se_vfs_resolve_wpath((const uint16_t *)(uintptr_t)HLE_ARG1(c), true, dst, sizeof(dst))) {
        k32_set_err(W32_ERROR_PATH_NOT_FOUND);
        HLE_RET32(c, 0);
        return;
    }
    ensure_parent_dirs(dst);
    int rc = rename(src, dst);
    if (rc != 0) k32_set_err(errno_to_w32(errno));
    HLE_RET32(c, rc == 0 ? 1u : 0u);
}

static void k32_CopyFileA(CPU *c) {
    char src[1024], dst[1024];
    const char *s = (const char *)(uintptr_t)HLE_ARG0(c);
    const char *d = (const char *)(uintptr_t)HLE_ARG1(c);
    bool fail_if_exists = (HLE_ARG2(c) != 0);
    if (!se_vfs_resolve_path(s, false, src, sizeof(src)) ||
        !se_vfs_resolve_path(d, true, dst, sizeof(dst))) {
        k32_set_err(W32_ERROR_PATH_NOT_FOUND);
        HLE_RET32(c, 0);
        return;
    }
    struct stat st;
    if (fail_if_exists && stat(dst, &st) == 0) {
        k32_set_err(W32_ERROR_FILE_EXISTS);
        HLE_RET32(c, 0);
        return;
    }
    ensure_parent_dirs(dst);
    FILE *fi = fopen(src, "rb");
    if (!fi) { k32_set_err(errno_to_w32(errno)); HLE_RET32(c, 0); return; }
    FILE *fo = fopen(dst, "wb");
    if (!fo) { fclose(fi); k32_set_err(errno_to_w32(errno)); HLE_RET32(c, 0); return; }
    uint8_t buf[16384];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, nr, fo);
    fclose(fi);
    fclose(fo);
    HLE_RET32(c, 1u);
}

static bool wild_match_ci(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') ++pat;
            if (!*pat) return true;
            while (*str) {
                if (wild_match_ci(pat, str)) return true;
                ++str;
            }
            return false;
        }
        if (*pat == '?') {
            if (!*str) return false;
            ++pat;
            ++str;
        } else {
            if (tolower((unsigned char)*pat) != tolower((unsigned char)*str)) return false;
            ++pat;
            ++str;
        }
    }
    return *str == '\0';
}

static uint64_t find_open_utf8(const char *pattern) {
    char norm[1024];
    size_t n = 0;
    for (; pattern[n] && n + 1 < sizeof(norm); ++n) {
        norm[n] = (pattern[n] == '\\') ? '/' : pattern[n];
    }
    norm[n] = '\0';

    const char *slash = strrchr(norm, '/');
    char dir_part[1024], pat_part[256];
    if (slash) {
        size_t dlen = (size_t)(slash - norm);
        if (dlen >= sizeof(dir_part)) dlen = sizeof(dir_part) - 1;
        memcpy(dir_part, norm, dlen);
        dir_part[dlen] = '\0';
        size_t plen = strlen(slash + 1);
        if (plen >= sizeof(pat_part)) plen = sizeof(pat_part) - 1;
        memcpy(pat_part, slash + 1, plen);
        pat_part[plen] = '\0';
    } else {
        memcpy(dir_part, ".", 2u);
        size_t plen = strlen(norm);
        if (plen >= sizeof(pat_part)) plen = sizeof(pat_part) - 1;
        memcpy(pat_part, norm, plen);
        pat_part[plen] = '\0';
    }
    if (strcmp(pat_part, "*.*") == 0) snprintf(pat_part, sizeof(pat_part), "*");

    char host_dir[1024];
    if (!se_vfs_resolve_path(dir_part[0] ? dir_part : ".", false, host_dir, sizeof(host_dir))) {
        return W32_INVALID_HANDLE;
    }

    DIR *d = opendir(host_dir);
    if (!d) return W32_INVALID_HANDLE;

    size_t cap = 64, cnt = 0;
    FindEntry *list = (FindEntry *)malloc(cap * sizeof(FindEntry));
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!wild_match_ci(pat_part, ent->d_name)) continue;
        if (cnt >= cap) {
            cap *= 2u;
            FindEntry *nl = (FindEntry *)realloc(list, cap * sizeof(FindEntry));
            if (!nl) break;
            list = nl;
        }
        char full[1284];
        snprintf(full, sizeof(full), "%s/%s", host_dir, ent->d_name);
        struct stat st;
        memset(&st, 0, sizeof(st));
        stat(full, &st);
        FindEntry *e = &list[cnt++];
        snprintf(e->name, sizeof(e->name), "%s", ent->d_name);
        e->attr = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
        e->size = (uint64_t)st.st_size;
        e->mtime_ft = unix_to_filetime(st.st_mtime, 0);
    }
    closedir(d);

    if (cnt == 0) {
        free(list);
        return W32_INVALID_HANDLE;
    }

    K32Handle *h = NULL;
    uint64_t hv = alloc_handle(HT_FIND, &h);
    if (!hv) {
        free(list);
        return W32_INVALID_HANDLE;
    }
    h->u.find.entries = list;
    h->u.find.count = cnt;
    h->u.find.index = 0;
    return hv;
}

static void fill_find_data_w(const FindEntry *e, uint64_t out_va) {
    uint8_t *p = (uint8_t *)(uintptr_t)out_va;
    memset(p, 0, 592u);
    uint32_t hi = (uint32_t)(e->size >> 32), lo = (uint32_t)e->size;
    memcpy(p + 0x00u, &e->attr, 4);
    memcpy(p + 0x04u, &e->mtime_ft, 8);
    memcpy(p + 0x0Cu, &e->mtime_ft, 8);
    memcpy(p + 0x14u, &e->mtime_ft, 8);
    memcpy(p + 0x1Cu, &hi, 4);
    memcpy(p + 0x20u, &lo, 4);
    se_utf8_to_utf16(e->name, -1, (uint16_t *)(p + 0x2Cu), 260);
}

static void fill_find_data_a(const FindEntry *e, uint64_t out_va) {
    uint8_t *p = (uint8_t *)(uintptr_t)out_va;
    memset(p, 0, 320u);
    uint32_t hi = (uint32_t)(e->size >> 32), lo = (uint32_t)e->size;
    memcpy(p + 0x00u, &e->attr, 4);
    memcpy(p + 0x04u, &e->mtime_ft, 8);
    memcpy(p + 0x0Cu, &e->mtime_ft, 8);
    memcpy(p + 0x14u, &e->mtime_ft, 8);
    memcpy(p + 0x1Cu, &hi, 4);
    memcpy(p + 0x20u, &lo, 4);
    {
        size_t nlen = strlen(e->name);
        if (nlen >= 260u) nlen = 259u;
        memcpy(p + 0x2Cu, e->name, nlen);
        p[0x2Cu + nlen] = '\0';
    }
}

static void k32_FindFirstFileExW(CPU *c) {
    const uint16_t *wpat = (const uint16_t *)(uintptr_t)HLE_ARG0(c);
    uint64_t out_va = HLE_ARG3(c);
    if (!wpat || !out_va) { k32_set_err(W32_ERROR_INVALID_PARAMETER); HLE_RET64(c, W32_INVALID_HANDLE); return; }
    char pat[1024];
    se_utf16_to_utf8(wpat, -1, pat, (int)sizeof(pat));
    uint64_t hv = find_open_utf8(pat);
    if (hv == W32_INVALID_HANDLE) { k32_set_err(W32_ERROR_FILE_NOT_FOUND); HLE_RET64(c, W32_INVALID_HANDLE); return; }
    K32Handle *h = lookup_handle(hv, HT_FIND);
    fill_find_data_w(&h->u.find.entries[h->u.find.index++], out_va);
    HLE_RET64(c, hv);
}

static void k32_FindNextFileW(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_FIND);
    uint64_t out_va = HLE_ARG1(c);
    if (!h || !out_va || h->u.find.index >= h->u.find.count) {
        k32_set_err(W32_ERROR_NO_MORE_FILES);
        HLE_RET32(c, 0);
        return;
    }
    fill_find_data_w(&h->u.find.entries[h->u.find.index++], out_va);
    HLE_RET32(c, 1u);
}

static void k32_FindFirstFileA(CPU *c) {
    const char *pat = (const char *)(uintptr_t)HLE_ARG0(c);
    uint64_t out_va = HLE_ARG1(c);
    if (!pat || !out_va) { k32_set_err(W32_ERROR_INVALID_PARAMETER); HLE_RET64(c, W32_INVALID_HANDLE); return; }
    uint64_t hv = find_open_utf8(pat);
    if (hv == W32_INVALID_HANDLE) { k32_set_err(W32_ERROR_FILE_NOT_FOUND); HLE_RET64(c, W32_INVALID_HANDLE); return; }
    K32Handle *h = lookup_handle(hv, HT_FIND);
    fill_find_data_a(&h->u.find.entries[h->u.find.index++], out_va);
    HLE_RET64(c, hv);
}

static void k32_FindNextFileA(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_FIND);
    uint64_t out_va = HLE_ARG1(c);
    if (!h || !out_va || h->u.find.index >= h->u.find.count) {
        k32_set_err(W32_ERROR_NO_MORE_FILES);
        HLE_RET32(c, 0);
        return;
    }
    fill_find_data_a(&h->u.find.entries[h->u.find.index++], out_va);
    HLE_RET32(c, 1u);
}

static void k32_FindClose(CPU *c) { HLE_RET32(c, close_k32_handle(HLE_ARG0(c)) ? 1u : 0u); }

static void k32_GetCurrentDirectoryW(CPU *c) {
    uint32_t cap = (uint32_t)HLE_ARG0(c);
    uint16_t *buf = (uint16_t *)(uintptr_t)HLE_ARG1(c);
    const char *dir = se_hle_get_game_dir();
    int need = se_utf8_to_utf16(dir, -1, NULL, 0);
    if (!buf || (int)cap < need) { HLE_RET32(c, (uint32_t)need); return; }
    int w = se_utf8_to_utf16(dir, -1, buf, (int)cap);
    HLE_RET32(c, w > 0 ? (uint32_t)(w - 1) : 0u);
}

static void k32_SetCurrentDirectoryW(CPU *c) {
    (void)c;
    HLE_RET32(c, 1u);
}

static void k32_GetFullPathNameW(CPU *c) {
    const uint16_t *wfn = (const uint16_t *)(uintptr_t)HLE_ARG0(c);
    uint32_t cap = (uint32_t)HLE_ARG1(c);
    uint16_t *out_w = (uint16_t *)(uintptr_t)HLE_ARG2(c);
    uint64_t file_part_va = HLE_ARG3(c);
    char resolved[1024];
    if (!se_vfs_resolve_wpath(wfn, false, resolved, sizeof(resolved))) { HLE_RET32(c, 0); return; }
    int need = se_utf8_to_utf16(resolved, -1, NULL, 0);
    if (!out_w || (int)cap < need) { HLE_RET32(c, (uint32_t)need); return; }
    int w = se_utf8_to_utf16(resolved, -1, out_w, (int)cap);
    if (file_part_va) {
        uint16_t *last = out_w;
        for (uint16_t *q = out_w; *q; ++q) if (*q == '/' || *q == '\\') last = q + 1;
        wr64(file_part_va, (uint64_t)(uintptr_t)last);
    }
    HLE_RET32(c, w > 0 ? (uint32_t)(w - 1) : 0u);
}

static void k32_GetDriveTypeW(CPU *c) { HLE_RET32(c, 3u); } /* DRIVE_FIXED */
static void k32_GetSystemDirectoryW(CPU *c) {
    uint16_t *buf = (uint16_t *)(uintptr_t)HLE_ARG0(c);
    uint32_t cap = (uint32_t)HLE_ARG1(c);
    const char *sys = "/system/bin";
    int need = se_utf8_to_utf16(sys, -1, NULL, 0);
    if (!buf || (int)cap < need) { HLE_RET32(c, (uint32_t)need); return; }
    int w = se_utf8_to_utf16(sys, -1, buf, (int)cap);
    HLE_RET32(c, w > 0 ? (uint32_t)(w - 1) : 0u);
}
static void k32_SearchPathW(CPU *c) { HLE_RET32(c, 0); }

static void k32_GetStdHandle(CPU *c) {
    int32_t n = (int32_t)(uint32_t)HLE_ARG0(c);
    if (n == -10) HLE_RET64(c, g_std_in_handle);
    else if (n == -11) HLE_RET64(c, g_std_out_handle);
    else if (n == -12) HLE_RET64(c, g_std_err_handle);
    else HLE_RET64(c, W32_INVALID_HANDLE);
}
static void k32_SetStdHandle(CPU *c) {
    int32_t n = (int32_t)(uint32_t)HLE_ARG0(c);
    uint64_t h = HLE_ARG1(c);
    if (n == -10) g_std_in_handle = h;
    else if (n == -11) g_std_out_handle = h;
    else if (n == -12) g_std_err_handle = h;
    HLE_RET32(c, 1u);
}
static void k32_WriteConsoleW(CPU *c) {
    const uint16_t *ws = (const uint16_t *)(uintptr_t)HLE_ARG1(c);
    uint32_t len = (uint32_t)HLE_ARG2(c);
    uint64_t out_va = HLE_ARG3(c);
    if (ws && len) {
        char *tmp = (char *)malloc(len * 4u + 1u);
        if (tmp) {
            int n = se_utf16_to_utf8(ws, (int)len, tmp, (int)(len * 4u + 1u));
            if (n > 0) fwrite(tmp, 1, (size_t)n, stdout);
            free(tmp);
        }
    }
    if (out_va) wr32(out_va, len);
    HLE_RET32(c, 1u);
}
static void k32_ReadConsoleW(CPU *c) { (void)c; HLE_RET32(c, 0); }
static void k32_GetConsoleMode(CPU *c) { (void)c; k32_set_err(W32_ERROR_INVALID_HANDLE); HLE_RET32(c, 0); }
static void k32_GetConsoleOutputCP(CPU *c) { HLE_RET32(c, 65001u); }
static void k32_PeekNamedPipe(CPU *c) { (void)c; HLE_RET32(c, 0); }

/* ---- Strings / CodePage / Locale ----------------------------------------- */

static void k32_MultiByteToWideChar(CPU *c) {
    const char *src = (const char *)(uintptr_t)HLE_ARG2(c);
    int slen = (int)(uint32_t)HLE_ARG3(c);
    uint16_t *dst = (uint16_t *)(uintptr_t)HLE_ARG4(c);
    int dcap = (int)(uint32_t)HLE_ARG5(c);
    if (!src) { k32_set_err(W32_ERROR_INVALID_PARAMETER); HLE_RET32(c, 0); return; }
    int res = se_utf8_to_utf16(src, slen, dst, (dcap > 0) ? dcap : 0);
    if (dst && dcap > 0 && res > dcap) { k32_set_err(W32_ERROR_INSUFFICIENT_BUF); HLE_RET32(c, 0); return; }
    HLE_RET32(c, (uint32_t)res);
}

static void k32_WideCharToMultiByte(CPU *c) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)HLE_ARG2(c);
    int slen = (int)(uint32_t)HLE_ARG3(c);
    char *dst = (char *)(uintptr_t)HLE_ARG4(c);
    int dcap = (int)(uint32_t)HLE_ARG5(c);
    if (!src) { k32_set_err(W32_ERROR_INVALID_PARAMETER); HLE_RET32(c, 0); return; }
    int res = se_utf16_to_utf8(src, slen, dst, (dcap > 0) ? dcap : 0);
    if (dst && dcap > 0 && res > dcap) { k32_set_err(W32_ERROR_INSUFFICIENT_BUF); HLE_RET32(c, 0); return; }
    HLE_RET32(c, (uint32_t)res);
}

static void k32_GetACP(CPU *c) { HLE_RET32(c, 65001u); }
static void k32_GetOEMCP(CPU *c) { HLE_RET32(c, 65001u); }
static void k32_IsValidCodePage(CPU *c) { HLE_RET32(c, 1u); }
static void k32_GetCPInfo(CPU *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)HLE_ARG1(c);
    if (!p) { HLE_RET32(c, 0); return; }
    memset(p, 0, 20u);
    uint32_t mcs = 4u;
    memcpy(p, &mcs, 4);
    p[4] = '?';
    HLE_RET32(c, 1u);
}
static void k32_IsValidLocale(CPU *c) { HLE_RET32(c, 1u); }
static void k32_GetUserDefaultLCID(CPU *c) { HLE_RET32(c, 0x0409u); }
static void k32_EnumSystemLocalesW(CPU *c) {
    uint64_t cb = HLE_ARG0(c);
    if (cb) {
        uint64_t s = se_hle_intern_utf16("en-US");
        (void)se_hle_call_guest(c, cb, 1, s, 0, 0, 0);
    }
    HLE_RET32(c, 1u);
}
static void k32_GetLocaleInfoW(CPU *c) {
    uint16_t *dst = (uint16_t *)(uintptr_t)HLE_ARG2(c);
    int cap = (int)(uint32_t)HLE_ARG3(c);
    const char *val = "en-US";
    int n = se_utf8_to_utf16(val, -1, dst, cap);
    HLE_RET32(c, (uint32_t)n);
}
static void k32_LCMapStringW(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG1(c);
    const uint16_t *src = (const uint16_t *)(uintptr_t)HLE_ARG2(c);
    int srclen = (int)(uint32_t)HLE_ARG3(c);
    uint16_t *dst = (uint16_t *)(uintptr_t)HLE_ARG4(c);
    int dcap = (int)(uint32_t)HLE_ARG5(c);
    if (!src) { HLE_RET32(c, 0); return; }
    int n = (srclen < 0) ? (int)se_utf16_len(src) + 1 : srclen;
    if (!dst || dcap == 0) { HLE_RET32(c, (uint32_t)n); return; }
    int copy = (n < dcap) ? n : dcap;
    for (int i = 0; i < copy; ++i) {
        uint16_t ch = src[i];
        if ((flags & 0x0100u) && ch >= 'A' && ch <= 'Z') ch = (uint16_t)(ch - 'A' + 'a');
        else if ((flags & 0x0200u) && ch >= 'a' && ch <= 'z') ch = (uint16_t)(ch - 'a' + 'A');
        dst[i] = ch;
    }
    HLE_RET32(c, (uint32_t)copy);
}
static void k32_CompareStringW(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG1(c);
    const uint16_t *s1 = (const uint16_t *)(uintptr_t)HLE_ARG2(c);
    int n1 = (int)(uint32_t)HLE_ARG3(c);
    const uint16_t *s2 = (const uint16_t *)(uintptr_t)HLE_ARG4(c);
    int n2 = (int)(uint32_t)HLE_ARG5(c);
    if (!s1 || !s2) { HLE_RET32(c, 0); return; }
    int l1 = (n1 < 0) ? (int)se_utf16_len(s1) : n1;
    int l2 = (n2 < 0) ? (int)se_utf16_len(s2) : n2;
    int m = (l1 < l2) ? l1 : l2;
    for (int i = 0; i < m; ++i) {
        uint16_t a = s1[i], b = s2[i];
        if (flags & 1u) {
            if (a >= 'A' && a <= 'Z') a = (uint16_t)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (uint16_t)(b - 'A' + 'a');
        }
        if (a < b) { HLE_RET32(c, 1u); return; } /* CSTR_LESS_THAN */
        if (a > b) { HLE_RET32(c, 3u); return; } /* CSTR_GREATER_THAN */
    }
    HLE_RET32(c, l1 < l2 ? 1u : (l1 > l2 ? 3u : 2u)); /* CSTR_EQUAL=2 */
}
static void k32_GetStringTypeW(CPU *c) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)HLE_ARG1(c);
    int n = (int)(uint32_t)HLE_ARG2(c);
    uint16_t *out = (uint16_t *)(uintptr_t)HLE_ARG3(c);
    if (!src || !out) { HLE_RET32(c, 0); return; }
    if (n < 0) n = (int)se_utf16_len(src) + 1;
    for (int i = 0; i < n; ++i) {
        uint16_t ch = src[i], t = 0;
        if (ch >= 'A' && ch <= 'Z') t = 0x0101u; /* C1_UPPER | C1_ALPHA */
        else if (ch >= 'a' && ch <= 'z') t = 0x0102u; /* C1_LOWER | C1_ALPHA */
        else if (ch >= '0' && ch <= '9') t = 0x0004u; /* C1_DIGIT */
        else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') t = 0x0008u; /* C1_SPACE */
        else if (ch < 32) t = 0x0020u;
        out[i] = t;
    }
    HLE_RET32(c, 1u);
}

/* ---- Synchronization, Critical Sections, SList, TLS, FLS, Threads -------- */

#define CS_MAGIC 0x53454353u
static pthread_mutex_t g_cs_init_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *ensure_cs_mutex(uint64_t cs_va) {
    if (!cs_va) return NULL;
    uint32_t magic = rd32(cs_va + 8ull);
    uint64_t mptr = rd64(cs_va);
    if (magic == CS_MAGIC && mptr) {
        return (pthread_mutex_t *)(uintptr_t)mptr;
    }
    pthread_mutex_lock(&g_cs_init_lock);
    magic = rd32(cs_va + 8ull);
    mptr = rd64(cs_va);
    if (magic != CS_MAGIC || !mptr) {
        pthread_mutex_t *pm = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(pm, &attr);
        pthread_mutexattr_destroy(&attr);
        wr64(cs_va, (uint64_t)(uintptr_t)pm);
        wr32(cs_va + 8ull, CS_MAGIC);
        mptr = (uint64_t)(uintptr_t)pm;
    }
    pthread_mutex_unlock(&g_cs_init_lock);
    return (pthread_mutex_t *)(uintptr_t)mptr;
}

static void k32_InitializeCriticalSection(CPU *c) {
    (void)ensure_cs_mutex(HLE_ARG0(c));
    HLE_RET32(c, 1u);
}
static void k32_EnterCriticalSection(CPU *c) {
    pthread_mutex_t *pm = ensure_cs_mutex(HLE_ARG0(c));
    if (pm) pthread_mutex_lock(pm);
}
static void k32_TryEnterCriticalSection(CPU *c) {
    pthread_mutex_t *pm = ensure_cs_mutex(HLE_ARG0(c));
    HLE_RET32(c, (pm && pthread_mutex_trylock(pm) == 0) ? 1u : 0u);
}
static void k32_LeaveCriticalSection(CPU *c) {
    pthread_mutex_t *pm = ensure_cs_mutex(HLE_ARG0(c));
    if (pm) pthread_mutex_unlock(pm);
}
static void k32_DeleteCriticalSection(CPU *c) {
    uint64_t cs_va = HLE_ARG0(c);
    if (!cs_va) return;
    pthread_mutex_lock(&g_cs_init_lock);
    if (rd32(cs_va + 8ull) == CS_MAGIC) {
        pthread_mutex_t *pm = (pthread_mutex_t *)(uintptr_t)rd64(cs_va);
        wr64(cs_va, 0);
        wr32(cs_va + 8ull, 0);
        pthread_mutex_unlock(&g_cs_init_lock);
        if (pm) {
            pthread_mutex_destroy(pm);
            free(pm);
        }
        return;
    }
    pthread_mutex_unlock(&g_cs_init_lock);
}

/* Events & Semaphores & Wait */
static uint64_t create_event_common(bool manual, bool initial, const uint16_t *wname) {
    char name[128] = "";
    if (wname) se_utf16_to_utf8(wname, -1, name, (int)sizeof(name));
    if (name[0]) {
        pthread_mutex_lock(&g_handle_lock);
        for (uint32_t i = 1; i < K32_MAX_HANDLES; ++i) {
            K32Handle *ex = g_handles[i];
            if (ex && ex->type == HT_EVENT && strcmp(ex->u.event.name, name) == 0) {
                ex->ref_count++;
                pthread_mutex_unlock(&g_handle_lock);
                k32_set_err(W32_ERROR_ALREADY_EXISTS);
                return K32_HANDLE_BASE + ((uint64_t)i * 4ull);
            }
        }
        pthread_mutex_unlock(&g_handle_lock);
    }
    K32Handle *h = NULL;
    uint64_t hv = alloc_handle(HT_EVENT, &h);
    if (!hv) return 0;
    pthread_mutex_init(&h->u.event.mu, NULL);
    pthread_cond_init(&h->u.event.cv, NULL);
    h->u.event.manual_reset = manual;
    h->u.event.signaled = initial;
    snprintf(h->u.event.name, sizeof(h->u.event.name), "%s", name);
    k32_set_err(W32_ERROR_SUCCESS);
    return hv;
}

static void k32_CreateEventW(CPU *c) {
    HLE_RET64(c, create_event_common(HLE_ARG1(c) != 0, HLE_ARG2(c) != 0,
                                     (const uint16_t *)(uintptr_t)HLE_ARG3(c)));
}

static void k32_CreateEventExW(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG2(c);
    HLE_RET64(c, create_event_common((flags & 1u) != 0, (flags & 2u) != 0,
                                     (const uint16_t *)(uintptr_t)HLE_ARG1(c)));
}

static void k32_OpenEventW(CPU *c) {
    const uint16_t *wname = (const uint16_t *)(uintptr_t)HLE_ARG2(c);
    if (!wname) { k32_set_err(W32_ERROR_FILE_NOT_FOUND); HLE_RET64(c, 0); return; }
    char name[128];
    se_utf16_to_utf8(wname, -1, name, (int)sizeof(name));
    pthread_mutex_lock(&g_handle_lock);
    for (uint32_t i = 1; i < K32_MAX_HANDLES; ++i) {
        K32Handle *ex = g_handles[i];
        if (ex && ex->type == HT_EVENT && strcmp(ex->u.event.name, name) == 0) {
            ex->ref_count++;
            pthread_mutex_unlock(&g_handle_lock);
            HLE_RET64(c, K32_HANDLE_BASE + ((uint64_t)i * 4ull));
            return;
        }
    }
    pthread_mutex_unlock(&g_handle_lock);
    k32_set_err(W32_ERROR_FILE_NOT_FOUND);
    HLE_RET64(c, 0);
}

static void k32_SetEvent(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_EVENT);
    if (!h) { HLE_RET32(c, 0); return; }
    pthread_mutex_lock(&h->u.event.mu);
    h->u.event.signaled = true;
    if (h->u.event.manual_reset) pthread_cond_broadcast(&h->u.event.cv);
    else pthread_cond_signal(&h->u.event.cv);
    pthread_mutex_unlock(&h->u.event.mu);
    HLE_RET32(c, 1u);
}

static void k32_ResetEvent(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_EVENT);
    if (!h) { HLE_RET32(c, 0); return; }
    pthread_mutex_lock(&h->u.event.mu);
    h->u.event.signaled = false;
    pthread_mutex_unlock(&h->u.event.mu);
    HLE_RET32(c, 1u);
}

static uint64_t create_semaphore_common(int32_t init_cnt, int32_t max_cnt, const uint16_t *wname) {
    if (max_cnt <= 0 || init_cnt < 0 || init_cnt > max_cnt) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char name[128] = "";
    if (wname) se_utf16_to_utf8(wname, -1, name, (int)sizeof(name));
    if (name[0]) {
        pthread_mutex_lock(&g_handle_lock);
        for (uint32_t i = 1; i < K32_MAX_HANDLES; ++i) {
            K32Handle *ex = g_handles[i];
            if (ex && ex->type == HT_SEMAPHORE && strcmp(ex->u.sem.name, name) == 0) {
                ex->ref_count++;
                pthread_mutex_unlock(&g_handle_lock);
                k32_set_err(W32_ERROR_ALREADY_EXISTS);
                return K32_HANDLE_BASE + ((uint64_t)i * 4ull);
            }
        }
        pthread_mutex_unlock(&g_handle_lock);
    }
    K32Handle *h = NULL;
    uint64_t hv = alloc_handle(HT_SEMAPHORE, &h);
    if (!hv) return 0;
    pthread_mutex_init(&h->u.sem.mu, NULL);
    pthread_cond_init(&h->u.sem.cv, NULL);
    h->u.sem.count = init_cnt;
    h->u.sem.max_count = max_cnt;
    snprintf(h->u.sem.name, sizeof(h->u.sem.name), "%s", name);
    k32_set_err(W32_ERROR_SUCCESS);
    return hv;
}

static void k32_CreateSemaphoreW(CPU *c) {
    HLE_RET64(c, create_semaphore_common((int32_t)(uint32_t)HLE_ARG1(c),
                                         (int32_t)(uint32_t)HLE_ARG2(c),
                                         (const uint16_t *)(uintptr_t)HLE_ARG3(c)));
}

static void k32_ReleaseSemaphore(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_SEMAPHORE);
    int32_t rel = (int32_t)(uint32_t)HLE_ARG1(c);
    uint64_t prev_va = HLE_ARG2(c);
    if (!h || rel <= 0) { HLE_RET32(c, 0); return; }
    pthread_mutex_lock(&h->u.sem.mu);
    if (prev_va) wr32(prev_va, (uint32_t)h->u.sem.count);
    h->u.sem.count += rel;
    pthread_cond_broadcast(&h->u.sem.cv);
    pthread_mutex_unlock(&h->u.sem.mu);
    HLE_RET32(c, 1u);
}

static void make_abs_timeout(uint32_t ms, struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000u;
    ts->tv_nsec += (long)((ms % 1000u) * 1000000L);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static uint32_t wait_handle_ms(uint64_t hv, uint32_t ms) {
    K32Handle *h = lookup_handle(hv, HT_NONE);
    if (!h) return W32_WAIT_FAILED;
    if (h->type == HT_EVENT) {
        pthread_mutex_lock(&h->u.event.mu);
        if (!h->u.event.signaled) {
            if (ms == 0) { pthread_mutex_unlock(&h->u.event.mu); return W32_WAIT_TIMEOUT; }
            if (ms == W32_INFINITE) {
                while (!h->u.event.signaled) pthread_cond_wait(&h->u.event.cv, &h->u.event.mu);
            } else {
                struct timespec ts;
                make_abs_timeout(ms, &ts);
                int rc = 0;
                while (!h->u.event.signaled && rc == 0) {
                    rc = pthread_cond_timedwait(&h->u.event.cv, &h->u.event.mu, &ts);
                }
                if (!h->u.event.signaled) {
                    pthread_mutex_unlock(&h->u.event.mu);
                    return W32_WAIT_TIMEOUT;
                }
            }
        }
        if (!h->u.event.manual_reset) h->u.event.signaled = false;
        pthread_mutex_unlock(&h->u.event.mu);
        return W32_WAIT_OBJECT_0;
    }
    if (h->type == HT_THREAD) {
        pthread_mutex_lock(&h->u.thread.mu);
        if (!h->u.thread.finished) {
            if (ms == 0) { pthread_mutex_unlock(&h->u.thread.mu); return W32_WAIT_TIMEOUT; }
            if (ms == W32_INFINITE) {
                while (!h->u.thread.finished) pthread_cond_wait(&h->u.thread.cv, &h->u.thread.mu);
            } else {
                struct timespec ts;
                make_abs_timeout(ms, &ts);
                int rc = 0;
                while (!h->u.thread.finished && rc == 0) {
                    rc = pthread_cond_timedwait(&h->u.thread.cv, &h->u.thread.mu, &ts);
                }
                if (!h->u.thread.finished) {
                    pthread_mutex_unlock(&h->u.thread.mu);
                    return W32_WAIT_TIMEOUT;
                }
            }
        }
        pthread_mutex_unlock(&h->u.thread.mu);
        return W32_WAIT_OBJECT_0;
    }
    if (h->type == HT_SEMAPHORE) {
        pthread_mutex_lock(&h->u.sem.mu);
        if (h->u.sem.count <= 0) {
            if (ms == 0) { pthread_mutex_unlock(&h->u.sem.mu); return W32_WAIT_TIMEOUT; }
            if (ms == W32_INFINITE) {
                while (h->u.sem.count <= 0) pthread_cond_wait(&h->u.sem.cv, &h->u.sem.mu);
            } else {
                struct timespec ts;
                make_abs_timeout(ms, &ts);
                int rc = 0;
                while (h->u.sem.count <= 0 && rc == 0) {
                    rc = pthread_cond_timedwait(&h->u.sem.cv, &h->u.sem.mu, &ts);
                }
                if (h->u.sem.count <= 0) {
                    pthread_mutex_unlock(&h->u.sem.mu);
                    return W32_WAIT_TIMEOUT;
                }
            }
        }
        h->u.sem.count--;
        pthread_mutex_unlock(&h->u.sem.mu);
        return W32_WAIT_OBJECT_0;
    }
    return W32_WAIT_OBJECT_0;
}

static void k32_WaitForSingleObject(CPU *c) {
    HLE_RET32(c, wait_handle_ms(HLE_ARG0(c), (uint32_t)HLE_ARG1(c)));
}
static void k32_SignalObjectAndWait(CPU *c) {
    uint64_t to_sig = HLE_ARG0(c);
    uint64_t to_wait = HLE_ARG1(c);
    uint32_t ms = (uint32_t)HLE_ARG2(c);
    K32Handle *h = lookup_handle(to_sig, HT_NONE);
    if (h && h->type == HT_EVENT) {
        pthread_mutex_lock(&h->u.event.mu);
        h->u.event.signaled = true;
        pthread_cond_signal(&h->u.event.cv);
        pthread_mutex_unlock(&h->u.event.mu);
    }
    HLE_RET32(c, wait_handle_ms(to_wait, ms));
}

/* TLS / FLS */
#define MAX_WIN32_TLS 512u
static pthread_mutex_t g_tls_bitmap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_tls_in_use[MAX_WIN32_TLS];
static pthread_key_t g_tls_keys[MAX_WIN32_TLS];
static uint64_t g_fls_callbacks[MAX_WIN32_TLS];

static void k32_TlsAlloc(CPU *c) {
    pthread_mutex_lock(&g_tls_bitmap_lock);
    for (uint32_t i = 0; i < MAX_WIN32_TLS; ++i) {
        if (!g_tls_in_use[i]) {
            if (pthread_key_create(&g_tls_keys[i], NULL) == 0) {
                g_tls_in_use[i] = 1;
                g_fls_callbacks[i] = 0;
                pthread_mutex_unlock(&g_tls_bitmap_lock);
                HLE_RET32(c, i);
                return;
            }
        }
    }
    pthread_mutex_unlock(&g_tls_bitmap_lock);
    HLE_RET32(c, 0xFFFFFFFFu); /* TLS_OUT_OF_INDEXES */
}
static void k32_FlsAlloc(CPU *c) {
    uint64_t cb = HLE_ARG0(c);
    k32_TlsAlloc(c);
    uint32_t idx = (uint32_t)c->rax;
    if (idx < MAX_WIN32_TLS) g_fls_callbacks[idx] = cb;
}
static void k32_TlsGetValue(CPU *c) {
    uint32_t idx = (uint32_t)HLE_ARG0(c);
    if (idx >= MAX_WIN32_TLS || !g_tls_in_use[idx]) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET64(c, 0);
        return;
    }
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET_PTR(c, pthread_getspecific(g_tls_keys[idx]));
}
static void k32_TlsSetValue(CPU *c) {
    uint32_t idx = (uint32_t)HLE_ARG0(c);
    uint64_t val = HLE_ARG1(c);
    if (idx >= MAX_WIN32_TLS || !g_tls_in_use[idx]) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0);
        return;
    }
    pthread_setspecific(g_tls_keys[idx], (const void *)(uintptr_t)val);
    HLE_RET32(c, 1u);
}
static void k32_TlsFree(CPU *c) {
    uint32_t idx = (uint32_t)HLE_ARG0(c);
    if (idx >= MAX_WIN32_TLS || !g_tls_in_use[idx]) { HLE_RET32(c, 0); return; }
    pthread_mutex_lock(&g_tls_bitmap_lock);
    g_tls_in_use[idx] = 0;
    g_fls_callbacks[idx] = 0;
    pthread_key_delete(g_tls_keys[idx]);
    pthread_mutex_unlock(&g_tls_bitmap_lock);
    HLE_RET32(c, 1u);
}

/* Win64 Lock-Free SLIST_HEADER (striped lock over exact 16-byte layout) */
static pthread_mutex_t g_slist_locks[64];
static pthread_once_t  g_slist_once = PTHREAD_ONCE_INIT;
static void init_slist_locks(void) {
    for (int i = 0; i < 64; ++i) pthread_mutex_init(&g_slist_locks[i], NULL);
}
static pthread_mutex_t *slist_lock_for(uint64_t head_va) {
    pthread_once(&g_slist_once, init_slist_locks);
    return &g_slist_locks[(head_va >> 4) & 63u];
}

static void k32_InitializeSListHead(CPU *c) {
    uint64_t head = HLE_ARG0(c);
    if (head) { wr64(head, 0); wr64(head + 8ull, 0); }
}
static void k32_QueryDepthSList(CPU *c) {
    uint64_t head = HLE_ARG0(c);
    if (!head) { HLE_RET32(c, 0); return; }
    pthread_mutex_t *mu = slist_lock_for(head);
    pthread_mutex_lock(mu);
    uint64_t align = rd64(head);
    pthread_mutex_unlock(mu);
    HLE_RET32(c, (uint32_t)(align & 0xFFFFu));
}
static void k32_InterlockedPushEntrySList(CPU *c) {
    uint64_t head = HLE_ARG0(c);
    uint64_t entry = HLE_ARG1(c);
    if (!head || !entry) { HLE_RET64(c, 0); return; }
    pthread_mutex_t *mu = slist_lock_for(head);
    pthread_mutex_lock(mu);
    uint64_t align = rd64(head);
    uint64_t region = rd64(head + 8ull);
    uint64_t old_first = region & ~0xFull;
    wr64(entry, old_first);
    uint64_t depth = ((align & 0xFFFFu) + 1u) & 0xFFFFu;
    uint64_t seq = ((align >> 16) + 1ull) & 0xFFFFFFFFFFFFull;
    wr64(head, depth | (seq << 16));
    wr64(head + 8ull, (entry & ~0xFull) | (region & 0xFull));
    pthread_mutex_unlock(mu);
    HLE_RET64(c, old_first);
}
static void k32_InterlockedPopEntrySList(CPU *c) {
    uint64_t head = HLE_ARG0(c);
    if (!head) { HLE_RET64(c, 0); return; }
    pthread_mutex_t *mu = slist_lock_for(head);
    pthread_mutex_lock(mu);
    uint64_t align = rd64(head);
    uint64_t region = rd64(head + 8ull);
    uint64_t first = region & ~0xFull;
    if (first) {
        uint64_t next = rd64(first);
        uint64_t depth = align & 0xFFFFu;
        if (depth > 0) --depth;
        uint64_t seq = ((align >> 16) + 1ull) & 0xFFFFFFFFFFFFull;
        wr64(head, depth | (seq << 16));
        wr64(head + 8ull, (next & ~0xFull) | (region & 0xFull));
    }
    pthread_mutex_unlock(mu);
    HLE_RET64(c, first);
}
static void k32_InterlockedFlushSList(CPU *c) {
    uint64_t head = HLE_ARG0(c);
    if (!head) { HLE_RET64(c, 0); return; }
    pthread_mutex_t *mu = slist_lock_for(head);
    pthread_mutex_lock(mu);
    uint64_t align = rd64(head);
    uint64_t region = rd64(head + 8ull);
    uint64_t first = region & ~0xFull;
    uint64_t seq = ((align >> 16) + 1ull) & 0xFFFFFFFFFFFFull;
    wr64(head, seq << 16);
    wr64(head + 8ull, region & 0xFull);
    pthread_mutex_unlock(mu);
    HLE_RET64(c, first);
}

/* Threads */
static void *k32_thread_trampoline(void *arg) {
    K32Handle *h = (K32Handle *)arg;
    size_t st_sz = h->u.thread.stack_size ? h->u.thread.stack_size : (4u * 1024u * 1024u);
    uint8_t *stack = (uint8_t *)malloc(st_sz);
    CPU wc;
    memset(&wc, 0, sizeof(wc));
    wc.mxcsr = 0x1F80u;
    if (stack) {
        uint64_t base = (uint64_t)(uintptr_t)(stack + st_sz);
        uint64_t limit = (uint64_t)(uintptr_t)stack;
        se_win64_teb_set_stack(base, limit);
        wc.rsp = (base - 0x40ull) & ~15ull;
        wr64(wc.rsp, 0ull);
    }
    h->u.thread.tid = se_win64_teb_get_tid();
    wc.rcx = h->u.thread.param;
    dispatch(&wc, h->u.thread.start_va);
    uint32_t exit_code = (uint32_t)wc.rax;
    free(stack);

    pthread_mutex_lock(&h->u.thread.mu);
    h->u.thread.exit_code = exit_code;
    h->u.thread.finished = true;
    pthread_cond_broadcast(&h->u.thread.cv);
    pthread_mutex_unlock(&h->u.thread.mu);
    return NULL;
}

static void k32_CreateThread(CPU *c) {
    size_t stack_sz = (size_t)HLE_ARG1(c);
    uint64_t start_va = HLE_ARG2(c);
    uint64_t param = HLE_ARG3(c);
    uint64_t out_tid_va = HLE_ARG5(c);
    if (!start_va) { k32_set_err(W32_ERROR_INVALID_PARAMETER); HLE_RET64(c, 0); return; }

    K32Handle *h = NULL;
    uint64_t hv = alloc_handle(HT_THREAD, &h);
    if (!hv) { k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY); HLE_RET64(c, 0); return; }
    pthread_mutex_init(&h->u.thread.mu, NULL);
    pthread_cond_init(&h->u.thread.cv, NULL);
    h->u.thread.start_va = start_va;
    h->u.thread.param = param;
    h->u.thread.stack_size = (stack_sz >= 1024u * 1024u) ? stack_sz : (4u * 1024u * 1024u);

    if (pthread_create(&h->u.thread.pt, NULL, k32_thread_trampoline, h) != 0) {
        close_k32_handle(hv);
        k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY);
        HLE_RET64(c, 0);
        return;
    }
    if (out_tid_va) wr32(out_tid_va, 1001u);
    HLE_RET64(c, hv);
}

static void k32_ExitThread(CPU *c) {
    se_hle_suppress_ret_pop_once();
    pthread_exit((void *)(uintptr_t)HLE_ARG0(c));
}
static void k32_GetExitCodeThread(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_THREAD);
    uint64_t out_va = HLE_ARG1(c);
    if (!h || !out_va) { HLE_RET32(c, 0); return; }
    pthread_mutex_lock(&h->u.thread.mu);
    wr32(out_va, h->u.thread.finished ? h->u.thread.exit_code : 259u); /* STILL_ACTIVE */
    pthread_mutex_unlock(&h->u.thread.mu);
    HLE_RET32(c, 1u);
}

static void k32_GetCurrentThread(CPU *c) { HLE_RET64(c, K32_CUR_THREAD); }
static void k32_GetCurrentThreadId(CPU *c) { HLE_RET32(c, se_win64_teb_get_tid()); }
static void k32_GetCurrentProcess(CPU *c) { HLE_RET64(c, K32_CUR_PROCESS); }
static void k32_GetCurrentProcessId(CPU *c) { HLE_RET32(c, (uint32_t)getpid()); }
static void k32_GetThreadPriority(CPU *c) { HLE_RET32(c, 0); }
static void k32_SetThreadPriority(CPU *c) { HLE_RET32(c, 1u); }
static void k32_SetThreadAffinityMask(CPU *c) { HLE_RET64(c, 1ull); }
static void k32_GetThreadTimes(CPU *c) {
    uint64_t pts[4] = { HLE_ARG1(c), HLE_ARG2(c), HLE_ARG3(c), HLE_ARG4(c) };
    for (int i = 0; i < 4; ++i) {
        if (pts[i]) wr64(pts[i], 0);
    }
    HLE_RET32(c, 1u);
}
static void k32_SwitchToThread(CPU *c) { sched_yield(); HLE_RET32(c, 1u); }
static void k32_Sleep(CPU *c) {
    uint32_t ms = (uint32_t)HLE_ARG0(c);
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { (time_t)(ms / 1000u), (long)((ms % 1000u) * 1000000L) };
    nanosleep(&ts, NULL);
}

/* Timers / Wait registration stubs (safe no-ops returning valid pseudo-handles) */
static void k32_CreateTimerQueue(CPU *c) { HLE_RET64(c, 0x20001ull); }
static void k32_CreateTimerQueueTimer(CPU *c) { if (HLE_ARG0(c)) wr64(HLE_ARG0(c), 0x20002ull); HLE_RET32(c, 1u); }
static void k32_ChangeTimerQueueTimer(CPU *c) { HLE_RET32(c, 1u); }
static void k32_DeleteTimerQueueTimer(CPU *c) { HLE_RET32(c, 1u); }
static void k32_RegisterWaitForSingleObject(CPU *c) { if (HLE_ARG0(c)) wr64(HLE_ARG0(c), 0x20003ull); HLE_RET32(c, 1u); }
static void k32_UnregisterWait(CPU *c) { HLE_RET32(c, 1u); }
static void k32_UnregisterWaitEx(CPU *c) { HLE_RET32(c, 1u); }
static void k32_DuplicateHandle(CPU *c) {
    uint64_t src = HLE_ARG1(c), dst_va = HLE_ARG3(c);
    K32Handle *h = lookup_handle(src, HT_NONE);
    if (h) {
        pthread_mutex_lock(&g_handle_lock);
        h->ref_count++;
        pthread_mutex_unlock(&g_handle_lock);
    }
    if (dst_va) wr64(dst_va, src);
    HLE_RET32(c, 1u);
}
static void k32_CloseHandle(CPU *c) {
    uint64_t hv = HLE_ARG0(c);
    if (hv >= 0x20000ull && hv < 0x30000ull) { HLE_RET32(c, 1u); return; }
    HLE_RET32(c, close_k32_handle(hv) ? 1u : 0u);
}

/* ---- Process, Time, SysInfo, Modules, Exceptions ------------------------- */

static void k32_ExitProcess(CPU *c) {
    se_hle_suppress_ret_pop_once();
#if defined(__ANDROID__)
    K32_LOG("KERNEL32!ExitProcess(%u) exiting worker thread cleanly", (uint32_t)HLE_ARG0(c));
    pthread_exit((void *)(uintptr_t)(uint32_t)HLE_ARG0(c));
#else
    exit((int)(uint32_t)HLE_ARG0(c));
#endif
}
static void k32_CreateProcessA(CPU *c) { k32_set_err(W32_ERROR_NOT_SUPPORTED); HLE_RET32(c, 0); }

static void k32_GetCommandLineA(CPU *c) {
    HLE_RET64(c, se_hle_intern_ascii("\"SpaceEngine.exe\""));
}
static void k32_GetCommandLineW(CPU *c) {
    HLE_RET64(c, se_hle_intern_utf16("\"SpaceEngine.exe\""));
}
static void k32_GetEnvironmentStringsW(CPU *c) {
    static const uint16_t kEmptyEnv[] = { 'S', 'E', '=', '1', 0, 0 };
    uint16_t *p = (uint16_t *)malloc(sizeof(kEmptyEnv));
    if (p) memcpy(p, kEmptyEnv, sizeof(kEmptyEnv));
    HLE_RET_PTR(c, p);
}
static void k32_FreeEnvironmentStringsW(CPU *c) {
    free((void *)(uintptr_t)HLE_ARG0(c));
    HLE_RET32(c, 1u);
}
static void k32_SetEnvironmentVariableW(CPU *c) { HLE_RET32(c, 1u); }
static void k32_GetStartupInfoW(CPU *c) {
    uint64_t p = HLE_ARG0(c);
    if (p) { memset((void *)(uintptr_t)p, 0, 104u); wr32(p, 104u); }
}

static void k32_QueryPerformanceFrequency(CPU *c) {
    if (HLE_ARG0(c)) wr64(HLE_ARG0(c), 10000000ull);
    HLE_RET32(c, 1u);
}
static void k32_QueryPerformanceCounter(CPU *c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ticks = (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)(ts.tv_nsec / 100L);
    if (HLE_ARG0(c)) wr64(HLE_ARG0(c), ticks);
    HLE_RET32(c, 1u);
}
static void k32_GetTickCount64(CPU *c) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    HLE_RET64(c, (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L));
}
static void k32_GetTickCount(CPU *c) {
    k32_GetTickCount64(c);
    SET32(c->rax, (uint32_t)c->rax);
}
static void k32_GetSystemTimeAsFileTime(CPU *c) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (HLE_ARG0(c)) wr64(HLE_ARG0(c), unix_to_filetime(ts.tv_sec, ts.tv_nsec));
}
static void tm_to_systemtime(const struct tm *t, uint16_t ms, uint64_t out_va) {
    wr16(out_va + 0u, (uint16_t)(t->tm_year + 1900));
    wr16(out_va + 2u, (uint16_t)(t->tm_mon + 1));
    wr16(out_va + 4u, (uint16_t)t->tm_wday);
    wr16(out_va + 6u, (uint16_t)t->tm_mday);
    wr16(out_va + 8u, (uint16_t)t->tm_hour);
    wr16(out_va + 10u, (uint16_t)t->tm_min);
    wr16(out_va + 12u, (uint16_t)t->tm_sec);
    wr16(out_va + 14u, ms);
}
static void k32_GetSystemTime(CPU *c) {
    if (!HLE_ARG0(c)) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    memset(&t, 0, sizeof(t));
#if !defined(_WIN32)
    gmtime_r(&ts.tv_sec, &t);
#else
    gmtime_s(&t, &ts.tv_sec);
#endif
    tm_to_systemtime(&t, (uint16_t)(ts.tv_nsec / 1000000L), HLE_ARG0(c));
}
static void k32_GetLocalTime(CPU *c) {
    if (!HLE_ARG0(c)) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm t;
    memset(&t, 0, sizeof(t));
#if !defined(_WIN32)
    localtime_r(&ts.tv_sec, &t);
#else
    localtime_s(&t, &ts.tv_sec);
#endif
    tm_to_systemtime(&t, (uint16_t)(ts.tv_nsec / 1000000L), HLE_ARG0(c));
}
static void k32_FileTimeToSystemTime(CPU *c) {
    uint64_t ft_va = HLE_ARG0(c), st_va = HLE_ARG1(c);
    if (!ft_va || !st_va) { HLE_RET32(c, 0); return; }
    uint64_t ft = rd64(ft_va);
    time_t sec = (ft >= W32_FILETIME_EPOCH_DIFF) ? (time_t)((ft - W32_FILETIME_EPOCH_DIFF) / 10000000ull) : 0;
    uint16_t ms = (uint16_t)(((ft / 10000ull)) % 1000ull);
    struct tm t;
    memset(&t, 0, sizeof(t));
#if !defined(_WIN32)
    gmtime_r(&sec, &t);
#else
    gmtime_s(&t, &sec);
#endif
    tm_to_systemtime(&t, ms, st_va);
    HLE_RET32(c, 1u);
}
static void k32_SystemTimeToTzSpecificLocalTime(CPU *c) {
    if (HLE_ARG1(c) && HLE_ARG2(c)) {
        memcpy((void *)(uintptr_t)HLE_ARG2(c), (const void *)(uintptr_t)HLE_ARG1(c), 16u);
    }
    HLE_RET32(c, 1u);
}
static void k32_GetTimeZoneInformation(CPU *c) {
    uint64_t p = HLE_ARG0(c);
    if (p) {
        memset((void *)(uintptr_t)p, 0, 172u);
        se_utf8_to_utf16("UTC", -1, (uint16_t *)(uintptr_t)(p + 4u), 32);
        se_utf8_to_utf16("UTC", -1, (uint16_t *)(uintptr_t)(p + 88u), 32);
    }
    HLE_RET32(c, 0); /* TIME_ZONE_ID_UNKNOWN */
}

static uint32_t host_cpu_count(void) {
#if !defined(_WIN32)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)(n > 8 ? 8 : n) : 4u;
#else
    return 4u;
#endif
}

static void k32_GetSystemInfo(CPU *c) {
    uint64_t p = HLE_ARG0(c);
    if (!p) return;
    memset((void *)(uintptr_t)p, 0, 48u);
    uint32_t ncpu = host_cpu_count();
    wr16(p + 0x00u, 9u); /* PROCESSOR_ARCHITECTURE_AMD64 */
    wr32(p + 0x04u, 4096u);
    wr64(p + 0x08u, 0x10000ull);
    wr64(p + 0x10u, 0x7FFFFFFEFFFFull);
    wr64(p + 0x18u, (1ull << ncpu) - 1ull);
    wr32(p + 0x20u, ncpu);
    wr32(p + 0x24u, 8664u);
    wr32(p + 0x28u, 65536u);
    wr16(p + 0x2Cu, 6u);
}

static void k32_GetLogicalProcessorInformation(CPU *c) {
    uint64_t buf = HLE_ARG0(c);
    uint64_t len_va = HLE_ARG1(c);
    if (!len_va) { HLE_RET32(c, 0); return; }
    uint32_t ncpu = host_cpu_count();
    uint32_t need = 32u * ncpu;
    uint32_t cap = rd32(len_va);
    wr32(len_va, need);
    if (!buf || cap < need) {
        k32_set_err(W32_ERROR_INSUFFICIENT_BUF);
        HLE_RET32(c, 0);
        return;
    }
    memset((void *)(uintptr_t)buf, 0, need);
    for (uint32_t i = 0; i < ncpu; ++i) {
        wr64(buf + i * 32ull, 1ull << i);
        wr32(buf + i * 32ull + 8ull, 0u); /* RelationProcessorCore */
    }
    HLE_RET32(c, 1u);
}

static void k32_GetProcessAffinityMask(CPU *c) {
    uint64_t mask = (1ull << host_cpu_count()) - 1ull;
    if (HLE_ARG1(c)) wr64(HLE_ARG1(c), mask);
    if (HLE_ARG2(c)) wr64(HLE_ARG2(c), mask);
    HLE_RET32(c, 1u);
}
static void k32_GetNumaHighestNodeNumber(CPU *c) {
    if (HLE_ARG0(c)) wr32(HLE_ARG0(c), 0u);
    HLE_RET32(c, 1u);
}

static void k32_GlobalMemoryStatusEx(CPU *c) {
    uint64_t p = HLE_ARG0(c);
    if (!p) { HLE_RET32(c, 0); return; }
    uint64_t total = 8ull * 1024ull * 1024ull * 1024ull;
#if !defined(_WIN32)
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) total = (uint64_t)pages * (uint64_t)psize;
#endif
    wr32(p + 0x00u, 64u);
    wr32(p + 0x04u, 30u);
    wr64(p + 0x08u, total);
    wr64(p + 0x10u, (total * 7ull) / 10ull);
    wr64(p + 0x18u, total);
    wr64(p + 0x20u, (total * 7ull) / 10ull);
    wr64(p + 0x28u, 0x7FFFFFFE0000ull);
    wr64(p + 0x30u, 0x7FFF00000000ull);
    wr64(p + 0x38u, 0);
    HLE_RET32(c, 1u);
}

static void k32_K32GetProcessMemoryInfo(CPU *c) {
    uint64_t p = HLE_ARG1(c);
    if (!p) { HLE_RET32(c, 0); return; }
    memset((void *)(uintptr_t)p, 0, 72u);
    wr32(p, 72u);
    wr64(p + 0x08u, 512ull * 1024ull * 1024ull);
    wr64(p + 0x10u, 256ull * 1024ull * 1024ull);
    HLE_RET32(c, 1u);
}

static void k32_GetVersionExW(CPU *c) {
    uint64_t p = HLE_ARG0(c);
    if (!p) { HLE_RET32(c, 0); return; }
    wr32(p + 0x04u, 10u);
    wr32(p + 0x08u, 0u);
    wr32(p + 0x0Cu, 19045u);
    wr32(p + 0x10u, 2u);
    HLE_RET32(c, 1u);
}
static void nt_RtlGetVersion(CPU *c) {
    uint64_t p = HLE_ARG0(c);
    if (!p) { HLE_RET32(c, 0xC000000Du); return; }
    wr32(p + 0x04u, 10u);
    wr32(p + 0x08u, 0u);
    wr32(p + 0x0Cu, 19045u);
    wr32(p + 0x10u, 2u);
    HLE_RET32(c, 0u); /* STATUS_SUCCESS */
}
static void k32_VerifyVersionInfoW(CPU *c) { HLE_RET32(c, 1u); }
static void k32_VerSetConditionMask(CPU *c) { HLE_RET64(c, HLE_ARG0(c)); }

static bool is_known_shimmed_dll(const char *name) {
    if (!name) return false;
    static const char *const kKnown[] = {
        "ntdll", "ntdll.dll",
        "KERNEL32", "KERNEL32.dll", "USER32", "USER32.dll", "GDI32", "GDI32.dll",
        "OPENGL32", "OPENGL32.dll", "ADVAPI32", "ADVAPI32.dll", "SHELL32", "SHELL32.dll",
        "COMDLG32", "COMDLG32.dll", "VERSION", "VERSION.dll", "SDL2", "SDL2.dll",
        "SDL2_mixer_ext", "SDL2_mixer_ext.dll", "libpng16", "libpng16.dll",
        "tiff", "tiff.dll", "zlibwapi", "zlibwapi.dll", "openvr_api", "openvr_api.dll",
        "steam_api64", "steam_api64.dll"
    };
    const char *base = strrchr(name, '\\');
    const char *b2 = strrchr(name, '/');
    if (b2 && (!base || b2 > base)) base = b2;
    const char *cmp = base ? base + 1 : name;
    for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); ++i) {
        if (strcasecmp_local(cmp, kKnown[i]) == 0) return true;
    }
    return false;
}

static uint64_t synthetic_dll_handle(const char *name) {
    if (!name || strcasecmp_local(name, "SpaceEngine.exe") == 0) {
        uint64_t b = se_pe_active_base();
        return b ? b : SE_EXPECTED_IMAGE_BASE;
    }
    if (!is_known_shimmed_dll(name)) return 0;
    uint32_t h = 2166136261u;
    for (const char *p = name; *p && *p != '.'; ++p) {
        h ^= (uint8_t)tolower((unsigned char)*p);
        h *= 16777619u;
    }
    return 0x7FFF00000000ull | (uint64_t)h;
}

static void k32_GetModuleHandleA(CPU *c) {
    const char *name = (const char *)(uintptr_t)HLE_ARG0(c);
    uint64_t h = synthetic_dll_handle(name);
    if (!h) k32_set_err(W32_ERROR_MOD_NOT_FOUND);
    HLE_RET64(c, h);
}
static void k32_GetModuleHandleW(CPU *c) {
    const uint16_t *wn = (const uint16_t *)(uintptr_t)HLE_ARG0(c);
    char buf[256];
    if (wn) se_utf16_to_utf8(wn, -1, buf, (int)sizeof(buf));
    uint64_t h = synthetic_dll_handle(wn ? buf : NULL);
    if (!h) k32_set_err(W32_ERROR_MOD_NOT_FOUND);
    HLE_RET64(c, h);
}
static void k32_GetModuleHandleExW(CPU *c) {
    uint32_t flags = (uint32_t)HLE_ARG0(c);
    uint64_t name_or_addr = HLE_ARG1(c);
    uint64_t out_va = HLE_ARG2(c);
    uint64_t h = 0;
    if (flags & 0x04u) { /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */
        uint64_t b = se_pe_active_base();
        h = b ? b : SE_EXPECTED_IMAGE_BASE;
    } else {
        char buf[256];
        if (name_or_addr) se_utf16_to_utf8((const uint16_t *)(uintptr_t)name_or_addr, -1, buf, (int)sizeof(buf));
        h = synthetic_dll_handle(name_or_addr ? buf : NULL);
    }
    if (out_va) wr64(out_va, h);
    HLE_RET32(c, h ? 1u : 0u);
}

static void k32_GetModuleFileNameA(CPU *c) {
    char *dst = (char *)(uintptr_t)HLE_ARG1(c);
    uint32_t cap = (uint32_t)HLE_ARG2(c);
    char full[1024];
    snprintf(full, sizeof(full), "%s/system/SpaceEngine.exe", se_hle_get_game_dir());
    if (!dst || cap == 0) { HLE_RET32(c, 0); return; }
    snprintf(dst, cap, "%s", full);
    HLE_RET32(c, (uint32_t)strlen(dst));
}
static void k32_GetModuleFileNameW(CPU *c) {
    uint16_t *dst = (uint16_t *)(uintptr_t)HLE_ARG1(c);
    uint32_t cap = (uint32_t)HLE_ARG2(c);
    char full[1024];
    snprintf(full, sizeof(full), "%s/system/SpaceEngine.exe", se_hle_get_game_dir());
    if (!dst || cap == 0) { HLE_RET32(c, 0); return; }
    int w = se_utf8_to_utf16(full, -1, dst, (int)cap);
    HLE_RET32(c, w > 0 ? (uint32_t)(w - 1) : 0u);
}

static void k32_LoadLibraryA(CPU *c) {
    const char *name = (const char *)(uintptr_t)HLE_ARG0(c);
    uint64_t h = synthetic_dll_handle(name);
    if (!h) {
        K32_LOG("LoadLibraryA('%s') -> NULL (optional DLL unavailable on Android)", name ? name : "");
        k32_set_err(W32_ERROR_MOD_NOT_FOUND);
    }
    HLE_RET64(c, h);
}
static void k32_LoadLibraryW(CPU *c) {
    const uint16_t *wn = (const uint16_t *)(uintptr_t)HLE_ARG0(c);
    char buf[256] = "";
    if (wn) se_utf16_to_utf8(wn, -1, buf, (int)sizeof(buf));
    uint64_t h = synthetic_dll_handle(buf);
    if (!h) k32_set_err(W32_ERROR_MOD_NOT_FOUND);
    HLE_RET64(c, h);
}
static void k32_FreeLibrary(CPU *c) { HLE_RET32(c, 1u); }
static void k32_GetProcAddress(CPU *c) {
    const char *proc = (const char *)(uintptr_t)HLE_ARG1(c);
    if (!proc || ((uint64_t)(uintptr_t)proc < 0x10000ull)) {
        k32_set_err(W32_ERROR_PROC_NOT_FOUND);
        HLE_RET64(c, 0);
        return;
    }
    uint64_t s = se_hle_find_sentinel(NULL, proc);
    if (!s) k32_set_err(W32_ERROR_PROC_NOT_FOUND);
    HLE_RET64(c, s);
}

/* ---- Error / Ptr / ProcessorFeature / C++ EH & Unwind -------------------- */

static void k32_GetLastError(CPU *c) { HLE_RET32(c, k32_get_err()); }
static void k32_SetLastError(CPU *c) { k32_set_err((uint32_t)HLE_ARG0(c)); }
static void k32_DecodePointer(CPU *c) { HLE_RET64(c, HLE_ARG0(c)); }
static void k32_EncodePointer(CPU *c) { HLE_RET64(c, HLE_ARG0(c)); }
static void k32_IsDebuggerPresent(CPU *c) { HLE_RET32(c, 0); }
static void k32_SetUnhandledExceptionFilter(CPU *c) { HLE_RET64(c, 0); }
static void k32_UnhandledExceptionFilter(CPU *c) { HLE_RET32(c, 0); }

static void k32_IsProcessorFeaturePresent(CPU *c) {
    uint32_t pf = (uint32_t)HLE_ARG0(c);
    /* Only expose baseline x64 features: CMPXCHG16B(14), SSE(6), SSE2(10), FASTFAIL(23), RDTSC(8). */
    switch (pf) {
    case 2u:  /* PF_COMPARE_EXCHANGE_DOUBLE */
    case 6u:  /* PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE) */
    case 8u:  /* PF_RDTSC_INSTRUCTION_AVAILABLE */
    case 10u: /* PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2) */
    case 14u: /* PF_COMPARE_EXCHANGE128 */
    case 23u: /* PF_FASTFAIL_AVAILABLE */
        HLE_RET32(c, 1u);
        return;
    default:
        HLE_RET32(c, 0);
        return;
    }
}

static void k32_RtlPcToFileHeader(CPU *c) {
    uint64_t pc = HLE_ARG0(c);
    uint64_t out_base_va = HLE_ARG1(c);
    uint64_t base = se_pe_active_base();
    uint32_t sz = se_pe_active_size();
    uint64_t hit = 0;

    if (base && sz && pc >= base && pc < base + (uint64_t)sz) {
        hit = base;
    } else if (sz && pc >= SE_EXPECTED_IMAGE_BASE && pc < SE_EXPECTED_IMAGE_BASE + (uint64_t)sz) {
        hit = base ? base : SE_EXPECTED_IMAGE_BASE;
    }
    if (out_base_va) wr64(out_base_va, hit);
    HLE_RET64(c, hit);
}

static void k32_RaiseException(CPU *c) {
    uint32_t code = (uint32_t)HLE_ARG0(c);
    uint32_t n_args = (uint32_t)HLE_ARG2(c);
    uint64_t args_va = HLE_ARG3(c);

    if (code == 0xE06D7363u && n_args >= 3u && args_va) { /* MSVC C++ EH */
        uint64_t obj = rd64(args_va + 8ull);
        uint64_t ti = rd64(args_va + 16ull);
        uint64_t ret_pc = rd64(c->rsp);
        c->rsp += 8ull;
        es3_eh_throw(c, ret_pc, obj, ti);
        /* Uncaught C++ exception: balance RSP before returning/aborting. */
        c->rsp -= 8ull;
        fprintf(stderr, "[RaiseException] Uncaught C++ exception obj=%#llx ti=%#llx\n",
                (unsigned long long)obj, (unsigned long long)ti);
    }
    /* Non-C++ exceptions (e.g. 0x406D1388 MS_VC_EXCEPTION SetThreadName) resume cleanly. */
}

static void k32_RtlCaptureContext(CPU *c) {
    uint64_t ctx = HLE_ARG0(c);
    if (!ctx) return;
    memset((void *)(uintptr_t)ctx, 0, 1232u); /* sizeof(CONTEXT) on x64 */
    wr32(ctx + 0x30ull, 0x0010000Full);     /* ContextFlags */
    wr64(ctx + 0x78ull, c->rax);
    wr64(ctx + 0x80ull, c->rcx);
    wr64(ctx + 0x88ull, c->rdx);
    wr64(ctx + 0x90ull, c->rbx);
    wr64(ctx + 0x98ull, c->rsp + 8ull);
    wr64(ctx + 0xA0ull, c->rbp);
    wr64(ctx + 0xA8ull, c->rsi);
    wr64(ctx + 0xB0ull, c->rdi);
    wr64(ctx + 0xB8ull, c->r8);
    wr64(ctx + 0xC0ull, c->r9);
    wr64(ctx + 0xC8ull, c->r10);
    wr64(ctx + 0xD0ull, c->r11);
    wr64(ctx + 0xD8ull, c->r12);
    wr64(ctx + 0xE0ull, c->r13);
    wr64(ctx + 0xE8ull, c->r14);
    wr64(ctx + 0xF0ull, c->r15);
    wr64(ctx + 0xF8ull, rd64(c->rsp));
}
static void k32_RtlLookupFunctionEntry(CPU *c) { (void)c; HLE_RET64(c, 0); }
static void k32_RtlVirtualUnwind(CPU *c) { (void)c; HLE_RET64(c, 0); }
static void k32_RtlUnwindEx(CPU *c) { (void)c; }

/* ---- Dynamic Win32 / ConcRT / UCRT Extensions ---------------------------- */

#if defined(_MSC_VER) && !defined(__clang__)
#define K32_TLS __declspec(thread)
#else
#define K32_TLS _Thread_local
#endif

static _Atomic uint32_t g_k32_cpu_rr = 0u;
static K32_TLS int      tl_cpu_assigned = 0;
static K32_TLS uint32_t tl_cpu_idx = 0u;
static K32_TLS int      tl_aff_inited = 0;
static K32_TLS uint64_t tl_aff_mask = 0ull;
static K32_TLS uint16_t tl_aff_group = 0u;

static uint32_t k32_cur_cpu_idx(void) {
    if (!tl_cpu_assigned) {
        uint32_t n = host_cpu_count();
        tl_cpu_idx = (n > 0u) ? (atomic_fetch_add(&g_k32_cpu_rr, 1u) % n) : 0u;
        tl_cpu_assigned = 1;
    }
    return tl_cpu_idx;
}

static uint64_t k32_all_cpu_mask(uint32_t ncpu) {
    return (ncpu >= 64u) ? ~0ull : ((1ull << ncpu) - 1ull);
}

static void ensure_tl_affinity(void) {
    if (!tl_aff_inited) {
        tl_aff_mask = k32_all_cpu_mask(host_cpu_count());
        tl_aff_group = 0u;
        tl_aff_inited = 1;
    }
}

static void k32_GetCurrentProcessorNumber(CPU *c) {
    HLE_RET32(c, k32_cur_cpu_idx());
}

static void k32_GetCurrentProcessorNumberEx(CPU *c) {
    uint64_t out_va = HLE_ARG0(c);
    if (!out_va) return;
    ensure_tl_affinity();
    wr16(out_va + 0ull, tl_aff_group);
    wr8(out_va + 2ull, (uint8_t)k32_cur_cpu_idx());
    wr8(out_va + 3ull, 0u);
}

static void k32_GetThreadGroupAffinity(CPU *c) {
    uint64_t out_va = HLE_ARG1(c);
    if (!out_va) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    ensure_tl_affinity();
    memset((void *)(uintptr_t)out_va, 0, 16u);
    wr64(out_va + 0ull, tl_aff_mask);
    wr16(out_va + 8ull, tl_aff_group);
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET32(c, 1u);
}

static void k32_SetThreadGroupAffinity(CPU *c) {
    uint64_t new_va = HLE_ARG1(c);
    uint64_t prev_va = HLE_ARG2(c);
    if (!new_va) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    ensure_tl_affinity();
    uint64_t new_mask = rd64(new_va + 0ull);
    uint16_t new_group = rd16(new_va + 8ull);
    if (prev_va) {
        memset((void *)(uintptr_t)prev_va, 0, 16u);
        wr64(prev_va + 0ull, tl_aff_mask);
        wr16(prev_va + 8ull, tl_aff_group);
    }
    if (new_mask != 0ull) {
        tl_aff_mask = new_mask;
        tl_aff_group = new_group;
    }
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET32(c, 1u);
}

static uint32_t emit_lpie_records(uint32_t rel, uint32_t ncpu, uint64_t all_mask, uint8_t *buf) {
    uint32_t off = 0u;
    if (rel == 4u /* RelationGroup */ || rel == 0xFFFFu /* RelationAll */) {
        if (buf) {
            uint8_t *r = buf + off;
            memset(r, 0, 80u);
            uint32_t rtype = 4u, rsz = 80u;
            uint16_t maxg = 1u, actg = 1u;
            memcpy(r + 0u, &rtype, 4);
            memcpy(r + 4u, &rsz, 4);
            memcpy(r + 8u, &maxg, 2);
            memcpy(r + 10u, &actg, 2);
            r[32] = (uint8_t)ncpu;
            r[33] = (uint8_t)ncpu;
            memcpy(r + 72u, &all_mask, 8);
        }
        off += 80u;
    }
    if (rel == 1u /* RelationNumaNode */ || rel == 0xFFFFu) {
        if (buf) {
            uint8_t *r = buf + off;
            memset(r, 0, 48u);
            uint32_t rtype = 1u, rsz = 48u;
            memcpy(r + 0u, &rtype, 4);
            memcpy(r + 4u, &rsz, 4);
            memcpy(r + 32u, &all_mask, 8);
        }
        off += 48u;
    }
    if (rel == 3u /* RelationProcessorPackage */ || rel == 0xFFFFu) {
        if (buf) {
            uint8_t *r = buf + off;
            memset(r, 0, 48u);
            uint32_t rtype = 3u, rsz = 48u;
            uint16_t gc = 1u;
            memcpy(r + 0u, &rtype, 4);
            memcpy(r + 4u, &rsz, 4);
            memcpy(r + 30u, &gc, 2);
            memcpy(r + 32u, &all_mask, 8);
        }
        off += 48u;
    }
    if (rel == 0u /* RelationProcessorCore */ || rel == 0xFFFFu) {
        for (uint32_t i = 0; i < ncpu; ++i) {
            if (buf) {
                uint8_t *r = buf + off;
                memset(r, 0, 48u);
                uint32_t rtype = 0u, rsz = 48u;
                uint16_t gc = 1u;
                uint64_t cmask = 1ull << i;
                memcpy(r + 0u, &rtype, 4);
                memcpy(r + 4u, &rsz, 4);
                memcpy(r + 30u, &gc, 2);
                memcpy(r + 32u, &cmask, 8);
            }
            off += 48u;
        }
    }
    if (rel == 2u /* RelationCache */ || rel == 0xFFFFu) {
        for (uint32_t i = 0; i < ncpu; ++i) {
            if (buf) {
                uint8_t *r = buf + off;
                memset(r, 0, 56u);
                uint32_t rtype = 2u, rsz = 56u, csz = 32768u, ctype = 2u; /* CacheData */
                uint16_t lsz = 64u;
                uint64_t cmask = 1ull << i;
                memcpy(r + 0u, &rtype, 4);
                memcpy(r + 4u, &rsz, 4);
                r[8] = 1u; /* Level 1 */
                r[9] = 8u;
                memcpy(r + 10u, &lsz, 2);
                memcpy(r + 12u, &csz, 4);
                memcpy(r + 16u, &ctype, 4);
                memcpy(r + 40u, &cmask, 8);
            }
            off += 56u;
        }
    }
    return off;
}

static void k32_GetLogicalProcessorInformationEx(CPU *c) {
    uint32_t rel = (uint32_t)HLE_ARG0(c);
    uint64_t buf_va = HLE_ARG1(c);
    uint64_t len_va = HLE_ARG2(c);
    if (!len_va) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t ncpu = host_cpu_count();
    uint64_t all_mask = k32_all_cpu_mask(ncpu);
    uint32_t need = emit_lpie_records(rel, ncpu, all_mask, NULL);
    if (need == 0u) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    uint32_t cap = rd32(len_va);
    wr32(len_va, need);
    if (!buf_va || cap < need) {
        k32_set_err(W32_ERROR_INSUFFICIENT_BUF);
        HLE_RET32(c, 0u);
        return;
    }
    (void)emit_lpie_records(rel, ncpu, all_mask, (uint8_t *)(uintptr_t)buf_va);
    k32_set_err(W32_ERROR_SUCCESS);
    HLE_RET32(c, 1u);
}

/* Native keyed sync table for SRWLOCK / CONDITION_VARIABLE / INIT_ONCE (no unaligned atomics) */
typedef struct K32SyncNode {
    uint64_t va;
    pthread_cond_t cv;
    int32_t srw_state;  /* 0: free, >0: shared holders, -1: exclusive holder */
    int32_t init_state; /* 0: uninit, 1: running, 2: complete */
    uint64_t init_ctx;
    struct K32SyncNode *next;
} K32SyncNode;

#define K32_SYNC_BUCKETS 512u
static K32SyncNode *g_sync_buckets[K32_SYNC_BUCKETS];
static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;

static K32SyncNode *sync_get_locked(uint64_t va) {
    uint32_t h = (uint32_t)((va >> 3) ^ (va >> 15)) & (K32_SYNC_BUCKETS - 1u);
    K32SyncNode *n = g_sync_buckets[h];
    while (n) {
        if (n->va == va) return n;
        n = n->next;
    }
    n = (K32SyncNode *)calloc(1u, sizeof(K32SyncNode));
    if (!n) return NULL;
    n->va = va;
    pthread_cond_init(&n->cv, NULL);
    n->next = g_sync_buckets[h];
    g_sync_buckets[h] = n;
    return n;
}

static void k32_InitializeSRWLock(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    wr64(va, 0ull);
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n) n->srw_state = 0;
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_AcquireSRWLockExclusive(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n) {
        while (n->srw_state != 0) pthread_cond_wait(&n->cv, &g_sync_mu);
        n->srw_state = -1;
    }
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_TryAcquireSRWLockExclusive(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) { HLE_RET32(c, 0u); return; }
    uint32_t ok = 0u;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n && n->srw_state == 0) {
        n->srw_state = -1;
        ok = 1u;
    }
    pthread_mutex_unlock(&g_sync_mu);
    HLE_RET32(c, ok);
}

static void k32_ReleaseSRWLockExclusive(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n && n->srw_state < 0) {
        n->srw_state = 0;
        pthread_cond_broadcast(&n->cv);
    }
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_AcquireSRWLockShared(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n) {
        while (n->srw_state < 0) pthread_cond_wait(&n->cv, &g_sync_mu);
        n->srw_state++;
    }
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_ReleaseSRWLockShared(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n && n->srw_state > 0) {
        n->srw_state--;
        if (n->srw_state == 0) pthread_cond_broadcast(&n->cv);
    }
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_InitializeConditionVariable(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    wr64(va, 0ull);
    pthread_mutex_lock(&g_sync_mu);
    (void)sync_get_locked(va);
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_WakeConditionVariable(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n) pthread_cond_signal(&n->cv);
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_WakeAllConditionVariable(CPU *c) {
    uint64_t va = HLE_ARG0(c);
    if (!va) return;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(va);
    if (n) pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&g_sync_mu);
}

static void k32_SleepConditionVariableSRW(CPU *c) {
    uint64_t cv_va = HLE_ARG0(c);
    uint64_t srw_va = HLE_ARG1(c);
    uint32_t ms = (uint32_t)HLE_ARG2(c);
    uint32_t flags = (uint32_t)HLE_ARG3(c);
    if (!cv_va || !srw_va) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    int shared = (flags & 1u) != 0;
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *cn = sync_get_locked(cv_va);
    K32SyncNode *sn = sync_get_locked(srw_va);
    if (!cn || !sn) {
        pthread_mutex_unlock(&g_sync_mu);
        k32_set_err(W32_ERROR_NOT_ENOUGH_MEMORY);
        HLE_RET32(c, 0u);
        return;
    }
    if (shared) {
        if (sn->srw_state > 0) sn->srw_state--;
    } else {
        if (sn->srw_state < 0) sn->srw_state = 0;
    }
    if (sn->srw_state == 0) pthread_cond_broadcast(&sn->cv);

    uint32_t ok = 1u;
    if (ms == 0u) {
        ok = 0u;
    } else if (ms == W32_INFINITE) {
        pthread_cond_wait(&cn->cv, &g_sync_mu);
    } else {
        struct timespec ts;
        make_abs_timeout(ms, &ts);
        int rc = pthread_cond_timedwait(&cn->cv, &g_sync_mu, &ts);
        if (rc == ETIMEDOUT) ok = 0u;
    }

    if (shared) {
        while (sn->srw_state < 0) pthread_cond_wait(&sn->cv, &g_sync_mu);
        sn->srw_state++;
    } else {
        while (sn->srw_state != 0) pthread_cond_wait(&sn->cv, &g_sync_mu);
        sn->srw_state = -1;
    }
    pthread_mutex_unlock(&g_sync_mu);
    if (!ok) k32_set_err(1460u); /* ERROR_TIMEOUT */
    HLE_RET32(c, ok);
}

static void k32_SleepConditionVariableCS(CPU *c) {
    uint64_t cv_va = HLE_ARG0(c);
    uint64_t cs_va = HLE_ARG1(c);
    uint32_t ms = (uint32_t)HLE_ARG2(c);
    pthread_mutex_t *pm = ensure_cs_mutex(cs_va);
    if (!cv_va || !pm) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    /* Lock ordering: pm -> g_sync_mu on release; unlock g_sync_mu BEFORE re-locking pm. */
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *cn = sync_get_locked(cv_va);
    pthread_mutex_unlock(pm);
    uint32_t ok = 1u;
    if (!cn || ms == 0u) {
        ok = 0u;
    } else if (ms == W32_INFINITE) {
        pthread_cond_wait(&cn->cv, &g_sync_mu);
    } else {
        struct timespec ts;
        make_abs_timeout(ms, &ts);
        int rc = pthread_cond_timedwait(&cn->cv, &g_sync_mu, &ts);
        if (rc == ETIMEDOUT) ok = 0u;
    }
    pthread_mutex_unlock(&g_sync_mu);
    pthread_mutex_lock(pm);
    if (!ok) k32_set_err(1460u); /* ERROR_TIMEOUT */
    HLE_RET32(c, ok);
}

static void k32_FlushProcessWriteBuffers(CPU *c) {
    (void)c;
    atomic_thread_fence(memory_order_seq_cst);
}

static void k32_InitOnceExecuteOnce(CPU *c) {
    uint64_t io_va = HLE_ARG0(c);
    uint64_t fn_va = HLE_ARG1(c);
    uint64_t param_va = HLE_ARG2(c);
    uint64_t ctx_va = HLE_ARG3(c);
    if (!io_va || !fn_va) {
        k32_set_err(W32_ERROR_INVALID_PARAMETER);
        HLE_RET32(c, 0u);
        return;
    }
    pthread_mutex_lock(&g_sync_mu);
    K32SyncNode *n = sync_get_locked(io_va);
    if (!n) {
        pthread_mutex_unlock(&g_sync_mu);
        HLE_RET32(c, 0u);
        return;
    }
    while (n->init_state == 1) {
        pthread_cond_wait(&n->cv, &g_sync_mu);
    }
    if (n->init_state == 2) {
        uint64_t saved = n->init_ctx;
        pthread_mutex_unlock(&g_sync_mu);
        if (ctx_va) wr64(ctx_va, saved);
        HLE_RET32(c, 1u);
        return;
    }
    n->init_state = 1;
    pthread_mutex_unlock(&g_sync_mu);

    uint64_t out_ctx = 0ull;
    uint32_t ok = (uint32_t)se_hle_call_guest(c, fn_va, 3, io_va, param_va,
                                              (uint64_t)(uintptr_t)&out_ctx, 0ull);
    pthread_mutex_lock(&g_sync_mu);
    if (ok != 0u) {
        n->init_ctx = out_ctx;
        n->init_state = 2;
        wr64(io_va, 2ull);
    } else {
        n->init_state = 0;
    }
    pthread_cond_broadcast(&n->cv);
    pthread_mutex_unlock(&g_sync_mu);

    if (ok != 0u && ctx_va) wr64(ctx_va, out_ctx);
    HLE_RET32(c, ok != 0u ? 1u : 0u);
}

static void k32_GetLocaleInfoEx(CPU *c) {
    uint32_t lctype = (uint32_t)HLE_ARG1(c);
    uint16_t *dst = (uint16_t *)(uintptr_t)HLE_ARG2(c);
    int cap = (int)(uint32_t)HLE_ARG3(c);
    if (lctype & 0x20000000u /* LOCALE_RETURN_NUMBER */) {
        if (!dst || cap == 0) { HLE_RET32(c, 2u); return; }
        if (cap < 2) { k32_set_err(W32_ERROR_INSUFFICIENT_BUF); HLE_RET32(c, 0u); return; }
        uint32_t val = 65001u;
        memcpy(dst, &val, 4);
        HLE_RET32(c, 2u);
        return;
    }
    k32_GetLocaleInfoW(c);
}

static void k32_GetCurrentPackageId(CPU *c) {
    (void)c;
    HLE_RET32(c, 15700u); /* APPMODEL_ERROR_NO_PACKAGE */
}

static void k32_GetFileInformationByHandleEx(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_FILE);
    uint32_t cls = (uint32_t)HLE_ARG1(c);
    uint64_t out_va = HLE_ARG2(c);
    uint32_t sz = (uint32_t)HLE_ARG3(c);
    if (!h || !out_va) {
        k32_set_err(h ? W32_ERROR_INVALID_PARAMETER : W32_ERROR_INVALID_HANDLE);
        HLE_RET32(c, 0u);
        return;
    }
    struct stat st;
    if (fstat(h->u.file.fd, &st) != 0) {
        k32_set_err(errno_to_w32(errno));
        HLE_RET32(c, 0u);
        return;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)out_va;
    if (cls == 0u /* FileBasicInfo */ && sz >= 40u) {
        memset(p, 0, 40u);
        uint64_t ft = unix_to_filetime(st.st_mtime, 0);
        uint32_t attr = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
        memcpy(p + 0u, &ft, 8);
        memcpy(p + 8u, &ft, 8);
        memcpy(p + 16u, &ft, 8);
        memcpy(p + 24u, &ft, 8);
        memcpy(p + 32u, &attr, 4);
        HLE_RET32(c, 1u);
        return;
    }
    if (cls == 1u /* FileStandardInfo */ && sz >= 24u) {
        memset(p, 0, 24u);
        uint64_t fsz = (uint64_t)st.st_size;
        uint32_t links = 1u;
        memcpy(p + 0u, &fsz, 8);
        memcpy(p + 8u, &fsz, 8);
        memcpy(p + 16u, &links, 4);
        p[21] = S_ISDIR(st.st_mode) ? 1u : 0u;
        HLE_RET32(c, 1u);
        return;
    }
    k32_set_err(W32_ERROR_NOT_SUPPORTED);
    HLE_RET32(c, 0u);
}

static void k32_SetFileInformationByHandle(CPU *c) {
    K32Handle *h = lookup_handle(HLE_ARG0(c), HT_FILE);
    uint32_t cls = (uint32_t)HLE_ARG1(c);
    uint64_t buf_va = HLE_ARG2(c);
    uint32_t sz = (uint32_t)HLE_ARG3(c);
    if (!h || !buf_va) {
        k32_set_err(h ? W32_ERROR_INVALID_PARAMETER : W32_ERROR_INVALID_HANDLE);
        HLE_RET32(c, 0u);
        return;
    }
    if (cls == 0u /* FileBasicInfo */ && sz >= 40u) {
        HLE_RET32(c, 1u);
        return;
    }
    if (cls == 6u /* FileEndOfFileInfo */ && sz >= 8u) {
        uint64_t eof = rd64(buf_va);
#if defined(_WIN32)
        int rc = _chsize_s(h->u.file.fd, (__int64)eof);
#else
        int rc = ftruncate(h->u.file.fd, (off_t)eof);
#endif
        HLE_RET32(c, rc == 0 ? 1u : 0u);
        return;
    }
    k32_set_err(W32_ERROR_NOT_SUPPORTED);
    HLE_RET32(c, 0u);
}

static void se_register_kernel32_dynamic_shims(void) {
    (void)se_hle_register_dynamic("KERNEL32.dll", "SetThreadGroupAffinity", k32_SetThreadGroupAffinity);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetThreadGroupAffinity", k32_GetThreadGroupAffinity);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetCurrentProcessorNumberEx", k32_GetCurrentProcessorNumberEx);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetCurrentProcessorNumber", k32_GetCurrentProcessorNumber);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetLogicalProcessorInformationEx", k32_GetLogicalProcessorInformationEx);
    (void)se_hle_register_dynamic("KERNEL32.dll", "InitializeSRWLock", k32_InitializeSRWLock);
    (void)se_hle_register_dynamic("KERNEL32.dll", "AcquireSRWLockExclusive", k32_AcquireSRWLockExclusive);
    (void)se_hle_register_dynamic("KERNEL32.dll", "TryAcquireSRWLockExclusive", k32_TryAcquireSRWLockExclusive);
    (void)se_hle_register_dynamic("KERNEL32.dll", "ReleaseSRWLockExclusive", k32_ReleaseSRWLockExclusive);
    (void)se_hle_register_dynamic("KERNEL32.dll", "AcquireSRWLockShared", k32_AcquireSRWLockShared);
    (void)se_hle_register_dynamic("KERNEL32.dll", "ReleaseSRWLockShared", k32_ReleaseSRWLockShared);
    (void)se_hle_register_dynamic("KERNEL32.dll", "InitializeConditionVariable", k32_InitializeConditionVariable);
    (void)se_hle_register_dynamic("KERNEL32.dll", "WakeConditionVariable", k32_WakeConditionVariable);
    (void)se_hle_register_dynamic("KERNEL32.dll", "WakeAllConditionVariable", k32_WakeAllConditionVariable);
    (void)se_hle_register_dynamic("KERNEL32.dll", "SleepConditionVariableSRW", k32_SleepConditionVariableSRW);
    (void)se_hle_register_dynamic("KERNEL32.dll", "SleepConditionVariableCS", k32_SleepConditionVariableCS);
    (void)se_hle_register_dynamic("KERNEL32.dll", "FlushProcessWriteBuffers", k32_FlushProcessWriteBuffers);
    (void)se_hle_register_dynamic("KERNEL32.dll", "InitOnceExecuteOnce", k32_InitOnceExecuteOnce);
    (void)se_hle_register_dynamic("KERNEL32.dll", "CreateEventExW", k32_CreateEventExW);
    (void)se_hle_register_dynamic("KERNEL32.dll", "CreateSemaphoreW", k32_CreateSemaphoreW);
    (void)se_hle_register_dynamic("KERNEL32.dll", "CreateSemaphoreExW", k32_CreateSemaphoreW);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetSystemTimePreciseAsFileTime", k32_GetSystemTimeAsFileTime);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetFileInformationByHandleEx", k32_GetFileInformationByHandleEx);
    (void)se_hle_register_dynamic("KERNEL32.dll", "SetFileInformationByHandle", k32_SetFileInformationByHandle);
    (void)se_hle_register_dynamic("KERNEL32.dll", "CompareStringEx", k32_CompareStringW);
    (void)se_hle_register_dynamic("KERNEL32.dll", "LCMapStringEx", k32_LCMapStringW);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetLocaleInfoEx", k32_GetLocaleInfoEx);
    (void)se_hle_register_dynamic("KERNEL32.dll", "GetCurrentPackageId", k32_GetCurrentPackageId);
}

void se_register_kernel32_shims(void) {
    se_register_kernel32_dynamic_shims();
    /* Memory & Heap */
    se_hle_register_import("KERNEL32.dll", "GetProcessHeap", k32_GetProcessHeap);
    se_hle_register_import("KERNEL32.dll", "HeapAlloc", k32_HeapAlloc);
    se_hle_register_import("KERNEL32.dll", "HeapReAlloc", k32_HeapReAlloc);
    se_hle_register_import("KERNEL32.dll", "HeapFree", k32_HeapFree);
    se_hle_register_import("KERNEL32.dll", "HeapSize", k32_HeapSize);
    se_hle_register_import("KERNEL32.dll", "HeapCompact", k32_HeapCompact);
    se_hle_register_import("KERNEL32.dll", "GlobalAlloc", k32_GlobalAlloc);
    se_hle_register_import("KERNEL32.dll", "GlobalFree", k32_GlobalFree);
    se_hle_register_import("KERNEL32.dll", "GlobalLock", k32_GlobalLock);
    se_hle_register_import("KERNEL32.dll", "GlobalUnlock", k32_GlobalUnlock);
    se_hle_register_import("KERNEL32.dll", "LocalAlloc", k32_LocalAlloc);
    se_hle_register_import("KERNEL32.dll", "LocalFree", k32_LocalFree);
    se_hle_register_import("KERNEL32.dll", "VirtualAlloc", k32_VirtualAlloc);
    se_hle_register_import("KERNEL32.dll", "VirtualFree", k32_VirtualFree);
    se_hle_register_import("KERNEL32.dll", "VirtualProtect", k32_VirtualProtect);

    /* File, Directory, Find, Console */
    se_hle_register_import("KERNEL32.dll", "CreateFileW", k32_CreateFileW);
    se_hle_register_import("KERNEL32.dll", "ReadFile", k32_ReadFile);
    se_hle_register_import("KERNEL32.dll", "WriteFile", k32_WriteFile);
    se_hle_register_import("KERNEL32.dll", "SetFilePointerEx", k32_SetFilePointerEx);
    se_hle_register_import("KERNEL32.dll", "GetFileSizeEx", k32_GetFileSizeEx);
    se_hle_register_import("KERNEL32.dll", "SetEndOfFile", k32_SetEndOfFile);
    se_hle_register_import("KERNEL32.dll", "FlushFileBuffers", k32_FlushFileBuffers);
    se_hle_register_import("KERNEL32.dll", "GetFileType", k32_GetFileType);
    se_hle_register_import("KERNEL32.dll", "GetFileInformationByHandle", k32_GetFileInformationByHandle);
    se_hle_register_import("KERNEL32.dll", "GetFileAttributesW", k32_GetFileAttributesW);
    se_hle_register_import("KERNEL32.dll", "GetFileAttributesExW", k32_GetFileAttributesExW);
    se_hle_register_import("KERNEL32.dll", "CreateDirectoryW", k32_CreateDirectoryW);
    se_hle_register_import("KERNEL32.dll", "DeleteFileW", k32_DeleteFileW);
    se_hle_register_import("KERNEL32.dll", "MoveFileExW", k32_MoveFileExW);
    se_hle_register_import("KERNEL32.dll", "CopyFileA", k32_CopyFileA);
    se_hle_register_import("KERNEL32.dll", "FindFirstFileExW", k32_FindFirstFileExW);
    se_hle_register_import("KERNEL32.dll", "FindNextFileW", k32_FindNextFileW);
    se_hle_register_import("KERNEL32.dll", "FindFirstFileA", k32_FindFirstFileA);
    se_hle_register_import("KERNEL32.dll", "FindNextFileA", k32_FindNextFileA);
    se_hle_register_import("KERNEL32.dll", "FindClose", k32_FindClose);
    se_hle_register_import("KERNEL32.dll", "GetCurrentDirectoryW", k32_GetCurrentDirectoryW);
    se_hle_register_import("KERNEL32.dll", "SetCurrentDirectoryW", k32_SetCurrentDirectoryW);
    se_hle_register_import("KERNEL32.dll", "GetFullPathNameW", k32_GetFullPathNameW);
    se_hle_register_import("KERNEL32.dll", "GetDriveTypeW", k32_GetDriveTypeW);
    se_hle_register_import("KERNEL32.dll", "GetSystemDirectoryW", k32_GetSystemDirectoryW);
    se_hle_register_import("KERNEL32.dll", "SearchPathW", k32_SearchPathW);
    se_hle_register_import("KERNEL32.dll", "GetStdHandle", k32_GetStdHandle);
    se_hle_register_import("KERNEL32.dll", "SetStdHandle", k32_SetStdHandle);
    se_hle_register_import("KERNEL32.dll", "WriteConsoleW", k32_WriteConsoleW);
    se_hle_register_import("KERNEL32.dll", "ReadConsoleW", k32_ReadConsoleW);
    se_hle_register_import("KERNEL32.dll", "GetConsoleMode", k32_GetConsoleMode);
    se_hle_register_import("KERNEL32.dll", "GetConsoleOutputCP", k32_GetConsoleOutputCP);
    se_hle_register_import("KERNEL32.dll", "PeekNamedPipe", k32_PeekNamedPipe);

    /* Strings, CodePage, Locale */
    se_hle_register_import("KERNEL32.dll", "MultiByteToWideChar", k32_MultiByteToWideChar);
    se_hle_register_import("KERNEL32.dll", "WideCharToMultiByte", k32_WideCharToMultiByte);
    se_hle_register_import("KERNEL32.dll", "GetACP", k32_GetACP);
    se_hle_register_import("KERNEL32.dll", "GetOEMCP", k32_GetOEMCP);
    se_hle_register_import("KERNEL32.dll", "IsValidCodePage", k32_IsValidCodePage);
    se_hle_register_import("KERNEL32.dll", "GetCPInfo", k32_GetCPInfo);
    se_hle_register_import("KERNEL32.dll", "IsValidLocale", k32_IsValidLocale);
    se_hle_register_import("KERNEL32.dll", "EnumSystemLocalesW", k32_EnumSystemLocalesW);
    se_hle_register_import("KERNEL32.dll", "GetUserDefaultLCID", k32_GetUserDefaultLCID);
    se_hle_register_import("KERNEL32.dll", "GetLocaleInfoW", k32_GetLocaleInfoW);
    se_hle_register_import("KERNEL32.dll", "LCMapStringW", k32_LCMapStringW);
    se_hle_register_import("KERNEL32.dll", "CompareStringW", k32_CompareStringW);
    se_hle_register_import("KERNEL32.dll", "GetStringTypeW", k32_GetStringTypeW);

    /* Sync, CS, Events, TLS/FLS, SList, Threads, Timers */
    se_hle_register_import("KERNEL32.dll", "InitializeCriticalSection", k32_InitializeCriticalSection);
    se_hle_register_import("KERNEL32.dll", "InitializeCriticalSectionAndSpinCount", k32_InitializeCriticalSection);
    se_hle_register_import("KERNEL32.dll", "InitializeCriticalSectionEx", k32_InitializeCriticalSection);
    se_hle_register_import("KERNEL32.dll", "EnterCriticalSection", k32_EnterCriticalSection);
    se_hle_register_import("KERNEL32.dll", "TryEnterCriticalSection", k32_TryEnterCriticalSection);
    se_hle_register_import("KERNEL32.dll", "LeaveCriticalSection", k32_LeaveCriticalSection);
    se_hle_register_import("KERNEL32.dll", "DeleteCriticalSection", k32_DeleteCriticalSection);
    se_hle_register_import("KERNEL32.dll", "CreateEventW", k32_CreateEventW);
    se_hle_register_import("KERNEL32.dll", "OpenEventW", k32_OpenEventW);
    se_hle_register_import("KERNEL32.dll", "SetEvent", k32_SetEvent);
    se_hle_register_import("KERNEL32.dll", "ResetEvent", k32_ResetEvent);
    se_hle_register_import("KERNEL32.dll", "ReleaseSemaphore", k32_ReleaseSemaphore);
    se_hle_register_import("KERNEL32.dll", "WaitForSingleObject", k32_WaitForSingleObject);
    se_hle_register_import("KERNEL32.dll", "WaitForSingleObjectEx", k32_WaitForSingleObject);
    se_hle_register_import("KERNEL32.dll", "SignalObjectAndWait", k32_SignalObjectAndWait);
    se_hle_register_import("KERNEL32.dll", "TlsAlloc", k32_TlsAlloc);
    se_hle_register_import("KERNEL32.dll", "TlsGetValue", k32_TlsGetValue);
    se_hle_register_import("KERNEL32.dll", "TlsSetValue", k32_TlsSetValue);
    se_hle_register_import("KERNEL32.dll", "TlsFree", k32_TlsFree);
    se_hle_register_import("KERNEL32.dll", "FlsAlloc", k32_FlsAlloc);
    se_hle_register_import("KERNEL32.dll", "FlsGetValue", k32_TlsGetValue);
    se_hle_register_import("KERNEL32.dll", "FlsSetValue", k32_TlsSetValue);
    se_hle_register_import("KERNEL32.dll", "FlsFree", k32_TlsFree);
    se_hle_register_import("KERNEL32.dll", "InitializeSListHead", k32_InitializeSListHead);
    se_hle_register_import("KERNEL32.dll", "QueryDepthSList", k32_QueryDepthSList);
    se_hle_register_import("KERNEL32.dll", "InterlockedPushEntrySList", k32_InterlockedPushEntrySList);
    se_hle_register_import("KERNEL32.dll", "InterlockedPopEntrySList", k32_InterlockedPopEntrySList);
    se_hle_register_import("KERNEL32.dll", "InterlockedFlushSList", k32_InterlockedFlushSList);
    se_hle_register_import("KERNEL32.dll", "CreateThread", k32_CreateThread);
    se_hle_register_import("KERNEL32.dll", "ExitThread", k32_ExitThread);
    se_hle_register_import("KERNEL32.dll", "FreeLibraryAndExitThread", k32_ExitThread);
    se_hle_register_import("KERNEL32.dll", "GetExitCodeThread", k32_GetExitCodeThread);
    se_hle_register_import("KERNEL32.dll", "GetCurrentThread", k32_GetCurrentThread);
    se_hle_register_import("KERNEL32.dll", "GetCurrentThreadId", k32_GetCurrentThreadId);
    se_hle_register_import("KERNEL32.dll", "GetThreadPriority", k32_GetThreadPriority);
    se_hle_register_import("KERNEL32.dll", "SetThreadPriority", k32_SetThreadPriority);
    se_hle_register_import("KERNEL32.dll", "SetThreadAffinityMask", k32_SetThreadAffinityMask);
    se_hle_register_import("KERNEL32.dll", "GetThreadTimes", k32_GetThreadTimes);
    se_hle_register_import("KERNEL32.dll", "SwitchToThread", k32_SwitchToThread);
    se_hle_register_import("KERNEL32.dll", "Sleep", k32_Sleep);
    se_hle_register_import("KERNEL32.dll", "CreateTimerQueue", k32_CreateTimerQueue);
    se_hle_register_import("KERNEL32.dll", "CreateTimerQueueTimer", k32_CreateTimerQueueTimer);
    se_hle_register_import("KERNEL32.dll", "ChangeTimerQueueTimer", k32_ChangeTimerQueueTimer);
    se_hle_register_import("KERNEL32.dll", "DeleteTimerQueueTimer", k32_DeleteTimerQueueTimer);
    se_hle_register_import("KERNEL32.dll", "RegisterWaitForSingleObject", k32_RegisterWaitForSingleObject);
    se_hle_register_import("KERNEL32.dll", "UnregisterWait", k32_UnregisterWait);
    se_hle_register_import("KERNEL32.dll", "UnregisterWaitEx", k32_UnregisterWaitEx);
    se_hle_register_import("KERNEL32.dll", "DuplicateHandle", k32_DuplicateHandle);
    se_hle_register_import("KERNEL32.dll", "CloseHandle", k32_CloseHandle);

    /* Process, Time, SysInfo, Modules, EH */
    se_hle_register_import("KERNEL32.dll", "GetCurrentProcess", k32_GetCurrentProcess);
    se_hle_register_import("KERNEL32.dll", "GetCurrentProcessId", k32_GetCurrentProcessId);
    se_hle_register_import("KERNEL32.dll", "ExitProcess", k32_ExitProcess);
    se_hle_register_import("KERNEL32.dll", "TerminateProcess", k32_ExitProcess);
    se_hle_register_import("KERNEL32.dll", "CreateProcessA", k32_CreateProcessA);
    se_hle_register_import("KERNEL32.dll", "GetCommandLineA", k32_GetCommandLineA);
    se_hle_register_import("KERNEL32.dll", "GetCommandLineW", k32_GetCommandLineW);
    se_hle_register_import("KERNEL32.dll", "GetEnvironmentStringsW", k32_GetEnvironmentStringsW);
    se_hle_register_import("KERNEL32.dll", "FreeEnvironmentStringsW", k32_FreeEnvironmentStringsW);
    se_hle_register_import("KERNEL32.dll", "SetEnvironmentVariableW", k32_SetEnvironmentVariableW);
    se_hle_register_import("KERNEL32.dll", "GetStartupInfoW", k32_GetStartupInfoW);
    se_hle_register_import("KERNEL32.dll", "QueryPerformanceFrequency", k32_QueryPerformanceFrequency);
    se_hle_register_import("KERNEL32.dll", "QueryPerformanceCounter", k32_QueryPerformanceCounter);
    se_hle_register_import("KERNEL32.dll", "GetTickCount64", k32_GetTickCount64);
    se_hle_register_import("KERNEL32.dll", "GetTickCount", k32_GetTickCount);
    se_hle_register_import("KERNEL32.dll", "GetSystemTimeAsFileTime", k32_GetSystemTimeAsFileTime);
    se_hle_register_import("KERNEL32.dll", "GetSystemTime", k32_GetSystemTime);
    se_hle_register_import("KERNEL32.dll", "GetLocalTime", k32_GetLocalTime);
    se_hle_register_import("KERNEL32.dll", "FileTimeToSystemTime", k32_FileTimeToSystemTime);
    se_hle_register_import("KERNEL32.dll", "SystemTimeToTzSpecificLocalTime", k32_SystemTimeToTzSpecificLocalTime);
    se_hle_register_import("KERNEL32.dll", "GetTimeZoneInformation", k32_GetTimeZoneInformation);
    se_hle_register_import("KERNEL32.dll", "GetSystemInfo", k32_GetSystemInfo);
    se_hle_register_import("KERNEL32.dll", "GetNativeSystemInfo", k32_GetSystemInfo);
    se_hle_register_import("KERNEL32.dll", "GetLogicalProcessorInformation", k32_GetLogicalProcessorInformation);
    se_hle_register_import("KERNEL32.dll", "GetProcessAffinityMask", k32_GetProcessAffinityMask);
    se_hle_register_import("KERNEL32.dll", "GetNumaHighestNodeNumber", k32_GetNumaHighestNodeNumber);
    se_hle_register_import("KERNEL32.dll", "GlobalMemoryStatusEx", k32_GlobalMemoryStatusEx);
    se_hle_register_import("KERNEL32.dll", "K32GetProcessMemoryInfo", k32_K32GetProcessMemoryInfo);
    se_hle_register_import("KERNEL32.dll", "GetVersionExW", k32_GetVersionExW);
    se_hle_register_import("KERNEL32.dll", "VerifyVersionInfoW", k32_VerifyVersionInfoW);
    se_hle_register_import("KERNEL32.dll", "VerSetConditionMask", k32_VerSetConditionMask);
    (void)se_hle_register_dynamic("ntdll.dll", "RtlGetVersion", nt_RtlGetVersion);
    se_hle_register_import("KERNEL32.dll", "GetModuleHandleA", k32_GetModuleHandleA);
    se_hle_register_import("KERNEL32.dll", "GetModuleHandleW", k32_GetModuleHandleW);
    se_hle_register_import("KERNEL32.dll", "GetModuleHandleExW", k32_GetModuleHandleExW);
    se_hle_register_import("KERNEL32.dll", "GetModuleFileNameA", k32_GetModuleFileNameA);
    se_hle_register_import("KERNEL32.dll", "GetModuleFileNameW", k32_GetModuleFileNameW);
    se_hle_register_import("KERNEL32.dll", "LoadLibraryA", k32_LoadLibraryA);
    se_hle_register_import("KERNEL32.dll", "LoadLibraryW", k32_LoadLibraryW);
    se_hle_register_import("KERNEL32.dll", "LoadLibraryExW", k32_LoadLibraryW);
    se_hle_register_import("KERNEL32.dll", "FreeLibrary", k32_FreeLibrary);
    se_hle_register_import("KERNEL32.dll", "GetProcAddress", k32_GetProcAddress);
    se_hle_register_import("KERNEL32.dll", "GetLastError", k32_GetLastError);
    se_hle_register_import("KERNEL32.dll", "SetLastError", k32_SetLastError);
    se_hle_register_import("KERNEL32.dll", "DecodePointer", k32_DecodePointer);
    se_hle_register_import("KERNEL32.dll", "EncodePointer", k32_EncodePointer);
    se_hle_register_import("KERNEL32.dll", "IsDebuggerPresent", k32_IsDebuggerPresent);
    se_hle_register_import("KERNEL32.dll", "SetUnhandledExceptionFilter", k32_SetUnhandledExceptionFilter);
    se_hle_register_import("KERNEL32.dll", "UnhandledExceptionFilter", k32_UnhandledExceptionFilter);
    se_hle_register_import("KERNEL32.dll", "IsProcessorFeaturePresent", k32_IsProcessorFeaturePresent);
    se_hle_register_import("KERNEL32.dll", "RtlPcToFileHeader", k32_RtlPcToFileHeader);
    se_hle_register_import("KERNEL32.dll", "RaiseException", k32_RaiseException);
    se_hle_register_import("KERNEL32.dll", "RtlCaptureContext", k32_RtlCaptureContext);
    se_hle_register_import("KERNEL32.dll", "RtlLookupFunctionEntry", k32_RtlLookupFunctionEntry);
    se_hle_register_import("KERNEL32.dll", "RtlVirtualUnwind", k32_RtlVirtualUnwind);
    se_hle_register_import("KERNEL32.dll", "RtlUnwindEx", k32_RtlUnwindEx);
    se_hle_register_import("KERNEL32.dll", "RtlUnwind", k32_RtlUnwindEx);
}
