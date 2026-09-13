// sim-bridge — drives the PPKA Simulator TypeScript engine as a child process.
//
// Protocol: one JSON object per line on stdin (command), one JSON object per
// line on stdout (response). Anything diagnostic goes to stderr only. See
// bridge/PROTOCOL.md for the full format.
//
// Run:  cd <ppka-wannabe-2> && npx tsx <engine>/bridge/sim-bridge.ts
// The PPKA repo root is resolved from the PPKA_ROOT env var, else from the
// current working directory (must contain src/engine/), else ../ppka-wannabe-2
// relative to this file. The engine modules are imported dynamically from
// that root so this script can live outside the PPKA repo.

import fs from 'node:fs';
import path from 'node:path';
import readline from 'node:readline';
import { fileURLToPath, pathToFileURL } from 'node:url';

// ---- locate the PPKA repo ----
const here = path.dirname(fileURLToPath(import.meta.url));
function findPpkaRoot(): string {
  const cands = [process.env.PPKA_ROOT, process.cwd(), path.resolve(here, '../../ppka-wannabe-2')];
  for (const c of cands) {
    if (c && fs.existsSync(path.join(c, 'src/engine/world.ts'))) return path.resolve(c);
  }
  throw new Error('PPKA repo not found (set PPKA_ROOT or run with cwd = ppka-wannabe-2)');
}
const PPKA = findPpkaRoot();
const mod = (rel: string) => import(pathToFileURL(path.join(PPKA, rel)).href);

// Dynamic imports: types are `any` on purpose — the bridge must not break on
// every internal TS refactor, only on the handful of entry points it uses.
const { World } = await mod('src/engine/world.ts');
const { Interlocking } = await mod('src/engine/interlocking.ts');
const { Session } = await mod('src/engine/session.ts');
const { AiPPKA } = await mod('src/engine/aippka.ts');
const { kelasKA } = await mod('src/engine/kelasKA.ts');
const { grupDariId, resolveBersama } = await mod('src/engine/sinyalBersama.ts');
const { GAP, CONSIST_LIBRARY } = await mod('src/engine/train.ts');
const { PanelLayout } = await mod('src/engine/panel.ts');
const { dedupPerTujuan } = await mod('src/engine/aippka.ts');
const { isSinyalMasuk, nomorJalurSinyal, stasiunSinyal } = await mod('src/engine/trackside.ts');
const { stasiunWajibS40 } = await mod('src/engine/scenery.ts');
const { susunJadwal } = await mod('src/ui/jadwal.ts');   // pure (imports engine/train only)
// armada.ts has no three.js imports; it maps (trainNo, sarana) -> 3D model id.
let armadaUntuk: ((no: string, paksa?: string) => any) | null = null;
let modelArmada: ((a: any, sarana: string) => string | undefined) | null = null;
try {
  const a = await mod('src/tiga/armada.ts');
  armadaUntuk = a.armadaUntuk; modelArmada = a.modelArmada;
} catch (e) { process.stderr.write(`sim-bridge: armada.ts unavailable (${e})\n`); }

// ---- single session state ----
let world: any = null;
let ixl: any = null;
let session: any = null;
let ai: any = null;
let mapName = '';
let logSeen = 0;
let s40Manual = false;  // bridge-side Semboyan 40 gate (ui/s40.ts tickS40, branch A)
let startOpts: any = {};   // last `start` options, re-used by set_clock
let panelCache: any = null;   // PanelLayout for the loaded world        // how many session.log entries were already emitted
const r3 = (v: number) => Math.round(v * 1000) / 1000;
const r2 = (v: number) => Math.round(v * 100) / 100;

function parseClock(s: string | number | undefined): number | undefined {
  if (s === undefined || s === null) return undefined;
  if (typeof s === 'number') return s;
  const m = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(s);
  if (!m) throw new Error(`bad clock "${s}" (want HH:MM[:SS])`);
  return +m[1] * 3600 + +m[2] * 60 + (m[3] ? +m[3] : 0);
}

// ---- static world summary (what a renderer needs, geometry pre-sampled) ----
function samplePoly(segId: string, stepM: number): number[] {
  const pts: number[] = [];
  const L = world.graph.length(segId);
  for (let s = 0; s < L; s += stepM) { const p = world.graph.pointAt(segId, s); pts.push(r3(p.x), r3(p.y)); }
  const e = world.graph.pointAt(segId, L); pts.push(r3(e.x), r3(e.y));
  return pts;
}

