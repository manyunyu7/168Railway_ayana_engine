// Editing ABI (engine/api/engine_api_edit.cpp) through the real C entry points on Mojokerto, flat ground (no
// terrain index): picking, hiasan add / set / remove + pick-back, node height + rails rebuild timing, brush
// deltas, full track edit. Needs a GL context: a hidden GLFW window (skipped when none can be created, e.g. CI
// without a display). Model geometry comes from build/wasm/models/*.emod when present (web/build-models.sh);
// without it the hiasan checks that need a resident model are skipped, the rest still runs.
#include "engine/api/engine_api.h"
#include "engine/api/engine_api_edit.h"
#include "engine/app/world_scene.h"
#include "engine/world/name_board.h"
#include "engine/core/window.h"
#include "tests/check.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace eng;

static std::string readText(const std::string& path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static std::vector<uint8_t> readBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary); if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::vector<double> nums(const std::string& s) {   // "a,b,c" -> doubles
  std::vector<double> out; std::stringstream ss(s); std::string t;
  while (std::getline(ss, t, ',')) out.push_back(std::atof(t.c_str()));
  return out;
}

int main() {
  const std::string root = ENG_SOURCE_DIR;
  const std::string ppka = root + "/../ppka-wannabe-2";
  if (!glfwInit()) { std::printf("SKIP: glfwInit failed (no display)\n"); return test::SKIP; }
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  Window win;
  if (!win.open(640, 400, "test_edit")) { std::printf("SKIP: no GL window\n"); return test::SKIP; }

  std::string err;
  Json save = Json::parse(readText(ppka + "/src/data/mojokerto.json"), &err); CHECK_MSG(err.empty(), err);
  std::string worldJson = save["world"].dump();
  std::string catalog = readText(ppka + "/public/model3d/model.json");
  CHECK(!catalog.empty());

  CHECK(eng_init(640, 400, 1));
  CHECK(!eng_ready());
  CHECK(eng_pick_ground(320, 200)[0] == '\0');   // before the world: ""
  CHECK(eng_load_world(worldJson.c_str(), "{}", "mojokerto", catalog.c_str()));
  CHECK(eng_terrain_index(nullptr, 0) == 0);      // no terrain: flat world, built right away
  CHECK(eng_ready());
  // decor models: answer with the web .emod when present, else fail the slot (box / skipped)
  auto provide = [&](const std::string& id) {
    std::vector<uint8_t> b = readBytes(root + "/build/wasm/models/" + id + ".emod");
    if (b.empty()) { eng_model_fail(id.c_str()); return false; }
    bool ok = eng_model_begin(id.c_str(), b.data(), (int)b.size());
    if (ok) eng_model_textures_unavailable(id.c_str());
    return ok;
  };
  bool haveTree = provide("pohon-05"), haveStation = provide("sttd-stasiun-mojokerto");
  for (const char* id : {"pohon-06", "pohon-07", "pohon-08", "pohon-09"}) provide(id);
  std::printf("models: tree %d station %d\n", haveTree, haveStation);
  { Json st = Json::parse(eng_stats(), &err); CHECK(st["decor"].boolOr(false));
    std::printf("textures: %d unique, %d refs, %.1f MB, %.1f MB shared\n", st["textures"]["unique"].intOr(0), st["textures"]["refs"].intOr(0),
                st["textures"]["mb"].numberOr(0), st["textures"]["sharedMb"].numberOr(0));
    std::printf("hiasan: %d drawn, %d instanced\n", st["hiasan"]["drawn"].intOr(0), st["hiasan"]["instanced"].intOr(0)); }
  eng_frame(0.016f);
  EngEditCtx c; CHECK(eng_edit_ctx(c) && c.ready && c.viewValid);
  WorldScene& scene = *c.scene;
  const TrackGraph& g = scene.graph();

  // ---- picking: the centre of the default view hits the ground near the station
  std::string gp = eng_pick_ground(320, 200);
  CHECK_MSG(!gp.empty(), "pick_ground at the view centre");
  std::vector<double> ground = nums(gp); CHECK(ground.size() == 3);
  vec3 st = scene.stationScene();
  CHECK_NEAR(ground[0], st.x + scene.origin().ox, 400); CHECK_NEAR(ground[1], st.z + scene.origin().oz, 400);
  CHECK(eng_pick_ground(320, 2)[0] == '\0');      // top rows look at the sky
  CHECK(std::strcmp(eng_pick_track(320, 200, 4000), "") != 0);   // a rail is somewhere on screen
  {
    std::vector<double> t = nums(std::strchr(eng_pick_track(320, 200, 4000), ',') + 1);
    CHECK(t.size() == 2 && t[0] >= 0 && (t[1] == 1 || t[1] == -1));
  }

  // ---- hiasan: add a tree 60 m from the station, read it back, pick it, move it, remove it
  const int n0 = (int)(*c.world)["hiasan"]["objek"].size();
  double tx = ground[0] + 60, ty = ground[1] + 25;
  char add[300]; std::snprintf(add, sizeof add, "{\"model\":\"pohon-05\",\"x\":%.2f,\"y\":%.2f,\"naik\":0,\"rot\":30,\"skala\":1.5}", tx, ty);
  int idx = eng_hiasan_add(add);
  CHECK_EQ(idx, n0);
  CHECK_EQ(eng_hiasan_add("nonsense"), -1);
  CHECK_EQ((int)(*c.world)["hiasan"]["objek"].size(), n0 + 1);
  Json info = Json::parse(eng_hiasan_info(idx), &err); CHECK_MSG(err.empty(), err);
  CHECK(info["model"].stringOr("") == "pohon-05"); CHECK_NEAR(info["rot"].numberOr(0), 30, 1e-6); CHECK_NEAR(info["skala"].numberOr(0), 1.5, 1e-6);
  CHECK_EQ(info["resident"].boolOr(false), haveTree);
  CHECK(eng_hiasan_info(999)[0] == '\0');
  if (haveTree) {
    CHECK_EQ(scene.hiasanCount(), n0 + (haveStation ? 1 : 0));   // the station is the only other placed entry
    CHECK(info["size"][1].numberOr(0) > 0.5);
    float sc[4]; eng_project(tx, ty, 0, sc); CHECK(sc[2] > 0.5f);
    // the tree is thin on screen: the pixel tolerance finds it around its foot / trunk
    std::string picked = eng_pick_object(sc[0], sc[1] - 4);
    CHECK_MSG(picked == "hiasan:" + std::to_string(idx), "pick_object -> " + picked);
    CHECK(std::string(eng_model_size("pohon-05")).size() > 4);
  }
  CHECK(eng_hiasan_set(idx, tx + 5, ty, 0.8f, 95, 0.7f));
  info = Json::parse(eng_hiasan_info(idx), &err);
  CHECK_NEAR(info["x"].numberOr(0), tx + 5, 1e-3); CHECK_NEAR(info["naik"].numberOr(0), 0.8, 1e-6); CHECK_NEAR(info["rot"].numberOr(0), 95, 1e-6);
  CHECK(!eng_hiasan_set(999, 0, 0, 0, 0, 1));
  eng_highlight(("hiasan:" + std::to_string(idx)).c_str(), 1);
  eng_gizmo("rotate", tx + 5, ty, 1, 0.3f, 4, 1);
  eng_ghost("pohon-05", tx, ty + 10, 0, 1);
  CHECK(eng_ukur_line("[{\"x\":12516100,\"y\":834200},{\"x\":12516180,\"y\":834260}]"));
  eng_frame(0.016f);   // overlays drawn without GL errors
  CHECK(eng_hiasan_remove(idx));
  CHECK_EQ((int)(*c.world)["hiasan"]["objek"].size(), n0);
  CHECK(!eng_hiasan_remove(idx));
  eng_highlight("", 0); eng_gizmo("", 0, 0, 0, 0, 1, 0); eng_ghost("", 0, 0, 0, 1); eng_ukur_line("[]");

  // ---- palette thumbnail: a resident model rendered offscreen (top-down RGBA rows, transparent background)
  CHECK(eng_thumbnail("no-such-model", 64) == nullptr);
  CHECK(eng_thumbnail("pohon-05", 4) == nullptr);
  if (haveTree) {
    const uint8_t* px = eng_thumbnail("pohon-05", 64);
    CHECK_MSG(px != nullptr, "thumbnail of a resident model");
    if (px) {
      int opaque = 0, clear = 0;
      for (int i = 0; i < 64 * 64; ++i) { if (px[i * 4 + 3] > 200) ++opaque; else if (px[i * 4 + 3] == 0) ++clear; }
      std::printf("thumbnail: %d opaque / %d clear px\n", opaque, clear);
      CHECK(opaque > 64 && clear > 64);   // the tree covers part of the card, the rest stays see-through
      // the trunk stands in the lower half, the canopy in the upper: something opaque in both
      bool upper = false, lower = false;
      for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) if (px[(y * 64 + x) * 4 + 3] > 200) { if (y < 32) upper = true; else lower = true; }
      CHECK(upper && lower);
      if (const char* dump = std::getenv("ENG_THUMB_DUMP")) {   // debug: the card as a PPM (transparent = black)
        if (FILE* fp = std::fopen(dump, "wb")) { std::fprintf(fp, "P6\n64 64\n255\n"); for (int i = 0; i < 64 * 64; ++i) { uint8_t p3[3] = {px[i * 4], px[i * 4 + 1], px[i * 4 + 2]}; if (px[i * 4 + 3] == 0) p3[0] = p3[1] = p3[2] = 40; std::fwrite(p3, 1, 3, fp); } std::fclose(fp); }
      }
    }
    eng_frame(0.016f);   // the window framebuffer is drawn again afterwards without GL errors
  }

  // ---- papan nama: text painted on the `papan-nama` quad of a station canopy (plain GLB, textures dropped)
  CHECK(NameBoards::normName("kebumen") == "STASIUN KEBUMEN");
  CHECK(NameBoards::normName(" Stasiun  Kebumen ") == "STASIUN KEBUMEN");
  CHECK(NameBoards::normName("") == "");
  CHECK(NameBoards::normHeight("21") == "+ 21 M"); CHECK(NameBoards::normHeight("+ 21 m") == "+ 21 M");
  CHECK(NameBoards::normHeight("-3.5") == "- 3.5 M"); CHECK(NameBoards::normHeight("+ 21 M dpl") == "+ 21 M DPL");
  {
    std::vector<uint8_t> glb = readBytes(ppka + "/public/model3d/cc0-kanopi-pwk-papan.glb");
    bool haveKanopi = !glb.empty() && eng_model_begin_glb("kanopi-pwk-papan", glb.data(), (int)glb.size());
    if (haveKanopi) eng_model_textures_unavailable("kanopi-pwk-papan");
    std::printf("models: kanopi-papan %d\n", haveKanopi);
    if (haveKanopi) {
      char addK[300]; std::snprintf(addK, sizeof addK, "{\"model\":\"kanopi-pwk-papan\",\"x\":%.2f,\"y\":%.2f,\"naik\":0,\"rot\":0,\"skala\":1,\"teks\":\"mojokerto\"}", tx, ty + 80);
      int k = eng_hiasan_add(addK);
      CHECK(k >= 0);
      Json ki = Json::parse(eng_hiasan_info(k), &err); CHECK_MSG(err.empty(), err);
      CHECK(ki["resident"].boolOr(false)); CHECK(ki["papan"].boolOr(false)); CHECK(ki["teks"].stringOr("") == "mojokerto");
      CHECK(!info["papan"].boolOr(true));   // the tree earlier: no board
      CHECK_EQ((int)scene.nameBoardCount(), 1);
      CHECK(eng_hiasan_text(k, "Mojokerto", "21"));
      ki = Json::parse(eng_hiasan_info(k), &err);
      CHECK(ki["teks"].stringOr("") == "Mojokerto"); CHECK(ki["ketinggian"].stringOr("") == "21");
      CHECK(!eng_hiasan_text(999, "x", ""));
      eng_frame(0.016f);   // board drawn without GL errors
      CHECK(eng_hiasan_text(k, "", ""));   // text removed: plain board again, fields gone from the save
      CHECK(!(*c.world)["hiasan"]["objek"][(size_t)k].has("teks"));
      CHECK_EQ((int)scene.nameBoardCount(), 0);
      CHECK(eng_hiasan_remove(k));
    }
  }

  // ---- markers: a batch at the view centre, picked back by pixel, labels projected, cleared
  {
    char mk[600];
    std::snprintf(mk, sizeof mk,
      "[{\"id\":\"h:0\",\"kind\":\"sphere\",\"x\":%.2f,\"y\":%.2f,\"naik\":1.2,\"size\":1.1,\"color\":\"#4a9fe8\",\"label\":true},"
      "{\"id\":\"c:0\",\"kind\":\"disc\",\"x\":%.2f,\"y\":%.2f,\"naik\":0.12,\"size\":1.7},"
      "{\"id\":\"far\",\"kind\":\"diamond\",\"x\":%.2f,\"y\":%.2f,\"hm\":1,\"naik\":3.4,\"px\":6,\"label\":true},"
      "{\"id\":\"ln\",\"kind\":\"polyline\",\"size\":0.5,\"pts\":[{\"x\":%.2f,\"y\":%.2f},{\"x\":%.2f,\"y\":%.2f,\"naik\":2}]}]",
      ground[0], ground[1], ground[0], ground[1], ground[0] + 300, ground[1] + 300, ground[0] - 20, ground[1], ground[0] + 20, ground[1]);
    CHECK(eng_markers(mk));
    CHECK_EQ(eng_markers_count(), 4);
    CHECK(!eng_markers("nonsense")); CHECK_EQ(eng_markers_count(), 0);
    CHECK(eng_markers(mk));
    eng_frame(0.016f);   // drawn without GL errors
    float sc[4]; eng_project(ground[0], ground[1], 0, sc);
    std::string pk = eng_markers_pick(sc[0], sc[1] - 6, 16);
    CHECK_MSG(pk == "h:0" || pk == "c:0", "markers_pick at the handle -> " + pk);
    CHECK(std::string(eng_markers_pick(2, 2, 4)).empty());   // sky corner: nothing within 4 px
    Json scr = Json::parse(eng_markers_screen(), &err); CHECK_MSG(err.empty(), err);
    CHECK(scr.size() >= 1 && scr.size() <= 2);   // only the `label` markers (the far one may be behind the camera)
    bool sawH = false;
    for (const Json& e : scr.arr) if (e["id"].stringOr("") == "h:0") { sawH = true; CHECK(e["v"].boolOr(false)); CHECK_NEAR(e["x"].numberOr(0), sc[0], 3); CHECK(e["y"].numberOr(0) < sc[1]); }
    CHECK(sawH);
    CHECK(eng_markers("[]")); CHECK_EQ(eng_markers_count(), 0);
  }

  // ---- node height + rails rebuild: pin a mid-line node 6 m up, the profile follows (flat DEM: demBase 0)
  int ni = -1;
  for (size_t i = 0; i < g.nodes.size(); ++i) if (g.nodes[i].segs.size() == 2 && !g.nodes[i].isPoint()) { ni = (int)i; break; }
  CHECK(ni >= 0);
  const std::string nodeId = g.nodes[(size_t)ni].id; int seg0 = g.nodes[(size_t)ni].segs[0];
  double sAt = g.segments[(size_t)seg0].a == ni ? 0.0 : g.segments[(size_t)seg0].length;
  float before = scene.profile().railHeight(seg0, sAt);
  CHECK(eng_node_height(nodeId.c_str(), 6.0, 1));
  CHECK(!eng_node_height("no-such-node", 6.0, 1));
  CHECK(g.nodes[(size_t)ni].hasHeight);
  double ms = eng_rails_rebuild();
  std::printf("rails rebuild: %.0f ms (rail at %s: %.2f -> %.2f)\n", ms, nodeId.c_str(), before, scene.profile().railHeight(seg0, sAt));
  CHECK(ms > 0);
