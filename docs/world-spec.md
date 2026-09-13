# PPKA Simulator — 3D World Specification for the C++ engine

Source: `/Users/henryaugusta/Developer/ppka-wannabe-2` (TypeScript + three.js, code in Indonesian).
All `file:line` references are relative to that repo root, working tree as of 2026-09-13.
Numbers below are copied from code, not paraphrased. Where the existing `docs/arsitektur-3d.md`
is stale relative to the code, the code wins and the discrepancy is noted.

Section order: 1 map choice · 2 coordinates · 3 track schema + rail geometry · 8 sim↔render
interface (moved up because it is load-bearing) · 4 terrain · 5 scenery · 6 signals/points ·
7 trains · 9 camera/sky · 10 asset catalog.

---

## 1. Map choice — "one station playable"

Only files with top-level `{ "world", "session" }` are playable saves (14 of 19). The five
others (`cikampek*.json`, `cilame.json`, `karawang.json`, `sasaksaat.json`) are raw corridor
imports (`meta, order, mainline_latlon, …, timetable`) and cannot be loaded by `World.fromJSON`.

Statistics computed with the real engine (`World.fromJSON` + `graph.length`):

| file | stations | nodes | segs | track km | wesel | signals (type/lamps) | trains | consists | bridges/tunnels | station GLB (hiasan) | extras |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **mojokerto.json** | 1 (MOJOKERTO) | 358 | 364 | 34.7 | 16 | 18: 14 ixl/2-lamp, 2 ixl/3-lamp, 2 muka | 64 | eksekutif 34, commuter 20, barang 6, ketel 4 | 12 / 0 | `sttd-stasiun-mojokerto` (pilot) | 11 JPL, startClock 21600 |
| **pemalang.json** | 3 (SD/PML/PTA) | 511 | 520 | 48.3 | 25 | 38: 26/2, 6/3, 6 muka | 112 | eksekutif 80, barang 30, lokal 2 | 15 / 0 | `nry-stasiun-pemalang` (deployable) | 24 JPL, 2 `tinggi` pins |
| **bks.json** | 3 (BKS/BKST/TB) | 511 | 523 | 34.6 | 31 | 52: 32 ixl/3, 20 auto | 503 | krl-8 321, eksekutif 150, barang 30, lokal 2 | 0 / 0 | 3× `nry-stasiun-*` (deployable) | 12 JPL, `tanah` brush, 27 pins, `public/kota/bekasi.json` |
| maswati-cilame.json | 3 | 153 | 155 | 18.5 | 7 | 24 | 32 | eksekutif 28, lokal 3, commuter 1 | 27 / 7 | none | mountain, `public/kota/` present |
| cepu.json | 3 | 568 | 583 | 41.4 | 40 | 48 | 64 | eksekutif 38, barang 22, commuter 4 | 19 / 0 | `sttd-stasiun-cepu` (pilot) | 12 JPL |
| bekasi.json | 3 | 331 | 333 | 22.9 | 8 | 30 (no `lampu` field, old save, no `skema`) | 342 | krl-8 160, eksekutif 150 | 6 / 0 | none | superseded by bks.json |

Ranked shortlist:

1. **`src/data/mojokerto.json`** — the only single-station save. Smallest playable emplacement
   that satisfies all four criteria: 16 points, 18 signals (entry `MR M*`, exit `MR K1T…`, 2
   distant signals), 64-train GAPEKA from 06:00 (`startClock: 21600`), 12 bridge segments, 11 JPL,
   station building placed via `world.hiasan.objek[0] = {model:"sttd-stasiun-mojokerto",
   x:12516168.37, y:834198.35, naik:0, rot:-1.7, skala:1}`. Caveat: the GLB
   (`pilot-sttd-stasiun-mojokerto.glb`, 304.71 × 19.86 × 62.63 m, 111 949 tris, 10.9 MB) is a
   `pilot` asset (use-only licence, not redistributable) — present locally, fine for a
   from-scratch engine's dev scene. No `public/kota/mojokerto.json` → no OSM buildings/roads.
2. **`src/data/pemalang.json`** — 3 stations, but Pemalang's building is our own deployable
   `nry-stasiun-pemalang-v6.glb` (11 114 tris, 1.8 MB Draco; ktx2 variant exists). Good second
   scene; trains are loco-hauled only.
3. **`src/data/bks.json`** — richest (3 own station GLBs, KRL JR205 low-poly consists, terrain
   brush data, 27 hand heights, baked OSM city). Too big for a first scene (503 trains).
4. `maswati-cilame.json` — smallest graph, but no station model and heavy bridge/tunnel content.
5. `cepu.json` — 40 points, pilot station only.

GLBs referenced by Mojokerto (see §10 for resolution rules): station
`pilot-sttd-stasiun-mojokerto.glb`; locomotives `pk-cc206.glb`/`pk-cc203.glb`/`pk-cc201.glb` +
`pk-bogie-cc206.glb`/`pk-bogie-ge-u18c.glb` + `nry-kopling-k3v4.glb`; passenger cars from the
armada lottery (`nry-*-badan.glb`, §7.2) + `nry-bogie-k5t-lp-v2.glb`; freight is drawn as
boxes (`gd`/`gk` in `SLOT_BALOK`). None of these is in git — all 3D assets live on Cloudflare R2
(bucket `168railways`, prefix `pk/`; `.gitignore:30-50`, `src/asetUrl.ts`).

---

## 2. Coordinate systems

### 2.1 Engine 2D world (`src/engine/geo.ts`)
* Web Mercator EPSG:3857 metres, **Y flipped** so y grows south (canvas convention). `geo.ts:11-18`:
  `R_BUMI = 6378137`; `x = R·lon_rad`; `y = −R·ln(tan(π/4 + lat_rad/2))`.
  Inverse `geo.ts:20-24`: `lon = x/R·180/π`; `lat = (2·atan(exp(−y/R)) − π/2)·180/π`.
* **No `cos(lat)` scale correction anywhere** — track metres are raw Mercator metres; ~1 %
  distortion at Java's −7° is accepted (`geo.ts:7-8`). `World.geo = true` in every save
  (`world.ts:83`).
* Slippy tiles: `tileSizeMeter(z) = 2πR/2^z`; `worldToTile: floor((p/LINGKAR + 0.5)·2^z)`
  (`geo.ts:27-44`). z10 ≈ 39 136 m, z13 ≈ 4 892 m, z14 ≈ 2 446 m, z16 ≈ 611 m, z17 ≈ 306 m.
* IDs: nodes `n<k>`, segments `s<k>`, trackside `t<k>`, scenery `sc<k>` (`track.ts:4-12`).

### 2.2 Mapping to three.js (`src/tiga/dunia3d.ts`, `bangun3d.ts`)
* Origin = centre of the bounding box of all **track nodes** (`dunia3d.ts:2181-2183, 2246-2254`):
  `ox = (x0+x1)/2`, `oz = (y0+y1)/2` (fallback ±400 if no nodes). Recomputed on every world load.
* **`three.x = world.x − ox`, `three.z = world.y − oz`, `three.y = height` (up).**
  No sign flip on z (`bangun3d.ts:130`, `dunia3d.ts:360, 815, 950`). Because Mercator y was
  already negated, north = −z, east = +x, south = +z.
* Yaw convention: `rotation.y = θ` maps local +X → `(cos θ, −sin θ)` in (x, z). Track-tangent
  yaw = `atan2(−tan.y, tan.x)` (`bangun3d.ts:599`, `uji3dGoyang.ts:191-193`). 2D scenery `rot`
  (radians, y-down) → three yaw = `−rot` (`bangun3d.ts:642`).
