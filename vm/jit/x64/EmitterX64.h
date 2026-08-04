#ifndef EMITTER_X64_H
#define EMITTER_X64_H

// The x86-64 emitter, exposed so each ABI can compose it with its own tables.
//
// The split is the point: an ABI file supplies WHICH REGISTERS and HOW MUCH
// STACK, this file supplies HOW TO ENCODE. SysV, Win64 and AIX disagree about
// the first and agree entirely about the second, so a new ABI is a table and
// not a second backend.

#include "jit/MacroAssembler.h"

// Per-instance backend scratch. Small and fixed: the label POOL it points at
// grows on demand, because a method's label count is not bounded by anything
// worth guessing. An earlier version inlined a fixed pool here and the static
// assertion below caught it at two megabytes per compilation.
#define X64_STATE_BYTES 32

void x64Prologue(MacroAssembler *, Value nilValue);
void x64Epilogue(MacroAssembler *, uint16_t resultSlot);
void x64LoadSlot(MacroAssembler *, uint16_t slot);
void x64LoadAbsolute(MacroAssembler *, const void *address);
void x64StoreSlot(MacroAssembler *, uint16_t slot);
void x64LoadImmediate(MacroAssembler *, Value value);
void x64LoadField(MacroAssembler *, uint16_t fieldIndex);
void x64StoreField(MacroAssembler *, uint16_t objectSlot, uint16_t fieldIndex,
	uint16_t valueSlot);
MaLabel *x64NewLabel(MacroAssembler *);
void x64Bind(MacroAssembler *, MaLabel *);
void x64Jump(MacroAssembler *, MaLabel *);
void x64BranchIfImmediate(MacroAssembler *, uint16_t slot, Value value,
	MaCondition, MaLabel *);
void x64BranchIfTag(MacroAssembler *, uint16_t slot, MaTagTest, MaLabel *);
void x64BranchIfNotClass(MacroAssembler *, uint16_t slot, uint32_t classIndex,
	MaLabel *);
void x64CallRuntime3(MacroAssembler *, MaRuntimeFunction, void *pointerArg,
	uint16_t slotAddressArg, uint64_t integerArg);
void x64Send(MacroAssembler *, const MaSendSite *site);
void x64CallPrimitive(MacroAssembler *, PrimitiveFunction, uint64_t argc);
void x64SafepointPoll(MacroAssembler *, volatile int *flag);
void x64End(MacroAssembler *);

// Every ABI file writes exactly this, so an operation added to the emitter
// reaches every ABI at once instead of being forgotten in one of them.
#define X64_EMITTER_OPS \
	.stateBytes = X64_STATE_BYTES, \
	.end = x64End, \
	.prologue = x64Prologue, \
	.epilogue = x64Epilogue, \
	.loadSlot = x64LoadSlot, \
	.loadAbsolute = x64LoadAbsolute, \
	.storeSlot = x64StoreSlot, \
	.loadImmediate = x64LoadImmediate, \
	.loadField = x64LoadField, \
	.storeField = x64StoreField, \
	.newLabel = x64NewLabel, \
	.bind = x64Bind, \
	.jump = x64Jump, \
	.branchIfImmediate = x64BranchIfImmediate, \
	.branchIfTag = x64BranchIfTag, \
	.branchIfNotClass = x64BranchIfNotClass, \
	.callRuntime3 = x64CallRuntime3, \
	.send = x64Send, \
	.callPrimitive = x64CallPrimitive, \
	.safepointPoll = x64SafepointPoll

#endif
