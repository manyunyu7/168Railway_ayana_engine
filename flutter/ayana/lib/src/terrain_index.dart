// The terrain index (tools/fetch_tiles output, or built by the web adapter from the save): which tile
// rectangles exist, at which zoom and pixel size. The engine is handed the JSON bytes verbatim; the host
// only needs the `sat` layers, because a `sat/<i>` request carries the layer index and NOT the zoom the
// tiles actually live at — `zoom` and `px` here are authoritative (indeksMedan.ts / ubinAyana.ts).
import 'dart:convert';
import 'dart:math' as math;
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

/// A point in world coordinates (Web Mercator metres, y south-positive like the save).
class _Titik {
  const _Titik(this.x, this.y);
  final double x, y;
}

class _Bbox {
  _Bbox(this.x0, this.y0, this.x1, this.y1);
  double x0, y0, x1, y1;

  static _Bbox of(List<_Titik> pts) {
    final _Bbox b = _Bbox(double.infinity, double.infinity, -double.infinity, -double.infinity);
    for (final _Titik p in pts) {
      if (p.x < b.x0) b.x0 = p.x;
      if (p.y < b.y0) b.y0 = p.y;
      if (p.x > b.x1) b.x1 = p.x;
      if (p.y > b.y1) b.y1 = p.y;
    }
    return b;
  }
}

class _Rentang {
  const _Rentang(this.z, this.tx0, this.ty0, this.nx, this.ny);
  final int z, tx0, ty0, nx, ny;
}

/// Slippy-tile maths, the same as ppka-wannabe-2 src/engine/geo.ts and tools/fetch_tiles.
const double _kRBumi = 6378137;
final double _kLingkar = 2 * math.pi * _kRBumi;
double _tileSizeMeter(int z) => _kLingkar / math.pow(2, z);
int _tileX(double x, int z) => ((x / _kLingkar + 0.5) * math.pow(2, z)).floor();
double _tileOriginX(int tx, int z) => (tx / math.pow(2, z) - 0.5) * _kLingkar;

_Rentang _rentangUbin(_Bbox b, double margin, int z) {
  final int ax = _tileX(b.x0 - margin, z), ay = _tileX(b.y0 - margin, z);
  final int cx = _tileX(b.x1 + margin, z), cy = _tileX(b.y1 + margin, z);
  return _Rentang(z, ax, ay, cx - ax + 1, cy - ay + 1);
}

/// The far satellite layer's zoom: z12, then z11 if its bbox+2000 m range is <= 40 tiles, else z10.
int _zoomJauh(_Bbox b) {
  for (int z = 12; z > 10; z--) {
    final _Rentang r = _rentangUbin(b, 2000, z);
    if (r.nx * r.ny <= 40) return z;
  }
  return 10;
}

/// Tiles at zoom z whose box is within `radius` of any point (fetch_tiles selectTiles).
_Rentang? _pilihUbin(int z, List<_Titik> pts, double radius, List<String> presentOut) {
  if (pts.isEmpty) return null;
  final _Rentang r = _rentangUbin(_Bbox.of(pts), radius, z);
  final double ts = _tileSizeMeter(z);
  final StringBuffer present = StringBuffer();
  int n = 0;
  for (int ty = r.ty0; ty < r.ty0 + r.ny; ty++) {
    for (int tx = r.tx0; tx < r.tx0 + r.nx; tx++) {
      final double ox = _tileOriginX(tx, z), oy = _tileOriginX(ty, z);
      bool ada = false;
      for (final _Titik p in pts) {
        final double dx = math.max(math.max(ox - p.x, 0), p.x - (ox + ts));
        final double dy = math.max(math.max(oy - p.y, 0), p.y - (oy + ts));
        if (dx * dx + dy * dy <= radius * radius) {
          ada = true;
          break;
        }
      }
      present.write(ada ? '1' : '0');
      if (ada) n++;
    }
  }
  if (n == 0) return null;
  presentOut.add(present.toString());
  return r;
}

class AyanaTerrainIndex {
  const AyanaTerrainIndex({required this.map, required this.dem, required this.sat, required this.bytes});

  final String map;
  final List<AyanaTerrainLayer> dem;
  final List<AyanaTerrainLayer> sat; // index i answers a "sat/<i>" request
  final Uint8List bytes; // exactly what goes to eng_terrain_index

  AyanaTerrainLayer? satLayer(int i) => i >= 0 && i < sat.length ? sat[i] : null;

  /// The index's own bbox, as the JSON carries it (x0, y0, x1, y1).
  List<double> bbox() {
    try {
      final dynamic j = jsonDecode(utf8.decode(bytes));
      final dynamic b = j is Map ? j['bbox'] : null;
      if (b is List && b.length >= 4) return b.map((dynamic v) => (v as num).toDouble()).toList(growable: false);
    } catch (_) {/* indeks tanpa bbox: bukan urusan pemanggil */}
    return const <double>[0, 0, 0, 0];
  }

