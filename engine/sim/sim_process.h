// SimProcess — runs the PPKA TypeScript simulation engine as a child process
// (bridge/sim-bridge.ts under `npx tsx`) and talks JSON-lines over pipes.
// Blocking, one request → one response. See bridge/PROTOCOL.md.
#pragma once
#include "engine/core/json.h"
#include "engine/sim/sim_state.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace eng {

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
  const Json& confirm(const Json& of);                               // answer a permission card: re-issues needsConfirm.confirm
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

  int pid_ = -1, toChild_ = -1, fromChild_ = -1;
  std::string buf_, err_;
  Json last_, world_, summary_;
  SimState state_;
  PanelLayout panel_;
  double lastMs_ = 0, startupMs_ = 0;
  size_t lastBytes_ = 0;
};

} // namespace eng
