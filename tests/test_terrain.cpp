// Terrain carving math without tiles: brush deltas (world.tanah, bilinear on the 8 m grid) and the
// bridge trough (ground lowered to deck bottom - JBT_RUANG within 7 m, blending to 26 m; never raised);
// then the streaming data path without a GPU: index.json layers, DEM tiles from Terrarium pixels,
// demTilesWanted / failTile / demComplete, and the satellite tile state machine (provide / edge distance).
#include "engine/core/json.h"
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
})
