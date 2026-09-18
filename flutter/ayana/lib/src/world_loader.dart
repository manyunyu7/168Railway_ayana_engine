// AyanaWorldLoader: answers the engine's asset requests. The engine never fetches anything itself, so
// this is the whole "where do the bytes come from" policy on Android — the mirror image of
// ppka-wannabe-2/src/tiga-ayana/duniaAyana.ts, with the same URLs and the same arithmetic.
//
// Two sources, deliberately separate:
//  * `resolve(kind, path)` — the HOST's asset store. The plugin stays app-agnostic: the 168Railway app
//    wires PpkaAsetService (its disk cache under ApplicationSupport/pk-aset) into it, a test wires a map.
//    Used for models, city JSON and a bundled terrain index. Returning null means "not available".
//  * the tile servers, for DEM and satellite imagery, straight over HttpClient with a small pool.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';
import 'dart:ui' as ui;

import '../ayana.dart';
import 'tile_http.dart';
import 'tile_images.dart';

/// Where the host keeps an asset. `kind` is the engine's ("model", "city", "terrain"), `path` is the
/// engine's path, and `assetPath` is the conventional PPKA asset path for the pair (already carrying the
/// chosen model variant directory) — the 168Railway app hands that one straight to PpkaAsetService.
typedef AyanaAssetResolver = Future<Uint8List?> Function(String kind, String path, String assetPath);

/// What the dev page (and later the HUD) shows while a world streams in.
class AyanaLoadProgress {
  const AyanaLoadProgress({
    required this.answered,
    required this.failed,
    required this.inFlight,
    required this.ready,
    required this.lastError,
  });

  final int answered, failed, inFlight;
  final bool ready;
  final String lastError;

  @override
  String toString() => 'answered $answered, failed $failed, in flight $inFlight, ready $ready'
      '${lastError.isEmpty ? '' : ', error: $lastError'}';
}

class AyanaWorldLoader {
  AyanaWorldLoader({
    required this.resolve,
    this.variant = AyanaModelVariant.penuh,
    AyanaTileFetcher? tiles,
    AyanaEngineSink? engine,
  })  : _tiles = tiles ?? HttpTileFetcher(),
        _engine = engine ?? Ayana.instance;

  final AyanaAssetResolver resolve;

  /// Which Android model build to ask the host for (see [AyanaModelVariant]). The app picks `hemat` on
  /// a low-memory device; the resolver sees it through [assetPathFor].
  final AyanaModelVariant variant;
  final AyanaTileFetcher _tiles;
  final AyanaEngineSink _engine;

  AyanaTerrainIndex? _index;
  Map<String, dynamic>? _world;   // the save `load()` carried: the terrain index is built from it
  String _map = '';
  StreamSubscription<AyanaAssetRequest>? _sub;
  int _answered = 0, _failed = 0, _inFlight = 0;
  final StreamController<AyanaLoadProgress> _progress = StreamController<AyanaLoadProgress>.broadcast();

  Stream<AyanaLoadProgress> get progress => _progress.stream;
  AyanaTerrainIndex? get index => _index;

  /// Starts answering requests and loads the world. The strings are big (the save ~60 KB, model.json
  /// ~600 KB) and go straight into engine memory.
  Future<bool> load({
    required String worldJson,
    required String summaryJson,
    required String map,
    required String catalogJson,
  }) async {
    _sub ??= _engine.assetRequests.listen(_answer);
    _answered = _failed = 0;
    _map = map;
    _index = null;
    try {
      final dynamic j = jsonDecode(worldJson);
      _world = j is Map<String, dynamic> ? j : null;
    } catch (_) {
      _world = null;   // the engine will reject it too, and say so in lastError
    }
    return _engine.loadWorld(worldJson: worldJson, summaryJson: summaryJson, map: map, catalogJson: catalogJson);
  }

  Future<void> dispose() async {
    await _sub?.cancel();
    _sub = null;
    _tiles.close();
    await _progress.close();
  }

  void _emit() {
    if (_progress.isClosed) return;
    _progress.add(AyanaLoadProgress(
      answered: _answered,
      failed: _failed,
      inFlight: _inFlight,
      ready: _engine.ready,
      lastError: _engine.lastError(),
    ));
  }

  /// Visible for tests: routes one request without touching the stream.
  Future<void> answer(AyanaAssetRequest r) => _answer(r);

