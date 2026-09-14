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
#include "engine/world/board_visual.h"
#include "engine/world/city_visual.h"
#include "engine/world/cloud_visual.h"
#include "engine/world/jpl_visual.h"
#include "engine/world/coords.h"
#include "engine/world/garis_visual.h"
#include "engine/world/point_visual.h"
#include "engine/world/rail_builder.h"
#include "engine/world/rail_profile.h"
#include "engine/world/rolling_stock.h"
#include "engine/world/route_visual.h"
#include "engine/world/signal_visual.h"
#include "engine/world/terrain.h"
#include "engine/world/track_graph.h"
#include "engine/world/train_visual.h"
#include "examples/ppka/camera_rig.h"
#include "examples/ppka/compass.h"
#include "examples/ppka/panel_view.h"
#include "examples/ppka/ui.h"
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
  void drawUi(int w, int h);           // top bar, train list + detail card, route menu, clock prompt
  void pushMessage(const std::string& s);
  void onClick(double mx, double my, int w, int h);
  // Player actions — the single path for 3D clicks, panel clicks and menus: sim command → applySimState.
  void clickSignal(const std::string& id);   // expert mode: set along the points / cancel
  void flipPoint(const std::string& id);
  void openRouteMenu(const std::string& sigId, float sx, float sy);   // beginner mode: destinations
  void chooseRoute(int index);
  void closeMenus() { menu_.open = false; clockPrompt_ = false; confirmHapus_ = false; izin_.open = false; }
  void selectTrain(const std::string& id, bool jump);
  void refreshDetail(bool force = false);
  void setTimeScale(double k);
  void setPaused(bool p);
  void setClock(const std::string& hhmm);
  void giveS40(const std::string& id);
  void removeTrain(const std::string& id);
  void afterCommand() { sim_.step(0); applySimState(); previewAt_ = -1; refreshDetail(true); }
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
  // camera modes (§9.1): bebas = orbit_/fly_, others = rig_. Subject = selected train, else subjectId_, else nearest.
  CameraRig rig_; std::string subjectId_; float worldW_ = 8000;
  void setCamMode(CamMode m);
  void cycleSubject(int dir);
  bool subjectPath(TrainPath& out, std::string* idOut = nullptr) const;
  void updateCamera(float dt);
  vec3 camEye() const;
  mat4 camView() const;
  mat4 camProj(float aspect) const;
  float camFovY() const;
  double timeScale_ = 1; bool paused_ = false; double simAccum_ = 0;
  double mxPrev_ = 0, myPrev_ = 0; bool dragging_ = false, clickArmed_ = false; double clickX_ = 0, clickY_ = 0;
  bool keyPrev_[512] = {};
  std::deque<std::string> messages_;
  int frame_ = 0;
  double fps_ = 0, realDt_ = 0;
  mat4 viewProj_; int screenW_ = 1, screenH_ = 1;
  bool ready_ = false;
  bool buildWorld();
  void injectTestGaris(Json& hiasan);   // debug: ENG_TEST_GARIS
  void applySimState();
  // world
  TrackGraph graph_; Terrain terrain_; VerticalProfile profile_; RailBuilder rails_;
  SignalVisuals signals_; PointVisuals points_; RouteVisuals routes_;
  TracksideBoards boards_; JplVisuals jpl_; CityVisuals city_; GarisVisuals garis_; CloudVisual clouds_;
  AssetCatalog catalog_; RollingStock stock_; TrainVisuals trains_;
  struct Placed { GpuModel* model; mat4 xf; AABB bounds; };
  std::vector<Placed> scenery_;      // hiasan objects (station building etc.)
  Vegetation trees_;
  std::string hoverId_;
  Compass compass_;             // hovered signal id or point node id ("" = none)
  bool hoverIsSignal_ = false; vec3 hoverPos_; float hoverX_ = 0, hoverY_ = 0;
  std::string hoverTip_, hoverAction_; bool hoverReject_ = false;
  double forceHoverX_ = -1, forceHoverY_ = -1;   // debug: ENG_AUTOHOVER pins the cursor on an object
  double previewAt_ = -1;           // last preview request time (s), -1 = none pending for this hover
  // ---- player UI (immediate mode) ----
  PanelView panel_; Ui ui_;
  bool pemula_ = false;             // route mode: beginner (menu of destinations) vs expert (click = trace along points)
  bool hoverOnPanel_ = false;       // hover/tooltip refer to the meja layan, not the 3D scene
  float fmx_ = 0, fmy_ = 0;         // cursor in framebuffer px
  bool pendingClick_ = false; float pcx_ = 0, pcy_ = 0;   // click waiting for the UI pass (framebuffer px)
  bool panelDrag_ = false, panelResize_ = false, uiPress_ = false;
  std::string selectedTrain_; Json detail_; double detailAt_ = -1; bool confirmHapus_ = false;
  struct RouteMenu { bool open = false; std::string signal, name; Json data; std::vector<std::string> lit; float x = 0, y = 0; } menu_;
  bool clockPrompt_ = false; std::string clockText_;
  // permission card (ui/rute.ts tawarkanIzin): shown when a route command returns needsConfirm
  struct Izin { bool open = false; std::string kind, judul, rute, akibat, batas, tombol; Json confirm; double until = 0; } izin_;
  void handleRouteResponse(const Json& r, const std::string& what);
  void confirmIzin();
  void drawIzinCard(int w, int h);
};

} // namespace eng
