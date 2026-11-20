#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "pe_loader.h"
#include "sha256.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <io.h>
#define SE_DUP _dup
#define SE_FDOPEN _fdopen
#define SE_CLOSE _close
#define SE_MKDIR(p) _mkdir(p)
#include <direct.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#define SE_DUP dup
#define SE_FDOPEN fdopen
#define SE_CLOSE close
#define SE_MKDIR(p) mkdir((p), 0755)
#endif

#define PE_DOS_HEADER_MIN 64U
#define PE_HEADER_BUF_MAX 4096U
#define PE_COFF_MACHINE_AMD64 0x8664U
#define PE_OPT_MAGIC_PE32PLUS 0x020bU

typedef struct {
    const char *section;
    const char *key;
    const char *value;
} se_cfg_override_t;

static char *read_cfg_template_file(const char *path, size_t *out_len) {
    if (!path || path[0] == '\0') {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 8L * 1024L * 1024L) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return buf;
}

static void find_cfg_code_span(const char *line, size_t len, size_t *out_s, size_t *out_e) {
    size_t s = 0u;
    while (s < len && isspace((unsigned char)line[s])) {
        s++;
    }
    size_t i = s;
    bool in_quote = false;
    while (i < len) {
        char ch = line[i];
        if (ch == '"' && (i == 0u || line[i - 1u] != '\\')) {
            in_quote = !in_quote;
        } else if (!in_quote && ch == '/' && i + 1u < len && line[i + 1u] == '/') {
            break;
        }
        i++;
    }
    size_t e = i;
    while (e > s && isspace((unsigned char)line[e - 1u])) {
        e--;
    }
    *out_s = s;
    *out_e = e;
}

