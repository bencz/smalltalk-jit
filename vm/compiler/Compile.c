#include "compiler/Compile.h"
#include "core/Namespace.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/Smalltalk.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "runtime/Closure.h"
#include "runtime/Collection.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

// The Symbol a name interns to. A RAW pointer, valid until the next allocation,
// so every caller uses it immediately and none of them stores it.
static RawObject *symbolOf(String *name)
{
	return (RawObject *) asSymbol(name)->raw;
}


static _Bool nameIs(RawObject *symbol, const char *text)
{
	return rawStringEqualsBytes((RawString *) symbol, text, strlen(text));
}


// Position of `symbol` in a collection of interned Symbols, or -1. Identity, so
// no string compare happens anywhere in resolution.
static int indexOfSymbol(OrderedCollection *symbols, RawObject *symbol)
{
	size_t size = ordCollSize(symbols);
	for (size_t i = 0; i < size; i++) {
		if (asObject(ordCollAt(symbols, i)) == symbol) {
			return (int) i;
		}
	}
	return -1;
}


static int indexOfObject(OrderedCollection *objects, Object *object)
{
	return indexOfSymbol(objects, (RawObject *) object->raw);
}


// ---------------------------------------------------------------------------
// Phase 0: which messages become control flow
// ---------------------------------------------------------------------------
//
// ONE predicate, read by BOTH the capture analysis and the emitter, and that is
// not tidiness. If the two ever disagreed about whether a block is inlined, the
// disagreement would be silent: a `sum := sum + i` inside a to:do: whose block
// the analysis believed was a real closure would turn `sum` into a heap cell,
// and the result is a correct program with a dead loop, which is precisely the
// loop this project exists to optimize.
//
// The Boolean check is a JUMPTRUE/JUMPFALSE pair rather than the GUARDCLASS that
// docs/jit-v2/04-bytecode.md names, and the difference is worth stating: true
// and false are SINGLETONS, so testing identity against them is one compare
// against an immediate, while a class guard is a load, a mask and a compare, and
// it would take TWO of them to establish "Boolean" rather than "True".

typedef enum {
	INLINE_NONE = 0,
	INLINE_IF,     // ifTrue:, ifFalse:, ifTrue:ifFalse:, ifFalse:ifTrue:
	INLINE_AND,    // and:
	INLINE_OR,     // or:
	INLINE_WHILE,  // whileTrue:, whileFalse:, and the argumentless forms
	INLINE_TO_DO,  // to:do:
} InlineForm;

typedef struct {
	InlineForm form;
	_Bool sense;       // INLINE_WHILE: keep looping while the test answers this
	BlockNode *test;   // INLINE_WHILE: the receiver block
	BlockNode *first;  // the true arm, the right-hand side, or the loop body
	BlockNode *second; // the false arm
	Object *limit;     // INLINE_TO_DO
} Inline;


static _Bool isBlockOfArity(Object *node, size_t arity)
{
	if (node == NULL || !isInstanceOf(node->raw, &Handles.BlockNode)) {
		return 0;
	}
	OrderedCollection *args = blockNodeGetArgs((BlockNode *) node);
	return args != NULL && ordCollSize(args) == arity;
}


// `receiverNode` non-NULL means the receiver has NOT been evaluated yet, which
// is the only difference between the two places this is asked: the loop forms
// take a BLOCK as receiver and have to be recognised before it is built, since
// building it is exactly the closure allocation they exist to avoid. A caller
// holding an already-evaluated receiver passes NULL and acts only on the forms
// that do not need it.
static Inline inlineFormOf(Object *receiverNode, String *selector,
	OrderedCollection *args)
{
	Inline decision;
	memset(&decision, 0, sizeof decision);

	size_t argc = args == NULL ? 0 : ordCollSize(args);
	Object *arg0 = argc > 0 ? scopeHandle(asObject(ordCollAt(args, 0))) : NULL;
	Object *arg1 = argc > 1 ? scopeHandle(asObject(ordCollAt(args, 1))) : NULL;

	if (receiverNode != NULL && isInstanceOf(receiverNode->raw, &Handles.BlockNode)) {
		if (!isBlockOfArity(receiverNode, 0)) {
			return decision;
		}
		if (argc == 0 && (stringEqualsC(selector, "whileTrue")
				|| stringEqualsC(selector, "whileFalse"))) {
			decision.form = INLINE_WHILE;
			decision.sense = stringEqualsC(selector, "whileTrue");
			decision.test = (BlockNode *) receiverNode;
			return decision;
		}
		if (argc == 1 && isBlockOfArity(arg0, 0)
				&& (stringEqualsC(selector, "whileTrue:")
					|| stringEqualsC(selector, "whileFalse:"))) {
			decision.form = INLINE_WHILE;
			decision.sense = stringEqualsC(selector, "whileTrue:");
			decision.test = (BlockNode *) receiverNode;
			decision.first = (BlockNode *) arg0;
			return decision;
		}
		// Any other message to a block literal wants a real closure.
		return decision;
	}

	// to:do: needs its RECEIVER node too, so that the counter can be the block's
	// argument register rather than a copy of it.
	if (receiverNode != NULL && argc == 2 && stringEqualsC(selector, "to:do:")
			&& isBlockOfArity(arg1, 1)) {
		decision.form = INLINE_TO_DO;
		decision.limit = arg0;
		decision.first = (BlockNode *) arg1;
		return decision;
	}

	if (argc == 1 && isBlockOfArity(arg0, 0)) {
		if (stringEqualsC(selector, "ifTrue:")) {
			decision.form = INLINE_IF;
			decision.first = (BlockNode *) arg0;
		} else if (stringEqualsC(selector, "ifFalse:")) {
			decision.form = INLINE_IF;
			decision.second = (BlockNode *) arg0;
		} else if (stringEqualsC(selector, "and:")) {
			decision.form = INLINE_AND;
			decision.first = (BlockNode *) arg0;
		} else if (stringEqualsC(selector, "or:")) {
			decision.form = INLINE_OR;
			decision.first = (BlockNode *) arg0;
		}
		return decision;
	}
	if (argc == 2 && isBlockOfArity(arg0, 0) && isBlockOfArity(arg1, 0)) {
		if (stringEqualsC(selector, "ifTrue:ifFalse:")) {
			decision.form = INLINE_IF;
			decision.first = (BlockNode *) arg0;
			decision.second = (BlockNode *) arg1;
		} else if (stringEqualsC(selector, "ifFalse:ifTrue:")) {
			decision.form = INLINE_IF;
			decision.first = (BlockNode *) arg1;
			decision.second = (BlockNode *) arg0;
		}
	}
	return decision;
}


// ---------------------------------------------------------------------------
// Phase 1: capture analysis
// ---------------------------------------------------------------------------
//
// What a closure needs decided BEFORE a single instruction is emitted:
//
//   * which outer names each non-inlined block reads, in an order that becomes
//     the closure's capture slots;
//   * which declarations need a heap CELL, which is exactly those that are
//     captured AND assigned. Everything else is captured by VALUE and touches no
//     heap at all (ADR 0008).
//
// It has to be decided first because a cell changes how the variable is read and
// written EVERYWHERE, including before the closure that captures it exists. A
// decision taken at the closure site would have to rewrite instructions already
// emitted.
//
// The tables are keyed by AST NODE IDENTITY and they are HEAP collections, not C
// arrays of RawObject*. Emission allocates, so a C array of node pointers would
// be a set of addresses the first collection invalidates. That is the mistake
// this project has already paid for three times (IcCell.selector,
// CodeUnit.literals, and nil/true/false), and it is not going to be paid a
// fourth time in the compiler.

enum {
	DECL_ASSIGNED = 1,
	DECL_CAPTURED = 2,
};

typedef struct {
	OrderedCollection *declarations;   // the VariableNode that declares a local
	OrderedCollection *flags;          // parallel, tagged DECL_* bits
	OrderedCollection *blockNodes;     // BlockNodes that become real closures
	OrderedCollection *blockCaptures;  // parallel, an OrderedCollection of Symbols
	OrderedCollection *blockUses;      // parallel, times the emitter used it
	OrderedCollection *instanceVariables;
	// Some block of this method does a `^`, so the method's activations have to
	// be findable by one at runtime (jit/Jit.c). Set while emitting a block and
	// read when the METHOD's unit is finished, which is after every block of it.
	_Bool hasNonLocalReturn;
} Analysis;

// One lexical level for the analysis. `block` is NULL for the method and for
// every INLINED block, because an inlined block shares the enclosing frame and
// therefore captures nothing. A non-NULL `block` is a real closure boundary, and
// crossing one outward is what makes a name a capture.
typedef struct AnalysisScope {
	struct AnalysisScope *parent;
	OrderedCollection *names;         // interned Symbols
	OrderedCollection *declarations;  // parallel, the declaring VariableNodes
	BlockNode *block;
	uint16_t blockIndex;
} AnalysisScope;


static void analysisDeclare(Analysis *a, AnalysisScope *scope, LiteralNode *declaration)
{
	String *name = literalNodeGetStringValue(declaration);
	ordCollAdd(scope->names, objectTagged(asSymbol(name)));
	ordCollAddObject(scope->declarations, (Object *) declaration);
	if (indexOfObject(a->declarations, (Object *) declaration) < 0) {
		ordCollAddObject(a->declarations, (Object *) declaration);
		ordCollAdd(a->flags, tagInt(0));
	}
}


static void analysisSetFlag(Analysis *a, Object *declaration, int flag)
{
	int index = indexOfObject(a->declarations, declaration);
	ASSERT(index >= 0); // every declaration passed here was declared above
	Value flags = ordCollAt(a->flags, (size_t) index);
	ordCollAtPut(a->flags, (size_t) index, tagInt(asCInt(flags) | flag));
}


