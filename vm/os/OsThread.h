#ifndef OS_THREAD_H
#define OS_THREAD_H

// The thread slice of the OS seam (Os.h explains the link-time model). The
// TYPES live in the per-OS impl header vm/os/<os>/OsThreadImpl.h, selected by
// the include path CMake's ST_OS block adds: they are by-value (embeddable in
// structs, sizeof identical to the native primitive, so struct Heap keeps its
// exact layout) and the hot operations are static inline there, so the
// wrappers cost exactly nothing over calling the native API directly.
//
// The impl header must define:
//   types   OsMutex, OsCond, OsThread            (by value)
//   macros  OS_MUTEX_STATIC_INIT, OS_COND_STATIC_INIT
//   static inline void osMutexInit / osMutexDestroy /
//                      osMutexLock / osMutexUnlock (OsMutex *)
//   static inline void osCondInit / osCondDestroy /
//                      osCondBroadcast (OsCond *)
//   static inline void osCondWait(OsCond *, OsMutex *)  // spurious wakeups ok
//
// Mutexes are NON-recursive (VM re-entrancy is by per-thread depth counters:
// codegenLock/symbolLock in Heap.c). No trylock, no timedwait, no signal:
// 1:1 with what the VM needs, like the rest of the seam. The shape maps clean
// to Win32 (SRWLOCK / CONDITION_VARIABLE / _beginthreadex) — see PORTING.md.

#include "OsThreadImpl.h"

#ifndef OS_THREAD_IMPL
#error "no OsThreadImpl.h on the include path - register the OS in CMakeLists' ST_OS block"
#endif

// Out of line (vm/os/<os>/OsThread.c). `thread` must stay at its address
// until osThreadJoin: the spawn thunk reads entry/arg from it. The entry
// returns void — every join site in the VM discards the pthread result.
_Bool osThreadSpawn(OsThread *thread, void (*entry)(void *), void *arg);
void osThreadJoin(OsThread *thread);

#endif
