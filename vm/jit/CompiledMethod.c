#include "jit/CompiledMethod.h"
#include "core/Assert.h"
#include "memory/Heap.h"


CompiledMethod *compiledMethodCreate(CodeUnit *unit, String *selector, Class *owner)
{
	HandleScope scope;
	openHandleScope(&scope);

	CompiledMethod *method = newObject(&Handles.CompiledMethod, 0);
	method->raw->bytecodeBytes = unit->instructionCount * sizeof(Instruction);
	method->raw->unit = unit;
	method->raw->native = NULL;
	if (selector != NULL) {
		rawObjectStorePtr((RawObject *) method->raw, &method->raw->selector,
			(RawObject *) selector->raw);
	}
	if (owner != NULL) {
		rawObjectStorePtr((RawObject *) method->raw, &method->raw->ownerClass,
			(RawObject *) owner->raw);
	}
	return closeHandleScope(&scope, method);
}
