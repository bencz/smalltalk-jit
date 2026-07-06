#include "Compiler.h"
#include "Scope.h"
#include "Variable.h"
#include "Heap.h"
#include "Smalltalk.h"
#include "Collection.h"
#include "Dictionary.h"
#include "Class.h"
#include "Handle.h"
#include "Iterator.h"
#include "Bytecodes.h"
#include "CodeDescriptors.h"
#include "Assert.h"
#include <stdlib.h>
#include <string.h>

typedef struct Compiler {
	struct Compiler *parent;
	AssemblerBuffer buffer;
	CompileError *error;
	CompiledCodeHeader header;
	BlockScope *scope;
	OrderedCollection *literals;
	OrderedCollection *descriptors;
	uintptr_t startLine;
	_Bool isBlock;
} Compiler;

static void compileMethodBody(Compiler *compiler, MethodNode *node);
static void processPragmas(Compiler *compiler, MethodNode *node);
static void processPrimitivePragma(Compiler *compiler, MessageExpressionNode *pragma);
static CompiledBlock *compileBlock(Compiler *parent, BlockNode *block);
static void initCompiler(Compiler *compiler, BlockNode *node);
static void freeCompiler(Compiler *compiler);
static void compileBody(Compiler *compiler, BlockNode *node, _Bool lastReturn);
static void processVariables(Compiler *compiler);
static void compileExpression(Compiler *compiler, ExpressionNode *node, _Bool store, Operand *result);
static void findResultTmpVar(Compiler *compiler, OrderedCollection *assigments, Operand *result);
static void createTmpVar(Compiler *compiler, Operand *operand);
static void compileAssigments(Compiler *compiler, OrderedCollection *assigments, Operand *result);
static _Bool compareOperands(Operand *a, Operand *b);
static void compileMessageExpression(Compiler *compiler, Operand *receiver, MessageExpressionNode *node, Operand *result);
static void compileLiteral(Compiler *compiler, LiteralNode *literal, Operand *operand);
static Array *compileArray(LiteralNode *node);
static void findVar(Compiler *compiler, LiteralNode *name, Operand *operand);
static void varAsOperand(Value var, Operand *operand);
static void notePosition(Compiler *compiler, SourceCode *sourceCode);
static CompiledMethod *createMethod(Compiler *compiler, MethodNode *node, Class *class);
static void setupMethodToBlocks(OrderedCollection *literals, CompiledMethod *method);
static CompiledBlock *createBlock(Compiler *compiler, BlockNode *node);
static void printSourceCode(SourceCode *source);


Object *compileMethod(MethodNode *node, Class *class)
{
	HandleScope scope;
	openHandleScope(&scope);

	Compiler compiler;
	BlockScope *blockScope = analyzeMethod(node, class);
	Object *result;

	if (blockScopeHasError(blockScope)) {
		return closeHandleScope(&scope, (Object *) blockScopeGetError(blockScope));
	}

	compileMethodBody(&compiler, node);
	if (compiler.error != NULL) {
		return closeHandleScope(&scope, (Object *) compiler.error);
	}

	result = (Object *) createMethod(&compiler, node, class);
	freeCompiler(&compiler);
	return closeHandleScope(&scope, result);
}


static void compileMethodBody(Compiler *compiler, MethodNode *node)
{
	BlockNode *body = methodNodeGetBody(node);
	BlockScope *blockScope = blockNodeGetScope(body);

	initCompiler(compiler, body);
	compiler->parent = NULL;
	compiler->literals = blockScopeGetLiterals(blockScope);
	compiler->startLine = sourceCodeGetLine(methodNodeGetSourceCode(node));
	processPragmas(compiler, node);
	if (compiler->error != NULL) {
		return;
	}
	compileBody(compiler, body, 0);
}


