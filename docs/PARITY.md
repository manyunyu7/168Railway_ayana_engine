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
* **The engine draws and sends clicks.** Visual parity work happens in `engine/world/*`; the world build/draw
  shared by the native app and the web ABI is `engine/app/world_scene.*` (`examples/ppka/*` keeps input, HUD,
  panel and cameras).
* **Assets are converted offline** (`tools/convert`, `tools/fetch_tiles`, `tools/fontgen`); the runtime
  reads only our own formats (`.emod`, `.dem/.sat`, `.efnt`). Third-party decoders live in `tools/` only.
* **Verify with screenshots.** Every example supports `ENG_CAPTURE=/tmp/x.ppm` (+ `ENG_CAPTURE_FRAME`,
  `ENG_VIEW=dist,yaw,pitch`, `ENG_AUTOCLICK`, `ENG_AUTOROUTE`, `ENG_AUTOHOVER`, `ENG_AUTOPANEL`,
  `ENG_AUTOSELECT`, `ENG_AUTOJUMP`, `ENG_AUTOMENU`/`ENG_AUTOCHOOSE`/`ENG_AUTOCONFIRM`, `ENG_CAMERA=kabin|samping|ekor|atas|jalan`,
  `ENG_FOLLOW=<no>`, `ENG_TEST_GARIS=1`); convert with `sips -s format png` and look at it.
* Build with `mac-debug` (ASan/UBSan, `-Werror`) before pushing; play with `mac-release`.
* Push to `github.com/manyunyu7/168Railway_ayana_engine` after every fix.

## Trains (rangkaian)

| Feature | Status | Notes / where |
|---|---|---|
| Consist from timetable, per-car placement (§7.4) | ✅ | `engine/world/train_visual.cpp` |
| Body/bogie/coupling GLBs, normalisation (§7.3) | ✅ | `rolling_stock.cpp`; lokos downloaded from R2 |
| Multi-UV, wrap modes, emissive strength, linear MR maps, texture transform | ✅ | `gltf.cpp`, EMOD v4 (`tools/glbinfo` to audit) |
| glTF animation clips (node TRS, LINEAR/STEP; CUBICSPLINE as LINEAR) | ✅ | `gltf.cpp`, EMOD v7, `scrubAnimation`/`computeWorld`, `ModelRenderer::draw(..., worldOverride)`; `glbinfo` lists clips |
| Box fallback for missing models | ✅ | |
| Last KRL cab flipped (`krl-8` last car: yaw+π, −roll, −pitch) | ✅ | `train_visual.cpp` (`nryJr205KuhaBadan` last car) |
| Door animation (`pintu-kiri/kanan` clips scrubbed by dwell) | ✅ | `train_visual.cpp`: `tPintu` runs in sim time (clock delta), opens while `dwell` and not `istirahat`, both clips at the same time as `setelPintu` (the reference opens both sides; there is no platform-side selection). Only the JR205 set carries the clips (`glbinfo`); `traintest ENG_DOORS=1` forces dwell |
| Pantograph clips frozen at last frame | ✅ | `rolling_stock.cpp` `restLocal/restWorld` (`panto-*` at `duration`) |
| Body sway (§7.5), cant roll | ✅ | `engine/world/sway.h` (`hitungGoyang`, `medan`, `kurvaRel`, `skalaLaju`; `tests/test_sway.cpp`); curvature from the bridge's coupler tangents `t1/t2`; roll into the YZX Euler, naik/geser at the coupler points. Longitudinal `aksel` (cab pitch only) not tracked |
| Head/tail lights at night, Semboyan 21 tail marker | ✅ | light nodes per `peranLampu` (+ outer-third tail filter, headlight copy, `lampuKarangan` fallback); additive coronas (`KORONA_PX 7`, directional opacity) only at night (clock < 6 or ≥ 18; the reference also shows them dimmer by day). S21: day red plate / night lantern (red glass rear, green front) at `x = −(L/2 − 0.38), y = 1.54, z = ±W/2`. Tunnel switch not done |
| Resting trains lights off (`istirahat`) | ✅ | bridge `istirahat` (`train.ts`); also closes doors and hides S21, as the reference |
| Freight `gd/gk` as GLB vs box per `SLOT_BALOK` | 🟡 | verify against `Konst:490-533` |

