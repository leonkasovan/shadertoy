/*
 * Shared shader plumbing for the viewer (main.c) and the MCP server (mcp.c).
 *
 * Everything that defines "what a Shadertoy shader is" lives here exactly
 * once: the GLSL version, the vertex shader, the injected uniform header and
 * mainImage() footer, program compilation/linking, uniform location lookup,
 * and uploading per-frame uniform values.
 *
 * Keeping this in one translation unit is deliberate: the interactive viewer
 * and the headless MCP renderer must produce identical programs and identical
 * uniform values, otherwise a shader renders differently depending on which
 * code path compiled it.
 */

#ifndef SHADER_COMMON_H
#define SHADER_COMMON_H

#include "glad.h"
#include <stddef.h>

/* GLSL version line: 330 core on desktop, 300 es on GLES. */
#ifdef USE_GLES
#define GLSL_VERSION "#version 300 es"
#else
#define GLSL_VERSION "#version 330 core"
#endif

/* ---------------------------------------------------------------- */
/* Source assembly                                                   */
/* ---------------------------------------------------------------- */

/* Fullscreen-quad vertex shader (writes gl_Position from a 2D corner). */
extern const char *shader_vertex_source(void);

/* Injected fragment prologue: #version, precision, and all iXXX uniforms. */
extern const char *shader_fragment_header(void);

/* Injected epilogue: calls mainImage(fragColor, vFragCoord). */
extern const char *shader_fragment_footer(void);

/* Wrap a user Shadertoy body into a complete fragment shader.
 * Returns a malloc'd NUL-terminated string, or NULL on allocation failure.
 * Caller frees. */
char *shader_wrap_fragment(const char *body);

/* Compile one shader stage. On failure returns 0 and, if logbuf is non-NULL,
 * writes the info log into it. On success the log is left empty. */
GLuint shader_compile_stage(GLenum type, const char *src,
                            char *logbuf, size_t loglen);

/* Compile + link a Shadertoy fragment body into a full program.
 * `body` is the raw .glsl file contents. The vertex shader is supplied by
 * this module. On failure returns 0 and writes a diagnostic to logbuf.
 * Requires a current GL context. */
GLuint shader_build_program(const char *body, char *logbuf, size_t loglen);

/* ---------------------------------------------------------------- */
/* Uniforms                                                          */
/* ---------------------------------------------------------------- */

/* Cached uniform locations for one linked program. A location of -1 means
 * the uniform was optimized out (or is unused); uploads skip those. */
typedef struct {
    GLint iResolution;
    GLint iTime;
    GLint iTimeDelta;
    GLint iFrame;
    GLint iFrameRate;
    GLint iMouse;
    GLint iDate;
    GLint iSampleRate;
    GLint uResolution;          /* vertex shader's resolution uniform */
    GLint iChannelResolution;
    GLint iChannelTime;
    GLint iChannel[4];
} shader_uniforms;

/* Fill `u` with the locations of every uniform in a linked program. */
void shader_uniforms_lookup(GLuint prog, shader_uniforms *u);

/* Values uploaded every frame. Everything both renderers must agree on. */
typedef struct {
    float width, height;
    float time;
    float time_delta;
    int   frame;
    float frame_rate;
    float mouse_x, mouse_y, mouse_left, mouse_right;
    int   date_valid;
    float date_year, date_month, date_day, date_seconds; /* month 0-based */
    float channel_resolution[4][3];
    float channel_time[4];
} shader_frame;

/* Populate `f` with neutral defaults so callers only set what they vary.
 * Sets iTimeDelta to 1/60, iFrameRate to 60, iSampleRate to 44100,
 * zeroes mouse and channel resolution/time, and marks iDate absent. */
void shader_frame_defaults(shader_frame *f);

/* Upload `f` to `prog` using cached locations `u`. Also binds the
 * sampler uniforms iChannel0..3 to texture units 0..3. */
void shader_upload_uniforms(const shader_uniforms *u, const shader_frame *f);

/* Scale factor for iResolution.z (always 1.0 here; kept for clarity). */
#define SHADER_PIXEL_ASPECT 1.0f

#endif /* SHADER_COMMON_H */