static void processPragmas(Compiler *compiler, MethodNode *node)
{
	HandleScope scope;
	openHandleScope(&scope);

	Iterator iterator;
	initOrdCollIterator(&iterator, methodNodeGetPragmas(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		MessageExpressionNode *pragma = (MessageExpressionNode *) iteratorNextObject(&iterator);
		if (stringEqualsC(messageExpressionNodeGetSelector(pragma), "primitive:")) {
			processPrimitivePragma(compiler, pragma);
			if (compiler->error != NULL) {
				closeHandleScope(&scope, NULL);
				return;
			}
		}
	}

	closeHandleScope(&scope, NULL);
}


static void processPrimitivePragma(Compiler *compiler, MessageExpressionNode *pragma)
{
	OrderedCollection *args = messageExpressionNodeGetArgs(pragma);
	ASSERT(ordCollSize(args) == 1);
	LiteralNode *arg = (LiteralNode *) ordCollObjectAt(args, 0);
	Value primitive;
	CompileError *error;

	if (arg->raw->class == Handles.VariableNode->raw) {
		primitive = globalAt(asSymbol(literalNodeGetStringValue(arg)));
	} else if (arg->raw->class == Handles.IntegerNode->raw) {
		primitive = literalNodeGetValue(arg);
	} else {
		goto error;
	}

	if (!valueTypeOf(primitive, VALUE_INT) || (primitive = asCInt(primitive)) > UINT16_MAX) {
		goto error;
	}

	compiler->header.primitive = primitive;
	return;

error:
	error = (CompileError *) newObject(Handles.InvalidPragmaError, 0);
	compileErrorSetVariable(error, (LiteralNode *) pragma);
	compiler->error = error;
}


static CompiledBlock *compileBlock(Compiler *parent, BlockNode *node)
{
	Compiler compiler;
	CompiledBlock *block;

	initCompiler(&compiler, node);
	compiler.parent = parent;
	compiler.literals = parent->literals;
	compiler.startLine = sourceCodeGetLine(blockNodeGetSourceCode(node));

	compileBody(&compiler, node, 1);
	block = createBlock(&compiler, node);
	freeCompiler(&compiler);
	return block;
}


static void initCompiler(Compiler *compiler, BlockNode *node)
{
	asmInitBuffer(&compiler->buffer, 256);
	compiler->error = NULL;
	compiler->scope = blockNodeGetScope(node);
	compiler->header = blockScopeGetHeader(compiler->scope);
	compiler->descriptors = newOrdColl(32);
}


static void freeCompiler(Compiler *compiler)
{
	asmFreeBuffer(&compiler->buffer);
}


static void compileBody(Compiler *compiler, BlockNode *node, _Bool isBlock)
{
	_Bool returns = 0;
	Operand result;
	Iterator iterator;

	// Record whether this compilation unit is a block, so an inlined conditional
	// that contains a `^` emits an outer (non-local) return when spliced into a
	// real block, and a plain method return when spliced into a method.
	compiler->isBlock = isBlock;

	processVariables(compiler);
	initOrdCollIterator(&iterator, blockNodeGetExpressions(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);

		ExpressionNode *expr = (ExpressionNode *) iteratorNextObject(&iterator);
		returns = expressionNodeReturns(expr) || (isBlock && !iteratorHasNext(&iterator));
		result.isValid = 0;
		compileExpression(compiler, expr, returns, &result);
		if (returns) {
			// Whether THIS return is a non-local (outer) return. Accumulate into the
			// header flag rather than overwriting it: an inlined conditional compiled
			// by compileExpression above may already have emitted an outer return and
			// set the flag, which setupMethodToBlocks needs to keep.
			_Bool outer = isBlock && expressionNodeReturns(expr);
			compiler->header.outerReturns = compiler->header.outerReturns || outer;
			bytecodeReturn(&compiler->buffer, &result, outer);
		}

		closeHandleScope(&scope, NULL);
	}

	if (isBlock && !returns) {
		result.type = OPERAND_NIL;
		bytecodeReturn(&compiler->buffer, &result, 0);
	}
	compiler->header.tempsSize = compiler->header.tempsSize - compiler->header.argsSize - 2;
}


static void processVariables(Compiler *compiler)
{
	HandleScope scope;
	openHandleScope(&scope);

	Iterator iterator;
	Operand src;
	Operand dst = { .isValid = 1, .type = OPERAND_CONTEXT_VAR, .level = 0 };
	uint8_t index = compiler->header.argsSize + 2; // +1 for thisContext and self

	initDictIterator(&iterator, blockScopeGetVars(compiler->scope));
	while (iteratorHasNext(&iterator)) {
		Association *assoc = (Association *) iteratorNextObject(&iterator);
		if (isNil(assoc)) {
			continue;
		}
		Value var = assoc->raw->value;
		if (getVarType(var) == OPERAND_TEMP_VAR) {
			setVarIndex(&assoc->raw->value, index++);
		} else if (getVarType(var) == OPERAND_ARG_VAR && hasVarCtxCopy(var)) {
			varAsOperand(var, &src);
			dst.index = getVarCtxCopy(var);
			bytecodeCopy(&compiler->buffer, &src, &dst);
		}
	}

	compiler->header.tempsSize = index;
	closeHandleScope(&scope, NULL);
}


static void compileExpression(Compiler *compiler, ExpressionNode *node, _Bool store, Operand *result)
{
	HandleScope scope;
	openHandleScope(&scope);

	OrderedCollection *assigments = expressionNodeGetAssigments(node);
	OrderedCollection *messages = expressionNodeGetMessageExpressions(node);

	if (ordCollSize(messages) > 0) {
		if (store || ordCollSize(assigments) > 0) {
			findResultTmpVar(compiler, assigments, result);
			if (!result->isValid) {
				createTmpVar(compiler, result);
			}
		}

		Operand receiver;
		compileLiteral(compiler, expressionNodeGetReceiver(node), &receiver);

		Iterator iterator;
		initOrdCollIterator(&iterator, messages, 0, 0);
		while (iteratorHasNext(&iterator)) {
			HandleScope scope2;
			openHandleScope(&scope2);
			MessageExpressionNode *messageExpr = (MessageExpressionNode *) iteratorNextObject(&iterator);
			Operand *messageExprResult = !iteratorHasNext(&iterator) && result->isValid ? result : NULL;
			compileMessageExpression(compiler, &receiver, messageExpr, messageExprResult);
			closeHandleScope(&scope2, NULL);
		}

	} else {
		compileLiteral(compiler, expressionNodeGetReceiver(node), result);
	}

	compileAssigments(compiler, assigments, result);
	closeHandleScope(&scope, NULL);
}


static void findResultTmpVar(Compiler *compiler, OrderedCollection *assigments, Operand *result)
{
	HandleScope scope;
	openHandleScope(&scope);

	Iterator iterator;
	initOrdCollIterator(&iterator, assigments, 0, 0);
	while (iteratorHasNext(&iterator)) {
		findVar(compiler, (LiteralNode *) iteratorNextObject(&iterator), result);
		if (result->type == OPERAND_TEMP_VAR || result->type == OPERAND_CONTEXT_VAR) {
			result->isValid = 1;
			closeHandleScope(&scope, NULL);
			return;
		}
	}
	result->isValid = 0;
	closeHandleScope(&scope, NULL);
}


static void createTmpVar(Compiler *compiler, Operand *operand)
{
	operand->isValid = 1;
	operand->type = OPERAND_TEMP_VAR;
	operand->index = compiler->header.tempsSize++;
}


static void compileAssigments(Compiler *compiler, OrderedCollection *assigments, Operand *source)
{
	HandleScope scope;
	openHandleScope(&scope);

	Operand var;
	Iterator iterator;
	initOrdCollIterator(&iterator, assigments, 0, 0);
	while (iteratorHasNext(&iterator)) {
		LiteralNode *assigment = (LiteralNode *) iteratorNextObject(&iterator);
		findVar(compiler, assigment, &var);
		if (!compareOperands(source, &var)) {
			notePosition(compiler, literalNodeGetSourceCode(assigment));
			bytecodeCopy(&compiler->buffer, source, &var);
		}
	}

	closeHandleScope(&scope, NULL);
}


static _Bool compareOperands(Operand *a, Operand *b)
{
	if (a->type != b->type) {
		return 0;
	}
	if (a->type == OPERAND_TEMP_VAR) {
		return a->index == b->index;
	}
	// Context vars are only the same location if BOTH index AND level match: two
	// captured vars can share an index at different nesting levels, and treating
	// them as equal would drop a real assignment (e.g. `a := b`).
	if (a->type == OPERAND_CONTEXT_VAR) {
		return a->index == b->index && a->level == b->level;
	}
	return 0;
}


// Inlined control-flow selectors (must agree with Scope.c's inline predicate).
enum { CF_NONE = 0, CF_IF_TRUE, CF_IF_FALSE, CF_IF_TRUE_FALSE, CF_IF_FALSE_TRUE, CF_AND, CF_OR };

static _Bool cfIsInlinableBlockNode(Object *node)
{
	if (node->raw->class != Handles.BlockNode->raw) {
		return 0;
	}
	BlockNode *block = (BlockNode *) node;
	return ordCollSize(blockNodeGetArgs(block)) == 0
		&& ordCollSize(blockNodeGetTempVars(block)) == 0;
}


// Returns the control-flow kind IF this message is an inlinable conditional (a
// control-flow selector whose every arg is an arg-free/temp-free literal block),
// else CF_NONE. Kept in sync with Scope.c's messageIsInlinableControlFlow.
static _Bool inlineControlFlowEnabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		enabled = getenv("ST_NO_INLINE_CF") == NULL;
	}
	return enabled;
}


