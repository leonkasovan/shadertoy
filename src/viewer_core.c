/*
 * Backend-independent viewer core.
 *
 * See viewer_core.h for the contract. This file is the single source of
 * truth for shader loading, switching, hot-reload bookkeeping, the MCP viewer
 * hooks and the keyboard state texture; main-sdl2.c and main-glfw.c keep only
 * their window/input layer.
 *
 * The function bodies here were moved verbatim out of both main-*.c files.
 * Where the two copies had already drifted (the keystate helpers, the status
 * line, the title refresh) the single copy is written against the backend
 * services in viewer_core.h instead of picking one side's behaviour.
 */

#define _DEFAULT_SOURCE   /* strdup, M_PI, etc. */

#include "glad.h"
#include "viewer_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MCP_UNAVAILABLE
/* Built without libjq (Makefile sets this define): keep the viewer building
 * with MCP disabled instead of failing at link time. This used to be
 * duplicated verbatim in both main-*.c files. */
int mcp_viewer_start(int port, const mcp_viewer_hooks *hooks)
{
    (void)hooks;
    if (port > 0)
        fprintf(stderr, "MCP disabled: this build has no libjq "
                        "(e.g. MSYS2: pacman -S mingw-w64-x86_64-jq).\n");
    return 1;
}
void mcp_viewer_poll(void) {}
void mcp_viewer_stop(void) {}
int mcp_viewer_port(void) { return 0; }
#endif

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

/* Shader paths, grown dynamically. */
static shader_list g_shaders;
#define shader_paths (g_shaders.items)
#define shader_count (g_shaders.count)
static int   shader_index = 0;
static unsigned long long current_source_hash;
static int    current_source_hash_valid = 0;
static int    reload_channels_pending = 0;

/* Current shader state (for hot-reload on switch) */
static GLuint  current_prog = 0;
static shader_uniforms current_u;

/* Uniform locations for the program currently being drawn. A shader switch
 * copies current_u into g_u, so the draw loop always reads one struct. */
static shader_uniforms g_u;

static float g_display_fps = 0.0f;

/* Explicit CLI channel textures (`--texture0..3`); auto-detected textures
 * live in shader_common and are never stored here. */
const char *vc_tex_cli[4] = { NULL, NULL, NULL, NULL };
static float   g_chanRes[4][3] = {{0}};
static GLuint  g_textures[4] = {0, 0, 0, 0};
static int     g_bindKeyboard = -1;

static vc_backend_services g_services;

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

shader_list *vc_shaders(void)          { return &g_shaders; }
int          vc_shader_count(void)     { return shader_count; }
const char  *vc_shader_path(int idx)   { return shader_paths[idx]; }
int          vc_shader_index(void)     { return shader_index; }
void         vc_set_shader_index(int i) { shader_index = i; }

GLuint          vc_current_program(void)   { return current_prog; }
void            vc_set_current_program(GLuint p) { current_prog = p; }
shader_uniforms vc_current_uniforms(void)  { return current_u; }
void            vc_set_current_uniforms(shader_uniforms u) { current_u = u; }
shader_uniforms vc_draw_uniforms(void)     { return g_u; }
void            vc_set_draw_uniforms(shader_uniforms u) { g_u = u; }

unsigned long long vc_source_hash(void)  { return current_source_hash; }
void               vc_set_source_hash(unsigned long long h) { current_source_hash = h; }
int                vc_source_hash_valid(void) { return current_source_hash_valid; }
void               vc_set_source_hash_valid(int v) { current_source_hash_valid = v; }
int                vc_reload_channels_pending(void) { return reload_channels_pending; }
void               vc_set_reload_channels_pending(int v) { reload_channels_pending = v; }

float (*vc_channel_resolution(void))[3] { return g_chanRes; }
GLuint *vc_channel_textures(void)       { return g_textures; }

int  vc_bind_keyboard(void)        { return g_bindKeyboard; }
void vc_set_bind_keyboard(int ch)  { g_bindKeyboard = ch; }

float vc_display_fps(void)         { return g_display_fps; }
void  vc_set_display_fps(float f)  { g_display_fps = f; }

void vc_set_backend_services(const vc_backend_services *services)
{
    if (services)
        g_services = *services;
    else
        memset(&g_services, 0, sizeof g_services);
}

void vc_request_title_refresh(void)
{
    if (g_services.refresh_title)
        g_services.refresh_title(g_display_fps);
}

/* ------------------------------------------------------------------ */
/* Channel textures                                                   */
/* ------------------------------------------------------------------ */

