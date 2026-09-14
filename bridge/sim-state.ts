// sim-state — the static world summary and the per-step dynamic state of the PPKA simulator as plain JSON
// (bridge/PROTOCOL.md `load.summary` and `step`). Shared by the Node bridge (sim-bridge.ts, JSON lines over
// pipes) and the browser adapter (ppka-wannabe-2/src/tiga-ayana, which feeds the same objects straight into
// the Wasm engine), so this file imports NOTHING: the engine entry points it needs are injected through
// `SimStateDeps` and every simulator object is `any` on purpose (the bridge must not break on internal
// TS refactors, only on the handful of entry points it uses).

export interface SimStateDeps {
  GAP: number;                                            // engine/train.ts coupling gap (m)
  kelasKA: (trainNo: string) => number;                   // engine/kelasKA.ts
  armadaUntuk?: ((no: string, paksa?: string) => any) | null;      // tiga/armada.ts (fleet by train number)
  modelArmada?: ((armada: any, sarana: string) => string | undefined) | null;
}

export const r3 = (v: number) => Math.round(v * 1000) / 1000;
export const r2 = (v: number) => Math.round(v * 100) / 100;

// ---- static world summary (what a renderer needs, geometry pre-sampled) ----
function samplePoly(world: any, segId: string, stepM: number): number[] {
  const pts: number[] = [];
  const L = world.graph.length(segId);
  for (let s = 0; s < L; s += stepM) { const p = world.graph.pointAt(segId, s); pts.push(r3(p.x), r3(p.y)); }
  const e = world.graph.pointAt(segId, L); pts.push(r3(e.x), r3(e.y));
  return pts;
}

export function worldSummary(world: any, stepM = 4) {
  const nodes = [...world.graph.nodes.values()].map((n: any) => ({ id: n.id, x: r3(n.pos.x), y: r3(n.pos.y) }));
  const segments = [...world.graph.segments.values()].map((sg: any) => ({
    id: sg.id, a: sg.a, b: sg.b, len: r3(world.graph.length(sg.id)),
    jenis: sg.jenisRel ?? null, poly: samplePoly(world, sg.id, stepM),
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
export function modelFor(deps: SimStateDeps, t: any, car: any): string {
  if (deps.armadaUntuk && deps.modelArmada && car.sarana) {
    try { const m = deps.modelArmada(deps.armadaUntuk(t.trainNo, car.armada), car.sarana); if (m) return m; } catch { /* fall through */ }
  }
  return car.sarana ?? (car.kind === 'loco' ? 'loko' : 'kereta');
}

export function trainState(deps: SimStateDeps, world: any, session: any, t: any) {
  const cars = t.consist?.cars ?? [];
  const vehicles: any[] = [];
  let d = 0;
  for (const car of cars) {
    const a = t.pointBehind(world, d);
    const b = t.pointBehind(world, d + car.length);
    d += car.length + deps.GAP;
    if (!a || !b) continue;
    const dx = a.p.x - b.p.x, dy = a.p.y - b.p.y;
    vehicles.push({
      model: modelFor(deps, t, car), sarana: car.sarana ?? null, kind: car.kind, len: r2(car.length),
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
    kelas: deps.kelasKA(t.trainNo), state: t.state, kendali: t.kendali,
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

/** Per-session bookkeeping the state builder keeps between calls: the log cursor (only lines new since the
 *  previous call are emitted) and the level-crossing cache (`World.jplClosed` re-evaluated every 0.25 s of
 *  sim time, as bangun3d.ts perbaruiJPL). Create one per session; `reset()` on load/start. */
/** engine/panel.ts PanelLayout: the schematic control table ("meja layan") as JSON (PROTOCOL.md `panel`).
 *  Coordinates are panel units; the web draws them with y scaled ×3 (`Renderer.yScale`). Static per world.
 *  `lay` = PanelLayout.build(world) (engine/panel.ts), `stasiunSinyal` = engine/trackside.ts. */
export function panelLayoutJson(world: any, lay: any, stasiunSinyal: (name: string) => string | null) {
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

export class SimStateBuilder {
  logSeen = 0;
  private jplAt = -1;
  private jplCache: { id: string; closed: boolean }[] = [];
  constructor(public deps: SimStateDeps) {}
  reset() { this.logSeen = 0; this.jplAt = -1; this.jplCache = []; }

  jplState(world: any, session: any) {
    if (this.jplAt >= 0 && session.clock - this.jplAt < 0.25 && session.clock >= this.jplAt) return this.jplCache;
    this.jplAt = session.clock;
    this.jplCache = [...world.scenery.values()].filter((o: any) => o.kind === 'jpl')
      .map((o: any) => ({ id: o.id, closed: world.jplClosed(o) }));
    return this.jplCache;
  }

  /** The `step` response body (without `ok`). `withLog` false skips the log diff (the browser adapter keeps
   *  its own log UI). */
  dynamicState(world: any, ixl: any, session: any, withLog = true) {
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
      junctions: (r.def.junctions ?? []).map((j: any) => [j.nodeId, j.legSeg]),   // point node + the leg the route takes
      sepurSalah: !!r.def.sepurSalah, izinTerisi: !!r.def.izinTerisi,
    }));
    // shunting plans (engine/putarLok.ts overlay): the yellow dashed ribbons; `legs` = segment ids per leg
    const langsir = (session.putarLok?.overlay?.() ?? []).map((o: any) => ({
      lok: o.lokId ?? null, kendali: o.kendali, legIdx: o.legIdx, legs: o.legs.map((l: any) => l.segs),
    }));
    let fresh: any[] = [];
    if (withLog) {
      // session.log is newest-first (unshift); emit only entries not seen before, oldest first
      const total = session.log.length;
      fresh = session.log.slice(0, Math.max(0, total - this.logSeen)).reverse()
        .map((l: any) => ({ t: r2(l.time), kind: l.kind, text: l.text }));
      this.logSeen = total;
    }
    return {
      clock: r2(session.clock), score: session.score, violations: session.violations,
      pending: session.pending.length, timeScale: session.timeScale,
      trains: session.trains.map((t: any) => trainState(this.deps, world, session, t)),
      points, signals, occupancy, routes, langsir, jpl: this.jplState(world, session), log: fresh,
    };
  }
}
