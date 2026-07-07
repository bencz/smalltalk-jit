#ifndef ISOLATE_H
#define ISOLATE_H

#include <stddef.h>
#include <stdint.h>

// Share-nothing isolates: N OS threads, each a full independent VM instance
// (its own heap, GC, scheduler, JIT and kernel image). They share no mutable
// object memory — every per-isolate VM global lives in initial-exec TLS (see
// PER_ISOLATE in Thread.h), so spawning an OS thread gives it a fresh set.
//
// Communication is by COPYING bytes: each isolate owns a thread-safe INBOX (an
// eventfd + a mutex-protected MPSC queue of buffers) in the shared `gIsolates`
// registry. Any thread posts a buffer with isolatePostBytes(); the target's
// fibers consume it via isolateInboxPop(), blocking on isolateInboxFd() through
// the ordinary scheduler fd-wait. The VM is deliberately agnostic about what the
// bytes mean — object (de)serialization and any actor semantics live above it.

#define MAX_ISOLATES 256

// The image the main VM booted from; workers reload it to get their own copy of
// the shared code. Set once by main() before any spawn.
void isolateSetSnapshotPath(const char *path);

// Spawn a WORKER isolate from within the running program (like Thread/Worker/
// Ractor, NOT a second copy of the process): a fresh VM on a new OS thread that
// reloads the image and runs `source` as its entry program (source is portable;
// a live block/closure is not — a worker uses the shared classes + copied data).
// The caller keeps running. Returns the worker's isolate id, or -1 on failure.
int isolateSpawn(const char *source);

// Join every worker spawned by this process. Call at process exit, after the
// main program's own work has drained, so worker threads finish cleanly.
void isolateJoinWorkers(void);

// Create this isolate's inbox (eventfd + MPSC queue). Call once after
// schedulerInit(). `id` becomes isolateCurrentId() (0 for the main isolate).
void isolateInboxInit(int id);
int isolateCurrentId(void);

// Post `size` bytes to isolate `target`'s inbox and wake it. Thread-safe; the
// bytes are copied. Returns 0 on success, -1 if the target isn't ready.
int isolatePostBytes(int target, const uint8_t *bytes, size_t size);

// Consume from THIS isolate's inbox. isolateInboxPop copies the next buffer into
// a fresh malloc'd block (*outBytes, caller frees) and returns 1, or returns 0
// if the inbox is empty. isolateInboxFd is the eventfd a fiber waits on for the
// "buffer available" signal; isolateInboxClearSignal drains that signal after a
// wake. (A single waiter fiber per isolate is assumed.)
int isolateInboxPop(uint8_t **outBytes, size_t *outSize);
int isolateInboxFd(void);
void isolateInboxClearSignal(void);

// C-level self-test: spawn a peer isolate, stream buffers 0->1, verify in-order
// arrival through a consumer fiber. Returns 0 on success.
int isolateTransportSelfTest(void);

#endif
