# Common brief for module agents

Repo: `/Users/henryaugusta/Developer/game-engine-experiment` — a from-scratch C++20 3D engine
that will render the PPKA train-dispatcher simulator. Reference project (READ ONLY, never modify,
never add files): `/Users/henryaugusta/Developer/ppka-wannabe-2` (TypeScript + three.js, Indonesian).

Rules
- Code, comments, identifiers, commit messages: **English**. Style: match existing files (compact, `eng` namespace).
- `-Wall -Wextra -Werror`; the `mac-debug` preset has ASan+UBSan — everything must run clean.
- Build: `cmake --preset mac-debug && cmake --build --preset mac-debug`. Binaries in `build/mac-debug/`.
- Do NOT edit the root `CMakeLists.txt`. `engine/world/*.cpp` is globbed automatically; a new
  example/tool gets its own `examples/<name>/CMakeLists.txt` / `tools/<name>/CMakeLists.txt`
  (auto-included; use `add_executable(<name> main.cpp)` + `target_link_libraries(<name> PRIVATE engine)`;
  tools using third-party headers add `target_compile_options(<name> PRIVATE -Wno-error -fno-sanitize=undefined)`).
- Third-party code is allowed ONLY in `tools/` (offline converters; `tools/third_party/` has stb_image,
  stb_image_resize2, stb_truetype). The runtime engine never decodes PNG/JPEG — use own binary formats.
- Disk is very tight (≈3 GB free). Don't write files > 100 MB; delete `.ppm` captures after converting.
  Convert models with `--max-texture 1024` (station) or `512` (vehicles).
- Verify visually: run your example with `ENG_CAPTURE=/tmp/x.ppm ./build/mac-debug/<exe> ...` (captures
  frame 30 and exits; follow the pattern in `examples/viewer/main.cpp`), `sips -s format png /tmp/x.ppm --out /tmp/x.png`,
  then look at the PNG with the Read tool. Iterate until it looks right. Delete the ppm.
- Commit your work with git when done (only your files; `git add <paths>`; message in English).

The spec: `docs/world-spec.md` (exact constants/formulas, with file:line refs into ppka-wannabe-2 —
open those files when the spec is not enough). Map for the first scene: **`src/data/mojokerto.json`**.

Existing engine API (read the headers)
- `engine/math/math.h` (vec2/3/4, mat4 column-major, quat, `trs`), `engine/math/geometry.h` (AABB, Ray, Frustum, `screenRay`).
- `engine/rhi/rhi.h` — GPU layer: `createMesh/drawMesh`, `createTexture(w,h,Format,pixels,mipmap,srgb)`, programs/uniforms.
- `engine/render/mesh_builder.h` — `MeshBuilder` (vertex/quad/box/append, `upload()` → `rhi::Mesh`); vertex = pos, normal, uv (32 B).
- `engine/render/model_renderer.h` — `Lighting`, `GpuModel::upload(Model)`, `ModelRenderer::beginFrame / draw(model, transform, frustum) / drawMesh(mesh, Material, baseTex, transform) / flushTransparent`.
  `Material` (from `engine/asset/model.h`): baseColor, metallic, roughness, emissive, alphaMode, doubleSided.
- `engine/asset/emod.h` + `tools/convert` — GLB → `.emod` (`./build/mac-debug/convert in.glb out.emod --max-texture N`); `loadEmod`.
- `engine/asset/gltf.h` — own GLB parser (`loadGlbFile` → `Model`, images still encoded).
- `engine/render/text.h` (HUD text; font `assets/font.efnt`), `engine/render/sky.h`, `engine/core/orbit_camera.h`, `engine/core/fly_camera.h`, `engine/core/window.h`, `engine/core/json.h` (`Json::parse`, `[]`, `numberOr/intOr/stringOr/boolOr`, `.arr/.obj`).
- `engine/sim/sim_process.h` — `SimProcess` drives the TS sim (`load(map)` returns the raw save under `world` + `summary`; `step(dt)` returns trains/points/signals/routes; see `bridge/PROTOCOL.md`). `examples/simtest` shows usage.
- Shared world headers: `engine/world/coords.h` (`WorldOrigin`: world double ↔ scene float, yaw convention), `engine/world/height_source.h` (`HeightSource`, `RailProfile`, `RailSample`).

Assets: `../ppka-wannabe-2/public/model3d/<berkas>` (catalog `model.json`); missing `pk-*.glb` lokos can be
downloaded from `https://168railway.space/pk/model3d/<berkas>`. Terrain tiles: `https://tiles.168railway.com/terrarium/{z}/{x}/{y}.png`
(Terrarium: h = R*256 + G + B/256 - 32768) and `https://tiles.168railway.com/satellite/{z}/{x}/{y}.png`
(may 204 → fall back to Esri `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`).
