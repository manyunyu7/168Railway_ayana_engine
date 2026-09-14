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
