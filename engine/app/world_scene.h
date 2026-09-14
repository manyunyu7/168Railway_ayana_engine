// WorldScene — everything drawn from a save + the per-step sim state: track graph, terrain, rails, signals,
// points, routes, boards, JPL gates, city, hiasan objects, spline objects, trees, clouds, trains.
// Shared by the native app (examples/ppka: input, HUD, panel, cameras stay there) and the web C ABI
// (engine/api). The build is split in two so the web build can stream: buildStatic() needs the terrain
// data and the save, buildDecor() needs the catalog models (hiasan / garis / vegetasi) — the native app
// calls both back to back.
#pragma once
#include "engine/core/json.h"
#include "engine/render/model_renderer.h"
#include "engine/render/sky.h"
#include "engine/sim/sim_state.h"
#include "engine/world/asset_catalog.h"
#include "engine/world/board_visual.h"
#include "engine/world/city_visual.h"
#include "engine/world/cloud_visual.h"
#include "engine/world/coords.h"
#include "engine/world/garis_visual.h"
#include "engine/world/jpl_visual.h"
#include "engine/world/point_visual.h"
#include "engine/world/rail_builder.h"
#include "engine/world/rail_profile.h"
#include "engine/world/rolling_stock.h"
#include "engine/world/route_visual.h"
#include "engine/world/signal_visual.h"
#include "engine/world/terrain.h"
#include "engine/world/track_graph.h"
#include "engine/world/train_visual.h"
#include "engine/world/vegetation.h"
#include <functional>
#include <string>
#include <vector>

namespace eng {

struct WorldSceneStats { double buildMs = 0; std::string summary; };

// Visibility toggles (the web client's "Tampilan" drawer, dunia3dKonst.ts TAMPIL_BAKU): everything on by default
// here, the host applies its own defaults (pita and wesel off in the reference client). Hidden point arrows are
// also not pickable (turning a switch without seeing its setting is guessing).
struct SceneLayers { bool pita = true, wesel = true, pohon = true, awan = true, kota = true; };

class WorldScene {
public:
  using Log = std::function<void(const std::string&)>;

  void initGpu();   // renderer + sky (needs a GL context)
  // Sets the origin / station target / corridor width from the save (before buildStatic; also usable
  // before the terrain data exists, e.g. to know which tiles to fetch).
  void setWorld(const Json& world);
  // Native terrain source: `<dir>/<map>/index.json` + per-tile files when present with a desktop
  // target (tools/fetch_tiles --target desktop), else the monolithic `<dir>/<map>.dem/.sat`. The DEM is read
  // eagerly; satellite tiles are read on request while streaming.
  bool loadTerrain(const std::string& terrainDir, const std::string& mapSlug, std::string& error);
  // Streaming step (every frame): terrain tiles + tree scatter follow `centre` (camera focus, scene space).
  void updateStreaming(vec3 centre, float dt);
  // Unthrottled fill around `centre` (native start: everything in the radii is built on return).
  void primeStreaming(vec3 centre);
  // Static part. `world` = the save's "world" object, `mapSlug` = save name (city bake, terrain files).
  // The terrain must already hold its data (Terrain::load or the streamed tiles + finishTiles()).
  // fontPath / cityPath: "" skips the boards' text atlas / the city.
  bool buildStatic(const Json& world, const std::string& mapSlug, const std::string& fontPath, const std::string& cityPath, Log log);
  // Decor part: hiasan objects, garis, trees. Needs catalog_ loaded (models are looked up here). Re-runnable
  // (the web build calls it again once streamed models arrived).
  void buildDecor(const Json& world, bool testGaris, const Json* summary);
  void applyState(const SimState& st, double timeScale);
  // Draws the world for one frame. `dt` = real seconds (JPL arm animation, cloud drift), paused = 0.
  void draw(const mat4& viewProj, const mat4& view, vec3 eye, float fovY, int viewportH, double clock, float dt, double timeScale,
            float fogDensity, bool drawTrains = true);
  void drawHoverRing(vec3 pos, vec3 eye) { routes_.drawHoverRing(renderer_, pos, std::max(1.f, length(pos - eye) / 46)); }
  void destroy();

  // Screen-space pick (spec §6.5): nearest signal within 26 px wins over a point within 40 px unless the
  // point is closer and the signal is farther than 18 px. px/py in framebuffer pixels.
  void pickAt(float px, float py, int w, int h, const mat4& viewProj, vec3 eye, std::string& sigId, std::string& ptId) const;
  bool objectPos(const std::string& id, bool signal, vec3& out) const;   // hover ring anchor

  float groundHeight(double wx, double wy) const { return terrain_.groundHeight(wx, wy); }
  float groundScene(float x, float z) const { return terrain_.groundHeight(x + origin_.ox, z + origin_.oz); }
  const WorldOrigin& origin() const { return origin_; }
  float worldWidth() const { return worldW_; }
  vec3 stationScene() const { return stationScene_; }
  void setStationScene(vec3 p) { stationScene_ = p; }

  ModelRenderer& renderer() { return renderer_; }
  Sky& sky() { return sky_; }
  Lighting& lighting() { return light_; }
  Terrain& terrain() { return terrain_; }
  AssetCatalog& catalog() { return catalog_; }
  RollingStock& stock() { return stock_; }
  TrackGraph& graph() { return graph_; }
  VerticalProfile& profile() { return profile_; }
  const VerticalProfile& profile() const { return profile_; }
  RailBuilder& rails() { return rails_; }
  SignalVisuals& signals() { return signals_; }
  const SignalVisuals& signals() const { return signals_; }
  PointVisuals& points() { return points_; }
  const PointVisuals& points() const { return points_; }
  RouteVisuals& routes() { return routes_; }
  TrainVisuals& trains() { return trains_; }
  CityVisuals& city() { return city_; }
  GarisVisuals& garis() { return garis_; }
  Vegetation& trees() { return trees_; }
  CloudVisual& clouds() { return clouds_; }
  TracksideBoards& boards() { return boards_; }
  JplVisuals& jpl() { return jpl_; }
  const WorldSceneStats& stats() const { return stats_; }
  bool built() const { return built_; }
  SceneLayers layers;

  // Debug helper (ENG_TEST_GARIS): a synthetic platform + fence + wall along the station track.
  static void injectTestGaris(Json& hiasan, const Json& summary, vec3& stationScene, const WorldOrigin& origin, Log log);

private:
  WorldOrigin origin_; vec3 stationScene_; float worldW_ = 8000;
  ModelRenderer renderer_; Sky sky_; Lighting light_;
  TrackGraph graph_; Terrain terrain_; VerticalProfile profile_; RailBuilder rails_;
  SignalVisuals signals_; PointVisuals points_; RouteVisuals routes_;
  TracksideBoards boards_; JplVisuals jpl_; CityVisuals city_; GarisVisuals garis_; CloudVisual clouds_;
  AssetCatalog catalog_; RollingStock stock_; TrainVisuals trains_;
  struct Placed { GpuModel* model; mat4 xf; AABB bounds; };
  std::vector<Placed> scenery_;
  Vegetation trees_;
  std::string tileDir_;   // per-tile terrain source ("" = monolithic / streamed by the host)
  WorldSceneStats stats_;
  bool built_ = false, decor_ = false;
};

} // namespace eng
