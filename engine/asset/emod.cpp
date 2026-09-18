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
  // A u32 element count that the rest of the file has to be able to hold: `elem` is the smallest number
  // of bytes one element costs on the wire. A corrupt or hostile header then fails here instead of
  // asking the allocator for gigabytes (the file is already fully in memory, so this is exact, not a guess).
  size_t count(size_t elem) {
    uint32_t n = get<uint32_t>();
    if (!ok) return 0;
    size_t left = buf.size() - pos;
    if (elem && (size_t)n > left / elem) { ok = false; pos = buf.size(); return 0; }
    return n;
  }
};

static size_t left(const Reader& r) { return r.buf.size() - r.pos; }   // bytes still unread

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
  if (ver < 5) {
    size_t need = (size_t)im.width * im.height * im.channels;
    if (need > left(r)) return false;
    im.pixels.resize(need); r.bytes(im.pixels.data(), im.pixels.size()); return r.ok;
  }
  im.variants.resize(r.get<uint8_t>());
  for (ImageVariant& v : im.variants) {
    v.format = (TexFormat)r.get<uint8_t>(); v.mips.resize(r.get<uint8_t>());
    if ((uint8_t)v.format > (uint8_t)TexFormat::BC7) { err = "unknown texture format"; return false; }
    for (MipLevel& l : v.mips) {
      l.width = r.get<uint16_t>(); l.height = r.get<uint16_t>(); uint32_t n = r.get<uint32_t>();
      if (!r.ok || n != texLevelBytes(v.format, l.width, l.height)) { err = "bad mip level"; return false; }
      if (n > left(r)) return false;
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
      w.put((uint32_t)p.skin.size()); w.bytes(p.skin.data(), p.skin.size() * sizeof(VertexSkin));   // v8
    }
  }
  w.put((uint32_t)m.nodes.size());
  for (const Node& n : m.nodes) {
    w.str(n.name); w.put((int32_t)n.mesh); w.put((int32_t)n.parent);
    w.put((uint32_t)n.children.size()); for (int c : n.children) w.put((int32_t)c);
    w.put(n.local);
    w.put((uint16_t)n.extras.size()); for (const auto& [k, v] : n.extras) { w.str(k); w.str(v); }
    w.put(n.translation); w.put(n.rotation); w.put(n.scale);   // v7
    w.put((int32_t)n.skin);   // v8
  }
  w.put((uint32_t)m.roots.size()); for (int r : m.roots) w.put((int32_t)r);
  w.put(m.boundsMin); w.put(m.boundsMax);
  w.put((uint32_t)m.animations.size());   // v7
  for (const Animation& a : m.animations) {
    w.str(a.name); w.put(a.duration);
    w.put((uint32_t)a.samplers.size());
    for (const AnimSampler& sm : a.samplers) {
      w.put(sm.comps); w.put((uint8_t)(sm.step ? 1 : 0)); w.put((uint32_t)sm.times.size());
      w.bytes(sm.times.data(), sm.times.size() * 4); w.bytes(sm.values.data(), sm.values.size() * 4);
    }
    w.put((uint32_t)a.channels.size());
    for (const AnimChannel& c : a.channels) { w.put((int32_t)c.sampler); w.put((int32_t)c.node); w.put((uint8_t)c.path); }
  }
  w.put((uint32_t)m.skins.size());   // v8
  for (const Skin& sk : m.skins) {
    w.str(sk.name); w.put((int32_t)sk.skeleton);
    w.put((uint32_t)sk.joints.size()); for (int j : sk.joints) w.put((int32_t)j);
    w.put((uint32_t)sk.inverseBind.size()); for (const mat4& ib : sk.inverseBind) w.put(ib);
  }
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
  m.images.resize(r.count(10));
  for (Image& im : m.images) if (!readImage(r, im, ver, err)) { if (err.empty()) err = "truncated EMOD"; return false; }
  m.materials.resize(r.count(40));
  for (Material& mt : m.materials) {
    mt.name = r.str(); mt.baseColor = r.get<vec4>(); mt.metallic = r.get<float>(); mt.roughness = r.get<float>(); mt.emissive = r.get<vec3>();
    mt.baseColorTex = r.get<int32_t>(); mt.metalRoughTex = r.get<int32_t>(); mt.normalTex = r.get<int32_t>();
    mt.emissiveTex = r.get<int32_t>(); mt.occlusionTex = r.get<int32_t>();
    mt.alphaMode = (AlphaMode)r.get<uint8_t>(); mt.alphaCutoff = r.get<float>(); mt.doubleSided = r.get<uint8_t>() != 0;
    if (ver >= 3) mt.unlit = r.get<uint8_t>() != 0;
  }
  m.meshes.resize(r.count(6));
  for (Mesh& me : m.meshes) {
    me.name = r.str(); me.primitives.resize(r.count(36));
    for (Primitive& p : me.primitives) {
      p.material = r.get<int32_t>();
      p.vertices.resize(r.count(sizeof(Vertex))); r.bytes(p.vertices.data(), p.vertices.size() * sizeof(Vertex));
      p.indices.resize(r.count(4)); r.bytes(p.indices.data(), p.indices.size() * 4);
      p.boundsMin = r.get<vec3>(); p.boundsMax = r.get<vec3>();
      if (ver >= 8) {
        uint32_t ns = r.get<uint32_t>();
        if (!r.ok || (ns && ns != p.vertices.size())) { err = "skin vertex count mismatch"; return false; }
        if (ns > left(r) / sizeof(VertexSkin)) { err = "truncated EMOD"; return false; }
        p.skin.resize(ns); r.bytes(p.skin.data(), p.skin.size() * sizeof(VertexSkin));
      }
      if (!r.ok) break;
    }
    if (!r.ok) break;
  }
  m.nodes.resize(r.count(2 + 4 + 4 + 4 + 64));
  for (Node& n : m.nodes) {
    n.name = r.str(); n.mesh = r.get<int32_t>(); n.parent = r.get<int32_t>();
    n.children.resize(r.count(4)); for (int& c : n.children) c = r.get<int32_t>();
    n.local = r.get<mat4>();
    if (ver >= 2) { uint16_t ne = r.get<uint16_t>(); for (uint16_t i = 0; i < ne && r.ok; ++i) { std::string k = r.str(); n.extras[k] = r.str(); } }
    if (ver >= 7) { n.translation = r.get<vec3>(); n.rotation = r.get<quat>(); n.scale = r.get<vec3>(); }
    else { n.translation = {n.local.m[3][0], n.local.m[3][1], n.local.m[3][2]}; }   // pre-v7: no clips, TRS unused
    if (ver >= 8) n.skin = r.get<int32_t>();
    if (!r.ok) break;
  }
  m.roots.resize(r.count(4)); for (int& x : m.roots) x = r.get<int32_t>();
  m.boundsMin = r.get<vec3>(); m.boundsMax = r.get<vec3>();
  if (ver >= 7) {
    m.animations.resize(r.count(2 + 4 + 4 + 4));
    for (Animation& a : m.animations) {
      a.name = r.str(); a.duration = r.get<float>();
      a.samplers.resize(r.count(1 + 1 + 4));
      for (AnimSampler& sm : a.samplers) {
        sm.comps = r.get<uint8_t>(); sm.step = r.get<uint8_t>() != 0; uint32_t n = r.get<uint32_t>();
        if (!r.ok || sm.comps == 0 || sm.comps > 4 || n > (1u << 24)) { err = "bad animation sampler"; return false; }
        if ((size_t)n * 4 > left(r)) { err = "bad animation sampler"; return false; }
        sm.times.resize(n); r.bytes(sm.times.data(), n * 4);
        if ((size_t)n * sm.comps * 4 > left(r)) { err = "bad animation sampler"; return false; }
        sm.values.resize((size_t)n * sm.comps); r.bytes(sm.values.data(), sm.values.size() * 4);
      }
      a.channels.resize(r.count(4 + 4 + 1));
      for (AnimChannel& c : a.channels) { c.sampler = r.get<int32_t>(); c.node = r.get<int32_t>(); c.path = (AnimPath)r.get<uint8_t>(); }
      if (!r.ok) break;
    }
  }
  if (ver >= 8) {
    m.skins.resize(r.count(2 + 4 + 4 + 4));
    for (Skin& sk : m.skins) {
      sk.name = r.str(); sk.skeleton = r.get<int32_t>();
      uint32_t nj = r.get<uint32_t>();
      if (!r.ok || nj > 255) { err = "bad skin"; return false; }
      sk.joints.resize(nj); for (int& j : sk.joints) j = r.get<int32_t>();
      uint32_t ni = r.get<uint32_t>();
      if (!r.ok || ni > nj) { err = "bad skin"; return false; }
      sk.inverseBind.resize(ni); for (mat4& ib : sk.inverseBind) ib = r.get<mat4>();
      if (!r.ok) break;
    }
  }
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
