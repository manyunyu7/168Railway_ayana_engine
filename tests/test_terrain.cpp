// Terrain carving math without tiles: brush deltas (world.tanah, bilinear on the 8 m grid) and the
// bridge trough (ground lowered to deck bottom - JBT_RUANG within 7 m, blending to 26 m; never raised).
#include "engine/core/json.h"
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
})
