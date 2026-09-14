#include "examples/ppka/game.h"
#include "engine/world/sun.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace eng {

static vec3 horizontalXZ(vec3 v) { v.y = 0; return v; }

bool Game::init(Window& win, const GameOptions& opt) {
  win_ = &win; opt_ = opt;
  rhi::init();
  rhi::setAnisotropy(8);
  scene_.initGpu();
  std::string err;
  if (!text_.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());

  pushMessage("starting simulation bridge...");
  if (!sim_.start()) { std::fprintf(stderr, "sim: %s\n", sim_.error().c_str()); return false; }
  const Json& loaded = sim_.load(opt.map);
  if (!loaded["ok"].boolOr(false)) { std::fprintf(stderr, "load failed: %s\n", loaded["error"].stringOr("?").c_str()); return false; }

  scene_.setWorld(sim_.world());
  vec3 station = scene_.stationScene();
  if (const char* t = std::getenv("ENG_TARGET")) { double wx, wy; float up = 0; if (std::sscanf(t, "%lf,%lf,%f", &wx, &wy, &up) >= 2) { station = scene_.origin().toScene(wx, wy, 0); targetUp_ = up; } }   // debug: aim at a world point (+ metres above ground)
  scene_.setStationScene(station);
  orbit_.target = station; orbit_.distance = 160; orbit_.pitch = radians(18); orbit_.yaw = radians(35);
  orbit_.near = 1; orbit_.far = 40000; orbit_.fovY = radians(52);
  if (const char* v = std::getenv("ENG_VIEW")) std::sscanf(v, "%f,%f,%f", &orbit_.distance, &orbit_.yaw, &orbit_.pitch);   // debug: dist,yaw,pitch
  fly_.position = station + vec3{0, 30, 120}; fly_.far = 40000;

  if (!buildWorld()) return false;

  const Json& started = sim_.startSession(opt.ai, opt.clock);
  if (!started["ok"].boolOr(false)) { std::fprintf(stderr, "start failed\n"); return false; }
  sim_.step(0); applySimState();
  panel_.init(&sim_.panel());
  scene_.setPanelLayout(&sim_.panel());   // in-world meja boards (station GLB `mejalayan` quads)
  if (std::getenv("ENG_AUTOPANEL")) panel_.visible = true;
  if (const char* m = std::getenv("ENG_MODE")) pemula_ = std::string(m) == "pemula";
  if (const char* f = std::getenv("ENG_FOLLOW")) for (const SimTrain& t : sim_.state().trains) if (t.no == f || t.id == f) subjectId_ = t.id;   // camera subject
  if (const char* c = std::getenv("ENG_CAMERA")) { CamMode cm; if (parseCamMode(c, cm)) setCamMode(cm); else pushMessage(std::string("ENG_CAMERA: unknown mode ") + c); }   // debug: start in a camera mode
  char m[160]; std::snprintf(m, sizeof m, "map %s loaded: %zu nodes, %d trains on line, bridge %.0f ms",
                             opt.map.c_str(), sim_.world()["graph"]["nodes"].size(), (int)sim_.state().trains.size(), sim_.startupMs());
  pushMessage(m);
  ready_ = true;
  return true;
}

// The world itself is built by WorldScene (shared with the web ABI); this only supplies the native file paths.
bool Game::buildWorld() {
  std::string err; const std::string root = ENG_SOURCE_DIR;
  if (!scene_.loadTerrain(root + "/assets/terrain", opt_.map, err)) {
    std::fprintf(stderr, "terrain: %s (run: ./build/mac-debug/fetch_tiles %s)\n", err.c_str(), opt_.map.c_str()); return false;
  }
  // baked OSM city: slug = map name; `bks` shares its geography with the `bekasi` bake (Bekasi Timur–Cibitung)
  std::string city;
  if (!std::getenv("ENG_NO_CITY")) city = root + "/../ppka-wannabe-2/public/kota/" + (opt_.map == "bks" ? "bekasi" : opt_.map) + ".json";   // debug: ENG_NO_CITY=1 skips the city (fps comparison)
  if (!scene_.catalog().load()) std::fprintf(stderr, "catalog: %s\n", scene_.catalog().error().c_str());
  if (!scene_.buildStatic(sim_.world(), opt_.map, root + "/assets/font.efnt", city, [this](const std::string& s) { pushMessage(s); })) return false;
  scene_.buildDecor(sim_.world(), std::getenv("ENG_TEST_GARIS") != nullptr, &sim_.summary());
  {   // the reference corridor atlas (catalog `tekstur.rel1067`, pilot picture) over the procedural one when present
    const AssetCatalog::Options& co = scene_.catalog().options();
    std::string eimg = RailBuilder::prepareAtlas(co.ppkaRoot, co.cacheDir, std::filesystem::path(co.convertExe).parent_path().string());
    if (eimg.empty() || !scene_.rails().loadAtlas(eimg)) pushMessage("rail atlas: procedural (pilot-rel-1067.jpg / imgconv not found)");
  }
  std::printf("%s\n", scene_.stats().summary.c_str());
  for (const Json& o : sim_.world()["hiasan"]["objek"].arr) if (!scene_.catalog().model(o["model"].stringOr(""))) pushMessage("missing model: " + o["model"].stringOr(""));
  // the station target was lifted to the carved ground (and maybe moved by ENG_TEST_GARIS)
  vec3 st = scene_.stationScene(); st.y = scene_.groundScene(st.x, st.z) + targetUp_; scene_.setStationScene(st);
  orbit_.target = st; fly_.position.y = st.y + 30;
  scene_.primeStreaming(st);   // the neighbourhood of the station before the first frame; the rest streams
  { const Terrain::Stats& ts = scene_.terrain().stats; char t[200];
    std::snprintf(t, sizeof t, "terrain streaming: %d near + %d far tiles, %d patches, %d textures resident, %zu trees around the station", ts.nearTiles, ts.farTiles, ts.patches, ts.resident, scene_.trees().stats.trees);
    pushMessage(t); }
  compass_.init([this](float x, float z) { return scene_.groundScene(x, z) + targetUp_; });   // the orbit target rides the ground (+ ENG_TARGET height)
  if (std::getenv("ENG_TERRAIN_DEBUG")) {
    double wx, wy; scene_.origin().toWorld(st, wx, wy);
    const Terrain& terrain = scene_.terrain();
    int li = terrain.finestLayerAt(wx, wy);
    std::printf("origins: game (%.1f, %.1f) terrain (%.1f, %.1f) graph (%.1f, %.1f)\n", scene_.origin().ox, scene_.origin().oz, terrain.origin().ox, terrain.origin().oz, scene_.graph().origin().ox, scene_.graph().origin().oz);
    std::printf("terrain: %d patches (%d detail); station (%.0f, %.0f) on layer %d (z%d @ %.2f m/px)\n", terrain.stats.patches, terrain.stats.detailPatches,
                wx, wy, li, li >= 0 ? terrain.sat().layers[(size_t)li].zoom : 0, li >= 0 ? terrain.sat().layers[(size_t)li].mpp : 0.0);
  }
  return true;
}