#ifdef NDEBUG
  CHECK_MSG(ms < 1000, "rails rebuild under 1 s (release)");
#else
  CHECK_MSG(ms < 8000, "rails rebuild (debug + sanitizers)");
#endif
  CHECK_NEAR(scene.profile().railHeight(seg0, sAt), 6.0, 0.6);
  {   // node info: the pin is reported raw (demBase 0 on flat ground), hand-written, one gradient per neighbour
    Json ni2 = Json::parse(eng_node_info(nodeId.c_str()), &err); CHECK_MSG(err.empty(), err);
    CHECK_NEAR(ni2["h"].numberOr(-1), 6.0, 1e-6); CHECK(ni2["tulis"].boolOr(false)); CHECK_EQ(ni2["grad"].size(), (size_t)2);
    CHECK(std::fabs(ni2["grad"][0].numberOr(0)) > 0.01);   // 6 m over a few hundred metres = tens of permille
    CHECK(eng_node_info("no-such-node")[0] == '\0');
  }
  { bool found = false; for (const Json& n : (*c.world)["graph"]["nodes"].arr) if (n["id"].stringOr("") == nodeId) { found = true; CHECK_NEAR(n["y"].numberOr(-1), 6.0, 1e-9); } CHECK(found); }
  CHECK(eng_node_height(nodeId.c_str(), 0, 0));
  CHECK(!g.nodes[(size_t)ni].hasHeight);
  eng_rails_rebuild();
  CHECK_NEAR(scene.profile().railHeight(seg0, sAt), before, 1e-3);
  { Json ni2 = Json::parse(eng_node_info(nodeId.c_str()), &err); CHECK(!ni2["tulis"].boolOr(true)); CHECK_NEAR(ni2["h"].numberOr(-1), before, 1e-3); }
  {   // pick_node: the handle projected through eng_project(mode 1) is found under its own pixel, whatever covers it
    float sc[4]; eng_project(g.nodes[(size_t)ni].wx, g.nodes[(size_t)ni].wy, 1, sc);
    if (sc[2] > 0.5f) { std::string p = eng_pick_node(sc[0], sc[1], 14); CHECK_MSG(p == nodeId, "pick_node -> " + p); }
    CHECK(eng_pick_node(1, 1, 2)[0] == '\0');
  }
  // editor overlays: node handles, node / segment highlight, ghost polylines draw without GL errors
  eng_node_handles(1, 0);
  eng_highlight(("node:" + nodeId).c_str(), 1);
  eng_frame(0.016f);
  eng_highlight(("segment:" + g.segments[(size_t)seg0].id + ":12.5").c_str(), 1);
  { char gl[200]; std::snprintf(gl, sizeof gl, "[[{\"x\":%.1f,\"y\":%.1f},{\"x\":%.1f,\"y\":%.1f}]]", g.nodes[(size_t)ni].wx, g.nodes[(size_t)ni].wy, g.nodes[(size_t)ni].wx + 40, g.nodes[(size_t)ni].wy + 10); CHECK(eng_ghost_lines(gl)); }
  eng_frame(0.016f);
  eng_node_handles(0, 0); eng_highlight("", 0); eng_ghost_lines("[]");
  eng_frame(0.016f);

  // ---- brush deltas: a +3 m node far from the rails lifts the ground there (bilinear peak at the grid node)
  double bx = ground[0] + 400, by = ground[1] + 400;
  int gx = (int)std::floor(bx / 8), gz = (int)std::floor(by / 8);
  float h0 = scene.groundHeight(gx * 8.0, gz * 8.0);
  char tanah[160]; std::snprintf(tanah, sizeof tanah, "{\"kisi\":8,\"delta\":{\"%d,%d\":3}}", gx, gz);
  CHECK(eng_terrain_delta(tanah));
  CHECK_NEAR(scene.groundHeight(gx * 8.0, gz * 8.0), h0 + 3, 0.01);
  CHECK((*c.world)["tanah"]["delta"].isObject());
  CHECK(eng_terrain_delta("null"));
  CHECK_NEAR(scene.groundHeight(gx * 8.0, gz * 8.0), h0, 1e-4);
  CHECK(!(*c.world)["tanah"].isObject());

  // ---- tree mask: stamps stored in the save, only the cells under them re-scattered (no imagery here: cells stay empty)
  { char vm[200]; std::snprintf(vm, sizeof vm, "[{\"x\":%.0f,\"y\":%.0f,\"r\":40,\"a\":-1}]", ground[0], ground[1]);
    CHECK(eng_veg_mask(vm)); CHECK_EQ((*c.world)["vegMask"].size(), (size_t)1); CHECK_EQ(scene.trees().mask().size(), (size_t)1);
    eng_frame(0.016f);
    CHECK(eng_veg_mask("null")); CHECK(!(*c.world)["vegMask"].isArray()); CHECK(scene.trees().mask().empty()); }

  // ---- full track edit: the same save again rebuilds everything and stays ready
  size_t segs = g.segments.size();
  CHECK(eng_track_edit(worldJson.c_str()));
  CHECK(!eng_track_edit("{\"graph\":{\"nodes\":[]}}"));
  CHECK(eng_ready()); CHECK_EQ(scene.graph().segments.size(), segs);
  eng_frame(0.016f);
  CHECK(std::strlen(eng_pick_ground(320, 200)) > 0);

  eng_shutdown();
  win.close();
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}
