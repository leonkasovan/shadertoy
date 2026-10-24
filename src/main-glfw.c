/*
 * Shadertoy-style fullscreen fragment shader viewer.
 * Uses GLFW3 for windowing/input and GLAD for GL 3.3+ function loading.
 *
 * This is the alternative backend to main-sdl2.c. Both files are meant to
 * stay behavioural twins: same CLI, same shader list, same hot reload, same
 * MCP viewer hooks. Only the window/input layer differs.
 *
 * Build:  make            (SDL2 backend, default)
 *         make BACKEND=glfw   (this file; needs GLFW3 via pkg-config)
 */

#define _DEFAULT_SOURCE   /* strdup, M_PI, etc. */

#define GLFW_INCLUDE_NONE /* glad.h supplies the GL types/functions */
#include "glad.h"
#include <GLFW/glfw3.h>

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

static GLFWwindow *g_window = NULL;
static char  g_title_buf[512];
static float g_display_fps = 0.0f;

/* Window/framebuffer size, kept current by the resize callback. */
static float g_width = 640.0f, g_height = 480.0f;

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
    if (g_window)
        glfwSetWindowTitle(g_window, g_title_buf);
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
        glfwGetWindowSize(g_window, &w, &h);
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
 * Window callbacks
 * ================================================================== */

static void framebuffer_size_cb(GLFWwindow *w, int width, int height)
{
    (void)w;
    glViewport(0, 0, width, height);
    g_width = (float)width;
    g_height = (float)height;
}

static float g_mouseX = 0.0f, g_mouseY = 0.0f;
static float g_mouseL = 0.0f, g_mouseR = 0.0f;

static void cursor_cb(GLFWwindow *w, double xpos, double ypos)
{
    (void)w;
    g_mouseX = (float)xpos;
    g_mouseY = g_height - (float)ypos;
}

static void mouse_cb(GLFWwindow *w, int button, int action, int mods)
{
    (void)w; (void)mods;
    if (button == GLFW_MOUSE_BUTTON_LEFT)  g_mouseL = action ? 1.0f : 0.0f;
    if (button == GLFW_MOUSE_BUTTON_RIGHT) g_mouseR = action ? 1.0f : 0.0f;
}

/* ==================================================================
 * Keyboard state texture (Shadertoy-compatible)
 * ================================================================== */

static unsigned char keyStateData[256 * 3];
static GLuint keyStateTexID = 0;

/* Map GLFW key codes → Shadertoy / JS key codes.
 *
 * Letters are GLFW's own 65..90, which already match the uppercase ASCII
 * codes JavaScript reports for both cases of a letter. Punctuation uses the
 * JS keyCode table (minus 189, equal 187, semicolon 186, ...), not ASCII.
 * Keep this table in step with map_sdl_key() in main-sdl2.c. */
static int keyLookup[GLFW_KEY_LAST + 1];

