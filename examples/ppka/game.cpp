#include "examples/ppka/game.h"
#include "engine/world/sun.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace eng {

bool Game::init(Window& win, const GameOptions& opt) {
  win_ = &win; opt_ = opt;
  rhi::init();
  rhi::setAnisotropy(8);
  renderer_.init(); sky_.init();
  std::string err;
  if (!text_.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());

  pushMessage("starting simulation bridge...");
  if (!sim_.start()) { std::fprintf(stderr, "sim: %s\n", sim_.error().c_str()); return false; }
  const Json& loaded = sim_.load(opt.map);
  if (!loaded["ok"].boolOr(false)) { std::fprintf(stderr, "load failed: %s\n", loaded["error"].stringOr("?").c_str()); return false; }

  // origin = bbox centre of track nodes (spec §2.2)
  const Json& nodes = sim_.world()["graph"]["nodes"];
  double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
  for (const Json& n : nodes.arr) { double x = n["x"].num, y = n["y"].num; x0 = std::fmin(x0, x); x1 = std::fmax(x1, x); y0 = std::fmin(y0, y); y1 = std::fmax(y1, y); }
  origin_ = {(x0 + x1) / 2, (y0 + y1) / 2};

  // first station scenery object → camera target
  stationScene_ = {};
  for (const Json& s : sim_.world()["scenery"].arr)
    if (s["kind"].stringOr("") == "station") { stationScene_ = origin_.toScene(s["pos"]["x"].num, s["pos"]["y"].num, 0); break; }

  orbit_.target = stationScene_; orbit_.distance = 160; orbit_.pitch = radians(18); orbit_.yaw = radians(35);
  orbit_.near = 1; orbit_.far = 40000; orbit_.fovY = radians(52);
  if (const char* v = std::getenv("ENG_VIEW")) std::sscanf(v, "%f,%f,%f", &orbit_.distance, &orbit_.yaw, &orbit_.pitch);   // debug: dist,yaw,pitch
  fly_.position = stationScene_ + vec3{0, 30, 120}; fly_.far = 40000;

  if (!buildWorld()) return false;

  const Json& started = sim_.startSession(opt.ai, opt.clock);
  if (!started["ok"].boolOr(false)) { std::fprintf(stderr, "start failed\n"); return false; }
  sim_.step(0); applySimState();
  panel_.init(&sim_.panel());
  if (std::getenv("ENG_AUTOPANEL")) panel_.visible = true;
  if (const char* m = std::getenv("ENG_MODE")) pemula_ = std::string(m) == "pemula";
  char m[160]; std::snprintf(m, sizeof m, "map %s loaded: %zu nodes, %d trains on line, bridge %.0f ms",
                             opt.map.c_str(), nodes.size(), (int)sim_.state().trains.size(), sim_.startupMs());
  pushMessage(m);
  ready_ = true;
  return true;
}