* Height: `three.y = (h_raw − demBase) · demSkala`, `demSkala = 1` (never changed,
  `medan3d.ts:62, 383-386`), `demBase` = raw Terrarium DEM at the bbox centre
  (`medan3d.ts:232-234`). **Vertical exaggeration 1.0.** Rail-head height comes from the 1-D
  profile (§3.4), not from DEM sampling per point.
* Hand-written node height `TrackNode.y` (file key `tinggi`) = metres above sea level, raw DEM
  datum (`track.ts:24-51, 565-567`).

---

## 3. Track data model and rail geometry

### 3.1 Save-file schema (`World.toJSON`, `world.ts:796-814`; `SKEMA_DUNIA = 1`, `world.ts:47`)
```jsonc
{ "world": {
    "skema": 1,
    "graph": {
      "nodes":    [{ "id":"n1", "x":12509827.69, "y":837557.05, "tinggi"?: 12.5,
                     "junction": null | { "setting": 0|1, "spring"?: 0|1 } }],
      "segments": [{ "id":"s3", "a":"n1", "b":"n2", "straight": false,
                     "jenisRel"?: "jembatan"|"terowongan", "tanpaLaa"?: true,
                     "jenisJembatan"?: "dek"|"rangka"|"viaduk" }] },
    "trackside": [ TracksideObj … ],          // §6.1
    "scenery":   [ SceneryObj … ],            // §5.1
    "hiasan"?:   { "objek": [ObjTaruh…], "garis": [Garis…] },   // §5.4
    "tanah"?:    { "kisi": 8, "delta": { "gx,gz": metres } },   // §4.1
    "vegMask"?:  [{ "x","y","r","a": 1|-1 }],
    "counters": {…}, "sceneryCounters": {…}, "geo": true, "panelDelta"?: {…} },
  "session": SessionDef }                       // §8.2
```
`track.ts:561-609` — nodes carry no control points; junction (`facingSeg`, `legs`) is
**recomputed** from topology on load (`refreshJunction`), then `setting`/`spring` restored.

### 3.2 Track graph (`src/engine/track.ts`)
* `TrackNode {id, pos, segs[], junction|null, y?}` (`:24-51`); `Segment {id,a,b,straight?,
  jenisRel?, tanpaLaa?, jenisJembatan?}` (`:55-83`); `TrackPos {segId, s (m from node a),
  dir: 1|-1 (+1 = toward b)}` (`:93-97`).
* **Points (wesel)** = any node with exactly 3 segments (`refreshJunction`, `:252-286`).
  `facingSeg` = the leg whose direction has the minimum Σ dot-product with the other two (most
  opposed). `legs = [other two in n.segs order]` — `legs[0]` is *not* guaranteed geometrically
  straight; `setting ∈ {0,1}` indexes `legs`; `spring` = leg to return to when free; `lockedBy` =
  route id (runtime). Traversal `nextSeg` (`:500-512`): from `facingSeg` → `legs[setting]`; from
  `legs[setting]` → `facingSeg`; from the other leg → blocked `'junction'`.
* **Path = one cubic Bézier per segment** (`geom()`, `:363-401`): endpoints `pa, pb`;
  `d = |pb−pa|`; if `straight`: `cp1 = lerp(pa,pb,1/3)`, `cp2 = lerp(pa,pb,2/3)`; else
  `cp1 = pa + ta·d·CP_K`, `cp2 = pb + tb·d·CP_K`, **`CP_K = 0.35`** (`:110`), with `ta/tb` from
  `nodeTangentAxis` (`:340-361`) flipped so they point toward the opposite node:
  1. non-junction node with an attached `straight` segment → that segment's chord direction;
  2. degree 1 → toward its only neighbour; 3. junction → `n.pos − facingNode.pos`;
  4. degree 2 → chord between the two neighbours; 5. fallback `(1,0)`.
  Standard cubic Bézier and derivative (`vec.ts:28-44`).
* **Arc-length LUT**: `count = min(240, max(10, ceil(d/4)))` uniform-t samples; `s` = cumulative
  polyline length; `length = sAcc` (`:384-397`). `sampleAt(segId, s)` clamps, binary-searches,
  linearly interpolates `p` and `tan` (renormalised) (`:411-428`). `walk(pos, d)` (`:515-557`)
  crosses nodes: entering at `a` → `s=0,dir=1`, at `b` → `s=L,dir=-1`.
  The C++ port must replicate this LUT exactly (`docs/port-cpp.md` M1 asks for 1e-9 agreement).

### 3.3 Rail mesh generation (`src/tiga/bangun3d.ts` `Bangun3D.bangunRel`, constants in `dunia3dKonst.ts`)
Note: there is no `bangunJalanRel`; the builder was extracted to `bangun3d.ts`.
* Sampling: `n = max(1, ceil(L / LANGKAH_SAMPEL))`, **`LANGKAH_SAMPEL = 4` m** (`Konst:87`,
  `bangun3d.ts:119-135`); sample = `{x: p.x−ox, y: tinggiRelSeg(seg,s), z: p.y−oz, tx, tz, s}`.
* Extrusion `profil()` (`:137-157`): lateral normal `nx = −tz, nz = tx`; vertex =
  `(x + nx·lat, y + h, z + nz·lat)`; uv `(u, s/PANJANG_TEX_REL)`, `PANJANG_TEX_REL = 7.76` m
  (`Konst:190`). Quads between consecutive rings. **No superelevation/cant, no pitch of the
  profile** — curves handled solely by rotating the section with the 2-D tangent.
* Cross-section, in (lat m, height m rel. rail head) (`Konst:296-322`):
  - `REL_TAPAK = −0.176`, `REL_L_DALAM = 0.534` (gauge **1.068 m**), `REL_L_LUAR = 0.618`
    (head width 0.084 m, rail height 0.176 m), `BALAS_KAKI = −0.580`.
  - `PROFIL_BALAS`: (−1.795,−0.580) (−1.117,−0.243) (−0.618,−0.176) (+0.618,−0.176)
    (+1.117,−0.243) (+1.795,−0.580) → base width 3.59 m, ~1:2 side slope.
  - `PROFIL_REL_KIRI`: rectangular U from lat −0.618 to −0.534, y −0.176…0 (mirrored for right).
  - Texture U: `uLat(lat) = 0.36 + lat·0.1042`; `U_REL_BADAN 0.775`, `U_REL_KEPALA0 0.800`,
    `U_REL_KEPALA1 0.822`.
* **Sleepers are not meshed** — painted into the 512² ballast canvas (`bangun3d.ts:88-116`):
  base `#4b463f`, 26 000 speckles `#5a544a/#3d3933`; sleeper bars every `JARAK_BANTALAN = 0.6` m
  (`Konst:191`), 0.26 m thick, lat ±0.75, colour `#3a3027` (+ `#463a2e` top line); rail web `#4a3527`;
  rail head gradient `#6b6560→#b8b2ab→#7a736c`. `PX_PER_M = 512/7.76`. wrapS clamp, wrapT repeat.
* Materials (`dunia3d.ts:1302-1303`): ballast `MeshLambert{map}`; rail `MeshStandard{map,
  metalness 0.3, roughness 0.42}`; concrete `0x9aa0a6 r0.92 m0.02`; tunnel `0x4a4d52 r0.98`;
  truss steel `0x5c6b5a r0.68 m0.35`.
