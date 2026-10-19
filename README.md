# shadertoy-sdl2

A Shadertoy-style GLSL fragment shader viewer built with SDL2 + OpenGL 3.3,
and a companion **MCP server** (`--mcp` / `-m`) that exposes shader tools
over JSON-RPC 2.0 on stdio.

## Building

Requires a MinGW-w64 cross-compiler (w64devkit), SDL2, and libjq.

```sh
make            # release build with -mwindows (GUI subsystem)
make console    # console-subsystem build — use this for --mcp
make DEBUG=1    # -O0 -g, no stripping
make SAN=1      # additionally enable AddressSanitizer (opt-in; some
                # toolchains such as w64devkit ship no libasan)
```

Two binaries are produced in `build/`:

| Binary | Subsystem | Use |
|---|---|---|
| `shadertoy.exe` | Windows GUI (`-mwindows`) | interactive viewer |
| `shadertoy-console.exe` | Console | `--mcp` server, or viewer with stdout |

The GUI build has stdout detached, so **use the console build for MCP**.
`make mcp` is an alias for `make console`.

### Dependencies (via pkg-config)

| Library   | Purpose                         |
|-----------|---------------------------------|
| SDL2      | Window + GL context management  |
| libjq     | JSON parsing/serialization (MCP)|
| OpenGL    | Shader rendering                |

If libjq is missing, the build warns and produces a viewer without MCP
support.

## Usage

### Interactive viewer

```sh
build/shadertoy.exe shaders/some_shader.glsl
build/shadertoy.exe -s shaders/some_shader.glsl -w 1280 -h 720
build/shadertoy.exe            # browse everything in shaders/
```

Passing `-s` loads exactly that one shader and prints a note that arrow-key
browsing is disabled (the cycle keys have nothing to cycle through). Omit `-s`
to browse the whole directory.

```
Usage: shadertoy [OPTIONS] [SHADER_PATH]

Options:
  -s, --source <path>     fragment shader source file
  -w, --width <value>     window width (default: 640)
  -h, --height <value>    window height (default: 480)
  -x, --window_x <value>  window X position
  -y, --window_y <value>  window Y position
  -f, --fullscreen        fullscreen mode
  -0, --texture0 <path>   texture for iChannel0
  -1, --texture1 <path>   texture for iChannel1
  -2, --texture2 <path>   texture for iChannel2
  -3, --texture3 <path>   texture for iChannel3
  -k, --keyboard <ch>     bind keyboard state texture to iChannel<ch>
  -m, --mcp               run as an MCP server on stdio
      --mcp-lsp           run as MCP server, reply with Content-Length framing
  -?, --help              show this help
```

> **`-h` is `--height`, not help.** Use `-?` or `--help`. A bare `-h` with no
> numeric value prints an explanatory error instead of the usage text.

With no arguments the viewer loads every `.glsl` in `./shaders`, starting on
the first one that compiles. `←`/`→`, `↑`/`↓`, `PageUp`/`PageDown`, space and
backspace cycle through shaders; `q` or `Esc` quits. Uncompilable shaders are
skipped with a message rather than aborting the session.

The shader list is sized dynamically — there is no cap on how many shaders a
directory may contain. Paths are sorted, so indices in the viewer and in
`list_shaders` agree with each other and with `ls`.

Channel textures are auto-detected next to the shader: `foo.glsl` picks up
`foo.0.png`, `foo.1.jpg`, etc. for `iChannel0..3`.

### MCP server mode

```sh
build/shadertoy-console.exe --mcp          # newline-delimited JSON (default)
build/shadertoy-console.exe --mcp --mcp-lsp  # Content-Length framing
```

It speaks JSON-RPC 2.0 over stdio. **The default framing is the MCP stdio
transport: one JSON object per line.** That is what Claude Desktop, VS Code
and other MCP clients send. Incoming `Content-Length:` framing is also
accepted, and `--mcp-lsp` switches *replies* to that style for LSP-shaped
clients.

