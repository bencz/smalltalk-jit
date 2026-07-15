#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "core/Object.h"
#include <stdio.h>
#include <stdint.h>

// Image format v2. The file opens with an 8-byte self-describing header:
//
//   bytes 0-3  magic "STIM"
//   byte  4    format version (SNAPSHOT_FORMAT_VERSION)
//   byte  5    byte order (1 = little-endian, 2 = big-endian)
//   byte  6    word size in bytes (8)
//   byte  7    reserved (0)
//
// The object stream that follows stays NATIVE-endian: images are per-build
// artifacts (re-bootstrapping takes seconds) and heap Values that were punned
// from C structs make true cross-endian portability a lie — so instead of
// byte-swapping, the loader REFUSES a foreign-endian or legacy image with an
// actionable message. See PORTING.md "endianness".
#define SNAPSHOT_MAGIC "STIM"
#define SNAPSHOT_FORMAT_VERSION 2
#define SNAPSHOT_BYTE_ORDER_LITTLE 1
#define SNAPSHOT_BYTE_ORDER_BIG 2

void snapshotWrite(FILE *file);
void snapshotRead(FILE *file);

// ---- format primitives (unit-testable without a heap: ST_SNAPSHOT_FORMAT_TEST) --

// InstanceShape <-> a DEFINED uint64 layout (byte i = field i, size in bytes
// 6-7 low-first): pure shift arithmetic, so the encoding is independent of
// struct padding, field order changes and host endianness — unlike the old
// *(int64_t *)&shape reinterpret it replaces.
uint64_t snapshotEncodeShape(InstanceShape shape);
InstanceShape snapshotDecodeShape(uint64_t bits);

// Persisted object header: identity hash + payload/vars sizes packed
// explicitly (tags are never persisted; the loader starts every object clean).
uint64_t snapshotEncodeObjectHeader(uint32_t hash, uint8_t payloadSize, uint8_t varsSize);
void snapshotDecodeObjectHeader(uint64_t bits, uint32_t *hash, uint8_t *payloadSize, uint8_t *varsSize);

void snapshotWriteHeader(FILE *file);
// 0 = valid v2 image for this host; nonzero = invalid, with an actionable
// message in err (legacy/corrupt image, foreign endianness, wrong word size).
int snapshotCheckHeader(FILE *file, char *err, size_t errSize);

#endif
