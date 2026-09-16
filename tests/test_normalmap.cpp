// Golden image for the normal map + ambient occlusion path of the PBR shader: a grey cube whose material
// carries a synthetic tangent-space normal map (a 4x4 grid of hemispherical bumps) and an occlusion map
// (dark grid lines with a vignette). The model is built here, saved as EMOD next to the build and rendered
// by the viewer with ENG_CAPTURE; compare with tests/golden/normalmap.ppm (mechanics in tests/golden_util.h).
// Without vertex tangents the shader derives the frame from screen-space derivatives, so every face of the
// cube must show the bumps lit from the sun side (top-left of each bump bright on the +Z face).
//   test_normalmap --update   regenerates the golden from the current build.
// Needs a window/GPU: labelled `gpu` in ctest.
#include "tests/golden_util.h"
#include "engine/asset/emod.h"
#include "tests/synth_model.h"
#include <cmath>
#include <cstring>

using namespace eng;

int main(int argc, char** argv) {
  bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
  const std::string src = ENG_SOURCE_DIR, bin = ENG_BINARY_DIR;

  Model m = synth::bumpyCube();
  const std::string emod = bin + "/normalmap.emod";
  std::string err; CHECK_MSG(saveEmod(m, emod, err), err);

  // the material texture slots survive the round trip and stay linear
  Model back; std::ifstream f(emod, std::ios::binary); std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  CHECK_MSG(loadEmod(bytes, back, err), err);
  CHECK(back.materials.size() == 1 && back.materials[0].normalTex == 0 && back.materials[0].occlusionTex == 1);
  CHECK(back.images.size() == 2 && back.images[0].linear && back.images[1].linear);

  golden::Img got;
  if (int rc = golden::capture(src, bin, emod, got)) return rc;
  int rc = golden::compare(got, src + "/tests/golden/normalmap.ppm", bin, "test_normalmap", update);
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return rc;
}
