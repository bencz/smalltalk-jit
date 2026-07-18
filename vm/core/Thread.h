#ifndef THREAD_H
#define THREAD_H

#include "memory/Heap.h"

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

// The pointer sites of an IN-FLIGHT codegen buffer. Codegen runs GC-active and
// allocates young objects (a stackmap per send), so a scavenge can move an
// object whose address was already baked into the malloc'd assembler buffer,
// which the collectors cannot see on their own: methods published with a stale
// from-space immediate dispatch on forwarded garbage. Every live buffer
// registers its sites here (asmInitBuffer/asmFreeBuffer, LIFO since block
// codegen nests inside method codegen) and both collectors walk the list of
// every registered mutator. Indirections, not copies: the buffer reallocs on
// growth (insts AND offsets, both heap arrays now) and keeps baking (count).
// The walk only happens with the world stopped (the owning mutator is parked),
// so the double dereference always observes the post-realloc pointers.
typedef struct CodegenSites {
	uint8_t **insts;              // &buffer->buffer
	uint32_t **offsets;           // &buffer->pointersOffsets (array reallocs on growth)
	size_t *count;                // &buffer->pointersOffsetsSize
	struct CodegenSites *next;
} CodegenSites;

typedef struct Thread {
	Heap *heap; // heap-allocated; ONE heap per isolate, shared by its worker threads
	struct Handle *handles;
	struct HandleScope *handleScopes;
	Value context;
	struct EntryStackFrame *stackFramesTail;
	// Head of this mutator's on:do: handler chain (was a standalone TLS `CurrentExceptionHandler`).
	// Per-mutator so the JIT reaches it via CTX->thread (like tlab/stackFramesTail) instead of a
	// baked TLS address — a baked address in SHARED JIT code would be the CODEGEN thread's slot,
	// breaking on:do: on every other worker. A fiber's saved handler is loaded here on resume; the
	// B2.5 context rebind keeps CTX->thread pointing at the running worker.
	Value exceptionHandler;
	// Head of this mutator's ensure:/ifCurtailed: unwind-handler chain (same discipline
	// as exceptionHandler: per-mutator, reached from JIT code via TLS, saved/restored per
	// fiber). Entries are UnwindHandler objects; the unwinders (exception, non-local
	// return, terminate) run and unlink them, so everything on the chain is pending.
	Value unwindHandler;
	TLAB tlab; // per-OS-thread young allocation buffer (stays embedded, per-mutator)
	// 8-byte scratch for the PPC baseline's GPR<->FPR raw bit moves (mtvsrd/
	// mfvsrd only exist from ISA 2.07 on): the JIT stores and immediately
	// reloads through here via TLS, straight-line. Never a GC root.
	uint64_t jitFpuScratch;
	RememberedSet rememberedSet; // per-mutator old->young log (merged at GC in the multicore model)
	CodegenSites *codegenSites; // in-flight codegen buffers whose baked pointers the GC must fix
	size_t lookupCacheEpoch; // last heap->gcEpoch this thread's TLS LookupCache was flushed at
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
	// Same publication for the unwind-handler chain head (see unwindHandler above).
	Value *schedUnwindHandler;
	// Striped sync monitor bookkeeping. The stripe for a sync object is computed ONCE at
	// monitorEnterOn: (mix of obj->hash) and stashed here; monitorExit/parkOnMonitor read
	// it back and NEVER recompute — so enter/exit/park provably drop the same lock they
	// took even if the object moved/was become:-d mid-critical-section. `heldMonitor`
	// makes a reentrant monitorEnter fail-fast (critical sections must stay flat). Valid
	// only while heldMonitor==1. APPENDED at struct end so JIT-baked field offsets are
	// unchanged (the StoreCheck golden reads Thread offsets).
	size_t heldMonitorStripe;
	_Bool heldMonitor;
} Thread;

extern __thread Thread CurrentThread;

// Offset of &CurrentThread from the OS thread pointer (%fs base) for the initial-exec
// TLS model — a link-time constant, identical on every thread. Computed once in
// initThread. JIT code bakes this to read EACH running worker's CurrentThread via
// %fs:tpoff (see asmLoadTls), instead of a per-thread address that would be wrong in
// shared code.
extern ptrdiff_t gCurrentThreadTpoff;

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