bool Game::buildWorld() {
  std::string err; const std::string root = ENG_SOURCE_DIR;
  auto t0 = std::chrono::steady_clock::now();
  const Json& world = sim_.world();
  if (!graph_.fromJson(world, &err)) { std::fprintf(stderr, "graph: %s\n", err.c_str()); return false; }
  if (!terrain_.load(root + "/assets/terrain/" + opt_.map + ".dem", root + "/assets/terrain/" + opt_.map + ".sat", err)) {
    std::fprintf(stderr, "terrain: %s (run: ./build/mac-debug/fetch_tiles %s)\n", err.c_str(), opt_.map.c_str()); return false;
  }
  std::vector<StationZone> stations;
  for (const Json& sc : world["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") stations.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0), 160});
  const float demBase = terrain_.dem().demBase;
  profile_.build(graph_, terrain_.dem(), stations, demBase);
  rails_.build(graph_, profile_, &terrain_.dem(), demBase);
  terrain_.setRails(rails_.samples());
  terrain_.build();
  signals_.build(graph_, profile_, world["trackside"]);
  points_.build(graph_, profile_);
  routes_.init(&graph_, &profile_);
  if (!catalog_.load()) std::fprintf(stderr, "catalog: %s\n", catalog_.error().c_str());
  stock_.init(catalog_); trains_.init(stock_);
  // hiasan objects (spec §5.4): position on carved ground, yaw = rot degrees
  for (const Json& o : world["hiasan"]["objek"].arr) {
    GpuModel* m = catalog_.model(o["model"].stringOr(""));
    if (!m) { pushMessage("missing model: " + o["model"].stringOr("")); continue; }
    double wx = o["x"].numberOr(0), wy = o["y"].numberOr(0);
    vec3 p = origin_.toScene(wx, wy, terrain_.groundHeight(wx, wy) + (float)o["naik"].numberOr(0));
    float yaw = radians((float)o["rot"].numberOr(0)), sc = (float)o["skala"].numberOr(1);
    mat4 norm = RollingStock::normalizeTransform(*m, true);
    mat4 xf = mat4::translation(p) * mat4::rotationY(yaw) * mat4::scale({sc, sc, sc}) * norm;
    scenery_.push_back({m, xf, m->bounds.transformed(xf)});
  }
  // trees from the satellite green mask, kept out of the hiasan footprints (§4.4)
  std::vector<AABB> footprints;
  for (const Placed& p : scenery_) footprints.push_back(p.bounds);
  trees_.build(terrain_, catalog_, footprints);
  compass_.init([this](float x, float z) { return terrain_.groundHeight(x + origin_.ox, z + origin_.oz); });
  // camera height follows the ground at the station
  float gy = terrain_.groundHeight(stationScene_.x + origin_.ox, stationScene_.z + origin_.oz);
  stationScene_.y = gy; orbit_.target = stationScene_; fly_.position.y = gy + 30;
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  char m[240];
  std::snprintf(m, sizeof m, "world built in %.0f ms: %.1f km track, %d points, %zu signals, rails %u tris, terrain %zu tris, %zu hiasan, %zu trees",
                ms, graph_.totalLength() / 1000, graph_.pointCount(), signals_.signals().size(), rails_.stats().tris, terrain_.stats.triangles, scenery_.size(), trees_.stats.trees);
  pushMessage(m);
  if (std::getenv("ENG_TERRAIN_DEBUG")) {
    double wx, wy; origin_.toWorld(stationScene_, wx, wy);
    int li = terrain_.finestLayerAt(wx, wy);
    std::printf("origins: game (%.1f, %.1f) terrain (%.1f, %.1f) graph (%.1f, %.1f)\n", origin_.ox, origin_.oz, terrain_.origin().ox, terrain_.origin().oz, graph_.origin().ox, graph_.origin().oz);
    std::printf("terrain: %d patches (%d detail); station (%.0f, %.0f) on layer %d (z%d @ %.2f m/px)\n", terrain_.stats.patches, terrain_.stats.detailPatches,
                wx, wy, li, li >= 0 ? terrain_.sat().layers[(size_t)li].zoom : 0, li >= 0 ? terrain_.sat().layers[(size_t)li].mpp : 0.0);
  }
  return true;
}

void Game::applySimState() {
  const SimState& st = sim_.state();
  for (const SimSignal& s : st.signals) signals_.setAspect(s.id, s.aspect);
  for (const SimPoint& p : st.points) points_.setState(p.id, p.setting, !p.lockedBy.empty());
  signals_.animate();
  routes_.update(st);
  trains_.update(st, origin_, &profile_);
}

