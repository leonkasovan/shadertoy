# shadertoy-sdl2

A Shadertoy-style GLSL fragment shader viewer built with SDL2 (or GLFW) +
OpenGL 3.3, with a built-in **MCP server** for editing shaders via AI agents.

```
shadertoy.exe  (viewer + MCP server)        MCP client
        |  HTTP 127.0.0.1:8642/mcp               |
        |  write_shader / screenshot  <----------+
        +--> shaders/*.glsl  --hot-reload--> window
```

The viewer hosts the MCP server itself (Streamable HTTP), so tools render
with the window's own GL context. HTTP is the only MCP transport: there is
no stdio server, because a separate process could neither see the live
window nor drive it.

## Building

Requires SDL2 and libjq via pkg-config (w64devkit on Windows; add GLFW for
the alternate backend). With MSYS2:

```
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-jq
pacman -S mingw-w64-x86_64-glfw   # only for BACKEND=glfw
```

```sh
make                # release build -> build/shadertoy(.exe); viewer + MCP HTTP
make BACKEND=glfw   # GLFW backend   -> build/shadertoy-glfw(.exe)
make glfw           # alias for the previous line
make console        # console-subsystem viewer -> build/shadertoy-console(.exe)
make glfw-console   # console-subsystem GLFW build
make DEBUG=1        # -O0 -g, no strip -> *-debug binaries
make SAN=1          # opt-in AddressSanitizer (unsupported under MSVCRT-based toolchains)
```

`BACKEND=sdl2` is the default. Both backends share `viewer_core.c`,
`shader_common.c`, `shader_list.c`, `mcp.c`, and the CLI table in
`config.h`; only the window/input layer differs (`main-sdl2.c` vs
`main-glfw.c`). The split is enforced by structure, not by `#ifdef`: the core
is compiled by both backends with no backend conditionals in it, and each
window layer hands it a small `vc_backend_services` vtable (set the title,
report the window size) so the core never needs to know which toolkit is
underneath. The GLFW build still links SDL2 because `mcp.c` uses SDL for its
HTTP thread and sync primitives.

Two binaries appear in `build/` per backend:

| Binary                    | Subsystem    | Use |
|---------------------------|--------------|-----|
| `shadertoy.exe`           | Windows GUI (`-mwindows`) | interactive viewer + MCP |
| `shadertoy-console.exe`   | Console | same viewer, keeps stdout/stderr for terminal diagnostics |

Since MCP runs over HTTP inside the viewer, the GUI build needs no console.

### Dependencies (via pkg-config)

| Library   | Purpose                          |
|-----------|----------------------------------|
| SDL2      | Window + GL context management, MCP thread/sync |
| libjq     | JSON parsing/serialization (MCP) |
| GLFW      | Optional alternate window backend |
| OpenGL    | Shader rendering                 |

If libjq is missing, the viewer still builds with MCP disabled and prints a
warning when a port is requested.

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
  -p, --mcp-port <n>      viewer MCP HTTP port (default: 8642, 0 disables)
  -?, --help              show this help
```

### Channel textures (`iChannel0` .. `iChannel3`)

Every shader gets four samplers, `iChannel0`..`iChannel3`. Each channel is
filled from the first of these that matches:

1. **Explicit CLI file** — `-0/--texture0 <path>` … `-3/--texture3 <path>`.
   Used as given: no extension probing, no guessing.
2. **Auto-detected sibling** — `<stem>.<ch>.<ext>` next to the `.glsl` file,
   trying `png`, `jpg`, `jpeg`, `bmp`, `tga` **in that order** (first hit
   wins). `<stem>` is the shader path with a trailing lowercase `.glsl`
   stripped, so the files sit beside the shader and share its name.
3. **Nothing** — the channel is a 1x1 black texture and
   `iChannelResolution[ch]` is `vec3(0.0)`.

So for `shaders/rainier_mood.glsl` (which samples `iChannel0`) the texture
is simply `shaders/rainier_mood.0.png`:

```
shaders/
  rainier_mood.glsl        <- mainImage() body
  rainier_mood.0.png       <- binds to iChannel0
  rainier_mood.1.jpg       <- would bind to iChannel1