function summary(stepM: number) {
  const nodes = [...world.graph.nodes.values()].map((n: any) => ({ id: n.id, x: r3(n.pos.x), y: r3(n.pos.y) }));
  const segments = [...world.graph.segments.values()].map((sg: any) => ({
    id: sg.id, a: sg.a, b: sg.b, len: r3(world.graph.length(sg.id)),
    jenis: sg.jenisRel ?? null, poly: samplePoly(sg.id, stepM),
  }));
  const points = [...world.graph.nodes.values()].filter((n: any) => n.junction).map((n: any) => ({
    id: n.id, x: r3(n.pos.x), y: r3(n.pos.y),
    facing: n.junction.facingSeg, legs: n.junction.legs, setting: n.junction.setting,
    spring: n.junction.spring ?? null,
  }));
  const trackside = [...world.trackside.values()].map((o: any) => {
    const sm = world.graph.sampleAt(o.segId, o.s);
    const out: any = {
      id: o.id, kind: o.kind, name: o.name, seg: o.segId, s: r3(o.s), dir: o.dir,
      x: r3(sm.p.x), y: r3(sm.p.y), tx: r3(sm.tan.x * o.dir), ty: r3(sm.tan.y * o.dir),
      sisi: o.sisi ?? 'kanan',
    };
    if (o.kind === 'signal') {
      out.signalType = o.signalType ?? 'interlocking'; out.lampu = o.lampu ?? 3;
      out.bentuk = o.bentuk ?? 'elektrik'; out.lengan = o.lengan ?? null; out.papanAngka = o.papanAngka ?? 3;
    }
    if (o.kind === 'trackmark') { out.berarah = !!o.berarah; out.sepur = o.sepur ?? null; out.linkedSignal = o.linkedSignal ?? null; }
    if (o.kind === 'speed') out.speed = o.speed ?? null;
    return out;
  });
  const scenery = [...world.scenery.values()].map((s: any) => ({
    id: s.id, kind: s.kind, code: s.code ?? null, label: s.label ?? null,
    x: r3(s.pos.x), y: r3(s.pos.y), rot: s.rot ?? 0, w: s.w, h: s.h,
  }));
  return {
    geo: !!world.geo, nodes, segments, points,
    signals: trackside.filter((o: any) => o.kind === 'signal'),
    trackmarks: trackside.filter((o: any) => o.kind === 'trackmark'),
    portals: trackside.filter((o: any) => o.kind === 'portal'),
    other: trackside.filter((o: any) => !['signal', 'trackmark', 'portal'].includes(o.kind)),
    scenery,
    stations: world.stations().map((s: any) => ({ id: s.id, code: s.code ?? null, label: s.label ?? null, x: r3(s.pos.x), y: r3(s.pos.y) })),
  };
}

// ---- dynamic state ----
function modelFor(t: any, car: any): string {
  if (armadaUntuk && modelArmada && car.sarana) {
    try { const m = modelArmada(armadaUntuk(t.trainNo, car.armada), car.sarana); if (m) return m; } catch { /* fall through */ }
  }
  return car.sarana ?? (car.kind === 'loco' ? 'loko' : 'kereta');
}

function trainState(t: any) {
  const cars = t.consist?.cars ?? [];
  const vehicles: any[] = [];
  let d = 0;
  for (const car of cars) {
    const a = t.pointBehind(world, d);
    const b = t.pointBehind(world, d + car.length);
    d += car.length + GAP;
    if (!a || !b) continue;
    const dx = a.p.x - b.p.x, dy = a.p.y - b.p.y;
    vehicles.push({
      model: modelFor(t, car), sarana: car.sarana ?? null, kind: car.kind, len: r2(car.length),
      x: r3((a.p.x + b.p.x) / 2), y: r3((a.p.y + b.p.y) / 2),
      heading: r3(Math.atan2(dy, dx)),            // radians, direction of travel (front minus rear)
      seg: a.segId, s: r3(a.s),                   // front coupler of this vehicle on the track
      x1: r3(a.p.x), y1: r3(a.p.y),               // front coupler in world XY
      x2: r3(b.p.x), y2: r3(b.p.y),               // rear coupler in world XY
      seg2: b.segId, s2: r3(b.s),                 // rear coupler on the track
      t1: r3(Math.atan2(a.tan.y, a.tan.x)),        // track tangent angle at each coupler (sway curvature, §7.5)
      t2: r3(Math.atan2(b.tan.y, b.tan.x)),
    });
  }
  const sm = world.graph.sampleAt(t.front.segId, t.front.s);
  const stop = t.nextStop?.() ?? null;
  return {
    id: t.id, no: t.trainNo, name: t.entry?.name ?? '', consist: t.consist?.name ?? '',
    kelas: kelasKA(t.trainNo), state: t.state, kendali: t.kendali,
    speed: r2(t.speed), maxSpeed: r2(t.maxSpeed), len: r2(t.totalLength),
    seg: t.front.segId, s: r3(t.front.s), dir: t.front.dir,
    x: r3(sm.p.x), y: r3(sm.p.y), heading: r3(Math.atan2(sm.tan.y * t.front.dir, sm.tan.x * t.front.dir)),
    hold: t.holdReason || null, holdSignal: t.holdSignalId || null,
    delay: Math.round(t.telatKini?.(session.clock) ?? t.delaySec ?? 0),
    nextStop: stop ? stop.trackmark : null,
    tungguS40: !!t.tungguS40, s40Siap: !!t.s40Siap, s40Diberi: !!t.s40Diberi,
    istirahat: !!t.istirahat?.(session.clock),   // parked consist (lights off, doors closed)
    vehicles,
  };
}

