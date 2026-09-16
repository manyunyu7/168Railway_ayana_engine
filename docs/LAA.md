# LAA — overhead catenary (Listrik Aliran Atas) plan

Status 2026-09-17: **design only, nothing built yet.** Written after the discussion with Henry on 2026-09-16.

## 1. What exists today

- No track-following catenary anywhere in the engine or in the three.js reference world.
- `engine/world/krl_station.cpp:367-379` draws station-local wires only: contact 5.0 m + messenger 6.2 m per
  platform track, portal gantries every 48 m, straight along local +X, no sag, no stagger. `KrlStation` is not
  wired into `game.cpp` (see `docs/PARITY.md`).
- Save data already carries a per-segment flag `tanpaLaa: true` ("no wire on this segment",
  `docs/world-spec.md:99`). `bks.json` has 81 such segments (~5.8 km), all around Bekasi station = the KAJJ
  tracks 1–3 and stabling/siding tracks. **Not parsed by C++ yet** — `TrackSegment` has no field for it.
- The reference catalog (`ppka-wannabe-2/public/model3d/model.json`) has pilot `laa` GLBs
  (`bn-laa-*`, `bn-la-*`, garis class `bn-laa-kembar-new-spline`), all private/gitignored and unused by any save.
- Pantograph clips on KRL stock are frozen raised (`rolling_stock.cpp:131`), so the wire is expected.

## 2. Real Indonesian specification

Source: **Permenhub PM 50 Tahun 2018**, Persyaratan Teknis Instalasi Listrik Perkeretaapian, lampiran §3.1
(DC overhead). PDF: https://peraturan.bpk.go.id/Download/93240/PM_50_TAHUN_2018.pdf. Daop 1 practice from
KAI/KCI reporting.

| Item | Value |
|---|---|
| System | 1500 V DC, simple catenary (messenger + contact), Japanese (JICA) lineage |
| Contact wire above rail head | nominal **5.30 m**, min 4.30, max 5.70; gradient ≤ 5‰ main line, ≤ 15‰ sidings |
| Stagger (deviasi) at supports | ±200 mm straight, ±300 mm curves (alternating each pole) |
| Messenger | steel stranded ≥ 90 mm², on insulators; mid-span sag > 15 cm relative to contact wire |
| Hangers (penggantung) | every ≤ 5 m, length ≥ 15 cm, symmetric about each pole |
| Pole (tiang) | concrete, round Ø350 mm (Ø400 at anchor poles), 12 m long, buried 1.9–2.4 m → ~9.6–10 m visible |
| Pole edge from track axis | normal **3.00 m**, min 2.75 m (pole centre ≈ 3.2 m) |
| Pole spacing | 50 m typical, **max 60 m**; by curve radius: R>1200→60, >1050→55, >850→50, >700→45, >550→40, >400→35, >300→30, >200→25 m |
| Supports on plain line | steel-tube cantilever on the pole, 2 insulators, ≥ 40 cm above messenger |
| Supports at stations | **V-truss portal beam** (batang penyangga) across the tracks, poles at both outer edges |
| Feeder (kawat penyulang) | copper, hung on the pole head between pole and track; ≥ 30 cm from structure, ≥ 120 cm from buildings |
| Ground wire (OHGW) | topmost, 45° protection angle, earthed every ≤ 250 m; arrester every ≤ 500 m |
| Tension lengths | ≤ 300 m spring ATD one end; 300–600 both ends; 600–800 pulley one end; > 800 pulley (counterweight) both ends |
| Overlap / air section | at the entry turnout of a station, ≥ 250 m behind a signal, between substations; poles ≥ 50 m apart (air section) / ≥ 40 m (air joint); wires 30 cm apart horizontally |
| Section insulators (FRP) | on sidings and crossings |
| Elevated line (DDT Jatinegara–Cakung viaduct) | steel masts bolted on the deck edge instead of concrete poles |
| Tunnels | no poles; contact wire hung from roof brackets at 4.3–4.6 m |

Consequence for the existing station: change `krl_station.h` `LAA_WIRE` 5.0 → 5.3 m for consistency.

## 3. Station track counts the design must survive

| Station | Tracks | Notes |
|---|---|---|
| Bekasi (BKS) | 8, 4 island platforms | 1–3 KAJJ (no wire, `tanpaLaa`), 4 shared, 5–7 KRL, 8 stabling |
| Bekasi Timur (BKST) | 2, 1 island | plain double track; DDT Bekasi–Cikarang planned 2027–2029 |
| Tambun (TB) | 4, 2 islands | tracks 2 & 4 are the through lines |
| Jatinegara (JNG) | 8, 4 islands | not in our data yet |
| Jakarta Kota (JAKK) | 11 (was 12), 5 islands + 2 side | terminus; not in our data yet |

