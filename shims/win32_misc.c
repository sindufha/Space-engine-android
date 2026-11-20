/*
 * win32_misc.c - ADVAPI32, COMDLG32, SHELL32, VERSION, clean-room openvr_api,
 * and clean-room offline steam_api64 Win64 HLE shims (32 IAT imports total).
 */
#define _POSIX_C_SOURCE 200809L

#include "hle_bridge.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__ANDROID__)
#include <android/log.h>
#define MISC_LOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "SpaceEngineMisc", fmt, ##__VA_ARGS__)
#else
#define MISC_LOG(fmt, ...) ((void)0)
#endif

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

static int misc_strcasecmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
        ++a;
        ++b;
    }
}

static uint64_t misc_guest_cstr_to_utf8(uint64_t ptr, char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (!ptr) return 0;
    const char *s = (const char *)(uintptr_t)ptr;
    size_t n = 0;
    while (n + 1 < cap && s[n] != '\0') {
        buf[n] = s[n];
        ++n;
    }
    buf[n] = '\0';
    return (uint64_t)n;
}

/* ========================================================================= */
/* 1. ADVAPI32.dll (6 imports): In-memory Registry Key/Value Store           */
/* ========================================================================= */

#define REG_MAX_KEYS    128
#define REG_MAX_VALUES  32
#define REG_HANDLE_BASE 0x4B000000ull

#define W32_REG_SZ        1u
#define W32_REG_EXPAND_SZ 2u
#define W32_REG_BINARY    3u
#define W32_REG_DWORD     4u

#define W32_ERR_SUCCESS        0u
#define W32_ERR_FILE_NOT_FOUND 2u
#define W32_ERR_INVALID_HANDLE 6u
#define W32_ERR_MORE_DATA      234u
#define W32_ERR_NO_MORE_ITEMS  259u

typedef struct {
    bool     used;
    char     name[128];
    uint32_t type;
    uint8_t *data;
    uint32_t size;
    bool     from_ansi;
} reg_value_t;

typedef struct {
    bool        used;
    uint64_t    root_hkey;
    char        subkey[256];
    int         refcount;
    reg_value_t values[REG_MAX_VALUES];
} reg_key_t;

static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static reg_key_t       g_reg_keys[REG_MAX_KEYS];

static uint64_t reg_canonical_root(uint64_t h) {
    /* Sign/zero-extended Win64 predefined HKEY values: 0x80000000..0x80000007 */
    uint32_t lo = (uint32_t)h;
    if ((lo & 0x80000000u) != 0 && (h == (uint64_t)lo || h == (uint64_t)(int64_t)(int32_t)lo)) {
        return (uint64_t)lo;
    }
    return h;
}

static reg_key_t *reg_find_by_handle(uint64_t hkey) {
    if (hkey < REG_HANDLE_BASE) return NULL;
    uint64_t idx = hkey - REG_HANDLE_BASE;
    if (idx >= REG_MAX_KEYS) return NULL;
    return g_reg_keys[idx].used ? &g_reg_keys[idx] : NULL;
}

static void reg_build_full_subkey(uint64_t parent_hkey, const char *rel,
                                  uint64_t *out_root, char *out_sub, size_t cap) {
    uint64_t canon = reg_canonical_root(parent_hkey);
    reg_key_t *parent = reg_find_by_handle(canon);
    if (parent) {
        *out_root = parent->root_hkey;
        if (parent->subkey[0] && rel && rel[0]) {
            snprintf(out_sub, cap, "%s\\%s", parent->subkey, rel);
        } else if (parent->subkey[0]) {
            snprintf(out_sub, cap, "%s", parent->subkey);
        } else {
            snprintf(out_sub, cap, "%s", rel ? rel : "");
        }
    } else {
        *out_root = canon;
        snprintf(out_sub, cap, "%s", rel ? rel : "");
    }
    for (size_t i = 0; out_sub[i]; ++i) {
        if (out_sub[i] == '/') out_sub[i] = '\\';
    }
}

static int reg_find_key_idx(uint64_t root, const char *subkey) {
    for (int i = 0; i < REG_MAX_KEYS; ++i) {
        if (g_reg_keys[i].used &&
            g_reg_keys[i].root_hkey == root &&
            misc_strcasecmp(g_reg_keys[i].subkey, subkey) == 0) {
            return i;
        }
    }
    return -1;
}

static void hle_advapi_RegCreateKeyExA(CPU *c) {
    uint64_t hKey = HLE_ARG0(c);
    uint64_t lpSubKey = HLE_ARG1(c);
    uint64_t phkResult = HLE_ARG7(c);
    uint64_t lpdwDisposition = hle_stack_arg(c, 8u);

    char rel[256];
    misc_guest_cstr_to_utf8(lpSubKey, rel, sizeof(rel));

    uint64_t root = 0;
    char full[256];
    reg_build_full_subkey(hKey, rel, &root, full, sizeof(full));

    pthread_mutex_lock(&g_reg_mu);
    int idx = reg_find_key_idx(root, full);
    uint32_t disp = 2u; /* REG_OPENED_EXISTING_KEY */
    if (idx < 0) {
        for (int i = 0; i < REG_MAX_KEYS; ++i) {
            if (!g_reg_keys[i].used) {
                idx = i;
                memset(&g_reg_keys[i], 0, sizeof(g_reg_keys[i]));
                g_reg_keys[i].used = true;
                g_reg_keys[i].root_hkey = root;
                snprintf(g_reg_keys[i].subkey, sizeof(g_reg_keys[i].subkey), "%s", full);
                disp = 1u; /* REG_CREATED_NEW_KEY */
                break;
            }
        }
    }
    if (idx >= 0) {
        g_reg_keys[idx].refcount++;
        if (phkResult) wr64(phkResult, REG_HANDLE_BASE + (uint64_t)idx);
        if (lpdwDisposition) wr32(lpdwDisposition, disp);
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_SUCCESS);
    } else {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, 8u); /* ERROR_NOT_ENOUGH_MEMORY */
    }
}

