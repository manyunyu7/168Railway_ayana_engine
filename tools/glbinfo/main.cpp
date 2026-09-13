// Audit tool: prints the glTF features a GLB uses that the engine's parser/renderer might
// ignore (extensions, UV sets, texture transforms, sampler wrap, vertex colours, alpha modes,
// mirrored nodes, image formats). Own JSON parser only; no third-party code.
//   glbinfo a.glb [b.glb ...]      (add --all to list every material/node, not just the odd ones)
#include "engine/core/json.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace eng;

namespace {

bool readGlb(const char* path, std::string& json, std::vector<uint8_t>& bin) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  if (bytes.size() < 20 || std::memcmp(bytes.data(), "glTF", 4) != 0) return false;
  uint32_t total; std::memcpy(&total, bytes.data() + 8, 4);
  if (total > bytes.size()) return false;
  for (size_t p = 12; p + 8 <= total;) {
    uint32_t len, type; std::memcpy(&len, &bytes[p], 4); std::memcpy(&type, &bytes[p + 4], 4);
    if (p + 8 + len > total) return false;
    if (type == 0x4E4F534A) json.assign((const char*)&bytes[p + 8], len);
    else if (type == 0x004E4942) bin.assign(bytes.begin() + (long)(p + 8), bytes.begin() + (long)(p + 8 + len));
    p += 8 + len;
  }
  return !json.empty();
}

const char* wrapName(int w) { return w == 33071 ? "CLAMP" : w == 33648 ? "MIRROR" : w == 10497 ? "REPEAT" : "?"; }

// PNG bit depth / colour type from the IHDR chunk; JPEG is always 8-bit.
std::string imageInfo(const Json& doc, const std::vector<uint8_t>& bin, int image) {
  const Json& im = doc["images"][(size_t)image];
  if (!im.has("bufferView")) return "uri!";
  const Json& bv = doc["bufferViews"][(size_t)im["bufferView"].intOr(0)];
  size_t off = (size_t)bv["byteOffset"].numberOr(0), len = (size_t)bv["byteLength"].numberOr(0);
  if (off + len > bin.size() || len < 29) return "bad";
  const uint8_t* p = bin.data() + off;
  char buf[96];
  if (std::memcmp(p, "\x89PNG", 4) == 0) {
    uint32_t w = (uint32_t)p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19], h = (uint32_t)p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23];
    std::snprintf(buf, sizeof buf, "png %ux%u %dbit ct%d", w, h, p[24], p[25]);
  } else if (p[0] == 0xFF && p[1] == 0xD8) std::snprintf(buf, sizeof buf, "jpeg");
  else if (std::memcmp(p, "RIFF", 4) == 0) std::snprintf(buf, sizeof buf, "webp!");
  else if (std::memcmp(p, "\xABKTX", 4) == 0) std::snprintf(buf, sizeof buf, "ktx2!");
  else std::snprintf(buf, sizeof buf, "%s", im["mimeType"].stringOr("?").c_str());
  return buf;
}

struct TexRef { int tex = -1, image = -1, uv = 0, wrapS = 10497, wrapT = 10497; bool transform = false; std::string transformStr; };

TexRef texRef(const Json& doc, const Json& info) {
  TexRef r;
  if (info.isNull()) return r;
  r.tex = info["index"].intOr(-1); r.uv = info["texCoord"].intOr(0);
  const Json& t = doc["textures"][(size_t)r.tex];
  r.image = t["source"].intOr(-1);
  if (r.image < 0) {   // KHR_texture_basisu / EXT_texture_webp sources
    for (const auto& [k, v] : t["extensions"].obj) if (v.has("source")) r.image = v["source"].intOr(-1);
  }
  if (t.has("sampler")) {
    const Json& s = doc["samplers"][(size_t)t["sampler"].intOr(0)];
    r.wrapS = s["wrapS"].intOr(10497); r.wrapT = s["wrapT"].intOr(10497);
  }
  const Json& tt = info["extensions"]["KHR_texture_transform"];
  if (!tt.isNull()) {
    r.transform = true;
    char buf[160];
    std::snprintf(buf, sizeof buf, "offset(%g,%g) scale(%g,%g) rot %g uv%d", tt["offset"][0].numberOr(0), tt["offset"][1].numberOr(0),
                  tt["scale"][0].numberOr(1), tt["scale"][1].numberOr(1), tt["rotation"].numberOr(0), tt["texCoord"].intOr(r.uv));
    r.transformStr = buf;
  }
  return r;
}

} // namespace

