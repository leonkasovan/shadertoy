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
- Two viewer backends share everything except the window layer:
  `src/main-sdl2.c` (default) and `src/main-glfw.c` (GLFW + GLAD). Any change
  to one `main-*.c` must be mirrored in the other; `config.h` holds the CLI
  table they both parse. Build the alternate with `make BACKEND=glfw`
  (or `make glfw`).
- MCP is HTTP-only (`mcp_viewer_start()` in `src/mcp.c`, default
  `127.0.0.1:8642/mcp`). There is no stdio transport in the viewer; do not
  reintroduce `--mcp`/`--mcp-lsp`. `-p/--mcp-port 0` disables the server.
- `tools/shadertoy-mcp-bridge.py` is a *client-side* stdio<->HTTP shim, not a
  server mode: it forwards to the viewer and, when the port is closed, prints
  `shadertoy.exe must be running`. Keep the message wording stable; clients
  and users key off it.
- Checks: `make && make console`, then `python tests/mcp_viewer_smoke.py`,
  `python tests/mcp_malformed.py`, and `python tests/mcp_bridge.py`. All three
  start the viewer on a random port, so all three need a display/GPU. Also
  build `make BACKEND=glfw` when you touch shared code or either `main-*.c`.
