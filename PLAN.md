# PLAN.md — Architecture B: single-process MCP (viewer-hosted)

Status: implemented on branch `architecture-b`. The viewer hosts MCP by
default; architecture A (standalone headless `shadertoy-console.exe --mcp`)
still works unchanged.

## Goal

One process, one GL context: the interactive viewer hosts the MCP server
itself, eliminating the second context and enabling viewer-coupled tools
that architecture A cannot offer.

## Design

### Transport
- The MCP client cannot stdio-spawn a GUI process, so the viewer listens on
  **MCP Streamable HTTP** at `http://127.0.0.1:<port>/mcp`.
- Requests are plain `POST` → JSON responses (SSE streams only where the
  spec requires them). A minimal HTTP/1.1 server (~200 lines, no deps) is
  enough: one route, JSON in/out, `Mcp-Session-Id` header handling.
- opencode config switches from `"type": "local"` to:

  ```json
  {
    "mcp": {
      "shadertoy": {
        "type": "remote",
        "url": "http://127.0.0.1:8642/mcp",
        "enabled": true
      }
    }
  }
  ```

### GL model
- Reuse the **viewer's own context**. Tool calls do not render from the
  server thread; they enqueue a job (compile / screenshot / fps / run) that
  the main loop executes between frames — the pattern the old embedded
  implementation used, minus the stdio reader.
- No offscreen context, no hidden window, no duplicate program
  compilation: the same GL objects the window draws are what tools inspect.
- Headless CLI use of `--mcp` (architecture A) remains available and
  unchanged; B is additive.

### New tools (only possible in B)
| Tool | Purpose |
|------|---------|
| `load_shader(name\|index)` | Switch the visible window to another shader (the cancelled A-side request) |
| `get_viewer_status` | Window open? current shader, FPS, window size |
| `screenshot` (extension) | Option to grab the *actual window backbuffer* instead of an offscreen render |

### Lifecycle
- Port: fixed default (e.g. 8642) + `--mcp-port <n>`; on conflict, viewer
  exits with a clear message (no auto-increment: deterministic config beats
  a discovery protocol).
- Lock file `build/.shadertoy-mcp` holding `port + pid` lets clients detect
  "viewer already running" and fail loudly.
- Viewer quit = server quit: the agent session ends. Documented tradeoff;
  users keep the window open during agent work (same as today).

### Phases
1. [x] **HTTP transport in viewer** — minimal POST server thread, request
   queue into main loop, initialize/tools/list/list_shaders/read/info.
2. [x] **Render tools via frame-loop jobs** — compile/screenshot/fps/run
   against the live context; window screenshot verified against the viewport.
3. [x] **Coupled tools** — `load_shader`, `get_viewer_status`; hot-reload
   poll keeps working (unchanged).
4. [x] **Client config + docs** — opencode.json example, README section,
   port/lockfile notes.

## Risks / open questions
- Verified with direct Streamable HTTP requests (initialize, tools/list,
  tools/call, notifications). End-to-end opencode remote-client negotiation
  (`MCP-Protocol-Version`, session echo) is not yet exercised in a new
  session.
- Localhost binds did not prompt the Windows firewall in testing.
- Viewer crash now takes the agent's server down; acceptable, documented.
- Multiple concurrent agent sessions share one viewer: serialize tool jobs
  (queue) — no parallel GL use.
- Windows GUI subsystem detaches stdout: server logs must go to a file
  (`build/shadertoy-mcp.log`), not stdout.

## What we are NOT doing
- No embedded stdio mode in the viewer (clients can't spawn GUI processes).
- No removal of architecture A; both stay buildable from one source tree.
- No auth beyond optional localhost bind restriction — it never leaves the
  machine.