## Track, signals, points

| Feature | Status | Notes |
|---|---|---|
| Bézier track, arc-length LUT, vertical profile, rails/ballast, bridges (deck+piers), tunnels | ✅ | `track_graph`, `rail_profile`, `rail_builder`. Tunnel mouths: engine addition — the carve corridor narrows to 4.5/13 m at the mouth (instead of fading to 0) and the first 12 m inside the portal are registered, so the portal ring is not buried when the z13 DEM sits above the rail (`railtest ENG_TARGET=mouth:n`) |
| Warren truss bridges, viaduct columns | ✅ | `rail_builder.cpp`: shape `jenisJembatan ?? bentukJembatan(chain span, deck height)`; truss members (8 m panels, diagonals, verticals, top bracing, portals) in a steel mesh per chunk replacing the parapets; piers every 28 m at the structure centre (84 m under a truss), twin columns 1.3 m in from the edges for a viaduct. `railtest` draws the real terrain when tiles exist (`ENG_TARGET=bridge:n\|tunnel:n`) |
| Profile step 3 (parallel roadbed pairing) | ❌ | heights of parallel tracks may differ slightly |
| Colour-light signal geometry (§6.2), coronas, LOD sphere | ✅ | `signal_visual.cpp` |
| Signal plate textures (bolts, number text, "3" strips), lit "angka" overlay | ❌ | |
| Semaphore (§6.3) with spring | ✅ | only `cpd-chaos.json` uses it |
| Pengulang 9C | ✅ | `awn.json` |
| Points arrows, padlock, glow (§6.4) | ✅ | `skalaWesel` distance scale missing |
| Route ribbons, occupancy, hover preview, tooltips | ✅ | `route_visual.cpp` |
| Trackside boards (s35, taspat, km post), 10G stop mark, buffer stop, scenery `platform` box | ✅ | `board_visual.cpp`; text atlas rasterised from `font.efnt`; 5 draw calls per corridor. `dirmarker`/trackmarks are not drawn (as in TS) |
| JPL level-crossing gates (`bangun3d.ts:657-691`) | ✅ | `jpl_visual.cpp`; `step` returns `jpl:[{id,closed}]` (`World.jplClosed`, cached 0.25 s sim time); arm 5 s, lamps blink 460 ms. Road strip not drawn |
| Rail "ikonik" yellow line when far | ❌ | |

## Terrain & scenery

