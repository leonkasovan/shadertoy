#!/usr/bin/env python3
"""Smoke-test the viewer-hosted MCP server (architecture B).

Run from anywhere after `make`: python tests/mcp_viewer_smoke.py
Requires a display/GPU, like the viewer itself.
"""

import base64
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "shadertoy.exe")

if not os.path.exists(EXE):
    sys.exit("build the viewer first: make")

probe = socket.socket()
probe.bind(("127.0.0.1", 0))
port = probe.getsockname()[1]
probe.close()

proc = subprocess.Popen([EXE, "-p", str(port)], cwd=ROOT,
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

    _, loaded = rpc("tools/call", {
        "name": "load_shader", "arguments": {"index": 0}}, id=5)
    assert "isError" not in loaded["result"], loaded
    assert "shader:" in loaded["result"]["content"][0]["text"], loaded

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
