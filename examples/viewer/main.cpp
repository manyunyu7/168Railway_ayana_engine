// Example 2: model viewer. `viewer model.emod` — drag to orbit, scroll to zoom.
// Models are loaded through fetchFile: synchronously on desktop, streamed with emscripten_fetch on the web,
// where the page (web/index.html) calls the exported viewer_load(url) to switch models and polls
// viewer_fps() / viewer_bytes() for the readouts. Textures may arrive separately (geometry-only EMOD, v6
// placeholders): the page transcodes KTX2 in JS (web/ktx2.js) and streams the GPU blocks through
// viewer_texture_begin / viewer_texture_mip / viewer_texture_end.
#include "engine/asset/emod.h"
#include "engine/core/fetch.h"
#include "engine/core/orbit_camera.h"
#include "engine/core/window.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/render/text.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Module.heap() returns the current Wasm heap view (HEAPU8 is reassigned on memory growth and not exported
// by default); the page copies transcoded mip data into buffers from viewer_alloc through it.
EM_JS(void, installHeapAccessor, (), { Module['heap'] = () => HEAPU8; });
#endif

using namespace eng;

struct App {
  Window win; GpuModel gpu; ModelRenderer renderer; TextRenderer text; Lighting light; Sky sky; OrbitCamera cam;
  std::string path, status; double mxPrev = 0, myPrev = 0; bool dragging = false; int frame = 0;
  bool loading = false, ready = false; size_t bytes = 0; double loadMs = 0;
  struct ImageHint { int source; uint8_t wrapS, wrapT; bool linear, placeholder; };
  std::vector<ImageHint> images;        // per image of the loaded model: wrap / colour-space hints + external source index
  Image incoming; ImageVariant incomingVar; int incomingIndex = -1;   // texture being streamed in by the host
  double fps = 0, fpsAccum = 0; int fpsFrames = 0; double lastTime = 0;
  void load(const std::string& url);
  void step();
};
static App* g_app = nullptr;
[[maybe_unused]] static void stepThunk() { g_app->step(); }

void App::load(const std::string& url) {
  path = url; loading = true; status = "loading " + url + " ...";
  auto t0 = std::chrono::steady_clock::now();
  fetchFile(url, [this, t0](FetchResult& r) {
    loading = false;
    if (!r.ok) { status = r.error; std::fprintf(stderr, "%s\n", r.error.c_str()); return; }
    Model model; std::string err;
    if (!loadEmod(r.bytes, model, err)) { status = err; std::fprintf(stderr, "%s\n", err.c_str()); return; }
    if (ready) gpu.destroy();
    gpu.upload(model);
    images.clear(); for (const Image& im : model.images) images.push_back({im.source, im.wrapS, im.wrapT, im.linear, im.placeholder()});
    bytes = r.bytes.size(); ready = true; status.clear();
    loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("loaded %s (%.1f MB) in %.0f ms: %zu nodes, %zu textures\n", path.c_str(), bytes / 1e6, loadMs, gpu.nodes.size(), gpu.textures.size());
    cam.target = gpu.bounds.center();
    cam.distance = length(gpu.bounds.extent()) * 1.8f;
    cam.far = cam.distance * 20; cam.near = cam.distance * 0.002f;
  });
}

extern "C" {
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void viewer_load(const char* url) { if (g_app) g_app->load(url); }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
double viewer_fps() { return g_app ? g_app->fps : 0; }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_bytes() { return g_app ? (int)g_app->bytes : 0; }   // size of the model file on screen (0 while loading)
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_ready() { return g_app && g_app->ready && !g_app->loading; }

// ---- texture streaming (C ABI for the page) ----
// Format codes = eng::TexFormat: 0 RGBA8, 1 ETC2_RGB, 2 ETC2_RGBA, 3 BC1, 4 BC3, 5 BC7.
static const rhi::Format kFormatMap[] = {rhi::Format::RGBA8, rhi::Format::ETC2_RGB, rhi::Format::ETC2_RGBA, rhi::Format::BC1, rhi::Format::BC3, rhi::Format::BC7};
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_supports(int format) { return format >= 0 && format <= 5 && rhi::supports(kFormatMap[format]); }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_image_count() { return g_app && g_app->ready ? (int)g_app->images.size() : 0; }
// Per-image hints of the loaded model: source index in the external texture file (-1 = stored in the EMOD),
// and flags = wrapS | wrapT << 8 | linear << 16 | placeholder << 17.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_image_source(int i) { return g_app && i >= 0 && i < (int)g_app->images.size() ? g_app->images[(size_t)i].source : -1; }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_image_flags(int i) {
  if (!g_app || i < 0 || i >= (int)g_app->images.size()) return 0;
  const App::ImageHint& im = g_app->images[(size_t)i];
  return im.wrapS | im.wrapT << 8 | (im.linear ? 1 : 0) << 16 | (im.placeholder ? 1 : 0) << 17;
}
// Heap buffer for the host's mip data (Module._malloc is not exported by default): alloc/free in the Wasm heap.
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void* viewer_alloc(int bytes) { return bytes > 0 ? std::malloc((size_t)bytes) : nullptr; }
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void viewer_free(void* p) { std::free(p); }
// Streams one texture: begin(image, w, h, format, mips, srgb, wrapS, wrapT), mip(level, ptr, bytes) per level
// (the host copies the transcoded blocks into the Wasm heap first), end() uploads and swaps it into the model.
// Returns 0 on a bad argument; end() returns the GL texture id (0 = upload failed).
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_texture_begin(int image, int width, int height, int format, int mipCount, int srgb, int wrapS, int wrapT) {
  if (!g_app || !g_app->ready || image < 0 || image >= (int)g_app->gpu.textures.size() || format < 0 || format > 5 || mipCount < 1 || width < 1 || height < 1) return 0;
  App& a = *g_app;
  a.incomingIndex = image; a.incoming = {}; a.incomingVar = {};
  a.incoming.width = width; a.incoming.height = height; a.incoming.channels = 4;
  a.incoming.wrapS = (uint8_t)wrapS; a.incoming.wrapT = (uint8_t)wrapT; a.incoming.linear = !srgb;
  a.incomingVar.format = (TexFormat)format; a.incomingVar.mips.resize((size_t)mipCount);
  int w = width, h = height;
  for (MipLevel& l : a.incomingVar.mips) { l.width = w; l.height = h; w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
  return 1;
}
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_texture_mip(int level, const uint8_t* data, int bytes) {
  if (!g_app || g_app->incomingIndex < 0 || level < 0 || level >= (int)g_app->incomingVar.mips.size() || !data || bytes < 0) return 0;
  MipLevel& l = g_app->incomingVar.mips[(size_t)level];
  if ((size_t)bytes != texLevelBytes(g_app->incomingVar.format, l.width, l.height)) { std::fprintf(stderr, "texture mip %d: %d bytes, expected %zu\n", level, bytes, texLevelBytes(g_app->incomingVar.format, l.width, l.height)); return 0; }
  l.data.assign(data, data + bytes);
  return 1;
}
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
int viewer_texture_end() {
  if (!g_app || g_app->incomingIndex < 0) return 0;
  App& a = *g_app; int i = a.incomingIndex; a.incomingIndex = -1;
  for (const MipLevel& l : a.incomingVar.mips) if (l.data.empty()) { std::fprintf(stderr, "texture %d: missing mip level\n", i); return 0; }
  a.incoming.variants = {std::move(a.incomingVar)};
  rhi::Texture t = uploadImage(a.incoming);
  a.incoming = {}; a.incomingVar = {};
  if (!t.id) return 0;
  rhi::destroyTexture(a.gpu.textures[(size_t)i]); a.gpu.textures[(size_t)i] = t;
  if ((size_t)i < a.images.size()) a.images[(size_t)i].placeholder = false;   // resident now
  return (int)t.id;
}
}

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
  const char* path = argc >= 2 ? argv[1] : "";   // the page decides what to load (viewer_load)
