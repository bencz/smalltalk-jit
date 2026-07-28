#ifndef ROOTS_H
#define ROOTS_H

// The root-scanning contract, and specifically the part that requirement R1 of
// ADR 0003 is about: a frame slot is a POINTER, a raw double, or a raw integer,
// and the collector must never treat the last two as pointers.
//
// This header exists BEFORE the JIT does, on purpose. The old VM's stackmap was
// a plain bitset of "slots to scan", every marked slot was assumed tagged, and
// `scavengeStackSlot` then SILENTLY REWROTE to nil any slot whose contents did
// not look like a live object. That defensive repair was there because the old
// map came from a linear-scan allocator blind to control flow and was routinely
// wrong. It also meant a missing slot produced no test failure, which under a
// dry cut (ADR 0002) is exactly the class of bug we cannot afford.
//
// The v2 contract is the opposite: the map is EXACT, produced from liveness over
// SSA, and a slot that disagrees with it is an assertion failure, not something
// to repair. Nothing in this file repairs anything.

#include "core/Object.h"

// What a frame slot holds at a given safepoint.
//
// The collector only needs POINTER versus everything-else. Deoptimization needs
// the full distinction: to rebuild an interpreter-visible frame it must know
// whether to re-box a raw double or a raw integer, and getting that wrong is a
// wrong answer rather than a crash. Both consumers read the same map, so there
// is one encoding and no chance of the two disagreeing.
typedef enum {
	SLOT_DEAD = 0,    // not live here: neither scanned nor materialized
	SLOT_POINTER = 1, // tagged Value: scanned and updated
	SLOT_F64 = 2,     // raw IEEE-754 double: never scanned, boxed on deopt
	SLOT_I64 = 3,     // raw 64-bit integer: never scanned, tagged on deopt
} SlotKind;

#define SLOT_KIND_BITS 2
#define SLOT_KIND_MASK 3
#define SLOT_KINDS_PER_BYTE (8 / SLOT_KIND_BITS)

// A safepoint's frame description: two bits per frame slot, packed.
// `slotCount` is the number of DESCRIBED slots; anything beyond it is dead.
typedef struct {
	uint32_t codeOffset; // byte offset into the method's native code
	uint16_t slotCount;
	uint16_t byteCount;  // ceil(slotCount / SLOT_KINDS_PER_BYTE)
	uint8_t kinds[];
} FrameMap;


static inline SlotKind frameMapKindAt(const FrameMap *map, size_t slot)
{
	if (slot >= map->slotCount) {
		return SLOT_DEAD;
	}
	size_t byte = slot / SLOT_KINDS_PER_BYTE;
	size_t shift = (slot % SLOT_KINDS_PER_BYTE) * SLOT_KIND_BITS;
	return (SlotKind) ((map->kinds[byte] >> shift) & SLOT_KIND_MASK);
}


static inline void frameMapSetKind(FrameMap *map, size_t slot, SlotKind kind)
{
	ASSERT(slot < map->slotCount);
	size_t byte = slot / SLOT_KINDS_PER_BYTE;
	size_t shift = (slot % SLOT_KINDS_PER_BYTE) * SLOT_KIND_BITS;
	map->kinds[byte] = (uint8_t) ((map->kinds[byte] & ~(SLOT_KIND_MASK << shift))
		| ((uint32_t) kind << shift));
}


static inline size_t frameMapByteCount(size_t slotCount)
{
	return (slotCount + SLOT_KINDS_PER_BYTE - 1) / SLOT_KINDS_PER_BYTE;
}


// Every root provider hands slots to the collector through this. `slot` points
// at a tagged Value the collector may READ and WRITE (a moving collector
// updates it in place).
typedef void (*RootVisitor)(void *ctx, Value *slot);

// Implemented by whichever engine owns native frames. Declared here rather than
// in the JIT's own header so the collector can be built and self-tested with no
// JIT at all, which is exactly gate level 1 (docs/jit-v2/01-gate.md).
//
// A build with no execution engine links the no-op in memory/Roots.c.
struct Thread;
void rootsVisitNativeFrames(struct Thread *thread, RootVisitor visit, void *ctx);

// The heap references that COMPILED CODE holds, and that live outside the heap.
//
// A CodeUnit is a malloc'd C struct with tagged Values in it (its literal
// frame), and an inline-cache cell holds its selector as a bare pointer so the
// dispatch path needs no untagging. Neither is inside any heap object, so
// nothing else can find them, and a young collection moves exactly the objects
// they name. Without this the failure is a selector that silently stops
// matching after the first collection, which surfaces as doesNotUnderstand for
// a method that plainly exists.
//
// Declared here rather than in the JIT's header for the same reason as the
// frame walk above: the collector is built and self-tested with no JIT at all.
void rootsVisitCompiledCode(RootVisitor visit, void *ctx);

// The heap references the UNWIND CHAIN holds, which also live outside the heap.
//
// A record sits on a C frame and carries tagged Values: the class and the block
// of an `on:do:`, the cleanup block of an `ensure:`, and the value an unwind is
// carrying out. A handler block is held for the ENTIRE evaluation of the block
// it protects, which allocates as freely as any other code, so these are
// ordinary long-lived roots. Without this the first collection inside a
// protected block leaves the handler pointing at a corpse, and the symptom only
// appears when the protected block allocated enough to collect.
void rootsVisitUnwindRecords(struct Thread *thread, RootVisitor visit, void *ctx);

// The roots of every fiber that is NOT running.
//
// A parked fiber's VM state is not in any Thread: its handle scopes, its frame
// chain, its handler chains and its entry block were moved into the Fiber when
// it switched off (concurrency/Fiber.h, FiberRoots), and its native frames are
// on a stack no mutator scan reaches. So a collection triggered by one fiber
// would collect everything every other fiber is holding.
//
// The RUNNING fiber is deliberately excluded and must be: its live state is in
// CurrentThread, which the per-mutator loop above already scans, and the copy
// left in its Fiber is whatever was there when it last parked. Visiting the
// stale copy would have the collector update a slot nobody reads while the live
// one goes unvisited -- the exact shape of a root that looks scanned and is not.
//
// Weak no-op in memory/Roots.c, so a build with no scheduler links and collects
// exactly as before, which is what keeps gate levels 0 and 2 standalone.
void rootsVisitFibers(RootVisitor visit, void *ctx);

#endif
