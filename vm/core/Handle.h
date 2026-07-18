#ifndef HANDLE_H
#define HANDLE_H

#include "core/Object.h"
#include "core/Thread.h"
#include "runtime/Dictionary.h"
#include <string.h>

#define REMEMBER_SCOPE_POSITION 0

// Inline capacity of a scope (on the C stack) and the size of each heap
// overflow chunk appended once the inline part is full. The inline part is
// back at the original 256 (it was raised to 1024 while overflow meant a hard
// FAIL): now that a full scope spills to chunks, a small inline part costs
// giant methods one malloc while every recursive C frame (parser, analyzer,
// codegen) carries 2KB instead of 8KB, which directly buys AST depth on the
// growable fiber stacks.
#define HANDLE_SCOPE_INLINE 256
#define HANDLE_CHUNK_SIZE 1024

typedef struct Handle {
	void *object;
	struct Handle *prev;
	struct Handle *next;
} Handle;

// Heap overflow for a scope that outgrew its inline array (a giant method's
// literals, a long `,` chain). Chunked so already-issued handle pointers never
// move; the chain is NEWEST chunk first, every chunk but the newest is full.
typedef struct HandleChunk {
	struct HandleChunk *next; // the previous (older, full) chunk
	Object handles[HANDLE_CHUNK_SIZE];
} HandleChunk;

typedef struct HandleScope {
	struct HandleScope *parent;
	Object handles[HANDLE_SCOPE_INLINE]; // 2KB/scope on the C stack; the common case
	size_t size;                         // total, inline part plus all chunks
	HandleChunk *overflow;               // NULL until the scope outgrows the inline part
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
		scope->overflow = NULL;
		scope->parent = CurrentThread.handleScopes;
		scope->file = file;
		scope->line = line;
		CurrentThread.handleScopes = scope;
	}
#else
	static void openHandleScope(HandleScope *scope)
	{
		scope->size = 0;
		scope->overflow = NULL;
		scope->parent = CurrentThread.handleScopes;
		CurrentThread.handleScopes = scope;
	}
#endif


// Free a scope's heap overflow chunks: on every path that retires a scope,
// closeHandleScope AND the exception unwinder (unwindThreadStateTo), which
// pops scopes without closing them.
static void handleScopeFreeChunks(HandleScope *scope)
{
	HandleChunk *chunk = scope->overflow;
	while (chunk != NULL) {
		HandleChunk *next = chunk->next;
		free(chunk);
		chunk = next;
	}
	scope->overflow = NULL;
}


// The handle at global index `index` of a scope, inline part or overflow
// chunk. O(1) for the inline part; beyond it walks the newest-first chain
// (rare: only scopes past 1024 live handles, and the GC walk stays O(chunks)
// per access). Used by the collectors and the snapshot writer.
static inline Object *handleScopeAt(HandleScope *scope, size_t index)
{
	if (index < HANDLE_SCOPE_INLINE) {
		return &scope->handles[index];
	}
	size_t overflowIndex = index - HANDLE_SCOPE_INLINE;
	size_t overflowSize = scope->size - HANDLE_SCOPE_INLINE;
	size_t chunkCount = (overflowSize + HANDLE_CHUNK_SIZE - 1) / HANDLE_CHUNK_SIZE;
	size_t ordinal = overflowIndex / HANDLE_CHUNK_SIZE; // 0-based from the OLDEST chunk
	HandleChunk *chunk = scope->overflow;               // newest first
	for (size_t back = chunkCount - 1; back > ordinal; back--) {
		chunk = chunk->next;
	}
	return &chunk->handles[overflowIndex % HANDLE_CHUNK_SIZE];
}


static void *closeHandleScope(HandleScope *scope, void *handle)
{
	ASSERT(CurrentThread.handleScopes == scope);
	CurrentThread.handleScopes = CurrentThread.handleScopes->parent;
	void *result = NULL;
	if (handle != NULL) {
		ASSERT(CurrentThread.handleScopes != NULL);
		// Re-home into the parent BEFORE freeing: `handle` may live in a chunk.
		result = scopeHandle(((Object *) handle)->raw);
	}
	handleScopeFreeChunks(scope);
	return result;
}


static void *scopeHandle(void *object)
{
	ASSERT(CurrentThread.handleScopes != NULL);
	HandleScope *scope = CurrentThread.handleScopes;
	if (scope->size < HANDLE_SCOPE_INLINE) {
		Object *handle = &scope->handles[scope->size++];
		handle->raw = object;
		return handle;
	}
	// Inline part exhausted: spill to heap chunks (entries never move, so
	// previously returned handle pointers stay valid). Replaces the old hard
	// FAIL() at 1024: a giant compiled method or a deep `,` chain now just
	// grows the scope.
	size_t index = (scope->size - HANDLE_SCOPE_INLINE) % HANDLE_CHUNK_SIZE;
	if (index == 0) {
		HandleChunk *chunk = malloc(sizeof(HandleChunk));
		if (chunk == NULL) {
			FAIL();
		}
		chunk->next = scope->overflow;
		scope->overflow = chunk;
	}
	Object *handle = &scope->overflow->handles[index];
	handle->raw = object;
	scope->size++;
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