#else
  if (argc < 2) { std::fprintf(stderr, "usage: viewer model.emod\n"); return 2; }
  const char* path = argv[1];
#endif
  static App app; g_app = &app;
#ifdef __EMSCRIPTEN__
  installHeapAccessor();
#endif
  Window& win = app.win;
  if (!win.open(1280, 800, "engine — viewer")) return 1;
  rhi::init();
  app.renderer.init();
  std::string err;
  if (!app.text.load("assets/font.efnt", err)) std::fprintf(stderr, "font: %s\n", err.c_str());
  app.sky.init(); app.sky.sunDir = app.light.sunDir;
  app.cam.distance = 10; app.cam.far = 200; app.cam.near = 0.02f;
  app.lastTime = win.time();
  if (*path) app.load(path);
#ifndef __EMSCRIPTEN__
  if (!app.ready) return 1;   // native loads are synchronous
#endif

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(stepThunk, 0, 1);
#else
  while (win.isOpen()) app.step();
#endif
  app.gpu.destroy(); app.renderer.shutdown(); app.text.shutdown(); app.sky.shutdown(); win.close();
  return 0;
}

void App::step() {
  win.pollEvents();
  double now = win.time();
  fpsAccum += now - lastTime; lastTime = now;
  if (++fpsFrames >= 30) { fps = fpsFrames / fpsAccum; fpsAccum = 0; fpsFrames = 0; }
  double mx, my; win.mousePos(mx, my);
  if (win.mouseButton(0)) { if (dragging) cam.rotate((float)(mx - mxPrev), (float)(my - myPrev)); dragging = true; }
  else dragging = false;
  mxPrev = mx; myPrev = my;
  if (win.scroll != 0) { cam.zoom((float)win.scroll); win.scroll = 0; }

  int w, h; win.framebufferSize(w, h);
  rhi::setViewport(w, h);
  rhi::clear(0, 0, 0, 1);
  mat4 vp = cam.projection((float)w / (float)h) * cam.view();
  sky.draw(vp.inverse(), cam.position());
  renderer.beginFrame(vp, cam.position(), light);
  if (ready) renderer.draw(gpu);
  renderer.flushTransparent();
  char hud[256];
  if (!status.empty()) std::snprintf(hud, sizeof hud, "%s", status.c_str());
  else if (std::getenv("ENG_CAPTURE")) std::snprintf(hud, sizeof hud, "%s  %.1f MB  draws %u  culled %u", path.c_str(), bytes / 1e6, renderer.drawCalls, renderer.culled);   // golden capture: no timing text
  else std::snprintf(hud, sizeof hud, "%s  %.1f MB  %.0f ms  draws %u  culled %u  %.0f fps", path.c_str(), bytes / 1e6, loadMs, renderer.drawCalls, renderer.culled, fps);
  text.rect(8, 8, text.measure(hud) + 16, text.lineHeight() + 8, {0, 0, 0, 0.5f});
  text.draw(hud, 16, 12);
  text.flush(w, h);

  if (frame++ == 0) rhi::checkErrors("first frame");
  if (const char* cap = std::getenv("ENG_CAPTURE"); cap && frame == 30) {
    rhi::captureFramebuffer(cap, w, h); std::printf("captured -> %s\n", cap); glfwSetWindowShouldClose((GLFWwindow*)win.handle, 1); return;
  }
  win.swapBuffers();
}
