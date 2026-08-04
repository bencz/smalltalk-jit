#include "core/Smalltalk.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"

// The system dictionary: the one place a NAME becomes an object.
//
// It lives in `Handles.globals`, which is a well-known handle, so it is a GC
// root by exactly the mechanism the kernel classes already use and needs nothing
// of its own.
//
// The COMPILER does not come through here. A method is compiled against the
// namespace named by its CompileContext, because a method can belong to a
// namespace that is not the system one; these entry points are for the VM's own
// C code, which has exactly two callers today (the parser, for a literal too
// wide for a SmallInteger) and the bootstrap.
//
// Symbol interning is NOT here any more: it is in runtime/String.c with the
// strings, which is where the table it uses lives.

Dictionary *smalltalkGlobals(void)
{
	ASSERT(Handles.globals.raw != NULL);
	return (Dictionary *) &Handles.globals;
}


void smalltalkInitGlobals(size_t capacity)
{
	ASSERT(Handles.globals.raw == NULL);
	Handles.globals.raw = ((Object *) newDictionary(capacity))->raw;
}


String *getSymbol(char *s)
{
	return asSymbol(stringFromC(s));
}


void globalAtPut(String *key, Value value)
{
	symbolDictAtPut(smalltalkGlobals(), asSymbol(key), value);
}


// A name nobody defined answers 0, which is tagInt(0) and therefore NOT a
// pointer, so a caller that forgets to check gets an obviously wrong value
// rather than a plausible object.
Value globalAt(String *key)
{
	Association *association = symbolDictAssocAt(smalltalkGlobals(), asSymbol(key));
	return association == NULL ? 0 : association->raw->value;
}


Object *globalObjectAt(String *key)
{
	Value value = globalAt(key);
	return valueTypeOf(value, VALUE_POINTER) ? scopeHandle(asObject(value)) : NULL;
}


void setGlobal(char *key, Value value)
{
	globalAtPut(stringFromC(key), value);
}


void setGlobalObject(char *key, Object *value)
{
	globalAtPut(stringFromC(key), objectTagged(value));
}


Value getGlobal(char *key)
{
	return globalAt(stringFromC(key));
}


Object *getGlobalObject(char *key)
{
	return globalObjectAt(stringFromC(key));
}


// A class by name, for the few places in C that need one that is not a
// well-known handle: the parser reaching LargePositiveInteger for a literal too
// wide for a SmallInteger, and ScaledDecimal for one written with an `s`.
//
// Answers NULL when the running kernel has not defined it, and both callers
// handle that, because the built-in kernel defines far less than packages/Core.
Class *getClass(char *key)
{
	return (Class *) getGlobalObject(key);
}
