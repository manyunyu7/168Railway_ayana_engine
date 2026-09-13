// Reference dump for tests/test_track.cpp: runs the TypeScript TrackGraph (the source of truth)
// on src/data/mojokerto.json and writes lengths, samples and junctions as JSON.
//   cd ../ppka-wannabe-2 && npx tsx ../game-engine-experiment/tests/ref/track_ref.ts
// Regenerate (and commit) tests/ref/mojokerto_track.json whenever track.ts changes.
import { readFileSync, writeFileSync } from 'fs';
import { dirname, join, resolve } from 'path';
import { fileURLToPath } from 'url';
import { TrackGraph } from '../../../ppka-wannabe-2/src/engine/track';

const here = dirname(fileURLToPath(import.meta.url));
const map = process.argv[2] ?? resolve(here, '../../../ppka-wannabe-2/src/data/mojokerto.json');
const out = process.argv[3] ?? join(here, 'mojokerto_track.json');
const save = JSON.parse(readFileSync(map, 'utf8'));
const g = TrackGraph.fromJSON(save.world.graph);

const segments = [...g.segments.values()].map((s) => {
  const L = g.length(s.id);
  const samples = [0, L / 3, L / 2, (2 * L) / 3, L].map((sm) => {
    const r = g.sampleAt(s.id, sm);
    return { s: sm, x: r.p.x, y: r.p.y, tx: r.tan.x, ty: r.tan.y };
  });
  return { id: s.id, a: s.a, b: s.b, length: L, samples };
});
const points = [...g.nodes.values()].filter((n) => n.junction).map((n) => ({
  id: n.id, facingSeg: n.junction!.facingSeg, legs: n.junction!.legs,
  setting: n.junction!.setting, spring: n.junction!.spring ?? null,
}));
const total = segments.reduce((a, s) => a + s.length, 0);
writeFileSync(out, JSON.stringify({ map: 'mojokerto', nodes: g.nodes.size, segments, points, totalLength: total }, null, 1));
console.log(`${out}: ${segments.length} segments, ${points.length} points, ${(total / 1000).toFixed(3)} km`);
