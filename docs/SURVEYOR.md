# Surveyor / editor tools on the Ayana renderer

Owner decision (2026-09-14): the 3D surveyor tools of the web client are ported to the Ayana renderer so the
Ayana path no longer needs three.js for editing. Rule of the port: **pure logic stays in TypeScript** (DOM
drawers, keyboard state machines, snap / ramp maths, save format, the headless `src/engine` graph ops); only what
needed three.js (raycasts, gizmo meshes, highlights, dynamic transforms, terrain re-carve, rail rebuild) moves
behind the engine's **editing ABI** (`engine/api/engine_api.h`, "editing" block; `engine/api/engine_api_edit.cpp`
→ `engine/app/world_edit.cpp`). Verification: `ctest` (`tests/test_edit.cpp` drives the real C entry points on
Mojokerto through a hidden GL window), `tsc` / `npm run build`, and one headless smoke
(`ppka-wannabe-2/playtest/_cek-ayana-tata.mjs`). No screenshot comparisons.

Status: **phase 1** = audit + ABI + first tool (Tata objek). **Phase 2 (2B, landed)**: node height (`tinggiAyana.ts`),
editor rel 3D (`relAyana.ts`), kuas tanah + pohon (`kuasAyana.ts`) — see §2 additions and §4. Gambar garis / objek
rel / ukur = 2A (`eng_markers`, `garisAyana.ts`, `objekRelAyana.ts`, `ukurAyana.ts`).

## 1. Audit — what each tool needed from three.js

`ref` = `ppka-wannabe-2/src/...`. "Pure" = kept as-is (or moved to a three-free module). "three" = replaced by
the ABI call in the last column.