static void hle_advapi_RegOpenKeyExW(CPU *c) {
    uint64_t hKey = HLE_ARG0(c);
    uint64_t lpSubKey = HLE_ARG1(c);
    uint64_t phkResult = HLE_ARG4(c);

    char rel[256];
    rel[0] = '\0';
    if (lpSubKey) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)lpSubKey, -1, rel, (int)sizeof(rel));
    }

    uint64_t root = 0;
    char full[256];
    reg_build_full_subkey(hKey, rel, &root, full, sizeof(full));

    pthread_mutex_lock(&g_reg_mu);
    int idx = reg_find_key_idx(root, full);
    if (idx < 0 && rel[0] == '\0' && (root >= 0x80000000ull && root <= 0x80000007ull)) {
        for (int i = 0; i < REG_MAX_KEYS; ++i) {
            if (!g_reg_keys[i].used) {
                idx = i;
                memset(&g_reg_keys[i], 0, sizeof(g_reg_keys[i]));
                g_reg_keys[i].used = true;
                g_reg_keys[i].root_hkey = root;
                break;
            }
        }
    }
    if (idx >= 0) {
        g_reg_keys[idx].refcount++;
        if (phkResult) wr64(phkResult, REG_HANDLE_BASE + (uint64_t)idx);
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_SUCCESS);
    } else {
        if (phkResult) wr64(phkResult, 0);
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_FILE_NOT_FOUND);
    }
}

static void hle_advapi_RegCloseKey(CPU *c) {
    uint64_t hKey = HLE_ARG0(c);
    uint64_t canon = reg_canonical_root(hKey);
    if (canon >= 0x80000000ull && canon <= 0x80000007ull) {
        HLE_RET32(c, W32_ERR_SUCCESS);
        return;
    }
    pthread_mutex_lock(&g_reg_mu);
    reg_key_t *k = reg_find_by_handle(canon);
    if (!k) {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_INVALID_HANDLE);
        return;
    }
    if (k->refcount > 0) k->refcount--;
    pthread_mutex_unlock(&g_reg_mu);
    HLE_RET32(c, W32_ERR_SUCCESS);
}

static void hle_advapi_RegSetValueExA(CPU *c) {
    uint64_t hKey = HLE_ARG0(c);
    uint64_t lpValueName = HLE_ARG1(c);
    uint32_t dwType = (uint32_t)HLE_ARG3(c);
    uint64_t lpData = HLE_ARG4(c);
    uint32_t cbData = (uint32_t)HLE_ARG5(c);

    char vname[128];
    misc_guest_cstr_to_utf8(lpValueName, vname, sizeof(vname));

    pthread_mutex_lock(&g_reg_mu);
    reg_key_t *k = reg_find_by_handle(reg_canonical_root(hKey));
    if (!k) {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_INVALID_HANDLE);
        return;
    }

    reg_value_t *slot = NULL;
    for (int i = 0; i < REG_MAX_VALUES; ++i) {
        if (k->values[i].used && misc_strcasecmp(k->values[i].name, vname) == 0) {
            slot = &k->values[i];
            break;
        }
    }
    if (!slot) {
        for (int i = 0; i < REG_MAX_VALUES; ++i) {
            if (!k->values[i].used) {
                slot = &k->values[i];
                break;
            }
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, 8u);
        return;
    }

    uint8_t *copy = NULL;
    if (cbData > 0 && lpData) {
        copy = (uint8_t *)malloc(cbData);
        if (!copy) {
            pthread_mutex_unlock(&g_reg_mu);
            HLE_RET32(c, 8u);
            return;
        }
        memcpy(copy, (const void *)(uintptr_t)lpData, cbData);
    }

    free(slot->data);
    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", vname);
    slot->type = dwType;
    slot->data = copy;
    slot->size = cbData;
    slot->from_ansi = true;

    pthread_mutex_unlock(&g_reg_mu);
    HLE_RET32(c, W32_ERR_SUCCESS);
}

static uint32_t reg_copy_value_wide(const reg_value_t *val, uint64_t lpType,
                                    uint64_t lpData, uint64_t lpcbData) {
    if (lpType) wr32(lpType, val->type);

    uint32_t needed = val->size;
    uint16_t *wbuf = NULL;
    if (val->from_ansi && (val->type == W32_REG_SZ || val->type == W32_REG_EXPAND_SZ)) {
        const char *astr = val->data ? (const char *)val->data : "";
        int wchars = se_utf8_to_utf16(astr, -1, NULL, 0);
        if (wchars < 1) wchars = 1;
        wbuf = (uint16_t *)calloc((size_t)wchars, sizeof(uint16_t));
        if (!wbuf) return 8u;
        se_utf8_to_utf16(astr, -1, wbuf, wchars);
        needed = (uint32_t)wchars * 2u;
    }

    uint32_t cap = lpcbData ? rd32(lpcbData) : 0u;
    if (lpcbData) wr32(lpcbData, needed);

    if (!lpData) {
        free(wbuf);
        return W32_ERR_SUCCESS;
    }
    if (cap < needed) {
        free(wbuf);
        return W32_ERR_MORE_DATA;
    }

    if (wbuf) {
        memcpy((void *)(uintptr_t)lpData, wbuf, needed);
        free(wbuf);
    } else if (needed > 0 && val->data) {
        memcpy((void *)(uintptr_t)lpData, val->data, needed);
    }
    return W32_ERR_SUCCESS;
}

static void hle_advapi_RegQueryValueExW(CPU *c) {
    uint64_t hKey = HLE_ARG0(c);
    uint64_t lpValueName = HLE_ARG1(c);
    uint64_t lpType = HLE_ARG3(c);
    uint64_t lpData = HLE_ARG4(c);
    uint64_t lpcbData = HLE_ARG5(c);

    char vname[128];
    vname[0] = '\0';
    if (lpValueName) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)lpValueName, -1, vname, (int)sizeof(vname));
    }

    pthread_mutex_lock(&g_reg_mu);
    reg_key_t *k = reg_find_by_handle(reg_canonical_root(hKey));
    if (!k) {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_INVALID_HANDLE);
        return;
    }

    for (int i = 0; i < REG_MAX_VALUES; ++i) {
        if (k->values[i].used && misc_strcasecmp(k->values[i].name, vname) == 0) {
            uint32_t rc = reg_copy_value_wide(&k->values[i], lpType, lpData, lpcbData);
            pthread_mutex_unlock(&g_reg_mu);
            HLE_RET32(c, rc);
            return;
        }
    }

    pthread_mutex_unlock(&g_reg_mu);
    HLE_RET32(c, W32_ERR_FILE_NOT_FOUND);
}

