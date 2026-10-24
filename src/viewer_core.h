/*
 * Backend-independent viewer core.
 *
 * Everything here used to be duplicated verbatim in main-sdl2.c and
 * main-glfw.c: the shader list and current-shader state, hot-reload and
 * shader switching, the MCP viewer hooks, and the shared channel/keystate
 * plumbing. Both backends compile this same translation unit, so the two
 * binaries cannot drift apart in how they load, switch or report shaders.
 *
 * What stays backend-specific is only the window/input layer in
 * main-sdl2.c / main-glfw.c: creating the window and GL context, pumping
 * events, mapping native key codes to Shadertoy/JS key codes, and the parts
 * of the status line that ask the window for its size.
 *
 * Nothing here is #ifdef'd on the backend. If a new backend needs different
 * behaviour, add a function to this interface instead of a conditional.
 */

#ifndef VIEWER_CORE_H
#define VIEWER_CORE_H

#include "glad.h"
#include "mcp.h"
#include "shader_common.h"
#include "shader_list.h"
#include <stddef.h>

/* Backend services are defined further down but referenced by the hooks
 * above them in this header; the struct is declared before first use. */

/* ------------------------------------------------------------------ */
/* Shader list (the no-arg browsing list)                             */
/* ------------------------------------------------------------------ */

/* The one shader list. Backends iterate it through the accessors below
 * rather than keeping a copy; `shader_paths`/`shader_count` exist only as
 * convenience macros inside viewer_core.c. */
shader_list *vc_shaders(void);
int          vc_shader_count(void);
const char  *vc_shader_path(int idx);

/* Index of the shader currently displayed. */
int  vc_shader_index(void);
void vc_set_shader_index(int idx);

/* Basename helper, re-exported so backends do not include shader_list.h
 * just for this. */
#define vc_basename shader_list_basename

/* ------------------------------------------------------------------ */
/* Current shader state                                               */
/* ------------------------------------------------------------------ */

/* Program actually being drawn. Backends bind and draw it, and set it when
 * they hot-reload a shader in place. */
GLuint          vc_current_program(void);
void            vc_set_current_program(GLuint prog);
shader_uniforms vc_current_uniforms(void);
void            vc_set_current_uniforms(shader_uniforms u);

/* Program last handed to the draw loop. A switch copies current into this,
 * so the draw loop can always read one struct. */
shader_uniforms vc_draw_uniforms(void);
void            vc_set_draw_uniforms(shader_uniforms u);

/* Content hash of the shader currently loaded, valid flag, and whether a
 * pending switch needs its channel textures reloaded. */
unsigned long long vc_source_hash(void);
void               vc_set_source_hash(unsigned long long h);
int                vc_source_hash_valid(void);
void               vc_set_source_hash_valid(int valid);
int                vc_reload_channels_pending(void);
void               vc_set_reload_channels_pending(int pending);

/* ------------------------------------------------------------------ */
/* Channel textures and CLI bindings                                  */
/* ------------------------------------------------------------------ */

/* Explicit `--texture0..3` files, as parsed by the backend's CLI loop.
 * Auto-detected sibling textures live in shader_common and are never
 * stored here. */
extern const char *vc_tex_cli[4];

float  (*vc_channel_resolution(void))[3];
GLuint  *vc_channel_textures(void);

/* Channel that `-k/--keyboard` bound to the keyboard state texture, or -1. */
int  vc_bind_keyboard(void);
void vc_set_bind_keyboard(int ch);

/* (Re)load channel textures for the current shader: CLI-explicit files win,
 * otherwise auto-detect "<shader>.<ch>.<ext>". Forces iChannelResolution for
 * a keyboard-bound channel to the 256x3 keyboard texture size. */
void vc_load_channel_textures(void);

/* ------------------------------------------------------------------ */
/* Backend services                                                   */
/* ------------------------------------------------------------------ */

/* The two things the core cannot do for itself, supplied by whichever
 * window layer is compiled in. Registered once at startup, before any
 * shader is loaded. Both may be NULL, in which case the corresponding
 * action is skipped. */
typedef struct {
    /* Set the window title for the current shader at `fps`. */
    void (*refresh_title)(float fps);
    /* Current window size in pixels, for the MCP status line. */
    void (*window_size)(int *w, int *h);
} vc_backend_services;

void vc_set_backend_services(const vc_backend_services *services);

/* Ask the backend to refresh the title with the current FPS. */
void vc_request_title_refresh(void);

/* FPS the viewer last measured, shared with the status line. */
float vc_display_fps(void);
void  vc_set_display_fps(float fps);

/* ------------------------------------------------------------------ */
/* Shader loading and switching                                       */
/* ------------------------------------------------------------------ */

/* Compile with the shared builder, logging failures to stderr. */
GLuint vc_create_program(const char *frag_src, shader_uniforms *out_u);

/* FNV-1a over the source bytes. Content beats mtime: Windows stat has
 * one-second resolution, so back-to-back agent writes were invisible. */
unsigned long long vc_hash_source(const char *src);

/* Read + compile shaders[idx]; reports the source hash. 0 on failure. */
GLuint vc_load_shader_index(int idx, shader_uniforms *out_u,
                            unsigned long long *out_hash);

/* Advance the visible shader by delta (+1/-1), skipping entries that fail to
 * compile. Leaves the previous shader running if nothing compiles. */
void vc_switch_shader(int delta);

/* Rescan shaders/ and keep the current shader selected by filename, so files
 * created by MCP tools appear without restarting the viewer. */
void vc_rescan_shaders_preserving_current(void);

/* ------------------------------------------------------------------ */
/* MCP viewer hooks                                                   */
/* ------------------------------------------------------------------ */

/* The hook table to hand to mcp_viewer_start(). */
const mcp_viewer_hooks *vc_mcp_hooks(void);

/* Format the status body. `win_w`/`win_h` come from the backend, which is
 * the only layer that knows how to ask its window for a size. */
void vc_format_status(char *buf, size_t len, int win_w, int win_h);

/* ------------------------------------------------------------------ */
/* Keyboard state texture (Shadertoy-compatible)                       */
/* ------------------------------------------------------------------ */

/* Backends translate native keys into Shadertoy/JS key codes and call these.
 * Edge handling lives here so both backends agree on what a press means. */
void vc_keystate_press(int js_code);
void vc_keystate_release(int js_code);
void vc_reset_keystate(void);

/* Clear the "pressed this frame" row. Call once per frame after presenting,
 * so a press is visible for exactly one frame. */
void vc_partially_reset_keystate(void);

/* Upload the state texture to the keyboard-bound channel, if any. Call each
 * frame before drawing. */
void vc_update_keystate_texture(void);

/* Re-bind the keyboard texture to its channel. Call after
 * shader_bind_channels(), which points every channel at its image (or the
 * black fallback) and would otherwise unbind the keyboard channel. */
void vc_bind_keystate_texture(void);

/* Release GL objects owned by the core (keyboard texture). */
void vc_free_gl(void);

#endif /* VIEWER_CORE_H */
