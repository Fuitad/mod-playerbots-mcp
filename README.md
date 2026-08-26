> **Work in progress**

This project is not ready for installation or use. It provides no deployment or compatibility guarantee.

# Playerbots MCP

Playerbots MCP provides an authenticated loopback verification server inside AzerothCore and a typed Python MCP
sidecar. The C++ bridge owns framing, authentication, bounded world thread operations, inspection, command dispatch,
anomaly reads, guarded homebind recovery, two verification staging operations (overwrite a skill the bot already
knows, teleport a bot beside the nearest spawned gameobject of an entry), a config reload (`reload_config`), and one
GM tooling operation that runs a worldserver console command with console authority and returns its output
(`run_gm_command`). It also owns bounded activity lease operations for observing selected random bots without
changing the global active population. The Python sidecar exposes those operations as fourteen typed MCP tools.

## Console authority and the four refused families

`run_gm_command` is a general escape hatch, not a curated list. If a console command exists, it runs, and it does
what typing it at the worldserver console would do. That includes commands that destroy data: `.ban`, `.unban`, and
`.character deleted delete` all pass through. Read the command before sending it.

Four command families are declined and answer error code `command_refused`:

* `server`, `quit`, `exit`. Stopping the worldserver from here would bypass the protected account guard that gates
  every other path to the same action, and the caller is an agent rather than a person.
* `account`. Account administration is not verification tooling.

`command_refused` is deliberately distinct from `invalid_command`. A refusal is a policy answer, not a syntax
mistake: do not retry it, do not rephrase it, and do not route around it. Run the command at the worldserver console
instead. Nothing outside those four families is filtered, so the refusal is not a general safety boundary. The line
it draws is "can end the session that is running right now", not "destructive".

A deployment that should not expose console authority at all leaves this module out of the build. It is opt in.

## Applying a configuration change without a restart

`reload_config` re-reads `worldserver.conf` and every `etc/modules/*.conf`, then refreshes visibility distances,
exactly as the `.reload config` console command does. Module settings take effect immediately, because each module
re-reads its own configuration from the same `OnAfterConfigLoad` hook. Prefer it over sending `.reload config`
through `run_gm_command`: it returns a real success flag rather than scraped console text, and it cannot be typo'd
into a different reload. Settings a module consumes only at startup still need a restart.

The server reads inspection data from mod-playerbots-telemetry and delegates intervention state to
mod-playerbots-recovery. It does not store MCP transport or protocol code in mod-playerbots.

## Travel diagnostics

`inspect_bot` includes typed `movement` and `travel` sections in inspection schema version 7. They are read only and
report the ordinary Playerbot travel state that already exists on the world thread.

* `movement.canMove` reports the actual Playerbot movement capability independently from TravelMgr.
* `movement.mounted` reports whether the bot is mounted. The core refuses every cast from a mounted player that
  is not flagged castable while mounted, answering `SPELL_FAILED_NOT_MOUNTED`, so a bot parked at a forge or a
  node with nothing else wrong may simply be mounted.
* `travel.status`, `travel.destination`, and `travel.forced` describe the active TravelMgr target.
* `travel.idleNoDestination` is true for `NullTravelDestination`. The destination is null while status and
  `timeLeftMs` preserve the actual idle cooldown state. Active idle state uses cooldown status with a remaining value
  between zero and five minutes. After expiry, truthful terminal states are expired or none with exactly zero
  milliseconds.
* `route.pointCount` reports the retained route size.
* `route.nextPathType`, `route.nextEntry`, and `route.nextPoint` report the next point selected by the ordinary travel
  route logic. An unavailable next point is explicit through `nextPoint.available`.
* `lastMovement` reports the last issued movement point, its age, its requested delay, and its priority.

These fields diagnose route selection and movement stalls. They do not select a destination, move a bot, clear a
route, or otherwise change game state. Coordinates and distances are observations from the current snapshot and can
change on the next world update.

## RPG target diagnostics

`inspect_bot` includes the required `rpgTarget` section in inspection schema version 7. Agents should read this
section instead of inferring an RPG target from the last action, nearby NPCs, or the ordinary `travel` section.

* `available` states whether the current `"rpg target"` AI value resolves to a live world object.
* `type`, `guid`, `entry`, and `name` identify that exact object.
* `npcFlags` exposes the complete creature NPC flag mask. It is zero for a player or gameobject target.
* `distanceYards` measures the current three dimensional distance from the bot to the target.
* `moving` states whether a unit target is moving at inspection time.

When `available` is false, every detail field is null. The typed Python result exposes the same section as
`rpg_target`, with fields such as `npc_flags` and `distance_yards`. The section is observational only. It does not
select, clear, or move toward a target.

## Recovery and equipment diagnostics

Inspection schema version 7 reports the state needed to verify death recovery without treating an attempted packet
as success.

* `recovery.alive`, `recovery.ghost`, and `recovery.inArena` report the current player state.
* `recovery.corpse` reports presence, loaded state, map, same map distance, reclaim radius, remaining reclaim delay,
  and authoritative reclaim readiness. The readiness flag must equal all encoded core conditions, so false is also a
  validated result rather than an unchecked default.
* `recovery.latestRevive` reports the latest revive action timestamp in milliseconds since server start, success, and
  whether the action left the bot alive. It also reports age, attempt generation, and whether the attempt belongs to
  the current physical death cycle. An unavailable result carries zero and false values.
* Every equipped item reports current durability, maximum durability, and a derived broken state.
* `economy.observedAt` reports when the current Economy observation was produced in epoch seconds.

The `rpgTarget` section remains the legacy `"rpg target"` AI value. It does not claim to expose New RPG internal
state.

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