* Bridges (`jenisRel:'jembatan'`, `:342-384`, `uji3dJembatan.ts`): deck slab from
  `BALAS_KAKI−0.02` down `DEK_TEBAL = 1.4` (`DEK_BAWAH = −1.98`), half-width `DEK_TEPI = 3.4`;
  parapet 1.05 h × 0.18; shape `jenisJembatan ?? bentukJembatan(span, height)`: viaduk if
  height ≥ 25, rangka if span ≥ 50 ∧ height ≥ 8, else dek (`uji3dJembatan.ts:265-269`); Warren
  truss `RANGKA_T 7.0`, panel 8 m; piers every `PILAR_JARAK 28` m (×3 for truss), 1.9 m box,
  skipped if < 2.5 m clearance. Parallel double-track structures paired when spacing ≤ 9 m and
  |cos| ≥ 0.985 (`:44-52`).
* Tunnels (`:385-415`): half-circle arch `r = halfwidth − 0.95`, spring line `TRW_SPRING 2.10`,
  14 arc segments, floor `kaki − 0.3`, portal ring 1.3 m long × 1.15.
* Draw calls: indices split per **640 m chunk** (`PETAK_DUNIA = SISI_PETAK = 640`, `petak.ts:21`,
  `Konst:633`); one mesh per chunk per material (balas, rel, jembatan, mulut-terowongan,
  terowongan, rangka) + 1 instanced pier mesh + 1 `LineSegments` "rel-ikonik" (yellow thread at
  y+0.5, colour `TEMA.garis`, shown when camera ref distance > 700 m, hidden < 500 m,
  `Konst:1388-1389`). Chunk bounding spheres computed by hand from indexed vertices
  (`petak.ts:55-68`) because chunks share one position buffer.

### 3.4 Vertical profile (`src/tiga/uji3dProfil.ts` `hitungProfil`, `:582`)
Inputs: graph, per-segment length, `demMentah(x,y)` raw DEM, `stasiun[{x,y,r}]` with
`r = PERON_PANJANG·0.8 = 160` m (`medan3d.ts:388-399`). Defaults `BAWAAN` (`:67-77`):
`langkah 20 m, sigma 120 m, tolDP 1.2 m, baseline 1000 m, margin 1.3, gradienLantai 0.005,
gradienAtap 0.05, radiusVertikal 4000 m, rampStasiun 150 m`.
Pipeline (line refs in file):
1. Chains (`:89-134`): walk from every node of degree ≠ 2; **chains break at every wesel**.
2. Sample every 20 m; mark `buta` (blind) on bridge/tunnel segments (`:594-607`).
3. Pair parallel roadbeds (`:331-577`): within `LAT_BADAN 14` m (30 m near stations), |cos| ≥ 0.985,
   ≥ 60 % overlap; passengers copy the leader's curve.
4. Blind runs → straight line between edges (`:632-650`).
5. Gaussian blur, σ = 6 samples, radius 18, edge-clamped (`:154-174`).
6. **`gmax = min(0.05, max(0.005, p98 · 1.3))`** where p98 = 98th percentile of |Δh/Δs| over
   1000 m windows (`:659-670`). Never hard-code.
7. Station flattening (`:676-713`): weight `smoothstep(1 − (d−r)/150)` for `d > r`, datum =
   median of smoothed heights within `r` (all chains).
8. Pins (`pinKanSemua`, `:772-852`): hand `tinggi` nodes and chain ends are exact; smoothstep
   interpolation between pins; ramps to zero over 6 samples outside.
9. Douglas–Peucker tol 1.2 m with forced knots at blind/station boundaries and pins (`:178-212`).
10. Gradient clamp `klemGradien` ≤ 80 alternating Lipschitz sweeps (`:216-239`).
11. Vertical curves R = 4000: `Lv = min(R·|Δg|, 0.8·gap_prev, 0.8·gap_next)`, parabola
    `h = kh + g₀(s−ks) + (g₁−g₀)/(2Lv)·(s−(ks−Lv/2))²` (`:903-932`).
12. Output `tinggiSeg: Map<segId,{s[],h[]}>` with `round(L/20)` uniform samples per segment;
    `tinggiDi(segId,s)` linear (`:995-1010`). Heights in raw DEM metres.
Invariant (`tests/profil3d.ts:164-194`): the three segments at a wesel agree within 0.05 m; real
saves gombong-wns/cepu/awn ≤ 0.03 m.

### 3.5 Spline objects "garis" (`src/tiga/uji3dSpline.ts`)
Centripetal Catmull-Rom through saved points (`:85-88`), duplicates < 0.05 m removed,
`arcLengthDivisions = clamp(ceil(chord/0.25), 200, 6000)`. `bingkaiGaris` (`:117-177`): body tiles
at `s = (i+0.5)·langkah`, remainder tile x-scaled if `sisa > 0.02·langkah`; posts at `i·langkah`
(+ one at L if remainder > 0.35·langkah); `yaw = atan2(−t.z, t.x)`; `datar` classes interpolate
height linearly end-to-end with constant pitch; instance Euler `(0, yaw, pitch, 'YZX')`, scale
`(skala,1,1)`. Step comes from `model.json.garis[].langkah`, never from the mesh bbox.
Platform `peron` (procedural): `PERON_TINGGI 1.0`, `PERON_LEBAR 6.0`, `PERON_ROK 1.8` (body
2.8 m tall centred at y −0.4), tile 2 m, yellow line 0.3 wide 0.8 from edge, canopy 3.4 high /
5.6 wide, posts every 6 m; colours floor 0xc9c5bd, wall 0xa8a49c, yellow 0xe0ac10 (`:276-337`).
No save currently contains any `garis`.

---

## 8. Sim ↔ render interface (what the 3D layer reads/writes)

### 8.1 Construction (`src/main.ts:265-376` → `new Dunia3D(KonteksDunia3D)`, `dunia3dKonst.ts:1272-1337`)
Everything is injected as getters/callbacks: `world()`, `ixl()` (Interlocking), `session()`,
`gelap()`, `koridorId()`, `modeSurveyor()`, `stasiunPemain()`, plus callbacks
`klikSinyal(id)`, `klikWesel(nodeId)`, `pilihKA(id)`, `rincianKA(id)`, `beriS40(id)`, `hapusKA(id)`,
`aksiLangsir(id, aksi)`, `pilih3D(jenis,id)`, `status(text)`, `progres(text,frac)`.

### 8.2 Session/GAPEKA schema (`session.ts:12-28`, `train.ts:266-321`)
```ts
SessionDef { name; startClock /*s since 00:00*/; entries: ScheduleEntry[]; durasiDinas?; wartaManual?; putarLok? }
ScheduleEntry { trainNo; name?; consist /*CONSIST_LIBRARY key*/; spawnPortal; spawnTime; stops: StopDef[];
                lintas?: {station, time}[]; exitPortal; exitTime; maxSpeed?; exitBerth?; spawnBerth?;
                sambungDari?; lanjutArah? }
StopDef { trackmark /*trackside name*/; arr; dep; meets?: [{type:'silang'|'susul', with}] }
```
Runtime `Session` (`session.ts:260-`): `clock` (s), `timeScale`, `paused`, `trains: Train[]`,
`putarLok?`. `step(realDt)` splits into substeps of **`MAX_STEP = 0.5` s** (`:523-541`).

### 8.3 Train state read per frame (`train.ts`)
`Train { id, trainNo, consist:{name, cars: CarDef[]}, speed (m/s), front: TrackPos, trace:
{segId,dir}[], state:'run'|'dwell'|'done', menahanJml, s40Siap, s40Fase, holdReason }`.
Only the **front coupler** is stored; cars are derived by
`pointBehind(world, d): {p, tan, segId, s}|null` (`train.ts:1408-1434`) which walks `trace`
backwards `d` metres and returns `tan` already flipped to the direction of travel.
`istirahat(clock)` (`:1489`) = resting (lights off); `telatKini(clock)` (`:1442`).
`GAP = 0` (`:363`); `CarDef.length` is coupler-to-coupler.

