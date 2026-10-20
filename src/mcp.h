#ifndef MCP_H
#define MCP_H

#include <stddef.h>

/* Standalone MCP server entry point (`--mcp`).
 *
 * Reads JSON-RPC 2.0 messages from stdin until EOF and writes responses to
 * stdout. Default framing: one JSON object per line (MCP stdio transport).
 * Incoming LSP-style "Content-Length: N" framing is accepted either way;
 * mcp_set_framing(1) switches replies to Content-Length framing too.
 * Call mcp_set_framing() before mcp_run(). */
int mcp_run(void);

/* 0 = newline-delimited JSON (default) / 1 = Content-Length framed.
 * Returns the framing now in effect. */
int mcp_set_framing(int lsp);

/* Viewer-hosted MCP server (architecture B). The HTTP server runs on its own
 * thread; every request is queued and executed by mcp_viewer_poll() on the
 * viewer's main thread, which owns the only GL context. */
typedef struct {
    /* Switch the visible shader. Called on the main thread with GL current.
     * Provide either a plain filename (with or without .glsl) or an index;
     * index < 0 means "use name". Returns 0 and fills err on failure. */
    int (*load_shader)(const char *name, int index,
                       char *err, size_t errlen);

    /* Fill buf with human-readable viewer status. Main thread only. */
    void (*status)(char *buf, size_t len);
} mcp_viewer_hooks;

/* Start listening on 127.0.0.1:port. Returns 0 on failure (message logged),
 * 1 on success. port <= 0 disables the server and returns 1. */
int mcp_viewer_start(int port, const mcp_viewer_hooks *hooks);

/* Execute at most one queued request. Call from the main loop after drawing
 * the frame and before SDL_GL_SwapWindow: window screenshots then read the
 * real backbuffer. No-op when the server is not running. */
void mcp_viewer_poll(void);

/* Stop the HTTP thread and release its socket. Safe to call twice. */
void mcp_viewer_stop(void);

/* Bound port, or 0 when the server is not running. */
int mcp_viewer_port(void);

#endif /* MCP_H */
