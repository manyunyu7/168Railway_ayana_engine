// Tiny immediate-mode UI on top of TextRenderer: buttons, rows, and "blocking" rects that keep
// clicks/hover away from the 3D scene. Everything is in framebuffer pixels.
#pragma once
#include "engine/render/text.h"
#include <string>
#include <vector>

namespace eng {

struct UiRect { float x = 0, y = 0, w = 0, h = 0; bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; } };

struct Ui {
  TextRenderer* text = nullptr;
  float mx = 0, my = 0;            // cursor (framebuffer px)
  bool clicked = false;            // a click (press+release without drag) landed this frame
  float cx = 0, cy = 0;            // where the click landed
  bool consumed = false;           // set when a widget took the click
  std::vector<UiRect> blocks;      // areas owned by the UI this frame (3D ignores input there)

  void begin(TextRenderer& t, float mouseX, float mouseY, bool click, float clickX, float clickY) {
    text = &t; mx = mouseX; my = mouseY; clicked = click; cx = clickX; cy = clickY; consumed = false; blocks.clear();
  }
  void block(UiRect r) { blocks.push_back(r); }
  bool blocked(float px, float py) const { for (const UiRect& b : blocks) if (b.contains(px, py)) return true; return false; }
  bool hot(const UiRect& r) const { return r.contains(mx, my); }
  // Consumes the click if it fell inside `r`; returns true once.
  bool clickIn(const UiRect& r) { if (clicked && !consumed && r.contains(cx, cy)) { consumed = true; return true; } return false; }

  // Panel background that owns its area.
  void panel(UiRect r, vec4 color) { text->rect(r.x, r.y, r.w, r.h, color); block(r); }

  // Button: returns true when clicked. `active` = toggled look, `enabled` false = greyed, no click.
  bool button(UiRect r, const std::string& label, bool active = false, bool enabled = true, float scale = 0.7f) {
    bool h = enabled && hot(r);
    vec4 bg = !enabled ? vec4{0.18f, 0.2f, 0.22f, 0.9f} : active ? vec4{0.1f, 0.45f, 0.75f, 0.95f} : h ? vec4{0.3f, 0.34f, 0.4f, 0.95f} : vec4{0.2f, 0.23f, 0.27f, 0.95f};
    text->rect(r.x, r.y, r.w, r.h, bg);
    float lh = text->lineHeight(scale), tw = text->measure(label, scale);
    text->draw(label, r.x + (r.w - tw) / 2, r.y + (r.h - lh) / 2, enabled ? vec4{1, 1, 1, 1} : vec4{0.55f, 0.58f, 0.62f, 1}, scale);
    return enabled && clickIn(r);
  }
  // Clickable text row (list entries): highlights on hover/selection.
  bool row(UiRect r, const std::string& label, vec4 color, bool selected, float scale = 0.7f) {
    if (selected) text->rect(r.x, r.y, r.w, r.h, {0.1f, 0.35f, 0.6f, 0.6f});
    else if (hot(r)) text->rect(r.x, r.y, r.w, r.h, {1, 1, 1, 0.08f});
    text->draw(label, r.x + 4, r.y + (r.h - text->lineHeight(scale)) / 2, color, scale);
    return clickIn(r);
  }
};

} // namespace eng
