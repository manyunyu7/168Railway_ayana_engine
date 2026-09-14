// Lists the catalog slots (model.json ids) the given maps can need in the web client: every vehicle model any
// fleet (armada) may resolve for the consists in the GAPEKA (+ their bogie / coupling attachments), the
// hiasan objects, the garis classes and the `vegetasi` category. One id per line: "<id> <berkas>".
// Run from the PPKA root:  cd ../ppka-wannabe-2 && npx tsx ../game-engine-experiment/web/slots-for-maps.mts mojokerto bks
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const PPKA = process.env.PPKA_ROOT || process.cwd();
const mod = (rel: string) => import(pathToFileURL(path.join(PPKA, rel)).href);
const { CONSIST_LIBRARY } = await mod('src/engine/train.ts');
const { modelSemuaArmada } = await mod('src/tiga/armada.ts');
const catalog = JSON.parse(fs.readFileSync(path.join(PPKA, 'public/model3d/model.json'), 'utf8'));
const sarana: Record<string, any> = catalog.sarana ?? {};
const objek: any[] = catalog.objek ?? [];
const garis: any[] = catalog.garis ?? [];
// §7.2 MODEL_SARANA fallbacks the bridge / engine use when no fleet model applies (engine/world/asset_catalog.cpp)
const SLOT: Record<string, string> = { cc201: 'loko201', cc203: 'loko203', cc206: 'loko206', 'krl-kuha': 'nryJr205KuhaBadan', 'krl-moha': 'nryJr205MohaBadan', 'krl-moha-p': 'nryJr205MpBadan', k1: 'kereta', k3: 'kereta', m1: 'makan', p: 'pembangkit', gd: 'gerbong', d14: 'atpD14Jaladara' };

const out = new Map<string, string>();
const add = (id: string, berkas?: string) => { if (id && berkas && !out.has(id)) out.set(id, berkas); };
const addSarana = (id: string) => {
  const e = sarana[id] ?? sarana[SLOT[id] ?? ''];
  if (!e) return;
  add(id, e.pilotBerkas ?? e.berkas);
  for (const att of [e.bogie, e.kopling]) for (const v of Object.values(att ?? {})) if (typeof v === 'string') addSarana(v);
};
for (const map of process.argv.slice(2)) {
  const data = JSON.parse(fs.readFileSync(path.join(PPKA, 'src/data', map.endsWith('.json') ? map : `${map}.json`), 'utf8'));
  const world = data.world ?? data;
  for (const e of data.session?.entries ?? []) {
    const def = CONSIST_LIBRARY[e.consist];
    for (const car of def?.cars ?? []) {
      if (car.sarana) { for (const m of modelSemuaArmada(car.sarana)) addSarana(m); addSarana(car.sarana); }
    }
  }
  for (const o of world.hiasan?.objek ?? []) { const e = objek.find((x: any) => x.id === o.model); if (e) add(o.model, e.pilotBerkas ?? e.berkas); }
  for (const g of world.hiasan?.garis ?? []) {
    const k = garis.find((x: any) => x.id === g.kelas); if (!k) continue;
    add(`garis:${k.id}`, k.berkas); add(`garis:${k.id}:tiang`, k.tiang);
    (k.slot ?? []).forEach((s: any, i: number) => add(`garis:${k.id}:slot${i}`, s.berkas));
  }
}
for (const e of objek) if (e.kategori === 'vegetasi') add(e.id, e.pilotBerkas ?? e.berkas);
for (const [id, berkas] of out) console.log(id, berkas);