void Game::applySimState() {
  const SimState& st = sim_.state();
  scene_.applyState(st, timeScale_);
  TrainVisuals& trains_ = scene_.trains();
  if (const char* f = std::getenv("ENG_FOLLOW")) {   // debug: orbit target tracks a train (number) every frame; ENG_FOLLOW_TAIL = its last car
    for (size_t i = 0; i < trains_.labels().size(); ++i) {
      const TrainLabel& l = trains_.labels()[i]; if (l.no != f && l.id != f) continue;
      orbit_.target = l.anchor - vec3{0, 3.f, 0};
      if (std::getenv("ENG_FOLLOW_TAIL")) { size_t n = 0; for (const SimTrain& t : st.trains) { if (t.no == f || t.id == f) { orbit_.target = trains_.vehicles()[n + t.vehicles.size() - 1].centre + vec3{0, 1.5f, 0}; break; } n += t.vehicles.size(); } }
    }
  }
}

void Game::shutdown() {
  sim_.stop(); compass_.shutdown(); scene_.destroy(); text_.shutdown();
}

void Game::handleInput(Window& win, double dt) {
  auto pressed = [&](int k) { bool now = win.key(k), was = keyPrev_[k]; keyPrev_[k] = now; return now && !was; };
  int fw, fh; win.framebufferSize(fw, fh); int ww, wh; glfwGetWindowSize((GLFWwindow*)win.handle, &ww, &wh);
  double mx, my; win.mousePos(mx, my);
  fmx_ = (float)(mx * fw / ww); fmy_ = (float)(my * fh / wh);

  if (clockPrompt_) {   // modal text entry: digits, backspace, enter, escape
    for (int k = GLFW_KEY_0; k <= GLFW_KEY_9; ++k) if (pressed(k) && clockText_.size() < 5) { if (clockText_.size() == 2) clockText_ += ':'; clockText_ += (char)('0' + k - GLFW_KEY_0); }
    for (int k = GLFW_KEY_KP_0; k <= GLFW_KEY_KP_9; ++k) if (pressed(k) && clockText_.size() < 5) { if (clockText_.size() == 2) clockText_ += ':'; clockText_ += (char)('0' + k - GLFW_KEY_KP_0); }
    if (pressed(GLFW_KEY_BACKSPACE) && !clockText_.empty()) { clockText_.pop_back(); if (!clockText_.empty() && clockText_.back() == ':') clockText_.pop_back(); }
    if (pressed(GLFW_KEY_ENTER) || pressed(GLFW_KEY_KP_ENTER)) { if (clockText_.size() == 5) setClock(clockText_); clockPrompt_ = false; }
    if (pressed(GLFW_KEY_ESCAPE)) clockPrompt_ = false;
  } else {
    if (pressed(GLFW_KEY_SPACE) && rig_.mode != CamMode::Jalan) setPaused(!paused_);   // in jalan Space = jump (the pause button stays)
    if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) setTimeScale(std::fmin(timeScale_ * 2, 64));
    if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) setTimeScale(std::fmax(timeScale_ / 2, 0.5));
    if (pressed(GLFW_KEY_M)) { panel_.visible = !panel_.visible; if (panel_.visible) panel_.fitStation("", fw, fh); }
    if (pressed(GLFW_KEY_P)) { pemula_ = !pemula_; menu_.open = false; pushMessage(pemula_ ? "mode pemula: click a signal to pick a destination" : "mode ahli: click a signal = route along the points"); }
    if (pressed(GLFW_KEY_J)) { clockPrompt_ = true; clockText_.clear(); }
    if (izin_.open) { if (pressed(GLFW_KEY_ENTER) || pressed(GLFW_KEY_KP_ENTER)) confirmIzin(); }
    if (menu_.open) { for (int k = GLFW_KEY_1; k <= GLFW_KEY_9; ++k) if (pressed(k)) chooseRoute(k - GLFW_KEY_1); }
    else { for (int k = GLFW_KEY_1; k <= GLFW_KEY_6; ++k) if (pressed(k)) setCamMode((CamMode)(k - GLFW_KEY_1)); }   // 1 bebas 2 jalan 3 kabin 4 samping 5 atas 6 ekor
    if (pressed(GLFW_KEY_COMMA)) cycleSubject(-1);
    if (pressed(GLFW_KEY_PERIOD)) cycleSubject(1);
    rig_.teropongTahan = win.key(GLFW_KEY_Z);
    if (pressed(GLFW_KEY_F)) { useFly_ = !useFly_; if (useFly_) { fly_.position = orbit_.position(); vec3 d = normalize(orbit_.target - fly_.position); fly_.yaw = std::atan2(-d.x, -d.z); fly_.pitch = std::asin(d.y); } }
    if (pressed(GLFW_KEY_ESCAPE)) {
      if (menu_.open || confirmHapus_ || izin_.open) closeMenus();
      else if (rig_.mode != CamMode::Bebas) setCamMode(CamMode::Bebas);
      else if (!selectedTrain_.empty()) selectedTrain_.clear();
      else glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1);
    }
  }

  float dx = (float)(mx - mxPrev_), dy = (float)(my - myPrev_);
  bool lmb = win.mouseButton(0), rmb = win.mouseButton(1);
  // debug: ENG_AUTOJUMP=1 simulates a short right-click at (30 %, 65 %) of the window at frame 40
  if (std::getenv("ENG_AUTOJUMP") && frame_ >= 40 && frame_ < 43) { mx = ww * 0.3; my = wh * 0.65; rmb = frame_ < 42; }
  // debug: ENG_AUTOGLIDE=1 holds the right button at (50 %, 12 %) from frame 40 on (compass glide forward: terrain streaming)
  if (std::getenv("ENG_AUTOGLIDE") && frame_ >= 40) { mx = ww * 0.5; my = wh * 0.12; rmb = true; }
  if (lmb) {
    if (!dragging_) {
      clickArmed_ = true; clickX_ = mx; clickY_ = my;
      // what the press landed on decides who owns the drag: panel edge (resize), panel (pan), UI (nothing), scene (orbit)
      panelResize_ = panel_.onTopEdge(fmx_, fmy_, fw, fh);
      panelDrag_ = !panelResize_ && panel_.contains(fmx_, fmy_, fw, fh);
      uiPress_ = !panelResize_ && !panelDrag_ && ui_.blocked(fmx_, fmy_);
    } else if (panelResize_) panel_.resizeTo((float)fh - fmy_, fh);
    else if (panelDrag_) { if (!clickArmed_) panel_.pan(dx * (float)fw / (float)ww, dy * (float)fh / (float)wh); }
    else if (uiPress_) {}
    else if (rig_.mode != CamMode::Bebas) { if (!clickArmed_) rig_.drag(dx, dy); } else if (useFly_) fly_.look(dx, dy); else orbit_.rotate(dx, dy);
    if (std::fabs(mx - clickX_) + std::fabs(my - clickY_) > 4) clickArmed_ = false;
    dragging_ = true;
  } else {
    if (dragging_ && clickArmed_ && !panelResize_) { pendingClick_ = true; pcx_ = (float)(clickX_ * fw / ww); pcy_ = (float)(clickY_ * fh / wh); }
    dragging_ = false; clickArmed_ = false; panelDrag_ = panelResize_ = uiPress_ = false;
  }
  if (rig_.mode != CamMode::Bebas) {}
  else if (useFly_) { if (rmb) fly_.look(dx, dy); }
  else {
    bool ctrl = win.key(GLFW_KEY_LEFT_CONTROL) || win.key(GLFW_KEY_RIGHT_CONTROL) || win.key(GLFW_KEY_LEFT_SUPER);
    bool overUi = panel_.contains(fmx_, fmy_, fw, fh) || ui_.blocked(fmx_, fmy_);
    compass_.update(orbit_, mx * fw / ww, my * fh / wh, fw, fh, rmb && !overUi, ctrl,
                    win.key(GLFW_KEY_LEFT), win.key(GLFW_KEY_RIGHT), win.key(GLFW_KEY_UP), win.key(GLFW_KEY_DOWN), (float)dt, viewProj_.inverse());
  }
  mxPrev_ = mx; myPrev_ = my;
  if (win.scroll != 0) {
    if (panel_.contains(fmx_, fmy_, fw, fh)) panel_.zoomAt((float)win.scroll, fmx_, fmy_, fw, fh);
    else if (ui_.blocked(fmx_, fmy_)) {}
    else if (rig_.mode != CamMode::Bebas) rig_.scroll((float)win.scroll); else if (useFly_) fly_.speed *= std::pow(1.2f, (float)win.scroll); else orbit_.zoom((float)win.scroll);
    win.scroll = 0;
  }
  if (useFly_ && rig_.mode == CamMode::Bebas) {
    float f = (win.key(GLFW_KEY_W) ? 1.f : 0.f) - (win.key(GLFW_KEY_S) ? 1.f : 0.f);
    float sd = (win.key(GLFW_KEY_D) ? 1.f : 0.f) - (win.key(GLFW_KEY_A) ? 1.f : 0.f);
    float u = (win.key(GLFW_KEY_E) ? 1.f : 0.f) - (win.key(GLFW_KEY_Q) ? 1.f : 0.f);
    float boost = win.key(GLFW_KEY_LEFT_SHIFT) ? 4.f : 1.f;
    fly_.move(f * boost, sd * boost, u * boost, (float)dt);
  }
}