static _Bool declarationNeedsCell(Analysis *a, LiteralNode *declaration)
{
	int index = indexOfObject(a->declarations, (Object *) declaration);
	if (index < 0) {
		return 0;
	}
	intptr_t flags = asCInt(ordCollAt(a->flags, (size_t) index));
	// Captured AND assigned. Captured alone is a copy into the closure and costs
	// nothing; assigned alone is an ordinary register.
	return (flags & DECL_ASSIGNED) != 0 && (flags & DECL_CAPTURED) != 0;
}


// Give `block` a capture list, or find the one it already has.
static uint16_t analysisRegisterBlock(Analysis *a, BlockNode *block)
{
	int index = indexOfObject(a->blockNodes, (Object *) block);
	if (index >= 0) {
		return (uint16_t) index;
	}
	index = (int) ordCollSize(a->blockNodes);
	ordCollAddObject(a->blockNodes, (Object *) block);
	ordCollAddObject(a->blockCaptures, (Object *) newOrdColl(4));
	ordCollAdd(a->blockUses, tagInt(0));
	return (uint16_t) index;
}


static OrderedCollection *analysisCapturesOf(Analysis *a, uint16_t blockIndex)
{
	return (OrderedCollection *) ordCollObjectAt(a->blockCaptures, blockIndex);
}


static void analysisAddCapture(Analysis *a, uint16_t blockIndex, RawObject *symbol)
{
	OrderedCollection *captures = analysisCapturesOf(a, blockIndex);
	if (indexOfSymbol(captures, symbol) < 0) {
		ordCollAdd(captures, tagPtr(symbol));
	}
}


// `self` lives in register 0 of the METHOD, so every block between here and the
// method has to carry it. An instance variable inside a block needs it too:
// GETIVAR takes an object register, and in a block that register can only come
// from a capture.
static void analysisCaptureSelf(Analysis *a, AnalysisScope *scope)
{
	String *name = stringFromC("self");
	for (AnalysisScope *s = scope; s != NULL; s = s->parent) {
		if (s->block != NULL) {
			analysisAddCapture(a, s->blockIndex, symbolOf(name));
		}
	}
}


static _Bool isInstanceVariable(Analysis *a, RawObject *symbol)
{
	return a->instanceVariables != NULL
		&& indexOfSymbol(a->instanceVariables, symbol) >= 0;
}


// One reference to a name. `assigned` says whether this is the target of an
// assignment rather than a read.
static void analysisUse(Analysis *a, AnalysisScope *scope, String *name,
	_Bool assigned)
{
	RawObject *symbol = symbolOf(name);
	if (nameIs(symbol, "self") || nameIs(symbol, "super")) {
		analysisCaptureSelf(a, scope);
		return;
	}

	for (AnalysisScope *s = scope; s != NULL; s = s->parent) {
		// BACKWARD, so a later declaration of a name shadows an earlier one,
		// which is what a block argument reusing an outer temporary's name does.
		int found = -1;
		for (size_t i = ordCollSize(s->names); i > 0 && found < 0; i--) {
			if (asObject(ordCollAt(s->names, i - 1)) == symbol) {
				found = (int) (i - 1);
			}
		}
		if (found < 0) {
			continue;
		}
		Object *declaration = ordCollObjectAt(s->declarations, (size_t) found);
		if (assigned) {
			analysisSetFlag(a, declaration, DECL_ASSIGNED);
		}
		// Every closure boundary strictly INSIDE the declaring scope has to
		// carry the name, including the intermediate ones: a doubly nested block
		// reaches a method temporary by way of its parent's capture, not by
		// walking anything (ADR 0008).
		for (AnalysisScope *t = scope; t != s; t = t->parent) {
			if (t->block == NULL) {
				continue;
			}
			analysisAddCapture(a, t->blockIndex, symbolOf(name));
			analysisSetFlag(a, declaration, DECL_CAPTURED);
		}
		return;
	}

	// Not a local. An instance variable needs self; a global is reached through
	// this unit's own literal frame and needs nothing.
	if (isInstanceVariable(a, symbol)) {
		analysisCaptureSelf(a, scope);
	}
}


static void analyzeExpression(Analysis *a, AnalysisScope *scope, ExpressionNode *node);
static void analyzeValue(Analysis *a, AnalysisScope *scope, Object *node);


static void analyzeStatements(Analysis *a, AnalysisScope *scope,
	OrderedCollection *statements)
{
	size_t count = statements == NULL ? 0 : ordCollSize(statements);
	for (size_t i = 0; i < count; i++) {
		HandleScope handles;
		openHandleScope(&handles);
		analyzeExpression(a, scope,
			(ExpressionNode *) scopeHandle(asObject(ordCollAt(statements, i))));
		closeHandleScope(&handles, NULL);
	}
}


// A block that is compiled INTO the enclosing frame. Its temporaries are
// ordinary registers of the same method, so its scope is not a boundary.
static void analyzeInlineBlock(Analysis *a, AnalysisScope *parent, BlockNode *block,
	LiteralNode *boundArgument)
{
	if (block == NULL) {
		return;
	}
	AnalysisScope scope;
	scope.parent = parent;
	scope.block = NULL;
	scope.blockIndex = 0;
	scope.names = newOrdColl(4);
	scope.declarations = newOrdColl(4);

	if (boundArgument != NULL) {
		analysisDeclare(a, &scope, boundArgument);
	}
	OrderedCollection *temps = blockNodeGetTempVars(block);
	size_t tempCount = temps == NULL ? 0 : ordCollSize(temps);
	for (size_t i = 0; i < tempCount; i++) {
		analysisDeclare(a, &scope,
			(LiteralNode *) scopeHandle(asObject(ordCollAt(temps, i))));
	}
	analyzeStatements(a, &scope, blockNodeGetExpressions(block));
}


// A block that becomes a real closure. Its scope IS a boundary, so every name it
// reads from outside becomes a capture.
static void analyzeClosureBlock(Analysis *a, AnalysisScope *parent, BlockNode *block)
{
	AnalysisScope scope;
	scope.parent = parent;
	scope.block = block;
	scope.blockIndex = analysisRegisterBlock(a, block);
	scope.names = newOrdColl(4);
	scope.declarations = newOrdColl(4);

	OrderedCollection *args = blockNodeGetArgs(block);
	size_t argCount = args == NULL ? 0 : ordCollSize(args);
	for (size_t i = 0; i < argCount; i++) {
		analysisDeclare(a, &scope,
			(LiteralNode *) scopeHandle(asObject(ordCollAt(args, i))));
	}
	OrderedCollection *temps = blockNodeGetTempVars(block);
	size_t tempCount = temps == NULL ? 0 : ordCollSize(temps);
	for (size_t i = 0; i < tempCount; i++) {
		analysisDeclare(a, &scope,
			(LiteralNode *) scopeHandle(asObject(ordCollAt(temps, i))));
	}
	analyzeStatements(a, &scope, blockNodeGetExpressions(block));
}


// `toSuper` is passed rather than rediscovered, because the emitter does not
// inline anything sent to super (the whole point of super is where the lookup
// starts) and the two have to reach the same verdict. Without it, the analysis
// would treat the arms of a `super ifTrue: [...]` as inlined while the emitter
// built closures for them, and the emitter would then look for capture lists
// that were never made.
static void analyzeMessage(Analysis *a, AnalysisScope *scope,
	MessageExpressionNode *message, _Bool toSuper)
{
	String *selector = messageExpressionNodeGetSelector(message);
	OrderedCollection *args = messageExpressionNodeGetArgs(message);
	Inline decision = toSuper ? (Inline) { INLINE_NONE, 0, NULL, NULL, NULL, NULL }
		: inlineFormOf(NULL, selector, args);

	if (decision.form == INLINE_IF || decision.form == INLINE_AND
			|| decision.form == INLINE_OR) {
		analyzeInlineBlock(a, scope, decision.first, NULL);
		analyzeInlineBlock(a, scope, decision.second, NULL);
		return;
	}
	size_t argc = args == NULL ? 0 : ordCollSize(args);
	for (size_t i = 0; i < argc; i++) {
		analyzeValue(a, scope, scopeHandle(asObject(ordCollAt(args, i))));
	}
}


static void analyzeValue(Analysis *a, AnalysisScope *scope, Object *node)
{
	if (isInstanceOf(node->raw, &Handles.VariableNode)) {
		analysisUse(a, scope, literalNodeGetStringValue((LiteralNode *) node), 0);
	} else if (isInstanceOf(node->raw, &Handles.ExpressionNode)) {
		analyzeExpression(a, scope, (ExpressionNode *) node);
	} else if (isInstanceOf(node->raw, &Handles.BlockNode)) {
		analyzeClosureBlock(a, scope, (BlockNode *) node);
	}
	// Literals and nil/true/false name nothing.
}


static void analyzeExpression(Analysis *a, AnalysisScope *scope, ExpressionNode *node)
{
	Object *receiverNode = (Object *) expressionNodeGetReceiver(node);
	OrderedCollection *messages = expressionNodeGetMessageExpressions(node);
	size_t messageCount = messages == NULL ? 0 : ordCollSize(messages);
	_Bool handled = 0;

	// The same two shapes the emitter recognises with the receiver still
	// unevaluated, recognised here through the same predicate.
	if (messageCount == 1) {
		MessageExpressionNode *only = scopeHandle(asObject(ordCollAt(messages, 0)));
		Inline decision = inlineFormOf(receiverNode,
			messageExpressionNodeGetSelector(only),
			messageExpressionNodeGetArgs(only));
		if (decision.form == INLINE_WHILE) {
			analyzeInlineBlock(a, scope, decision.test, NULL);
			analyzeInlineBlock(a, scope, decision.first, NULL);
			handled = 1;
		} else if (decision.form == INLINE_TO_DO) {
			analyzeValue(a, scope, receiverNode);
			analyzeValue(a, scope, decision.limit);
			OrderedCollection *args = blockNodeGetArgs(decision.first);
			analyzeInlineBlock(a, scope, decision.first,
				(LiteralNode *) scopeHandle(asObject(ordCollAt(args, 0))));
			handled = 1;
		}
	}

	if (!handled) {
		_Bool toSuper = isInstanceOf(receiverNode->raw, &Handles.VariableNode)
			&& stringEqualsC(literalNodeGetStringValue((LiteralNode *) receiverNode),
				"super");
		analyzeValue(a, scope, receiverNode);
		for (size_t i = 0; i < messageCount; i++) {
			analyzeMessage(a, scope,
				(MessageExpressionNode *) scopeHandle(asObject(ordCollAt(messages, i))),
				toSuper);
		}
	}

	OrderedCollection *targets = expressionNodeGetAssigments(node);
	size_t targetCount = targets == NULL ? 0 : ordCollSize(targets);
	for (size_t i = 0; i < targetCount; i++) {
		LiteralNode *target = scopeHandle(asObject(ordCollAt(targets, i)));
		analysisUse(a, scope, literalNodeGetStringValue(target), 1);
	}
}


