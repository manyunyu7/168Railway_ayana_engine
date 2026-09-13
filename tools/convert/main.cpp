// Offline converter: GLB -> .emod (v5). The only place third-party image decoding / texture encoding lives.
//   convert in.glb out.emod [--max-texture 1024] [--target desktop|web|android] [--ktx2 auto|none|FILE]
//                           [--fallback PX] [--raw]
// Textures are stored GPU-ready with full mip chains: web/android = ETC2 (RGB or RGBA/EAC), desktop = BC1/BC3
// (macOS GL exposes S3TC only). Sources, in order of preference:
//   1. the Basis Universal (KTX2, ETC1S) twin of the GLB shipped by the reference project
//      (`<dir>/ktx2/<name>.glb`, KHR_texture_basisu), transcoded per level with the BinomialLLC transcoder;
//   2. the GLB's own PNG/JPEG, decoded, resized to the budget and encoded with tools/texcomp (own ETC1/EAC,
//      stb_dxt for BC).
// `--fallback PX` adds a PX-wide RGBA8 copy for GPUs without the format (web default 256, others none);
// `--raw` writes plain RGBA8 (EMOD v5 without compression, as v4 did).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "tools/third_party/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "tools/third_party/stb_image_resize2.h"
#define TEXCOMP_IMPLEMENTATION
#include "tools/texcomp/texcomp.h"

// Basis Universal transcoder (unity build; only ETC1S -> ETC1/EAC/BC1/BC3 is used; UASTC stays on because the
// upstream #ifdef guards do not compile without it)
#define BASISD_SUPPORT_KTX2 1
#define BASISD_SUPPORT_KTX2_ZSTD 0
#define BASISD_SUPPORT_ASTC 0
#define BASISD_SUPPORT_ATC 0
#define BASISD_SUPPORT_PVRTC1 0
#define BASISD_SUPPORT_PVRTC2 0
#define BASISD_SUPPORT_FXT1 0
#define BASISD_SUPPORT_BC7 0
#define BASISD_SUPPORT_BC7_MODE5 0
#define BASISD_SUPPORT_ETC2_EAC_RG11 0
#include "tools/third_party/basisu/basisu_transcoder.cpp"

#include "engine/asset/emod.h"
#include "engine/asset/gltf.h"
#include "engine/core/json.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace eng;
namespace fs = std::filesystem;

// Images (raw bytes) of a GLB, ignoring everything else — used for the KTX2 twin whose meshes are Draco
// compressed and whose extensionsRequired the engine parser refuses.
static bool glbImages(const std::string& path, std::vector<std::vector<uint8_t>>& out, std::string& err) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { err = "cannot open " + path; return false; }
  std::vector<uint8_t> b((size_t)f.tellg()); f.seekg(0); f.read((char*)b.data(), (std::streamsize)b.size());
  if (b.size() < 20 || std::memcmp(b.data(), "glTF", 4) != 0) { err = "not a GLB"; return false; }
  uint32_t jl = 0; std::memcpy(&jl, b.data() + 12, 4);
  if (20 + (size_t)jl + 8 > b.size()) { err = "bad GLB"; return false; }
  uint32_t bl = 0; std::memcpy(&bl, b.data() + 20 + jl, 4);
  const uint8_t* bin = b.data() + 28 + jl;
  if (28 + (size_t)jl + bl > b.size()) { err = "bad GLB"; return false; }
  std::string jerr; Json doc = Json::parse(std::string_view((const char*)b.data() + 20, jl), &jerr);
  if (!jerr.empty()) { err = jerr; return false; }
  for (const Json& im : doc["images"].arr) {
    const Json& bv = doc["bufferViews"][(size_t)im["bufferView"].intOr(-1)];
    size_t off = (size_t)bv["byteOffset"].intOr(0), len = (size_t)bv["byteLength"].intOr(0);
    if (bv.isNull() || off + len > bl) { err = "bad image bufferView"; return false; }
    out.emplace_back(bin + off, bin + off + len);
  }
  return true;
}

