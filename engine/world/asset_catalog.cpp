#include "engine/world/asset_catalog.h"
#include "engine/asset/emod.h"
#include "engine/core/json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifndef ENG_SOURCE_DIR
#define ENG_SOURCE_DIR "."
#endif

namespace eng {

namespace fs = std::filesystem;

namespace {

std::string shellQuote(const std::string& s) {
  std::string r = "'";
  for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
  return r + "'";
}

void readAttach(const Json& j, std::map<std::string, std::string>& out) {
  for (const auto& [k, v] : j.obj) if (v.isString()) out[k] = v.str;
}

CatalogEntry entryFrom(const std::string& id, const Json& j) {
  CatalogEntry e;
  e.id = id;
  e.berkas = j["pilotBerkas"].stringOr(j["berkas"].stringOr(""));
  e.nama = j["nama"].stringOr("");
  e.kategori = j["kategori"].stringOr("");
  e.pilot = j["pilot"].boolOr(false);
  e.prosedural = j.has("prosedural");
  readAttach(j["bogie"], e.bogie);
  readAttach(j["kopling"], e.kopling);
  return e;
}

} // namespace

bool AssetCatalog::load(const Options& opt) {
  opt_ = opt;
  const std::string root = ENG_SOURCE_DIR;
  if (opt_.ppkaRoot.empty()) { const char* e = std::getenv("PPKA_ROOT"); opt_.ppkaRoot = e && *e ? e : root + "/../ppka-wannabe-2"; }
  if (opt_.cacheDir.empty()) opt_.cacheDir = root + "/build/assets/cache";
  if (opt_.convertExe.empty()) opt_.convertExe = root + "/build/mac-debug/convert";

  std::string path = opt_.ppkaRoot + "/public/model3d/model.json";
  std::ifstream f(path, std::ios::binary);
  if (!f) { err_ = "cannot open " + path; return false; }
  std::string text((std::istreambuf_iterator<char>(f)), {});
  std::string jerr; Json doc = Json::parse(text, &jerr);
  if (!jerr.empty()) { err_ = "model.json: " + jerr; return false; }
  entries_.clear();
  for (const auto& [id, j] : doc["sarana"].obj) if (j.isObject()) entries_[id] = entryFrom(id, j);
  for (const Json& j : doc["objek"].arr) {
    std::string id = j["id"].stringOr("");
    if (!id.empty() && !entries_.count(id)) entries_[id] = entryFrom(id, j);
  }
  garis_.clear();
  for (const Json& j : doc["garis"].arr) {
    GarisEntry g;
    g.id = j["id"].stringOr(""); if (g.id.empty()) continue;
    g.nama = j["nama"].stringOr(""); g.kategori = j["kategori"].stringOr("");
    g.berkas = j["berkas"].stringOr(""); g.prosedural = j["prosedural"].stringOr("");
    g.tiang = j["tiang"].stringOr(""); g.tiangProsedural = j["tiangProsedural"].stringOr("");
    g.langkah = (float)j["langkah"].numberOr(1); g.jarakTiang = (float)j["jarakTiang"].numberOr(0); g.naik = (float)j["naik"].numberOr(0);
    g.datar = j["datar"].boolOr(false);
    for (const Json& sl : j["slot"].arr) g.slot.push_back({sl["berkas"].stringOr(""), sl["prosedural"].stringOr(""), (float)sl["jarak"].numberOr(0)});
    auto reg = [&](const std::string& key, const std::string& berkas) {
      if (berkas.empty()) return;
      CatalogEntry e; e.id = key; e.berkas = berkas; e.nama = g.nama; e.kategori = g.kategori; e.pilot = j["pilot"].boolOr(false) || berkas.rfind("pilot-", 0) == 0;
      entries_[key] = e;
    };
    reg("garis:" + g.id, g.berkas); reg("garis:" + g.id + ":tiang", g.tiang);
    for (size_t i = 0; i < g.slot.size(); ++i) reg("garis:" + g.id + ":slot" + std::to_string(i), g.slot[i].berkas);
    garis_.push_back(g);
  }
  std::error_code ec; fs::create_directories(opt_.cacheDir, ec);
  return true;
}

// §7.2 MODEL_SARANA: sarana ids the bridge falls back to when no fleet model applies.
static const char* saranaSlot(const std::string& id) {
  static const std::pair<const char*, const char*> table[] = {
    {"cc201", "loko201"}, {"cc203", "loko203"}, {"cc206", "loko206"},
    {"krl-kuha", "nryJr205KuhaBadan"}, {"krl-moha", "nryJr205MohaBadan"}, {"krl-moha-p", "nryJr205MpBadan"},
    {"k1", "kereta"}, {"k3", "kereta"}, {"m1", "makan"}, {"p", "pembangkit"}, {"gd", "gerbong"}, {"d14", "atpD14Jaladara"}};
  for (const auto& [k, v] : table) if (id == k) return v;
  return nullptr;
}

const CatalogEntry* AssetCatalog::find(const std::string& id) const {
  auto it = entries_.find(id);
  if (it == entries_.end()) if (const char* alias = saranaSlot(id)) it = entries_.find(alias);
  return it == entries_.end() ? nullptr : &it->second;
}

const GarisEntry* AssetCatalog::findGaris(const std::string& id) const {
  for (const GarisEntry& g : garis_) if (g.id == id) return &g;
  return nullptr;
}

std::vector<std::string> AssetCatalog::idsByCategory(const std::string& kategori) const {
  std::vector<std::string> out;
  for (const auto& [id, e] : entries_) if (e.kategori == kategori) out.push_back(id);
  return out;
}

std::string AssetCatalog::glbPath(const CatalogEntry& e) {
  if (e.berkas.empty() || e.prosedural) return "";
  std::error_code ec;
  std::string local = opt_.ppkaRoot + "/public/model3d/" + e.berkas;
  if (fs::exists(local, ec)) return local;
  std::string cached = opt_.cacheDir + "/" + e.berkas;
  if (fs::exists(cached, ec)) return cached;
  if (e.pilot || !opt_.allowDownload) return "";   // third-party local-only assets are never fetched
  std::string url = opt_.downloadBase + e.berkas, tmp = cached + ".part";
  std::string cmd = "curl -fsSL --max-time 300 -o " + shellQuote(tmp) + " " + shellQuote(url);
  std::printf("[assets] downloading %s\n", url.c_str());
  int rc = std::system(cmd.c_str());
  if (rc != 0 || !fs::exists(tmp, ec) || fs::file_size(tmp, ec) == 0) {
    std::printf("[assets] download failed for %s (rc %d)\n", e.berkas.c_str(), rc);
    fs::remove(tmp, ec); return "";
  }
  fs::rename(tmp, cached, ec);
  return ec ? "" : cached;
}

std::string AssetCatalog::emodPath(const std::string& id) {
  const CatalogEntry* e = find(id);
  if (!e) return "";
  std::string glb = glbPath(*e);
  if (glb.empty()) return "";
  std::error_code ec;
  std::string emod = opt_.cacheDir + "/" + fs::path(e->berkas).stem().string() + ".emod";
  bool stale = !fs::exists(emod, ec) || fs::last_write_time(emod, ec) < fs::last_write_time(glb, ec);
  if (!stale) {   // also re-convert files written by an older converter (format version)
    std::ifstream f(emod, std::ios::binary); char hdr[8] = {}; f.read(hdr, 8);
    uint32_t ver = 0; std::memcpy(&ver, hdr + 4, 4);
    stale = ver != EMOD_VERSION;
  }
  if (stale) {
    if (!opt_.allowConvert) return "";
    std::string cmd = shellQuote(opt_.convertExe) + " " + shellQuote(glb) + " " + shellQuote(emod) +
                      " --max-texture " + std::to_string(opt_.maxTexture) + " >/dev/null";
    std::printf("[assets] converting %s -> %s (max texture %d)\n", glb.c_str(), emod.c_str(), opt_.maxTexture);
    int rc = std::system(cmd.c_str());
    if (rc != 0 || !fs::exists(emod, ec)) { std::printf("[assets] convert failed for %s (rc %d)\n", e->berkas.c_str(), rc); fs::remove(emod, ec); return ""; }
  }
  return emod;
}

GpuModel* AssetCatalog::model(const std::string& id) {
  auto it = models_.find(id);
  if (it != models_.end()) return it->second.get();
  std::unique_ptr<GpuModel> gpu;
  std::string path = emodPath(id);
  if (!path.empty()) {
    Model m; std::string err;
    if (loadEmod(path, m, err)) { gpu = std::make_unique<GpuModel>(); gpu->upload(m); }
    else std::printf("[assets] %s: %s\n", id.c_str(), err.c_str());
  }
  GpuModel* raw = gpu.get();
  models_[id] = std::move(gpu);
  return raw;
}

void AssetCatalog::destroy() {
  for (auto& [id, m] : models_) if (m) m->destroy();
  models_.clear();
}

} // namespace eng