static void write_merged_mobile_cfg(
    FILE *fp,
    const char *def_path_primary,
    const char *def_path_alt,
    int surface_w,
    int surface_h) {
    int eff_w = surface_w;
    int eff_h = surface_h;
    if (eff_w <= 0 || eff_h <= 0) {
        eff_w = 1920;
        eff_h = 1080;
    }
    if (eff_h > eff_w) {
        int tmp = eff_w;
        eff_w = eff_h;
        eff_h = tmp;
    }

    char sw_str[32];
    char sh_str[32];
    snprintf(sw_str, sizeof(sw_str), "%d", eff_w);
    snprintf(sh_str, sizeof(sh_str), "%d", eff_h);

    const se_cfg_override_t ov[] = {
        { "Memory", "VideoMemoryStrategy", "0" },
        { "Memory", "VideoMemorySize", "256" },
        { "Memory", "VideoMemoryReserved", "50" },
        { "Memory", "RAMSizeMax", "768" },
        { "Memory", "CacheSizeSolarSystems", "8" },
        { "Memory", "CacheSizeLandTextures", "32" },
        { "Memory", "CacheSizeLandNode", "1024" },
        { "Memory", "CacheSizeStarNode", "4000" },
        { "Memory", "CacheSizeGalNode", "1024" },
        { "Memory", "CacheSizeGalModels", "8" },
        { "Memory", "CacheSizeGalSysModels", "8" },

        { "Vendor", "glVendor", "\"SpaceEngine Android Port\"" },
        { "Vendor", "glRenderer", "\"Android EGL/Zink GLES3 Bridge\"" },
        { "Vendor", "glVersion", "\"4.6.0 SpaceEngine Android Bridge\"" },
        { "Vendor", "glslVersion", "\"4.60 SpaceEngine Android Bridge\"" },
        { "Vendor", "Platform", "\"Win64\"" },
        { "Vendor", "CoreContext", "true" },
        { "Vendor", "CoreContextVersion", "\"4.6\"" },
        { "Vendor", "ShaderVersion", "\"AUTO\"" },
        { "Vendor", "IgnoreShaderBinary", "false" },
        { "Vendor", "LoaderAsyncTextures", "false" },
        { "Vendor", "LoaderAsyncMeshes", "false" },
        { "Vendor", "LoaderAsyncShaders", "false" },

        { "Display", "WindowSizeX", sw_str },
        { "Display", "WindowSizeY", sh_str },
        { "Display", "WindowPosX", "0" },
        { "Display", "WindowPosY", "0" },
        { "Display", "Maximized", "false" },
        { "Display", "FullScreen", "1" },
        { "Display", "ScreenResX", sw_str },
        { "Display", "ScreenResY", sh_str },
        { "Display", "RefreshRate", "60" },
        { "Display", "AspectRatio", "1" },

        { "RenderingQuality", "QualityPreset", "5" },
        { "RenderingQuality", "VSync", "0" },
        { "RenderingQuality", "AnisotropyLevel", "0" },
        { "RenderingQuality", "MSAALevel", "0" },
        { "RenderingQuality", "FXAA", "false" },
        { "RenderingQuality", "Dithering", "false" },
        { "RenderingQuality", "TextureCompress", "false" },

        { "PlanetRendering", "LandLOD", "-2" },
        { "PlanetRendering", "LandTargetRes", "480" },
        { "PlanetRendering", "PlanetDetailTextures", "false" },
        { "PlanetRendering", "DisplaceDetailTextures", "false" },
        { "PlanetRendering", "MipmapsLand", "false" },
        { "PlanetRendering", "PlanetSphereMeshLOD", "3" },
        { "PlanetRendering", "PlanetCubeMeshLOD", "3" },
        { "PlanetRendering", "PlanetShineMode", "0" },
        { "PlanetRendering", "EnablePlanetShine", "false" },
        { "PlanetRendering", "EnableEclipses", "false" },
        { "PlanetRendering", "EnableSelfEmission", "false" },
        { "PlanetRendering", "VolumetricRings", "false" },
        { "PlanetRendering", "AuroraQuality", "0" },
        { "PlanetRendering", "ShipEngineQuality", "0" },

        { "AtmosphereRendering", "AtmoRingsShadow", "false" },

        { "PointRendering", "StarPointMethod", "0" },
        { "PointRendering", "PlanetPointMethod", "0" },
        { "PointRendering", "AutoGalaxyMagnitude", "false" },
        { "PointRendering", "StarMagnLimit", "5.5" },
        { "PointRendering", "GalaxyMagnLimit", "5.5" },
        { "PointRendering", "GlowMagnLimit", "5.5" },

        { "WarpRendering", "BlackHoleQuality", "0" },
        { "WarpRendering", "ShipWarpQuality", "0" },
        { "WarpRendering", "WarpPrecision", "0" },

        { "PostEffects", "EnableBloom", "false" },
        { "PostEffects", "EnableLensFlares", "false" },
        { "PostEffects", "EnableLensGhosts", "false" },
        { "PostEffects", "EnablePseudoFlares", "false" },

        { "TextureRendering", "MaxImpostors", "1" },
        { "TextureRendering", "MaxSkyboxes", "1" },
        { "TextureRendering", "MaxEnvmaps", "1" },
        { "TextureRendering", "SolidTargetRes", "540" },
        { "TextureRendering", "SolidTargetResVR", "540" },
        { "TextureRendering", "SolidResolution", "0.5" },
        { "TextureRendering", "VolumResolution", "0.25" },
        { "TextureRendering", "ImpostorResolution", "0.25" },
        { "TextureRendering", "SkyboxResolution", "0.25" },
        { "TextureRendering", "SkyboxMaxDimensions", "256" },
        { "TextureRendering", "EnvmapDimensions", "256" },
        { "TextureRendering", "FrameFP16", "false" },
        { "TextureRendering", "ImpostorFP16", "false" },
        { "TextureRendering", "EnvmapFP16", "true" },
        { "TextureRendering", "SolidUpscaleMode", "0" },
        { "TextureRendering", "VolumUpscaleMode", "0" },
        { "TextureRendering", "SolidTemporalUpscale", "false" },
        { "TextureRendering", "SolidFXAA", "false" },
        { "TextureRendering", "SolidAutoSharp", "false" },
        { "TextureRendering", "VolumAutoSharp", "false" },
        { "TextureRendering", "VolumBilateral", "false" },
        { "TextureRendering", "FXAAImp", "false" },
        { "TextureRendering", "MipmapsGUI", "false" },
        { "TextureRendering", "MipmapsFrame", "false" },
        { "TextureRendering", "MipmapsEnv", "false" },
        { "TextureRendering", "EnableWarpFBO", "false" },
        { "TextureRendering", "EnableWarpImpostors", "false" },
        { "TextureRendering", "EnableAuroraFBO", "false" },
        { "TextureRendering", "EnableCometFBO", "false" },
        { "TextureRendering", "EnableCometImpostors", "false" },
        { "TextureRendering", "EnableRingsFBO", "false" },

        { "Loader", "LoaderMode", "2" },
        { "Loader", "MaxThreads", "1" },
        { "Loader", "MultithreadedStartup", "false" },
        { "Loader", "PreloadShaders", "0" },
        { "Loader", "LoaderQueueSizeGPU", "16" },
        { "Loader", "LoaderQueueSizeCPU", "32" },
        { "Loader", "SplashPrintInfo", "true" },
        { "Loader", "TimingStrategy", "0" },
        { "Loader", "MaxTilesPerFrame", "1" },

        { "DebugOverlay", "CheckGLErrors", "false" },

        { "Interface", "Use3Dmenu", "false" },
        { "Interface", "ShowSeizureWarning", "false" },

        { "Engine", "PowerSaveMaxFPS", "0" },
    };
    const size_t n_ov = sizeof(ov) / sizeof(ov[0]);
    bool applied[sizeof(ov) / sizeof(ov[0])];
    memset(applied, 0, sizeof(applied));

    fprintf(
        fp,
        "// Generated by SpaceEngine Android Landscape Launcher\n"
        "// Mobile Landscape & Memory Profile (merged over config/main-def.cfg)\n\n");

    size_t tpl_len = 0u;
    char *tpl = read_cfg_template_file(def_path_primary, &tpl_len);
    if (!tpl) {
        tpl = read_cfg_template_file(def_path_alt, &tpl_len);
    }

    if (tpl) {
        char cur_section[128] = {0};
        int depth = 0;
        const char *p = tpl;
        const char *end = tpl + tpl_len;
        while (p < end) {
            const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
            size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            while (line_len > 0u && p[line_len - 1u] == '\r') {
                line_len--;
            }

            size_t s = 0u, e = 0u;
            find_cfg_code_span(p, line_len, &s, &e);
            bool replaced = false;

            if (s < e) {
                if (e - s == 1u && p[s] == '{') {
                    depth++;
                } else if (depth == 0) {
                    size_t tok_end = s;
                    while (tok_end < e && !isspace((unsigned char)p[tok_end]) && p[tok_end] != '{') {
                        tok_end++;
                    }
                    size_t slen = tok_end - s;
                    if (slen > 0u && slen < sizeof(cur_section)) {
                        memcpy(cur_section, p + s, slen);
                        cur_section[slen] = '\0';
                    }
                    if (p[e - 1u] == '{') {
                        depth = 1;
                    }
                } else if (p[s] == '}') {
                    if (depth == 1 && cur_section[0] != '\0') {
                        for (size_t k = 0u; k < n_ov; ++k) {
                            if (!applied[k] && strcmp(ov[k].section, cur_section) == 0) {
                                fprintf(fp, "\t%-23s %s\n", ov[k].key, ov[k].value);
                                applied[k] = true;
                            }
                        }
                    }
                    if (depth > 0) {
                        depth--;
                    }
                    if (depth == 0) {
                        cur_section[0] = '\0';
                    }
                } else if (depth == 1 && cur_section[0] != '\0') {
                    if (p[e - 1u] == '{') {
                        depth++;
                    } else {
                        size_t tok_end = s;
                        while (tok_end < e && !isspace((unsigned char)p[tok_end])) {
                            tok_end++;
                        }
                        size_t klen = tok_end - s;
                        for (size_t k = 0u; k < n_ov; ++k) {
                            if (strcmp(ov[k].section, cur_section) == 0 &&
                                strlen(ov[k].key) == klen &&
                                memcmp(ov[k].key, p + s, klen) == 0) {
                                fprintf(fp, "\t%-23s %s\n", ov[k].key, ov[k].value);
                                applied[k] = true;
                                replaced = true;
                                break;
                            }
                        }
                    }
                } else if (p[e - 1u] == '{') {
                    depth++;
                }
            }

            if (!replaced) {
                if (line_len > 0u) {
                    fwrite(p, 1u, line_len, fp);
                }
                fputc('\n', fp);
            }
            p = nl ? (nl + 1) : end;
        }
        free(tpl);
    }

    /* Emit any override sections not present in template (or all sections when no template exists). */
    for (size_t i = 0u; i < n_ov; ++i) {
        if (applied[i]) {
            continue;
        }
        const char *sec = ov[i].section;
        fprintf(fp, "\n%s\n{\n", sec);
        for (size_t j = i; j < n_ov; ++j) {
            if (!applied[j] && strcmp(ov[j].section, sec) == 0) {
                fprintf(fp, "\t%-23s %s\n", ov[j].key, ov[j].value);
                applied[j] = true;
            }
        }
        fprintf(fp, "}\n");
    }
}