// ---- player actions (one path for 3D, panel and menus) ----

void Game::clickSignal(const std::string& id) {
  handleRouteResponse(sim_.clickSignal(id), "signal " + id);
}

// Route command outcome: a `needsConfirm` card (ui/rute.ts tawarkanIzin) or a message; then the state refresh.
void Game::handleRouteResponse(const Json& r0, const std::string& what) {
  Json r = r0;   // copy: the next command overwrites the bridge's last response
  if (r["needsConfirm"].isObject()) {
    const Json& c = r["needsConfirm"];
    izin_ = Izin{}; izin_.open = true;
    izin_.kind = c["kind"].stringOr(""); izin_.judul = c["judul"].stringOr("Izin"); izin_.rute = c["rute"].stringOr("");
    izin_.akibat = c["akibat"].stringOr(""); izin_.batas = c["batas"].stringOr(""); izin_.tombol = c["tombol"].stringOr("Beri izin");
    izin_.confirm = c["confirm"]; izin_.until = win_->time() + 12;   // IZIN_DETIK
    pushMessage(what + ": " + izin_.judul + " - lihat kartu keputusan (Enter = " + izin_.tombol + ", Esc = batal)");
  } else {
    std::string msg = what + ": " + (r["ok"].boolOr(false) ? r["status"].stringOr("ok") : "rejected");
    if (r.has("reason")) msg += " (" + r["reason"].stringOr("") + ")";
    if (r.has("action")) msg += " " + r["action"].stringOr("");
    if (r.has("exitLabel")) msg += " -> " + r["exitLabel"].stringOr("");
    if (r["sepurSalah"].boolOr(false)) msg += " SEPUR SALAH"; else if (r["izinTerisi"].boolOr(false)) msg += " SEPUR TERISI";
    pushMessage(msg);
  }
  afterCommand();
}

