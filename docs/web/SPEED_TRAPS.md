# Speed traps and gate editor

Turn on **Online → Free roam** before starting the game. Camera markers appear in the game’s minimap and full map while playing free roam. Crossing feedback requires a server running this feature. Players in the same Room compete; the empty Room is the default public room.

The saved production placements are in `web/server-rs/crates/roam/src/speedtraps.def`. Gates are defined by two surveyed road-edge endpoints spanning the road, covering both directions of traffic. The crossing test projects onto that segment (including interpolated road height), rather than assuming an axis-aligned lane.

Traps use transparent camera glyphs without a circular background as native icons on both maps. Camera glyphs stay upright on both maps. The minimap handles their position and edge clipping, and the full map has a **Speed traps** legend toggle. There is no separate radar or side panel, and no unique location name on the banner. Each stable internal trap ID still owns its own scores.

A confirmed pass plays the original two-click shutter and short translucent screen flash. A five-second banner shows **Speed trap**, your recorded speed, and up to three room leaders for the crossed trap (names and best speeds). Fewer are shown if fewer players have recorded a pass. All speeds are rounded to whole numbers and follow the game’s Speedometer Units setting (km/h or mph), including the leaderboard. The current player is highlighted if ranked in the top three. Reduced-motion mode disables the flash and entrance animation. The sound, driving controls and session scoring rules are unchanged.

A player must approach from at least 25 m away and reach at least 18 km/h. After a pass there is a five-second cooldown; leave the gate before trying again. Scores belong to the current free-roam session: changing rooms, restarting the game, disconnecting until the hub removes the player, or restarting the server clears them. Blocked players do not appear on each other's boards. Pausing or entering a race interrupts the crossing, while retaining the best score.

## Gate editor

Run the local server with `--dev`, build the frontend and game module, and reload the game. In free roam, use **F9 → Development → Gate editor**. The editor is loaded only on demand. `/test/gates.html` is an optional launcher for a separate `GateEditor` career and private room; allow roughly 80 seconds for its initial career setup.

### Place and test gates

The editor displays the **live game canvas**, not a screenshot. The overhead camera starts at **100 m**. Drag empty space to pan and scroll to zoom. Geometry uses 10–250 m altitude. Zoom farther out to transition automatically to a city overview built from the game's road network, with gate pins. Pan across the city without loading districts; zoom back in below 220 m to load geometry at the selected location. Panning geometry into another neighbourhood automatically moves the car's streaming anchor and loads the district. The last overhead frame stays visible while loading; when entering from the overview, the city map stays visible until geometry is ready. The temporary third-person camera is hidden.

- **New gate**: click one road edge, then the opposite edge. Escape cancels placement. Drag the selected gate's endpoints to adjust it.
- The sidebar lists all gates. Select one to edit it and move the camera to it. **Delete gate** removes the selected gate, including the last one.
- Endpoint elevation follows the nearest road automatically. A gate detects cars from its road level to **45 m above it**, with a 4 m allowance below the car-centre plane. This intentionally includes elevated roads within that range. Roofs can obscure a lower road; zoom in and verify the placement while driving.
- **Drive** returns to gameplay. **Start on side 1 / 2** places the car on a nearby approach, facing the selected gate. Drive through normally to test server scoring, the banner, shutter and flash. Free roam with others must be enabled for server scoring; the isolated editor launcher enables it.

Geometry navigation and test jumps move the current car. The ordinary F9 entry uses the current career; the optional isolated launcher uses its separate profile.

### Live saving

Edits are saved automatically to `web/server-rs/crates/roam/src/speedtraps.def`. The status line shows saving, success or errors. Temporary failures retry automatically. If a save response is lost, the next attempt reconciles the file against the attempted save before applying newer edits. A concurrent file change stops saving: close and reopen the editor to discard the unsaved conflicting draft and reload the file. Unsaved edits trigger the browser's navigation warning.

The dev server applies the saved catalog immediately to scoring. Catalog changes reset development scores to avoid retaining results for old geometry. All connected development games synchronize minimap/world-map positions on startup and once per second, even without opening the editor. No rebuild is needed between editing and testing. Review the `.def` diff and commit the placements when ready. Production builds still compile that catalog into the game and server.

The catalog accepts up to 64 gates (including an empty catalog), stable unique lowercase IDs of up to 32 characters, finite world coordinates, and widths of 2–200 m.

### Development endpoint

`GET /test/gates/catalog` reads the catalog and returns an ETag. `PUT` requires that ETag in `If-Match`, validates the entire catalog, and atomically replaces the fixed repository file. It takes no filesystem path from the caller.

The route returns 404 without `--dev`. It accepts direct loopback connections only, rejects forwarded client headers and cross-origin browser requests, and bounds the body to 32 KiB. Concurrent writes are serialized; a stale revision gets 412 without modifying the file. It is not a production editing API. The read-only `GET /test/gates/markers` route exposes active marker centres to all development players, including proxied clients; it also returns 404 without `--dev`.


## Implementation

`web/server-rs/crates/roam/src/speedtrap.rs` detects segment crossings between successive hub snapshots, including gate width and height. Speed is interpolated at the crossing. It rejects stale or out-of-order samples, clock discontinuities, implausible displacement and excessive speed. The server owns the score but still receives client-generated positions and velocities: this is a casual challenge, not cheat-proof server physics.