### 8.4 Per-frame reads (`dunia3d.ts:3921-4139` `loop`)
| what | source |
|---|---|
| dt | `min(0.1, real dt)`; sim dt = `dt·(paused ? 0 : timeScale)` |
| signal aspects | `ixl.beginAspectFrame()` then `ixl.aspectOf(id)` → `'red'|'yellow'|'green'` (`interlocking.ts:894-1023`) |
| semaphore arm | `lenganTarget(obj, asp)` (`trackside.ts:135-163`) + spring (§6.3) |
| lamp night mode | `jam = clock/3600 % 24`; night if `< 6 ∨ ≥ 18` (`:3981-3982`) |
| indicator board lit | `ruteMasukBelok(ixl,id)`: route from this signal passes a wesel on `legs[1]` (`keretaVisual3d.ts:391-401`) |
| points | `graph.nodes.get(id).junction` → `setting`, `legs`, `lockedBy` (`bangun3d.ts:798-826`) |
| JPL barrier | every 0.25 s: `world.distToTrainAhead({segId,s,dir}, 350, '')` on tracks within `JPL_LEBAR_M = 25` m (`bangun3d.ts:657-691`, `world.ts:31`) |
| route ribbons | `ixl.routes.values()` → `r.def.segs − r.released`; occupancy `world.occupancy` (`hud3d.ts:584-607`) |
| trains | `session.trains`, each car via `pointBehind`, `t.speed`, `t.state`, `t.istirahat()` (§7.4) |
| world edits | `world.onTrackEdit(TrackEditEvent{removed,touched,posRemap,chainRemap})` (`world.ts:51-56`, `app.ts:125-133`) → `dunia3d.tandaiRelKotor()` → full rail rebuild next frame |
| trackside/scenery changes | 1 s fingerprint over trackside/scenery fields → rebuild objects (`:3937-3971`) |
| sun | `session.clock` + `worldToLonLat(origin)` (§9.2) |

### 8.5 Inputs sent back
* Click signal → `klikSinyal(id)`: in `main.ts:346-375` resolves shared-exit groups, cancels the
  active route if `ixl.dapatDibatalkan`, else `tarikAhli(sig,false)` (= `trySetRoute`).
* Click points → `klikWesel(nodeId)` → `world.balikWesel(nodeId)` (`world.ts:683`, returns
  `{ok}|{ok:false, alasan:'terkunci'|'terinjak'}`).
* Label buttons → `pilihKA`, `rincianKA`, `beriS40`, `hapusKA` (penalty 2000), `aksiLangsir`.
* Surveyor edits write `node.y` and call `world.moveNode` (cosmetic, out of scope).

---

## 4. Terrain ("tanah", `src/tiga/medan3d.ts`, `ubinStream.ts`, `dunia3d.ts`)

### 4.1 Height source
* Terrarium PNG tiles: `${TILE_168}/terrarium/{z}/{x}/{y}.png`, `TILE_168 =
  VITE_TILES_BASE_URL || 'https://tiles.168railway.com'` (`medan3d.ts:27-29`, `Konst:838-840`);
  fallback once `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/…` (`:34-36`).
* Decode **`h = R·256 + G + B/256 − 32768`** (`:238, :313`), 256 px tiles, bilinear with
  `fx = (wx−x0)/ts·(n−1)` (`:306-318`).
* Zooms: `DEM_Z = 13` core (track nodes ± `MARGIN_DEM_INTI 5000` m, never evicted),
  `DEM_Z_JAUH = 10` for the whole bbox ± 2000 m (`Konst:88, 219, 227`; `:157-243`).
* `tinggiTanah(wx,wy) = (dem − demBase)·1` (`:383-386`).
* Brush deltas `world.tanah.delta`, grid `KISI_DELTA = 8` m, bilinear (`:518-527`).

### 4.2 Corridor carving = embankment/cutting (`tanahTerukir`, `:529-549`)
Rail centreline registered every 12 m (3 samples) into a 64 m hash grid (`SEL_GRID`), **only for
at-grade segments** (bridges/tunnels skipped, `bangun3d.ts:422-431`), weight `b` = smoothstep of
distance to tunnel mouth / `TRW_RAMP 45`.
```
d = distance to nearest rail centreline
t = d <= UKIR_DALAM(9) ? 1 : 1 − (d − 9)/(UKIR_LUAR(60) − 9)
w = t²(3−2t) · b
h = h·(1−w) + (dasarUkir + BALAS_KAKI − 0.04)·w        // plateau 0.62 m below rail head
```
`dasarUkir` lowers to the lowest rail within 20 m by ≤ 1.5 m when the difference ≤ 4 m
(`RENDAH_*`, `Konst:100-107`). Bridge trough: ground only lowered to `deck_y − 1.98 − 1.5` within
7 m, blending to 26 m (`JBT_*`, `Konst:162-169`). Order: DEM + delta → rail carve → bridge.

### 4.3 Ground mesh — the map tile *is* the terrain (`dunia3d.ts:2505-2634`)
* Tile = z14 slippy tile (**2 446 m**), `UBIN_Z = 14` (`Konst:252`), textured with satellite
  imagery `${TILE_168}/satellite/{z}/{x}/{y}.png` (Esri fallback, order z/y/x). Loading colour
  `0x1a2027`, then `MeshLambert{map, color 0xcfcfcf}` sRGB, max anisotropy (`:2926-2946`).
  Detail textures: z16 (1024 px) within 1500/2200 m, z17 (2048 px) within 450/800 m
  (`DETAIL_TANAH`, `Konst:685-689`).
* Grid: 19 blocks of `BLOK_TANAH 128` m per tile; block cell size `SEL_TANAH_DEKAT 8` m if rail
  within `MARGIN_BLOK 68` m, `SEDANG 30` m within 500 m of a near block, else `JAUH 60` m
  (`Konst:133-151`). Vertex y = `tanahTerukir`. UV `u=(lx+ts/2)/ts, v=1−(lz+ts/2)/ts`. Skirts at
  seams and tile edges, depth `max(4, 0.45·max(cell))`.
* Streaming (`ubinStream.ts:88-124`): centre = orbit target; distance to tile **edge**;
  load ≤ `UBIN_R_MUAT 4000` m, evict > `UBIN_R_BUANG 6000` m (hysteresis), 2 tiles per 220 ms
  check (`Konst:237-246`); touch tiers 2000/3000 … 1100/1700.
* Far layer: z10–12 tiles at 6 cells per z14 tile (≈ 408 m), uncarved DEM, holes punched where
  near tiles exist (`:2722-2792`). Backdrop plane 80 km at `min(−12, demMin − demBase − 30)`,
  colour `TEMA.hampar` (dark 0x151c24 / light 0x9db089).
* **There is no ±11 m grass shoulder mesh** (dropped; only a comment in `uji3dJalan.ts:106-107`).
* Everything here is procedural; no terrain GLBs. Tunnel portals `nry-terowongan-ijo-*.glb` are
  hiasan objects only in kroya-style scenes.

