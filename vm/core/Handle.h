#ifndef HANDLE_H
#define HANDLE_H

// Scoped handles: how C code holds a heap object across an allocation.
//
// The young generation MOVES (memory/Nursery.h), so a bare RawObject* held by C
// across anything that can allocate is a dangling pointer waiting for a
// collection. A handle is a slot the collector knows about and UPDATES, so the
// C code reads the object's current address every time through `->raw`.
//
// Scopes nest and are stack-shaped: a function opens one, allocates through it,
// and closes it, optionally promoting one result into the parent scope. Nothing
// here is a garbage-collection root in the "keeps alive forever" sense; a scope
// keeps its contents alive exactly as long as the C frame that owns it.
//
// The inline array is 256 entries, on the C stack, and the overflow is CHUNKED
// rather than reallocated: a handle pointer already handed out must never move,
// because callers hold it. Chunk chain is newest-first and every chunk but the
// newest is full.

#include "core/Object.h"
#include "core/Thread.h"
#include "memory/Roots.h"
#include <stddef.h>
#include <string.h>

#define HANDLE_SCOPE_INLINE 256
#define HANDLE_CHUNK_SIZE 1024

typedef struct HandleChunk {
	struct HandleChunk *next; // the older, full chunk
	Object handles[HANDLE_CHUNK_SIZE];
} HandleChunk;

typedef struct HandleScope {
	struct HandleScope *parent;
	Object handles[HANDLE_SCOPE_INLINE];
	size_t size;           // total across the inline array and every chunk
	HandleChunk *overflow; // NULL until the scope outgrows the inline array
} HandleScope;

// The well-known objects and classes of one heap. Populated at bootstrap and
// reached through the `Handles` macro below.
//
// Every entry is a HANDLE and not a raw pointer, for the same reason the rest of
// this file exists: these classes are ordinary movable objects. What is stable
// about a class is its INDEX (ADR 0005), which is why generated code bakes the
// index and only C code that needs the class OBJECT comes through here.
// Every field is exactly one pointer wide, and that is load-bearing: the
// collector walks this struct as a flat array of slots
// (smalltalkHandleSlotCount below) rather than naming each field, so adding a
// well-known class here can never be forgotten on the root-scanning side.
typedef struct SmalltalkHandles {
	Object nil;
	Object true_;
	Object false_;

	Class ObjectClass;
	Class MetaClass;
	Class ClassClass;
	Class UndefinedObject;
	Class True;
	Class False;
	Class SmallInteger;
	Class LargeInteger;
	Class Character;
	// Float is the class the built-in kernel hangs float arithmetic on, and it
	// exists for one reason: packages/Core declares `Float := Number` with the
	// real `+`, `/` and the rest, and a scaffold method has to go where the real
	// one goes or it shadows it forever (tools/Bootstrap.c).
	Class Float;
	Class SmallFloat64;
	Class BoxedFloat64;
	Class String;
	Class Symbol;
	Class Array;
	Class ByteArray;
	Class FloatArray;
	Class Association;
	Class Dictionary;
	Class OrderedCollection;
	Class CompiledMethod;
	Class CompiledBlock;
	Class Closure;
	Class Cell;
	Class Namespace;

	// The syntax tree, which is made of ORDINARY HEAP OBJECTS.
	//
	// That is the parser's design and it is kept (docs/jit-v2/03-escopo-revisado.md
	// keeps Tokenizer, Parser and Ast). It has one consequence worth stating: a
	// node is allocated, so a parse can collect, so every node the parser holds
	// has to be in a handle scope. The upside is that the collector needs to know
	// nothing about syntax.
	//
	// The literal kinds are separate CLASSES rather than a tag field, because
	// that is how the parser tells them apart and how the semantic analyser will:
	// a VariableNode and an IntegerNode differ only in class.
	Class SourceCode;
	Class FileSourceCode;
	Class ClassNode;
	Class MethodNode;
	Class BlockNode;
	Class ExpressionNode;
	Class MessageExpressionNode;
	Class IntegerNode;
	Class StringNode;
	Class SymbolNode;
	Class CharacterNode;
	Class ArrayNode;
	Class VariableNode;
	Class NilNode;
	Class TrueNode;
	Class FalseNode;

	Array symbolTable;
	Object globals;
} SmalltalkHandles;

