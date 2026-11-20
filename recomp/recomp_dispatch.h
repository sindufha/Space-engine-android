#pragma once
#include "cpu64.h"

typedef struct { uint64_t va; void (*fn)(CPU *); } recomp_entry_t;
extern const recomp_entry_t recomp_dispatch_table[];
extern const uint32_t recomp_dispatch_count;

/* Import boundary. The loader writes HLE_SENTINEL(i) into IAT
 * slot i; dispatch() routes that range to the HLE layer. */
#define HLE_BASE   0xE5E30000ull
#define HLE_STRIDE 8
#define HLE_COUNT  420
#define HLE_SENTINEL(i) (HLE_BASE + (uint64_t)(i) * HLE_STRIDE)

typedef struct { uint64_t slot; const char *dll; const char *name; } hle_import_t;
extern const hle_import_t recomp_imports[];