static void set_err(char *buf, size_t cap, const char *fmt, ...) {
    if (!buf || cap == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
    return (uint64_t)read_u32_le(p) | ((uint64_t)read_u32_le(p + 4) << 32);
}

static bool join_path(char *out, size_t cap, const char *dir, const char *rel) {
    if (!dir || !rel || dir[0] == '\0') {
        return false;
    }
    size_t dlen = strlen(dir);
    while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) {
        --dlen;
    }
    int n = snprintf(out, cap, "%.*s/%s", (int)dlen, dir, rel);
    return n > 0 && (size_t)n < cap;
}

static bool file_exists_regular(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

static bool dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static bool ensure_dir_exists(const char *path) {
    if (dir_exists(path)) {
        return true;
    }
    return SE_MKDIR(path) == 0 || dir_exists(path);
}

static bool dir_is_writable(const char *dir_path) {
    char probe[1024];
    if (!join_path(probe, sizeof(probe), dir_path, ".se_write_probe.tmp")) {
        return false;
    }
    FILE *fp = fopen(probe, "wb");
    if (!fp) {
        return false;
    }
    fclose(fp);
    remove(probe);
    return true;
}

static bool validate_pe_header_bytes(
    const uint8_t *hdr,
    size_t hdr_len,
    uint64_t file_size,
    se_pe_info_t *info,
    char *err_buf,
    size_t err_cap) {
    if (hdr_len < PE_DOS_HEADER_MIN || hdr[0] != 'M' || hdr[1] != 'Z') {
        set_err(err_buf, err_cap, "Invalid PE: missing MZ DOS header");
        return false;
    }

    uint32_t lfanew = read_u32_le(hdr + 0x3c);
    if ((uint64_t)lfanew + 24U + 240U > hdr_len) {
        set_err(err_buf, err_cap, "Invalid PE: e_lfanew out of header range (0x%x)", lfanew);
        return false;
    }

    const uint8_t *pe = hdr + lfanew;
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) {
        set_err(err_buf, err_cap, "Invalid PE: missing PE signature");
        return false;
    }

    const uint8_t *coff = pe + 4;
    uint16_t machine = read_u16_le(coff + 0);
    uint16_t num_sections = read_u16_le(coff + 2);
    uint16_t opt_size = read_u16_le(coff + 16);

    if (machine != PE_COFF_MACHINE_AMD64) {
        set_err(err_buf, err_cap, "Unsupported PE architecture 0x%04x (expected x86-64 0x8664)", machine);
        return false;
    }

    const uint8_t *opt = coff + 20;
    if ((size_t)(opt - hdr) + opt_size > hdr_len || opt_size < 112U) {
        set_err(err_buf, err_cap, "Invalid PE: optional header truncated");
        return false;
    }

    uint16_t opt_magic = read_u16_le(opt + 0);
    if (opt_magic != PE_OPT_MAGIC_PE32PLUS) {
        set_err(err_buf, err_cap, "Unsupported PE magic 0x%04x (expected PE32+ 0x020b)", opt_magic);
        return false;
    }

    uint32_t entry_rva = read_u32_le(opt + 16);
    uint64_t image_base = read_u64_le(opt + 24);
    uint32_t size_of_image = read_u32_le(opt + 56);

    if (image_base != SE_EXPECTED_IMAGE_BASE ||
        size_of_image != SE_EXPECTED_SIZE_OF_IMAGE ||
        entry_rva != SE_EXPECTED_ENTRY_RVA) {
        set_err(
            err_buf,
            err_cap,
            "Unexpected SpaceEngine.exe layout (ImageBase=0x%llx SizeOfImage=0x%x Entry=0x%x)",
            (unsigned long long)image_base,
            size_of_image,
            entry_rva);
        return false;
    }

    if (num_sections == 0 || num_sections > SE_MAX_SECTIONS) {
        set_err(err_buf, err_cap, "Unexpected PE section count: %u", num_sections);
        return false;
    }

    const uint8_t *sec_tbl = opt + opt_size;
    size_t sec_bytes = (size_t)num_sections * 40U;
    if ((size_t)(sec_tbl - hdr) + sec_bytes > hdr_len) {
        set_err(err_buf, err_cap, "Invalid PE: section table exceeds header buffer");
        return false;
    }

    info->machine = machine;
    info->pe_magic = opt_magic;
    info->image_base = image_base;
    info->size_of_image = size_of_image;
    info->entry_rva = entry_rva;
    info->num_sections = num_sections;

    for (uint16_t i = 0; i < num_sections; ++i) {
        const uint8_t *s = sec_tbl + (size_t)i * 40U;
        se_pe_section_info_t *dst = &info->sections[i];
        memcpy(dst->name, s, 8);
        dst->name[8] = '\0';
        dst->virtual_size = read_u32_le(s + 8);
        dst->virtual_address = read_u32_le(s + 12);
        dst->raw_size = read_u32_le(s + 16);
        dst->raw_offset = read_u32_le(s + 20);
        dst->characteristics = read_u32_le(s + 36);

        if ((uint64_t)dst->raw_offset + (uint64_t)dst->raw_size > file_size ||
            (uint64_t)dst->virtual_address + (uint64_t)dst->virtual_size > size_of_image) {
            set_err(err_buf, err_cap, "Invalid PE section bounds for '%s'", dst->name);
            return false;
        }
    }

    return true;
}

