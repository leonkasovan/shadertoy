/*
 * MCP (Model Context Protocol) server for the Shadertoy SDL2 viewer.
 *
 * Implements the MCP stdio transport: JSON-RPC 2.0 messages framed with
 * Content-Length headers over stdin/stdout.  Uses libjq for JSON parsing
 * and serialization.
 *
 * Tools provided:
 *   - list_shaders   : list all .glsl files in shaders/
 *   - read_shader    : read a shader's source by index
 *   - get_shader_info: metadata (name, path, line count) by index
 *   - compile_shader : compile-check a shader (requires headless GL)
 *   - get_fps        : benchmark a shader's rendering FPS (requires headless GL)
 *   - screenshot     : render a shader frame and return as PNG image (requires GL)
 *   - run_shaders    : run a shader with advancing time, report FPS,
 *                      optionally capture a frame (requires GL)
 *   - write_shader   : create a new .glsl shader file in shaders/
 *   - reload_shaders : rescan the shaders/ directory
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include <jq.h>
#include <jv.h>

#include "mcp.h"
#include "shader_common.h"

/* Texture loading for iChannel0..3. main.c owns the STB_IMAGE_IMPLEMENTATION
 * translation unit, so only the declarations are needed here. */
#include "../stb_image.h"

#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_SERVER_NAME      "shadertoy-sdl2"
#define MCP_SERVER_VERSION   "1.0.0"
#define MCP_SERVER_TITLE     "Shadertoy SDL2 Renderer"

#define MAX_MCP_SHADERS 256

/* ------------------------------------------------------------------ */
/* Shader list                                                        */
/* ------------------------------------------------------------------ */

static char *mcp_shader_list[MAX_MCP_SHADERS];
static int   mcp_shader_count = 0;
static int   mcp_shaders_scanned = 0;

static char *mcp_read_file(const char *path)
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

static int mcp_compare_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static const char *mcp_shader_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    return p ? p + 1 : path;
}

static void mcp_scan_shaders_dir(const char *dir)
{
    for (int i = 0; i < mcp_shader_count; i++)
        free(mcp_shader_list[i]);
    mcp_shader_count = 0;

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && mcp_shader_count < MAX_MCP_SHADERS) {
        const char *nm = ent->d_name;
        size_t nlen = strlen(nm);
        if (nlen > 5 && strcmp(nm + nlen - 5, ".glsl") == 0) {
            size_t pl = strlen(dir) + 1 + nlen + 1;
            char *path = malloc(pl);
            snprintf(path, pl, "%s/%s", dir, nm);
            mcp_shader_list[mcp_shader_count++] = path;
        }
    }
    closedir(d);
    if (mcp_shader_count > 1)
        qsort(mcp_shader_list, mcp_shader_count, sizeof(char *),
              mcp_compare_strings);
    mcp_shaders_scanned = 1;
}

static void mcp_ensure_scanned(void)
{
    if (!mcp_shaders_scanned)
        mcp_scan_shaders_dir("shaders");
}

/* ------------------------------------------------------------------ */
/* MCP framing over stdio                                             */
/*                                                                    */
/* Accepts both wire formats in use:                                  */
/*   1. Newline-delimited JSON  — what MCP clients actually send.     */
/*   2. LSP-style Content-Length headers — kept for compatibility.    */
/* ------------------------------------------------------------------ */

/* Read one header block. Returns content_length (>0) for Content-Length
 * framed messages, or 0 if the first non-empty line was not a header
 * (the caller then re-parses it as a bare JSON line). */
static char *mcp_read_message_body(void)
{
    char line[8192];
    long content_length = 0;
    int saw_header = 0;

    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) {
            if (saw_header) break;   /* end of header block */
            continue;                /* tolerate leading blank lines */
        }

        if (content_length == 0 &&
            strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = strtol(line + 15, NULL, 10);
            saw_header = 1;
            continue;
        }
        if (strncasecmp(line, "Content-Type:", 13) == 0) {
            saw_header = 1;
            continue;
        }

        if (!saw_header) {
            /* Not a header: this is a raw JSON line (MCP stdio transport).
             * Hand it straight back as the message body. */
            char *body = malloc(len + 1);
            if (!body) return NULL;
            memcpy(body, line, len + 1);
            return body;
        }
        /* unknown header inside a header block: ignore */
    }

    if (content_length <= 0)
        return NULL;
    if (content_length > (long)(256 * 1024 * 1024)) /* sanity cap: 256 MiB */
        return NULL;

    char *body = malloc((size_t)content_length + 1);
    if (!body) return NULL;
    size_t n = fread(body, 1, (size_t)content_length, stdin);
    body[n] = '\0';
    return body;
}

/* Serialize a JSON value using the framing this client speaks. Default is
 * the MCP stdio transport (one JSON object per line); --mcp-lsp switches
 * to Content-Length framing for LSP-style clients. */
static int mcp_framing = 0; /* 0 = newline-delimited, 1 = Content-Length */