| Tool | ref | three-specific needs | Pure logic (kept) | ABI |
|---|---|---|---|---|
| **Tata objek** (hiasan placement: palette, ghost, rotate ring, drag / Alt-height, Q/E/[ ]/=/-, F snap to rail, L lock, Del, save) | `tiga/uji3dTata.ts` (1270 lines), hosted in `tiga/dunia3d.ts:1415-1434, 3434-3466, 4009` | ground raycast (`titikTanah`: raycaster vs terrain tiles), object pick (raycast vs `grup` + screen-box tolerance), `Box3Helper` selection box, `RingGeometry` gizmo + needle/knob (+ hit ring, plane intersection for the cursor angle), translucent ghost clone (`jadikanHantu`), per-object `Object3D` transform on every edit, thumbnail render (`MesinThumb`, offscreen WebGL) | snap constants, key ramp (`TUNDA/RAMP/LAJU_*`), `bulatKe`, screen tolerance rule (`TOL_PILIH/TIPIS_PX/LUAS_KECIL`, `jarakKeKotak`, `sasaranTipis`), palette / props DOM, lock / delete / F-cycle logic, `impor/ekspor` (`world.hiasan.objek`) → **`tiga/tataInti.ts`** (three-free, re-exported by `uji3dTata.ts`; tests/tata3d.ts unchanged) | `eng_pick_ground`, `eng_ground`, `eng_pick_object` (triangle-precise + the same 16 px tolerance in C++), `eng_hiasan_set/add/remove/info`, `eng_model_size`, `eng_highlight`, `eng_gizmo` + `eng_gizmo_hit` + `eng_gizmo_angle`, `eng_ghost`, `eng_project` (angle label). **Ported**: `tiga-ayana/tataAyana.ts` |
| **Gambar garis** (spline fences / walls / platforms / LAA: points, tiles along a centripetal Catmull-Rom, Enter/Backspace/I/X/Q/A/L, dblclick, "ikuti rel") | `tiga/uji3dSpline.ts` (1399), `dunia3d.ts:1436-1450, 3886-3923` | `THREE.CatmullRomCurve3` (arc-length `getPointAt/getTangentAt`), `InstancedMesh` per part, procedural prototypes (peron / tembok boxes), handle spheres + torus rings + end markers as raycast targets, preview group rebuilt per move, raycast vs `grup` + projected-polyline fallback, thumbnails | `bersih`, `bingkaiGaris` tile placement maths (step from the catalog, remainder scaling, pitch from sampled heights), nearest point / segment, `rabaKelas`, key state machine, insert / remove / split / continue, `jalurRel` (graph walk), `impor/ekspor` (`world.hiasan.garis`) | `eng_pick_ground`, `eng_pick_object` (`garis:<i>`: nearest projected polyline within 16 px), `eng_garis_set` (rebuild one entry; the engine already tiles garis classes: `engine/world/garis_visual`), `eng_highlight("garis:i")`, `eng_project` (handles as DOM dots or a future `eng_markers`). The Catmull-Rom sampler exists in `engine/world/spline.h`; the TS side keeps only the point list |
| **Kuas tanah** (Naik / Turun / Rata / Halus 8–200 m → `world.tanah`; Pohon hapus / tanam → `world.vegMask`) | `dunia3d.ts:1069-1116, 3444-3503, 3786-3879, 3929-3959`, `tiga/medan3d.ts:518-549` | ground raycast, brush ring (`RingGeometry`, depth-test-off), terrain tile vertex rewrite + normals (`segarkanTile`), forced fine blocks, re-seat objects / splines, vegetation cell rebuild | per-cell brush maths (`sapuTanah`: smoothstep falloff, rata / halus rules, 1e-4 pruning), `world.tanah` format (`kisi` 8, `"gx,gz"` keys), commit on pointer-up | `eng_pick_ground`, `eng_terrain_delta` (whole `world.tanah`; only tiles whose cells changed are re-cut, hiasan / garis re-placed), `eng_gizmo("move" ring as the brush ring)`. vegMask: not in the ABI yet (engine trees come from the imagery mask; a `eng_veg_mask(json)` is phase 2) |
| **Node height** (Alt-drag, T = follow DEM, props-panel ±0.5 / Ikut DEM) | `dunia3d.ts:1080-1131, 1256`, `tiga/editorRel3d.ts:156-190, 445-452, 517-540`, `tiga/uji3dProfil.ts` (pure), `ui/propsPanel.ts:142-187` | node handles (`Points`, size-attenuation off), profile recompute per move, full rail rebuild on release (`bangun3d.ts:833-855`) | `uji3dProfil.ts` (pure, already ported: `engine/world/rail_profile`), `infoTinggiNode` (h, hand-written flag, gradients ‰), `n.y` save field (raw DEM metres) | `eng_node_height(id, h, has)` + `eng_rails_rebuild()` (profile + rails + terrain chords + signals / points / boards / JPL re-placed; 26 ms on Mojokerto in the wasm build, 344 ms Debug+ASan native). `infoTinggiNode` needs a `eng_node_info(id)` (rail height at the node) — phase 2 |
| **Editor rel 3D** (node drag, chain draw, ghost, B/I/X/T, select) | `tiga/editorRel3d.ts` (555), `dunia3d.ts:1080-1214, 1244-1263, 3690-3760` | node handle points + dashed instanced rings, hover sphere, segment highlight / drag ghost `LineSegments`, chain ghost dashed line, ground raycast, camera lock during drag, `sampelSegmen` from the three rail build | `nodeDiLayar` (14 px), `segsTerpengaruh`, all graph mutations (`src/engine/world.ts`: `insertNode`, `setStraight`, `deleteNodeFull`, `deleteSegment`, `connect`, `moveNode`; `surveyorOps.anchorRel`), status strings | `eng_pick_object` (`node:<id>` 14 px, `segment:<id>:<s>` 12 px), `eng_pick_track`, `eng_pick_ground`, `eng_track_edit(worldJson)` (in-place: graph → rails rebuild + hiasan / garis re-placed; terrain / imagery / city / trees kept, scene origin kept), `eng_highlight` (node / segment kinds: phase 2), `eng_ukur_line` can draw the drag ghost polyline meanwhile |
| **Objek rel / scenery markers** (place, drag along the rail, delete, R / F, selection → `pilih3D`) | `tiga/uji3dObjekRel.ts` (988), `dunia3d.ts:1391-1412` | instanced diamond / cone / cube / foot-ring markers, stalk `LineSegments`, torus selection ring, DOM name labels via projection, ground raycast + `graph.nearestOnTrack` snapping, camera lock | catalog tables, `Entri` bookkeeping, `markerDiLayar`, `dirKeKamera`, label layout, all edits through `src/engine/surveyorOps.ts` (`taruh/geser/balikArah/hapusTrackside`, `taruh/geser/putar/hapusScenery`) | `eng_pick_ground` + host `nearestOnTrack` (pure), `eng_pick_object` (`signal:` / `point:` exist; trackside kinds without a 3D body need an `eng_markers(json)` overlay + `marker:<id>` pick — phase 2), `eng_track_edit` after each edit (the engine rebuilds signals / boards from the save) |
| **Ukur** (measure: free points or Shift = along the rail) | `src/engine/ukur.ts` (pure), `main.ts:1236-1276`, `render/renderer.ts:1003-1073` (2D only — **there is no 3D display in the three path**) | — (never drawn in 3D) | Dijkstra along the graph, azimuth, `formatJarak`, rubber band, Enter / Esc | `eng_ukur_line(json)` draws the polyline + end posts 0.4 m over the ground; labels = `eng_project` + DOM. Shift-snap: `eng_pick_track` → `graph.pointAt` |
| **Surveyor overlays** (node handles, ghost track, brush ring, object name labels) | `editorRel3d.ts`, `dunia3d.ts:1319-1326, 4035-4054` | as above | — | `eng_gizmo`, `eng_highlight`, `eng_ukur_line`; per-node handles need `eng_markers` (phase 2) |
| **Sky time forced to noon in Surveyor** | `dunia3d.ts:2410-2420` | — | — | already `eng_set_sky_time` (`terapkanWaktu`) |

