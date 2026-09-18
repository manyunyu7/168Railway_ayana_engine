// Tile downloads: a FIFO pool of 6 in-flight requests (KONKUREN_UBIN in ubinAyana.ts) and a 20 s
// deadline per fetch, over dart:io's own HttpClient — nothing third-party, and the platform stack
// already speaks gzip and brotli.
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

/// Fetches tiles. Abstracted so tests can answer without a network.
abstract class AyanaTileFetcher {
  /// Tries each URL in order and returns the first body that arrives, or null.
  Future<Uint8List?> first(List<String> urls);
  void close();
}

class HttpTileFetcher implements AyanaTileFetcher {
  HttpTileFetcher({this.concurrency = 6, this.timeout = const Duration(seconds: 20)});

  final int concurrency;
  final Duration timeout;
  final HttpClient _client = HttpClient()..autoUncompress = true;
  int _busy = 0;
  final List<Completer<void>> _waiting = <Completer<void>>[];

  Future<void> _acquire() async {
    if (_busy < concurrency) {
      _busy++;
      return;
    }
    final Completer<void> c = Completer<void>();
    _waiting.add(c);
    await c.future;
  }

  void _release() {
    if (_waiting.isNotEmpty) {
      _waiting.removeAt(0).complete();
      return;
    }
    _busy--;
  }

  @override
  Future<Uint8List?> first(List<String> urls) async {
    await _acquire();
    try {
      for (final String url in urls) {
        try {
          final HttpClientRequest req = await _client.getUrl(Uri.parse(url)).timeout(timeout);
          final HttpClientResponse res = await req.close().timeout(timeout);
          if (res.statusCode != 200) {
            await res.drain<void>();
            continue;
          }
          final BytesBuilder b = BytesBuilder(copy: false);
          await for (final List<int> chunk in res.timeout(timeout)) {
            b.add(chunk);
          }
          final Uint8List bytes = b.takeBytes();
          if (bytes.isNotEmpty) return bytes;
        } catch (_) {
          // try the next URL; a transient failure must not poison the whole tile
        }
      }
      return null;
    } finally {
      _release();
    }
  }

  @override
  void close() => _client.close(force: true);
}