static void mcp_write_message(const char *json)
{
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
/* Response helpers                                                   */
/* ------------------------------------------------------------------ */

static void mcp_send_response(jv id, jv result)
{
    jv resp = jv_object();
    resp = jv_object_set(resp, jv_string("jsonrpc"), jv_string("2.0"));
    resp = jv_object_set(resp, jv_string("id"), id);
    resp = jv_object_set(resp, jv_string("result"), result);

    jv dumped = jv_dump_string(resp, 0);
    const char *s = jv_string_value(dumped);
    mcp_write_message(s);
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
    const char *s = jv_string_value(dumped);
    mcp_write_message(s);
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

static jv mcp_tool_error(const char *message)
{
    jv result = mcp_tool_text(message);
    result = jv_object_set(result, jv_string("isError"), jv_true());
    return result;
}

/* ------------------------------------------------------------------ */
/* Base64 encoding                                                     */
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

/* ------------------------------------------------------------------ */
/* Byte-order helpers (shared by the PNG encoder)                     */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* PNG encoding                                                       */
/* ------------------------------------------------------------------ */

/* Encode RGBA pixels as a PNG (truecolor + alpha, filter type 0 per row).
 * PNG is what MCP image consumers can actually display, unlike BMP. */
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

    /* One IDAT chunk with zlib "stored" blocks: no deflate compression
     * needed, and the result is still a valid PNG. */
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
/* BMP encoding (uncompressed 32-bit BGRA, bottom-up)                 */
/* ------------------------------------------------------------------ */

static void mcp_put16le(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void mcp_put32le(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Kept for reference/tests: BMP is no longer returned by tools. */
__attribute__((unused))
static char *mcp_encode_bmp(int width, int height,
                            const unsigned char *rgba, size_t *out_size)
{
    int row_size = width * 4;
    int pixel_data_size = row_size * height;
    int file_size = 14 + 40 + pixel_data_size;

    unsigned char *bmp = malloc((size_t)file_size);
    if (!bmp) return NULL;

    /* File header (14 bytes) */
    bmp[0] = 'B'; bmp[1] = 'M';
    mcp_put32le(bmp + 2, (uint32_t)file_size);
    mcp_put32le(bmp + 6, 0);
    mcp_put32le(bmp + 10, 54);

    /* DIB header — BITMAPINFOHEADER (40 bytes) */
    mcp_put32le(bmp + 14, 40);
    mcp_put32le(bmp + 18, (uint32_t)width);
    mcp_put32le(bmp + 22, (uint32_t)height);  /* positive = bottom-up */
    mcp_put16le(bmp + 26, 1);   /* planes */
    mcp_put16le(bmp + 28, 32);  /* bits per pixel */
    mcp_put32le(bmp + 30, 0);   /* compression = BI_RGB */
    mcp_put32le(bmp + 34, (uint32_t)pixel_data_size);
    mcp_put32le(bmp + 38, 2835); /* x ppm ≈ 72 DPI */
    mcp_put32le(bmp + 42, 2835); /* y ppm ≈ 72 DPI */
    mcp_put32le(bmp + 46, 0);    /* colors used */
    mcp_put32le(bmp + 50, 0);    /* important colors */

    /* Pixel data (bottom-to-top, swap R/B for BGR(A)) */
    unsigned char *dst = bmp + 54;
    for (int y = 0; y < height; y++) {
        const unsigned char *src_row =
            rgba + (size_t)(height - 1 - y) * (size_t)row_size;
        for (int x = 0; x < row_size; x += 4) {
            dst[x + 0] = src_row[x + 2]; /* B */
            dst[x + 1] = src_row[x + 1]; /* G */
            dst[x + 2] = src_row[x + 0]; /* R */
            dst[x + 3] = src_row[x + 3]; /* A */
        }
        dst += (size_t)row_size;
    }

    *out_size = (size_t)file_size;
    return (char *)bmp;
}

/* ------------------------------------------------------------------ */
/* GL context for headless rendering                                 */
/* ------------------------------------------------------------------ */

#define MCP_HAS_GL 1

#if MCP_HAS_GL

#include <SDL2/SDL.h>
#include "glad.h"

static SDL_Window   *mcp_gl_window  = NULL;
static SDL_GLContext mcp_gl_context = NULL;
static int             mcp_gl_ready   = 0;

static int mcp_gl_init(void)
{
    if (mcp_gl_ready) return 1;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            return 0;
        }
    }

    mcp_gl_window = SDL_CreateWindow("shadertoy-mcp",
        0, 0, 1, 1,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (!mcp_gl_window) {
        SDL_Quit();
        return 0;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    mcp_gl_context = SDL_GL_CreateContext(mcp_gl_window);
    if (!mcp_gl_context) {
        SDL_DestroyWindow(mcp_gl_window);
        SDL_Quit();
        return 0;
    }

#ifdef USE_GLES
    if (!gladLoadGLES2Loader((GLADloadproc)SDL_GL_GetProcAddress)) {
#else
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
#endif
        SDL_GL_DeleteContext(mcp_gl_context);
        SDL_DestroyWindow(mcp_gl_window);
        SDL_Quit();
        return 0;
    }

    mcp_gl_ready = 1;
    return 1;
}

static void mcp_gl_cleanup(void)
{
    if (mcp_gl_context)
        SDL_GL_DeleteContext(mcp_gl_context);
    if (mcp_gl_window)
        SDL_DestroyWindow(mcp_gl_window);
    mcp_gl_ready = 0;
    SDL_Quit();
}

/* --- shader compilation ---
 * All GLSL text, program building and uniform uploading lives in
 * shader_common.c so the MCP renderer and the interactive viewer cannot
 * disagree about what a Shadertoy shader gets. */

/* Link a shader program from a Shadertoy fragment shader file */
static GLuint mcp_prepare_program(int idx, char *logbuf, size_t loglen)
{
    char *src = mcp_read_file(mcp_shader_list[idx]);
    if (!src) {
        snprintf(logbuf, loglen, "Could not read shader file");
        return 0;
    }

    GLuint prog = shader_build_program(src, logbuf, loglen);
    free(src);
    return prog;
}

/* Create a VAO/VBO for a fullscreen triangle strip */
static void mcp_setup_quad(GLuint *vao_out, GLuint *vbo_out)
{
    static const float quad[] = { -1, -1,  1, -1,  -1, 1,  1, 1 };
    glGenVertexArrays(1, vao_out);
    glBindVertexArray(*vao_out);
    glGenBuffers(1, vbo_out);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo_out);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
}

/* Set Shadertoy-compatible uniforms.
 *
 * Uses the same uploader as the interactive viewer so headless output matches
 * what the window shows. Locations are looked up per call (these paths are
 * already dominated by rendering and glReadPixels) but the values are shared.
 *
 * Channel textures are bound by the caller; iChannelResolution is passed in
 * via `chan_res` so a shader that samples a texture sees a sane size instead
 * of zero. */
static void mcp_set_uniforms(GLuint prog, float w, float h,
                             float time_val, int frame,
                             const float chan_res[4][3])
{
    shader_uniforms u;
    shader_uniforms_lookup(prog, &u);

    shader_frame f;
    shader_frame_defaults(&f);
    f.width       = w;
    f.height      = h;
    f.time        = time_val;
    f.time_delta  = 1.0f / 60.0f;
    f.frame       = frame;
    f.frame_rate  = 60.0f;
    f.mouse_x = f.mouse_y = f.mouse_left = f.mouse_right = 0.0f;
    for (int ch = 0; ch < 4; ch++)
        f.channel_time[ch] = time_val;
    if (chan_res)
        memcpy(f.channel_resolution, chan_res, sizeof f.channel_resolution);

    shader_upload_uniforms(&u, &f);
}

/* Channel textures for headless rendering.
 *
 * Auto-detects "<shader>.<ch>.<ext>" next to the shader (same rule the viewer
 * uses) and uploads it. Returns the resolution to publish via
 * iChannelResolution; channels without a texture report zeros, matching the
 * viewer's behaviour for unbound channels. */
static void mcp_load_channels(int idx, GLuint tex[4], float res[4][3])
{
    for (int ch = 0; ch < 4; ch++) {
        tex[ch] = 0;
        res[ch][0] = res[ch][1] = res[ch][2] = 0.0f;
    }

    static const char *exts[] = { "png", "jpg", "jpeg", "bmp", "tga" };
    const char *shader_path = mcp_shader_list[idx];

    /* Strip ".glsl" from the shader path to build the texture stem. */
    char stem[1024];
    snprintf(stem, sizeof stem, "%s", shader_path);
    char *dot = strrchr(stem, '.');
    if (dot && strcmp(dot, ".glsl") == 0) *dot = 0;

    for (int ch = 0; ch < 4; ch++) {
        char path[1200] = {0};
        for (size_t e = 0; e < sizeof exts / sizeof exts[0]; e++) {
            snprintf(path, sizeof path, "%s.%d.%s", stem, ch, exts[e]);
            FILE *fp = fopen(path, "rb");
            if (!fp) continue;
            fclose(fp);

            int w = 0, h = 0, n = 0;
            unsigned char *pixels = stbi_load(path, &w, &h, &n, 4);
            if (!pixels) continue;

            glGenTextures(1, &tex[ch]);
            glBindTexture(GL_TEXTURE_2D, tex[ch]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            stbi_image_free(pixels);

            res[ch][0] = (float)w;
            res[ch][1] = (float)h;
            res[ch][2] = 1.0f;
            break;
        }
    }
}

/* Bind channel textures and a 1x1 black fallback so samplers are always
 * valid (undefined sampler behaviour would corrupt the image). */
static void mcp_bind_channels(GLuint tex[4])
{
    static GLuint black_tex = 0;
    if (!black_tex) {
        const unsigned char black[4] = { 0, 0, 0, 255 };
        glGenTextures(1, &black_tex);
        glBindTexture(GL_TEXTURE_2D, black_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, black);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }
    for (int ch = 0; ch < 4; ch++) {
        glActiveTexture(GL_TEXTURE0 + ch);
        glBindTexture(GL_TEXTURE_2D, tex[ch] ? tex[ch] : black_tex);
    }
}

static void mcp_free_channels(GLuint tex[4])
{
    for (int ch = 0; ch < 4; ch++)
        if (tex[ch]) glDeleteTextures(1, &tex[ch]);
}

/* Render one frame of a shader to the given framebuffer.
   Leaves the FBO bound so callers can read pixels via glReadPixels. */
static void mcp_render_frame(GLuint prog, GLuint vao, GLuint fbo,
                             int width, int height,
                             float time_val, int frame,
                             const float chan_res[4][3])
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glUseProgram(prog);
    mcp_set_uniforms(prog, (float)width, (float)height, time_val, frame,
                     chan_res);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
}

#endif /* MCP_HAS_GL */

/* ------------------------------------------------------------------ */
/* Tool implementations                                               */
/* ------------------------------------------------------------------ */

static jv tool_list_shaders(jv params)
{
    (void)params;
    mcp_ensure_scanned();

    char buf[65536];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "Shaders found: %d\n\n",
                    mcp_shader_count);
    for (int i = 0; i < mcp_shader_count; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%d: %s\n",
                        i, mcp_shader_basename(mcp_shader_list[i]));
    }
    if (mcp_shader_count == 0)
        snprintf(buf + off, sizeof(buf) - off, "(no .glsl files in shaders/)\n");

    return mcp_tool_text(buf);
}

static jv tool_read_shader(jv params)
{
    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) != JV_KIND_NUMBER) {
        jv_free(idx_jv);
        return mcp_tool_error("Missing required argument: index (integer)");
    }
    int idx = (int)jv_number_value(idx_jv);
    jv_free(idx_jv);

    mcp_ensure_scanned();
    if (idx < 0 || idx >= mcp_shader_count) {
        char ebuf[128];
        snprintf(ebuf, sizeof ebuf, "Shader index %d out of range (0-%d)",
                 idx, mcp_shader_count > 0 ? mcp_shader_count - 1 : 0);
        return mcp_tool_error(ebuf);
    }

    char *src = mcp_read_file(mcp_shader_list[idx]);
    if (!src)
        return mcp_tool_error("Could not read shader file");

    jv result = mcp_tool_text(src);
    free(src);
    return result;
}

