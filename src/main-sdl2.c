/*
 * Shadertoy-style fullscreen fragment shader viewer.
 * Uses SDL2 for windowing/input and GLAD for GL 3.3+ function loading.
 *
 * Based on the GLFW version (shadertoy-glfw).
 *
 * Build:  make          (Linux / Windows with MSYS2/MinGW + sdl2-config)
 */

#define _DEFAULT_SOURCE   /* strdup, M_PI, etc. */

#include "glad.h"
#include <SDL2/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "config.h"
#include "mcp.h"
#ifdef MCP_UNAVAILABLE
/* Built without libjq (Makefile sets this define): keep the viewer building
 * with MCP disabled instead of failing at link time. */
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
#include "shader_common.h"
#include "shader_list.h"

/* ==================================================================
 * Shader list (for no-arg browsing)
 * ================================================================== */

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

static SDL_Window   *g_window = NULL;
static SDL_GLContext  g_gl_ctx = NULL;
static char  g_title_buf[512];
static float g_display_fps = 0.0f;

/* Explicit CLI channel textures (`--texture0..3`); auto-detected textures
 * live in shader_common and are never stored here. */
static const char *g_tex_cli[4] = { NULL, NULL, NULL, NULL };
static float   g_chanRes[4][3] = {{0}};
static GLuint  g_textures[4] = {0, 0, 0, 0};
static int     g_bindKeyboard = -1;

#define shader_basename shader_list_basename

/* (Re)load the channel textures for the current shader: CLI-explicit files
 * win, otherwise auto-detect "<shader>.<ch>.<ext>" via shader_common. */
static void load_channel_textures(void)
{
    shader_free_channels(g_textures);
    shader_load_channels(shader_paths[shader_index], g_tex_cli,
                         g_textures, g_chanRes);

    if (g_bindKeyboard >= 0) {
        g_chanRes[g_bindKeyboard][0] = 256.0f;
        g_chanRes[g_bindKeyboard][1] = 3.0f;
        g_chanRes[g_bindKeyboard][2] = 1.0f;
    }
}

static void update_window_title(float fps)
{
    const char *name = shader_basename(shader_paths[shader_index]);
    if (shader_count > 1)
        snprintf(g_title_buf, sizeof g_title_buf, "[%d/%d] %s  %.1f FPS",
                 shader_index + 1, shader_count, name, fps);
    else
        snprintf(g_title_buf, sizeof g_title_buf, "%s  %.1f FPS",
                 name, fps);
    SDL_SetWindowTitle(g_window, g_title_buf);
}

/* Viewer-side compilation: the shared builder, with diagnostics on stderr. */
static GLuint create_program(const char *frag_src, shader_uniforms *out_u)
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
static unsigned long long hash_source(const char *src)
{
    unsigned long long h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static GLuint load_shader_index(int idx, shader_uniforms *out_u,
                                unsigned long long *out_hash)
{
    char *src = shader_read_file(shader_paths[idx]);
    if (!src) {
        fprintf(stderr, "Error: could not read %s\n", shader_paths[idx]);
        return 0;
    }
    fprintf(stderr, "Loading shader [%d/%d]: %s\n", idx + 1, shader_count, shader_paths[idx]);
    if (out_hash)
        *out_hash = hash_source(src);
    GLuint p = create_program(src, out_u);
    free(src);
    return p;
}


static void switch_shader(int delta)
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
        GLuint new_prog = load_shader_index(new_idx, &current_u, &new_hash);
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
        load_channel_textures();
        return;
    }
    shader_index = original_index;
    fprintf(stderr, "No other shader compiled; keeping current one.\n");
}

/* ==================================================================
 * MCP viewer hooks (architecture B)
 * ================================================================== */

/* Rescan shaders/ and keep the current shader selected by filename, so
 * files created by MCP tools appear without restarting the viewer. */