static int compileControlFlowKind(MessageExpressionNode *node)
{
	if (!inlineControlFlowEnabled()) {
		return CF_NONE;
	}
	String *selector = messageExpressionNodeGetSelector(node);
	int kind = CF_NONE;
	if (stringEqualsC(selector, "ifTrue:")) kind = CF_IF_TRUE;
	else if (stringEqualsC(selector, "ifFalse:")) kind = CF_IF_FALSE;
	else if (stringEqualsC(selector, "ifTrue:ifFalse:")) kind = CF_IF_TRUE_FALSE;
	else if (stringEqualsC(selector, "ifFalse:ifTrue:")) kind = CF_IF_FALSE_TRUE;
	else if (stringEqualsC(selector, "and:")) kind = CF_AND;
	else if (stringEqualsC(selector, "or:")) kind = CF_OR;
	if (kind == CF_NONE) {
		return CF_NONE;
	}
	Iterator it;
	initOrdCollIterator(&it, messageExpressionNodeGetArgs(node), 0, 0);
	while (iteratorHasNext(&it)) {
		if (!cfIsInlinableBlockNode(iteratorNextObject(&it))) {
			return CF_NONE;
		}
	}
	return kind;
}


// Splice an inlined block's body into the current method: its last value is
// stored to `result` (a valid temp/context var, or NULL if unused); an explicit
// `^` becomes a PLAIN method return (BYTECODE_RETURN), since the block is not a
// real closure but part of the enclosing frame.
static void compileInlinedBlockBody(Compiler *compiler, BlockNode *block, Operand *result)
{
	Iterator iterator;
	initOrdCollIterator(&iterator, blockNodeGetExpressions(block), 0, 0);
	if (!iteratorHasNext(&iterator)) {
		if (result != NULL) {
			Operand nilOp = { .isValid = 1, .type = OPERAND_NIL };
			bytecodeCopy(&compiler->buffer, &nilOp, result);
		}
		return;
	}
	while (iteratorHasNext(&iterator)) {
		HandleScope scope;
		openHandleScope(&scope);
		ExpressionNode *expr = (ExpressionNode *) iteratorNextObject(&iterator);
		_Bool isLast = !iteratorHasNext(&iterator);
		_Bool explicitReturn = expressionNodeReturns(expr);
		Operand exprResult;
		exprResult.isValid = 0;
		compileExpression(compiler, expr, explicitReturn || (isLast && result != NULL), &exprResult);
		if (explicitReturn) {
			// A `^` in the inlined block is a return from the ENCLOSING unit: a
			// non-local (outer) return if that unit is a real block, else a plain
			// method return. When it is an outer return, flag it exactly like
			// compileBody does so setupMethodToBlocks forces the home context to be
			// materialized (else generateOuterReturn walks a missing context).
			if (compiler->isBlock) {
				compiler->header.outerReturns = 1;
				compiler->header.hasContext = 1;
			}
			bytecodeReturn(&compiler->buffer, &exprResult, compiler->isBlock);
		} else if (isLast && result != NULL && !compareOperands(&exprResult, result)) {
			bytecodeCopy(&compiler->buffer, &exprResult, result);
		}
		closeHandleScope(&scope, NULL);
	}
}


