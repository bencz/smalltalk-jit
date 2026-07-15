#ifndef SAFEPOINT_H
#define SAFEPOINT_H

// ---------------------------------------------------------------------------
// PORT_ME(memory-model): all cross-thread protocols in the VM (this safepoint
// handshake, JIT code publish, remembered sets) use __atomic acquire/release
// builtins — portable in principle, but validated only under x86-TSO. Audit
// every publish/observe pair before the first weak-memory port (ppc64/arm/riscv).
// ---------------------------------------------------------------------------
// Cooperative stop-the-world safepoint coordination for the multicore VM.
//
// This is the backbone that lets N mutator OS threads share one heap: to run a
// stop-the-world GC (or any operation that needs the object graph frozen), a
// coordinator calls safepointBegin(); every other mutator thread reaches a
// safepoint and parks; the coordinator runs exclusively; then safepointEnd()
// releases everyone. It leans on the fibers being cooperative and non-preemptive
// (the mutator only has to POLL at chosen points), exactly like the HotSpot /
// Go handshake.
//
// A mutator thread is in one of two states:
//   SP_ACTIVE  -- running managed code; it MUST call safepointPoll() often
//                 enough that a requested safepoint is reached promptly.
//   SP_BLOCKED -- parked in a syscall / native wait where it will not touch the
//                 heap. Its roots are already saved (stable), so it counts as
//                 "safe" for GC without having to poll; but it cannot return to
//                 SP_ACTIVE while a safepoint is in progress.
//
// The fast path (safepointPoll) is a single acquire-load of a global flag, so it
// is cheap enough to inline at every send back-edge / allocation slow path.
// ---------------------------------------------------------------------------

enum { SP_ACTIVE = 0, SP_BLOCKED = 1 };

typedef struct SafepointThread {
	int state;                    // SP_ACTIVE / SP_BLOCKED (guarded by the safepoint lock)
	int atSafepoint;              // parked at a poll while a safepoint is in progress
	struct SafepointThread *next; // intrusive registry linkage
} SafepointThread;

// Hot-path flag: read locklessly by safepointPoll(); set/cleared under the lock
// by safepointBegin()/safepointEnd().
extern int gSafepointRequested;

void safepointInit(void);
void safepointRegister(SafepointThread *thread);
void safepointUnregister(SafepointThread *thread);

// Slow path (out of line): park until the in-progress safepoint is released.
void safepointBlock(SafepointThread *thread);

// Fast path: one acquire-load; park only if a safepoint is actually in progress.
static inline void safepointPoll(SafepointThread *thread)
{
	if (__atomic_load_n(&gSafepointRequested, __ATOMIC_ACQUIRE)) {
		safepointBlock(thread);
	}
}

// Bracket a blocking syscall / native wait. Between Enter and Leave the thread
// counts as safe (its roots are stable); Leave blocks if a safepoint is running.
void safepointEnterBlocked(SafepointThread *thread);
void safepointLeaveBlocked(SafepointThread *thread);

// Coordinator: stop the world — return only once every registered thread except
// `exclude` (the caller's own record, or NULL) is parked at a safepoint or
// blocked — then safepointEnd() to resume everyone.
void safepointBegin(SafepointThread *exclude);
void safepointEnd(void);

// Standalone pthread self-test (no heap/image). ST_SAFEPOINT_TEST=1 ./st
int safepointSelfTest(void);

#endif
