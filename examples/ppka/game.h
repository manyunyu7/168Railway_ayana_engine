// PPKA — the playable dispatcher scene: sim bridge + world modules + camera + input.
#pragma once
#include "engine/core/fly_camera.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include "engine/sim/sim_process.h"
#include "engine/world/coords.h"
#include <deque>
#include <string>

namespace eng {

struct GameOptions { std::string map = "mojokerto"; std::string clock = "06:30"; bool ai = true; };

class Game {
public:
  bool init(Window& win, const GameOptions& opt);
  void shutdown();
  void frame(Window& win, double realDt);
  bool wantsCapture(int& frameNo) const;

private:
  void handleInput(Window& win, double dt);
  void stepSim(double realDt);
  void render(Window& win);
  void drawHud(int w, int h);
  void pushMessage(const std::string& s);
  void onClick(double mx, double my, int w, int h);

  Window* win_ = nullptr;
  SimProcess sim_;
  GameOptions opt_;
  WorldOrigin origin_;
  vec3 stationScene_;               // scene position of the (first) station
  ModelRenderer renderer_; Sky sky_; TextRenderer text_; Lighting light_;
  OrbitCamera orbit_; FlyCamera fly_; bool useFly_ = false;
  double timeScale_ = 1; bool paused_ = false; double simAccum_ = 0;
  double mxPrev_ = 0, myPrev_ = 0; bool dragging_ = false, clickArmed_ = false; double clickX_ = 0, clickY_ = 0;
  bool keyPrev_[512] = {};
  std::deque<std::string> messages_;
  int frame_ = 0;
  double fps_ = 0;
  mat4 viewProj_; int screenW_ = 1, screenH_ = 1;
  bool ready_ = false;
  friend class GameWorldAccess;
public:
  // integration points (filled as world modules land)
  struct WorldModules;
  WorldModules* world_ = nullptr;
};

} // namespace eng
