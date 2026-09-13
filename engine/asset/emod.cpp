#include "engine/asset/emod.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace eng {

namespace {
struct Writer {
  std::ofstream f;
  template <class T> void put(const T& v) { f.write((const char*)&v, sizeof v); }
  void str(const std::string& s) { put((uint16_t)s.size()); f.write(s.data(), (std::streamsize)s.size()); }
  void bytes(const void* p, size_t n) { f.write((const char*)p, (std::streamsize)n); }
};
struct Reader {   // over a byte span (files are read whole; the web build gets the bytes from fetch)
  std::span<const uint8_t> buf; size_t pos = 0; bool ok = true;
  template <class T> T get() { T v{}; if (pos + sizeof v > buf.size()) { ok = false; pos = buf.size(); return v; } std::memcpy(&v, buf.data() + pos, sizeof v); pos += sizeof v; return v; }
  std::string str() { uint16_t n = get<uint16_t>(); std::string s(n, 0); bytes(s.data(), n); return s; }
  void bytes(void* p, size_t n) { if (pos + n > buf.size()) { ok = false; pos = buf.size(); return; } if (n) std::memcpy(p, buf.data() + pos, n); pos += n; }
};

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate); if (!f) return false;
  std::streamsize n = f.tellg(); f.seekg(0); out.resize((size_t)n);
  return n == 0 || (bool)f.read((char*)out.data(), n);
}

bool writeImage(Writer& w, const Image& im, std::string& err) {
  bool raw = im.variants.empty(), placeholder = im.placeholder();
  if (raw && !placeholder && im.channels != 4) { err = "image not decoded to RGBA8"; return false; }
  w.put((uint16_t)im.width); w.put((uint16_t)im.height); w.put((uint8_t)4);
  w.put(im.wrapS); w.put(im.wrapT); w.put((uint8_t)(im.linear ? 1 : 0)); w.put((int16_t)im.source);
  if (placeholder) { w.put((uint8_t)0); return true; }
  if (raw) {   // a raw image becomes a one-variant, one-mip RGBA8 record
    w.put((uint8_t)1); w.put((uint8_t)TexFormat::RGBA8); w.put((uint8_t)1);
    w.put((uint16_t)im.width); w.put((uint16_t)im.height); w.put((uint32_t)im.pixels.size()); w.bytes(im.pixels.data(), im.pixels.size());
    return true;
  }
  w.put((uint8_t)im.variants.size());
  for (const ImageVariant& v : im.variants) {
    w.put((uint8_t)v.format); w.put((uint8_t)v.mips.size());
    for (const MipLevel& l : v.mips) {
      if (l.data.size() != texLevelBytes(v.format, l.width, l.height)) { err = "mip level size mismatch"; return false; }
      w.put((uint16_t)l.width); w.put((uint16_t)l.height); w.put((uint32_t)l.data.size()); w.bytes(l.data.data(), l.data.size());
    }
  }
  return true;
}

bool readImage(Reader& r, Image& im, uint32_t ver, std::string& err) {
  im.width = r.get<uint16_t>(); im.height = r.get<uint16_t>(); im.channels = r.get<uint8_t>();
  im.wrapS = r.get<uint8_t>(); im.wrapT = r.get<uint8_t>(); im.linear = r.get<uint8_t>() != 0;   // zero (= repeat, sRGB) before v4
  if (ver >= 6) im.source = r.get<int16_t>();
  if (ver < 5) { im.pixels.resize((size_t)im.width * im.height * im.channels); r.bytes(im.pixels.data(), im.pixels.size()); return r.ok; }
  im.variants.resize(r.get<uint8_t>());
  for (ImageVariant& v : im.variants) {
    v.format = (TexFormat)r.get<uint8_t>(); v.mips.resize(r.get<uint8_t>());
    if ((uint8_t)v.format > (uint8_t)TexFormat::BC7) { err = "unknown texture format"; return false; }
    for (MipLevel& l : v.mips) {
      l.width = r.get<uint16_t>(); l.height = r.get<uint16_t>(); uint32_t n = r.get<uint32_t>();
      if (!r.ok || n != texLevelBytes(v.format, l.width, l.height)) { err = "bad mip level"; return false; }
      l.data.resize(n); r.bytes(l.data.data(), n);
    }
  }
  // a plain single RGBA8 variant is also exposed as `pixels` so CPU users (tests, procedural code) keep working
  if (im.variants.size() == 1 && im.variants[0].format == TexFormat::RGBA8 && im.variants[0].mips.size() == 1)
    im.pixels = im.variants[0].mips[0].data;
  return r.ok;
}
} // namespace

