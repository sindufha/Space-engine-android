/*
 * shader_es3.c — Desktop GLSL (#version 460 core) -> GLSL ES 3.20 (#version 320 es) translator.
 *
 * Used automatically in Android GLES3 fallback mode (!g_egl_desktop_gl) when Mesa Zink
 * is unavailable (e.g. AdrenoGLES-0).
 */

#include "shader_es3.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================= */
/* Growable String Builder                                                   */
/* ========================================================================= */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   oom;
} strbuf_t;

static void sb_init(strbuf_t *sb, size_t initial_cap) {
    sb->len = 0u;
    sb->cap = (initial_cap < 64u) ? 64u : initial_cap;
    sb->data = (char *)malloc(sb->cap);
    sb->oom = (sb->data == NULL);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static void sb_free(strbuf_t *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0u;
    sb->cap = 0u;
}

static bool sb_ensure(strbuf_t *sb, size_t extra) {
    if (sb->oom) return false;
    size_t need = sb->len + extra + 1u;
    if (need <= sb->cap) return true;
    size_t ncap = sb->cap;
    while (ncap < need) {
        if (ncap > ((size_t)-1) / 2u) {
            ncap = need;
            break;
        }
        ncap *= 2u;
    }
    char *np = (char *)realloc(sb->data, ncap);
    if (!np) {
        sb->oom = true;
        return false;
    }
    sb->data = np;
    sb->cap = ncap;
    return true;
}

static void sb_append_n(strbuf_t *sb, const char *s, size_t n) {
    if (!s || n == 0u || !sb_ensure(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(strbuf_t *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_char(strbuf_t *sb, char c) {
    if (!sb_ensure(sb, 1u)) return;
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

static char *sb_detach(strbuf_t *sb) {
    if (sb->oom) {
        sb_free(sb);
        return NULL;
    }
    char *r = sb->data;
    sb->data = NULL;
    sb->len = 0u;
    sb->cap = 0u;
    return r;
}

/* ========================================================================= */
/* Lexer / Identifier Helpers                                                */
/* ========================================================================= */

static bool is_ident_start(char c) {
    unsigned char uc = (unsigned char)c;
    return isalpha(uc) || uc == '_';
}

static bool is_ident_char(char c) {
    unsigned char uc = (unsigned char)c;
    return isalnum(uc) || uc == '_';
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/* Skip //...\n or /\*...\*\/ starting at p; returns pointer after comment (or p if not comment). */
static size_t comment_span(const char *p) {
    if (p[0] == '/' && p[1] == '/') {
        size_t n = 2u;
        while (p[n] && p[n] != '\n') ++n;
        return n;
    }
    if (p[0] == '/' && p[1] == '*') {
        size_t n = 2u;
        while (p[n]) {
            if (p[n] == '*' && p[n + 1] == '/') return n + 2u;
            ++n;
        }
        return n;
    }
    return 0u;
}

static bool ident_equals(const char *id, size_t id_len, const char *lit) {
    size_t ll = strlen(lit);
    return id_len == ll && memcmp(id, lit, ll) == 0;
}

/* Find whole-word occurrence outside comments in buffer, returning offset or (size_t)-1. */
static bool contains_code_token(const char *src, const char *tok) {
    size_t tlen = strlen(tok);
    for (size_t i = 0u; src[i];) {
        size_t cspan = comment_span(src + i);
        if (cspan > 0u) {
            i += cspan;
            continue;
        }
        if (is_ident_start(src[i])) {
            size_t j = i + 1u;
            while (is_ident_char(src[j])) ++j;
            if ((j - i) == tlen && memcmp(src + i, tok, tlen) == 0) {
                return true;
            }
            i = j;
            continue;
        }
        ++i;
    }
    return false;
}

/* Check whether an identifier in code matches layout(... index ...) for blend_func_extended */
static bool uses_dual_source_blend(const char *src) {
    for (size_t i = 0u; src[i];) {
        size_t cspan = comment_span(src + i);
        if (cspan > 0u) { i += cspan; continue; }
        if (strncmp(src + i, "layout", 6) == 0 &&
            (i == 0u || !is_ident_char(src[i - 1])) &&
            !is_ident_char(src[i + 6])) {
            const char *p = skip_ws(src + i + 6);
            if (*p == '(') {
                int depth = 0;
                const char *q = p;
                while (*q) {
                    if (*q == '(') ++depth;
                    else if (*q == ')') { --depth; if (depth == 0) break; }
                    else if (strncmp(q, "index", 5) == 0 &&
                             !is_ident_char(q[-1]) && !is_ident_char(q[5])) {
                        return true;
                    }
                    ++q;
                }
            }
        }
        ++i;
    }
    return false;
}

/* ========================================================================= */
/* Normalize Line Endings & Early-Out Check                                  */
/* ========================================================================= */

static char *normalize_newlines(const char *src) {
    size_t n = strlen(src);
    strbuf_t sb;
    sb_init(&sb, n + 16u);
    for (size_t i = 0u; i < n; ++i) {
        if (src[i] == '\r') {
            sb_append_char(&sb, '\n');
            if (i + 1u < n && src[i + 1u] == '\n') ++i;
        } else {
            sb_append_char(&sb, src[i]);
        }
    }
    return sb_detach(&sb);
}

static bool is_already_es_shader(const char *src) {
    const char *p = src;
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (strncmp(p, "#version", 8) != 0) return false;
    p += 8;
    while (*p && *p != '\n') {
        if (p[0] == 'e' && p[1] == 's' && !is_ident_char(p[2]) &&
            (p == src || !is_ident_char(p[-1]))) {
            return true;
        }
        ++p;
    }
    return false;
}

/* ========================================================================= */
/* Collector for sampler1D Variables & Unknown Preprocessor Macros           */
/* ========================================================================= */

#define MAX_S1D_NAMES 32
typedef struct {
    char names[MAX_S1D_NAMES][64];
    int  count;
} s1d_list_t;

static void s1d_add(s1d_list_t *list, const char *name, size_t len) {
    if (len == 0u || len >= 63u || list->count >= MAX_S1D_NAMES) return;
    for (int i = 0; i < list->count; ++i) {
        if (strlen(list->names[i]) == len && memcmp(list->names[i], name, len) == 0) {
            return;
        }
    }
    memcpy(list->names[list->count], name, len);
    list->names[list->count][len] = '\0';
    list->count++;
}

static bool s1d_contains(const s1d_list_t *list, const char *name, size_t len) {
    for (int i = 0; i < list->count; ++i) {
        if (strlen(list->names[i]) == len && memcmp(list->names[i], name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* Collect variables declared as sampler1D/isampler1D/usampler1D */
static void collect_sampler1d_names(const char *src, s1d_list_t *list) {
    memset(list, 0, sizeof(*list));
    for (size_t i = 0u; src[i];) {
        size_t cspan = comment_span(src + i);
        if (cspan > 0u) { i += cspan; continue; }
        size_t tok_len = 0u;
        if ((i == 0u || !is_ident_char(src[i - 1]))) {
            if (strncmp(src + i, "sampler1D", 9) == 0 && !is_ident_char(src[i + 9])) tok_len = 9u;
            else if (strncmp(src + i, "isampler1D", 10) == 0 && !is_ident_char(src[i + 10])) tok_len = 10u;
            else if (strncmp(src + i, "usampler1D", 10) == 0 && !is_ident_char(src[i + 10])) tok_len = 10u;
        }
        if (tok_len > 0u) {
            const char *p = skip_ws(src + i + tok_len);
            /* Parse comma-separated identifiers up to ';' or ')' */
            while (*p && *p != ';' && *p != ')') {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',') ++p;
                if (*p == ';' || *p == ')' || !*p) break;
                if (is_ident_start(*p)) {
                    const char *id = p;
                    while (is_ident_char(*p)) ++p;
                    size_t idl = (size_t)(p - id);
                    if (!ident_equals(id, idl, "in") &&
                        !ident_equals(id, idl, "out") &&
                        !ident_equals(id, idl, "inout")) {
                        s1d_add(list, id, idl);
                    }
                    p = skip_ws(p);
                    if (*p == '[') {
                        while (*p && *p != ']') ++p;
                        if (*p == ']') ++p;
                    }
                } else {
                    break;
                }
            }
            i += tok_len;
        } else {
            ++i;
        }
    }
}

#define MAX_GUARD_MACROS 64
typedef struct {
    char names[MAX_GUARD_MACROS][64];
    int  count;
} macro_list_t;

static void macro_add(macro_list_t *m, const char *name, size_t len) {
    if (len == 0u || len >= 63u || m->count >= MAX_GUARD_MACROS) return;
    if (ident_equals(name, len, "defined")) return;
    for (int i = 0; i < m->count; ++i) {
        if (strlen(m->names[i]) == len && memcmp(m->names[i], name, len) == 0) return;
    }
    memcpy(m->names[m->count], name, len);
    m->names[m->count][len] = '\0';
    m->count++;
}

static bool macro_contains(const macro_list_t *m, const char *name, size_t len) {
    for (int i = 0; i < m->count; ++i) {
        if (strlen(m->names[i]) == len && memcmp(m->names[i], name, len) == 0) return true;
    }
    return false;
}

/* Scan #if / #elif lines to find macros evaluated in integer expressions without defined(). */
static void collect_if_eval_macros(const char *src, macro_list_t *out_eval, macro_list_t *defined_top) {
    memset(out_eval, 0, sizeof(*out_eval));
    memset(defined_top, 0, sizeof(*defined_top));
    int if_depth = 0;

    const char *p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        const char *s = p;
        const char *end = p + llen;
        while (s < end && (*s == ' ' || *s == '\t')) ++s;
        if (s < end && *s == '#') {
            ++s;
            while (s < end && (*s == ' ' || *s == '\t')) ++s;
            const char *kw = s;
            while (s < end && is_ident_char(*s)) ++s;
            size_t kwl = (size_t)(s - kw);
            if (ident_equals(kw, kwl, "ifdef") || ident_equals(kw, kwl, "ifndef")) {
                if_depth++;
            } else if (ident_equals(kw, kwl, "endif")) {
                if (if_depth > 0) if_depth--;
            } else if (ident_equals(kw, kwl, "define")) {
                while (s < end && (*s == ' ' || *s == '\t')) ++s;
                const char *id = s;
                while (s < end && is_ident_char(*s)) ++s;
                if (if_depth == 0 && s > id && s < end && *s != '(') {
                    macro_add(defined_top, id, (size_t)(s - id));
                }
            } else if (ident_equals(kw, kwl, "if") || ident_equals(kw, kwl, "elif")) {
                if (ident_equals(kw, kwl, "if")) if_depth++;
                /* Collect identifiers not inside defined(...) */
                while (s < end) {
                    if (s + 1 < end && s[0] == '/' && s[1] == '/') break;
                    if (s + 1 < end && s[0] == '/' && s[1] == '*') {
                        s += 2;
                        while (s + 1 < end && !(s[0] == '*' && s[1] == '/')) ++s;
                        if (s + 1 < end) s += 2;
                        continue;
                    }
                    if (is_ident_start(*s)) {
                        const char *id = s;
                        while (s < end && is_ident_char(*s)) ++s;
                        size_t idl = (size_t)(s - id);
                        if (ident_equals(id, idl, "defined")) {
                            while (s < end && (*s == ' ' || *s == '\t')) ++s;
                            if (s < end && *s == '(') {
                                int d = 1;
                                ++s;
                                while (s < end && d > 0) {
                                    if (*s == '(') ++d;
                                    else if (*s == ')') --d;
                                    ++s;
                                }
                            } else {
                                while (s < end && is_ident_char(*s)) ++s;
                            }
                        } else {
                            /* Skip function-like macro calls ID(...) */
                            const char *after = s;
                            while (after < end && (*after == ' ' || *after == '\t')) ++after;
                            if (after >= end || *after != '(') {
                                macro_add(out_eval, id, idl);
                            }
                        }
                    } else {
                        ++s;
                    }
                }
            }
        }
        p = eol ? (eol + 1) : end;
    }
}

/* ========================================================================= */
/* Pass A: Line-level cleanup & preprocessor / layout fixes                  */
/* ========================================================================= */

static bool line_is_stripped_extension(const char *s, size_t len) {
    static const char *const k_strip_exts[] = {
        "GL_ARB_conservative_depth",
        "GL_EXT_gpu_shader4",
        "GL_EXT_texture_array",
        "GL_ARB_shader_bit_encoding",
        "GL_ARB_gpu_shader5",
        "GL_ARB_gpu_shader_fp64",
        "GL_ARB_explicit_attrib_location",
        "GL_ARB_shading_language_420pack",
        "GL_ARB_shader_draw_parameters",
    };
    const char *p = s;
    const char *end = s + len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p != '#') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if ((size_t)(end - p) < 9u || memcmp(p, "extension", 9) != 0 ||
        (p + 9 < end && is_ident_char(p[9]))) {
        return false;
    }
    p += 9;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    for (size_t k = 0u; k < sizeof(k_strip_exts) / sizeof(k_strip_exts[0]); ++k) {
        size_t el = strlen(k_strip_exts[k]);
        if ((size_t)(end - p) >= el && memcmp(p, k_strip_exts[k], el) == 0 &&
            (p + el == end || !is_ident_char(p[el]))) {
            return true;
        }
    }
    return false;
}

static bool line_is_version_directive(const char *s, size_t len) {
    const char *p = s;
    const char *end = s + len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p != '#') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if ((size_t)(end - p) >= 7u && memcmp(p, "version", 7) == 0 &&
        (p + 7 == end || !is_ident_char(p[7]))) {
        return true;
    }
    if ((size_t)(end - p) >= 12u && memcmp(p, "auto_version", 12) == 0 &&
        (p + 12 == end || !is_ident_char(p[12]))) {
        return true;
    }
    return false;
}

/* Strip `layout(depth_less) out float gl_FragDepth;` or `layout(origin_upper_left) in vec4 gl_FragCoord;` */
static bool line_is_fragdepth_or_origin_upper_left(const char *s, size_t len, bool *was_origin_upper) {
    /* Quick search within line outside leading comment */
    const char *p = s;
    const char *end = s + len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if ((size_t)(end - p) >= 2u && p[0] == '/' && p[1] == '/') return false;

    /* Check bounded substring within line */
    char tmp[1024];
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, s, len);
    tmp[len] = '\0';

    if (strstr(tmp, "origin_upper_left") && strstr(tmp, "gl_FragCoord")) {
        *was_origin_upper = true;
        return true;
    }
    if ((strstr(tmp, "depth_less") || strstr(tmp, "depth_greater") ||
         strstr(tmp, "depth_unchanged") || strstr(tmp, "depth_any")) &&
        strstr(tmp, "gl_FragDepth")) {
        *was_origin_upper = false;
        return true;
    }
    return false;
}

static bool line_rewrite_ship_pbr_global(const char *s, size_t len, strbuf_t *out) {
    const char *p = s;
    const char *end = s + len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if ((size_t)(end - p) >= 24u && memcmp(p, "vec2 uEnvironmentSize", 21) == 0 && !is_ident_char(p[21])) {
        const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));
        if (eq) {
            sb_append(out, "#define uEnvironmentSize (EnvMapParams.xx)\n");
            return true;
        }
    }
    if ((size_t)(end - p) >= 28u && memcmp(p, "vec2 uEnvironmentLodRange", 25) == 0 && !is_ident_char(p[25])) {
        const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));
        if (eq) {
            sb_append(out, "#define uEnvironmentLodRange vec2(0.0, EnvMapParams.y)\n");
            return true;
        }
    }
    return false;
}

static bool line_rewrite_det_tex_max_level(const char *s, size_t len, strbuf_t *out) {
    char tmp[256];
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    if (strstr(tmp, "#define") && strstr(tmp, "DET_TEX_MAX_LEVEL") && strstr(tmp, "10") &&
        !strstr(tmp, "10.0")) {
        sb_append(out, "#define DET_TEX_MAX_LEVEL 10.0\n");
        return true;
    }
    return false;
}

static bool line_rewrite_defined_macro(const char *s, size_t len, strbuf_t *out) {
    /* Convert `#define NAME <expr containing defined(...)>` into #if/#else #define NAME 1/0
     * because GLSL ES forbids expanding `defined()` inside `#if` macros. */
    const char *p = s;
    const char *end = s + len;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || *p != '#') return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if ((size_t)(end - p) < 6u || memcmp(p, "define", 6) != 0 ||
        (p + 6 < end && p[6] != ' ' && p[6] != '\t')) {
        return false;
    }
    p += 6;
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end || !is_ident_start(*p)) return false;
    const char *name = p;
    while (p < end && is_ident_char(*p)) ++p;
    size_t name_len = (size_t)(p - name);
    if (p < end && *p == '(') return false; /* function-like macro */
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    if (p >= end) return false;

    /* Strip trailing // comment on same line from condition */
    const char *cond_end = end;
    for (const char *c = p; c + 1 < end; ++c) {
        if (c[0] == '/' && c[1] == '/') {
            cond_end = c;
            break;
        }
    }
    while (cond_end > p && (cond_end[-1] == ' ' || cond_end[-1] == '\t')) --cond_end;

    /* Check for token `defined` inside condition */
    bool has_defined = false;
    for (const char *c = p; c + 7 <= cond_end; ++c) {
        if (memcmp(c, "defined", 7) == 0 &&
            (c == p || !is_ident_char(c[-1])) &&
            (c + 7 == cond_end || !is_ident_char(c[7]))) {
            has_defined = true;
            break;
        }
    }
    if (!has_defined) return false;

    sb_append(out, "#undef ");
    sb_append_n(out, name, name_len);
    sb_append(out, "\n#if ");
    sb_append_n(out, p, (size_t)(cond_end - p));
    sb_append(out, "\n#define ");
    sb_append_n(out, name, name_len);
    sb_append(out, " 1\n#else\n#define ");
    sb_append_n(out, name, name_len);
    sb_append(out, " 0\n#endif\n");
    return true;
}

