# Web gaps: three.js 3D mode vs the Ayana renderer path

Audit of everything a **player** sees in the reference client's three.js 3D mode (`ppka-wannabe-2`, `?renderer=three`)
that the Ayana path (`src/tiga-ayana/duniaAyana.ts` + `engine/api/engine_api.*` + `engine/app/world_scene.*`) does not
do yet. Editor/surveyor tools are listed but deliberately not ported.

**Status 2026-09-14:** recommended-order items 1–5 are implemented (`src/tiga-ayana/labelAyana.ts`, `pengaturanAyana.ts`,
`duniaAyana.ts`; ABI `eng_set_paused`, `eng_set_layer`, `eng_train_screen` for every train, `eng_camera_json.subject`).
Items 9–12 landed later the same day: quality tiers / dpr cap / tree density (`eng_set_quality`, `eng_set_tree_radius`,
`eng_set_tree_density`), touch input (`sentuhAyana.ts`: MOVE/ROTATE, pinch, walk joystick), compass settings + per-mode
slider + `Pindah sisi` (`eng_compass_settings`, `eng_set_rig_param`/`eng_get_rig_param`, `eng_side_flip`), imagery source /
sky time / theme (`eng_reset_imagery`, `eng_set_sky_time`, `eng_set_theme`), the GLB fallback (`eng_model_begin_glb`: a map
without prebuilt `.emod` loads the plain GLBs from `asetUrl()`), the in-world meja panel (`eng_set_panel`), the rail atlas
(`eng_rail_atlas_rgba`), walk boxes + Space, `refDistance`/`cabView`/`benang`. Rows below are updated to **present** where
that landed; the rest is still the list.

Sources read: `duniaAyana.ts` (592 lines), `src/main.ts` (`// AYANA:` dispatch, `KonteksDunia3D` wiring at 289-398),
`src/tiga/dunia3d.ts`, `dunia3dKonst.ts`, `hud3d.ts`, `uji3dLabel.ts` (the train cards — `keretaVisual3d.ts` only calls it
at :774), `mejaApung3d.ts`, `kompas3d.ts`, `src/ui/laciSurveyor.ts`, `src/ui/hud.ts`, `src/audio/*`, and on the engine side
`engine/api/engine_api.h/.cpp`, `engine/app/world_scene.cpp`, `engine/app/camera_rig.h`, `engine/app/compass.h`,
`examples/ppka/*`. `ref` = file in ppka-wannabe-2, `eng` = file in this repo.

Legend — status: **missing** · **partial** · **wired but off** (the engine/ABI has it, the adapter does not call it) ·
**present** · ➖ deliberately not ported. Effort: S ≤ ½ day, M 1–2 days, L > 2 days.

Two facts shape most rows:

* The engine exposes camera/hover/pick/label-anchor calls but **no generic projection** (`eng_object_screen` only knows
  `signal:`/`point:` ids). Station bubbles, signal plates and tethers need either a new `eng_project(x,y,z)` / list
  call or a JS-side view-projection from `eng_camera_json` (which lacks the projection matrix).
* The adapter has full access to `world`/`ixl`/`session`, so most **card content** (delay, next stop, S40 state,
  langsir buttons) is a pure TS job: the three.js `LabelKA3D` (`ref src/tiga/uji3dLabel.ts`) can largely be reused
  given screen anchors from `eng_train_screen`.

## 1. HUD & labels

