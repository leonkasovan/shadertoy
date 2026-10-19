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
#include <dirent.h>
#include <time.h>
#include "config.h"
#include "mcp.h"
#include "shader_common.h"

/* ==================================================================
 * Utility
 * ================================================================== */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (buf) {
        size_t n = fread(buf, 1, (size_t)len, f);
        buf[n] = '\0';
    }
    fclose(f);
    return buf;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* ==================================================================
 * Texture loading via stb_image
 * ================================================================== */

static GLuint load_texture(const char *path)
{
    int w, h, channels;
    stbi_set_flip_vertically_on_load(1);
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) {
        fprintf(stderr, "failed to load texture '%s': %s\n", path, stbi_failure_reason());
        return 0;
    }
    printf("Loaded texture: %s (%dx%d, %d ch)\n", path, w, h, channels);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(data);
    return tex;
}

/* ==================================================================
 * Shader compilation and program linking
 *
 * The GLSL version, vertex shader, fragment header/footer and uniform
 * handling all live in shader_common.c so the viewer and the MCP server
 * cannot drift apart. This file only adds viewer-specific behaviour:
 * writing diagnostics to stderr.\n * ================================================================== */

static GLuint create_program(const char *frag_src)
{
    char log[4096] = {0};
    GLuint p = shader_build_program(frag_src, log, sizeof log);
    if (!p)
        fprintf(stderr, "fragment shader compile error:\n%s\n",
                log[0] ? log : "(no log available)");
    return p;
}

/* ==================================================================
 * Shader list (for no-arg browsing)
 * ================================================================== */

#define MAX_SHADERS 256
static char *shader_list[MAX_SHADERS];
static int   shader_count = 0;
static int   shader_index = 0;

/* Current shader state (for hot-reload on switch) */
static GLuint  current_prog = 0;
static shader_uniforms current_u;

/* Uniform locations for the program currently being drawn. A shader switch
 * copies current_u into g_u, so the draw loop always reads one struct. */
static shader_uniforms g_u;

static SDL_Window   *g_window = NULL;
static SDL_GLContext  g_gl_ctx = NULL;
static char  g_title_buf[512];

/* Global texture state */
static GLuint  g_textures[4] = {0, 0, 0, 0};
static float   g_chanRes[4][3] = {{0}};
static const char *g_texture_files[4] = {NULL, NULL, NULL, NULL};
static int     g_auto_texture[4] = {0, 0, 0, 0};
static int     g_bindKeyboard = -1;

static int compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void scan_shaders_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && shader_count < MAX_SHADERS) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen > 5 && strcmp(nm + nlen - 5, ".glsl") == 0) {
            size_t len = strlen(dir) + 1 + strlen(ent->d_name) + 1;
            char *path = malloc(len);
            snprintf(path, len, "%s/%s", dir, ent->d_name);
            shader_list[shader_count++] = path;
        }
    }
    closedir(d);
    if (shader_count > 1)
        qsort(shader_list, shader_count, sizeof(char *), compare_strings);
}

static const char *shader_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    return p ? p + 1 : path;
}

static void auto_detect_textures(const char *shader_path)
{
    size_t len = strlen(shader_path);
    if (len < 5) return;
    const char *ext = shader_path + len - 5;
    if (strcmp(ext, ".glsl") != 0) return;

    size_t base_len = len - 5;
    for (int ch = 0; ch < 4; ch++) {
        if (g_texture_files[ch]) continue;
        char path[512];
        snprintf(path, sizeof path, "%.*s.%d.png", (int)base_len, shader_path, ch);
        if (file_exists(path)) {
            g_texture_files[ch] = strdup(path);
            g_auto_texture[ch] = 1;
        } else {
            snprintf(path, sizeof path, "%.*s.%d.jpg", (int)base_len, shader_path, ch);
            if (file_exists(path)) {
                g_texture_files[ch] = strdup(path);
                g_auto_texture[ch] = 1;
            }
        }
    }
}

