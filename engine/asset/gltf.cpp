#include "engine/asset/gltf.h"
#include "engine/core/json.h"
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

mat4 nodeMatrix(const Json& n) {
  if (n.has("matrix")) {
    mat4 m; const Json& a = n["matrix"];
    for (int i = 0; i < 16; ++i) (&m.m[0][0])[i] = (float)a[(size_t)i].numberOr(i % 5 == 0 ? 1 : 0);
    return m;
  }
  vec3 t{0, 0, 0}, s{1, 1, 1}; quat r;
  if (n.has("translation")) t = {(float)n["translation"][0].num, (float)n["translation"][1].num, (float)n["translation"][2].num};
  if (n.has("scale")) s = {(float)n["scale"][0].num, (float)n["scale"][1].num, (float)n["scale"][2].num};
  if (n.has("rotation")) r = {(float)n["rotation"][0].num, (float)n["rotation"][1].num, (float)n["rotation"][2].num, (float)n["rotation"][3].num};
  return trs(t, r, s);
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
  if (doc["extensionsRequired"].size()) { err = "required extension: " + doc["extensionsRequired"][0].stringOr("?"); return false; }
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
  // textures: map texture index -> image index
  std::vector<int> texToImage;
  for (const Json& t : doc["textures"].arr) texToImage.push_back(t["source"].intOr(-1));
  auto tex = [&](const Json& info) { int i = info["index"].intOr(-1); return i >= 0 && i < (int)texToImage.size() ? texToImage[(size_t)i] : -1; };

  for (const Json& m : doc["materials"].arr) {
    Material mat; mat.name = m["name"].stringOr("");
    const Json& pbr = m["pbrMetallicRoughness"];
    if (pbr.has("baseColorFactor")) mat.baseColor = {(float)pbr["baseColorFactor"][0].num, (float)pbr["baseColorFactor"][1].num, (float)pbr["baseColorFactor"][2].num, (float)pbr["baseColorFactor"][3].num};
    mat.metallic = (float)pbr["metallicFactor"].numberOr(1);
    mat.roughness = (float)pbr["roughnessFactor"].numberOr(1);
    if (pbr.has("baseColorTexture")) mat.baseColorTex = tex(pbr["baseColorTexture"]);
    if (pbr.has("metallicRoughnessTexture")) mat.metalRoughTex = tex(pbr["metallicRoughnessTexture"]);
    if (m.has("normalTexture")) mat.normalTex = tex(m["normalTexture"]);
    if (m.has("emissiveTexture")) mat.emissiveTex = tex(m["emissiveTexture"]);
    if (m.has("occlusionTexture")) mat.occlusionTex = tex(m["occlusionTexture"]);
    if (m.has("emissiveFactor")) mat.emissive = {(float)m["emissiveFactor"][0].num, (float)m["emissiveFactor"][1].num, (float)m["emissiveFactor"][2].num};
    std::string am = m["alphaMode"].stringOr("OPAQUE");
    mat.alphaMode = am == "BLEND" ? AlphaMode::Blend : am == "MASK" ? AlphaMode::Mask : AlphaMode::Opaque;
    mat.alphaCutoff = (float)m["alphaCutoff"].numberOr(0.5);
    mat.doubleSided = m["doubleSided"].boolOr(false);
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
      Accessor pos, nrm, uv; bool hasN = at.has("NORMAL"), hasUV = at.has("TEXCOORD_0");
      if (!ctx.accessor(at["POSITION"].intOr(0), pos)) return false;
      if (hasN && !ctx.accessor(at["NORMAL"].intOr(0), nrm)) return false;
      if (hasUV && !ctx.accessor(at["TEXCOORD_0"].intOr(0), uv)) return false;
      prim.vertices.resize(pos.count);
      prim.boundsMin = {1e30f, 1e30f, 1e30f}; prim.boundsMax = -prim.boundsMin;
      for (size_t i = 0; i < pos.count; ++i) {
        Vertex& v = prim.vertices[i];
        v.pos = {pos.readFloat(i, 0), pos.readFloat(i, 1), pos.readFloat(i, 2)};
        if (hasN) v.normal = {nrm.readFloat(i, 0), nrm.readFloat(i, 1), nrm.readFloat(i, 2)};
        if (hasUV) v.uv = {uv.readFloat(i, 0), uv.readFloat(i, 1)};
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
    node.name = n["name"].stringOr(""); node.mesh = n["mesh"].intOr(-1); node.local = nodeMatrix(n);
    for (const Json& c : n["children"].arr) { int ci = c.intOr(-1); if (ci >= 0 && ci < (int)nodes.size()) { node.children.push_back(ci); out.nodes[(size_t)ci].parent = (int)i; } }
  }
  int scene = doc["scene"].intOr(0);
  const Json& sc = doc["scenes"][(size_t)scene];
  if (sc.isNull()) { for (size_t i = 0; i < out.nodes.size(); ++i) if (out.nodes[i].parent < 0) out.roots.push_back((int)i); }
  else for (const Json& r : sc["nodes"].arr) out.roots.push_back(r.intOr(0));

  out.computeBounds();
  return true;
}

bool loadGlbFile(const std::string& path, Model& out, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { err = "cannot open " + path; return false; }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  return loadGlb(bytes, out, err);
}

} // namespace eng
