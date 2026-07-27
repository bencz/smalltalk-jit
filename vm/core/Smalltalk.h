#ifndef SMALLTALK_H
#define SMALLTALK_H

// The system dictionary and the well-known singletons.
//
// Adapted to the v2 handle model: SmalltalkHandles holds handles BY VALUE, not
// as pointers, so `Handles.nil` is an Object and `Handles.nil.raw` is what it
// names. The old spelling `Handles.nil->raw` is what most of the compile errors
// in the surviving front end are.

#include "core/Handle.h"
#include "core/Object.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"

#define SYMBOL_TABLE_SIZE 1024

String *asSymbol(String *string);
String *getSymbol(char *s);
void setGlobal(char *key, Value value);
void setGlobalObject(char *key, Object *value);
Value getGlobal(char *key);
Object *getGlobalObject(char *key);
void globalAtPut(String *key, Value value);
Value globalAt(String *key);
Object *globalObjectAt(String *key);
Class *getClass(char *key);
void objectBecome(Object *object, Object *other);


static inline _Bool isNil(void *handle)
{
	return ((Object *) handle)->raw == Handles.nil.raw;
}


static inline _Bool isRawNil(void *raw)
{
	return (RawObject *) raw == Handles.nil.raw;
}


static inline _Bool isTaggedNil(Value value)
{
	return value == tagPtr(Handles.nil.raw);
}


static inline _Bool isTaggedTrue(Value value)
{
	return value == tagPtr(Handles.true_.raw);
}


// The singleton for a C boolean. Answers the HANDLE, so the caller can store it
// through the write barrier like any other object.
static inline Object *asBool(_Bool value)
{
	return value ? &Handles.true_ : &Handles.false_;
}

#endif
