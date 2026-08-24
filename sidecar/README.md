# Playerbots MCP Sidecar

This package is the typed Python client and standard input and output MCP adapter for the C++ server in
`mod-playerbots-mcp`.

The C++ server listens only on `127.0.0.1`. Every request uses wire schema version 2 and requires
`PLAYERBOT_VERIFICATION_TOKEN` with at least 32 bytes. `PLAYERBOT_VERIFICATION_PORT` must match
`PlayerbotsMCP.Port` from `mod_playerbots_mcp.conf`.

The sidecar exposes status, bot listing, inspection, loop anomaly inspection, bounded waits, command dispatch, and
guarded homebind recovery. Inspection schema version 4 includes typed read only travel route, last movement, and RPG
target diagnostics. `inspect_bot.rpg_target` identifies the exact current target, including its GUID, entry, name,
NPC flags, distance, and movement state. Tests use a test owned loopback server and never contact a live worldserver.

The activity lease tools acquire, inspect, renew, and release one self expiring lease for an online, masterless random
bot. They allow a bounded monitor to bypass `AiPlayerbot.BotActiveAlone` rotation for only its selected cohort. Save
each acquire result's ownership token, present it on renewal and release, and always attempt cleanup after monitoring.

## Development

```bash
uv sync --locked --dev
uv run pytest -q
uv run ruff format --check .
uv run ruff check .
uv run basedpyright src tests
```
