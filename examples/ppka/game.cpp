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
  sim_.stop(); trains_.shutdown(); trees_.destroy(); catalog_.destroy(); rails_.destroy(); terrain_.destroy(); signals_.destroy(); points_.destroy(); routes_.destroy();
  renderer_.shutdown(); sky_.shutdown(); text_.shutdown();
}

void Game::handleInput(Window& win, double dt) {
  auto pressed = [&](int k) { bool now = win.key(k), was = keyPrev_[k]; keyPrev_[k] = now; return now && !was; };
  if (pressed(GLFW_KEY_SPACE)) paused_ = !paused_;
  if (pressed(GLFW_KEY_EQUAL) || pressed(GLFW_KEY_KP_ADD)) timeScale_ = std::fmin(timeScale_ * 2, 64);
  if (pressed(GLFW_KEY_MINUS) || pressed(GLFW_KEY_KP_SUBTRACT)) timeScale_ = std::fmax(timeScale_ / 2, 0.25);
  if (pressed(GLFW_KEY_F)) { useFly_ = !useFly_; if (useFly_) { fly_.position = orbit_.position(); vec3 d = normalize(orbit_.target - fly_.position); fly_.yaw = std::atan2(-d.x, -d.z); fly_.pitch = std::asin(d.y); } }
  if (pressed(GLFW_KEY_ESCAPE)) glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1);

  double mx, my; win.mousePos(mx, my);
  float dx = (float)(mx - mxPrev_), dy = (float)(my - myPrev_);
  bool lmb = win.mouseButton(0), rmb = win.mouseButton(1);
  if (lmb) {
    if (!dragging_) { clickArmed_ = true; clickX_ = mx; clickY_ = my; }
    else if (useFly_) fly_.look(dx, dy); else orbit_.rotate(dx, dy);
    if (std::fabs(mx - clickX_) + std::fabs(my - clickY_) > 4) clickArmed_ = false;
    dragging_ = true;
  } else {
    if (dragging_ && clickArmed_) { int w, h; win.framebufferSize(w, h); onClick(clickX_, clickY_, w, h); }
    dragging_ = false; clickArmed_ = false;
  }
  if (rmb) { if (useFly_) fly_.look(dx, dy); else orbit_.rotate(dx, dy); }
  mxPrev_ = mx; myPrev_ = my;
  if (win.scroll != 0) { if (useFly_) fly_.speed *= std::pow(1.2f, (float)win.scroll); else orbit_.zoom((float)win.scroll); win.scroll = 0; }
  if (useFly_) {
    float f = (win.key(GLFW_KEY_W) ? 1.f : 0.f) - (win.key(GLFW_KEY_S) ? 1.f : 0.f);
    float s = (win.key(GLFW_KEY_D) ? 1.f : 0.f) - (win.key(GLFW_KEY_A) ? 1.f : 0.f);
    float u = (win.key(GLFW_KEY_E) ? 1.f : 0.f) - (win.key(GLFW_KEY_Q) ? 1.f : 0.f);
    float boost = win.key(GLFW_KEY_LEFT_SHIFT) ? 4.f : 1.f;
    fly_.move(f * boost, s * boost, u * boost, (float)dt);
  }
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
  if (!dragging_) pickAt(mx, my, screenW_, screenH_, sigId, ptId);
  std::string id = sigId.empty() ? ptId : sigId;
  if (id != hoverId_) { hoverId_ = id; hoverIsSignal_ = !sigId.empty(); previewAt_ = -1; routes_.clearPreview(); hoverTip_.clear(); hoverAction_.clear(); hoverReject_ = false; }
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

void Game::onClick(double mx, double my, int w, int h) {
  std::string sigId, ptId;
  pickAt(mx, my, w, h, sigId, ptId);
  if (!sigId.empty()) {
    const Json& r = sim_.clickSignal(sigId);
    std::string msg = "signal " + sigId + ": " + (r["ok"].boolOr(false) ? "ok" : "rejected");
    if (r.has("reason")) msg += " (" + r["reason"].stringOr("") + ")";
    if (r.has("action")) msg += " " + r["action"].stringOr("");
    pushMessage(msg);
  } else if (!ptId.empty()) {
    const Json& r = sim_.flipPoint(ptId);
    std::string msg = "point " + ptId + ": " + (r["ok"].boolOr(false) ? "flipped" : "rejected");
    if (r.has("reason")) msg += " (" + r["reason"].stringOr("") + ")";
    pushMessage(msg);
  } else return;
  sim_.step(0); applySimState();
  previewAt_ = -1;   // the route/point state changed: refresh the hover preview at once
}

void Game::stepSim(double realDt) {
  if (paused_) return;
  // sim dt capped like the web client (0.1 s real → ×timeScale); step in ≤0.5 s chunks
  simAccum_ += std::fmin(realDt, 0.1) * timeScale_;
  while (simAccum_ >= 0.05) {
    double dt = std::fmin(simAccum_, 0.5);
    sim_.step(dt); simAccum_ -= dt;
    for (const SimLogLine& l : sim_.state().log) pushMessage(l.text);
  }
  applySimState();
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
  signals_.draw(renderer_, eye, &frustum);
  points_.draw(renderer_, &frustum);
  updateHover();
  routes_.draw(renderer_);   // blended ribbons before the trains' own transparent parts
  if (!hoverId_.empty()) {
    routes_.drawHoverRing(renderer_, hoverPos_, std::max(1.f, length(hoverPos_ - eye) / 46));
    vec4 c = viewProj_ * vec4(hoverPos_ + vec3{0, hoverIsSignal_ ? 4.5f : 3.f, 0}, 1);
    hoverX_ = c.w > 0 ? (c.x / c.w * 0.5f + 0.5f) * (float)w : -1; hoverY_ = c.w > 0 ? (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h : -1;
  }
  trains_.draw(renderer_, &frustum);
  renderer_.flushTransparent();
  drawHud(w, h);
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
