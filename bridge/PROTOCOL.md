# sim-bridge protocol

`bridge/sim-bridge.ts` runs the PPKA Simulator TypeScript engine (`../ppka-wannabe-2/src/engine`)
headless and exposes it over **JSON lines**: one JSON object per line on stdin (command), one JSON
object per line on stdout (response), in order. Diagnostics go to stderr only.

```bash
cd ../ppka-wannabe-2 && npx tsx ../game-engine-experiment/bridge/sim-bridge.ts
# or from anywhere:  PPKA_ROOT=/path/to/ppka-wannabe-2 npx tsx bridge/sim-bridge.ts
```

The PPKA root is found from `$PPKA_ROOT`, else the cwd, else `../ppka-wannabe-2` next to this repo.
`bridge/package.json` (`"type":"module"`) is required so tsx treats the script as ESM.

On startup the bridge prints `{"ready":true,"ppka":"<root>","pid":N}`. Every response has `ok`
(`true`/`false`); failures carry `error` (exception) or `reason` (a game-rule rejection, in the
engine's own Indonesian wording so it matches the UI). If a command carries `seq`, it is echoed.
Units: metres, seconds, m/s, radians. Clock = seconds since 00:00 (sim time). Coordinates are the
save's world coordinates (Web Mercator metres when `geo` is true, so ~1.2e7 — keep them in doubles).
Numbers are rounded to 3 decimals (coordinates/metres) or 2 (speeds/times).

## Commands

### `{"cmd":"load","map":"kroya"}`
`map` = name of `src/data/<map>.json` or an absolute path. Optional `step` (m, default 4) =
polyline sampling step. Response:

```
{ ok, map, file, session:{name,startClock,entries},
  world: <raw save "world" object, verbatim>,
  summary: {
    geo: bool,
    nodes:    [{id,x,y}],
    segments: [{id,a,b,len,jenis:null|"jembatan"|"terowongan", poly:[x0,y0,x1,y1,...]}],
    points:   [{id,x,y,facing:<segId>,legs:[<normal segId>,<reverse segId>],setting:0|1,spring:null|0|1}],
    signals:  [{id,kind:"signal",name,seg,s,dir:1|-1,x,y,tx,ty,sisi:"kiri"|"kanan",
                signalType:"interlocking"|"auto"|"muka"|"pengulang"|"bersama",
                lampu:2|3,bentuk:"elektrik"|"mekanik",lengan,papanAngka}],
    trackmarks:[{id,kind:"trackmark",name,seg,s,dir,x,y,tx,ty,sisi,berarah,sepur,linkedSignal}],
    portals:  [{id,kind:"portal",name,seg,s,dir,x,y,tx,ty}],
    other:    [... same shape, kind = dirmarker|speed|stopmark|...],
    scenery:  [{id,kind,code,label,x,y,rot,w,h}],      (kind "station" = station building)
    stations: [{id,code,label,x,y}]
  } }
```
`poly` is the arc-length sampled centreline (already includes the end node), so the C++ side needs no
Bezier code. `(tx,ty)` is the unit tangent in the direction the object faces (already multiplied by
`dir`). Points: `setting 0` = `legs[0]` (normal/straight), `1` = `legs[1]` (reverse). This response
is large (kroya: ~670 KB) and sent once.

### `{"cmd":"start","ai":true,"clock":"05:00"}`
Creates a session from the save's GAPEKA. `clock` `"HH:MM[:SS]"` or seconds; omitted = save
default. `ai:false` disables the AI PPKA (nobody sets routes unless you do). Optional
`duration` (s, `durasiDinas`), `warm:false` disables warm start (trains already on the line at the
start clock are otherwise spawned in place). Response `{ok, clock, ai, trains, pending}`.
`load` again or `start` again both reset the session (fresh interlocking).

### `{"cmd":"step","dt":0.1}`
Advances `dt` sim seconds (the engine substeps at <= 0.5 s; AI PPKA runs every 3 s of sim time).
Response = the **dynamic state**:

```
{ ok, clock, score, violations, pending,
  trains: [{
    id, no, name, consist, kelas (0 KLB .. higher = more important), state ("run"|"dwell"|"done"|...),
    kendali:"ai"|"manual", speed, maxSpeed, len,
    seg, s, dir,             front of train: segment id, metres from node a, travel direction
    x, y, heading,           front of train in world XY, heading = atan2 of travel tangent
    hold, holdSignal, delay (s), nextStop (trackmark name),
    vehicles: [{model, sarana, kind:"loco"|"car", len, x, y, heading, seg, s,
                x1, y1, x2, y2, seg2, s2}]                                        front to rear
  }],
  points:  [{id, setting:0|1, locked:<routeId>|null}],
  signals: [{id, aspect:"red"|"yellow"|"green"}],
  occupancy: [{seg, iv:[[trainId, a, b], ...]}],          only segments with an interval
  routes:  [{id, entry, exit, exitLabel, segs, released, sepurSalah, izinTerisi}],
  log:     [{t, kind:"info"|"good"|"bad", text}]           only lines NEW since the previous step/state
}
```
Vehicle `x,y` is the midpoint between its two couplers (both projected on the track, so consists bend on
curves); `heading` is front-minus-rear. `(x1,y1)` / `seg,s` is the vehicle's front coupler and `(x2,y2)` / `seg2,s2`
its rear coupler, so the 3D layer can place each car exactly between its two couplers (§7.4). `model` is the 3D model id resolved the same way the web 3D layer
does it (`src/tiga/armada.ts`: fleet by train number / per-slot override), falling back to the `sarana`
catalogue id. `{"cmd":"state"}` returns the same object without advancing.

### `{"cmd":"click_signal","id":"t9506"}`
Mirrors a signal click in the 3D view / expert mode (`main.ts klikSinyal` → `ui/rute.ts tarikAhli`):
`id` may be a trackside id or a signal name (`"SKP MT"`); shared exit-signal masks are resolved
along the current point settings.
- Signal already has a cancellable route → the route is cancelled: `{ok, action:"cancel", route}`.
- Not an interlocking signal → `{ok:false, reason:"bukan sinyal interlocking"}`.
- Path traced along the current point settings (`traceByPoints`): no continuous path →
  `{ok:false, action:"set", reason, path, wesel, dm}` (e.g. `"wesel belum mengarah ke jalur ini"`,
  `wesel` = the point that is set the wrong way).
- Otherwise the same gate as the UI: `{ok:false, reason:"sudah berute", ...}`,
  `{ok:false, reason:<TolakRute.alasan>, tolak:{alasan,segs,wesel,sinyal,ruteLawan,ka}, semua:[...]}`
  (`semua` = every obstruction at once), or `{ok:true, status:"set", route}` /
  `{ok:true, status:"minta"}` (single-track block: request sent, route forms when the neighbour
  answers "aman"). Optional `sepurSalah:true` = force the wrong-line path the player can confirm.

### `{"cmd":"set_route","from":"SKP MT","to":"SKP K2B"}`
Beginner mode: choose among `findRoutes` candidates. `to` matches `exitLabel`, exit signal id or name;
or pass `index`. Unknown target → `{ok:false, reason:"tak ada kandidat", candidates:[...]}`.
Optional `izinTerisi:true` (calling-on: ignore occupancy only), `sepurSalah:true`.
Success/rejection shapes are the same as `click_signal`.

### `{"cmd":"routes","id":"SKP MT"}`
Lists candidates with `blocked` = `whyBlocked` result or null.

### `{"cmd":"preview","signal":"SKP MT"}`
Hover preview: the path `click_signal` would lock along the *current* point settings
(`Interlocking.traceByPoints`), without changing anything. `{ok, signal, name, manual, active,
path:[segId...], cand:{exitLabel, exitSignal, segs}|null, reason, wesel, dm}`. `cand` null = the
trace dead-ends (`path` stops at the offending point `wesel`, `reason` in the engine's wording);
`active` = id of the route already set from this signal (or null); `manual` false = not an
interlocking/bersama signal (cannot be operated by hand).

### `{"cmd":"cancel_route","signal":"SKP MT"}` (or `"route":"r123"`)
`{ok:false, reason:"sedang dilalui", ka}` when a train is already inside.

### `{"cmd":"flip_point","id":"n111"}`
`World.balikWesel` — the single human entry for point flips. `{ok:true, setting, leg}` or
`{ok:false, reason:"terkunci"|"terinjak"|"bukan wesel", ka, setting, locked}`.

## Player commands (mirror the web UI paths)

### `{"cmd":"set_time_scale","k":8}`
Sets `session.timeScale`. **The bridge is authoritative for time**: `step` takes REAL seconds and the
session multiplies by its own scale (web game loop). `start` accepts `timeScale` too. `{ok, timeScale}`.

### `{"cmd":"set_clock","clock":"07:30"}`
The web "Set jam" (`ui/waktu.ts restartSessionAt`) does not fast-forward: it rebuilds the session from
the GAPEKA at the new start clock. Same here — equivalent to `start` again with the previous options
(`ai`, `s40`, `warm`) and the current time scale. **Caveat:** live trains vanish and re-spawn (warm start),
routes/points/score reset, the log restarts. `{ok, ..., restarted:true}`.

### `{"cmd":"beri_s40","train":"89"}` (train id or number)
Semboyan 40 (`ui/s40.ts berikanS40`) without the 40→41→35 audio ritual: sets `s40Diberi` at once.
The gate itself (`tickS40`, dwell branch only — no BLB detection) runs inside `step` when `s40` is on:
`start {ai:false}` turns it on (the player holds every station), `start {s40:true|false}` overrides.
Trains then report `tungguS40` (held) and `s40Siap` (exit signal clear → button may be offered).
`{ok}` or `{ok:false, reason:"tidak menunggu S40"|"sinyal keluar belum aman"}`.

### `{"cmd":"hapus_ka","train":"89"}`
`Session.hapusKA`: force-removes the train, −2000 (`PENALTI_HAPUS`), session marked impure.
`{ok, train, no, penalty, score}`.

### `{"cmd":"train_detail","train":"89"}`
Train sheet payload (`ui/panelKereta.ts`): `{ok, id, no, name, consist, cars:[{kind,sarana,len}], kelas,
state, kendali, speed, limit, maxSpeed, delay (s, telatKini), hold, holdDist, holdSignal,
nextStop:{trackmark,arr,dep}|null, spawnPortal, spawnTime, exitPortal, exitTime, exitBerth,
stops:[{trackmark,arr,dep,actArr,actDep}], jadwal:[{cls:"done"|"now"|"next", ls, name, plan, actual, late, meets}]
(ui/jadwal.ts susunJadwal, en-dash replaced by "-"), lookahead:[{d,v,why}] (first 5),
tungguS40, s40Siap, s40Diberi, log:[{t,kind,text}] (lines naming this train)}`.

### `{"cmd":"route_menu","signal":"BKS MB1"}`
Beginner-mode menu (`ui/rute.ts kandidatMenu`): `findRoutes` (wrong-line branches included) deduplicated
per destination (straightest wins, `dedupPerTujuan`), entry signals sorted by track number, wrong-line
entries last. `{ok, signal, name, manual, active, activeLabel, cancellable,
candidates:[{index, exitLabel, exitSignal, exitName, dist, segs, sepurSalah, dm, blocked:<tolak>|null, blockedText}],
prunes:[{reason,n}]}` (`prunes` = why the search found nothing). Choose with `set_route {from, index}`
(pass `sepurSalah:true` for a wrong-line entry — the web asks for confirmation first). Route mode
(pemula/ahli) itself is client-side; the engine has no notion of it.

### `{"cmd":"panel"}`
The schematic control table (`engine/panel.ts PanelLayout`, what the web draws in panel mode and on the
3D floating table). Static per loaded world; panel units, y drawn scaled by `yScale` (3):

```
{ ok, key, yScale, bbox:{x0,y0,x1,y1},
  segments:[{id,a,b,len, pts:[x0,y0,x1,y1,...], cum:[m at each vertex], sepur:"lurus"|"belok"|null, jalur:n|null}],
  nodes:[{id,x,y,deg}], points:[{id,x,y,facing,legs:[normal,reverse]}],
  signals:[{id,kind,name,seg,s,dir,x,y,tx,ty,signalType,lampu,bentuk,station}],
  berths:[{... trackmarks, sepur, jalur}], portals:[{...}],
  stations:[{code,label,x0,y0,x1,y1,signals}],     bbox of the station's interlocking signals (renderer.ts ensureAreaStasiun)
  jalur:[{station,n,x,y}],                         "JALUR n" pill positions (drawPanelJalur)
  scenery:[{id,kind,code,label,x,y}] }
```
`segments[].pts/cum` is the exact schematic polyline, so per-step state (occupied intervals, locked route
segments, vehicles from `seg,s`/`seg2,s2`) is drawn by interpolating along it — no extra per-step panel
state is needed. Segment sepur/jalur come from the renderer's lane classification (`ensureSepur`).

### `{"cmd":"ping"}`, `{"cmd":"quit"}`

## C++ side
`eng::SimProcess` (`engine/sim/sim_process.h`) spawns the bridge with fork/exec (cwd = PPKA root,
`npx tsx`), blocks on one line per command, parses with `eng::Json` and fills `SimState`
(`SimTrain{... vehicles[{model,sarana,kind,length,x,y,heading,seg,s,x1,y1,x2,y2,seg2,s2}]}`, `SimPoint{id,setting,lockedBy}`, `SimSignal{id,aspect}`, `SimRoute{id,entry,exit,exitLabel,segs,released}`,
`SimOccupancy{seg, intervals[{train,a,b}]}`, `SimTrain.tungguS40/s40Siap`, new log lines); `preview(signal)` wraps the `preview` command.
Player commands: `setTimeScale(k)`, `setClock("HH:MM")`, `beriS40(train)`, `hapusKA(train)`, `trainDetail(train)`,
`routeMenu(signal)` (raw `Json`), and `panel()` → typed `PanelLayout` (segments/points/signals/berths/portals/
stations/jalur, `posOnSeg(seg, s, x, y, tx, ty)` interpolating the schematic polyline; cached after the first call). Raw responses stay available in `lastResponse()`; `world()`/`summary()` keep the load
result. `examples/simtest` exercises everything and prints latency/size statistics.
