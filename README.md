> **Work in progress**

This project is not ready for installation or use. It provides no deployment or compatibility guarantee.

# Playerbots MCP

Playerbots MCP provides an authenticated loopback verification server inside AzerothCore and a typed Python MCP
sidecar. The C++ bridge owns framing, authentication, bounded world thread operations, inspection, command dispatch,
anomaly reads, guarded homebind recovery, two verification staging operations (overwrite a skill the bot already
knows, teleport a bot beside the nearest spawned gameobject of an entry), and one GM tooling operation that runs a
worldserver console command with console authority and returns its output (`run_gm_command`; server lifecycle and
account administration commands are refused). It also owns bounded activity lease operations for observing selected
random bots without changing the global active population. The Python sidecar exposes those operations as typed MCP
tools.

The server reads inspection data from mod-playerbots-telemetry and delegates intervention state to
mod-playerbots-recovery. It does not store MCP transport or protocol code in mod-playerbots.

## Travel diagnostics

`inspect_bot` includes a typed `travel` section in inspection schema version 4. It is read only and reports the
ordinary Playerbot travel state that already exists on the world thread.

* `status`, `destination`, `forced`, and `canMove` describe the active travel target and whether the bot can move.
* `route.pointCount` reports the retained route size.
* `route.nextPathType`, `route.nextEntry`, and `route.nextPoint` report the next point selected by the ordinary travel
  route logic. An unavailable next point is explicit through `nextPoint.available`.
* `lastMovement` reports the last issued movement point, its age, its requested delay, and its priority.

These fields diagnose route selection and movement stalls. They do not select a destination, move a bot, clear a
route, or otherwise change game state. Coordinates and distances are observations from the current snapshot and can
change on the next world update.

## RPG target diagnostics

`inspect_bot` includes the required `rpgTarget` section in inspection schema version 4. Agents should read this
section instead of inferring an RPG target from the last action, nearby NPCs, or the ordinary `travel` section.

* `available` states whether the current `"rpg target"` AI value resolves to a live world object.
* `type`, `guid`, `entry`, and `name` identify that exact object.
* `npcFlags` exposes the complete creature NPC flag mask. It is zero for a player or gameobject target.
* `distanceYards` measures the current three dimensional distance from the bot to the target.
* `moving` states whether a unit target is moving at inspection time.

When `available` is false, every detail field is null. The typed Python result exposes the same section as
`rpg_target`, with fields such as `npc_flags` and `distance_yards`. The section is observational only. It does not
select, clear, or move toward a target.

## Random bot activity leases

`hold_bot_activity` keeps one online, masterless bot from a configured random bot account active for a bounded
observation window. It bypasses `AiPlayerbot.BotActiveAlone` rotation for that bot only. It does not change global
configuration, objectives, travel, grouping, combat, or queue state. A lease lasts at most 45 minutes and expires
inside `PlayerbotAI` even if the MCP client disconnects.

The acquire result returns an ownership token. A renewal must present that same token while the lease remains active.
`inspect_bot_activity_lease` reports the active state, expiry, and remaining seconds without exposing the token.
`release_bot_activity` accepts only the ownership token and is safe to repeat after a successful release. An exact
token permits cleanup even if the bot gains a master after acquisition.

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
