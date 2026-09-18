// The M3 bridge: one message per frame from the page's JavaScript, one snapshot back.
//
// The simulation and the whole DOM UI stay in the WebView; only the world renderer moved into
// libayana.so. So every engine call the page makes is appended to a per-frame batch, sent once
// through `flutter_inappwebview`'s `callHandler('ayana', batch)`, and answered with the snapshot the
// render thread built after its last frame. Writes are fire-and-forget (the sim already decided them);
// reads come from the snapshot and are therefore one frame old — which is exactly what the DOM labels
// already tolerate on the web, where they are drawn from the previous frame's projection too.
//
// Nothing here blocks: `ayana_cmd` and `ayana_set_state` post to the render thread, `ayana_snapshot`
// copies a string the render thread swapped in. A batch handler that waited on the render thread is
// what froze the UI isolate during the 9 s decor build.
import 'dart:convert';

import '../ayana.dart';

/// Round-trip timing, so the cost of the bridge is a number and not a feeling.
class AyanaLatency {
  AyanaLatency({this.window = 300});
  final int window;
  final List<double> _ms = <double>[];
  int _count = 0;

  void add(double ms) {
    _count++;
    if (_ms.length >= window) _ms.removeAt(0);
    _ms.add(ms);
  }

  int get count => _count;
  double get p50 => _percentile(0.50);
  double get p95 => _percentile(0.95);

  double _percentile(double p) {
    if (_ms.isEmpty) return 0;
    final List<double> sorted = List<double>.of(_ms)..sort();
    final int i = ((sorted.length - 1) * p).round();
    return sorted[i];
  }

  @override
  String toString() => 'n=$_count p50=${p50.toStringAsFixed(2)}ms p95=${p95.toStringAsFixed(2)}ms';
}

/// Applies one frame's batch and returns the engine's latest snapshot.
///
/// A batch is a JSON array of commands. Two shapes only:
///   {"c":"state","s":"<step json>"}            -> eng_set_state
///   {"c":"<name>","a":1,"b":2,"d":3,"s":"id"}  -> the named engine command (see ayana_cmd)
///   {"c":"pointer","a":x,"b":y,"d":button,"e":phase}
class AyanaBridge {
  AyanaBridge({AyanaCommandSink? engine}) : _engine = engine ?? Ayana.instance;

  final AyanaCommandSink _engine;
  final AyanaLatency latency = AyanaLatency();
  bool _enabled = false;

  /// Turns the engine's per-frame snapshot on. Costs a few hundred microseconds of JSON per frame, so
  /// it stays off until a page actually asks for it.
  void enable() {
    if (_enabled) return;
    _enabled = true;
    _engine.snapshotEnable(true);
  }

  void disable() {
    if (!_enabled) return;
    _enabled = false;
    _engine.snapshotEnable(false);
  }

  /// `batch` is what `callHandler('ayana', batch)` delivered: a List of maps, or the JSON text of one.
  /// Returns the snapshot JSON text to hand straight back to the page.
  String handleBatch(Object? batch) {
    final Stopwatch sw = Stopwatch()..start();
    enable();
    final List<dynamic> cmds = _asList(batch);
    for (final dynamic raw in cmds) {
      if (raw is! Map) continue;
      final String c = (raw['c'] ?? '') as String;
      if (c.isEmpty) continue;
      final String s = (raw['s'] ?? '') as String;
      if (c == 'state') {
        _engine.setState(s.isEmpty ? '{}' : s);
        continue;
      }
      final double a = _num(raw['a']), b = _num(raw['b']), d = _num(raw['d']);
      if (c == 'pointer') {
        _engine.pointer(a, b, d.toInt(), _num(raw['e']).toInt());
        continue;
      }
      _engine.command(c, a, b, d, s);
    }
    final String snap = _engine.snapshot();
    sw.stop();
    latency.add(sw.elapsedMicroseconds / 1000.0);
    return snap;
  }

  static List<dynamic> _asList(Object? batch) {
    if (batch is List) return batch;
    if (batch is String && batch.isNotEmpty) {
      final dynamic j = jsonDecode(batch);
      if (j is List) return j;
    }
    return const <dynamic>[];
  }

  static double _num(Object? v) => v is num ? v.toDouble() : 0;
}
