// PPKA — the playable dispatcher scene: sim bridge + world modules + camera + input.
#pragma once
#include "engine/core/fly_camera.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include "engine/sim/sim_process.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/coords.h"
#include "engine/world/point_visual.h"
#include "engine/world/rail_builder.h"
#include "engine/world/rail_profile.h"
#include "engine/world/rolling_stock.h"
#include "engine/world/route_visual.h"
#include "engine/world/signal_visual.h"
#include "engine/world/terrain.h"
#include "engine/world/track_graph.h"
#include "engine/world/train_visual.h"
#include "engine/world/vegetation.h"
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
  // Screen-space pick (§6.5): fills sigId or ptId (one of them) for a window-space cursor position.
  void pickAt(double mx, double my, int w, int h, std::string& sigId, std::string& ptId) const;
  void updateHover();

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
  bool buildWorld();
  void applySimState();
  // world
  TrackGraph graph_; Terrain terrain_; VerticalProfile profile_; RailBuilder rails_;
  SignalVisuals signals_; PointVisuals points_; RouteVisuals routes_;
  AssetCatalog catalog_; RollingStock stock_; TrainVisuals trains_;
  struct Placed { GpuModel* model; mat4 xf; AABB bounds; };
  std::vector<Placed> scenery_;      // hiasan objects (station building etc.)
  Vegetation trees_;
  std::string hoverId_;             // hovered signal id or point node id ("" = none)
  bool hoverIsSignal_ = false; vec3 hoverPos_; float hoverX_ = 0, hoverY_ = 0;
  std::string hoverTip_, hoverAction_; bool hoverReject_ = false;
  double forceHoverX_ = -1, forceHoverY_ = -1;   // debug: ENG_AUTOHOVER pins the cursor on an object
  double previewAt_ = -1;           // last preview request time (s), -1 = none pending for this hover
};

} // namespace eng
