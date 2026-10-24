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
#include "shader_common.h"
#include "shader_list.h"
#include "viewer_core.h"

/* ==================================================================
 * Backend state
 * ==================================================================
 *
 * Shader list, current-shader state, channel textures, keystate bookkeeping
 * and the MCP viewer hooks live in viewer_core.c, shared with main-sdl2.c.
 * What is left here is the window/input layer.
 */

static GLFWwindow *g_window = NULL;
static char  g_title_buf[512];

/* Window/framebuffer size, kept current by the resize callback. */
static float g_width = 640.0f, g_height = 480.0f;

#define shader_basename shader_list_basename

static void update_window_title(float fps)
{
    int count = vc_shader_count();
    const char *name = shader_basename(vc_shader_path(vc_shader_index()));
    if (count > 1)
        snprintf(g_title_buf, sizeof g_title_buf, "[%d/%d] %s  %.1f FPS",
                 vc_shader_index() + 1, count, name, fps);
    else
        snprintf(g_title_buf, sizeof g_title_buf, "%s  %.1f FPS", name, fps);
    if (g_window)
        glfwSetWindowTitle(g_window, g_title_buf);
}

/* Backend service: report window size for the MCP status line. */
static void get_window_size(int *w, int *h)
{
    *w = 0;
    *h = 0;
    if (g_window)
        glfwGetWindowSize(g_window, w, h);
}

static const vc_backend_services g_services = {
    update_window_title,
    get_window_size
};

/* Viewer-side compilation: the shared builder, with diagnostics on stderr. */
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