  static List<AyanaTerrainLayer> _layers(dynamic v) => v is List
      ? v.whereType<Map<String, dynamic>>().map(AyanaTerrainLayer.fromJson).toList(growable: false)
      : const <AyanaTerrainLayer>[];

  /// Builds the index from the world save instead of a file — what the web does in
  /// `src/tiga-ayana/indeksMedan.ts` when `ayana/terrain/<map>/index.json` is missing, which on the
  /// CDN it almost always is (fetch_tiles output is never uploaded). The tiles themselves come from
  /// the tile server either way; all the engine needs is the LIST OF LAYERS, and the rules here are
  /// the same ones tools/fetch_tiles uses:
  ///   dem     z13 bbox +- 5000 m, z10 bbox +- 2000 m               (256 px)
  ///   sat/0   z14 bbox +- 2500 m @512 (four z15 children)
  ///   sat/1   z12..11 bbox +- 2000 m @256 (largest zoom <= 40 tiles, else z10)
  ///   sat/2   z16 @256 within 1500 m of any rail node
  ///   sat/3   z16 @512 within 1500 m of a station
  ///   sat/4   z17 @512 within 450 m of a station
  /// A detail layer with no points is skipped and the numbering stays sequential. `present` is all
  /// '1' for the coarse layers (a tile that fails to download is reported with eng_terrain_tile_fail)
  /// and per-tile for the detail ones; `mean` is not emitted (the engine keeps a running average).
  /// Returns null when the save has no rail nodes.
  static AyanaTerrainIndex? fromWorld(String map, Map<String, dynamic> world) {
    final List<_Titik> nodes = <_Titik>[];
    final dynamic graf = world['graph'];
    if (graf is Map && graf['nodes'] is List) {
      for (final dynamic n in graf['nodes'] as List<dynamic>) {
        if (n is! Map) continue;
        final double? x = (n['x'] as num?)?.toDouble(), y = (n['y'] as num?)?.toDouble();
        if (x != null && y != null && x.isFinite && y.isFinite) nodes.add(_Titik(x, y));
      }
    }
    if (nodes.isEmpty) return null;
    final List<_Titik> stasiun = <_Titik>[];
    if (world['scenery'] is List) {
      for (final dynamic s in world['scenery'] as List<dynamic>) {
        if (s is! Map || s['kind'] != 'station' || s['pos'] is! Map) continue;
        final Map<dynamic, dynamic> pos = s['pos'] as Map<dynamic, dynamic>;
        final double? x = (pos['x'] as num?)?.toDouble(), y = (pos['y'] as num?)?.toDouble();
        if (x != null && y != null) stasiun.add(_Titik(x, y));
      }
    }

    final _Bbox bb = _Bbox.of(nodes);
    Map<String, dynamic> lapis(String dir, _Rentang r, int px, [String? present]) => <String, dynamic>{
          'dir': dir,
          'zoom': r.z,
          'tx0': r.tx0,
          'ty0': r.ty0,
          'nx': r.nx,
          'ny': r.ny,
          'px': px,
          'present': present ?? '1' * (r.nx * r.ny),
        };

    final List<Map<String, dynamic>> dem = <Map<String, dynamic>>[
      lapis('dem', _rentangUbin(bb, 5000, 13), 256),
      lapis('dem', _rentangUbin(bb, 2000, 10), 256),
    ];
    final List<Map<String, dynamic>> sat = <Map<String, dynamic>>[
      lapis('sat/0', _rentangUbin(bb, 2500, 14), 512),
      lapis('sat/1', _rentangUbin(bb, 2000, _zoomJauh(bb)), 256),
    ];
    final List<List<Object>> detail = <List<Object>>[
      <Object>[16, nodes, 1500.0, 256],
      <Object>[16, stasiun, 1500.0, 512],
      <Object>[17, stasiun, 450.0, 512],
    ];
    for (final List<Object> d in detail) {
      final List<String> present = <String>[];
      final _Rentang? r = _pilihUbin(d[0] as int, d[1] as List<_Titik>, d[2] as double, present);
      if (r != null) sat.add(lapis('sat/${sat.length}', r, d[3] as int, present.single));
    }

    final Map<String, dynamic> j = <String, dynamic>{
      'map': map,
      'bbox': <double>[bb.x0, bb.y0, bb.x1, bb.y1],
      'dem': dem,
      'sat': sat,
    };
    return AyanaTerrainIndex(
      map: map,
      dem: dem.map(AyanaTerrainLayer.fromJson).toList(growable: false),
      sat: sat.map(AyanaTerrainLayer.fromJson).toList(growable: false),
      bytes: Uint8List.fromList(utf8.encode(jsonEncode(j))),
    );
  }

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
