#include "memory/Roots.h"

// Native-frame roots, weakly defined so a build with NO execution engine links
// and collects (gate level 1, docs/jit-v2/01-gate.md). The JIT overrides this
// with the real walk when it exists.
//
// Weak rather than #ifdef on purpose: an #ifdef would let a build silently
// scan no frames because a define was missing, and "the collector quietly
// stopped seeing a whole root set" is the exact failure mode the dry cut left
// us least able to detect.
__attribute__((weak))
void rootsVisitNativeFrames(struct Thread *thread, RootVisitor visit, void *ctx)
{
	(void) thread;
	(void) visit;
	(void) ctx;
}


__attribute__((weak))
void rootsVisitCompiledCode(RootVisitor visit, void *ctx)
{
	(void) visit;
	(void) ctx;
}


__attribute__((weak))
void rootsVisitUnwindRecords(struct Thread *thread, RootVisitor visit, void *ctx)
{
	(void) thread;
	(void) visit;
	(void) ctx;
}


// No scheduler linked: there are no fibers besides the one this thread is
// running, and its roots are the Thread's (memory/Roots.h).
__attribute__((weak))
void rootsVisitFibers(RootVisitor visit, void *ctx)
{
	(void) visit;
	(void) ctx;
}