### 4.4 Vegetation (`vegetasi.ts`, `vegetasi3d.ts`) — instanced GLB trees
Cells `SEL_VEG = 192` m; hash `acak(x,z,k) = frac(sin(12.9898x + 78.233z + 37.719k)·43758.5453)`.
Green mask from the satellite tile: `ExG = (2G−R−B)/(R+G+B)`, `hijau = clamp((ExG−0.05)/0.18)`,
`gelap = clamp((meanBrightness + 0.06 − v)/0.16)`, `p = hijau·(0.35 + 0.65·gelap)` (`:97-151`).
Spacing `11/√rapat` m (`RAPAT_BAKU = 2` → 5.5 m), accept if `acak(x,z,5) < p`, reject within
`bebas = 30` m of a track or inside a building; scale `0.72 + 0.75·acak`, `POHON_TINGGI 9` m;
LOD `ambang = d ≤ 500 ? 1 : max(0.05, 500²/d²)`; cap 48 000 instances. The sphere-tree fallback
(`BOLA_KAP`) described in `arsitektur-3d.md` was **removed** (commit f50f716).

### 4.5 Roads
Baked in `public/kota/<slug>.json` `jalan[]` (widths `uji3dJalan.ts:35-40`: motorway 14,
primary 10, residential 5, service 3.5 …; clip 7 m from rail) but **not rendered** in the game
(`kota3d.ts:467-469`) and not baked by default (`pitaJalan` default 0).

---

## 5. Scenery

### 5.1 `SceneryObj` (`src/engine/scenery.ts:8-36`)
`kind: 'station'|'building'|'platform'|'jpl'|'house'|'shed'|'tree'|'kmpost'`;
`{id, kind, pos:{x,y}, rot (rad), w, h, label, code?, dpl?, semboyan40?, s40Untuk?}`. Defaults
station 30×30, jpl 14×14, platform 120×6 (`:49-62`). Stations carry no model/berth data — the
building is a separate hiasan object, berths are trackmarks (§6.1).

### 5.2 Baked OSM city (`src/tiga/kota3d.ts`, data `public/kota/<slug>.json`)
Exists for awn, bekasi, cpd-chaos, gombong-wns, kedungbanteng-magetan, maswati-cilame, pwk-pdl
(not mojokerto/pemalang). Schema: `{koridor, pita, pitaHijau, pitaJalan, bebas:30, asal:[lon,lat],
bangunan:[{k, h, r}], hijau, baris, pohon, jalan, perlintasan}`; ring `r` = integer 1e-6°
offsets from `asal`, first pair absolute then deltas (`:57-68`). Rules (`:258-355`): skip if any
vertex < 30 m from track; minimum-area bounding rectangle → ridge axis; base = min carved ground
− 0.5; `h` from OSM (`height` or `levels·3.2+1`, 0 = unknown); flat roof if type ∈ `ATAP_DATAR`
or h > 8, height default 8 (flat) / 4.2 (gable); gable rise `clamp(0.7·halfwidth, 0.9, 2.8)`,
overhang 0.5; wall palette `[0xd9cfbc,0xe4dfd5,0xcfd6c4,0xc7d2d9,0xd3c3ab,0xe0d3c3,0xcbc4b4]`,
roof `[0x9c4f30,0x8a4429,0x6d4632,0x7f8a90,0x66757e,0xa8583a,0x5f6a70]`, chosen by `acak(cx,cz,1|2)`;
one walls-mesh + one roof-mesh per 640 m chunk; max 60 000 buildings.

### 5.3 What `bangun3d.ts` does *not* do
It builds rails, bridges, tunnels, signals, boards, JPL gates and point arrows; it instantiates
**no GLB** and no station/platform. (`arsitektur-3d.md` is vague here.)

### 5.4 Hiasan — free objects (`src/tiga/uji3dTata.ts`)
Saved (`:1252-1269`): `{model, x, y (world Mercator), naik (m above ground), rot (degrees, may be
negative), skala, kunci?, teks?, ketinggian?}`. Runtime (`:734-738`):
`position = (x−ox, tanahTerukir(x,y) + naik, y−oz)`, `rotation.y = rot·π/180`, uniform scale.
Objects placed ≤ 25 m from a track default to the track yaw (`:780`). No per-catalog scale or
y-offset — only the model normalisation of §10.3. Station buildings in saves: all `naik 0,
skala 1` (mojokerto rot −1.7°, pemalang 171.5°, bks 147.6/159.1/164.4°, cepu 36.5°, kroya
188/7.8/112.8/113°).

### 5.5 Procedural KRL station (`uji3dStasiunKRL.ts`) — reference dimensions
Platform floor 1.00 m, LAA wire 5.0 m, JPO deck 6.6 m, roof ridge 14.3 m, platform width 8 m,
`tataLetakKRL(n)`: track at platform-axis ± (4 + 1.7) m, next platform +15.9 m (`:525-553`).

### 5.6 Trackside boards and JPL (`uji3dPapan.ts`, `uji3dJPL.ts`)
Boards 3 m from the track axis on `sisi` (default right of travel), pole 2.2 m, facing the
approaching train; s35 0.6×0.5 `#151515`, taspat 0.55×0.45 `#e8b23b`, kmpost 0.42×0.24 at 0.68 m;
10G stop mark 2.8×0.28 across the track. Trackmarks are deliberately not drawn.
JPL: two posts at `(±a, ±b)` in road frame, `a = max(7.0, halfSpanOfTracks + 3.2)`, `b = 3.6`,
barrier 6.0 m at 1.05 m rotating about X by `−open·π/2`; closes when `distToTrainAhead ≤ 350` m,
`DETIK_PALANG = 5` s.

---

## 6. Signals and points

### 6.1 `TracksideObj` (`src/engine/trackside.ts:1-91`)
`kind: 'signal'|'buffer'|'trackmark'|'trigger'|'dirmarker'|'speed'|'portal'|'stopmark'|'s35'`;
fields `{id, kind, segId, s, dir: 1|-1 (applies to trains moving this way), name, radius,
speed?, signalType?: 'interlocking'|'auto'|'muka'|'pengulang'|'bersama', lampu?: 2|3 (default 3),
linkedSignal?, berarah?, sisi?: 'kiri'|'kanan' (default kanan = right of travel), sepur?:
'lurus'|'belok', arah?, baris?, bentuk?: 'elektrik'|'mekanik' (default elektrik), tiup?,
papanAngka?: 0|3|4, lengan?: 1|2|3, batasLangsir?}`. **Note: the value is `'mekanik'` for
semaphores and `'elektrik'` (not `'cahaya'`) for colour-light.** Station code is parsed from the
name: `/^(.+?) (M[A-Z]\d*|K\d+[BT])$/` (`:115-118`); entry signal `M…`, exit `K<track>T/B`.
Berths/platforms = `trackmark` objects (name = station code, e.g. `MR`), referenced by
`StopDef.trackmark`. Portals = `portal` objects (`P.CRM` …).
`Aspect = 'red'|'yellow'|'green'` (`:13`). Lamps present: muka → [green,yellow]; `lampu 2` →
[green,red]; else [green,yellow,red] (`daftarLampu`, `:16-19`); if the aspect's lamp is absent, red
(or last lamp) lights with `izin:true` (`:33-37`).