// Emit an inlined conditional: run the true-arm if the receiver is `true`, the
// false-arm if `false`, else send #mustBeBoolean. Each arm is either a spliced
// block body or a constant (for and:/or:). Value (if used) lands in `result`.
static void compileInlinedControlFlow(Compiler *compiler, Operand *receiver, int kind, OrderedCollection *args, Operand *result)
{
	AssemblerBuffer *buffer = &compiler->buffer;

	BlockNode *blocks[2] = { NULL, NULL };
	Iterator it;
	initOrdCollIterator(&it, args, 0, 0);
	for (int n = 0; n < 2 && iteratorHasNext(&it); n++) {
		blocks[n] = (BlockNode *) iteratorNextObject(&it);
	}

	BlockNode *trueBlk = NULL, *falseBlk = NULL;
	Operand trueConst = { .isValid = 1, .type = OPERAND_NIL };
	Operand falseConst = { .isValid = 1, .type = OPERAND_NIL };
	switch (kind) {
	case CF_IF_TRUE:       trueBlk = blocks[0]; break;
	case CF_IF_FALSE:      falseBlk = blocks[0]; break;
	case CF_IF_TRUE_FALSE: trueBlk = blocks[0]; falseBlk = blocks[1]; break;
	case CF_IF_FALSE_TRUE: falseBlk = blocks[0]; trueBlk = blocks[1]; break;
	case CF_AND:           trueBlk = blocks[0]; falseConst.type = OPERAND_FALSE; break;
	case CF_OR:            falseBlk = blocks[0]; trueConst.type = OPERAND_TRUE; break;
	}

	uint8_t trueClassIdx = ordCollAddObjectIfNotExists(compiler->literals, (Object *) Handles.True);
	uint8_t falseClassIdx = ordCollAddObjectIfNotExists(compiler->literals, (Object *) Handles.False);

	AssemblerLabel lFalse, lNotBool, lEnd1, lEnd2;
	asmInitLabel(&lFalse);
	asmInitLabel(&lNotBool);
	asmInitLabel(&lEnd1);
	asmInitLabel(&lEnd2);

	// true arm: taken when the receiver IS true
	bytecodeJumpNotMemberOf(buffer, receiver, trueClassIdx, &lFalse);
	if (trueBlk != NULL) {
		compileInlinedBlockBody(compiler, trueBlk, result);
	} else if (result != NULL) {
		bytecodeCopy(buffer, &trueConst, result);
	}
	bytecodeJump(buffer, &lEnd1);

	// false arm: taken when the receiver IS false
	asmLabelBind(buffer, &lFalse, asmOffset(buffer));
	bytecodeJumpNotMemberOf(buffer, receiver, falseClassIdx, &lNotBool);
	if (falseBlk != NULL) {
		compileInlinedBlockBody(compiler, falseBlk, result);
	} else if (result != NULL) {
		bytecodeCopy(buffer, &falseConst, result);
	}
	bytecodeJump(buffer, &lEnd2);

	// neither true nor false -> #mustBeBoolean (raises)
	asmLabelBind(buffer, &lNotBool, asmOffset(buffer));
	uint8_t mbbIdx = ordCollAddObjectIfNotExists(compiler->literals, (Object *) asSymbol(asString("mustBeBoolean")));
	bytecodeSend(buffer, mbbIdx, receiver, NULL, 0);

	asmLabelBind(buffer, &lEnd1, asmOffset(buffer));
	asmLabelBind(buffer, &lEnd2, asmOffset(buffer));
}


