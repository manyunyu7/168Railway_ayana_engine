// Golden-image helpers shared by the GPU tests: run the viewer with ENG_CAPTURE, box-downsample the capture
// to 320 px wide (independent of the retina scale) and compare with a stored PPM.
// Pass: mean |diff| < 2/255 and < 0.5 % of pixels differ by more than 16 in any channel.
#pragma once
#include "tests/check.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace golden {
struct Img { int w = 0, h = 0; std::vector<unsigned char> px; };   // RGB8

inline bool readPpm(const std::string& path, Img& img) {
  std::ifstream f(path, std::ios::binary); if (!f) return false;
  std::string magic; int maxv = 0; f >> magic >> img.w >> img.h >> maxv; f.get();
  if (magic != "P6" || maxv != 255 || img.w <= 0 || img.h <= 0) return false;
  img.px.resize((size_t)img.w * img.h * 3); f.read((char*)img.px.data(), (std::streamsize)img.px.size());
  return (size_t)f.gcount() == img.px.size();
}
inline bool writePpm(const std::string& path, const Img& img) {
  std::ofstream f(path, std::ios::binary); if (!f) return false;
  f << "P6\n" << img.w << " " << img.h << "\n255\n"; f.write((const char*)img.px.data(), (std::streamsize)img.px.size());
  return (bool)f;
}
inline Img downsample(const Img& in, int targetW) {
  int k = in.w / targetW; if (k < 1) k = 1;
  Img out; out.w = in.w / k; out.h = in.h / k; out.px.resize((size_t)out.w * out.h * 3);
  for (int y = 0; y < out.h; ++y) for (int x = 0; x < out.w; ++x) for (int c = 0; c < 3; ++c) {
    unsigned sum = 0;
    for (int dy = 0; dy < k; ++dy) for (int dx = 0; dx < k; ++dx) sum += in.px[((size_t)(y * k + dy) * in.w + (x * k + dx)) * 3 + c];
    out.px[((size_t)y * out.w + x) * 3 + c] = (unsigned char)((sum + (unsigned)(k * k / 2)) / (unsigned)(k * k));
  }
  return out;
}

// Runs `bin/viewer model` from `src` (cwd: the HUD path text and assets/font.efnt are stable), captures frame 30.
// Returns test::SKIP when the viewer cannot run (no window/GPU), 1 on a failed capture, 0 with `got` filled.
inline int capture(const std::string& src, const std::string& bin, const std::string& model, Img& got) {
  const std::string cap = bin + "/golden_capture.ppm";
  std::remove(cap.c_str());
  std::string cmd = "cd '" + src + "' && ENG_CAPTURE='" + cap + "' '" + bin + "/viewer' '" + model + "' >/dev/null";
  int rc = std::system(cmd.c_str());
  if (rc != 0) { std::printf("SKIP: viewer could not run (no window/GPU?), exit %d\n", rc); return test::SKIP; }
  Img raw; CHECK_MSG(readPpm(cap, raw), "capture missing: " + cap);
  if (raw.w == 0) return 1;
  std::remove(cap.c_str());
  got = downsample(raw, 320);
  std::printf("captured %dx%d -> %dx%d\n", raw.w, raw.h, got.w, got.h);
  return 0;
}

// --update writes `got` as the golden; otherwise compares and, on failure, writes bin/<name>_actual.ppm and
// <name>_diff.ppm next to the build. Returns the process exit code.
inline int compare(const Img& got, const std::string& goldenPath, const std::string& bin, const std::string& name, bool update) {
  if (update) { CHECK(writePpm(goldenPath, got)); std::printf("golden updated: %s\n", goldenPath.c_str()); return test::failures ? 1 : 0; }
  Img ref; CHECK_MSG(readPpm(goldenPath, ref), "golden missing: " + goldenPath + " (run " + name + " --update)");
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
  if (test::failures) {
    writePpm(bin + "/" + name + "_actual.ppm", got); writePpm(bin + "/" + name + "_diff.ppm", diff);
    std::printf("wrote %s/%s_actual.ppm and %s_diff.ppm\n", bin.c_str(), name.c_str(), name.c_str());
  }
  return test::failures ? 1 : 0;
}
} // namespace golden