/** Semboyan 40 gate — the dwell branch of ui/s40.ts `tickS40` (BLB detection and the
 *  40→41→35 audio ritual are UI-side and not mirrored). Only when `s40Manual`: a train
 *  dwelling at a station flagged `semboyan40` holds until `beri_s40`. */
function tickS40() {
  for (const t of session.trains) {
    t.s40Siap = false;
    if (t.speed > 2) t.s40Diberi = false;
    if (t.manual || t.dinasLangsir || t.state !== 'dwell') { t.tungguS40 = false; continue; }
    const cmd = t.aktifCmd?.();
    const code = cmd?.kind === 'berhentiDi' ? session.announcer.kodeDari(cmd.trackmark) : null;
    const st = code ? world.stations().find((x: any) => x.code === code) : null;
    const listrik = !!CONSIST_LIBRARY[t.entry.consist]?.listrik;
    const butuh = !!st && stasiunWajibS40(st, listrik);
    t.tungguS40 = s40Manual && butuh && !t.s40Diberi;
    t.s40Siap = t.tungguS40 && ixl.sinyalDepanIzin(t.front);
  }
}

/** Level-crossing barriers: `World.jplClosed` (a train within 350 m on any track within 25 m of the
 *  crossing, along the current point settings) — the web 3D layer evaluates it every 0.25 s of sim
 *  time (bangun3d.ts perbaruiJPL), so the result is cached for that long. */
let jplAt = -1;
let jplCache: { id: string; closed: boolean }[] = [];
function jplState() {
  if (jplAt >= 0 && session.clock - jplAt < 0.25 && session.clock >= jplAt) return jplCache;
  jplAt = session.clock;
  jplCache = [...world.scenery.values()].filter((o: any) => o.kind === 'jpl')
    .map((o: any) => ({ id: o.id, closed: world.jplClosed(o) }));
  return jplCache;
}

function dynamicState() {
  ixl.beginAspectFrame();   // aspectOf caches per frame
  const signals = [...world.trackside.values()].filter((o: any) => o.kind === 'signal')
    .map((o: any) => ({ id: o.id, aspect: ixl.aspectOf(o.id) }));
  const points = [...world.graph.nodes.values()].filter((n: any) => n.junction)
    .map((n: any) => ({ id: n.id, setting: n.junction.setting, locked: n.junction.lockedBy ?? null }));
  const occupancy: any[] = [];
  for (const [segId, list] of world.occupancy.entries()) {
    if (list.length) occupancy.push({ seg: segId, iv: list.map((x: any) => [x.trainId, r2(x.a), r2(x.b)]) });
  }
  const routes = [...ixl.routes.values()].map((r: any) => ({
    id: r.id, entry: r.def.entrySignal, exit: r.def.exitSignal, exitLabel: r.def.exitLabel,
    segs: r.def.segs, released: [...r.released],
    sepurSalah: !!r.def.sepurSalah, izinTerisi: !!r.def.izinTerisi,
  }));
  // session.log is newest-first (unshift); emit only entries not seen before, oldest first
  const total = session.log.length;
  const fresh = session.log.slice(0, Math.max(0, total - logSeen)).reverse()
    .map((l: any) => ({ t: r2(l.time), kind: l.kind, text: l.text }));
  logSeen = total;
  return {
    clock: r2(session.clock), score: session.score, violations: session.violations,
    pending: session.pending.length,
    trains: session.trains.map(trainState),
    points, signals, occupancy, routes, jpl: jplState(), log: fresh,
  };
}

// ---- commands ----
function requireSession() { if (!session) throw new Error('no session: send "load" then "start" first'); }

function cmdLoad(c: any) {
  const name: string = c.map ?? 'kroya';
  const file = path.isAbsolute(name) ? name : path.join(PPKA, 'src/data', name.endsWith('.json') ? name : `${name}.json`);
  const data = JSON.parse(fs.readFileSync(file, 'utf8'));
  world = World.fromJSON(data.world ?? data);
  ixl = new Interlocking(world);
  session = null; ai = null; logSeen = 0; panelCache = null; jplAt = -1;
  mapName = path.basename(file, '.json');
  const def = data.session ?? null;
  (globalThis as any).__ppkaDef = def;
  return {
    ok: true, map: mapName, file,
    session: def ? { name: def.name, startClock: def.startClock, entries: def.entries.length } : null,
    world: data.world ?? data,
    summary: summary(typeof c.step === 'number' ? c.step : 4),
  };
}

