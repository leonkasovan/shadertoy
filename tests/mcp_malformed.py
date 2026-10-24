#!/usr/bin/env python3
"""Regression test: malformed JSON-RPC messages must not kill the server.

The MCP server is HTTP-only and hosted by the viewer, so this starts
`build/shadertoy(.exe)` on a random port (like mcp_viewer_smoke.py) and
POSTs the bad payloads at http://127.0.0.1:<port>/mcp. Requires a
display/GPU, because the viewer always opens a window.

libjq asserts when jv_object_get() is called on a non-object, which used to
abort the process on a top-level array/string/number/bool/null. This test
sends each kind, plus missing methods and invalid JSON, and then a valid
request to prove the server is still alive.
"""

import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build",
                   "shadertoy.exe" if os.name == "nt" else "shadertoy")

if not os.path.exists(EXE):
    sys.exit("build the viewer first: make")

START_REL = os.path.join("build", "mcp_malformed_start.glsl")
START = os.path.join(ROOT, START_REL)

with open(START, "w") as f:
    f.write("void mainImage(out vec4 c, in vec2 p){ c = vec4(0.5); }\n")

probe = socket.socket()
probe.bind(("127.0.0.1", 0))
port = probe.getsockname()[1]
probe.close()

proc = subprocess.Popen([EXE, "-s", START_REL, "-p", str(port)], cwd=ROOT,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
url = f"http://127.0.0.1:{port}/mcp"


def post(payload):
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        return resp.status, resp.read()


# (message, expected response id, expected error code)
cases = [
    ('[{"jsonrpc":"2.0","id":9,"method":"tools/list"}]', None, -32600),
    ('"not an object"', None, -32600),
    ("42", None, -32600),
    ("true", None, -32600),
    ("null", None, -32600),
    ("{}", None, -32600),
    ('{"jsonrpc":"2.0","id":7}', 7, -32600),
    ("{not json", None, -32700),
]

try:
    for _ in range(50):
        try:
            status, raw = post({"jsonrpc": "2.0", "id": 1,
                                "method": "initialize", "params": {}})
            break
        except OSError:
            time.sleep(0.2)
    else:
        sys.exit("viewer MCP server did not start")
    assert status == 200, status

    # Raw bodies, not wrapped by post(), so JSON parse errors survive.
    for message, want_id, want_code in cases:
        req = urllib.request.Request(
            url, data=message.encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=20) as resp:
            assert resp.status == 200, (message, resp.status)
            raw = resp.read()
        obj = json.loads(raw)
        error = obj.get("error")
        assert obj.get("id") == want_id, (message, obj)
        assert error and error.get("code") == want_code, (message, obj)

    # Notifications get 202 and no body.
    status, body = post({"jsonrpc": "2.0",
                         "method": "notifications/initialized"})
    assert status == 202 and body == b"", (status, body)

    # Still alive after all of the above.
    status, raw = post({"jsonrpc": "2.0", "id": 11, "method": "tools/list"})
    final = json.loads(raw)
    assert status == 200, status
    assert final.get("id") == 11, final
    assert "tools" in final.get("result", {}), final
finally:
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    lock = os.path.join(ROOT, "build", ".shadertoy-mcp")
    stale = False
    try:
        with open(lock) as f:
            stale = f.read().split()[0] == str(port)
    except (OSError, IndexError):
        pass
    if stale:
        os.remove(lock)
    try:
        os.remove(START)
    except OSError:
        pass

print("malformed JSON-RPC regression test OK")
