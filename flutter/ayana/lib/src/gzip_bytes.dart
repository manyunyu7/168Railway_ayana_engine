// `.emod` files are uploaded pre-compressed (docs/TEXTURE-FORMAT.md: Cloudflare will not gzip
// application/octet-stream for us, so the object is stored gzipped with Content-Encoding: gzip).
// Whether the HTTP stack already inflated it or handed us the raw object — and whether the file on disk
// is the compressed or the plain one — the loader looks at the two magic bytes and does the right thing.
import 'dart:io';
import 'dart:typed_data';

bool looksGzipped(Uint8List b) => b.length > 2 && b[0] == 0x1f && b[1] == 0x8b;

Uint8List maybeGunzip(Uint8List b) => looksGzipped(b) ? Uint8List.fromList(gzip.decode(b)) : b;
