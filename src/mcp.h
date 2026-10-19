#ifndef MCP_H
#define MCP_H

/* Model Context Protocol (MCP) server entry point.
 *
 * Speaks JSON-RPC 2.0 over stdio. By default it uses the MCP stdio
 * transport: one JSON object per line (newline-delimited). It also
 * accepts incoming LSP-style "Content-Length: N\r\n\r\n<body>" framing,
 * and mcp_set_framing(1) selects that framing for replies.
 *
 * Runs until stdin is closed. */
int mcp_run(void);

/* 0 = newline-delimited JSON (default, MCP stdio transport)
 * 1 = Content-Length framed (LSP-style clients)
 * Returns the framing now in effect. Call before mcp_run(). */
int mcp_set_framing(int lsp);

#endif /* MCP_H */