// ---------------------------------------------------------------------------
// Phase 2: resolution
// ---------------------------------------------------------------------------

typedef enum {
	PLACE_NONE,
	PLACE_SELF,
	PLACE_SUPER,          // self, but a send through it starts lookup one level up
	PLACE_REGISTER,       // an argument or a temporary: index IS the frame slot
	PLACE_CELL_REGISTER,  // that slot holds a Cell, so reads and writes go through it
	PLACE_CAPTURE,        // captured by the running closure, by value
	PLACE_CELL_CAPTURE,   // captured by the running closure, and it is a Cell
	PLACE_INSTANCE_VAR,
	PLACE_GLOBAL,         // index is a literal slot holding the Association
} PlaceKind;

typedef struct {
	PlaceKind kind;
	uint16_t index;
} Place;

// One lexical level of the emitter: a method, or a block INLINED into it.
//
// An inlined block shares the enclosing frame, so its arguments and temporaries
// are ordinary registers of the same unit and resolution just walks out through
// `parent`. That is the whole reason inlined control flow costs nothing here.
//
// The names are a HEAP collection and not a C array of RawObject*, because
// emission allocates (symbols, literal frames, block methods) and a raw name
// pointer held across that is a dangling pointer as soon as anything collects.
typedef struct Scope {
	struct Scope *parent;
	OrderedCollection *names;
	uint16_t *registers;
	uint8_t *isCell;
	uint16_t count;
	uint16_t capacity;
} Scope;

typedef struct {
	// -- output
	Instruction *code;
	uint16_t count;
	uint16_t capacity;
	OrderedCollection *literals;
	OrderedCollection *blocks;  // CompiledMethods, one per non-inlined block

	// -- frame
	uint16_t registerCount; // high-water mark: the frame is this wide
	uint16_t top;           // first free register, a stack discipline

	// -- context
	Scope *scope;
	const CompileContext *context;
	OrderedCollection *instanceVariables; // Symbols, in slot order
	Analysis *analysis;
	CompileError *error;

	// -- only when this unit is a BLOCK
	_Bool isBlock;
	OrderedCollection *captureNames; // Symbols, in capture-slot order
	const uint8_t *captureIsCell;
} Emitter;


static _Bool failed(Emitter *e)
{
	return e->error->status != COMPILE_OK;
}


static void fail(Emitter *e, CompileStatus status, String *what)
{
	if (e->error->status == COMPILE_OK) {
		e->error->status = status;
		e->error->what = what;
	}
}


// A register, from the stack discipline that makes consecutive send arguments
// possible: a sub-expression may only use registers ABOVE the caller's, so
// evaluating an argument can never clobber one already computed.
static uint16_t allocRegister(Emitter *e)
{
	if (e->top >= BYTECODE_MAX_REGISTERS) {
		fail(e, COMPILE_TOO_MANY_REGISTERS, NULL);
		return 0;
	}
	uint16_t reg = e->top++;
	if (e->top > e->registerCount) {
		e->registerCount = e->top;
	}
	return reg;
}


static void releaseTo(Emitter *e, uint16_t mark)
{
	e->top = mark;
}


static void scopePush(Emitter *e, Scope *scope)
{
	scope->parent = e->scope;
	scope->names = newOrdColl(4);
	scope->registers = NULL;
	scope->isCell = NULL;
	scope->count = 0;
	scope->capacity = 0;
	e->scope = scope;
}


static void scopePop(Emitter *e, Scope *scope)
{
	ASSERT(e->scope == scope);
	e->scope = scope->parent;
	free(scope->registers);
	free(scope->isCell);
}


static void scopeBind(Emitter *e, Scope *scope, String *name, uint16_t reg,
	_Bool isCell)
{
	(void) e;
	if (scope->count == scope->capacity) {
		scope->capacity = scope->capacity == 0 ? 8 : scope->capacity * 2;
		scope->registers = realloc(scope->registers, scope->capacity * sizeof(uint16_t));
		scope->isCell = realloc(scope->isCell, scope->capacity * sizeof(uint8_t));
		ASSERT(scope->registers != NULL && scope->isCell != NULL);
	}
	// The Symbol is INTERNED, so identity is the comparison and no string
	// compare happens during resolution.
	ordCollAdd(scope->names, objectTagged(asSymbol(name)));
	scope->registers[scope->count] = reg;
	scope->isCell[scope->count] = isCell != 0;
	scope->count++;
}


// Declare a local and give it a register. The register is permanent for the
// scope's lifetime, which is why locals are allocated before any expression of
// the body: the expression stack has to start above them.
static uint16_t scopeDeclare(Emitter *e, Scope *scope, String *name, _Bool isCell)
{
	uint16_t reg = allocRegister(e);
	scopeBind(e, scope, name, reg, isCell);
	return reg;
}


// The literal frame. Answers the index of `value`, reusing an equal entry when
// there is one: selectors repeat, and every duplicate would otherwise be a
// separate Symbol slot the collector has to visit.
static uint16_t literalIndex(Emitter *e, Value value)
{
	size_t size = ordCollSize(e->literals);
	for (size_t i = 0; i < size; i++) {
		if (ordCollAt(e->literals, i) == value) {
			return (uint16_t) i;
		}
	}
	if (size >= BYTECODE_MAX_LITERALS) {
		fail(e, COMPILE_TOO_MANY_LITERALS, NULL);
		return 0;
	}
	ordCollAdd(e->literals, value);
	return (uint16_t) size;
}


static uint16_t selectorIndex(Emitter *e, String *selector)
{
	return literalIndex(e, objectTagged(asSymbol(selector)));
}


// Where does this identifier live? Innermost scope outward, then the captures of
// the running closure, then instance variables, then globals. The order IS
// Smalltalk's shadowing rule, with the captures sitting exactly where an
// enclosing scope would be if a block shared its frame.
static Place resolveName(Emitter *e, String *name)
{
	Place place = { PLACE_NONE, 0 };
	RawObject *symbol = symbolOf(name);

	for (Scope *scope = e->scope; scope != NULL; scope = scope->parent) {
		// BACKWARD, so a later declaration of the same name shadows an earlier
		// one within a scope, which is what a block argument reusing an outer
		// temporary's name has to do.
		for (uint16_t i = scope->count; i > 0; i--) {
			if (asObject(ordCollAt(scope->names, i - 1)) == symbol) {
				place.kind = scope->isCell[i - 1]
					? PLACE_CELL_REGISTER : PLACE_REGISTER;
				place.index = scope->registers[i - 1];
				return place;
			}
		}
	}

	if (e->captureNames != NULL) {
		int index = indexOfSymbol(e->captureNames, symbol);
		if (index >= 0) {
			place.kind = e->captureIsCell[index]
				? PLACE_CELL_CAPTURE : PLACE_CAPTURE;
			place.index = (uint16_t) index;
			return place;
		}
	}

	if (nameIs(symbol, "self")) {
		place.kind = PLACE_SELF;
		return place;
	}
	if (nameIs(symbol, "super")) {
		place.kind = PLACE_SUPER;
		return place;
	}

	if (e->instanceVariables != NULL) {
		int index = indexOfSymbol(e->instanceVariables, symbol);
		if (index >= 0) {
			place.kind = PLACE_INSTANCE_VAR;
			place.index = (uint16_t) index;
			return place;
		}
	}

	// CLASS VARIABLES, which sit between instance variables and globals for the
	// same reason they do in every Smalltalk: they are shared by a class and its
	// subclasses, and they shadow a global of the same name.
	//
	// One is an Association, exactly like a global, so nothing new is emitted to
	// read or write it: the difference is only where the Association was found.
	//
	// The search starts at classVariableScope and not at ownerClass, because a
	// CLASS-SIDE method is compiled against the METACLASS and a metaclass carries
	// none of the class's class variables. See the note in compiler/Compile.h.
	Class *scopeStart = e->context->classVariableScope != NULL
		? e->context->classVariableScope : e->context->ownerClass;
	for (Class *level = scopeStart; level != NULL; ) {
		Value variables = level->raw->classVariables;
		if (valueTypeOf(variables, VALUE_POINTER)) {
			Dictionary *dictionary = scopeHandle(asObject(variables));
			String *key = scopeHandle(symbol);
			Association *association = symbolDictAssocAt(dictionary, key);
			if (association != NULL) {
				place.kind = PLACE_GLOBAL;
				place.index = literalIndex(e, objectTagged(association));
				return place;
			}
			symbol = symbolOf(key); // the lookup may have moved it
		}
		RawClass *super = rawClassSuperclass(level->raw);
		level = super == NULL ? NULL : scopeHandle((RawObject *) super);
	}

	if (e->context->globals != NULL) {
		String *key = scopeHandle(symbol);
		// THROUGH THE NAMESPACE when there is one: own bindings, then imports in
		// declaration order, then Core. With none, this is the plain dictionary
		// probe it has always been (core/Namespace.h; NULL means core only).
		Association *association = e->context->namespace != NULL
			? namespaceResolveAssoc(e->context->namespace, key)
			: symbolDictAssocAt(e->context->globals, key);
		if (association == NULL && rawStringSize((RawString *) symbol) > 0
				&& ((RawString *) symbol)->contents[0] >= 'A'
				&& ((RawString *) symbol)->contents[0] <= 'Z') {
			// A GLOBAL THAT DOES NOT EXIST YET gets its Association now, holding
			// nil, and the definition fills that same Association in later
			// (runtime/Dictionary.c reuses an existing entry).
			//
			// The kernel needs this and cannot be reordered out of it: its files
			// refer to each other in both directions, so `Object>>at:` raises an
			// OutOfRangeError that is defined thirty files further down. Without
			// forward reference, the load order would have to be a total order
			// that does not exist.
			//
			// The rule is the CAPITAL, which is Smalltalk's own convention for a
			// global. A lowercase name that resolves nowhere is still an error,
			// because that is a misspelled variable and there is nothing later
			// that could define it.
			//
			// It is minted in the namespace's OWN bindings, never in an import's
			// and never in Core: a package that forward-references a name is
			// declaring its own, and putting the placeholder in Core would let
			// one package's unresolved name become every other package's.
			association = e->context->namespace != NULL
				? symbolDictAtPut(namespaceBindings(e->context->namespace), key,
					tagPtr(Handles.nil.raw))
				: symbolDictAtPut(e->context->globals, key,
					tagPtr(Handles.nil.raw));
		}
		if (association != NULL) {
			place.kind = PLACE_GLOBAL;
			place.index = literalIndex(e, objectTagged(association));
			return place;
		}
	}
	return place;
}