function cmdStart(c: any) {
  if (!world) throw new Error('load a map first');
  const base = (globalThis as any).__ppkaDef;
  if (!base) throw new Error('save has no session (GAPEKA) block');
  const def = JSON.parse(JSON.stringify(base));   // Session mutates/holds the def
  const clk = parseClock(c.clock);
  if (clk !== undefined) def.startClock = clk;
  if (typeof c.duration === 'number') def.durasiDinas = c.duration;
  ixl = new Interlocking(world);                   // fresh interlocking (drops routes of a prior run)
  session = new Session(def, world, ixl, c.warm !== false);
  session.timeScale = 1;
  ai = c.ai === false ? null : new AiPPKA(world, ixl, session);
  // S40 gate: like the web client it only bites at stations the player holds; without
  // an AI PPKA the player holds every station. `s40:true|false` overrides.
  s40Manual = typeof c.s40 === 'boolean' ? c.s40 : c.ai === false;
  if (typeof c.timeScale === 'number') session.timeScale = c.timeScale;
  startOpts = { ...c };
  logSeen = 0;
  return { ok: true, clock: session.clock, ai: !!ai, s40: s40Manual, timeScale: session.timeScale,
    trains: session.trains.length, pending: session.pending.length };
}

/** ui/waktu.ts `restartSessionAt`: the web "Set jam" does NOT fast-forward — it rebuilds the
 *  session from the GAPEKA at the new start clock (live trains vanish, routes are dropped,
 *  trains scheduled earlier are treated as already gone, warm start spawns those on the line).
 *  Same here: `start` again with the previous options and the new clock. */
function cmdSetClock(c: any) {
  requireSession();
  const scale = session.timeScale;
  const r = cmdStart({ ...startOpts, clock: c.clock, timeScale: scale });
  return { ...r, restarted: true };
}

function cmdSetTimeScale(c: any) {
  requireSession();
  const k = Number(c.k ?? c.scale);
  if (!(k >= 0) || !Number.isFinite(k)) throw new Error(`bad time scale ${c.k}`);
  session.timeScale = k;
  return { ok: true, timeScale: k };
}

function findTrain(c: any) {
  const key = String(c.train ?? c.id ?? c.no ?? '');
  const t = session.trains.find((x: any) => x.id === key || x.trainNo === key);
  if (!t) throw new Error(`train not found: ${key}`);
  return t;
}

/** ui/s40.ts `berikanS40` without the audio ritual: the web holds the train ~2 s for the
 *  40→41→35 handshake (clock forced to 1×) and then sets `s40Diberi`; here it is immediate. */
function cmdBeriS40(c: any) {
  requireSession();
  const t = findTrain(c);
  if (!t.tungguS40) return { ok: false, train: t.id, no: t.trainNo, reason: 'tidak menunggu S40' };
  if (!t.s40Siap) return { ok: false, train: t.id, no: t.trainNo, reason: 'sinyal keluar belum aman' };
  t.s40Diberi = true; t.tungguS40 = false; t.s40Siap = false;
  session.addLog({ time: session.clock, kind: 'info', text: `🚩 S40 — KA ${t.trainNo} diberangkatkan` });
  return { ok: true, train: t.id, no: t.trainNo };
}

/** main.ts hooks.hapusKA → Session.hapusKA (penalty PENALTI_HAPUS = 2000). */
function cmdHapusKA(c: any) {
  requireSession();
  const t = findTrain(c);
  const no = t.trainNo;
  const ok = session.hapusKA(t.id);
  return { ok, train: t.id, no, penalty: ok ? 2000 : 0, score: session.score };
}

/** ui/panelKereta.ts train sheet (`rincianKA`): schedule vs realisation, delay, hold, consist. */
function cmdTrainDetail(c: any) {
  requireSession();
  const t = findTrain(c);
  const clock = session.clock;
  const jadwal = susunJadwal(t.entry, t.actualSpawn || null, t.actualStops, t.actualExit ?? null,
    session.lewatDari?.(t.id) ?? [], t.state === 'done' ? null : t.stopIndex,
    (code: string) => session.announcer.labelFor?.(code) ?? code)
    .map((it: any) => ({ cls: it.cls, ls: !!it.ls, name: it.nama, plan: String(it.ren).replace(/–/g, '-'), actual: String(it.real).replace(/–/g, '-'), late: !!it.merah,
      meets: (it.meets ?? []).map((m: any) => ({ ...m })) }));
  const stops = t.entry.stops.map((s: any, i: number) => ({
    trackmark: s.trackmark, arr: s.arr, dep: s.dep,
    actArr: t.actualStops[i]?.arr ?? null, actDep: t.actualStops[i]?.dep ?? null,
  }));
  const ik = t.holdReason ? t.lookahead.find((r: any) => r.why === t.holdReason) ?? null : null;
  const stop = t.nextStop?.() ?? null;
  const lim = t.currentLimit > 0 ? t.currentLimit : t.maxSpeed;
  return {
    ok: true, id: t.id, no: t.trainNo, name: t.entry.name ?? '', consist: t.consist?.name ?? '',
    cars: (t.consist?.cars ?? []).map((car: any) => ({ kind: car.kind, sarana: car.sarana ?? null, len: r2(car.length) })),
    kelas: kelasKA(t.trainNo), state: t.state, kendali: t.kendali,
    speed: r2(t.speed), limit: r2(lim), maxSpeed: r2(t.maxSpeed),
    delay: Math.round(t.telatKini?.(clock) ?? t.delaySec ?? 0),
    hold: t.holdReason || null, holdDist: ik ? r2(ik.d) : null, holdSignal: t.holdSignalId || null,
    nextStop: stop ? { trackmark: stop.trackmark, arr: stop.arr ?? null, dep: stop.dep ?? null } : null,
    spawnPortal: t.entry.spawnPortal ?? null, spawnTime: t.entry.spawnTime ?? null,
    exitPortal: t.entry.exitPortal ?? null, exitTime: t.entry.exitTime ?? null, exitBerth: t.entry.exitBerth ?? null,
    stops, jadwal,
    lookahead: t.lookahead.slice(0, 5).map((r: any) => ({ d: r2(r.d), v: r2(r.v), why: r.why })),
    tungguS40: !!t.tungguS40, s40Siap: !!t.s40Siap, s40Diberi: !!t.s40Diberi,
    log: session.log.filter((l: any) => l.text.includes(`KA ${t.trainNo}`)).slice(0, 20)
      .map((l: any) => ({ t: r2(l.time), kind: l.kind, text: l.text })),
  };
}

