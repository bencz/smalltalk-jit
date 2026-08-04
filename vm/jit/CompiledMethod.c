#include "jit/CompiledMethod.h"
#include "core/Assert.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"


static CompiledMethod *codeCreate(Class *class, CodeUnit *unit, String *selector,
	Class *owner)
{
	HandleScope scope;
	openHandleScope(&scope);

	CompiledMethod *method = newObject(class, 0);
	method->raw->bytecodeBytes = unit->instructionCount * sizeof(Instruction);
	method->raw->unit = unit;
	method->raw->native = NULL;
	// An ABSENT selector or owner is stored as nil, not left as the
	// allocator's zero: both fields are read from Smalltalk as ordinary
	// instance variables (CompiledMethod.st, CompiledBlock.st), and a
	// top-level block's `ownerClass isNil` must answer true rather than
	// handing the image a SmallInteger 0.
	rawObjectStorePtr((RawObject *) method->raw, &method->raw->selector,
		selector != NULL ? (RawObject *) selector->raw : Handles.nil.raw);
	rawObjectStorePtr((RawObject *) method->raw, &method->raw->ownerClass,
		owner != NULL ? (RawObject *) owner->raw : Handles.nil.raw);
	// The unit points BACK at its object: a native frame only knows its unit
	// (NativeCode->unit), and this is what lets a materialised Context answer
	// `code` as the object the image already holds. No barrier for the same
	// reason bindBlocksOfUnit needs none: a unit is C memory the roots visit
	// keeps current, not an old object pointing at a young one.
	unit->codeObject = objectTagged(method);
	return closeHandleScope(&scope, method);
}


CompiledMethod *compiledMethodCreate(CodeUnit *unit, String *selector, Class *owner)
{
	return codeCreate(&Handles.CompiledMethod, unit, selector, owner);
}


CompiledMethod *compiledBlockCreate(CodeUnit *unit, String *selector, Class *owner)
{
	// The class exists before packages/Core is read (tools/Bootstrap.c). If a
	// build ever reaches here without it, a block's code is still a perfectly
	// usable method object and only reflection is poorer, which beats aborting.
	return codeCreate(Handles.CompiledBlock.raw != NULL
		? &Handles.CompiledBlock : &Handles.CompiledMethod, unit, selector, owner);
}


// The recursive half: a block nested inside a block has the same HOME METHOD,
// not the block around it. `^` obeys the same rule (jitMakeClosure) and for the
// same reason -- what a block belongs to is the method it was WRITTEN in,
// however many brackets are in between.
static void bindBlocksOfUnit(CodeUnit *unit, CompiledMethod *method)
{
	if (unit == NULL || !valueTypeOf(unit->blocks, VALUE_POINTER)) {
		return;
	}
	RawArray *blocks = (RawArray *) asObject(unit->blocks);
	for (size_t i = 0; i < (size_t) blocks->size; i++) {
		Value slot = blocks->vars[i];
		if (!valueTypeOf(slot, VALUE_POINTER)) {
			continue;
		}
		RawCompiledMethod *block = (RawCompiledMethod *) asObject(slot);
		if (block->unit == NULL) {
			continue;
		}
		// A unit is C memory holding tagged Values that rootsVisitCompiledCode
		// keeps current, so this store needs no barrier: the remembered set is
		// about OLD OBJECTS pointing at young ones, and a unit is not an object.
		block->unit->homeMethod = objectTagged(method);
		bindBlocksOfUnit(block->unit, method);
		// Nothing above allocates, but the array is re-read anyway so that
		// stays true by construction rather than by inspection.
		blocks = (RawArray *) asObject(unit->blocks);
	}
}


void compiledMethodBindBlocks(CompiledMethod *method)
{
	if (method == NULL || method->raw->unit == NULL) {
		return;
	}
	bindBlocksOfUnit(method->raw->unit, method);
}