static jv tool_get_shader_info(jv params)
{
    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) != JV_KIND_NUMBER) {
        jv_free(idx_jv);
        return mcp_tool_error("Missing required argument: index (integer)");
    }
    int idx = (int)jv_number_value(idx_jv);
    jv_free(idx_jv);

    mcp_ensure_scanned();
    if (idx < 0 || idx >= mcp_shader_count) {
        char ebuf[128];
        snprintf(ebuf, sizeof ebuf, "Shader index %d out of range (0-%d)",
                 idx, mcp_shader_count > 0 ? mcp_shader_count - 1 : 0);
        return mcp_tool_error(ebuf);
    }

    char *src = mcp_read_file(mcp_shader_list[idx]);
    if (!src)
        return mcp_tool_error("Could not read shader file");

    int lines = 1;
    for (char *p = src; *p; p++)
        if (*p == '\n') lines++;

    /* Text representation */
    char txt[512];
    snprintf(txt, sizeof txt,
             "Index: %d\nName: %s\nPath: %s\nLines: %d\nSize: %zu bytes",
             idx, mcp_shader_basename(mcp_shader_list[idx]),
             mcp_shader_list[idx], lines, strlen(src));

    jv result = mcp_tool_text(txt);

    /* Structured content */
    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("index"), jv_number(idx));
    structured = jv_object_set(structured, jv_string("name"),
                               jv_string(mcp_shader_basename(mcp_shader_list[idx])));
    structured = jv_object_set(structured, jv_string("path"),
                               jv_string(mcp_shader_list[idx]));
    structured = jv_object_set(structured, jv_string("lineCount"),
                               jv_number(lines));
    structured = jv_object_set(structured, jv_string("byteSize"),
                               jv_number((double)strlen(src)));
    result = jv_object_set(result, jv_string("structuredContent"), structured);

    free(src);
    return result;
}