/** Beginner-mode menu (ui/rute.ts `kandidatMenu`): findRoutes deduplicated per destination
 *  (straightest wins), entry signals sorted by track number, wrong-line entries last; each
 *  with `blocked` = whyBlocked or null. */
function cmdRouteMenu(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.signal ?? c.id));
  const active = ixl.routeFromSignal(sig.id);
  const manual = sig.signalType === 'interlocking' || sig.signalType === 'bersama';
  const diag = { explored: new Set<string>(), prunes: [] as any[] };
  let cands: any[] = manual ? dedupPerTujuan(world, ixl.findRoutes(sig, diag, true)) : [];
  if (isSinyalMasuk(sig.name))
    cands.sort((a, b) => (nomorJalurSinyal(a.exitLabel) ?? 99) - (nomorJalurSinyal(b.exitLabel) ?? 99));
  cands.sort((a, b) => Number(!!a.sepurSalah) - Number(!!b.sepurSalah));
  const byReason = new Map<string, number>();
  for (const p of diag.prunes) byReason.set(p.reason, (byReason.get(p.reason) ?? 0) + 1);
  return {
    ok: true, signal: sig.id, name: sig.name, manual, active: active?.id ?? null,
    activeLabel: active?.def.exitLabel ?? null,
    cancellable: active ? !!ixl.dapatDibatalkan(active.id).bisa : false,
    candidates: cands.map((x: any, i: number) => {
      const tolak = ixl.whyBlocked(x);
      const exitName = x.exitSignal ? world.trackside.get(x.exitSignal)?.name ?? null : null;
      return { index: i, exitLabel: x.exitLabel, exitSignal: x.exitSignal ?? null, exitName, dist: r2(x.dist),
        segs: x.segs, sepurSalah: !!x.sepurSalah, dm: x.dm ?? null,
        blocked: tolak ? tolakToJson(tolak) : null,
        blockedText: tolak ? (tolak.ruteLawan ? `bentrok rute ${tolak.ruteLawan.entry} → ${tolak.ruteLawan.exit}`
          : tolak.alasan === 'blok terisi' ? `blok terisi ${trainLabel(tolak.ka)}` : tolak.alasan) : null };
    }),
    prunes: [...byReason.entries()].map(([reason, n]) => ({ reason, n })),
  };
}

function trainLabel(trainId?: string): string {
  const t = trainId ? session?.trains.find((x: any) => x.id === trainId) : null;
  return t ? `KA ${t.trainNo}` : 'kereta lain';
}

/** engine/panel.ts PanelLayout: the schematic control table ("meja layan"). Coordinates are
 *  panel units; the web draws them with y scaled ×3 (`Renderer.yScale`). Static per world. */
