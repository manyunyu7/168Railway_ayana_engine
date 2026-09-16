// PPKA — the playable dispatcher scene: sim bridge + world modules + camera + input.
#pragma once
#include "engine/app/camera_rig.h"
#include "engine/app/compass.h"
#include "engine/app/world_scene.h"
#include "engine/render/texture_cache.h"
#include "engine/core/fly_camera.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/text.h"
#include "engine/sim/sim_process.h"
#include "examples/ppka/panel_view.h"
#include "examples/ppka/ui.h"
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
  // Per-frame render counters for the capture log: draw calls, culled primitives, hiasan instancing, texture sharing.
  std::string debugRender() const {
    const WorldSceneStats& st = scene_.stats(); const TextureCacheStats tc = textureCacheStats();
    char b[240]; std::snprintf(b, sizeof b, "draws %u culled %u hiasan %u/%u instanced %u lod textures %u unique %u refs %.1f MB (%.1f MB shared)",
                               scene_.renderer().drawCalls, scene_.renderer().culled, st.hiasanInstanced, st.hiasanDrawn, st.hiasanLod, tc.entries, tc.references, tc.bytes / 1e6, tc.bytesShared / 1e6);
    return b;
  }
  std::string debugCamera() const { char b[200]; vec3 e = camEye(); std::snprintf(b, sizeof b, "target %.4f %.4f %.4f eye %.4f %.4f %.4f d %.3f yaw %.5f pitch %.5f clock %.3f", orbit_.target.x, orbit_.target.y, orbit_.target.z, e.x, e.y, e.z, orbit_.distance, orbit_.yaw, orbit_.pitch, sim_.state().clock); return b; }

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
  WorldScene scene_;                // the drawn world (engine/app/world_scene.h); origin/station target live there
  TextRenderer text_;
  OrbitCamera orbit_; FlyCamera fly_; bool useFly_ = false; float targetUp_ = 0;
  // camera modes (§9.1): bebas = orbit_/fly_, others = rig_. Subject = selected train, else subjectId_, else nearest.
  CameraRig rig_; std::string subjectId_;
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
  void applySimState();
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
