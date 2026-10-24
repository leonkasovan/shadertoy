# Agent notes

- Use `rg.exe` for content searches. The `grep` on PATH is BusyBox and
  rejects common flags (`--null`, `-P`, etc.).
- Windows tools need single-slash flags in this shell: `taskkill /F /IM
  shadertoy-console.exe`, not the MSYS-style `//F` (passed through literally
  and rejected).
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
