# shadertoy-sdl2

A Shadertoy-style GLSL fragment shader viewer built with SDL2 + OpenGL 3.3,
with a built-in **MCP server** for editing shaders via AI agents.

```
shadertoy.exe  (viewer + MCP server)        MCP client
        |  HTTP 127.0.0.1:8642/mcp               |
        |  write_shader / screenshot  <----------+
        +--> shaders/*.glsl  --hot-reload--> window
```

The viewer hosts the MCP server itself (Streamable HTTP), so tools render
with the window's own GL context. `shadertoy-console.exe --mcp` remains
available as a standalone headless stdio server (architecture A) for
headless/CI use.

## Building

Requires SDL2 and libjq via pkg-config (w64devkit on Windows:

```
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-jq
```

if using MSYS2).

```sh
make            # release build -> build/shadertoy(.exe); viewer + MCP HTTP
make console    # console-subsystem build -> build/shadertoy-console(.exe); standalone --mcp
make mcp        # alias for make console
make DEBUG=1    # -O0 -g, no strip -> build/shadertoy-debug(.exe), build/shadertoy-debug-console(.exe)
make SAN=1      # opt-in AddressSanitizer (unsupported under MSVCRT-based toolchains)
```

Two binaries appear in `build/`:

| Binary              | Subsystem    | Use |
|---------------------|-----------|---|
| `shadertoy.exe`     | Windows GUI (`-mwindows`)  | interactive viewer |
| `shadertoy-console.exe` | Console | `--mcp` server, or viewer with stdout |

The viewer hosts MCP over HTTP, so the GUI build needs no console. The
console build is only required for standalone stdio `--mcp`.

### Dependencies (via pkg-config)

| Library   | Purpose                         |
|-----------|---------------------------------|
| SDL2      | Window + GL context management  |
| libjq     | JSON parsing/serialization (MCP)|
| OpenGL    | Shader rendering                |

If libjq is missing, the viewer still builds; `--mcp` exits with an error
asking for libjq instead of failing the build.

## Usage

### Interactive viewer

```sh
build/shadertoy.exe shaders/some_shader.glsl
build/shadertoy.exe -s shaders/some_shader.glsl -w 1280 -h 720
build/shadertoy.exe            # browse everything in shaders/
```

Passing `-s` loads exactly that one shader and prints a note that arrow-key
browsing is disabled. Without `-s` the viewer loads every `.glsl` starting on
the first that compiles; `←`/`→`, `↑`/`↓`, `PageUp`/`PageDown` cycle; `q`/`Esc`
quits.

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
  -m, --mcp               run as a standalone headless MCP server on stdio
      --mcp-lsp           MCP server replying with Content-Length framing
  -p, --mcp-port <n>      viewer MCP HTTP port (default: 8642, 0 disables)
  -?, --help              show this help
```

Channel textures auto-detect next to the shader: `foo.glsl` picks up
`foo.0.png`, `foo.1.jpg`, etc. (if not explicitly set with `--texture<ch>`).
The viewer and the MCP renderer use the same loader.

### MCP server mode (viewer-hosted, default)

1. Start the viewer from the project root (it serves MCP automatically):

   ```sh
   build\shadertoy.exe "shaders\Paralax Cloud.glsl"
   ```

2. Point the MCP client at `http://127.0.0.1:8642/mcp` (config below).
3. Edits appear live; tools run on the viewer's main thread between frames,
   so `screenshot` with `"window": true` captures exactly what is on screen.

Only one viewer can own the port: a second instance exits with a clear
error rather than silently sharing. `--mcp-port 0` disables the server;
`--mcp-port <n>` moves it. The bound port and pid are written to
`build/.shadertoy-mcp`, and diagnostics go to `build/shadertoy-mcp.log`
(the GUI subsystem has no console). Quitting the viewer stops the server.

The **standalone headless server** (architecture A) is still available for
CI or machines with no display; it speaks stdio and never shows a window:

```sh
build/shadertoy-console.exe --mcp              # one JSON object per line
build/shadertoy-console.exe --mcp --mcp-lsp    # Content-Length framed replies
```

Incoming `Content-Length:` framing is accepted either way; `--mcp-lsp` makes
**replies** Content-Length framed for LSP-shaped clients. Launch it from the
project root so `shaders/` resolves. Example:

```sh
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  | build/shadertoy-console.exe --mcp
```

### opencode / Claude / VS Code config