### 6.2 Colour-light signal model (`src/tiga/uji3dSinyal.ts`)
Local axes: **−X = face** (train approaches from −X), +Y up, +Z right of travel; origin = mast foot
at rail-head height. Placement (`bangun3d.ts:535-620`): `p = sampleAt(seg,s)`, `t = tan·dir`,
`sisi = kiri ? −1 : +1`, `n = (−t.y·sisi, t.x·sisi)`, **position = p + n·3 m**, `rotation.y =
atan2(−t.y, t.x)`.
`UKUR` (`:38-89`): lens Ø 0.200, lamp pitch **0.230**, ring Ø 0.240, hood 0.120, plate 0.400 ×
0.780 × 0.050, mast Ø 0.114, mast from −2.50 to +3.55 (6.05 long), **red lens centre y = 3.60**.
Lens i at `(−0.070, 3.60 + 0.23·i, 0)`, order [red, yellow, green] (3-lamp) or [red, green]
(`titikLampuSinyal`, `:726-729`). Plate bottom 3.48; diamond board 0.573² at plate top + 0.23;
number panel 0.288 × 0.470; shunting octagon head y 3.19; number plate y 2.37. Mast stripes
`#f2a516/#1c1f24` every 0.90 m. Lit lens colour = `WARNA_ASPEK = {red 0xff3b30, yellow 0xffcc00,
green 0x34c759}` (`Konst:331-333`), unlit `0x2a2a2a`; pengulang uses white.
Corona: two additive sprites at the lens, `scale = max(0.38, 13·dist·(2·tan(fov/2)/H))`,
`opacity = (night?1:0.7)·(0.06 + 0.94·clamp(cos)²)`, fade out by 2500 m (`:685-743`).
LOD: head mesh only when camera reference distance < `AMBANG_LOD 900` m, else a 1.5 m sphere at
rail +1.2 m in the aspect colour, scale `max(1, d/320)`.
Pengulang (Semboyan 9C): disc Ø 0.75 at y 3.55, 14 LEDs pitch 0.125 in vertical/diagonal/
horizontal bars = green/yellow/red (`:903-1072`).

### 6.3 Semaphore (`uji3dSemafor.ts`, spec `docs/semafor-sculpt-spec.json`)
Arm pivot heights: masuk **7.0**, keluar **5.5**, muka **5.0**; arm 1.20 m long × 0.19, disc Ø 0.34
at 1.03; second arm 0.85 m below; mast lattice = 2 crossed alpha planes (0.40 → 0.18 wide) from
−2.20 to pivot + 0.55; spectacle glasses at pivot angles [0, 45°], colours red 0xff2a1f / green
0x2fd45f (muka: yellow 0xffc21f / green). Arm angle from `lenganTarget` (`trackside.ts:155-162`):
1 arm `[red ? 0 : 45°]`; 2 arms red `[0,0]`, yellow `[45°,0]`, green `[45°,45°]`; muka
`[green ? 45° : 0]`. Spring `k = 150, c = 15`, `h ≤ 0.05` s per step (`:740-771`).

### 6.4 Points visuals (`bangun3d.ts:694-761`, `Konst:1093-1098, 1203-1213`)
Group at the node, rail height. Arrow polygon (−0.50,−0.13)(0.04,−0.13)(0.04,−0.34)(0.50,0)
(0.04,0.34)(0.04,0.13)(−0.50,0.13) extruded 0.16, scaled by `PANAH_M 2.4`; one arrow per leg,
placed `PANAH_JARAK 1.45` m laterally on that leg's side, `PANAH_TINGGI 2.2` m up, pointing
sideways toward its leg. Colours per frame: set+unlocked `0x2fd85a`, set+locked `0x1ea94a`,
unset `0xff3b30`, unset+locked `0x8f342e`. Padlock sprite at height 4.1 when `lockedBy`. Within
340 m: ground glow planes 30×12 and 30×7. **Layer default OFF** (`TAMPIL_BAKU.wesel = false`).

### 6.5 Picking (`dunia3d.ts:2259-2309`, `hud3d.ts:161-175`)
Pure **screen-space distance**, not a ray cast (Raycaster is used only for terrain/hiasan editing).
Project to pixels; nearest visible points arrow-centre within `RADIUS_WESEL 40` px; nearest
signal (top lens, or LOD sphere) within `RADIUS_SINYAL 26` px; **signal wins** if no wesel or
`ds < dw` or `ds < RADIUS_SINYAL_MENANG 18` px. Clickable: signals, points (only when the wesel
layer is visible), HUD signal name plates, train label DOM buttons, station bubbles (fly-to).
Trains themselves are not pickable in 3D. Hover: white ring `RingGeometry(0.86,1)`, tooltip,
route preview ribbon via `ixl.traceByPoints` (blue 0x1cb0f6 / red dead-end).

---

## 7. Trains (rangkaian)

### 7.1 Rolling stock (`src/engine/sarana.ts:89-240`, coupler-to-coupler lengths)
cc203 15.26 m · cc201 15.26 · cc206 17.20 · k1 20.62 · k3 20.53 · m1 20.62 · p 20.62 · mp 20.50 ·
gd 13.37 · gk 12.44 · d14 12.85 · krl-kuha/moha/moha-p 20.0. Colours (`warna`, used for boxes):
cc203/cc206 `#e8e8e8`, cc201 `#e8b23b`, k1/m1 `#3b6fd6`, k3 `#d67f3b`, p/mp `#4a5058`, gd `#4a4136`,
gk `#3c3c40`, krl `#d63b3b`.
`CONSIST_LIBRARY` (`train.ts:59-154`): `krl-8` kuha,moha,moha-p,moha,moha,moha-p,moha,kuha;
`eksekutif` cc206 + 4 k1 + m1 + 3 k1 + p; `lokal` cc203 + 5 k3; `commuter` cc201 + p(commuter) +
8 k3(commuter); `barang` 2 cc206 + 20 gd; `barang-ketel` 2 cc206 + 16 gk. Loco lottery
`VARIAN_LOKO` by FNV-1a `hashNomor(trainNo) % pool` (`:172-188`); second loco kept only if
`hashNomor(trainNo+'#traksi') % 6 === 0`. **`consistUntuk(key, trainNo)`** (`:219-261`) is the
single source of the car list — the 3D layer calls the same function.

### 7.2 Sarana → model (`dunia3dKonst.ts:544-557`, `armada.ts`)
`MODEL_SARANA`: cc201→`loko201` (`pk-cc201.glb`), cc203→`loko203` (`pk-cc203.glb`),
cc206→`loko206` (`pk-cc206.glb`), krl-kuha→`nryJr205KuhaBadan`, krl-moha→`nryJr205MohaBadan`,
krl-moha-p→`nryJr205MpBadan`, k1/k3→`kereta`, m1→`makan`, p→`pembangkit`, gd→`gerbong`,
d14→`atpD14Jaladara`. Passenger cars resolve **armada first**
(`keretaVisual3d.ts:262-289`): `car.armada` slot → `modelArmada(armadaUntuk(trainNo), sarana)` →
`MODEL_SARANA`. `armadaUntuk` = `UNDIAN[hash % 11]`, `UNDIAN = [taksaka, pecut-merah, pecut-pink,
new-era, new-era, ekonomi, ekonomi, kaca-lebar-96, new-image-16, baku, baku]` (`armada.ts:334-397`).
`baku` = k1 `nryNeK118Badan`, k3 `nryK324BasicBadan`, m1 `nryM1BasicBadan`, p `nryP24BasicBadan`,
mp `nryMp1604Badan`, gd `nryGerbongDatarBadan`, gk `nryGk306348Badan`; `commuter` = k3
`nryK3V4LpBadan`, p `nryKp3V2CommuterBadan` (full table `armada.ts:81-322`).
`SLOT_BALOK` (`Konst:490-533`) = models drawn as boxes: `kereta, makan, pembangkit,
atpD14Jaladara` and every non-`nry*` armada model (`anr*`, `atp*`) — so **freight gd/gk with
`nry` models are drawn as GLBs**, third-party ones as boxes. (`arsitektur-3d.md` still says lokos
are boxes; since 2026-08-31 lokos are our own `pk-cc20x.glb`.)
Catalog attachments: lokos `bogie {a.bog0,a.bog1: pkBogieGeU18c|pkBogieCc206}`, `kopling {'*':
nryKoplingK3V4}`; all `nry*Badan` passenger → `nryBogieK5tLp` (`nry-bogie-k5t-lp-v2.glb`, 3.68 ×
0.91 × 2.10 m) + `nryKoplingK3V4`; gd/gk → `nryBogieBarberLp`; JR205 per-end couplings
`nryKoplingJr205{Kuha,Moha,Mp}{Depan,Belakang}`.