Adapter stubs in `tiga-ayana/duniaAyana.ts` after phase 1: `setAlatGame` (katalog + waktu), `lepasAlat3D` /
`keluarHias` (Tata objek exits), `sentuhObjek` / `sentuhRel` / `segarkanObjek3d` / `tandaiRelKotor` (while 3D is
open) → debounced `eng_track_edit` with the fresh save (250 ms), `infoTinggiNode` still a stub (phase 2).

## 2. Editing ABI

Declared in `engine/api/engine_api.h` ("editing" block), implemented in `engine/api/engine_api_edit.cpp` through
`eng_edit_ctx()` (`engine_api_edit.h`, same bridge pattern as the HUD) and `WorldScene` (`engine/app/world_edit.cpp`).
x / y = css px, wx / wy = world (Mercator m, y south-positive), heights = scene metres. Everything answers "" / 0
before `eng_ready()`; picking also needs a drawn frame.

Picking
- `eng_pick_ground(x, y) → "wx,wy,h"` — heightfield march + bisect against the carved ground (`Compass::groundHit`).
- `eng_ground(wx, wy) → h` — carved ground height at a world point (`tanahTerukir`).
- `eng_pick_object(x, y) → "hiasan:<i>" | "garis:<i>" | "signal:<id>" | "point:<id>" | "node:<id>" | "segment:<id>:<s>" | ""`
  — hiasan: Möller–Trumbore through the model's collision copy (`GpuPrimitive::collisionPos`, kept for scenery models),
  AABB when a primitive has none, then the `uji3dTata.ts objDiLayar` tolerance (16 px, thin / small targets only,
  ties to the camera-nearest); garis: projected polyline within 16 px; signal / point: the existing `pickAt`;
  node: 14 px (`editorRel3d.ts nodeDiLayar`, anchor rail head + 0.6); segment: centreline within 12 px.
- `eng_pick_track(x, y, maxPx) → "segId,s,side"` — nearest rail centreline (LUT chords projected), side +1 = cursor left of the tangent.

Live edits (the save `world` object inside the engine is edited in place; the host mirrors every edit into its own
World so both renderers read the same `world.hiasan` / `world.tanah` / node `y`)
- `eng_hiasan_set(i, wx, wy, naik, rotDeg, skala)` — re-places one model (`WorldScene::hiasanPlace`); walk collider rebuilt lazily.
- `eng_hiasan_add(json) → index`, `eng_hiasan_remove(i)`, `eng_hiasan_info(i) → {model,x,y,naik,rot,skala,resident,size}`,
  `eng_model_size(id) → "x,y,z"` (resident models only; never triggers a fetch).
