#include "compiler/Compile.h"
#include "core/Assert.h"
#include "core/Class.h"
#include "core/Smalltalk.h"
#include "runtime/Collection.h"
#include "runtime/Primitive.h"
#include "runtime/String.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Phase 1: resolution
// ---------------------------------------------------------------------------

typedef enum {
	PLACE_NONE,
	PLACE_SELF,
	PLACE_SUPER,        // self, but a send through it starts lookup one level up
	PLACE_REGISTER,     // an argument or a temporary: index IS the frame slot
	PLACE_INSTANCE_VAR,
	PLACE_GLOBAL,       // index is a literal slot holding the Association
} PlaceKind;

typedef struct {
	PlaceKind kind;
	uint16_t index;
} Place;

// One lexical level: a method, or a block that has been INLINED into it.
//
// An inlined block shares the enclosing frame, so its arguments and temporaries
// are ordinary registers of the same method and resolution just walks out
// through `parent`. That is the whole reason inlined control flow costs nothing
// here: there is no capture, no cell and no closure, because there is no
// separate activation.
typedef struct Scope {
	struct Scope *parent;
	RawObject **names;   // interned Symbols, one per local, in register order
	uint16_t *registers;
	uint16_t count;
	uint16_t capacity;
} Scope;

typedef struct {
	// -- output
	Instruction *code;
	uint16_t count;
	uint16_t capacity;
	OrderedCollection *literals;

	// -- frame
	uint16_t registerCount; // high-water mark: the frame is this wide
	uint16_t top;           // first free register, a stack discipline

	// -- context
	Scope *scope;
	const CompileContext *context;
	OrderedCollection *instanceVariables; // Symbols, in slot order
	CompileError *error;
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
	scope->names = NULL;
	scope->registers = NULL;
	scope->count = 0;
	scope->capacity = 0;
	e->scope = scope;
}


static void scopePop(Emitter *e, Scope *scope)
{
	ASSERT(e->scope == scope);
	e->scope = scope->parent;
	free(scope->names);
	free(scope->registers);
}


// Declare a local and give it a register. The register is permanent for the
// scope's lifetime, which is why locals are allocated before any expression of
// the body: the expression stack has to start above them.
static uint16_t scopeDeclare(Emitter *e, Scope *scope, String *name)
{
	if (scope->count == scope->capacity) {
		scope->capacity = scope->capacity == 0 ? 8 : scope->capacity * 2;
		scope->names = realloc(scope->names, scope->capacity * sizeof(RawObject *));
		scope->registers = realloc(scope->registers, scope->capacity * sizeof(uint16_t));
		ASSERT(scope->names != NULL && scope->registers != NULL);
	}
	uint16_t reg = allocRegister(e);
	// The Symbol is INTERNED, so identity is the comparison and no string
	// compare happens during resolution.
	scope->names[scope->count] = (RawObject *) asSymbol(name)->raw;
	scope->registers[scope->count] = reg;
	scope->count++;
	return reg;
}