function cmdPanel() {
  if (!world) throw new Error('load a map first');
  if (!panelCache || panelCache.key !== PanelLayout.keyOf(world)) panelCache = PanelLayout.build(world);
  const lay = panelCache;
  const g = world.graph;
  // lane classification (render/renderer.ts ensureSepur): union through degree-2 nodes,
  // 'lurus'/'belok' from the platform trackmark flag, track number from exit signals "K<n>[BT]"
  const par = new Map<string, string>();
  for (const id of g.segments.keys()) par.set(id, id);
  const find = (x: string): string => { let r = x; while (par.get(r) !== r) r = par.get(r)!; while (par.get(x) !== r) { const nx = par.get(x)!; par.set(x, r); x = nx; } return r; };
  for (const n of g.nodes.values()) if (n.segs.length === 2) par.set(find(n.segs[0]), find(n.segs[1]));
  const laneSepur = new Map<string, string>(), laneJalur = new Map<string, number>();
  for (const o of world.trackside.values()) {
    if (!g.segments.has(o.segId)) continue;
    if (o.kind === 'trackmark' && o.sepur) laneSepur.set(find(o.segId), o.sepur);
    if (o.kind === 'signal') { const m = o.name.match(/ K(\d+)[BT]$/); if (m) laneJalur.set(find(o.segId), +m[1]); }
  }
  let bx0 = Infinity, by0 = Infinity, bx1 = -Infinity, by1 = -Infinity;
  const segments = [...g.segments.values()].map((sg: any) => {
    const poly = lay.segPolyline(sg.id);
    bx0 = Math.min(bx0, poly.bx0); by0 = Math.min(by0, poly.by0); bx1 = Math.max(bx1, poly.bx1); by1 = Math.max(by1, poly.by1);
    const pts: number[] = []; for (const p of poly.pts) pts.push(r2(p.x), r2(p.y));
    return { id: sg.id, a: sg.a, b: sg.b, len: r3(g.length(sg.id)), pts, cum: poly.cum.map(r2),
      sepur: laneSepur.get(find(sg.id)) ?? null, jalur: laneJalur.get(find(sg.id)) ?? null };
  });
  const nodes = [...g.nodes.values()].map((n: any) => { const p = lay.nodePanelPos(n.id); return { id: n.id, x: r2(p.x), y: r2(p.y), deg: n.segs.length }; });
  const points = [...g.nodes.values()].filter((n: any) => n.junction).map((n: any) => {
    const p = lay.nodePanelPos(n.id);
    return { id: n.id, x: r2(p.x), y: r2(p.y), facing: n.junction.facingSeg, legs: n.junction.legs };
  });
  const tsObj = (o: any) => {
    const r = lay.posOnSeg(o.segId, o.s);
    const out: any = { id: o.id, kind: o.kind, name: o.name, seg: o.segId, s: r3(o.s), dir: o.dir,
      x: r2(r.p.x), y: r2(r.p.y), tx: r3(r.tan.x * o.dir), ty: r3(r.tan.y * o.dir) };
    if (o.kind === 'signal') { out.signalType = o.signalType ?? 'interlocking'; out.lampu = o.lampu ?? 3; out.bentuk = o.bentuk ?? 'elektrik'; out.station = stasiunSinyal(o.name); }
    if (o.kind === 'trackmark') { out.sepur = o.sepur ?? null; out.jalur = laneJalur.get(find(o.segId)) ?? null; }
    return out;
  };
  const ts = [...world.trackside.values()].filter((o: any) => g.segments.has(o.segId));
  // station boxes (renderer.ts ensureAreaStasiun): bbox of a station's interlocking signals
  const grup = new Map<string, { x0: number; y0: number; x1: number; y1: number; n: number }>();
  for (const o of ts) {
    if (o.kind !== 'signal' || (o.signalType ?? 'interlocking') !== 'interlocking') continue;
    const code = stasiunSinyal(o.name); if (!code) continue;
    const p = lay.posOnSeg(o.segId, o.s).p;
    const a = grup.get(code);
    if (!a) grup.set(code, { x0: p.x, y0: p.y, x1: p.x, y1: p.y, n: 1 });
    else { a.x0 = Math.min(a.x0, p.x); a.x1 = Math.max(a.x1, p.x); a.y0 = Math.min(a.y0, p.y); a.y1 = Math.max(a.y1, p.y); a.n++; }
  }
  const stationLabel = new Map<string, string>();
  for (const s of world.stations()) if (s.code) stationLabel.set(s.code, s.label ?? s.code);
  const stations = [...grup.entries()].map(([code, a]) => ({ code, label: stationLabel.get(code) ?? code,
    x0: r2(a.x0), y0: r2(a.y0), x1: r2(a.x1), y1: r2(a.y1), signals: a.n }));
  // "JALUR n" labels (renderer.ts drawPanelJalur): midpoint between the K<n>T / K<n>B pair
  const byName = new Map<string, any>();
  for (const o of ts) if (o.kind === 'signal') byName.set(o.name, o);
  const jalur: any[] = [];
  for (const o of byName.values()) {
    const m = o.name.match(/^(.+) K(\d+)T$/); if (!m || m[2] === '0') continue;
    const pair = byName.get(`${m[1]} K${m[2]}B`); if (!pair) continue;
    const a = lay.posOnSeg(o.segId, o.s).p, b = lay.posOnSeg(pair.segId, pair.s).p;
    jalur.push({ station: m[1], n: +m[2], x: r2((a.x + b.x) / 2), y: r2(Math.abs(a.y) > Math.abs(b.y) ? a.y : b.y) });
  }
  const scenery = [...world.scenery.values()].map((s: any) => { const p = lay.toPanel(s.pos); return { id: s.id, kind: s.kind, code: s.code ?? null, label: s.label ?? null, x: r2(p.x), y: r2(p.y) }; });
  return {
    ok: true, key: lay.key, yScale: 3, bbox: { x0: r2(bx0), y0: r2(by0), x1: r2(bx1), y1: r2(by1) },
    segments, nodes, points,
    signals: ts.filter((o: any) => o.kind === 'signal').map(tsObj),
    berths: ts.filter((o: any) => o.kind === 'trackmark').map(tsObj),
    portals: ts.filter((o: any) => o.kind === 'portal').map(tsObj),
    stations, jalur, scenery,
  };
}