bool saveEmod(const Model& m, const std::string& path, std::string& err) {
  Writer w{std::ofstream(path, std::ios::binary)};
  if (!w.f) { err = "cannot write " + path; return false; }
  w.bytes("EMOD", 4); w.put(EMOD_VERSION);
  w.put((uint32_t)m.images.size());
  for (const Image& im : m.images) if (!writeImage(w, im, err)) return false;
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
  std::vector<uint8_t> bytes;
  if (!readFile(path, bytes)) { err = "cannot open " + path; return false; }
  return loadEmod(bytes, m, err);
}

bool loadEmod(std::span<const uint8_t> bytes, Model& m, std::string& err) {
  Reader r{bytes};
  char magic[4] = {}; r.bytes(magic, 4);
  if (!r.ok || std::memcmp(magic, "EMOD", 4) != 0) { err = "not an EMOD file"; return false; }
  uint32_t ver = r.get<uint32_t>();
  if (ver < 1 || ver > EMOD_VERSION) { err = "EMOD version " + std::to_string(ver) + " unsupported"; return false; }
  m = {};
  m.images.resize(r.get<uint32_t>());
  for (Image& im : m.images) if (!readImage(r, im, ver, err)) { if (err.empty()) err = "truncated EMOD"; return false; }
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
      if (!r.ok) break;
    }
    if (!r.ok) break;
  }
  m.nodes.resize(r.get<uint32_t>());
  for (Node& n : m.nodes) {
    n.name = r.str(); n.mesh = r.get<int32_t>(); n.parent = r.get<int32_t>();
    n.children.resize(r.get<uint32_t>()); for (int& c : n.children) c = r.get<int32_t>();
    n.local = r.get<mat4>();
    if (ver >= 2) { uint16_t ne = r.get<uint16_t>(); for (uint16_t i = 0; i < ne && r.ok; ++i) { std::string k = r.str(); n.extras[k] = r.str(); } }
    if (!r.ok) break;
  }
  m.roots.resize(r.get<uint32_t>()); for (int& x : m.roots) x = r.get<int32_t>();
  m.boundsMin = r.get<vec3>(); m.boundsMax = r.get<vec3>();
  if (!r.ok) { err = "truncated EMOD"; return false; }
  return true;
}

bool saveImageFile(const Image& im, const std::string& path, std::string& err) {
  Writer w{std::ofstream(path, std::ios::binary)};
  if (!w.f) { err = "cannot write " + path; return false; }
  w.bytes("EIMG", 4); w.put((uint32_t)2);
  return writeImage(w, im, err) && (bool)w.f;
}

bool loadImageFile(std::span<const uint8_t> bytes, Image& im, std::string& err) {
  Reader r{bytes};
  char magic[4] = {}; r.bytes(magic, 4);
  if (!r.ok || std::memcmp(magic, "EIMG", 4) != 0) { err = "not an EIMG file"; return false; }
  uint32_t ver = r.get<uint32_t>();   // 1 = v5 image record, 2 = v6 (source index)
  if (ver < 1 || ver > 2) { err = "EIMG version unsupported"; return false; }
  im = {};
  if (!readImage(r, im, ver == 1 ? 5 : 6, err)) { if (err.empty()) err = "truncated EIMG"; return false; }
  return true;
}

} // namespace eng