// Instance variables of a class AND of every superclass, superclass-first, so
// the index is the instance's slot number.
static OrderedCollection *collectInstanceVariables(Class *class)
{
	if (class == NULL) {
		return NULL;
	}
	OrderedCollection *names = newOrdColl(8);
	// Walk UP collecting into a stack, then unwind: a subclass's own variables
	// come after its inherited ones, and only the superclass-first order makes
	// the position match the slot.
	Class *chain[64];
	size_t depth = 0;
	Class *current = class;
	while (current != NULL && depth < 64) {
		chain[depth++] = current;
		RawClass *super = rawClassSuperclass(current->raw);
		current = super == NULL ? NULL : scopeHandle((RawObject *) super);
	}
	while (depth > 0) {
		Class *level = chain[--depth];
		Value declared = level->raw->instanceVariables;
		if (!valueTypeOf(declared, VALUE_POINTER)) {
			continue;
		}
		OrderedCollection *own = scopeHandle(asObject(declared));
		size_t size = ordCollSize(own);
		for (size_t i = 0; i < size; i++) {
			String *name = scopeHandle(asObject(ordCollAt(own, i)));
			ordCollAdd(names, objectTagged(asSymbol(name)));
		}
	}
	return names;
}


// ---------------------------------------------------------------------------
// Phase 3: emission
// ---------------------------------------------------------------------------

static uint16_t emit(Emitter *e, Opcode op, uint8_t n, uint16_t a, uint16_t b,
	uint16_t c)
{
	if (failed(e)) {
		return 0;
	}
	if (e->count == e->capacity) {
		e->capacity = e->capacity == 0 ? 32 : e->capacity * 2;
		e->code = realloc(e->code, e->capacity * sizeof(Instruction));
		ASSERT(e->code != NULL);
	}
	if (e->count >= BYTECODE_MAX_INSTRUCTIONS) {
		fail(e, COMPILE_TOO_MANY_INSTRUCTIONS, NULL);
		return 0;
	}
	Instruction *instruction = &e->code[e->count];
	instruction->op = (uint8_t) op;
	instruction->n = n;
	instruction->a = a;
	instruction->b = b;
	instruction->c = c;
	return e->count++;
}


// A jump whose target is not known yet. Answers the instruction index, which is
// patched once the destination is reached. The bci IS the index, so patching is
// a store and not a relocation.
static uint16_t emitJumpForward(Emitter *e, Opcode op, uint16_t conditionReg)
{
	return op == OP_JUMP
		? emit(e, op, 0, BYTECODE_NO_TARGET, 0, 0)
		: emit(e, op, 0, conditionReg, BYTECODE_NO_TARGET, 0);
}


static void patchHere(Emitter *e, uint16_t jump)
{
	if (failed(e)) {
		return;
	}
	Instruction *instruction = &e->code[jump];
	if ((Opcode) instruction->op == OP_JUMP) {
		instruction->a = e->count;
	} else {
		instruction->b = e->count;
	}
}


static void emitExpression(Emitter *e, ExpressionNode *node, uint16_t dest);
static void emitValue(Emitter *e, Object *node, uint16_t dest);
static void emitStatements(Emitter *e, OrderedCollection *statements,
	uint16_t dest, _Bool wantValue);


// Load `self` into `dest`. In a method it is register 0; in a block register 0
// is the CLOSURE, so self is an ordinary capture and reaching it is one GETUP.
//
// The failure below is loud on purpose. If a block reached this with no `self`
// capture, the silent answer would be register 0, which is the closure, and
// every send to "self" would go to the block object instead. That is a wrong
// answer with no crash, so the analysis and the emitter disagreeing about it has
// to stop the compile rather than be discovered later.
static void emitSelfLoad(Emitter *e, uint16_t dest)
{
	if (!e->isBlock) {
		emit(e, OP_MOVE, 0, dest, 0, 0); // register 0 IS self
		return;
	}
	int index = e->captureNames == NULL
		? -1 : indexOfSymbol(e->captureNames, symbolOf(stringFromC("self")));
	if (index < 0) {
		ASSERT(index >= 0);
		fail(e, COMPILE_UNSUPPORTED, stringFromC("self inside a block that did "
			"not capture it"));
		return;
	}
	emit(e, OP_GETUP, 0, dest, (uint16_t) index, 0);
}


// The register holding self, for GETIVAR and SETIVAR. In a block that costs one
// instruction and a scratch register; in a method it is free.
static uint16_t selfRegister(Emitter *e, uint16_t scratch)
{
	if (!e->isBlock) {
		return 0;
	}
	emitSelfLoad(e, scratch);
	return scratch;
}


// Load whatever `place` names into `dest`.
static void emitLoad(Emitter *e, Place place, String *name, uint16_t dest)
{
	switch (place.kind) {
	case PLACE_SELF:
	case PLACE_SUPER:
		emitSelfLoad(e, dest);
		break;
	case PLACE_REGISTER:
		emit(e, OP_MOVE, 0, dest, place.index, 0);
		break;
	case PLACE_CELL_REGISTER:
		emit(e, OP_GETCELL, 0, dest, place.index, 0);
		break;
	case PLACE_CAPTURE:
		emit(e, OP_GETUP, 0, dest, place.index, 0);
		break;
	case PLACE_CELL_CAPTURE:
		// The capture slot holds the cell, so the value is one more load. `dest`
		// doubles as the scratch: GETCELL reads its source before writing.
		emit(e, OP_GETUP, 0, dest, place.index, 0);
		emit(e, OP_GETCELL, 0, dest, dest, 0);
		break;
	case PLACE_INSTANCE_VAR: {
		uint16_t mark = e->top;
		uint16_t object = selfRegister(e, dest);
		emit(e, OP_GETIVAR, 0, dest, object, place.index);
		releaseTo(e, mark);
		break;
	}
	case PLACE_GLOBAL:
		emit(e, OP_GETGLOBAL, 0, dest, place.index, 0);
		break;
	default:
		fail(e, COMPILE_UNDECLARED_NAME, name);
		break;
	}
}


static void emitStore(Emitter *e, Place place, String *name, uint16_t source)
{
	switch (place.kind) {
	case PLACE_REGISTER:
		emit(e, OP_MOVE, 0, place.index, source, 0);
		break;
	case PLACE_CELL_REGISTER:
		emit(e, OP_SETCELL, 0, place.index, source, 0);
		break;
	case PLACE_CELL_CAPTURE: {
		uint16_t mark = e->top;
		uint16_t cell = allocRegister(e);
		emit(e, OP_GETUP, 0, cell, place.index, 0);
		emit(e, OP_SETCELL, 0, cell, source, 0);
		releaseTo(e, mark);
		break;
	}
	case PLACE_CAPTURE:
		// A capture that is written is captured AND assigned, so the analysis
		// gave it a cell and this place kind cannot be reached. Loud, because a
		// store into a by-value capture would be silently lost.
		ASSERT(place.kind != PLACE_CAPTURE);
		fail(e, COMPILE_UNSUPPORTED, stringFromC("an assignment to a captured "
			"value with no cell"));
		break;
	case PLACE_INSTANCE_VAR: {
		uint16_t mark = e->top;
		// A scratch register only when there is something to put in it: in a
		// method self is register 0 and this costs nothing.
		uint16_t object = e->isBlock ? selfRegister(e, allocRegister(e)) : 0;
		emit(e, OP_SETIVAR, 0, object, place.index, source);
		releaseTo(e, mark);
		break;
	}
	case PLACE_GLOBAL:
		emit(e, OP_SETGLOBAL, 0, place.index, source, 0);
		break;
	default:
		// Assigning to self, to super, or to a name that resolves nowhere.
		fail(e, COMPILE_UNDECLARED_NAME, name);
		break;
	}
}


