// Cargo (model.json `muatan`, the old src/tiga/muatan.ts): catalog parsing keeps string / list / null apart, and the
// per-vehicle pick is the same FNV-1a the reference client used (reference values from the JS pilihMuatan).
#include "engine/world/asset_catalog.h"
#include "engine/world/rolling_stock.h"
#include "tests/check.h"

using namespace eng;

TEST_MAIN({
  AssetCatalog cat;
  std::string json = R"({"sarana":{
    "gd":{"berkas":"gd.glb","muatan":{"*":["a","b","a","","c"],"a.muatan1":null,"a.muatan2":"d"}},
    "k1":{"berkas":"k1.glb"}}})";
  CHECK(cat.loadFromText(json, [](const std::string&, const std::string&) {}));
  const CatalogEntry* gd = cat.find("gd"); CHECK(gd);
  CHECK(gd->muatan.count("*") && gd->muatan.at("*").size() == 3);        // dedup + empty dropped
  CHECK(gd->muatan.at("*")[0] == "a" && gd->muatan.at("*")[2] == "c");
  CHECK(gd->muatan.count("a.muatan1") && gd->muatan.at("a.muatan1").empty());   // null = deliberately empty
  CHECK(gd->muatan.at("a.muatan2").size() == 1 && gd->muatan.at("a.muatan2")[0] == "d");
  const CatalogEntry* k1 = cat.find("k1"); CHECK(k1 && k1->muatan.empty());
  // pilihMuatan(8, "2503", 0) == 2, (8, "2503", 7) == 3, (3, "KLB", 3) == 2
  CHECK_EQ(RollingStock::cargoHash("2503", 0), 366193274u);
  CHECK_EQ(RollingStock::cargoHash("2503", 7), 282305179u);
  CHECK_EQ(RollingStock::cargoHash("KLB", 3), 1303749326u);
  CHECK_EQ(RollingStock::cargoVariant(RollingStock::cargoHash("2503", 0), 8), 2u);
  CHECK_EQ(RollingStock::cargoVariant(RollingStock::cargoHash("2503", 7), 8), 3u);
  CHECK_EQ(RollingStock::cargoVariant(RollingStock::cargoHash("KLB", 3), 3), 2u);
  CHECK_EQ(RollingStock::cargoVariant(12345u, 1), 0u); CHECK_EQ(RollingStock::cargoVariant(12345u, 0), 0u);
})