static jv tool_compile_shader(jv params)
{
    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) != JV_KIND_NUMBER) {
        jv_free(idx_jv);
        return mcp_tool_error("Missing required argument: index (integer)");
    }
    int idx = (int)jv_number_value(idx_jv);
    jv_free(idx_jv);

    mcp_ensure_scanned();
    if (idx < 0 || idx >= mcp_shader_count) {
        char ebuf[128];
        snprintf(ebuf, sizeof ebuf, "Shader index %d out of range (0-%d)",
                 idx, mcp_shader_count > 0 ? mcp_shader_count - 1 : 0);
        return mcp_tool_error(ebuf);
    }

    char *src = mcp_read_file(mcp_shader_list[idx]);
    if (!src)
        return mcp_tool_error("Could not read shader file");

#if MCP_HAS_GL
    if (!mcp_gl_init()) {
        free(src);
        return mcp_tool_error(
            "Could not create OpenGL context for shader compilation "
            "(no GPU/display available).");
    }

    /* Compile via the shared builder. It reports the most specific failure
     * (fragment log, else vertex log, else link log), which is all the caller
     * needs to act on. */
    char logbuf[4096] = {0};
    GLuint prog = shader_build_program(src, logbuf, sizeof logbuf);
    free(src);

    int gl_err = glGetError();

    char result[16384];
    int off = 0;
    off += snprintf(result + off, sizeof(result) - off,
                    "Shader: %s\nIndex: %d\n\n"
                    "Compilation: %s\n"
                    "GL error: %d\n",
                    mcp_shader_basename(mcp_shader_list[idx]), idx,
                    prog ? "OK" : "FAILED",
                    gl_err);

    if (!prog && logbuf[0])
        off += snprintf(result + off, sizeof(result) - off,
                        "\nCompiler log:\n%s\n", logbuf);

    if (prog) glDeleteProgram(prog);

    return prog ? mcp_tool_text(result) : mcp_tool_error(result);
#else
    free(src);
    return mcp_tool_error("OpenGL headers not available; cannot compile shader.");
#endif
}

static jv tool_reload_shaders(jv params)
{
    (void)params;
    mcp_scan_shaders_dir("shaders");
    char buf[256];
    snprintf(buf, sizeof buf,
             "Rescanned shaders/ directory. Found %d shader(s).",
             mcp_shader_count);
    return mcp_tool_text(buf);
}

static jv tool_write_shader(jv params)
{
    jv name_jv = jv_object_get(jv_copy(params), jv_string("name"));
    if (jv_get_kind(name_jv) != JV_KIND_STRING) {
        jv_free(name_jv);
        return mcp_tool_error("Missing required argument: name (string)");
    }
    /* Copy before freeing the jv: jv_string_value() points into it. */
    char safe_name[256];
    snprintf(safe_name, sizeof safe_name, "%s", jv_string_value(name_jv));
    jv_free(name_jv);

    /* Reject anything that is not a plain filename inside shaders/. Without
     * this, a name like "../../evil" escapes the shader directory. */
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
        jv_free(name_jv);
        jv_free(content_jv);
        return mcp_tool_error("Missing required argument: content (string)");
    }
    const char *content = jv_string_value(content_jv);

    /* Build file path: shaders/<name>.glsl */
    char filepath[320];
    const char *ext = strrchr(safe_name, '.');
    if (ext && strcmp(ext, ".glsl") == 0)
        snprintf(filepath, sizeof(filepath), "shaders/%s", safe_name);
    else
        snprintf(filepath, sizeof(filepath), "shaders/%s.glsl", safe_name);

    /* Ensure the shaders/ directory exists */
#ifdef _WIN32
    mkdir("shaders");
#else
    mkdir("shaders", 0755);
