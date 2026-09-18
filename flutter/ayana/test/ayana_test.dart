// Everything about the host side that does not need a device: the tile arithmetic, the terrain index,
// the Terrarium decode (through Skia, the same path the phone uses) and the request routing.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:ayana/ayana.dart';
import 'package:ayana/src/tile_images.dart';
import 'package:flutter_test/flutter_test.dart';

// ---- a fake engine: records what the loader would have called over FFI ----
class FakeEngine implements AyanaEngineSink {
  final List<String> calls = <String>[];
  final List<Uint8List> tiles = <Uint8List>[];
  bool readyFlag = false;
  String error = '';

  @override
  Stream<AyanaAssetRequest> get assetRequests => const Stream<AyanaAssetRequest>.empty();
  @override
  bool get ready => readyFlag;
  @override
  String lastError() => error;
  @override
  bool loadWorld({required String worldJson, required String summaryJson, required String map, required String catalogJson}) {
    calls.add('loadWorld($map, world ${worldJson.length}, catalog ${catalogJson.length})');
    return true;
  }

  @override
  int terrainIndex(Uint8List? bytes) {
    calls.add('terrainIndex(${bytes?.length ?? 0})');
    return bytes == null ? 0 : 1;
  }

  @override
  int terrainTileRgba(String dir, int z, int x, int y, int w, int h, Uint8List rgba) {
    calls.add('tile($dir,$z,$x,$y,${w}x$h,${rgba.length})');
    tiles.add(rgba);
    return 1;
  }

  @override
  void terrainTileFail(String dir, int z, int x, int y) => calls.add('tileFail($dir,$z,$x,$y)');
  @override
  int cityJson(Uint8List? bytes) {
    calls.add('city(${bytes?.length ?? 0})');
    return 1;
  }

  @override
  int loadModel(String slot, Uint8List bytes) {
    calls.add('model($slot,${bytes.length})');
    return 1;
  }

  @override
  void modelFail(String slot) => calls.add('modelFail($slot)');
}

class FakeTiles implements AyanaTileFetcher {
  FakeTiles(this.body);
  final Uint8List? body;
  final List<String> asked = <String>[];
  @override
  Future<Uint8List?> first(List<String> urls) async {
    asked.addAll(urls.take(1));
    return body;
  }

  @override
  void close() {}
}

// The bridge's view of the engine: records commands, answers with a canned snapshot.
class FakeCommands implements AyanaCommandSink {
  final List<String> calls = <String>[];
  bool snapOn = false;
  String snap = '{"frame":1,"ready":1}';

  @override
  void setState(String stepJson) => calls.add('state(${stepJson.length})');
  @override
  void pointer(double x, double y, int button, int phase) => calls.add('pointer($x,$y,$button,$phase)');
  @override
  void command(String name, double a, double b, double c, String s) =>
      calls.add('$name($a,$b,$c${s.isEmpty ? '' : ',$s'})');
  @override
  void snapshotEnable(bool on) => snapOn = on;
  @override
  String snapshot() => snap;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('request paths', () {
    test('tile paths parse, index.json does not', () {
      final TileRef? dem = TileRef.parse('mojokerto/dem/13_6652_4265.bin');
      expect(dem, isNotNull);
      expect(dem!.map, 'mojokerto');
      expect(dem.dir, 'dem');
      expect(dem.isDem, isTrue);
      expect(<int>[dem.z, dem.x, dem.y], <int>[13, 6652, 4265]);
      expect(dem.satIndex, -1);

      final TileRef? sat = TileRef.parse('mojokerto/sat/3/16_13305_8531.bin');
      expect(sat!.dir, 'sat/3');
      expect(sat.satIndex, 3);
      expect(sat.isDem, isFalse);

      expect(TileRef.parse('mojokerto/index.json'), isNull);
      expect(TileRef.parse('mojokerto/dem/13_6652.bin'), isNull);
      expect(TileRef.parse('mojokerto/blah/1_2_3.bin'), isNull);
    });

    test('tile URLs: 168 proxy first, Esri takes z/y/x with no extension', () {
      expect(demUrls(13, 6652, 4265).first, 'https://tiles.168railway.com/terrarium/13/6652/4265.png');
      expect(demUrls(13, 6652, 4265).last, contains('elevation-tiles-prod/terrarium/13/6652/4265.png'));
      expect(satUrls(14, 13305, 8531).first, 'https://tiles.168railway.com/satellite/14/13305/8531.png');
      expect(satUrls(14, 13305, 8531).last, endsWith('/MapServer/tile/14/8531/13305'));
    });

    test('model files and asset paths mirror web/build-models.sh', () {
      expect(modelFileName('pohon-05'), 'pohon-05.emod');
      expect(modelFileName('atp:ss23-ekonomi'), 'atp__ss23-ekonomi.emod');
      expect(assetPathFor('model', 'pohon-05'), 'ayana/models-android/pohon-05.emod');
      expect(assetPathFor('model', 'pohon-05', variant: AyanaModelVariant.hemat), 'ayana/models-android-hemat/pohon-05.emod');
      expect(assetPathFor('city', 'mojokerto.json'), 'kota/mojokerto.json');
      expect(assetPathFor('terrain', 'mojokerto/index.json'), 'ayana/terrain/mojokerto/index.json');
    });
  });

