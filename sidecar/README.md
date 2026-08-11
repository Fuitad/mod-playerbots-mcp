# Playerbots MCP Sidecar

This package is the typed Python client and standard input and output MCP adapter for the C++ server in
`mod-playerbots-mcp`.

The C++ server listens only on `127.0.0.1`. Every request uses wire schema version 2 and requires
`PLAYERBOT_VERIFICATION_TOKEN` with at least 32 bytes. `PLAYERBOT_VERIFICATION_PORT` must match
`PlayerbotsMCP.Port` from `mod_playerbots_mcp.conf`.

The sidecar exposes status, bot listing, inspection, loop anomaly inspection, bounded waits, command dispatch, and
guarded homebind recovery. Tests use a test owned loopback server and never contact a live worldserver.

## Development

```bash
uv sync --locked --dev
uv run pytest -q
uv run ruff format --check .
uv run ruff check .
uv run basedpyright src tests
```
