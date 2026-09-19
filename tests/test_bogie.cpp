// Bogies from the body's im2glb manifest (engine/world/rolling_stock.h bacaManifesBogie + AssetCatalog kuid index;
// the old src/tiga/bogie.ts): the catalog maps kuid -> entry through `kuid` / `kuidTrainz`, a `null` attach point is
// kept apart from an absent key, and the manifest is read back from the node extras text.
#include "engine/world/asset_catalog.h"
#include "engine/world/rolling_stock.h"
#include "tests/check.h"

using namespace eng;

TEST_MAIN({
  AssetCatalog cat;
  std::string json = R"({"sarana":{
    "anrBogieK5T":{"berkas":"k5t.glb","kuid":"kuid:2025:12052025"},
    "anrGkBadan":{"berkas":"gk.glb","kuidTrainz":"kuid:20:3072180942"},
    "k1":{"berkas":"k1.glb","bogie":{"a.bog1":null,"*":"anrBogieK5T"}},
    "k3":{"berkas":"k3.glb"}}})";
  CHECK(cat.loadFromText(json, [](const std::string&, const std::string&) {}));
  CHECK_EQ(cat.idByKuid("kuid:2025:12052025"), std::string("anrBogieK5T"));
  CHECK_EQ(cat.idByKuid("kuid:20:3072180942"), std::string("anrGkBadan"));   // kuidTrainz spelling too
  CHECK(cat.idByKuid("kuid:9:9").empty());
  const CatalogEntry* k1 = cat.find("k1"); CHECK(k1);
  CHECK(k1->bogie.count("a.bog1") && k1->bogie.at("a.bog1").empty());   // null = deliberately empty
  CHECK(k1->bogie.at("*") == "anrBogieK5T");
  const CatalogEntry* k3 = cat.find("k3"); CHECK(k3 && k3->bogie.empty());   // absent = fall back to the manifest
  // manifest read from the extras text gltf.cpp keeps
  GpuModel body; body.nodes.resize(2);
  body.nodes[0].name = "TrainzTraincar";
  body.nodes[0].extras["bogie"] = R"([{"titik":"a.bog0","kuid":"kuid:2025:12052025"},{"titik":"a.bog1","kuid":"kuid:2025:12052025","balik":true},{"kuid":"x"}])";
  body.nodes[1].name = "a.bog0";
  auto m = bacaManifesBogie(body);
  CHECK_EQ(m.size(), 2u);   // the entry without a point is dropped
  CHECK(m[0].titik == "a.bog0" && !m[0].balik);
  CHECK(m[1].titik == "a.bog1" && m[1].balik && m[1].kuid == "kuid:2025:12052025");
  GpuModel blender; blender.nodes.resize(1); blender.nodes[0].name = "a.bog0";
  CHECK(bacaManifesBogie(blender).empty());
  GpuModel rusak; rusak.nodes.resize(1); rusak.nodes[0].extras["bogie"] = "not json";
  CHECK(bacaManifesBogie(rusak).empty());
})
