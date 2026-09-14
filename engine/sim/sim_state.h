// SimState — the dynamic simulation state (the bridge's `step` response, bridge/PROTOCOL.md) as plain
// structs, plus the parser. Shared by the native SimProcess (pipes) and the web build (the page hands the
// same JSON object to eng_set_state), so this file has no process code.
#pragma once
#include "engine/core/json.h"
#include <string>
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
struct SimRoute { std::string id, entry, exit, exitLabel; std::vector<std::string> segs, released; };
struct SimJpl { std::string id; bool closed = false; };   // level crossing barrier state (World.jplClosed)
struct SimOccupancy { struct Interval { std::string train; float a = 0, b = 0; }; std::string seg; std::vector<Interval> intervals; };

struct SimState {
  double clock = 0; int score = 0, violations = 0, pending = 0;
  std::vector<SimTrain> trains;
  std::vector<SimPoint> points;
  std::vector<SimSignal> signals;
  std::vector<SimRoute> routes;          // active routes (segs minus released = still locked)
  std::vector<SimOccupancy> occupancy;   // only segments with an interval
  std::vector<SimJpl> jpl;               // every scenery `jpl`, re-evaluated by the bridge every 0.25 s of sim time
  std::vector<SimLogLine> log;       // only lines new since the previous step
};

// Fills a SimState from a `step`/`state` response object (missing fields keep their defaults).
SimState parseSimState(const Json& j);

} // namespace eng