int main(int argc, char** argv) {
  bool all = false;
  std::vector<const char*> files;
  for (int i = 1; i < argc; ++i) { if (!std::strcmp(argv[i], "--all")) all = true; else files.push_back(argv[i]); }
  if (files.empty()) { std::fprintf(stderr, "usage: glbinfo [--all] file.glb ...\n"); return 2; }
  int rc = 0;
  for (const char* path : files) {
    std::string json; std::vector<uint8_t> bin;
    if (!readGlb(path, json, bin)) { std::printf("== %s: not a GLB\n", path); rc = 1; continue; }
    std::string jerr; Json doc = Json::parse(json, &jerr);
    if (!jerr.empty()) { std::printf("== %s: JSON error %s\n", path, jerr.c_str()); rc = 1; continue; }
    std::printf("== %s (%s)\n", path, doc["asset"]["generator"].stringOr("?").c_str());
    std::string ext;
    for (const Json& e : doc["extensionsUsed"].arr) ext += e.stringOr("?") + " ";
    std::string req;
    for (const Json& e : doc["extensionsRequired"].arr) req += e.stringOr("?") + " ";
    if (!ext.empty()) std::printf("  extensionsUsed: %s\n", ext.c_str());
    if (!req.empty()) std::printf("  extensionsRequired: %s  <-- parser rejects\n", req.c_str());

    std::set<std::string> flags;
    // which materials are used by primitives that have COLOR_0 / several UV sets
    std::vector<std::string> matAttrs(doc["materials"].size());
    for (const Json& me : doc["meshes"].arr)
      for (const Json& pr : me["primitives"].arr) {
        int m = pr["material"].intOr(-1);
        std::string a;
        if (pr["attributes"].has("COLOR_0")) a += " COLOR_0";
        if (pr["attributes"].has("TEXCOORD_1")) a += " TEXCOORD_1";
        if (pr["attributes"].has("TEXCOORD_2")) a += " TEXCOORD_2";
        if (!pr["attributes"].has("NORMAL")) a += " noNormal";
        if (!pr["attributes"].has("TEXCOORD_0")) a += " noUV";
        if (pr["mode"].intOr(4) != 4) a += " mode" + std::to_string(pr["mode"].intOr(4));
        if (pr.has("targets")) a += " morph";
        if (m >= 0 && m < (int)matAttrs.size() && matAttrs[(size_t)m].find(a) == std::string::npos) matAttrs[(size_t)m] += a;
        if (m < 0) flags.insert("primitive without material");
      }

    std::printf("  images: %zu, textures: %zu, materials: %zu, meshes: %zu, nodes: %zu\n", doc["images"].size(), doc["textures"].size(),
                doc["materials"].size(), doc["meshes"].size(), doc["nodes"].size());
    for (size_t i = 0; i < doc["images"].size(); ++i) {
      std::string info = imageInfo(doc, bin, (int)i);
      if (all || info.find('!') != std::string::npos || info.find("16bit") != std::string::npos) std::printf("  image %zu: %s\n", i, info.c_str());
      if (info.find("16bit") != std::string::npos) flags.insert("16-bit PNG");
    }
    size_t mi = 0;
    for (const Json& m : doc["materials"].arr) {
      const Json& pbr = m["pbrMetallicRoughness"];
      TexRef base = texRef(doc, pbr["baseColorTexture"]), mr = texRef(doc, pbr["metallicRoughnessTexture"]),
             em = texRef(doc, m["emissiveTexture"]), nm = texRef(doc, m["normalTexture"]), oc = texRef(doc, m["occlusionTexture"]);
      std::string odd;
      auto texDesc = [&](const char* label, const TexRef& t) {
        if (t.tex < 0) return;
        char buf[64]; std::snprintf(buf, sizeof buf, " %s=img%d", label, t.image); odd += buf;
        if (t.uv != 0) { odd += " uv" + std::to_string(t.uv); flags.insert(std::string(label) + " texCoord>0"); }
        if (t.wrapS != 10497 || t.wrapT != 10497) { odd += std::string(" wrap ") + wrapName(t.wrapS) + "/" + wrapName(t.wrapT); flags.insert("sampler wrap != REPEAT"); }
        if (t.transform) { odd += " transform[" + t.transformStr + "]"; flags.insert("KHR_texture_transform"); }
      };
      texDesc("base", base); texDesc("mr", mr); texDesc("emis", em); texDesc("normal", nm); texDesc("occl", oc);
      std::string am = m["alphaMode"].stringOr("OPAQUE");
      if (am != "OPAQUE") { odd += " " + am; if (am == "MASK") odd += " cutoff " + std::to_string(m["alphaCutoff"].numberOr(0.5)); }
      if (m["doubleSided"].boolOr(false)) odd += " doubleSided";
      if (pbr.has("baseColorFactor") && pbr["baseColorFactor"][3].numberOr(1) < 1) { odd += " baseAlpha<1"; if (am == "OPAQUE") flags.insert("baseColor alpha<1 while OPAQUE"); }
      if (m.has("emissiveFactor")) { const Json& e = m["emissiveFactor"]; if (e[0].numberOr(0) > 0 || e[1].numberOr(0) > 0 || e[2].numberOr(0) > 0) odd += " emissive"; }
      for (const auto& [k, v] : m["extensions"].obj) {
        odd += " " + k;
        if (k == "KHR_materials_emissive_strength") { odd += "=" + std::to_string(v["emissiveStrength"].numberOr(1)); flags.insert(k); }
        else if (k != "KHR_materials_unlit") flags.insert(k);
      }
      if (mi < matAttrs.size() && !matAttrs[mi].empty()) { odd += " attrs:" + matAttrs[mi]; if (matAttrs[mi].find("COLOR_0") != std::string::npos) flags.insert("COLOR_0"); }
      if (all || !odd.empty()) std::printf("  mat %zu '%s':%s\n", mi, m["name"].stringOr("").c_str(), odd.c_str());
      ++mi;
    }
    size_t ni = 0;
    for (const Json& n : doc["nodes"].arr) {
      float det = 1; std::string odd;
      if (n.has("matrix")) {
        float m[16]; for (int i = 0; i < 16; ++i) m[i] = (float)n["matrix"][(size_t)i].numberOr(0);
        det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
        odd += " matrix";
      } else if (n.has("scale")) {
        float sx = (float)n["scale"][0].numberOr(1), sy = (float)n["scale"][1].numberOr(1), sz = (float)n["scale"][2].numberOr(1);
        det = sx * sy * sz;
        if (std::fabs(sx - sy) > 1e-4f || std::fabs(sy - sz) > 1e-4f) { char b[64]; std::snprintf(b, sizeof b, " nonuniform(%g,%g,%g)", sx, sy, sz); odd += b; }
      }
      if (det < 0) { odd += " MIRRORED"; flags.insert("negative-determinant node"); }
      if (n.has("skin")) { odd += " skin"; flags.insert("skin"); }
      if (n.has("camera")) odd += " camera";
      if (n["extensions"].has("KHR_lights_punctual")) odd += " light";
      if (all || odd.find("MIRRORED") != std::string::npos || odd.find("skin") != std::string::npos || odd.find("nonuniform") != std::string::npos)
        std::printf("  node %zu '%s':%s\n", ni, n["name"].stringOr("").c_str(), odd.c_str());
      ++ni;
    }
    if (doc["animations"].size()) flags.insert("animations");
    if (doc["skins"].size()) flags.insert("skins");
    std::string fl; for (const std::string& f : flags) fl += f + "; ";
    std::printf("  FLAGS: %s\n", fl.empty() ? "(none)" : fl.c_str());
  }
  return rc;
}
