// package:ayana — the Ayana engine inside a Flutter Texture.
//
// Two channels, on purpose:
//  * a MethodChannel, used exactly three times (create / resize / dispose), to get a TextureRegistry
//    entry and hand its Surface to the native side;
//  * dart:ffi straight into libayana.so for everything else. Every `ayana_*` symbol is a wrapper over
//    the engine's render-thread queue (engine/api/host/host_queue.h): Dart never calls an eng_* symbol,
//    because the C ABI assumes a single caller and that caller is the render thread.
import 'dart:async';
import 'dart:ffi' hide Size;

import 'package:ffi/ffi.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'src/asset_request.dart';
import 'src/engine_sink.dart';

export 'src/asset_request.dart';
export 'src/engine_sink.dart';
export 'src/terrain_index.dart';
export 'src/tile_urls.dart' show AyanaModelVariant, TileRef, TileComposition, SubTile, demUrls, satUrls, satComposition, modelFileName, assetPathFor, kTileBase;
export 'src/world_loader.dart';
export 'src/tile_http.dart' show AyanaTileFetcher;
export 'src/gzip_bytes.dart' show looksGzipped, maybeGunzip;

// ---- FFI signatures ----
typedef _VoidF = void Function();
typedef _SetFontC = Void Function(Pointer<Uint8>, Int32);
typedef _SetFont = void Function(Pointer<Uint8>, int);
typedef _FloatArgC = Void Function(Float);
typedef _FloatArg = void Function(double);
typedef _Float2C = Void Function(Float, Float);
typedef _Float2 = void Function(double, double);
typedef _Float3C = Void Function(Float, Float, Float);
typedef _Float3 = void Function(double, double, double);
typedef _IntArgC = Void Function(Int32);
typedef _IntArg = void Function(int);
typedef _RetIntC = Int32 Function();
typedef _RetInt = int Function();
typedef _RetFloatC = Float Function();
typedef _RetFloat = double Function();
typedef _ModelBeginC = Int32 Function(Pointer<Utf8>, Pointer<Uint8>, Int32);
typedef _ModelBegin = int Function(Pointer<Utf8>, Pointer<Uint8>, int);
typedef _StatsC = Int32 Function(Pointer<Utf8>, Int32);
typedef _Stats = int Function(Pointer<Utf8>, int);
typedef _PollC = Int32 Function(Pointer<Utf8>, Int32, Pointer<Utf8>, Int32);
typedef _Poll = int Function(Pointer<Utf8>, int, Pointer<Utf8>, int);
typedef _LoadWorldC = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _LoadWorld = int Function(Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _BytesC = Int32 Function(Pointer<Uint8>, Int32);
typedef _Bytes = int Function(Pointer<Uint8>, int);
typedef _TileRgbaC = Int32 Function(Pointer<Utf8>, Int32, Int32, Int32, Int32, Int32, Pointer<Uint8>);
typedef _TileRgba = int Function(Pointer<Utf8>, int, int, int, int, int, Pointer<Uint8>);
typedef _TileFailC = Void Function(Pointer<Utf8>, Int32, Int32, Int32);
typedef _TileFail = void Function(Pointer<Utf8>, int, int, int);
typedef _StrArgC = Void Function(Pointer<Utf8>);
typedef _StrArg = void Function(Pointer<Utf8>);
typedef _PointerC = Void Function(Float, Float, Int32, Int32);
typedef _PointerFn = void Function(double, double, int, int);
typedef _IntRetIntC = Int32 Function(Int32);
typedef _IntRetInt = int Function(int);
typedef _DoubleArgC = Void Function(Double);
typedef _DoubleArg = void Function(double);

/// The engine, one per process (the C ABI is a single global instance).
class Ayana implements AyanaEngineSink {
  Ayana._();
  static final Ayana instance = Ayana._();

  static const MethodChannel _channel = MethodChannel('ayana');
  DynamicLibrary? _lib;
  int? _textureId;
  Timer? _poller;
  final StreamController<AyanaAssetRequest> _requests = StreamController<AyanaAssetRequest>.broadcast();

  /// Asset requests coming out of the engine. M1 has nothing to answer them with yet (no world);
  /// M2 wires them to PpkaAsetService and the tile server.
  @override
  Stream<AyanaAssetRequest> get assetRequests => _requests.stream;

  int? get textureId => _textureId;

  DynamicLibrary get _l => _lib ??= DynamicLibrary.open('libayana.so');

  late final _SetFont _setFont = _l.lookupFunction<_SetFontC, _SetFont>('ayana_set_font');
  late final _FloatArg _setTargetFps = _l.lookupFunction<_FloatArgC, _FloatArg>('ayana_set_target_fps');
  late final _VoidF _shutdown = _l.lookupFunction<Void Function(), _VoidF>('ayana_shutdown');
  late final _RetInt _readyFn = _l.lookupFunction<_RetIntC, _RetInt>('ayana_ready');
  late final _RetFloat _fpsFn = _l.lookupFunction<_RetFloatC, _RetFloat>('ayana_fps');
  late final _Float2 _orbitFn = _l.lookupFunction<_Float2C, _Float2>('ayana_orbit');
  late final _FloatArg _zoomFn = _l.lookupFunction<_FloatArgC, _FloatArg>('ayana_zoom');
  late final _Float3 _setViewFn = _l.lookupFunction<_Float3C, _Float3>('ayana_set_view');
  late final _IntArg _setThemeFn = _l.lookupFunction<_IntArgC, _IntArg>('ayana_set_theme');
  late final _ModelBegin _modelBeginFn = _l.lookupFunction<_ModelBeginC, _ModelBegin>('ayana_model_begin');
  late final _Stats _statsFn = _l.lookupFunction<_StatsC, _Stats>('ayana_stats');
  late final _Poll _pollFn = _l.lookupFunction<_PollC, _Poll>('ayana_poll_request');
  late final _LoadWorld _loadWorldFn = _l.lookupFunction<_LoadWorldC, _LoadWorld>('ayana_load_world');
  late final _Bytes _terrainIndexFn = _l.lookupFunction<_BytesC, _Bytes>('ayana_terrain_index');
  late final _TileRgba _tileRgbaFn = _l.lookupFunction<_TileRgbaC, _TileRgba>('ayana_terrain_tile_rgba');
  late final _TileFail _tileFailFn = _l.lookupFunction<_TileFailC, _TileFail>('ayana_terrain_tile_fail');
  late final _Bytes _cityJsonFn = _l.lookupFunction<_BytesC, _Bytes>('ayana_city_json');
  late final _StrArg _modelFailFn = _l.lookupFunction<_StrArgC, _StrArg>('ayana_model_fail');
  late final _StrArg _texUnavailFn = _l.lookupFunction<_StrArgC, _StrArg>('ayana_model_textures_unavailable');
  late final _StrArg _setStateFn = _l.lookupFunction<_StrArgC, _StrArg>('ayana_set_state');
  late final _PointerFn _pointerFn = _l.lookupFunction<_PointerC, _PointerFn>('ayana_pointer');
  late final _Float2 _compassClickFn = _l.lookupFunction<_Float2C, _Float2>('ayana_compass_click');
  late final _IntRetInt _cameraModeFn = _l.lookupFunction<_IntRetIntC, _IntRetInt>('ayana_camera_mode');
  late final _IntRetInt _setQualityFn = _l.lookupFunction<_IntRetIntC, _IntRetInt>('ayana_set_quality');
  late final _DoubleArg _setSkyTimeFn = _l.lookupFunction<_DoubleArgC, _DoubleArg>('ayana_set_sky_time');
  late final _Stats _lastErrorFn = _l.lookupFunction<_StatsC, _Stats>('ayana_last_error');

  // ---- M2: the world ----
  // The four strings are large; they are copied once into native memory and freed as soon as the
  // engine has parsed them (ayana_load_world blocks until the render thread is done with them).
  @override
  bool loadWorld({
    required String worldJson,
    required String summaryJson,
    required String map,
    required String catalogJson,
  }) {
    final Pointer<Utf8> w = worldJson.toNativeUtf8();
    final Pointer<Utf8> s = summaryJson.toNativeUtf8();
    final Pointer<Utf8> m = map.toNativeUtf8();
    final Pointer<Utf8> c = catalogJson.toNativeUtf8();
    try {
      return _loadWorldFn(w, s, m, c) == 1;
    } finally {
      malloc.free(w);
      malloc.free(s);
      malloc.free(m);
      malloc.free(c);
    }
  }

  /// null / empty = no terrain: the world is built flat at rail height, which is not an error.
  @override
  int terrainIndex(Uint8List? bytes) => _withBytes(bytes, (Pointer<Uint8> p, int n) => _terrainIndexFn(p, n));

  /// `rgba` is tightly packed RGBA8, row 0 = north. DEM tiles are the raw Terrarium pixels.
  @override
  int terrainTileRgba(String dir, int z, int x, int y, int w, int h, Uint8List rgba) {
    final Pointer<Utf8> d = dir.toNativeUtf8();
    final Pointer<Uint8> buf = malloc.allocate<Uint8>(rgba.length);
    try {
      buf.asTypedList(rgba.length).setAll(0, rgba);
      return _tileRgbaFn(d, z, x, y, w, h, buf);
    } finally {
      malloc.free(buf);
      malloc.free(d);
    }
  }

  @override
  void terrainTileFail(String dir, int z, int x, int y) => _withStr(dir, (Pointer<Utf8> p) => _tileFailFn(p, z, x, y));

  @override
  int cityJson(Uint8List? bytes) => _withBytes(bytes, (Pointer<Uint8> p, int n) => _cityJsonFn(p, n));

  @override
  void modelFail(String slot) => _withStr(slot, _modelFailFn);
  void modelTexturesUnavailable(String slot) => _withStr(slot, _texUnavailFn);

  /// The bridge's per-frame `step` object. Fire and forget.
  void setState(String stepJson) => _withStr(stepJson, _setStateFn);

  void pointer(double x, double y, int button, int phase) => _pointerFn(x, y, button, phase);
  void compassClick(double x, double y) => _compassClickFn(x, y);
  int cameraMode(int mode) => _cameraModeFn(mode);
  int setQuality(int tier) => _setQualityFn(tier);
  void setSkyTime(double seconds) => _setSkyTimeFn(seconds);

  /// The engine's last failure message, "" when none.
  @override
  String lastError() {
    const int cap = 1024;
    final Pointer<Utf8> buf = malloc.allocate<Uint8>(cap).cast<Utf8>();
    try {
      final int n = _lastErrorFn(buf, cap);
      return n <= 0 ? '' : buf.toDartString();
    } finally {
      malloc.free(buf);
    }
  }

  void _withStr(String s, void Function(Pointer<Utf8>) fn) {
    final Pointer<Utf8> p = s.toNativeUtf8();
    try {
      fn(p);
    } finally {
      malloc.free(p);
    }
  }

  int _withBytes(Uint8List? bytes, int Function(Pointer<Uint8>, int) fn) {
    if (bytes == null || bytes.isEmpty) return fn(nullptr, 0);
    final Pointer<Uint8> buf = malloc.allocate<Uint8>(bytes.length);
    try {
      buf.asTypedList(bytes.length).setAll(0, bytes);
      return fn(buf, bytes.length);
    } finally {
      malloc.free(buf);
    }
  }

  /// Creates the texture, hands its Surface to the engine and starts the render thread.
  /// The font must be installed *before* this: the engine loads it inside eng_init.
  Future<int> start({required int width, required int height, double dpr = 1.0}) async {
    if (_textureId != null) return _textureId!;
    await setFont(await rootBundle.load('packages/ayana/assets/font.efnt'));
    final int id = await _channel.invokeMethod<int>('create', <String, dynamic>{
      'width': width,
      'height': height,
      'dpr': dpr,
    }) as int;
    _textureId = id;
    _poller ??= Timer.periodic(const Duration(milliseconds: 100), (_) => _drainRequests());
    return id;
  }

  Future<void> resize(int width, int height, double dpr) =>
      _channel.invokeMethod<void>('resize', <String, dynamic>{'width': width, 'height': height, 'dpr': dpr});

  Future<void> stop() async {
    _poller?.cancel();
    _poller = null;
    if (_textureId == null) return;
    _textureId = null;
    await _channel.invokeMethod<void>('dispose');
    _shutdown();
  }

  /// Copies the .efnt into native memory (the engine keeps its own copy).
  Future<void> setFont(ByteData font) async {
    final Uint8List bytes = font.buffer.asUint8List(font.offsetInBytes, font.lengthInBytes);
    final Pointer<Uint8> buf = malloc.allocate<Uint8>(bytes.length);
    try {
      buf.asTypedList(bytes.length).setAll(0, bytes);
      _setFont(buf, bytes.length);
    } finally {
      malloc.free(buf);
    }
  }

  /// Loads an `.emod` (convert --target android) into the catalog slot `slot`. Returns 1 on success.
  @override
  int loadModel(String slot, Uint8List bytes) {
    final Pointer<Utf8> name = slot.toNativeUtf8();
    final Pointer<Uint8> buf = malloc.allocate<Uint8>(bytes.length);
    try {
      buf.asTypedList(bytes.length).setAll(0, bytes);
      return _modelBeginFn(name, buf, bytes.length);
    } finally {
      malloc.free(buf);
      malloc.free(name);
    }
  }

  Future<int> loadBundledModel(String slot, String assetPath) async {
    final ByteData data = await rootBundle.load(assetPath);
    return loadModel(slot, data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes));
  }

  @override
  bool get ready => _readyFn() != 0;
  double get fps => _fpsFn();
  void orbit(double dx, double dy) => _orbitFn(dx, dy);
  void zoom(double steps) => _zoomFn(steps);
  void setView(double distance, double yaw, double pitch) => _setViewFn(distance, yaw, pitch);
  void setTheme(bool dark) => _setThemeFn(dark ? 1 : 0);
  void setTargetFps(double fps) => _setTargetFps(fps);

  /// The engine's stats JSON (fps, draw calls, pending assets, ...).
  String stats() {
    const int cap = 4096;
    final Pointer<Utf8> buf = malloc.allocate<Uint8>(cap).cast<Utf8>();
    try {
      final int n = _statsFn(buf, cap);
      return n <= 0 ? '{}' : buf.toDartString();
    } finally {
      malloc.free(buf);
    }
  }

  // Polling rather than a NativeCallable.listener: the engine asks for assets in bursts after
  // eng_load_world, never per frame, and polling keeps the C side free of any Dart API.
  void _drainRequests() {
    const int cap = 512;
    final Pointer<Utf8> kind = malloc.allocate<Uint8>(cap).cast<Utf8>();
    final Pointer<Utf8> path = malloc.allocate<Uint8>(cap).cast<Utf8>();
    try {
      for (int i = 0; i < 64; i++) {
        if (_pollFn(kind, cap, path, cap) == 0) break;
        _requests.add(AyanaAssetRequest(kind.toDartString(), path.toDartString()));
      }
    } finally {
      malloc.free(kind);
      malloc.free(path);
    }
  }
}