static void compileMessageExpression(Compiler *compiler, Operand *receiver, MessageExpressionNode *node, Operand *result)
{
	HandleScope scope;
	openHandleScope(&scope);
	AssemblerBuffer *buffer = &compiler->buffer;

	int cfKind = compileControlFlowKind(node);
	if (cfKind != CF_NONE) {
		compileInlinedControlFlow(compiler, receiver, cfKind, messageExpressionNodeGetArgs(node), result);
		closeHandleScope(&scope, NULL);
		return;
	}

	Operand args[256];
	Iterator iterator;
	initOrdCollIterator(&iterator, messageExpressionNodeGetArgs(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		ptrdiff_t index = iteratorIndex(&iterator);
		if (index >= 256) {
			FAIL();
		}
		compileLiteral(compiler, (LiteralNode *) iteratorNextObject(&iterator), &args[index]);
	}

	String *selector = messageExpressionNodeGetSelector(node);
	uint8_t literalIndex = ordCollAddObjectIfNotExists(compiler->literals, (Object *) asSymbol(selector));
	notePosition(compiler, messageExpressionNodeGetSourceCode(node));
	if (result == NULL) {
		bytecodeSend(buffer, literalIndex, receiver, args, iteratorIndex(&iterator));
	} else {
		bytecodeSendWithStore(buffer, literalIndex, receiver, result, args, iteratorIndex(&iterator));
	}

	closeHandleScope(&scope, NULL);
}


static void compileLiteral(Compiler *compiler, LiteralNode *literal, Operand *operand)
{
	operand->isValid = 1;

	if (literal->raw->class == Handles.VariableNode->raw) {
		findVar(compiler, literal, operand);

	} else if (literal->raw->class == Handles.NilNode->raw) {
		operand->type = OPERAND_NIL;

	} else if (literal->raw->class == Handles.TrueNode->raw) {
		operand->type = OPERAND_TRUE;

	} else if (literal->raw->class == Handles.FalseNode->raw) {
		operand->type = OPERAND_FALSE;

	} else if (literal->raw->class == Handles.IntegerNode->raw || literal->raw->class == Handles.CharacterNode->raw) {
		Value value = literalNodeGetValue(literal);
		if (valueTypeOf(value, VALUE_POINTER)) {
			/* arbitrary precision integer literal: keep it in the literal frame */
			operand->type = OPERAND_LITERAL;
			operand->index = ordCollAddObjectIfNotExists(compiler->literals, scopeHandle(asObject(value)));
		} else {
			operand->type = OPERAND_VALUE;
			operand->value = value;
		}

	} else if (literal->raw->class == Handles.StringNode->raw) {
		operand->type = OPERAND_LITERAL;
		operand->index = ordCollAddObjectIfNotExists(compiler->literals, (Object *) literalNodeGetStringValue(literal));

	} else if (literal->raw->class == Handles.SymbolNode->raw) {
		operand->type = OPERAND_LITERAL;
		operand->index = ordCollAddObjectIfNotExists(compiler->literals, (Object *) asSymbol(literalNodeGetStringValue(literal)));

	} else if (literal->raw->class == Handles.ArrayNode->raw) {
		operand->type = OPERAND_LITERAL;
		operand->index = ordCollAddObjectIfNotExists(compiler->literals, (Object *) compileArray(literal));

	} else if (literal->raw->class == Handles.BlockNode->raw) {
		operand->type = OPERAND_BLOCK;
		operand->index = ordCollAddObjectIfNotExists(compiler->literals, (Object *) compileBlock(compiler, (BlockNode *) literal));

	} else if (literal->raw->class == Handles.ExpressionNode->raw) {
		compileExpression(compiler, (ExpressionNode *) literal, 1, operand);
	}
}


static Array *compileArray(LiteralNode *node)
{
	HandleScope scope;
	openHandleScope(&scope);

	OrderedCollection *ordColl = literalNodeGetOrdCollValue(node);
	Array *array = newArray(ordCollSize(ordColl));
	Iterator iterator;

	initOrdCollIterator(&iterator, ordColl, 0, 0);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope2;
		openHandleScope(&scope2);
		ptrdiff_t index = iteratorIndex(&iterator);
		LiteralNode *literal = (LiteralNode *) iteratorNextObject(&iterator);
		if (literal->raw->class == Handles.ArrayNode->raw) {
			arrayAtPutObject(array, index, (Object *) compileArray(literal));
		} else if (literal->raw->class == Handles.SymbolNode->raw) {
			arrayAtPutObject(array, index, (Object *) asSymbol(literalNodeGetStringValue(literal)));
		} else if (literal->raw->class == Handles.NilNode->raw) {
			arrayAtPutObject(array, index, Handles.nil);
		} else if (literal->raw->class == Handles.TrueNode->raw) {
			arrayAtPutObject(array, index, Handles.true);
		} else if (literal->raw->class == Handles.FalseNode->raw) {
			arrayAtPutObject(array, index, Handles.false);
		} else {
			array->raw->vars[index] = literalNodeGetValue(literal);
		}
		closeHandleScope(&scope2, NULL);
	}
	return closeHandleScope(&scope, array);
}


