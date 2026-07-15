#include "compiler/Scope.h"
#include "compiler/Variable.h"
#include "runtime/Dictionary.h"
#include "compiler/Compiler.h"
#include "core/Smalltalk.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "runtime/Iterator.h"
#include "core/Class.h"
#include "runtime/String.h"
#include "compiler/Bytecodes.h"
#include <stdlib.h>
#include "core/Assert.h"
#include <string.h>

#define RETURN_IF_ERROR() \
	if (blockScopeHasError(blockScope)) { \
		closeHandleScope(&scope, NULL); \
		return; \
	}

static void analyzeInstanceVars(BlockScope *blockScope, Array *instVars);
static void analyzeBlock(BlockScope *blockScope, BlockNode *node);
static void analyzeDefinitions(BlockScope *blockScope, BlockNode *node);
static _Bool isDuplicateVariable(Dictionary *vars, String *name);
static void analyzeExpression(BlockScope *blockScope, ExpressionNode *node);
static void analyzeAssigments(BlockScope *blockScope, ExpressionNode *node);
static void analyzeAssigment(BlockScope *blockScope, LiteralNode *literal);
static CompileError *createReadonlyVariableError(LiteralNode *node);
static void analyzeMessageExpression(BlockScope *blockScope, MessageExpressionNode *node);
static void analyzeLiteral(BlockScope *blockScope, Object *literal);
static void analyzeVar(BlockScope *blockScope, LiteralNode *name);
static _Bool analyzeContextVar(BlockScope *blockScope, String *name);
static void setupBlockMetasAsContexts(BlockScope *blockScope, BlockScope *upTo);
static _Bool analyzeGlobalVar(BlockScope *blockScope, String *name);
static _Bool analyzeDictionaryVar(BlockScope *blockScope, Dictionary *dict, String *name);
static BlockScope *createBlockScope(BlockScope *parent);


BlockScope *analyzeMethod(MethodNode *node, Class *class)
{
	HandleScope scope;
	openHandleScope(&scope);

	BlockScope *blockScope = createBlockScope((BlockScope *) Handles.nil);
	Array *instVars = classGetInstanceVariables(class);

	if (class->raw->class == Handles.MetaClass->raw) {
		blockScopeSetOwnerClass(blockScope, metaClassGetInstanceClass((MetaClass *) class));
	} else {
		blockScopeSetOwnerClass(blockScope, class);
	}

	blockScopeSetLiterals(blockScope, newOrdColl(64));

	if (!isNil(instVars)) {
		analyzeInstanceVars(blockScope, instVars);
	}
	analyzeBlock(blockScope, methodNodeGetBody(node));
	return closeHandleScope(&scope, blockScope);
}


static void analyzeInstanceVars(BlockScope *blockScope, Array *instVars)
{
	HandleScope scope;
	openHandleScope(&scope);

	Dictionary *vars = blockScopeGetVars(blockScope);
	Iterator iterator;

	initArrayIterator(&iterator, instVars, 0, 0);
	while (iteratorHasNext(&iterator)) {
		Value var = defineVariable(OPERAND_INST_VAR, iteratorIndex(&iterator), 0);
		stringDictAtPut(vars, (String *) iteratorNextObject(&iterator), var);
	}

	closeHandleScope(&scope, NULL);
}


static void analyzeBlock(BlockScope *blockScope, BlockNode *node)
{
	blockNodeSetScope(node, blockScope);
	analyzeDefinitions(blockScope, node);
	if (blockScopeHasError(blockScope)) {
		return;
	}

	Iterator iterator;
	initOrdCollIterator(&iterator, blockNodeGetExpressions(node), 0, 0);

	while (iteratorHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);
		analyzeExpression(blockScope, (ExpressionNode *) iteratorNextObject(&iterator));
		RETURN_IF_ERROR();
		closeHandleScope(&scope, NULL);
	}
}


