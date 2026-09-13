#include "examples/ppka/game.h"
#include <cstdio>

namespace eng {

static std::string clockText(double clock) {
  int s = (int)clock % 86400; char b[16];
  std::snprintf(b, sizeof b, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  return b;
}

void Game::drawHud(int w, int h) {
  const float pad = 10, lh = text_.lineHeight(0.8f);
  const vec4 panel{0, 0, 0, 0.55f}, white{1, 1, 1, 1}, dim{0.75f, 0.78f, 0.82f, 1}, warn{1, 0.8f, 0.3f, 1};
  const SimState& st = sim_.state();

  // top bar: clock, time scale, score, fps
  char top[200];
  std::snprintf(top, sizeof top, "%s   x%.0f%s   score %d   viol %d   pending %d   fps %.0f   draws %u",
                clockText(st.clock).c_str(), timeScale_, paused_ ? " PAUSED" : "", st.score, st.violations, st.pending, fps_, renderer_.drawCalls);
  text_.rect(0, 0, (float)w, lh + pad, panel);
  text_.draw(top, pad, pad * 0.5f, white, 0.8f);

  // train list (right)
  float x = (float)w - 420, y = lh + pad * 2;
  text_.rect(x - pad, y - pad * 0.5f, 420, lh * ((float)st.trains.size() + 1) + pad, panel);
  text_.draw("Trains", x, y, dim, 0.8f); y += lh;
  for (const SimTrain& t : st.trains) {
    char line[160];
    std::snprintf(line, sizeof line, "%-7s %-16.16s %5.0f km/h %s", t.no.c_str(), t.name.c_str(), t.speed * 3.6f, t.state.c_str());
    text_.draw(line, x, y, t.hold.empty() ? white : warn, 0.8f);
    if (!t.hold.empty()) text_.draw(t.hold, x + 300, y, warn, 0.65f);
    y += lh;
  }

  // messages (bottom-left)
  float my = (float)h - pad - lh * (float)messages_.size();
  text_.rect(0, my - pad * 0.5f, 700, (float)h - my + pad, panel);
  for (const std::string& m : messages_) { text_.draw(m, pad, my, white, 0.8f); my += lh; }

  // help (bottom-right)
  text_.draw("LMB drag orbit | RMB/WASD fly (F) | click signal/point | space pause | +/- speed | Esc quit",
             (float)w - 760, (float)h - lh - pad * 0.5f, dim, 0.65f);
  text_.flush(w, h);
}

void Game::pushMessage(const std::string& s) {
  messages_.push_back(s);
  while (messages_.size() > 8) messages_.pop_front();
}

} // namespace eng
