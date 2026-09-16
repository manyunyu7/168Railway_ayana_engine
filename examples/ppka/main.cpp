// PPKA playable scene. `ppka [map] [HH:MM] [--no-ai]`
#include "examples/ppka/game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace eng;

int main(int argc, char** argv) {
  GameOptions opt;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--no-ai")) opt.ai = false;
    else if (std::strchr(argv[i], ':')) opt.clock = argv[i];
    else opt.map = argv[i];
  }
  Window win;
  if (!win.open(1440, 900, "PPKA")) return 1;
  Game game;
  if (!game.init(win, opt)) return 1;
  double tPrev = win.time();
  int captureAt = std::getenv("ENG_CAPTURE_FRAME") ? std::atoi(std::getenv("ENG_CAPTURE_FRAME")) : 60;
  while (win.isOpen()) {
    win.pollEvents();
    double t = win.time(), dt = t - tPrev; tPrev = t;
    game.frame(win, dt);
    int f; if (game.wantsCapture(f) && f >= captureAt) {
      int w, h; win.framebufferSize(w, h);
      rhi::captureFramebuffer(std::getenv("ENG_CAPTURE"), w, h); std::printf("captured %s\n%s\n", game.debugCamera().c_str(), game.debugRender().c_str()); break;
    }
    win.swapBuffers();
  }
  game.shutdown(); win.close();
  return 0;
}