| Feature | Status | Notes |
|---|---|---|
| DEM z13/z10, satellite z14 + detail z16/z17, corridor carving | ✅ | `terrain.cpp`, `tools/fetch_tiles` |
| Streaming/eviction by distance (`ubinStream.ts`) | ✅ | `Terrain::update(centre, dt)`: near z14 tiles live within `UBIN_R_MUAT` 4000 m of the tile edge, evicted beyond 6000; detail z16 1500/2200 m, z17 450/800 m; far layer + DEM core resident. 2 requests per 220 ms nearest first (`prime()` fills the start unthrottled), ≤ 2 GPU jobs (mesh build / texture upload) per frame, blocks re-cut 0.25 s after a finer tile arrives; loading colour `0x1a2027` until imagery is there (near patches and far tiles alike). The far layer (z10–12, ≤ 40 tiles, `zoomMuat`) is requested eagerly on the first check, outside the 2-per-check budget, and its meshes (uncarved DEM, 6 cells per z14 tile) are punched only where a near tile is **built** (`segarkanLubangJauh`) and re-cut when that set changes — the near layer's range covers the whole corridor, so punching the range left everything beyond 4 km in the backdrop colour on long maps (gombong-wns). `tests/test_terrain`: synthesized index (client `indeksMedan.ts` rules) → 4 far tiles requested on a throttled check, far tile geometry whole. Trees scatter per 192 m cell as the imagery arrives (`Vegetation::update`). Native: per-tile files `assets/terrain/<map>/` (`fetch_tiles`, `index.json` target desktop) else the monolithic `.dem/.sat` as an in-memory source. Web: only `index.json` is served; DEM = Terrarium PNGs and satellite = `tiles.168railway.com` (+ Esri fallback) decoded by the browser (`ubinAyana.ts`, 6 fetches, z16/z17 composed 2×2 like `fetch_tiles`), Mojokerto 3D view: ~5.7 MB of tiles instead of 48 MB up front. Not ported: `rAmbil` prefetch ring, touch tiers |
| Brush deltas `world.tanah.delta`, bridge trough carving | ✅ | `terrain.cpp`: `setBrushDeltas(world["tanah"])` (8 m grid, bilinear) added before carving; `RailSample::bridgeBlend` registers deck chords, ground lowered to deck bottom − 1.5 m within 7 m, blending to 26 m, never raised (`tests/test_terrain`) |
| Trees from green mask (instanced) | ✅ | `vegetation.cpp`; `vegMask` not applied |
| Station buildings from `hiasan.objek` | ✅ | `game.cpp buildWorld()` |
| `hiasan.garis` spline objects (fences, LAA poles, platforms) | ✅ | `engine/world/spline.h` (centripetal Catmull-Rom + `bingkaiGaris` tiling), `garis_visual.cpp`: GLB classes from `model.json.garis[]` instanced (`AssetCatalog::findGaris`, ids `garis:<id>[:tiang|:slotN]`), procedural `peron`/`peron-kanopi`/`peron-tiang`/`tembok-beton` baked per colour; `datar` = straight grade; `peron-krl*` prototypes not ported (see KRL station row). No save has `garis` yet — `ENG_TEST_GARIS=1` injects a platform + fence + wall along the station track |
| Baked OSM city `public/kota/<slug>.json` (buildings) | ✅ | `city_visual.cpp`; per 640 m chunk × palette meshes (no vertex colours), frustum-culled; bks uses the `bekasi` bake (alias in `game.cpp`). Roads not drawn (as in TS); `hijau`/`pohon` data unused |
| Procedural KRL station | 🟡 | `krl_station.cpp` (`KrlStation`, `krlLayout`): hall + sweeping roof, bowstring stair arch, concourse with a stair per island platform, platforms/canopies/portals/furniture, LAA wires + gantries; flat colours instead of the canvas textures (ACP joints, letters, boards). No save references `stasiun-krl-*` yet, so it is not wired into `game.cpp`; `railtest ENG_TEST_KRL=<tracks> ENG_TARGET=krl` |
| Clouds | ✅ | `cloud_visual.cpp`: world-pinned billboard sprites over the corridor bbox + 6 km, 0.35/km² (40..220), layers 760–1100 m (¾, 420–1250 m wide) and 1250–1750 m, 8 procedural blob textures × 3 opacities, tinted by the ladder's `awan` column (`Sky::cloudTint`), fogged, slow wind drift (the reference sprites are static). `terraintest` draws them (`ENG_CLOCK`, `ENG_NO_CLOUDS`) |

## Camera, sky, UI

