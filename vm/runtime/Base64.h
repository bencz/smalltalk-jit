#ifndef BASE64_H
#define BASE64_H

#include "core/Object.h"
#include "core/Class.h"
#include "runtime/String.h"

// Fast base-64 runtime backing packages/Core/src/Base64.st (which is also the
// validating fallback and the reference implementation — keep semantics in
// sync). RFC 4648, standard alphabet, '=' padding, no line breaks; the
// base64url variant lives in the .st on top of these two.
//
// base64Encode: encode the bytes of `input` (a String or ByteArray HANDLE;
// the wrapper has already checked the exact class) into a padded base-64
// String (*result, handled in the caller's scope). Never fails.
_Bool base64Encode(Object *input, String **result);

// base64Decode: strict decode of `input` (a String handle) into a new
// byte-shaped instance of `outputClass` (Handles.String or Handles.ByteArray).
// Validates BEFORE allocating (length % 4, alphabet membership, '=' only as
// final padding) and returns 0 on malformed input WITHOUT allocating — the
// Smalltalk fallback then re-scans and raises the precise
// InvalidArgumentError.
_Bool base64Decode(String *input, Class *outputClass, Object **result);

#endif
