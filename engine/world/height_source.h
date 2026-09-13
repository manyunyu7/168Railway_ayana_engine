// Height providers shared between terrain, track profile and object placement (metres).
#pragma once

namespace eng {

// Raw DEM height above sea level at a world (Mercator) position.
struct HeightSource {
  virtual ~HeightSource() = default;
  virtual float rawHeight(double wx, double wy) const = 0;
};

// Rail-head height along a segment (scene y), from the 1-D vertical profile (§3.4).
struct RailProfile {
  virtual ~RailProfile() = default;
  virtual float railHeight(const char* segId, double s) const = 0;
};

// One registered point of a rail centreline, used by terrain carving (§4.2).
struct RailSample {
  double wx, wy;      // world position
  float railY;        // scene y of the rail head
  bool atGrade;       // false on bridges/tunnels (skipped by carving)
  float mouthBlend;   // 0..1 weight near tunnel mouths (1 = full carve)
};

} // namespace eng