Nothing may be hard-coded per station. Track groups, portal widths and wire presence are derived from the
graph geometry and the `tanpaLaa` flag, so Jatinegara / Jakarta Kota work the day their maps exist.

## 4. Design

### 4.1 Track groups from geometry
`rail_builder.cpp` already samples every segment into 4 m rings and finds lateral neighbours
(`neighbours()`, `rail_builder.cpp:566-585`). At each support chainage the rings that are ≤ ~9 m apart are
chained into one **track group**; a group is the unit a portal spans.

| Group width | Support |
|---|---|
| 1 track | concrete pole one side, cantilever |
| 2 tracks | poles on the outer sides (alternating sides is fine), or a twin cantilever on one pole where spacing allows |
| ≥ 3 tracks | V-truss portal from outer pole to outer pole |
| portal span > ~30 m | split into several portals with intermediate poles standing on island platforms (as at Manggarai / Jakarta Kota) |

### 4.2 Per-track wire
- Contact wire at 5.3 m above `RailProfile::railHeight`, stagger ±200 mm alternating per support (±300 in curves).
- Messenger at support ≈ contact + 1.0 m, parabolic sag ~0.5–0.6 m at mid-span for a 50 m span (never closer
  than 15 cm to the contact wire); hangers every 5 m.
- Segments with `tanpaLaa` get **no wires** but are still crossed by the portal, so Bekasi tracks 1–3 come
  out right from the data as it is.
- Support spacing from the PM 50 radius table, radius estimated from the segment LUT tangents, so curves
  (Kranji, Jatinegara) tighten automatically; 50 m on straights.

### 4.3 Where the catenary meets structures
- **Bridges / viaducts:** steel mast on the deck edge (parapet) instead of a concrete pole in the ground.
- **Tunnels:** no poles; contact wire lowered to 4.3–4.6 m, hung from the arch.
- **Level crossings (JPL):** no change in wire, but never place a pole on the road.
- **Station platforms:** poles/portal legs may stand on island platforms but never on the platform edge strip.

Later details (not in the first pass): overlap air sections at station entry turnouts, tension-length anchors
with counterweights, feeder/OHGW wires, section insulators, insulators as small instanced models.

## 5. Module split: rail, structures and LAA

Today `rail_builder.cpp` (1080 lines) does ballast, rails, sleepers, turnouts, bridges, tunnels and trusses.
Adding LAA there would push it past 2000 lines. Decision: **separate modules sharing one corridor model.**

1. `TrackCorridor` — per map, once: 4 m rings per segment, lateral neighbours, track groups, rail height,
   structure spans (bridge / tunnel / station portal), curvature. Extracted from the current `rail_builder`.
2. `RailBuilder` — ballast, rails, sleepers, turnouts. Reads the corridor.
3. `StructureBuilder` — decks, piers, trusses, tunnels, portals. Reads the corridor. Split out of `rail_builder`
   without changing its output.
4. `CatenaryBuilder` — poles, cantilevers, portals, wires. Reads the corridor **and** the structure result
   (mast on deck, wire on tunnel roof).

Build order: corridor → rails → structures → LAA. The split can be done incrementally: corridor first, LAA as
a new module reading it, bridge/tunnel extraction afterwards.

### Rendering budget
- Poles, cantilevers, insulators, portal beams: small prototype models instanced per 640 m chunk exactly like
  the meshed sleepers (`RailChunk::sleepers`, `drawInstanced`), distance-gated.
- Wires: thin ribbon mesh per chunk near the camera, plain line segments beyond a few hundred metres.
  This keeps 11 tracks at Jakarta Kota cheap on Android.

## 6. Milestones

1. Parse `tanpaLaa` into `TrackSegment`; extract `TrackCorridor` from `rail_builder` (no visual change; ctest green).
2. `CatenaryBuilder` on plain double track: poles + cantilevers + contact/messenger with stagger and sag,
   spacing from the radius table. Verify on Bekasi Timur–Tambun.
3. Track groups + V-truss portals for ≥ 3 tracks, split portals with platform poles. Verify at Bekasi (tracks 1–3
   must be bare). Set `krl_station` wire to 5.3 m.
4. Structures: masts on bridge decks, roof-hung wire in tunnels (Maswati–Cilame map for tunnels).
5. Extract `StructureBuilder` from `rail_builder`.
6. Details: overlap sections at station entries, anchors with counterweights, feeder + ground wire, insulators.

Sources: PM 50/2018 (link above); id.wikipedia.org Stasiun Bekasi / Bekasi Timur / Tambun / Jatinegara /
Jakarta Kota; detik.com "513,9 km LAA KRL Jabodetabek dirawat" (2026); CNBC Indonesia "Jalur ganda
Bekasi–Cikarang mulai 2027, tuntas 2029" (2026-05-21).