### 7.3 Model conventions (`sarana3d.ts:120-325`)
* **Forward = +X**, up +Y, right +Z. If bbox `sz.z > sz.x` and no node has `userData.sumbuX`,
  rotate `y += π/2` (`:217-220`).
* Pivot: translate by `(−centre.x, dy, −centre.z)`; `dy = 0` if any node matches `/^abog\d+$/`
  (y = 0 of the GLB is the rail head), else `−bbox.min.y` (`:246-255`). Attachments loaded
  with `datumAsli = true` keep their origin.
* `panjang` = `extras.panjangKopling` → `|x(a.limfront) − x(a.limback)|` → bbox `sz.x` (`:264-288`).
  Scale at render = `car.length / proto.panjang` (`keretaVisual3d.ts:629`).
* Attach nodes (measured): passenger `a.bog0/1 (±7.0,0,0)`, `a.kopling0/1 (±9.5,0.78,0)`,
  `a.limfront/back ±10.265`; cc206 bogies `(±5.0,0,0)`, couplers `(±7.835,0.78,0)`, lim ±8.60;
  cc201/203 bogies `(3.723,−0.048,0)/(−4.22,−0.048,0)`, lim +7.623/−7.634; JR205 kuha bogies
  `(−6.6412,0,0)/(+6.8172,0,0)`, couplers `(+9.2065,0.9,0)/(−9.473,0.9,0)`, lim ±10.0.
  Coupling models: origin at the attach point, extend +X (0.026…0.876 m for `nryKoplingK3V4`);
  odd-numbered `a.kopling<n>` rotated 180°. Bogies are **rigid children** (no curve steering, no
  wheel rotation — `sarana3d.ts:503-506`).
* Lights: nodes `light\d` = headlight (`sorot`), `ditch_white` = `muka`, `ditch_red|s21|red\d` =
  tail (`akhiran`); fallback points at `x = ±L/2·0.97, y = h·0.41, z = ±W/2·0.64`; colours
  `0xffe9b0` head, `0xff2f1c` tail, corona 0.22 m (`Konst:398-446`). Doors: clips `pintu-kiri/kanan`
  scrubbed by dwell time; pantograph clips `panto-*` frozen at the last frame.
* Driver eye `MATA_KABIN` `{mundur, tinggi, sisi}`: default `{2.2, 3.2, 0}`; cc201/cc203
  `{2.4, 3.05, 0.6}`; cc206 `{0, 3.05, 0.6}`; krl-kuha `{0.5, 2.75, −0.55}` (`Konst:1237-1244`).

### 7.4 Per-car placement (`keretaVisual3d.ts:563-704`) — exact
For car `i` at running offset `d` from the front coupler:
```
A = t.pointBehind(world, d);  B = t.pointBehind(world, d + len);  d += len   (GAP = 0)
a = (A.p.x−ox, A.p.y−oz), b likewise; (dx,dz) = a − b; lateral k = (−dz, dx)/|.|
hA = tinggiRelSeg(A.segId, A.s), hB likewise
mA = medan(ax, az, v, skala); a += k·mA.geser; hA += mA.naik   (same for B)
L = |a−b|; centre = (a+b)/2; hy = (hA+hB)/2
yaw = atan2(−dz, dx); pitch = atan2(hA − hB, L); roll = hitungGoyang(...).roll
mesh.position = (cx, hy + 0.02, cz)         // third-party 'bawaan' models: +0.35
mesh.rotation = Euler(roll, yaw, pitch, 'YZX'); scale = len / proto.panjang
```
Last car of a `krl-8` (`MODEL_KRL_KABIN`) is flipped: `yaw + π`, `−roll`, `−pitch`. Box
fallback: `Box(len, loco ? 4.0 : 3.7, 3.0)` at `y = hy + 0.55 + h/2`, colour `car.color` or
`0x9aa3ac`/`0x6f7a85`. Resting trains (`istirahat`) have all lights off. Tail marker Semboyan 21
at `x = −(L/2 − 0.38), y = 1.54, z = ±W/2`.

### 7.5 Sway (`uji3dGoyang.ts`) — body uses only `roll` and `medan`
`respons(v) = r²(3−2r), r = min(1, |v|/22)`; `medan`: `naik = 0.020·r·derau(D_NAIK)`,
`geser = 0.015·r·derau(D_GESER)`; `derau = Σ w·sin(2π/λ·(x·cos a + z·sin a) + φ)` with octave
tables `:61-65` (λ 23–114 m). `hitungGoyang`: `seimbang = −atan(v²κ/9.81)·0.5`,
`cant = clamp(clamp(seimbang, ±3°)·skala, ±7°)`, `roll = cant + 0.40°·r·derau(D_ROLL)`;
`κ = kurvaRel(yaw(A.tan), yaw(B.tan), L)`; `skalaLaju(ts) = ts ≤ 2 ? 1 : 2/ts`.
Euler order **'YZX'** is mandatory.

---

## 9. Camera and sky

### 9.1 Camera (`dunia3d.ts:1546-1591`, `uji3dOrbit.ts`, `uji3dZoom.ts`)
Base `PerspectiveCamera(52°, aspect, 1, 40000)`. Modes and profiles:

| mode | near | far | fov | fog [near,far] | damping | default |
|---|---|---|---|---|---|---|
| bebas (orbit) | 1 | 40000 | 52 | corridor formula | — | OrbitControls, damping 0.08, maxPolar π/2−0.03 |
| jalan (walk) | 0.1 | 12000 | 70 | [400,5000] | — | eye 1.62 m, walk 4.5 m/s, run 12 |
| kabin | 0.15 | 12000 | 62 | [500,6000] | 26 | `MATA_KABIN`, look 100 m ahead |
| samping | 0.5 | 20000 | 52 | [700,9000] | 7 | dist 28, height 7 |
| atas | 1 | 40000 | 52 | corridor | 5 | height 120 |
| ekor | 0.5 | 20000 | 52 | [700,9000] | 8 | dist 34 |

Corridor fog: `near = min(W·0.4, far·0.6)`, `far = min(W·2.2, far·0.98)`, `W` = bbox width
(`:4083-4097`); linear `THREE.Fog`. Damping `k = 1 − exp(−redam·dt)`. Telescope `Z`: fov/4, min 8°.
Initial view (`:2229-2236`): `d = min(max(600, 0.75·max(bboxW, bboxH, 800)), 22000)`, camera at
`(0.35d, 0.62d, 0.72d)` looking at the origin.

