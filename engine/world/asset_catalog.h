// Asset catalog: reads ppka-wannabe-2's public/model3d/model.json and resolves catalog ids
// (a `sarana` slot such as `loko206` / `nryK1TaksakaBadan`, or an `objek` id) to a GPU model.
// GLBs come from the local checkout, or (missing `pk-*` lokos) are downloaded once into the cache
// dir, then converted on demand to `.emod` with tools/convert (dev-time; logged). Missing files
// are skipped silently: model() returns nullptr and callers fall back to a box.
#pragma once
#include "engine/render/model_renderer.h"
#include <map>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace eng {

struct CatalogEntry {
  std::string id, berkas, nama, kategori;
  std::map<std::string, std::string> bogie, kopling;   // attach node name (or "*") -> slot id
  bool pilot = false, prosedural = false;
};

// `garis[]` class (spec §3.5): a body tile repeated every `langkah` metres along a spline, optional posts
// every `jarakTiang`, extra periodic slots. GLB bodies/posts are reachable through model() under the
// synthetic ids `garis:<id>` and `garis:<id>:tiang`; procedural ones name the code-built prototype.
struct GarisEntry {
  struct Slot { std::string berkas, prosedural; float jarak = 0; };
  std::string id, nama, kategori, berkas, prosedural, tiang, tiangProsedural;
  float langkah = 1, jarakTiang = 0, naik = 0;
  bool datar = false;
  std::vector<Slot> slot;
};

class AssetCatalog {
public:
  struct Options {
    std::string ppkaRoot;      // default: <repo>/../ppka-wannabe-2
    std::string cacheDir;      // default: <repo>/build/assets/cache
    std::string convertExe;    // default: <repo>/build/mac-debug/convert
    std::string downloadBase;
    int maxTexture;
    bool allowDownload, allowConvert;
    Options() : downloadBase("https://168railway.space/pk/model3d/"), maxTexture(512), allowDownload(true), allowConvert(true) {}
  };

  bool load(const Options& opt = Options());   // parses model.json; false + error() on failure
  // Host-streamed mode (web C ABI): the catalog text is handed over and there is no filesystem. model(id)
  // then returns nullptr for anything not provided yet and reports the id ONCE through `request`; the host
  // fetches the geometry-only `.emod` and calls provide(). Textures arrive later through texture streaming
  // (imageHints + streamedModel), the model draws white until then.
  using RequestFn = std::function<void(const std::string& id, const std::string& berkas)>;
  bool loadFromText(const std::string& modelJson, RequestFn request);
  bool provide(const std::string& id, std::span<const uint8_t> emodBytes, std::string& error);
  void fail(const std::string& id) { models_[id] = nullptr; }   // the host could not fetch it: box fallback from now on
  struct ImageHint { int source = -1; uint8_t wrapS = 0, wrapT = 0; bool linear = false, placeholder = false; };
  const std::vector<ImageHint>* imageHints(const std::string& id) const;   // streamed models only
  GpuModel* streamedModel(const std::string& id);                          // resident streamed model (nullptr otherwise)
  bool streaming() const { return (bool)request_; }
  bool pending(const std::string& id) const { return requested_.count(id) && !models_.count(id); }   // asked, not answered
  const std::string& error() const { return err_; }
  const Options& options() const { return opt_; }

  const CatalogEntry* find(const std::string& id) const;
  const GarisEntry* findGaris(const std::string& id) const;
  const std::vector<GarisEntry>& garis() const { return garis_; }
  std::vector<std::string> idsByCategory(const std::string& kategori) const;   // sorted by id
  // Path to a ready `.emod` for the id ("" if unavailable). Downloads/converts as needed.
  std::string emodPath(const std::string& id);
  std::string glbPath(const CatalogEntry& e);   // local or cached GLB ("" if unavailable)
  // GPU model for the id, cached by id. nullptr when the file is absent or broken.
  GpuModel* model(const std::string& id);
  void destroy();                               // frees every cached GpuModel

private:
  Options opt_;
  std::string err_;
  std::map<std::string, CatalogEntry> entries_;
  std::vector<GarisEntry> garis_;
  std::map<std::string, std::unique_ptr<GpuModel>> models_;   // nullptr = known missing
  bool parseCatalog(const std::string& text);
  RequestFn request_;
  std::map<std::string, std::vector<ImageHint>> hints_;   // streamed models: per-image wrap / colour-space / source
  std::map<std::string, bool> requested_;
};

} // namespace eng
