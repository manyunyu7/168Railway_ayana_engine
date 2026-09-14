// Markers — a batch of world-space editor markers (spheres, discs, cubes, diamonds, cones, segments, polylines)
// set from JSON by the host and drawn each frame after the world, depth-test-off by default like the compass
// (docs/SURVEYOR.md, `eng_markers`). The surveyor tools of the web client use them for what three.js drew as
// handle meshes: spline point handles / end markers (uji3dSpline.ts), trackside / scenery markers and their
// stalks (uji3dObjekRel.ts), measurement polylines (ukur.ts). Picking answers the id under a pixel.
//
// JSON: [{"id": "h:3", "kind": "sphere", "x": wx, "y": wy, "naik": 1.2, "hm": 0, "color": "#4a9fe8",
//         "alpha": 1, "size": 1.1, "px": 0, "depth": false, "yaw": 0, "label": false,
//         "pts": [{"x", "y", "naik", "hm", "h"}]}]
//   kind   sphere | disc (flat ring on the ground plane) | cube | diamond (octahedron) | cone (tip along +yaw,
//          three convention) | segment / polyline (`pts`, ribbon + vertical fin so it reads from every angle)
//   x, y   world (Mercator m); the height = ground / rail head (hm 0 / 1) at the point + `naik`, or `h` = an
//          absolute scene height when given
//   size   metres (radius for sphere / disc / diamond, edge for cube, length for cone, ribbon width for lines)
//   px     > 0: point markers keep this SCREEN radius instead (handles that must stay grabbable when far)
//   depth  true = occluded by the world (default false: overlay)
//   label  true = listed by screen() (the host's DOM name plates)
#pragma once
#include "engine/core/json.h"
#include "engine/math/geometry.h"
#include "engine/render/model_renderer.h"
#include "engine/world/coords.h"
#include <functional>
#include <string>
#include <vector>

namespace eng {

struct MarkerScreen { std::string id; float x, y, dist; bool visible; };

class Markers {
public:
  // Height at a world point: mode 0 carved ground, 1 rail head at the nearest track point.
  using HeightFn = std::function<float(double, double, int)>;
  enum class Kind { Sphere, Disc, Cube, Diamond, Cone, Line };
  struct Marker {
    std::string id; Kind kind = Kind::Sphere;
    std::vector<vec3> pts;   // scene points (1 for the point kinds)
    vec3 color{1, 1, 1}; float alpha = 1, size = 1, px = 0, yaw = 0; bool depth = false, label = false;
    rhi::Mesh mesh;          // Line kinds only (built per marker: colour is per draw)
  };

  // Replaces the whole batch. False = not an array (the batch is cleared anyway).
  bool set(const Json& arr, const WorldOrigin& origin, const HeightFn& height);
  void clear();
  // Draws after the world; fovY / viewportH (framebuffer px) scale the `px` markers.
  void draw(ModelRenderer& r, vec3 eye, float fovY, int viewportH);
  // Id of the nearest marker within maxPx of the pixel (framebuffer px), "" = none. Point markers count
  // their projected radius too; lines measure to the projected polyline. Ties go to the camera-nearest.
  std::string pick(float px, float py, int w, int h, const mat4& viewProj, vec3 eye, float maxPx) const;
  // Screen anchors (top of the marker) of every `label` marker; css px when w / h are css.
  std::vector<MarkerScreen> screen(const mat4& viewProj, int w, int h, vec3 eye) const;
  bool anchor(const std::string& id, vec3& out) const;
  size_t count() const { return items_.size(); }
  void destroy();

private:
  std::vector<Marker> items_;
  rhi::Mesh sphere_, disc_, cube_, diamond_, cone_; bool built_ = false;
  void buildMeshes();
  static vec3 top(const Marker& m);
  float worldRadius(const Marker& m, vec3 eye, float fovY, int viewportH) const;
};

} // namespace eng
