// Example: GPU skinning. `skintest [model.glb|model.emod] [clip]` — drag to orbit, scroll to zoom.
// Default model: assets/test/rigged.glb (tools/testdata/make_rigged.py), a two-bone cylinder with the
// clips "diam" and "jalan". Three copies are drawn side by side to show what the API does:
//   left   the rest pose (no clip)
//   middle the clip playing through AnimationPlayer's locomotion blend (idle <-> walk by speed)
//   right  the same, plus a gesture layer on the "Upper" joint only
// ENG_CAPTURE=/tmp/x.ppm captures frame 30 and exits, like the viewer; the animation advances at a
// fixed 1/60 s per frame so that capture is reproducible.
#include "engine/asset/emod.h"
#include "engine/asset/gltf.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/animator.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace eng;

int main(int argc, char** argv) {
  std::string path = argc >= 2 ? argv[1] : "assets/test/rigged.glb";
  std::string clipName = argc >= 3 ? argv[2] : "jalan";

  Model model; std::string err;
  bool ok = path.size() > 5 && path.compare(path.size() - 5, 5, ".emod") == 0
                ? loadEmod(path, model, err) : loadGlbFile(path, model, err);
  if (!ok) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
  if (model.skins.empty()) { std::fprintf(stderr, "%s has no skin\n", path.c_str()); return 1; }
  std::printf("%s: %zu nodes, %zu skins (%zu joints), %zu clips\n", path.c_str(), model.nodes.size(),
              model.skins.size(), model.skins[0].joints.size(), model.animations.size());

  Window win;
  if (!win.open(1280, 800, "engine — skintest")) return 1;
  rhi::init();
  ModelRenderer renderer; renderer.init();
  TextRenderer text; if (!text.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  Lighting light; Sky sky; sky.init(); sky.sunDir = light.sunDir;
  GpuModel gpu; gpu.upload(model);

  AnimationPlayer player;
  player.setSource(&gpu.nodes, &gpu.animations);
  int idle = player.clipIndex("diam"), clip = player.clipIndex(clipName);
  if (clip < 0) clip = gpu.animations.empty() ? -1 : 0;
  player.setLocomotion({{idle >= 0 ? idle : clip, 0.f, 0.f}, {clip, 1.4f, 1.4f}});
  player.setParam(1.4f);                                   // walking speed
  std::vector<int> mask = jointMask(gpu.nodes, {"Upper"});  // the test rig's gesture joint

  AnimationPlayer gestured;   // second player so the gesture copy keeps its own timeline
  gestured.setSource(&gpu.nodes, &gpu.animations);
  gestured.setLocomotion({{idle >= 0 ? idle : clip, 0.f, 0.f}, {clip, 1.4f, 1.4f}});
  gestured.setParam(1.4f);
  gestured.gesture(clip, mask, 0.25f);

  Pose rest; restPose(gpu.nodes, rest);
  std::vector<mat4> restLocal, restWorld, restPalette, palette, gestPalette;
  poseToLocal(rest, restLocal);
  computeWorld(gpu.nodes, gpu.roots, restLocal, restWorld);
  jointPalette(gpu.skins[0], restWorld, restPalette);

  float span = length(gpu.bounds.extent());
  OrbitCamera cam;
  cam.target = gpu.bounds.center();
  cam.distance = span * 4.5f;
  cam.near = span * 0.01f; cam.far = span * 40;

  double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0;
  const float step = 1.f / 60;
  while (win.isOpen()) {
    win.pollEvents();
    double mx, my; win.mousePos(mx, my);
    if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
    else dragging = false;
    mxPrev = mx; myPrev = my;
    if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }

    player.update(step);
    gestured.update(step);
    if (!gestured.gestureActive()) gestured.gesture(clip, mask, 0.25f);   // loop the gesture for the demo
    player.palette(gpu.nodes, gpu.roots, gpu.skins[0], palette);
    gestured.palette(gpu.nodes, gpu.roots, gpu.skins[0], gestPalette);

    int w, h; win.framebufferSize(w, h);
    rhi::setViewport(w, h);
    rhi::clear(0, 0, 0, 1);
    mat4 vp = cam.projection((float)w / (float)h) * cam.view();
    sky.draw(vp.inverse(), cam.position());
    renderer.beginFrame(vp, cam.position(), light);
    float gap = span * 0.9f;
    renderer.drawSkinned(gpu, mat4::translation({-gap, 0, 0}), restPalette);
    renderer.drawSkinned(gpu, mat4::identity(), palette);
    renderer.drawSkinned(gpu, mat4::translation({gap, 0, 0}), gestPalette);
    renderer.flushTransparent();

    char hud[256];
    std::snprintf(hud, sizeof hud, "%s  joints %zu  clip \"%s\"  phase %.2f  gesture %.2f  draws %u   (left: rest pose, middle: clip, right: clip + gesture layer)",
                  path.c_str(), gpu.skins[0].joints.size(), clipName.c_str(), player.phase(),
                  gestured.gestureWeight(), renderer.drawCalls);
    text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
    text.draw(hud, 16, 12);
    text.flush(w, h);

    if (frame++ == 0) rhi::checkErrors("first frame");
    if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
      rhi::captureFramebuffer(cap, w, h);
      std::printf("captured -> %s\n", cap);
      glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1);
      continue;
    }
    win.swapBuffers();
  }
  gpu.destroy(); renderer.shutdown(); text.shutdown(); sky.shutdown(); win.close();
  return 0;
}