| Feature | What the player sees in three.js | Ayana status | Where | Effort | Notes |
|---|---|---|---|---|---|
| Train chip (near trains) | `KA <no>` + short name, delay pill (`Tepat`/`+N mnt`/`SEMBOYAN 45`), signal-ahead pill (dot + distance to next main signal), `⛔ menahan N`, row 2 `→ next stop · brkt HH:MM · km/h/limit`, colour stripe by state, tether line + roof dot, scale 1→0.72 over 250–2850 m, max 8 chips, overlap demotion | present | `labelAyana.ts` (copy of `LabelKA3D` with `eng_train_screen` anchors), adapter `gambarLabel()` | — | `eng_train_screen` returns `{id,no,name,state,speed,x,y,d}`; the rest comes from `session.trains` in TS. Reusing `LabelKA3D` with an anchor provider is the cheapest path |
| Train mark (TANDA, far) | state dot, 8-way arrow, number, name, delay; fades by 9 km | present | ref `uji3dLabel.ts:727-758` | S | with the chip row above |
| Edge markers (TEPI) | off-screen trains pinned to the screen edge with `◀/▶`, distance; layer `tepi` (default off) | present | ref `uji3dLabel.ts:760-781` | S | needs off-screen direction: `eng_train_screen` clips to `d ≤ 2500` only in the adapter; the engine returns every train (check whether x,y are valid when off-screen) |
| Subject card (KARTU) | full card for the followed train: state header, `📌` pin, `✕` release (→ bebas), Laju/Telat tiles, next stop + dwell progress bar, S40 ritual phase text, buttons below | present | ref `uji3dLabel.ts:430-484`, `:783-893` | M | card is DOM; anchor = same as chip |
| Card button `📋 Rincian` | opens the game train sheet (`kembang`) | present | ref `uji3dLabel.ts:322` → `dunia3d.ts:1354`; main.ts `rincianKA` :314 | S | `ctx3.rincianKA` exists in the context the adapter already receives |
| Card/chip button `🚩 Berikan Semboyan 40` | visible when `t.s40Siap`; starts the S40 ritual (main.ts owns the logic) | present | ref `uji3dLabel.ts:326,351,835`; main.ts `beriS40` :363 | S | forward `ctx3.beriS40(id)` |
| Card button `🗑 Hapus KA` (two-tap `⚠ Yakin? −2000`) | removes a train with penalty | present | ref `uji3dLabel.ts:329-338,851-857`; main.ts `hapusKA` :368 | S | forward `ctx3.hapusKA(id)` |
| Langsir pupitre (`▶`, `⏸`, `⇄`, `🤖`, `✋ Ambil alih`) | only for `t.dinasLangsir` | present | ref `uji3dLabel.ts:220-225,341-346`; main.ts `aksiLangsir` :367 | S | forward `ctx3.aksiLangsir(id, aksi)` |
| Card camera buttons `🚂 Naik kabin`, `🎬 Samping`, `🔭 Ekor` | switch mode on that train | present | ref `uji3dLabel.ts:456-458` | S | `eng_follow_train(id)` + `eng_camera_mode` — both exist, neither is called by the adapter |
| Chip click = select + follow | click sets the game selection AND switches `bebas → samping` on that train; hover forces a chip | present | ref `dunia3d.ts:1341-1347`; adapter :379 calls only `pilihKA` | S | add `eng_follow_train(id)` (+ optional mode change) — **wired but off** in the ABI |
| Labels hidden in kabin, pill-mini in jalan (fade 60–400 m) | | present | ref `keretaVisual3d.ts:774`, `uji3dLabel.ts:96-103` | S | adapter draws chips in every mode |
| Signal name plates | `.sig-nama` on masts: aspect dot + name + action glyph (`▸` set / `✕` cancel / `⇄` set points first), elbow line to the mast, clustering `TLS keluar B 1 2 3 +2`, ≤ 4500 m, max 16; **clickable = pull route**; layer `pelat` (default off) | missing | ref `hud3d.ts:254-471`, click `dunia3d.ts:850-855` | M | needs a signal list with screen anchors (`SignalVisuals::screenPositions` exists — expose as `eng_signal_screen`); plate DOM is reusable |
| Hover ring | white screen-sized ring at the lamp | present | eng `WorldScene::drawHoverRing`; adapter `hoverDi` | — | |
| Signal tooltip | `.sig-tip` card: name, `HIJAU/KUNING/MERAH · role`, ` · lewat <sig>` for shared masks, action line (`▸ klik = tarik rute ke <exit>` / `✕ klik = batalkan rute` / red `.tolak` reason) | partial | ref `hud3d.ts:202-234`; adapter `hoverDi` :455-478 (plain text, aspect from `ixl.aspectOf`, no colour/role/`.tolak`) | S | cosmetics only; wording already matches the trace |
| Route preview on hover | blue ribbon on the traced path, red when dead-ends, refreshed 250–500 ms | present | eng `route_visual` preview; adapter `eng_set_preview` | — | |
| Point tooltip | none in three (hover ring only for signals) | present (extra) | adapter :456-458 | — | engine path shows more than the reference |
| Station bubbles | `.st-papan` code + label + `N KA`, 700 m–40 km, fade/shrink, tether; **click = fly-to** (`terbangKeScenery`, 780 m / 430 m, 1.1 s smoothstep, toast `Menuju CODE — label`); layer `papan` (default on); hidden in kabin | missing | ref `hud3d.ts:480-573`, `dunia3d.ts:4318-4343` | M | needs projection of scenery positions (new ABI or `eng_project`); fly-to = `eng_look_at(wx, wy)` (**wired but off**, animated by the compass jump) |
| Route/occupancy ribbons | green locked route, red occupied section; layer `pita` (default **off**), hidden in kabin | present (toggle `eng_set_layer("pita")`, default off) | eng `route_visual.cpp`; three default off | S | add the toggle (see §2) |
| Langsir plan ribbons | yellow dashed ribbons per leg of `session.putarLok.overlay()`, active leg bright | missing | ref `hud3d.ts:615-633` | M | needs a new state field + ribbon style in `route_visual` |
| Floating meja layan (`MejaApung3D`) | `🗺 Meja M` button / pil (code + aspect lamps + `N tugas`) / window (station select, S/M/L, dock, opacity, zoom; **click signal/wesel = route/toggle**); `M` key; persisted `ppka-meja-apung`; hidden ≤ 560 px short side | missing (stub; `laciBody` filled with a different body) | ref `mejaApung3d.ts`, `dunia3d.ts:1269-1273,1452-1462` | M | pure DOM/2D canvas; needs `ctx3.stasiunPemain/tugasJml/klikSinyal/klikWesel` (all in the context) and the camera focus for "nearest station" (`eng_camera_json` has it) |
| In-world meja layan board (`mejaLayan3d.ts`) + `⤴ Angkat meja` | schematic drawn on the station GLB desk, clickable within 8 m | present (board; `⤴ Angkat meja` / click-on-board not ported) | eng `meja_board`, `eng_set_panel` | — | |
| Status toasts | `3D siap — klik label KA…`, `Belum ada KA di lintas`, mode hints on `setModeKam`, `Subjek kamera: KA n`, wheel readout in locked modes | present (except the wheel readout) | ref `dunia3d.ts:1229,1620,1848,1881-1886,2260`; adapter :196,:493 | S | |
| Loading progress + per-file drawer | stage texts + `Mengunduh model sarana n/N — X MB` | present | adapter `muat()` uses `progres`/`berkas` | — | texts differ; fine |
| Compass readout `#kompas-koord` | `fokus X Z · arah NNN°` bottom-left in bebas | missing | ref `kompas3d.ts:239-243,528-529` | S | `eng_camera_json.azimuth` + `look` already give the data |
| Engine debug HUD `Ayana · fps · draw · mode · ubin` | (three shows the load in the drawer `#d3-fps`, not on canvas) | present (extra) | adapter :125-128,:356-361 | S | move it into the drawer / hide by default |

