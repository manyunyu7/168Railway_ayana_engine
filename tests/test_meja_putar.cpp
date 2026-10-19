// Turntables (meja_putar_visual): the bridge segment is found by the same rule as the sim (both ends on the
// rim, midpoint at the pivot) and handed to RailBuilder::skipSegs; the step state carries `meja` angles and
// the per-vehicle `balik` flag (facing-sensitive loco running reversed). No GL: build() is not called.
#include "engine/sim/sim_state.h"
#include "engine/world/meja_putar_visual.h"
#include "tests/check.h"

using namespace eng;

TEST_MAIN({
  std::string err;
  // straight spur A —90 m— R1 —20 m bridge— R2 —90 m— B, pivot at x = 100, diameter 20
  Json world = Json::parse(R"({
    "graph": {
      "nodes": [{"id":"A","x":0,"y":0},{"id":"R1","x":90,"y":0},{"id":"R2","x":110,"y":0},{"id":"B","x":200,"y":0}],
      "segments": [{"id":"s1","a":"A","b":"R1","straight":true},{"id":"s2","a":"R1","b":"R2","straight":true},
                   {"id":"s3","a":"R2","b":"B","straight":true}]
    },
    "scenery": [{"id":"m1","kind":"mejaPutar","pos":{"x":100,"y":0},"w":20,"h":20},
                {"id":"j1","kind":"jpl","pos":{"x":40,"y":0},"w":14,"h":14}]
  })", &err);
  CHECK_MSG(err.empty(), err);
  TrackGraph g; CHECK(g.fromJson(world, &err));
  std::set<std::string> seg = MejaPutarVisuals::bridgeSegments(g, world);
  CHECK(seg.size() == 1); CHECK(seg.count("s2") == 1);
  // a turntable whose pivot is off the track has no bridge
  Json lain = Json::parse(R"({"graph":{"nodes":[{"id":"A","x":0,"y":0},{"id":"B","x":200,"y":0}],
    "segments":[{"id":"s1","a":"A","b":"B","straight":true}]},
    "scenery":[{"id":"m1","kind":"mejaPutar","pos":{"x":100,"y":30},"w":20}]})", &err);
  TrackGraph g2; CHECK(g2.fromJson(lain, &err));
  CHECK(MejaPutarVisuals::bridgeSegments(g2, lain).empty());

  // step state: meja angles + vehicle balik (absent = false)
  SimState st = parseSimState(Json::parse(R"({"clock":10,"trains":[{"id":"t1","vehicles":[
      {"model":"cc201","kind":"loco","balik":true},{"model":"k3","kind":"car"}]}],
    "meja":[{"id":"m1","angle":1.5707}]})", &err));
  CHECK(st.trains.size() == 1); CHECK(st.trains[0].vehicles.size() == 2);
  CHECK(st.trains[0].vehicles[0].balik); CHECK(!st.trains[0].vehicles[1].balik);
  CHECK(st.meja.size() == 1); CHECK(st.meja[0].id == "m1"); CHECK_NEAR(st.meja[0].angle, 1.5707, 1e-4);
})