/* (Re)load the channel textures for the current shader: CLI-explicit files
 * win, otherwise auto-detect "<shader>.<ch>.<ext>" via shader_common. */
void vc_load_channel_textures(void)
{
    shader_free_channels(g_textures);
    shader_load_channels(shader_paths[shader_index], vc_tex_cli,
                         g_textures, g_chanRes);

    if (g_bindKeyboard >= 0) {
        g_chanRes[g_bindKeyboard][0] = 256.0f;
        g_chanRes[g_bindKeyboard][1] = 3.0f;
        g_chanRes[g_bindKeyboard][2] = 1.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Compilation and loading                                            */
/* ------------------------------------------------------------------ */

/* Viewer-side compilation: the shared builder, with diagnostics on stderr. */
GLuint vc_create_program(const char *frag_src, shader_uniforms *out_u)
{
    char log[4096] = {0};
    GLuint p = shader_build_program(frag_src, log, sizeof log);
    if (!p) {
        fprintf(stderr, "fragment shader compile error:\n%s\n",
                log[0] ? log : "(no log available)");
        return 0;
    }
    if (out_u)
        shader_uniforms_lookup(p, out_u);
    return p;
}

/* FNV-1a over the source bytes. Content beats mtime here: Windows stat has
 * one-second resolution, so back-to-back agent writes were invisible. */
unsigned long long vc_hash_source(const char *src)
{
    unsigned long long h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

GLuint vc_load_shader_index(int idx, shader_uniforms *out_u,
                            unsigned long long *out_hash)
{
    char *src = shader_read_file(shader_paths[idx]);
    if (!src) {
        fprintf(stderr, "Error: could not read %s\n", shader_paths[idx]);
        return 0;
    }
    fprintf(stderr, "Loading shader [%d/%d]: %s\n", idx + 1, shader_count, shader_paths[idx]);
    if (out_hash)
        *out_hash = vc_hash_source(src);
    GLuint p = vc_create_program(src, out_u);
    free(src);
    return p;
}

void vc_switch_shader(int delta)
{
    if (shader_count <= 1) return;
    /* Advance until a shader actually compiles, trying each entry at most
     * once so a run of broken files can't loop forever. On total failure
     * the previously loaded shader keeps running. */
    int original_index = shader_index;
    for (int attempt = 0; attempt < shader_count; attempt++) {
        int new_idx = (shader_index + delta + shader_count) % shader_count;
        if (new_idx == shader_index) return;
        unsigned long long new_hash = 0;
        GLuint new_prog = vc_load_shader_index(new_idx, &current_u, &new_hash);
        if (!new_prog) {
            fprintf(stderr, "Skipping '%s' (compilation failed).\n", shader_paths[new_idx]);
            shader_index = new_idx;
            continue;
        }
        if (current_prog)
            glDeleteProgram(current_prog);
        shader_index = new_idx;
        current_prog = new_prog;
        current_source_hash = new_hash;
        current_source_hash_valid = 1;
        /* Auto-detect and reload textures for new shader */
        vc_load_channel_textures();
        return;
    }
    shader_index = original_index;
    fprintf(stderr, "No other shader compiled; keeping current one.\n");
}

/* ------------------------------------------------------------------ */
/* MCP viewer hooks (architecture B)                                  */
/* ------------------------------------------------------------------ */

/* Rescan shaders/ and keep the current shader selected by filename, so
 * files created by MCP tools appear without restarting the viewer. */
void vc_rescan_shaders_preserving_current(void)
{
    shader_list fresh;
    shader_list_init(&fresh);
    if (shader_list_scan(&fresh, "shaders") <= 0) {
        shader_list_free(&fresh);
        return;
    }
    char current_name[512];
    snprintf(current_name, sizeof current_name, "%s",
             shader_list_basename(shader_paths[shader_index]));
    shader_list_free(&g_shaders);
    g_shaders = fresh;
    int found = 0;
    for (int i = 0; i < shader_count; i++) {
        if (strcmp(shader_list_basename(shader_paths[i]), current_name) == 0) {
            shader_index = i;
            found = 1;
            break;
        }
    }
    if (!found) {
        shader_index = 0;
        current_source_hash_valid = 0;
        reload_channels_pending = 1;
    }
}

static int find_shader_by_name(const char *name)
{
    for (int i = 0; i < shader_count; i++) {
        const char *path = shader_paths[i];
        const char *base = shader_list_basename(path);
        if (strcmp(path, name) == 0 || strcmp(base, name) == 0)
            return i;
        size_t blen = strlen(base), nlen = strlen(name);
        if (blen > 5 && strcmp(base + blen - 5, ".glsl") == 0 &&
            nlen == blen - 5 && strncmp(base, name, nlen) == 0)
            return i;
    }
    return -1;
}

/* Main thread, GL context current: called from mcp_viewer_poll(). */
static int viewer_hook_load_shader(const char *name, int index,
                                   char *err, size_t errlen)
{
    int idx = index;
    if (name && *name) {
        idx = find_shader_by_name(name);
        if (idx < 0) {
            /* May have just been written by write_shader. */
            vc_rescan_shaders_preserving_current();
            idx = find_shader_by_name(name);
        }
        if (idx < 0) {
            snprintf(err, errlen, "Unknown shader: %s", name);
            return 0;
        }
    }
    if (idx < 0 || idx >= shader_count) {
        vc_rescan_shaders_preserving_current();
        if (idx < 0 || idx >= shader_count) {
            snprintf(err, errlen, "Shader index out of range (0..%d)",
                     shader_count - 1);
            return 0;
        }
    }
    if (idx == shader_index)
        return 1;

    shader_uniforms u;
    unsigned long long h = 0;
    GLuint p = vc_load_shader_index(idx, &u, &h);
    if (!p) {
        snprintf(err, errlen, "Shader failed to compile: %s",
                 shader_list_basename(shader_paths[idx]));
        return 0;
    }
    if (current_prog)
        glDeleteProgram(current_prog);
    shader_index = idx;
    current_prog = p;
    current_u = u;
    g_u = u;
    current_source_hash = h;
    current_source_hash_valid = 1;
    vc_load_channel_textures();
    vc_request_title_refresh();
    return 1;
}

void vc_format_status(char *buf, size_t len, int win_w, int win_h)
{
    snprintf(buf, len,
             "running: yes\n"
             "shader: %s\n"
             "index: %d/%d\n"
             "window: %dx%d\n"
             "fps: %.1f",
             shader_list_basename(shader_paths[shader_index]),
             shader_index + 1, shader_count, win_w, win_h, g_display_fps);
}

static void viewer_hook_status(char *buf, size_t len)
{
    int w = 0, h = 0;
    if (g_services.window_size)
        g_services.window_size(&w, &h);
    vc_format_status(buf, len, w, h);
}

static const mcp_viewer_hooks g_mcp_viewer_hooks = {
    viewer_hook_load_shader,
    viewer_hook_status
};

const mcp_viewer_hooks *vc_mcp_hooks(void)
{
    return &g_mcp_viewer_hooks;
}

/* ------------------------------------------------------------------ */
/* Keyboard state texture (Shadertoy-compatible)                       */
/* ------------------------------------------------------------------ */

static unsigned char keyStateData[256 * 3];
static GLuint keyStateTexID = 0;

void vc_reset_keystate(void)
{
    memset(keyStateData, 0, sizeof(keyStateData));
}

void vc_partially_reset_keystate(void)
{
    for (int i = 0; i < 256; i++)
        keyStateData[i + 256] = 0;
}

/* Row 0 = held, row 1 = pressed this frame, row 2 = toggled. Toggling lives
 * here so no backend can forget (or double-apply) it on auto-repeat: callers
 * only report real presses. */
void vc_keystate_press(int js_code)
{
    if (js_code < 0 || js_code >= 256)
        return;
    keyStateData[js_code + 0]   = 255;
    keyStateData[js_code + 256] = 255;
    keyStateData[js_code + 512] ^= 0xff;
}

void vc_keystate_release(int js_code)
{
    if (js_code < 0 || js_code >= 256)
        return;
    keyStateData[js_code + 0]   = 0;
    keyStateData[js_code + 256] = 0;
}

void vc_update_keystate_texture(void)
{
    if (g_bindKeyboard < 0)
        return;
    if (!keyStateTexID)
        glGenTextures(1, &keyStateTexID); /* lazy init: needs live GL context */
    glActiveTexture(GL_TEXTURE0 + g_bindKeyboard);
    glBindTexture(GL_TEXTURE_2D, keyStateTexID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 3, 0,
                 GL_RED, GL_UNSIGNED_BYTE, keyStateData);
}

void vc_bind_keystate_texture(void)
{
    if (g_bindKeyboard < 0 || g_bindKeyboard >= 4 || !keyStateTexID)
        return;
    glActiveTexture(GL_TEXTURE0 + g_bindKeyboard);
    glBindTexture(GL_TEXTURE_2D, keyStateTexID);
}

void vc_free_gl(void)
{    if (keyStateTexID) {
        glDeleteTextures(1, &keyStateTexID);
        keyStateTexID = 0;
    }
}