static void analyzeDefinitions(BlockScope *blockScope, BlockNode *node)
{
	HandleScope scope;
	openHandleScope(&scope);

	Dictionary *vars = blockScopeGetVars(blockScope);
	Iterator iterator;
	uint8_t index = 0;

	stringDictAtPut(vars, asString("thisContext"), defineVariable(OPERAND_THIS_CONTEXT, index++, 0));
	stringDictAtPut(vars, asString("super"), defineVariable(OPERAND_SUPER, index, 0));
	stringDictAtPut(vars, asString("self"), defineVariable(OPERAND_ARG_VAR, index++, 0));

	initOrdCollIterator(&iterator, blockNodeGetArgs(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		LiteralNode *arg = (LiteralNode *) iteratorNextObject(&iterator);
		String *name = literalNodeGetStringValue(arg);
		if (isDuplicateVariable(vars, name)) {
			blockScopeSetError(blockScope, createRedefinitionError(arg));
		}
		stringDictAtPut(vars, name, defineVariable(OPERAND_ARG_VAR, index++, 0));
	}

	blockScope->raw->header.argsSize = index - 2; // -2 for thisContext and self

	initOrdCollIterator(&iterator, blockNodeGetTempVars(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		LiteralNode *arg = (LiteralNode *) iteratorNextObject(&iterator);
		String *name = literalNodeGetStringValue(arg);
		if (isDuplicateVariable(vars, name)) {
			blockScopeSetError(blockScope, createRedefinitionError(arg));
		}
		stringDictAtPut(vars, name, defineVariable(OPERAND_TEMP_VAR, index++, 0));
	}

	closeHandleScope(&scope, NULL);
}


static _Bool isDuplicateVariable(Dictionary *vars, String *name)
{
	Value var = stringDictAt(vars, name);
	if (isTaggedNil(var)) {
		return 0;
	}
	switch (getVarType(var)) {
	case OPERAND_THIS_CONTEXT:
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_SUPER:
	case OPERAND_CONTEXT_VAR:
		return 1;
	default:
		return 0;
	}
}


static void analyzeExpression(BlockScope *blockScope, ExpressionNode *node)
{
	analyzeAssigments(blockScope, node);
	analyzeLiteral(blockScope, (Object *) expressionNodeGetReceiver(node));
	if (blockScopeHasError(blockScope)) {
		return;
	}

	Iterator iterator;
	initOrdCollIterator(&iterator, expressionNodeGetMessageExpressions(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);
		analyzeMessageExpression(blockScope, (MessageExpressionNode *) iteratorNextObject(&iterator));
		RETURN_IF_ERROR();
		closeHandleScope(&scope, NULL);
	}
}


static void analyzeAssigments(BlockScope *blockScope, ExpressionNode *node)
{
	HandleScope scope;
	openHandleScope(&scope);

	Iterator iterator;
	initOrdCollIterator(&iterator, expressionNodeGetAssigments(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		analyzeAssigment(blockScope, (LiteralNode *) iteratorNextObject(&iterator));
		RETURN_IF_ERROR();
	}

	closeHandleScope(&scope, NULL);
}


static void analyzeAssigment(BlockScope *blockScope, LiteralNode *literal)
{
	Value var;

	if (literal->raw->class != Handles.VariableNode->raw) {
		blockScopeSetError(blockScope, createReadonlyVariableError(literal));
	} else {
		analyzeVar(blockScope, literal);
		var = stringDictAt(blockScopeGetVars(blockScope), literalNodeGetStringValue(literal));
		if (!isTaggedNil(var) && (getVarType(var) == OPERAND_ARG_VAR || getVarType(var) == OPERAND_SUPER || hasVarCtxCopy(var))) {
			blockScopeSetError(blockScope, createReadonlyVariableError(literal));
		}
	}
}


static CompileError *createReadonlyVariableError(LiteralNode *node)
{
	CompileError *error = (CompileError *) newObject(Handles.ReadonlyVariableError, 0);
	objectStorePtr((Object *) error,  &error->raw->variable, (Object *) node);
	return error;
}


// A block argument to a control-flow selector is inlined (its body spliced into
// the enclosing method) only when it is a literal block with no args and no
// temps — see compileInlinedControlFlow in Compiler.c, which must agree.
static _Bool isInlinableBlockNode(Object *node)
{
	if (node->raw->class != Handles.BlockNode->raw) {
		return 0;
	}
	BlockNode *block = (BlockNode *) node;
	return ordCollSize(blockNodeGetArgs(block)) == 0
		&& ordCollSize(blockNodeGetTempVars(block)) == 0;
}


static _Bool messageIsInlinableControlFlow(MessageExpressionNode *node)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_INLINE_CF") == NULL;
	}
	if (!enabled) {
		return 0;
	}
	String *selector = messageExpressionNodeGetSelector(node);
	if (!(stringEqualsC(selector, "ifTrue:")
		|| stringEqualsC(selector, "ifFalse:")
		|| stringEqualsC(selector, "ifTrue:ifFalse:")
		|| stringEqualsC(selector, "ifFalse:ifTrue:")
		|| stringEqualsC(selector, "and:")
		|| stringEqualsC(selector, "or:"))) {
		return 0;
	}
	Iterator it;
	initOrdCollIterator(&it, messageExpressionNodeGetArgs(node), 0, 0);
	while (iteratorHasNext(&it)) {
		if (!isInlinableBlockNode(iteratorNextObject(&it))) {
			return 0;
		}
	}
	return 1;
}


// Analyze an inlined block's statements directly in the ENCLOSING scope so its
// variable references resolve at the enclosing level (no context promotion, no
// forced context). Valid only for arg-free/temp-free blocks.
static void analyzeInlinedBlock(BlockScope *blockScope, BlockNode *node)
{
	blockNodeSetScope(node, blockScope);
	Iterator iterator;
	initOrdCollIterator(&iterator, blockNodeGetExpressions(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);
		analyzeExpression(blockScope, (ExpressionNode *) iteratorNextObject(&iterator));
		RETURN_IF_ERROR();
		closeHandleScope(&scope, NULL);
	}
}


// `to: stop do: [:i | ...]` with a literal one-arg, temp-free block is inlined into
// the enclosing frame (no closure, no BlockContext, no per-iteration send): the loop
// variable becomes an enclosing temp. Purely syntactic — kept in sync with Compiler.c's
// compileLoopKind, which additionally verifies Scope.c inlined the block (scope marker).
static _Bool messageIsInlinableLoop(MessageExpressionNode *node)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_INLINE_CF") == NULL;
	}
	if (!enabled) {
		return 0;
	}
	if (!stringEqualsC(messageExpressionNodeGetSelector(node), "to:do:")) {
		return 0;
	}
	OrderedCollection *args = messageExpressionNodeGetArgs(node);
	if (ordCollSize(args) != 2) {
		return 0;
	}
	Object *blockObj = ordCollObjectAt(args, 1);
	if (blockObj->raw->class != Handles.BlockNode->raw) {
		return 0;
	}
	BlockNode *block = (BlockNode *) blockObj;
	return ordCollSize(blockNodeGetArgs(block)) == 1
		&& ordCollSize(blockNodeGetTempVars(block)) == 0;
}