static void rescan_shaders_preserving_current(void)
{
    shader_list fresh;
    shader_list_init(&fresh);
    if (shader_list_scan(&fresh, "shaders") <= 0) {
        shader_list_free(&fresh);
        return;
    }
    char current_name[512];
    snprintf(current_name, sizeof current_name, "%s",
             shader_basename(shader_paths[shader_index]));
    shader_list_free(&g_shaders);
    g_shaders = fresh;
    int found = 0;
    for (int i = 0; i < shader_count; i++) {
        if (strcmp(shader_basename(shader_paths[i]), current_name) == 0) {
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
        const char *base = shader_basename(path);
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
            rescan_shaders_preserving_current();
            idx = find_shader_by_name(name);
        }
        if (idx < 0) {
            snprintf(err, errlen, "Unknown shader: %s", name);
            return 0;
        }
    }
    if (idx < 0 || idx >= shader_count) {
        rescan_shaders_preserving_current();
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
    GLuint p = load_shader_index(idx, &u, &h);
    if (!p) {
        snprintf(err, errlen, "Shader failed to compile: %s",
                 shader_basename(shader_paths[idx]));
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
    load_channel_textures();
    update_window_title(g_display_fps);
    return 1;
}

static void viewer_hook_status(char *buf, size_t len)
{
    int w = 0, h = 0;
    if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    snprintf(buf, len,
             "running: yes\n"
             "shader: %s\n"
             "index: %d/%d\n"
             "window: %dx%d\n"
             "fps: %.1f",
             shader_basename(shader_paths[shader_index]),
             shader_index + 1, shader_count, w, h, g_display_fps);
}

static const mcp_viewer_hooks g_mcp_viewer_hooks = {
    viewer_hook_load_shader,
    viewer_hook_status
};

/* ==================================================================
 * Keyboard state texture (Shadertoy-compatible)
 * ================================================================== */

static unsigned char keyStateData[256 * 3];
static GLuint keyStateTexID = 0;

static void reset_keystate(void)
{
    memset(keyStateData, 0, sizeof(keyStateData));
}

static void partially_reset_keystate(void)
{
    for (int i = 0; i < 256; i++)
        keyStateData[i + 256] = 0;
}

/* Map SDL scancodes → Shadertoy / JS key codes */
static int map_sdl_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'a' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 112 + (sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_SPACE:         return 32;
    case SDL_SCANCODE_APOSTROPHE:    return 222;
    case SDL_SCANCODE_COMMA:         return 188;
    case SDL_SCANCODE_MINUS:         return 173;
    case SDL_SCANCODE_PERIOD:        return 190;
    case SDL_SCANCODE_SLASH:         return 191;
    case SDL_SCANCODE_SEMICOLON:     return 59;
    case SDL_SCANCODE_EQUALS:        return 61;
    case SDL_SCANCODE_LEFTBRACKET:   return 219;
    case SDL_SCANCODE_BACKSLASH:     return 220;
    case SDL_SCANCODE_RIGHTBRACKET:  return 221;
    case SDL_SCANCODE_GRAVE:         return 192;
    case SDL_SCANCODE_ESCAPE:        return 27;
    case SDL_SCANCODE_RETURN:        return 13;
    case SDL_SCANCODE_TAB:           return 9;
    case SDL_SCANCODE_BACKSPACE:     return 8;
    case SDL_SCANCODE_INSERT:        return 45;
    case SDL_SCANCODE_DELETE:        return 46;
    case SDL_SCANCODE_RIGHT:         return 39;
    case SDL_SCANCODE_LEFT:          return 37;
    case SDL_SCANCODE_DOWN:          return 40;
    case SDL_SCANCODE_UP:            return 38;
    case SDL_SCANCODE_PAGEUP:        return 33;
    case SDL_SCANCODE_PAGEDOWN:      return 34;
    case SDL_SCANCODE_HOME:          return 36;
    case SDL_SCANCODE_END:           return 35;
    case SDL_SCANCODE_CAPSLOCK:      return 20;
    case SDL_SCANCODE_SCROLLLOCK:    return 145;
    case SDL_SCANCODE_NUMLOCKCLEAR:  return 144;
    case SDL_SCANCODE_PRINTSCREEN:   return 42;
    case SDL_SCANCODE_PAUSE:         return 19;
    case SDL_SCANCODE_KP_DECIMAL:    return 110;
    case SDL_SCANCODE_KP_DIVIDE:     return 111;
    case SDL_SCANCODE_KP_MULTIPLY:   return 106;
    case SDL_SCANCODE_KP_MINUS:      return 109;
    case SDL_SCANCODE_KP_PLUS:       return 107;
    case SDL_SCANCODE_KP_ENTER:      return 13;
    case SDL_SCANCODE_LSHIFT:        return 16;
    case SDL_SCANCODE_LCTRL:         return 17;
    case SDL_SCANCODE_LALT:          return 18;
    case SDL_SCANCODE_LGUI:          return 91;
    case SDL_SCANCODE_RSHIFT:        return 16;
    case SDL_SCANCODE_RCTRL:         return 17;
    case SDL_SCANCODE_RALT:          return 225;
    case SDL_SCANCODE_RGUI:          return 91;
    case SDL_SCANCODE_APPLICATION:   return 93;
    default: return -1;
    }
}

static void update_keystate_texture(void)
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

/* ==================================================================
 * Main
 * ================================================================== */

int main(int argc, char **argv)
{
    (void)argc;
    const char *shader_path = NULL;
    int window_width  = 640;
    int window_height = 480;
    int window_x = SDL_WINDOWPOS_UNDEFINED;
    int window_y = SDL_WINDOWPOS_UNDEFINED;
    int fullscreen = 0;
    int bindKeyboard = -1;
    int mcp_port = 8642;

    /* parse command line options via getopt_long (from config.h) */
    int opt;
    int opt_idx = 0;
    /* Leading ':' makes getopt return ':' (not '?') for a missing argument,
     * so we can tell "-w with no value" apart from "-h = tall". */
    opterr = 0;
    while ((opt = getopt_long(argc, argv, options_string, long_options, &opt_idx)) != -1) {
        switch (opt) {
        case 's': shader_path = optarg; break;
        case '0': case '1': case '2': case '3':
            g_tex_cli[opt - '0'] = optarg; break;
        case 'f': fullscreen = 1; break;
        case 'w': { int v = atoi(optarg); if (v > 0) window_width = v; break; }
        case 'h': { int v = atoi(optarg); if (v > 0) window_height = v; break; }
        case 'x': { int v = atoi(optarg); if (v >= 0) window_x = v; break; }
        case 'y': { int v = atoi(optarg); if (v >= 0) window_y = v; break; }
        case 'k': { int v = atoi(optarg); if (v >= 0 && v < 4) bindKeyboard = v; break; }
        case 'p': {
            int v = atoi(optarg);
            if (v >= 0 && v <= 65535) mcp_port = v;
            break;
        }
        case '?':
        case ':':
            if (opt == ':' && optopt == 'h') {
                /* Bare "-h" has always meant help for users, even though
                 * --height also needs an argument. Keep -h meaning height
                 * only when a numeric value follows it. */
                fprintf(stderr, "Error: -h/--height requires a numeric value "
                                "(did you mean --help?)\n");
                return 1;
            }
            fprintf(stderr,
                "Usage: %s [OPTIONS] [SHADER_PATH]\n\n"
                "Runs the interactive viewer with no options, loading every\n"
                ".glsl file in ./shaders (arrow keys / space to cycle).\n\n"
                "Options:\n"
                "  -s, --source <path>     fragment shader source file\n"
                "  -w, --width <value>     window width (default: 640)\n"
                "  -h, --height <value>    window height (default: 480)\n"
                "  -x, --window_x <value>  window X position\n"
                "  -y, --window_y <value>  window Y position\n"
                "  -f, --fullscreen        fullscreen mode\n"
                "  -0, --texture0 <path>   texture for iChannel0\n"
                "  -1, --texture1 <path>   texture for iChannel1\n"
                "  -2, --texture2 <path>   texture for iChannel2\n"
                "  -3, --texture3 <path>   texture for iChannel3\n"
                "  -k, --keyboard <ch>     bind keyboard state texture to iChannel<ch>\n"
                "  -p, --mcp-port <n>      viewer MCP HTTP port (default: 8642,\n"
                "                             0 disables the server)\n"
                "  -?, --help              show this help\n\n"
                "Note: -h is height, not help. Use -? or --help for this text.\n",
                argv[0]);
            return (opt == '?') ? 0 : 1;
        }
    }

    /* Usage documents [SHADER_PATH] as the positional form of --source;
     * -s <path> wins if both are given. */
    if (optind < argc)
        shader_path = argv[optind];

    if (bindKeyboard >= 0 && g_tex_cli[bindKeyboard]) {
        fprintf(stderr, "Error: cannot bind texture and keyboard to the same channel.\n");
        return 1;
    }
    if (bindKeyboard >= 0)
        printf("Dynamic keyboard texture bound to iChannel%d.\n", bindKeyboard);

    /* Init SDL2 */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* ------------------------------------------------------------------
     * Create OpenGL context — probe versions on ARM ES, fixed on desktop
     * ------------------------------------------------------------------ */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

#ifdef USE_GLES
    /* On ARM/aarch64, probe ES 3.2 → 3.1 → 3.0 */
    {
        int es_versions[][2] = { {3,2}, {3,1}, {3,0} };
        int n_versions = sizeof(es_versions) / sizeof(es_versions[0]);
        int ctx_ok = 0;
        for (int i = 0; i < n_versions; i++) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, es_versions[i][0]);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es_versions[i][1]);

            Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
            if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

            g_window = SDL_CreateWindow("Shadertoy",
                window_x, window_y, window_width, window_height, flags);
            if (!g_window) continue;

            g_gl_ctx = SDL_GL_CreateContext(g_window);
            if (g_gl_ctx) {
                printf("OpenGL ES %d.%d context created\n",
                       es_versions[i][0], es_versions[i][1]);
                ctx_ok = 1;
                break;
            }
            /* Context failed — destroy window and try next version */
            SDL_DestroyWindow(g_window);
            g_window = NULL;
        }
        if (!ctx_ok) {
            fprintf(stderr, "Failed to create any OpenGL ES context\n");
            SDL_Quit();
            return 1;
        }
    }