```

Both channels are checked in for `tribute_journey_opt.glsl`:

```sh
build/shadertoy.exe shaders/tribute_journey_opt.glsl   # picks up .0.png and .1.png
```

Explicit files override auto-detection per channel, and the two can be
mixed — this keeps `iChannel0` from the sibling PNG and feeds a different
image to `iChannel2`:

```sh
build/shadertoy.exe shaders/rainier_mood.glsl -2 shaders/tribute_journey.1.png
```

Relative paths are resolved from the viewer's working directory, so run it
from the project root or pass absolute paths.

#### How the textures are uploaded

Identical for the viewer and every MCP render path, because both call
`shader_load_channels()` in `shader_common.c`:

| Property | Value |
|----------|-------|
| Formats | PNG, JPEG, BMP, TGA (via `stb_image`) |
| Internal format | `GL_RGBA8`, always 4 channels |
| Mipmaps | Generated on load |
| Min / mag filter | `GL_LINEAR_MIPMAP_LINEAR` / `GL_LINEAR` |
| Wrap S / T | `GL_REPEAT` |
| Orientation | Vertically flipped on load, so `v = 0` is the image bottom (matches Shadertoy UVs) |
| `iChannelResolution[ch]` | `vec3(width, height, 1.0)` |
| `iChannelTime[ch]` | Equal to `iTime` in this viewer |

The samplers are always bound (a missing channel gets the shared 1x1 black
texture), so a shader never samples an unbound unit. Check
`iChannelResolution` when a channel is meant to be optional.

#### Keyboard state texture (`-k`)

The viewer can synthesise `iChannel<ch>` as a live keyboard instead of an
image:

```sh
build/shadertoy.exe shaders/some_shader.glsl -k 0    # keyboard on iChannel0
```

- Layout is 256x3 `GL_R8`, indexed by Shadertoy/JS key code:
  row 0 = currently held, row 1 = pressed this frame, row 2 = toggled.
- Read it with `texelFetch(iChannel0, ivec2(keyCode, 0), 0).x`.
- Codes follow the **JavaScript `keyCode`** values, not ASCII. Letters are
  uppercase regardless of Shift (`W` and `w` are both `87`, `A` is `65`), so
  `ivec2(87, 0)` works whether or not Shift is held. Digits are `48`..`57`,
  keypad digits `96`..`105`, F-keys `112`..`123`. Punctuation uses the JS
  numbers, which differ from ASCII: `;` is `186`, `=` is `187`, `-` is `189`.
  Both backends produce identical indices (verified by rendering the same
  key on each).
- Auto-repeat is ignored: holding a key does **not** re-fire row 1 or flip
  row 2. Toggled state changes exactly once per physical press.
- `iChannelResolution[ch]` is reported as `vec3(256.0, 3.0, 1.0)`.
- `-k 0` and `-0 <file>` target the same channel and are rejected at
  startup (`cannot bind texture and keyboard to the same channel`). An
  out-of-range or non-numeric value (`-k 7`, `-k abc`) silently leaves the
  keyboard unbound; `-k abc` parses as 0 and binds `iChannel0`.
- Row 0 clears on key release and row 1 is cleared after every frame, so
  "pressed this frame" is exactly one frame wide; row 2 persists. A key
  pressed *and* released between two polls is never seen as pressed.
- Keyboard channels exist only in the viewer. Offscreen MCP renders have no
  keyboard, so the channel falls back to black there.

#### When textures are (re)loaded

- At startup, and on every shader switch (`←`/`→`, `Space`, MCP
  `load_shader`) the channels are reloaded for the new shader.
- Adding `foo.0.png` **while the viewer is running** does not refresh the
  current shader's channel — switch away and back, or restart.
- MCP tools are the exception: `render_ctx_create()` reloads channels for
  every call, so a newly written sibling texture is picked up immediately by
  `screenshot`/`run_shaders`/`get_fps`.
- MCP renders never receive CLI textures (`--texture0..3` are viewer-only),
  so tool output uses auto-detected siblings only. Pass textures as siblings
  if screenshots must match the window.

#### Troubleshooting a black channel

- A failed `stbi_load` (corrupt file, unsupported format) is silent: the
  channel falls back to black with zero resolution. Verify the file opens.
- The auto-detect suffix is case-sensitive: `shaders/x.GLSL` counts as shader
  path `shaders/x.GLSL`, so it looks for `shaders/x.GLSL.0.png`. Keep the
  `.glsl` suffix lowercase (the browser also only scans lowercase `.glsl`).
- Channel numbers are 0-based in both the filename and the flag: `-0` →
  `iChannel0` → `<stem>.0.png`.

### MCP server (HTTP, hosted by the viewer)

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
The client-side config for every harness is in the next section.

## Registering the MCP client

Every client talks to the same endpoint, so the only variables are *where*
the config file lives and *what* the transport key is called.

**Prerequisite:** the viewer must be running (it is the server), and the URL
in the client config must match the port the viewer bound — `8642` unless
`-p/--mcp-port` says otherwise. `-p 0` disables the server entirely.

| Harness | Where to register | Transport key |
|---------|-------------------|---------------|
| OpenCode | `opencode.json` (project), `~/.config/opencode/opencode.json[c]` | `"mcp": { … "type": "remote", "url" }` |
| CommandCode | `cmd mcp add`, `~/.commandcode/mcp.json` | `"mcpServers": { … "transport": "http", "url" }` |
| OMP | `~/.omp/agent/mcp.json`, project `.omp/mcp.json` | `"mcpServers": { … "url" }` |
| pi | `~/.pi/agent/mcp.json`, project `.pi/mcp.json` | `"mcpServers": { … "url" }` |
| Claude Code | `claude mcp add`, project `.mcp.json` | `"mcpServers": { … "type": "http", "url" }` |
| Codex | `~/.codex/config.toml` | `[mcp_servers.shadertoy]` + `url` |
| Cursor | `~/.cursor/mcp.json`, `.cursor/mcp.json` | `"mcpServers": { … "url" }` |
| VS Code / Copilot | `.vscode/mcp.json` | `"servers": { … "type": "http", "url" }` |
| Cline | `cline_mcp_settings.json` | `"mcpServers": { … "type": "streamableHttp", "url" }` |
| Gemini CLI | `~/.gemini/settings.json` | `"mcpServers": { … "httpUrl" }` |

### OpenCode

Project config (`opencode.json` in the repo) — already checked in here:

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

User-wide, put the same `mcp` block in `~/.config/opencode/opencode.json`
(or merge it into the existing `opencode.jsonc`). A stdio-only entry uses
`"type": "local"` with a `command` array instead.

### CommandCode

```sh
cmd mcp add --transport http shadertoy http://127.0.0.1:8642/mcp
cmd mcp list
```

On Windows' own shells `cmd` is the command interpreter, so use the
`cmdc` alias the installer also provides:

```sh
# Git Bash / PowerShell on Windows
cmdc mcp add --transport http shadertoy http://127.0.0.1:8642/mcp
cmdc mcp list        # NAME / TYPE / SCOPE / AUTH / STATUS
cmdc mcp remove shadertoy
```

Flags (`--transport`, `--scope`, `--env`, `--header`) must precede the server
name. `--scope` defaults to `local` (project); use `--scope user` to store it
in `~/.commandcode/mcp.json`, which is what the snippet below shows. Note
that `mcp add` probes the endpoint and warns `Server unreachable` when the
viewer is not running, but still writes the entry — start the viewer, then
run `/mcp` in the session to connect. The equivalent `~/.commandcode/mcp.json`
entry is:

```json
{
  "mcpServers": {
    "shadertoy": {
      "transport": "http",
      "url": "http://127.0.0.1:8642/mcp",
      "enabled": true
    }
  }
}
```

`type` is accepted as an alias of `transport`. If OpenCode is already
configured, `cmd` → `/import opencode` copies the entry across.

### OMP

User config `~/.omp/agent/mcp.json`:

```json
{
  "mcpServers": {
    "shadertoy": {
      "url": "http://127.0.0.1:8642/mcp"
    }
  }
}
```

Project-scoped, use `.omp/mcp.json` (or `.omp/.mcp.json`). OMP also reads a
plain `mcp.json` / `.mcp.json` from the project root when
`mcp.enableProjectConfig: true` is set in `~/.omp/agent/config.yml`.
Optional keys per server: `enabled`, `timeout`, `headers`. Use `/mcp` inside
a session to inspect the connection, and restart the session after editing
the file.

### pi

```json
{
  "mcpServers": {
    "shadertoy": {
      "url": "http://127.0.0.1:8642/mcp",
      "description": "Shadertoy shader viewer/editor MCP server"
    }
  }
}
```

Add `"timeout": 120` if screenshots of heavy shaders exceed the default 60 s.
Check it with `pi mcp list` after editing, and `/reload` inside a running
session.

### Claude Code / Codex / Cursor / VS Code / Cline / Gemini CLI

```sh
# Claude Code
claude mcp add --transport http shadertoy http://127.0.0.1:8642/mcp

