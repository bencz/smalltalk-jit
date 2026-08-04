#ifndef SNAPSHOT_H
#define SNAPSHOT_H

// The image: the object graph of a running heap, written out and read back.
//
// WHAT AN IMAGE HAS TO CARRY IN JIT-V2, and each of these is a difference from
// the format this file replaces:
//
//   * the CLASS TABLE, restored index for index. An object header names its
//     class by a 22-bit INDEX (ADR 0005), a guard in generated code compares
//     that index, and the class's own trailer repeats it. Renumbering on load
//     would be silent and total;
//   * CODE UNITS, which are malloc'd C structs and not heap objects. A
//     CompiledMethod reaches its bytecode through a raw word the collector never
//     follows (jit/CompiledMethod.h), so nothing about a unit is in the object
//     graph and all of it has to be written explicitly;
//   * NOTHING about native code. A loaded method has `native == NULL` and is
//     compiled again on its first send. That is not a shortcut: generated code
//     BAKES the addresses of nil, true and false as immediates, and those
//     addresses are different in the process that loads the image.
//
// THE STREAM IS NATIVE-ENDIAN and the header says which. An image is a
// per-build artifact, regenerated from packages/Core in seconds, and heap
// Values punned out of C structs make true cross-endian portability a claim
// this format cannot honestly make. So instead of byte-swapping, the loader
// REFUSES a foreign-endian, foreign-word-size or foreign-version image with a
// message that says how to regenerate it. See PORTING.md "endianness".
//
//   bytes 0-3  magic "STIM"
//   byte  4    format version (SNAPSHOT_FORMAT_VERSION)
//   byte  5    byte order (1 = little-endian, 2 = big-endian)
//   byte  6    word size in bytes (8)
//   byte  7    reserved (0)

#include "core/Object.h"
#include <stdint.h>
#include <stdio.h>

#define SNAPSHOT_MAGIC "STIM"
// v6: the jit-v2 object model. One-word object headers with a class INDEX, a
// restored class table, and code units written explicitly. Nothing before this
// version shares a single record with it.
// 7: CodeUnit gained `codeObject` (the CompiledMethod a unit belongs to, for
// Context materialisation), one more word per unit record.
#define SNAPSHOT_FORMAT_VERSION 7
#define SNAPSHOT_BYTE_ORDER_LITTLE 1
#define SNAPSHOT_BYTE_ORDER_BIG 2

// Both answer 0 on success. They REPORT rather than abort: writing an image is
// something a command line asked for, and a failed write has to become an exit
// code and not a core dump.
int snapshotWrite(FILE *file);
int snapshotRead(FILE *file);

void snapshotWriteHeader(FILE *file);
// 0 = an image this build can read; nonzero = refused, with an actionable
// message in `err` (legacy or corrupt image, foreign endianness, wrong word
// size).
int snapshotCheckHeader(FILE *file, char *err, size_t errSize);

#endif