static _Bool nameIs(RawObject *symbol, const char *text)
{
	return rawStringEqualsBytes((RawString *) symbol, text, strlen(text));
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


// Where does this identifier live? Innermost scope outward, then instance
// variables, then globals. The order IS Smalltalk's shadowing rule.
static Place resolveName(Emitter *e, String *name)
{
	Place place = { PLACE_NONE, 0 };
	RawObject *symbol = (RawObject *) asSymbol(name)->raw;

	if (nameIs(symbol, "self")) {
		place.kind = PLACE_SELF;
		return place;
	}
	if (nameIs(symbol, "super")) {
		place.kind = PLACE_SUPER;
		return place;
	}

	for (Scope *scope = e->scope; scope != NULL; scope = scope->parent) {
		// BACKWARD, so a later declaration of the same name shadows an earlier
		// one within a scope, which is what a block argument reusing an outer
		// temporary's name has to do.
		for (uint16_t i = scope->count; i > 0; i--) {
			if (scope->names[i - 1] == symbol) {
				place.kind = PLACE_REGISTER;
				place.index = scope->registers[i - 1];
				return place;
			}
		}
	}

	if (e->instanceVariables != NULL) {
		size_t size = ordCollSize(e->instanceVariables);
		for (size_t i = 0; i < size; i++) {
			if (asObject(ordCollAt(e->instanceVariables, i)) == symbol) {
				place.kind = PLACE_INSTANCE_VAR;
				place.index = (uint16_t) i;
				return place;
			}
		}
	}

	if (e->context->globals != NULL) {
		String *key = scopeHandle(symbol);
		Association *association = symbolDictAssocAt(e->context->globals, key);
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
		Value super = current->raw->superClass;
		current = valueTypeOf(super, VALUE_POINTER)
			? scopeHandle(asObject(super)) : NULL;
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
// Phase 2: emission
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


// Load whatever `place` names into `dest`.
static void emitLoad(Emitter *e, Place place, String *name, uint16_t dest)
{
	switch (place.kind) {
	case PLACE_SELF:
	case PLACE_SUPER:
		emit(e, OP_MOVE, 0, dest, 0, 0); // register 0 IS self
		break;
	case PLACE_REGISTER:
		emit(e, OP_MOVE, 0, dest, place.index, 0);
		break;
	case PLACE_INSTANCE_VAR:
		emit(e, OP_GETIVAR, 0, dest, 0, place.index);
		break;
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
	case PLACE_INSTANCE_VAR:
		emit(e, OP_SETIVAR, 0, 0, place.index, source);
		break;
	case PLACE_GLOBAL:
		emit(e, OP_SETGLOBAL, 0, place.index, source, 0);
		break;
	default:
		// Assigning to self, to super, or to a name that resolves nowhere.
		fail(e, COMPILE_UNDECLARED_NAME, name);
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


static void emitLiteral(Emitter *e, LiteralNode *node, uint16_t dest)
{
	Value value = literalNodeGetValue(node);
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


// ---- inlined control flow ---------------------------------------------------
//
// ifTrue:, and:, whileTrue: and to:do: are compiled INTO the method, with their
// block arguments becoming ordinary statements of the same frame. Without this
// there is no loop for the optimizer to work on: every iteration would be a
// send and a block activation, and the phase-6 target could not exist.
//
// The Boolean check is a JUMPTRUE/JUMPFALSE pair rather than a GUARDCLASS, and
// the difference is worth stating because docs/jit-v2/04-bytecode.md names
// GUARDCLASS: true and false are SINGLETONS, so testing identity against them is
// one compare against an immediate, while a class guard is a load, a mask and a
// compare, and it would take TWO of them to establish "Boolean" rather than
// "True". The pair is both cheaper and more precise. GUARDCLASS keeps its other
// documented use, which is the tier's own class speculation.

static _Bool isBlockOfArity(Object *node, size_t arity)
{
	if (!isInstanceOf(node->raw, &Handles.BlockNode)) {
		return 0;
	}
	OrderedCollection *args = blockNodeGetArgs((BlockNode *) node);
	return args != NULL && ordCollSize(args) == arity;
}


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
			uint16_t reg = scopeDeclare(e, &scope,
				literalNodeGetStringValue(declaration));
			emit(e, OP_LOADNIL, 0, reg, 0, 0);
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
	uint16_t skip = emitJumpForward(e, isAnd ? OP_JUMPFALSE : OP_JUMPTRUE, testReg);
	emitInlineBlock(e, rest, dest, 1);
	uint16_t done = emitJumpForward(e, OP_JUMP, 0);
	patchHere(e, skip);
	emit(e, isAnd ? OP_LOADFALSE : OP_LOADTRUE, 0, dest, 0, 0);
	patchHere(e, done);
}


// Send `selector` to `receiverReg` with `argc` arguments already sitting in the
// registers above it, answering into `dest`.
static void emitSendTo(Emitter *e, uint16_t dest, String *selector,
	uint16_t receiverReg, uint8_t argc, _Bool toSuper)
{
	emit(e, toSuper ? OP_SENDSUPER : OP_SEND, argc, dest,
		selectorIndex(e, selector), receiverReg);
}


// `start to: stop do: [:i | ...]`.
//
// The comparison and the increment are ORDINARY SENDS, deliberately: `<=` and
// `+` are exactly the sites ADR 0006 says must carry a profile, and a loop
// counter is the hottest instance of both in the system. What is inlined is the
// CONTROL FLOW, not the arithmetic.
static void emitToDo(Emitter *e, ExpressionNode *receiverExpression,
	Object *receiverNode, Object *limitNode, BlockNode *body, uint16_t dest)
{
	(void) receiverExpression;
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
	// activation: that is what inlining to:do: buys.
	Scope scope;
	scopePush(e, &scope);
	OrderedCollection *args = blockNodeGetArgs(body);
	LiteralNode *declaration = scopeHandle(asObject(ordCollAt(args, 0)));
	String *name = literalNodeGetStringValue(declaration);
	if (scope.count == scope.capacity) {
		scope.capacity = 8;
		scope.names = realloc(scope.names, scope.capacity * sizeof(RawObject *));
		scope.registers = realloc(scope.registers, scope.capacity * sizeof(uint16_t));
		ASSERT(scope.names != NULL && scope.registers != NULL);
	}
	scope.names[scope.count] = (RawObject *) asSymbol(name)->raw;
	scope.registers[scope.count] = counter;
	scope.count++;

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
	uint16_t exit = emitJumpForward(e, whileTrue ? OP_JUMPFALSE : OP_JUMPTRUE, test);
	releaseTo(e, mark);

	if (body != NULL) {
		mark = e->top;
		uint16_t discard = allocRegister(e);
		emitInlineBlock(e, body, discard, 0);
		releaseTo(e, mark);
	}
	emit(e, OP_JUMP, 0, loop, 0, 0);
	patchHere(e, exit);
	emitNil(e, dest); // whileTrue: answers nil
}


// Try to compile `receiver selector: args` as inlined control flow. Answers
// whether it did; when it did not, nothing was emitted and the caller falls
// back to an ordinary send.
static _Bool emitInlinedMessage(Emitter *e, Object *receiverNode,
	String *selector, OrderedCollection *args, uint16_t receiverReg,
	_Bool receiverEmitted, uint16_t dest)
{
	size_t argc = args == NULL ? 0 : ordCollSize(args);
	Object *arg0 = argc > 0 ? scopeHandle(asObject(ordCollAt(args, 0))) : NULL;
	Object *arg1 = argc > 1 ? scopeHandle(asObject(ordCollAt(args, 1))) : NULL;

	// The receiverless loop forms take a BLOCK receiver and must be caught
	// before the receiver has been evaluated, because evaluating it would
	// allocate the closure this exists to avoid.
	if (!receiverEmitted && isInstanceOf(receiverNode->raw, &Handles.BlockNode)) {
		BlockNode *conditionBlock = (BlockNode *) receiverNode;
		if (!isBlockOfArity(receiverNode, 0)) {
			return 0;
		}
		if (argc == 0 && (stringEqualsC(selector, "whileTrue")
				|| stringEqualsC(selector, "whileFalse"))) {
			emitWhile(e, conditionBlock, NULL, stringEqualsC(selector, "whileTrue"), dest);
			return 1;
		}
		if (argc == 1 && isBlockOfArity(arg0, 0)
				&& (stringEqualsC(selector, "whileTrue:")
					|| stringEqualsC(selector, "whileFalse:"))) {
			emitWhile(e, conditionBlock, (BlockNode *) arg0,
				stringEqualsC(selector, "whileTrue:"), dest);
			return 1;
		}
		return 0;
	}

	if (argc == 1 && isBlockOfArity(arg0, 0)) {
		if (stringEqualsC(selector, "ifTrue:")) {
			emitConditional(e, receiverReg, (BlockNode *) arg0, NULL, dest);
			return 1;
		}
		if (stringEqualsC(selector, "ifFalse:")) {
			emitConditional(e, receiverReg, NULL, (BlockNode *) arg0, dest);
			return 1;
		}
		if (stringEqualsC(selector, "and:")) {
			emitShortCircuit(e, receiverReg, (BlockNode *) arg0, 1, dest);
			return 1;
		}
		if (stringEqualsC(selector, "or:")) {
			emitShortCircuit(e, receiverReg, (BlockNode *) arg0, 0, dest);
			return 1;
		}
	}
	if (argc == 2 && isBlockOfArity(arg0, 0) && isBlockOfArity(arg1, 0)) {
		if (stringEqualsC(selector, "ifTrue:ifFalse:")) {
			emitConditional(e, receiverReg, (BlockNode *) arg0, (BlockNode *) arg1, dest);
			return 1;
		}
		if (stringEqualsC(selector, "ifFalse:ifTrue:")) {
			emitConditional(e, receiverReg, (BlockNode *) arg1, (BlockNode *) arg0, dest);
			return 1;
		}
	}
	return 0;
}


// One message of an expression, sent to a receiver already in `receiverReg`.
static void emitMessage(Emitter *e, MessageExpressionNode *message,
	uint16_t receiverReg, _Bool toSuper, uint16_t dest)
{
	String *selector = messageExpressionNodeGetSelector(message);
	OrderedCollection *args = messageExpressionNodeGetArgs(message);
	size_t argc = args == NULL ? 0 : ordCollSize(args);

	if (!toSuper && emitInlinedMessage(e, NULL, selector, args, receiverReg, 1, dest)) {
		return;
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
		// A block that was NOT inlined needs a real closure, which is the next
		// milestone (ADR 0008: flat closures with cells). Failing cleanly here
		// beats emitting something that looks like it works.
		fail(e, COMPILE_UNSUPPORTED, stringFromC("a non-inlined block"));
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
	// where the receiverless loop forms can be inlined, because inlining them
	// depends on NOT building the receiver block.
	if (messageCount == 1) {
		MessageExpressionNode *only = scopeHandle(asObject(ordCollAt(messages, 0)));
		String *selector = messageExpressionNodeGetSelector(only);
		OrderedCollection *args = messageExpressionNodeGetArgs(only);
		if (isInstanceOf(receiverNode->raw, &Handles.BlockNode)
				&& emitInlinedMessage(e, receiverNode, selector, args, 0, 0, dest)) {
			goto assignments;
		}
		// to:do: needs its receiver node UNevaluated too, so the counter can be
		// the block's argument register rather than a copy of it.
		if (args != NULL && ordCollSize(args) == 2
				&& stringEqualsC(selector, "to:do:")) {
			Object *limit = scopeHandle(asObject(ordCollAt(args, 0)));
			Object *body = scopeHandle(asObject(ordCollAt(args, 1)));
			if (isBlockOfArity(body, 1)) {
				emitToDo(e, node, receiverNode, limit, (BlockNode *) body, dest);
				goto assignments;
			}
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
			emit(e, OP_RET, 0, into, 0, 0);
			releaseTo(e, mark);
			closeHandleScope(&scope, NULL);
			return; // everything after an explicit return is unreachable
		}
		releaseTo(e, mark);
		closeHandleScope(&scope, NULL);
	}
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

	Emitter e;
	memset(&e, 0, sizeof(e));
	e.context = context;
	e.error = error;
	e.literals = newOrdColl(8);
	e.instanceVariables = collectInstanceVariables(context->ownerClass);

	BlockNode *body = methodNodeGetBody(method);

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

	Scope scope;
	scopePush(&e, &scope);

	// Register 0 is self, ALWAYS, in every unit. That is the frame contract the
	// tier-1 template and the deopt map both read (jit/Jit.h).
	allocRegister(&e);

	OrderedCollection *args = blockNodeGetArgs(body);
	size_t argumentCount = args == NULL ? 0 : ordCollSize(args);
	for (size_t i = 0; i < argumentCount; i++) {
		LiteralNode *arg = scopeHandle(asObject(ordCollAt(args, i)));
		scopeDeclare(&e, &scope, literalNodeGetStringValue(arg));
	}

	OrderedCollection *temps = blockNodeGetTempVars(body);
	size_t tempCount = temps == NULL ? 0 : ordCollSize(temps);
	for (size_t i = 0; i < tempCount; i++) {
		LiteralNode *declaration = scopeHandle(asObject(ordCollAt(temps, i)));
		uint16_t reg = scopeDeclare(&e, &scope,
			literalNodeGetStringValue(declaration));
		emit(&e, OP_LOADNIL, 0, reg, 0, 0);
	}

	uint16_t result = allocRegister(&e);
	emitStatements(&e, blockNodeGetExpressions(body), result, 1);
	// A method with no explicit return answers self, and the trailing RET is
	// unconditional: falling off the end of generated code has no defined
	// meaning, so there is always one here even when the body ended in one.
	emit(&e, OP_MOVE, 0, result, 0, 0);
	emit(&e, OP_RET, 0, result, 0, 0);

	scopePop(&e, &scope);

	if (failed(&e)) {
		free(e.code);
		closeHandleScope(&outer, NULL);
		return NULL;
	}

	CodeUnit *unit = calloc(1, sizeof(CodeUnit));
	ASSERT(unit != NULL);
	unit->code = e.code;
	unit->instructionCount = e.count;
	unit->registerCount = e.registerCount;
	unit->argumentCount = (uint16_t) argumentCount;
	unit->primitive = primitive;
	unit->literals = objectTagged(ordCollAsArray(e.literals));
	unit->selector = objectTagged(asSymbol(methodNodeGetSelector(method)));
	if (context->ownerClass != NULL) {
		unit->ownerClass = objectTagged(context->ownerClass);
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