#endif

    /* Write the file */
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "Could not write shader file '%s' (check permissions).",
                 filepath);
        jv_free(content_jv);
        return mcp_tool_error(buf);
    }
    size_t content_len = strlen(content);
    fwrite(content, 1, content_len, f);
    fclose(f);

    jv_free(content_jv);

    /* Reload shader list */
    mcp_scan_shaders_dir("shaders");

    /* Find the new shader's index */
    int new_idx = -1;
    for (int i = 0; i < mcp_shader_count; i++) {
        if (strcmp(mcp_shader_basename(mcp_shader_list[i]),
                   mcp_shader_basename(filepath)) == 0) {
            new_idx = i;
            break;
        }
    }

    char result[512];
    snprintf(result, sizeof(result),
             "Shader written to: %s\n"
             "Index: %d\n"
             "Size: %zu bytes\n"
             "Total shaders: %d\n",
             filepath, new_idx, content_len, mcp_shader_count);

    jv res = mcp_tool_text(result);

    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("path"),
                               jv_string(filepath));
    structured = jv_object_set(structured, jv_string("index"),
                               jv_number(new_idx));
    structured = jv_object_set(structured, jv_string("size"),
                               jv_number((double)content_len));
    res = jv_object_set(res, jv_string("structuredContent"), structured);
    return res;
}

#if MCP_HAS_GL
static jv tool_get_fps(jv params)
{
    int idx, frames, width, height;
    char ebuf[128];

    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) != JV_KIND_NUMBER) {
        jv_free(idx_jv);
        return mcp_tool_error("Missing required argument: index (integer)");
    }
    idx = (int)jv_number_value(idx_jv);
    jv_free(idx_jv);

    jv frames_jv = jv_object_get(jv_copy(params), jv_string("frames"));
    frames = jv_get_kind(frames_jv) == JV_KIND_NUMBER
               ? (int)jv_number_value(frames_jv) : 60;
    if (frames < 1) frames = 1;
    if (frames > 1000) frames = 1000;
    jv_free(frames_jv);

    jv w_jv = jv_object_get(jv_copy(params), jv_string("width"));
    width = jv_get_kind(w_jv) == JV_KIND_NUMBER
            ? (int)jv_number_value(w_jv) : 640;
    if (width < 1) width = 1;
    if (width > 4096) width = 4096;
    jv_free(w_jv);

    jv h_jv = jv_object_get(jv_copy(params), jv_string("height"));
    height = jv_get_kind(h_jv) == JV_KIND_NUMBER
             ? (int)jv_number_value(h_jv) : 480;
    if (height < 1) height = 1;
    if (height > 4096) height = 4096;
    jv_free(h_jv);

    mcp_ensure_scanned();
    if (idx < 0 || idx >= mcp_shader_count) {
        snprintf(ebuf, sizeof ebuf, "Shader index %d out of range (0-%d)",
                 idx, mcp_shader_count > 0 ? mcp_shader_count - 1 : 0);
        return mcp_tool_error(ebuf);
    }

    if (!mcp_gl_init())
        return mcp_tool_error("Could not create OpenGL context for rendering.");

    char logbuf[4096];
    GLuint prog = mcp_prepare_program(idx, logbuf, sizeof(logbuf));
    if (!prog) {
        char result[8192];
        snprintf(result, sizeof(result), "Shader compilation failed:\n%s",
                 logbuf[0] ? logbuf : "(no log available)");
        return mcp_tool_error(result);
    }

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteProgram(prog);
        glDeleteTextures(1, &tex);
        glDeleteFramebuffers(1, &fbo);
        return mcp_tool_error("Failed to create framebuffer object.");
    }

    GLuint vao, vbo;
    mcp_setup_quad(&vao, &vbo);

    GLuint chan_tex[4];
    float  chan_res[4][3];
    mcp_load_channels(idx, chan_tex, chan_res);
    mcp_bind_channels(chan_tex);

    Uint32 t0 = SDL_GetTicks();
    for (int i = 0; i < frames; i++)
        mcp_render_frame(prog, vao, fbo, width, height,
                         (float)i * 0.016f, i, chan_res);
    Uint32 t1 = SDL_GetTicks();
    double elapsed = (double)(t1 - t0) / 1000.0;
    double fps = elapsed > 0.0 ? (double)frames / elapsed : 0.0;

    mcp_free_channels(chan_tex);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(prog);

    char result[512];
    snprintf(result, sizeof(result),
             "FPS benchmark for shader: %s\n"
             "Frames rendered: %d\n"
             "Resolution: %dx%d\n"
             "Elapsed: %.3f seconds\n"
             "Average FPS: %.1f\n",
             mcp_shader_basename(mcp_shader_list[idx]),
             frames, width, height, elapsed, fps);
    return mcp_tool_text(result);
}
#endif /* MCP_HAS_GL */