static void hle_advapi_RegEnumValueW(CPU *c) {
    uint64_t hKey = HLE_ARG0(c);
    uint32_t dwIndex = (uint32_t)HLE_ARG1(c);
    uint64_t lpValueName = HLE_ARG2(c);
    uint64_t lpcchValueName = HLE_ARG3(c);
    uint64_t lpType = HLE_ARG5(c);
    uint64_t lpData = HLE_ARG6(c);
    uint64_t lpcbData = HLE_ARG7(c);

    if (!lpValueName || !lpcchValueName) {
        HLE_RET32(c, 87u); /* ERROR_INVALID_PARAMETER */
        return;
    }

    pthread_mutex_lock(&g_reg_mu);
    reg_key_t *k = reg_find_by_handle(reg_canonical_root(hKey));
    if (!k) {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_INVALID_HANDLE);
        return;
    }

    uint32_t seen = 0;
    const reg_value_t *found = NULL;
    for (int i = 0; i < REG_MAX_VALUES; ++i) {
        if (k->values[i].used) {
            if (seen == dwIndex) {
                found = &k->values[i];
                break;
            }
            seen++;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_NO_MORE_ITEMS);
        return;
    }

    uint32_t name_cap = rd32(lpcchValueName);
    int needed_wchars = se_utf8_to_utf16(found->name, -1, NULL, 0);
    if ((uint32_t)needed_wchars > name_cap) {
        wr32(lpcchValueName, (uint32_t)needed_wchars);
        pthread_mutex_unlock(&g_reg_mu);
        HLE_RET32(c, W32_ERR_MORE_DATA);
        return;
    }

    int written = se_utf8_to_utf16(found->name, -1,
                                   (uint16_t *)(uintptr_t)lpValueName, (int)name_cap);
    wr32(lpcchValueName, (written > 0) ? (uint32_t)(written - 1) : 0u);

    uint32_t rc = reg_copy_value_wide(found, lpType, lpData, lpcbData);
    pthread_mutex_unlock(&g_reg_mu);
    HLE_RET32(c, rc);
}

/* ========================================================================= */
/* 2. COMDLG32.dll (2 imports)                                               */
/* ========================================================================= */

static void hle_comdlg_GetOpenFileNameA(CPU *c) {
    (void)HLE_ARG0(c);
    MISC_LOG("COMDLG32!GetOpenFileNameA cancelled in Landscape Android runtime");
    HLE_RET32(c, 0u);
}

static void hle_comdlg_GetSaveFileNameA(CPU *c) {
    (void)HLE_ARG0(c);
    MISC_LOG("COMDLG32!GetSaveFileNameA cancelled in Landscape Android runtime");
    HLE_RET32(c, 0u);
}

/* ========================================================================= */
/* 3. SHELL32.dll (3 imports)                                                */
/* ========================================================================= */

static void hle_shell32_SHFileOperationA(CPU *c) {
    uint64_t lpFileOp = HLE_ARG0(c);
    if (!lpFileOp) {
        HLE_RET32(c, 87u);
        return;
    }
    /* Win64 SHFILEOPSTRUCTA: hwnd(0), wFunc(8), pFrom(16), pTo(24) */
    uint32_t wFunc = rd32(lpFileOp + 8ull);
    uint64_t pFrom = rd64(lpFileOp + 16ull);
    uint64_t pTo = rd64(lpFileOp + 24ull);

    if (wFunc == 3u /* FO_DELETE */ && pFrom) {
        const char *cur = (const char *)(uintptr_t)pFrom;
        while (*cur) {
            char resolved[1024];
            if (se_vfs_resolve_path(cur, true, resolved, sizeof(resolved))) {
                unlink(resolved);
                rmdir(resolved);
            }
            cur += strlen(cur) + 1;
        }
    } else if ((wFunc == 1u /* FO_MOVE */ || wFunc == 4u /* FO_RENAME */) && pFrom && pTo) {
        char src[1024], dst[1024];
        if (se_vfs_resolve_path((const char *)(uintptr_t)pFrom, true, src, sizeof(src)) &&
            se_vfs_resolve_path((const char *)(uintptr_t)pTo, true, dst, sizeof(dst))) {
            rename(src, dst);
        }
    }
    HLE_RET32(c, 0u);
}

static void hle_shell32_ShellExecuteA(CPU *c) {
    char file[512];
    misc_guest_cstr_to_utf8(HLE_ARG2(c), file, sizeof(file));
    MISC_LOG("SHELL32!ShellExecuteA(%s)", file);
    HLE_RET64(c, 33ull); /* > 32 indicates Win32 shell success */
}

static void hle_shell32_ShellExecuteW(CPU *c) {
    char file[512];
    file[0] = '\0';
    if (HLE_ARG2(c)) {
        se_utf16_to_utf8((const uint16_t *)(uintptr_t)HLE_ARG2(c), -1, file, (int)sizeof(file));
    }
    MISC_LOG("SHELL32!ShellExecuteW(%s)", file);
    HLE_RET64(c, 33ull);
}

static void hle_shell32_SHGetFolderPathW(CPU *c) {
    uint64_t pszPath = HLE_ARG4(c);
    if (!pszPath) {
        HLE_RET32(c, 0x80070057u); /* E_INVALIDARG */
        return;
    }
    const char *dir = se_hle_get_internal_dir();
    if (!dir || !dir[0]) dir = se_hle_get_game_dir();
    if (!dir || !dir[0]) dir = ".";
    se_utf8_to_utf16(dir, -1, (uint16_t *)(uintptr_t)pszPath, 260);
    HLE_RET32(c, 0u); /* S_OK */
}

