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
#include "engine/app/camera_rig.h"
#include "engine/world/meja_board.h"
#include "engine/world/name_board.h"
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
#include "engine/world/walk_collision.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace eng {

struct WorldSceneStats {
  double buildMs = 0; std::string summary;
  unsigned hiasanDrawn = 0, hiasanInstanced = 0;   // per frame: visible placements, and how many of them went out instanced
  unsigned hiasanLod = 0;                          // of the drawn ones, how many used a coarser version
};

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
  // Per-frame time budget for the work that can be spread out (the tree scatter, today). 0 = do it all
  // at once, which is what the desktop tools and the tests want; a phone sets ~8 ms so the 60 000-tree
  // scatter of a map like Mojokerto streams in over a second of drawn frames instead of freezing the
  // render thread for ten. See eng_set_decor_budget.
  double decorBudgetMs = 0;
  bool decorStreaming() const { return trees_.scattering(); }
  void applyState(const SimState& st, double timeScale);
  // In-world meja board (station GLB `mejalayan` quads): the schematic layout from the bridge `panel` command.
  // The board redraws from the state given to applyState (nearest board within 45 m, <= every 120 ms).
  void setPanelLayout(const PanelLayout* layout) { meja_.setLayout(layout); }
  MejaBoard& meja() { return meja_; }
  const SimState& lastState() const { return state_; }
  // Draws the world for one frame. `dt` = real seconds (JPL arm animation, cloud drift), paused = 0.
  void draw(const mat4& viewProj, const mat4& view, vec3 eye, float fovY, int viewportH, double clock, float dt, double timeScale,
            float fogDensity, bool drawTrains = true);
  void drawHoverRing(vec3 pos, vec3 eye) { routes_.drawHoverRing(renderer_, pos, std::max(1.f, length(pos - eye) / 46)); }
  void destroy();

  // Screen-space pick (spec §6.5): nearest signal within 26 px wins over a point within 40 px unless the
  // point is closer and the signal is farther than 18 px. px/py in framebuffer pixels.
  void pickAt(float px, float py, int w, int h, const mat4& viewProj, vec3 eye, std::string& sigId, std::string& ptId) const;
  bool objectPos(const std::string& id, bool signal, vec3& out) const;   // hover ring anchor

  // Walk-mode collision (CameraRig WalkInput): the scenery triangles (hiasan station models: walls, floors,
  // ceilings, `peron`-named materials flagged as platforms; garis platforms/roads/decks: floors) built with the
  // decor, and the train vehicles as boxes (walls, collected per step since they move).
  const WalkCollider& walkCollider() const { if (walkDirty_) buildWalkCollider(); return walk_; }
  void collectWalkBoxes(std::vector<WalkBox>& out) const;
  float groundHeight(double wx, double wy) const { return terrain_.groundHeight(wx, wy); }
  float groundScene(float x, float z) const { return terrain_.groundHeight(x + origin_.ox, z + origin_.oz); }
  const WorldOrigin& origin() const { return origin_; }
  float worldWidth() const { return worldW_; }
  vec3 stationScene() const { return stationScene_; }
  void setStationScene(vec3 p) { stationScene_ = p; }

  ModelRenderer& renderer() { return renderer_; }
  const ModelRenderer& renderer() const { return renderer_; }
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
  // Hiasan LOD: a placement whose projected height on screen falls under lodPixels[n] draws its `lod<n+1>` version
  // (catalog `_kit.lod1`/`lod2`), falling back to the nearest finer one that is loaded and textured.
  static constexpr int MAX_LOD = 3;
  float lodPixels[MAX_LOD] = {200, 70, 25};
  bool built() const { return built_; }
  SceneLayers layers;
  // Camera context for the LOD rules (dunia3d.ts profilKam `acuan`): the mode's reference distance (bebas =
  // orbit distance, jalan 90, kabin 60, samping/ekor = their distance, atas = its height; CameraRig::acuan)
  // drives the iconic rail line, the points' skalaWesel and the glow "detail" flag; cabView (kabin) hides
  // the route / occupancy / shunting ribbons; benangAlways forces the iconic line (layer "benang").
  float refDistance = 0; bool cabView = false, benangAlways = false;

  // Debug helper (ENG_TEST_GARIS): a synthetic platform + fence + wall along the station track.
  static void injectTestGaris(Json& hiasan, const Json& summary, vec3& stationScene, const WorldOrigin& origin, Log log);

  // ---- editing (engine/app/world_edit.cpp; the C ABI in engine/api/engine_api_edit.cpp, docs/SURVEYOR.md) ----
  // Everything edits the save `world` object in place (the host serialises it back into its own World) and
  // re-places only what changed; a full buildStatic/buildDecor is the track editor's job (trackEdit).
  // Ray against the carved heightfield (march + bisect, Compass::groundHit); hit in scene space.
  bool rayGround(const Ray& ray, vec3& hit) const;
  // Nearest placed hiasan under the pixel: triangle-precise against the model's collision copy (AABB when a
  // model kept none), then the screen-space tolerance of uji3dTata.ts objDiLayar (TOL_PILIH 16 px for thin
  // / small targets only). Returns the index into world.hiasan.objek, -1 = none. px/py framebuffer pixels.
  int pickHiasan(const Ray& ray, float px, float py, int w, int h, const mat4& viewProj, vec3 eye) const;
  // Nearest rail centreline point within maxPx of the pixel: segment index, s (m) and side (+1 = the cursor is
  // left of the tangent, -1 right). False when nothing is that close.
  bool pickTrack(float px, float py, int w, int h, const mat4& viewProj, float maxPx, int& seg, double& s, int& side) const;
  // Nearest hiasan.garis polyline within maxPx (segments of the spline projected): index, -1 = none.
  int pickGaris(const Json& world, float px, float py, int w, int h, const mat4& viewProj, float maxPx) const;
  // Screen box (css-independent: framebuffer px) of the placed hiasan `objIndex`; false when behind the camera.
  bool hiasanScreenBox(int objIndex, const mat4& viewProj, int w, int h, float box[4]) const;
  // Transform of one hiasan entry (yaw degrees like the save); the model is looked up in the catalog.
  bool hiasanPlace(int objIndex, const Json& o);
  // Live edits: replace / add / remove one hiasan entry in `world` and re-place its model; the walk collider
  // and the meja scan are rebuilt lazily (next walkCollider() / draw). Return false when the index is bad.
  bool hiasanSet(Json& world, int objIndex, double wx, double wy, float naik, float rotDeg, float skala);
  int hiasanAdd(Json& world, const Json& obj);          // returns the new index
  bool hiasanRemove(Json& world, int objIndex);
  void hiasanRefresh(const Json& world);                  // every entry re-placed (the ground under them changed)
  // Station name board text of one entry (`teks` / `ketinggian`, uji3dPapanNama.ts): stored in `world` (empty =
  // field removed) and the `papan-nama` quads repainted (engine/world/name_board.h). False = bad index.
  bool hiasanText(Json& world, int objIndex, const std::string& teks, const std::string& ketinggian);
  bool hiasanHasBoard(int objIndex) const;                // the placed model carries a `papan-nama` quad (punyaPapan)
  // One hiasan.garis entry: {"index": i, ...entry} replaces (i = -1 / >= size appends), {"index": i, "remove": true}
  // deletes; the garis visuals are rebuilt (one build of every class - cheap next to the rails).
  bool garisSet(Json& world, const Json& patch);
  // Hand-written node height (raw DEM metres, `tinggi`): has = false clears it. Nothing is rebuilt until
  // railsRebuild().
  bool nodeHeight(Json& world, const std::string& nodeId, double h, bool has);
  // Profile + rails + terrain chords + signals / points / boards / JPL / routes from the current graph and node
  // heights; the trains and decor stay. Returns the elapsed milliseconds.
  double railsRebuild(const Json& world, const std::string& fontPath);
  // Brush deltas: `tanah` = the whole world.tanah object ({kisi, delta:{"gx,gz": m}}); stored in `world`, the
  // terrain re-carves only the tiles whose cells changed, hiasan / garis / boards on them are re-placed.
  bool terrainDelta(Json& world, const Json& tanah);
  // Full graph rebuild for the rail editor (and the host's trackside / scenery edits): a new save object ->
  // graph, then railsRebuild() and the hiasan / garis re-placed from it. The terrain (DEM, imagery, tiles:
  // re-cut lazily), city, clouds and trees stay; the scene origin is kept.
  bool trackEdit(const Json& world, const std::string& mapSlug, const std::string& fontPath, const Json* summary);
  // Tree brush (world.vegMask, dunia3d.ts sapuPohon): `stamps` = the whole [{x, y, r, a}] list (or null to clear),
  // stored in `world`; only the cells under stamps that differ from the previous list are re-scattered.
  bool vegMask(Json& world, const Json& stamps);
  // Rail head height (scene m) at the nearest track point within 250 m, else the carved ground (eng_project mode 1).
  float railHeadNear(double wx, double wy) const;

  // Overlays (drawn after the world, depth-test-off like the compass): selection box, gizmo, ghost, ukur.
  enum class HighlightMode { Off, Selected, Locked };
  // kind "hiasan" | "garis" (index) | "node" | "segment" (id, index ignored) | "": node = green sphere on the
  // handle (editorRel3d.ts sorotNode), segment = amber ribbon along the centreline (gambarSorotSeg).
  void setHighlight(const std::string& kind, int index, HighlightMode mode, const std::string& id = "");
  // Node handles (editorRel3d.ts segarkanTitikRel): screen-sized dots at rail head + 0.6 over every node with a
  // segment; colour = points orange, hand-written height magenta, chain end white, plain blue. tier 1 = only the
  // "important" ones (points / hand-written / ends). on = false hides them.
  void setNodeHandles(bool on, int tier);
  bool nodeHandlesOn() const { return ov_.handles; }
  // Editor ghost polylines (drag preview / chain draw, editorRel3d.ts gambarBayang / gambarGhost): world
  // point lists drawn as thin blue ribbons 0.9 m over the rail head near each point; empty clears.
  void setGhostLines(const std::vector<std::vector<std::pair<double, double>>>& lines);
  // Gizmo at a scene point: `kind` "rotate" (ring + needle + knob), "move" (ring + 4 arrows), "" hides;
  // axisHover 1 = ring, 2 = knob highlighted. `scale` = ring radius in metres.
  void setGizmo(const std::string& kind, vec3 pos, float yaw, float scale, int axisHover);
  // Which gizmo part is under the pixel: 0 none, 1 ring, 2 knob (thick hit ring 0.7..1.3 like uji3dTata).
  int gizmoHit(const Ray& ray) const;
  // Cursor angle in the gizmo plane (three convention: rotation.y = theta maps +X to (cos, -sin)); false = miss.
  bool gizmoAngle(const Ray& ray, float& angle) const;
  // Ghost of a catalog model at the cursor (translucent blue, uji3dTata jadikanHantu); id "" hides.
  void setGhost(const std::string& modelId, double wx, double wy, float rotDeg, float skala);
  // Measurement polyline (ukur.ts): [{x, y}] world points, drawn 0.4 m over the ground; empty clears.
  void setUkur(const std::vector<std::pair<double, double>>& pts);
  void drawOverlays(vec3 eye, float fovY);
  void destroyOverlays();
  // Scene position of a node handle (rail head at the node + 0.6, editorRel3d.ts posNode3D); false = no such node
  // (orphan nodes sit on the carved ground).
  bool nodeHandlePos(int nodeIndex, vec3& out) const;
  int hiasanCount() const { return (int)scenery_.size(); }
  size_t nameBoardCount() const { return papan_.count(); }

