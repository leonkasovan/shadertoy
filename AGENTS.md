# Agent notes

- Always use `rg` for content searches, never `grep`. The `grep` on PATH is
  BusyBox and rejects common flags (`--null`, `-P`, etc.). The same applies to
  commands you put in docs, scripts and CI: `rg` only.
- This repo is indexed in codebase-memory as project `C-Projects-shadertoy`.
  Index with `repo_path: "C:/Projects/shadertoy"` — the MSYS form
  `/c/Projects/shadertoy` fails with `Pipeline failed` at any mode. `tools/`
  is excluded from the index, so read the bridge and the flagged `glad*`
  headers with `rg`. See the README section "Codebase memory".
- Windows tools need single-slash flags, but MSYS rewrites them into paths,
  so prefix with `MSYS2_ARG_CONV_EXCL='*'`:
  `MSYS2_ARG_CONV_EXCL='*' taskkill /F /IM shadertoy-console.exe`. Without it
  taskkill gets `F:/` and fails with `Invalid argument/option - 'F:/'`.
  (Older notes claimed plain `/F` worked; it does not under this shell.) A
  failed kill leaves the old viewer holding the MCP port, so screenshots and
  tool calls silently hit the stale process — check `tasklist | rg shadertoy`
  when results look impossible.
- **Shared core, thin backends.** Do not put shared viewer logic in a
  `main-*.c`. `src/viewer_core.c` owns the shader list, current-shader
  state, hot-reload bookkeeping, channel textures, shader switching, the
  MCP viewer hooks, and the keyboard state buffer — one implementation
  compiled by both backends. Only the window/input layer is per-backend:
  `src/main-sdl2.c` (default) and `src/main-glfw.c` (GLFW + GLAD), which
  keep window creation, the event pump, and native-key → JS-keyCode
  translation. Both register a `vc_backend_services` vtable
  (`refresh_title`, `window_size`) before any shader loads, because the core
  cannot set a title or size its window itself.
- **There is no `#ifdef BUILD_*` in this tree.** Do not add one. If a
  backend needs different behaviour, add a function to `viewer_core.h` (or a
  slot to the services struct) instead of a conditional. `viewer_core.c`
  must stay backend-neutral.
- **Still genuinely duplicated between the two `main-*.c`:** the CLI/help
  text, the `-k` vs `--texture` conflict check, and the frame-loop
  scaffolding (timing, rescan/poll blocks, draw + present sequence). A change
  to one of those must still be mirrored in the other; a change to anything
  listed as core-owned must not.
- **Both backends must produce identical key indices.** They each carry a
  keyCode table (`map_sdl_key` in `main-sdl2.c`, `populate_key_lookup` in
  `main-glfw.c`) and these already drifted once: SDL2 emitted lowercase
  letter codes while GLFW emitted uppercase, so keyboard shaders worked on
  one backend and not the other, and neither suite caught it. Keep both
  tables in step with the JS `keyCode` values (letters uppercase 65..90,
  `;`=186, `=`=187, `-`=189) and verify by rendering the affected index, not
  by reading the table. `config.h` holds the CLI table both parse.
- MCP is HTTP-only (`mcp_viewer_start()` in `src/mcp.c`, default
  `127.0.0.1:8642/mcp`). There is no stdio transport in the viewer; do not
  reintroduce `--mcp`/`--mcp-lsp`. `-p/--mcp-port 0` disables the server.
- `tools/shadertoy-mcp-bridge.py` is a *client-side* stdio<->HTTP shim, not a
  server mode: it forwards to the viewer and, when the port is closed, prints
  `shadertoy.exe must be running`. Keep the message wording stable; clients
  and users key off it.
- Checks: `make && make console`, then `python tests/mcp_viewer_smoke.py`,
  `python tests/mcp_malformed.py`, and `python tests/mcp_bridge.py`. All three
  start the viewer on a random port, so all three need a display/GPU. When
  you touch shared code, `viewer_core.*`, or either `main-*.c`, also build
  and run both backends: `make BACKEND=glfw && make BACKEND=glfw console`.
  The suites cover the MCP surface only — they do not check rendering or
  input, so a keyboard/mouse change needs a rendered probe (draw the value
  you expect into a pixel, screenshot the live window with
  `screenshot {"window": true}`, read the pixel back).