static void load_channel_textures(void)
{
    for (int ch = 0; ch < 4; ch++) {
        if (g_textures[ch]) {
            glDeleteTextures(1, &g_textures[ch]);
            g_textures[ch] = 0;
            g_chanRes[ch][0] = g_chanRes[ch][1] = g_chanRes[ch][2] = 0;
        }
        if (g_auto_texture[ch]) {
            free((void *)g_texture_files[ch]);
            g_texture_files[ch] = NULL;
            g_auto_texture[ch] = 0;
        }
    }

    auto_detect_textures(shader_list[shader_index]);

    for (int ch = 0; ch < 4; ch++) {
        if (g_texture_files[ch]) {
            g_textures[ch] = load_texture(g_texture_files[ch]);
            if (g_textures[ch]) {
                /* Query into GLint locals: glGetTexLevelParameteriv writes
                 * integers, so passing a float* via a cast would alias and
                 * (worse) store bit patterns, not values. Widen afterwards. */
                GLint tex_w = 0, tex_h = 0;
                glBindTexture(GL_TEXTURE_2D, g_textures[ch]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &tex_w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_h);
                g_chanRes[ch][0] = (float)tex_w;
                g_chanRes[ch][1] = (float)tex_h;
                g_chanRes[ch][2] = 1.0f;
            }
        }
    }

    if (g_bindKeyboard >= 0) {
        g_chanRes[g_bindKeyboard][0] = 256.0f;
        g_chanRes[g_bindKeyboard][1] = 3.0f;
        g_chanRes[g_bindKeyboard][2] = 1.0f;
    }
}

static void update_window_title(float fps)
{
    const char *name = shader_basename(shader_list[shader_index]);
    if (shader_count > 1)
        snprintf(g_title_buf, sizeof g_title_buf, "[%d/%d] %s  %.1f FPS",
                 shader_index + 1, shader_count, name, fps);
    else
        snprintf(g_title_buf, sizeof g_title_buf, "%s  %.1f FPS",
                 name, fps);
    SDL_SetWindowTitle(g_window, g_title_buf);
}

static GLuint load_shader_index(int idx, shader_uniforms *out_u)
{
    char *src = read_file(shader_list[idx]);
    if (!src) {
        fprintf(stderr, "Error: could not read %s\n", shader_list[idx]);
        return 0;
    }
    printf("Loading shader [%d/%d]: %s\n", idx + 1, shader_count, shader_list[idx]);
    GLuint p = create_program(src);
    free(src);
    if (!p) return 0;
    shader_uniforms_lookup(p, out_u);
    return p;
}