function cmdStep(c: any) {
  requireSession();
  const dt = typeof c.dt === 'number' ? c.dt : 0.1;
  if (ai) ai.step(null);      // AI serves every station; null = no player-owned stations
  tickS40();
  session.step(dt);
  return { ok: true, ...dynamicState() };
}

function tolakToJson(tolak: any) {
  return { alasan: tolak.alasan, segs: tolak.segs, wesel: tolak.wesel ?? null, sinyal: tolak.sinyal ?? null,
    ruteLawan: tolak.ruteLawan ?? null, ka: tolak.ka ?? null };
}

/** Same gate the UI uses (ui/rute.ts trySetRoute), minus the confirmation cards. */
function setCandidate(cand: any) {
  const entry = world.trackside.get(cand.entrySignal);
  const holding = ixl.ruteMenahanSinyal(cand.entrySignal);
  if (holding) {
    const p = ixl.dapatDibatalkan(holding.id);
    return { ok: false, reason: 'sudah berute', detail: `${entry?.name} already routed to ${holding.def.exitLabel}`,
      route: holding.id, cancellable: p.bisa, ka: p.ka ?? null };
  }
  const tolak = ixl.whyBlocked(cand);
  if (tolak) {
    const all = ixl.semuaHalangan(cand).map(tolakToJson);
    return { ok: false, reason: tolak.alasan, tolak: tolakToJson(tolak), semua: all };
  }
  const hasil = ixl.ajukan(cand, session.clock, true);
  if (hasil === 'minta') return { ok: true, status: 'minta', detail: 'block requested, waiting for "aman" from the next station' };
  if (hasil) return { ok: true, status: 'set', route: hasil.id, exitLabel: cand.exitLabel };
  return { ok: false, reason: 'ditolak', detail: 'block request refused (cooldown) or route not settable' };
}

function resolveSignal(id: string) {
  // Accept a trackside id or a signal name; resolve shared-exit-signal masks.
  const gk = grupDariId(world, id);
  let sig = gk ? resolveBersama(world, gk) : world.trackside.get(id);
  if (!sig) sig = world.findByName(id);
  if (gk && !sig) throw new Error('shared exit signal: flip the point to the departing track first');
  if (!sig || sig.kind !== 'signal') throw new Error(`signal not found: ${id}`);
  return sig;
}

/** Mirrors main.ts `klikSinyal` (3D click, expert mode): click a routed signal =
 *  cancel its route; otherwise trace the path along current point settings
 *  and set the route. */
function cmdClickSignal(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.id));
  const active = ixl.routeFromSignal(sig.id);
  if (active && ixl.dapatDibatalkan(active.id).bisa) {
    const ok = ixl.cancelRoute(active.id);
    return { ok, action: 'cancel', signal: sig.id, route: active.id, detail: ok ? 'route cancelled' : 'cannot cancel: train already inside' };
  }
  if (sig.signalType !== 'interlocking' && sig.signalType !== 'bersama') {
    return { ok: false, action: 'set', signal: sig.id, reason: 'bukan sinyal interlocking', detail: `${sig.name}: ${sig.signalType} signal is not operated manually` };
  }
  const tr = ixl.traceByPoints(sig, !!c.sepurSalah);
  if (!tr.cand) {
    return { ok: false, action: 'set', signal: sig.id, reason: tr.dm ? 'melawan penanda arah' : (tr.alasan ?? 'jalur belum menerus'),
      path: tr.path, wesel: tr.wesel ?? null, dm: tr.dm ?? null };
  }
  return { action: 'set', signal: sig.id, path: tr.path, ...setCandidate(tr.cand) };
}

/** Beginner mode: route from a signal to a named exit (exitLabel / exit signal id or name). */
function cmdSetRoute(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.from));
  const cands = ixl.findRoutes(sig, undefined, !!c.sepurSalah);
  const to = c.to === undefined ? null : String(c.to);
  let cand = null;
  if (to !== null) {
    cand = cands.find((x: any) => x.exitLabel === to || x.exitSignal === to
      || (x.exitSignal && world.trackside.get(x.exitSignal)?.name === to)) ?? null;
  } else if (typeof c.index === 'number') cand = cands[c.index] ?? null;
  if (!cand) {
    return { ok: false, signal: sig.id, reason: 'tak ada kandidat',
      candidates: cands.map((x: any) => ({ exitLabel: x.exitLabel, exitSignal: x.exitSignal, dist: r2(x.dist), segs: x.segs.length })) };
  }
  if (c.izinTerisi) cand = { ...cand, izinTerisi: true };
  return { signal: sig.id, ...setCandidate(cand) };
}

