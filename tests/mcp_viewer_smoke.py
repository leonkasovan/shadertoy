#!/usr/bin/env python3
"""Smoke-test the viewer-hosted MCP server (architecture B).

Run from anywhere after `make`: python tests/mcp_viewer_smoke.py
Requires a display/GPU, like the viewer itself.
"""

import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build",
                   "shadertoy.exe" if os.name == "nt" else "shadertoy")

if not os.path.exists(EXE):
    sys.exit("build the viewer first: make")

START_REL = os.path.join("build", "mcp_smoke_start.glsl")
START = os.path.join(ROOT, START_REL)
DT_PROBE = "mcp_smoke_dt"
CLEAR_PROBE = "mcp_smoke_clear"
PROBE_FILES = [
    START,
    os.path.join(ROOT, "shaders", DT_PROBE + ".glsl"),
    os.path.join(ROOT, "shaders", CLEAR_PROBE + ".glsl"),
]

# A start shader outside shaders/ makes the viewer's list (one entry) differ
# from the server's scan of shaders/, which is the load_shader index case.
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


def rpc(method, params=None, id=1):
    status, raw = post({"jsonrpc": "2.0", "id": id,
                        "method": method, "params": params or {}})
    return status, json.loads(raw)


def call(name, arguments, id):
    return rpc("tools/call", {"name": name, "arguments": arguments}, id)


def png_first_pixel(png):
    assert png[:8] == b"\x89PNG\r\n\x1a\n", png[:8]
    pos, idat = 8, b""
    while pos < len(png):
        length = struct.unpack(">I", png[pos:pos + 4])[0]
        kind = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height = struct.unpack(">II", data[:8])
            assert width == 8 and height == 8, (width, height)
        elif kind == b"IDAT":
            idat += data
        pos += 12 + length
    raw = zlib.decompress(idat)
    assert raw[0] == 0, raw[0]  # no filter on the first scanline
    return raw[1], raw[2], raw[3], raw[4]


def capture_pixel(result):
    content = result["result"]["content"]
    return png_first_pixel(base64.b64decode(content[1]["data"]))


try:
    for _ in range(50):
        try:
            status, init = rpc("initialize", {
                "protocolVersion": "2024-11-05", "capabilities": {},
                "clientInfo": {"name": "smoke", "version": "0"}})
            break
        except OSError:
            time.sleep(0.2)
    else:
        sys.exit("viewer MCP server did not start")

    assert status == 200
    assert init["result"]["serverInfo"]["name"] == "shadertoy-sdl2"

    _, tools = rpc("tools/list", id=2)
    names = {t["name"] for t in tools["result"]["tools"]}
    assert {"load_shader", "get_viewer_status", "screenshot"} <= names, names

    _, status_res = rpc("tools/call", {
        "name": "get_viewer_status", "arguments": {}}, id=3)
    text = status_res["result"]["content"][0]["text"]
    assert "running: yes" in text and "shader:" in text, text

    _, shot = rpc("tools/call", {
        "name": "screenshot", "arguments": {"window": True}}, id=4)
    png = base64.b64decode(shot["result"]["content"][0]["data"])
    assert png.startswith(b"\x89PNG\r\n")
    assert shot["result"]["structuredContent"]["window"] is True

    _, listed = call("list_shaders", {}, id=5)
    items = listed["result"]["structuredContent"]["items"]
    assert items, "shaders/ is empty"

    # Index 0 of the server's list is not index 0 of the viewer's -s list.
    _, loaded = call("load_shader", {"index": 0}, id=6)
    assert "isError" not in loaded["result"], loaded
    text = loaded["result"]["content"][0]["text"]
    assert "shader: " + items[0]["name"] in text, (items[0]["name"], text)

    # Negative indices used to be clamped to 0 and silently load the first.
    _, negative = call("load_shader", {"index": -1}, id=7)
    assert negative["result"].get("isError") is True, negative
    assert "out of range" in negative["result"]["content"][0]["text"], negative

    # A top-level array (JSON-RPC batch) used to abort the viewer via libjq.
    status, body = post([{"jsonrpc": "2.0", "id": 90, "method": "tools/list"}])
    batch = json.loads(body)
    assert status == 200 and batch["error"]["code"] == -32600, (status, batch)
    _, alive = call("get_viewer_status", {}, id=9)
    assert "running: yes" in alive["result"]["content"][0]["text"], alive

    # run_shaders advances the shader clock: iTimeDelta must be duration/frames,
    # not the default 1/60.
    _, written = call("write_shader", {
        "name": DT_PROBE,
        "content": ("void mainImage(out vec4 c, in vec2 p){\n"
                    "    c = vec4(iTimeDelta, iTime, 0.0, 1.0);\n"
                    "}\n")}, id=10)
    assert "isError" not in written["result"], written
    _, run = call("run_shaders", {
        "name": DT_PROBE, "frames": 5, "duration": 1.0,
        "width": 8, "height": 8, "captureFrame": True}, id=11)
    r, g, b, a = capture_pixel(run)
    assert abs(r - 51) <= 2, (r, "iTimeDelta should be 0.2s (51/255)")
    assert abs(g - 204) <= 2, (g, "iTime should be 0.8s (204/255)")

    # Each offscreen frame must start from a cleared FBO; otherwise a shader
    # that discards leaves the previous frame's pixels behind.
    _, written = call("write_shader", {
        "name": CLEAR_PROBE,
        "content": ("void mainImage(out vec4 c, in vec2 p){\n"
                    "    if (iFrame == 0) c = vec4(1.0, 0.0, 0.0, 1.0);\n"
                    "    else discard;\n"
                    "}\n")}, id=12)
    assert "isError" not in written["result"], written
    _, run = call("run_shaders", {
        "name": CLEAR_PROBE, "frames": 2, "duration": 0.1,
        "width": 8, "height": 8, "captureFrame": True}, id=13)
    assert capture_pixel(run)[:3] == (0, 0, 0), (
        "offscreen FBO was not cleared between frames")

    status, body = post({"jsonrpc": "2.0",
                         "method": "notifications/initialized"})
    assert status == 202 and body == b"", (status, body)

    print("viewer MCP smoke test OK")
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
    for path in PROBE_FILES:
        try:
            os.remove(path)
        except OSError:
            pass