static bool validate_exe_stream(FILE *fp, se_pe_info_t *out_info, char *err_buf, size_t err_cap) {
    se_pe_info_t local_info;
    memset(&local_info, 0, sizeof(local_info));

    if (fseek(fp, 0, SEEK_SET) != 0) {
        set_err(err_buf, err_cap, "Failed to seek to start of executable");
        return false;
    }

    uint8_t header_buf[PE_HEADER_BUF_MAX];
    size_t hdr_read = fread(header_buf, 1, sizeof(header_buf), fp);
    if (hdr_read < PE_DOS_HEADER_MIN) {
        set_err(err_buf, err_cap, "File is too small to be a PE executable");
        return false;
    }

    se_sha256_ctx_t sha;
    se_sha256_init(&sha);
    se_sha256_update(&sha, header_buf, hdr_read);
    uint64_t total = (uint64_t)hdr_read;

    uint8_t io_buf[65536];
    for (;;) {
        size_t n = fread(io_buf, 1, sizeof(io_buf), fp);
        if (n > 0) {
            se_sha256_update(&sha, io_buf, n);
            total += (uint64_t)n;
        }
        if (n < sizeof(io_buf)) {
            if (ferror(fp)) {
                set_err(err_buf, err_cap, "I/O error while reading executable");
                return false;
            }
            break;
        }
    }

    uint8_t digest[32];
    se_sha256_final(&sha, digest);
    se_sha256_to_hex(digest, local_info.sha256_hex);
    local_info.file_size = total;

    if (total != SE_EXPECTED_EXE_SIZE) {
        set_err(
            err_buf,
            err_cap,
            "Size mismatch: got %llu bytes, expected %llu bytes (SpaceEngine v0.991.49.2095)",
            (unsigned long long)total,
            (unsigned long long)SE_EXPECTED_EXE_SIZE);
        if (out_info) {
            *out_info = local_info;
        }
        return false;
    }

    if (strcmp(local_info.sha256_hex, SE_EXPECTED_EXE_SHA256) != 0) {
        set_err(
            err_buf,
            err_cap,
            "SHA-256 mismatch: got %.16s..., expected %.16s...",
            local_info.sha256_hex,
            SE_EXPECTED_EXE_SHA256);
        if (out_info) {
            *out_info = local_info;
        }
        return false;
    }

    if (!validate_pe_header_bytes(header_buf, hdr_read, total, &local_info, err_buf, err_cap)) {
        if (out_info) {
            *out_info = local_info;
        }
        return false;
    }

    local_info.valid = true;
    if (out_info) {
        *out_info = local_info;
    }
    return true;
}

bool se_pe_validate_exe_path(const char *path, se_pe_info_t *out_info, char *err_buf, size_t err_cap) {
    if (!path || path[0] == '\0') {
        set_err(err_buf, err_cap, "Executable path is empty");
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        set_err(err_buf, err_cap, "Cannot open executable '%s': %s", path, strerror(errno));
        return false;
    }
    bool ok = validate_exe_stream(fp, out_info, err_buf, err_cap);
    fclose(fp);
    return ok;
}