/* ========================================================================= */
/* 4. VERSION.dll (3 imports): SpaceEngine v0.991.49.2095 VS_FIXEDFILEINFO   */
/* ========================================================================= */

#define SE_VER_BLOCK_SIZE     128u
#define SE_VER_FFI_OFFSET     40u
#define SE_VER_FFI_SIZE       52u
#define SE_VER_TRANS_OFFSET   92u
#define SE_VER_TRANS_SIZE     4u
#define SE_VER_PRODSTR_OFFSET 96u
#define SE_VER_PRODSTR        "0.991.49.2095"
#define SE_VER_PRODSTR_LEN    ((uint32_t)sizeof(SE_VER_PRODSTR))

static int ver_strncasecmp_ascii(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || ca == 0 || cb == 0) return ca - cb;
    }
    return 0;
}

static void fill_se_version_block(uint8_t *dst) {
    memset(dst, 0, SE_VER_BLOCK_SIZE);
    /* VS_VERSION_INFO header */
    dst[0] = (uint8_t)SE_VER_BLOCK_SIZE;
    dst[1] = 0;
    dst[2] = (uint8_t)SE_VER_FFI_SIZE;
    dst[3] = 0;
    dst[4] = 0; /* binary */
    dst[5] = 0;

    static const char k_key[] = "VS_VERSION_INFO";
    for (size_t i = 0; i < sizeof(k_key); ++i) {
        dst[6u + i * 2u] = (uint8_t)k_key[i];
        dst[6u + i * 2u + 1u] = 0;
    }

    uint32_t ffi[13];
    memset(ffi, 0, sizeof(ffi));
    ffi[0] = 0xFEEF04BDu;                  /* dwSignature */
    ffi[1] = 0x00010000u;                  /* dwStrucVersion */
    ffi[2] = (0u << 16) | 991u;            /* dwFileVersionMS: 0.991 */
    ffi[3] = (49u << 16) | 2095u;          /* dwFileVersionLS: 49.2095 */
    ffi[4] = ffi[2];                       /* dwProductVersionMS */
    ffi[5] = ffi[3];                       /* dwProductVersionLS */
    ffi[6] = 0x3Fu;                        /* dwFileFlagsMask */
    ffi[7] = 0x02u;                        /* dwFileFlags: VS_FF_PRERELEASE */
    ffi[8] = 0x00040004u;                  /* dwFileOS: VOS_NT_WINDOWS32 */
    ffi[9] = 1u;                           /* dwFileType: VFT_APP */
    memcpy(dst + SE_VER_FFI_OFFSET, ffi, SE_VER_FFI_SIZE);

    /* VarFileInfo\Translation: US English (0x0409), Unicode (0x04B0) */
    uint16_t trans[2] = { 0x0409u, 0x04B0u };
    memcpy(dst + SE_VER_TRANS_OFFSET, trans, sizeof(trans));

    /* StringFileInfo ProductVersion / FileVersion */
    memcpy(dst + SE_VER_PRODSTR_OFFSET, SE_VER_PRODSTR, SE_VER_PRODSTR_LEN);
}

static void hle_version_GetFileVersionInfoSizeW(CPU *c) {
    uint64_t lpdwHandle = HLE_ARG1(c);
    if (lpdwHandle) wr32(lpdwHandle, 0u);
    HLE_RET32(c, SE_VER_BLOCK_SIZE);
}

static void hle_version_GetFileVersionInfoA(CPU *c) {
    uint32_t dwLen = (uint32_t)HLE_ARG2(c);
    uint64_t lpData = HLE_ARG3(c);
    if (!lpData || dwLen < SE_VER_BLOCK_SIZE) {
        se_win64_teb_set_last_error(122u); /* ERROR_INSUFFICIENT_BUFFER */
        HLE_RET32(c, 0u);
        return;
    }
    uint8_t tmp[SE_VER_BLOCK_SIZE];
    fill_se_version_block(tmp);
    memcpy((void *)(uintptr_t)lpData, tmp, SE_VER_BLOCK_SIZE);
    HLE_RET32(c, 1u);
}

static void hle_version_VerQueryValueA(CPU *c) {
    uint64_t pBlock = HLE_ARG0(c);
    uint64_t lpSubBlock = HLE_ARG1(c);
    uint64_t lplpBuffer = HLE_ARG2(c);
    uint64_t puLen = HLE_ARG3(c);

    if (!pBlock || !lpSubBlock || !lplpBuffer) {
        HLE_RET32(c, 0u);
        return;
    }
    const char *sub = (const char *)(uintptr_t)lpSubBlock;
    if (strcmp(sub, "\\") == 0) {
        wr64(lplpBuffer, pBlock + SE_VER_FFI_OFFSET);
        if (puLen) wr32(puLen, SE_VER_FFI_SIZE);
        HLE_RET32(c, 1u);
        return;
    }
    if (misc_strcasecmp(sub, "\\VarFileInfo\\Translation") == 0) {
        wr64(lplpBuffer, pBlock + SE_VER_TRANS_OFFSET);
        if (puLen) wr32(puLen, SE_VER_TRANS_SIZE);
        HLE_RET32(c, 1u);
        return;
    }
    if (ver_strncasecmp_ascii(sub, "\\StringFileInfo\\", 16u) == 0) {
        const char *leaf = strrchr(sub + 16u, '\\');
        if (leaf && (misc_strcasecmp(leaf, "\\ProductVersion") == 0 ||
                     misc_strcasecmp(leaf, "\\FileVersion") == 0)) {
            wr64(lplpBuffer, pBlock + SE_VER_PRODSTR_OFFSET);
            if (puLen) wr32(puLen, SE_VER_PRODSTR_LEN);
            HLE_RET32(c, 1u);
            return;
        }
    }
    if (puLen) wr32(puLen, 0u);
    HLE_RET32(c, 0u);
}

/* ========================================================================= */
/* 5. openvr_api.dll (8 clean-room stubs)                                    */
/* ========================================================================= */