// What a closure captures is the CELL and not the value in it: that is the whole
// point of the cell, and dereferencing here would capture by value and lose
// every later assignment.
static void emitCaptureLoad(Emitter *e, Place place, String *name, uint16_t dest)
{
	switch (place.kind) {
	case PLACE_SELF:
		emitSelfLoad(e, dest);
		break;
	case PLACE_REGISTER:
	case PLACE_CELL_REGISTER:
		emit(e, OP_MOVE, 0, dest, place.index, 0);
		break;
	case PLACE_CAPTURE:
	case PLACE_CELL_CAPTURE:
		emit(e, OP_GETUP, 0, dest, place.index, 0);
		break;
	default:
		// The analysis only ever puts locals and self in a capture list.
		ASSERT(0);
		fail(e, COMPILE_UNSUPPORTED, name);
		break;
	}
}


static _Bool isInstance(Object *node, Class *class)
{
	return isInstanceOf(node->raw, class);
}


// A literal whose value is known at compile time, as opposed to a VariableNode.
static _Bool isLiteralNode(Object *node)
{
	return isInstance(node, &Handles.IntegerNode)
		|| isInstance(node, &Handles.StringNode)
		|| isInstance(node, &Handles.SymbolNode)
		|| isInstance(node, &Handles.CharacterNode)
		|| isInstance(node, &Handles.ArrayNode);
}


// The RUNTIME VALUE of a literal node, which is not always the value the node
// carries.
//
// Two kinds hold the PARSER'S WORKING REPRESENTATION rather than the object the
// program is meant to see, and emitting the field directly published both:
//
//   * a SYMBOL node holds a plain String. A Symbol is an INTERNED String, and
//     interning is the whole reason symbol identity is pointer identity, so
//     `#sym` came out a String: wrong class, and `#sym == #sym` false;
//   * an ARRAY node holds an OrderedCollection of LITERAL NODES, which is how
//     the parser accumulates elements while it reads them. So `#(1 2 3)` came
//     out an OrderedCollection whose elements were SYNTAX TREE NODES.
//
// Both failures are quiet in the way this project keeps paying for: the object
// exists, it answers `size` with the right number, and only its class is wrong.
// `#(1 2 3) size` answered 3 the whole time.
//
// nil, true and false appear here only INSIDE an array literal; at statement
// level emitValue turns them into their own instructions.
static Value literalValueOf(Emitter *e, Object *node)
{
	if (isInstance(node, &Handles.SymbolNode)) {
		return objectTagged(asSymbol(literalNodeGetStringValue((LiteralNode *) node)));
	}
	if (isInstance(node, &Handles.NilNode)) {
		return tagPtr(Handles.nil.raw);
	}
	if (isInstance(node, &Handles.TrueNode)) {
		return tagPtr(Handles.true_.raw);
	}
	if (isInstance(node, &Handles.FalseNode)) {
		return tagPtr(Handles.false_.raw);
	}
	if (!isInstance(node, &Handles.ArrayNode)) {
		return literalNodeGetValue((LiteralNode *) node);
	}

	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *items = (OrderedCollection *) scopeHandle(
		asObject(literalNodeGetValue((LiteralNode *) node)));
	size_t count = items == NULL ? 0 : ordCollSize(items);
	// A HANDLE, and it has to be: building a nested array allocates, and a
	// collection in the middle of this loop would move an Array held only as a
	// raw pointer.
	Array *array = newArray(count);
	for (size_t i = 0; i < count; i++) {
		Object *item = scopeHandle(asObject(ordCollAt(items, i)));
		Value element = literalValueOf(e, item);
		if (valueTypeOf(element, VALUE_POINTER)) {
			arrayAtPutObject(array, i, scopeHandle(asObject(element)));
		} else {
			array->raw->vars[i] = element;
		}
	}
	return objectTagged((Array *) closeHandleScope(&scope, array));
}


static void emitLiteral(Emitter *e, LiteralNode *node, uint16_t dest)
{
	Value value = literalValueOf(e, (Object *) node);
	// A small integer goes in the instruction itself. It is by far the most
	// common literal and does not deserve a slot in the frame the collector
	// walks.
	if (valueTypeOf(value, VALUE_INT)) {
		intptr_t number = asCInt(value);
		if (number >= INT16_MIN && number <= INT16_MAX) {
			emit(e, OP_LOADI, 0, dest, (uint16_t) (int16_t) number, 0);
			return;
		}
	}
	emit(e, OP_LOADK, 0, dest, literalIndex(e, value), 0);
}


// A local that is captured and assigned lives in a CELL, and the cell is made
// exactly where the register is initialised. That placement is what gives an
// inlined loop body a FRESH cell per iteration, which is what makes closures
// made in a loop capture different variables, and a method temporary ONE cell
// for the whole activation, which is what makes them share.
static void emitCellIfNeeded(Emitter *e, LiteralNode *declaration, uint16_t reg)
{
	if (declarationNeedsCell(e->analysis, declaration)) {
		// NEWCELL reads its source slot before writing its destination, so a
		// register can be replaced by a cell over itself.
		emit(e, OP_NEWCELL, 0, reg, reg, 0);
	}
}


static uint16_t declareLocal(Emitter *e, Scope *scope, LiteralNode *declaration)
{
	_Bool cell = declarationNeedsCell(e->analysis, declaration);
	return scopeDeclare(e, scope, literalNodeGetStringValue(declaration), cell);
}


// ---- inlined control flow ---------------------------------------------------
//
// ifTrue:, and:, whileTrue: and to:do: are compiled INTO the method, with their
// block arguments becoming ordinary statements of the same frame. Without this
// there is no loop for the optimizer to work on: every iteration would be a send
// and a block activation, and the phase-6 target could not exist.

// Emit an inlined block's body into the current frame, answering its value in
// `dest`. Its temporaries become registers of this method.
static void emitInlineBlock(Emitter *e, BlockNode *block, uint16_t dest,
	_Bool wantValue)
{
	Scope scope;
	scopePush(e, &scope);
	OrderedCollection *temps = blockNodeGetTempVars(block);
	if (temps != NULL) {
		size_t size = ordCollSize(temps);
		for (size_t i = 0; i < size; i++) {
			// A temporary is a VariableNode, not a String: the parser records
			// declarations with the same node it records uses with.
			LiteralNode *declaration = scopeHandle(asObject(ordCollAt(temps, i)));
			uint16_t reg = declareLocal(e, &scope, declaration);
			emit(e, OP_LOADNIL, 0, reg, 0, 0);
			emitCellIfNeeded(e, declaration, reg);
		}
	}
	emitStatements(e, blockNodeGetExpressions(block), dest, wantValue);
	scopePop(e, &scope);
}


// The value of an empty inlined block, and of the missing arm of an ifTrue:.
static void emitNil(Emitter *e, uint16_t dest)
{
	emit(e, OP_LOADNIL, 0, dest, 0, 0);
}


// `receiver ifTrue: [...] ifFalse: [...]`, with either arm possibly absent.
static void emitConditional(Emitter *e, uint16_t testReg, BlockNode *whenTrue,
	BlockNode *whenFalse, uint16_t dest)
{
	// Not true and not false: neither arm is legal, so the receiver is told so
	// by the only means Smalltalk has, which is a message.
	uint16_t toTrue = emitJumpForward(e, OP_JUMPTRUE, testReg);
	uint16_t toFalse = emitJumpForward(e, OP_JUMPFALSE, testReg);

	uint16_t mark = e->top;
	uint16_t base = allocRegister(e);
	emit(e, OP_MOVE, 0, base, testReg, 0);
	emit(e, OP_SEND, 0, dest, selectorIndex(e, stringFromC("mustBeBoolean")), base);
	releaseTo(e, mark);
	uint16_t overNonBoolean = emitJumpForward(e, OP_JUMP, 0);

	patchHere(e, toTrue);
	if (whenTrue != NULL) {
		emitInlineBlock(e, whenTrue, dest, 1);
	} else {
		emitNil(e, dest);
	}
	uint16_t overFalse = emitJumpForward(e, OP_JUMP, 0);

	patchHere(e, toFalse);
	if (whenFalse != NULL) {
		emitInlineBlock(e, whenFalse, dest, 1);
	} else {
		emitNil(e, dest);
	}

	patchHere(e, overFalse);
	patchHere(e, overNonBoolean);
}


// `a and: [b]`: b is evaluated only when a is true, and the value is false
// otherwise. Short-circuit, so it cannot be a send.
static void emitShortCircuit(Emitter *e, uint16_t testReg, BlockNode *rest,
	_Bool isAnd, uint16_t dest)
{
	// BOTH senses, then mustBeBoolean, exactly as emitConditional and emitWhile
	// do. With one jump, anything that is not false FELL THROUGH into the rest,
	// so `nil and: [true]` answered TRUE -- a wrong answer with no error, which
	// is worse than the loop that hung on the same mistake.
	uint16_t toRest = emitJumpForward(e, isAnd ? OP_JUMPTRUE : OP_JUMPFALSE, testReg);
	uint16_t toShort = emitJumpForward(e, isAnd ? OP_JUMPFALSE : OP_JUMPTRUE, testReg);

	uint16_t mark = e->top;
	uint16_t base = allocRegister(e);
	emit(e, OP_MOVE, 0, base, testReg, 0);
	emit(e, OP_SEND, 0, dest, selectorIndex(e, stringFromC("mustBeBoolean")), base);
	releaseTo(e, mark);
	uint16_t overNonBoolean = emitJumpForward(e, OP_JUMP, 0);

	patchHere(e, toRest);
	emitInlineBlock(e, rest, dest, 1);
	uint16_t done = emitJumpForward(e, OP_JUMP, 0);

	patchHere(e, toShort);
	emit(e, isAnd ? OP_LOADFALSE : OP_LOADTRUE, 0, dest, 0, 0);

	patchHere(e, done);
	patchHere(e, overNonBoolean);
}