bool se_pe_validate_exe_fd(int fd, se_pe_info_t *out_info, char *err_buf, size_t err_cap) {
    if (fd < 0) {
        set_err(err_buf, err_cap, "Invalid file descriptor (%d)", fd);
        return false;
    }
    int dup_fd = SE_DUP(fd);
    if (dup_fd < 0) {
        set_err(err_buf, err_cap, "Failed to duplicate file descriptor: %s", strerror(errno));
        return false;
    }
    FILE *fp = SE_FDOPEN(dup_fd, "rb");
    if (!fp) {
        SE_CLOSE(dup_fd);
        set_err(err_buf, err_cap, "fdopen failed: %s", strerror(errno));
        return false;
    }
    bool ok = validate_exe_stream(fp, out_info, err_buf, err_cap);
    fclose(fp);
    return ok;
}

static bool should_map_runtime_section(const char *name) {
    /* .text must be mapped too: MSVC x64 embeds switch jump tables and
     * read-only constants directly inside .text (e.g. 0x1401a1e98). */
    return strcmp(name, ".text") == 0 ||
           strcmp(name, ".rdata") == 0 ||
           strcmp(name, ".data") == 0 ||
           strcmp(name, ".pdata") == 0 ||
           strcmp(name, ".gehcont") == 0 ||
           strcmp(name, ".fptable") == 0 ||
           strcmp(name, "_RDATA") == 0 ||
           strcmp(name, ".rsrc") == 0 ||
           strcmp(name, ".reloc") == 0;
}

int64_t g_image_delta = 0;
static uint64_t g_active_arena_base = 0;
static uint32_t g_active_arena_size = 0;

