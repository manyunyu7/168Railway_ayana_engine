# Parity checklist & contributor guide

This is the working list of what the engine reproduces from the reference three.js client
(`ppka-wannabe-2/src/tiga`, ~30 k lines) and what is still missing. Update it whenever a feature
lands or a gap is found. The reference behaviour is documented with exact constants in
`docs/world-spec.md`; when the spec and the TS code disagree, the code wins.

Legend: ✅ done · 🟡 partial · ❌ missing · ➖ deliberately not ported (stays in the web/Flutter host)

## How work is organised

* **Game logic never moves.** Rules, timetable, interlocking, AI, panel layout, route menus: all in
  the TypeScript simulator, reached through `bridge/sim-bridge.ts` (`bridge/PROTOCOL.md`).
  If the engine needs new information, add a bridge command that calls the existing TS function —
  do not re-implement it in C++.
* **The engine draws and sends clicks.** Visual parity work happens in `engine/world/*` and
  `examples/ppka/*`.
* **Assets are converted offline** (`tools/convert`, `tools/fetch_tiles`, `tools/fontgen`); the runtime
  reads only our own formats (`.emod`, `.dem/.sat`, `.efnt`). Third-party decoders live in `tools/` only.
* **Verify with screenshots.** Every example supports `ENG_CAPTURE=/tmp/x.ppm` (+ `ENG_CAPTURE_FRAME`,
  `ENG_VIEW=dist,yaw,pitch`, `ENG_AUTOCLICK`, `ENG_AUTOROUTE`, `ENG_AUTOHOVER`, `ENG_AUTOPANEL`,
  `ENG_AUTOSELECT`, `ENG_AUTOJUMP`); convert with `sips -s format png` and look at it.
* Build with `mac-debug` (ASan/UBSan, `-Werror`) before pushing; play with `mac-release`.
* Push to `github.com/manyunyu7/168Railway_ayana_engine` after every fix.

## Trains (rangkaian)

| Feature | Status | Notes / where |
|---|---|---|
| Consist from timetable, per-car placement (§7.4) | ✅ | `engine/world/train_visual.cpp` |
| Body/bogie/coupling GLBs, normalisation (§7.3) | ✅ | `rolling_stock.cpp`; lokos downloaded from R2 |
| Multi-UV, wrap modes, emissive strength, linear MR maps, texture transform | ✅ | `gltf.cpp`, EMOD v4 (`tools/glbinfo` to audit) |
| Box fallback for missing models | ✅ | |
| **Last KRL cab flipped** (`krl-8` last car: yaw+π, −roll, −pitch) | ❌ | small fix in `train_visual.cpp` |
| **Door animation** (`pintu-kiri/kanan` clips scrubbed by dwell) | ❌ | needs glTF animation parsing → EMOD → node player |
| Pantograph clips frozen at last frame | ❌ | same as above |
| Body sway (§7.5), cant roll | ❌ | `uji3dGoyang.ts` |
| Head/tail lights at night, Semboyan 21 tail marker | ❌ | `Konst:398-446` |
| Resting trains lights off (`istirahat`) | ❌ | bridge already has state |
| Freight `gd/gk` as GLB vs box per `SLOT_BALOK` | 🟡 | verify against `Konst:490-533` |

## Track, signals, points

| Feature | Status | Notes |
|---|---|---|
| Bézier track, arc-length LUT, vertical profile, rails/ballast, bridges (deck+piers), tunnels | ✅ | `track_graph`, `rail_profile`, `rail_builder` |
| Warren truss bridges, viaduct columns | ❌ | `uji3dJembatan.ts` |
| Profile step 3 (parallel roadbed pairing) | ❌ | heights of parallel tracks may differ slightly |
| Colour-light signal geometry (§6.2), coronas, LOD sphere | ✅ | `signal_visual.cpp` |
| Signal plate textures (bolts, number text, "3" strips), lit "angka" overlay | ❌ | |
| Semaphore (§6.3) with spring | ✅ | only `cpd-chaos.json` uses it |
| Pengulang 9C | ✅ | `awn.json` |
| Points arrows, padlock, glow (§6.4) | ✅ | `skalaWesel` distance scale missing |
| Route ribbons, occupancy, hover preview, tooltips | ✅ | `route_visual.cpp` |
| Trackside boards (s35, taspat, km post), 10G stop mark | ❌ | `uji3dPapan.ts` |
| JPL level-crossing gates (`bangun3d.ts:657-691`) | ❌ | needs `distToTrainAhead` via bridge |
| Rail "ikonik" yellow line when far | ❌ | |

## Terrain & scenery

| Feature | Status | Notes |
|---|---|---|
| DEM z13/z10, satellite z14 + detail z16/z17, corridor carving | ✅ | `terrain.cpp`, `tools/fetch_tiles` |
| Streaming/eviction by distance (`ubinStream.ts`) | ❌ | everything loads at start; fine for one corridor |
| Brush deltas `world.tanah.delta`, bridge trough carving | ❌ | |
| Trees from green mask (instanced) | ✅ | `vegetation.cpp`; `vegMask` not applied |
| Station buildings from `hiasan.objek` | ✅ | `game.cpp buildWorld()` |
| `hiasan.garis` spline objects (fences, LAA poles, platforms) | ❌ | `uji3dSpline.ts` |
| Baked OSM city `public/kota/<slug>.json` (buildings) | ❌ | `kota3d.ts`; bks has data |
| Procedural KRL station | ❌ | `uji3dStasiunKRL.ts` |
| Clouds | ❌ | |

## Camera, sky, UI

| Feature | Status | Notes |
|---|---|---|
| Orbit, free-fly, Trainz compass | ✅ | `compass.cpp` |
| Cab / side / tail / top camera modes, telescope | ❌ | `dunia3d.ts:1546-1591` |
| Sun + light ladder from clock, gradient sky | ✅ | `sun.h`; fog is exponential, reference is linear corridor fog |
| Atmosphere sky mode | ➖ | |
| Meja layan (schematic panel) | ✅ | `panel_view.cpp`; layout from `panel.ts` via bridge |
| Time scale, set clock, pause | ✅ | bridge authoritative |
| Mode pemula (destination menu) / ahli | ✅ | |
| Train card, Semboyan 40, Hapus KA | ✅ | S40 audio ritual not mirrored |
| Permission cards (sepur salah / izin terisi / mengikuti) | ❌ | flags come back from `set_route`; no confirm UI |
| Warta, genta, block-bar tasks, BLB penalty, auto-warp | ❌ | |
| Audio (platform PA, genta, train sounds) | ❌ | no audio system yet |
| Editor drawers (REL, surveyor, ukur, tata hiasan, tanah) | ➖ | stay in the web client; the engine reads the same save |

## Platform

| Feature | Status | Notes |
|---|---|---|
| macOS (GLFW + OpenGL 4.1) | ✅ | |
| Texture compression (ETC2/ASTC/BC) in EMOD | ❌ | raw RGBA today; required before mobile |
| GLES3 backend + Android EGL, Flutter `Texture` plugin | ❌ | prerequisite: C++ sim port (no Node on Android) |
| WebAssembly (WebGL2) | ❌ | |
| Automated tests (`ctest`: math, parsers, LUT vs TS, golden images) | ✅ | `tests/`, `ctest --preset mac-debug` (+ `mac-debug-gpu`) |
