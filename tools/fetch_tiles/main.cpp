// Offline terrain fetcher: map name -> assets/terrain/<map>.dem + <map>.sat (see engine/world/terrain.h
// for the formats). Downloads Terrarium DEM tiles (z13 core, z10 far) and satellite tiles (z14 near
// composed from --sat-detail sub-tiles, one coarse far layer, and z16/z17 detail layers along the track
// and around stations) with curl, decodes with stb_image.
//   fetch_tiles <map> [--maps DIR] [--out DIR] [--cache DIR] [--sat-detail 15] [--sat-size 512]
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "tools/third_party/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "tools/third_party/stb_image_resize2.h"

#include "engine/core/json.h"
#include "engine/world/slippy.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <vector>

using namespace eng;
namespace fs = std::filesystem;

static const char* TILE_168 = "https://tiles.168railway.com";
static const char* ESRI = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile";
static const char* AWS_DEM = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium";

struct Bbox { double x0, y0, x1, y1; };
struct TileRange { int z, tx0, ty0, tx1, ty1; int nx() const { return tx1 - tx0 + 1; } int ny() const { return ty1 - ty0 + 1; } };

static TileRange rangeFor(const Bbox& b, double margin, int z) {
  return {z, slippy::worldToTileX(b.x0 - margin, z), slippy::worldToTileY(b.y0 - margin, z),
          slippy::worldToTileX(b.x1 + margin, z), slippy::worldToTileY(b.y1 + margin, z)};
}

// Runs curl; returns the HTTP status (0 on transport failure). The body is written to `path`.
static int curlTo(const std::string& url, const fs::path& path) {
  std::string cmd = "curl -sL --max-time 40 -o '" + path.string() + "' -w '%{http_code}' '" + url + "'";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return 0;
  char buf[32] = {}; size_t n = fread(buf, 1, sizeof buf - 1, p); buf[n] = 0;
  pclose(p);
  return std::atoi(buf);
}

// Fetch with cache; tries `urls` in order, accepts the first 200 with a non-empty body.
static bool fetch(const std::vector<std::string>& urls, const fs::path& cached, int& downloads) {
  std::error_code ec;
  if (fs::exists(cached, ec) && fs::file_size(cached, ec) > 0) return true;
  fs::create_directories(cached.parent_path(), ec);
  for (const std::string& u : urls) {
    int code = curlTo(u, cached);
    ++downloads;
    if (code == 200 && fs::exists(cached, ec) && fs::file_size(cached, ec) > 0) return true;
    fs::remove(cached, ec);
  }
  return false;
}

struct Pt { double x, y; };

// Tiles at zoom z whose box lies within `radius` of any point: bounding range + present mask.
static bool selectTiles(int z, const std::vector<Pt>& pts, double radius, TileRange& r, std::vector<uint8_t>& present) {
  if (pts.empty()) return false;
  Bbox b{1e30, 1e30, -1e30, -1e30};
  for (const Pt& p : pts) { b.x0 = std::min(b.x0, p.x); b.y0 = std::min(b.y0, p.y); b.x1 = std::max(b.x1, p.x); b.y1 = std::max(b.y1, p.y); }
  r = rangeFor(b, radius, z);
  const double ts = slippy::tileSizeMeter(z);
  present.assign((size_t)r.nx() * r.ny(), 0);
  int count = 0;
  for (int ty = r.ty0; ty <= r.ty1; ++ty)
    for (int tx = r.tx0; tx <= r.tx1; ++tx) {
      double x0 = slippy::tileOriginX(tx, z), y0 = slippy::tileOriginY(ty, z);
      for (const Pt& p : pts) {
        double dx = std::max({x0 - p.x, 0.0, p.x - (x0 + ts)}), dy = std::max({y0 - p.y, 0.0, p.y - (y0 + ts)});
        if (dx * dx + dy * dy <= radius * radius) { present[(size_t)(ty - r.ty0) * r.nx() + (tx - r.tx0)] = 1; ++count; break; }
      }
    }
  return count > 0;
}