function cmdRoutes(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.id));
  return { ok: true, signal: sig.id, candidates: ixl.findRoutes(sig, undefined, !!c.sepurSalah).map((x: any, i: number) => ({
    index: i, exitLabel: x.exitLabel, exitSignal: x.exitSignal, dist: r2(x.dist), segs: x.segs,
    junctions: x.junctions, blocked: (() => { const t = ixl.whyBlocked(x); return t ? tolakToJson(t) : null; })(),
  })) };
}

/** Hover preview (hud3d.ts setHoverSinyal): the path `click_signal` WOULD lock along the
 *  current point settings, without touching anything. `cand` is null when the trace dead-ends
 *  (then `path` stops at the offending point and `reason` says why). */
function cmdPreview(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.signal ?? c.id));
  const active = ixl.routeFromSignal(sig.id);
  const manual = sig.signalType === 'interlocking' || sig.signalType === 'bersama';
  const tr = ixl.traceByPoints(sig, !!c.sepurSalah);
  return {
    ok: true, signal: sig.id, name: sig.name, manual, active: active?.id ?? null,
    path: tr.path, cand: tr.cand ? { exitLabel: tr.cand.exitLabel, exitSignal: tr.cand.exitSignal, segs: tr.cand.segs } : null,
    reason: tr.cand ? null : (tr.dm ? 'melawan penanda arah' : (tr.alasan ?? 'jalur belum menerus')),
    wesel: tr.wesel ?? null, dm: tr.dm ?? null,
  };
}

function cmdCancelRoute(c: any) {
  requireSession();
  let id: string = c.route ?? '';
  if (!id && c.signal) id = ixl.routeFromSignal(resolveSignal(String(c.signal)).id)?.id ?? '';
  if (!id || !ixl.routes.has(id)) return { ok: false, reason: 'rute tak ada' };
  const p = ixl.dapatDibatalkan(id);
  if (!p.bisa) return { ok: false, reason: 'sedang dilalui', ka: p.ka ?? null };
  return { ok: ixl.cancelRoute(id), route: id };
}

/** World.balikWesel is the single human entry for point flips (refuses when
 *  locked by a route or when a train is on/near the blades). */
function cmdFlipPoint(c: any) {
  if (!world) throw new Error('load a map first');
  const id = String(c.id);
  const n = world.graph.nodes.get(id);
  if (!n?.junction) return { ok: false, id, reason: 'bukan wesel' };
  const r = world.balikWesel(id);
  const j = n.junction;
  if (r.ok) return { ok: true, id, setting: j.setting, leg: j.legs[j.setting] };
  return { ok: false, id, reason: r.alasan, ka: r.ka ?? null, setting: j.setting, locked: j.lockedBy ?? null };
}

function cmdState() { requireSession(); return { ok: true, ...dynamicState() }; }

function handle(c: any) {
  switch (c.cmd) {
    case 'load': return cmdLoad(c);
    case 'start': return cmdStart(c);
    case 'step': return cmdStep(c);
    case 'state': return cmdState();
    case 'click_signal': return cmdClickSignal(c);
    case 'set_route': return cmdSetRoute(c);
    case 'routes': return cmdRoutes(c);
    case 'preview': return cmdPreview(c);
    case 'cancel_route': return cmdCancelRoute(c);
    case 'flip_point': return cmdFlipPoint(c);
    case 'set_time_scale': return cmdSetTimeScale(c);
    case 'set_clock': return cmdSetClock(c);
    case 'beri_s40': return cmdBeriS40(c);
    case 'hapus_ka': return cmdHapusKA(c);
    case 'train_detail': return cmdTrainDetail(c);
    case 'route_menu': return cmdRouteMenu(c);
    case 'panel': return cmdPanel();
    case 'ping': return { ok: true, pong: true, ppka: PPKA };
    case 'quit': return { ok: true, bye: true };
    default: throw new Error(`unknown cmd "${c.cmd}"`);
  }
}

// ---- main loop ----
// The parent may close its end before reading the last line; that is not an error.
process.stdout.on('error', (e: any) => { if (e?.code !== 'EPIPE') throw e; });
const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
process.stdout.write(JSON.stringify({ ready: true, ppka: PPKA, pid: process.pid }) + '\n');
for await (const line of rl) {
  const s = line.trim();
  if (!s) continue;
  let cmd: any;
  try { cmd = JSON.parse(s); } catch (e) {
    process.stdout.write(JSON.stringify({ ok: false, error: `bad JSON: ${(e as Error).message}` }) + '\n');
    continue;
  }
  let resp: any;
  try { resp = handle(cmd); } catch (e) {
    resp = { ok: false, error: (e as Error).message };
    process.stderr.write(`sim-bridge: ${cmd.cmd}: ${(e as Error).stack ?? e}\n`);
  }
  if (cmd.seq !== undefined) resp.seq = cmd.seq;
  process.stdout.write(JSON.stringify(resp) + '\n');
  if (cmd.cmd === 'quit') break;
}
// No process.exit(): stdout to a pipe is asynchronous in Node and exit() would
// truncate a large pending response. Let the event loop drain and end.
rl.close();
process.stdin.pause();
process.exitCode = 0;
