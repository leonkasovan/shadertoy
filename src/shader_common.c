/*
 * Shared shader plumbing. See shader_common.h for the contract.
 *
 * This file is compiled into both binaries (viewer and MCP server), so it must
 * not depend on anything application-specific: no globals, no logging to a
 * particular sink, no SDL. Only GLAD and libc.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader_common.h"

/* ------------------------------------------------------------------ */
/* Source assembly                                                    */
/* ------------------------------------------------------------------ */

static const char vs_source[] =
    GLSL_VERSION "\n"
    "layout(location = 0) in vec2 aPos;\n"
    "out vec2 vFragCoord;\n"
    "uniform vec2 uResolution;\n"
    "void main() {\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "    vFragCoord = (aPos * 0.5 + 0.5) * uResolution;\n"
    "}\n";

static const char frag_header[] =
    GLSL_VERSION "\n"
    "precision highp float;\n"
    "in vec2 vFragCoord;\n"
    "uniform vec3  iResolution;\n"
    "uniform float iTime;\n"
    "uniform float iTimeDelta;\n"
    "uniform int   iFrame;\n"
    "uniform float iFrameRate;\n"
    "uniform vec4  iMouse;\n"
    "uniform vec4  iDate;\n"
    "uniform sampler2D iChannel0;\n"
    "uniform sampler2D iChannel1;\n"
    "uniform sampler2D iChannel2;\n"
    "uniform sampler2D iChannel3;\n"
    "uniform vec3  iChannelResolution[4];\n"
    "uniform float iChannelTime[4];\n"
    "uniform float iSampleRate;\n"
    "out vec4 fragColor;\n";

static const char frag_footer[] =
    "\nvoid main() { mainImage(fragColor, vFragCoord); }\n";

const char *shader_vertex_source(void)    { return vs_source; }
const char *shader_fragment_header(void)  { return frag_header; }
const char *shader_fragment_footer(void)  { return frag_footer; }

char *shader_wrap_fragment(const char *body)
{
    if (!body) return NULL;
    size_t len = strlen(frag_header) + strlen(body) + strlen(frag_footer) + 1;
    char *out = malloc(len);
    if (!out) return NULL;
    snprintf(out, len, "%s%s%s", frag_header, body, frag_footer);
    return out;
}

