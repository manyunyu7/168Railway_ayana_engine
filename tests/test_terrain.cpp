// Terrain carving math without tiles: brush deltas (world.tanah, bilinear on the 8 m grid) and the
// bridge trough (ground lowered to deck bottom - JBT_RUANG within 7 m, blending to 26 m; never raised);
// then the streaming data path without a GPU: index.json layers, DEM tiles from Terrarium pixels,
// demTilesWanted / failTile / demComplete, and the satellite tile state machine (provide / edge distance).
#include "engine/core/json.h"
#include "engine/render/mesh_builder.h"
#include "engine/world/slippy.h"
#include "engine/world/terrain.h"
#include "tests/check.h"
#include <cmath>

using namespace eng;

TEST_MAIN({
  Terrain t;   // no DEM loaded: raw height 0 everywhere, origin (0,0)
  std::string err;
  Json tanah = Json::parse(R"({"kisi":8,"delta":{"10,20":2.0,"11,20":4.0,"10,21":-2.0,"11,21":0.0}})", &err); CHECK_MSG(err.empty(), err);
  t.setBrushDeltas(tanah);
  CHECK_NEAR(t.brushDelta(80, 160), 2.0, 1e-5);       // grid node (10,20)
  CHECK_NEAR(t.brushDelta(88, 160), 4.0, 1e-5);
  CHECK_NEAR(t.brushDelta(84, 160), 3.0, 1e-5);       // halfway along x
  CHECK_NEAR(t.brushDelta(84, 164), 1.0, 1e-5);       // centre of the cell: (2+4-2+0)/4
  CHECK_NEAR(t.brushDelta(500, 500), 0.0, 1e-6);      // outside: 0
  CHECK_NEAR(t.groundHeight(84, 164), 1.0, 1e-5);     // DEM 0 + delta, no rails
  t.setBrushDeltas(Json{});                            // null accepted
  CHECK_NEAR(t.groundHeight(84, 164), 0.0, 1e-6);

  // Bridge deck along x from 0 to 120 m at rail y = -2 (deck bottom = -3.98), full weight in the middle.
  std::vector<RailSample> s;
  for (int i = 0; i <= 10; ++i) s.push_back({i * 12.0, 0.0, -2.f, false, 1.f, i == 0 || i == 10 ? 0.f : 1.f});
  t.setRails(s);
  float ceiling = -2 + terrain::DECK_BOTTOM - terrain::BRIDGE_CLEAR;   // -5.48
  CHECK_NEAR(t.groundHeight(60, 0), ceiling, 1e-4);   // under the deck
  CHECK_NEAR(t.groundHeight(60, 5), ceiling, 1e-4);   // inside JBT_DALAM
  float mid = t.groundHeight(60, 16.5);                // halfway through the blend: smoothstep(0.5) = 0.5
  CHECK_NEAR(mid, ceiling * 0.5, 1e-3);
  CHECK_NEAR(t.groundHeight(60, 30), 0.0, 1e-6);      // beyond JBT_LUAR
  CHECK_NEAR(t.groundHeight(0, 0), 0.0, 1e-6);        // weight 0 at the embankment end
  // never raised: a deck high above the ground leaves it alone
  std::vector<RailSample> hi;
  for (int i = 0; i <= 10; ++i) hi.push_back({i * 12.0, 0.0, 30.f, false, 1.f, 1.f});
  t.setRails(hi);
  CHECK_NEAR(t.groundHeight(60, 0), 0.0, 1e-6);

  // ---- streaming source: index.json (tools/fetch_tiles) ----
  Terrain s2;
  Json idx = Json::parse(R"({"bbox":[12509827.7,832797.1,12524648.3,837560.4],"target":"desktop","mean":0.35,
    "dem":[{"dir":"dem","zoom":13,"tx0":6652,"ty0":4265,"nx":2,"ny":1,"px":256,"present":"11"}],
    "sat":[{"dir":"sat/0","zoom":14,"tx0":13305,"ty0":8531,"nx":2,"ny":2,"px":512,"present":"1111"},
           {"dir":"sat/1","zoom":12,"tx0":3326,"ty0":2132,"nx":1,"ny":1,"px":256,"present":"1"},
           {"dir":"sat/2","zoom":16,"tx0":53223,"ty0":34127,"nx":2,"ny":1,"px":256,"present":"10"}]})", &err);
  CHECK_MSG(err.empty(), err);
  CHECK_MSG(s2.loadIndex(idx, err), err);
  CHECK(s2.streamed());
  CHECK(s2.sat().layers.size() == 3 && s2.sat().detail.size() == 1 && s2.sat().detail[0] == 2);
  CHECK_NEAR(s2.sat().meanBrightness, 0.35, 1e-6);
  CHECK(s2.demTilesWanted().size() == 2 && !s2.demComplete());
  // a Terrarium tile: every pixel encodes 100.5 m (R 128, G 100, B 128 -> 128*256 + 100 + 0.5 - 32768)
  std::vector<uint8_t> px(256 * 256 * 4);
  for (size_t i = 0; i < px.size(); i += 4) { px[i] = 128; px[i + 1] = 100; px[i + 2] = 128; px[i + 3] = 255; }
  CHECK(s2.provideDemRgba(13, 6652, 4265, 256, 256, px.data()));
  CHECK(!s2.provideDemRgba(13, 6660, 4265, 256, 256, px.data()));   // outside the layer
  CHECK(s2.demTilesWanted().size() == 1);
  s2.failTile({"dem", 13, 6653, 4265});
  CHECK(s2.demComplete());
  s2.finishDem();
  const DemLayer& L0 = s2.dem().layers[0];
  CHECK_NEAR(s2.rawHeight(L0.x0 + 100, L0.y0 + 100), 100.5, 1e-3);            // delivered tile
  CHECK_NEAR(s2.rawHeight(L0.x0 + L0.ts + 100, L0.y0 + 100), 100.5, 1e-3);    // failed tile: nearest present tile, clamped
  // satellite state: only requested tiles accept pixels; resident tiles answer satColor / imageryKeyAt
  const SatLayer& n = s2.sat().layers[0];
  CHECK(!n.has(13305, 8531) && n.inside(13305, 8531) && !n.inside(13307, 8531));
  CHECK_NEAR(n.edgeDistance(13305, 8531, slippy::tileOriginX(13305, 14) + n.ts / 2, slippy::tileOriginY(13305 - 13305 + 8531, 14) + n.ts / 2), 0.0, 1e-6);
  CHECK_NEAR(n.edgeDistance(13306, 8531, slippy::tileOriginX(13305, 14) + n.ts / 2, slippy::tileOriginY(8531, 14) + n.ts / 2), n.ts / 2, 1e-6);
  std::vector<uint8_t> green(64 * 64 * 4);
  for (size_t i = 0; i < green.size(); i += 4) { green[i] = 20; green[i + 1] = 120; green[i + 2] = 30; green[i + 3] = 255; }
  CHECK(!s2.provideSatRgba(0, 14, 13305, 8531, 64, 64, green.data(), err));   // not requested: refused
  double wx = slippy::tileOriginX(13305, 14) + 10, wy = slippy::tileOriginY(8531, 14) + 10;
  CHECK(s2.finestLayerAt(wx, wy) == -1 && s2.imageryKeyAt(wx, wy) == -1);
  float rgb[3];
  CHECK(!s2.satColor(wx, wy, 4, rgb));
  CHECK(s2.imageryVersion() == 0);

  // ---- client-synthesized index (indeksMedan.ts, a 31 x 11 km corridor like gombong-wns): the near layer's
  // RANGE covers the whole bbox but only tiles within R_LOAD get a mesh; the far layer (z10 here, <= 40 tiles)
  // must be requested eagerly on the first (throttled) check and its meshes must stay whole where no near tile
  // is built — otherwise the ground ends at R_LOAD in the backdrop colour.
  Terrain s4; err.clear();
  Json big = Json::parse(R"({"bbox":[12280000,860000,12311000,871000],
    "dem":[{"dir":"dem","zoom":13,"tx0":6605,"ty0":4270,"nx":9,"ny":6,"px":256,"present":"111111111111111111111111111111111111111111111111111111"},
           {"dir":"dem","zoom":10,"tx0":825,"ty0":533,"nx":2,"ny":2,"px":256,"present":"1111"}],
    "sat":[{"dir":"sat/0","zoom":14,"tx0":13210,"ty0":8541,"nx":18,"ny":10,"px":512,"present":"111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111111"},
           {"dir":"sat/1","zoom":10,"tx0":825,"ty0":533,"nx":2,"ny":2,"px":256,"present":"1111"}]})", &err);
  CHECK_MSG(err.empty(), err);
  CHECK_MSG(s4.loadIndex(big, err), err);
  std::vector<Terrain::TileRef> asked;
  s4.setRequestFn([&](const Terrain::TileRef& t) { asked.push_back(t); });
  for (const Terrain::TileRef& d : s4.demTilesWanted()) CHECK(s4.provideDemRgba(d.z, d.x, d.y, 256, 256, px.data()));
  s4.finishDem();
  vec3 centre = s4.origin().toScene((big["bbox"][0].num + big["bbox"][2].num) / 2, (big["bbox"][1].num + big["bbox"][3].num) / 2, 0);
  s4.checkStreaming(centre, false);   // throttled: REQUESTS_PER_CHECK near tiles + EVERY far tile
  int farAsked = 0, nearAsked = 0;
  for (const Terrain::TileRef& t : asked) { if (t.dir == "sat/1") ++farAsked; else if (t.dir == "sat/0") ++nearAsked; }
  CHECK(farAsked == 4 && nearAsked == terrain::REQUESTS_PER_CHECK);
  CHECK(s4.sat().layers[1].state[0] == SatLayer::Requested && s4.sat().layers[1].state[1] == SatLayer::Requested);
  MeshBuilder fmb;
  int N = 16 * terrain::FAR_CELLS_PER_TILE;   // z10 = 16 z14 tiles per side
  CHECK(s4.farTileGeometry(825, 533, fmb) == N * N);                         // no near tile built: no hole
  CHECK(fmb.vertices.size() == (size_t)(N + 1) * (N + 1) && fmb.indices.size() == (size_t)N * N * 6);
  CHECK_NEAR(fmb.vertices[0].pos.y, 100.5 - s4.dem().demBase, 1e-3);          // uncarved DEM height
  CHECK(!s4.nearTileBuilt(13218, 8545));

  // ---- no terrain: a missing / empty / invalid index degrades to flat ground, nothing indexes into empty layers ----
  Terrain s3;
  Json empty = Json::parse("{}", &err);
  CHECK(!s3.loadIndex(empty, err) && !err.empty());              // bbox missing
  CHECK(!s3.hasTerrain() && s3.streamed());
  Json noLayers = Json::parse(R"({"bbox":[0,0,100,100],"dem":[],"sat":[]})", &err);
  CHECK(!s3.loadIndex(noLayers, err) && err == "index.json: no layers");
  CHECK(!s3.hasTerrain() && s3.dem().layers.empty() && s3.sat().layers.empty());
  Json badLayer = Json::parse(R"({"bbox":[0,0,100,100],"dem":[{"zoom":13,"tx0":1,"ty0":1,"nx":0,"ny":0}],"sat":[]})", &err);
  CHECK(!s3.loadIndex(badLayer, err) && s3.dem().layers.empty());   // the half-built layer list is dropped
  CHECK(s3.demTilesWanted().empty() && s3.demComplete());
  s3.finishDem();
  // A wide corridor's detail grid spans the whole bbox (whoosh: z17 307 x 267 cells for 52 present tiles): the
  // index still loads, a grid over MAX_SAT_CELLS (4M) only empties that layer (slot kept: "sat/<i>" indexing).
  Terrain s5; err.clear();
  std::string wide = R"({"bbox":[12280000,860000,12311000,871000],
    "dem":[{"dir":"dem","zoom":13,"tx0":6605,"ty0":4270,"nx":1,"ny":1,"px":256,"present":"1"}],
    "sat":[{"dir":"sat/0","zoom":14,"tx0":13210,"ty0":8541,"nx":1,"ny":1,"px":512,"present":"1"},
           {"dir":"sat/1","zoom":10,"tx0":825,"ty0":533,"nx":1,"ny":1,"px":256,"present":"1"},
           {"dir":"sat/2","zoom":17,"tx0":105000,"ty0":68000,"nx":307,"ny":267,"px":512,"present":"1"},
           {"dir":"sat/3","zoom":17,"tx0":105000,"ty0":68000,"nx":3000,"ny":3000,"px":512,"present":"1"}]})";
  Json wideIdx = Json::parse(wide, &err);
  CHECK_MSG(err.empty(), err);
  CHECK_MSG(s5.loadIndex(wideIdx, err), err);
  CHECK(s5.hasTerrain() && s5.sat().layers.size() == 4);
  CHECK(s5.sat().layers[2].nx == 307 && s5.sat().layers[2].ny == 267 && s5.sat().layers[2].indexed[0] == 1);
  CHECK(s5.sat().layers[3].nx == 1 && s5.sat().layers[3].ny == 1 && s5.sat().layers[3].indexed[0] == 0);   // 9M cells: emptied
  CHECK_NEAR(s3.origin().ox, 50.0, 1e-9);                          // bbox centre survives as the origin
  CHECK_NEAR(s3.rawHeight(10, 10), 0.0, 1e-6);
  CHECK_NEAR(s3.groundHeight(10, 10), 0.0, 1e-6);                  // flat at rail height (DEM 0)
  CHECK(s3.finestLayerAt(10, 10) == -1 && s3.imageryKeyAt(10, 10) == -1 && !s3.satColor(10, 10, 4, rgb));
  CHECK(!s3.provideDemRgba(13, 1, 1, 256, 256, px.data()));       // no layer takes tiles
  CHECK(!s3.provideSatRgba(0, 14, 1, 1, 64, 64, green.data(), err));
  s3.failTile({"dem", 13, 1, 1}); s3.failTile({"sat/0", 14, 1, 1});   // ignored
  s3.update({0, 0, 0}, 0.1f); s3.prime({0, 0, 0});                    // streaming without layers / GPU: no-op
  CHECK(s3.stats.requested == 0 && s3.stats.nearTiles == 0);
  double bb[4] = {10, 20, 30, 40};
  s3.loadNone(bb);
  CHECK(!s3.hasTerrain() && s3.streamed());
  CHECK_NEAR(s3.origin().ox, 20.0, 1e-9); CHECK_NEAR(s3.origin().oz, 30.0, 1e-9);
  s3.loadNone(nullptr);
  CHECK_NEAR(s3.origin().ox, 0.0, 1e-9);
})
