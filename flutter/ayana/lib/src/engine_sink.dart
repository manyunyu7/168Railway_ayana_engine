// What AyanaWorldLoader needs from the engine. `Ayana` implements it over FFI; a test implements it
// with a list of calls, which is how the routing is checked without a device or a .so.
import 'dart:typed_data';

import 'asset_request.dart';

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
