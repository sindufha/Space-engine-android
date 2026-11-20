#ifndef SE_SHADER_ES3_H
#define SE_SHADER_ES3_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL shader stage constants used by se_shader_translate_to_es3 */
#define SE_GL_FRAGMENT_SHADER        0x8B30u
#define SE_GL_VERTEX_SHADER          0x8B31u
#define SE_GL_GEOMETRY_SHADER        0x8DD9u
#define SE_GL_TESS_EVALUATION_SHADER 0x8E87u
#define SE_GL_TESS_CONTROL_SHADER    0x8E88u
#define SE_GL_COMPUTE_SHADER         0x91B9u

/* Translate desktop GLSL (e.g. #version 460 core) into GLSL ES 3.20 (#version 320 es).
 * Returns a heap-allocated NUL-terminated string (release via se_shader_free_translated),
 * or NULL on invalid input / allocation failure. */
char *se_shader_translate_to_es3(const char *src, uint32_t shader_type);

/* Release buffer returned by se_shader_translate_to_es3. */
void se_shader_free_translated(char *translated);

#ifdef __cplusplus
}
#endif

#endif /* SE_SHADER_ES3_H */
