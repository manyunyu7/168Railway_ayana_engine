#include "engine/world/city_visual.h"
#include "tests/check.h"
#include <filesystem>
#include <fstream>
using namespace eng;
int main() {
  const auto path = std::filesystem::temp_directory_path()/"ayana-test-city-kit.json";
  AssetCatalog catalog; TrackGraph graph; CityVisuals city; WorldOrigin origin;
  auto write = [&](const char* text) { std::ofstream out(path); out << text; };
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
  std::filesystem::remove(path);
  return test::failures ? 1 : 0;
}