The browser requests results once per second over the existing LAN switch. `STQ1` requests are bound to an already joined source endpoint; `STP1` JSON responses come only from the hub's reserved address. The switch rewrites client source addresses. Polling replies are rate-limited per browser. A valid crossing also pushes an immediate result to that endpoint if it requested the HUD within the last three seconds; ordinary clients receive no extra packets. A session-scoped crossing sequence deduplicates feedback independently of which trap is nearest. Gameplay scoring needs no new HTTP endpoints; the editor uses the development-only endpoints described above. Leaderboard rows correspond to the most recently crossed trap, even if another camera is now nearer.

The crossing banner lives in `web/src/app/speedtrap.ts`. `runtime/web/status_web.c` publishes the native Speedometer Units byte through the status channel as `speed_kmh`. Native marker creation and the original camera texture are in `runtime/speedtrap_maps.inc` and `runtime/speedtrap_texture.inc`, included by `roam_game.c`. Markers use the existing native FE/map objects and are owned and freed with their HUD/map packages; the minimap recreates them after HUD/icon rebuilding.

`web/server-rs/crates/roam/src/speedtraps.def` is the shared catalog: `TRAP(id, ax, ay, az, bx, by, bz)`. C includes it directly; the roam crate’s build script generates its Rust constants from the same file. Add surveyed road-edge pairs there, then rebuild **both** the game module (`web/build.sh`) and server (`cargo build --manifest-path web/server-rs/Cargo.toml`). The saved catalog contains 15 gates; these are the production placements, not test fixtures. Native map markers are available in the recompiled browser/macOS runtime, not the separate Windows ASI plugin. GPS selection/routing and roadside 3D camera props are not implemented.

## Validation

### Gameplay checks

From the repository, with the development environment sourced:

```sh
cargo test --manifest-path web/server-rs/Cargo.toml -p nfsu2-roam
node --test web/test/speedtrap.test.mjs web/test/speedtrap-ui.test.mjs web/test/net-worker.test.mjs
(cd web && npm run check && npm run build)
node web/tools/speedtrap-test.mjs http://localhost:8081 out/web/speedtrap-test
```

The last test needs a running `--dev` server with the game assets. It creates a fresh test profile, enters career free roam, positions a car on the approach, and uses browser throttle input to physically drive across the gate. It asserts that a speed and the player's name appear on the leaderboard, the result banner and leaderboard appear, and exactly one shutter audio buffer is scheduled, then captures screenshots of the minimap and full map, including reopening the full map. The setup teleport itself must not award a score.

Verified locally: TypeScript check and frontend/WASM builds pass; the speed-trap browser UI/transport/audio tests pass, including both units and a three-row board. All five server integration tests pass, including a five-player leaderboard ordering check. Physical browser drives recorded 62 mph with imperial selected and 100 km/h with metric selected, one shutter per crossing, and native camera markers on both maps. Full-map reopening was checked visually; HUD camera copies now follow their original HUD image’s visibility and do not leak over the full-map legend.

The broader frontend run passed 501 tests, skipped seven, and hit one unrelated timing-sensitive Friends test; all nine Friends tests passed on standalone rerun.

### Editor checks

With a running `--dev` server for the live checks:

```sh
node --test web/test/gate-editor.test.mjs web/test/gate-editor-ui.test.mjs web/test/gate-save.test.mjs web/test/gate-sync.test.mjs
cargo test --manifest-path web/server-rs/Cargo.toml --profile ci -p nfsu2-server --bin nfsu2-server gate_catalog_local_dev
node web/tools/gate-live-test.mjs out/web/gate-live
node web/tools/gate-pan-test.mjs out/web/gate-pan
```


The live drive test temporarily renames a gate and restores the original catalog with a revision check; it refuses to overwrite concurrent edits.

The panning regression check repeatedly crosses streaming boundaries with the editor left open. Native photo status bit 1 means it can open only while closed; bit 2 means it is already open. Camera transitions accept either initial state before closing for streaming.

### Production regression

Build both the WASM game (`web/build.sh`) and Rust server after saving placements. Start the resulting server **without `--dev`**, with game assets and a separate test state directory, then run:

```sh
node web/tools/gate-production-test.mjs http://localhost:8082 out/web/gate-production
```

This read-only catalog test starts a fresh career through the normal game UI, checks that all saved gate coordinates were compiled into both maps, and physically drives through the first saved gate to verify the server result and leaderboard. It also checks that camera icons stay upright while turning and after reopening the full map. Only the car's approach position is set by the harness. It verifies that development endpoints and the editor menu are unavailable and no development marker overrides are loaded. It saves a screenshot and JSON result. Scoring unit/integration tests use separate fixed gates so production placement edits do not invalidate their scenarios.

## References and adaptation

- [Heat speed traps](https://nfs.fandom.com/wiki/Need_for_Speed:_Heat/Activities/Speed_Traps): nearby minimap markers, discovery and three target speeds.
- [Most Wanted (2012) manual](https://eaassets-a.akamaihd.net/eahelp/manuals/need-for-speed-most-wanted-manuals_PC.pdf): each speed camera has a separate SpeedWall.
- [Unbound locations](https://www.powerpyx.com/need-for-speed-unbound-all-speed-trap-locations/): 50 activities with three-star completion.

We adapt proximity-based presentation and per-camera records. The shutter/flash is our original feedback treatment; no EA sound or UI asset was copied. Star targets, career rewards and persistent discovery remain outside this prototype.
