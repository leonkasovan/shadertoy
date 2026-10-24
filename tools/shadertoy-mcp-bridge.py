#!/usr/bin/env python3
"""stdio <-> HTTP bridge for the Shadertoy viewer's MCP server.

The viewer is the MCP server and it speaks HTTP only (127.0.0.1:8642/mcp by
default). It has no stdio mode on purpose: a separate process could not drive
the live window or read its backbuffer.

Some MCP clients can only launch a local process, and their generic
"connection failed" text does not tell you *why*. Point such a client at this
script and the failure becomes actionable:

    shadertoy MCP: shadertoy.exe must be running.
    Nothing is listening on http://127.0.0.1:8642/mcp

When the viewer *is* running, the bridge is transparent: each JSON-RPC
message arrives on stdin as one JSON object per line (the MCP stdio
transport), is POSTed to the HTTP endpoint, and the reply is written back as
one line on stdout. Stdlib only, no dependencies.

Usage (client config):
    command: python
    args:    ["tools/shadertoy-mcp-bridge.py"]

Options:
    --url URL           endpoint to forward to (default $SHADERTOY_MCP_URL or
                        http://127.0.0.1:8642/mcp)
    --probe-timeout SEC how long to wait for the viewer at startup (default 2)
    --check             only probe the endpoint, print the result, exit
"""

from __future__ import annotations

import argparse
import http.client
import os
import socket
import sys
import urllib.parse

DEFAULT_URL = "http://127.0.0.1:8642/mcp"

VIEWER_HINTS = (
    "  build\\shadertoy.exe          (Windows)\n"
    "  build/shadertoy              (Linux/macOS)"
)


def parse_url(url: str):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", ""):
        sys.exit(f"shadertoy MCP bridge: unsupported URL scheme '{parts.scheme}'")
    host = parts.hostname or "127.0.0.1"
    port = parts.port or 80
    path = parts.path or "/mcp"
    if parts.query:
        path += "?" + parts.query
    return host, port, path


def unreachable_message(url: str, detail: str = "") -> str:
    msg = (
        "shadertoy MCP: shadertoy.exe must be running.\n"
        f"Nothing is listening on {url}"
    )
    if detail:
        msg += f" ({detail})"
    msg += (
        ".\n\n"
        "The viewer hosts the MCP server itself, so start it first:\n"
        f"{VIEWER_HINTS}\n"
        "It prints 'MCP server listening on ...' once the port is open. "
        "Then retry.\n"
    )
    return msg


def probe(host: str, port: int, timeout: float) -> str:
    """Return "" if something is listening, else a short reason."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return ""
    except socket.timeout:
        return "timed out"
    except OSError as exc:
        return exc.strerror or str(exc)


def post(host: str, port: int, path: str, body: bytes, timeout: float) -> bytes:
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(
            "POST",
            path,
            body=body,
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "Content-Length": str(len(body)),
            },
        )
        resp = conn.getresponse()
        return resp.read()
    finally:
        conn.close()


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--url", default=os.environ.get("SHADERTOY_MCP_URL", DEFAULT_URL))
    ap.add_argument("--probe-timeout", type=float, default=2.0)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    host, port, path = parse_url(args.url)

    # Fail loudly and early: this is the whole reason the bridge exists.
    reason = probe(host, port, args.probe_timeout)
    if reason:
        sys.stderr.write(unreachable_message(args.url, reason))
        sys.stderr.flush()
        return 1
    if args.check:
        print(f"shadertoy MCP server is listening on {args.url}")
        return 0

    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer

    for raw in stdin:
        line = raw.strip()
        if not line:
            continue  # MCP stdio allows blank separators
        try:
            reply = post(host, port, path, line, timeout=60.0)
        except OSError as exc:
            sys.stderr.write(unreachable_message(args.url, exc.strerror or str(exc)))
            sys.stderr.flush()
            return 2

        # Notifications are answered with 202 + empty body: stay silent.
        if not reply:
            continue
        stdout.write(reply.rstrip(b"\r\n") + b"\n")
        stdout.flush()

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