// Send `selector` to `receiverReg` with `argc` arguments already sitting in the
// registers above it, answering into `dest`.
static void emitSendTo(Emitter *e, uint16_t dest, String *selector,
	uint16_t receiverReg, uint8_t argc, _Bool toSuper)
{
	if (toSuper && e->context->ownerClass == NULL) {
		// Where a super send starts looking is the superclass of the class that
		// DEFINED the method. With no defining class there is no such place, and
		// the tier would have to invent one.
		fail(e, COMPILE_UNSUPPORTED, stringFromC("a super send outside a class"));
		return;
	}
	emit(e, toSuper ? OP_SENDSUPER : OP_SEND, argc, dest,
		selectorIndex(e, selector), receiverReg);
}


// `start to: stop do: [:i | ...]`.
//
// The comparison and the increment are ORDINARY SENDS, deliberately: `<=` and
// `+` are exactly the sites ADR 0006 says must carry a profile, and a loop
// counter is the hottest instance of both in the system. What is inlined is the
// CONTROL FLOW, not the arithmetic.
static void emitToDo(Emitter *e, Object *receiverNode, Object *limitNode,
	BlockNode *body, uint16_t dest)
{
	uint16_t counter = allocRegister(e);
	uint16_t limit = allocRegister(e);
	emitValue(e, receiverNode, counter);
	emitValue(e, limitNode, limit);
	// to:do: answers its receiver, and the counter is about to change, so the
	// answer is taken now.
	emit(e, OP_MOVE, 0, dest, counter, 0);

	uint16_t loop = e->count;
	emit(e, OP_SAFEPOINT, 0, 0, 0, 0);

	uint16_t mark = e->top;
	uint16_t base = allocRegister(e);
	uint16_t argument = allocRegister(e);
	emit(e, OP_MOVE, 0, base, counter, 0);
	emit(e, OP_MOVE, 0, argument, limit, 0);
	uint16_t condition = allocRegister(e);
	emitSendTo(e, condition, stringFromC("<="), base, 1, 0);
	uint16_t exit = emitJumpForward(e, OP_JUMPFALSE, condition);
	releaseTo(e, mark);

	// The block's argument IS the counter register. No copy, and no separate
	// activation: that is what inlining to:do: buys. It also means the counter
	// cannot be a cell, because the loop reads and writes it as a number; a
	// captured counter is captured BY VALUE, once per iteration, which is what
	// a fresh activation per iteration would have given anyway.
	Scope scope;
	scopePush(e, &scope);
	OrderedCollection *args = blockNodeGetArgs(body);
	LiteralNode *declaration = scopeHandle(asObject(ordCollAt(args, 0)));
	if (declarationNeedsCell(e->analysis, declaration)) {
		fail(e, COMPILE_UNSUPPORTED,
			stringFromC("an assignment to the argument of an inlined block"));
		scopePop(e, &scope);
		return;
	}
	scopeBind(e, &scope, literalNodeGetStringValue(declaration), counter, 0);

	uint16_t discard = allocRegister(e);
	emitInlineBlock(e, body, discard, 0);
	releaseTo(e, discard);
	scopePop(e, &scope);

	mark = e->top;
	base = allocRegister(e);
	argument = allocRegister(e);
	emit(e, OP_MOVE, 0, base, counter, 0);
	emit(e, OP_LOADI, 0, argument, 1, 0);
	emitSendTo(e, counter, stringFromC("+"), base, 1, 0);
	releaseTo(e, mark);
	emit(e, OP_JUMP, 0, loop, 0, 0);
	patchHere(e, exit);
}


// `[...] whileTrue: [...]`, and the receiverless forms.
static void emitWhile(Emitter *e, BlockNode *condition, BlockNode *body,
	_Bool whileTrue, uint16_t dest)
{
	uint16_t loop = e->count;
	emit(e, OP_SAFEPOINT, 0, 0, 0, 0);

	uint16_t mark = e->top;
	uint16_t test = allocRegister(e);
	emitInlineBlock(e, condition, test, 1);

	// BOTH senses are tested, exactly as emitConditional does, and the third
	// case is a message rather than a branch.
	//
	// One jump is not enough and the difference is not cosmetic: with only
	// "leave when false", anything that is not false FALLS INTO THE BODY, so
	// `[] whileTrue` and `[1] whileTrue` spun forever instead of raising. A
	// conditional that got this wrong answers the wrong arm once; a loop that
	// gets it wrong never comes back, which is why it showed up as a test that
	// hung rather than one that failed.
	uint16_t toBody = emitJumpForward(e, whileTrue ? OP_JUMPTRUE : OP_JUMPFALSE, test);
	uint16_t toExit = emitJumpForward(e, whileTrue ? OP_JUMPFALSE : OP_JUMPTRUE, test);

	// Neither true nor false: tell the receiver so, by the only means Smalltalk
	// has. `test` is still live here, so the register is released after.
	uint16_t nonBooleanMark = e->top;
	uint16_t base = allocRegister(e);
	emit(e, OP_MOVE, 0, base, test, 0);
	emit(e, OP_SEND, 0, dest, selectorIndex(e, stringFromC("mustBeBoolean")), base);
	releaseTo(e, nonBooleanMark);
	uint16_t overNonBoolean = emitJumpForward(e, OP_JUMP, 0);

	patchHere(e, toBody);
	releaseTo(e, mark);

	if (body != NULL) {
		mark = e->top;
		uint16_t discard = allocRegister(e);
		emitInlineBlock(e, body, discard, 0);
		releaseTo(e, mark);
	}
	emit(e, OP_JUMP, 0, loop, 0, 0);
	patchHere(e, toExit);
	patchHere(e, overNonBoolean);
	emitNil(e, dest); // whileTrue: answers nil
}


// ---- closures ---------------------------------------------------------------

static CodeUnit *compileUnit(Emitter *parent, BlockNode *body, uint16_t arity,
	OrderedCollection *captureNames, const uint8_t *captureIsCell,
	String *selector, uint16_t primitive, _Bool isBlock);


// `[ :x | ... ]` where the block is NOT inlined: a real closure (ADR 0008).
//
// Three things happen here and their order matters. The capture list comes from
// the analysis, so it is already final, including the names an inner block needs
// only in order to forward them outward. The block's own unit is compiled first,
// because it has to exist before anything can point at it. Only then is the
// capture run emitted into THIS frame, into consecutive registers, for the same
// reason a send's arguments are consecutive: one address describes the whole run.
static void emitBlockClosure(Emitter *e, BlockNode *block, uint16_t dest)
{
	Analysis *analysis = e->analysis;
	int found = indexOfObject(analysis->blockNodes, (Object *) block);
	if (found < 0) {
		// The analysis decided this block was inlined and emission did not. The
		// two share one predicate precisely so this cannot happen, so it is a
		// compiler bug and not a program error.
		ASSERT(found >= 0);
		fail(e, COMPILE_UNSUPPORTED, stringFromC("a block the analysis and the "
			"emitter disagree about"));
		return;
	}
	uint16_t blockIndex = (uint16_t) found;
	OrderedCollection *captures = analysisCapturesOf(analysis, blockIndex);
	size_t captureCount = ordCollSize(captures);
	if (captureCount > CLOSURE_MAX_CAPTURES) {
		// A ceiling in this bytecode fails LOUDLY rather than truncating
		// (docs/jit-v2/04-bytecode.md). What sets this one is the object model
		// rather than the instruction, see runtime/Closure.h.
		fail(e, COMPILE_TOO_MANY_CAPTURES, NULL);
		return;
	}

	// Resolve every capture ONCE before compiling the block, because whether a
	// captured name is a cell is part of the block's own contract: it decides
	// whether the block dereferences the capture slot or uses it directly.
	uint8_t *isCell = captureCount > 0 ? calloc(captureCount, 1) : NULL;
	ASSERT(captureCount == 0 || isCell != NULL);
	for (size_t i = 0; i < captureCount; i++) {
		String *name = scopeHandle(asObject(ordCollAt(captures, i)));
		Place place = resolveName(e, name);
		isCell[i] = place.kind == PLACE_CELL_REGISTER
			|| place.kind == PLACE_CELL_CAPTURE;
	}

	OrderedCollection *args = blockNodeGetArgs(block);
	size_t arity = args == NULL ? 0 : ordCollSize(args);
	// PENDING: a block's unit is named "aBlock" rather than after the method it
	// was written in, which is what a backtrace will want once there is one.
	String *selector = asSymbol(stringFromC("aBlock"));
	CodeUnit *unit = compileUnit(e, block, (uint16_t) arity, captures, isCell,
		selector, PRIM_NONE, 1);
	if (unit == NULL) {
		free(isCell);
		return;
	}

	// The unit is reached through a HEAP OBJECT and not through a raw C pointer,
	// which is what keeps a block's code and its literal frame alive for as long
	// as any closure over it: the alternative is a word the collector cannot
	// follow.
	Class *owner = e->context->ownerClass;
	CompiledMethod *method = compiledMethodCreate(unit, selector, owner);
	uint16_t slot = (uint16_t) ordCollSize(e->blocks);
	ordCollAddObject(e->blocks, (Object *) method);

	// The base register is allocated even when there is nothing to capture. The
	// instruction names it either way, and the runtime turns the address of that
	// slot back into the frame pointer in order to walk the compiled frames
	// underneath the allocation, so it has to be a slot this frame really has.
	uint16_t mark = e->top;
	uint16_t base = allocRegister(e);
	for (size_t i = 0; i < captureCount; i++) {
		uint16_t reg = i == 0 ? base : allocRegister(e);
		String *name = scopeHandle(asObject(ordCollAt(captures, i)));
		emitCaptureLoad(e, resolveName(e, name), name, reg);
	}
	emit(e, OP_CLOSURE, (uint8_t) captureCount, dest, slot, base);
	releaseTo(e, mark);
	free(isCell);

	// Used exactly once, and checked at the end of the compile: a block the
	// analysis prepared and emission never reached would mean the two walks
	// diverged in the other direction, which is silent by nature.
	Value uses = ordCollAt(analysis->blockUses, blockIndex);
	ordCollAtPut(analysis->blockUses, blockIndex, tagInt(asCInt(uses) + 1));
}


