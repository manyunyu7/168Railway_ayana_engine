# engine — a from-scratch 3D engine for PPKA

C++20, no third-party engine. External libs: GLFW (desktop window/input) and the GPU API (OpenGL).
Assets and map data come from `../ppka-wannabe-2` (never copied into this repo).

```bash
cmake --preset mac-debug && cmake --build --preset mac-debug
./build/mac-debug/cube                                   # example 1: spinning cube
./build/mac-debug/convert in.glb out.emod --max-texture 2048   # GLB -> engine format (offline)
./build/mac-debug/viewer out.emod                        # example 2: PBR model viewer
ENG_CAPTURE=/tmp/shot.ppm ./build/mac-debug/viewer x.emod      # capture frame 30, then exit
./build/mac-debug/simtest kroya 15:00                     # example 3: drive the PPKA TS sim via bridge/sim-bridge.ts
```

Layout: `engine/math`, `engine/rhi` (GPU layer), `engine/core` (window, camera, JSON),
`engine/asset` (GLB parser, EMOD format), `engine/render` (PBR), `engine/sim` (SimProcess: PPKA TypeScript
simulation as a child process, protocol in `bridge/PROTOCOL.md`), `examples/`, `tools/` (offline converters —
the only place third-party decoders are allowed).

## Playable PPKA scene

```bash
./build/mac-debug/fetch_tiles mojokerto          # once: DEM + satellite tiles -> assets/terrain/
./build/mac-release/ppka mojokerto 07:00          # map, start clock; --no-ai for manual dispatching
```
Controls: LMB drag = orbit, scroll = zoom, F = fly camera (WASD/QE, RMB look, Shift = fast),
click a signal = set/cancel route, click a point arrow = flip, space = pause, +/- = time scale, Esc = quit.
The simulation runs in the original TypeScript engine via `bridge/sim-bridge.ts` (Node); see `bridge/PROTOCOL.md`.
Module tests: `tracktest`, `railtest`, `terraintest`, `traintest`, `simtest`.
