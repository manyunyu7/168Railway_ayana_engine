#include "examples/ppka/game.h"
#include <cmath>
#include <cstdio>

namespace eng {

static std::string clockText(double clock) {
  int s = (int)clock % 86400; char b[16];
  std::snprintf(b, sizeof b, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
  return b;
}

static std::string hm(double t) { int s = (int)t % 86400; char b[8]; std::snprintf(b, sizeof b, "%02d:%02d", s / 3600, (s / 60) % 60); return b; }

void Game::drawHud(int w, int h) {
  const float pad = 10, lh = text_.lineHeight(0.8f);
  const vec4 panel{0, 0, 0, 0.55f}, white{1, 1, 1, 1}, dim{0.75f, 0.78f, 0.82f, 1};
  const SimState& st = sim_.state();
  ui_.begin(text_, fmx_, fmy_, pendingClick_, pcx_, pcy_);

  // top bar: clock, time scale, score, fps
  char top[200];
  std::snprintf(top, sizeof top, "%s   x%g%s   score %d   viol %d   pending %d   fps %.0f   draws %u   az %03d   %s",
                clockText(st.clock).c_str(), timeScale_, paused_ ? " PAUSED" : "", st.score, st.violations, st.pending, fps_, scene_.renderer().drawCalls, compass_.azimuth(orbit_), pemula_ ? "PEMULA" : "AHLI");
  ui_.panel({0, 0, (float)w, lh * 2 + pad * 1.5f}, panel);
  text_.draw(top, pad, pad * 0.5f, white, 0.8f);

  // train labels projected from 3D
  for (const TrainLabel& l : scene_.trains().labels()) {
    vec4 c = viewProj_ * vec4(l.anchor, 1);
    if (c.w <= 0) continue;
    float sx = (c.x / c.w * 0.5f + 0.5f) * (float)w, sy = (1 - (c.y / c.w * 0.5f + 0.5f)) * (float)h;
    if (sx < 0 || sx > w || sy < 0 || sy > h) continue;
    char lab[96]; std::snprintf(lab, sizeof lab, "KA %s %s  %.0f", l.no.c_str(), l.name.c_str(), l.speed * 3.6f);
    float tw = text_.measure(lab, 0.7f);
    UiRect r{sx - tw / 2 - 4, sy - lh, tw + 8, lh};
    bool sel = l.id == selectedTrain_;
    text_.rect(r.x, r.y, r.w, r.h, sel ? vec4{0.05f, 0.4f, 0.6f, 0.9f} : vec4{0.05f, 0.2f, 0.4f, 0.75f});
    text_.draw(lab, sx - tw / 2, sy - lh + 2, white, 0.7f);
    if (ui_.clickIn(r)) selectTrain(l.id, false);   // label click = select (main.ts hooks.pilihKA)
  }

  // meja layan (bottom overlay) — before the widgets that may sit on top of it
  panel_.draw(ui_, st, w, h, hoverOnPanel_ ? hoverId_ : "", menu_.open ? menu_.lit : std::vector<std::string>{}, selectedTrain_);

  drawUi(w, h);
  drawIzinCard(w, h);

  // hover tooltip above the picked signal/point (3D or panel)
  if (!hoverId_.empty() && !hoverTip_.empty() && !menu_.open) {
    float hx = hoverX_, hy = hoverY_;
    if (hoverOnPanel_) { if (!panel_.screenPos(hoverId_, hoverIsSignal_, w, h, hx, hy)) hx = -1; hy -= 14; }
    if (hx >= 0) {
      float tw = text_.measure(hoverTip_, 0.75f);
      text_.rect(hx - tw / 2 - 6, hy - lh - 4, tw + 12, lh + 6, hoverReject_ ? vec4{0.45f, 0.08f, 0.08f, 0.85f} : vec4{0.05f, 0.25f, 0.45f, 0.85f});
      text_.draw(hoverTip_, hx - tw / 2, hy - lh - 1, white, 0.75f);
    }
  }

  // messages (bottom-left, above the panel when it is open)
  float bottom = panel_.visible ? (float)h - panel_.height : (float)h;
  float my = bottom - pad - lh * (float)messages_.size();
  text_.rect(0, my - pad * 0.5f, 700, bottom - my + pad, panel);
  for (const std::string& m : messages_) { text_.draw(m, pad, my, white, 0.8f); my += lh; }

  // help (bottom-right)
  text_.draw("LMB orbit | RMB tap = fly focus, hold = glide | F fly | 1-6 kamera | , . KA | Z teropong | click signal/point | space pause | +/- speed | M meja | P mode | J jam",
             (float)w - 900, bottom - lh - pad * 0.5f, dim, 0.65f);
  text_.flush(w, h);
}

// Top-bar buttons, train list + detail card, beginner route menu, clock prompt (all immediate mode).
void Game::drawUi(int w, int h) {
  const float pad = 10, lh = text_.lineHeight(0.8f), bh = lh * 0.95f;
  const vec4 panel{0, 0, 0, 0.55f}, white{1, 1, 1, 1}, dim{0.75f, 0.78f, 0.82f, 1}, warn{1, 0.8f, 0.3f, 1}, bad{1, 0.45f, 0.4f, 1}, good{0.4f, 1, 0.5f, 1};
  const SimState& st = sim_.state();

  // --- top bar row 2: pause, speeds, jam, mode, meja ---
  float x = pad, y = lh + pad * 0.75f;
  if (ui_.button({x, y, 64, bh}, paused_ ? "> play" : "|| pause", paused_)) setPaused(!paused_); x += 70;
  static const double SCALES[] = {0.5, 1, 2, 4, 8, 16, 32, 64};
  for (double k : SCALES) { char b[8]; std::snprintf(b, sizeof b, "x%g", k); float bw = 44; if (ui_.button({x, y, bw, bh}, b, timeScale_ == k)) setTimeScale(k); x += bw + 4; }
  x += 8;
  if (ui_.button({x, y, 96, bh}, "jam " + hm(st.clock), clockPrompt_)) { clockPrompt_ = !clockPrompt_; clockText_.clear(); } x += 102;
  if (ui_.button({x, y, 76, bh}, "pemula", pemula_)) { pemula_ = true; menu_.open = false; } x += 80;
  if (ui_.button({x, y, 60, bh}, "ahli", !pemula_)) { pemula_ = false; menu_.open = false; } x += 72;
  if (ui_.button({x, y, 90, bh}, "meja layan", panel_.visible)) { panel_.visible = !panel_.visible; if (panel_.visible) panel_.fitStation("", w, h); } x += 96;
  if (ui_.button({x, y, 60, bh}, "fly", useFly_ && rig_.mode == CamMode::Bebas, rig_.mode == CamMode::Bebas)) { useFly_ = !useFly_; if (useFly_) { fly_.position = orbit_.position(); vec3 d = normalize(orbit_.target - fly_.position); fly_.yaw = std::atan2(-d.x, -d.z); fly_.pitch = std::asin(d.y); } } x += 72;
  // camera modes (§9.1): 1 bebas .. 6 ekor; Z = telescope (hold) / click = lock
  for (int i = 0; i < 6; ++i) { CamMode m = (CamMode)i; float bw = 64; if (ui_.button({x, y, bw, bh}, camModeName(m), rig_.mode == m)) setCamMode(m); x += bw + 4; }
  if (ui_.button({x, y, 30, bh}, "Z", rig_.teropongKunci || rig_.teropongTahan, rig_.bolehTeropong())) rig_.teropongKunci = !rig_.teropongKunci;

  // --- clock prompt (modal-ish box under the top bar) ---
  if (clockPrompt_) {
    UiRect r{(float)w / 2 - 170, lh * 2 + pad * 2, 340, lh * 3 + pad};
    ui_.panel(r, {0.08f, 0.1f, 0.14f, 0.96f});
    text_.draw("Set jam (HH:MM) — session restarts from the GAPEKA at that time", r.x + pad, r.y + pad * 0.5f, dim, 0.65f);
    std::string shown = clockText_ + "_"; text_.draw(shown, r.x + pad, r.y + lh + pad * 0.5f, white, 1.1f);
    if (ui_.button({r.x + r.w - 150, r.y + lh + pad, 66, bh}, "OK", false, clockText_.size() == 5)) { setClock(clockText_); clockPrompt_ = false; }
    if (ui_.button({r.x + r.w - 76, r.y + lh + pad, 66, bh}, "batal")) clockPrompt_ = false;
  }

  // --- train list (right) ---
  x = (float)w - 430; y = lh * 2 + pad * 2.5f;
  float rowH = lh * 0.9f;
  ui_.panel({x - pad, y - pad * 0.5f, 430, rowH * ((float)st.trains.size() + 1) + pad}, panel);
  text_.draw("Trains  (click = select, camera jumps)", x, y, dim, 0.7f); y += rowH;
  for (const SimTrain& t : st.trains) {
    char line[160];
    std::snprintf(line, sizeof line, "%-5.5s %-13.13s %4.0f km/h %-5.5s %s%.18s", t.no.c_str(), t.name.c_str(), t.speed * 3.6f, t.state.c_str(), t.tungguS40 ? "[S40] " : "", t.hold.c_str());
    if (ui_.row({x - 4, y, 420, rowH}, line, t.tungguS40 ? warn : t.hold.empty() ? white : warn, t.id == selectedTrain_)) selectTrain(t.id, true);
    y += rowH;
  }

  // --- detail card (rincianKA) under the list ---
  if (!selectedTrain_.empty() && detail_["ok"].boolOr(false)) {
    const Json& d = detail_;
    y += pad;
    float cardH = lh * 7 + rowH * (float)d["jadwal"].size() + pad * 2;
    UiRect card{x - pad, y, 430, cardH};
    ui_.panel(card, {0.03f, 0.08f, 0.14f, 0.85f});
    float cy = y + pad * 0.5f;
    std::string head = "KA " + d["no"].stringOr("") + "  " + d["name"].stringOr("");
    text_.draw(head, x, cy, white, 0.9f);
    if (ui_.button({x + 380, cy, 28, bh}, "x")) selectedTrain_.clear();
    cy += lh;
    text_.draw(d["consist"].stringOr("") + "  (" + std::to_string(d["cars"].size()) + " cars, " + d["kendali"].stringOr("") + ")", x, cy, dim, 0.65f); cy += lh * 0.85f;
    int delay = d["delay"].intOr(0);
    char st1[160]; std::snprintf(st1, sizeof st1, "%s  %.0f / %.0f km/h   %s%s", d["state"].stringOr("").c_str(), d["speed"].numberOr(0) * 3.6, d["limit"].numberOr(0) * 3.6,
                                 delay <= 60 ? "tepat" : ("+" + std::to_string(delay / 60) + " min").c_str(), d["hold"].isString() ? ("   hold: " + d["hold"].stringOr("")).c_str() : "");
    text_.draw(st1, x, cy, delay <= 60 ? good : delay <= 300 ? warn : bad, 0.7f); cy += lh * 0.9f;
    if (d["nextStop"].isObject()) { text_.draw("next: " + d["nextStop"]["trackmark"].stringOr("") + "  " + hm(d["nextStop"]["arr"].numberOr(0)) + " - " + hm(d["nextStop"]["dep"].numberOr(0)), x, cy, dim, 0.7f); }
    else if (d["exitPortal"].isString()) text_.draw("next: keluar " + d["exitPortal"].stringOr("") + "  " + hm(d["exitTime"].numberOr(0)), x, cy, dim, 0.7f);
    cy += lh * 0.9f;
    text_.draw("     stasiun               GAPEKA               realisasi", x, cy, dim, 0.6f); cy += rowH * 0.9f;
    for (const Json& it : d["jadwal"].arr) {
      std::string cls = it["cls"].stringOr("");
      char ln[200]; std::snprintf(ln, sizeof ln, "%s %-20.20s %-20.20s %s", cls == "done" ? "v" : cls == "now" ? ">" : "o", it["name"].stringOr("").c_str(), it["plan"].stringOr("").c_str(), it["actual"].stringOr("").c_str());
      text_.draw(ln, x, cy, it["late"].boolOr(false) ? bad : cls == "now" ? white : dim, 0.62f); cy += rowH * 0.85f;
    }
    cy += pad * 0.5f;
    bool s40 = d["s40Siap"].boolOr(false), wait = d["tungguS40"].boolOr(false);
    if (wait) { if (ui_.button({x, cy, 130, bh}, s40 ? "Beri S40" : "S40 (sinyal merah)", false, s40)) giveS40(selectedTrain_); }
    if (!confirmHapus_) { if (ui_.button({x + 300, cy, 100, bh}, "Hapus KA")) confirmHapus_ = true; }
    else {
      text_.draw("-2000, sure?", x + 150, cy + 2, bad, 0.7f);
      if (ui_.button({x + 250, cy, 70, bh}, "ya")) removeTrain(selectedTrain_);
      if (ui_.button({x + 330, cy, 70, bh}, "batal")) confirmHapus_ = false;
    }
  }

  // --- beginner route menu (destinations from route_menu) ---
  if (menu_.open) {
    const Json& cands = menu_.data["candidates"];
    float mw = 380, mh = lh * 1.3f + rowH * ((float)cands.size() + 0.4f) + pad;
    float mx = std::fmin(std::fmax(menu_.x, 4.f), (float)w - mw - 4), my = std::fmin(std::fmax(menu_.y - mh - 12, 4.f), (float)h - mh - 4);
    UiRect r{mx, my, mw, mh};
    ui_.panel(r, {0.06f, 0.1f, 0.16f, 0.96f});
    text_.draw("Rute dari " + menu_.name + "   (1-" + std::to_string(cands.size()) + ", Esc)", r.x + pad * 0.5f, r.y + pad * 0.4f, white, 0.75f);
    float cy = r.y + lh * 1.2f; int i = 0;
    menu_.lit.clear();
    for (const Json& c : cands.arr) {
      bool blocked = c["blocked"].isObject(), ss = c["sepurSalah"].boolOr(false);
      char ln[200]; std::snprintf(ln, sizeof ln, "%d  %s%s  %.0f m%s", i + 1, ss ? "! " : "", c["exitLabel"].stringOr("").c_str(), c["dist"].numberOr(0),
                                   blocked ? ("  - " + c["blockedText"].stringOr("")).c_str() : "");
      UiRect rr{r.x + 4, cy, mw - 8, rowH};
      if (ui_.hot(rr)) for (const Json& sg : c["segs"].arr) menu_.lit.push_back(sg.stringOr(""));
      if (ui_.row(rr, ln, blocked ? bad : ss ? warn : white, false)) chooseRoute(i);
      cy += rowH; ++i;
    }
  }
}

// The HUD font has no arrows/dashes: map the web wording's U+2192 / U+2014 to ASCII.
static std::string ascii(std::string t) {
  for (const auto& [from, to] : {std::pair<const char*, const char*>{"\xe2\x86\x92", "->"}, {"\xe2\x80\x94", "-"}})
    for (size_t p; (p = t.find(from)) != std::string::npos;) t.replace(p, 3, to);
  return t;
}

// Permission card (ui/blokBar.ts tawarkanIzin): title, route, consequence, limits, [tombol] [Batal], countdown bar.
void Game::drawIzinCard(int w, int h) {
  if (!izin_.open) return;
  double now = win_->time();
  if (now >= izin_.until) { izin_.open = false; return; }
  const float pad = 10, lh = text_.lineHeight(0.7f), bh = text_.lineHeight(0.8f) * 0.95f, cw = 560, sc = 0.68f;
  auto wrap = [&](const std::string& t) {   // greedy word wrap to the card width
    std::vector<std::string> lines; std::string line, word;
    auto flush = [&]() { if (!line.empty()) lines.push_back(line); line.clear(); };
    for (size_t i = 0; i <= t.size(); ++i) {
      if (i == t.size() || t[i] == ' ') {
        std::string cand = line.empty() ? word : line + " " + word;
        if (text_.measure(cand, sc) > cw - pad * 2 && !line.empty()) { flush(); line = word; } else line = cand;
        word.clear();
      } else word += t[i];
    }
    flush(); return lines;
  };
  std::vector<std::string> akibat = wrap(ascii(izin_.akibat)), batas = wrap(ascii(izin_.batas));
  float ch = lh * (2.6f + (float)akibat.size() + (float)batas.size()) + bh + pad * 3;
  float bottom = panel_.visible ? (float)h - panel_.height : (float)h;
  UiRect r{(float)w / 2 - cw / 2, bottom - ch - pad - text_.lineHeight(0.8f) * 9, cw, ch};   // above the message log
  ui_.panel(r, {0.16f, 0.09f, 0.04f, 0.96f});
  text_.rect(r.x, r.y, r.w, 3, {1, 0.7f, 0.2f, 1});
  float cy = r.y + pad * 0.8f;
  text_.draw("! " + izin_.judul, r.x + pad, cy, {1, 0.85f, 0.4f, 1}, 0.85f); cy += lh * 1.3f;
  text_.draw(ascii(izin_.rute), r.x + pad, cy, {1, 1, 1, 1}, 0.75f); cy += lh * 1.2f;
  for (const std::string& l : akibat) { text_.draw(l, r.x + pad, cy, {0.92f, 0.92f, 0.9f, 1}, sc); cy += lh; }
  cy += lh * 0.1f;
  for (const std::string& l : batas) { text_.draw(l, r.x + pad, cy, {0.8f, 0.75f, 0.65f, 1}, sc); cy += lh; }
  cy += pad * 0.6f;
  float bw = std::fmax(120.f, text_.measure(izin_.tombol, 0.7f) + 24);
  if (ui_.button({r.x + pad, cy, bw, bh}, izin_.tombol)) confirmIzin();
  if (ui_.button({r.x + pad + bw + 8, cy, 70, bh}, "Batal")) izin_.open = false;
  char sisa[16]; std::snprintf(sisa, sizeof sisa, "%d s", (int)std::ceil(izin_.until - now));
  text_.draw(std::string("Enter / Esc   ") + sisa, r.x + r.w - pad - text_.measure(std::string("Enter / Esc   ") + sisa, 0.62f), cy + 4, {0.7f, 0.7f, 0.7f, 1}, 0.62f);
  float frac = (float)((izin_.until - now) / 12.0);
  text_.rect(r.x, r.y + r.h - 4, r.w * frac, 4, {1, 0.7f, 0.2f, 0.9f});
}

void Game::pushMessage(const std::string& s) {
  messages_.push_back(s);
  while (messages_.size() > 8) messages_.pop_front();
}

} // namespace eng