static void findVar(Compiler *compiler, LiteralNode *name, Operand *operand)
{
	Value var = stringDictAt(blockScopeGetVars(compiler->scope), literalNodeGetStringValue(name));
	varAsOperand(var, operand);
}


static void varAsOperand(Value var, Operand *operand)
{
	operand->isValid = 1;
	operand->type = getVarType(var);
	operand->index = getVarIndex(var);
	operand->level = getVarLevel(var);
}


static void notePosition(Compiler *compiler, SourceCode *sourceCode)
{
	ptrdiff_t pos = compiler->buffer.instOffset;
	if (pos > UINT16_MAX) FAIL();
	uintptr_t line = sourceCodeGetLine(sourceCode) - compiler->startLine;
	if (line > UINT16_MAX) FAIL();
	uintptr_t column = sourceCodeGetColumn(sourceCode);
	if (column > UINT16_MAX) FAIL();
	ordCollAdd(compiler->descriptors, createSouceCodeDescriptor(pos, line, column));
}


static CompiledMethod *createMethod(Compiler *compiler, MethodNode *node, Class *class)
{
	size_t size = compiler->buffer.p - compiler->buffer.buffer;
	CompiledMethod *method = (CompiledMethod *) newObject(Handles.CompiledMethod, size);
	compiledMethodSetHeader(method, compiler->header);
	compiledMethodSetSelector(method, asSymbol(methodNodeGetSelector(node)));
	compiledMethodSetLiterals(method, ordCollAsArray(compiler->literals));
	compiledMethodSetOwnerClass(method, class);
	compiledMethodSetSourceCode(method, methodNodeGetSourceCode(node));
	compiledMethodSetDescriptors(method, ordCollAsArray(compiler->descriptors));
	setupMethodToBlocks(compiler->literals, method);
	memcpy(method->raw->bytes, compiler->buffer.buffer, size);
	return method;
}