  group('terrain index', () {
    late AyanaTerrainIndex index;
    setUpAll(() {
      index = AyanaTerrainIndex.parse(File('test/fixtures/mojokerto_index.json').readAsBytesSync());
    });

    test('layers are read and sat/<i> keeps its order', () {
      expect(index.map, 'mojokerto');
      expect(index.dem.first.zoom, 13);
      expect(index.dem.first.px, 256);
      expect(index.dem.first.nx * index.dem.first.ny, index.dem.first.present.length);
      expect(index.satLayer(0)!.dir, 'sat/0');
      expect(index.satLayer(0)!.px, 512);
      expect(index.satLayer(index.sat.length), isNull);
    });

    test('present bitmap answers per tile, out of range is absent', () {
      final AyanaTerrainLayer l = index.dem.first;
      expect(l.has(l.tx0, l.ty0), isTrue);
      expect(l.has(l.tx0 - 1, l.ty0), isFalse);
      expect(l.has(l.tx0, l.ty0 + l.ny), isFalse);
    });

    test('a 512 px layer composes 2x2 children one zoom deeper, north row first', () {
      final AyanaTerrainLayer l = index.satLayer(0)!; // z14, px 512
      final TileComposition c = satComposition(l, 13305, 8531);
      expect(c.px, 512);
      expect(c.sub, 256);
      expect(c.n, 2);
      expect(c.sourceZoom, l.zoom + 1);
      expect(c.tiles.length, 4);
      expect(<int>[c.tiles[0].x, c.tiles[0].y, c.tiles[0].sx, c.tiles[0].sy], <int>[26610, 17062, 0, 0]);
      expect(<int>[c.tiles[3].x, c.tiles[3].y, c.tiles[3].sx, c.tiles[3].sy], <int>[26611, 17063, 1, 1]);
    });

    test('a 256 px layer is a single tile at its own zoom', () {
      const AyanaTerrainLayer l = AyanaTerrainLayer(dir: 'sat/1', zoom: 12, tx0: 0, ty0: 0, nx: 4, ny: 4, px: 256, present: '');
      final TileComposition c = satComposition(l, 3326, 2132);
      expect(c.n, 1);
      expect(c.sourceZoom, 12);
      expect(c.tiles.single.x, 3326);
    });

    test('rubbish bytes are a FormatException, not a crash', () {
      expect(() => AyanaTerrainIndex.parse(Uint8List.fromList(utf8.encode('[1,2]'))), throwsFormatException);
    });
  });

