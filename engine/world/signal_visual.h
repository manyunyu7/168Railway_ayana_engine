// Colour-light signals (docs/world-spec.md §6.2, reference uji3dSinyal.ts): striped mast, head plate
// with a semicircular top, lens rings, hoods (visors) with lips, LED-matrix lenses, diamond board with
// number panel, steel cage with the shunting octagon head, number plate; the lit lens gets two additive
// camera-facing coronas (silauSinyal). Beyond 900 m a coloured LOD sphere. Pengulang (Semboyan 9C):
// disc with 14 white LEDs in vertical/diagonal/horizontal bars.
// Semaphores (§6.3, trackside `bentuk:'mekanik'`): lattice mast, 1-2 arms pivoting at 7.0 m (masuk
// `… M…`) / 5.5 (keluar) / 5.0 (muka), spectacle glasses; arm angles follow the aspect with a
// damped spring (k 150, c 15) stepped by animate().
// Plate textures (uji3dSinyal.ts canvas textures, rasterised with engine/world/pixel_canvas.h): head plate
// with bolt rows and the platform grime, number plate text (font.efnt), the diamond's number panel with the
// digit as strips of lamps, and its lit overlay — shown while a route from the signal diverges
// (ruteMasukBelok; setAngkaLit from the sim routes).
// Local frame: -X = face (trains approach from -X), +Y up, +Z right of travel; origin = mast foot.
#pragma once
#include "engine/asset/model.h"
#include "engine/core/json.h"
#include "engine/render/mesh_builder.h"
#include "engine/render/model_renderer.h"
#include "engine/world/height_source.h"
#include "engine/world/track_graph.h"
#include <map>
#include <string>
#include <vector>

namespace eng {

enum class Aspect : uint8_t { Red, Yellow, Green };
inline Aspect aspectFromString(const std::string& s) { return s == "green" ? Aspect::Green : s == "yellow" ? Aspect::Yellow : Aspect::Red; }

struct SignalInstance {
  std::string id, name, signalType, segId;
  double s = 0; int dir = 1; bool left = false;
  int lamps = 3;                 // 2 or 3
  Aspect lensAspect[3]{};        // lens i (bottom -> top) shows this aspect when lit
  Aspect aspect = Aspect::Red;
  vec3 pos;                      // mast foot (scene)
  vec3 railPos;                  // on the track axis, for the LOD sphere
  float yaw = 0;
  mat4 world;
  vec3 lensWorld[3];             // lens centres (scene); pengulang: all = the centre LED
  // colour-light head variant (uji3dSinyal.ts OpsiSinyal): diamond board (+ number panel), steel cage,
  // shunting unit inside the cage; pengulang = 9C disc instead of the head
  bool board = false, cage = false, shunting = false, pengulang = false;
  int headVariant = -1;          // index into SignalVisuals' head mesh table
  char angka = '3';              // number panel digit (papanAngka)
  bool angkaLit = false;         // lit overlay (route from this signal diverges)
  rhi::Texture nameTex{};        // number plate text (owned by SignalVisuals::nameTex_)
  // semaphore
  bool mechanical = false;
  int arms = 1;                  // 1 or 2 (index 0 = top arm)
  float pivotY = 5.5f;           // top-arm pivot above the rail head (local y)
  float armAngle[2]{}, armVel[2]{}, armTarget[2]{};   // radians from horizontal (+ = raised 45°)
  vec3 pivotWorld;               // top-arm pivot (scene), used for picking
};

struct ScreenPoint { std::string id; float x = 0, y = 0; bool visible = false; };

class SignalVisuals {
public:
  static constexpr float LOD_DISTANCE = 900;
  // trackside: the save's world.trackside array (kind 'signal' entries are used).
  // fontPath: assets/font.efnt for the number plates ("" = blank plates).
  void build(const TrackGraph& g, const RailProfile& profile, const Json& trackside, const std::string& fontPath = "");
  void setAngkaLit(const std::string& id, bool lit) { int i = indexOf(id); if (i >= 0) signals_[(size_t)i].angkaLit = lit; }
  void setAspect(const std::string& id, const std::string& aspect) { setAspect(id, aspectFromString(aspect)); }
  void setAspect(const std::string& id, Aspect a);
  // Steps the semaphore arm springs; `dt` < 0 = measure real time since the previous call.
  void animate(float dt = -1);
  // Corona sizing needs the vertical field of view and viewport height (screen-space size floor,
  // §6.2); `night` brightens the coronas. Optional; defaults 52°, 800 px, day.
  void setView(float fovY, int viewportH, bool night) { fovY_ = fovY; viewportH_ = viewportH; night_ = night; }
  void draw(ModelRenderer& r, vec3 eye, const Frustum* frustum = nullptr) const;
  // Top lens (or LOD sphere) pixel position of every signal, for screen-space picking (§6.5).
  std::vector<ScreenPoint> screenPositions(const mat4& viewProj, int w, int h, vec3 eye) const;
  void destroy();
  const std::vector<SignalInstance>& signals() const { return signals_; }
  int indexOf(const std::string& id) const;

private:
  struct HeadMesh { int flags; rhi::Mesh dark, shell, plate; AABB bounds; };   // flags: bit0 3 lamps, 1 board, 2 cage, 3 shunting
  void buildMeshes();
  void buildSemaphoreMeshes();
  int headFor(int flags);
  void buildPengulang();
  void drawHead(ModelRenderer& r, const SignalInstance& s, int lit) const;
  void drawPengulang(ModelRenderer& r, const SignalInstance& s) const;
  void drawCorona(ModelRenderer& r, const SignalInstance& s, vec3 lens, vec3 eye, vec4 colour) const;
  void drawSemaphore(ModelRenderer& r, const SignalInstance& s) const;
  static void armTargets(const SignalInstance& s, float out[2]);
  std::vector<SignalInstance> signals_;
  std::vector<HeadMesh> heads_;
  rhi::Mesh mastYellow_, mastDark_, lens_, sphere_, billboard_;
  rhi::Mesh pengDark_, pengShell_, pengLed_;
  std::vector<vec3> pengLedPos_; std::vector<int> pengLedLine_;   // line: 0 red/horizontal, 1 yellow/diagonal, 2 green/vertical, 3 centre
  rhi::Texture lensTex_{}, coronaTex_{};
  rhi::Mesh panel_, plateNo_;                       // textured quads: number panel (PANEL_W x PANEL_H), number plate
  rhi::Texture plateTex_[2]{};                      // head plate, 2 / 3 lamps
  std::map<char, rhi::Texture> angkaTex_, angkaLitTex_;
  std::map<std::string, rhi::Texture> nameTex_;
  Material plateMat_, angkaMat_, angkaLitMat_;
  Material yellowMat_, darkMat_, shellMat_, unlitMat_, litMat_[3], whiteMat_, sphereMat_[3];
  Material coronaMat_;
  mutable std::vector<Material> coronaPool_;   // per-frame corona materials (see drawCorona)
  float fovY_ = 0.9075f; int viewportH_ = 800; bool night_ = false;
  // semaphore: lattice masts per pivot height (0 masuk 7.0, 1 keluar 5.5, 2 muka 5.0), arm parts
  rhi::Mesh semMast_[3], semMastDark_[3], semLamp_, armYellow_, armDark_, spectacle_, glass_;
  Material steelMat_, glassMat_[3];   // glass: red, green, yellow
  AABB semBounds_[3];
  double animClock_ = 0;
};

} // namespace eng