/// The engine as a widget: a `Texture` sized to its box, orbit/zoom by touch, fps in a corner.
class AyanaView extends StatefulWidget {
  const AyanaView({super.key, this.showFps = true, this.onReady});

  final bool showFps;
  final VoidCallback? onReady;

  @override
  State<AyanaView> createState() => _AyanaViewState();
}

class _AyanaViewState extends State<AyanaView> with WidgetsBindingObserver {
  int? _textureId;
  String? _error;
  Size _size = Size.zero;
  double _dpr = 1;
  double _fps = 0;
  Timer? _hud;

  @override
  void dispose() {
    _hud?.cancel();
    Ayana.instance.stop();
    super.dispose();
  }

  Future<void> _ensureStarted(Size size, double dpr) async {
    final int w = (size.width * dpr).round().clamp(1, 4096);
    final int h = (size.height * dpr).round().clamp(1, 4096);
    if (_textureId == null) {
      _size = size;
      _dpr = dpr;
      try {
        final int id = await Ayana.instance.start(width: w, height: h, dpr: dpr);
        if (!mounted) return;
        setState(() => _textureId = id);
        _hud = Timer.periodic(const Duration(milliseconds: 500), (_) {
          if (mounted) setState(() => _fps = Ayana.instance.fps);
        });
        widget.onReady?.call();
      } catch (e) {
        if (mounted) setState(() => _error = '$e');
      }
    } else if (size != _size || dpr != _dpr) {
      _size = size;
      _dpr = dpr;
      await Ayana.instance.resize(w, h, dpr);
    }
  }

