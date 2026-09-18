// Turning the engine's asset requests into URLs, the same arithmetic the web adapter does
// (ppka-wannabe-2 src/tiga-ayana/ubinAyana.ts + src/tiga/konstInti.ts). Pure Dart on purpose: this is
// the part that is easy to get wrong and easy to unit-test without a device.
import 'terrain_index.dart';

/// Everything goes through 168's own tile proxy (CORS/CSP on the web, one place to cache here).
const String kTileBase = 'https://tiles.168railway.com';

/// A parsed `<map>/<dir>/<z>_<x>_<y>.bin` request.
class TileRef {
  const TileRef(this.map, this.dir, this.z, this.x, this.y);
  final String map;
  final String dir; // "dem" or "sat/<i>"
  final int z, x, y;

  bool get isDem => dir == 'dem';

  /// The layer index of a "sat/<i>" dir, or -1.
  int get satIndex => dir.startsWith('sat/') ? int.tryParse(dir.substring(4)) ?? -1 : -1;

  static final RegExp _re = RegExp(r'^([^/]+)/(dem|sat/\d+)/(\d+)_(\d+)_(\d+)\.bin$');

  /// null when `path` is not a tile path (e.g. `<map>/index.json`).
  static TileRef? parse(String path) {
    final RegExpMatch? m = _re.firstMatch(path);
    if (m == null) return null;
    return TileRef(m.group(1)!, m.group(2)!, int.parse(m.group(3)!), int.parse(m.group(4)!), int.parse(m.group(5)!));
  }

  @override
  String toString() => '$map/$dir/${z}_${x}_$y.bin';
}

/// One source image to fetch, and where it belongs in the composed tile.
class SubTile {
  const SubTile(this.z, this.x, this.y, this.sx, this.sy);
  final int z, x, y; // slippy indices of the source tile
  final int sx, sy; // its cell in the n x n grid, sy = 0 is the north row
}

/// Terrarium DEM: one tile, no composition. `urls` is tried in order.
List<String> demUrls(int z, int x, int y) => <String>[
      '$kTileBase/terrarium/$z/$x/$y.png',
      'https://s3.amazonaws.com/elevation-tiles-prod/terrarium/$z/$x/$y.png',
    ];

/// Satellite: the 168 proxy first, then Esri — which takes **z/y/x** and has no extension.
List<String> satUrls(int z, int x, int y) => <String>[
      '$kTileBase/satellite/$z/$x/$y.png',
      'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/$z/$y/$x',
    ];

/// How a layer tile is built: a `px` x `px` image made of `n x n` source tiles at `zoom + k`
/// (`px` 256 -> one tile at `zoom`; `px` 512 -> four tiles at `zoom + 1`). The z in the request path is
/// ignored — the layer's own `zoom` is authoritative, exactly as in ubinAyana.ts.
class TileComposition {
  const TileComposition(this.px, this.sub, this.sourceZoom, this.tiles);
  final int px; // the composed edge in pixels
  final int sub; // the edge of one source tile inside it
  final int sourceZoom;
  final List<SubTile> tiles;

  int get n => px ~/ sub;
}

TileComposition satComposition(AyanaTerrainLayer layer, int x, int y) {
  final int px = layer.px < 256 ? 256 : layer.px;
  int k = 0;
  while ((256 << k) < px) k++; // px / 256 is a power of two in every index fetch_tiles writes
  final int n = 1 << k, sub = px ~/ n, zs = layer.zoom + k;
  final List<SubTile> tiles = <SubTile>[];
  for (int i = 0; i < n * n; i++) {
    final int sx = i % n, sy = i ~/ n;
    tiles.add(SubTile(zs, x * n + sx, y * n + sy, sx, sy));
  }
  return TileComposition(px, sub, zs, tiles);
}

/// A catalog id as a file name: `web/build-models.sh` and `web/build-models-android.sh` both replace
/// every ':' with '__'. Android models live next to the web ones, in their own directory.
String modelFileName(String slot) => '${slot.replaceAll(':', '__')}.emod';

/// Which Android model build to fetch. Both are `convert --target android` with ETC2 embedded and both
/// are uploaded gzipped; they differ only in the texture cap (web/build-models-android.sh).
/// `penuh` (1024 px atlases) is the default because 512 px is visibly soft in the kabin/samping cameras;
/// `hemat` (512 px) is for low-memory devices, the way the web quality tiers pick a cheaper look.
enum AyanaModelVariant {
  penuh('ayana/models-android'),
  hemat('ayana/models-android-hemat');

  const AyanaModelVariant(this.dir);
  final String dir;
}

/// The asset path (relative to the PPKA asset root) for each request kind.
/// `city` paths already carry their own `.json`.
String assetPathFor(String kind, String path, {AyanaModelVariant variant = AyanaModelVariant.penuh}) {
  switch (kind) {
    case 'model':
      return '${variant.dir}/${modelFileName(path)}';
    case 'city':
      return 'kota/$path';
    case 'terrain':
      return 'ayana/terrain/$path';
    default:
      return path;
  }
}