GLuint shader_compile_stage(GLenum type, const char *src,
                            char *logbuf, size_t loglen)
{
    if (logbuf && loglen) logbuf[0] = '\0';

    GLuint s = glCreateShader(type);
    if (!s) {
        if (logbuf && loglen)
            snprintf(logbuf, loglen, "glCreateShader failed");
        return 0;
    }

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (logbuf && loglen)
            glGetShaderInfoLog(s, (GLsizei)loglen, NULL, logbuf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint shader_build_program(const char *body, char *logbuf, size_t loglen)
{
    if (logbuf && loglen) logbuf[0] = '\0';

    char *wrapped = shader_wrap_fragment(body);
    if (!wrapped) {
        if (logbuf && loglen) snprintf(logbuf, loglen, "Out of memory");
        return 0;
    }

    char vs_log[2048] = {0};
    char fs_log[2048] = {0};

    GLuint vs = shader_compile_stage(GL_VERTEX_SHADER, vs_source,
                                     vs_log, sizeof vs_log);
    GLuint fs = shader_compile_stage(GL_FRAGMENT_SHADER, wrapped,
                                     fs_log, sizeof fs_log);
    free(wrapped);

    GLuint prog = 0;
    GLint linked = 0;

    if (vs && fs) {
        prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    }

    if (!vs || !fs || !linked) {
        if (logbuf && loglen) {
            /* Prefer the most specific failure: fragment, then vertex, then
             * link. Report only the relevant one so callers get a terse log. */
            if (!fs && fs_log[0])
                snprintf(logbuf, loglen, "%s", fs_log);
            else if (!vs && vs_log[0])
                snprintf(logbuf, loglen, "%s", vs_log);
            else if (prog)
                glGetProgramInfoLog(prog, (GLsizei)loglen, NULL, logbuf);
            else
                snprintf(logbuf, loglen, "Shader compilation failed");
        }
        if (prog) glDeleteProgram(prog);
        if (vs)   glDeleteShader(vs);
        if (fs)   glDeleteShader(fs);
        return 0;
    }

    /* Shaders are attached to the program now; they can be dropped. */
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

/* ------------------------------------------------------------------ */
/* Uniforms                                                           */
/* ------------------------------------------------------------------ */

void shader_uniforms_lookup(GLuint prog, shader_uniforms *u)
{
    memset(u, 0, sizeof *u);
    u->iResolution        = glGetUniformLocation(prog, "iResolution");
    u->iTime              = glGetUniformLocation(prog, "iTime");
    u->iTimeDelta         = glGetUniformLocation(prog, "iTimeDelta");
    u->iFrame             = glGetUniformLocation(prog, "iFrame");
    u->iFrameRate         = glGetUniformLocation(prog, "iFrameRate");
    u->iMouse             = glGetUniformLocation(prog, "iMouse");
    u->iDate              = glGetUniformLocation(prog, "iDate");
    u->iSampleRate        = glGetUniformLocation(prog, "iSampleRate");
    u->uResolution        = glGetUniformLocation(prog, "uResolution");
    u->iChannelResolution = glGetUniformLocation(prog, "iChannelResolution");
    u->iChannelTime       = glGetUniformLocation(prog, "iChannelTime");

    for (int ch = 0; ch < 4; ch++) {
        char name[16];
        snprintf(name, sizeof name, "iChannel%d", ch);
        u->iChannel[ch] = glGetUniformLocation(prog, name);
    }
}

void shader_frame_defaults(shader_frame *f)
{
    memset(f, 0, sizeof *f);
    f->width = 640.0f;
    f->height = 480.0f;
    f->time = 0.0f;
    f->time_delta = 1.0f / 60.0f;
    f->frame_rate = 60.0f;
    f->date_valid = 0;
    for (int ch = 0; ch < 4; ch++)
        f->channel_time[ch] = f->time;
}

void shader_upload_uniforms(const shader_uniforms *u, const shader_frame *f)
{
    if (u->iResolution >= 0)
        glUniform3f(u->iResolution, f->width, f->height, SHADER_PIXEL_ASPECT);
    if (u->iTime >= 0)
        glUniform1f(u->iTime, f->time);
    if (u->iTimeDelta >= 0)
        glUniform1f(u->iTimeDelta, f->time_delta);
    if (u->iFrame >= 0)
        glUniform1i(u->iFrame, f->frame);
    if (u->iFrameRate >= 0)
        glUniform1f(u->iFrameRate, f->frame_rate);
    if (u->iMouse >= 0)
        glUniform4f(u->iMouse, f->mouse_x, f->mouse_y,
                    f->mouse_left, f->mouse_right);
    if (u->iDate >= 0 && f->date_valid)
        glUniform4f(u->iDate, f->date_year, f->date_month,
                    f->date_day, f->date_seconds);
    if (u->iSampleRate >= 0)
        glUniform1f(u->iSampleRate, 44100.0f);
    if (u->uResolution >= 0)
        glUniform2f(u->uResolution, f->width, f->height);
    if (u->iChannelResolution >= 0)
        glUniform3fv(u->iChannelResolution, 4,
                     (const float *)f->channel_resolution);
    if (u->iChannelTime >= 0)
        glUniform1fv(u->iChannelTime, 4, f->channel_time);

    /* Bind sampler uniforms to texture units 0..3. */
    for (int ch = 0; ch < 4; ch++)
        if (u->iChannel[ch] >= 0)
            glUniform1i(u->iChannel[ch], ch);
}
