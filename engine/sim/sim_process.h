// SimProcess — runs the PPKA TypeScript simulation engine as a child process
// (bridge/sim-bridge.ts under `npx tsx`) and talks JSON-lines over pipes.
// Blocking, one request → one response. See bridge/PROTOCOL.md.
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

class SimProcess {
public:
  struct Options {
    std::string ppkaRoot;     // default: $PPKA_ROOT, else <engine repo>/../ppka-wannabe-2
    std::string bridgeScript; // default: <engine repo>/bridge/sim-bridge.ts
    bool inheritStderr;       // false = bridge stderr to /dev/null
    Options() : inheritStderr(true) {}
  };

  ~SimProcess();
  SimProcess() = default;
  SimProcess(const SimProcess&) = delete;
  SimProcess& operator=(const SimProcess&) = delete;

  // Spawns the bridge and waits for its {"ready":true} line. false + error() on failure.
  bool start(const Options& opt = Options());
  void stop();                        // sends quit, closes pipes, reaps the child
  bool running() const { return pid_ > 0; }
  const std::string& error() const { return err_; }

  // Typed commands. Each returns the parsed response (also kept in lastResponse()).
  // `ok` of a response is response()["ok"].boolOr(false).
  const Json& load(const std::string& map);                          // static world in ["world"], ["summary"]
  const Json& startSession(bool ai, const std::string& clock = "");  // clock "HH:MM" or "" = save default
  const Json& step(double dtSeconds);                                // fills state()
  const Json& clickSignal(const std::string& id);
  const Json& flipPoint(const std::string& id);
  const Json& setRoute(const std::string& from, const std::string& to);
  const Json& cancelRoute(const std::string& signalId);
  const Json& routes(const std::string& signalId);
  const Json& preview(const std::string& signalId);                  // hover preview: path along current points
  const Json& command(const std::string& jsonLine);                  // raw: any command object
  // Player-facing commands mirroring the web UI (see PROTOCOL.md).
  const Json& setTimeScale(double k);                                // session.timeScale (bridge is authoritative; send real dt to step)
  const Json& setClock(const std::string& hhmm);                     // web "Set jam": session restarted at that clock
  const Json& beriS40(const std::string& train);                     // Semboyan 40 (train id or number)
  const Json& hapusKA(const std::string& train);                     // force-remove a train (-2000)
  const Json& trainDetail(const std::string& train);                 // train sheet payload
  const Json& routeMenu(const std::string& signal);                  // beginner-mode destinations
  const PanelLayout& panel();                                        // schematic layout (cached after first call)

  const Json& lastResponse() const { return last_; }
  const SimState& state() const { return state_; }                   // from the last step()
  const Json& world() const { return world_; }                       // raw save, from the last load()
  const Json& summary() const { return summary_; }                   // pre-sampled geometry, from the last load()
  // NOTE: references returned by the command methods point into lastResponse() and are
  // overwritten by the next command; copy what you need to keep.

  // Instrumentation for the most recent command().
  double lastRoundTripMs() const { return lastMs_; }
  size_t lastResponseBytes() const { return lastBytes_; }
  double startupMs() const { return startupMs_; }

  static std::string escape(const std::string& s);                   // JSON string escaping

private:
  bool writeLine(const std::string& line);
  bool readLine(std::string& out);
  void parseState(const Json& j);

  int pid_ = -1, toChild_ = -1, fromChild_ = -1;
  std::string buf_, err_;
  Json last_, world_, summary_;
  SimState state_;
  PanelLayout panel_;
  double lastMs_ = 0, startupMs_ = 0;
  size_t lastBytes_ = 0;
};

} // namespace eng
