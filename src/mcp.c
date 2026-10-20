/*
 * MCP (Model Context Protocol) server for the Shadertoy SDL2 viewer.
 *
 * Standalone headless server over stdio (`--mcp`): JSON-RPC 2.0, default
 * transport is one JSON object per line (MCP stdio). Incoming LSP-style
 * "Content-Length: N" framing is also accepted, and --mcp-lsp switches
 * replies to that style. No window is created unless a GL tool
 * (compile/screenshot/run) needs one; even then it stays hidden.
 *
 * For a live preview run the interactive viewer separately on the same
 * shaders/ directory — it hot-reloads files written by this server.
 *
 * Uses libjq for JSON parsing and serialization.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#define mcp_getpid _getpid
typedef SOCKET mcp_socket_t;
#define MCP_INVALID_SOCKET INVALID_SOCKET
#define mcp_close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define mcp_getpid getpid
typedef int mcp_socket_t;
#define MCP_INVALID_SOCKET (-1)
#define mcp_close_socket close
#endif

#include <SDL2/SDL.h>

#include <jq.h>
#include <jv.h>

#include "mcp.h"
#include "shader_common.h"
#include "shader_list.h"

/* Windows opens stdio in text mode, which turns every \n into \r\n and
 * corrupts Content-Length framing (CR would be doubled). MCP/LSP framing
 * is byte-exact, so both streams must be binary. */