void Game::confirmIzin() {
  if (!izin_.open) return;
  Json of = izin_.confirm; std::string what = izin_.judul + " (" + izin_.rute + ")";
  izin_.open = false;
  handleRouteResponse(sim_.confirm(of), what);   // may hand back the next card (wrong line, then occupied)
}

void Game::flipPoint(const std::string& id) {
  const Json& r = sim_.flipPoint(id);
  std::string msg = "point " + id + ": " + (r["ok"].boolOr(false) ? "flipped" : "rejected");
  if (r.has("reason")) msg += " (" + r["reason"].stringOr("") + ")";
  pushMessage(msg);
  afterCommand();
}

// Beginner mode (main.ts menuPemula): a routed signal is cancelled by the click; otherwise the
// destination menu opens (candidates from route_menu, wrong-line entries last, blocked ones greyed).
void Game::openRouteMenu(const std::string& sigId, float sx, float sy) {
  const Json& r = sim_.routeMenu(sigId);
  if (!r["ok"].boolOr(false)) { pushMessage("route menu: " + r["error"].stringOr("?")); return; }
  if (!r["manual"].boolOr(true)) { pushMessage(r["name"].stringOr(sigId) + ": not operated manually"); return; }
  if (r["active"].isString()) { clickSignal(sigId); return; }   // click on a routed signal = cancel (both modes)
  menu_ = RouteMenu{}; menu_.open = true; menu_.signal = r["signal"].stringOr(sigId); menu_.name = r["name"].stringOr(sigId);
  menu_.data = r; menu_.x = sx; menu_.y = sy;
  if (r["candidates"].size() == 0) {
    std::string why; for (const Json& p : r["prunes"].arr) why += (why.empty() ? "" : " / ") + p["reason"].stringOr("");
    pushMessage("no route from " + menu_.name + (why.empty() ? "" : " - " + why)); menu_.open = false;
  }
}

void Game::chooseRoute(int index) {
  if (!menu_.open) return;
  const Json& c = menu_.data["candidates"][(size_t)index];
  if (!c.isObject()) return;
  std::string to = c["exitLabel"].stringOr("");
  std::string cmd = "{\"cmd\":\"set_route\",\"from\":\"" + SimProcess::escape(menu_.signal) + "\",\"index\":" + std::to_string(index)
                  + (c["sepurSalah"].boolOr(false) ? ",\"sepurSalah\":true" : "") + "}";
  menu_.open = false;
  handleRouteResponse(sim_.command(cmd), "route " + menu_.name + " -> " + to);
}

void Game::selectTrain(const std::string& id, bool jump) {
  selectedTrain_ = id; confirmHapus_ = false;
  refreshDetail(true);
  if (!jump) return;
  for (const SimTrain& t : sim_.state().trains) if (t.id == id) {
    vec3 p = scene_.origin().toScene(t.x, t.y, 0); p.y = scene_.groundScene(p.x, p.z);
    compass_.jumpTo(orbit_, p);
  }
}

