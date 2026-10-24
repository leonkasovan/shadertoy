#!/usr/bin/env python3
"""Test tools/shadertoy-mcp-bridge.py (client-side stdio <-> HTTP bridge).

Covers the two behaviours that matter:
  1. No viewer listening -> exits non-zero with "shadertoy.exe must be running"
     (the message a client shows at startup).
  2. Viewer listening     -> transparently forwards JSON-RPC to /mcp.

Run from anywhere after `make`. Needs a display/GPU, because it starts the
viewer like mcp_viewer_smoke.py does.
"""

import json
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build",
                   "shadertoy.exe" if os.name == "nt" else "shadertoy")
BRIDGE = os.path.join(ROOT, "tools", "shadertoy-mcp-bridge.py")

if not os.path.exists(EXE):
    sys.exit("build the viewer first: make")

START_REL = os.path.join("build", "mcp_bridge_start.glsl")
START = os.path.join(ROOT, START_REL)


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def bridge(*args, **kwargs):
    return subprocess.run([sys.executable, BRIDGE, *args],
                          capture_output=True, text=True, **kwargs)


# --- 1. nothing listening: the actionable message -------------------------
dead_port = free_port()
dead_url = f"http://127.0.0.1:{dead_port}/mcp"

r = bridge("--url", dead_url, "--check")
assert r.returncode != 0, r
assert "shadertoy.exe must be running" in r.stderr, r.stderr
assert dead_url in r.stderr, r.stderr

# Starting as a client would (no --check) must fail the same way, without
# consuming stdin forever.
r = bridge("--url", dead_url, input="")
assert r.returncode != 0, r
assert "shadertoy.exe must be running" in r.stderr, r.stderr

# --- 2. viewer up: transparent forwarding ---------------------------------
with open(START, "w") as f:
    f.write("void mainImage(out vec4 c, in vec2 p){ c = vec4(0.5); }\n")

port = free_port()
url = f"http://127.0.0.1:{port}/mcp"
proc = subprocess.Popen([EXE, "-s", START_REL, "-p", str(port)], cwd=ROOT,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    for _ in range(60):
        r = bridge("--url", url, "--check")
        if r.returncode == 0:
            break
        time.sleep(0.2)
    else:
        sys.exit(f"bridge --check never succeeded\n{r.stdout}{r.stderr}")

    assert url in r.stdout, r.stdout

    payload = "\n".join([
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}',
        '{"jsonrpc":"2.0","method":"notifications/initialized"}',
        '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
        '{"jsonrpc":"2.0","id":3,"method":"tools/call",'
        '"params":{"name":"get_viewer_status","arguments":{}}}',
    ]) + "\n"

    r = bridge("--url", url, input=payload, timeout=60)
    assert r.returncode == 0, (r.returncode, r.stderr)
    lines = [line for line in r.stdout.splitlines() if line.strip()]
    assert len(lines) == 3, (len(lines), lines)  # notification stays silent

    init, tools, status = (json.loads(line) for line in lines)
    assert init["result"]["serverInfo"]["name"] == "shadertoy-sdl2", init
    assert len(tools["result"]["tools"]) >= 8, tools
    assert "running: yes" in status["result"]["content"][0]["text"], status

    # Killing the viewer mid-session must produce the same message, not a
    # traceback from http.client.
    proc.terminate()
    proc.wait(timeout=5)
    r = bridge("--url", url, input='{"jsonrpc":"2.0","id":9,"method":"tools/list"}\n',
               timeout=60)
    assert r.returncode != 0, r.stdout
    assert "shadertoy.exe must be running" in r.stderr, r.stderr
finally:
    if proc.poll() is None:
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

print("MCP bridge test OK")