// ---- expressions -------------------------------------------------------------

// One message of an expression, sent to a receiver already in `receiverReg`.
static void emitMessage(Emitter *e, MessageExpressionNode *message,
	uint16_t receiverReg, _Bool toSuper, uint16_t dest)
{
	String *selector = messageExpressionNodeGetSelector(message);
	OrderedCollection *args = messageExpressionNodeGetArgs(message);
	size_t argc = args == NULL ? 0 : ordCollSize(args);

	if (!toSuper) {
		Inline decision = inlineFormOf(NULL, selector, args);
		switch (decision.form) {
		case INLINE_IF:
			emitConditional(e, receiverReg, decision.first, decision.second, dest);
			return;
		case INLINE_AND:
			emitShortCircuit(e, receiverReg, decision.first, 1, dest);
			return;
		case INLINE_OR:
			emitShortCircuit(e, receiverReg, decision.first, 0, dest);
			return;
		default:
			break;
		}
	}
	if (argc > 255) {
		fail(e, COMPILE_UNSUPPORTED, selector);
		return;
	}

	// The receiver and its arguments have to be CONSECUTIVE, so the receiver is
	// copied to the base of a fresh run and the arguments are evaluated straight
	// into the slots above it. The copies are what the bytecode design paid for
	// a fixed-width SEND, and the optimizer removes them as trivial.
	uint16_t mark = e->top;
	uint16_t base = allocRegister(e);
	emit(e, OP_MOVE, 0, base, receiverReg, 0);
	for (size_t i = 0; i < argc; i++) {
		uint16_t slot = allocRegister(e);
		Object *arg = scopeHandle(asObject(ordCollAt(args, i)));
		emitValue(e, arg, slot);
	}
	emitSendTo(e, dest, selector, base, (uint8_t) argc, toSuper);
	releaseTo(e, mark);
}


// Any value-producing node: a literal, a variable, a nested expression, a block.
static void emitValue(Emitter *e, Object *node, uint16_t dest)
{
	if (failed(e)) {
		return;
	}
	if (isInstance(node, &Handles.NilNode)) {
		emit(e, OP_LOADNIL, 0, dest, 0, 0);
	} else if (isInstance(node, &Handles.TrueNode)) {
		emit(e, OP_LOADTRUE, 0, dest, 0, 0);
	} else if (isInstance(node, &Handles.FalseNode)) {
		emit(e, OP_LOADFALSE, 0, dest, 0, 0);
	} else if (isInstance(node, &Handles.VariableNode)) {
		String *name = literalNodeGetStringValue((LiteralNode *) node);
		emitLoad(e, resolveName(e, name), name, dest);
	} else if (isLiteralNode(node)) {
		emitLiteral(e, (LiteralNode *) node, dest);
	} else if (isInstance(node, &Handles.ExpressionNode)) {
		emitExpression(e, (ExpressionNode *) node, dest);
	} else if (isInstance(node, &Handles.BlockNode)) {
		emitBlockClosure(e, (BlockNode *) node, dest);
	} else {
		fail(e, COMPILE_UNSUPPORTED, stringFromC("an unknown node"));
	}
}


static void emitExpression(Emitter *e, ExpressionNode *node, uint16_t dest)
{
	HandleScope scope;
	openHandleScope(&scope);

	Object *receiverNode = (Object *) expressionNodeGetReceiver(node);
	OrderedCollection *messages = expressionNodeGetMessageExpressions(node);
	size_t messageCount = messages == NULL ? 0 : ordCollSize(messages);

	// A single message whose receiver has not been evaluated yet: the only shape
	// where the loop forms can be inlined, because inlining them depends on NOT
	// building the receiver block.
	if (messageCount == 1) {
		MessageExpressionNode *only = scopeHandle(asObject(ordCollAt(messages, 0)));
		Inline decision = inlineFormOf(receiverNode,
			messageExpressionNodeGetSelector(only),
			messageExpressionNodeGetArgs(only));
		if (decision.form == INLINE_WHILE) {
			emitWhile(e, decision.test, decision.first, decision.sense, dest);
			goto assignments;
		}
		if (decision.form == INLINE_TO_DO) {
			emitToDo(e, receiverNode, decision.limit, decision.first, dest);
			goto assignments;
		}
	}

	{
		// The receiver is evaluated ONCE, whatever follows: a cascade sends every
		// message to that same value, which is the whole difference between
		// `a foo; bar` and `a foo bar`.
		_Bool toSuper = isInstance(receiverNode, &Handles.VariableNode)
			&& stringEqualsC(literalNodeGetStringValue((LiteralNode *) receiverNode),
				"super");
		uint16_t mark = e->top;
		uint16_t receiverReg = allocRegister(e);
		emitValue(e, receiverNode, receiverReg);

		if (messageCount == 0) {
			emit(e, OP_MOVE, 0, dest, receiverReg, 0);
		}
		for (size_t i = 0; i < messageCount; i++) {
			MessageExpressionNode *message = scopeHandle(asObject(ordCollAt(messages, i)));
			// Only the LAST message's value is the expression's value; the
			// earlier ones in a cascade are evaluated for effect.
			uint16_t into = i + 1 == messageCount ? dest : allocRegister(e);
			emitMessage(e, message, receiverReg, toSuper, into);
		}
		releaseTo(e, mark);
	}

assignments:
	{
		OrderedCollection *targets = expressionNodeGetAssigments(node);
		size_t count = targets == NULL ? 0 : ordCollSize(targets);
		// Right to left, which is what `a := b := expr` means.
		for (size_t i = count; i > 0; i--) {
			LiteralNode *target = scopeHandle(asObject(ordCollAt(targets, i - 1)));
			String *name = literalNodeGetStringValue(target);
			emitStore(e, resolveName(e, name), name, dest);
		}
	}
	closeHandleScope(&scope, NULL);
}


static void emitStatements(Emitter *e, OrderedCollection *statements,
	uint16_t dest, _Bool wantValue)
{
	size_t count = statements == NULL ? 0 : ordCollSize(statements);
	if (count == 0) {
		if (wantValue) {
			emitNil(e, dest);
		}
		return;
	}
	for (size_t i = 0; i < count && !failed(e); i++) {
		HandleScope scope;
		openHandleScope(&scope);
		ExpressionNode *statement = scopeHandle(asObject(ordCollAt(statements, i)));
		_Bool last = i + 1 == count;
		// Every statement gets a destination, because a statement evaluated for
		// effect still has to go somewhere; only the last one's lands in `dest`.
		uint16_t mark = e->top;
		uint16_t into = last ? dest : allocRegister(e);
		emitExpression(e, statement, into);
		if (expressionNodeReturns(statement)) {
			if (e->isBlock) {
				// `^` inside a non-inlined block returns from the METHOD the
				// block was written in, not from the block, and that is a
				// different instruction (ADR 0008). Inside an INLINED block there
				// is no separate activation, so a plain RET is already right and
				// this branch is not reached: e->isBlock is a property of the
				// UNIT, and an inlined block has no unit of its own.
				emit(e, OP_RETOUTER, 0, into, 0, 0);
				// The method this block belongs to has to be findable at runtime,
				// so its activations get a token and a record. Recorded on the
				// analysis, which every nested emitter shares, because the method
				// unit is finished last and reads it then.
				e->analysis->hasNonLocalReturn = 1;
			} else {
				emit(e, OP_RET, 0, into, 0, 0);
			}
			releaseTo(e, mark);
			closeHandleScope(&scope, NULL);
			return; // everything after an explicit return is unreachable
		}
		releaseTo(e, mark);
		closeHandleScope(&scope, NULL);
	}
}


// ---------------------------------------------------------------------------
// One unit: a method, or a block
// ---------------------------------------------------------------------------
//
// Both have the same shape, which is the point of the CodeUnit being one type:
// the tier-1 frame and the deopt map need only one layout. The two differences
// are what register 0 holds (self for a method, the CLOSURE for a block) and
// what falling off the end answers (self for a method, the last statement's
// value for a block).

