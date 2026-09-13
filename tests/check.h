// Tiny test harness: CHECK / CHECK_NEAR record failures with file:line; TEST_MAIN counts them.
// A test is an executable that returns 0 on success, 1 on failure, 77 when it must be skipped.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

namespace test {
inline int failures = 0, checks = 0;
inline void fail(const char* file, int line, const std::string& what) {
  ++failures; std::fprintf(stderr, "%s:%d: FAIL %s\n", file, line, what.c_str());
}
inline bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }
constexpr int SKIP = 77;
}

#define CHECK(cond) do { ++test::checks; if (!(cond)) test::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_MSG(cond, msg) do { ++test::checks; if (!(cond)) test::fail(__FILE__, __LINE__, std::string(#cond) + " (" + (msg) + ")"); } while (0)
#define CHECK_NEAR(a, b, eps) do { ++test::checks; double _a = (double)(a), _b = (double)(b); \
  if (!test::near(_a, _b, (eps))) test::fail(__FILE__, __LINE__, std::string(#a " ~ " #b ": ") + std::to_string(_a) + " vs " + std::to_string(_b)); } while (0)
#define CHECK_EQ(a, b) do { ++test::checks; if (!((a) == (b))) test::fail(__FILE__, __LINE__, #a " == " #b); } while (0)

// Body runs inside main; returns the process exit code.
#define TEST_MAIN(body) int main() { body; \
  std::printf("%s: %d checks, %d failures\n", __FILE__, test::checks, test::failures); \
  return test::failures ? 1 : 0; }
