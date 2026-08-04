#ifndef TLS_H
#define TLS_H

// Per-isolate VM globals are thread-local so every isolate OS thread owns its
// own copy. They MUST use the initial-exec TLS model: libVM.so is linked
// directly into the executable (never dlopen'd) so IE is valid, and it is
// required for correctness, not speed. The default general-dynamic model
// resolves a __thread address through a __tls_get_addr call, which misbehaves
// when such a global is touched from a callout made by generated code during a
// collection. IE is a direct thread-pointer-relative access.
//
// This lives in a header of its own so memory/Heap.h can declare per-isolate
// globals without including core/Thread.h, which includes Heap.h back.

#define PER_ISOLATE __thread __attribute__((tls_model("initial-exec")))

#endif