private:
  WorldOrigin origin_; vec3 stationScene_; float worldW_ = 8000;
  ModelRenderer renderer_; Sky sky_; Lighting light_;
  TrackGraph graph_; Terrain terrain_; VerticalProfile profile_; RailBuilder rails_;
  SignalVisuals signals_; PointVisuals points_; RouteVisuals routes_;
  TracksideBoards boards_; JplVisuals jpl_; CityVisuals city_; GarisVisuals garis_; CloudVisual clouds_;
  AssetCatalog catalog_; RollingStock stock_; TrainVisuals trains_;
  // model: the full version (collision, picking, boards); lod[n]: the coarser `<id>:lod<n+1>` catalog models,
  // nullptr while missing / not streamed yet (re-resolved each frame until they arrive). objIndex: world.hiasan.objek[]
  struct Placed { GpuModel* model; mat4 xf; AABB bounds; int objIndex = -1; std::string id; GpuModel* lod[MAX_LOD] = {}; int lodCount = 0; };
  std::vector<Placed> scenery_;
  GpuModel* pickLod(Placed& p, vec3 eye);   // the version to draw this frame (never nullptr)
  // Repeated hiasan (the same model placed several times: houses, shops, lamps) go out as one instanced draw
  // per model: each frame the visible placements are bucketed by model, singles drawn as before. Groups are
  // keyed by the catalog's GpuModel pointer (stable until destroy()); buffers grow on demand and live on.
  struct InstanceGroup { rhi::Buffer buf{}; size_t cap = 0; std::vector<mat4> mats; };
  std::map<GpuModel*, InstanceGroup> hiasanGroups_;
  void drawScenery(const Frustum& frustum);
  mutable WalkCollider walk_;
  mutable bool walkDirty_ = false;   // hiasan / garis edited: rebuilt on the next walkCollider()
  void buildWalkCollider() const;
  void scanMeja(const Json& world);
  void scanPapan(const Json& world);
  // overlays
  struct Overlay {
    std::string hlKind; int hlIndex = -1; HighlightMode hlMode = HighlightMode::Off; std::string hlId;
    bool handles = false; int handleTier = 0;
    rhi::Mesh ghostLines; unsigned ghostLineCount = 0;
    rhi::Mesh segMesh; std::string segMeshId;   // segment highlight ribbon, rebuilt when the id / rails change
    std::string gizmoKind; vec3 gizmoPos; float gizmoYaw = 0, gizmoScale = 1; int gizmoHover = 0;
    std::string ghostId; vec3 ghostPos; float ghostYaw = 0, ghostScale = 1;
    std::vector<vec3> ukur;
    rhi::Mesh ring, thickRing, needle, knob, arrow, bar, unitBox, sphere; bool built = false;
    rhi::Mesh ukurMesh; Material hlMat, gizmoMat, gizmoHotMat, needleMat, ghostMat, ukurMat, handleMat, ghostLineMat;
  } ov_;
  void buildOverlayMeshes();
  int viewportH_ = 1;   // framebuffer height of the last draw() (screen-sized handles)
  float fovY_ = 1; vec3 eye_;   // camera of the last draw() (hiasan LOD)
  const Json* worldForEdit_ = nullptr;   // the save object given to the last buildDecor / edit (garis / ukur lookups)
  Vegetation trees_;
  MejaBoard meja_; NameBoards papan_; SimState state_;
  std::string tileDir_;   // per-tile terrain source ("" = monolithic / streamed by the host)
  WorldSceneStats stats_;
  bool built_ = false, decor_ = false;
};

} // namespace eng