static _Bool exprHasRealBlock(ExpressionNode *e);

// A block LITERAL passed to a non-inlined selector becomes a real closure that captures
// its free variables by reference. If the loop body contains such a block, inlining the
// loop (which flattens the loop variable into a single enclosing slot) would give every
// closure the loop variable's FINAL value instead of a per-iteration binding. Detect any
// such real block so the loop falls back to a normal send (real block => correct capture).
// Block args to inlined control-flow / to:do: selectors are spliced, not captured, so we
// recurse into them rather than flag them.
// The walk allocates scope handles (ordCollObjectAt / iteratorNextObject) but does no GC,
// so each call brackets its handles in its own HandleScope to keep the recursion from
// overflowing the fixed handle scope.
static _Bool nodeHasRealBlock(Object *node)
{
	if (node->raw->class == Handles.ExpressionNode->raw) {
		return exprHasRealBlock((ExpressionNode *) node);
	}
	if (node->raw->class != Handles.BlockNode->raw) {
		return 0;
	}
	HandleScope scope;
	openHandleScope(&scope);
	_Bool found = 0;
	Iterator it;
	initOrdCollIterator(&it, blockNodeGetExpressions((BlockNode *) node), 0, 0);
	while (!found && iteratorHasNext(&it)) {
		found = exprHasRealBlock((ExpressionNode *) iteratorNextObject(&it));
	}
	closeHandleScope(&scope, NULL);
	return found;
}

static _Bool exprHasRealBlock(ExpressionNode *e)
{
	HandleScope scope;
	openHandleScope(&scope);
	_Bool found = nodeHasRealBlock((Object *) expressionNodeGetReceiver(e));
	Iterator it;
	initOrdCollIterator(&it, expressionNodeGetMessageExpressions(e), 0, 0);
	while (!found && iteratorHasNext(&it)) {
		MessageExpressionNode *m = (MessageExpressionNode *) iteratorNextObject(&it);
		_Bool inlined = messageIsInlinableControlFlow(m) || messageIsInlinableLoop(m);
		Iterator ai;
		initOrdCollIterator(&ai, messageExpressionNodeGetArgs(m), 0, 0);
		while (!found && iteratorHasNext(&ai)) {
			Object *arg = iteratorNextObject(&ai);
			found = (!inlined && arg->raw->class == Handles.BlockNode->raw) || nodeHasRealBlock(arg);
		}
	}
	closeHandleScope(&scope, NULL);
	return found;
}

