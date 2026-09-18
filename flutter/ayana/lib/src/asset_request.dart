import 'package:flutter/foundation.dart';

/// One asset the engine asked the host for (`kind` = terrain / city / model, `path` = what to fetch).
@immutable
class AyanaAssetRequest {
  const AyanaAssetRequest(this.kind, this.path);
  final String kind;
  final String path;

  @override
  bool operator ==(Object other) => other is AyanaAssetRequest && other.kind == kind && other.path == path;

  @override
  int get hashCode => Object.hash(kind, path);

  @override
  String toString() => 'AyanaAssetRequest($kind, $path)';
}