static void populate_key_lookup(void)
{
    for (int i = 0; i <= GLFW_KEY_LAST; i++)
        keyLookup[i] = -1;
    for (int i = GLFW_KEY_0; i <= GLFW_KEY_9; i++)
        keyLookup[i] = i;
    for (int i = GLFW_KEY_A; i <= GLFW_KEY_Z; i++)
        keyLookup[i] = i;
    for (int i = GLFW_KEY_F1; i <= GLFW_KEY_F12; i++)
        keyLookup[i] = 112 + i - GLFW_KEY_F1;
    for (int i = GLFW_KEY_KP_0; i <= GLFW_KEY_KP_9; i++)
        keyLookup[i] = 96 + i - GLFW_KEY_KP_0;
    keyLookup[GLFW_KEY_SPACE]         = 32;
    keyLookup[GLFW_KEY_APOSTROPHE]    = 222;
    keyLookup[GLFW_KEY_COMMA]         = 188;
    keyLookup[GLFW_KEY_MINUS]         = 189;
    keyLookup[GLFW_KEY_PERIOD]        = 190;
    keyLookup[GLFW_KEY_SLASH]         = 191;
    keyLookup[GLFW_KEY_SEMICOLON]     = 186;
    keyLookup[GLFW_KEY_EQUAL]         = 187;
    keyLookup[GLFW_KEY_LEFT_BRACKET]  = 219;
    keyLookup[GLFW_KEY_BACKSLASH]     = 220;
    keyLookup[GLFW_KEY_RIGHT_BRACKET] = 221;
    keyLookup[GLFW_KEY_GRAVE_ACCENT]  = 192;
    keyLookup[GLFW_KEY_ESCAPE]        = 27;
    keyLookup[GLFW_KEY_ENTER]         = 13;
    keyLookup[GLFW_KEY_TAB]           = 9;
    keyLookup[GLFW_KEY_BACKSPACE]     = 8;
    keyLookup[GLFW_KEY_INSERT]        = 45;
    keyLookup[GLFW_KEY_DELETE]        = 46;
    keyLookup[GLFW_KEY_RIGHT]         = 39;
    keyLookup[GLFW_KEY_LEFT]          = 37;
    keyLookup[GLFW_KEY_DOWN]          = 40;
    keyLookup[GLFW_KEY_UP]            = 38;
    keyLookup[GLFW_KEY_PAGE_UP]       = 33;
    keyLookup[GLFW_KEY_PAGE_DOWN]     = 34;
    keyLookup[GLFW_KEY_HOME]          = 36;
    keyLookup[GLFW_KEY_END]           = 35;
    keyLookup[GLFW_KEY_CAPS_LOCK]     = 20;
    keyLookup[GLFW_KEY_SCROLL_LOCK]   = 145;
    keyLookup[GLFW_KEY_NUM_LOCK]      = 144;
    keyLookup[GLFW_KEY_PRINT_SCREEN]  = 42;
    keyLookup[GLFW_KEY_PAUSE]         = 19;
    keyLookup[GLFW_KEY_KP_DECIMAL]    = 110;
    keyLookup[GLFW_KEY_KP_DIVIDE]     = 111;
    keyLookup[GLFW_KEY_KP_MULTIPLY]   = 106;
    keyLookup[GLFW_KEY_KP_SUBTRACT]   = 109;
    keyLookup[GLFW_KEY_KP_ADD]        = 107;
    keyLookup[GLFW_KEY_KP_ENTER]      = 13;
    keyLookup[GLFW_KEY_LEFT_SHIFT]    = 16;
    keyLookup[GLFW_KEY_LEFT_CONTROL]  = 17;
    keyLookup[GLFW_KEY_LEFT_ALT]      = 18;
    keyLookup[GLFW_KEY_LEFT_SUPER]    = 91;
    keyLookup[GLFW_KEY_RIGHT_SHIFT]   = 16;
    keyLookup[GLFW_KEY_RIGHT_CONTROL] = 17;
    keyLookup[GLFW_KEY_RIGHT_ALT]     = 225;
    keyLookup[GLFW_KEY_RIGHT_SUPER]   = 91;
    keyLookup[GLFW_KEY_MENU]          = 93;
}

static void reset_keystate(void)
{
    memset(keyStateData, 0, sizeof(keyStateData));
}

static void partially_reset_keystate(void)
{
    /* Reset 'just pressed' (rising edge) flags */
    for (int i = 0; i < 256; i++)
        keyStateData[i + 256] = 0;
}

