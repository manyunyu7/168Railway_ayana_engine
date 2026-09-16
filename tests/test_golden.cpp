// Golden image: runs the viewer on tests/golden/coupling.emod with ENG_CAPTURE and compares with
// tests/golden/coupling.ppm (thresholds and mechanics in tests/golden_util.h).
//   test_golden --update   regenerates the golden from the current build.
// Needs a window/GPU: labelled `gpu` in ctest and excluded by the `mac-debug` preset (run with `ctest --preset mac-debug-gpu`).
#include "tests/golden_util.h"
#include <cstring>

int main(int argc, char** argv) {
  bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
  const std::string src = ENG_SOURCE_DIR, bin = ENG_BINARY_DIR;
  golden::Img got;
  if (int rc = golden::capture(src, bin, "tests/golden/coupling.emod", got)) return rc;
  int rc = golden::compare(got, src + "/tests/golden/coupling.ppm", bin, "test_golden", update);
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return rc;
}