# Codex (~/.codex/config.toml)
#   [mcp_servers.shadertoy]
#   url = "http://127.0.0.1:8642/mcp"
```

Cursor (`~/.cursor/mcp.json` or `.cursor/mcp.json`):

```json
{ "mcpServers": { "shadertoy": { "url": "http://127.0.0.1:8642/mcp" } } }
```

VS Code / Copilot (`.vscode/mcp.json`, note `servers` not `mcpServers`):

```json
{
  "servers": {
    "shadertoy": { "type": "http", "url": "http://127.0.0.1:8642/mcp" }
  }
}
```

Cline (`cline_mcp_settings.json`):

```json
{
  "mcpServers": {
    "shadertoy": { "type": "streamableHttp", "url": "http://127.0.0.1:8642/mcp" }
  }
}
```

Gemini CLI (`~/.gemini/settings.json`) uses `httpUrl` for streamable HTTP:

```json
{
  "mcpServers": {
    "shadertoy": { "httpUrl": "http://127.0.0.1:8642/mcp" }
  }
}
```

### Clients that can only spawn a process

Some harnesses have no HTTP transport. Point them at
`tools/shadertoy-mcp-bridge.py`, which forwards stdio to the viewer and fails
with an actionable message when the viewer is not up:

```json
{
  "mcpServers": {
    "shadertoy": {
      "command": "python",
      "args": ["C:/Projects/shadertoy/tools/shadertoy-mcp-bridge.py"],
      "env": { "SHADERTOY_MCP_URL": "http://127.0.0.1:8642/mcp" },
      "description": "Shadertoy shader viewer/editor MCP server"
    }
  }
}
```

The bridge is client-side only — the server still speaks HTTP and nothing
about the viewer changes. `--url` overrides the environment variable, and
`--probe-timeout` (default 2 s) controls how long it waits at startup.

### Troubleshooting: "shadertoy.exe must be running"

The viewer is the MCP server, so a client cannot start it. If a client
reports that the server is unreachable, start the viewer first:

```sh
build\shadertoy.exe            # Windows
build/shadertoy                # Linux/macOS
```

It logs `MCP server listening on http://127.0.0.1:8642/mcp` once the port is
open; then reconnect the client (`/mcp` → reconnect, or restart the session).

