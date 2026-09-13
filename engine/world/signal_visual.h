// Colour-light signals (docs/world-spec.md §6.2): mast with stripes, head plate, lens rings, hoods;
// per-signal aspect drives the lit lens' emissive colour. Beyond 900 m a coloured LOD sphere.
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
};

struct ScreenPoint { std::string id; float x = 0, y = 0; bool visible = false; };

class SignalVisuals {
public:
  static constexpr float LOD_DISTANCE = 900;
  // trackside: the save's world.trackside array (kind 'signal' entries are used).
  void build(const TrackGraph& g, const RailProfile& profile, const Json& trackside);
  void setAspect(const std::string& id, const std::string& aspect) { setAspect(id, aspectFromString(aspect)); }
  void setAspect(const std::string& id, Aspect a);
  void draw(ModelRenderer& r, vec3 eye, const Frustum* frustum = nullptr) const;
  // Top lens (or LOD sphere) pixel position of every signal, for screen-space picking (§6.5).
  std::vector<ScreenPoint> screenPositions(const mat4& viewProj, int w, int h, vec3 eye) const;
  void destroy();
  const std::vector<SignalInstance>& signals() const { return signals_; }
  int indexOf(const std::string& id) const;

private:
  void buildMeshes();
  std::vector<SignalInstance> signals_;
  rhi::Mesh mastYellow_, dark2_, dark3_, lens_, sphere_;
  Material yellowMat_, darkMat_, unlitMat_, litMat_[3], sphereMat_[3];
  AABB bodyBounds_;
};

} // namespace eng