#if MCP_HAS_GL
static jv tool_screenshot(jv params)
{
    int idx, width, height;
    float time_val;
    char ebuf[128];

    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) != JV_KIND_NUMBER) {
        jv_free(idx_jv);
        return mcp_tool_error("Missing required argument: index (integer)");
    }
    idx = (int)jv_number_value(idx_jv);
    jv_free(idx_jv);

    jv w_jv = jv_object_get(jv_copy(params), jv_string("width"));
    width = jv_get_kind(w_jv) == JV_KIND_NUMBER
            ? (int)jv_number_value(w_jv) : 640;
    if (width < 1) width = 1;
    if (width > 2048) width = 2048;
    jv_free(w_jv);

    jv h_jv = jv_object_get(jv_copy(params), jv_string("height"));
    height = jv_get_kind(h_jv) == JV_KIND_NUMBER
             ? (int)jv_number_value(h_jv) : 480;
    if (height < 1) height = 1;
    if (height > 2048) height = 2048;
    jv_free(h_jv);

    jv t_jv = jv_object_get(jv_copy(params), jv_string("time"));
    time_val = jv_get_kind(t_jv) == JV_KIND_NUMBER
               ? (float)jv_number_value(t_jv) : 0.0f;
    jv_free(t_jv);

    mcp_ensure_scanned();
    if (idx < 0 || idx >= mcp_shader_count) {
        snprintf(ebuf, sizeof ebuf, "Shader index %d out of range (0-%d)",
                 idx, mcp_shader_count > 0 ? mcp_shader_count - 1 : 0);
        return mcp_tool_error(ebuf);
    }

    if (!mcp_gl_init())
        return mcp_tool_error("Could not create OpenGL context for rendering.");

    char logbuf[4096];
    GLuint prog = mcp_prepare_program(idx, logbuf, sizeof(logbuf));
    if (!prog) {
        char result[8192];
        snprintf(result, sizeof(result), "Shader compilation failed:\n%s",
                 logbuf[0] ? logbuf : "(no log available)");
        return mcp_tool_error(result);
    }

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteProgram(prog);
        glDeleteTextures(1, &tex);
        glDeleteFramebuffers(1, &fbo);
        return mcp_tool_error("Failed to create framebuffer object.");
    }

    GLuint vao, vbo;
    mcp_setup_quad(&vao, &vbo);

    GLuint chan_tex[4];
    float  chan_res[4][3];
    mcp_load_channels(idx, chan_tex, chan_res);
    mcp_bind_channels(chan_tex);
    mcp_render_frame(prog, vao, fbo, width, height, time_val, 0, chan_res);
    mcp_free_channels(chan_tex);

    size_t npix = (size_t)width * (size_t)height * 4;
    unsigned char *pixels = malloc(npix);
    if (!pixels) {
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteTextures(1, &tex);
        glDeleteFramebuffers(1, &fbo);
        glDeleteProgram(prog);
        return mcp_tool_error("Out of memory for pixel buffer.");
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(prog);

    size_t png_size;
    char *png_data = mcp_encode_png(width, height, pixels, &png_size);
    free(pixels);
    if (!png_data)
        return mcp_tool_error("Failed to encode PNG image.");

    size_t b64_len;
    char *b64 = mcp_base64_encode((unsigned char *)png_data, png_size, &b64_len);
    free(png_data);
    if (!b64)
        return mcp_tool_error("Failed to base64-encode image.");

    /* Build MCP image content result */
    jv result = jv_object();
    jv content = jv_array();
    jv item = jv_object();
    item = jv_object_set(item, jv_string("type"), jv_string("image"));
    item = jv_object_set(item, jv_string("data"), jv_string(b64));
    item = jv_object_set(item, jv_string("mimeType"), jv_string("image/png"));
    content = jv_array_append(content, item);
    result = jv_object_set(result, jv_string("content"), content);

    /* Structured content with dimensions */
    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("width"),
                               jv_number(width));
    structured = jv_object_set(structured, jv_string("height"),
                               jv_number(height));
    structured = jv_object_set(structured, jv_string("format"),
                               jv_string("png (8-bit RGBA)"));
    result = jv_object_set(result, jv_string("structuredContent"), structured);

    free(b64);
    return result;
}

/* Run a shader with advancing time for a number of frames, report FPS,
   optionally capture the last frame as a BMP image. */
