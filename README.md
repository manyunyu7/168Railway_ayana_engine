# Ayana Engine — a from-scratch 3D engine for the 168Railway PPKA Simulator

![Mojokerto station in the engine](docs/screenshot-mojokerto.jpg)

Ayana is a small C++20 game engine written from zero for the **PPKA Simulator** (a train-dispatcher
game): no Unity, no Godot, no three.js. It renders the simulator's real maps, rolling stock and
interlocking in 3D, and it is designed to run everywhere the game needs to live — macOS today,
Android (Flutter `Texture`) and the web (WebAssembly) next.

The **simulation is not rewritten**: the original TypeScript engine in `ppka-wannabe-2` runs as a
child process and the engine talks to it over a tiny JSON-lines protocol (`bridge/PROTOCOL.md`).
That protocol mirrors the planned C API of the C++ simulation port, so the Node bridge can be
swapped for a native library later without touching the renderer.

## What works

| Area | Status |
|---|---|
| Math, JSON parser, GLB parser, own binary model format (`.emod`) | own code, no dependencies |
| Renderer | OpenGL 4.1 / GLES 3 behind a thin RHI, metallic-roughness PBR, `KHR_materials_unlit`, alpha mask/blend, fog, frustum culling, instancing |
| Terrain | Terrarium DEM (z13/z10) + satellite imagery (z14 → z17 near stations), rail corridor carving (embankments/cuttings), per-texture ground patches |
| Track | cubic-Bézier segments from the map save, arc-length LUT, 1-D vertical profile, ballast + rails (gauge 1.068 m), bridges, tunnels |
| Signals & points | colour-light and semaphore signals with live aspects, point arrows with set/locked colours, screen-space picking, hover tooltips, route ribbons |
| Trains | consists from the timetable, body/bogie/coupling GLBs with the simulator's normalisation rules, box fallback |
| Scenery | station buildings from the save's `hiasan`, instanced trees from the satellite green mask |
| Sky & light | gradient sky and light ladder driven by the simulated clock |
| Camera | orbit, free-fly, Trainz-style compass (right-click focus, glide, Ctrl+arrows) |
| HUD | clock, score, time scale, train list, projected train labels, message log |

![Bekasi with the AI dispatcher](docs/screenshot-bekasi.jpg)

## Build (macOS)

```bash
brew install cmake ninja glfw
cmake --preset mac-release && cmake --build --preset mac-release   # or mac-debug (ASan + UBSan)
```

The reference project must sit next to this repo: `../ppka-wannabe-2` (maps in `src/data/*.json`,
models in `public/model3d/`). Node ≥ 20 is needed for the simulation bridge (`npx tsx`).

## Play

```bash
./build/mac-release/fetch_tiles mojokerto            # once per map: DEM + imagery -> assets/terrain/
./build/mac-release/ppka mojokerto 07:00 --no-ai     # map, start clock; drop --no-ai to watch the AI
```

| Input | Action |
|---|---|
| left drag / scroll | orbit / zoom |
| right-click tap | fly the camera focus (compass) to that ground point |
| right-click hold | glide; speed grows with cursor distance from the screen centre |
| Ctrl + arrows / arrows | nudge focus / rotate & tilt |
| F, then WASD + QE, Shift | free-fly camera |
| click a signal | set a route (expert mode) or cancel it; the tooltip explains rejections |
| click a point arrow | flip the point |
| space, +, − | pause, faster, slower |

Maps known to work: `mojokerto` (one station, 64 trains), `bks` (Bekasi, 3 stations, 503 trains).

## Web (WebAssembly)

![CC203 rendered by the Wasm build in Chromium](docs/screenshot-wasm.jpg)

```bash
brew install emscripten
cmake --preset wasm && cmake --build --preset wasm      # -> build/wasm/viewer.html (+ .wasm 200 KB)
npx serve build/wasm                                    # open /viewer.html
```
The same RHI runs on WebGL2 (`ENG_GL_ES`); only the window hints and the main loop differ.
The simulator bridge is native-only, so the web build currently ships the model viewer.

## Tests

```bash
cmake --preset mac-debug && cmake --build --preset mac-debug
ctest --preset mac-debug        # 7 tests, ~5 s: math, JSON, GLB/EMOD, track LUT vs TS, compass, profile, sim bridge
ctest --preset mac-debug-gpu    # golden image (opens a window; compares tests/golden/coupling.ppm)
```

Tests live in `tests/` (one executable each, `tests/check.h` is the whole framework). `test_track`
compares the C++ track geometry with a dump of the TypeScript engine (`tests/ref/mojokerto_track.json`;
regenerate with `cd ../ppka-wannabe-2 && npx tsx ../game-engine-experiment/tests/ref/track_ref.ts`).
`test_bridge` skips when `npx` is missing. After an intentional rendering change run
`./build/mac-debug/test_golden --update` and commit the new `tests/golden/coupling.ppm`.

## Layout

```
engine/   math · rhi (GPU layer) · core (window, cameras, JSON) · asset (GLB, EMOD)
          render (PBR, sky, text) · sim (bridge client) · world (track, rails, terrain,
          signals, points, routes, rolling stock, trains, vegetation)
bridge/   sim-bridge.ts — JSON-lines front end for the TypeScript simulation
examples/ ppka (the game) · viewer · railtest · terraintest · traintest · tracktest · simtest · cube
tools/    offline converters (the only place third-party decoders are allowed):
          convert (GLB → .emod) · fetch_tiles (DEM/imagery → .dem/.sat) · fontgen (TTF → .efnt)
docs/     world-spec.md (how the reference three.js scene is built, with exact constants)
```

## Roadmap / what is still missing

See **[docs/PARITY.md](docs/PARITY.md)** — the feature-by-feature checklist against the reference
three.js client, plus the working rules for contributors.

## Principles

* Everything the runtime executes is our code; third-party code (stb) lives only in offline tools.
* World coordinates stay `double` (Web Mercator, ~1.2e7 m); the scene is `float` around a local origin.
* Game rules live in one place — the simulator. The engine draws state and sends clicks.
* Every example can render headless (`ENG_CAPTURE=/tmp/x.ppm`) for screenshot-based verification.
