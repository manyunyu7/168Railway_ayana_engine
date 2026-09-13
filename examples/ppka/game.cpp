#include "examples/ppka/game.h"
#include "engine/world/sun.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>

namespace eng {

bool Game::init(Window& win, const GameOptions& opt) {
  win_ = &win; opt_ = opt;
  rhi::init();
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

  orbit_.target = stationScene_; orbit_.distance = 350; orbit_.pitch = radians(28); orbit_.yaw = radians(20);
  orbit_.near = 1; orbit_.far = 40000; orbit_.fovY = radians(52);
  fly_.position = stationScene_ + vec3{0, 30, 120}; fly_.far = 40000;

  const Json& started = sim_.startSession(opt.ai, opt.clock);
  if (!started["ok"].boolOr(false)) { std::fprintf(stderr, "start failed\n"); return false; }
  sim_.step(0);
  char m[160]; std::snprintf(m, sizeof m, "map %s loaded: %zu nodes, %d trains on line, bridge %.0f ms",
                             opt.map.c_str(), nodes.size(), (int)sim_.state().trains.size(), sim_.startupMs());
  pushMessage(m);
  ready_ = true;
  return true;
}

void Game::shutdown() {
  sim_.stop(); renderer_.shutdown(); sky_.shutdown(); text_.shutdown();
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

void Game::onClick(double, double, int, int) {
  // filled in when signal/point visuals land (screen-space picking, spec §6.5)
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
  (void)frustum;
  // world modules draw here
  renderer_.flushTransparent();
  drawHud(w, h);
}

void Game::frame(Window& win, double realDt) {
  fps_ = fps_ * 0.95 + (realDt > 0 ? 1.0 / realDt : 0) * 0.05;
  handleInput(win, realDt);
  stepSim(realDt);
  render(win);
  ++frame_;
}

bool Game::wantsCapture(int& frameNo) const { frameNo = frame_; return std::getenv("ENG_CAPTURE") != nullptr; }

} // namespace eng
