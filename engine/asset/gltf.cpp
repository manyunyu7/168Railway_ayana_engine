#include "engine/asset/gltf.h"
#include "engine/core/json.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace eng {

void Model::computeBounds() {
  boundsMin = {1e30f, 1e30f, 1e30f}; boundsMax = -boundsMin;
  std::vector<mat4> world(nodes.size());
  // parents always come before children in our ordering (roots first, DFS)
  auto visit = [&](auto&& self, int n, const mat4& parentM) -> void {
    world[n] = parentM * nodes[n].local;
    if (nodes[n].mesh >= 0)
      for (const Primitive& p : meshes[nodes[n].mesh].primitives) {
        // transform the 8 corners of the primitive box
        for (int c = 0; c < 8; ++c) {
          vec3 corner{c & 1 ? p.boundsMax.x : p.boundsMin.x, c & 2 ? p.boundsMax.y : p.boundsMin.y, c & 4 ? p.boundsMax.z : p.boundsMin.z};
          vec3 w = world[n].transformPoint(corner);
          boundsMin = vmin(boundsMin, w); boundsMax = vmax(boundsMax, w);
        }
      }
    for (int ch : nodes[n].children) self(self, ch, world[n]);
  };
  for (int r : roots) visit(visit, r, mat4::identity());
  if (boundsMin.x > boundsMax.x) boundsMin = boundsMax = {};
}

namespace {

struct Accessor {
  const uint8_t* data = nullptr; size_t count = 0; int comps = 1; int compType = 5126; int stride = 0; bool normalized = false;
  float readFloat(size_t i, int c) const {
    const uint8_t* p = data + i * stride;
    switch (compType) {
      case 5126: { float f; std::memcpy(&f, p + c * 4, 4); return f; }
      case 5123: { uint16_t v; std::memcpy(&v, p + c * 2, 2); return normalized ? v / 65535.f : (float)v; }
      case 5121: { uint8_t v = p[c]; return normalized ? v / 255.f : (float)v; }
      case 5122: { int16_t v; std::memcpy(&v, p + c * 2, 2); return normalized ? std::fmax(v / 32767.f, -1.f) : (float)v; }
      case 5120: { int8_t v = (int8_t)p[c]; return normalized ? std::fmax(v / 127.f, -1.f) : (float)v; }
      case 5125: { uint32_t v; std::memcpy(&v, p + c * 4, 4); return (float)v; }
    }
    return 0;
  }
  uint32_t readIndex(size_t i) const {
    const uint8_t* p = data + i * stride;
    switch (compType) {
      case 5125: { uint32_t v; std::memcpy(&v, p, 4); return v; }
      case 5123: { uint16_t v; std::memcpy(&v, p, 2); return v; }
      case 5121: return p[0];
    }
    return 0;
  }
};

int componentSize(int t) { return t == 5126 || t == 5125 ? 4 : t == 5123 || t == 5122 ? 2 : 1; }
int typeComponents(const std::string& t) {
  if (t == "SCALAR") return 1; if (t == "VEC2") return 2; if (t == "VEC3") return 3; if (t == "VEC4") return 4;
  if (t == "MAT4") return 16; if (t == "MAT3") return 9; if (t == "MAT2") return 4; return 1;
}

struct Ctx {
  const Json& doc; std::span<const uint8_t> bin; std::string& err;

  bool bufferView(int idx, const uint8_t*& ptr, size_t& len, int& stride) {
    const Json& bv = doc["bufferViews"][(size_t)idx];
    if (bv.isNull()) { err = "bad bufferView " + std::to_string(idx); return false; }
    if (bv["buffer"].intOr(0) != 0) { err = "external buffers not supported"; return false; }
    size_t off = (size_t)bv["byteOffset"].numberOr(0); len = (size_t)bv["byteLength"].numberOr(0);
    stride = bv["byteStride"].intOr(0);
    if (off + len > bin.size()) { err = "bufferView out of range"; return false; }
    ptr = bin.data() + off; return true;
  }