### 9.2 Sky and lighting (`uji3dLangit.ts`, `dunia3dKonst.ts:815-818`)
Game default mode **`gradien` + `ikutJam`** (`dunia3d.ts:1035-1043`). Sun from the sim clock
(`matahariDariJam`, `:172-184`): `jamSurya = h + (lon − 105)/15`; `H = (jamSurya − 12)·15°`;
declination 0; `el = asin(sinφ·sinδ + cosφ·cosδ·cosH)`, `az = atan2(−cosδ·sinH, sinδ·cosφ −
cosδ·sinφ·cosH)`; sun direction `(cos el·sin az, sin el, −cos el·cos az)`, light at 4000 m.
Light ladder `TANGGA` by elevation (`:112-119`), columns fog / hemi-sky / hemi-ground / sun colour
/ sun intensity / ambient intensity / zenith / horizon:
```
el −18: 0x121a2b 0x44577a 0x161c26 0x8ea2cc 0.26 0.72 zen 0x0a1020 ufuk 0x18223a
el  −4: 0x2f3a52  …        …        0xff8f63 0.55 0.84
el   2: 0xc98a5e  …        …        0xffb070 1.70 0.95
el  12: 0xd6c1a6  …        …        0xffd9a8 2.45 1.20
el  32: 0xbdd0e4  …        …        0xfff2dc 3.00 1.50
el  75: 0xc8dcef 0xcadff5 0x53534a 0xfffaf0 3.20 1.60 zen 0x4f92cf ufuk 0xc7dcef
```
In gradien mode intensities are scaled by theme: `sun = TEMA.matahari·(kuatSinar/3.20)`,
`ambient = TEMA.ambien·(kuatAmbien/1.60)`; `TEMA.terang = {ambien 2.0, matahari 1.8, kabut
0xcadced, langitAtas 0x4f92cf, langitBawah 0xc7dcef}`, `TEMA.gelap = {1.4, 1.2, 0x121821,
0x0a1420, 0x1d2732}`. Gradient dome `SphereGeometry(8000)` follows the camera. Atmosphere mode
(`sky`) uses three `Sky` with turbidity 3.4, rayleigh 1.35, mie 0.005, mieG 0.8, ACES tone
mapping, exposure 0.58; gradien mode uses **NoToneMapping, exposure 1**, sRGB output.
Clouds: sprite layers 760–1100 m and 1250–1750 m, 0.35 /km², 40–220 sprites.
Shadows **disabled** (`BAYANGAN_AKTIF = false`). Renderer: `antialias = !touch`, pixel ratio
≤ 3, quality tiers `penuh/tinggi/sedang/hemat/minimum` dpr 3/1.6/1.35/1.1/1, vegetation radius
3200/2600/2000/1400/900 m (`Konst:702-711`).

---

## 10. Asset catalog (`public/model3d/model.json`, `ATRIBUSI.md`)

### 10.1 Layout
Top-level keys: `_` (note), `sarana` (object keyed by slot id), `tekstur` (`rel1067`), `objek`
(array, 248 entries), `garis` (array, 52), `__garis` (note). Common entry fields: `id`, `berkas`
(file under `model3d/`), `nama`, `kategori`, `pembuat`, `lisensi`, `sumber` (build recipe; also
records measured size "W × H × L m · tris"), `pilot: true` (third-party, gitignored, skipped
silently if absent), `_catatan`. **No explicit dimension/orientation/scale fields** — sizes are
measured at load (§7.3), orientation follows the +X rule.
* `sarana[slot]` extra fields: `bogie: {"a.bog0": slotId | "*": slotId}`, `kopling: {"a.kopling0":…
  | "*":…}`, `pilotBerkas`/`pilotNama` (local override), `kuid`.
* `garis[]` extra fields: `langkah` (tile step m, mandatory), `datar`, `tiang`/`tiangProsedural`,
  `jarakTiang`, `slot[{prosedural|berkas, jarak}]`, `prosedural` (built in code).
* `objek[]`: `prosedural` entries (`stasiun-krl-*`) are built in code.
* Prefix rules (`ATRIBUSI.md`): `cc0-*` free, `nry-*` our own (deployable), `pk-*` our own lokos,
  `pilot-*` third-party local-only. `.gitignore` excludes **every** GLB; source of truth is R2
  (`VITE_ASET_BASE` + `/model3d/<berkas>`, `src/asetUrl.ts`).
* KTX2: `muatSatu` first tries `model3d/ktx2/<berkas>` (same file name, Basis-compressed
  textures, 497 files locally) then the raw file (`sarana3d.ts:136-141`). Draco geometry allowed
  since 2026-09-08 (`KHR_draco_mesh_compression`).
* Rail texture `tekstur.rel1067` (`pilot-rel-1067.jpg`, 512² atlas, 1 tile = 7.76 m, sleeper band
  u 0.266…0.463) is optional — the procedural canvas of §3.3 has the same layout.

### 10.2 GLBs needed for the Mojokerto scene
| purpose | catalog id | file | status |
|---|---|---|---|
| station | `sttd-stasiun-mojokerto` | `pilot-sttd-stasiun-mojokerto.glb` (304.7×19.9×62.6 m) | pilot, local only |
| locos | `loko206/loko203/loko201` | `pk-cc206.glb`, `pk-cc203.glb`, `pk-cc201.glb` | R2 (not in this checkout) |
| loco bogies | `pkBogieCc206`, `pkBogieGeU18c` | `pk-bogie-cc206.glb`, `pk-bogie-ge-u18c.glb` | R2 |
| coupling | `nryKoplingK3V4` | `nry-kopling-k3v4.glb` | local |
| K1/M1/P (armada lottery) | `nryNeK118Badan, nryK1TaksakaBadan, nryK1PecutMerahBadan, nryK1PecutPinkBadan, nryK1EksekutifBadan, nryK11605Badan, nryM1BasicBadan, nryM1TaksakaBadan, nryM1PecutMerahBadan, nryM1PecutPinkBadan, nryNeM118Badan, nryM19602PutihBadan, nryM1606Badan, nryP24BasicBadan, nryP23TaksakaBadan, nryP23PecutMerahBadan, nryP23PecutPinkBadan, nryNeP18Badan, nryP9601ParahyanganBadan, nryP1604Badan` | `nry-*-badan.glb` | local |
| K3 (commuter) | `nryK3V4LpBadan`, `nryKp3V2CommuterBadan` | `nry-k3-v4-commuter-badan.glb`, `nry-kp3-v2-commuter-badan.glb` | local |
| passenger bogie | `nryBogieK5tLp` | `nry-bogie-k5t-lp-v2.glb` | local |
| freight gd/gk | `nryGerbongDatarBadan`, `nryGk306348Badan` + `nryBogieBarberLp` | `nry-gerbong-datar-badan.glb`, `nry-gk-306348-badan.glb`, `nry-bogie-barber-lp-v2.glb` | local |
| trees | catalog `kategori:'vegetasi'` (`pohon-05…09`, pilot) | `pilot-pohon-0x.glb` | pilot |

Pemalang additionally needs `nry-stasiun-pemalang-v6.glb`; bks needs `nry-stasiun-bekasi-v6.glb`,
`nry-stasiun-bekasi-timur-v4.glb`, `nry-stasiun-tambun-v3.glb` and the JR205 set
(`nry-jr205-{kuha-v2,moha,mp}-badan.glb`, `nry-bogie-jr205-*.glb`, `nry-kopling-jr205-*.glb`).

### 10.3 Minimum loader contract for the C++ engine
1. Load GLB (KTX2 + Draco optional), compute bbox; if `sz.z > sz.x` rotate +90° about Y.
2. Recentre X/Z on the bbox centre; drop bottom to y = 0 unless an `a.bog<n>` node exists.
3. `panjang` per §7.3; scale bodies by `CarDef.length / panjang`.
4. Clone bogie/coupling GLBs into `a.bog<n>` / `a.kopling<n>` nodes (odd couplings rotated π).
5. Hiasan objects: no normalisation beyond steps 1–2; place per §5.4.