static void key_callback(GLFWwindow *w, int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    if (action == GLFW_PRESS) {
        int js = (key >= 0 && key <= GLFW_KEY_LAST) ? keyLookup[key] : -1;
        if (js >= 0) {
            keyStateData[js + 0]   = 255;  /* held */
            keyStateData[js + 256] = 255;  /* just pressed */
            keyStateData[js + 512] ^= 0xff; /* toggled */
        }
        if (key == GLFW_KEY_Q || key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(w, 1);
        /* Shader navigation (switch_shader skips shaders that fail to compile) */
        if (key == GLFW_KEY_SPACE || key == GLFW_KEY_RIGHT || key == GLFW_KEY_PAGE_DOWN)
            switch_shader(1);
        if (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_LEFT || key == GLFW_KEY_PAGE_UP)
            switch_shader(-1);
    } else if (action == GLFW_RELEASE) {
        int js = (key >= 0 && key <= GLFW_KEY_LAST) ? keyLookup[key] : -1;
        if (js >= 0) {
            keyStateData[js + 0]   = 0;
            keyStateData[js + 256] = 0;
        }
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
    const char *shader_path = NULL;
    int window_width  = 640;
    int window_height = 480;
    int window_x = -1;
    int window_y = -1;
    int fullscreen = 0;
    int bindKeyboard = -1;
    int mcp_port = 8642;

    /* parse command line options via getopt_long (from config.h) */
    int opt;
    int opt_idx = 0;
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

    /* Init GLFW */
    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    /* ------------------------------------------------------------------
     * Create OpenGL context — probe versions on ARM ES, fixed on desktop
     * ------------------------------------------------------------------ */
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

    GLFWmonitor *monitor = NULL;
    if (fullscreen) {
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        if (mode) {
            window_width  = mode->width;
            window_height = mode->height;
        }
    }

#ifdef USE_GLES
    /* On ARM/aarch64, probe ES 3.2 → 3.1 → 3.0 */
    {
        int es_versions[][2] = { {3,2}, {3,1}, {3,0} };
        int n_versions = sizeof(es_versions) / sizeof(es_versions[0]);
        int ctx_ok = 0;
        for (int i = 0; i < n_versions; i++) {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, es_versions[i][0]);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, es_versions[i][1]);

            g_window = glfwCreateWindow(window_width, window_height,
                                        "Shadertoy", monitor, NULL);
            if (g_window) {
                printf("OpenGL ES %d.%d context created\n",
                       es_versions[i][0], es_versions[i][1]);
                ctx_ok = 1;
                break;
            }
            /* Context failed — reset hints and try the next version */
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
        }
        if (!ctx_ok) {
            fprintf(stderr, "Failed to create any OpenGL ES context\n");
            glfwTerminate();
            return 1;
        }
    }
#else
    /* Desktop: try 3.3 core, then compat */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    g_window = glfwCreateWindow(window_width, window_height,
                                "Shadertoy", monitor, NULL);
    if (!g_window) {
        fprintf(stderr, "GL 3.3 core failed, trying compat...\n");
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
        glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
        g_window = glfwCreateWindow(window_width, window_height,
                                    "Shadertoy", monitor, NULL);
        if (!g_window) {
            fprintf(stderr, "Failed to create any OpenGL context\n");
            glfwTerminate();
            return 1;
        }
    }
#endif

    if (window_x >= 0 && window_y >= 0)
        glfwSetWindowPos(g_window, window_x, window_y);

    glfwMakeContextCurrent(g_window);
    glfwSwapInterval(1); /* vsync */

    glfwSetFramebufferSizeCallback(g_window, framebuffer_size_cb);
    glfwSetCursorPosCallback(g_window, cursor_cb);
    glfwSetMouseButtonCallback(g_window, mouse_cb);
    glfwSetKeyCallback(g_window, key_callback);
    populate_key_lookup();
    reset_keystate();

    /* Load GL functions via GLAD */
#ifdef USE_GLES
    if (!gladLoadGLES2Loader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "GLAD GLES init failed\n");
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }
#else
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "GLAD init failed\n");
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }
#endif

    printf("GL Vendor:   %s\n", glGetString(GL_VENDOR));
    printf("GL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL Version:  %s\n", glGetString(GL_VERSION));

    /* Initial viewport */
    {
        int fbw = window_width, fbh = window_height;
        glfwGetFramebufferSize(g_window, &fbw, &fbh);
        framebuffer_size_cb(g_window, fbw, fbh);
    }

    /* Shader list */
    shader_list_init(&g_shaders);
    if (shader_path) {
        if (shader_list_add(&g_shaders, shader_path) != 0) {
            fprintf(stderr, "Error: out of memory adding shader path.\n");
            glfwDestroyWindow(g_window);
            glfwTerminate();
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
            shader_list_free(&g_shaders);
            glfwDestroyWindow(g_window);
            glfwTerminate();
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
            shader_list_free(&g_shaders);
            glfwDestroyWindow(g_window);
            glfwTerminate();
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
        shader_list_free(&g_shaders);
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    g_u = current_u;

    g_bindKeyboard = bindKeyboard;

    load_channel_textures();

    /* Architecture B: the viewer hosts the MCP server. A port conflict is
     * fatal so the configured client URL never talks to the wrong instance. */
    if (!mcp_viewer_start(mcp_port, &g_mcp_viewer_hooks)) {
        fprintf(stderr,
                "Could not start the MCP server on 127.0.0.1:%d.\n"
                "Is another viewer already running?\n"
                "See build/shadertoy-mcp.log.\n", mcp_port);
        shader_free_channels(g_textures);
        glDeleteProgram(prog);
        shader_list_free(&g_shaders);
        glfwDestroyWindow(g_window);
        glfwTerminate();
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

    /* Timing */
    glfwSetTime(0.0);
    int frame_number = 0;
    int frame_count = 0;
    float fps_timer = 0.0f;
    float current_fps = 0.0f;
    float prev_t = 0.0f;   /* previous frame's time, for iTimeDelta */

    update_window_title(0.0f);

    /* Main loop */
    while (!glfwWindowShouldClose(g_window)) {
        glfwPollEvents();

        float t = (float)glfwGetTime();
        float lastFrameTime = t - prev_t;
        prev_t = t;

        /* Pick up shaders created by the MCP server without restarting. */
        {
            static double last_scan = 0.0;
            if (!shader_path && t - last_scan >= 1.0) {
                last_scan = t;
                rescan_shaders_preserving_current();
            }
        }

        /* Hot-reload by content, not mtime: Windows stat has one-second
         * resolution, so same-second rewrites used to be missed. */
        {
            static double last_poll = 0.0;
            if (t - last_poll >= 0.25) {
                last_poll = t;
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

        glfwSwapBuffers(g_window);
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

    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