static void key_callback(GLFWwindow *w, int key, int scancode, int action, int mods)
{
    (void)scancode; (void)mods;
    if (action == GLFW_PRESS) {
        int js = (key >= 0 && key <= GLFW_KEY_LAST) ? keyLookup[key] : -1;
        if (js >= 0)
            vc_keystate_press(js);
        if (key == GLFW_KEY_Q || key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(w, 1);
        /* Shader navigation (switch_shader skips shaders that fail to compile) */
        if (key == GLFW_KEY_SPACE || key == GLFW_KEY_RIGHT || key == GLFW_KEY_PAGE_DOWN)
            vc_switch_shader(1);
        if (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_LEFT || key == GLFW_KEY_PAGE_UP)
            vc_switch_shader(-1);
    } else if (action == GLFW_RELEASE) {
        int js = (key >= 0 && key <= GLFW_KEY_LAST) ? keyLookup[key] : -1;
        if (js >= 0)
            vc_keystate_release(js);
    }
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
            vc_tex_cli[opt - '0'] = optarg; break;
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

    if (bindKeyboard >= 0 && vc_tex_cli[bindKeyboard]) {
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
    /* The core cannot set a window title or ask the window for its size, so
     * it calls back into this file for both. Register before any shader
     * loads, since loading a shader refreshes the title. */
    vc_set_backend_services(&g_services);

    vc_reset_keystate();

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
    shader_list_init(vc_shaders());
    if (shader_path) {
        if (shader_list_add(vc_shaders(), shader_path) != 0) {
            fprintf(stderr, "Error: out of memory adding shader path.\n");
            glfwDestroyWindow(g_window);
            glfwTerminate();
            return 1;
        }
        vc_set_shader_index(0);
        /* Be explicit: without the directory scan there is nothing to browse,
         * so the cycle keys do nothing. Say so rather than failing silently. */
        fprintf(stderr,
                "Note: -s loads a single shader, so arrow-key browsing is "
                "disabled.\n"
                "      Run without -s to browse everything in shaders/.\n");
    } else {
        shader_list_scan(vc_shaders(), "shaders");
        if (vc_shader_count() == 0) {
            fprintf(stderr, "Error: no .glsl files found in shaders/ and no -s flag.\n");
            shader_list_free(vc_shaders());
            glfwDestroyWindow(g_window);
            glfwTerminate();
            return 1;
        }
        printf("Found %d shader(s) in shaders/\n", vc_shader_count());

        /* Try shaders in order and start on the first one that compiles,
         * so one broken file doesn't kill the whole browser session. */
        int found = 0;
        for (int i = 0; i < vc_shader_count(); i++) {
            char *src = shader_read_file(vc_shader_path(i));
            if (!src) {
                fprintf(stderr, "Could not read '%s', skipping.\n", vc_shader_path(i));
                continue;
            }
            GLuint test_prog = vc_create_program(src, NULL);
            free(src);
            if (test_prog) {
                glDeleteProgram(test_prog);
                vc_set_shader_index(i);
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "Error: none of the %d shaders in shaders/ compiled.\n",
                    vc_shader_count());
            shader_list_free(vc_shaders());
            glfwDestroyWindow(g_window);
            glfwTerminate();
            return 1;
        }
    }

    /* Load initial shader */
    shader_uniforms init_u;
    unsigned long long init_hash = 0;
    GLuint prog = vc_load_shader_index(vc_shader_index(), &init_u, &init_hash);
    vc_set_current_program(prog);
    vc_set_current_uniforms(init_u);
    vc_set_source_hash(init_hash);
    vc_set_source_hash_valid(1);
    if (!prog) {
        fprintf(stderr, "Error: shader compilation failed.\n");
        shader_list_free(vc_shaders());
        glfwDestroyWindow(g_window);
        glfwTerminate();
        return 1;
    }

    vc_set_draw_uniforms(vc_current_uniforms());

    vc_set_bind_keyboard(bindKeyboard);

    vc_load_channel_textures();

    /* Architecture B: the viewer hosts the MCP server. A port conflict is
     * fatal so the configured client URL never talks to the wrong instance. */
    if (!mcp_viewer_start(mcp_port, vc_mcp_hooks())) {
        fprintf(stderr,
                "Could not start the MCP server on 127.0.0.1:%d.\n"
                "Is another viewer already running?\n"
                "See build/shadertoy-mcp.log.\n", mcp_port);
        shader_free_channels(vc_channel_textures());
        glDeleteProgram(prog);
        shader_list_free(vc_shaders());
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
                vc_rescan_shaders_preserving_current();
            }
        }

        /* Hot-reload by content, not mtime: Windows stat has one-second
         * resolution, so same-second rewrites used to be missed. */
        {
            static double last_poll = 0.0;
            if (t - last_poll >= 0.25) {
                last_poll = t;
                char *src = shader_read_file(vc_shader_path(vc_shader_index()));
                if (src) {
                    unsigned long long h = vc_hash_source(src);
                    if (!vc_source_hash_valid() ||
                        h != vc_source_hash()) {
                        vc_set_source_hash(h);
                        vc_set_source_hash_valid(1);
                        shader_uniforms new_u = vc_current_uniforms();
                        GLuint reloaded = vc_create_program(src, &new_u);
                        if (reloaded) {
                            if (vc_current_program() && vc_current_program() != reloaded)
                                glDeleteProgram(vc_current_program());
                            vc_set_current_program(reloaded);
                            vc_set_current_uniforms(new_u);
                            if (vc_reload_channels_pending()) {
                                vc_load_channel_textures();
                                vc_set_reload_channels_pending(0);
                            }
                            fprintf(stderr, "Reloaded shader: %s\n",
                                    vc_shader_path(vc_shader_index()));
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
            vc_set_display_fps(current_fps);
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
        if (vc_current_program() != prog) {
            prog = vc_current_program();
            vc_set_draw_uniforms(vc_current_uniforms());
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
        memcpy(f.channel_resolution, (*vc_channel_resolution()), sizeof f.channel_resolution);
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
        shader_uniforms draw_u = vc_draw_uniforms();
        shader_upload_uniforms(&draw_u, &f);

        /* Bind textures to iChannel0..3, then override the keyboard channel
         * if one is bound. Black fallbacks come from shader_common. */
        shader_bind_channels(vc_channel_textures());
        vc_update_keystate_texture();
        vc_bind_keystate_texture();

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        /* MCP jobs run here so window screenshots see this frame's pixels. */
        mcp_viewer_poll();

        glfwSwapBuffers(g_window);
        vc_partially_reset_keystate();
    }

    mcp_viewer_stop();

    /* Cleanup */
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    shader_free_channels(vc_channel_textures());
    vc_free_gl();
    glDeleteProgram(prog);

    shader_list_free(vc_shaders());

    glfwDestroyWindow(g_window);
    glfwTerminate();
    return 0;
}