| Feature | Status | Notes |
|---|---|---|
| Orbit, free-fly, Trainz compass | ✅ | `compass.cpp` |
| Cab / side / tail / top / walk camera modes, telescope | ✅ | `examples/ppka/camera_rig.*`: profiles (near/far/fov/fog/redam), `MATA_KABIN` per loco, rigs (kabin look 100 m ahead, samping 28/7 m, atas 120 m heading-up, ekor 34 m), damping `k = 1−exp(−redam·dt)` with the 0.4 s blend-in, kabin drag = neck (decays after 1.2 s), others orbit the subject (`orbitSubjek`), scroll = the mode's parameter (kabin: eye forward/back), `Z` telescope fov/4 ≥ 8° (τ 0.09 s, drag scaled by tan ratio), jalan 1.62 m eye / 4.5 / 12 m/s with step bob. Keys 1–6, top-bar buttons, `,`/`.` cycle the subject (selected train, else nearest). Fog: linear ranges mapped to the exp² density that gives 50 % at the midpoint (corridor formula for bebas/atas). Cab sway ported (`uji3dGoyang.ts goyangKabin` → `sway::goyangKabin`, curvature over `BASIS_KURVA` 25 m from the subject polyline, acceleration filtered τ 0.35 s like `perbaruiAksel`, shifting terms damped by `sqrt(tan ratio)` while zoomed, roll into `up`). Engine addition (native + web): in kabin the eye can be moved — W/S or ↑/↓ along the vehicle axis (−L/2..+L/2), A/D sideways (±1.2 m), Q/E height (0.5..3.5 m), Shift faster, scroll forward/back, `R` back to `MATA_KABIN`; nothing persisted (`CameraRig::kabinMaju/Sisi/Naik`, `eng_key` KeyQ/KeyE/KeyR, adapter `TOMBOL_KABIN`). Not ported: jalan collision/jump, touch joystick |
| Sun + light ladder from clock, gradient sky | ✅ | `sun.h`; fog is exponential, reference is linear corridor fog |
| Atmosphere sky mode | ➖ | |
| Meja layan (schematic panel) | ✅ | `panel_view.cpp`; layout from `panel.ts` via bridge |
| Time scale, set clock, pause | ✅ | bridge authoritative |
| Mode pemula (destination menu) / ahli | ✅ | |
| Train card, Semboyan 40, Hapus KA | ✅ | S40 audio ritual not mirrored |
| Permission cards (sepur salah / izin terisi / mengikuti / batalkan rute lawan) | ✅ | bridge `setCandidate` returns `needsConfirm{kind, judul, rute, akibat, batas, tombol, confirm}` with the `ui/rute.ts` wording (incl. `jarakPenghuni`, `sisaHalangan`); `confirm` command re-issues with `izin`/`sepurSalah`/`izinTerisi`/`batalLawan`; `set_route {index}` now counts the `route_menu` list. App: `Game::handleRouteResponse` → card (`drawIzinCard`, 12 s countdown, Enter/Esc), chained cards (wrong line, then occupied). No countdown audio |
| Warta, genta, block-bar tasks, BLB penalty, auto-warp | ❌ | |
| Audio (platform PA, genta, train sounds) | ❌ | no audio system yet |
| Editor drawers (REL, surveyor, ukur, tata hiasan, tanah) | ➖ | stay in the web client; the engine reads the same save |

## Platform

| Feature | Status | Notes |
|---|---|---|
| macOS (GLFW + OpenGL 4.1) | ✅ | |
| Texture compression (ETC2/BC) in EMOD | ✅ | EMOD v5: `convert --target web\|desktop\|android`, KTX2 twins transcoded, mip chains, RGBA8 fallback; v6 placeholders (`--textures external`) for host-streamed textures; ASTC later |
| GLES3 backend + Android EGL, Flutter `Texture` plugin | ❌ | prerequisite: C++ sim port (no Node on Android) |
| WebAssembly (WebGL2) | ✅ | `ayana` target = the engine as a C ABI ES module (`engine/api`) inside ppka-wannabe-2 (`src/tiga-ayana/duniaAyana.ts`, `?renderer=ayana`): the browser runs the TS sim, the engine draws; terrain streamed from the tile servers by camera distance (`Terrain::loadIndex/provideDem*/provideSat*`, `eng_terrain_tile_rgba`), geometry-only `.emod` per catalog slot on demand + KTX2 textures via `web/ktx2.js` (`ayanaApi`). Not yet in the web adapter: surveyor/hiasan/tanah editing tools, HUD signal plates, floating meja layan (stubs warn once) |
| Automated tests (`ctest`: math, parsers, LUT vs TS, golden images) | ✅ | `tests/`, `ctest --preset mac-debug` (+ `mac-debug-gpu`) |
