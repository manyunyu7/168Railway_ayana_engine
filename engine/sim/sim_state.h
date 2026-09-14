// SimState — the dynamic simulation state (the bridge's `step` response, bridge/PROTOCOL.md) as plain
// structs, plus the parser. Shared by the native SimProcess (pipes) and the web build (the page hands the
// same JSON object to eng_set_state), so this file has no process code.
#pragma once
#include "engine/core/json.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {

// Coordinates are double: Web Mercator maps sit at ~1.2e7 m where float resolution is ~1 m.
struct SimVehicle {
  std::string model, sarana, kind, seg, seg2;
  double x = 0, y = 0;            // midpoint between the couplers
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;   // front / rear coupler (world XY)
  float heading = 0, length = 0, s = 0, s2 = 0;
  float t1 = 0, t2 = 0;           // track tangent angle (world XY) at the front / rear coupler
};
struct SimTrain {
  std::string id, no, name, consist, state, hold, seg;
  int kelas = 0;
  double x = 0, y = 0;
  float speed = 0, s = 0, heading = 0, length = 0;
  int dir = 1;
  bool tungguS40 = false, s40Siap = false;   // Semboyan 40 gate: waiting / button may be offered
  bool istirahat = false;                    // parked consist waiting for a later departure (lights off)
  std::vector<SimVehicle> vehicles;
};
struct SimPoint { std::string id, lockedBy; int setting = 0; };
struct SimSignal { std::string id, aspect; };
struct SimLogLine { double time = 0; std::string kind, text; };
struct SimRoute {
  std::string id, entry, exit, exitLabel; std::vector<std::string> segs, released;
  std::vector<std::pair<std::string, std::string>> junctions;   // (point node id, leg segment the route takes)
};
// Shunting plan (putarLok overlay): the legs a detached loco will run; `kendali` "auto" (legIdx = the active leg) or "manual".
struct SimLangsir { std::string lok, kendali; int legIdx = 0; std::vector<std::vector<std::string>> legs; };
struct SimJpl { std::string id; bool closed = false; };   // level crossing barrier state (World.jplClosed)
struct SimOccupancy { struct Interval { std::string train; float a = 0, b = 0; }; std::string seg; std::vector<Interval> intervals; };

struct SimState {
  double clock = 0; int score = 0, violations = 0, pending = 0;
  std::vector<SimTrain> trains;
  std::vector<SimPoint> points;
  std::vector<SimSignal> signals;
  std::vector<SimRoute> routes;          // active routes (segs minus released = still locked)
  std::vector<SimOccupancy> occupancy;   // only segments with an interval
  std::vector<SimLangsir> langsir;       // live shunting plans (yellow dashed ribbons)
  std::vector<SimJpl> jpl;               // every scenery `jpl`, re-evaluated by the bridge every 0.25 s of sim time
  std::vector<SimLogLine> log;       // only lines new since the previous step
};

// Fills a SimState from a `step`/`state` response object (missing fields keep their defaults).
SimState parseSimState(const Json& j);

// Schematic control table ("meja layan", engine/panel.ts PanelLayout). Panel units; the web
// draws y scaled by `yScale` (3). Static per loaded world — fetched once with panel().
struct PanelSeg { std::string id, a, b, sepur; int jalur = 0; float len = 0; std::vector<float> pts; std::vector<float> cum; };   // pts = x0,y0,x1,y1,...; cum = metres at each vertex
struct PanelPoint { std::string id, facing; std::string legs[2]; float x = 0, y = 0; };
struct PanelObj { std::string id, kind, name, seg, signalType, station; float s = 0, x = 0, y = 0, tx = 1, ty = 0; int dir = 1, lampu = 3, jalur = 0; };
struct PanelStation { std::string code, label; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
struct PanelJalur { std::string station; int n = 0; float x = 0, y = 0; };
struct PanelLayout {
  bool ok = false; float yScale = 3; float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // bbox
  std::vector<PanelSeg> segments; std::vector<PanelPoint> points;
  std::vector<PanelObj> signals, berths, portals;
  std::vector<PanelStation> stations; std::vector<PanelJalur> jalur;
  const PanelSeg* seg(const std::string& id) const { auto it = segIndex.find(id); return it == segIndex.end() ? nullptr : &segments[it->second]; }
  // Panel position + tangent at `s` metres along a segment (interpolates the schematic polyline).
  bool posOnSeg(const std::string& seg, float s, float& x, float& y, float& tx, float& ty) const;
  std::unordered_map<std::string, size_t> segIndex;
};

// bridge `panel` JSON (sim-state.ts panelLayoutJson) -> PanelLayout; `ok` false when the object is not a panel.
PanelLayout parsePanelLayout(const Json& j);

} // namespace eng