Example session:

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | build/shadertoy-console.exe --mcp
```

## MCP Tools

| Tool            | Requires GL | Description |
|-----------------|-------------|-------------|
| `list_shaders`  | No          | List all `.glsl` files in `shaders/` |
| `read_shader`   | No          | Read shader source by index |
| `get_shader_info` | No        | Metadata (name, path, line count, byte size) |
| `compile_shader`| Yes         | Compile-check a shader, returns compiler log |
| `get_fps`       | Yes         | Benchmark rendering FPS for a shader |
| `screenshot`    | Yes         | Render a frame, return as base64-encoded **PNG** |
| `run_shaders`   | Yes         | Run shader with advancing time, report FPS, optional frame capture (PNG) |
| `write_shader`  | No          | Create a new `.glsl` shader file in `shaders/` |
| `reload_shaders`| No          | Rescan `shaders/` directory |

Images are returned as PNG (`image/png`), which MCP clients can display
directly.

An unknown tool name is reported as a normal tool error
(`isError: true`) rather than a JSON-RPC protocol error.

### Tool parameters

**get_fps:** `index` (integer, required), `frames` (integer, default 60),
`width` (integer, default 640), `height` (integer, default 480). Renders
`frames` frames with fixed 16 ms steps and reports throughput.

**run_shaders:** `index` (integer, required), `frames` (integer, default 60),
`width` (integer, default 320), `height` (integer, default 240),
`duration` (number, default 1.0, seconds), `captureFrame` (boolean, default false).

**screenshot:** `index` (required), `width` (default 640), `height` (default 480),
`time` (number, default 0).

**write_shader:** `name` (string, required), `content` (string, required).
`name` must be a plain filename — path separators, `..` and drive/URI
prefixes are rejected so writes cannot escape `shaders/`.

## Project structure

```
src/
  main.c          — SDL2 viewer, CLI parsing, --mcp dispatch
  mcp.c           — MCP server: JSON-RPC framing, tool implementations
  mcp.h           — MCP entry point + framing selection
  shader_common.c — shared GLSL header/footer, program build, uniforms
  shader_common.h — shader_common API
  shader_list.c   — shared dynamic shader-directory scanner
  shader_list.h   — shader_list API
  config.h        — CLI options table
glad/       — GLAD OpenGL loader (desktop GL)
glad_es/    — GLAD OpenGL ES loader (ARM / aarch64)
stb_image.h — texture loading (single-header)
shaders/    — GLSL fragment shaders (.glsl) + optional <name>.<ch>.png textures
Makefile    — GNU Make build
```

`shader_common.c` is the single source of truth for what a Shadertoy shader
receives: the `#version` line, the fullscreen-quad vertex shader, the injected
`iXXX` uniform header, the `mainImage()` footer, and the per-frame uniform
upload. Both the viewer and the MCP renderer call it, so a shader cannot render
differently depending on which code path compiled it. If you add a uniform,
add it there.

### How the shader wrapper works

Every `.glsl` file is a bare Shadertoy body — just `mainImage()` and helpers.
It is wrapped as:

```glsl
#version 330 core          // or 300 es on GLES
precision highp float;
in  vec2  vFragCoord;
uniform vec3  iResolution;  // ... iTime, iTimeDelta, iFrame, iFrameRate,
uniform vec4  iMouse;       //     iMouse, iDate, iSampleRate,
uniform sampler2D iChannel0; //    iChannel0..3, iChannelResolution[4],
                            //     iChannelTime[4]
out vec4 fragColor;

<your .glsl file verbatim>

void main() { mainImage(fragColor, vFragCoord); }
```

## Headless rendering

The MCP server creates a hidden, offscreen SDL/OpenGL context on demand
for tools that need GL (compile_shader, get_fps, screenshot, run_shaders).
No GPU/display access is required at startup; the context is initialized
lazily and cleaned up on exit.

Note that headless rendering uses the same wrapper and the same uniform
uploader as the viewer, and auto-detects `<shader>.<ch>.<ext>` channel textures
the same way. Channels without a texture are bound to a 1×1 black texture and
reported as zero in `iChannelResolution`. Because there is no window, `iMouse`
is always zero in headless renders.

## Known limitations

These are real, currently unfixed issues — not rough edges in the docs.

### `make SAN=1` does not work on w64devkit

AddressSanitizer is opt-in via `SAN=1` and is **not** part of the default
debug build. It requires a toolchain that ships `libasan`; the bundled
w64devkit MinGW does not, so `make SAN=1` fails at link time with
`cannot find -lasan`. This is intentional — a hard dependency would have
broken `make DEBUG=1` — but it means there is currently no working memory
sanitizer path on the recommended Windows toolchain. On a Linux/glibc host
(or MSYS2 with `mingw-w64-x86_64-gcc` plus the sanitizer runtime) `SAN=1`
works as expected.

### Minor

- `get_fps` renders with a fixed 16 ms timestep regardless of wall-clock time,
  so its numbers are throughput measurements, not a frame-accurate simulation.
- Screenshots are encoded by a small built-in PNG writer (zlib stored blocks,
  filter 0). Output is valid but uncompressed — expect a larger file than a
  normal PNG encoder would produce.
