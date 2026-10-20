#!/usr/bin/env python3
"""Regression test: malformed JSON-RPC messages must not kill the server.

Runs the standalone stdio server, so no display or GPU is needed. Build with
`make console` first.

libjq asserts when jv_object_get() is called on a non-object, which used to
abort the process on a top-level array/string/number/bool/null. This test
sends each kind, plus missing methods and invalid JSON, and then a valid
request to prove the server is still alive.
"""

import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build",
                   "shadertoy-console.exe" if os.name == "nt"
                   else "shadertoy-console")

if not os.path.exists(EXE):
    sys.exit("build the console server first: make console")

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

valid = '{"jsonrpc":"2.0","id":11,"method":"tools/list"}'
notification = '{"jsonrpc":"2.0","method":"notifications/initialized"}'
payload = "\n".join([m for m, _, _ in cases] + [valid, notification]) + "\n"

proc = subprocess.run([EXE, "--mcp"], input=payload.encode(), cwd=ROOT,
                      capture_output=True, timeout=30)
stderr = proc.stderr.decode(errors="replace")
if proc.returncode != 0:
    sys.exit(f"server exited with {proc.returncode} on malformed input\n"
             f"stderr:\n{stderr}")

lines = [line for line in proc.stdout.splitlines() if line.strip()]
expected = len(cases) + 1
if len(lines) != expected:
    sys.exit(f"expected {expected} responses (notification is silent), "
             f"got {len(lines)}\nstdout:\n{proc.stdout!r}\nstderr:\n{stderr}")

for i, (message, want_id, want_code) in enumerate(cases):
    obj = json.loads(lines[i])
    error = obj.get("error")
    assert obj.get("id") == want_id, (message, obj)
    assert error and error.get("code") == want_code, (message, obj)

final = json.loads(lines[len(cases)])
assert final.get("id") == 11, final
assert "tools" in final.get("result", {}), final

print("malformed JSON-RPC regression test OK")
