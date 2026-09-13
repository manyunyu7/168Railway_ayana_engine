// Golden image: runs the viewer on tests/golden/coupling.emod with ENG_CAPTURE, box-downsamples the
// capture to 320 px wide (independent of the retina scale) and compares with tests/golden/coupling.ppm.
// Pass: mean |diff| < 2/255 and < 0.5 % of pixels differ by more than 16 in any channel.
//   test_golden --update   regenerates the golden from the current build.
// Needs a window/GPU: labelled `gpu` in ctest and excluded by the `mac-debug` preset (run with `ctest --preset mac-debug-gpu`).
#include "tests/check.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct Img { int w = 0, h = 0; std::vector<unsigned char> px; };   // RGB8

bool readPpm(const std::string& path, Img& img) {
  std::ifstream f(path, std::ios::binary); if (!f) return false;
  std::string magic; int maxv = 0; f >> magic >> img.w >> img.h >> maxv; f.get();
  if (magic != "P6" || maxv != 255 || img.w <= 0 || img.h <= 0) return false;
  img.px.resize((size_t)img.w * img.h * 3); f.read((char*)img.px.data(), (std::streamsize)img.px.size());
  return (size_t)f.gcount() == img.px.size();
}
bool writePpm(const std::string& path, const Img& img) {
  std::ofstream f(path, std::ios::binary); if (!f) return false;
  f << "P6\n" << img.w << " " << img.h << "\n255\n"; f.write((const char*)img.px.data(), (std::streamsize)img.px.size());
  return (bool)f;
}
Img downsample(const Img& in, int targetW) {
  int k = in.w / targetW; if (k < 1) k = 1;
  Img out; out.w = in.w / k; out.h = in.h / k; out.px.resize((size_t)out.w * out.h * 3);
  for (int y = 0; y < out.h; ++y) for (int x = 0; x < out.w; ++x) for (int c = 0; c < 3; ++c) {
    unsigned sum = 0;
    for (int dy = 0; dy < k; ++dy) for (int dx = 0; dx < k; ++dx) sum += in.px[((size_t)(y * k + dy) * in.w + (x * k + dx)) * 3 + c];
    out.px[((size_t)y * out.w + x) * 3 + c] = (unsigned char)((sum + (unsigned)(k * k / 2)) / (unsigned)(k * k));
  }
  return out;
}
}

int main(int argc, char** argv) {
  bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
  const std::string src = ENG_SOURCE_DIR, bin = ENG_BINARY_DIR;
  const std::string model = "tests/golden/coupling.emod", golden = src + "/tests/golden/coupling.ppm";
  const std::string capture = bin + "/golden_capture.ppm";
  std::remove(capture.c_str());
  // cwd is the source dir (ctest WORKING_DIRECTORY) so the HUD path text and assets/font.efnt are stable
  std::string cmd = "cd '" + src + "' && ENG_CAPTURE='" + capture + "' '" + bin + "/viewer' " + model + " >/dev/null";
  int rc = std::system(cmd.c_str());
  if (rc != 0) { std::printf("SKIP: viewer could not run (no window/GPU?), exit %d\n", rc); return test::SKIP; }
  Img raw; CHECK_MSG(readPpm(capture, raw), "capture missing: " + capture);
  if (raw.w == 0) return 1;
  std::remove(capture.c_str());
  Img got = downsample(raw, 320);
  std::printf("captured %dx%d -> %dx%d\n", raw.w, raw.h, got.w, got.h);
  if (update) { CHECK(writePpm(golden, got)); std::printf("golden updated: %s\n", golden.c_str()); return test::failures ? 1 : 0; }

  Img ref; CHECK_MSG(readPpm(golden, ref), "golden missing: " + golden + " (run test_golden --update)");
  if (ref.w == 0) return 1;
  CHECK(ref.w == got.w && ref.h == got.h);
  if (ref.w != got.w || ref.h != got.h) return 1;
  double sum = 0; size_t bad = 0, n = (size_t)got.w * got.h;
  Img diff = got;
  for (size_t i = 0; i < n; ++i) {
    int worst = 0;
    for (int c = 0; c < 3; ++c) { int d = std::abs((int)got.px[i * 3 + c] - (int)ref.px[i * 3 + c]); sum += d; worst = std::max(worst, d); diff.px[i * 3 + c] = (unsigned char)std::min(255, d * 4); }
    if (worst > 16) ++bad;
  }
  double mean = sum / (double)(n * 3) / 255.0, badFrac = (double)bad / (double)n;
  std::printf("mean |diff| = %.4f (limit 0.0078), pixels off by >16: %.3f %% (limit 0.5 %%)\n", mean, badFrac * 100);
  CHECK(mean < 2.0 / 255); CHECK(badFrac < 0.005);
  if (test::failures) { writePpm(bin + "/golden_actual.ppm", got); writePpm(bin + "/golden_diff.ppm", diff); std::printf("wrote %s/golden_actual.ppm and golden_diff.ppm\n", bin.c_str()); }
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures);
  return test::failures ? 1 : 0;
}
