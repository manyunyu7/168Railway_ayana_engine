// PNG -> RGBA8 for the engine, with the 2x2 composition a 512 px layer needs. dart:ui only (Flutter's
// own Skia): no third-party image package, and the same pixels the web adapter produces from a canvas.
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'tile_urls.dart';

/// One decoded source image plus where it goes.
class DecodedSub {
  const DecodedSub(this.image, this.sx, this.sy);
  final ui.Image image;
  final int sx, sy;
}

/// Decodes a PNG (or any format Skia knows) to a `ui.Image`.
Future<ui.Image> decodeImage(Uint8List bytes) async {
  final ui.Codec codec = await ui.instantiateImageCodec(bytes);
  final ui.FrameInfo frame = await codec.getNextFrame();
  codec.dispose();
  return frame.image;
}

/// Straight (non-premultiplied) RGBA8, row 0 = the north edge — what eng_terrain_tile_rgba expects.
Future<Uint8List> toRgba(ui.Image image) async {
  final ByteData? d = await image.toByteData(format: ui.ImageByteFormat.rawStraightRgba);
  if (d == null) throw StateError('toByteData failed');
  return d.buffer.asUint8List(d.offsetInBytes, d.lengthInBytes);
}

/// Draws the decoded sub-tiles into one `px` x `px` image. Missing cells stay the grey the web adapter
/// uses (#606060), so a partly available tile is still usable instead of being dropped.
Future<Uint8List> composeRgba(TileComposition c, List<DecodedSub> subs) async {
  if (c.n == 1 && subs.length == 1 && subs.first.image.width == c.px && subs.first.image.height == c.px) {
    return toRgba(subs.first.image);
  }
  final ui.PictureRecorder rec = ui.PictureRecorder();
  final ui.Canvas canvas = ui.Canvas(rec, ui.Rect.fromLTWH(0, 0, c.px.toDouble(), c.px.toDouble()));
  canvas.drawRect(
    ui.Rect.fromLTWH(0, 0, c.px.toDouble(), c.px.toDouble()),
    ui.Paint()..color = const ui.Color(0xFF606060),
  );
  for (final DecodedSub s in subs) {
    final ui.Rect src = ui.Rect.fromLTWH(0, 0, s.image.width.toDouble(), s.image.height.toDouble());
    final ui.Rect dst = ui.Rect.fromLTWH(
      (s.sx * c.sub).toDouble(),
      (s.sy * c.sub).toDouble(),
      c.sub.toDouble(),
      c.sub.toDouble(),
    );
    canvas.drawImageRect(s.image, src, dst, ui.Paint()..filterQuality = ui.FilterQuality.low);
  }
  final ui.Picture picture = rec.endRecording();
  final ui.Image out = await picture.toImage(c.px, c.px);
  picture.dispose();
  final Uint8List rgba = await toRgba(out);
  out.dispose();
  return rgba;
}
