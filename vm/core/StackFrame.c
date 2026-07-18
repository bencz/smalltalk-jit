#include "core/StackFrame.h"
#include "core/Thread.h"
#include "core/Smalltalk.h"
#include "core/CompiledCode.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/Assert.h"
#include <string.h>


StackFrame *stackFrameGetParent(StackFrame *frame, EntryStackFrame *entryFrame)
{
	if (frame->parent == entryFrame->entry) {
		return NULL;
	}
	return frame->parent;
}


RawContext *stackFrameGetContext(StackFrame *frame)
{
	return (RawContext *) asObject(stackFrameGetSlot(frame, CONTEXT_SLOT));
}


RawContext *stackFrameGetParentContext(StackFrame *frame)
{
	StackFrame *parent = stackFrameGetParent(frame, CurrentThread.stackFramesTail);
	if (parent == NULL) {
		return NULL;
	}

	Value contextSlotValue = stackFrameGetSlot(parent, CONTEXT_SLOT);
	RawContext *context;
	if (contextSlotValue == CurrentThread.context) {
		context = (RawContext *) allocateObject(CurrentThread.heap, Handles.MethodContext->raw, 0);
		context->frame = parent;
		context->code = tagPtr(stackFrameGetNativeCode(parent)->compiledCode);
		stackFrameSetSlot(parent, CONTEXT_SLOT, tagPtr(context));
	} else {
		context = (RawContext *) asObject(contextSlotValue);
	}
	context->ic = tagInt((intptr_t) frame->parentIc/* - code->nativeCode->insts*/);
	return context;
}


void stackFrameSetArg(StackFrame *frame, ptrdiff_t index, Value value)
{
	frame->args[index] = value;
}


Value stackFrameGetArg(StackFrame *frame, ptrdiff_t index)
{
	return frame->args[index];
}


Value *stackFrameGetArgPtr(StackFrame *frame, ptrdiff_t index)
{
	return &frame->args[index];
}


// PORT_ME(frame-layout): the slot arithmetic below mirrors the frame the JIT's
// generatePrologue emits (saved BP at *frame, slots growing down from it). A new
// backend must either reproduce this exact layout or make these accessors part
// of the per-arch contract.
void stackFrameSetSlot(StackFrame *frame, ptrdiff_t index, Value value)
{
	Value *slots = (Value *) frame - 1;
	slots[-index] = value;
}


Value stackFrameGetSlot(StackFrame *frame, ptrdiff_t index)
{
	Value *slots = (Value *) frame - 1;
	return slots[-index];
}


Value *stackFrameGetSlotPtr(StackFrame *frame, ptrdiff_t index)
{
	Value *slots = (Value *) frame - 1;
	return &slots[-index];
}


NativeCode *stackFrameGetNativeCode(StackFrame *frame)
{
	return (NativeCode *) (stackFrameGetSlot(frame, FRAME_CODE_OFFSET) - offsetof(NativeCode, insts));
}


_Bool contextHasValidFrame(RawContext *context)
{
	// A block's closure doubles as its BlockContext, and a closure that was
	// never activated by a FRAMED prologue (frameless block, or not yet called)
	// still has the zero frame pointer from allocation: dereferencing it faults.
	if (context->frame == NULL) {
		return 0;
	}
	return stackFrameGetSlot(context->frame, CONTEXT_SLOT) == tagPtr(context);
}


// Validity check for the reflective Context accessors (Context>>parent,
// receiver, argumentAt:, temporaryAt:). Those can be handed a context that
// ESCAPED its activation: the frame pointer then dangles into popped stack,
// another fiber's stack (which can be reclaimed at any moment), or the
// munmapped stack of a dead fiber. contextHasValidFrame alone is not enough,
// because it must READ the frame's context slot to decide, and that read
// itself faults on unmapped memory. Only accept a frame inside the CURRENT
// fiber's active stack span: below the outermost Smalltalk entry frame and
// above this very C frame (the primitive runs deeper on the same fiber
// stack, so anything in between is mapped). The slot match then rejects the
// popped-but-mapped frames. Contexts living on OTHER fibers' stacks are
// conservatively reported dead: nil beats racing their owner.
_Bool contextFrameOnCurrentStack(RawContext *context)
{
	uintptr_t frame = (uintptr_t) context->frame;
	if (frame == 0) {
		return 0;
	}
	EntryStackFrame *entry = CurrentThread.stackFramesTail;
	if (entry == NULL) {
		return 0;
	}
	while (entry->prev != NULL) {
		entry = entry->prev;
	}
	uintptr_t hi = (uintptr_t) entry->entry;
	uintptr_t lo = (uintptr_t) __builtin_frame_address(0);
	if (frame <= lo || frame > hi) {
		return 0;
	}
	return contextHasValidFrame(context);
}


void printBacktrace(void)
{
	EntryStackFrame *entryFrame = CurrentThread.stackFramesTail;
	while (entryFrame != NULL) {
		StackFrame *prev = entryFrame->exit;
		StackFrame *frame = stackFrameGetParent(prev, entryFrame);
		while (frame != NULL) {
			NativeCode *code = stackFrameGetNativeCode(frame);

			RawCompiledMethod *method = code->compiledCode;
			RawCompiledBlock *block = NULL;
			if (method->class == Handles.CompiledBlock->raw) {
				block = (RawCompiledBlock *) method;
				method = (RawCompiledMethod *) asObject(block->method);
			}

			RawClass *class = (RawClass *) asObject(method->ownerClass);
			if (class->class == Handles.MetaClass->raw) {
				class = (RawClass *) asObject(((RawMetaClass *) class)->instanceClass);
			}

			RawString *className = (RawString *) asObject(class->name);
			RawString *selector = (RawString *) asObject(method->selector);
			printf(
				"%p in %.*s#%.*s%s\n",
				(void *) prev->parentIc,
				(int) className->size, className->contents,
				(int) selector->size, selector->contents,
				block == NULL ? "" : "[]"
			);

			prev = frame;
			frame = stackFrameGetParent(frame, entryFrame);
		}
		entryFrame = entryFrame->prev;
	}
}
