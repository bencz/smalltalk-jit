#ifndef THREAD_H
#define THREAD_H

#include "Heap.h"

struct HandleScope;
struct StackFrame;
struct EntryStackFrame;
struct Fiber;

// Per-mutator thread-local allocation buffer: a chunk carved from the shared
// young space that this OS thread bump-allocates into WITHOUT locking (Go's
// mcache idea). `top` carries NEW_SPACE_TAG like Scavenger.top; `end` is
// untagged; free bytes = end - top. Refilled from the nursery when exhausted.
typedef struct TLAB {
	uint8_t *top;
	uint8_t *end;
} TLAB;

typedef struct Thread {
	Heap *heap; // heap-allocated; ONE heap per isolate, shared by its worker threads
	struct Handle *handles;
	struct HandleScope *handleScopes;
	Value context;
	struct EntryStackFrame *stackFramesTail;
	TLAB tlab; // per-OS-thread young allocation buffer (stays embedded, per-mutator)
	RememberedSet rememberedSet; // per-mutator old->young log (merged at GC in the multicore model)
	struct Thread *nextMutator; // intrusive link in heap->mutators (GC root-scans every mutator)
	int spBlocked;              // mutator is in a blocking native wait (counts as safe for GC)
	int spAtSafepoint;          // mutator parked at a safepoint poll
	// Published by schedulerInit so a cross-thread GC collector can reach THIS
	// mutator's fibers (the Fiber structs are heap-allocated; only the registry
	// array + count + current are per-OS-thread TLS, so we expose their addresses).
	struct Fiber ***schedFibers;
	size_t *schedFiberSlots;
	struct Fiber **schedCurrent;
	// Address of this mutator's TLS CurrentExceptionHandler (a Value holding the head
	// of its running fiber's on:do: chain), so a cross-thread GC collector can scan a
	// peer's live handler chain — it is a root reachable ONLY through this slot, not
	// the Smalltalk stack. NULL until published (initThread / schedulerInit / worker
	// registration). Stale for a scheduler mutator parked in the scheduler context.
	Value *schedExceptionHandler;
} Thread;

extern __thread Thread CurrentThread;

// Per-isolate VM globals (Handles, the scheduler, LookupCache, JIT stubs, ...)
// are thread-local so every isolate OS-thread owns its own copy. They MUST use
// the initial-exec TLS model: libVM.so is linked directly into `st` (never
// dlopen'd) so IE is valid, and it is required for correctness — the default
// general-dynamic model resolves a __thread address via a __tls_get_addr call,
// which misbehaves when such a global is touched from JIT-generated code's C
// callouts during a GC. IE is a direct %fs-relative access (also faster).
#define PER_ISOLATE __thread __attribute__((tls_model("initial-exec")))

void initThread(Thread *thread);
void initThreadContext(Thread *thread);
void freeThread(Thread *thread);
void threadSetExitFrame(struct StackFrame *stackFrame);


static inline void rawObjectStorePtr(RawObject *object, Value *field, RawObject *value)
{
	if (isOldObject(object) && isNewObject(value) && (object->tags & TAG_REMEMBERED) == 0) {
		rememberedSetAdd(&CurrentThread.rememberedSet, object);
	}
	*field = tagPtr(value);
}


static inline void objectStorePtr(Object *object, Value *field, Object *value)
{
	rawObjectStorePtr(object->raw, field, value->raw);
}

#endif