  bool accessor(int idx, Accessor& a) {
    const Json& ac = doc["accessors"][(size_t)idx];
    if (ac.isNull()) { err = "bad accessor " + std::to_string(idx); return false; }
    if (!ac.has("bufferView")) { err = "sparse/empty accessors not supported"; return false; }
    const uint8_t* ptr; size_t len; int stride;
    if (!bufferView(ac["bufferView"].intOr(0), ptr, len, stride)) return false;
    a.count = (size_t)ac["count"].numberOr(0);
    a.compType = ac["componentType"].intOr(5126);
    a.comps = typeComponents(ac["type"].stringOr("SCALAR"));
    a.normalized = ac["normalized"].boolOr(false);
    int elem = componentSize(a.compType) * a.comps;
    a.stride = stride ? stride : elem;
    size_t off = (size_t)ac["byteOffset"].numberOr(0);
    if (off + (a.count ? (a.count - 1) * a.stride + elem : 0) > len) { err = "accessor out of range"; return false; }
    a.data = ptr + off; return true;
  }
};

// Rotation quaternion of an orthonormal 3x3 (column-major mat4), Shepperd's method.
quat quatFromMat(const mat4& m) {
  float m00 = m.m[0][0], m11 = m.m[1][1], m22 = m.m[2][2];
  float m01 = m.m[0][1], m02 = m.m[0][2], m10 = m.m[1][0], m12 = m.m[1][2], m20 = m.m[2][0], m21 = m.m[2][1];
  float tr = m00 + m11 + m22; quat q;
  if (tr > 0) { float s = std::sqrt(tr + 1) * 2; q.w = 0.25f * s; q.x = (m12 - m21) / s; q.y = (m20 - m02) / s; q.z = (m01 - m10) / s; }
  else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1 + m00 - m11 - m22) * 2; q.w = (m12 - m21) / s; q.x = 0.25f * s; q.y = (m10 + m01) / s; q.z = (m20 + m02) / s; }
  else if (m11 > m22) { float s = std::sqrt(1 + m11 - m00 - m22) * 2; q.w = (m20 - m02) / s; q.x = (m10 + m01) / s; q.y = 0.25f * s; q.z = (m21 + m12) / s; }
  else { float s = std::sqrt(1 + m22 - m00 - m11) * 2; q.w = (m01 - m10) / s; q.x = (m20 + m02) / s; q.y = (m21 + m12) / s; q.z = 0.25f * s; }
  return q;
}

void nodeTransform(const Json& n, Node& node) {
  if (n.has("matrix")) {
    mat4 m; const Json& a = n["matrix"];
    for (int i = 0; i < 16; ++i) (&m.m[0][0])[i] = (float)a[(size_t)i].numberOr(i % 5 == 0 ? 1 : 0);
    node.local = m;
    // decompose so animated channels can still replace a single component
    node.translation = {m.m[3][0], m.m[3][1], m.m[3][2]};
    vec3 cx{m.m[0][0], m.m[0][1], m.m[0][2]}, cy{m.m[1][0], m.m[1][1], m.m[1][2]}, cz{m.m[2][0], m.m[2][1], m.m[2][2]};
    node.scale = {length(cx), length(cy), length(cz)};
    float det = dot(cx, cross(cy, cz)); if (det < 0) node.scale.x = -node.scale.x;
    mat4 r = mat4::identity();
    if (node.scale.x != 0 && node.scale.y != 0 && node.scale.z != 0) {
      cx = cx / node.scale.x; cy = cy / node.scale.y; cz = cz / node.scale.z;
      r.m[0][0] = cx.x; r.m[0][1] = cx.y; r.m[0][2] = cx.z; r.m[1][0] = cy.x; r.m[1][1] = cy.y; r.m[1][2] = cy.z; r.m[2][0] = cz.x; r.m[2][1] = cz.y; r.m[2][2] = cz.z;
    }
    node.rotation = quatFromMat(r);
    return;
  }
  if (n.has("translation")) node.translation = {(float)n["translation"][0].num, (float)n["translation"][1].num, (float)n["translation"][2].num};
  if (n.has("scale")) node.scale = {(float)n["scale"][0].num, (float)n["scale"][1].num, (float)n["scale"][2].num};
  if (n.has("rotation")) node.rotation = {(float)n["rotation"][0].num, (float)n["rotation"][1].num, (float)n["rotation"][2].num, (float)n["rotation"][3].num};
  node.local = trs(node.translation, node.rotation, node.scale);
}

} // namespace