static char *pass_lines_and_macros(const char *src,
                                   const macro_list_t *eval_macros,
                                   bool has_viewport,
                                   bool *out_used_fragcoord_flip) {
    size_t n = strlen(src);
    strbuf_t sb;
    sb_init(&sb, n + 4096u);
    *out_used_fragcoord_flip = false;

    /* Emit default = 0 guards for any #if macro not defined unconditionally at file top;
     * later non-conditional #define for that same macro gets #undef first to prevent
     * ES "Macro redefined; different substitutions". Wait: we emit guards right AFTER
     * SpaceEngine's auto_defines (which always precede #ifdef _VERTEX_/#else or uniforms),
     * OR emit them at the very start and #undef before every subsequent #define!
     * Actually, putting `#ifndef M\n#define M 0\n#endif` right BEFORE a subsequent
     * top-level `#if` isn't even needed if `#undef M` guards `#define M`! */
    macro_list_t auto_guarded;
    memset(&auto_guarded, 0, sizeof(auto_guarded));
    bool emitted_macro_guards = false;

    /* Track whether inside a block comment when looking for header end */
    bool in_block_comment = false;

    const char *p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);

        const char *trim = p;
        const char *lend = p + llen;
        while (trim < lend && (*trim == ' ' || *trim == '\t')) ++trim;

        bool line_starts_comment = in_block_comment;
        for (size_t ci = 0u; ci < llen; ++ci) {
            if (!in_block_comment) {
                if (p[ci] == '/' && ci + 1u < llen && p[ci + 1u] == '/') break;
                if (p[ci] == '/' && ci + 1u < llen && p[ci + 1u] == '*') {
                    in_block_comment = true;
                    line_starts_comment = true;
                    ++ci;
                }
            } else {
                if (p[ci] == '*' && ci + 1u < llen && p[ci + 1u] == '/') {
                    in_block_comment = false;
                    ++ci;
                }
            }
        }

        bool is_top_define = false;
        const char *def_id = NULL;
        size_t def_id_len = 0u;
        if (!line_starts_comment && trim < lend && *trim == '#') {
            const char *q = trim + 1;
            while (q < lend && (*q == ' ' || *q == '\t')) ++q;
            if ((size_t)(lend - q) >= 6u && memcmp(q, "define", 6) == 0 &&
                (q + 6 < lend && (q[6] == ' ' || q[6] == '\t'))) {
                q += 6;
                while (q < lend && (*q == ' ' || *q == '\t')) ++q;
                if (q < lend && is_ident_start(*q)) {
                    def_id = q;
                    while (q < lend && is_ident_char(*q)) ++q;
                    def_id_len = (size_t)(q - def_id);
                    if (q < lend && *q != '(') is_top_define = true;
                }
            }
        }

        if (!emitted_macro_guards && trim < lend && !line_starts_comment) {
            /* Emit before the first real non-comment, non-#version/#define top-level directive or code */
            bool keep_before = line_is_version_directive(p, llen) || is_top_define ||
                               (lend - trim >= 2 && trim[0] == '/' && trim[1] == '/');
            if (!keep_before) {
                for (int mi = 0; mi < eval_macros->count; ++mi) {
                    const char *mname = eval_macros->names[mi];
                    sb_append(&sb, "#ifndef ");
                    sb_append(&sb, mname);
                    sb_append(&sb, "\n#define ");
                    sb_append(&sb, mname);
                    sb_append(&sb, " 0\n#endif\n");
                    macro_add(&auto_guarded, mname, strlen(mname));
                }
                emitted_macro_guards = true;
            }
        }

        if (line_is_version_directive(p, llen) || line_is_stripped_extension(p, llen)) {
            sb_append_char(&sb, '\n');
            p = eol ? (eol + 1) : lend;
            continue;
        }

        bool was_origin_upper = false;
        if (line_is_fragdepth_or_origin_upper_left(p, llen, &was_origin_upper)) {
            if (was_origin_upper && has_viewport) {
                sb_append(&sb,
                          "vec4 _se_fragcoord() { return vec4(gl_FragCoord.x, "
                          "(1.0 / max(abs(Viewport.w), 1e-6)) - gl_FragCoord.y, gl_FragCoord.zw); }\n");
                *out_used_fragcoord_flip = true;
            } else {
                sb_append(&sb, "// stripped ES-unsupported frag layout\n");
            }
            p = eol ? (eol + 1) : lend;
            continue;
        }

        if (line_rewrite_ship_pbr_global(p, llen, &sb)) {
            p = eol ? (eol + 1) : lend;
            continue;
        }

        if (line_rewrite_det_tex_max_level(p, llen, &sb)) {
            p = eol ? (eol + 1) : lend;
            continue;
        }

        if (!line_starts_comment && line_rewrite_defined_macro(p, llen, &sb)) {
            p = eol ? (eol + 1) : lend;
            continue;
        }

        if (emitted_macro_guards && is_top_define &&
            macro_contains(&auto_guarded, def_id, def_id_len)) {
            sb_append(&sb, "#undef ");
            sb_append_n(&sb, def_id, def_id_len);
            sb_append_char(&sb, '\n');
        }

        sb_append_n(&sb, p, llen);
        if (eol) sb_append_char(&sb, '\n');
        p = eol ? (eol + 1) : lend;
    }

    return sb_detach(&sb);
}