static void setupMethodToBlocks(OrderedCollection *literals, CompiledMethod *method)
{
	HandleScope scope;
	openHandleScope(&scope);

	Iterator iterator;
	initOrdCollIterator(&iterator, literals, 0, 0);
	while (iteratorHasNext(&iterator)) {
		Value value = iteratorNext(&iterator);
		if (valueTypeOf(value, VALUE_POINTER) && asObject(value)->class == Handles.CompiledBlock->raw) {
			RawCompiledBlock *block = (RawCompiledBlock *) asObject(value);
			block->method = getTaggedPtr(method);
			if (block->header.outerReturns) {
				method->raw->header.hasContext = 1;
			}
		}
	}

	closeHandleScope(&scope, NULL);
}


static CompiledBlock *createBlock(Compiler *compiler, BlockNode *node)
{
	size_t size = compiler->buffer.p - compiler->buffer.buffer;
	CompiledBlock *block = (CompiledBlock *) newObject(Handles.CompiledBlock, size);
	compiledBlockSetHeader(block, compiler->header);
	compiledBlockSetSourceCode(block, blockNodeGetSourceCode(node));
	compiledBlockSetDescriptors(block, ordCollAsArray(compiler->descriptors));
	memcpy(block->raw->bytes, compiler->buffer.buffer, size);
	return block;
}