#else
    /* Desktop: try 3.3 core, then compat */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    {
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
        if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

        g_window = SDL_CreateWindow("Shadertoy",
            window_x, window_y, window_width, window_height, flags);
        if (!g_window) {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
        g_gl_ctx = SDL_GL_CreateContext(g_window);
        if (!g_gl_ctx) {
            fprintf(stderr, "GL 3.3 core failed, trying compat...\n");
            SDL_GL_DeleteContext(g_gl_ctx);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            g_gl_ctx = SDL_GL_CreateContext(g_window);
            if (!g_gl_ctx) {
                fprintf(stderr, "Failed to create any OpenGL context\n");
                SDL_DestroyWindow(g_window);
                SDL_Quit();
                return 1;
            }
        }
    }
#endif

    if (window_x != SDL_WINDOWPOS_UNDEFINED && window_y != SDL_WINDOWPOS_UNDEFINED)
        SDL_SetWindowPosition(g_window, window_x, window_y);

    SDL_GL_SetSwapInterval(1); /* vsync */

    /* Load GL functions via GLAD */
#ifdef USE_GLES
    if (!gladLoadGLES2Loader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "GLAD GLES init failed\n");
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }
#else
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "GLAD init failed\n");
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }
#endif

    printf("GL Vendor:   %s\n", glGetString(GL_VENDOR));
    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version:  %s\n", glGetString(GL_VERSION));

    /* Initial viewport */
    glViewport(0, 0, window_width, window_height);
    float g_width = (float)window_width, g_height = (float)window_height;

    reset_keystate();

    /* Shader list */
    shader_list_init(&g_shaders);
    if (shader_path) {
        if (shader_list_add(&g_shaders, shader_path) != 0) {
            fprintf(stderr, "Error: out of memory adding shader path.\n");
            SDL_GL_DeleteContext(g_gl_ctx);
            SDL_DestroyWindow(g_window);
            SDL_Quit();
            return 1;
        }
        shader_index = 0;
        /* Be explicit: without the directory scan there is nothing to browse,
         * so the cycle keys do nothing. Say so rather than failing silently. */
        fprintf(stderr,
                "Note: -s loads a single shader, so arrow-key browsing is "
                "disabled.\n"
                "      Run without -s to browse everything in shaders/.\n");
    } else {
        shader_list_scan(&g_shaders, "shaders");
        if (shader_count == 0) {
            fprintf(stderr, "Error: no .glsl files found in shaders/ and no -s flag.\n");
            SDL_GL_DeleteContext(g_gl_ctx);
            SDL_DestroyWindow(g_window);
            SDL_Quit();
            return 1;
        }
        printf("Found %d shader(s) in shaders/\n", shader_count);

        /* Try shaders in order and start on the first one that compiles,
         * so one broken file doesn't kill the whole browser session. */
        int found = 0;
        for (int i = 0; i < shader_count; i++) {
            char *src = shader_read_file(shader_paths[i]);
            if (!src) {
                fprintf(stderr, "Could not read '%s', skipping.\n", shader_paths[i]);
                continue;
            }
            GLuint test_prog = create_program(src, NULL);
            free(src);
            if (test_prog) {
                glDeleteProgram(test_prog);
                shader_index = i;
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Error: none of the %d shaders in shaders/ compiled.\n",
                    shader_count);
            SDL_GL_DeleteContext(g_gl_ctx);
            SDL_DestroyWindow(g_window);
            SDL_Quit();
            return 1;
        }
    }

    /* Load initial shader */
    GLuint prog = load_shader_index(shader_index, &current_u,
                                    &current_source_hash);
    current_prog = prog;
    current_source_hash_valid = 1;
    if (!prog) {
        fprintf(stderr, "Error: shader compilation failed.\n");
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    g_u = current_u;

    g_bindKeyboard = bindKeyboard;

    load_channel_textures();

    /* Architecture B: the viewer hosts the MCP server. A port conflict is
     * fatal so the configured client URL never talks to the wrong instance. */
    if (!mcp_viewer_start(mcp_port, &g_mcp_viewer_hooks)) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "Could not start the MCP server on 127.0.0.1:%d.\n"
                 "Is another viewer already running?\n"
                 "See build/shadertoy-mcp.log.", mcp_port);
        fprintf(stderr, "%s\n", msg);
        /* GUI build has no console; don't disappear silently. */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "Shadertoy MCP", msg, g_window);
        shader_free_channels(g_textures);
        glDeleteProgram(prog);
        shader_list_free(&g_shaders);
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    /* Fullscreen quad */
    static const float quad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    /* Mouse state */
    float g_mouseX = 0.0f, g_mouseY = 0.0f;
    float g_mouseL = 0.0f, g_mouseR = 0.0f;

    /* Timing */
    Uint32 start_ticks = SDL_GetTicks();
    int frame_number = 0;
    int frame_count = 0;
    float fps_timer = 0.0f;
    float current_fps = 0.0f;
    float prev_t = 0.0f;   /* previous frame's time, for iTimeDelta */

    update_window_title(0.0f);

    /* Main loop */
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    int w = ev.window.data1;
                    int h = ev.window.data2;
                    glViewport(0, 0, w, h);
                    g_width = (float)w;
                    g_height = (float)h;
                }
                break;

            case SDL_MOUSEMOTION:
                g_mouseX = (float)ev.motion.x;
                g_mouseY = g_height - (float)ev.motion.y;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)  g_mouseL = 1.0f;
                if (ev.button.button == SDL_BUTTON_RIGHT) g_mouseR = 1.0f;
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)  g_mouseL = 0.0f;
                if (ev.button.button == SDL_BUTTON_RIGHT) g_mouseR = 0.0f;
                break;

            case SDL_KEYDOWN: {
                SDL_Keycode key = ev.key.keysym.sym;
                SDL_Scancode sc = ev.key.keysym.scancode;

                /* Shadertoy key state texture */
                int js = map_sdl_key(sc);
                if (js >= 0 && js < 256) {
                    keyStateData[js + 0]   = 255;
                    keyStateData[js + 256] = 255;
                    keyStateData[js + 512] ^= 0xff;
                }

                /* Exit */
                if (key == SDLK_q || key == SDLK_ESCAPE)
                    running = 0;

                /* Shader navigation */
                if (key == SDLK_SPACE || key == SDLK_RIGHT || key == SDLK_PAGEDOWN)
                    switch_shader(1);
                if (key == SDLK_BACKSPACE || key == SDLK_LEFT || key == SDLK_PAGEUP)
                    switch_shader(-1);
                break;
            }

            case SDL_KEYUP: {
                SDL_Scancode sc = ev.key.keysym.scancode;
                int js = map_sdl_key(sc);
                if (js >= 0 && js < 256) {
                    keyStateData[js + 0]   = 0;
                    keyStateData[js + 256] = 0;
                }
                break;
            }
            }
        }

        float t = (float)(SDL_GetTicks() - start_ticks) / 1000.0f;
        float lastFrameTime = t - prev_t;
        prev_t = t;

        /* Pick up shaders created by the MCP server without restarting. */
        {
            static Uint32 last_scan = 0;
            Uint32 now_ticks = SDL_GetTicks();
            if (!shader_path && now_ticks - last_scan >= 1000) {
                last_scan = now_ticks;
                rescan_shaders_preserving_current();
            }
        }

        /* Hot-reload by content, not mtime: Windows stat has one-second
         * resolution, so same-second rewrites used to be missed. */
        {
            static Uint32 last_poll = 0;
            Uint32 now_ticks = SDL_GetTicks();
            if (now_ticks - last_poll >= 250) {
                last_poll = now_ticks;
                char *src = shader_read_file(shader_paths[shader_index]);
                if (src) {
                    unsigned long long h = hash_source(src);
                    if (!current_source_hash_valid ||
                        h != current_source_hash) {
                        current_source_hash = h;
                        current_source_hash_valid = 1;
                        GLuint reloaded = create_program(src, &current_u);
                        if (reloaded) {
                            if (current_prog && current_prog != reloaded)
                                glDeleteProgram(current_prog);
                            current_prog = reloaded;
                            if (reload_channels_pending) {
                                load_channel_textures();
                                reload_channels_pending = 0;
                            }
                            fprintf(stderr, "Reloaded shader: %s\n",
                                    shader_paths[shader_index]);
                        }
                    }
                    free(src);
                }
            }
        }

        /* FPS calculation */
        frame_count++;
        frame_number++;
        float dt = t - fps_timer;
        if (dt >= 0.5f) {
            current_fps = (float)frame_count / dt;
            g_display_fps = current_fps;
            frame_count = 0;
            fps_timer = t;
            update_window_title(current_fps);
#ifdef DEBUG
            printf("FPS: %.1f  Frame: %d\n", current_fps, frame_number);
#endif
        }

        time_t now = time(NULL);
        struct tm *tm_now = localtime(&now);

        /* Hot-reload uniforms after shader switch */
        if (current_prog != prog) {
            prog = current_prog;
            g_u = current_u;
        }

        /* Offscreen MCP tools leave their own framebuffer bound. */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, (int)g_width, (int)g_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(prog);

        /* One shared uploader, so viewer and MCP set identical values. */
        shader_frame f;
        shader_frame_defaults(&f);
        f.width       = g_width;
        f.height      = g_height;
        f.time        = t;
        f.time_delta  = lastFrameTime;
        f.frame       = frame_number;
        f.frame_rate  = current_fps;
        f.mouse_x     = g_mouseX;
        f.mouse_y     = g_mouseY;
        f.mouse_left  = g_mouseL;
        f.mouse_right = g_mouseR;
        memcpy(f.channel_resolution, g_chanRes, sizeof f.channel_resolution);
        for (int ch = 0; ch < 4; ch++)
            f.channel_time[ch] = t;
        if (tm_now) {
            f.date_valid   = 1;
            f.date_year    = (float)(tm_now->tm_year + 1900);
            f.date_month   = (float)tm_now->tm_mon;   /* Shadertoy: 0-based */
            f.date_day     = (float)tm_now->tm_mday;
            f.date_seconds = (float)tm_now->tm_hour * 3600.0f +
                             (float)tm_now->tm_min * 60.0f +
                             (float)tm_now->tm_sec +
                             (t - floor(t));
        }
        shader_upload_uniforms(&g_u, &f);

        /* Bind textures to iChannel0..3, then override the keyboard channel
         * if one is bound. Black fallbacks come from shader_common. */
        shader_bind_channels(g_textures);
        update_keystate_texture();
        if (g_bindKeyboard >= 0 && keyStateTexID && g_bindKeyboard < 4) {
            glActiveTexture(GL_TEXTURE0 + g_bindKeyboard);
            glBindTexture(GL_TEXTURE_2D, keyStateTexID);
        }

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* MCP jobs run here so window screenshots see this frame's pixels. */
        mcp_viewer_poll();

        SDL_GL_SwapWindow(g_window);
        partially_reset_keystate();
    }

    mcp_viewer_stop();

    /* Cleanup */
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    shader_free_channels(g_textures);
    if (keyStateTexID)
        glDeleteTextures(1, &keyStateTexID);
    glDeleteProgram(prog);

    shader_list_free(&g_shaders);

    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
