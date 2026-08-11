> **Work in progress**

This project is not ready for installation or use. It provides no deployment or compatibility guarantee.

# Playerbots MCP

Playerbots MCP provides an authenticated loopback verification server inside AzerothCore and a typed Python MCP
sidecar. The C++ bridge owns framing, authentication, bounded world thread operations, inspection, command dispatch,
anomaly reads, and guarded homebind recovery. The Python sidecar exposes those operations as typed MCP tools.

The server reads inspection data from mod-playerbots-telemetry and delegates intervention state to
mod-playerbots-recovery. It does not store MCP transport or protocol code in mod-playerbots.

## Dependencies

* A Playerbot compatible AzerothCore checkout
* The public mod-playerbots fork with the generic extension registry
* mod-playerbots-telemetry
* mod-playerbots-recovery
* Python 3.12 or newer and `uv` for the sidecar

## Configuration

Copy `conf/mod_playerbots_mcp.conf.dist` to the server configuration directory and remove the `.dist` suffix.
`PlayerbotsMCP.Port` selects the loopback listener. Zero disables it. `PLAYERBOT_VERIFICATION_TOKEN` must contain at
least 32 bytes and is shared by the C++ server and the Python sidecar.

## Verification

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
cd sidecar
uv sync --locked --dev
uv run pytest -q
```

The sidecar tests use a test owned loopback endpoint. They do not contact a live worldserver or the public network.

## License

Playerbots MCP is licensed under the GNU General Public License version 2. See `LICENSE`.