_Static_assert(sizeof(Object) == sizeof(void *), "a handle is one pointer");

static inline size_t smalltalkHandleSlotCount(void)
{
	return sizeof(SmalltalkHandles) / sizeof(void *);
}

#define Handles (*CurrentThread.heap->handles)

void initHandles(void);
void freeHandles(void);

void openHandleScope(HandleScope *scope);
// Close `scope`, promoting `result` into the parent scope when non-NULL.
// Returns the promoted handle, or NULL.
void *closeHandleScope(HandleScope *scope, void *result);
// Wrap a raw object in a handle belonging to the innermost open scope.
void *scopeHandle(void *rawObject);

// Allocate an instance of `class` with `elements` indexed elements, already
// wrapped in a handle. THE ONLY way C code should create objects: the raw
// allocator hands back a pointer that the very next allocation may invalidate.
void *newObject(Class *class, size_t elements);
// An instance that will NEVER MOVE (memory/Heap.h). For nil, true and false,
// whose addresses generated code bakes as immediates.
void *newImmortalObject(Class *class, size_t elements);

// Visit the heap's well-known objects. Walked as a flat slot array, so a field
// added above is scanned without touching this.
void smalltalkHandlesVisitRoots(struct Heap *heap, RootVisitor visit, void *ctx);

// Every handle in every scope of one mutator, for the collector.
void handlesVisitRoots(struct Thread *thread, RootVisitor visit, void *ctx);

// The tagged Value naming a handled object. The one place C code converts a
// handle into something storable in a heap slot.
static inline Value objectTagged(void *handle)
{
	return tagPtr(((Object *) handle)->raw);
}


// The named instance variables of an object, and its indexed part.
//
// Both take a HANDLE and re-read `->raw`, which is the point: the caller is C
// code that may allocate between one call and the next, and a bare body pointer
// held across an allocation is a dangling pointer into the evacuated semispace.
static inline Value *objectVars(void *handle)
{
	return (Value *) ((Object *) handle)->raw->body;
}


// The indexed region, whatever the format uses it for: tagged elements for an
// Array, bytes for a String or a LargeInteger, doubles for a FloatArray. Body
// word 0 is the element count in every indexed format, so it is stepped over
// here rather than at each call site.
static inline void *objectIndexedVars(void *handle)
{
	return (uint8_t *) ((Object *) handle)->raw->body + sizeof(uint64_t);
}


// A copy of an indexed object with a different element count, truncating or
// zero-extending. The parser builds keyword selectors this way: it does not know
// the final length until it has read the last keyword.
Object *copyResizedObject(Object *object, size_t elements);

// Is this slot EMPTY, in either of the two spellings the VM produces?
//
// The allocator writes ZERO into a slot nobody set, which is the SmallInteger 0
// and is what ABSENT means inside the VM (memory/Heap.c). But an Array that
// SMALLTALK reads holds nil in its empty slots instead, because nil is the only
// absent Smalltalk has and the kernel's own hash probes test `isNil`
// (runtime/Collection.c, newArray).
//
// Both spellings therefore exist in one running system: an Array allocated
// before nil did carries zeros, every one after carries nil. Every hash probe in
// C has to accept both, or a table built on one side reads as permanently full
// from the other, and the probe walks off into a slot it thinks is occupied.
static inline _Bool slotIsEmpty(Value slot)
{
	return !valueTypeOf(slot, VALUE_POINTER) || asObject(slot) == Handles.nil.raw;
}


// Class index of a handled class, which is what generated code and inline
// caches compare against.
static inline uint32_t classIndexOf(Class *class)
{
	return class->raw->classIndex;
}


// Is `object` an instance of `class`? An index compare, no dereference of the
// class and no second cache line.
static inline _Bool isInstanceOf(RawObject *object, Class *class)
{
	return rawObjectClassIndex(object) == class->raw->classIndex;
}

#endif
