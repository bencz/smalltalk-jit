#ifndef NUMBER_H
#define NUMBER_H

// Boxed numbers: the ones that do not fit an immediate.
//
// A double lives in a tagged Value whenever its magnitude is in 2^[-255, 256]
// (core/Object.h), which is nearly always. This is the other case: subnormals,
// infinities, NaN, and anything outside that window.
//
// NOT reachable from a primitive. Allocating inside a primitive would let a
// collection walk native frames while rootsVisitNativeFrames is still a no-op,
// so the arithmetic primitives FAIL instead of boxing (runtime/Primitive.h).
// This is for C code that holds its operands in handle scopes: the parser
// building a literal, and the bootstrap.

#include "core/Handle.h"
#include "core/Object.h"

Float *newFloat(double value);
// The tagged Value for a double: an immediate when it fits, a fresh
// BoxedFloat64 when it does not. One place decides, so no caller has to
// remember the window.
Value floatValue(double value);

#endif