static void hle_vr_IsRuntimeInstalled(CPU *c) {
    HLE_RET32(c, 0u);
}

static void hle_vr_IsHmdPresent(CPU *c) {
    HLE_RET32(c, 0u);
}

static void hle_vr_InitInternal2(CPU *c) {
    uint64_t peError = HLE_ARG0(c);
    if (peError) wr32(peError, 108u); /* VRInitError_Init_HmdNotFound */
    HLE_RET64(c, 0ull);
}

static void hle_vr_ShutdownInternal(CPU *c) {
    HLE_RET32(c, 0u);
}

static void hle_vr_GetInitToken(CPU *c) {
    HLE_RET32(c, 0u);
}

static void hle_vr_IsInterfaceVersionValid(CPU *c) {
    HLE_RET32(c, 0u);
}

static void hle_vr_GetGenericInterface(CPU *c) {
    uint64_t peError = HLE_ARG1(c);
    if (peError) wr32(peError, 105u); /* VRInitError_Init_InterfaceNotFound */
    HLE_RET64(c, 0ull);
}

static void hle_vr_GetVRInitErrorAsEnglishDescription(CPU *c) {
    (void)HLE_ARG0(c);
    HLE_RET64(c, se_hle_intern_ascii("OpenVR disabled on Android Landscape runtime"));
}

/* ========================================================================= */
/* 6. steam_api64.dll (10 clean-room offline stubs + typed vtables)          */
/* ========================================================================= */

typedef enum se_steam_iface_kind {
    SE_STEAM_IFACE_GENERIC = 0,
    SE_STEAM_IFACE_USER,
    SE_STEAM_IFACE_APPS,
    SE_STEAM_IFACE_FRIENDS,
    SE_STEAM_IFACE_UGC,
    SE_STEAM_IFACE_UTILS,
    SE_STEAM_IFACE_CLIENT,
    SE_STEAM_IFACE_COUNT
} se_steam_iface_kind_t;

#define SE_STEAM_MAX_PENDING_CALLS 32u

typedef struct se_steam_call_result_reg {
    int      used;
    uint64_t pCallback;
    uint64_t hAPICall;
} se_steam_call_result_reg_t;

typedef struct se_steam_ready_ugc_call {
    int      used;
    uint64_t hAPICall;
    uint64_t ugc_handle;
} se_steam_ready_ugc_call_t;

static pthread_mutex_t           g_steam_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t                  g_next_ugc_query = 1ull;
static uint64_t                  g_next_api_call = 0x53450001ull;
static se_steam_call_result_reg_t g_steam_call_results[SE_STEAM_MAX_PENDING_CALLS];
static se_steam_ready_ugc_call_t  g_ready_ugc_calls[SE_STEAM_MAX_PENDING_CALLS];

static void steam_reset_pending_locked(void) {
    memset(g_steam_call_results, 0, sizeof(g_steam_call_results));
    memset(g_ready_ugc_calls, 0, sizeof(g_ready_ugc_calls));
}

static void hle_steam_noop_ret0(CPU *c) {
    HLE_RET64(c, 0ull);
}

static void hle_steam_User_GetSteamID(CPU *c) {
    uint64_t sret_ptr = HLE_ARG1(c);
    uint64_t steam_id = 0x0110000100000001ull;
    if (sret_ptr) wr64(sret_ptr, steam_id);
    HLE_RET64(c, sret_ptr ? sret_ptr : steam_id);
}

static void hle_steam_Apps_GetCurrentGameLanguage(CPU *c) {
    HLE_RET64(c, se_hle_intern_ascii("english"));
}

static void hle_steam_Friends_GetPersonaName(CPU *c) {
    HLE_RET64(c, se_hle_intern_ascii("Player"));
}

static void hle_steam_UGC_CreateQueryUserUGCRequest(CPU *c) {
    (void)HLE_ARG0(c);
    pthread_mutex_lock(&g_steam_lock);
    uint64_t handle = g_next_ugc_query++;
    if (handle == 0ull) handle = g_next_ugc_query++;
    pthread_mutex_unlock(&g_steam_lock);
    MISC_LOG("steam_api64!ISteamUGC::CreateQueryUserUGCRequest -> handle=%llu",
             (unsigned long long)handle);
    HLE_RET64(c, handle);
}