bool loadGlb(std::span<const uint8_t> bytes, Model& out, std::string& err) {
  if (bytes.size() < 20 || std::memcmp(bytes.data(), "glTF", 4) != 0) { err = "not a GLB file"; return false; }
  uint32_t total; std::memcpy(&total, bytes.data() + 8, 4);
  if (total > bytes.size()) { err = "truncated GLB"; return false; }
  std::string_view jsonText; std::span<const uint8_t> bin;
  for (size_t p = 12; p + 8 <= total;) {
    uint32_t len, type; std::memcpy(&len, bytes.data() + p, 4); std::memcpy(&type, bytes.data() + p + 4, 4);
    if (p + 8 + len > total) { err = "bad chunk"; return false; }
    if (type == 0x4E4F534A) jsonText = {(const char*)bytes.data() + p + 8, len};
    else if (type == 0x004E4942) bin = bytes.subspan(p + 8, len);
    p += 8 + len;
  }
  if (jsonText.empty()) { err = "no JSON chunk"; return false; }
  std::string jerr; Json doc = Json::parse(jsonText, &jerr);
  if (!jerr.empty()) { err = "JSON: " + jerr; return false; }
  // Required extensions the parser understands: KHR_materials_unlit (Material::unlit), KHR_lights_punctual (lights
  // are not part of the model: ignored), EXT_texture_webp (the image source moves under the extension; the bytes
  // stay encoded as always, the converter / the KTX2 twin provide the pixels). Anything else (Draco, meshopt) is refused.
  for (const Json& e : doc["extensionsRequired"].arr) {
    std::string n = e.stringOr("?");
    if (n != "KHR_materials_unlit" && n != "KHR_lights_punctual" && n != "EXT_texture_webp") { err = "required extension: " + n; return false; }
  }
  Ctx ctx{doc, bin, err};

  // images (kept encoded; converter decodes)
  for (const Json& im : doc["images"].arr) {
    Image img; img.mime = im["mimeType"].stringOr("");
    if (!im.has("bufferView")) { err = "image with URI not supported (embed in GLB)"; return false; }
    const uint8_t* ptr; size_t len; int stride;
    if (!ctx.bufferView(im["bufferView"].intOr(0), ptr, len, stride)) return false;
    img.encoded.assign(ptr, ptr + len);
    out.images.push_back(std::move(img));
  }
  // textures: map texture index -> image index; the sampler's wrap mode is recorded on the image
  // (one image is in practice referenced by one texture; the last one wins otherwise)
  std::vector<int> texToImage;
  for (const Json& t : doc["textures"].arr) {
    int img = t["source"].intOr(-1);
    if (img < 0) img = t["extensions"]["EXT_texture_webp"]["source"].intOr(-1);
    texToImage.push_back(img);
    if (img < 0 || img >= (int)out.images.size() || !t.has("sampler")) continue;
    const Json& sm = doc["samplers"][(size_t)t["sampler"].intOr(0)];
    auto wrap = [](int w) -> uint8_t { return w == 33071 ? 1 : w == 33648 ? 2 : 0; };
    out.images[(size_t)img].wrapS = wrap(sm["wrapS"].intOr(10497)); out.images[(size_t)img].wrapT = wrap(sm["wrapT"].intOr(10497));
  }
  auto tex = [&](const Json& info, bool linear = false) {
    int i = info["index"].intOr(-1); int img = i >= 0 && i < (int)texToImage.size() ? texToImage[(size_t)i] : -1;
    if (linear && img >= 0) out.images[(size_t)img].linear = true;
    return img;
  };

  for (const Json& m : doc["materials"].arr) {
    Material mat; mat.name = m["name"].stringOr("");
    const Json& pbr = m["pbrMetallicRoughness"];
    if (pbr.has("baseColorFactor")) mat.baseColor = {(float)pbr["baseColorFactor"][0].num, (float)pbr["baseColorFactor"][1].num, (float)pbr["baseColorFactor"][2].num, (float)pbr["baseColorFactor"][3].num};
    mat.metallic = (float)pbr["metallicFactor"].numberOr(1);
    mat.roughness = (float)pbr["roughnessFactor"].numberOr(1);
    if (pbr.has("baseColorTexture")) {
      const Json& bt = pbr["baseColorTexture"];
      mat.baseColorTex = tex(bt); mat.baseColorUv = bt["texCoord"].intOr(0);
      const Json& tt = bt["extensions"]["KHR_texture_transform"];   // baked into the vertex UVs below
      if (!tt.isNull()) {
        mat.uvOffset = {(float)tt["offset"][0].numberOr(0), (float)tt["offset"][1].numberOr(0)};
        mat.uvScale = {(float)tt["scale"][0].numberOr(1), (float)tt["scale"][1].numberOr(1)};
        mat.uvRotation = (float)tt["rotation"].numberOr(0);
        if (tt.has("texCoord")) mat.baseColorUv = tt["texCoord"].intOr(mat.baseColorUv);
      }
    }
    if (pbr.has("metallicRoughnessTexture")) mat.metalRoughTex = tex(pbr["metallicRoughnessTexture"], true);
    if (m.has("normalTexture")) mat.normalTex = tex(m["normalTexture"], true);
    if (m.has("emissiveTexture")) mat.emissiveTex = tex(m["emissiveTexture"]);   // sRGB per spec
    if (m.has("occlusionTexture")) mat.occlusionTex = tex(m["occlusionTexture"], true);
    if (m.has("emissiveFactor")) mat.emissive = {(float)m["emissiveFactor"][0].num, (float)m["emissiveFactor"][1].num, (float)m["emissiveFactor"][2].num};
    mat.emissive = mat.emissive * (float)m["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"].numberOr(1);
    std::string am = m["alphaMode"].stringOr("OPAQUE");
    mat.alphaMode = am == "BLEND" ? AlphaMode::Blend : am == "MASK" ? AlphaMode::Mask : AlphaMode::Opaque;
    mat.alphaCutoff = (float)m["alphaCutoff"].numberOr(0.5);
    mat.doubleSided = m["doubleSided"].boolOr(false);
    mat.unlit = m["extensions"].has("KHR_materials_unlit");
    out.materials.push_back(std::move(mat));
  }

  for (const Json& me : doc["meshes"].arr) {
    Mesh mesh; mesh.name = me["name"].stringOr("");
    for (const Json& pr : me["primitives"].arr) {
      int mode = pr["mode"].intOr(4);
      if (mode != 4) continue;                       // triangles only
      Primitive prim; prim.material = pr["material"].intOr(-1);
      const Json& at = pr["attributes"];
      if (!at.has("POSITION")) continue;
      // one UV set per vertex: take the set the material's base colour texture samples
      int uvSet = prim.material >= 0 && prim.material < (int)out.materials.size() ? out.materials[(size_t)prim.material].baseColorUv : 0;
      std::string uvKey = "TEXCOORD_" + std::to_string(uvSet);
      if (!at.has(uvKey)) uvKey = "TEXCOORD_0";
      Accessor pos, nrm, uv; bool hasN = at.has("NORMAL"), hasUV = at.has(uvKey);
      if (!ctx.accessor(at["POSITION"].intOr(0), pos)) return false;
      if (hasN && !ctx.accessor(at["NORMAL"].intOr(0), nrm)) return false;
      if (hasUV && !ctx.accessor(at[uvKey].intOr(0), uv)) return false;
      // KHR_texture_transform of the base colour texture, baked: uv' = offset + R(rotation) * (scale * uv)
      const Material* pm = prim.material >= 0 && prim.material < (int)out.materials.size() ? &out.materials[(size_t)prim.material] : nullptr;
      bool xform = pm && (pm->uvOffset.x != 0 || pm->uvOffset.y != 0 || pm->uvScale.x != 1 || pm->uvScale.y != 1 || pm->uvRotation != 0);
      float cr = xform ? std::cos(pm->uvRotation) : 1, sr = xform ? std::sin(pm->uvRotation) : 0;
      prim.vertices.resize(pos.count);
      prim.boundsMin = {1e30f, 1e30f, 1e30f}; prim.boundsMax = -prim.boundsMin;
      for (size_t i = 0; i < pos.count; ++i) {
        Vertex& v = prim.vertices[i];
        v.pos = {pos.readFloat(i, 0), pos.readFloat(i, 1), pos.readFloat(i, 2)};
        if (hasN) v.normal = {nrm.readFloat(i, 0), nrm.readFloat(i, 1), nrm.readFloat(i, 2)};
        if (hasUV) v.uv = {uv.readFloat(i, 0), uv.readFloat(i, 1)};
        if (xform) { vec2 t{v.uv.x * pm->uvScale.x, v.uv.y * pm->uvScale.y}; v.uv = {pm->uvOffset.x + cr * t.x - sr * t.y, pm->uvOffset.y + sr * t.x + cr * t.y}; }
        prim.boundsMin = vmin(prim.boundsMin, v.pos); prim.boundsMax = vmax(prim.boundsMax, v.pos);
      }
      if (pr.has("indices")) {
        Accessor idx; if (!ctx.accessor(pr["indices"].intOr(0), idx)) return false;
        prim.indices.resize(idx.count);
        for (size_t i = 0; i < idx.count; ++i) prim.indices[i] = idx.readIndex(i);
      } else {
        prim.indices.resize(pos.count);
        for (size_t i = 0; i < pos.count; ++i) prim.indices[i] = (uint32_t)i;
      }
      if (!hasN) {   // flat normals from triangles
        for (size_t i = 0; i + 2 < prim.indices.size(); i += 3) {
          Vertex &a = prim.vertices[prim.indices[i]], &b = prim.vertices[prim.indices[i + 1]], &c = prim.vertices[prim.indices[i + 2]];
          vec3 n = normalize(cross(b.pos - a.pos, c.pos - a.pos));
          a.normal += n; b.normal += n; c.normal += n;
        }
        for (Vertex& v : prim.vertices) v.normal = normalize(v.normal);
      }
      mesh.primitives.push_back(std::move(prim));
    }
    out.meshes.push_back(std::move(mesh));
  }

  const Json& nodes = doc["nodes"];
  out.nodes.resize(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Json& n = nodes[i]; Node& node = out.nodes[i];
    node.name = n["name"].stringOr(""); node.mesh = n["mesh"].intOr(-1); nodeTransform(n, node);
    for (const auto& [k, v] : n["extras"].obj) {   // scalars only; arrays/objects are dropped
      if (v.isNumber()) { char buf[32]; std::snprintf(buf, sizeof buf, "%.9g", v.num); node.extras[k] = buf; }
      else if (v.isString()) node.extras[k] = v.str;
      else if (v.type == Json::Type::Bool) node.extras[k] = v.b ? "true" : "false";
    }
    for (const Json& c : n["children"].arr) { int ci = c.intOr(-1); if (ci >= 0 && ci < (int)nodes.size()) { node.children.push_back(ci); out.nodes[(size_t)ci].parent = (int)i; } }
  }
  int scene = doc["scene"].intOr(0);
  const Json& sc = doc["scenes"][(size_t)scene];
  if (sc.isNull()) { for (size_t i = 0; i < out.nodes.size(); ++i) if (out.nodes[i].parent < 0) out.roots.push_back((int)i); }
  else for (const Json& r : sc["nodes"].arr) out.roots.push_back(r.intOr(0));

  // animations: node TRS channels only (no morph weights); tangents of CUBICSPLINE samplers dropped
  for (const Json& an : doc["animations"].arr) {
    Animation a; a.name = an["name"].stringOr("");
    for (const Json& sm : an["samplers"].arr) {
      AnimSampler s; Accessor in, val;
      if (!ctx.accessor(sm["input"].intOr(0), in) || !ctx.accessor(sm["output"].intOr(0), val)) return false;
      std::string ip = sm["interpolation"].stringOr("LINEAR");
      bool cubic = ip == "CUBICSPLINE"; s.step = ip == "STEP";
      s.comps = (uint8_t)val.comps;
      s.times.resize(in.count);
      for (size_t i = 0; i < in.count; ++i) { s.times[i] = in.readFloat(i, 0); a.duration = std::fmax(a.duration, s.times[i]); }
      s.values.resize(in.count * s.comps);
      for (size_t i = 0; i < in.count; ++i) {
        size_t src = cubic ? i * 3 + 1 : i;   // cubic: [inTangent, value, outTangent] per key
        if (src >= val.count) break;
        for (int c = 0; c < s.comps; ++c) s.values[i * s.comps + (size_t)c] = val.readFloat(src, c);
      }
      a.samplers.push_back(std::move(s));
    }
    for (const Json& ch : an["channels"].arr) {
      AnimChannel c; c.sampler = ch["sampler"].intOr(-1); c.node = ch["target"]["node"].intOr(-1);
      std::string path = ch["target"]["path"].stringOr("");
      if (path == "translation") c.path = AnimPath::Translation; else if (path == "rotation") c.path = AnimPath::Rotation;
      else if (path == "scale") c.path = AnimPath::Scale; else continue;   // weights: unsupported
      if (c.sampler < 0 || c.sampler >= (int)a.samplers.size() || c.node < 0 || c.node >= (int)out.nodes.size()) continue;
      a.channels.push_back(c);
    }
    out.animations.push_back(std::move(a));
  }

  out.computeBounds();
  return true;
}