static CodeUnit *compileUnit(Emitter *parent, BlockNode *body, uint16_t arity,
	OrderedCollection *captureNames, const uint8_t *captureIsCell,
	String *selector, uint16_t primitive, _Bool isBlock)
{
	Emitter e;
	memset(&e, 0, sizeof e);
	e.context = parent->context;
	e.error = parent->error;
	e.analysis = parent->analysis;
	e.instanceVariables = parent->instanceVariables;
	e.literals = newOrdColl(8);
	e.blocks = newOrdColl(2);
	e.isBlock = isBlock;
	e.captureNames = captureNames;
	e.captureIsCell = captureIsCell;

	Scope scope;
	scopePush(&e, &scope);

	// Register 0 is self in a method and the CLOSURE in a block, ALWAYS, in
	// every unit. That is the frame contract the tier-1 template and the deopt
	// map both read (jit/Jit.h), and it is what makes GETUP one load.
	allocRegister(&e);

	OrderedCollection *args = blockNodeGetArgs(body);
	size_t argumentCount = args == NULL ? 0 : ordCollSize(args);
	ASSERT(argumentCount == arity);
	for (size_t i = 0; i < argumentCount; i++) {
		LiteralNode *declaration = scopeHandle(asObject(ordCollAt(args, i)));
		uint16_t reg = declareLocal(&e, &scope, declaration);
		// An argument arrives in its register, so the cell wraps what is already
		// there rather than nil.
		emitCellIfNeeded(&e, declaration, reg);
	}

	OrderedCollection *temps = blockNodeGetTempVars(body);
	size_t tempCount = temps == NULL ? 0 : ordCollSize(temps);
	for (size_t i = 0; i < tempCount; i++) {
		LiteralNode *declaration = scopeHandle(asObject(ordCollAt(temps, i)));
		uint16_t reg = declareLocal(&e, &scope, declaration);
		emit(&e, OP_LOADNIL, 0, reg, 0, 0);
		emitCellIfNeeded(&e, declaration, reg);
	}

	uint16_t result = allocRegister(&e);
	OrderedCollection *statements = blockNodeGetExpressions(body);
	size_t statementCount = statements == NULL ? 0 : ordCollSize(statements);

	// A METHOD THAT IS NOTHING BUT A PRIMITIVE fails loudly instead of answering
	// the receiver.
	//
	// The fall-through below a primitive is the general case written in
	// Smalltalk, and when there is no body there is no general case: the implicit
	// `^self` then answers the RECEIVER for a method that computed nothing.
	// Silently. It is a wrong answer that travels, and it has already cost this
	// campaign two debugging sessions -- `Character>>codePoint` answered a
	// Character, and `IoError class last` answered the class IoError.
	//
	// The built-in kernel has always done this (definePrimitiveMethod in
	// tools/Bootstrap.c), so until now the SAME source shape meant two different
	// things depending on which kernel compiled it. That divergence was the real
	// defect; this is one rule for both.
	//
	// The fallback names the PRIMITIVE, because the receiver's class alone says
	// what did not work and leaves out what was being attempted. Nothing is paid
	// by a primitive that works: this code is only reached when it did not run.
	if (!isBlock && primitive != PRIM_NONE && statementCount == 0) {
		uint16_t mark = e.top;
		uint16_t base = allocRegister(&e);
		emit(&e, OP_MOVE, 0, base, 0, 0); // the receiver
		uint16_t named = allocRegister(&e);
		emit(&e, OP_LOADK, 0, named,
			literalIndex(&e, objectTagged(asSymbol(stringFromC(
				primitiveName((PrimitiveNumber) primitive))))), 0);
		emitSendTo(&e, result, stringFromC("primitiveFailed:"), base, 1, 0);
		releaseTo(&e, mark);
	} else {
		emitStatements(&e, statements, result, 1);
		if (!isBlock) {
			// A method with no explicit return answers self. A block answers its
			// last statement, which is already in `result`.
			emit(&e, OP_MOVE, 0, result, 0, 0);
		}
	}
	// The trailing RET is unconditional: falling off the end of generated code
	// has no defined meaning, so there is always one here even when the body
	// ended in one.
	emit(&e, OP_RET, 0, result, 0, 0);

	scopePop(&e, &scope);

	if (e.error->status != COMPILE_OK) {
		free(e.code);
		return NULL;
	}

	Array *literalArray = ordCollAsArray(e.literals);
	Array *blockArray = ordCollAsArray(e.blocks);
	String *selectorSymbol = selector == NULL ? NULL : asSymbol(selector);

	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	ASSERT(unit != NULL);
	unit->code = e.code;
	unit->instructionCount = e.count;
	unit->registerCount = e.registerCount;
	unit->argumentCount = arity;
	unit->captureCount = captureNames == NULL
		? 0 : (uint16_t) ordCollSize(captureNames);
	unit->primitive = primitive;
	unit->isBlock = isBlock;
	// A block is never a home: `^` inside it returns from the METHOD it was
	// written in, and that is the unit that has to be findable.
	unit->couldBeHome = !isBlock && e.analysis->hasNonLocalReturn;
	// No allocation between here and jitRegisterUnit: until the unit is on the
	// registry these four words are reachable from nowhere the collector looks.
	unit->literals = objectTagged(literalArray);
	unit->blocks = objectTagged(blockArray);
	if (selectorSymbol != NULL) {
		unit->selector = objectTagged(selectorSymbol);
	}
	if (e.context->ownerClass != NULL) {
		unit->ownerClass = objectTagged(e.context->ownerClass);
	}
	jitRegisterUnit(unit);
	return unit;
}


// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

CodeUnit *compileMethod(MethodNode *method, const CompileContext *context,
	CompileError *error)
{
	HandleScope outer;
	openHandleScope(&outer);

	error->status = COMPILE_OK;
	error->what = NULL;

	// <primitive: IntAddPrimitive>
	//
	// The pragma is how a method declares what runs before its Smalltalk body,
	// and the NAME is the contract with packages/ (runtime/Primitives.def). A
	// name the VM has never heard of is an ERROR, because it is a typo or a
	// primitive nobody wrote; a name that is known but NOT IMPLEMENTED is fine,
	// and the method simply runs the fallback it already carries. Those two
	// cases look identical from the outside and must not be treated the same.
	uint16_t primitive = PRIM_NONE;
	OrderedCollection *pragmas = methodNodeGetPragmas(method);
	size_t pragmaCount = pragmas == NULL ? 0 : ordCollSize(pragmas);
	for (size_t i = 0; i < pragmaCount; i++) {
		MessageExpressionNode *pragma = scopeHandle(asObject(ordCollAt(pragmas, i)));
		if (!stringEqualsC(messageExpressionNodeGetSelector(pragma), "primitive:")) {
			continue;
		}
		OrderedCollection *arguments = messageExpressionNodeGetArgs(pragma);
		if (arguments == NULL || ordCollSize(arguments) != 1) {
			continue;
		}
		LiteralNode *named = scopeHandle(asObject(ordCollAt(arguments, 0)));
		String *name = literalNodeGetStringValue(named);
		PrimitiveNumber number = primitiveNumberNamed(name->raw->contents,
			rawStringSize(name->raw));
		if (number == PRIM_NONE) {
			error->status = COMPILE_UNKNOWN_PRIMITIVE;
			error->what = name;
			closeHandleScope(&outer, NULL);
			return NULL;
		}
		primitive = (uint16_t) number;
	}

	BlockNode *body = methodNodeGetBody(method);

	Analysis analysis;
	analysis.declarations = newOrdColl(8);
	analysis.flags = newOrdColl(8);
	analysis.blockNodes = newOrdColl(4);
	analysis.blockCaptures = newOrdColl(4);
	analysis.blockUses = newOrdColl(4);
	analysis.instanceVariables = collectInstanceVariables(context->ownerClass);
	analysis.hasNonLocalReturn = 0;

	// The capture analysis runs over the WHOLE method before a single
	// instruction exists, because whether a variable lives in a register or in a
	// cell changes every read and every write of it, including the ones emitted
	// before the closure that captures it.
	{
		AnalysisScope scope;
		scope.parent = NULL;
		scope.block = NULL;
		scope.blockIndex = 0;
		scope.names = newOrdColl(8);
		scope.declarations = newOrdColl(8);

		OrderedCollection *args = blockNodeGetArgs(body);
		size_t argumentCount = args == NULL ? 0 : ordCollSize(args);
		for (size_t i = 0; i < argumentCount; i++) {
			analysisDeclare(&analysis, &scope,
				(LiteralNode *) scopeHandle(asObject(ordCollAt(args, i))));
		}
		OrderedCollection *temps = blockNodeGetTempVars(body);
		size_t tempCount = temps == NULL ? 0 : ordCollSize(temps);
		for (size_t i = 0; i < tempCount; i++) {
			analysisDeclare(&analysis, &scope,
				(LiteralNode *) scopeHandle(asObject(ordCollAt(temps, i))));
		}
		analyzeStatements(&analysis, &scope, blockNodeGetExpressions(body));
	}
	if (error->status != COMPILE_OK) {
		closeHandleScope(&outer, NULL);
		return NULL;
	}

	// A stand-in emitter, holding only what a unit inherits from the context it
	// is compiled in. The method's own unit is compiled through the same
	// function a block's is.
	Emitter root;
	memset(&root, 0, sizeof root);
	root.context = context;
	root.error = error;
	root.analysis = &analysis;
	root.instanceVariables = analysis.instanceVariables;

	OrderedCollection *args = blockNodeGetArgs(body);
	uint16_t argumentCount = (uint16_t) (args == NULL ? 0 : ordCollSize(args));
	CodeUnit *unit = compileUnit(&root, body, argumentCount, NULL, NULL,
		methodNodeGetSelector(method), primitive, 0);
	if (unit == NULL) {
		closeHandleScope(&outer, NULL);
		return NULL;
	}

	// Every block the analysis prepared was emitted exactly once. The other
	// direction of a disagreement between the two walks (a block the emitter
	// inlined and the analysis did not) leaves an unused capture list and a
	// variable needlessly in a cell, which is correct and silent, and this is
	// what makes it loud.
	size_t blockCount = ordCollSize(analysis.blockNodes);
	for (size_t i = 0; i < blockCount; i++) {
		ASSERT(asCInt(ordCollAt(analysis.blockUses, i)) == 1);
	}

	closeHandleScope(&outer, NULL);
	return unit;
}


const char *compileStatusName(CompileStatus status)
{
	switch (status) {
	case COMPILE_OK: return "ok";
	case COMPILE_UNDECLARED_NAME: return "undeclared name";
	case COMPILE_TOO_MANY_REGISTERS: return "too many registers";
	case COMPILE_TOO_MANY_LITERALS: return "too many literals";
	case COMPILE_TOO_MANY_INSTRUCTIONS: return "too many instructions";
	case COMPILE_TOO_MANY_CAPTURES: return "too many captured variables";
	case COMPILE_BAD_INLINE_BLOCK: return "block of the wrong shape";
	case COMPILE_UNKNOWN_PRIMITIVE: return "unknown primitive name";
	default: return "unsupported construct";
	}
}


void codeUnitPrint(const CodeUnit *unit)
{
	for (uint16_t i = 0; i < unit->instructionCount; i++) {
		const Instruction *instruction = &unit->code[i];
		printf("  %3u  %-10s n=%-3u a=%-5u b=%-5u c=%u\n", i,
			opcodeName((Opcode) instruction->op), instruction->n,
			instruction->a, instruction->b, instruction->c);
	}
}