With the direct `url` config most clients only show a generic network error.
The bridge makes the diagnosis explicit and exits non-zero at startup:

```
shadertoy MCP: shadertoy.exe must be running.
Nothing is listening on http://127.0.0.1:8642/mcp (Connection refused).

Start the viewer first, e.g.:
  build\shadertoy.exe          (Windows)
  build/shadertoy              (Linux/macOS)
```

Check the endpoint by hand with:

```sh
python tools/shadertoy-mcp-bridge.py --check
python tools/shadertoy-mcp-bridge.py --check --url http://127.0.0.1:8642/mcp
```

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
| `load_shader` | Yes | Switch the visible viewer to another shader |
| `get_viewer_status` | No | Current shader, index, window size, FPS |

\* write/update try to compile; if the GL context cannot be created they say
`Compile check: skipped`, and the write still happens.

Because every tool runs inside the viewer, `load_shader`,
`get_viewer_status`, and `screenshot {"window": true}` are always available.

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
rather than a JSON-RPC protocol error. Non-object messages (including
JSON-RPC batches) get `-32600` and malformed JSON gets `-32700`; neither
kills the server.

## Codebase memory (symbol graph)

The repository is indexed into a codebase-memory graph — symbols, calls,
includes — so an agent can navigate with graph queries instead of
re-reading files. Nothing in the build or the viewer depends on it; it is
an inspection aid that lives beside the source.

**Index once per checkout** (`index_repository`):

| Field | Value |
|-------|-------|
| `repo_path` | `C:/Projects/shadertoy` |
| `mode` | `full` (everything + semantics) |
| project id | `C-Projects-shadertoy` |

`repo_path` must be the Windows form. The MSYS form `/c/Projects/shadertoy`
works in the shell but fails with `Pipeline failed`; `mode: "fast"` fails the
same way. A CLI session has no file watcher (`index_status` reports
`watch: watched: false`), so re-run the index after a large refactor rather
than expecting it to follow edits.

**Querying** — every tool takes `project: "C-Projects-shadertoy"`:

| Tool | Use it for |
|------|------------|
| `search_graph` | find symbols by regex name / BM25 / semantic query |
| `get_code_snippet` | read one symbol by qualified name |
| `get_file_outline` | declaration outline of one exact file |
| `trace_path` | callers or callees, depth-limited, tests excluded by default |
| `query_graph` | multi-hop and aggregate Cypher (`CALLS`, `INHERITS`, …) |
| `get_architecture` | packages, entry points, hotspots, layers, cycles |
| `detect_changes` | map a `git diff` to symbols and their blast radius |
| `index_status` / `check_index_coverage` | freshness and coverage gaps |