  group('terrarium decode', () {
    test('a 2x2 PNG round-trips to the RGBA the engine decodes heights from', () async {
      final Uint8List png = File('test/fixtures/terrarium_2x2.png').readAsBytesSync();
      final ui.Image img = await decodeImage(png);
      expect(img.width, 2);
      expect(img.height, 2);
      final Uint8List rgba = await toRgba(img);
      img.dispose();
      expect(rgba.length, 2 * 2 * 4);
      // heights: R*256 + G + B/256 - 32768, row 0 first (north)
      double h(int i) => rgba[i * 4] * 256 + rgba[i * 4 + 1] + rgba[i * 4 + 2] / 256 - 32768;
      expect(h(0), closeTo(0, 0.01));
      expect(h(1), closeTo(1, 0.01));
      expect(h(2), closeTo(100.5, 0.01));
      expect(h(3), closeTo(-5, 0.01));
      expect(rgba[3], 255);
    });
  });

  group('gzip', () {
    test('a gzipped .emod is inflated, a plain one is passed through', () {
      final Uint8List plain = Uint8List.fromList(utf8.encode('EMOD not really'));
      final Uint8List gz = Uint8List.fromList(gzip.encode(plain));
      expect(looksGzipped(gz), isTrue);
      expect(looksGzipped(plain), isFalse);
      expect(maybeGunzip(gz), plain);
      expect(identical(maybeGunzip(plain), plain), isTrue);
    });
  });

  group('bridge', () {
    test('a batch becomes engine calls, in order, and the snapshot comes back', () {
      final FakeCommands e = FakeCommands();
      final AyanaBridge b = AyanaBridge(engine: e);
      final String reply = b.handleBatch(<Map<String, dynamic>>[
        <String, dynamic>{'c': 'state', 's': '{"clock":25200,"trains":[]}'},
        <String, dynamic>{'c': 'orbit', 'a': 12.0, 'b': -3.0},
        <String, dynamic>{'c': 'zoom', 'a': 1.5},
        <String, dynamic>{'c': 'pointer', 'a': 100.0, 'b': 200.0, 'd': 0, 'e': 1},
        <String, dynamic>{'c': 'follow_train', 's': 'KA-123'},
        <String, dynamic>{'c': 'camera_mode', 'a': 3},
      ]);
      expect(e.snapOn, isTrue);
      expect(e.calls, <String>[
        'state(27)',
        'orbit(12.0,-3.0,0.0)',
        'zoom(1.5,0.0,0.0)',
        'pointer(100.0,200.0,0,1)',
        'follow_train(0.0,0.0,0.0,KA-123)',
        'camera_mode(3.0,0.0,0.0)',
      ]);
      expect(reply, '{"frame":1,"ready":1}');
      expect(b.latency.count, 1);
    });

    test('a batch as JSON text works too, and rubbish entries are skipped', () {
      final FakeCommands e = FakeCommands();
      final AyanaBridge b = AyanaBridge(engine: e);
      b.handleBatch('[{"c":"zoom","a":2},{"nope":1},{"c":""},3,"x"]');
      expect(e.calls, <String>['zoom(2.0,0.0,0.0)']);
      b.handleBatch(null);
      b.handleBatch('');
      expect(e.calls.length, 1);
    });

    test('latency percentiles are reported', () {
      final AyanaLatency l = AyanaLatency(window: 5);
      for (final double ms in <double>[1, 2, 3, 4, 5, 6, 7]) {
        l.add(ms);
      }
      expect(l.count, 7);
      expect(l.p50, 5); // the window kept 3..7
      expect(l.p95, 7);
      expect(AyanaLatency().p50, 0);
    });

    test('load_world without a resolver is refused, not half-done', () {
      final FakeCommands e = FakeCommands();
      final AyanaBridge b = AyanaBridge(engine: e);
      b.handleBatch(<Map<String, dynamic>>[
        <String, dynamic>{'c': 'load_world', 's': '{"w":1}', 's2': '{}', 's3': 'mojokerto', 's4': '{}'},
      ]);
      expect(b.loader, isNull);
      expect(e.calls, isEmpty);
    });

    test('disable turns the snapshot off', () {
      final FakeCommands e = FakeCommands();
      final AyanaBridge b = AyanaBridge(engine: e);
      b.handleBatch(const <dynamic>[]);
      expect(e.snapOn, isTrue);
      b.disable();
      expect(e.snapOn, isFalse);
    });
  });