void Game::shutdown() {
  sim_.stop(); compass_.shutdown(); trains_.shutdown(); trees_.destroy(); catalog_.destroy(); rails_.destroy(); terrain_.destroy(); signals_.destroy(); points_.destroy(); routes_.destroy();
  renderer_.shutdown(); sky_.shutdown(); text_.shutdown();
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
    if (pressed(GLFW_KEY_SPACE)) setPaused(!paused_);
    if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) setTimeScale(std::fmin(timeScale_ * 2, 64));
    if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) setTimeScale(std::fmax(timeScale_ / 2, 0.5));
    if (pressed(GLFW_KEY_M)) { panel_.visible = !panel_.visible; if (panel_.visible) panel_.fitStation("", fw, fh); }
    if (pressed(GLFW_KEY_P)) { pemula_ = !pemula_; menu_.open = false; pushMessage(pemula_ ? "mode pemula: click a signal to pick a destination" : "mode ahli: click a signal = route along the points"); }
    if (pressed(GLFW_KEY_J)) { clockPrompt_ = true; clockText_.clear(); }
    if (menu_.open) for (int k = GLFW_KEY_1; k <= GLFW_KEY_9; ++k) if (pressed(k)) chooseRoute(k - GLFW_KEY_1);
    if (pressed(GLFW_KEY_F)) { useFly_ = !useFly_; if (useFly_) { fly_.position = orbit_.position(); vec3 d = normalize(orbit_.target - fly_.position); fly_.yaw = std::atan2(-d.x, -d.z); fly_.pitch = std::asin(d.y); } }
    if (pressed(GLFW_KEY_ESCAPE)) {
      if (menu_.open || confirmHapus_) closeMenus();
      else if (!selectedTrain_.empty()) selectedTrain_.clear();
      else glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1);
    }
  }

  float dx = (float)(mx - mxPrev_), dy = (float)(my - myPrev_);
  bool lmb = win.mouseButton(0), rmb = win.mouseButton(1);
  // debug: ENG_AUTOJUMP=1 simulates a short right-click at (30 %, 65 %) of the window at frame 40
  if (std::getenv("ENG_AUTOJUMP") && frame_ >= 40 && frame_ < 43) { mx = ww * 0.3; my = wh * 0.65; rmb = frame_ < 42; }
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
    else if (useFly_) fly_.look(dx, dy); else orbit_.rotate(dx, dy);
    if (std::fabs(mx - clickX_) + std::fabs(my - clickY_) > 4) clickArmed_ = false;
    dragging_ = true;
  } else {
    if (dragging_ && clickArmed_ && !panelResize_) { pendingClick_ = true; pcx_ = (float)(clickX_ * fw / ww); pcy_ = (float)(clickY_ * fh / wh); }
    dragging_ = false; clickArmed_ = false; panelDrag_ = panelResize_ = uiPress_ = false;
  }
  if (useFly_) { if (rmb) fly_.look(dx, dy); }
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
    else if (useFly_) fly_.speed *= std::pow(1.2f, (float)win.scroll); else orbit_.zoom((float)win.scroll);
    win.scroll = 0;
  }
  if (useFly_) {
    float f = (win.key(GLFW_KEY_W) ? 1.f : 0.f) - (win.key(GLFW_KEY_S) ? 1.f : 0.f);
    float sd = (win.key(GLFW_KEY_D) ? 1.f : 0.f) - (win.key(GLFW_KEY_A) ? 1.f : 0.f);
    float u = (win.key(GLFW_KEY_E) ? 1.f : 0.f) - (win.key(GLFW_KEY_Q) ? 1.f : 0.f);
    float boost = win.key(GLFW_KEY_LEFT_SHIFT) ? 4.f : 1.f;
    fly_.move(f * boost, sd * boost, u * boost, (float)dt);
  }
}

// ---- player actions (one path for 3D, panel and menus) ----

void Game::clickSignal(const std::string& id) {
  const Json& r = sim_.clickSignal(id);
  std::string msg = "signal " + id + ": " + (r["ok"].boolOr(false) ? "ok" : "rejected");
  if (r.has("reason")) msg += " (" + r["reason"].stringOr("") + ")";
  if (r.has("action")) msg += " " + r["action"].stringOr("");
  if (r.has("exitLabel")) msg += " -> " + r["exitLabel"].stringOr("");
  pushMessage(msg);
  afterCommand();
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
  const Json& r = sim_.command(cmd);
  std::string msg = "route " + menu_.name + " -> " + to + ": " + (r["ok"].boolOr(false) ? r["status"].stringOr("ok") : "rejected");
  if (r.has("reason")) msg += " (" + r["reason"].stringOr("") + ")";
  pushMessage(msg);
  menu_.open = false;
  afterCommand();
}

