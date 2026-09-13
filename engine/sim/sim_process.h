// SimProcess — runs the PPKA TypeScript simulation engine as a child process
// (bridge/sim-bridge.ts under `npx tsx`) and talks JSON-lines over pipes.
// Blocking, one request → one response. See bridge/PROTOCOL.md.
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
};
struct SimTrain {
  std::string id, no, name, consist, state, hold, seg;
  int kelas = 0;
  double x = 0, y = 0;
  float speed = 0, s = 0, heading = 0, length = 0;
  int dir = 1;
  std::vector<SimVehicle> vehicles;
};
struct SimPoint { std::string id, lockedBy; int setting = 0; };
struct SimSignal { std::string id, aspect; };
struct SimLogLine { double time = 0; std::string kind, text; };

struct SimState {
  double clock = 0; int score = 0, violations = 0, pending = 0;
  std::vector<SimTrain> trains;
  std::vector<SimPoint> points;
  std::vector<SimSignal> signals;
  std::vector<SimLogLine> log;       // only lines new since the previous step
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
  const Json& command(const std::string& jsonLine);                  // raw: any command object

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
  double lastMs_ = 0, startupMs_ = 0;
  size_t lastBytes_ = 0;
};

} // namespace eng
