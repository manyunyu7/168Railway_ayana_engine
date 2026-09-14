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
import { SimStateBuilder, worldSummary, panelLayoutJson, r2, r3, type SimStateDeps } from './sim-state.ts';

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
let s40Manual = false;  // bridge-side Semboyan 40 gate (ui/s40.ts tickS40, branch A)
let startOpts: any = {};   // last `start` options, re-used by set_clock
let panelCache: any = null;   // PanelLayout for the loaded world

function parseClock(s: string | number | undefined): number | undefined {
  if (s === undefined || s === null) return undefined;
  if (typeof s === 'number') return s;
  const m = /^(\d{1,2}):(\d{2})(?::(\d{2}))?$/.exec(s);
  if (!m) throw new Error(`bad clock "${s}" (want HH:MM[:SS])`);
  return +m[1] * 3600 + +m[2] * 60 + (m[3] ? +m[3] : 0);
}

// ---- static summary + dynamic state: bridge/sim-state.ts (shared with the browser adapter) ----
const stateDeps: SimStateDeps = { GAP, kelasKA, armadaUntuk, modelArmada };
const builder = new SimStateBuilder(stateDeps);
const summary = (stepM: number) => worldSummary(world, stepM);
const dynamicState = () => builder.dynamicState(world, ixl, session);

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

// ---- commands ----
function requireSession() { if (!session) throw new Error('no session: send "load" then "start" first'); }

function cmdLoad(c: any) {
  const name: string = c.map ?? 'kroya';
  const file = path.isAbsolute(name) ? name : path.join(PPKA, 'src/data', name.endsWith('.json') ? name : `${name}.json`);
  const data = JSON.parse(fs.readFileSync(file, 'utf8'));
  world = World.fromJSON(data.world ?? data);
  ixl = new Interlocking(world);
  session = null; ai = null; builder.reset(); panelCache = null;
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
  builder.reset();
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
/** ui/rute.ts kandidatMenu: dedup per destination (straightest), entry signals by track number,
 *  wrong-line entries last. Shared by route_menu and set_route {index} so the indices agree. */
function menuCandidates(sig: any, diag?: any): any[] {
  const cands: any[] = dedupPerTujuan(world, ixl.findRoutes(sig, diag, true));
  if (isSinyalMasuk(sig.name))
    cands.sort((a, b) => (nomorJalurSinyal(a.exitLabel) ?? 99) - (nomorJalurSinyal(b.exitLabel) ?? 99));
  cands.sort((a, b) => Number(!!a.sepurSalah) - Number(!!b.sepurSalah));
  return cands;
}

function cmdRouteMenu(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.signal ?? c.id));
  const active = ixl.routeFromSignal(sig.id);
  const manual = sig.signalType === 'interlocking' || sig.signalType === 'bersama';
  const diag = { explored: new Set<string>(), prunes: [] as any[] };
  const cands: any[] = manual ? menuCandidates(sig, diag) : [];
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

/** engine/panel.ts PanelLayout: the schematic control table ("meja layan") — serialised by sim-state.ts
 *  `panelLayoutJson` (shared with the browser adapter). Static per world. */