static void hle_steam_UGC_SendQueryUGCRequest(CPU *c) {
    uint64_t handle = HLE_ARG1(c);
    pthread_mutex_lock(&g_steam_lock);
    uint64_t hCall = g_next_api_call++;
    if (hCall == 0ull) hCall = g_next_api_call++;
    size_t slot = SE_STEAM_MAX_PENDING_CALLS;
    for (size_t i = 0; i < SE_STEAM_MAX_PENDING_CALLS; ++i) {
        if (!g_ready_ugc_calls[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == SE_STEAM_MAX_PENDING_CALLS) {
        memmove(&g_ready_ugc_calls[0], &g_ready_ugc_calls[1],
                (SE_STEAM_MAX_PENDING_CALLS - 1u) * sizeof(g_ready_ugc_calls[0]));
        slot = SE_STEAM_MAX_PENDING_CALLS - 1u;
    }
    g_ready_ugc_calls[slot].used = 1;
    g_ready_ugc_calls[slot].hAPICall = hCall;
    g_ready_ugc_calls[slot].ugc_handle = handle;
    pthread_mutex_unlock(&g_steam_lock);
    MISC_LOG("steam_api64!ISteamUGC::SendQueryUGCRequest(handle=%llu) -> hCall=0x%llx",
             (unsigned long long)handle, (unsigned long long)hCall);
    HLE_RET64(c, hCall);
}

static se_steam_iface_kind_t classify_steam_iface_name(const char *ver) {
    if (!ver || !ver[0]) return SE_STEAM_IFACE_GENERIC;
    if (ver_strncasecmp_ascii(ver, "SteamUser", 9u) == 0) return SE_STEAM_IFACE_USER;
    if (ver_strncasecmp_ascii(ver, "SteamApps", 9u) == 0 ||
        ver_strncasecmp_ascii(ver, "STEAMAPPS", 9u) == 0) return SE_STEAM_IFACE_APPS;
    if (ver_strncasecmp_ascii(ver, "SteamFriends", 12u) == 0) return SE_STEAM_IFACE_FRIENDS;
    if (ver_strncasecmp_ascii(ver, "STEAMUGC_INTERFACE_VERSION", 26u) == 0 ||
        ver_strncasecmp_ascii(ver, "SteamUGC", 8u) == 0) return SE_STEAM_IFACE_UGC;
    if (ver_strncasecmp_ascii(ver, "SteamUtils", 10u) == 0) return SE_STEAM_IFACE_UTILS;
    if (ver_strncasecmp_ascii(ver, "SteamClient", 11u) == 0) return SE_STEAM_IFACE_CLIENT;
    return SE_STEAM_IFACE_GENERIC;
}

static se_steam_iface_kind_t classify_steam_ctx_descriptor(uint64_t ctx_va, uint64_t fn_va) {
    uint64_t canon_fn = fn_va ? se_hle_normalize_target(fn_va) : 0ull;
    if (canon_fn == 0x14006AB40ull) return SE_STEAM_IFACE_FRIENDS;
    if (canon_fn == 0x14006AB70ull) return SE_STEAM_IFACE_UTILS;
    if (canon_fn == 0x14006ABA0ull) return SE_STEAM_IFACE_USER;
    if (canon_fn == 0x14006ABD0ull) return SE_STEAM_IFACE_UGC;
    if (canon_fn == 0x14006AC00ull) return SE_STEAM_IFACE_APPS;
    if (canon_fn == 0x14006AC30ull) return SE_STEAM_IFACE_CLIENT;

    uint64_t canon_ctx = ctx_va ? se_hle_normalize_target(ctx_va) : 0ull;
    if (canon_ctx == 0x140812F30ull) return SE_STEAM_IFACE_FRIENDS;
    if (canon_ctx == 0x140812F48ull) return SE_STEAM_IFACE_UTILS;
    if (canon_ctx == 0x140812F60ull) return SE_STEAM_IFACE_USER;
    if (canon_ctx == 0x140812F78ull) return SE_STEAM_IFACE_UGC;
    if (canon_ctx == 0x140812F90ull) return SE_STEAM_IFACE_APPS;
    if (canon_ctx == 0x140812FA8ull) return SE_STEAM_IFACE_CLIENT;
    return SE_STEAM_IFACE_GENERIC;
}

static uint64_t get_steam_interface_obj(se_steam_iface_kind_t kind) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t objs[SE_STEAM_IFACE_COUNT] = { 0 };
    static int initialized = 0;

    if ((unsigned)kind >= (unsigned)SE_STEAM_IFACE_COUNT) {
        kind = SE_STEAM_IFACE_GENERIC;
    }

    pthread_mutex_lock(&mu);
    if (!initialized) {
        uint64_t sent_ret0 = se_hle_register_dynamic(
            "steam_api64.dll", "__DummySteamMethod", hle_steam_noop_ret0);
        uint64_t sent_get_id = se_hle_register_dynamic(
            "steam_api64.dll", "ISteamUser::GetSteamID", hle_steam_User_GetSteamID);
        uint64_t sent_get_lang = se_hle_register_dynamic(
            "steam_api64.dll", "ISteamApps::GetCurrentGameLanguage", hle_steam_Apps_GetCurrentGameLanguage);
        uint64_t sent_get_name = se_hle_register_dynamic(
            "steam_api64.dll", "ISteamFriends::GetPersonaName", hle_steam_Friends_GetPersonaName);
        uint64_t sent_ugc_create = se_hle_register_dynamic(
            "steam_api64.dll", "ISteamUGC::CreateQueryUserUGCRequest", hle_steam_UGC_CreateQueryUserUGCRequest);
        uint64_t sent_ugc_send = se_hle_register_dynamic(
            "steam_api64.dll", "ISteamUGC::SendQueryUGCRequest", hle_steam_UGC_SendQueryUGCRequest);

        for (unsigned k = 0; k < (unsigned)SE_STEAM_IFACE_COUNT; ++k) {
            uint64_t *vtable = (uint64_t *)calloc(128u, sizeof(uint64_t));
            uint64_t *obj = (uint64_t *)calloc(2u, sizeof(uint64_t));
            if (vtable && obj) {
                for (size_t i = 0; i < 128u; ++i) {
                    vtable[i] = sent_ret0;
                }
                if (k == SE_STEAM_IFACE_USER) {
                    vtable[2] = sent_get_id;     /* +0x10 */
                } else if (k == SE_STEAM_IFACE_APPS) {
                    vtable[4] = sent_get_lang;   /* +0x20 */
                } else if (k == SE_STEAM_IFACE_FRIENDS) {
                    vtable[0] = sent_get_name;   /* +0x00 */
                } else if (k == SE_STEAM_IFACE_UGC) {
                    vtable[0] = sent_ugc_create; /* +0x00 */
                    vtable[4] = sent_ugc_send;   /* +0x20 */
                }
                obj[0] = (uint64_t)(uintptr_t)vtable;
                objs[k] = (uint64_t)(uintptr_t)obj;
            }
        }
        initialized = 1;
    }
    uint64_t res = objs[(unsigned)kind];
    pthread_mutex_unlock(&mu);
    return res;
}

static void hle_steam_RestartAppIfNecessary(CPU *c) {
    (void)HLE_ARG0(c);
    HLE_RET32(c, 0u); /* false: do not relaunch via Steam */
}

static void hle_steam_Init(CPU *c) {
    pthread_mutex_lock(&g_steam_lock);
    steam_reset_pending_locked();
    pthread_mutex_unlock(&g_steam_lock);
    MISC_LOG("steam_api64!SteamAPI_Init clean-room offline mode (returning true)");
    HLE_RET32(c, 1u);
}

static void hle_steam_Shutdown(CPU *c) {
    pthread_mutex_lock(&g_steam_lock);
    steam_reset_pending_locked();
    pthread_mutex_unlock(&g_steam_lock);
    HLE_RET32(c, 0u);
}

static void hle_steam_RunCallbacks(CPU *c) {
    for (uint32_t iter = 0; iter < SE_STEAM_MAX_PENDING_CALLS; ++iter) {
        uint64_t pCallback = 0ull;
        uint64_t hAPICall = 0ull;
        uint64_t ugc_handle = 0ull;

        pthread_mutex_lock(&g_steam_lock);
        for (size_t i = 0; i < SE_STEAM_MAX_PENDING_CALLS; ++i) {
            if (g_steam_call_results[i].used) {
                pCallback = g_steam_call_results[i].pCallback;
                hAPICall = g_steam_call_results[i].hAPICall;
                g_steam_call_results[i].used = 0;
                g_steam_call_results[i].pCallback = 0ull;
                g_steam_call_results[i].hAPICall = 0ull;
                break;
            }
        }
        if (pCallback && hAPICall) {
            for (size_t j = 0; j < SE_STEAM_MAX_PENDING_CALLS; ++j) {
                if (g_ready_ugc_calls[j].used && g_ready_ugc_calls[j].hAPICall == hAPICall) {
                    ugc_handle = g_ready_ugc_calls[j].ugc_handle;
                    g_ready_ugc_calls[j].used = 0;
                    g_ready_ugc_calls[j].hAPICall = 0ull;
                    g_ready_ugc_calls[j].ugc_handle = 0ull;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&g_steam_lock);

        if (!pCallback || !hAPICall) {
            break;
        }

        uint64_t vptr = rd64(pCallback);
        uint64_t run_va = vptr ? rd64(vptr + 0x00ull) : 0ull;
        if (!run_va) {
            MISC_LOG("steam_api64!SteamAPI_RunCallbacks skipping null CCallbackBase::Run pCb=0x%llx vptr=0x%llx",
                     (unsigned long long)pCallback, (unsigned long long)vptr);
            continue;
        }

        /* SteamUGCQueryCompleted_t (280 / 0x118 bytes max; zeroed 512-byte buffer). */
        uint8_t param_buf[512];
        memset(param_buf, 0, sizeof(param_buf));
        uint64_t pv = (uint64_t)(uintptr_t)param_buf;
        wr64(pv + 0x00ull, ugc_handle); /* m_handle */
        wr32(pv + 0x08ull, 1u);         /* m_eResult = k_EResultOK */
        wr32(pv + 0x0Cull, 0u);         /* m_unNumResultsReturned = 0 */
        wr32(pv + 0x10ull, 0u);         /* m_unTotalMatchingResults = 0 */
        wr8(pv + 0x14ull, 0u);          /* m_bCachedData = false */

        MISC_LOG("steam_api64!SteamAPI_RunCallbacks invoking CCallResult::Run pCb=0x%llx hCall=0x%llx run=0x%llx",
                 (unsigned long long)pCallback, (unsigned long long)hAPICall, (unsigned long long)run_va);
        (void)se_hle_call_guest(c, run_va, 4, pCallback, pv, 0ull, hAPICall);
    }
    HLE_RET32(c, 0u);
}

static void hle_steam_RegisterCallResult(CPU *c) {
    uint64_t pCallback = HLE_ARG0(c);
    uint64_t hAPICall = HLE_ARG1(c);
    if (pCallback && hAPICall) {
        pthread_mutex_lock(&g_steam_lock);
        size_t slot = SE_STEAM_MAX_PENDING_CALLS;
        for (size_t i = 0; i < SE_STEAM_MAX_PENDING_CALLS; ++i) {
            if (g_steam_call_results[i].used &&
                (g_steam_call_results[i].pCallback == pCallback ||
                 g_steam_call_results[i].hAPICall == hAPICall)) {
                slot = i;
                break;
            }
            if (!g_steam_call_results[i].used && slot == SE_STEAM_MAX_PENDING_CALLS) {
                slot = i;
            }
        }
        if (slot == SE_STEAM_MAX_PENDING_CALLS) {
            slot = 0u;
        }
        g_steam_call_results[slot].used = 1;
        g_steam_call_results[slot].pCallback = pCallback;
        g_steam_call_results[slot].hAPICall = hAPICall;
        pthread_mutex_unlock(&g_steam_lock);
        MISC_LOG("steam_api64!SteamAPI_RegisterCallResult pCb=0x%llx hCall=0x%llx",
                 (unsigned long long)pCallback, (unsigned long long)hAPICall);
    }
    HLE_RET32(c, 0u);
}

static void hle_steam_UnregisterCallResult(CPU *c) {
    uint64_t pCallback = HLE_ARG0(c);
    uint64_t hAPICall = HLE_ARG1(c);
    if (pCallback || hAPICall) {
        pthread_mutex_lock(&g_steam_lock);
        for (size_t i = 0; i < SE_STEAM_MAX_PENDING_CALLS; ++i) {
            if (!g_steam_call_results[i].used) continue;
            int cb_match = (!pCallback || g_steam_call_results[i].pCallback == pCallback);
            int call_match = (!hAPICall || g_steam_call_results[i].hAPICall == hAPICall);
            if (cb_match && call_match) {
                g_steam_call_results[i].used = 0;
                g_steam_call_results[i].pCallback = 0ull;
                g_steam_call_results[i].hAPICall = 0ull;
            }
        }
        if (hAPICall) {
            for (size_t j = 0; j < SE_STEAM_MAX_PENDING_CALLS; ++j) {
                if (g_ready_ugc_calls[j].used && g_ready_ugc_calls[j].hAPICall == hAPICall) {
                    g_ready_ugc_calls[j].used = 0;
                    g_ready_ugc_calls[j].hAPICall = 0ull;
                    g_ready_ugc_calls[j].ugc_handle = 0ull;
                }
            }
        }
        pthread_mutex_unlock(&g_steam_lock);
    }
    HLE_RET32(c, 0u);
}

static void hle_steam_GetHSteamUser(CPU *c) {
    HLE_RET32(c, 1u);
}

static void hle_steam_CreateInterface(CPU *c) {
    char ver[128];
    misc_guest_cstr_to_utf8(HLE_ARG0(c), ver, sizeof(ver));
    HLE_RET64(c, get_steam_interface_obj(classify_steam_iface_name(ver)));
}

static void hle_steam_FindOrCreateUserInterface(CPU *c) {
    char ver[128];
    misc_guest_cstr_to_utf8(HLE_ARG1(c), ver, sizeof(ver));
    HLE_RET64(c, get_steam_interface_obj(classify_steam_iface_name(ver)));
}

static void hle_steam_ContextInit(CPU *c) {
    uint64_t pCtxInitData = HLE_ARG0(c);
    if (!pCtxInitData) {
        HLE_RET64(c, get_steam_interface_obj(SE_STEAM_IFACE_GENERIC));
        return;
    }
    /* SteamInternal_ContextInit receives struct { void *fn; uintptr_t counter; void *iface; } (24 bytes) */
    uint64_t fn_va = rd64(pCtxInitData);
    uint64_t counter = rd64(pCtxInitData + 8ull);
    uint64_t out_ctx = pCtxInitData + 16ull;
    uint64_t iface = get_steam_interface_obj(classify_steam_ctx_descriptor(pCtxInitData, fn_va));
    if (counter != 1ull || rd64(out_ctx) == 0ull) {
        wr64(pCtxInitData + 8ull, 1ull);
        wr64(out_ctx, iface);
    }
    HLE_RET64(c, out_ctx);
}

/* ========================================================================= */
/* Registration                                                              */
/* ========================================================================= */

void se_register_misc_win32_shims(void) {
    /* ADVAPI32.dll (6) */
    se_hle_register_import("ADVAPI32.dll", "RegCreateKeyExA", hle_advapi_RegCreateKeyExA);
    se_hle_register_import("ADVAPI32.dll", "RegOpenKeyExW",   hle_advapi_RegOpenKeyExW);
    se_hle_register_import("ADVAPI32.dll", "RegCloseKey",     hle_advapi_RegCloseKey);
    se_hle_register_import("ADVAPI32.dll", "RegSetValueExA",  hle_advapi_RegSetValueExA);
    se_hle_register_import("ADVAPI32.dll", "RegQueryValueExW", hle_advapi_RegQueryValueExW);
    se_hle_register_import("ADVAPI32.dll", "RegEnumValueW",   hle_advapi_RegEnumValueW);

    /* COMDLG32.dll (2) */
    se_hle_register_import("COMDLG32.dll", "GetOpenFileNameA", hle_comdlg_GetOpenFileNameA);
    se_hle_register_import("COMDLG32.dll", "GetSaveFileNameA", hle_comdlg_GetSaveFileNameA);

    /* SHELL32.dll (3 static + 1 dynamic) */
    se_hle_register_import("SHELL32.dll", "SHFileOperationA", hle_shell32_SHFileOperationA);
    se_hle_register_import("SHELL32.dll", "ShellExecuteA",    hle_shell32_ShellExecuteA);
    se_hle_register_import("SHELL32.dll", "ShellExecuteW",    hle_shell32_ShellExecuteW);
    (void)se_hle_register_dynamic("SHELL32.dll", "SHGetFolderPathW", hle_shell32_SHGetFolderPathW);

    /* VERSION.dll (3) */
    se_hle_register_import("VERSION.dll", "GetFileVersionInfoSizeW", hle_version_GetFileVersionInfoSizeW);
    se_hle_register_import("VERSION.dll", "GetFileVersionInfoA",     hle_version_GetFileVersionInfoA);
    se_hle_register_import("VERSION.dll", "VerQueryValueA",          hle_version_VerQueryValueA);

    /* openvr_api.dll (8) */
    se_hle_register_import("openvr_api.dll", "VR_IsRuntimeInstalled",              hle_vr_IsRuntimeInstalled);
    se_hle_register_import("openvr_api.dll", "VR_IsHmdPresent",                    hle_vr_IsHmdPresent);
    se_hle_register_import("openvr_api.dll", "VR_InitInternal2",                   hle_vr_InitInternal2);
    se_hle_register_import("openvr_api.dll", "VR_ShutdownInternal",                hle_vr_ShutdownInternal);
    se_hle_register_import("openvr_api.dll", "VR_GetInitToken",                    hle_vr_GetInitToken);
    se_hle_register_import("openvr_api.dll", "VR_IsInterfaceVersionValid",         hle_vr_IsInterfaceVersionValid);
    se_hle_register_import("openvr_api.dll", "VR_GetGenericInterface",             hle_vr_GetGenericInterface);
    se_hle_register_import("openvr_api.dll", "VR_GetVRInitErrorAsEnglishDescription", hle_vr_GetVRInitErrorAsEnglishDescription);

    /* steam_api64.dll (10) */
    se_hle_register_import("steam_api64.dll", "SteamAPI_RestartAppIfNecessary",          hle_steam_RestartAppIfNecessary);
    se_hle_register_import("steam_api64.dll", "SteamAPI_Init",                           hle_steam_Init);
    se_hle_register_import("steam_api64.dll", "SteamAPI_Shutdown",                       hle_steam_Shutdown);
    se_hle_register_import("steam_api64.dll", "SteamAPI_RunCallbacks",                   hle_steam_RunCallbacks);
    se_hle_register_import("steam_api64.dll", "SteamAPI_RegisterCallResult",             hle_steam_RegisterCallResult);
    se_hle_register_import("steam_api64.dll", "SteamAPI_UnregisterCallResult",           hle_steam_UnregisterCallResult);
    se_hle_register_import("steam_api64.dll", "SteamAPI_GetHSteamUser",                  hle_steam_GetHSteamUser);
    se_hle_register_import("steam_api64.dll", "SteamInternal_CreateInterface",           hle_steam_CreateInterface);
    se_hle_register_import("steam_api64.dll", "SteamInternal_FindOrCreateUserInterface", hle_steam_FindOrCreateUserInterface);
    se_hle_register_import("steam_api64.dll", "SteamInternal_ContextInit",               hle_steam_ContextInit);
}
