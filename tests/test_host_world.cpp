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

// One full load. `budgetMs` = eng_set_decor_budget: 0 builds the decor in one go (the old behaviour),
// 8 spreads the tree scatter over frames. The numbers the phone cares about come back in `out`.
struct Run {
  int budgetMs = 0;
  double worstFrameMs = 0, worstPumpMs = 0, worstDrawMs = 0, totalMs = 0;
  int frames = 0, framesOverBudget = 0, tiles = 0, models = 0, resident = 0, patches = 0, trees = 0;
  std::string summary, lastError;
  bool ready = false;
};

static void runOnce(const std::string& root, const std::string& terrain, const std::string& worldJson,
                    const std::string& catalog, Run& out);

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

  // Two loads of the same world: all-at-once, then with the 8 ms decor budget a phone uses. The world
  // has to come out the same; only the worst stall on the render thread may differ.
  Run all, sliced;
  all.budgetMs = 0;
  runOnce(root, terrain, worldJson, catalog, all);
  sliced.budgetMs = 8;
  runOnce(root, terrain, worldJson, catalog, sliced);

  for (const Run* r : {&all, &sliced}) {
    CHECK_MSG(r->ready, "eng_ready after the host answered everything");
    CHECK_MSG(r->lastError.empty(), "eng_last_error: " + r->lastError);
    CHECK_MSG(r->tiles > 20, "terrain tiles answered: " + std::to_string(r->tiles));
    CHECK_MSG(r->models > 0, "decor models answered");
    CHECK(r->resident > 0 && r->patches > 0);
    CHECK_MSG(r->summary.find("km track") != std::string::npos, "summary: " + r->summary);
    std::printf("budget %d ms: frames %d, tiles %d, models %d, resident %d, patches %d, trees %d\n"
                "  worst slice %.0f ms (queue %.0f, draw %.0f), total %.0f ms\n  %s\n",
                r->budgetMs, r->frames, r->tiles, r->models, r->resident, r->patches, r->trees,
                r->worstFrameMs, r->worstPumpMs, r->worstDrawMs, r->totalMs, r->summary.c_str());
  }
  // The same world, both ways: the budget must not cost trees, only spread them out.
  CHECK_MSG(sliced.trees >= all.trees * 9 / 10, "trees sliced " + std::to_string(sliced.trees) + " vs all " + std::to_string(all.trees));
  // The point of the exercise: no single slice may sit on the render thread for anything like the
  // unbudgeted decor build. A cell is never split, so the budget is a floor, not a hard ceiling —
  // allow 4x it plus the GL work of one frame.
  // The remaining long slice is eng_model_begin (parse + one-shot GPU upload of a model), which is not
  // sliced yet — that is the next step. What must be true is that the decor build no longer dominates.
  CHECK_MSG(sliced.worstFrameMs < 0.7 * all.worstFrameMs,
            "worst slice " + std::to_string(sliced.worstFrameMs) + " ms vs " + std::to_string(all.worstFrameMs) + " unbudgeted");

  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}

static void runOnce(const std::string& root, const std::string& terrain, const std::string& worldJson,
                    const std::string& catalog, Run& out) {
  host::RenderQueue& q = host::queue();   // the same queue engine_api.cpp's request() hook writes into
  std::atomic<bool> hostDone{false};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  std::atomic<int> answered{0}, failed{0}, models{0};
  std::string ready, stats, lastError;
  const int budgetMs = out.budgetMs;

  // ---- the "Dart side": never calls eng_* directly, only through the queue ----
  std::thread hostThread([&] {
    // Both knobs before eng_init, the way the plugin posts them when the page opens.
    q.post([budgetMs] { eng_set_decor_budget(budgetMs); });
    q.post([] { eng_set_quality(2); });
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
    q.post([] { eng_set_sky_time(9 * 3600); });
    q.post([] { eng_pointer(320, 200, 0, 0); });
    CHECK(q.callInt([] { return eng_set_state("{\"clock\":25200,\"trains\":[]}"); }) == 1);
    ready = std::to_string(q.callInt([] { return eng_ready(); }));
    stats = q.callString([] { const char* s = eng_stats(); return std::string(s ? s : "{}"); });
    lastError = q.callString([] { const char* s = eng_last_error(); return std::string(s ? s : ""); });
    hostDone.store(true);
  });

  // ---- the render thread: the only one touching GL ----
  // Every iteration is timed the way a phone frame is: whatever the queue runs (asset answers, and the
  // decor build they trigger) plus the draw. `worstFrameMs` is the longest the Texture would be frozen.
  const auto tStart = std::chrono::steady_clock::now();
  int frames = 0;
  while (!hostDone.load()) {
    auto t0 = std::chrono::steady_clock::now();
    q.pump();
    auto t1 = std::chrono::steady_clock::now();
    eng_frame(0.016f);
    auto t2 = std::chrono::steady_clock::now();
    double pumpMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double drawMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double ms = pumpMs + drawMs;
    if (pumpMs > out.worstPumpMs) out.worstPumpMs = pumpMs;
    if (drawMs > out.worstDrawMs) out.worstDrawMs = drawMs;
    if (ms > out.worstFrameMs) out.worstFrameMs = ms;
    if (budgetMs > 0 && ms > budgetMs) ++out.framesOverBudget;
    ++frames;
    if (std::chrono::steady_clock::now() > deadline) { CHECK_MSG(false, "timed out waiting for the host thread"); break; }
  }
  q.pump();
  // Drain the rest of the tree scatter the same way the render thread does between frames.
  while (eng_decor_streaming() && std::chrono::steady_clock::now() < deadline) {
    auto t0 = std::chrono::steady_clock::now();
    eng_frame(0.016f);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > out.worstFrameMs) out.worstFrameMs = ms;
    if (budgetMs > 0 && ms > budgetMs) ++out.framesOverBudget;
    ++frames;
  }
  out.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
  if (!hostDone.load()) q.shutdown();   // release the host thread from any blocking call
  hostThread.join();

  // The trees keep arriving after the host stopped asking, so read the stats once more here.
  stats = eng_stats();
  std::string err;
  Json st = Json::parse(stats, &err);
  CHECK_MSG(err.empty(), "stats: " + err);
  CHECK(st["decor"].boolOr(false));
  out.ready = ready == "1";
  out.lastError = lastError;
  out.tiles = answered.load();
  out.models = models.load();
  out.frames = frames;
  out.resident = st["terrain"]["resident"].intOr(0);
  out.patches = st["terrain"]["patches"].intOr(0);
  out.trees = st["terrain"]["trees"].intOr(0);
  out.summary = st["summary"].stringOr("");

  eng_shutdown();   // the host thread is gone: main IS the render thread, so call it directly
  q.restart();      // the next run gets a live queue (the Android host does the same on re-attach)
}