- `eng_garis_set({"index", ...entry} | {"index","remove":true})` — one entry replaced / appended / removed, garis visuals rebuilt.
- `eng_node_height(id, h, has)` + `eng_rails_rebuild() → ms` — profile, rails, terrain chords (built tiles re-cut under
  the per-frame budget), signals, points, boards, JPL, routes; hiasan re-placed; trains and decor stay.
- `eng_terrain_delta(json)` — whole `world.tanah`; `Terrain::applyBrushDeltas` diffs the cell map and marks only the
  near tiles within 16 m of a changed node (and the far tiles under them) dirty; hiasan / garis re-placed.
- `eng_track_edit(worldJson)` — new save object → `TrackGraph::fromJson`, then `railsRebuild` and the hiasan / garis /
  meja re-placed. Terrain data, imagery, city, clouds, trees and the scene origin are kept (a moved bbox-extreme node
  only shifts the fog reference). 26 ms on Mojokerto (wasm release).

Overlays (drawn after the world in `eng_frame`; gizmo / ukur depth-test-off like the compass)
- `eng_highlight("hiasan:<i>" | "garis:<i>", mode)` — 0 off, 1 blue box (12 bars on the placed bounds), 2 amber (locked).
- `eng_gizmo(kind, wx, wy, h, yaw, scale, axisHover)` — "rotate": ring 0.9..1.06 + needle + knob (`uji3dTata` cincin /
  jarum / knop), "move": ring + 4 arrows; `eng_gizmo_hit(x, y)` → 0 / 1 ring (thick 0.7..1.3 hit band) / 2 knob;
  `eng_gizmo_angle(x, y)` → cursor angle in the ring plane (three convention: `rotation.y = θ` maps +X to (cos θ, −sin θ)).
- `eng_ghost(modelId, wx, wy, rotDeg, skala)` — translucent blue model (`ModelRenderer::draw` material override); requests the model when not resident.
- `eng_ukur_line(json)` — `[{x,y}]` polyline 0.4 m over the ground + end posts; labels via `eng_project`.

Phase 2 additions (2B)
- `eng_pick_node(x, y, maxPx) → "<id>"` — node handle within maxPx whatever covers it (the `editorRel3d.ts nodeDiLayar`
  order: handles win over models); orphan nodes have no handle.
- `eng_node_info(id) → {h, tulis, grad[]}` — raw DEM metres at the rail head (the pin when hand-written), permille to
  each neighbour; the host's `infoTinggiNode` (`tiga/editorRelInti.ts`) reads `h` for the nodes that follow the DEM.
- `eng_veg_mask(json)` — the whole `world.vegMask` (`[{x,y,r,a}]`, last stamp wins, a −1 clear / +1 plant) or `null`;
  `Vegetation::setMask` diffs against the previous list and re-scatters only the cells under the stamps that changed
  (one stroke = one or two cells). The save's `vegMask` is applied in `buildDecor`.
- `eng_highlight("node:<id>" | "segment:<id>[:s]", mode)` — green screen-sized sphere on the handle / amber ribbon
  along the centreline (cached mesh, rebuilt when the id or the profile changes).
- `eng_node_handles(on, tier)` — every node with a segment as a 4.5 px dot at rail head + 0.6: points orange,
  hand-written magenta, chain ends white, plain blue; tier 1 = the important ones only. Minimal handles drawn in
  `world_edit.cpp` (no `eng_markers` dependency).
- `eng_ghost_lines(json)` — `[[{x,y}],...]` thin blue ribbons 0.9 m over the rail head near each point (drag preview
  of the affected segments, chain-draw rubber band).
- Timing (Mojokerto, wasm release, headless smoke `playtest/_cek-ayana-sunting.mjs`): `eng_rails_rebuild` 23–32 ms per
  Alt-drag step (host debounce 80 ms), `eng_track_edit` 25–39 ms on node release, `eng_terrain_delta` 0.2–1.2 ms per
  stroke frame (the tiles re-cut under the per-frame budget afterwards).

