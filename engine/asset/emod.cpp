#include "engine/asset/emod.h"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace eng {

namespace {
struct Writer {
  std::ofstream f;
  template <class T> void put(const T& v) { f.write((const char*)&v, sizeof v); }
  void str(const std::string& s) { put((uint16_t)s.size()); f.write(s.data(), (std::streamsize)s.size()); }
  void bytes(const void* p, size_t n) { f.write((const char*)p, (std::streamsize)n); }
};
struct Reader {
  std::ifstream f; bool ok = true;
  template <class T> T get() { T v{}; if (!f.read((char*)&v, sizeof v)) ok = false; return v; }
  std::string str() { uint16_t n = get<uint16_t>(); std::string s(n, 0); if (n && !f.read(s.data(), n)) ok = false; return s; }
  void bytes(void* p, size_t n) { if (n && !f.read((char*)p, (std::streamsize)n)) ok = false; }
};
}

bool saveEmod(const Model& m, const std::string& path, std::string& err) {
  Writer w{std::ofstream(path, std::ios::binary)};
  if (!w.f) { err = "cannot write " + path; return false; }
  w.bytes("EMOD", 4); w.put(EMOD_VERSION);
  w.put((uint32_t)m.images.size());
  for (const Image& im : m.images) {
    if (im.pixels.empty() || im.channels != 4) { err = "image not decoded to RGBA8"; return false; }
    w.put((uint16_t)im.width); w.put((uint16_t)im.height); w.put((uint8_t)4);
    w.put(im.wrapS); w.put(im.wrapT); w.put((uint8_t)(im.linear ? 1 : 0));
    w.bytes(im.pixels.data(), im.pixels.size());
  }
  w.put((uint32_t)m.materials.size());
  for (const Material& mt : m.materials) {
    w.str(mt.name); w.put(mt.baseColor); w.put(mt.metallic); w.put(mt.roughness); w.put(mt.emissive);
    w.put((int32_t)mt.baseColorTex); w.put((int32_t)mt.metalRoughTex); w.put((int32_t)mt.normalTex);
    w.put((int32_t)mt.emissiveTex); w.put((int32_t)mt.occlusionTex);
    w.put((uint8_t)mt.alphaMode); w.put(mt.alphaCutoff); w.put((uint8_t)mt.doubleSided); w.put((uint8_t)mt.unlit);
  }
  w.put((uint32_t)m.meshes.size());
  for (const Mesh& me : m.meshes) {
    w.str(me.name); w.put((uint32_t)me.primitives.size());
    for (const Primitive& p : me.primitives) {
      w.put((int32_t)p.material);
      w.put((uint32_t)p.vertices.size()); w.bytes(p.vertices.data(), p.vertices.size() * sizeof(Vertex));
      w.put((uint32_t)p.indices.size()); w.bytes(p.indices.data(), p.indices.size() * 4);
      w.put(p.boundsMin); w.put(p.boundsMax);
    }
  }
  w.put((uint32_t)m.nodes.size());
  for (const Node& n : m.nodes) {
    w.str(n.name); w.put((int32_t)n.mesh); w.put((int32_t)n.parent);
    w.put((uint32_t)n.children.size()); for (int c : n.children) w.put((int32_t)c);
    w.put(n.local);
    w.put((uint16_t)n.extras.size()); for (const auto& [k, v] : n.extras) { w.str(k); w.str(v); }
  }
  w.put((uint32_t)m.roots.size()); for (int r : m.roots) w.put((int32_t)r);
  w.put(m.boundsMin); w.put(m.boundsMax);
  return (bool)w.f;
}

bool loadEmod(const std::string& path, Model& m, std::string& err) {
  Reader r{std::ifstream(path, std::ios::binary)};
  if (!r.f) { err = "cannot open " + path; return false; }
  char magic[4]; r.bytes(magic, 4);
  if (std::memcmp(magic, "EMOD", 4) != 0) { err = "not an EMOD file"; return false; }
  uint32_t ver = r.get<uint32_t>();
  if (ver < 1 || ver > EMOD_VERSION) { err = "EMOD version " + std::to_string(ver) + " unsupported"; return false; }
  m = {};
  m.images.resize(r.get<uint32_t>());
  for (Image& im : m.images) {
    im.width = r.get<uint16_t>(); im.height = r.get<uint16_t>(); im.channels = r.get<uint8_t>();
    im.wrapS = r.get<uint8_t>(); im.wrapT = r.get<uint8_t>(); im.linear = r.get<uint8_t>() != 0;   // zero (= repeat, sRGB) before v4
    im.pixels.resize((size_t)im.width * im.height * im.channels); r.bytes(im.pixels.data(), im.pixels.size());
  }
  m.materials.resize(r.get<uint32_t>());
  for (Material& mt : m.materials) {
    mt.name = r.str(); mt.baseColor = r.get<vec4>(); mt.metallic = r.get<float>(); mt.roughness = r.get<float>(); mt.emissive = r.get<vec3>();
    mt.baseColorTex = r.get<int32_t>(); mt.metalRoughTex = r.get<int32_t>(); mt.normalTex = r.get<int32_t>();
    mt.emissiveTex = r.get<int32_t>(); mt.occlusionTex = r.get<int32_t>();
    mt.alphaMode = (AlphaMode)r.get<uint8_t>(); mt.alphaCutoff = r.get<float>(); mt.doubleSided = r.get<uint8_t>() != 0;
    if (ver >= 3) mt.unlit = r.get<uint8_t>() != 0;
  }
  m.meshes.resize(r.get<uint32_t>());
  for (Mesh& me : m.meshes) {
    me.name = r.str(); me.primitives.resize(r.get<uint32_t>());
    for (Primitive& p : me.primitives) {
      p.material = r.get<int32_t>();
      p.vertices.resize(r.get<uint32_t>()); r.bytes(p.vertices.data(), p.vertices.size() * sizeof(Vertex));
      p.indices.resize(r.get<uint32_t>()); r.bytes(p.indices.data(), p.indices.size() * 4);
      p.boundsMin = r.get<vec3>(); p.boundsMax = r.get<vec3>();
    }
  }
  m.nodes.resize(r.get<uint32_t>());
  for (Node& n : m.nodes) {
    n.name = r.str(); n.mesh = r.get<int32_t>(); n.parent = r.get<int32_t>();
    n.children.resize(r.get<uint32_t>()); for (int& c : n.children) c = r.get<int32_t>();
    n.local = r.get<mat4>();
    if (ver >= 2) { uint16_t ne = r.get<uint16_t>(); for (uint16_t i = 0; i < ne && r.ok; ++i) { std::string k = r.str(); n.extras[k] = r.str(); } }
  }
  m.roots.resize(r.get<uint32_t>()); for (int& x : m.roots) x = r.get<int32_t>();
  m.boundsMin = r.get<vec3>(); m.boundsMax = r.get<vec3>();
  if (!r.ok) { err = "truncated EMOD"; return false; }
  return true;
}

} // namespace eng