static void put32(std::ofstream& f, int32_t v) { f.write((const char*)&v, 4); }
static void putf64(std::ofstream& f, double v) { f.write((const char*)&v, 8); }

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: fetch_tiles <map> [--maps DIR] [--out DIR] [--cache DIR] [--sat-detail Z] [--sat-size PX]\n"); return 2; }
  std::string map = argv[1];
  std::string mapsDir = std::string(ENG_SOURCE_DIR) + "/../ppka-wannabe-2/src/data";
  std::string outDir = std::string(ENG_SOURCE_DIR) + "/assets/terrain";
  const char* tmp = std::getenv("TMPDIR");
  std::string cacheDir = std::string(tmp ? tmp : "/tmp") + "/eng-tile-cache";
  int satDetail = 15, satSize = 512;
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--maps")) mapsDir = argv[i + 1];
    else if (!std::strcmp(argv[i], "--out")) outDir = argv[i + 1];
    else if (!std::strcmp(argv[i], "--cache")) cacheDir = argv[i + 1];
    else if (!std::strcmp(argv[i], "--sat-detail")) satDetail = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--sat-size")) satSize = std::atoi(argv[i + 1]);
    else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  auto t0 = std::chrono::steady_clock::now();

  // ---- map bbox from track nodes ----
  std::ifstream in(mapsDir + "/" + map + ".json");
  if (!in) { std::fprintf(stderr, "cannot open %s/%s.json\n", mapsDir.c_str(), map.c_str()); return 1; }
  std::stringstream ss; ss << in.rdbuf();
  std::string err; Json doc = Json::parse(ss.str(), &err);
  if (!err.empty()) { std::fprintf(stderr, "json: %s\n", err.c_str()); return 1; }
  const Json& nodes = doc["world"]["graph"]["nodes"];
  if (!nodes.size()) { std::fprintf(stderr, "no world.graph.nodes in map\n"); return 1; }
  Bbox bb{1e30, 1e30, -1e30, -1e30};
  for (const Json& n : nodes.arr) {
    double x = n["x"].numberOr(0), y = n["y"].numberOr(0);
    bb.x0 = std::min(bb.x0, x); bb.y0 = std::min(bb.y0, y); bb.x1 = std::max(bb.x1, x); bb.y1 = std::max(bb.y1, y);
  }
  std::printf("%s: %zu nodes, bbox x %.1f..%.1f (%.1f km) y %.1f..%.1f (%.1f km)\n", map.c_str(), nodes.size(),
              bb.x0, bb.x1, (bb.x1 - bb.x0) / 1000, bb.y0, bb.y1, (bb.y1 - bb.y0) / 1000);

  int downloads = 0, failed = 0;
  fs::path cache(cacheDir);

  // ---- DEM: z13 core (bbox ± 5000) and z10 far (bbox ± 2000) ----
  TileRange demRanges[2] = {rangeFor(bb, 5000, 13), rangeFor(bb, 2000, 10)};
  fs::create_directories(outDir);
  std::ofstream dem(outDir + "/" + map + ".dem", std::ios::binary);
  dem.write("EDEM", 4); put32(dem, 1);
  putf64(dem, bb.x0); putf64(dem, bb.y0); putf64(dem, bb.x1); putf64(dem, bb.y1);
  put32(dem, 2);
  for (const TileRange& r : demRanges) {
    const int n = 256;
    put32(dem, r.z); put32(dem, r.tx0); put32(dem, r.ty0); put32(dem, r.nx()); put32(dem, r.ny()); put32(dem, n);
    std::vector<uint8_t> present((size_t)r.nx() * r.ny(), 0);
    std::vector<float> heights((size_t)r.nx() * r.ny() * n * n, 0.f);
    for (int ty = r.ty0; ty <= r.ty1; ++ty)
      for (int tx = r.tx0; tx <= r.tx1; ++tx) {
        std::string tail = "/" + std::to_string(r.z) + "/" + std::to_string(tx) + "/" + std::to_string(ty) + ".png";
        fs::path file = cache / "terrarium" / (std::to_string(r.z) + "_" + std::to_string(tx) + "_" + std::to_string(ty) + ".png");
        size_t ti = (size_t)(ty - r.ty0) * r.nx() + (tx - r.tx0);
        if (!fetch({std::string(TILE_168) + "/terrarium" + tail, std::string(AWS_DEM) + tail}, file, downloads)) {
          std::fprintf(stderr, "DEM tile %d/%d/%d failed (left flat)\n", r.z, tx, ty); ++failed; continue;
        }
        int w, h, c;
        unsigned char* px = stbi_load(file.string().c_str(), &w, &h, &c, 3);
        if (!px || w != n || h != n) { std::fprintf(stderr, "DEM tile %d/%d/%d: bad image\n", r.z, tx, ty); ++failed; if (px) stbi_image_free(px); continue; }
        float* dst = heights.data() + ti * n * n;
        for (int i = 0; i < n * n; ++i) dst[i] = px[i * 3] * 256.f + px[i * 3 + 1] + px[i * 3 + 2] / 256.f - 32768.f;
        stbi_image_free(px);
        present[ti] = 1;
      }
    dem.write((const char*)present.data(), (std::streamsize)present.size());
    dem.write((const char*)heights.data(), (std::streamsize)(heights.size() * 4));
    std::printf("DEM z%d: %dx%d tiles (%d..%d, %d..%d)\n", r.z, r.nx(), r.ny(), r.tx0, r.tx1, r.ty0, r.ty1);
  }
  dem.close();

  // ---- satellite (ESAT v2, sparse): near z14 (bbox ± 2500) composed from z<satDetail> sub-tiles, far z10..12
  // (bbox ± 2000), then detail layers (spec DETAIL_TANAH): z16 @256 within 1500 m of any track node,
  // z16 @512 (4× z17) within 1500 m of a station, z17 @512 (4× z18) within 450 m of a station.
  int zFar = 10;
  for (int z = 12; z > 10; --z) { TileRange r = rangeFor(bb, 2000, z); if (r.nx() * r.ny() <= 40) { zFar = z; break; } }
  std::vector<Pt> trackPts, stationPts;
  for (const Json& n : nodes.arr) trackPts.push_back({n["x"].numberOr(0), n["y"].numberOr(0)});
  for (const Json& sc : doc["world"]["scenery"].arr)
    if (sc["kind"].stringOr("") == "station") stationPts.push_back({sc["pos"]["x"].numberOr(0), sc["pos"]["y"].numberOr(0)});
  struct LayerSpec { TileRange r; std::vector<uint8_t> present; int sub, px; };
  std::vector<LayerSpec> layers;
  {
    TileRange r = rangeFor(bb, 2500, 14);
    layers.push_back({r, std::vector<uint8_t>((size_t)r.nx() * r.ny(), 1), std::max(0, satDetail - 14), std::min(256 << std::max(0, satDetail - 14), satSize)});
    r = rangeFor(bb, 2000, zFar);
    layers.push_back({r, std::vector<uint8_t>((size_t)r.nx() * r.ny(), 1), 0, 256});
    LayerSpec d;
    if (selectTiles(16, trackPts, 1500, d.r, d.present)) { d.sub = 0; d.px = 256; layers.push_back(d); }
    if (selectTiles(16, stationPts, 1500, d.r, d.present)) { d.sub = 1; d.px = 512; layers.push_back(d); }
    if (selectTiles(17, stationPts, 450, d.r, d.present)) { d.sub = 1; d.px = 512; layers.push_back(d); }
  }
  std::ofstream sat(outDir + "/" + map + ".sat", std::ios::binary);
  sat.write("ESAT", 4); put32(sat, 2); put32(sat, (int32_t)layers.size());
  for (const LayerSpec& L : layers) {
    const TileRange& r = L.r;
    int k = 1 << L.sub, srcPx = 256 * k, px = L.px;
    put32(sat, r.z); put32(sat, r.tx0); put32(sat, r.ty0); put32(sat, r.nx()); put32(sat, r.ny()); put32(sat, px);
    sat.write((const char*)L.present.data(), (std::streamsize)L.present.size());
    std::vector<unsigned char> img((size_t)srcPx * srcPx * 3), out((size_t)px * px * 3);
    int ok = 0, want = 0;
    for (int ty = r.ty0; ty <= r.ty1; ++ty)
      for (int tx = r.tx0; tx <= r.tx1; ++tx) {
        if (!L.present[(size_t)(ty - r.ty0) * r.nx() + (tx - r.tx0)]) continue;
        ++want;
        std::fill(img.begin(), img.end(), 0x60);
        bool any = false;
        for (int sy = 0; sy < k; ++sy)
          for (int sx = 0; sx < k; ++sx) {
            int z = r.z + L.sub, x = tx * k + sx, y = ty * k + sy;
            std::string zs = std::to_string(z), xs = std::to_string(x), ys = std::to_string(y);
            fs::path file = cache / "satellite" / (zs + "_" + xs + "_" + ys + ".img");
            if (!fetch({std::string(TILE_168) + "/satellite/" + zs + "/" + xs + "/" + ys + ".png",
                        std::string(ESRI) + "/" + zs + "/" + ys + "/" + xs}, file, downloads)) {
              std::fprintf(stderr, "satellite tile %d/%d/%d failed\n", z, x, y); ++failed; continue;
            }
            int w, h, c;
            unsigned char* p = stbi_load(file.string().c_str(), &w, &h, &c, 3);
            if (!p) { std::fprintf(stderr, "satellite tile %d/%d/%d: bad image\n", z, x, y); ++failed; continue; }
            unsigned char* fit = p;
            if (w != 256 || h != 256) {
              fit = (unsigned char*)std::malloc(256 * 256 * 3);
              stbir_resize_uint8_srgb(p, w, h, 0, fit, 256, 256, 0, STBIR_RGB);
            }
            for (int row = 0; row < 256; ++row)
              std::memcpy(&img[(((size_t)sy * 256 + row) * srcPx + (size_t)sx * 256) * 3], fit + (size_t)row * 256 * 3, 256 * 3);
            if (fit != p) std::free(fit);
            stbi_image_free(p);
            any = true;
          }
        if (px != srcPx) stbir_resize_uint8_srgb(img.data(), srcPx, srcPx, 0, out.data(), px, px, 0, STBIR_RGB);
        else out = img;
        sat.write((const char*)out.data(), (std::streamsize)out.size());
        ok += any;
      }
    std::printf("satellite z%d @ %d px (%.2f m/px): %d/%d tiles in a %dx%d range (%d with imagery), %.1f MB\n", r.z, px,
                slippy::tileSizeMeter(r.z) / px, want, r.nx() * r.ny(), r.nx(), r.ny(), ok, (double)want * px * px * 3 / 1e6);
  }
  sat.close();

  std::error_code ec;
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("-> %s/%s.dem (%.1f MB), %s/%s.sat (%.1f MB); %d downloads, %d failures, %.1f s\n", outDir.c_str(), map.c_str(),
              fs::file_size(outDir + "/" + map + ".dem", ec) / 1e6, outDir.c_str(), map.c_str(),
              fs::file_size(outDir + "/" + map + ".sat", ec) / 1e6, downloads, failed, s);
  return failed ? 1 : 0;
}