Engine-side pieces added: `WorldScene::{rayGround, pickHiasan, pickTrack, pickGaris, hiasanScreenBox, hiasanPlace,
hiasanSet/Add/Remove/Refresh, garisSet, nodeHeight, railsRebuild, terrainDelta, trackEdit, setHighlight, setGizmo,
gizmoHit, gizmoAngle, setGhost, setUkur, drawOverlays}`, `Placed::objIndex` (scenery ↔ `world.hiasan.objek` index),
lazy `walkCollider()`, `Terrain::applyBrushDeltas`, `ModelRenderer::draw(..., materialOverride)`. Web exports:
`engine/api/CMakeLists.txt` `AYANA_EXPORTS`.

Markers (`engine_api_markers.cpp` → `engine/app/markers.*`; drawn after the world overlays, depth-test-off by default)
- `eng_markers(json)` — replaces the whole batch: `[{id, kind: sphere|disc|cube|diamond|cone|polyline, x, y, naik, hm (0 ground /
  1 rail head), h (absolute), color "#rrggbb", alpha, size (m), px (screen radius for point kinds), depth, yaw, label, pts:[{x,y,naik,hm,h}]}]`;
  `"[]"` clears. `eng_markers_pick(x, y, maxPx) → id` (point kinds count their projected radius, lines measure to the projected
  polyline, ties to the camera-nearest), `eng_markers_screen() → [{id, x, y, d, v}]` for the `label` markers (DOM name plates),
  `eng_markers_count()`. The host side shares one batch between tools through `tiga-ayana/markerAyana.ts` (`KumpulanMarker`:
  per-tool layers, ids prefixed `<layer>/`). Used by `garisAyana.ts` (handles / rings / end spheres / rubber band),
  `objekRelAyana.ts` (diamond / cone / cube / stalk / foot ring / selection ring, names) and `ukurAyana.ts` (finished measurements).

## 3. Phase 2 — remaining tools and effort

| Tool | Work | Effort |
|---|---|---|
| Gambar garis (`garisAyana.ts`) | Port `uji3dSpline.ts` on `eng_pick_ground` / `eng_pick_object("garis:")` / `eng_garis_set` / `eng_highlight`; handles + end markers as DOM dots through `eng_project` (or a small `eng_markers(json)` overlay: spheres / rings at world points, pickable as `marker:<id>`); procedural classes already exist in `garis_visual` | 1.5 days (0.5 with `eng_markers`) |
| Kuas tanah (done, `kuasAyana.ts`) | Brush maths already pure; wire `eng_terrain_delta` per stroke (debounced) + brush ring via `eng_gizmo("move")`; `world.vegMask` needs `eng_veg_mask(json)` in `Vegetation` (stamp list → cell re-scatter) | 1 day |
| Node height + Editor rel 3D (done, `tinggiAyana.ts` / `relAyana.ts`) | `eng_node_info(id)` (rail height / hand-written / gradients) for `infoTinggiNode`; node handles = `eng_markers`; drag: `eng_pick_ground` + `world.moveNode` + `eng_track_edit` on release; Alt-drag: `eng_node_height` + `eng_rails_rebuild` per move (26 ms); `node:` / `segment:` highlight kinds | 2 days |
| Objek rel / scenery markers | `eng_markers` (diamond / cone / cube / foot ring per kind) + `marker:<id>` pick; edits through `surveyorOps` + `eng_track_edit` (already) | 1 day |
| Ukur in 3D | New for both renderers: `eng_ukur_line` + labels; `eng_pick_track` for Shift snap | 0.5 day |
| Palette thumbnails | `eng_thumbnail(id, px) → rgba` (offscreen FBO render of a resident model) | 0.5 day |
| Papan nama text | Engine text-on-mesh for `papan-nama` materials (the `teks` / `ketinggian` fields are saved but not drawn) | 0.5 day |
| Bundle decoupling | The Ayana chunk still pulls the `dunia3d` chunk (three) through `tiga/dunia3dKonst.ts` (imports `TataObjek`), `mejaApungAyana.ts` (`mejaApug3d`), `kompas3d.ts`; move those value imports to types / three-free modules | 0.5 day |