For the viewer-hosted server (start the viewer first):

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "shadertoy": {
      "type": "remote",
      "url": "http://127.0.0.1:8642/mcp",
      "enabled": true
    }
  }
}
```

For the standalone server, use `"type": "local"` with
`"command": ["build/shadertoy-console.exe", "--mcp"]` instead.

## MCP Tools

All tools accept a shader id as `name` (plain filename, with or without
`.glsl`, so use `name: "2D Clouds.glsl"`) or `index` (0-based, from
`list_shaders`) — whichever is most convenient.

| Tool | GL required | Purpose |
|------|-------------|---------|
| `list_shaders` | No | Files in `shaders/` (text + JSON in `structuredContent.items`) |
| `read_shader` | No | Full source of a shader |
| `get_shader_info` | No | Name, path, line count, bytes |
| `compile_shader` | Yes | Compile-only check; compiler log if failing |
| `get_fps` | Yes | Benchmark: N frames at fixed 16 ms steps |
| `screenshot` | Yes | Base64 PNG: offscreen frame at time `t`, or `window: true` for the live viewer backbuffer |
| `run_shaders` | Yes | Advance time over N frames, FPS stats; optional PNG |
| `write_shader` | No\* | **Write + immediately compile-check in one call** |
| `update_shader` | No\* | Same, updating an existing shader (by name/index) |
| `reload_shaders` | No | Rescan `shaders/` |
| `load_shader` | Yes | Switch the visible viewer to another shader (viewer-hosted only) |
| `get_viewer_status` | No | Current shader, index, window size, FPS (viewer-hosted only) |

\* write/update try to compile; if the GL context cannot be created they say
`Compile check: skipped`, and the write still happens.

`load_shader`, `get_viewer_status`, and `screenshot {"window": true}` return
an `isError` tool result when called against the standalone `--mcp` server.

### The agent edit loop

The big design point for agent interactivity: `write_shader` and
`update_shader` **write AND compile in the same round trip**. A bad shader
comes back with `isError: true` and the compiler log inline; the agent can
immediately call `screenshot` and see the result, no bureaucracy in
between.

Params: every tool takes the shader id plus view/resolution options that
default sensibly (`frames`, `width`, `height`, `time`, `duration`,
`captureFrame`). `write_shader(name, content)` uses a plain filename with
the `.glsl` suffix appended when missing.

An unknown tool name is reported as a normal tool error (`isError: true`)
rather than a JSON-RPC protocol error.

## Project structure

```
src/
  main.c          — SDL2 viewer (window, input, hot reload); hosts the
                    viewer MCP hooks and --mcp dispatch
  mcp.c           — MCP server: JSON-RPC/tools, stdio framing, HTTP transport
  mcp.h           — mcp_run(), viewer hooks, HTTP lifecycle
  shader_common.c — shared GLSL header/footer, program builder, uniforms,
                    file IO, channel texture loading (single source of truth)
  shader_common.h — its API
  shader_list.c   — shared dynamic shader-directory scanner
  shader_list.h   — its API
  config.h        — CLI options table
glad/       — GLAD OpenGL loader (desktop GL)
glad_es/    — GLAD OpenGL ES loader (ARM / aarch64)
stb_image.h — texture loading (single-header)
shaders/    — GLSL fragment shaders (.glsl) + optional <name>.<ch>.png textures
Makefile    — GNU Make build
```

### What a Shadertoy shader is (per this project)

Every `.glsl` is a bare Shadertoy body (`mainImage()` and helpers). It is
wrapped by `shader_common.c`:

```glsl
#version 330 core          // or 300 es on GLES
precision highp float;
in  vec2  vFragCoord;
uniform vec3  iResolution;   // ... iTime, iTimeDelta, iFrame, iFrameRate,
uniform vec4  iMouse;        //     iMouse, iDate, iSampleRate, ...
uniform sampler2D iChannel0; //     iChannel0..3, iChannelResolution, iChannelTime
out vec4  fragColor;

<your .glsl verbatim>

void main() { mainImage(fragColor, vFragCoord); }
```

The wrapper is shared by the viewer and every MCP render path, so time-only
shaders render the same offscreen as in the window. Headless renders have
`iMouse = 0` and no `iDate`, so interactive or date-dependent shaders differ.

## Headless rendering

The standalone server (`--mcp`) creates a hidden SDL/GL context lazily, at
tool-call time. The viewer-hosted server reuses the window's context instead.
If no GPU/display is available the GL tools fail with a clean message;
read/write/list tools keep working. Channels without a texture get a 1x1
black texture and are reported as zero in `iChannelResolution`; offscreen
renders have `iMouse = 0` and no `iDate`, so both stay at their GLSL
defaults. `screenshot {"window": true}` in viewer mode captures the actual
frame, including mouse/keyboard/date state.

## Known limitations

- Offscreen screenshots (the default, and the only kind in `--mcp` mode) have
  `iMouse = 0`, no `iDate`, and no keyboard texture. Use `window: true` in
  viewer mode to capture the real frame.
- The viewer-hosted server binds `127.0.0.1` only; remote MCP clients need a
  local client or a tunnel. Quitting the viewer disconnects the client.
- `get_fps` renders with a fixed 16 ms timestep, so numbers are throughput
  measurements, not frame-accurate simulation.
- PNG output is a small built-in encoder (zlib stored blocks). Valid but
  uncompressed.
- `make SAN=1` needs libasan; MSVCRT-based toolchains (w64devkit) don't
  ship it. On Linux or MSYS2 clang64/mingw toolchains it works.
- The MCP server resolves `shaders/` relative to its own CWD; run it with
  the project directory as the working directory (MCP clients do this).
