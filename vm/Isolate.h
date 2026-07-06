#ifndef ISOLATE_H
#define ISOLATE_H

#include <stddef.h>
#include <stdint.h>

// Share-nothing isolates: N OS threads, each a full independent VM instance
// (its own heap, GC, scheduler, JIT and kernel image). They share no mutable
// object memory — every per-isolate VM global lives in initial-exec TLS (see
// PER_ISOLATE in Thread.h), so spawning an OS thread gives it a fresh set.
//
// Phase 1: prove independence (`isolatesRun`). Phase 2: let isolates talk. Each
// isolate owns a thread-safe INBOX (an eventfd + a mutex-protected MPSC queue of
// message buffers) in the shared `gIsolates` registry, so any thread can post a
// buffer to any isolate and wake it through its epoll loop.

#define MAX_ISOLATES 256

int isolatesRun(int count, const char *snapshotFile, const char *program);

// Per-isolate boot: create this isolate's inbox eventfd and register it in the
// running scheduler's epoll. Call after schedulerInit(). `id` is 0..count-1.
void isolateInboxInit(int id);
int isolateCurrentId(void);

// Post `size` bytes to isolate `target`'s inbox and wake it. Thread-safe; called
// from any isolate. Returns 0 on success. The bytes are copied.
int isolatePostBytes(int target, const uint8_t *bytes, size_t size);

// Drain + process this isolate's inbox (called by the scheduler when the inbox
// eventfd fires). Phase 2: hands each buffer to the registered receive handler.
void isolateDrainInbox(void);

// Test/glue hook: what to do with each received buffer (Phase 3 will deserialize
// + deliver to a local actor). Set per isolate.
typedef void (*IsolateReceiveFn)(int fromHint, const uint8_t *bytes, size_t size);
void isolateSetReceiveHandler(IsolateReceiveFn handler);

// C-level Phase 2 self-test: spawn 2 isolates, send buffers 0->1, verify arrival.
// Returns 0 if the round trip delivered correctly.
int isolateTransportSelfTest(void);

#endif