void Game::selectTrain(const std::string& id, bool jump) {
  selectedTrain_ = id; confirmHapus_ = false;
  refreshDetail(true);
  if (!jump) return;
  for (const SimTrain& t : sim_.state().trains) if (t.id == id) {
    vec3 p = origin_.toScene(t.x, t.y, 0); p.y = terrain_.groundHeight(p.x + origin_.ox, p.z + origin_.oz);
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
  selectedTrain_.clear(); menu_.open = false; routes_.clearPreview(); hoverId_.clear();
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

// Screen-space picking (spec §6.5): nearest signal within 26 px wins over a point within 40 px
// unless the point is closer and the signal is farther than 18 px.
void Game::pickAt(double mx, double my, int w, int h, std::string& sigId, std::string& ptId) const {
  int ww, wh; glfwGetWindowSize((GLFWwindow*)win_->handle, &ww, &wh);
  float px = (float)(mx * w / ww), py = (float)(my * h / wh);
  vec3 eye = useFly_ ? fly_.position : orbit_.position();
  std::string bs, bp; float ds = 1e9f, dw = 1e9f;
  for (const ScreenPoint& sp : signals_.screenPositions(viewProj_, w, h, eye)) {
    if (!sp.visible) continue; float d = std::hypot(sp.x - px, sp.y - py); if (d < ds) { ds = d; bs = sp.id; }
  }
  for (const ScreenPoint& sp : points_.screenPositions(viewProj_, w, h)) {
    if (!sp.visible) continue; float d = std::hypot(sp.x - px, sp.y - py); if (d < dw) { dw = d; bp = sp.id; }
  }
  bool hitSig = ds <= 26, hitPt = dw <= 40;
  sigId.clear(); ptId.clear();
  if (hitSig && (!hitPt || ds < dw || ds < 18)) sigId = bs; else if (hitPt) ptId = bp;
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
  if (id != hoverId_ || onPanel != hoverOnPanel_) { hoverId_ = id; hoverOnPanel_ = onPanel; hoverIsSignal_ = !sigId.empty(); previewAt_ = -1; routes_.clearPreview(); hoverTip_.clear(); hoverAction_.clear(); hoverReject_ = false; }
  if (hoverId_.empty()) return;
  const SimState& st = sim_.state();
  if (hoverIsSignal_) {
    int i = signals_.indexOf(hoverId_); if (i < 0) { hoverId_.clear(); return; }
    const SignalInstance& si = signals_.signals()[(size_t)i];
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
    for (const PointInstance& p : points_.points()) if (p.nodeId == hoverId_) pi = &p;
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
  float aspect = (float)w / (float)h;
  mat4 view = useFly_ ? fly_.view() : orbit_.view();
  mat4 proj = useFly_ ? fly_.projection(aspect) : orbit_.projection(aspect);
  vec3 eye = useFly_ ? fly_.position : orbit_.position();
  viewProj_ = proj * view;
  { double lon, lat; worldToLonLat(origin_.ox, origin_.oz, lon, lat); applySun(sim_.state().clock, lon, lat, light_, sky_); }
  sky_.draw(viewProj_.inverse(), eye);
  renderer_.beginFrame(viewProj_, eye, light_);
  Frustum frustum(viewProj_);
  terrain_.draw(renderer_, &frustum);
  trees_.draw(renderer_, eye, &frustum);
  rails_.draw(renderer_, &frustum);
  for (const Placed& p : scenery_) if (frustum.contains(p.bounds)) renderer_.draw(*p.model, p.xf, &frustum);
  { double hh = std::fmod(sim_.state().clock / 3600.0, 24.0); signals_.setView(useFly_ ? fly_.fovY : orbit_.fovY, h, hh < 6 || hh >= 18); }
  signals_.draw(renderer_, eye, &frustum);
  points_.draw(renderer_, &frustum);
  updateHover();
  routes_.draw(renderer_);   // blended ribbons before the trains' own transparent parts
  if (!hoverId_.empty() && !hoverOnPanel_) {
    routes_.drawHoverRing(renderer_, hoverPos_, std::max(1.f, length(hoverPos_ - eye) / 46));
    vec4 c = viewProj_ * vec4(hoverPos_ + vec3{0, hoverIsSignal_ ? 4.5f : 3.f, 0}, 1);
    hoverX_ = c.w > 0 ? (c.x / c.w * 0.5f + 0.5f) * (float)w : -1; hoverY_ = c.w > 0 ? (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h : -1;
  }
  trains_.draw(renderer_, &frustum);
  if (!useFly_) compass_.draw(renderer_, orbit_);
  renderer_.flushTransparent();
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
  handleInput(win, realDt);
  // debug: ENG_AUTOCLICK=signal|point clicks the nearest-to-centre visible object at frame 40;
  // ENG_AUTOHOVER=signal|point pins the hover cursor on it from frame 40 on.
  auto nearestToCentre = [&](const char* kind, int& ww, int& wh) {
    int w = screenW_, h = screenH_; glfwGetWindowSize((GLFWwindow*)win.handle, &ww, &wh);
    vec3 eye = useFly_ ? fly_.position : orbit_.position();
    std::string k = kind, want;   // "signal" | "point" | "signal:<name>" | "point:<nodeId>"
    if (size_t c = k.find(':'); c != std::string::npos) { want = k.substr(c + 1); k = k.substr(0, c); }
    auto pts = k == "point" ? points_.screenPositions(viewProj_, w, h) : signals_.screenPositions(viewProj_, w, h, eye);
    float best = 1e9f; ScreenPoint hit;
    for (const ScreenPoint& sp : pts) {
      if (!sp.visible) continue;
      if (!want.empty()) { int i = k == "point" ? -1 : signals_.indexOf(sp.id); std::string nm = i >= 0 ? signals_.signals()[(size_t)i].name : sp.id; if (nm != want) continue; }
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
  if (const char* ah = std::getenv("ENG_AUTOHOVER"); ah && frame_ == 40) {
    int ww, wh; ScreenPoint hit = nearestToCentre(ah, ww, wh);
    if (!hit.id.empty()) { forceHoverX_ = hit.x; forceHoverY_ = hit.y; }
  }
  stepSim(realDt);
  render(win);
  ++frame_;
}

bool Game::wantsCapture(int& frameNo) const { frameNo = frame_; return std::getenv("ENG_CAPTURE") != nullptr; }

} // namespace eng