/* ========================================================================= */
/* Pass B: Strip Extraneous Semicolons After Function Bodies (`};`)          */
/* ========================================================================= */

static char *pass_strip_extraneous_semicolons(const char *src) {
    size_t n = strlen(src);
    strbuf_t sb;
    sb_init(&sb, n + 16u);

    /* Track for each '{' whether it opened a struct / uniform / buffer / interface block */
    bool block_needs_semi[128];
    int depth = 0;
    /* Track last significant keyword at brace depth == 0 before '{' */
    char stmt_first[64] = {0};
    bool at_stmt_start = true;
    bool in_layout = false;
    int paren_depth = 0;

    for (size_t i = 0u; i < n;) {
        size_t cspan = comment_span(src + i);
        if (cspan > 0u) {
            sb_append_n(&sb, src + i, cspan);
            i += cspan;
            continue;
        }
        /* Preprocessor line */
        if (src[i] == '#' && (i == 0u || src[i - 1] == '\n')) {
            size_t j = i;
            while (j < n && src[j] != '\n') ++j;
            sb_append_n(&sb, src + i, j - i);
            i = j;
            continue;
        }

        char ch = src[i];
        if (ch == '(') {
            paren_depth++;
        } else if (ch == ')') {
            if (paren_depth > 0) paren_depth--;
            in_layout = false;
        } else if (ch == ';') {
            if (depth == 0) {
                stmt_first[0] = '\0';
                at_stmt_start = true;
            }
        } else if (ch == '{') {
            bool keep_semi = false;
            if (depth == 0) {
                if (strcmp(stmt_first, "struct") == 0 ||
                    strcmp(stmt_first, "uniform") == 0 ||
                    strcmp(stmt_first, "buffer") == 0 ||
                    strcmp(stmt_first, "in") == 0 ||
                    strcmp(stmt_first, "out") == 0 ||
                    strcmp(stmt_first, "layout") == 0) {
                    keep_semi = true;
                }
            } else {
                keep_semi = true; /* structs inside blocks etc. handled conservatively */
            }
            if (depth < 128) {
                block_needs_semi[depth] = keep_semi;
            }
            depth++;
            stmt_first[0] = '\0';
            at_stmt_start = false;
        } else if (ch == '}') {
            if (depth > 0) depth--;
            sb_append_char(&sb, '}');
            ++i;
            bool needs_semi = (depth < 128) ? block_needs_semi[depth] : true;
            if (depth == 0) {
                stmt_first[0] = '\0';
                at_stmt_start = true;
                if (!needs_semi) {
                    size_t k = i;
                    while (k < n && (src[k] == ' ' || src[k] == '\t')) ++k;
                    if (k < n && src[k] == ';') {
                        /* Drop trailing semicolon immediately after function definition '}' */
                        i = k + 1u;
                        continue;
                    }
                }
            }
            continue;
        } else if (depth == 0 && paren_depth == 0 && !in_layout && is_ident_start(ch)) {
            size_t j = i + 1u;
            while (j < n && is_ident_char(src[j])) ++j;
            size_t len = j - i;
            if (at_stmt_start) {
                if (ident_equals(src + i, len, "layout")) {
                    in_layout = true;
                    snprintf(stmt_first, sizeof(stmt_first), "layout");
                } else {
                    size_t cp = (len < sizeof(stmt_first) - 1u) ? len : (sizeof(stmt_first) - 1u);
                    memcpy(stmt_first, src + i, cp);
                    stmt_first[cp] = '\0';
                    at_stmt_start = false;
                }
            } else if (ident_equals(src + i, len, "struct") ||
                       ident_equals(src + i, len, "uniform") ||
                       ident_equals(src + i, len, "buffer")) {
                size_t cp = (len < sizeof(stmt_first) - 1u) ? len : (sizeof(stmt_first) - 1u);
                memcpy(stmt_first, src + i, cp);
                stmt_first[cp] = '\0';
            }
            sb_append_n(&sb, src + i, len);
            i = j;
            continue;
        }

        sb_append_char(&sb, ch);
        ++i;
    }

    return sb_detach(&sb);
}