function cmdPanel() {
  if (!world) throw new Error('load a map first');
  if (!panelCache || panelCache.key !== PanelLayout.keyOf(world)) panelCache = PanelLayout.build(world);
  return panelLayoutJson(world, panelCache, stasiunSinyal);
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

/** Permission cards (ui/rute.ts tawarSepurSalah / tawarIzinTerisi / tawarIzinIkut / tawarBatalLawan,
 *  text from there). `confirm` is the command the client re-sends through `confirm` once the
 *  player pressed the button: the same command plus the permission flag (`izin:true` = card
 *  answered, `izinTerisi:true` = calling-on, `sepurSalah:true` = forced wrong-line trace,
 *  `batalLawan:<routeId>` = cancel the opposing route first). */
function needsConfirm(kind: string, judul: string, rute: string, akibat: string, batas: string, tombol: string, confirm: any) {
  return { ok: false, reason: 'perlu izin', needsConfirm: { kind, judul, rute, akibat, batas, tombol, confirm } };
}

/** ui/rute.ts jarakPenghuni: how far the occupant stands ahead of the signal. */
function jarakPenghuni(cand: any): string {
  const sig = world.trackside.get(cand.entrySignal);
  if (!sig) return '';
  const d = world.distToTrainAhead({ segId: sig.segId, s: sig.s, dir: sig.dir }, 4000, '');
  if (d === null) return '';
  return d < 60 ? `, berdiri hanya ${Math.round(d)} m di muka sinyal — KA-mu praktis tak akan bergerak`
    : `, berdiri ${Math.round(d)} m di muka sinyal`;
}

/** ui/rute.ts sisaHalangan: obstructions other than occupancy that will still refuse. */
function sisaHalangan(cand: any): string {
  const lain = ixl.semuaHalangan(cand).filter((t: any) => t.alasan !== 'blok terisi');
  if (!lain.length) return '';
  return ` Izin ini TIDAK cukup: masih terhalang ${lain.map((t: any) => (t.ruteLawan ? `rute ${t.ruteLawan.entry} → ${t.ruteLawan.exit}` : t.alasan)).join(', ')}.`;
}

function kartuSepurSalah(sigId: string, dm: string, confirm: any) {
  const sig = world.trackside.get(sigId);
  return needsConfirm('sepurSalah', 'Sepur salah', `${sig?.name ?? sigId} → sepur arus lawan`,
    `Jalur melawan Penanda Arah ${dm}. Rute dibentuk dengan aspek KUNING dan WAJIB warta izin dari stasiun depan — di petak itu tak ada sinyal blok yang menghadapmu.`,
    'Penanda arah adalah satu-satunya proteksi adu muka di petak ini. Sesudah ini, kamu yang menjaganya.',
    'Paksa sepur salah', confirm);
}

/** Same gate the UI uses (ui/rute.ts trySetRoute). `c` = the originating command (re-sent as
 *  `confirm` with the flags); without `c.izin` the player-permission cases return `needsConfirm`. */
function setCandidate(cand: any, c: any) {
  const entry = world.trackside.get(cand.entrySignal);
  const rute = `${entry?.name ?? cand.entrySignal} → ${cand.exitLabel}`;
  if (cand.sepurSalah && !c.izin) return kartuSepurSalah(cand.entrySignal, cand.dm ?? 'petak', { ...c, sepurSalah: true, izin: true });
  if (c.batalLawan) {
    if (!ixl.routes.has(c.batalLawan)) return { ok: false, reason: 'rute tak ada', route: c.batalLawan };
    if (!ixl.cancelRoute(c.batalLawan)) return { ok: false, reason: 'sedang dilalui', route: c.batalLawan, ka: ixl.dapatDibatalkan(c.batalLawan).ka ?? null };
  }
  const holding = ixl.ruteMenahanSinyal(cand.entrySignal);
  if (holding) {
    const p = ixl.dapatDibatalkan(holding.id);
    return { ok: false, reason: 'sudah berute', detail: `${entry?.name} already routed to ${holding.def.exitLabel}`,
      route: holding.id, cancellable: p.bisa, ka: p.ka ?? null };
  }
  const tolak = ixl.whyBlocked(cand);
  if (tolak) {
    const all = ixl.semuaHalangan(cand).map(tolakToJson);
    const rejected = { ok: false, reason: tolak.alasan, tolak: tolakToJson(tolak), semua: all, izinTerisi: !!cand.izinTerisi };
    if (tolak.alasan === 'blok terisi' && !cand.izinTerisi) {
      const ka = trainLabel(tolak.ka);
      return { ...rejected, ...needsConfirm('izinTerisi', 'Izin masuk sepur terisi', rute,
        `Jalur ditempati ${ka}${jarakPenghuni(cand)}. Sinyal diberi aspek KUNING; KA merayap masuk dan berhenti di belakang rangkaian yang berdiri.`,
        `Izin ini HANYA melewati okupansi; bentrok rute aktif & wesel terkunci tetap menolak.${sisaHalangan(cand)}`,
        'Beri izin', { ...c, izinTerisi: true, izin: true }) };
    }
    const lawan = tolak.ruteLawan;
    if (lawan?.id && ixl.routes.has(lawan.id) && !cand.izinTerisi && ixl.bolehMengikuti(cand, ixl.routes.get(lawan.id))) {
      return { ...rejected, ...needsConfirm('mengikuti', 'Izin masuk petak belum bebas', rute,
        `Petak masih dikuasai rute ${lawan.entry} → ${lawan.exit} yang SEARAH dan setujuan${jarakPenghuni(cand)}. Sinyal diberi aspek KUNING; KA berjalan hati-hati di belakangnya.`,
        'Hanya sah karena tujuan bloknya sama (searah) dan tak ada wesel yang diminta berbeda. Jarak ke KA di depan tetap dijaga batas keras masinis.',
        'Izinkan mengikuti', { ...c, izinTerisi: true, izin: true }) };
    }
    if (lawan?.id && ixl.dapatDibatalkan(lawan.id).bisa) {
      return { ...rejected, ...needsConfirm('batalLawan', 'Bentrok rute aktif', rute,
        `Segmennya sudah dikunci rute ${lawan.entry} → ${lawan.exit}. Batalkan rute itu (sinyalnya kembali merah), lalu rutemu dibentuk.`,
        'Pembatalan ditolak bila sudah ada KA di dalam rute tersebut — tunggu sampai lewat.',
        'Batalkan rute lawan', { ...c, batalLawan: lawan.id, izin: true }) };
    }
    return rejected;
  }
  const hasil = ixl.ajukan(cand, session.clock, true);
  if (hasil === 'minta') return { ok: true, status: 'minta', detail: 'block requested, waiting for "aman" from the next station' };
  if (hasil) return { ok: true, status: 'set', route: hasil.id, exitLabel: cand.exitLabel, sepurSalah: !!cand.sepurSalah, izinTerisi: !!cand.izinTerisi };
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
    if (tr.dm && !c.sepurSalah)
      return { action: 'set', signal: sig.id, path: tr.path, dm: tr.dm, ...kartuSepurSalah(sig.id, tr.dm, { cmd: 'click_signal', id: sig.id, sepurSalah: true, izin: true }) };
    return { ok: false, action: 'set', signal: sig.id, reason: tr.dm ? 'melawan penanda arah' : (tr.alasan ?? 'jalur belum menerus'),
      path: tr.path, wesel: tr.wesel ?? null, dm: tr.dm ?? null };
  }
  const cand = c.izinTerisi ? { ...tr.cand, izinTerisi: true } : tr.cand;
  return { action: 'set', signal: sig.id, path: tr.path, ...setCandidate(cand, { cmd: 'click_signal', id: sig.id, sepurSalah: !!c.sepurSalah, izin: !!c.izin, izinTerisi: !!c.izinTerisi, batalLawan: c.batalLawan ?? undefined }) };
}

/** Beginner mode: route from a signal to a named exit (exitLabel / exit signal id or name). */
function cmdSetRoute(c: any) {
  requireSession();
  const sig = resolveSignal(String(c.from));
  // `index` counts the route_menu list (dedup + sorted, wrong-line included) so a menu pick round-trips
  const cands = typeof c.index === 'number' && c.to === undefined ? menuCandidates(sig) : ixl.findRoutes(sig, undefined, !!c.sepurSalah);
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
  const orig = { cmd: 'set_route', from: sig.id, to: c.to, index: c.index, sepurSalah: !!c.sepurSalah, izinTerisi: !!c.izinTerisi, izin: !!c.izin, batalLawan: c.batalLawan ?? undefined };
  return { signal: sig.id, ...setCandidate(cand, orig) };
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
    case 'confirm': {   // the card's button: re-issue the stored command (needsConfirm.confirm) with its flags
      const of = c.of;
      if (!of || (of.cmd !== 'click_signal' && of.cmd !== 'set_route')) throw new Error('confirm: `of` must be a click_signal/set_route command');
      return handle({ ...of, izin: true });
    }
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
