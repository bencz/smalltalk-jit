#ifndef HANDLE_H
#define HANDLE_H

#include "core/Object.h"
#include "core/Thread.h"
#include "runtime/Dictionary.h"
#include <string.h>

#define REMEMBER_SCOPE_POSITION 0

typedef struct Handle {
	void *object;
	struct Handle *prev;
	struct Handle *next;
} Handle;

typedef struct HandleScope {
	struct HandleScope *parent;
	Object handles[256];   // scopeHandle hard-bounds size < 256 (was [1024], 4× oversized)
	size_t size;
#if REMEMBER_SCOPE_POSITION
	char *file;
	size_t line;
#endif
} HandleScope;

typedef struct {
	Handle *current;
} HandlesIterator;

typedef struct {
	HandleScope *current;
} HandleScopeIterator;

typedef struct SmalltalkHandles {
	Object *nil;
	Object *false;
	Object *true;
	Class *MetaClass;
	Class *UndefinedObject;
	Class *True;
	Class *False;
	Class *SmallInteger;
	Class *Symbol;
	Class *Character;
	Class *Float;
	Class *SmallFloat64;
	Class *BoxedFloat64;
	Class *String;
	Class *Array;
	Class *ByteArray;
	Class *Association;
	Class *Dictionary;
	Class *OrderedCollection;
	Class *Class;
	Class *TypeFeedback;
	Class *CompiledMethod;
	Class *CompiledBlock;
	Class *SourceCode;
	Class *FileSourceCode;
	Class *Block;
	Class *Message;
	Class *MethodContext;
	Class *BlockContext;
	Class *ExceptionHandler;
	Class *UnwindHandler;
	Class *ClassNode;
	Class *MethodNode;
	Class *BlockNode;
	Class *BlockScope;
	Class *ExpressionNode;
	Class *MessageExpressionNode;
	Class *NilNode;
	Class *TrueNode;
	Class *FalseNode;
	Class *VariableNode;
	Class *IntegerNode;
	Class *CharacterNode;
	Class *SymbolNode;
	Class *StringNode;
	Class *ArrayNode;
	Class *ParseError;
	Class *UndefinedVariableError;
	Class *RedefinitionError;
	Class *ReadonlyVariableError;
	Class *InvalidPragmaError;
	Class *IoError;
	Dictionary *Smalltalk;
	Array *SymbolTable;
	String *initializeSymbol;
	String *finalizeSymbol;
	String *valueSymbol;
	String *value_Symbol;
	String *valueValueSymbol;
	String *doesNotUnderstandSymbol;
	String *cannotReturnSymbol;
	String *handlesSymbol;
	String *generateBacktraceSymbol;
	String *runHandledBySymbol;
} SmalltalkHandles;

// The well-known handles are per-HEAP (shared by all worker OS threads of a heap),
// NOT per-OS-thread TLS: they point at the shared old-space kernel objects, so every
// thread mutating one heap must see the SAME set (removes the old `Handles = gMainHandles`
// TLS-copy hack). Multiple isolates keep separate handles because they have separate
// heaps. Storage lives in `struct Heap` (allocated in initHeap); this macro reaches it
// through the current thread's heap.
#define Handles (*CurrentThread.heap->handles)

static void *scopeHandle(void *object);
static void *closeHandleScope(HandleScope *scope, void *handle);

static void *persistHandle(void *handle);
static void *handle(void *object);
void freeHandle(void *handle);
void freeHandles(void);

void *newObject(Class *class, size_t size);
Float *newFloat(double value);
static Value getTaggedPtr(void *handle);
Object *copyResizedObject(Object *object, size_t newSize);

void initHandlesIterator(HandlesIterator *iterator, Handle *handles);
_Bool handlesIteratorHasNext(HandlesIterator *iterator);
Object *handlesIteratorNext(HandlesIterator *iterator);

void initHandleScopeIterator(HandleScopeIterator *iterator, HandleScope *scopes);
_Bool handleScopeIteratorHasNext(HandleScopeIterator *iterator);
HandleScope *handleScopeIteratorNext(HandleScopeIterator *iterator);


// NB: do NOT memset the whole scope — `handles[]` is up to ~4 KB of stack and is
// pure waste to zero (every consumer reads only [0..size), and scopeHandle writes
// slots sequentially as size grows). Zeroing only `size` (and `parent`) is enough.
// This is a hot path (every allocateObject / primitive / entry) and a big chunk of
// each C frame's stack, so the memset dominated both perf and the compile spike.
#if REMEMBER_SCOPE_POSITION
	#define openHandleScope(scope) _openHandleScope(scope, __FILE__, __LINE__)
	static void _openHandleScope(HandleScope *scope, char *file, size_t line)
	{
		scope->size = 0;
		scope->parent = CurrentThread.handleScopes;
		scope->file = file;
		scope->line = line;
		CurrentThread.handleScopes = scope;
	}
#else
	static void openHandleScope(HandleScope *scope)
	{
		scope->size = 0;
		scope->parent = CurrentThread.handleScopes;
		CurrentThread.handleScopes = scope;
	}
#endif


static void *closeHandleScope(HandleScope *scope, void *handle)
{
	ASSERT(CurrentThread.handleScopes == scope);
	CurrentThread.handleScopes = CurrentThread.handleScopes->parent;
	if (handle != NULL) {
		ASSERT(CurrentThread.handleScopes != NULL);
		return scopeHandle(((Object *) handle)->raw);
	}
	return NULL;
}


static void *scopeHandle(void *object)
{
	ASSERT(CurrentThread.handleScopes != NULL);
	// HARD bound (survives -DNDEBUG): handles[] is exactly 256, so a 257th handle
	// would corrupt the stack. FAIL() aborts instead.
	if (CurrentThread.handleScopes->size >= 256) {
		FAIL();
	}
	Object *handle = &CurrentThread.handleScopes->handles[CurrentThread.handleScopes->size++];
	handle->raw = object;
	return handle;
}


static void *persistHandle(void *object)
{
	return handle(((Object *) object)->raw);
}


static void *handle(void *object)
{
	Handle *handle = malloc(sizeof(Handle));
	ASSERT(handle != NULL);
	handle->object = object;
	handle->prev = NULL;
	handle->next = CurrentThread.handles;
	if (handle->next != NULL) {
		handle->next->prev = handle; // keep the list doubly-linked so freeHandle works
	}
	CurrentThread.handles = handle;
	return (void *) handle;
}


static Value getTaggedPtr(void *handle)
{
	return tagPtr(((Object *) handle)->raw);
}

#endif