void Game::refreshDetail(bool force) {
  if (selectedTrain_.empty()) return;
  double now = win_ ? win_->time() : 0;
  if (!force && detailAt_ >= 0 && now - detailAt_ < 1.0) return;
  detailAt_ = now;
  detail_ = sim_.trainDetail(selectedTrain_);
  if (!detail_["ok"].boolOr(false)) { selectedTrain_.clear(); detail_ = Json{}; }
}

// The bridge owns the time scale (session.timeScale); the app only sends real seconds to step.
void Game::setTimeScale(double k) { timeScale_ = k; const Json& r = sim_.setTimeScale(k); if (!r["ok"].boolOr(false)) pushMessage("time scale rejected"); }
void Game::setPaused(bool p) { paused_ = p; }

// Web "Set jam" = session restart at that clock (trains on the line vanish, routes drop).
void Game::setClock(const std::string& hhmm) {
  const Json& r = sim_.setClock(hhmm);
  if (!r["ok"].boolOr(false)) { pushMessage("set clock failed: " + r["error"].stringOr("?")); return; }
  selectedTrain_.clear(); menu_.open = false; scene_.routes().clearPreview(); hoverId_.clear();
  pushMessage("session restarted at " + hhmm + " (live trains re-spawned from the GAPEKA)");
  afterCommand();
}

void Game::giveS40(const std::string& id) {
  const Json& r = sim_.beriS40(id);
  pushMessage(r["ok"].boolOr(false) ? "S40 given to KA " + r["no"].stringOr("") : "S40 rejected: " + r["reason"].stringOr(r["error"].stringOr("?")));
  afterCommand();
}

void Game::removeTrain(const std::string& id) {
  const Json& r = sim_.hapusKA(id);
  pushMessage(r["ok"].boolOr(false) ? "KA " + r["no"].stringOr("") + " removed from the line (-2000)" : "remove rejected: " + r["error"].stringOr("?"));
  if (r["ok"].boolOr(false) && selectedTrain_ == id) selectedTrain_.clear();
  confirmHapus_ = false;
  afterCommand();
}

// Screen-space picking (spec §6.5): WorldScene::pickAt with the window -> framebuffer scale.
void Game::pickAt(double mx, double my, int w, int h, std::string& sigId, std::string& ptId) const {
  int ww, wh; glfwGetWindowSize((GLFWwindow*)win_->handle, &ww, &wh);
  scene_.pickAt((float)(mx * w / ww), (float)(my * h / wh), w, h, viewProj_, camEye(), sigId, ptId);
}

// Hover (§6.5): ring + tooltip on the picked object; for an unrouted signal the route preview
// (the path a click would lock) is fetched from the bridge at most every 250 ms.
void Game::updateHover() {
  double mx, my; win_->mousePos(mx, my);
  if (forceHoverX_ >= 0) { mx = forceHoverX_; my = forceHoverY_; }
  std::string sigId, ptId;
  bool onPanel = panel_.contains(fmx_, fmy_, screenW_, screenH_);
  if (!dragging_ && !menu_.open) {
    if (onPanel) { PanelHit h = panel_.pick(fmx_, fmy_, screenW_, screenH_); sigId = h.signalId; ptId = h.pointId; }
    else if (!ui_.blocked(fmx_, fmy_)) pickAt(mx, my, screenW_, screenH_, sigId, ptId);
  }
  std::string id = sigId.empty() ? ptId : sigId;
  RouteVisuals& routes_ = scene_.routes();
  if (id != hoverId_ || onPanel != hoverOnPanel_) { hoverId_ = id; hoverOnPanel_ = onPanel; hoverIsSignal_ = !sigId.empty(); previewAt_ = -1; routes_.clearPreview(); hoverTip_.clear(); hoverAction_.clear(); hoverReject_ = false; }
  if (hoverId_.empty()) return;
  const SimState& st = sim_.state();
  if (hoverIsSignal_) {
    int i = scene_.signals().indexOf(hoverId_); if (i < 0) { hoverId_.clear(); return; }
    const SignalInstance& si = scene_.signals().signals()[(size_t)i];
    hoverPos_ = si.pos;
    const SimRoute* active = nullptr;
    for (const SimRoute& r : st.routes) if (r.entry == hoverId_) active = &r;
    static const char* ASP[3] = {"RED", "YELLOW", "GREEN"};
    std::string head = si.name + "  " + ASP[(int)si.aspect] + " · " + si.signalType;
    if (active) { hoverTip_ = head + "  |  click = cancel route to " + active->exitLabel; hoverReject_ = false; routes_.clearPreview(); previewAt_ = -1; return; }
    if (pemula_) { hoverTip_ = head + "  |  click = choose destination"; return; }
    double now = win_->time();
    if (previewAt_ < 0 || now - previewAt_ > 0.25) {
      previewAt_ = now;
      const Json& r = sim_.preview(hoverId_);
      std::vector<std::string> path; for (const Json& sg : r["path"].arr) path.push_back(sg.stringOr(""));
      bool ok = r["cand"].isObject();
      if (!r["manual"].boolOr(true)) { hoverAction_ = "not operated manually"; hoverReject_ = true; routes_.clearPreview(); }
      else {
        hoverAction_ = ok ? "click = set route to " + r["cand"]["exitLabel"].stringOr("?") : r["reason"].stringOr("no path");
        hoverReject_ = !ok;
        if (!path.empty()) routes_.setPreview(path, !ok); else routes_.clearPreview();
      }
    }
    hoverTip_ = head + (hoverAction_.empty() ? "" : "  |  " + hoverAction_);
  } else {
    const PointInstance* pi = nullptr;
    for (const PointInstance& p : scene_.points().points()) if (p.nodeId == hoverId_) pi = &p;
    if (!pi) { hoverId_.clear(); return; }
    hoverPos_ = pi->pos;
    hoverTip_ = "point " + hoverId_ + "  " + (pi->setting ? "reverse" : "normal") + (pi->locked ? " · locked" : "") + "  |  click = flip";
    hoverReject_ = pi->locked;
  }
}