  Future<void> _answer(AyanaAssetRequest r) async {
    _inFlight++;
    try {
      switch (r.kind) {
        case 'terrain':
          await _terrain(r.path);
          break;
        case 'model':
          await _model(r.path);
          break;
        case 'city':
          await _city(r.path);
          break;
        default:
          break; // an unknown kind is simply not answered; the engine treats it as optional
      }
    } catch (_) {
      _failed++;
    } finally {
      _inFlight--;
      _emit();
    }
  }

  // ---- terrain ----
  Future<void> _terrain(String path) async {
    final TileRef? ref = TileRef.parse(path);
    if (ref == null) {
      // `<map>/index.json`. The host may have a fetch_tiles file (the dev page bundles one), but the
      // CDN almost never does, so the normal case is to BUILD the index from the save — same rule as
      // the web's indeksMedan.ts. Only if neither works is the world flat, which is not an error.
      Uint8List? bytes;
      try {
        bytes = await resolve('terrain', path, assetPathFor('terrain', path));
        if (bytes != null) _index = AyanaTerrainIndex.parse(bytes);
      } catch (_) {
        bytes = null;   // a stale or corrupt file must not beat the synthesised index
      }
      if (bytes == null) {
        final Map<String, dynamic>? world = _world;
        final AyanaTerrainIndex? dibuat = world == null ? null : AyanaTerrainIndex.fromWorld(_map, world);
        if (dibuat != null) {
          _index = dibuat;
          bytes = dibuat.bytes;
        }
      }
      if (bytes == null) {
        _engine.terrainIndex(null);
        _failed++;
        return;
      }
      _engine.terrainIndex(bytes);
      _answered++;
      return;
    }
    if (ref.isDem) {
      final Uint8List? png = await _tiles.first(demUrls(ref.z, ref.x, ref.y));
      if (png == null) {
        _engine.terrainTileFail(ref.dir, ref.z, ref.x, ref.y);
        _failed++;
        return;
      }
      final ui.Image img = await decodeImage(png);
      final Uint8List rgba = await toRgba(img);
      final int w = img.width, h = img.height;
      img.dispose();
      // Terrarium goes in RAW: the engine decodes R*256 + G + B/256 - 32768 itself.
      _engine.terrainTileRgba(ref.dir, ref.z, ref.x, ref.y, w, h, rgba);
      _answered++;
      return;
    }
    final AyanaTerrainLayer? layer = _index?.satLayer(ref.satIndex);
    if (layer == null) {
      _engine.terrainTileFail(ref.dir, ref.z, ref.x, ref.y);
      _failed++;
      return;
    }
    final TileComposition c = satComposition(layer, ref.x, ref.y);
    final List<DecodedSub> subs = <DecodedSub>[];
    await Future.wait(c.tiles.map((SubTile s) async {
      final Uint8List? png = await _tiles.first(satUrls(s.z, s.x, s.y));
      if (png == null) return;
      subs.add(DecodedSub(await decodeImage(png), s.sx, s.sy));
    }));
    if (subs.isEmpty) {
      _engine.terrainTileFail(ref.dir, ref.z, ref.x, ref.y);
      _failed++;
      return;
    }
    final Uint8List rgba = await composeRgba(c, subs);
    for (final DecodedSub s in subs) {
      s.image.dispose();
    }
    _engine.terrainTileRgba(ref.dir, ref.z, ref.x, ref.y, c.px, c.px, rgba);
    _answered++;
  }

  // ---- decor / train models ----
  Future<void> _model(String slot) async {
    final Uint8List? raw = await resolve('model', slot, assetPathFor('model', slot, variant: variant));
    if (raw == null) {
      _engine.modelFail(slot);
      _failed++;
      return;
    }
    final Uint8List bytes = maybeGunzip(raw);
    if (_engine.loadModel(slot, bytes) != 1) {
      _engine.modelFail(slot);
      _failed++;
      return;
    }
    _answered++;
  }

  // ---- baked OSM city (optional) ----
  Future<void> _city(String path) async {
    final Uint8List? bytes = await resolve('city', path, assetPathFor('city', path));
    _engine.cityJson(bytes == null ? null : maybeGunzip(bytes));
    if (bytes == null) {
      _failed++;
    } else {
      _answered++;
    }
  }
}
