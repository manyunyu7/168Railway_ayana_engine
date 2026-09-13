// Slippy-map tile math on the flipped Web Mercator world (see docs/world-spec.md §2.1).
#pragma once
#include <cmath>

namespace eng::slippy {

constexpr double R_EARTH = 6378137.0;
constexpr double CIRCUMFERENCE = 2.0 * 3.14159265358979323846 * R_EARTH;

inline double tileSizeMeter(int z) { return CIRCUMFERENCE / std::ldexp(1.0, z); }
// Tile index containing a world point. World y is already south-positive, so it maps straight to tile y.
inline int worldToTileX(double wx, int z) { return (int)std::floor((wx / CIRCUMFERENCE + 0.5) * std::ldexp(1.0, z)); }
inline int worldToTileY(double wy, int z) { return (int)std::floor((wy / CIRCUMFERENCE + 0.5) * std::ldexp(1.0, z)); }
// World coordinate of a tile's north-west corner.
inline double tileOriginX(int tx, int z) { return (tx / std::ldexp(1.0, z) - 0.5) * CIRCUMFERENCE; }
inline double tileOriginY(int ty, int z) { return (ty / std::ldexp(1.0, z) - 0.5) * CIRCUMFERENCE; }

} // namespace eng::slippy
