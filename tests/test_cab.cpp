// Cab view + interior switch helpers (engine/world/rolling_stock.h; the old keretaVisual3d.ts sisiTunggal /
// sakelarInterior, dunia3dKonst.ts KACA_KABIN / KULIT_DUA_SISI): pure material / node-pose rules, no GPU.
#include "engine/world/rolling_stock.h"
#include "tests/check.h"

using namespace eng;

TEST_MAIN({
  // interior names: any case, anywhere in the name
  CHECK(isInteriorName("SM_CC206_Interior.005"));
  CHECK(isInteriorName("interior_kabin"));
  CHECK(!isInteriorName("SM_CC206_Cab"));
  CHECK(!isInteriorName(""));
  // cab materials, pilot loco: everything double-sided in the file -> single-sided from inside; no glass rule
  std::vector<Material> asli(2);
  asli[0].name = "Body"; asli[0].doubleSided = true;
  asli[1].name = "CL-Glass.001"; asli[1].doubleSided = true;
  auto cab = cabMaterialsFor("loko206", asli);
  CHECK_EQ(cab.size(), 2u);
  CHECK(!cab[0].doubleSided && !cab[1].doubleSided);
  CHECK(cab[1].alphaMode == AlphaMode::Opaque && cab[1].baseColor.w == 1);
  // JR 205 low-poly: skin STAYS double-sided (one-layer skin), `CL-Glass*` blended at 0.12
  cab = cabMaterialsFor("nryJr205KuhaBadan", asli);
  CHECK(cab[0].doubleSided && cab[0].alphaMode == AlphaMode::Opaque);
  CHECK(cab[1].doubleSided && cab[1].alphaMode == AlphaMode::Blend);
  CHECK_NEAR(cab[1].baseColor.w, RollingStock::KACA_OPASITAS, 1e-6);
  // the sarana alias the bridge sends resolves to the same slot
  auto viaAlias = cabMaterialsFor("krl-kuha", asli);
  CHECK(viaAlias[1].alphaMode == AlphaMode::Blend && viaAlias[0].doubleSided);
  // the regex is anchored: "XCL-Glass" is not glass
  asli[1].name = "XCL-Glass"; cab = cabMaterialsFor("nryJr205KuhaBadan", asli);
  CHECK(cab[1].alphaMode == AlphaMode::Opaque);
  // the originals are untouched
  CHECK(asli[0].doubleSided);
  // collapsing interior nodes: only the listed nodes lose their scale, the translation stays
  VehicleProto p; p.interiorNodes = {1};
  std::vector<mat4> pose(2, mat4::translation({1, 2, 3}));
  RollingStock::sembunyikanInterior(p, pose);
  CHECK_NEAR(pose[0].m[0][0], 1, 1e-6);
  CHECK_NEAR(pose[1].m[0][0], 0, 1e-6); CHECK_NEAR(pose[1].m[1][1], 0, 1e-6);
  vec3 t = pose[1].transformPoint({5, 5, 5}); CHECK_NEAR(t.x, 1, 1e-6); CHECK_NEAR(t.y, 2, 1e-6);
})