## 2. Settings & toggles

The three.js KAMERA drawer (`ref dunia3d.ts:3300-3407`, `laciBody('kamera')`) is replaced wholesale by the adapter's
`isiLaciKamera()` (`duniaAyana.ts:501-528`): six mode buttons, a "use three.js" button and a help line. Everything
else in the drawer is gone.

| Feature | What the player sees in three.js | Ayana status | Where | Effort | Notes |
|---|---|---|---|---|---|
| Layer `pelat` Pelat sinyal (default off) | signal name plates | present | ref `dunia3dKonst.ts:1394-1399`, drawer `:3386-3394`, key `pk-3d-tampil` | S (+ plates) | |
| Layer `papan` Papan stasiun (default on) | station bubbles | present | same | S (+ bubbles) | |
| Layer `label` Label KA (default on) | train chips/cards | present | same; adapter has no toggle | S | |
| Layer `pita` Pita rute (default off) | route/occupancy ribbons | present (`eng_set_layer`) | eng `route_visual` has no visibility flag | S | add `eng_set_layer("pita", on)` |
| Layer `wesel` Panah wesel (default off; hidden arrows are also not clickable) | switch arrows + padlock | present (`eng_set_layer`, pick skips hidden arrows) | eng `point_visual` always drawn | S | same `eng_set_layer` |
| Layer `tepi` Penanda tepi (default off) | edge markers | present | ref `uji3dLabel.ts:558` | S | |
| Layer `benang` Benang selalu | iconic yellow rail line always vs only when far (700/500 m hysteresis) | present (`eng_set_layer("benang")` → `WorldScene::benangAlways`; drawer row) | eng `rail_builder`; adapter `hudAyana.ts` LAPIS_MESIN | — | |
| Quality tier `#d3-mutu` auto/Penuh/Tinggi/Sedang/Hemat/Minimum | dpr cap 3/1.6/1.35/1.1/1, tree radius 3200…900 m, clouds on/off, detail imagery rings z16+z17/z16/z14, touch fog/tile radii; auto ladder (24 ms down / 13 ms up); touch starts and caps at Sedang; key `pk-3d-mutu` | present (dpr cap via `eng_resize`, tree radius + clouds via `eng_set_quality`/`eng_set_tree_radius` incl. the touch `JARAK_SENTUH` radii, auto ladder 45/240 frames, `mutuAwal`/`plafonMutu`) | adapter `duniaAyana.ts` `timbangMutu`/`setMutu`; the detail imagery rings and touch fog/tile radii stay the engine's own (`terrain.h` radii) | — | |
| Kerapatan pohon `#d3-pohon` 0…16× (default 2, key `pk-3d-pohon`) | tree density | present | `eng_set_tree_density(k)` → `Vegetation::setDensity` (every cell re-scattered) | — | |
| Citra tanah `#d3-tanah` satelit/peta/petaPolos(default)/voyager/osm/topo/polos (key `pk-3d-tanah`) | ground imagery source; default in three is the plain CARTO map, not satellite | present (`ubinAyana.ts` picks the `SUMBER_TANAH` URL family + theme; `eng_reset_imagery(flat)` drops the resident tiles so they are re-fetched; `polos` = every tile request answered by `eng_terrain_tile_fail`, tiles painted in `TEMA.hampar`) | adapter `gantiSumberTanah`; eng `Terrain::dropImagery/setGroundColors` | — | the vegetation mask follows whatever imagery is resident (as three: trees only from satellite-like pixels; CARTO tiles rarely pass the ExG test) |
| Waktu langit `#d3-waktu` auto/jam/siang (key `pk-3d-waktu-langit`) | sun/sky follow the duty clock or fixed noon | present (`eng_set_sky_time(sec)`; `siang` = 09:48 solar ≈ SIANG_KERJA elevation 58°, auto = jam unless `modeSurveyor`) | adapter `terapkanWaktu` (also on `setAlatGame`) | — | |
| Sky mode Gradien/Atmosfer `#d3-langit` | button is **hidden** in three; forced to gradien | ➖ | ref `dunia3d.ts:3333-3339,1044-1047`; PARITY ➖ | — | not a player-visible gap |
| Theme dark/light (`#btn-theme`, `body.light`, `TEMA`) | dome colours (overridden by the clock), hemisphere/sun intensity, backdrop plane, fog colour fallback, iconic line colour, CARTO tiles swap; label CSS via `body.light` | present (`eng_set_theme(dark)`: fog / dome ground blended 20 % towards `TEMA.kabut`, backdrop plane = `TEMA.hampar`; theme-aware CARTO tiles re-fetched) | adapter `setTema`; eng `sun.h` (`Sky::darkTheme`), `Terrain::setGroundColors` | — | iconic line colour stays the engine's |
| Compass: Rotation/Panning `#d3-kompas-rotasi`, speed ½–2× `#d3-kompas-laju`, show reticle `#d3-kompas-tampil` (key `pk-3d-kompas`, `KUNCI_KOMPAS`) | compass behaviour in bebas | present | `eng_compass_settings(rotation, speed, show)`; drawer rows in `pengaturanAyana.ts` (same markup), persisted via `kompas3d.ts` `bacaSetelan`/`KUNCI_KOMPAS` | — | |
| Per-mode slider `.d3-atur` (jalan Sudut 45–95°, kabin 40–95°, samping Jarak 8–160, atas Tinggi 40–2000, ekor Jarak 10–200) + wheel toast | | present (`eng_set_rig_param`/`eng_get_rig_param`; wheel/pinch → toast `Jarak: 31 m` + slider refresh; kabin wheel stays eye forward/back as before, no toast) | adapter `aturMode`/`bacaanParam` | — | |
| `↔ Pindah sisi` (samping) | mirror the side camera | present (`eng_side_flip`) | three also mirrors the orbit azimuth (`orbitAz`), private in `CameraRig` — the side flips, the orbit angle is kept | — | |
| `🔭 Teropong 4×` lock chip | toggle telescope (vs hold Z) | present | ref `dunia3d.ts:3312,3489-3491,1812` | S | `eng_telescope(1)` held = lock |
| Load meter `#d3-fps` + `Rincian beban` | fps / draw calls / tiles in the drawer | present (drawer + on-canvas HUD) | adapter :356-361 | S | |
| Keyboard shortcut list `<details>` | | present | ref `dunia3d.ts:3395-3406` | S | |
| Mobile MOVE / ROTATE buttons (`#kompas-move/-rotate`, touch only, bebas) | tap = sticky, hold ≥ 260 ms = temporary; MOVE: tap = fly, drag = glide, pinch = zoom; ROTATE: one finger orbit, two fingers dolly/pan | present (`sentuhAyana.ts`: same DOM/CSS; MOVE tap → `eng_compass_click`, drag → `eng_compass_glide`; pinch → `eng_zoom` in every mode; ROTATE one finger = the adapter's orbit drag; two-finger pan not ported) | | — | |
| Walk-mode touch joystick (left half thumbstick, `JARI_TUAS` 58 px; right half look) | | present (`sentuhAyana.ts`: stick → synthetic `eng_key` W/A/S/D + Shift beyond the ring, right half → `eng_orbit`) | | — | |
| Sound mixer (`#btn-sound`, `ppka-mix-*`) | global, not 3D | present | `src/ui/hud.ts:215-317` | — | untouched by the renderer |
| Spatial audio (`penempat`) | engine/horn PannerNode from the camera | present | adapter `penempat()` :531-551 | — | `sumber()` uses y = 0 instead of rail height; negligible |
| Fullscreen auto (touch) | | present | `src/ui/layarPenuh.ts` — renderer-agnostic | — | |

## 3. Camera

| Feature | What the player sees in three.js | Ayana status | Where | Effort | Notes |
|---|---|---|---|---|---|
| Modes bebas/jalan/kabin/samping/atas/ekor | | present | adapter `setModeKam` :488; eng `camera_rig` | — | |
| Key `C` / `Shift+C` cycle modes, `Esc` → bebas, `1–6` (native only) | | present | ref `dunia3d.ts:1274-1286` | S | adapter forwards keys to `eng_key` only for walk; add the shortcuts in TS |
| `,` / `.` subject cycling + toast | | present | ref `dunia3d.ts:1284-1285,1614`; eng `eng_cycle_subject` exists; adapter **never calls it** although its help line advertises it (`duniaAyana.ts:526`) | S | |
| Follow the selected train | label click → subject → `samping`; card `✕` releases | present | eng `eng_follow_train`; adapter never calls it, engine follows the nearest train | S | |
| Telescope `Z` hold | fov/4 ≥ 8° | present | adapter :416-417 | — | lock chip: §2 |
| `#kabin-keluar` `↩ Keluar kabin` button (bottom-right, kabin only) | | present | ref `dunia3d.ts:874-881,1838-1842`, `style.css:3607-3618` | S | |
| Cab extras: arrows/Shift+arrows move the eye in the cab, `Home` reset, `H` hold = horn (audio) | | missing | ref `dunia3d.ts:1287-1302,1655` | S–M | horn is `klakson` in TS (no engine work); eye offsets need `eng_cab_offset` |
| Cab shake (`goyangKabin`) | | missing | ref `uji3dGoyang.ts`; PARITY "not ported" | M | |
| Walk mode: collision (`RabaTiga`), jump (Space), head bob | | present | eng `camera_rig` + `engine/world/walk_collision` (scenery triangle mesh: walls/floors/ceilings, platform kerb 1.1 m; vehicles AABB), `engine_api` passes `WalkInput.mesh` + `boxes` and Space | S | |
| Orbit limits | `maxPolarAngle = π/2 − 0.03`, damping 0.08, no min/max distance, no target clamp | present (engine's own limits) | ref `dunia3d.ts:941-945`; eng `orbit_camera.h` | — | verify pitch floor matches (three allows ~1.7° above horizon) |
| Initial view | corridor overview: target = bbox centre, `d = clamp(0.75 × max(bbox), 600, 22000)`, camera `(0.35d, 0.62d, 0.72d)`, mode bebas | partial | ref `dunia3d.ts:2252-2258`, `IKHTISAR_MAKS` `dunia3dKonst.ts:270`; eng `buildStaticWorld()` starts 160 m from the station at 18°/35° | S | `eng_set_view` + `eng_look_at` exist; choose one behaviour |
| Right-click compass: tap = fly (0.55 s, ≤ 4× distance), hold = glide, Ctrl+arrows slide, arrows yaw/pitch | | present | adapter pointer button 1 + `eng_key` Control/Arrows; eng `compass.cpp` | — | |
| Fly-to station (bubble click) | | wired but off | `eng_look_at` (see §1) | S | |
| Leaving jalan pushes the orbit target 60 m ahead | | present | eng `eng_camera_mode` | — | |
| Mode refused without trains → toast | | present | adapter :493 | — | |
| Fog per mode (linear corridor fog) | | partial | PARITY: exponential fog approximating the linear ranges | — | |

## 4. World visuals missing in the web path

`WorldScene::draw` (`eng engine/app/world_scene.cpp:156-179`) draws terrain, trees, rails, scenery, signals, points,
boards, JPL, city, clouds, garis and route ribbons, and `eng_frame` draws trains and the compass — so everything the
native `examples/ppka` shows is also drawn on the web. The remaining gaps are engine-wide (see `docs/PARITY.md`) or
web-specific:

| Feature | What the player sees in three.js | Ayana status | Where | Effort | Notes |
|---|---|---|---|---|---|
| Clouds / city / boards / JPL / garis / trees | | present | `world_scene.cpp:163-176`; adapter serves `city` + `model` requests | — | roads not drawn in either |
| Ground imagery default | three defaults to the **plain CARTO map** (`petaPolos`), satellite is opt-in | different | ref `dunia3dKonst.ts:904` | — | see §2 Citra tanah |
| Signal plate textures (bolts, number text, "3" strips), lit `angka` overlay when the route diverges | | present | eng `signal_visual.cpp`; the lit overlay needs `routes[].junctions` in the state the adapter builds (`bridge/sim-state.ts` copy) | — | |
| Far LOD signal dot `v.titik` | | present | PARITY "LOD sphere" | — | |
| Wesel `skalaWesel = max(1, d/240)` distance scaling, `tirai` curtain within `KABUT_JARAK` | | engine done, adapter not wired | eng `point_visual`; the adapter must set `WorldScene::refDistance` (= `CameraRig::acuan(orbit distance)`) and `cabView` every frame — one small ABI call (`engine_api_polish.cpp` candidate) | S | |
| Semaphore arm `clunk` sound | | missing | ref `dunia3d.ts:3982-3990` | S | adapter can play `suara.clunk()` on aspect change from the state it already builds |
| Rail iconic yellow line when far | | present (`refDistance` from `CameraRig::acuan` per frame in `eng_frame`; `benangAlways` = the "Benang selalu" toggle) | | — | |
| Reference corridor atlas `tekstur.rel1067` | three swaps the picture into the rail materials | present (`eng_rail_atlas_rgba`; adapter `muatAtlasRel` decodes `pilot-rel-1067.jpg` with `createImageBitmap({imageOrientation:'flipY'})`) | | — | |
| Shunting-plan ribbons (`perbaruiPitaLangsir`) | | present | state `langsir[]` (`bridge/sim-state.ts`, copy to `tiga-ayana/simState.ts`) | — | |
| In-world meja board (`mejaLayan3d.ts`) | station GLB `mejalayan` quad shows the live panel | present (`eng_set_panel(json)`: the adapter serialises `PanelLayout.build(world)` with `sim-state.ts panelLayoutJson` — the same JSON as the bridge `panel` command, parsed by `sim_state.cpp parsePanelLayout`) | eng `MejaBoard` | — | |
| Cab interior meshes only in kabin, single-sided skin + translucent glass | | unverified | ref `keretaVisual3d.ts:380-395` | S | check `train_visual` when in kabin (near plane 0.15 m) |
| Train cull tiers by distance/fog (`cullKA.ts`) | | partial | frustum cull only | S | |
| Lights dimmer by day, tunnel switch | | partial | PARITY | S | |
| Shadows, post-processing, weather, stars, night building lights, people, road vehicles, water | none in three either | — | | — | not gaps |
| Profile step 3 (parallel roadbed pairing), `vegMask` brush | | missing | PARITY | M | engine-wide |
| Models without a prebuilt `.emod` (`web/build-models.sh` not run for the map's fleet) | three loads the GLB from `asetUrl()` | present (`eng_model_begin_glb`: the plain GLB from `asetUrl('model3d/<berkas>')` parsed by the engine's `loadGlb`; textures still from the KTX2 twin, white placeholder without one; Draco GLBs in `ktx2/` are not parsable) | adapter `layaniModel` | — | `gombong-wns` opens without `public/ayana/models` (smoke `playtest/_cek-ayana-setelan.mjs`) |
| Paused sim | JPL arms/cloud drift keep animating in the engine while the game is paused | present (`eng_set_paused`) | adapter always passes real `dt`; `session.paused` not forwarded | S | pass `dt = 0` when paused |

## 5. Editor / surveyor tools — ➖ deliberately not ported

Stay in the web client's three.js path (`ref src/ui/laciSurveyor.ts:43-45`: OBJEK/TANAH drawers are `butuh3D`). The adapter
stubs warn once (`duniaAyana.ts:74-79`).

| Tool | ref | Adapter method | Status |
|---|---|---|---|
| Tata objek (hiasan placement: palette, ghost, rotate gizmo, snap-to-rail, lock, delete) | `uji3dTata.ts`, `dunia3d.ts:3410-3439` | `setAlatGame`, `lepasAlat3D`, `keluarHias`, `sentuhObjek`, `segarkanObjek3d` | ➖ |
| Gambar garis (spline fences/walls/platforms/LAA; Enter/Backspace/I/X/Q/A/L, dblclick) | `uji3dSpline.ts`, `dunia3d.ts:3415-3448` | same | ➖ |
| Kuas tanah (Naik/Turun/Rata/Halus, 8–200 m) → `world.tanah` | `dunia3d.ts:3422-3436,3816,3929` | `gantiSumberTanah`, `sentuhRel` | ➖ |
| Kuas pohon (hapus/tanam → `world.vegMask`) | `dunia3d.ts:3905` | — | ➖ |
| Editor rel 3D (node drag, Alt height, chain draw, B/I/X/T) | `editorRel3d.ts` | `tandaiRelKotor` (present: rebuild on next open), `infoTinggiNode`, `sentuhRel` | ➖ |
| Objek rel / scenery markers (place, drag, delete; selection → `pilih3D`) | `uji3dObjekRel.ts` | `pilih3D` not forwarded | ➖ |
| Surveyor overlays (node handles, ghost track, brush ring, object name labels) | `editorRel3d.ts`, `dunia3d.ts:1319-1326,4035-4054` | — | ➖ |
| Sky time forced to noon in Surveyor | `dunia3d.ts:2410-2420` | — | ➖ |

Note: edits made in 2D while the Ayana view is closed are picked up only through `tandaiRelKotor()` (full reload on
the next `tampil(true)`); `sentuhObjek()` / `segarkanObjek3d()` are stubs, so a signal/board property edited in the 2D
props panel while 3D is open is not redrawn until the view is reopened (S: call `eng_load_world` again, or add
`eng_reload_objects`).

## 6. Events / callbacks the adapter does not forward

`KonteksDunia3D` (`ref src/tiga/dunia3dKonst.ts:1286-1351`) as wired in `src/main.ts:289-398`.

| Callback | Trigger in three.js | Ayana | Effort |
|---|---|---|---|
| `pilihKA(id)` | chip/mark/edge click | present (with follow + `samping`) | — |
| `rincianKA(id)` | `📋 Rincian` button | present | — |
| `beriS40(id)` | S40 button on card/chip | present | — |
| `hapusKA(id)` | `🗑 Hapus KA` two-tap | present | — |
| `aksiLangsir(id, aksi)` | langsir pupitre | present | — |
| `klikSinyal(id)` | signal head, name plate, meja click | present (head only) | — |
| `klikWesel(id)` | arrow click, meja click | present (arrow only) | — |
| `klik()` | UI click sound | present | — |
| `status(t)` | many toasts | present (mode changes, subject, telescope) | — |
| `progres`, `berkas` | loading | present | — |
| `konteksHilang()` | `webglcontextlost` on the canvas → main.ts closes 3D with a hint | present (loop stopped; `restored` shuts the engine down, the world reloads on the next open) | — |
| `webglcontextrestored` | not handled in three either | — | — |
| `visibilitychange` | not handled in three (rAF throttles; dt clamped 0.1 s) | same (dt clamped 0.1 s) | — |
| resize / dpr | ResizeObserver on `wadah`; dpr re-read on tier change | present (`ResizeObserver` → `eng_resize` with live dpr) | — |
| `blur` clears held keys | walk keys + compass keys cleared on window blur | present (also on `visibilitychange`) | — |
| Keyboard focus | three ignores keys while typing in inputs? (main.ts `tombolJalanDisita` gate only) | present (input/select/textarea/contenteditable ignored) | — |
| `stasiunPemain`, `tugasJml` | floating meja | missing (with the meja) | — |
| `alatChip`, `pilih3D`, `laciAktif`, `sceneryKind`, `modeSurveyor` | editor | ➖ | — |
| `laciBody('kamera')` | full settings drawer | partial (`pengaturanAyana.ts`: camera, telescope, load, Tampilan, shortcuts, renderer — no kompas/sky/ground/tree/quality rows) | see §2 |
| `laciBody('objek'/'tanah')` | editor drawers | ➖ (left empty) | — |
| `gelap()` / `setTema` | theme | partial (HUD/meja follow; sky is clock-driven) | S |
| `koridorId()` | city bake key | present | — |

## Recommended order (player-visible impact ÷ effort)

1. ✅ **Forward the four card callbacks + follow** (S): `eng_follow_train` on chip click (with the `samping` switch), `,`/`.` → `eng_cycle_subject`, buttons `Rincian` / `Semboyan 40` / `Hapus KA` / langsir on the chip. Unblocks playing a whole shift in Ayana without leaving to 2D.
2. ✅ **Real train chips/cards** (M): reuse `LabelKA3D` (`uji3dLabel.ts`) with `eng_train_screen` anchors — delay, next stop, signal-ahead pill, state colours, tether, 8-chip cap, hide in kabin, mark/edge tiers, subject card with pin/release.
3. ✅ **`webglcontextlost` → `konteksHilang`** and clear keys on `blur`, ignore keydown from inputs (S). Robustness on phones, the audience the web path exists for.
4. ✅ **`#kabin-keluar` button, `Esc`/`C` shortcuts, telescope lock chip, status toasts** (S).
5. ✅ **Layer toggles + `eng_set_layer`** (S): pita/wesel off by default as in three, label/papan/tepi in the drawer, persisted under `pk-3d-tampil`.
6. **Station bubbles + fly-to** (M): needs one projection ABI (`eng_project` or `eng_scenery_screen`); fly-to is `eng_look_at`.
7. **Floating meja layan** (M): port `MejaApung3D` as-is (DOM), `M` key, pil badge — the core dispatcher tool in 3D.
8. **Signal name plates** (M): `eng_signal_screen` + the `hud3d.ts` plate DOM (clustering, click = route).
9. ✅ **Quality tiers + dpr cap + tree density** (M): dpr cap and touch defaults first (S, adapter-side via `eng_resize`), then `eng_set_quality`/`eng_set_tree_density`.
10. ✅ **Mobile touch**: MOVE/ROTATE buttons, pinch zoom, walk joystick (M).
11. ✅ **Compass settings ABI + drawer, Pindah sisi, per-mode slider** (S each, engine fields exist).
12. ✅ **Ground imagery source select + sky time select + theme tint** (S–M, mostly adapter-side).
13. ✅ **Langsir ribbons, signal plate textures/angka, iconic far line, wesel distance scale** — engine + adapter (`refDistance`/`cabView` per frame, `langsir`/`junctions` from the `sim-state.ts` copy, rail atlas); semaphore clunk still missing (adapter-side audio).
14. ✅ In-world meja board (`eng_set_panel` with `panelLayoutJson` from `sim-state.ts`) and walk collision (walk boxes + Space in `eng_frame`/`eng_key`); cab shake done earlier.

Items 9–12 ✅ (see §2). Still open from the tables: langsir/two-finger pan on ROTATE, semaphore clunk, `#kompas-koord` readout,
cab arrow/Home/H extras, train cull tiers.