static uint16_t pe_rd16_le(const uint8_t *p) {
    uint16_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint32_t pe_rd32_le(const uint8_t *p) {
    uint32_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

static uint64_t pe_rd64_le(const uint8_t *p) {
    uint64_t v = 0;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void pe_wr64_le(uint8_t *p, uint64_t v) {
    memcpy(p, &v, sizeof(v));
}

static void pe_wr32_le(uint8_t *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}

static bool pe_rva_in_mapped_data(const se_pe_info_t *info, uint32_t rva, size_t width) {
    for (uint16_t i = 0; i < info->num_sections; ++i) {
        const se_pe_section_info_t *sec = &info->sections[i];
        if (!should_map_runtime_section(sec->name)) {
            continue;
        }
        if (strcmp(sec->name, ".text") == 0 || strcmp(sec->name, ".reloc") == 0) {
            continue;
        }
        if (rva >= sec->virtual_address &&
            (uint64_t)rva + (uint64_t)width <= (uint64_t)sec->virtual_address + (uint64_t)sec->virtual_size) {
            return true;
        }
    }
    return false;
}

static uint32_t pe_apply_base_relocs(
    uint8_t *arena,
    size_t arena_size,
    const se_pe_info_t *info,
    uint32_t reloc_rva,
    uint32_t reloc_size,
    int64_t image_delta)
{
    if (image_delta == 0 || reloc_rva == 0 || reloc_size < 8u ||
        (uint64_t)reloc_rva + (uint64_t)reloc_size > (uint64_t)arena_size) {
        return 0;
    }

    uint32_t applied = 0;
    uint32_t off = 0;
    while (off + 8u <= reloc_size) {
        const uint8_t *blk = arena + reloc_rva + off;
        uint32_t page_rva = pe_rd32_le(blk);
        uint32_t blk_size = pe_rd32_le(blk + 4u);
        if (blk_size < 8u || off + blk_size > reloc_size) {
            break;
        }
        uint32_t count = (blk_size - 8u) / 2u;
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t entry = pe_rd16_le(blk + 8u + i * 2u);
            uint16_t type = (uint16_t)(entry >> 12);
            uint32_t target_rva = page_rva + (uint32_t)(entry & 0x0FFFu);
            if (type == 0u) {
                continue; /* IMAGE_REL_BASED_ABSOLUTE */
            }
            if (type == 10u) { /* IMAGE_REL_BASED_DIR64 */
                if ((uint64_t)target_rva + 8u <= (uint64_t)arena_size &&
                    pe_rva_in_mapped_data(info, target_rva, 8u)) {
                    uint64_t orig = pe_rd64_le(arena + target_rva);
                    pe_wr64_le(arena + target_rva, (uint64_t)((int64_t)orig + image_delta));
                    applied++;
                }
            } else if (type == 3u) { /* IMAGE_REL_BASED_HIGHLOW */
                if ((uint64_t)target_rva + 4u <= (uint64_t)arena_size &&
                    pe_rva_in_mapped_data(info, target_rva, 4u)) {
                    uint32_t orig = pe_rd32_le(arena + target_rva);
                    pe_wr32_le(arena + target_rva, (uint32_t)((int64_t)(uint64_t)orig + image_delta));
                    applied++;
                }
            }
        }
        off += blk_size;
    }
    return applied;
}

static void pe_zero_crt_isa_globals(uint8_t *arena, size_t arena_size, uint64_t image_base) {
    /* SpaceEngine CRT / UCRT math ISA globals from recomp/CRT_TAGS.json. */
    static const uint64_t kIsaGlobals[] = {
        0x140773928ull, /* __isa_available */
        0x14077392Cull, /* __isa_enabled */
        0x14081FA64ull, /* __favor */
        0x140820724ull, /* ucrt math __isa_available */
        0x140820728ull, /* ucrt math __isa_enabled */
    };
    for (size_t i = 0; i < sizeof(kIsaGlobals) / sizeof(kIsaGlobals[0]); ++i) {
        if (kIsaGlobals[i] >= image_base) {
            uint64_t rva = kIsaGlobals[i] - image_base;
            if (rva + 4u <= arena_size) {
                pe_wr32_le(arena + rva, 0u);
            }
        }
    }
}

uint64_t se_pe_active_base(void) {
    return g_active_arena_base;
}

uint32_t se_pe_active_size(void) {
    return g_active_arena_size;
}

bool se_pe_bind_iat(
    se_pe_arena_t *arena,
    uint64_t hle_base,
    uint32_t hle_stride,
    const uint64_t *slot_vas,
    uint32_t count,
    char *err_buf,
    size_t err_cap)
{
    if (!arena || !arena->base || !slot_vas) {
        set_err(err_buf, err_cap, "Invalid arguments to se_pe_bind_iat");
        return false;
    }
    uint64_t img_base = arena->pe_info.image_base;
    uint32_t bound = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t slot_va = slot_vas[i];
        if (slot_va < img_base) {
            set_err(err_buf, err_cap, "IAT slot #%u VA %#llx below image base %#llx",
                    i, (unsigned long long)slot_va, (unsigned long long)img_base);
            return false;
        }
        uint64_t rva = slot_va - img_base;
        if (rva + 8u > arena->size) {
            set_err(err_buf, err_cap, "IAT slot #%u RVA %#llx exceeds arena size %zu",
                    i, (unsigned long long)rva, arena->size);
            return false;
        }
        uint64_t sentinel = hle_base + (uint64_t)i * (uint64_t)hle_stride;
        pe_wr64_le(arena->base + rva, sentinel);
        bound++;
    }
    arena->iat_bound = bound;
    return true;
}

bool se_pe_map_runtime_data(const char *exe_path, se_pe_arena_t *out_arena, char *err_buf, size_t err_cap) {
    if (!out_arena) {
        set_err(err_buf, err_cap, "Null arena output pointer");
        return false;
    }
    memset(out_arena, 0, sizeof(*out_arena));

    se_pe_info_t info;
    if (!se_pe_validate_exe_path(exe_path, &info, err_buf, err_cap)) {
        return false;
    }

    size_t arena_size = (size_t)info.size_of_image;
    uint8_t *arena = NULL;
#if defined(_WIN32)
    arena = (uint8_t *)calloc(1, arena_size);
    if (!arena) {
        set_err(err_buf, err_cap, "Failed to allocate %zu-byte guest arena", arena_size);
        return false;
    }
#else
    void *mapped = mmap(
        (void *)(uintptr_t)info.image_base,
        arena_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if (mapped == MAP_FAILED) {
        mapped = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (mapped == MAP_FAILED) {
        set_err(err_buf, err_cap, "mmap(%zu bytes) failed: %s", arena_size, strerror(errno));
        return false;
    }
    arena = (uint8_t *)mapped;
#endif

    FILE *fp = fopen(exe_path, "rb");
    if (!fp) {
        out_arena->base = arena;
        out_arena->size = arena_size;
        se_pe_free_arena(out_arena);
        set_err(err_buf, err_cap, "Re-open '%s' failed: %s", exe_path, strerror(errno));
        return false;
    }

    /* Read PE headers (DOS + NT + section table) into arena + 0. */
    uint8_t dos_buf[64];
    uint32_t size_of_headers = 0x400u;
    uint32_t reloc_rva = 0, reloc_sz = 0;
    uint32_t tls_dir_rva = 0, tls_dir_sz = 0;
    if (fseek(fp, 0L, SEEK_SET) == 0 && fread(dos_buf, 1, sizeof(dos_buf), fp) == sizeof(dos_buf)) {
        uint32_t e_lfanew = pe_rd32_le(dos_buf + 0x3cu);
        uint8_t nt_hdr[0x108];
        if (e_lfanew < 0x1000u &&
            fseek(fp, (long)e_lfanew, SEEK_SET) == 0 &&
            fread(nt_hdr, 1, sizeof(nt_hdr), fp) == sizeof(nt_hdr)) {
            uint32_t hdr_sz = pe_rd32_le(nt_hdr + 24u + 60u);
            if (hdr_sz >= 0x200u && hdr_sz <= arena_size) {
                size_of_headers = hdr_sz;
            }
            uint32_t num_dd = pe_rd32_le(nt_hdr + 24u + 108u);
            if (num_dd > 5u) {
                reloc_rva = pe_rd32_le(nt_hdr + 24u + 112u + 5u * 8u);
                reloc_sz = pe_rd32_le(nt_hdr + 24u + 112u + 5u * 8u + 4u);
            }
            if (num_dd > 9u) {
                tls_dir_rva = pe_rd32_le(nt_hdr + 24u + 112u + 9u * 8u);
                tls_dir_sz = pe_rd32_le(nt_hdr + 24u + 112u + 9u * 8u + 4u);
            }
        }
    }
    if (size_of_headers > arena_size ||
        fseek(fp, 0L, SEEK_SET) != 0 ||
        fread(arena, 1, size_of_headers, fp) != size_of_headers) {
        fclose(fp);
        out_arena->base = arena;
        out_arena->size = arena_size;
        se_pe_free_arena(out_arena);
        set_err(err_buf, err_cap, "Failed copying PE headers (%u bytes)", size_of_headers);
        return false;
    }

    for (uint16_t i = 0; i < info.num_sections; ++i) {
        const se_pe_section_info_t *sec = &info.sections[i];
        if (!should_map_runtime_section(sec->name)) {
            continue;
        }
        uint32_t copy_len = sec->raw_size;
        if (copy_len > sec->virtual_size) {
            copy_len = sec->virtual_size;
        }
        if (copy_len == 0) {
            continue;
        }
        if ((uint64_t)sec->virtual_address + (uint64_t)copy_len > (uint64_t)arena_size ||
            fseek(fp, (long)sec->raw_offset, SEEK_SET) != 0 ||
            fread(arena + sec->virtual_address, 1, copy_len, fp) != copy_len) {
            fclose(fp);
            out_arena->base = arena;
            out_arena->size = arena_size;
            se_pe_free_arena(out_arena);
            set_err(err_buf, err_cap, "Failed reading PE section '%s'", sec->name);
            return false;
        }
    }

    fclose(fp);

    int64_t image_delta = (int64_t)((uintptr_t)arena - (uintptr_t)info.image_base);
    out_arena->base = arena;
    out_arena->size = arena_size;
    out_arena->image_delta = image_delta;
    out_arena->pe_info = info;

    out_arena->reloc_applied = pe_apply_base_relocs(arena, arena_size, &info, reloc_rva, reloc_sz, image_delta);

    /* Extract TLS directory fields and initialize _tls_index = 0. */
    uint64_t live_base = (uint64_t)(uintptr_t)arena;
    if (tls_dir_rva && tls_dir_sz >= 36u && (uint64_t)tls_dir_rva + 40u <= (uint64_t)arena_size) {
        uint64_t start_va = pe_rd64_le(arena + tls_dir_rva);
        uint64_t end_va = pe_rd64_le(arena + tls_dir_rva + 8u);
        uint64_t idx_va = pe_rd64_le(arena + tls_dir_rva + 16u);
        uint32_t zero_fill = pe_rd32_le(arena + tls_dir_rva + 32u);
        if (start_va >= live_base && end_va >= start_va && (end_va - live_base) <= arena_size) {
            out_arena->tls_raw_rva = (uint32_t)(start_va - live_base);
            out_arena->tls_raw_size = (uint32_t)(end_va - start_va);
            out_arena->tls_zero_size = zero_fill;
        }
        if (idx_va >= live_base && (idx_va - live_base + 4u) <= arena_size) {
            out_arena->tls_index_rva = (uint32_t)(idx_va - live_base);
            pe_wr32_le(arena + out_arena->tls_index_rva, 0u);
        }
    }
    /* Fallback explicit _tls_index VA check (0x14081FA68). */
    if (0x14081FA68ull >= info.image_base) {
        uint64_t idx_rva = 0x14081FA68ull - info.image_base;
        if (idx_rva + 4u <= arena_size) {
            pe_wr32_le(arena + idx_rva, 0u);
            if (!out_arena->tls_index_rva) {
                out_arena->tls_index_rva = (uint32_t)idx_rva;
            }
        }
    }

    pe_zero_crt_isa_globals(arena, arena_size, info.image_base);

    g_image_delta = image_delta;
    g_active_arena_base = live_base;
    g_active_arena_size = (uint32_t)arena_size;
    return true;
}

void se_pe_free_arena(se_pe_arena_t *arena) {
    if (!arena || !arena->base) {
        return;
    }
    if ((uint64_t)(uintptr_t)arena->base == g_active_arena_base) {
        g_active_arena_base = 0;
        g_active_arena_size = 0;
        g_image_delta = 0;
    }
#if defined(_WIN32)
    free(arena->base);
#else
    munmap(arena->base, arena->size);
#endif
    memset(arena, 0, sizeof(*arena));
}

bool se_game_validate_dir(const char *dir_path, se_game_dir_info_t *out_info, char *err_buf, size_t err_cap) {
    se_game_dir_info_t local;
    memset(&local, 0, sizeof(local));

    if (!dir_path || dir_path[0] == '\0' || !dir_exists(dir_path)) {
        set_err(err_buf, err_cap, "Directory does not exist or is inaccessible: '%s'", dir_path ? dir_path : "");
        if (out_info) {
            *out_info = local;
        }
        return false;
    }

    char path_buf[1024];
    local.has_shaders_pak =
        join_path(path_buf, sizeof(path_buf), dir_path, "data/shaders/Shaders.pak") &&
        file_exists_regular(path_buf);
    local.has_catalogs_pak =
        join_path(path_buf, sizeof(path_buf), dir_path, "data/catalogs/Catalogs.pak") &&
        file_exists_regular(path_buf);
    local.has_main_def_cfg =
        join_path(path_buf, sizeof(path_buf), dir_path, "config/main-def.cfg") &&
        file_exists_regular(path_buf);
    local.has_folders_cfg =
        join_path(path_buf, sizeof(path_buf), dir_path, "system/folders.cfg") &&
        file_exists_regular(path_buf);

    if (join_path(path_buf, sizeof(path_buf), dir_path, "config")) {
        local.config_writable = dir_is_writable(path_buf);
    }

    if (!local.has_shaders_pak || !local.has_catalogs_pak || !local.has_main_def_cfg || !local.has_folders_cfg) {
        set_err(
            err_buf,
            err_cap,
            "Missing SpaceEngine files in '%s' (Shaders.pak=%d, Catalogs.pak=%d, main-def.cfg=%d, folders.cfg=%d)",
            dir_path,
            local.has_shaders_pak ? 1 : 0,
            local.has_catalogs_pak ? 1 : 0,
            local.has_main_def_cfg ? 1 : 0,
            local.has_folders_cfg ? 1 : 0);
        if (out_info) {
            *out_info = local;
        }
        return false;
    }

    local.valid = true;
    if (out_info) {
        *out_info = local;
    }
    return true;
}

static void backup_desktop_config_once(const char *cfg_path, const char *bak_path) {
    if (!file_exists_regular(cfg_path) || file_exists_regular(bak_path)) {
        return;
    }
    FILE *src = fopen(cfg_path, "rb");
    if (!src) {
        return;
    }
    char probe[128] = {0};
    size_t pr = fread(probe, 1, sizeof(probe) - 1, src);
    probe[pr] = '\0';
    if (strstr(probe, "SpaceEngine Android Portrait Launcher") != NULL ||
    strstr(probe, "SpaceEngine Android Landscape Launcher") != NULL) {
        fclose(src);
        return;
    }
    rewind(src);
    FILE *dst = fopen(bak_path, "wb");
    if (!dst) {
        fclose(src);
        return;
    }
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(dst);
    fclose(src);
}

bool se_generate_mobile_config(
    const char *game_dir,
    const char *fallback_internal_dir,
    int surface_w,
    int surface_h,
    char *out_cfg_path,
    size_t out_cap,
    char *err_buf,
    size_t err_cap) {
    if (surface_w <= 0 || surface_h <= 0) {
        surface_w = 2400;
        surface_h = 1080;
    }
    if (surface_w < surface_h) {
        int tmp = surface_w;
        surface_w = surface_h;
        surface_h = tmp;
    }

    char cfg_dir[1024] = {0};
    char cfg_path[1024] = {0};
    char bak_path[1024] = {0};
    bool target_ready = false;

    if (game_dir && game_dir[0] != '\0' &&
        join_path(cfg_dir, sizeof(cfg_dir), game_dir, "config") &&
        ensure_dir_exists(cfg_dir) &&
        dir_is_writable(cfg_dir) &&
        join_path(cfg_path, sizeof(cfg_path), cfg_dir, "main-user.cfg") &&
        join_path(bak_path, sizeof(bak_path), cfg_dir, "main-user.desktop.bak")) {
        backup_desktop_config_once(cfg_path, bak_path);
        target_ready = true;
    }

    if (!target_ready && fallback_internal_dir && fallback_internal_dir[0] != '\0' &&
        join_path(cfg_dir, sizeof(cfg_dir), fallback_internal_dir, "config") &&
        ensure_dir_exists(cfg_dir) &&
        join_path(cfg_path, sizeof(cfg_path), cfg_dir, "main-user.cfg")) {
        target_ready = true;
    }

    if (!target_ready) {
        set_err(err_buf, err_cap, "No writable config directory available for main-user.cfg");
        return false;
    }

    FILE *fp = fopen(cfg_path, "wb");
    if (!fp) {
        set_err(err_buf, err_cap, "Failed opening '%s' for write: %s", cfg_path, strerror(errno));
        return false;
    }

    char def_dir[1024] = {0};
    char def_path_game[1024] = {0};
    char def_path_alt[1024] = {0};
    if (game_dir && game_dir[0] != '\0' &&
        join_path(def_dir, sizeof(def_dir), game_dir, "config")) {
        (void)join_path(def_path_game, sizeof(def_path_game), def_dir, "main-def.cfg");
    }
    if (fallback_internal_dir && fallback_internal_dir[0] != '\0' &&
        join_path(def_dir, sizeof(def_dir), fallback_internal_dir, "config")) {
        (void)join_path(def_path_alt, sizeof(def_path_alt), def_dir, "main-def.cfg");
    }

    write_merged_mobile_cfg(fp, def_path_game, def_path_alt, surface_w, surface_h);

    fclose(fp);

    /* Also sync fallback_internal_dir/config/main-user.cfg if present so an older
     * internal copy never shadows game_dir/config/main-user.cfg in VFS resolution. */
    if (fallback_internal_dir && fallback_internal_dir[0] != '\0') {
        char int_cfg_dir[1024] = {0};
        char int_cfg_path[1024] = {0};
        if (join_path(int_cfg_dir, sizeof(int_cfg_dir), fallback_internal_dir, "config") &&
            ensure_dir_exists(int_cfg_dir) &&
            join_path(int_cfg_path, sizeof(int_cfg_path), int_cfg_dir, "main-user.cfg") &&
            strcmp(int_cfg_path, cfg_path) != 0) {
            FILE *fp_int = fopen(int_cfg_path, "wb");
            if (fp_int) {
                write_merged_mobile_cfg(fp_int, def_path_game, def_path_alt, surface_w, surface_h);
                fclose(fp_int);
            }
        }
    }

    if (out_cfg_path && out_cap > 0) {
        snprintf(out_cfg_path, out_cap, "%s", cfg_path);
    }
    return true;
}