  @override
  Widget build(BuildContext context) {
    final double dpr = MediaQuery.of(context).devicePixelRatio;
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints c) {
        final Size size = Size(c.maxWidth, c.maxHeight);
        WidgetsBinding.instance.addPostFrameCallback((_) => _ensureStarted(size, dpr));
        if (_error != null) {
          return ColoredBox(
            color: Colors.black,
            child: Center(child: Text(_error!, style: const TextStyle(color: Colors.redAccent))),
          );
        }
        if (_textureId == null) {
          return const ColoredBox(color: Colors.black, child: Center(child: CircularProgressIndicator()));
        }
        return Stack(
          fit: StackFit.expand,
          children: <Widget>[
            GestureDetector(
              behavior: HitTestBehavior.opaque,
              onScaleUpdate: (ScaleUpdateDetails d) {
                if (d.pointerCount > 1) {
                  Ayana.instance.zoom((1 - d.scale) * 2);
                } else {
                  Ayana.instance.orbit(d.focalPointDelta.dx * dpr, d.focalPointDelta.dy * dpr);
                }
              },
              child: Texture(textureId: _textureId!),
            ),
            if (widget.showFps)
              Positioned(
                left: 8,
                top: 8,
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                  color: Colors.black54,
                  child: Text('${_fps.toStringAsFixed(0)} fps',
                      style: const TextStyle(color: Colors.white, fontSize: 12)),
                ),
              ),
          ],
        );
      },
    );
  }
}