**What is not in the graph.** Excluded by design: `.git/`, `build/`,
`tools/`, `*.o` and `*.exe`. `glad/glad.h`, `glad_es/glad.h` and the
`Makefile` only parse partially, so their symbols can be missing or stale.
When the graph disagrees with the source — and for anything under `tools/`,
notably `tools/shadertoy-mcp-bridge.py` — fall back to `rg`. Use `rg` for all
content searches, never `grep`: the `grep` on PATH on this machine is BusyBox
and rejects common flags (`--null`, `-P`, …).

The index is kept in the codebase-memory store outside the repo. Passing
`persistence: true` writes `.codebase-memory/` as well; that directory is
gitignored (like `.codegraph/`, the older CLI indexer's database) and is
never committed.

## Project structure

```
src/
  main-sdl2.c     — SDL2 window/input layer (default backend): window + GL
                    context, event pump, SDL scancode → JS keyCode table
  main-glfw.c     — GLFW window/input layer, behavioural twin of main-sdl2.c
  viewer_core.c   — backend-independent viewer core: shader list + current
                    shader state, hot reload, shader switching, channel
                    textures, MCP viewer hooks, keyboard state buffer
                    (single source of truth for both backends)
  viewer_core.h   — its API, incl. the vc_backend_services vtable
  mcp.c           — MCP server: JSON-RPC/tools, HTTP transport, PNG encoder
  mcp.h           — viewer hooks + HTTP lifecycle
  shader_common.c — shared GLSL header/footer, program builder, uniforms,
                    file IO, channel texture loading (single source of truth)
  shader_common.h — its API
  shader_list.c   — shared dynamic shader-directory scanner
  shader_list.h   — its API
  config.h        — CLI options table (shared by both backends)
tools/
  shadertoy-mcp-bridge.py — optional client-side stdio <-> HTTP bridge;
                    reports "shadertoy.exe must be running" when the viewer
                    is not up (the server itself has no stdio mode)
glad/       — GLAD OpenGL loader (desktop GL)
glad_es/    — GLAD OpenGL ES loader (ARM / aarch64)
stb_image.h — texture loading (single-header)
shaders/    — GLSL fragment shaders (.glsl) + optional <name>.<ch>.png textures
Makefile    — GNU Make build
.codegraph/ — legacy CLI symbol-index DB (gitignored; not part of the build)
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
shaders render the same offscreen as in the window. Offscreen renders have
`iMouse = 0` and no `iDate`, so interactive or date-dependent shaders differ.
`iChannel0..3` are described in [Channel textures](#channel-textures-ichannel0--ichannel3).

## Offscreen rendering

The MCP tools render into their own framebuffer object (FBO), using the
viewer's GL context on the viewer's main thread. There is no hidden window or
second context. If no GPU/display is available the viewer itself cannot run,
so the GL tools simply never get a chance to fail on context creation;
read/write/list tools keep working as long as the viewer runs. Channels
without a texture get a 1x1 black texture and are reported as zero in
`iChannelResolution`; offscreen renders have `iMouse = 0` and no `iDate`, so
both stay at their GLSL defaults. Sibling textures are re-read on every tool
call, but CLI textures (`--texture0..3`) and the keyboard channel are
viewer-only. Each offscreen frame is cleared to opaque
black first, matching the viewer, and `run_shaders` sets `iTimeDelta` to
`duration / frames` (so `iTime` advances by the same step). `screenshot
{"window": true}` captures the actual frame instead, including
mouse/keyboard/date state.

## Known limitations

- Offscreen screenshots (the default) have `iMouse = 0`, no `iDate`, and no
  keyboard texture. Use `window: true` to capture the real frame.
- A client cannot start the viewer. Tools are unavailable until
  `shadertoy.exe` runs and the MCP port is open; the optional bridge turns
  that into a clear message, direct `url` configs only see a network error.
- The viewer-hosted server binds `127.0.0.1` only; remote MCP clients need a
  local client or a tunnel. Quitting the viewer disconnects the client.
- `get_fps` renders with a fixed 16 ms timestep, so numbers are throughput
  measurements, not frame-accurate simulation.
- PNG output is a small built-in encoder (zlib stored blocks). Valid but
  uncompressed.
- `make SAN=1` needs libasan; MSVCRT-based toolchains (w64devkit) don't
  ship it. On Linux or MSYS2 clang64/mingw toolchains it works.
- The MCP server resolves `shaders/` relative to the viewer's CWD; start the
  viewer with the project directory as the working directory.
