// The terrain index (tools/fetch_tiles output, or built by the web adapter from the save): which tile
// rectangles exist, at which zoom and pixel size. The engine is handed the JSON bytes verbatim; the host
// only needs the `sat` layers, because a `sat/<i>` request carries the layer index and NOT the zoom the
// tiles actually live at — `zoom` and `px` here are authoritative (indeksMedan.ts / ubinAyana.ts).
import 'dart:convert';
import 'dart:typed_data';

class AyanaTerrainLayer {
  const AyanaTerrainLayer({
    required this.dir,
    required this.zoom,
    required this.tx0,
    required this.ty0,
    required this.nx,
    required this.ny,
    required this.px,
    required this.present,
  });

  final String dir; // "dem" | "sat/<i>"
  final int zoom, tx0, ty0, nx, ny, px;
  final String present; // row-major '1'/'0', ty outer, tx inner; empty = everything present

  /// Whether the index claims this tile exists (an absent tile is answered with `fail`, not a fetch).
  bool has(int x, int y) {
    final int ix = x - tx0, iy = y - ty0;
    if (ix < 0 || iy < 0 || ix >= nx || iy >= ny) return false;
    if (present.isEmpty) return true;
    final int at = iy * nx + ix;
    return at < present.length && present[at] == '1';
  }

  static AyanaTerrainLayer fromJson(Map<String, dynamic> j) => AyanaTerrainLayer(
        dir: (j['dir'] ?? '') as String,
        zoom: (j['zoom'] as num?)?.toInt() ?? 0,
        tx0: (j['tx0'] as num?)?.toInt() ?? 0,
        ty0: (j['ty0'] as num?)?.toInt() ?? 0,
        nx: (j['nx'] as num?)?.toInt() ?? 0,
        ny: (j['ny'] as num?)?.toInt() ?? 0,
        px: (j['px'] as num?)?.toInt() ?? 256,
        present: (j['present'] ?? '') as String,
      );
}

class AyanaTerrainIndex {
  const AyanaTerrainIndex({required this.map, required this.dem, required this.sat, required this.bytes});

  final String map;
  final List<AyanaTerrainLayer> dem;
  final List<AyanaTerrainLayer> sat; // index i answers a "sat/<i>" request
  final Uint8List bytes; // exactly what goes to eng_terrain_index

  AyanaTerrainLayer? satLayer(int i) => i >= 0 && i < sat.length ? sat[i] : null;

  static List<AyanaTerrainLayer> _layers(dynamic v) => v is List
      ? v.whereType<Map<String, dynamic>>().map(AyanaTerrainLayer.fromJson).toList(growable: false)
      : const <AyanaTerrainLayer>[];

  /// Throws [FormatException] when the bytes are not the index JSON.
  static AyanaTerrainIndex parse(Uint8List bytes) {
    final dynamic j = jsonDecode(utf8.decode(bytes));
    if (j is! Map<String, dynamic>) throw const FormatException('terrain index: not an object');
    return AyanaTerrainIndex(
      map: (j['map'] ?? '') as String,
      dem: _layers(j['dem']),
      sat: _layers(j['sat']),
      bytes: bytes,
    );
  }
}