// Transcodes a KTX2 (ETC1S, full mip chain) to the target's format, dropping levels above the budget.
static bool transcodeKtx2(const std::vector<uint8_t>& ktx, texcomp::Target target, int budgetW, int budgetH,
                          ImageVariant& out, std::string& err) {
  basist::ktx2_transcoder tr;
  if (!tr.init(ktx.data(), (uint32_t)ktx.size())) { err = "KTX2 header rejected"; return false; }
  if (!tr.is_etc1s()) { err = "KTX2 is not ETC1S (UASTC not built in)"; return false; }
  if (!tr.start_transcoding()) { err = "KTX2 start_transcoding failed"; return false; }
  bool alpha = tr.get_has_alpha() != 0;
  out.format = texcomp::formatFor(target, alpha);
  basist::transcoder_texture_format fmt = target == texcomp::Target::Desktop
      ? (alpha ? basist::transcoder_texture_format::cTFBC3_RGBA : basist::transcoder_texture_format::cTFBC1_RGB)
      : (alpha ? basist::transcoder_texture_format::cTFETC2_RGBA : basist::transcoder_texture_format::cTFETC1_RGB);
  int skip = 0;
  for (uint32_t w = tr.get_width(), h = tr.get_height(); (int)w > budgetW || (int)h > budgetH; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) ++skip;
  if ((uint32_t)skip >= tr.get_levels()) { err = "KTX2 mip chain too short for the budget"; return false; }
  for (uint32_t level = (uint32_t)skip; level < tr.get_levels(); ++level) {
    basist::ktx2_image_level_info info;
    if (!tr.get_image_level_info(info, level, 0, 0)) { err = "KTX2 level info failed"; return false; }
    MipLevel l{(int)info.m_orig_width, (int)info.m_orig_height, {}};
    l.data.resize(texLevelBytes(out.format, l.width, l.height));
    if (!tr.transcode_image_level(level, 0, 0, l.data.data(), info.m_total_blocks, fmt)) { err = "KTX2 transcode failed"; return false; }
    out.mips.push_back(std::move(l));
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: convert in.glb out.emod [--max-texture N] [--target desktop|web|android] [--ktx2 auto|none|FILE] [--fallback PX] [--raw]\n"); return 2; }
  int maxTex = 2048; bool raw = false; std::string ktx2 = "auto";
  texcomp::Options opt;
  for (int i = 3; i < argc; ++i) {
    auto val = [&](const char* name) { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", name); std::exit(2); } return argv[++i]; };
    if (!std::strcmp(argv[i], "--max-texture")) maxTex = std::atoi(val("--max-texture"));
    else if (!std::strcmp(argv[i], "--target")) { bool ok; opt.target = texcomp::parseTarget(val("--target"), ok); if (!ok) { std::fprintf(stderr, "unknown target\n"); return 2; } }
    else if (!std::strcmp(argv[i], "--ktx2")) ktx2 = val("--ktx2");
    else if (!std::strcmp(argv[i], "--fallback")) opt.fallback = std::atoi(val("--fallback"));
    else if (!std::strcmp(argv[i], "--raw")) raw = true;
    else if (!std::strcmp(argv[i], "--fast")) opt.highQuality = false;
    else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }

  auto t0 = std::chrono::steady_clock::now();
  Model model; std::string err;
  if (!loadGlbFile(argv[1], model, err)) { std::fprintf(stderr, "GLB: %s\n", err.c_str()); return 1; }

  size_t tris = 0; for (auto& me : model.meshes) for (auto& p : me.primitives) tris += p.indices.size() / 3;
  std::printf("%s: %zu meshes, %zu tris, %zu materials, %zu images, %zu nodes\n", argv[1],
              model.meshes.size(), tris, model.materials.size(), model.images.size(), model.nodes.size());

  // KTX2 twin: <dir>/ktx2/<name>.glb (reference project layout) unless given explicitly
  std::vector<std::vector<uint8_t>> ktxImages;
  if (!raw && ktx2 != "none" && !model.images.empty()) {
    fs::path src(argv[1]);
    std::string twin = ktx2 == "auto" ? (src.parent_path() / "ktx2" / src.filename()).string() : ktx2;
    std::error_code ec;
    if (fs::exists(twin, ec)) {
      std::string e2;
      if (!glbImages(twin, ktxImages, e2)) { std::fprintf(stderr, "KTX2 twin %s: %s (ignored)\n", twin.c_str(), e2.c_str()); ktxImages.clear(); }
      else if (ktxImages.size() != model.images.size()) { std::fprintf(stderr, "KTX2 twin %s has %zu images, GLB %zu (ignored)\n", twin.c_str(), ktxImages.size(), model.images.size()); ktxImages.clear(); }
      else { std::printf("  KTX2 twin: %s\n", twin.c_str()); basist::basisu_transcoder_init(); }
    } else if (ktx2 != "auto") { std::fprintf(stderr, "KTX2 file %s not found\n", twin.c_str()); return 1; }
  }

  size_t totalTex = 0;
  for (size_t i = 0; i < model.images.size(); ++i) {
    Image& im = model.images[i];
    int w, h, c;
    unsigned char* px = stbi_load_from_memory(im.encoded.data(), (int)im.encoded.size(), &w, &h, &c, 4);
    if (!px) { std::fprintf(stderr, "image %zu (%s): decode failed: %s\n", i, im.mime.c_str(), stbi_failure_reason()); return 1; }
    int ow = w, oh = h;
    // budget: no side above 2*maxTex and at most (2*maxTex)^2 pixels — with 512: a 4096x2048 livery atlas
    // keeps 1024x512, a 4096x4096 loco atlas 1024x1024 (at 512x512 the numbers were unreadable)
    while ((size_t)w * h > (size_t)4 * maxTex * maxTex || w > 2 * maxTex || h > 2 * maxTex) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    if (w != ow || h != oh) {
      unsigned char* rs = (unsigned char*)std::malloc((size_t)w * h * 4);
      if (im.linear) stbir_resize_uint8_linear(px, ow, oh, 0, rs, w, h, 0, STBIR_RGBA);   // data maps: no sRGB curve
      else stbir_resize_uint8_srgb(px, ow, oh, 0, rs, w, h, 0, STBIR_RGBA);
      stbi_image_free(px); px = rs;
    }
    im.width = w; im.height = h; im.channels = 4;
    im.pixels.assign(px, px + (size_t)w * h * 4);
    (w != ow) ? std::free(px) : stbi_image_free(px);
    im.encoded.clear(); im.encoded.shrink_to_fit();
    std::string how = "raw RGBA8";
    if (!raw) {
      ImageVariant v; std::string e2;
      // the KTX2 twin may be a different resolution: same aspect ratio (±1 px per halving) or it is not the same picture
      bool twinOk = i < ktxImages.size();
      if (twinOk && !im.linear) {
        if (transcodeKtx2(ktxImages[i], opt.target, w, h, v, e2)) {
          double arGlb = (double)ow / oh, arKtx = (double)v.mips[0].width / v.mips[0].height;
          if (std::abs(arGlb - arKtx) > 0.05 * arGlb) { std::fprintf(stderr, "image %zu: KTX2 twin aspect %.3f vs GLB %.3f, encoding from PNG instead\n", i, arKtx, arGlb); v = {}; }
        } else { std::fprintf(stderr, "image %zu: KTX2 twin: %s, encoding from PNG instead\n", i, e2.c_str()); v = {}; }
      }
      if (!v.mips.empty()) {
        im.width = v.mips[0].width; im.height = v.mips[0].height;   // GPU size is the KTX2 level's
        im.variants.push_back(std::move(v));
        texcomp::appendFallback(im.variants, im.pixels.data(), w, h, im.linear, opt);
        how = "KTX2 -> ";
      } else {
        im.variants = texcomp::buildVariants(im.pixels.data(), w, h, im.linear, opt);
        how = "encoded ";
      }
      static const char* names[] = {"RGBA8", "ETC2_RGB", "ETC2_RGBA", "BC1", "BC3", "BC7"};
      how += names[(int)im.variants[0].format]; how += " x" + std::to_string(im.variants[0].mips.size()) + " mips";
      if (im.variants.size() > 1) how += ", fallback " + std::to_string(im.variants[1].mips[0].width) + "x" + std::to_string(im.variants[1].mips[0].height);
      im.pixels.clear(); im.pixels.shrink_to_fit();
    }
    size_t bytes = raw ? (size_t)w * h * 4 : texcomp::variantBytes(im.variants);
    totalTex += bytes;
    std::printf("  image %zu: %dx%d -> %dx%d%s%s, %s, %.0f KB\n", i, ow, oh, im.width, im.height, im.linear ? " linear" : "",
                im.wrapS == 1 || im.wrapT == 1 ? " clamp" : "", how.c_str(), bytes / 1024.0);
  }

  if (!saveEmod(model, argv[2], err)) { std::fprintf(stderr, "EMOD: %s\n", err.c_str()); return 1; }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::error_code ec;
  std::printf("-> %s (%.1f MB, textures %.1f MB, %.0f ms), bounds [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n", argv[2],
              fs::file_size(argv[2], ec) / 1e6, totalTex / 1e6, ms,
              model.boundsMin.x, model.boundsMin.y, model.boundsMin.z, model.boundsMax.x, model.boundsMax.y, model.boundsMax.z);
  return 0;
}