// 3D click: the same helpers the panel and menus use. Beginner mode opens the destination menu.
void Game::onClick(double mx, double my, int w, int h) {
  std::string sigId, ptId;
  pickAt(mx, my, w, h, sigId, ptId);
  int ww, wh; glfwGetWindowSize((GLFWwindow*)win_->handle, &ww, &wh);
  if (!sigId.empty()) { if (pemula_) openRouteMenu(sigId, (float)(mx * w / ww), (float)(my * h / wh)); else clickSignal(sigId); }
  else if (!ptId.empty()) flipPoint(ptId);
}

void Game::stepSim(double realDt) {
  if (paused_) return;
  // like the web client: one step per frame with REAL seconds (capped at 0.1 s); the session
  // multiplies by its own timeScale (set via set_time_scale) and substeps at <= 0.5 s
  double dt = std::fmin(realDt, 0.1);
  if (dt <= 0) return;
  sim_.step(dt);
  for (const SimLogLine& l : sim_.state().log) pushMessage(l.text);
  applySimState();
  refreshDetail();
}

void Game::render(Window& win) {
  int w, h; win.framebufferSize(w, h); screenW_ = w; screenH_ = h;
  rhi::setViewport(w, h);
  rhi::clear(0, 0, 0, 1);
  if (rig_.mode == CamMode::Bebas && !useFly_) orbit_.keepAboveGround([this](float x, float z) { return scene_.groundScene(x, z); });
  float aspect = (float)w / (float)h;
  mat4 view = camView();
  mat4 proj = camProj(aspect);
  vec3 eye = camEye();
  viewProj_ = proj * view;
  float dt = paused_ ? 0.f : (float)std::fmin(realDt_, 0.1);
  scene_.refDistance = useFly_ && rig_.mode == CamMode::Bebas ? length(fly_.position - orbit_.target) : rig_.acuan(orbit_.distance);
  scene_.cabView = rig_.mode == CamMode::Kabin;
  // per-mode fog (§9.1), exponential-squared matched at the linear midpoint; trains drawn after the hover ring
  scene_.draw(viewProj_, view, eye, camFovY(), h, sim_.state().clock, dt, timeScale_, rig_.fogDensity(scene_.worldWidth()), false);
  updateHover();
  if (!hoverId_.empty() && !hoverOnPanel_) {
    scene_.drawHoverRing(hoverPos_, eye);
    vec4 c = viewProj_ * vec4(hoverPos_ + vec3{0, hoverIsSignal_ ? 4.5f : 3.f, 0}, 1);
    hoverX_ = c.w > 0 ? (c.x / c.w * 0.5f + 0.5f) * (float)w : -1; hoverY_ = c.w > 0 ? (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h : -1;
  }
  { Frustum frustum(viewProj_); scene_.trains().draw(scene_.renderer(), &frustum); }
  if (!useFly_ && rig_.mode == CamMode::Bebas) compass_.draw(scene_.renderer(), orbit_);
  scene_.renderer().flushTransparent();
  drawHud(w, h);
  // the click that no widget took goes to the meja layan or the 3D scene
  if (pendingClick_) {
    pendingClick_ = false;
    if (!ui_.consumed) {
      if (menu_.open) menu_.open = false;
      else if (panel_.contains(pcx_, pcy_, w, h)) {
        PanelHit hit = panel_.pick(pcx_, pcy_, w, h);
        if (!hit.signalId.empty()) { if (pemula_) openRouteMenu(hit.signalId, hit.sx, hit.sy); else clickSignal(hit.signalId); }
        else if (!hit.pointId.empty()) flipPoint(hit.pointId);
      } else if (!ui_.blocked(pcx_, pcy_)) { int ww, wh; glfwGetWindowSize((GLFWwindow*)win.handle, &ww, &wh); onClick(pcx_ * ww / w, pcy_ * wh / h, w, h); }
    }
  }
}

void Game::frame(Window& win, double realDt) {
  fps_ = fps_ * 0.95 + (realDt > 0 ? 1.0 / realDt : 0) * 0.05;
  realDt_ = realDt;
  handleInput(win, realDt);
  // debug: ENG_AUTOCLICK=signal|point clicks the nearest-to-centre visible object at frame 40;
  // ENG_AUTOHOVER=signal|point pins the hover cursor on it from frame 40 on.
  auto nearestToCentre = [&](const char* kind, int& ww, int& wh) {
    int w = screenW_, h = screenH_; glfwGetWindowSize((GLFWwindow*)win.handle, &ww, &wh);
    vec3 eye = camEye();
    std::string k = kind, want;   // "signal" | "point" | "signal:<name>" | "point:<nodeId>"
    if (size_t c = k.find(':'); c != std::string::npos) { want = k.substr(c + 1); k = k.substr(0, c); }
    auto pts = k == "point" ? scene_.points().screenPositions(viewProj_, w, h) : scene_.signals().screenPositions(viewProj_, w, h, eye);
    float best = 1e9f; ScreenPoint hit;
    for (const ScreenPoint& sp : pts) {
      if (!sp.visible) continue;
      if (!want.empty()) { int i = k == "point" ? -1 : scene_.signals().indexOf(sp.id); std::string nm = i >= 0 ? scene_.signals().signals()[(size_t)i].name : sp.id; if (nm != want) continue; }
      float d = std::hypot(sp.x - w / 2.f, sp.y - h / 2.f); if (d < best) { best = d; hit = sp; }
    }
    hit.x = hit.x * (float)ww / (float)w; hit.y = hit.y * (float)wh / (float)h;
    return hit;
  };
  if (const char* ac = std::getenv("ENG_AUTOCLICK"); ac && frame_ == 40) {
    int ww, wh; ScreenPoint hit = nearestToCentre(ac, ww, wh);
    if (!hit.id.empty()) { pushMessage("autoclick " + hit.id); onClick(hit.x, hit.y, screenW_, screenH_); }
  }
  if (const char* ar = std::getenv("ENG_AUTOROUTE"); ar && frame_ == 40) {   // debug: click a signal by name, on/off screen
    const Json& r = sim_.clickSignal(ar);
    pushMessage(std::string("autoroute ") + ar + ": " + (r["ok"].boolOr(false) ? "ok" : "rejected") + " " + r["reason"].stringOr(""));
    sim_.step(0); applySimState();
  }
  if (const char* as = std::getenv("ENG_AUTOSELECT"); as && frame_ == 5) {   // debug: select a train by number (detail card + camera jump)
    for (const SimTrain& t : sim_.state().trains) if (t.no == as || t.id == as) selectTrain(t.id, true);
    if (selectedTrain_.empty()) pushMessage(std::string("autoselect: train ") + as + " not on the line");
  }
  if (const char* am = std::getenv("ENG_AUTOMENU"); am && frame_ == 40) { pemula_ = true; openRouteMenu(am, screenW_ * 0.5f, screenH_ * 0.4f); }   // debug: beginner menu for a signal
  if (const char* ac = std::getenv("ENG_AUTOCHOOSE"); ac && frame_ == 45) chooseRoute(std::atoi(ac));
  if (std::getenv("ENG_AUTOCONFIRM") && frame_ == 50) confirmIzin();   // debug: press the card's button   // debug: pick a menu entry (with ENG_AUTOMENU); a wrong-line entry shows the permission card
  if (const char* ah = std::getenv("ENG_AUTOHOVER"); ah && frame_ == 40) {
    int ww, wh; ScreenPoint hit = nearestToCentre(ah, ww, wh);
    if (!hit.id.empty()) { forceHoverX_ = hit.x; forceHoverY_ = hit.y; }
  }
  stepSim(realDt);
  updateCamera((float)std::fmin(realDt, 0.1));
  scene_.updateStreaming(rig_.mode != CamMode::Bebas ? rig_.look() : useFly_ ? fly_.position : orbit_.target, (float)std::fmin(realDt, 0.1));
  render(win);
  ++frame_;
}

// ---- camera modes (§9.1) ----

vec3 Game::camEye() const { return rig_.mode != CamMode::Bebas ? rig_.eye() : useFly_ ? fly_.position : orbit_.position(); }
mat4 Game::camView() const { return rig_.mode != CamMode::Bebas ? rig_.view() : useFly_ ? fly_.view() : orbit_.view(); }
mat4 Game::camProj(float aspect) const { return rig_.mode != CamMode::Bebas ? rig_.projection(aspect) : useFly_ ? fly_.projection(aspect) : orbit_.projection(aspect); }
float Game::camFovY() const { return rig_.mode != CamMode::Bebas ? radians(rig_.fovDeg()) : useFly_ ? fly_.fovY : orbit_.fovY; }

// Subject train polyline: selected train, else the debug/cycled subject, else the nearest to the camera
// (dunia3d.ts subjek). Points = the vehicles' coupler positions on the rail head, nose first.
bool Game::subjectPath(TrainPath& out, std::string* idOut) const {
  const SimState& st = sim_.state();
  if (st.trains.empty()) return false;
  const SimTrain* t = nullptr;
  for (const SimTrain& x : st.trains) if (!selectedTrain_.empty() && x.id == selectedTrain_) t = &x;
  if (!t) for (const SimTrain& x : st.trains) if (!subjectId_.empty() && x.id == subjectId_) t = &x;
  if (!t) {
    vec3 eye = camEye(); float bd = 1e30f;
    for (const SimTrain& x : st.trains) { vec3 p = scene_.origin().toScene(x.x, x.y, 0); float d = (p.x - eye.x) * (p.x - eye.x) + (p.z - eye.z) * (p.z - eye.z); if (d < bd) { bd = d; t = &x; } }
  }
  if (!t || t->vehicles.empty()) return false;
  out = TrainPath{};
  out.sarana = t->vehicles[0].sarana;
  float acc = 0;
  auto push = [&](double wx, double wy, const std::string& seg, float s) {
    vec3 p = scene_.origin().toScene(wx, wy, scene_.profile().railHeight(seg.c_str(), s));
    if (!out.pts.empty()) acc += length(horizontalXZ(p - out.pts.back()));
    out.pts.push_back(p); out.cum.push_back(acc);
  };
  for (const SimVehicle& v : t->vehicles) { push(v.x1, v.y1, v.seg, v.s); push(v.x2, v.y2, v.seg2, v.s2); }
  out.length = t->length > 0 ? t->length : acc;
  out.id = t->id; out.speed = t->speed;
  if (idOut) *idOut = t->id;
  return out.valid();
}

void Game::setCamMode(CamMode m) {
  if (m == rig_.mode) return;
  TrainPath tp;
  if (m != CamMode::Bebas && m != CamMode::Jalan && !subjectPath(tp)) { pushMessage("Belum ada KA di lintas"); return; }
  vec3 eye = camEye(), look = rig_.mode != CamMode::Bebas ? rig_.look() : useFly_ ? fly_.position + fly_.forward() * 60 : orbit_.target;
  CamMode old = rig_.mode;
  if (m == CamMode::Bebas) {   // hand back to the orbit FROM the current view; out of jalan push the target 60 m ahead
    if (old == CamMode::Jalan) look = eye + normalize(look - eye) * 60;
    vec3 o = eye - look; float d = std::fmax(length(o), 2.f);
    orbit_.target = look; orbit_.distance = d; orbit_.pitch = std::asin(std::clamp(o.y / d, -0.999f, 0.999f)); orbit_.yaw = std::atan2(o.x, o.z);
    useFly_ = false;
  }
  rig_.setMode(m, eye, look);
  auto ground = [this](float x, float z) { return scene_.groundScene(x, z); };
  if (m == CamMode::Jalan) rig_.enterWalk(eye, look, ground);
  const CamProfile p = camProfile(m);
  pushMessage(m == CamMode::Bebas ? "Kamera bebas (orbit)" : m == CamMode::Jalan ? "Jalan-jalan - WASD = jalan, Shift = lari, seret = menoleh, Esc kembali"
              : m == CamMode::Kabin ? "Kamera Kabin - seret = menoleh, WASD/QE = geser mata, scroll = maju/mundur, R = kembali, , . = ganti KA, Z = teropong"
              : std::string("Kamera ") + p.nama + " - seret = mengelilingi KA, scroll = atur, , . = ganti KA, Z = teropong");
}

void Game::cycleSubject(int dir) {
  const SimState& st = sim_.state();
  if (st.trains.empty()) return;
  std::string cur; TrainPath tp; subjectPath(tp, &cur);
  int i = -1; for (size_t k = 0; k < st.trains.size(); ++k) if (st.trains[k].id == cur) i = (int)k;
  const SimTrain& t = st.trains[(size_t)((i + dir + (int)st.trains.size()) % (int)st.trains.size())];
  subjectId_ = t.id; selectedTrain_.clear();
  pushMessage("Subjek kamera: KA " + t.no);
}

void Game::updateCamera(float dt) {
  if (rig_.mode == CamMode::Bebas) { rig_.step(dt, nullptr, {}, {}); return; }
  auto ground = [this](float x, float z) { return scene_.groundScene(x, z); };
  WalkInput in;
  if ((rig_.mode == CamMode::Jalan || rig_.mode == CamMode::Kabin) && !clockPrompt_) {   // jalan = walk, kabin = move the eye
    in.forward = (win_->key(GLFW_KEY_W) || win_->key(GLFW_KEY_UP) ? 1.f : 0.f) - (win_->key(GLFW_KEY_S) || win_->key(GLFW_KEY_DOWN) ? 1.f : 0.f);
    in.side = (win_->key(GLFW_KEY_D) || win_->key(GLFW_KEY_RIGHT) ? 1.f : 0.f) - (win_->key(GLFW_KEY_A) || win_->key(GLFW_KEY_LEFT) ? 1.f : 0.f);
    in.up = (win_->key(GLFW_KEY_E) ? 1.f : 0.f) - (win_->key(GLFW_KEY_Q) ? 1.f : 0.f);
    in.run = win_->key(GLFW_KEY_LEFT_SHIFT) || win_->key(GLFW_KEY_RIGHT_SHIFT);
    in.reset = rig_.mode == CamMode::Kabin && win_->key(GLFW_KEY_R);
    in.jump = rig_.mode == CamMode::Jalan && win_->key(GLFW_KEY_SPACE);
  }
  std::vector<WalkBox> boxes;
  if (rig_.mode == CamMode::Jalan) { scene_.collectWalkBoxes(boxes); in.boxes = &boxes; }
  TrainPath tp; bool has = subjectPath(tp);
  if (has) tp.timeScale = paused_ ? 0 : timeScale_;
  if (!rig_.step(dt, has ? &tp : nullptr, ground, in)) { pushMessage("KA subjek hilang dari lintas - kembali ke kamera bebas"); setCamMode(CamMode::Bebas); }
}

bool Game::wantsCapture(int& frameNo) const { frameNo = frame_; return std::getenv("ENG_CAPTURE") != nullptr; }

} // namespace eng