static void switch_shader(int delta)
{
    if (shader_count <= 1) return;
    /* Advance until a shader actually compiles, trying each entry at most
     * once so a run of broken files can't loop forever. On total failure
     * the previously loaded shader keeps running. */
    for (int attempt = 0; attempt < shader_count; attempt++) {
        int new_idx = (shader_index + delta + shader_count) % shader_count;
        if (new_idx == shader_index) return;
        GLuint new_prog = load_shader_index(new_idx, &current_u);
        if (!new_prog) {
            fprintf(stderr, "Skipping '%s' (compilation failed).\n", shader_list[new_idx]);
            shader_index = new_idx;
            continue;
        }
        if (current_prog)
            glDeleteProgram(current_prog);
        shader_index = new_idx;
        current_prog = new_prog;
        /* Auto-detect and reload textures for new shader */
        load_channel_textures();
        return;
    }
    fprintf(stderr, "No other shader compiled; keeping current one.\n");
}

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
    const char *texture_files[4] = { NULL, NULL, NULL, NULL };
    int window_width  = 640;
    int window_height = 480;
    int window_x = SDL_WINDOWPOS_UNDEFINED;
    int window_y = SDL_WINDOWPOS_UNDEFINED;
    int fullscreen = 0;
    int bindKeyboard = -1;
    int mcp_mode = 0;
    int mcp_lsp  = 0;

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
            texture_files[opt - '0'] = optarg; break;
        case 'f': fullscreen = 1; break;
        case 'w': { int v = atoi(optarg); if (v > 0) window_width = v; break; }
        case 'h': { int v = atoi(optarg); if (v > 0) window_height = v; break; }
        case 'x': { int v = atoi(optarg); if (v >= 0) window_x = v; break; }
        case 'y': { int v = atoi(optarg); if (v >= 0) window_y = v; break; }
        case 'k': { int v = atoi(optarg); if (v >= 0 && v < 4) bindKeyboard = v; break; }
        case 'm': mcp_mode = 1; break;
        case 'M': mcp_mode = 1; mcp_lsp = 1; break;
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
                "  -m, --mcp               run as an MCP server on stdio\n"
                "                            (newline-delimited JSON, accepts\n"
                "                             Content-Length input too)\n"
                "      --mcp-lsp           run as MCP server, reply with\n"
                "                             Content-Length framing\n"
                "  -?, --help              show this help\n\n"
                "Note: -h is height, not help. Use -? or --help for this text.\n",
                argv[0]);
            return (opt == '?') ? 0 : 1;
        }
    }

    if (mcp_mode) {
        if (mcp_lsp) mcp_set_framing(1);
        return mcp_run();
    }

    if (bindKeyboard >= 0 && texture_files[bindKeyboard]) {
        fprintf(stderr, "Error: cannot bind texture and keyboard to the same channel.\n");
        return 1;
    }
    if (bindKeyboard >= 0) {
        printf("Dynamic keyboard texture bound to iChannel%d.\n", bindKeyboard);
    }

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
    if (shader_path) {
        shader_list[0] = strdup(shader_path);
        shader_count = 1;
        shader_index = 0;
    } else {
        scan_shaders_dir("shaders");
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
            char *src = read_file(shader_list[i]);
            if (!src) {
                fprintf(stderr, "Could not read '%s', skipping.\n", shader_list[i]);
                continue;
            }
            GLuint test_prog = create_program(src);
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
    GLuint prog = load_shader_index(shader_index, &current_u);
    current_prog = prog;
    if (!prog) {
        fprintf(stderr, "Error: shader compilation failed.\n");
        SDL_GL_DeleteContext(g_gl_ctx);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    g_u = current_u;

    /* Copy explicit texture paths and keyboard binding to globals */
    for (int ch = 0; ch < 4; ch++)
        g_texture_files[ch] = texture_files[ch];
    g_bindKeyboard = bindKeyboard;

    load_channel_textures();

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

        /* FPS calculation */
        frame_count++;
        frame_number++;
        float dt = t - fps_timer;
        if (dt >= 0.5f) {
            current_fps = (float)frame_count / dt;
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

        /* Bind textures to iChannel0..3 */
        for (int ch = 0; ch < 4; ch++) {
            glActiveTexture(GL_TEXTURE0 + ch);
            if (g_textures[ch]) {
                glBindTexture(GL_TEXTURE_2D, g_textures[ch]);
            } else {
                static GLuint empty_tex = 0;
                if (!empty_tex) {
                    unsigned char black[4] = {0, 0, 0, 255};
                    glGenTextures(1, &empty_tex);
                    glBindTexture(GL_TEXTURE_2D, empty_tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, black);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                }
                glBindTexture(GL_TEXTURE_2D, empty_tex);
            }
        }

        /* Update keyboard texture if bound */
        update_keystate_texture();
        if (g_bindKeyboard >= 0 && g_bindKeyboard < 4) {
            glActiveTexture(GL_TEXTURE0 + g_bindKeyboard);
            if (keyStateTexID)
                glBindTexture(GL_TEXTURE_2D, keyStateTexID);
        }

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        SDL_GL_SwapWindow(g_window);
        partially_reset_keystate();
    }

    /* Cleanup */
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    for (int ch = 0; ch < 4; ch++)
        if (g_textures[ch]) glDeleteTextures(1, &g_textures[ch]);
    if (keyStateTexID)
        glDeleteTextures(1, &keyStateTexID);
    glDeleteProgram(prog);

    for (int i = 0; i < shader_count; i++)
        free(shader_list[i]);

    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