  group('routing', () {
    late FakeEngine engine;
    late Map<String, Uint8List> store;
    late AyanaWorldLoader loader;

    Future<Uint8List?> resolve(String kind, String path, String assetPath) async => store[assetPath];

    setUp(() {
      engine = FakeEngine();
      store = <String, Uint8List>{};
      loader = AyanaWorldLoader(resolve: resolve, tiles: FakeTiles(null), engine: engine);
    });

    test('the index goes to the engine and is remembered for sat/<i>', () async {
      store['ayana/terrain/mojokerto/index.json'] = File('test/fixtures/mojokerto_index.json').readAsBytesSync();
      await loader.answer(const AyanaAssetRequest('terrain', 'mojokerto/index.json'));
      expect(engine.calls.single, startsWith('terrainIndex('));
      expect(loader.index!.map, 'mojokerto');
    });

    test('a missing index is not fatal: the engine is told there is no terrain', () async {
      await loader.answer(const AyanaAssetRequest('terrain', 'mojokerto/index.json'));
      expect(engine.calls.single, 'terrainIndex(0)');
    });

    test('a DEM tile the server does not have becomes eng_terrain_tile_fail', () async {
      await loader.answer(const AyanaAssetRequest('terrain', 'mojokerto/dem/13_6652_4265.bin'));
      expect(engine.calls.single, 'tileFail(dem,13,6652,4265)');
    });

    test('a DEM tile that arrives is decoded and handed over raw', () async {
      loader = AyanaWorldLoader(
        resolve: resolve,
        tiles: FakeTiles(File('test/fixtures/terrarium_2x2.png').readAsBytesSync()),
        engine: engine,
      );
      await loader.answer(const AyanaAssetRequest('terrain', 'mojokerto/dem/13_6652_4265.bin'));
      expect(engine.calls.single, 'tile(dem,13,6652,4265,2x2,16)');
    });

    test('sat without an index fails instead of guessing a zoom', () async {
      await loader.answer(const AyanaAssetRequest('terrain', 'mojokerto/sat/0/14_13305_8531.bin'));
      expect(engine.calls.single, 'tileFail(sat/0,14,13305,8531)');
    });

    test('a model comes from the host store, gzipped or not, and a missing one fails the slot', () async {
      final Uint8List emod = Uint8List.fromList(<int>[0x45, 0x4d, 0x4f, 0x44, 8, 0, 0, 0]);
      store['ayana/models-android/pohon-05.emod'] = Uint8List.fromList(gzip.encode(emod));
      await loader.answer(const AyanaAssetRequest('model', 'pohon-05'));
      expect(engine.calls.single, 'model(pohon-05,8)');

      engine.calls.clear();
      await loader.answer(const AyanaAssetRequest('model', 'tidak-ada'));
      expect(engine.calls.single, 'modelFail(tidak-ada)');
    });

    test('the hemat variant asks for the other directory', () async {
      final AyanaWorldLoader hemat = AyanaWorldLoader(
        resolve: resolve,
        variant: AyanaModelVariant.hemat,
        tiles: FakeTiles(null),
        engine: engine,
      );
      store['ayana/models-android-hemat/pohon-05.emod'] = Uint8List.fromList(<int>[1, 2, 3]);
      await hemat.answer(const AyanaAssetRequest('model', 'pohon-05'));
      expect(engine.calls.single, 'model(pohon-05,3)');
    });

    test('city is optional: a missing one still tells the engine, and progress counts it', () async {
      final List<AyanaLoadProgress> seen = <AyanaLoadProgress>[];
      loader.progress.listen(seen.add);
      await loader.answer(const AyanaAssetRequest('city', 'mojokerto.json'));
      await Future<void>.delayed(Duration.zero);
      expect(engine.calls.single, 'city(0)');
      expect(seen.single.failed, 1);
      expect(seen.single.inFlight, 0);
    });

    test('an unknown kind is ignored rather than guessed at', () async {
      await loader.answer(const AyanaAssetRequest('sesuatu', 'apa'));
      expect(engine.calls, isEmpty);
    });
  });
}