/* ========================================================================= */
/* Pass C: OrbitUniforms dmat4x4 UBO & star_sprite_blur Struct Flattening    */
/* ========================================================================= */

static bool find_uniform_block_range(const char *src, const char *block_name,
                                     size_t *out_lbrace, size_t *out_rbrace) {
    size_t blen = strlen(block_name);
    size_t n = strlen(src);
    for (size_t i = 0u; i < n;) {
        size_t cspan = comment_span(src + i);
        if (cspan > 0u) { i += cspan; continue; }
        if (is_ident_start(src[i])) {
            size_t j = i + 1u;
            while (j < n && is_ident_char(src[j])) ++j;
            if (ident_equals(src + i, j - i, "uniform")) {
                const char *p = skip_ws(src + j);
                if (strncmp(p, block_name, blen) == 0 && !is_ident_char(p[blen])) {
                    const char *q = skip_ws(p + blen);
                    if (*q == '{') {
                        int d = 1;
                        size_t lb = (size_t)(q - src);
                        size_t k = lb + 1u;
                        while (k < n && d > 0) {
                            size_t cs = comment_span(src + k);
                            if (cs > 0u) { k += cs; continue; }
                            if (src[k] == '{') ++d;
                            else if (src[k] == '}') --d;
                            ++k;
                        }
                        if (d == 0) {
                            *out_lbrace = lb;
                            *out_rbrace = k - 1u;
                            return true;
                        }
                    }
                }
            }
            i = j;
            continue;
        }
        ++i;
    }
    return false;
}

/* Decode IEEE-754 double (uvec2 lo, hi) inside std140 UBO padding-matched uvec4[8] */
static const char k_orbit_dmat_helpers[] =
    "\nfloat _se_f64_to_f32(uvec2 bits) {\n"
    "    uint hi = bits.y;\n"
    "    uint sign = hi & 0x80000000u;\n"
    "    int exp64 = int((hi >> 20u) & 0x7FFu);\n"
    "    if (exp64 == 0) return 0.0;\n"
    "    if (exp64 == 0x7FF) return uintBitsToFloat(sign | 0x7F800000u);\n"
    "    int exp32 = exp64 - 1023 + 127;\n"
    "    if (exp32 <= 0) return 0.0;\n"
    "    if (exp32 >= 255) return uintBitsToFloat(sign | 0x7F800000u);\n"
    "    uint mant = ((hi & 0xFFFFFu) << 3u) | (bits.x >> 29u);\n"
    "    return uintBitsToFloat(sign | (uint(exp32) << 23u) | mant);\n"
    "}\n"
    "mat4 _se_dmat4_to_mat4(uvec4 d[8]) {\n"
    "    return mat4(\n"
    "        vec4(_se_f64_to_f32(d[0].xy), _se_f64_to_f32(d[0].zw), _se_f64_to_f32(d[1].xy), _se_f64_to_f32(d[1].zw)),\n"
    "        vec4(_se_f64_to_f32(d[2].xy), _se_f64_to_f32(d[2].zw), _se_f64_to_f32(d[3].xy), _se_f64_to_f32(d[3].zw)),\n"
    "        vec4(_se_f64_to_f32(d[4].xy), _se_f64_to_f32(d[4].zw), _se_f64_to_f32(d[5].xy), _se_f64_to_f32(d[5].zw)),\n"
    "        vec4(_se_f64_to_f32(d[6].xy), _se_f64_to_f32(d[6].zw), _se_f64_to_f32(d[7].xy), _se_f64_to_f32(d[7].zw))\n"
    "    );\n"
    "}\n";