CompileError *createUndefinedVariableError(LiteralNode *node)
{
	CompileError *error = (CompileError *) newObject(Handles.UndefinedVariableError, 0);
	objectStorePtr((Object *) error,  &error->raw->variable, (Object *) node);
	return error;
}


CompileError *createRedefinitionError(LiteralNode *var)
{
	CompileError *error = (CompileError *) newObject(Handles.RedefinitionError, 0);
	objectStorePtr((Object *) error,  &error->raw->variable, (Object *) var);
	return error;
}


void compileErrorSetVariable(CompileError *error, LiteralNode *node)
{
	objectStorePtr((Object *) error,  &error->raw->variable, (Object *) node);
}


LiteralNode *compileErrorGetVariable(CompileError *error)
{
	return scopeHandle(asObject(error->raw->variable));
}


_Bool isCompileError(Object *object)
{
	RawClass *class = object->raw->class;
	return class == Handles.UndefinedVariableError->raw
		|| class == Handles.RedefinitionError->raw
		|| class == Handles.ReadonlyVariableError->raw
		|| class == Handles.InvalidPragmaError->raw;
}


void printCompileError(CompileError *error)
{
	if (error->raw->class == Handles.UndefinedVariableError->raw) {
		LiteralNode *literal = compileErrorGetVariable(error);
		String *name = literalNodeGetStringValue(literal);
		printf("Undefined variable: %.*s in ", (unsigned int) name->raw->size, name->raw->contents);
		printSourceCode(literalNodeGetSourceCode(literal));
		printf("\n");

	} else if (error->raw->class == Handles.RedefinitionError->raw) {
		LiteralNode *literal = compileErrorGetVariable(error);
		String *name = literalNodeGetStringValue(literal);
		printf("Cannot redefine: %.*s in ", (unsigned int) name->raw->size, name->raw->contents);
		printSourceCode(literalNodeGetSourceCode(literal));
		printf("\n");

	} else if (error->raw->class == Handles.ReadonlyVariableError->raw) {
		LiteralNode *literal = compileErrorGetVariable(error);
		String *name = literalNodeGetStringValue(literal);
		printf("Cannot write to readonly variable: %.*s in ", (unsigned int) name->raw->size, name->raw->contents);
		printSourceCode(literalNodeGetSourceCode(literal));
		printf("\n");

	} else if (error->raw->class == Handles.InvalidPragmaError->raw) {
		MessageExpressionNode *expression = (MessageExpressionNode *) compileErrorGetVariable(error);
		String *selector = messageExpressionNodeGetSelector(expression);
		printf("Invalid pragma: '%.*s' in ", (unsigned int) selector->raw->size, selector->raw->contents);
		printSourceCode(messageExpressionNodeGetSourceCode(expression));
		printf("\n");

	} else {
		printf("Unknown compile error\n");
	}
}


static void printSourceCode(SourceCode *source)
{
	String *str = sourceCodeGetSourceOrFileName(source);
	printf(
		"'%.*s' line %li column %li",
		(unsigned int) str->raw->size, str->raw->contents,
		sourceCodeGetLine(source), sourceCodeGetColumn(source)
	);
}