static jv tool_run_shaders(jv params)
{
    int idx, frames, width, height;
    char ebuf[128];

    jv idx_jv = jv_object_get(jv_copy(params), jv_string("index"));
    if (jv_get_kind(idx_jv) != JV_KIND_NUMBER) {
        jv_free(idx_jv);
        return mcp_tool_error("Missing required argument: index (integer)");
    }
    idx = (int)jv_number_value(idx_jv);
    jv_free(idx_jv);

    jv frames_jv = jv_object_get(jv_copy(params), jv_string("frames"));
    frames = jv_get_kind(frames_jv) == JV_KIND_NUMBER
               ? (int)jv_number_value(frames_jv) : 60;
    if (frames < 1) frames = 1;
    if (frames > 1000) frames = 1000;
    jv_free(frames_jv);

    jv dur_jv = jv_object_get(jv_copy(params), jv_string("duration"));
    double duration = jv_get_kind(dur_jv) == JV_KIND_NUMBER
                      ? jv_number_value(dur_jv) : 1.0;
    if (duration < 0.01) duration = 0.01;
    jv_free(dur_jv);

    jv w_jv = jv_object_get(jv_copy(params), jv_string("width"));
    width = jv_get_kind(w_jv) == JV_KIND_NUMBER
            ? (int)jv_number_value(w_jv) : 320;
    if (width < 1) width = 1;
    if (width > 4096) width = 4096;
    jv_free(w_jv);

    jv h_jv = jv_object_get(jv_copy(params), jv_string("height"));
    height = jv_get_kind(h_jv) == JV_KIND_NUMBER
             ? (int)jv_number_value(h_jv) : 240;
    if (height < 1) height = 1;
    if (height > 4096) height = 4096;
    jv_free(h_jv);

    jv cap_jv = jv_object_get(jv_copy(params), jv_string("captureFrame"));
    int capture = jv_get_kind(cap_jv) == JV_KIND_TRUE;
    jv_free(cap_jv);

    mcp_ensure_scanned();
    if (idx < 0 || idx >= mcp_shader_count) {
        snprintf(ebuf, sizeof ebuf, "Shader index %d out of range (0-%d)",
                 idx, mcp_shader_count > 0 ? mcp_shader_count - 1 : 0);
        return mcp_tool_error(ebuf);
    }

    if (!mcp_gl_init())
        return mcp_tool_error("Could not create OpenGL context for rendering.");

    char logbuf[4096];
    GLuint prog = mcp_prepare_program(idx, logbuf, sizeof(logbuf));
    if (!prog) {
        char result[8192];
        snprintf(result, sizeof(result), "Shader compilation failed:\n%s",
                 logbuf[0] ? logbuf : "(no log available)");
        return mcp_tool_error(result);
    }

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteProgram(prog);
        glDeleteTextures(1, &tex);
        glDeleteFramebuffers(1, &fbo);
        return mcp_tool_error("Failed to create framebuffer object.");
    }

    GLuint vao, vbo;
    mcp_setup_quad(&vao, &vbo);

    GLuint chan_tex[4];
    float  chan_res[4][3];
    mcp_load_channels(idx, chan_tex, chan_res);
    mcp_bind_channels(chan_tex);

    float dt = (float)(duration / (double)frames);
    Uint32 t0 = SDL_GetTicks();
    for (int i = 0; i < frames; i++)
        mcp_render_frame(prog, vao, fbo, width, height,
                         (float)i * dt, i, chan_res);
    Uint32 t1 = SDL_GetTicks();
    double elapsed = (double)(t1 - t0) / 1000.0;
    double fps = elapsed > 0.0 ? (double)frames / elapsed : 0.0;

    char stats[512];
    snprintf(stats, sizeof(stats),
             "Shader: %s\nIndex: %d\n"
             "Frames rendered: %d\n"
             "Duration: %.3f s\n"
             "Elapsed: %.3f seconds\n"
             "Average FPS: %.1f\n",
             mcp_shader_basename(mcp_shader_list[idx]),
             idx, frames, duration, elapsed, fps);

    jv result_obj = jv_object();
    jv content = jv_array();

    /* Add text content (FPS stats) */
    jv text_item = jv_object();
    text_item = jv_object_set(text_item, jv_string("type"), jv_string("text"));
    text_item = jv_object_set(text_item, jv_string("text"), jv_string(stats));
    content = jv_array_append(content, text_item);

    /* Optionally capture the last frame as an image */
    if (capture) {
        /* Render one more frame at the final time for the capture */
        mcp_render_frame(prog, vao, fbo, width, height,
                         (float)(frames - 1) * dt, frames - 1, chan_res);

        size_t npix = (size_t)width * (size_t)height * 4;
        unsigned char *pixels = malloc(npix);
        if (pixels) {
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            size_t png_size;
            char *png_data = mcp_encode_png(width, height, pixels, &png_size);
            free(pixels);
            if (png_data) {
                size_t b64_len;
                char *b64 = mcp_base64_encode(
                    (unsigned char *)png_data, png_size, &b64_len);
                free(png_data);
                if (b64) {
                    jv img_item = jv_object();
                    img_item = jv_object_set(img_item, jv_string("type"),
                                             jv_string("image"));
                    img_item = jv_object_set(img_item, jv_string("data"),
                                             jv_string(b64));
                    img_item = jv_object_set(img_item, jv_string("mimeType"),
                                             jv_string("image/png"));
                    content = jv_array_append(content, img_item);
                    free(b64);
                }
            }
        }
    }

    /* Set fully-built content array on the result object */
    result_obj = jv_object_set(result_obj, jv_string("content"), content);

    /* Structured content */
    jv structured = jv_object();
    structured = jv_object_set(structured, jv_string("fps"),
                               jv_number((double)fps));
    structured = jv_object_set(structured, jv_string("frames"),
                               jv_number(frames));
    structured = jv_object_set(structured, jv_string("elapsed"),
                               jv_number(elapsed));
    structured = jv_object_set(structured, jv_string("width"),
                               jv_number(width));
    structured = jv_object_set(structured, jv_string("height"),
                               jv_number(height));
    result_obj = jv_object_set(result_obj, jv_string("structuredContent"),
                               structured);

    mcp_free_channels(chan_tex);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(prog);

    return result_obj;
}
#endif /* MCP_HAS_GL */

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
          "List all available .glsl shader files in the shaders/ directory.",
          "{\"type\":\"object\",\"properties\":{}}" },
        { "read_shader",
          "Read the full source code of a shader by its 0-based index.",
          "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"0-based shader index from list_shaders\"}},\"required\":[\"index\"]}" },
        { "get_shader_info",
          "Get metadata about a shader (name, path, line count, byte size) by its 0-based index.",
          "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"index\"]}" },
        { "compile_shader",
          "Compile-check a GLSL shader for errors using an OpenGL context. Returns compiler log if failed.",
          "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0}},\"required\":[\"index\"]}" },
#if MCP_HAS_GL
        { "get_fps",
          "Benchmark a shader's rendering performance by rendering multiple frames to an offscreen framebuffer and measuring FPS.",
          "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0},\"frames\":{\"type\":\"integer\",\"minimum\":1,\"default\":60},\"width\":{\"type\":\"integer\",\"minimum\":1,\"default\":640},\"height\":{\"type\":\"integer\",\"minimum\":1,\"default\":480}},\"required\":[\"index\"]}" },
        { "screenshot",
          "Render a shader frame to an offscreen framebuffer and return it as a base64-encoded PNG image.",
          "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0},\"width\":{\"type\":\"integer\",\"minimum\":1,\"default\":640},\"height\":{\"type\":\"integer\",\"minimum\":1,\"default\":480},\"time\":{\"type\":\"number\",\"default\":0}},\"required\":[\"index\"]}" },
        { "run_shaders",
          "Run a shader with advancing time for a number of frames. Reports FPS and can optionally capture the last frame as an image.",
          "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"0-based shader index\"},\"frames\":{\"type\":\"integer\",\"minimum\":1,\"default\":60},\"duration\":{\"type\":\"number\",\"minimum\":0.01,\"default\":1.0,\"description\":\"Total time range in seconds\"},\"width\":{\"type\":\"integer\",\"minimum\":1,\"default\":320},\"height\":{\"type\":\"integer\",\"minimum\":1,\"default\":240},\"captureFrame\":{\"type\":\"boolean\",\"default\":false,\"description\":\"If true, capture the last frame as a base64 PNG image\"}},\"required\":[\"index\"]}" },