static _Bool loopBodyIsCaptureFree(MessageExpressionNode *node)
{
	HandleScope scope;
	openHandleScope(&scope);
	BlockNode *block = (BlockNode *) ordCollObjectAt(messageExpressionNodeGetArgs(node), 1);
	_Bool found = 0;
	Iterator it;
	initOrdCollIterator(&it, blockNodeGetExpressions(block), 0, 0);
	while (!found && iteratorHasNext(&it)) {
		found = exprHasRealBlock((ExpressionNode *) iteratorNextObject(&it));
	}
	closeHandleScope(&scope, NULL);
	return !found;
}


// The loop variable is flattened into the enclosing scope, so inline only when its name
// is completely unbound here — otherwise it would clobber a temp/arg/inst var/self of the
// same name. Any collision falls back to a normal send (a real block).
static _Bool inlinedLoopVarIsFresh(BlockScope *blockScope, MessageExpressionNode *node)
{
	BlockNode *block = (BlockNode *) ordCollObjectAt(messageExpressionNodeGetArgs(node), 1);
	LiteralNode *loopArg = (LiteralNode *) ordCollObjectAt(blockNodeGetArgs(block), 0);
	return isTaggedNil(stringDictAt(blockScopeGetVars(blockScope), literalNodeGetStringValue(loopArg)));
}


// Analyze an inlined `to:do:`: the stop expression and the block body live in the
// enclosing scope, and the block's single argument (the loop variable) is declared as an
// enclosing temp so its references resolve at this level. processVariables() renumbers it.
static void analyzeInlinedLoop(BlockScope *blockScope, MessageExpressionNode *node)
{
	OrderedCollection *args = messageExpressionNodeGetArgs(node);
	analyzeLiteral(blockScope, ordCollObjectAt(args, 0));   // stop expression
	if (blockScopeHasError(blockScope)) {
		return;
	}
	BlockNode *block = (BlockNode *) ordCollObjectAt(args, 1);
	LiteralNode *loopArg = (LiteralNode *) ordCollObjectAt(blockNodeGetArgs(block), 0);
	String *name = literalNodeGetStringValue(loopArg);
	stringDictAtPut(blockScopeGetVars(blockScope), name, defineVariable(OPERAND_TEMP_VAR, 0, 0));
	analyzeInlinedBlock(blockScope, block);
}


static void analyzeMessageExpression(BlockScope *blockScope, MessageExpressionNode *node)
{
	HandleScope scope;
	openHandleScope(&scope);

	if (messageIsInlinableLoop(node) && inlinedLoopVarIsFresh(blockScope, node)
	    && loopBodyIsCaptureFree(node)) {
		analyzeInlinedLoop(blockScope, node);
		closeHandleScope(&scope, NULL);
		return;
	}

	Iterator iterator;
	initOrdCollIterator(&iterator, messageExpressionNodeGetArgs(node), 0, 0);
	if (messageIsInlinableControlFlow(node)) {
		while (iteratorHasNext(&iterator)) {
			analyzeInlinedBlock(blockScope, (BlockNode *) iteratorNextObject(&iterator));
			RETURN_IF_ERROR();
		}
		closeHandleScope(&scope, NULL);
		return;
	}
	while (iteratorHasNext(&iterator)) {
		analyzeLiteral(blockScope, iteratorNextObject(&iterator));
		RETURN_IF_ERROR();
	}

	closeHandleScope(&scope, NULL);
}


static void analyzeLiteral(BlockScope *blockScope, Object *literal)
{
	if (literal->raw->class == Handles.VariableNode->raw) {
		analyzeVar(blockScope, (LiteralNode *) literal);

	} else if (literal->raw->class == Handles.ExpressionNode->raw) {
		analyzeExpression(blockScope, (ExpressionNode *) literal);

	} else if (literal->raw->class == Handles.BlockNode->raw) {
		BlockScope *blockMeta = createBlockScope(blockScope);
		blockScopeSetOwnerClass(blockMeta, blockScopeGetOwnerClass(blockScope));
		blockScopeSetLiterals(blockMeta, blockScopeGetLiterals(blockScope));
		analyzeBlock(blockMeta, (BlockNode *) literal);
		blockScopeSetError(blockScope, blockScopeGetError(blockMeta));
	}
}