void restPose(const std::vector<Node>& nodes, std::vector<mat4>& local) {
  local.resize(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) local[i] = nodes[i].local;
}

void scrubAnimation(const std::vector<Node>& nodes, const Animation& a, float t, std::vector<mat4>& local) {
  if (local.size() != nodes.size()) restPose(nodes, local);
  // per-node TRS, only for the targeted nodes (a channel replaces one component)
  struct Pose { vec3 t; quat r; vec3 s; bool used = false; };
  std::vector<Pose> pose;
  for (const AnimChannel& ch : a.channels) {
    const AnimSampler& s = a.samplers[(size_t)ch.sampler];
    if (s.times.empty()) continue;
    if (pose.empty()) pose.resize(nodes.size());
    Pose& p = pose[(size_t)ch.node];
    if (!p.used) { const Node& n = nodes[(size_t)ch.node]; p = {n.translation, n.rotation, n.scale, true}; }
    // keyframe pair around t (clamped)
    size_t n = s.times.size(), k = 0;
    while (k + 1 < n && s.times[k + 1] <= t) ++k;
    size_t k1 = std::min(k + 1, n - 1);
    float f = 0;
    if (!s.step && k1 != k) { float dt = s.times[k1] - s.times[k]; f = dt > 1e-9f ? std::fmin(1.f, std::fmax(0.f, (t - s.times[k]) / dt)) : 0; }
    const float* v0 = &s.values[k * s.comps]; const float* v1 = &s.values[k1 * s.comps];
    if (ch.path == AnimPath::Rotation && s.comps == 4) {
      quat q0{v0[0], v0[1], v0[2], v0[3]}, q1{v1[0], v1[1], v1[2], v1[3]};
      float d = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
      if (d < 0) { q1 = {-q1.x, -q1.y, -q1.z, -q1.w}; d = -d; }
      float w0 = 1 - f, w1 = f;
      if (d < 0.9995f) { float th = std::acos(std::fmin(1.f, d)), sn = std::sin(th); w0 = std::sin((1 - f) * th) / sn; w1 = std::sin(f * th) / sn; }
      quat q{q0.x * w0 + q1.x * w1, q0.y * w0 + q1.y * w1, q0.z * w0 + q1.z * w1, q0.w * w0 + q1.w * w1};
      float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); if (l > 1e-9f) { q.x /= l; q.y /= l; q.z /= l; q.w /= l; }
      p.r = q;
    } else if (s.comps >= 3) {
      vec3 v{v0[0] + (v1[0] - v0[0]) * f, v0[1] + (v1[1] - v0[1]) * f, v0[2] + (v1[2] - v0[2]) * f};
      if (ch.path == AnimPath::Translation) p.t = v; else if (ch.path == AnimPath::Scale) p.s = v;
    }
  }
  for (size_t i = 0; i < pose.size(); ++i) if (pose[i].used) local[i] = trs(pose[i].t, pose[i].r, pose[i].s);
}

void computeWorld(const std::vector<Node>& nodes, const std::vector<int>& roots, const std::vector<mat4>& local, std::vector<mat4>& world) {
  world.assign(nodes.size(), mat4::identity());
  auto visit = [&](auto&& self, int n, const mat4& parent) -> void {
    world[(size_t)n] = parent * local[(size_t)n];
    for (int c : nodes[(size_t)n].children) self(self, c, world[(size_t)n]);
  };
  for (int r : roots) visit(visit, r, mat4::identity());
}

bool loadGlbFile(const std::string& path, Model& out, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open " + path; return false; }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  return loadGlb(bytes, out, err);
}

} // namespace eng