#endif
        { "write_shader",
          "Write a new GLSL shader file to the shaders/ directory.",
          "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Shader filename (with or without .glsl extension)\"},\"content\":{\"type\":\"string\",\"description\":\"GLSL fragment shader source code\"}},\"required\":[\"name\",\"content\"]}" },
        { "reload_shaders",
          "Rescan the shaders/ directory for new or changed shader files.",
          "{\"type\":\"object\",\"properties\":{}}" },
    };

    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
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
        return jv_invalid();  /* signal unknown tool to caller */
    }
    const char *name = jv_string_value(name_jv);

    jv args = jv_object_get(jv_copy(params), jv_string("arguments"));
    if (!jv_is_valid(args)) { jv_free(args); args = jv_object(); }

    jv result;

    if (strcmp(name, "list_shaders") == 0) {
        result = tool_list_shaders(args);
    } else if (strcmp(name, "read_shader") == 0) {
        result = tool_read_shader(args);
    } else if (strcmp(name, "get_shader_info") == 0) {
        result = tool_get_shader_info(args);
    } else if (strcmp(name, "compile_shader") == 0) {
        result = tool_compile_shader(args);
#if MCP_HAS_GL
    } else if (strcmp(name, "get_fps") == 0) {
        result = tool_get_fps(args);
    } else if (strcmp(name, "screenshot") == 0) {
        result = tool_screenshot(args);
    } else if (strcmp(name, "run_shaders") == 0) {
        result = tool_run_shaders(args);
#endif
    } else if (strcmp(name, "write_shader") == 0) {
        result = tool_write_shader(args);
    } else if (strcmp(name, "reload_shaders") == 0) {
        result = tool_reload_shaders(args);
    } else {
        jv_free(name_jv);
        jv_free(args);
        /* Unknown tool: not a protocol violation. Report it as a tool-level
         * error so clients can surface it without tearing down the session. */
        {
            char msg[512];
            snprintf(msg, sizeof msg, "Unknown tool: %s", name);
            return mcp_tool_error(msg);
        }
    }

    jv_free(name_jv);
    jv_free(args);
    return result;
}

/* ------------------------------------------------------------------ */
/* initialize handler                                                 */
/* ------------------------------------------------------------------ */

static jv handle_initialize(jv params)
{
    (void)params;
    jv result = jv_object();
    result = jv_object_set(result, jv_string("protocolVersion"),
                           jv_string(MCP_PROTOCOL_VERSION));

    jv caps = jv_object();
    caps = jv_object_set(caps, jv_string("tools"), jv_object());
    caps = jv_object_set(caps, jv_string("prompts"), jv_object());
    caps = jv_object_set(caps, jv_string("resources"), jv_object());
    caps = jv_object_set(caps, jv_string("completion"), jv_object());
    caps = jv_object_set(caps, jv_string("logging"), jv_object());
    result = jv_object_set(result, jv_string("capabilities"), caps);

    jv info = jv_object();
    info = jv_object_set(info, jv_string("name"), jv_string(MCP_SERVER_NAME));
    info = jv_object_set(info, jv_string("version"), jv_string(MCP_SERVER_VERSION));
    info = jv_object_set(info, jv_string("title"), jv_string(MCP_SERVER_TITLE));
    result = jv_object_set(result, jv_string("serverInfo"), info);

    result = jv_object_set(result, jv_string("instructions"),
        jv_string("MCP server for the Shadertoy-style GLSL shader viewer. "
                   "Provides tools for listing, reading, writing, "
                   "compiling, and benchmarking GLSL fragment shaders, "
                   "with optional headless rendering and screenshots."));

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
        if (has_id)
            mcp_send_error(id, -32600, "Invalid Request",
                           "Request must have a valid method");
        else
            jv_free(id);
        jv_free(method_jv);
        jv_free(request);
        return;
    }

    jv params = jv_object_get(jv_copy(request), jv_string("params"));
    if (!jv_is_valid(params)) { jv_free(params); params = jv_object(); }

    int is_notification =
        (strncmp(method, "notifications/", 14) == 0) ||
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
    } else if (strcmp(method, "prompts/list") == 0) {
        result = jv_object();
        result = jv_object_set(result, jv_string("prompts"), jv_array());
    } else if (strcmp(method, "resources/list") == 0) {
        result = jv_object();
        result = jv_object_set(result, jv_string("resources"), jv_array());
    } else if (strcmp(method, "resources/read") == 0) {
        result = jv_object();
        result = jv_object_set(result, jv_string("contents"), jv_array());
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

/* ------------------------------------------------------------------ */
/* Main MCP server entry point                                        */
/* ------------------------------------------------------------------ */

int mcp_run(void)
{
    mcp_ensure_scanned();

    while (!feof(stdin)) {
        char *body = mcp_read_message_body();
        if (!body)
            break;

        /* Skip whitespace-only bodies (stray blank lines between messages). */
        int blank = 1;
        for (const char *p = body; *p; p++)
            if (!isspace((unsigned char)*p)) { blank = 0; break; }
        if (blank) {
            free(body);
            continue;
        }

        jv request = jv_parse(body);
        free(body);

        if (!jv_is_valid(request)) {
            jv_free(request);
            mcp_send_error(jv_null(), -32700, "Parse error",
                           "Could not parse JSON request");
            continue;
        }

        mcp_handle_request(request);
    }

#if MCP_HAS_GL
    if (mcp_gl_ready)
        mcp_gl_cleanup();
#endif

    return 0;
}
