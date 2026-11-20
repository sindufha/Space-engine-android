#ifndef SE_PE_LOADER_H
#define SE_PE_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SE_EXPECTED_EXE_SIZE      9062400ULL
#define SE_EXPECTED_EXE_SHA256    "172154e965da152b61646dbd8fedb4e287533ecd82b03c9bf22ed46eabaeb0d3"
#define SE_EXPECTED_IMAGE_BASE    0x140000000ULL
#define SE_EXPECTED_SIZE_OF_IMAGE 0x01abd000U
#define SE_EXPECTED_ENTRY_RVA     0x000c9b40U
#define SE_MAX_SECTIONS           16

/** Parsed metadata for a single PE32+ section header. */
typedef struct se_pe_section_info {
    char name[9];
    uint32_t virtual_address;
    uint32_t virtual_size;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t characteristics;
} se_pe_section_info_t;

/** Validated metadata extracted from SpaceEngine.exe. */
typedef struct se_pe_info {
    uint64_t file_size;
    char sha256_hex[65];
    uint16_t machine;
    uint16_t pe_magic;
    uint64_t image_base;
    uint32_t size_of_image;
    uint32_t entry_rva;
    uint16_t num_sections;
    se_pe_section_info_t sections[SE_MAX_SECTIONS];
    bool valid;
} se_pe_info_t;

/** Validated status of the SpaceEngine game data directory. */
typedef struct se_game_dir_info {
    bool has_shaders_pak;
    bool has_catalogs_pak;
    bool has_main_def_cfg;
    bool has_folders_cfg;
    bool config_writable;
    bool valid;
} se_game_dir_info_t;

/** Guest PE runtime arena mapped at 0x140000000 + image_delta. */
typedef struct se_pe_arena {
    uint8_t *base;
    size_t size;
    int64_t image_delta;
    uint32_t tls_raw_rva;
    uint32_t tls_raw_size;
    uint32_t tls_zero_size;
    uint32_t tls_index_rva;
    uint32_t reloc_applied;
    uint32_t iat_bound;
    se_pe_info_t pe_info;
} se_pe_arena_t;

/** Forward declaration matching recomp_dispatch.h::hle_import_t layout. */
struct hle_import;

/** Active runtime relocation delta used by GVA(abs) in cpu64.h. */
extern int64_t g_image_delta;

/** Validate SpaceEngine.exe at a filesystem path (size, PE32+ header, SHA-256). */
bool se_pe_validate_exe_path(const char *path, se_pe_info_t *out_info, char *err_buf, size_t err_cap);

/** Validate SpaceEngine.exe from an already-open file descriptor (does not close fd). */
bool se_pe_validate_exe_fd(int fd, se_pe_info_t *out_info, char *err_buf, size_t err_cap);

/** Map PE headers and non-code PE sections into the guest arena, apply .reloc
 *  fixups when relocated, initialize PE TLS index, and zero CRT ISA globals. */
bool se_pe_map_runtime_data(const char *exe_path, se_pe_arena_t *out_arena, char *err_buf, size_t err_cap);

/** Populate IAT slots inside the mapped guest arena with HLE_SENTINEL(i). */
bool se_pe_bind_iat(se_pe_arena_t *arena, uint64_t hle_base, uint32_t hle_stride,
                    const uint64_t *slot_vas, uint32_t count, char *err_buf, size_t err_cap);

/** Return the active mapped guest arena pointer/size (0 if none mapped). */
uint64_t se_pe_active_base(void);
uint32_t se_pe_active_size(void);

/** Release an allocated guest PE arena. */
void se_pe_free_arena(se_pe_arena_t *arena);

/** Validate the SpaceEngine root directory containing data/, config/, and system/folders.cfg. */
bool se_game_validate_dir(const char *dir_path, se_game_dir_info_t *out_info, char *err_buf, size_t err_cap);

/** Write portrait-locked mobile tuning settings into main-user.cfg before engine boot. */
bool se_generate_mobile_config(
    const char *game_dir,
    const char *fallback_internal_dir,
    int surface_w,
    int surface_h,
    char *out_cfg_path,
    size_t out_cap,
    char *err_buf,
    size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* SE_PE_LOADER_H */
