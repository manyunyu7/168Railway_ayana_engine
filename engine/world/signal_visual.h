// Colour-light signals (docs/world-spec.md §6.2): mast with stripes, head plate, lens rings, hoods;
// per-signal aspect drives the lit lens' emissive colour. Beyond 900 m a coloured LOD sphere.
// Semaphores (§6.3, trackside `bentuk:'mekanik'`): lattice mast, 1-2 arms pivoting at 7.0 m (masuk
// `… M…`) / 5.5 (keluar) / 5.0 (muka), spectacle glasses; arm angles follow the aspect with a
// damped spring (k 150, c 15) stepped by animate().
// Local frame: -X = face (trains approach from -X), +Y up, +Z right of travel; origin = mast foot.
#pragma once
#include "engine/asset/model.h"
#include "engine/core/json.h"
#include "engine/render/mesh_builder.h"
#include "engine/render/model_renderer.h"
#include "engine/world/height_source.h"
#include "engine/world/track_graph.h"
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
  vec3 lensWorld[3];             // lens centres (scene)
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
  void build(const TrackGraph& g, const RailProfile& profile, const Json& trackside);
  void setAspect(const std::string& id, const std::string& aspect) { setAspect(id, aspectFromString(aspect)); }
  void setAspect(const std::string& id, Aspect a);
  // Steps the semaphore arm springs; `dt` < 0 = measure real time since the previous call.
  void animate(float dt = -1);
  void draw(ModelRenderer& r, vec3 eye, const Frustum* frustum = nullptr) const;
  // Top lens (or LOD sphere) pixel position of every signal, for screen-space picking (§6.5).
  std::vector<ScreenPoint> screenPositions(const mat4& viewProj, int w, int h, vec3 eye) const;
  void destroy();
  const std::vector<SignalInstance>& signals() const { return signals_; }
  int indexOf(const std::string& id) const;

private:
  void buildMeshes();
  void buildSemaphoreMeshes();
  void drawSemaphore(ModelRenderer& r, const SignalInstance& s) const;
  static void armTargets(const SignalInstance& s, float out[2]);
  std::vector<SignalInstance> signals_;
  rhi::Mesh mastYellow_, dark2_, dark3_, lens_, sphere_;
  Material yellowMat_, darkMat_, unlitMat_, litMat_[3], sphereMat_[3];
  AABB bodyBounds_;
  // semaphore: lattice masts per pivot height (0 masuk 7.0, 1 keluar 5.5, 2 muka 5.0), arm parts
  rhi::Mesh semMast_[3], semMastDark_[3], semLamp_, armYellow_, armDark_, spectacle_, glass_;
  Material steelMat_, glassMat_[3];   // glass: red, green, yellow
  AABB semBounds_[3];
  double animClock_ = 0;
};

} // namespace eng
