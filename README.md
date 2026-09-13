# engine — a from-scratch 3D engine for PPKA

C++20, no third-party engine. External libs: GLFW (desktop window/input) and the GPU API (OpenGL).
Assets and map data come from `../ppka-wannabe-2` (never copied into this repo).

```bash
cmake --preset mac-debug && cmake --build --preset mac-debug
./build/mac-debug/cube                                   # example 1: spinning cube
./build/mac-debug/convert in.glb out.emod --max-texture 2048   # GLB -> engine format (offline)
./build/mac-debug/viewer out.emod                        # example 2: PBR model viewer
ENG_CAPTURE=/tmp/shot.ppm ./build/mac-debug/viewer x.emod      # capture frame 30, then exit
```

Layout: `engine/math`, `engine/rhi` (GPU layer), `engine/core` (window, camera, JSON),
`engine/asset` (GLB parser, EMOD format), `engine/render` (PBR), `examples/`, `tools/` (offline converters —
the only place third-party decoders are allowed).