static char *pass_orbit_and_star_blur_structs(const char *src) {
    size_t n = strlen(src);
    char *cur = (char *)malloc(n + 1u);
    if (!cur) return NULL;
    memcpy(cur, src, n + 1u);

    /* 1. Handle OrbitUniforms dmat4x4 ModelViewMatrix & ModelViewProjMatrix */
    size_t lb = 0u, rb = 0u;
    if (find_uniform_block_range(cur, "OrbitUniforms", &lb, &rb)) {
        size_t blen = rb - lb;
        char *body = (char *)malloc(blen + 1u);
        if (body) {
            memcpy(body, cur + lb, blen);
            body[blen] = '\0';
            if (strstr(body, "dmat4x4")) {
                /* Replace dmat4x4 inside OrbitUniforms while preserving std140 480-byte offsets (224 & 352) */
                strbuf_t ub;
                sb_init(&ub, strlen(cur) + 2048u);
                /* Emit up to ';' after OrbitUniforms '}' */
                size_t semi = rb + 1u;
                while (cur[semi] && cur[semi] != ';') ++semi;
                if (cur[semi] == ';') ++semi;

                sb_append_n(&ub, cur, lb);
                sb_append(&ub, "{\n"
                               "    vec4 Color;\n"
                               "    vec4 PlanetPosMV;\n"
                               "    vec4 PlanetPosMVP;\n"
                               "    vec4 VertId;\n"
                               "    vec4 Viewport;\n"
                               "    mat4 ProjectionMatrix;\n"
                               "    mat3 NormalMatrix;\n"
                               "    vec4 Clip;\n"
                               "    vec4 _se_orbit_pad;\n"
                               "    uvec4 ModelViewMatrix[8];\n"
                               "    uvec4 ModelViewProjMatrix[8];\n"
                               "};");
                sb_append(&ub, k_orbit_dmat_helpers);

                /* Copy rest, wrapping ModelViewMatrix / ModelViewProjMatrix references */
                const char *rest = cur + semi;
                for (size_t i = 0u; rest[i];) {
                    size_t cs = comment_span(rest + i);
                    if (cs > 0u) { sb_append_n(&ub, rest + i, cs); i += cs; continue; }
                    if (is_ident_start(rest[i])) {
                        size_t j = i + 1u;
                        while (is_ident_char(rest[j])) ++j;
                        size_t il = j - i;
                        if (ident_equals(rest + i, il, "ModelViewMatrix")) {
                            sb_append(&ub, "_se_dmat4_to_mat4(ModelViewMatrix)");
                        } else if (ident_equals(rest + i, il, "ModelViewProjMatrix")) {
                            sb_append(&ub, "_se_dmat4_to_mat4(ModelViewProjMatrix)");
                        } else {
                            sb_append_n(&ub, rest + i, il);
                        }
                        i = j;
                        continue;
                    }
                    sb_append_char(&ub, rest[i]);
                    ++i;
                }
                free(body);
                free(cur);
                cur = sb_detach(&ub);
                if (!cur) return NULL;
            } else {
                free(body);
            }
        }
    }

    /* 2. Handle star_sprite_blur.glsl struct QuadData & struct GSOutData array flattening */
    if (contains_code_token(cur, "QuadData") && contains_code_token(cur, "GSOutData")) {
        /* Place QuadDataES before #ifdef _VERTEX_ so both VS & GS share it;
         * rewrite VS `gData.` to local `_se_lData.` while only rewriting GS `gData[0].` fields! */
        static const struct {
            const char *pat;
            const char *rep;
        } k_blur_subs[] = {
            { "#ifdef _VERTEX_",
              "struct QuadDataES {\n"
              "    vec4 pos0; vec4 pos1;\n"
              "    vec3 vecU0; vec3 vecU1;\n"
              "    vec3 vecR0; vec3 vecR1;\n"
              "    vec3 size0; vec3 size1;\n"
              "    vec3 color0; vec3 color1;\n"
              "#if (TRAIL_METHOD == TRAIL_SSLD)\n"
              "    vec2 scrPos0; vec2 scrPos1;\n"
              "#endif\n"
              "};\n"
              "#ifdef _VERTEX_" },
            { "gData.size[i] = vec3(spriteSizeS * 2.0, spriteSizeS * visSize * 2.0, 1.0 / max(spriteSize, 1.0e-6));\n\t}",
              "_se_lData.size[i] = vec3(spriteSizeS * 2.0, spriteSizeS * visSize * 2.0, 1.0 / max(spriteSize, 1.0e-6));\n\t}\n    _se_flush_gdata();" },
            { "gData.", "_se_lData." },
            { "out QuadData gData;",
              "QuadData _se_lData;\n"
              "out QuadDataES gData;\n"
              "void _se_flush_gdata() {\n"
              "    gData.pos0 = _se_lData.pos[0]; gData.pos1 = _se_lData.pos[1];\n"
              "    gData.vecU0 = _se_lData.vecU[0]; gData.vecU1 = _se_lData.vecU[1];\n"
              "    gData.vecR0 = _se_lData.vecR[0]; gData.vecR1 = _se_lData.vecR[1];\n"
              "    gData.size0 = _se_lData.size[0]; gData.size1 = _se_lData.size[1];\n"
              "    gData.color0 = _se_lData.color[0]; gData.color1 = _se_lData.color[1];\n"
              "#if (TRAIL_METHOD == TRAIL_SSLD)\n"
              "    gData.scrPos0 = _se_lData.scrPos[0]; gData.scrPos1 = _se_lData.scrPos[1];\n"
              "#endif\n"
              "}" },
            { "in QuadData gData[];", "in QuadDataES gData[];" },
            { "flat vec4 TexCoord[4];", "flat vec4 TexCoord0; flat vec4 TexCoord1; flat vec4 TexCoord2; flat vec4 TexCoord3;" },
            { "flat vec4 ProjPos[4];", "flat vec4 ProjPos0; flat vec4 ProjPos1; flat vec4 ProjPos2; flat vec4 ProjPos3;" },
            { "gData[0].pos [0]", "gData[0].pos0" }, { "gData[0].pos [1]", "gData[0].pos1" },
            { "gData[0].pos[0]", "gData[0].pos0" },  { "gData[0].pos[1]", "gData[0].pos1" },
            { "gData[0].vecU[0]", "gData[0].vecU0" }, { "gData[0].vecU[1]", "gData[0].vecU1" },
            { "gData[0].vecR[0]", "gData[0].vecR0" }, { "gData[0].vecR[1]", "gData[0].vecR1" },
            { "gData[0].size[0]", "gData[0].size0" }, { "gData[0].size[1]", "gData[0].size1" },
            { "gData[0].color[0]", "gData[0].color0" }, { "gData[0].color[1]", "gData[0].color1" },
            { "gData[0].scrPos[0]", "gData[0].scrPos0" }, { "gData[0].scrPos[1]", "gData[0].scrPos1" },
            { "for (int i=0; i<4; i++) fData.ProjPos[i] = projPos[i];",
              "fData.ProjPos0 = projPos[0]; fData.ProjPos1 = projPos[1]; fData.ProjPos2 = projPos[2]; fData.ProjPos3 = projPos[3];" },
            { "for (int i=0; i<4; i++)\n        vertPos[i] = ViewPort.xy + ViewPort.zw * (1.0 + fData.ProjPos[i].xy / fData.ProjPos[i].w);",
              "vertPos[0] = ViewPort.xy + ViewPort.zw * (1.0 + fData.ProjPos0.xy / fData.ProjPos0.w);\n"
              "    vertPos[1] = ViewPort.xy + ViewPort.zw * (1.0 + fData.ProjPos1.xy / fData.ProjPos1.w);\n"
              "    vertPos[2] = ViewPort.xy + ViewPort.zw * (1.0 + fData.ProjPos2.xy / fData.ProjPos2.w);\n"
              "    vertPos[3] = ViewPort.xy + ViewPort.zw * (1.0 + fData.ProjPos3.xy / fData.ProjPos3.w);" },
            { "fData.ProjPos[0]", "fData.ProjPos0" },
            { "fData.ProjPos[1]", "fData.ProjPos1" },
            { "fData.ProjPos[2]", "fData.ProjPos2" },
            { "fData.ProjPos[3]", "fData.ProjPos3" },
            { "fData.TexCoord[0]", "fData.TexCoord0" },
            { "fData.TexCoord[1]", "fData.TexCoord1" },
            { "fData.TexCoord[2]", "fData.TexCoord2" },
            { "fData.TexCoord[3]", "fData.TexCoord3" },
        };
        for (size_t s = 0u; s < sizeof(k_blur_subs) / sizeof(k_blur_subs[0]); ++s) {
            const char *pat = k_blur_subs[s].pat;
            const char *rep = k_blur_subs[s].rep;
            size_t plen = strlen(pat);
            if (!strstr(cur, pat)) continue;
            strbuf_t tb;
            sb_init(&tb, strlen(cur) + 256u);
            const char *q = cur;
            while (*q) {
                const char *hit = strstr(q, pat);
                if (!hit) {
                    sb_append(&tb, q);
                    break;
                }
                sb_append_n(&tb, q, (size_t)(hit - q));
                sb_append(&tb, rep);
                q = hit + plen;
            }
            free(cur);
            cur = sb_detach(&tb);
            if (!cur) return NULL;
        }
    }

    return cur;
}

/* ========================================================================= */
/* Pass D: Token-level Rewrites (keywords, double, sampler1D, pow/lod, etc.) */
/* ========================================================================= */

/* Scan a function call argument list starting right at '(', splitting top-level commas.
 * Returns index of matching ')' or 0 on failure. */
static size_t find_matching_paren(const char *s, size_t open_idx) {
    int d = 0;
    for (size_t i = open_idx; s[i]; ++i) {
        size_t cs = comment_span(s + i);
        if (cs > 0u) { i += cs - 1u; continue; }
        if (s[i] == '(') ++d;
        else if (s[i] == ')') {
            --d;
            if (d == 0) return i;
        }
    }
    return 0u;
}

static int split_top_args(const char *s, size_t lparen, size_t rparen,
                          size_t *arg_starts, size_t *arg_ends, int max_args) {
    int count = 0;
    int d = 0;
    size_t cur_start = lparen + 1u;
    for (size_t i = lparen + 1u; i < rparen; ++i) {
        size_t cs = comment_span(s + i);
        if (cs > 0u) { i += cs - 1u; continue; }
        if (s[i] == '(' || s[i] == '[' || s[i] == '{') ++d;
        else if (s[i] == ')' || s[i] == ']' || s[i] == '}') { if (d > 0) --d; }
        else if (s[i] == ',' && d == 0) {
            if (count < max_args) {
                arg_starts[count] = cur_start;
                arg_ends[count] = i;
            }
            count++;
            cur_start = i + 1u;
        }
    }
    if (cur_start <= rparen) {
        if (count < max_args) {
            arg_starts[count] = cur_start;
            arg_ends[count] = rparen;
        }
        count++;
    }
    return count;
}

static bool is_pure_int_literal(const char *s, size_t a, size_t b) {
    while (a < b && (*s == ' ' || *s == '\t' || *s == '\n')) { ++s; --b; } /* bug avoid: use index */
    return false;
}

