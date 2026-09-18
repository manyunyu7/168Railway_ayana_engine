// The Android host path, without Android: a second thread loads Mojokerto through the render-thread
// queue (engine/api/host/host_queue.h) exactly the way package:ayana does over FFI — every eng_* call
// is posted or blocked on, never made directly — and answers the engine's outgoing asset requests from
// assets/terrain/mojokerto (tools/fetch_tiles output) and build/wasm/models/*.emod.
// What it proves: the queue carries the big strings (world save, model.json), the outbox delivers every
// request the engine makes, and the world becomes ready with rails and resident terrain tiles.
// Needs a GL context (hidden GLFW window) like the other gpu tests; skipped without a display, and
// skipped when the terrain assets are not in the checkout.
#include "engine/api/engine_api.h"
#include "engine/api/host/host_queue.h"
#include "engine/core/json.h"
#include "engine/core/window.h"
#include "tests/check.h"
#include <GLFW/glfw3.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

using namespace eng;
namespace fs = std::filesystem;

static std::string readText(const std::string& p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::vector<uint8_t> readBytes(const std::string& p) {
  std::ifstream f(p, std::ios::binary); if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// "dem/13_6652_4265.bin" -> dir + z/x/y. false when it is not a tile path.
static bool parseTile(const std::string& rel, std::string& dir, int& z, int& x, int& y) {
  size_t slash = rel.rfind('/');
  if (slash == std::string::npos) return false;
  dir = rel.substr(0, slash);
  std::string file = rel.substr(slash + 1);
  return std::sscanf(file.c_str(), "%d_%d_%d.bin", &z, &x, &y) == 3;
}

int main() {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);   // progress shows up even through a pipe
  const std::string root = ENG_SOURCE_DIR;
  const std::string ppka = root + "/../ppka-wannabe-2";
  const std::string terrain = root + "/assets/terrain/mojokerto";
  if (!fs::exists(terrain + "/index.json")) { std::printf("SKIP: %s missing (run tools/fetch_tiles mojokerto)\n", terrain.c_str()); return test::SKIP; }
  std::string worldJson = Json::parse(readText(ppka + "/src/data/mojokerto.json"), nullptr)["world"].dump();
  std::string catalog = readText(ppka + "/public/model3d/model.json");
  if (worldJson.size() < 100 || catalog.empty()) { std::printf("SKIP: ppka-wannabe-2 data not found\n"); return test::SKIP; }
  if (!glfwInit()) { std::printf("SKIP: glfwInit failed (no display)\n"); return test::SKIP; }
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  Window win;
  if (!win.open(640, 400, "test_host_world")) { std::printf("SKIP: no GL window\n"); return test::SKIP; }

  host::RenderQueue& q = host::queue();   // the same queue engine_api.cpp's request() hook writes into
  std::atomic<bool> hostDone{false};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  std::atomic<int> answered{0}, failed{0}, models{0};
  std::string ready, stats, lastError;

  // ---- the "Dart side": never calls eng_* directly, only through the queue ----
  std::thread hostThread([&] {
    CHECK(q.callInt([&] { return eng_init(640, 400, 1); }) == 1);
    CHECK(q.callInt([&] { return eng_ready(); }) == 0);
    // The big strings live on the heap and are read on the render thread; nothing is copied to a stack.
    CHECK(q.callInt([&] { return eng_load_world(worldJson.c_str(), "{}", "mojokerto", catalog.c_str()); }) == 1);

    // Answer everything the engine asks for, the way AyanaWorldLoader does on the phone.
    host::AssetRequest r;
    int idle = 0, handled = 0;
    // Done when the world is ready and the engine stopped asking for anything (streaming settled);
    // bounded by `handled` and the deadline so a re-requesting engine cannot spin the test forever.
    while (handled < 4000 && std::chrono::steady_clock::now() < deadline) {
      if (!q.popRequest(r)) {
        if (q.callInt([] { return eng_ready(); }) == 1 && ++idle > 20) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      idle = 0; ++handled;
      const std::string& kind = r.kind;
      std::string rel = r.path;
      if (kind == "terrain") {
        size_t slash = rel.find('/');
        std::string sub = slash == std::string::npos ? rel : rel.substr(slash + 1);   // drop "mojokerto/"
        if (sub == "index.json") {
          std::vector<uint8_t> b = readBytes(terrain + "/index.json");
          CHECK(q.callInt([&] { return eng_terrain_index(b.data(), (int)b.size()); }) > 0);
          continue;
        }
        std::string dir; int z = 0, x = 0, y = 0;
        if (!parseTile(sub, dir, z, x, y)) continue;
        std::vector<uint8_t> b = readBytes(terrain + "/" + sub);
        if (b.empty()) { q.run([&] { eng_terrain_tile_fail(dir.c_str(), z, x, y); }); ++failed; continue; }
        CHECK(q.callInt([&] { return eng_terrain_tile(dir.c_str(), z, x, y, b.data(), (int)b.size()); }) == 1);
        ++answered;
      } else if (kind == "model") {
        std::string file = rel;
        for (size_t i = file.find(':'); i != std::string::npos; i = file.find(':', i)) file.replace(i, 1, "__");   // web/build-models.sh
        std::vector<uint8_t> b = readBytes(root + "/build/wasm/models/" + file + ".emod");
        if (b.empty()) { q.run([&] { eng_model_fail(rel.c_str()); }); continue; }
        std::string id = rel;
        if (q.callInt([&] { return eng_model_begin(id.c_str(), b.data(), (int)b.size()); }) == 1) {
          q.run([&] { eng_model_textures_unavailable(id.c_str()); });   // web .emod: textures are separate KTX2
          ++models;
        } else {
          q.run([&] { eng_model_fail(id.c_str()); });
        }
      } else if (kind == "city") {
        q.run([&] { eng_city_json(nullptr, 0); });   // no baked city in the checkout: optional layer
      }
    }
    // camera / state writes, the same wrappers the plugin exposes
    q.post([] { eng_camera_mode(0); });
    q.post([] { eng_orbit(20, 10); });
    q.post([] { eng_zoom(-1); });
    q.post([] { eng_set_quality(1); });
    q.post([] { eng_set_sky_time(9 * 3600); });
    q.post([] { eng_pointer(320, 200, 0, 0); });
    CHECK(q.callInt([] { return eng_set_state("{\"clock\":25200,\"trains\":[]}"); }) == 1);
    ready = std::to_string(q.callInt([] { return eng_ready(); }));
    stats = q.callString([] { const char* s = eng_stats(); return std::string(s ? s : "{}"); });
    lastError = q.callString([] { const char* s = eng_last_error(); return std::string(s ? s : ""); });
    hostDone.store(true);
  });

  // ---- the render thread: the only one touching GL ----
  int frames = 0;
  while (!hostDone.load()) {
    q.pump();
    eng_frame(0.016f);
    if (++frames % 100000 == 0) std::printf("  frame %d, tiles %d, models %d\n", frames, answered.load(), models.load());
    if (std::chrono::steady_clock::now() > deadline) { CHECK_MSG(false, "timed out waiting for the host thread"); break; }
  }
  q.pump();
  if (!hostDone.load()) q.shutdown();   // release the host thread from any blocking call
  hostThread.join();

  CHECK_MSG(ready == "1", "eng_ready after the host answered everything");
  CHECK_MSG(lastError.empty(), "eng_last_error: " + lastError);
  CHECK_MSG(answered.load() > 20, "terrain tiles answered: " + std::to_string(answered.load()));
  CHECK_MSG(models.load() > 0, "decor models answered");
  std::string err;
  Json st = Json::parse(stats, &err);
  CHECK_MSG(err.empty(), "stats: " + err);
  CHECK(st["terrain"]["resident"].intOr(0) > 0);
  CHECK(st["terrain"]["patches"].intOr(0) > 0);
  CHECK(st["decor"].boolOr(false));
  CHECK_MSG(st["summary"].stringOr("").find("km track") != std::string::npos, "summary: " + st["summary"].stringOr(""));
  std::printf("frames %d, tiles %d (%d failed), models %d, resident %d, patches %d\n%s\n",
              frames, answered.load(), failed.load(), models.load(),
              st["terrain"]["resident"].intOr(0), st["terrain"]["patches"].intOr(0), st["summary"].stringOr("").c_str());

  eng_shutdown();   // the host thread is gone: main IS the render thread, so call it directly
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}
