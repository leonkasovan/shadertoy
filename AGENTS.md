# Agent notes

- Use `rg.exe` for content searches. The `grep` on PATH is BusyBox and
  rejects common flags (`--null`, `-P`, etc.).
- Windows tools need single-slash flags in this shell: `taskkill /F /IM
  shadertoy-console.exe`, not the MSYS-style `//F` (passed through literally
  and rejected).
- Checks: `make && make console`, then `python tests/mcp_viewer_smoke.py`
  (starts the viewer on a random port; needs a display/GPU) and
  `python tests/mcp_malformed.py` (headless malformed-JSON-RPC regression).