static void mcp_stdio_binary(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

#define MCP_PROTOCOLS_SUPPORTED "2024-11-05", "2025-03-26", "2025-06-18"
#define MCP_SERVER_NAME      "shadertoy-sdl2"
#define MCP_SERVER_VERSION   "2.0.0"
#define MCP_SERVER_TITLE     "Shadertoy SDL2 Renderer"
#define SHADER_DIR           "shaders"

/* Shader paths, grown dynamically via the shared scanner. */
static shader_list mcp_shaders;
static int mcp_shaders_scanned = 0;

/* Architecture B (viewer-hosted): the HTTP thread queues requests and the
 * viewer's main thread executes them with its own GL context current. */
static int mcp_viewer_mode = 0;
static const mcp_viewer_hooks *g_mcp_hooks = NULL;

static void mcp_ensure_scanned(void)
{
    if (!mcp_shaders_scanned) {
        shader_list_scan(&mcp_shaders, SHADER_DIR);
        mcp_shaders_scanned = 1;
    }
}

static const char *mcp_shader_path(int i)
{
    return mcp_shaders.items[i];
}

/* ------------------------------------------------------------------ */
/* stdio framing                                                       */
/*                                                                    */
/* One reader for both wire formats:                                  */
/*   1. Newline-delimited JSON — what MCP clients send. Lines are     */
/*      read with a growing buffer, so a large write_shader content   */
/*      payload is never truncated mid-message.                        */
/*   2. LSP-style Content-Length headers.                             */
/* Returns a malloc'd NUL-terminated body, or NULL on EOF/error.      */
/* ------------------------------------------------------------------ */

static char *mcp_read_line(size_t *out_len)
{
    size_t cap = 128, len = 0;
    char *buf = malloc(cap + 2);
    if (!buf) return NULL;
    int c = fgetc(stdin);
    if (c == EOF) { free(buf); return NULL; }
    while (c != '\n' && c != EOF) {
        if (len + 2 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
        c = fgetc(stdin);
    }
    if (len && buf[len - 1] == '\r') len--; /* strip CRLF */
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static char *mcp_read_message_body(void)
{
    for (;;) {
        size_t len = 0;
        char *line = mcp_read_line(&len);
        if (!line) return NULL; /* EOF between messages */

        if (len > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            long clen = strtol(line + 15, NULL, 10);
            free(line);
            if (clen < 0 || clen > (long)(256 * 1024 * 1024))
                return NULL;

            /* Consume remaining header lines up to the blank line. */
            for (;;) {
                size_t hl;
                char *h = mcp_read_line(&hl);
                if (!h) return NULL; /* EOF inside header block */
                int is_blank = (hl == 0);
                if (hl > 15 && strncasecmp(h, "Content-Length:", 15) == 0)
                    clen = strtol(h + 15, NULL, 10);
                free(h);
                if (is_blank) break;
            }

            char *body = malloc((size_t)clen + 1);
            if (!body) return NULL;
            size_t n = fread(body, 1, (size_t)clen, stdin);
            body[n] = '\0';
            return body;
        }

        if (len == 0) { free(line); continue; } /* blank separator */
        return line; /* the whole JSON line, uncapped */
    }
}

/* 0 = newline-delimited JSON (default, MCP stdio transport)
 * 1 = Content-Length framed (LSP-style clients) */
static int mcp_framing = 0;

/* Architecture B captures the reply instead of printing it. Only the main
 * thread touches these; the HTTP thread just receives the owned string. */
static int mcp_capture_enabled = 0;
static char *mcp_captured = NULL;

static void mcp_write_message(const char *json)
{
    if (mcp_capture_enabled) {
        free(mcp_captured);
        mcp_captured = malloc(strlen(json) + 1);
        if (mcp_captured)
            strcpy(mcp_captured, json);
        return;
    }
    if (mcp_framing)
        printf("Content-Length: %d\r\n\r\n%s", (int)strlen(json), json);
    else
        printf("%s\n", json);
    fflush(stdout);
}

int mcp_set_framing(int lsp)
{
    mcp_framing = lsp ? 1 : 0;
    return mcp_framing;
}

/* ------------------------------------------------------------------ */
/* JSON-RPC response helpers                                          */
/* ------------------------------------------------------------------ */

static void mcp_send_response(jv id, jv result)
{
    jv resp = jv_object();
    resp = jv_object_set(resp, jv_string("jsonrpc"), jv_string("2.0"));
    resp = jv_object_set(resp, jv_string("id"), id);
    resp = jv_object_set(resp, jv_string("result"), result);

    jv dumped = jv_dump_string(resp, 0);
    mcp_write_message(jv_string_value(dumped));
    jv_free(dumped);
}

static void mcp_send_error(jv id, int code, const char *message,
                           const char *data)
{
    jv err = jv_object();
    err = jv_object_set(err, jv_string("code"), jv_number(code));
    err = jv_object_set(err, jv_string("message"), jv_string(message));
    if (data)
        err = jv_object_set(err, jv_string("data"), jv_string(data));

    jv resp = jv_object();
    resp = jv_object_set(resp, jv_string("jsonrpc"), jv_string("2.0"));
    resp = jv_object_set(resp, jv_string("id"), id);
    resp = jv_object_set(resp, jv_string("error"), err);

    jv dumped = jv_dump_string(resp, 0);
    mcp_write_message(jv_string_value(dumped));
    jv_free(dumped);
}

/* ------------------------------------------------------------------ */
/* Tool result helpers                                                */
/* ------------------------------------------------------------------ */

static jv mcp_tool_text(const char *text)
{
    jv result = jv_object();
    jv content = jv_array();
    jv item = jv_object();
    item = jv_object_set(item, jv_string("type"), jv_string("text"));
    item = jv_object_set(item, jv_string("text"), jv_string(text));
    content = jv_array_append(content, item);
    result = jv_object_set(result, jv_string("content"), content);
    return result;
}

/* mcp_tool_text with a printf format (short single-chunk outputs). */
static jv tool_textf(const char *fmt, ...)
{
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return mcp_tool_text(buf);
}

static jv mcp_tool_error(const char *message)
{
    jv result = mcp_tool_text(message);
    result = jv_object_set(result, jv_string("isError"), jv_true());
    return result;
}

/* Attach structuredContent to a tool result. Consumes `structured`. */
static jv with_structured(jv result, jv structured)
{
    return jv_object_set(result, jv_string("structuredContent"), structured);
}

/* ------------------------------------------------------------------ */
/* Tool arguments                                                     */
/*                                                                    */
/* Typed accessors so tools skip the jv boilerplate. They consume a   */
/* copy of the looked-up value; callers must still free `params`.     */
/* ------------------------------------------------------------------ */

static int arg_int(jv params, const char *key, int def, int min, int max)
{
    jv v = jv_object_get(jv_copy(params), jv_string(key));
    if (jv_get_kind(v) == JV_KIND_NUMBER) {
        double n = jv_number_value(v);
        if (n > (double)max) n = max;
        if (n < (double)min) n = min;
        jv_free(v);
        return (int)n;
    }
    jv_free(v);
    return def;
}

static float arg_float(jv params, const char *key, float def)
{
    jv v = jv_object_get(jv_copy(params), jv_string(key));
    if (jv_get_kind(v) == JV_KIND_NUMBER) {
        float f = (float)jv_number_value(v);
        jv_free(v);
        return f;
    }
    jv_free(v);
    return def;
}

static int arg_bool(jv params, const char *key, int def)
{
    jv v = jv_object_get(jv_copy(params), jv_string(key));
    int b = def;
    if (jv_get_kind(v) == JV_KIND_TRUE)  b = 1;
    else if (jv_get_kind(v) == JV_KIND_FALSE) b = 0;
    jv_free(v);
    return b;
}

/* ------------------------------------------------------------------ */
/* Shader resolution: "name" (preferred) or "index"                    */
/*                                                                    */
/* Every shader tool accepts both, so agents never need a round trip  */
/* through list_shaders just to learn an index.                       */
/* ------------------------------------------------------------------ */

static int mcp_resolve_shader(jv params, char *errbuf, size_t errlen)
{
    mcp_ensure_scanned();

    jv name_jv = jv_object_get(jv_copy(params), jv_string("name"));
    if (jv_get_kind(name_jv) == JV_KIND_STRING) {
        char want[280];
        snprintf(want, sizeof want, "%s", jv_string_value(name_jv));
        jv_free(name_jv);

        /* Accept with or without the .glsl suffix. */
        size_t wl = strlen(want);
        if (!(wl > 5 && strcmp(want + wl - 5, ".glsl") == 0) &&
            wl + 5 < sizeof want) {
            memcpy(want + wl, ".glsl", 6);
        }

        for (int i = 0; i < mcp_shaders.count; i++)
            if (strcmp(shader_list_basename(mcp_shader_path(i)), want) == 0)
                return i;

        if (strlen(want) > 5)
            want[strlen(want) - 5] = '\0';
        snprintf(errbuf, errlen,
                 "No shader named '%s' (use list_shaders to see names)", want);
        return -1;
    }
    jv_free(name_jv);

    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) == JV_KIND_NUMBER) {
        int idx = (int)jv_number_value(idx_jv);
        jv_free(idx_jv);
        if (idx >= 0 && idx < mcp_shaders.count) return idx;
        snprintf(errbuf, errlen,
                 "Shader index %d out of range (0-%d)",
                 idx, mcp_shaders.count > 0 ? mcp_shaders.count - 1 : 0);
        return -1;
    }
    jv_free(idx_jv);

    snprintf(errbuf, errlen, "Provide a shader by 'name' or 'index'");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Base64 + PNG encoding                                              */
/* ------------------------------------------------------------------ */

static const char mcp_b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *mcp_base64_encode(const unsigned char *data, size_t len,
                               size_t *out_len)
{
    size_t b64_len = ((len + 2) / 3) * 4;
    char *out = malloc(b64_len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = ((unsigned)data[i]) << 16;
        if (i + 1 < len) v |= ((unsigned)data[i + 1]) << 8;
        if (i + 2 < len) v |= ((unsigned)data[i + 2]);
        out[j++] = mcp_b64_chars[(v >> 18) & 0x3F];
        out[j++] = mcp_b64_chars[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? mcp_b64_chars[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? mcp_b64_chars[v & 0x3F] : '=';
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

static void mcp_put32be(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

/* CRC-32 (IEEE) used by PNG chunks. */
static uint32_t mcp_crc32(const unsigned char *data, size_t len)
{
    static uint32_t table[256];
    static int table_ready = 0;
    if (!table_ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        table_ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* Write a PNG chunk (length, type, payload, crc) at png+p; returns new p. */
static size_t mcp_put_chunk(unsigned char *png, size_t p, const char *type,
                            const unsigned char *data, size_t len)
{
    mcp_put32be(png + p, (uint32_t)len);
    memcpy(png + p + 4, type, 4);
    if (len && data)
        memcpy(png + p + 8, data, len);
    uint32_t crc = mcp_crc32(png + p + 4, len + 4);
    mcp_put32be(png + p + 8 + len, crc);
    return p + 12 + len;
}

/* Encode RGBA pixels as a PNG (truecolor + alpha, filter type 0 per row).
 * One IDAT chunk with zlib "stored" blocks: no deflate compression,
 * and the result is still a valid PNG. */
static char *mcp_encode_png(int width, int height,
                            const unsigned char *rgba, size_t *out_size)
{
    static const unsigned char sig[8] =
        { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    size_t stride = (size_t)width * 4;
    size_t raw_len = (stride + 1) * (size_t)height;
    unsigned char *raw = malloc(raw_len);
    if (!raw) return NULL;
    for (int y = 0; y < height; y++) {
        unsigned char *row = raw + (stride + 1) * (size_t)y;
        row[0] = 0; /* filter: none */
        memcpy(row + 1, rgba + stride * (size_t)y, stride);
    }

    size_t max_block = 65535;
    size_t nblocks = (raw_len + max_block - 1) / max_block;
    if (nblocks == 0) nblocks = 1;
    size_t zlen = 2 /* zlib hdr */ + raw_len + nblocks * 5 + 4 /* adler */;
    unsigned char *z = malloc(zlen);
    if (!z) { free(raw); return NULL; }

    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01; /* zlib header, no preset dict */
    size_t off = 0;
    while (off < raw_len) {
        size_t n = raw_len - off;
        if (n > max_block) n = max_block;
        z[zi++] = (unsigned char)(off + n >= raw_len ? 1 : 0); /* BFINAL */
        z[zi++] = (unsigned char)(n & 0xFF);
        z[zi++] = (unsigned char)((n >> 8) & 0xFF);
        z[zi++] = (unsigned char)(~n & 0xFF);
        z[zi++] = (unsigned char)((~n >> 8) & 0xFF);
        memcpy(z + zi, raw + off, n);
        zi += n;
        off += n;
    }
    {   /* adler32 of the uncompressed data */
        uint32_t a = 1, b = 0;
        for (size_t i = 0; i < raw_len; i++) {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        uint32_t adler = (b << 16) | a;
        z[zi++] = (unsigned char)(adler >> 24);
        z[zi++] = (unsigned char)(adler >> 16);
        z[zi++] = (unsigned char)(adler >> 8);
        z[zi++] = (unsigned char)(adler);
    }
    free(raw);

    /* signature(8) + IHDR(12+13) + IDAT(12+zlen) + IEND(12) */
    size_t total = 8 + 25 + 12 + zi + 12;
    unsigned char *png = malloc(total);
    if (!png) { free(z); return NULL; }

    size_t p = 0;
    memcpy(png + p, sig, 8); p += 8;

    unsigned char ihdr[13];
    mcp_put32be(ihdr + 0, (uint32_t)width);
    mcp_put32be(ihdr + 4, (uint32_t)height);
    ihdr[8]  = 8;  /* bit depth */
    ihdr[9]  = 6;  /* colour type: RGBA */
    ihdr[10] = 0;  /* compression */
    ihdr[11] = 0;  /* filter */
    ihdr[12] = 0;  /* interlace */
    p = mcp_put_chunk(png, p, "IHDR", ihdr, 13);
    p = mcp_put_chunk(png, p, "IDAT", z, zi);
    p = mcp_put_chunk(png, p, "IEND", NULL, 0);
    free(z);

    *out_size = p;
    return (char *)png;
}

/* ------------------------------------------------------------------ */
/* Hidden GL context for headless compile/render                       */
/* ------------------------------------------------------------------ */

#include "glad.h"

static SDL_Window   *mcp_gl_window  = NULL;
static SDL_GLContext mcp_gl_context = NULL;
static int            mcp_gl_ready   = 0;

static int mcp_gl_init(void)
{
    /* Viewer-hosted: the caller already runs on the main thread with the
     * viewer's context current. Never create a hidden window here. */
    if (mcp_viewer_mode) return 1;
    if (mcp_gl_ready) return 1;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0)
            return 0;
    }

    /* Set attributes BEFORE the window is created — SDL ignores them
     * afterwards. Probe desktop core 3.3 first, mirroring the viewer. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
#ifdef USE_GLES
    static const struct { int profile, major, minor; } tries[] = {
        { SDL_GL_CONTEXT_PROFILE_ES, 3, 2 },
        { SDL_GL_CONTEXT_PROFILE_ES, 3, 1 },
        { SDL_GL_CONTEXT_PROFILE_ES, 3, 0 },
    };
#else
    static const struct { int profile, major, minor; } tries[] = {
        { SDL_GL_CONTEXT_PROFILE_CORE,          3, 3 },
        { SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 3, 3 },
    };
#endif

    SDL_Window *window = NULL;
    SDL_GLContext ctx = NULL;
    for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, tries[i].profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, tries[i].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, tries[i].minor);
        window = SDL_CreateWindow("shadertoy-mcp",
            0, 0, 1, 1, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
        if (!window) continue;
        ctx = SDL_GL_CreateContext(window);
        if (ctx) break;
        SDL_DestroyWindow(window);
        window = NULL;
    }
    if (!ctx) {
        SDL_Quit();
        return 0;
    }
    mcp_gl_window = window;
    mcp_gl_context = ctx;

#ifdef USE_GLES
    if (!gladLoadGLES2Loader((GLADloadproc)SDL_GL_GetProcAddress)) {
#else
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
#endif
        SDL_GL_DeleteContext(mcp_gl_context);
        SDL_DestroyWindow(mcp_gl_window);
        mcp_gl_window = NULL;
        mcp_gl_context = NULL;
        SDL_Quit();
        return 0;
    }

    mcp_gl_ready = 1;
    return 1;
}

static void mcp_gl_cleanup(void)
{
    if (mcp_viewer_mode)
        return;
    if (!mcp_gl_ready)
        return;
    if (mcp_gl_context)
        SDL_GL_DeleteContext(mcp_gl_context);
    if (mcp_gl_window)
        SDL_DestroyWindow(mcp_gl_window);
    mcp_gl_window = NULL;
    mcp_gl_context = NULL;
    mcp_gl_ready = 0;
    SDL_Quit();
}

/* ------------------------------------------------------------------ */
/* Rendering: one bundle shared by the render-based tools              */
/* ------------------------------------------------------------------ */

struct render_ctx {
    GLuint prog, tex, fbo, vao, vbo;
    GLuint chan_tex[4];
    float  chan_res[4][3];
    int    width, height;
};

static void render_ctx_free(struct render_ctx *r);

/* Compile the shader at `idx`, build an FBO of width x height, a quad,
 * and channel textures. On failure fills `errbuf` (tool-error text) and
 * returns 0 with the context zeroed. */
static int render_ctx_create(struct render_ctx *r, int idx,
                             int width, int height,
                             char *errbuf, size_t errlen)
{
    memset(r, 0, sizeof *r);
    r->width = width;
    r->height = height;

    char logbuf[4096] = {0};
    char *src = shader_read_file(mcp_shader_path(idx));
    if (!src) {
        snprintf(errbuf, errlen, "Could not read shader file.");
        return 0;
    }
    r->prog = shader_build_program(src, logbuf, sizeof logbuf);
    free(src);
    if (!r->prog) {
        snprintf(errbuf, errlen,
                 "Shader compilation failed:\n%s",
                 logbuf[0] ? logbuf : "(no log available)");
        render_ctx_free(r);
        return 0;
    }

    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, r->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(errbuf, errlen, "Failed to create framebuffer object.");
        render_ctx_free(r);
        return 0;
    }

    static const float quad[] = { -1, -1,  1, -1,  -1, 1,  1, 1 };
    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);

    shader_load_channels(mcp_shader_path(idx), NULL, r->chan_tex, r->chan_res);
    shader_bind_channels(r->chan_tex);
    return 1;
}

static void render_ctx_free(struct render_ctx *r)
{
    shader_free_channels(r->chan_tex);
    if (r->vbo)  glDeleteBuffers(1, &r->vbo);
    if (r->vao)  glDeleteVertexArrays(1, &r->vao);
    if (r->tex)  glDeleteTextures(1, &r->tex);
    if (r->fbo)  glDeleteFramebuffers(1, &r->fbo);
    if (r->prog) glDeleteProgram(r->prog);
    memset(r, 0, sizeof *r);
}

/* Draw one frame; leaves the FBO bound for glReadPixels. */
static void render_draw_frame(struct render_ctx *r, float time_val, int frame)
{
    shader_uniforms u;
    shader_uniforms_lookup(r->prog, &u);

    shader_frame f;
    shader_frame_defaults(&f);
    f.width  = (float)r->width;
    f.height = (float)r->height;
    f.time   = time_val;
    f.frame  = frame;
    memcpy(f.channel_resolution, r->chan_res, sizeof f.channel_resolution);
    for (int ch = 0; ch < 4; ch++)
        f.channel_time[ch] = time_val;

    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glViewport(0, 0, r->width, r->height);
    glUseProgram(r->prog);
    shader_upload_uniforms(&u, &f);
    glBindVertexArray(r->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
}

/* Read w x h RGBA8 from the currently bound framebuffer, with GL's
 * bottom-up rows flipped to top-down. Returns NULL on allocation failure. */
static unsigned char *read_rgba_pixels(int w, int h)
{
    size_t row = (size_t)w * 4;
    unsigned char *pixels = malloc((size_t)w * h * 4);
    if (!pixels) return NULL;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    /* Many shaders output alpha 0 (e.g. fragColor = vec4(col, 0.0)); that
     * would make every screenshot a transparent PNG. Screenshots are for
     * viewing, so the alpha channel is forced opaque. */
    for (size_t p = 0; p < (size_t)w * h; p++)
        pixels[p * 4 + 3] = 255;
    /* glReadPixels row 0 = GL bottom; swap vertically in place */
    unsigned char *tmp = malloc(row);
    if (!tmp) { free(pixels); return NULL; }
    for (int y = 0; y < h / 2; y++) {
        memcpy(tmp, pixels + (size_t)y * row, row);
        memcpy(pixels + (size_t)y * row, pixels + (size_t)(h - 1 - y) * row, row);
        memcpy(pixels + (size_t)(h - 1 - y) * row, tmp, row);
    }
    free(tmp);
    return pixels;
}

static unsigned char *render_read_pixels(struct render_ctx *r)
{
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    return read_rgba_pixels(r->width, r->height);
}

/* Decode the current FBO to a base64 PNG string; NULL on failure. */
static char *render_png_b64(struct render_ctx *r)
{
    unsigned char *pixels = render_read_pixels(r);
    if (!pixels) return NULL;
    size_t png_size;
    char *png_data = mcp_encode_png(r->width, r->height, pixels, &png_size);
    free(pixels);
    if (!png_data) return NULL;
    char *b64 = mcp_base64_encode((unsigned char *)png_data, png_size, NULL);
    free(png_data);
    return b64;
}

/* An MCP image content item (owned jv) for a base64 PNG. */
static jv image_content_item(const char *b64)
{
    jv item = jv_object();
    item = jv_object_set(item, jv_string("type"), jv_string("image"));
    item = jv_object_set(item, jv_string("data"), jv_string(b64));
    item = jv_object_set(item, jv_string("mimeType"), jv_string("image/png"));
    return item;
}

/* ------------------------------------------------------------------ */
/* Tool implementations                                               */
/* ------------------------------------------------------------------ */

static jv tool_list_shaders(jv params)
{
    (void)params;
    mcp_ensure_scanned();

    jv text = jv_string_fmt("Found %d shader(s):\n", mcp_shaders.count);
    for (int i = 0; i < mcp_shaders.count; i++) {
        text = jv_string_fmt("%s%d: %s\n", jv_string_value(text), i,
                             shader_list_basename(mcp_shader_path(i)));
    }
    jv result = mcp_tool_text(jv_string_value(text));
    jv_free(text);

    jv items = jv_array();
    for (int i = 0; i < mcp_shaders.count; i++) {
        jv it = jv_object();
        it = jv_object_set(it, jv_string("index"), jv_number(i));
        it = jv_object_set(it, jv_string("name"),
                           jv_string(shader_list_basename(mcp_shader_path(i))));
        it = jv_object_set(it, jv_string("path"), jv_string(mcp_shader_path(i)));
        items = jv_array_append(items, it);
    }
    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("count"),
                               jv_number(mcp_shaders.count));
    structured = jv_object_set(structured, jv_string("items"), items);
    result = with_structured(result, structured);
    return result;
}

static jv tool_read_shader(jv params)
{
    char err[256];
    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    char *src = shader_read_file(mcp_shader_path(idx));
    if (!src)
        return mcp_tool_error("Could not read shader file");

    jv result = mcp_tool_text(src);
    free(src);
    return result;
}

static jv tool_get_shader_info(jv params)
{
    char err[256];
    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    char *src = shader_read_file(mcp_shader_path(idx));
    if (!src)
        return mcp_tool_error("Could not read shader file");

    int lines = 1;
    for (char *p = src; *p; p++)
        if (*p == '\n') lines++;

    const char *path = mcp_shader_path(idx);
    const char *name = shader_list_basename(path);
    /* jv_string_fmt uses the CRT printf, which does not support %zu. */
    jv text = jv_string_fmt(
        "Index: %d\nName: %s\nPath: %s\nLines: %d\nSize: %d bytes",
        idx, name, path, lines, (int)strlen(src));
    jv result = mcp_tool_text(jv_string_value(text));
    jv_free(text);

    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("index"), jv_number(idx));
    structured = jv_object_set(structured, jv_string("name"), jv_string(name));
    structured = jv_object_set(structured, jv_string("path"), jv_string(path));
    structured = jv_object_set(structured, jv_string("lineCount"), jv_number(lines));
    structured = jv_object_set(structured, jv_string("byteSize"),
                               jv_number((double)strlen(src)));
    free(src);
    result = with_structured(result, structured);
    return result;
}

static jv tool_compile_shader(jv params)
{
    char err[256];
    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    if (!mcp_gl_init())
        return mcp_tool_error(
            "Could not create OpenGL context for shader compilation "
            "(no GPU/display available).");

    char logbuf[4096] = {0};
    char *src = shader_read_file(mcp_shader_path(idx));
    if (!src)
        return mcp_tool_error("Could not read shader file");

    GLuint prog = shader_build_program(src, logbuf, sizeof logbuf);
    free(src);
    if (prog) glDeleteProgram(prog);

    const char *name = shader_list_basename(mcp_shader_path(idx));
    if (prog)
        return tool_textf("Shader '%s' compiles OK.\n", name);

    jv text = jv_string_fmt("Shader: '%s'\nCompilation: FAILED\n\n%s",
                            name, logbuf[0] ? logbuf : "(no log available)");
    jv result = mcp_tool_error(jv_string_value(text));
    jv_free(text);
    return result;
}

static jv tool_get_fps(jv params)
{
    char err[4096 + 64];
    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    if (!mcp_gl_init())
        return mcp_tool_error("Could not create OpenGL context for rendering.");

    int frames = arg_int(params, "frames", 60, 1, 1000);
    int width  = arg_int(params, "width",  640, 1, 4096);
    int height = arg_int(params, "height", 480, 1, 4096);

    struct render_ctx r;
    if (!render_ctx_create(&r, idx, width, height, err, sizeof err))
        return mcp_tool_error(err);

    Uint32 t0 = SDL_GetTicks();
    for (int i = 0; i < frames; i++)
        render_draw_frame(&r, (float)i * 0.016f, i);
    Uint32 t1 = SDL_GetTicks();
    double elapsed = (double)(t1 - t0) / 1000.0;
    double fps = elapsed > 0.0 ? (double)frames / elapsed : 0.0;

    render_ctx_free(&r);

    jv text = jv_string_fmt(
        "FPS benchmark for shader: %s\n"
        "Frames rendered: %d\n"
        "Resolution: %dx%d\n"
        "Elapsed: %.3f seconds\n"
        "Average FPS: %.1f\n",
        shader_list_basename(mcp_shader_path(idx)),
        frames, width, height, elapsed, fps);
    jv result = mcp_tool_text(jv_string_value(text));
    jv_free(text);

    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("fps"), jv_number(fps));
    structured = jv_object_set(structured, jv_string("frames"), jv_number(frames));
    structured = jv_object_set(structured, jv_string("elapsed"), jv_number(elapsed));
    structured = jv_object_set(structured, jv_string("width"), jv_number(width));
    structured = jv_object_set(structured, jv_string("height"), jv_number(height));
    result = with_structured(result, structured);
    return result;
}

static jv tool_screenshot(jv params)
{
    char err[4096 + 64];

    /* Viewer mode can grab the backbuffer of the frame the main loop just
     * drew. That includes mouse/keyboard/date state; the offscreen path
     * cannot. Only available in the viewer-hosted server. */
    if (arg_bool(params, "window", 0)) {
        if (!mcp_viewer_mode)
            return mcp_tool_error(
                "window capture requires the viewer-hosted MCP server "
                "(start the viewer instead of --mcp).");

        GLint vp[4] = {0};
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGetIntegerv(GL_VIEWPORT, vp);
        int width = vp[2], height = vp[3];
        if (width <= 0 || height <= 0)
            return mcp_tool_error("Viewer window has no drawable area.");

        unsigned char *pixels = read_rgba_pixels(width, height);
        if (!pixels)
            return mcp_tool_error("Could not read window pixels.");
        size_t png_size = 0;
        char *png = mcp_encode_png(width, height, pixels, &png_size);
        free(pixels);
        if (!png)
            return mcp_tool_error("Failed to encode screenshot as PNG.");
        char *b64 = mcp_base64_encode((unsigned char *)png, png_size, NULL);
        free(png);
        if (!b64)
            return mcp_tool_error("Failed to encode screenshot as PNG.");

        jv result = jv_object();
        jv content = jv_array();
        content = jv_array_append(content, image_content_item(b64));
        free(b64);
        result = jv_object_set(result, jv_string("content"), content);

        jv structured = jv_object();
        structured = jv_object_set(structured, jv_string("width"),
                                   jv_number(width));
        structured = jv_object_set(structured, jv_string("height"),
                                   jv_number(height));
        structured = jv_object_set(structured, jv_string("window"), jv_true());
        result = with_structured(result, structured);
        return result;
    }

    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    if (!mcp_gl_init())
        return mcp_tool_error("Could not create OpenGL context for rendering.");

    int width  = arg_int(params, "width",  640, 1, 2048);
    int height = arg_int(params, "height", 480, 1, 2048);
    float time_val = arg_float(params, "time", 0.0f);

    struct render_ctx r;
    if (!render_ctx_create(&r, idx, width, height, err, sizeof err))
        return mcp_tool_error(err);

    render_draw_frame(&r, time_val, 0);
    char *b64 = render_png_b64(&r);
    render_ctx_free(&r);
    if (!b64)
        return mcp_tool_error("Failed to encode screenshot as PNG.");

    jv result = jv_object();
    jv content = jv_array();
    content = jv_array_append(content, image_content_item(b64));
    free(b64);
    result = jv_object_set(result, jv_string("content"), content);

    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("width"), jv_number(width));
    structured = jv_object_set(structured, jv_string("height"), jv_number(height));
    structured = jv_object_set(structured, jv_string("time"), jv_number(time_val));
    structured = jv_object_set(structured, jv_string("format"),
                               jv_string("png (8-bit RGBA)"));
    result = with_structured(result, structured);
    return result;
}

static jv tool_run_shaders(jv params)
{
    char err[4096 + 64];
    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    if (!mcp_gl_init())
        return mcp_tool_error("Could not create OpenGL context for rendering.");

    int frames  = arg_int(params, "frames", 60, 1, 1000);
    int width   = arg_int(params, "width",  320, 1, 4096);
    int height  = arg_int(params, "height", 240, 1, 4096);
    double duration = (double)arg_float(params, "duration", 1.0f);
    if (duration < 0.01) duration = 0.01;
    float dt = (float)(duration / (double)frames);
    int capture = arg_bool(params, "captureFrame", 0);

    struct render_ctx r;
    if (!render_ctx_create(&r, idx, width, height, err, sizeof err))
        return mcp_tool_error(err);

    Uint32 t0 = SDL_GetTicks();
    for (int i = 0; i < frames; i++)
        render_draw_frame(&r, (float)i * dt, i);
    Uint32 t1 = SDL_GetTicks();
    double elapsed = (double)(t1 - t0) / 1000.0;
    double fps = elapsed > 0.0 ? (double)frames / elapsed : 0.0;
    float end_time = (float)(frames - 1) * dt;

    jv text = jv_string_fmt(
        "Shader: %s\nIndex: %d\n"
        "Frames rendered: %d\n"
        "Duration: %.3f s\n"
        "Elapsed: %.3f seconds\n"
        "Average FPS: %.1f\n"
        "Captured at t=%.3f s\n",
        shader_list_basename(mcp_shader_path(idx)), idx,
        frames, duration, elapsed, fps, end_time);

    jv content = jv_array();
    jv text_item = jv_object();
    text_item = jv_object_set(text_item, jv_string("type"), jv_string("text"));
    text_item = jv_object_set(text_item, jv_string("text"),
                              jv_string(jv_string_value(text)));
    jv_free(text);
    content = jv_array_append(content, text_item);

    if (capture) {
        char *b64 = render_png_b64(&r);
        if (b64) {
            content = jv_array_append(content, image_content_item(b64));
            free(b64);
        } else {
            jv fail_item = jv_object();
            fail_item = jv_object_set(fail_item, jv_string("type"),
                                      jv_string("text"));
            fail_item = jv_object_set(fail_item, jv_string("text"),
                                      jv_string("Frame capture failed."));
            content = jv_array_append(content, fail_item);
        }
    }
    render_ctx_free(&r);

    jv result = jv_object();
    result = jv_object_set(result, jv_string("content"), content);

    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("fps"), jv_number(fps));
    structured = jv_object_set(structured, jv_string("frames"), jv_number(frames));
    structured = jv_object_set(structured, jv_string("elapsed"), jv_number(elapsed));
    structured = jv_object_set(structured, jv_string("duration"), jv_number(duration));
    structured = jv_object_set(structured, jv_string("width"), jv_number(width));
    structured = jv_object_set(structured, jv_string("height"), jv_number(height));
    structured = jv_object_set(structured, jv_string("time"), jv_number(end_time));
    result = with_structured(result, structured);
    return result;
}

/* Shared write path: validates the filename, stores the file and — when a
 * GL context is available — compiles it, returning the compiler log in the
 * same response. One round trip is the whole point of the tool. */
static jv tool_write_shader(jv params)
{
    jv name_jv = jv_object_get(jv_copy(params), jv_string("name"));
    if (jv_get_kind(name_jv) != JV_KIND_STRING) {
        jv_free(name_jv);
        return mcp_tool_error("Missing required argument: name (string)");
    }
    char safe_name[256];
    snprintf(safe_name, sizeof safe_name, "%s", jv_string_value(name_jv));
    jv_free(name_jv);

    /* Reject anything that is not a plain filename inside shaders/: a name
     * like "../../evil" would otherwise escape the shader directory. */
    if (safe_name[0] == '\0' ||
        strstr(safe_name, "..") ||
        strchr(safe_name, '/') || strchr(safe_name, '\\') ||
        strchr(safe_name, ':')) {
        return mcp_tool_error(
            "Invalid shader name: must be a plain filename without path "
            "separators, '..', or drive/URI prefixes (e.g. \"plasma\").");
    }

    jv content_jv = jv_object_get(jv_copy(params), jv_string("content"));
    if (jv_get_kind(content_jv) != JV_KIND_STRING) {
        jv_free(content_jv);
        return mcp_tool_error("Missing required argument: content (string)");
    }
    const char *content = jv_string_value(content_jv);
    size_t content_len = strlen(content);

    char filepath[320];
    const char *ext = strrchr(safe_name, '.');
    if (ext && strcmp(ext, ".glsl") == 0)
        snprintf(filepath, sizeof(filepath), SHADER_DIR "/%s", safe_name);
    else
        snprintf(filepath, sizeof(filepath), SHADER_DIR "/%s.glsl", safe_name);

#ifdef _WIN32
    mkdir(SHADER_DIR);
#else
    mkdir(SHADER_DIR, 0755);
#endif

    /* Write a sibling temp file and replace atomically: the viewer polls
     * this path and must never compile a half-written shader. */
    char tmppath[sizeof filepath + 8];
    snprintf(tmppath, sizeof tmppath, "%s.tmp", filepath);

    FILE *f = fopen(tmppath, "wb");
    if (!f) {
        jv_free(content_jv);
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Could not write shader file '%s' (check permissions).",
                 filepath);
        return mcp_tool_error(buf);
    }
    int write_failed = fwrite(content, 1, content_len, f) != content_len;
    if (fclose(f) != 0)
        write_failed = 1;
    jv_free(content_jv);

    if (write_failed) {
        remove(tmppath);
        char buf[512];
        snprintf(buf, sizeof(buf), "Could not write shader file '%s'.",
                 filepath);
        return mcp_tool_error(buf);
    }

#ifdef _WIN32
    if (!MoveFileExA(tmppath, filepath, MOVEFILE_REPLACE_EXISTING)) {
#else
    if (rename(tmppath, filepath) != 0) {
#endif
        remove(tmppath);
        char buf[512];
        snprintf(buf, sizeof(buf), "Could not replace shader file '%s'.",
                 filepath);
        return mcp_tool_error(buf);
    }

    shader_list_scan(&mcp_shaders, SHADER_DIR);

    /* Locate the fresh entry. */
    int new_idx = -1;
    for (int i = 0; i < mcp_shaders.count; i++) {
        if (strcmp(shader_list_basename(mcp_shader_path(i)),
                   shader_list_basename(filepath)) == 0) {
            new_idx = i;
            break;
        }
    }

    /* Compile check in the same call: this is what makes the edit loop
     * one round trip for the agent. Failure is the expected (useful)
     * outcome while iterating; isError marks it so clients can show it. */
    char logbuf[4096] = {0};
    int gl_ok = 0, compiled = 0;
    char *src = shader_read_file(filepath);
    if (src && mcp_gl_init()) {
        gl_ok = 1;
        GLuint prog = shader_build_program(src, logbuf, sizeof logbuf);
        if (prog) {
            glDeleteProgram(prog);
            compiled = 1;
        }
    }
    free(src);

    jv text = jv_string_fmt(
        "Shader written to: %s\n"
        "Index: %d\n"
        "Size: %d bytes\n"
        "Total shaders: %d\n",
        filepath, new_idx, (int)content_len, mcp_shaders.count);
    if (!gl_ok) {
        text = jv_string_fmt(
            "%sCompile check: skipped (no GL context available).",
            jv_string_value(text));
        compiled = 1; /* treat as success; nothing to report */
    } else if (compiled) {
        text = jv_string_fmt("%sCompile check: OK", jv_string_value(text));
    } else {
        text = jv_string_fmt(
            "%s\nCompile FAILED:\n%s",
            jv_string_value(text),
            logbuf[0] ? logbuf : "(no log available)");
    }
    jv result = mcp_tool_text(jv_string_value(text));
    jv_free(text);
    if (!compiled)
        result = jv_object_set(result, jv_string("isError"), jv_true());

    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("path"), jv_string(filepath));
    structured = jv_object_set(structured, jv_string("index"), jv_number(new_idx));
    structured = jv_object_set(structured, jv_string("size"),
                               jv_number((double)content_len));
    result = with_structured(result, structured);
    return result;
}

/* Update an existing shader by name/index; otherwise identical to write. */
static jv tool_update_shader(jv params)
{
    char err[256];
    int idx = mcp_resolve_shader(params, err, sizeof err);
    if (idx < 0) return mcp_tool_error(err);

    jv content = jv_object_get(jv_copy(params), jv_string("content"));
    if (jv_get_kind(content) != JV_KIND_STRING) {
        jv_free(content);
        return mcp_tool_error("Missing required argument: content (string)");
    }

    jv wparams = jv_object();
    wparams = jv_object_set(wparams, jv_string("name"),
                            jv_string(shader_list_basename(mcp_shader_path(idx))));
    wparams = jv_object_set(wparams, jv_string("content"), content);
    jv res = tool_write_shader(wparams);
    jv_free(wparams);
    return res;
}

static jv tool_reload_shaders(jv params)
{
    (void)params;
    shader_list_scan(&mcp_shaders, SHADER_DIR);
    return tool_textf("Rescanned %s directory. Found %d shader(s).",
                      SHADER_DIR, mcp_shaders.count);
}

/* Switch the visible shader. Only the viewer-hosted server can do this. */
static jv tool_load_shader(jv params)
{
    if (!mcp_viewer_mode || !g_mcp_hooks || !g_mcp_hooks->load_shader)
        return mcp_tool_error(
            "load_shader requires the viewer-hosted MCP server "
            "(start the viewer instead of --mcp).");

    char name[280] = {0};
    jv name_jv = jv_object_get(jv_copy(params), jv_string("name"));
    if (jv_get_kind(name_jv) == JV_KIND_STRING)
        snprintf(name, sizeof name, "%s", jv_string_value(name_jv));
    jv_free(name_jv);

    int index = arg_int(params, "index", -1, 0, 1000000);
    if (!name[0] && index < 0)
        return mcp_tool_error("Provide a shader by 'name' or 'index'");

    char err[512] = {0};
    if (!g_mcp_hooks->load_shader(name[0] ? name : NULL, index,
                                  err, sizeof err))
        return mcp_tool_error(err[0] ? err : "Could not load shader");

    char status[1024] = {0};
    if (g_mcp_hooks->status)
        g_mcp_hooks->status(status, sizeof status);
    return mcp_tool_text(status[0] ? status : "Shader loaded.");
}

static jv tool_get_viewer_status(jv params)
{
    (void)params;
    if (!mcp_viewer_mode || !g_mcp_hooks || !g_mcp_hooks->status)
        return mcp_tool_error(
            "get_viewer_status requires the viewer-hosted MCP server "
            "(start the viewer instead of --mcp).");

    char buf[1024] = {0};
    g_mcp_hooks->status(buf, sizeof buf);
    return mcp_tool_text(buf);
}

/* ------------------------------------------------------------------ */
/* tools/list handler                                                 */
/* ------------------------------------------------------------------ */

static jv handle_tools_list(jv params)
{
    (void)params;
    jv tools = jv_array();

    static const struct {
        const char *name;
        const char *desc;
        const char *schema;
    } defs[] = {
        { "list_shaders",
          "List all .glsl shader files in the shaders/ directory.",
          "{\"type\":\"object\",\"properties\":{}}" },
        { "read_shader",
          "Read the full source code of a shader, by name or 0-based index.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Shader filename, with or without .glsl\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"0-based index from list_shaders\"}},"
            "\"description\":\"Provide either name or index\"}" },
        { "get_shader_info",
          "Get metadata about a shader (name, path, line count, byte size), by name or index.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0}},"
            "\"description\":\"Provide either name or index\"}" },
        { "compile_shader",
          "Compile a shader with an OpenGL context and return the compiler log if it fails. Name or index.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0}},"
            "\"description\":\"Provide either name or index\"}" },
        { "get_fps",
          "Benchmark a shader's rendering performance (offscreen framebuffer). Name or index.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0},"
            "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"default\":60},"
            "\"width\":{\"type\":\"integer\",\"minimum\":1,\"default\":640},"
            "\"height\":{\"type\":\"integer\",\"minimum\":1,\"default\":480}},"
            "\"description\":\"Provide either name or index\"}" },
        { "screenshot",
          "Return a base64 PNG. Default: offscreen render (iMouse=0, needs name/index). window=true captures the live viewer backbuffer instead.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0},"
            "\"width\":{\"type\":\"integer\",\"minimum\":1,\"default\":640},"
            "\"height\":{\"type\":\"integer\",\"minimum\":1,\"default\":480},"
            "\"time\":{\"type\":\"number\",\"default\":0},"
            "\"window\":{\"type\":\"boolean\",\"default\":false,\"description\":\"Capture the actual viewer backbuffer (viewer-hosted server only)\"}},"
            "\"description\":\"Provide either name or index unless window is true\"}" },
        { "run_shaders",
          "Run a shader with advancing time, reporting FPS; optionally capture the last frame as PNG. Name or index.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0},"
            "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"default\":60},"
            "\"duration\":{\"type\":\"number\",\"minimum\":0.01,\"default\":1.0,\"description\":\"Total time span in seconds\"},"
            "\"width\":{\"type\":\"integer\",\"minimum\":1,\"default\":320},"
            "\"height\":{\"type\":\"integer\",\"minimum\":1,\"default\":240},"
            "\"captureFrame\":{\"type\":\"boolean\",\"default\":false}},"
            "\"description\":\"Provide either name or index\"}" },
        { "write_shader",
          "Write a new .glsl shader file and immediately compile-check it (compiler log returned in the same response).",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Plain filename; .glsl appended if missing\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"GLSL fragment shader source (Shadertoy body)\"}},"
            "\"required\":[\"name\",\"content\"]}" },
        { "update_shader",
          "Update an existing shader's source (by name or index) and immediately compile-check it.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0},"
            "\"content\":{\"type\":\"string\",\"description\":\"New GLSL source\"}},"
            "\"required\":[\"content\"],\"description\":\"Provide either name or index (name preferred)\"}" },
        { "reload_shaders",
          "Rescan the shaders/ directory for new or changed files.",
          "{\"type\":\"object\",\"properties\":{}}" },
        { "load_shader",
          "Switch the visible viewer to another shader (viewer-hosted server only). Name or index.",
          "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"},"
            "\"index\":{\"type\":\"integer\",\"minimum\":0}},"
            "\"description\":\"Provide either name or index\"}" },
        { "get_viewer_status",
          "Report viewer state: running, current shader, FPS, and window size (viewer-hosted server only).",
          "{\"type\":\"object\",\"properties\":{}}" },
    };

    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        jv t = jv_object();
        t = jv_object_set(t, jv_string("name"), jv_string(defs[i].name));
        t = jv_object_set(t, jv_string("description"), jv_string(defs[i].desc));
        t = jv_object_set(t, jv_string("inputSchema"), jv_parse(defs[i].schema));
        tools = jv_array_append(tools, t);
    }

    jv result = jv_object();
    result = jv_object_set(result, jv_string("tools"), tools);
    return result;
}

/* ------------------------------------------------------------------ */
/* tools/call handler                                                 */
/* ------------------------------------------------------------------ */

static jv handle_tools_call(jv params)
{
    jv name_jv = jv_object_get(jv_copy(params), jv_string("name"));
    if (jv_get_kind(name_jv) != JV_KIND_STRING) {
        jv_free(name_jv);
        char msg[256];
        snprintf(msg, sizeof msg, "Unknown tool request");
        return mcp_tool_error(msg);
    }
    char tool_name[128];
    snprintf(tool_name, sizeof tool_name, "%s", jv_string_value(name_jv));
    jv_free(name_jv);

    jv args = jv_object_get(jv_copy(params), jv_string("arguments"));
    if (jv_get_kind(args) != JV_KIND_OBJECT) {
        jv_free(args);
        args = jv_object();
    }

    jv result;
    if      (strcmp(tool_name, "list_shaders")    == 0) result = tool_list_shaders(args);
    else if (strcmp(tool_name, "read_shader")     == 0) result = tool_read_shader(args);
    else if (strcmp(tool_name, "get_shader_info") == 0) result = tool_get_shader_info(args);
    else if (strcmp(tool_name, "compile_shader")  == 0) result = tool_compile_shader(args);
    else if (strcmp(tool_name, "get_fps")         == 0) result = tool_get_fps(args);
    else if (strcmp(tool_name, "screenshot")      == 0) result = tool_screenshot(args);
    else if (strcmp(tool_name, "run_shaders")     == 0) result = tool_run_shaders(args);
    else if (strcmp(tool_name, "write_shader")    == 0) result = tool_write_shader(args);
    else if (strcmp(tool_name, "update_shader")   == 0) result = tool_update_shader(args);
    else if (strcmp(tool_name, "reload_shaders")  == 0) result = tool_reload_shaders(args);
    else if (strcmp(tool_name, "load_shader")     == 0) result = tool_load_shader(args);
    else if (strcmp(tool_name, "get_viewer_status") == 0) result = tool_get_viewer_status(args);
    else {
        jv_free(args);
        /* Unknown tool: not a protocol violation. Report it as a tool-level
         * error so clients can surface it without tearing down the session. */
        char msg[256];
        snprintf(msg, sizeof msg, "Unknown tool: %s", tool_name);
        return mcp_tool_error(msg);
    }

    jv_free(args);
    return result;
}

/* ------------------------------------------------------------------ */
/* initialize handler                                                 */
/* ------------------------------------------------------------------ */

static jv handle_initialize(jv params)
{
    static const char *supported[] = { MCP_PROTOCOLS_SUPPORTED };

    /* Echo the client's requested version when we support it; otherwise
     * answer with ours (the lowest we speak), per the spec. */
    const char *version = supported[0];
    jv requested = jv_object_get(jv_copy(params), jv_string("protocolVersion"));
    if (jv_get_kind(requested) == JV_KIND_STRING) {
        for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) {
            if (strcmp(jv_string_value(requested), supported[i]) == 0) {
                version = supported[i];
                break;
            }
        }
    }
    jv_free(requested);

    jv result = jv_object();
    result = jv_object_set(result, jv_string("protocolVersion"), jv_string(version));

    jv caps = jv_object();
    caps = jv_object_set(caps, jv_string("tools"), jv_object());
    result = jv_object_set(result, jv_string("capabilities"), caps);

    jv info = jv_object();
    info = jv_object_set(info, jv_string("name"), jv_string(MCP_SERVER_NAME));
    info = jv_object_set(info, jv_string("version"), jv_string(MCP_SERVER_VERSION));
    info = jv_object_set(info, jv_string("title"), jv_string(MCP_SERVER_TITLE));
    result = jv_object_set(result, jv_string("serverInfo"), info);

    result = jv_object_set(result, jv_string("instructions"),
        jv_string(
            "MCP server for editing Shadertoy-style GLSL shaders. "
            "The edit loop is: write_shader (writes AND compiles; errors "
            "come back in the same response) -> screenshot/run to see the "
            "result."));
    return result;
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                   */
/* ------------------------------------------------------------------ */

static void mcp_handle_request(jv request)
{
    jv id = jv_object_get(jv_copy(request), jv_string("id"));
    int has_id = jv_is_valid(id) && jv_get_kind(id) != JV_KIND_NULL;

    jv method_jv = jv_object_get(jv_copy(request), jv_string("method"));
    const char *method = (jv_get_kind(method_jv) == JV_KIND_STRING)
                         ? jv_string_value(method_jv) : NULL;

    if (!method) {
        jv_free(method_jv);
        if (jv_is_valid(id) && jv_get_kind(id) == JV_KIND_NULL)
            jv_free(id); /* "id": null — not a valid request per JSON-RPC */
        else if (has_id)
            mcp_send_error(id, -32600, "Invalid Request",
                           "Request must have a valid method");
        jv_free(request);
        return;
    }

    jv params = jv_object_get(jv_copy(request), jv_string("params"));
    if (jv_get_kind(params) != JV_KIND_OBJECT) {
        jv_free(params);
        params = jv_object();
    }

    int is_notification =
        (strncmp(method, "notifications/", strlen("notifications/")) == 0) ||
        (strcmp(method, "initialized") == 0);

    int dispatch_error = 0;
    jv result = jv_invalid();

      if (is_notification) {
        /* no response */
    } else if (strcmp(method, "initialize") == 0) {
        result = handle_initialize(params);
    } else if (strcmp(method, "tools/list") == 0) {
        result = handle_tools_list(params);
    } else if (strcmp(method, "tools/call") == 0) {
        result = handle_tools_call(params);
    } else {
        dispatch_error = 1;
    }

    if (!is_notification && !dispatch_error && has_id && jv_is_valid(result)) {
        mcp_send_response(id, result);
    } else if (!is_notification && dispatch_error && has_id) {
        mcp_send_error(id, -32601, "Method not found", NULL);
    } else {
        jv_free(id);
        if (jv_is_valid(result)) jv_free(result);
    }

    jv_free(method_jv);
    jv_free(params);
    jv_free(request);
}

/* Process one JSON-RPC message body. In capture mode the reply (if any)
 * lands in mcp_captured; notifications produce no reply. */
static void mcp_process_request(const char *body)
{
    jv request = jv_parse(body);
    if (!jv_is_valid(request)) {
        jv_free(request);
        mcp_send_error(jv_null(), -32700, "Parse error",
                       "Could not parse JSON request");
        return;
    }
    mcp_handle_request(request);
}

/* ------------------------------------------------------------------ */
/* HTTP transport (architecture B: viewer-hosted)                     */
/* ------------------------------------------------------------------ */

#define MCP_HTTP_MAX_BODY   (8u * 1024u * 1024u)
#define MCP_HTTP_MAX_HEADER 16384u

static SDL_Thread *mcp_http_thread = NULL;
static SDL_mutex  *mcp_http_mutex  = NULL;
static SDL_cond   *mcp_http_work   = NULL;
static SDL_cond   *mcp_http_done   = NULL;
static mcp_socket_t mcp_http_listen = MCP_INVALID_SOCKET;
static volatile int mcp_http_stop = 0;
static volatile int mcp_http_running = 0;
static int mcp_http_port = 0;
#ifdef _WIN32
static int mcp_wsa_started = 0;
#endif

/* 0 idle, 1 queued, 2 running on the main thread, 3 response ready. */
static int mcp_job_state = 0;
static char *mcp_job_body = NULL;
static char *mcp_job_response = NULL;

static void mcp_log(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s\n", msg);
    /* GUI-subsystem builds have no console; keep a file for diagnostics. */
    FILE *f = fopen("build/shadertoy-mcp.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void mcp_write_lockfile(int port)
{
    FILE *f = fopen("build/.shadertoy-mcp", "w");
    if (!f)
        return;
    fprintf(f, "%d %ld\n", port, (long)mcp_getpid());
    fclose(f);
}

/* Wait for the socket to become readable/writable, with a timeout in ms.
 * Returns 1 when ready, 0 on timeout, -1 on error. */
static int mcp_socket_wait(mcp_socket_t fd, int for_write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    int n = select(0, for_write ? NULL : &set, for_write ? &set : NULL,
                   NULL, &tv);
#else
    int n = select((int)fd + 1, for_write ? NULL : &set,
                   for_write ? &set : NULL, NULL, &tv);
#endif
    return n > 0 ? 1 : (n == 0 ? 0 : -1);
}

/* Send everything or give up after a timeout. */
static void mcp_send_all(mcp_socket_t fd, const char *data, size_t len)
{
    while (len > 0) {
        if (mcp_socket_wait(fd, 1, 5000) != 1)
            return;
        int n = send(fd, data, (int)len, 0);
        if (n <= 0)
            return;
        data += n;
        len -= (size_t)n;
    }
}

static void mcp_http_reply(mcp_socket_t fd, int code, const char *reason,
                           const char *body, size_t body_len)
{
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %lu\r\n"
                     "Mcp-Session-Id: shadertoy-viewer\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, (unsigned long)body_len);
    if (n > 0)
        mcp_send_all(fd, head, (size_t)n);
    if (body && body_len)
        mcp_send_all(fd, body, body_len);
}

static int mcp_strnicmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

/* Case-insensitive search for a header value; returns a pointer into the
 * header block or NULL. */
static const char *mcp_header_value(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = headers;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > name_len && mcp_strnicmp(line, name, name_len) == 0 &&
            line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
        line = eol ? eol + 2 : NULL;
    }
    return NULL;
}

/* Execute a request body on the main thread and return its JSON reply
 * (NULL when the message was a notification). HTTP thread only. */
static char *mcp_run_job(char *body)
{
    char *response = NULL;
    SDL_LockMutex(mcp_http_mutex);
    while (mcp_job_state != 0 && !mcp_http_stop)
        SDL_CondWait(mcp_http_work, mcp_http_mutex);
    if (mcp_http_stop) {
        SDL_UnlockMutex(mcp_http_mutex);
        free(body);
        return NULL;
    }
    mcp_job_body = body;
    mcp_job_state = 1;
    SDL_CondSignal(mcp_http_work);

    while (mcp_job_state != 3 && !mcp_http_stop)
        SDL_CondWait(mcp_http_done, mcp_http_mutex);

    if (mcp_http_stop) {
        /* The main loop only stops after this thread is joined, so state 2
         * cannot be mid-flight here; anything still queued is discarded. */
        if (mcp_job_state == 1) {
            free(mcp_job_body);
            mcp_job_body = NULL;
            mcp_job_state = 0;
        }
        SDL_UnlockMutex(mcp_http_mutex);
        return NULL;
    }

    response = mcp_job_response;
    mcp_job_response = NULL;
    mcp_job_state = 0;
    SDL_UnlockMutex(mcp_http_mutex);
    return response;
}

static void mcp_http_handle_client(mcp_socket_t fd)
{
    char headers[MCP_HTTP_MAX_HEADER + 1];
    size_t have = 0;
    char *body_start = NULL;

    while (have < MCP_HTTP_MAX_HEADER) {
        if (mcp_socket_wait(fd, 0, 5000) != 1)
            return;
        int n = recv(fd, headers + have, (int)(MCP_HTTP_MAX_HEADER - have), 0);
        if (n <= 0)
            return;
        have += (size_t)n;
        headers[have] = '\0';
        body_start = strstr(headers, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            break;
        }
    }
    if (!body_start) {
        const char msg[] = "{\"error\":\"request headers too large\"}";
        mcp_http_reply(fd, 431, "Request Header Fields Too Large",
                       msg, sizeof msg - 1);
        return;
    }

    char method[16] = {0}, target[1024] = {0};
    if (sscanf(headers, "%15s %1023s", method, target) != 2) {
        const char msg[] = "{\"error\":\"bad request line\"}";
        mcp_http_reply(fd, 400, "Bad Request", msg, sizeof msg - 1);
        return;
    }
    char *query = strchr(target, '?');
    if (query) *query = '\0';

    if (strcmp(method, "GET") == 0 || strcmp(method, "DELETE") == 0) {
        if (strcmp(target, "/mcp") != 0) {
            const char msg[] = "{\"error\":\"not found\"}";
            mcp_http_reply(fd, 404, "Not Found", msg, sizeof msg - 1);
        } else {
            /* No SSE stream and no server-side session state to tear down. */
            mcp_http_reply(fd, 204, "No Content", NULL, 0);
        }
        return;
    }
    if (strcmp(method, "POST") != 0 || strcmp(target, "/mcp") != 0) {
        const char msg[] = "{\"error\":\"not found\"}";
        mcp_http_reply(fd, 404, "Not Found", msg, sizeof msg - 1);
        return;
    }

    const char *cl = mcp_header_value(headers, "Content-Length");
    size_t body_len = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    size_t already = have - (size_t)(body_start - headers);
    if (body_len == 0 || body_len > MCP_HTTP_MAX_BODY) {
        const char msg[] = "{\"error\":\"missing or oversized Content-Length\"}";
        mcp_http_reply(fd, body_len ? 413 : 411, "Content-Length Required",
                       msg, sizeof msg - 1);
        return;
    }

    char *body = malloc(body_len + 1);
    if (!body) {
        const char msg[] = "{\"error\":\"out of memory\"}";
        mcp_http_reply(fd, 500, "Internal Server Error", msg, sizeof msg - 1);
        return;
    }
    if (already > body_len)
        already = body_len;
    memcpy(body, body_start, already);
    size_t got = already;
    while (got < body_len) {
        if (mcp_socket_wait(fd, 0, 5000) != 1)
            break;
        int n = recv(fd, body + got, (int)(body_len - got), 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    if (got != body_len) {
        free(body);
        const char msg[] = "{\"error\":\"incomplete body\"}";
        mcp_http_reply(fd, 400, "Bad Request", msg, sizeof msg - 1);
        return;
    }
    body[body_len] = '\0';

    char *response = mcp_run_job(body);
    if (response) {
        mcp_http_reply(fd, 200, "OK", response, strlen(response));
        free(response);
    } else {
        /* Notification: accepted, nothing to say. */
        mcp_http_reply(fd, 202, "Accepted", NULL, 0);
    }
}

static int SDLCALL mcp_http_thread_fn(void *data)
{
    (void)data;
    while (!mcp_http_stop) {
        if (mcp_socket_wait(mcp_http_listen, 0, 200) != 1)
            continue;
        mcp_socket_t client = accept(mcp_http_listen, NULL, NULL);
        if (client == MCP_INVALID_SOCKET)
            continue;
        mcp_http_handle_client(client);
        mcp_close_socket(client);
    }
    return 0;
}

int mcp_viewer_start(int port, const mcp_viewer_hooks *hooks)
{
    if (port <= 0)
        return 1;
    if (mcp_http_running) {
        mcp_log("MCP server already running on port %d", mcp_http_port);
        return 1;
    }

    mcp_viewer_mode = 1;
    g_mcp_hooks = hooks;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        mcp_log("MCP server: WSAStartup failed");
        return 0;
    }
    mcp_wsa_started = 1;
#endif

    mcp_http_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mcp_http_listen == MCP_INVALID_SOCKET) {
        mcp_log("MCP server: could not create socket");
        goto fail;
    }

#ifndef _WIN32
    {
        int yes = 1;
        setsockopt(mcp_http_listen, SOL_SOCKET, SO_REUSEADDR,
                   &yes, sizeof yes);
    }
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(mcp_http_listen, (struct sockaddr *)&addr, sizeof addr) != 0) {
        mcp_log("MCP server: port %d is already in use "
                "(is another viewer running?)", port);
        goto fail;
    }
    if (listen(mcp_http_listen, 8) != 0) {
        mcp_log("MCP server: listen failed on port %d", port);
        goto fail;
    }

    mcp_http_mutex = SDL_CreateMutex();
    mcp_http_work  = SDL_CreateCond();
    mcp_http_done  = SDL_CreateCond();
    if (!mcp_http_mutex || !mcp_http_work || !mcp_http_done) {
        mcp_log("MCP server: could not create synchronization objects");
        goto fail;
    }

    mcp_http_stop = 0;
    mcp_http_port = port;
    mcp_http_thread = SDL_CreateThread(mcp_http_thread_fn,
                                       "shadertoy-mcp-http", NULL);
    if (!mcp_http_thread) {
        mcp_log("MCP server: could not create HTTP thread: %s",
                SDL_GetError());
        mcp_http_port = 0;
        goto fail;
    }

    mcp_http_running = 1;
    mcp_write_lockfile(port);
    mcp_log("MCP server listening on http://127.0.0.1:%d/mcp", port);
    return 1;

fail:
    if (mcp_http_listen != MCP_INVALID_SOCKET) {
        mcp_close_socket(mcp_http_listen);
        mcp_http_listen = MCP_INVALID_SOCKET;
    }
    if (mcp_http_mutex) { SDL_DestroyMutex(mcp_http_mutex); mcp_http_mutex = NULL; }
    if (mcp_http_work)  { SDL_DestroyCond(mcp_http_work);  mcp_http_work = NULL; }
    if (mcp_http_done)  { SDL_DestroyCond(mcp_http_done);  mcp_http_done = NULL; }
#ifdef _WIN32
    if (mcp_wsa_started) { WSACleanup(); mcp_wsa_started = 0; }
#endif
    return 0;
}

void mcp_viewer_poll(void)
{
    if (!mcp_http_running || !mcp_http_mutex)
        return;

    SDL_LockMutex(mcp_http_mutex);
    if (mcp_job_state == 1) {
        mcp_job_state = 2;
        char *body = mcp_job_body;
        mcp_job_body = NULL;

        mcp_capture_enabled = 1;
        free(mcp_captured);
        mcp_captured = NULL;
        mcp_process_request(body);
        mcp_capture_enabled = 0;

        free(body);
        mcp_job_response = mcp_captured;
        mcp_captured = NULL;
        mcp_job_state = 3;
        SDL_CondSignal(mcp_http_done);
    }
    SDL_UnlockMutex(mcp_http_mutex);
}

void mcp_viewer_stop(void)
{
    if (!mcp_http_thread && mcp_http_listen == MCP_INVALID_SOCKET)
        return;

    if (mcp_http_mutex) {
        SDL_LockMutex(mcp_http_mutex);
        mcp_http_stop = 1;
        SDL_CondBroadcast(mcp_http_work);
        SDL_CondBroadcast(mcp_http_done);
        SDL_UnlockMutex(mcp_http_mutex);
    }
    if (mcp_http_thread) {
        SDL_WaitThread(mcp_http_thread, NULL);
        mcp_http_thread = NULL;
    }
    if (mcp_http_listen != MCP_INVALID_SOCKET) {
        mcp_close_socket(mcp_http_listen);
        mcp_http_listen = MCP_INVALID_SOCKET;
    }
    if (mcp_http_mutex) {
        SDL_DestroyMutex(mcp_http_mutex);
        mcp_http_mutex = NULL;
    }
    if (mcp_http_work) {
        SDL_DestroyCond(mcp_http_work);
        mcp_http_work = NULL;
    }
    if (mcp_http_done) {
        SDL_DestroyCond(mcp_http_done);
        mcp_http_done = NULL;
    }
    free(mcp_job_body);
    mcp_job_body = NULL;
    free(mcp_job_response);
    mcp_job_response = NULL;
    mcp_job_state = 0;
#ifdef _WIN32
    if (mcp_wsa_started) {
        WSACleanup();
        mcp_wsa_started = 0;
    }
#endif
    if (mcp_http_running)
        remove("build/.shadertoy-mcp");
    mcp_http_running = 0;
    mcp_http_port = 0;
    mcp_log("MCP server stopped");
}

int mcp_viewer_port(void)
{
    return mcp_http_port;
}

/* ------------------------------------------------------------------ */
/* Server main loop                                                   */
/* ------------------------------------------------------------------ */

int mcp_run(void)
{
    mcp_stdio_binary();
    while (!feof(stdin)) {
        char *body = mcp_read_message_body();
        if (!body)
            break;

        /* Skip whitespace-only bodies (stray blank lines). */
        int blank = 1;
        for (const char *p = body; *p; p++)
            if (!isspace((unsigned char)*p)) { blank = 0; break; }
        if (blank) {
            free(body);
            continue;
        }

        mcp_process_request(body);
        free(body);
    }

    mcp_gl_cleanup();
    shader_list_free(&mcp_shaders);
    return 0;
}
