#include "engine/world/city_visual.h"
#include "tests/check.h"
#include <filesystem>
#include <cmath>
#include <fstream>
#include <string>
using namespace eng;
int main() {
  const auto path = std::filesystem::temp_directory_path()/"ayana-test-city-kit.json";
  AssetCatalog catalog; TrackGraph graph; CityVisuals city; WorldOrigin origin;
  auto write = [&](const std::string& text) { std::ofstream out(path); out << text; };
  auto ground = [](double,double) { return 0.f; };
  write(R"({"bangunan":[{"r":[0,0,1,0,1,1,0,1]}]})");
  CHECK(!city.build(path.string(),origin,graph,ground,catalog));
  CHECK(!city.stats.loaded && city.stats.buildings == 0);
  write(R"({"kit":{"versi":1,"pustaka":"kota-kit","instansi":[{"node":"SM_Kota_rumah","x":100,"y":200,"rot":90,"skala":1,"lebar":10,"dalam":8,"tinggi":6},{"node":"rusak","x":100,"y":200,"rot":0,"skala":-1,"lebar":10,"dalam":8,"tinggi":6}]}})");
  CHECK(city.build(path.string(),origin,graph,ground,catalog));
  CHECK(city.stats.loaded && city.stats.buildings == 1 && city.stats.chunks == 1);
  city.destroy(); city.destroy(); CHECK(city.stats.buildings == 0 && !city.stats.loaded);
  write(R"({"kit":{"versi":99,"pustaka":"kota-kit","instansi":[]}})");
  CHECK(!city.build(path.string(),origin,graph,ground,catalog));
  // Data tanah OSM (jejak / hijau / pohon eksplisit) → VegetationOsm. Origin = titik `asal` supaya scene xz kecil.
  { double ox, oz; city::lonLatToWorld(107.0, -6.3, ox, oz); origin.ox = ox; origin.oz = oz; }
  // bangunan ~10×10 m di (100 m, 50 m) timur-selatan asal: 1e-6° lon ≈ 0,111 m, y dibalik → lat turun = z naik
  write(R"({"asal":[107.0,-6.3],
    "bangunan":[{"k":"yes","h":0,"r":[900,-450,90,0,0,-90,-90,0]}],
    "hijau":[{"k":"park","r":[3000,0,3000,0,0,-3000,-3000,0]}],
    "pohon":[10000,0],
    "baris":[{"k":"tree_row","r":[20000,0,144,0]}],
    "kit":{"versi":1,"pustaka":"kota-kit","instansi":[{"node":"SM_Kota_rumah","x":)" + std::to_string(origin.ox + 500) + R"(,"y":)" + std::to_string(origin.oz + 500) + R"(,"rot":0,"skala":1,"lebar":10,"dalam":8,"tinggi":6}]}})");
  CHECK(city.build(path.string(),origin,graph,ground,catalog));
  CHECK(city.stats.footprints == 1 && city.stats.green == 1);
  const VegetationOsm& osm = city;
  CHECK(osm.insideBuilding(105.f, 55.f));       // di tengah poligon OSM
  CHECK(!osm.insideBuilding(95.f, 55.f));       // 5 m di barat tapak
  CHECK(osm.insideBuilding(502.f, 502.f));      // persegi instansi kit ikut menghalangi
  CHECK(!osm.insideBuilding(520.f, 502.f));
  CHECK(osm.insideGreen(400.f, 150.f) && !osm.insideGreen(400.f, -10.f));
  // pohon: 1 titik + deret 16 m / 8 m = 2 pohon
  CHECK(osm.fixedTrees().size() == 3 && city.stats.fixedTrees == 3);
  CHECK(std::abs(osm.fixedTrees()[0].x - 1113.2f) < 2 && std::abs(osm.fixedTrees()[0].y) < 1);
  city.destroy();
  CHECK(osm.fixedTrees().empty() && !osm.insideBuilding(105.f, 55.f) && !osm.insideGreen(400.f, 150.f));
  std::filesystem::remove(path);
  return test::failures ? 1 : 0;
}
