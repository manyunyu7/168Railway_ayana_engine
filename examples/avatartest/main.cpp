// Example: avatars + third-person camera. `avatartest` — flat ground, a wall, one LOCAL avatar driven by the
// walker of CamMode::Orang (WASD relative to the camera, Shift = run, Space = jump, drag = orbit, scroll = boom)
// and two REMOTE avatars fed through AvatarSet::upsert at 15 Hz so the interpolation is exercised.
// Keys: G cycles the local gesture (s40, s3, s1, hormat, lambai, tunjuk).
// Env: ENG_CAPTURE=file.ppm (frame 30, exit), ENG_GESTURE=<nama> starts the local gesture, ENG_WALK=1 walks forward,
//      ENG_BOXES=1 forces the placeholder body (no catalog).
// The figures are the skinned models of ppka-wannabe-2/public/model3d/nry-avatar-*.glb, resolved through the
// AssetCatalog (`avatar:<id>`); without them the placeholder boxes draw. In CAPTURE mode the scene is a fixed
// line-up instead of the walking circles — idle, mid-stride and semboyan 40 — with a fixed 1/60 s step.
#include "engine/app/camera_rig.h"
#include "engine/core/window.h"
#include "engine/render/mesh_builder.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/avatar.h"
#include "engine/world/avatar_visual.h"
#include "engine/world/coords.h"
#include "engine/world/walk_collision.h"
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace eng;

namespace {
AvatarState remoteState(double t, int i) {
  AvatarState s;
  // two figures walking circles of different radius around the origin (world = scene here: origin at 0)
  double r = i == 0 ? 4.5 : 6.5, w = i == 0 ? 0.55 : -0.38, ph = i * 1.7;
  s.x = std::cos(w * t + ph) * r;
  s.y = std::sin(w * t + ph) * r;
  s.z = 0;
  // world yaw 0 = +x; the tangent of the circle is the heading
  s.yaw = (float)std::atan2(std::cos(w * t + ph) * w, -std::sin(w * t + ph) * w);
  s.anim = i == 0 ? AvatarAnim::Jalan : AvatarAnim::Lari;
  s.pakaian.kulit = i == 0 ? 1 : 4;
  if (i == 0) { s.gesture = AvatarGesture::Lambai; s.gestureT = (float)std::fmod(t, 3.2); }
  if (i == 1) { s.pakaian.baju = "baju-kaos"; s.pakaian.warnaBaju = 0x2f9e5a; s.pakaian.celana = "celana-jeans"; s.pakaian.topi = ""; }
  return s;
}
} // namespace

namespace {
// Capture line-up: the local avatar (idle) flanked by a walking figure and one giving semboyan 40, all facing
// the camera, which is dragged round to the front. The local figure's yaw is +y (walker default), so is theirs.
AvatarState poseState(double t, int i) {
  AvatarState s;
  const float sp = std::getenv("ENG_DEKAT") ? 1.3f : 2.4f;
  s.x = i == 0 ? -sp : sp; s.y = 0; s.z = 0;
  s.yaw = (float)(-PI / 2);   // scene yaw = -world yaw: this matches the local walker's 1.57
  s.anim = i == 0 ? AvatarAnim::Jalan : AvatarAnim::Diam;
  if (i == 1) { s.gesture = AvatarGesture::S40; s.gestureT = (float)std::fmin(t, 1.9); }
  s.pakaian.kulit = i == 0 ? 4 : 1;
  return s;
}
} // namespace

