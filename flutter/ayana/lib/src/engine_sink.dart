// What AyanaWorldLoader needs from the engine. `Ayana` implements it over FFI; a test implements it
// with a list of calls, which is how the routing is checked without a device or a .so.
import 'dart:typed_data';

import 'asset_request.dart';

/// What the M3 bridge needs from the engine: post a command, hand over the frame's state, read the
/// latest snapshot. Separate from AyanaEngineSink so a test can drive the batch handling alone.
abstract class AyanaCommandSink {
  void setState(String stepJson);
  void pointer(double x, double y, int button, int phase);
  void command(String name, double a, double b, double c, String s);
  void snapshotEnable(bool on);
  String snapshot();
}

abstract class AyanaEngineSink {
  Stream<AyanaAssetRequest> get assetRequests;
  bool get ready;
  String lastError();

  bool loadWorld({
    required String worldJson,
    required String summaryJson,
    required String map,
    required String catalogJson,
  });

  int terrainIndex(Uint8List? bytes);
  int terrainTileRgba(String dir, int z, int x, int y, int w, int h, Uint8List rgba);
  void terrainTileFail(String dir, int z, int x, int y);
  int cityJson(Uint8List? bytes);
  int loadModel(String slot, Uint8List bytes);
  void modelFail(String slot);
}