static void analyzeVar(BlockScope *blockScope, LiteralNode *literal)
{
	String *name = literalNodeGetStringValue(literal);
	Value var = stringDictAt(blockScopeGetVars(blockScope), name);

	if (!isTaggedNil(var)) {
		if (getVarIndex(var) == CONTEXT_INDEX) {
			blockScope->raw->header.hasContext = 1;
		}
		return;
	}
	if (analyzeContextVar(blockScope, name)) {
		return;
	}

	Class *class = blockScopeGetOwnerClass(blockScope);
	do {
		Dictionary *classVars = classGetClassVariables(class);
		if (!isNil((Object *) classVars) && analyzeDictionaryVar(blockScope, classVars, name)) {
			return;
		}
		class = classGetSuperClass(class);
	} while (!isNil((Object *) class));

	if (analyzeGlobalVar(blockScope, name)) {
		return;
	}

	blockScopeSetError(blockScope, createUndefinedVariableError(literal));
}


static _Bool analyzeContextVar(BlockScope *blockScope, String *name)
{
	uint8_t level = 1;
	BlockScope *ctx = blockScope;
	Value var;

	while (!isNil(ctx = blockScopeGetParent(ctx))) {
		Dictionary *vars = blockScopeGetVars(ctx);
		var = stringDictAt(vars, name);
		if (!isTaggedNil(var)) {
			if (getVarType(var) == OPERAND_ARG_VAR) {
				if (!hasVarCtxCopy(var)) {
					setVarCtxCopy(&var, ctx->raw->header.contextSize++);
					stringDictAtPut(vars, name, var);
				}
				setVarType(&var, OPERAND_CONTEXT_VAR);
				setVarIndex(&var, getVarCtxCopy(var));
			} else if (getVarType(var) == OPERAND_TEMP_VAR) {
				setVarType(&var, OPERAND_CONTEXT_VAR);
				setVarIndex(&var, ctx->raw->header.contextSize++);
				stringDictAtPut(vars, name, var);
			}
			setVarLevel(&var, level + getVarLevel(var));
			stringDictAtPut(blockScopeGetVars(blockScope), name, var);
			setupBlockMetasAsContexts(blockScope, ctx);
			return 1;
		}
		level++;
	}

	return 0;
}


static void setupBlockMetasAsContexts(BlockScope *blockScope, BlockScope *upTo)
{
	do {
		blockScope->raw->header.hasContext = 1;
	} while((blockScope = blockScopeGetParent(blockScope))->raw != upTo->raw);
	blockScope->raw->header.hasContext = 1;
}


static _Bool analyzeGlobalVar(BlockScope *blockScope, String *name)
{
	Value var;

	if (analyzeDictionaryVar(blockScope, Handles.Smalltalk, name)) {
		return 1;
	} else if (name->raw->contents[0] >= 'A' && name->raw->contents[0] <= 'Z') {
		OrderedCollection *literals = blockScopeGetLiterals(blockScope);
		Association *assoc = symbolDictAtPutObject(Handles.Smalltalk, asSymbol(name), Handles.nil);
		var = defineVariable(OPERAND_ASSOC, ordCollAddObjectIfNotExists(literals, (Object *) assoc), 0);
		stringDictAtPut(blockScopeGetVars(blockScope), name, var);
		return 1;
	}
	return 0;
}


static _Bool analyzeDictionaryVar(BlockScope *blockScope, Dictionary *dict, String *name)
{
	Association *assoc = symbolDictAssocAt(dict, asSymbol(name));
	Value var;

	if (isNil(assoc)) {
		return 0;
	} else {
		OrderedCollection *literals = blockScopeGetLiterals(blockScope);
		var = defineVariable(OPERAND_ASSOC, ordCollAddObjectIfNotExists(literals, (Object *) assoc), 0);
		stringDictAtPut(blockScopeGetVars(blockScope), name, var);
		return 1;
	}
}


static BlockScope *createBlockScope(BlockScope *parent)
{
	BlockScope *blockScope = (BlockScope *) newObject(Handles.BlockScope, 0);
	memset(&blockScope->raw->header, 0, sizeof(blockScope->raw->header));
	blockScopeSetParent(blockScope, parent);
	blockScopeSetVars(blockScope, newDictionary(32));
	return blockScope;
}
