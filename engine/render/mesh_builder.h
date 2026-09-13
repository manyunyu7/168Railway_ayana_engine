// CPU-side triangle mesh accumulator for procedural geometry (rails, ground, platforms...).
#pragma once
#include "engine/asset/model.h"
#include "engine/math/geometry.h"
#include "engine/rhi/rhi.h"
#include <span>
#include <vector>

namespace eng {

struct MeshBuilder {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  AABB bounds;

  uint32_t vertex(vec3 p, vec3 n, vec2 uv) { vertices.push_back({p, n, uv}); bounds.expand(p); return (uint32_t)vertices.size() - 1; }
  void triangle(uint32_t a, uint32_t b, uint32_t c) { indices.push_back(a); indices.push_back(b); indices.push_back(c); }
  void quad(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { triangle(a, b, c); triangle(a, c, d); }
  // Quad from 4 corner positions (counter-clockwise seen from the normal side), flat normal.
  void quad(vec3 a, vec3 b, vec3 c, vec3 d, vec2 uv0 = {0, 0}, vec2 uv1 = {1, 1}) {
    vec3 n = normalize(cross(b - a, d - a));
    uint32_t i = vertex(a, n, uv0);
    vertex(b, n, {uv1.x, uv0.y}); vertex(c, n, uv1); vertex(d, n, {uv0.x, uv1.y});
    quad(i, i + 1, i + 2, i + 3);
  }
  void box(vec3 min, vec3 max) {
    vec3 a{min.x, min.y, min.z}, b{max.x, min.y, min.z}, c{max.x, max.y, min.z}, d{min.x, max.y, min.z};
    vec3 e{min.x, min.y, max.z}, f{max.x, min.y, max.z}, g{max.x, max.y, max.z}, h{min.x, max.y, max.z};
    quad(f, e, h, g); quad(a, b, c, d);   // front (+z), back (-z)
    quad(b, f, g, c); quad(e, a, d, h);   // right, left
    quad(d, c, g, h); quad(e, f, b, a);   // top, bottom
  }
  void append(const MeshBuilder& o, const mat4& m) {
    uint32_t base = (uint32_t)vertices.size();
    for (const Vertex& v : o.vertices) vertex(m.transformPoint(v.pos), normalize(m.transformDir(v.normal)), v.uv);
    for (uint32_t i : o.indices) indices.push_back(base + i);
  }
  void computeSmoothNormals() {
    for (Vertex& v : vertices) v.normal = {};
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      Vertex &a = vertices[indices[i]], &b = vertices[indices[i + 1]], &c = vertices[indices[i + 2]];
      vec3 n = cross(b.pos - a.pos, c.pos - a.pos);
      a.normal += n; b.normal += n; c.normal += n;
    }
    for (Vertex& v : vertices) v.normal = normalize(v.normal);
  }
  bool empty() const { return indices.empty(); }
  void clear() { vertices.clear(); indices.clear(); bounds = {}; }

  rhi::Mesh upload() const {
    const rhi::Attribute layout[] = {{0, 3, sizeof(Vertex), 0}, {1, 3, sizeof(Vertex), 12}, {2, 2, sizeof(Vertex), 24}};
    return rhi::createMesh(std::as_bytes(std::span(vertices)), layout, indices);
  }
};

} // namespace eng
