#ifndef THREAD_H
#define THREAD_H

#include "Heap.h"

struct HandleScope;
struct StackFrame;
struct EntryStackFrame;

typedef struct Thread {
	Heap heap;
	struct Handle *handles;
	struct HandleScope *handleScopes;
	Value context;
	struct EntryStackFrame *stackFramesTail;
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
		rememberedSetAdd(&CurrentThread.heap.rememberedSet, object);
	}
	*field = tagPtr(value);
}


static inline void objectStorePtr(Object *object, Value *field, Object *value)
{
	rawObjectStorePtr(object->raw, field, value->raw);
}

#endif