int main() {
  Window win;
  if (!win.open(1280, 800, "engine — avatartest")) return 1;
  rhi::init();

  // ground slab + a wall the camera boom has to avoid
  MeshBuilder gb; gb.box({-80, -1, -80}, {80, 0, 80});
  rhi::Mesh ground = gb.upload();
  MeshBuilder wb; wb.box({-14, 0, -17}, {14, 4, -16});
  rhi::Mesh wall = wb.upload();
  WalkCollider col;
  col.addBox(AABB{{-14, 0, -17}, {14, 4, -16}});
  col.finish();

  ModelRenderer renderer; renderer.init();
  Sky sky; sky.init();
  Lighting light; light.fogDensity = 1.f / 900;
  sky.sunDir = light.sunDir;
  AvatarVisuals vis; vis.build();
  AssetCatalog cat;
  if (!std::getenv("ENG_BOXES")) {
    if (cat.load()) vis.setCatalog(&cat);
    else std::printf("[avatartest] no catalog (%s) — placeholder bodies\n", cat.error().c_str());
  }
  AvatarSet set;
  WorldOrigin origin;   // scene == world in this example

  auto flat = [](float, float) { return 0.f; };
  CameraRig rig;
  rig.setMode(CamMode::Orang, {0, 3, 12}, {0, 0, 0});
  rig.enterOrang({0, 3, 12}, {0, 0, 0}, flat);
  {   // PPKA defaults for the local figure
    AvatarState s; set.setLocal(s, origin);
  }
  if (const char* gs = std::getenv("ENG_GESTURE")) {
    AvatarGesture g; if (parseAvatarGesture(gs, g)) set.startGesture(g);
  }
  const bool capture = std::getenv("ENG_CAPTURE") != nullptr;
  if (capture) { rig.jarakOrang = std::getenv("ENG_DEKAT") ? 3.f : 7.f; rig.drag(628, std::getenv("ENG_DEKAT") ? 8.f : 20.f); }   // orbit to the front (628 px = 180 deg)   // wider framing for the screenshot
  const bool autoWalk = std::getenv("ENG_WALK") != nullptr;

  double last = win.time(), t = 0, netAcc = 0;
  int frame = 0, gestureIdx = 0; bool gPrev = false;
  double mxPrev = 0, myPrev = 0; bool dragging = false;
  while (win.isOpen()) {
    win.pollEvents();
    double now = win.time(), dt = now - last; last = now;
    if (dt > 0.1) dt = 0.1;
    if (capture) dt = 1.0 / 60;   // deterministic poses
    t += dt;

    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) rig.drag((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; } else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { rig.scroll((float)win.scroll); win.scroll = 0; }

    std::vector<WalkBox> boxes;
    WalkInput in;
    in.forward = (win.key(GLFW_KEY_W) ? 1.f : 0.f) - (win.key(GLFW_KEY_S) ? 1.f : 0.f);
    in.side = (win.key(GLFW_KEY_D) ? 1.f : 0.f) - (win.key(GLFW_KEY_A) ? 1.f : 0.f);
    in.run = win.key(GLFW_KEY_LEFT_SHIFT);
    in.jump = win.key(GLFW_KEY_SPACE);
    if (autoWalk) in.forward = 1;
    in.mesh = &col; in.boxes = &boxes;
    bool gNow = win.key(GLFW_KEY_G);
    if (gNow && !gPrev) set.startGesture((AvatarGesture)(1 + gestureIdx++ % 6));
    gPrev = gNow;

    rig.step((float)dt, nullptr, flat, in);
    set.driveLocal(rig.walkerPos(), rig.walkerBodyYaw(), rig.walkerSpeed(), rig.walkerOnGround(), origin);
    netAcc += dt;
    while (netAcc >= 1.0 / 15) {   // the host's 15 Hz avatar relay
      netAcc -= 1.0 / 15;
      if (capture) for (int i = 0; i < 2; ++i) set.upsert(100 + i, poseState(t, i), origin, t);
      else for (int i = 0; i < 2; ++i) set.upsert(100 + i, remoteState(t, i), origin, t);
    }
    set.tick((float)dt, t);

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(light.fogColor.x, light.fogColor.y, light.fogColor.z, 1);
    mat4 vp = rig.projection((float)w / (float)h) * rig.view();
    sky.draw(vp.inverse(), rig.eye());
    renderer.beginFrame(vp, rig.eye(), light);
    Material gm; gm.baseColor = {0.30f, 0.42f, 0.24f, 1}; gm.metallic = 0; gm.roughness = 1;
    renderer.drawMesh(ground, gm, rhi::Texture{}, mat4::identity());
    Material wm; wm.baseColor = {0.62f, 0.60f, 0.56f, 1}; wm.metallic = 0; wm.roughness = 0.9f;
    renderer.drawMesh(wall, wm, rhi::Texture{}, mat4::identity());
    vis.draw(renderer, set, (float)t);
    renderer.flushTransparent();

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
      rhi::captureFramebuffer(cap, w, h);
      std::printf("captured -> %s (local %.2f %.2f %.2f yaw %.2f, %zu remote)\n", cap,
                  set.local.pos.x, set.local.pos.y, set.local.pos.z, set.local.bodyYaw, set.remote.size());
      break;
    }
    win.swapBuffers();
  }
  vis.destroy(); cat.destroy(); rhi::destroyMesh(ground); rhi::destroyMesh(wall);
  renderer.shutdown(); sky.shutdown(); win.close();
  return 0;
}
