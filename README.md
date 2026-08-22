> **Work in progress**

This project is not ready for installation or use. It provides no deployment or compatibility guarantee.

# Playerbots MCP

Playerbots MCP provides an authenticated loopback verification server inside AzerothCore and a typed Python MCP
sidecar. The C++ bridge owns framing, authentication, bounded world thread operations, inspection, command dispatch,
anomaly reads, guarded homebind recovery, two verification staging operations (overwrite a skill the bot already
knows, teleport a bot beside the nearest spawned gameobject of an entry), and one GM tooling operation that runs a
worldserver console command with console authority and returns its output (`run_gm_command`; server lifecycle and
account administration commands are refused). The Python sidecar exposes those operations as typed MCP tools.

The server reads inspection data from mod-playerbots-telemetry and delegates intervention state to
mod-playerbots-recovery. It does not store MCP transport or protocol code in mod-playerbots.

## Travel diagnostics

`inspect_bot` includes a typed `travel` section in inspection schema version 3. It is read only and reports the
ordinary Playerbot travel state that already exists on the world thread.

* `status`, `destination`, `forced`, and `canMove` describe the active travel target and whether the bot can move.
* `route.pointCount` reports the retained route size.
* `route.nextPathType`, `route.nextEntry`, and `route.nextPoint` report the next point selected by the ordinary travel
  route logic. An unavailable next point is explicit through `nextPoint.available`.
* `lastMovement` reports the last issued movement point, its age, its requested delay, and its priority.

These fields diagnose route selection and movement stalls. They do not select a destination, move a bot, clear a
route, or otherwise change game state. Coordinates and distances are observations from the current snapshot and can
change on the next world update.

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
