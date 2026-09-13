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
  std::snprintf(top, sizeof top, "%s   x%.0f%s   score %d   viol %d   pending %d   fps %.0f   draws %u   az %03d",
                clockText(st.clock).c_str(), timeScale_, paused_ ? " PAUSED" : "", st.score, st.violations, st.pending, fps_, renderer_.drawCalls, compass_.azimuth(orbit_));
  text_.rect(0, 0, (float)w, lh + pad, panel);
  text_.draw(top, pad, pad * 0.5f, white, 0.8f);

  // train list (right)
  float x = (float)w - 430, y = lh + pad * 2;
  text_.rect(x - pad, y - pad * 0.5f, 430, lh * ((float)st.trains.size() + 1) + pad, panel);
  text_.draw("Trains", x, y, dim, 0.8f); y += lh;
  for (const SimTrain& t : st.trains) {
    char line[160];
    std::snprintf(line, sizeof line, "%-6.6s %-14.14s %4.0f km/h %-5.5s %s", t.no.c_str(), t.name.c_str(), t.speed * 3.6f, t.state.c_str(), t.hold.c_str());
    text_.draw(line, x, y, t.hold.empty() ? white : warn, 0.7f);
    y += lh;
  }

  // train labels projected from 3D
  for (const TrainLabel& l : trains_.labels()) {
    vec4 c = viewProj_ * vec4(l.anchor, 1);
    if (c.w <= 0) continue;
    float sx = (c.x / c.w * 0.5f + 0.5f) * (float)w, sy = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
    if (sx < 0 || sx > w || sy < 0 || sy > h) continue;
    char lab[96]; std::snprintf(lab, sizeof lab, "KA %s %s  %.0f", l.no.c_str(), l.name.c_str(), l.speed * 3.6f);
    float tw = text_.measure(lab, 0.7f);
    text_.rect(sx - tw / 2 - 4, sy - lh, tw + 8, lh, {0.05f, 0.2f, 0.4f, 0.75f});
    text_.draw(lab, sx - tw / 2, sy - lh + 2, white, 0.7f);
  }

  // hover tooltip above the picked signal/point
  if (!hoverId_.empty() && !hoverTip_.empty() && hoverX_ >= 0) {
    float tw = text_.measure(hoverTip_, 0.75f);
    text_.rect(hoverX_ - tw / 2 - 6, hoverY_ - lh - 4, tw + 12, lh + 6, hoverReject_ ? vec4{0.45f, 0.08f, 0.08f, 0.85f} : vec4{0.05f, 0.25f, 0.45f, 0.85f});
    text_.draw(hoverTip_, hoverX_ - tw / 2, hoverY_ - lh - 1, white, 0.75f);
  }

  // messages (bottom-left)
  float my = (float)h - pad - lh * (float)messages_.size();
  text_.rect(0, my - pad * 0.5f, 700, (float)h - my + pad, panel);
  for (const std::string& m : messages_) { text_.draw(m, pad, my, white, 0.8f); my += lh; }

  // help (bottom-right)
  text_.draw("LMB orbit | RMB tap = fly focus there, hold = glide | Ctrl+arrows nudge | F fly cam | click signal/point | space pause | +/- speed",
             (float)w - 760, (float)h - lh - pad * 0.5f, dim, 0.65f);
  text_.flush(w, h);
}

void Game::pushMessage(const std::string& s) {
  messages_.push_back(s);
  while (messages_.size() > 8) messages_.pop_front();
}

} // namespace eng