static bool span_is_int_literal(const char *buf, size_t a, size_t b) {
    while (a < b && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\n')) ++a;
    while (b > a && (buf[b - 1] == ' ' || buf[b - 1] == '\t' || buf[b - 1] == '\n')) --b;
    if (a >= b) return false;
    if (buf[a] == '+' || buf[a] == '-') ++a;
    if (a >= b) return false;
    for (size_t k = a; k < b; ++k) {
        if (!isdigit((unsigned char)buf[k])) return false;
    }
    return true;
}

static bool span_matches_ident(const char *buf, size_t a, size_t b, const s1d_list_t *s1d) {
    while (a < b && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\n')) ++a;
    while (b > a && (buf[b - 1] == ' ' || buf[b - 1] == '\t' || buf[b - 1] == '\n')) --b;
    if (a >= b) return false;
    return s1d_contains(s1d, buf + a, b - a);
}

static bool span_is_trimmed_ident(const char *buf, size_t a, size_t b, const char *lit) {
    while (a < b && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\n')) ++a;
    while (b > a && (buf[b - 1] == ' ' || buf[b - 1] == '\t' || buf[b - 1] == '\n')) --b;
    return ident_equals(buf + a, b - a, lit);
}

static char *pass_tokens_and_calls(const char *src,
                                   const s1d_list_t *s1d,
                                   bool flip_fragcoord);

static void sb_append_call_arg(strbuf_t *sb, const char *src, size_t a, size_t b,
                               const s1d_list_t *s1d, bool flip_fragcoord,
                               bool wrap_float) {
    if (wrap_float) sb_append(sb, "float(");
    char *arg = (char *)malloc(b - a + 1u);
    if (arg) {
        memcpy(arg, src + a, b - a);
        arg[b - a] = '\0';
        char *tr = pass_tokens_and_calls(arg, s1d, flip_fragcoord);
        sb_append(sb, tr ? tr : arg);
        free(tr);
        free(arg);
    } else {
        sb_append_n(sb, src + a, b - a);
    }
    if (wrap_float) sb_append(sb, ")");
}

static char *pass_tokens_and_calls(const char *src,
                                   const s1d_list_t *s1d,
                                   bool flip_fragcoord) {
    (void)is_pure_int_literal;
    size_t n = strlen(src);
    strbuf_t sb;
    sb_init(&sb, n + 2048u);

    for (size_t i = 0u; i < n;) {
        size_t cs = comment_span(src + i);
        if (cs > 0u) {
            sb_append_n(&sb, src + i, cs);
            i += cs;
            continue;
        }
        /* Keep preprocessor lines untouched here */
        if (src[i] == '#' && (i == 0u || src[i - 1] == '\n')) {
            size_t j = i;
            while (j < n && src[j] != '\n') ++j;
            sb_append_n(&sb, src + i, j - i);
            i = j;
            continue;
        }
        /* Skip _se_fragcoord body untouched so gl_FragCoord inside it isn't recursed */
        if (strncmp(src + i, "vec4 _se_fragcoord()", 20) == 0) {
            size_t j = i + 20u;
            while (j < n && src[j] != '\n') ++j;
            sb_append_n(&sb, src + i, j - i);
            i = j;
            continue;
        }

        if (is_ident_start(src[i])) {
            size_t j = i + 1u;
            while (j < n && is_ident_char(src[j])) ++j;
            size_t il = j - i;

            /* Strip noperspective keyword */
            if (ident_equals(src + i, il, "noperspective")) {
                i = j;
                continue;
            }
            /* Map double/dvec/dmat -> float/vec/mat (OrbitUniforms already handled) */
            if (ident_equals(src + i, il, "double")) { sb_append(&sb, "float"); i = j; continue; }
            if (ident_equals(src + i, il, "dvec2"))  { sb_append(&sb, "vec2");  i = j; continue; }
            if (ident_equals(src + i, il, "dvec3"))  { sb_append(&sb, "vec3");  i = j; continue; }
            if (ident_equals(src + i, il, "dvec4"))  { sb_append(&sb, "vec4");  i = j; continue; }
            if (ident_equals(src + i, il, "dmat4") || ident_equals(src + i, il, "dmat4x4")) {
                sb_append(&sb, "mat4"); i = j; continue;
            }
            if (ident_equals(src + i, il, "dmat3") || ident_equals(src + i, il, "dmat3x3")) {
                sb_append(&sb, "mat3"); i = j; continue;
            }
            if (ident_equals(src + i, il, "dmat2") || ident_equals(src + i, il, "dmat2x2")) {
                sb_append(&sb, "mat2"); i = j; continue;
            }
            /* sampler1D -> sampler2D */
            if (ident_equals(src + i, il, "sampler1D"))  { sb_append(&sb, "sampler2D");  i = j; continue; }
            if (ident_equals(src + i, il, "isampler1D")) { sb_append(&sb, "isampler2D"); i = j; continue; }
            if (ident_equals(src + i, il, "usampler1D")) { sb_append(&sb, "usampler2D"); i = j; continue; }
            /* gl_FragCoord flip in gui.glsl */
            if (flip_fragcoord && ident_equals(src + i, il, "gl_FragCoord")) {
                sb_append(&sb, "_se_fragcoord()");
                i = j;
                continue;
            }

            /* Function call rewrites for texture*, pow */
            const char *after = skip_ws(src + j);
            if (*after == '(') {
                size_t lp = (size_t)(after - src);
                size_t rp = find_matching_paren(src, lp);
                if (rp > lp) {
                    size_t as[8], ae[8];
                    int nargs = split_top_args(src, lp, rp, as, ae, 8);

                    bool is_tex = ident_equals(src + i, il, "texture") ||
                                  ident_equals(src + i, il, "texture2D") ||
                                  ident_equals(src + i, il, "textureCube") ||
                                  ident_equals(src + i, il, "texture3D") ||
                                  ident_equals(src + i, il, "texture1D");
                    bool is_lod = ident_equals(src + i, il, "textureLod") ||
                                  ident_equals(src + i, il, "texture2DLod") ||
                                  ident_equals(src + i, il, "textureCubeLod") ||
                                  ident_equals(src + i, il, "textureCubeLodEXT");
                    bool is_fetch = ident_equals(src + i, il, "texelFetch");
                    bool is_tsize = ident_equals(src + i, il, "textureSize");
                    bool is_pow = ident_equals(src + i, il, "pow");
                    bool is_step = ident_equals(src + i, il, "step");

                    /* Clean subslice translation helper inline via recursive slice */
                    if (is_tex && nargs == 2) {
                        bool is_1d = ident_equals(src + i, il, "texture1D") ||
                                     span_matches_ident(src, as[0], ae[0], s1d);
                        sb_append(&sb, "texture(");
                        sb_append_n(&sb, src + as[0], ae[0] - as[0]);
                        sb_append(&sb, ", ");
                        char *arg1 = (char *)malloc(ae[1] - as[1] + 1u);
                        memcpy(arg1, src + as[1], ae[1] - as[1]);
                        arg1[ae[1] - as[1]] = '\0';
                        char *t1 = pass_tokens_and_calls(arg1, s1d, flip_fragcoord);
                        if (is_1d) {
                            sb_append(&sb, "vec2(");
                            sb_append(&sb, t1 ? t1 : arg1);
                            sb_append(&sb, ", 0.5)");
                        } else {
                            sb_append(&sb, t1 ? t1 : arg1);
                        }
                        free(t1);
                        free(arg1);
                        sb_append_char(&sb, ')');
                        i = rp + 1u;
                        continue;
                    }

                    if (is_lod && nargs == 3) {
                        bool is_1d = span_matches_ident(src, as[0], ae[0], s1d);
                        sb_append(&sb, "textureLod(");
                        sb_append_n(&sb, src + as[0], ae[0] - as[0]);
                        sb_append(&sb, ", ");
                        char *arg1 = (char *)malloc(ae[1] - as[1] + 1u);
                        memcpy(arg1, src + as[1], ae[1] - as[1]);
                        arg1[ae[1] - as[1]] = '\0';
                        char *t1 = pass_tokens_and_calls(arg1, s1d, flip_fragcoord);
                        if (is_1d) {
                            sb_append(&sb, "vec2(");
                            sb_append(&sb, t1 ? t1 : arg1);
                            sb_append(&sb, ", 0.5)");
                        } else {
                            sb_append(&sb, t1 ? t1 : arg1);
                        }
                        free(t1);
                        free(arg1);
                        sb_append(&sb, ", ");
                        /* Always coerce LOD argument to float(...) so integer macros/literals compile in ES 3.20 */
                        char *arg2 = (char *)malloc(ae[2] - as[2] + 1u);
                        memcpy(arg2, src + as[2], ae[2] - as[2]);
                        arg2[ae[2] - as[2]] = '\0';
                        char *t2 = pass_tokens_and_calls(arg2, s1d, flip_fragcoord);
                        const char *lod_s = t2 ? t2 : arg2;
                        const char *lod_trim = skip_ws(lod_s);
                        if (strncmp(lod_trim, "float(", 6) == 0) {
                            sb_append(&sb, lod_s);
                        } else {
                            sb_append(&sb, "float(");
                            sb_append(&sb, lod_trim);
                            sb_append_char(&sb, ')');
                        }
                        free(t2);
                        free(arg2);
                        sb_append_char(&sb, ')');
                        i = rp + 1u;
                        continue;
                    }

                    if (is_fetch && nargs == 3 && span_matches_ident(src, as[0], ae[0], s1d)) {
                        sb_append(&sb, "texelFetch(");
                        sb_append_n(&sb, src + as[0], ae[0] - as[0]);
                        sb_append(&sb, ", ivec2(");
                        char *arg1 = (char *)malloc(ae[1] - as[1] + 1u);
                        memcpy(arg1, src + as[1], ae[1] - as[1]);
                        arg1[ae[1] - as[1]] = '\0';
                        char *t1 = pass_tokens_and_calls(arg1, s1d, flip_fragcoord);
                        sb_append(&sb, t1 ? t1 : arg1);
                        free(t1);
                        free(arg1);
                        sb_append(&sb, ", 0), ");
                        sb_append_n(&sb, src + as[2], ae[2] - as[2]);
                        sb_append_char(&sb, ')');
                        i = rp + 1u;
                        continue;
                    }

                    if (is_tsize && nargs == 2 && span_matches_ident(src, as[0], ae[0], s1d)) {
                        sb_append(&sb, "(textureSize(");
                        sb_append_n(&sb, src + as[0], ae[0] - as[0]);
                        sb_append(&sb, ", ");
                        sb_append_n(&sb, src + as[1], ae[1] - as[1]);
                        sb_append(&sb, ").x)");
                        i = rp + 1u;
                        continue;
                    }

                    if (is_pow && nargs == 2 && span_is_int_literal(src, as[1], ae[1])) {
                        sb_append(&sb, "pow(");
                        sb_append_call_arg(&sb, src, as[0], ae[0], s1d, flip_fragcoord, false);
                        sb_append(&sb, ", ");
                        sb_append_call_arg(&sb, src, as[1], ae[1], s1d, flip_fragcoord, true);
                        sb_append(&sb, ")");
                        i = rp + 1u;
                        continue;
                    }
                    if (is_step && nargs == 2) {
                        bool lit0 = span_is_int_literal(src, as[0], ae[0]);
                        bool lit1 = span_is_int_literal(src, as[1], ae[1]);
                        if (lit0 || lit1) {
                            sb_append(&sb, "step(");
                            sb_append_call_arg(&sb, src, as[0], ae[0], s1d, flip_fragcoord, lit0);
                            sb_append(&sb, ", ");
                            sb_append_call_arg(&sb, src, as[1], ae[1], s1d, flip_fragcoord, lit1);
                            sb_append(&sb, ")");
                            i = rp + 1u;
                            continue;
                        }
                    }

                    /* Legacy aliases without arg transforms */
                    if (ident_equals(src + i, il, "texture2D") ||
                        ident_equals(src + i, il, "textureCube") ||
                        ident_equals(src + i, il, "texture3D")) {
                        sb_append(&sb, "texture");
                        i = j;
                        continue;
                    }
                    if (ident_equals(src + i, il, "texture2DLod") ||
                        ident_equals(src + i, il, "textureCubeLod") ||
                        ident_equals(src + i, il, "textureCubeLodEXT")) {
                        sb_append(&sb, "textureLod");
                        i = j;
                        continue;
                    }
                    (void)span_is_trimmed_ident;
                }
            }

            sb_append_n(&sb, src + i, il);
            i = j;
            continue;
        }

        sb_append_char(&sb, src[i]);
        ++i;
    }

    return sb_detach(&sb);
}

/* ========================================================================= */
/* Pass E: Known Exact Int-vs-Float Expression Rewrites                      */
/* ========================================================================= */

static char *pass_known_int_float_exprs(const char *src) {
    static const struct {
        const char *pat;
        const char *rep;
    } k_subs[] = {
        /* 3D_anaglyph.glsl */
        { "max(left.rgb, 0)", "max(left.rgb, 0.0)" },
        { "max(right.rgb, 0)", "max(right.rgb, 0.0)" },
        /* accretion_jet.glsl & comet_tail.glsl */
        { "clamp(t * 2, 0.0, 1.0)", "clamp(t * 2.0, 0.0, 1.0)" },
        /* aurora.glsl */
        { "if (tc.x < 0)", "if (tc.x < 0.0)" },
        /* gr_geodesic.glh (#ifdef ALCUBIERRE_METRIC) */
        { "clamp(x, -4, 4)", "clamp(x, -4.0, 4.0)" },
        /* proj_fisheye.glsl (#ifdef GUI) */
        { "if (ray.x > 0)", "if (ray.x > 0.0)" },
        /* prim_tex_obj.glsl (#ifdef SHOWMIP+NEAREST / MSAA) */
        { "ivec2(fTexCoord.xy * ts / 2)", "ivec2(fTexCoord.xy * vec2(ts) / 2.0)" },
        { "ivec2(fTexCoord.xy * ts)", "ivec2(fTexCoord.xy * vec2(ts))" },
        /* compress.glsl (#ifdef YCOCG / SelectYCoCgDiagonal / EmitEndPointsYCoCgDXT5) */
        { "if (cov < 0)", "if (cov < 0.0)" },
        { "(maxcol - offset) * scale + offset", "(maxcol - offset) * float(scale) + offset" },
        { "(mincol - offset) * scale + offset", "(mincol - offset) * float(scale) + offset" },
        { "(maxcol - offset) / scale + offset", "(maxcol - offset) / float(scale) + offset" },
        { "(mincol - offset) / scale + offset", "(mincol - offset) / float(scale) + offset" },
        /* bicubic_common.glh */
        { "-s*v3 +     2*s*v2 - sv", "-s*v3 + 2.0*s*v2 - sv" },
        { "(2-s)*v3 +   (s-3)*v2", "(2.0-s)*v3 + (s-3.0)*v2" },
        { "(s-2)*v3 + (3-2*s)*v2", "(s-2.0)*v3 + (3.0-2.0*s)*v2" },
        /* orbit.glsl & orbit_cubemap.glsl */
        { "if (index != 0)", "if (index != 0.0)" },
        { "layout(location = 0) in  vec2  inNorm;", "layout(location = 1) in  vec2  inNorm;" },
        /* planet.glsl & water.glsl */
        { "if (dot(tangent, pos) != 0)", "if (dot(tangent, pos) != 0.0)" },
        { "if (VSFetchParams.w < 0.0)", "if (VSFetchParams.w < 0)" },
        /* atmo_common.glh (sky.glsl & planet.glsl & atmo_transm.glsl) */
        { "if (fog_path_len > 0)", "if (fog_path_len > 0.0)" },
        { "1.0 / RES_NU;", "1.0 / float(RES_NU);" },
        /* compress.glsl */
        { "2.0 * GREEN_RANGE", "2.0 * float(GREEN_RANGE)" },
        { ") * GREEN_RANGE", ") * float(GREEN_RANGE)" },
        { "2.0 * ALPHA_RANGE", "2.0 * float(ALPHA_RANGE)" },
        { ") * ALPHA_RANGE", ") * float(ALPHA_RANGE)" },
        { "float cov = 0;", "float cov = 0.0;" },
        { "if (cov.x < 0)", "if (cov.x < 0.0)" },
        { "if (cov.y < 0)", "if (cov.y < 0.0)" },
        /* tg_common.glh & tg_*.glsl */
        { "res.matIDs.x = biome;", "res.matIDs.x = float(biome);" },
        { "float win = 0;", "float win = 0.0;" },
        { "800 * scale", "800.0 * scale" },
        { "float height = 0, slope = 0;", "float height = 0.0, slope = 0.0;" },
        { "if (plantsBiomeOffset > 0.0)", "if (plantsBiomeOffset > 0)" },
        { "DunesNoise(point, 3)", "DunesNoise(point, 3.0)" },
        { "(1 - smoothstep(", "(1.0 - smoothstep(" },
        { "noiseOctaves - oct", "noiseOctaves - float(oct)" },
        { "(1 + dot(", "(1.0 + dot(" },
        { "1 + dot(n.yz, n.yz)", "1.0 + dot(n.yz, n.yz)" },
        /* rings_common.glh & rings_raymarch.glsl */
        { "abs((i + 0.5)", "abs((float(i) + 0.5)" },
        { "const float frq=", "float frq=" },
        /* gr_geodesic.glh / gr_intersection.glh / einstein.glsl */
        { "float time = 0;", "float time = 0.0;" },
        { "step(0, pos.y)", "step(0.0, pos.y)" },
        { "max(r - 1.0, 0.0) / 120)", "max(r - 1.0, 0.0) / 120.0)" },
        /* tg_common.glh */
        { "max(a0 - ma, 0)", "max(a0 - ma, 0.0)" },
        { "max(a1 - ma, 0)", "max(a1 - ma, 0.0)" },
        /* rings_common.glh & gr_intersection.glh runtime consts */
        { "const float k = 1.5 / 806.202", "float k = 1.5 / 806.202" },
        { "const float LineSmoothness ", "float LineSmoothness " },
        { "const float NearSmoothFactor ", "float NearSmoothFactor " },
        { "const float FarSmoothFactor ", "float FarSmoothFactor " },
        /* gr_geodesic.glh / gr_intersection.glh (einstein.glsl) */
        { "#define SPEED_OF_LIGHT 16000 * starRadiusScale", "#define SPEED_OF_LIGHT (16000.0 * starRadiusScale)" },
        /* star_sprite_blur.glsl */
        { "!(c > 0) && !(c < 0)", "!(c > 0.0) && !(c < 0.0)" },
        { "!(s > 0) || !(s < 1)", "!(s > 0.0) || !(s < 1.0)" },
        { "!(t > 0) || !(t < 1)", "!(t > 0.0) || !(t < 1.0)" },
    };

    char *cur = (char *)malloc(strlen(src) + 1u);
    if (!cur) return NULL;
    strcpy(cur, src);

    for (size_t k = 0u; k < sizeof(k_subs) / sizeof(k_subs[0]); ++k) {
        const char *pat = k_subs[k].pat;
        const char *rep = k_subs[k].rep;
        if (!strstr(cur, pat)) continue;
        size_t plen = strlen(pat);
        strbuf_t sb;
        sb_init(&sb, strlen(cur) + 64u);
        const char *q = cur;
        while (*q) {
            const char *hit = strstr(q, pat);
            if (!hit) {
                sb_append(&sb, q);
                break;
            }
            sb_append_n(&sb, q, (size_t)(hit - q));
            sb_append(&sb, rep);
            q = hit + plen;
        }
        free(cur);
        cur = sb_detach(&sb);
        if (!cur) return NULL;
    }

    static const char *const k_float_loop_vars[] = {
        "noiseOctaves",
        "cloudsOctaves",
        "cratOctaves",
        "volcanoOctaves",
        "cracksOctaves",
        "cycloneOctaves",
        "cycloneOctaves2",
        "octaves",
    };
    for (size_t vi = 0u; vi < sizeof(k_float_loop_vars) / sizeof(k_float_loop_vars[0]); ++vi) {
        const char *tok = k_float_loop_vars[vi];
        if (!strstr(cur, tok)) continue;
        size_t tlen = strlen(tok);
        bool fix_assign = (vi == 0u); /* noiseOctaves */
        strbuf_t sb;
        sb_init(&sb, strlen(cur) + 256u);
        const char *q = cur;
        while (*q) {
            const char *hit = strstr(q, tok);
            if (!hit) {
                sb_append(&sb, q);
                break;
            }
            bool prev_id = (hit > cur) && is_ident_char(hit[-1]);
            bool next_id = is_ident_char(hit[tlen]);
            if (prev_id || next_id) {
                sb_append_n(&sb, q, (size_t)((hit + tlen) - q));
                q = hit + tlen;
                continue;
            }

            /* Case 1: loop bound `< var` (e.g. `for (int i=0; i<cloudsOctaves; ++i)`) */
            const char *b = hit;
            while (b > cur && (b[-1] == ' ' || b[-1] == '\t')) --b;
            const char *a = hit + tlen;
            while (*a == ' ' || *a == '\t') ++a;
            if (b > cur && b[-1] == '<' && (*a == ';' || *a == ')')) {
                sb_append_n(&sb, q, (size_t)(hit - q));
                sb_append(&sb, "int(");
                sb_append_n(&sb, tok, tlen);
                sb_append(&sb, ")");
                q = hit + tlen;
                continue;
            }

            /* Case 2: assignment `noiseOctaves = <int>` or float *Octaves comparison (`> 1`, `> 0`, etc.) */
            bool is_cmp = (strstr(tok, "Octaves") != NULL) &&
                          (((a[0] == '>' || a[0] == '<') && a[1] != a[0]) ||
                           ((a[0] == '=' || a[0] == '!') && a[1] == '='));
            bool is_assign = (fix_assign && a[0] == '=' && a[1] != '=');
            if (is_assign || is_cmp) {
                const char *v = a + ((a[1] == '=') ? 2 : 1);
                while (*v == ' ' || *v == '\t') ++v;
                const char *d = v;
                while (*d >= '0' && *d <= '9') ++d;
                if (d > v && *d != '.' && *d != 'e' && *d != 'E' && *d != 'u' && *d != 'U' && !is_ident_char(*d)) {
                    sb_append_n(&sb, q, (size_t)(d - q));
                    sb_append(&sb, ".0");
                    q = d;
                    continue;
                }
            }

            sb_append_n(&sb, q, (size_t)((hit + tlen) - q));
            q = hit + tlen;
        }
        free(cur);
        cur = sb_detach(&sb);
        if (!cur) return NULL;
    }

    return cur;
}

/* ========================================================================= */
/* Public Translation Entry Point                                            */
/* ========================================================================= */

char *se_shader_translate_to_es3(const char *src, uint32_t shader_type) {
    if (!src) return NULL;

    char *norm = normalize_newlines(src);
    if (!norm) return NULL;

    if (is_already_es_shader(norm)) {
        return norm;
    }

    bool is_geom = (shader_type == SE_GL_GEOMETRY_SHADER) ||
                   contains_code_token(norm, "EmitVertex") ||
                   contains_code_token(norm, "EndPrimitive");
    bool has_blend_idx = uses_dual_source_blend(norm);
    bool has_viewport = contains_code_token(norm, "Viewport");

    s1d_list_t s1d;
    collect_sampler1d_names(norm, &s1d);

    macro_list_t eval_macros, defined_top;
    collect_if_eval_macros(norm, &eval_macros, &defined_top);

    bool used_fragcoord_flip = false;
    char *s1 = pass_lines_and_macros(norm, &eval_macros, has_viewport, &used_fragcoord_flip);
    free(norm);
    if (!s1) return NULL;

    char *s2 = pass_strip_extraneous_semicolons(s1);
    free(s1);
    if (!s2) return NULL;

    char *s3 = pass_orbit_and_star_blur_structs(s2);
    free(s2);
    if (!s3) return NULL;

    char *s4 = pass_tokens_and_calls(s3, &s1d, used_fragcoord_flip);
    free(s3);
    if (!s4) return NULL;

    char *s5 = pass_known_int_float_exprs(s4);
    free(s4);
    if (!s5) return NULL;

    strbuf_t final_sb;
    sb_init(&final_sb, strlen(s5) + 512u);

    sb_append(&final_sb,
              "#version 320 es\n"
              "#ifdef GL_EXT_shader_io_blocks\n"
              "#extension GL_EXT_shader_io_blocks : enable\n"
              "#endif\n");
    if (is_geom) {
        sb_append(&final_sb,
                  "#ifdef GL_EXT_geometry_shader\n"
                  "#extension GL_EXT_geometry_shader : enable\n"
                  "#endif\n");
    }
    if (has_blend_idx) {
        sb_append(&final_sb,
                  "#ifdef GL_EXT_blend_func_extended\n"
                  "#extension GL_EXT_blend_func_extended : enable\n"
                  "#endif\n");
    }
    sb_append(&final_sb,
              "precision highp float;\n"
              "precision highp int;\n"
              "precision highp sampler2D;\n"
              "precision highp sampler3D;\n"
              "precision highp samplerCube;\n"
              "precision highp sampler2DArray;\n"
              "precision highp sampler2DMS;\n"
              "precision highp sampler2DShadow;\n"
              "precision highp samplerCubeArray;\n"
              "precision highp isampler2D;\n"
              "precision highp isampler3D;\n"
              "precision highp isampler2DArray;\n"
              "precision highp usampler2D;\n"
              "precision highp usampler3D;\n"
              "precision highp usampler2DArray;\n");

    sb_append(&final_sb, s5);
    free(s5);
    return sb_detach(&final_sb);
}

void se_shader_free_translated(char *translated) {
    free(translated);
}
