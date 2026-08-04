#ifndef AST_H
#define AST_H

#include "core/Object.h"
#include "core/Thread.h"
#include "memory/Heap.h"
#include "runtime/String.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "memory/ObjectWalk.h"

// A syntax-tree node, with every slot the parser does not fill set to NIL.
//
// THIS IS THE BOUNDARY WHERE THE SMALLTALK RULE STARTS APPLYING, the same one
// `Object new` crosses in runtime/primitives/Allocation.c. Inside the VM an
// unset slot holds the allocator's ZERO and means ABSENT; in Smalltalk there is
// no zero, only nil. An AST node used to be C-side only, so zero was fine.
//
// It stopped being fine the moment the node classes were NAMED and packages/Core
// reopened them (tools/Bootstrap.c), because the image now reads these slots.
// It cost exactly one bug, and a wide one: ClassNode>>isNamespace answers
// `members notNil`, the parser leaves `members` unset for an ordinary class, and
// zero is not nil -- so EVERY class node answered "I am a namespace", and
// PackageLoader skips `classes add:` for a namespace. The result was that no
// class-side `initialize` of any package ever ran, which is a wrong answer with
// no error anywhere.
//
// The C accessors in this file already answer NULL for an unset slot, so they
// are unaffected either way; this is for the reader that is not C.
//
// nil is IMMORTAL, so storing it needs no write barrier.
// `void *`, exactly like newObject, so a caller keeps assigning to its own node
// type without a cast at each of the thirteen call sites.
static inline void *newAstNode(Class *class)
{
	Object *node = newObject(class, 0);
	size_t count;
	Value *slots = objectPointerSlots(&CurrentThread.heap->classes, node->raw,
		&count);
	for (size_t i = 0; i < count; i++) {
		slots[i] = tagPtr(Handles.nil.raw);
	}
	return node;
}


// An AST slot that can be ABSENT, read from C: nil (what newAstNode filled and
// nobody overwrote) answers NULL, so the caller asks with one compare. Every
// getter below whose slot has a meaning for "unset" goes through this; the
// ones that do not are the slots the parser always fills.
//
// The nil case is NOT theoretical: a MethodNode built by hand (main.c runBody,
// core/Entry.c evalBlockNode) sets only the slots it has values for. Testing
// the tag alone let nil through here -- nil IS a heap pointer -- and the first
// consumer that treated the nil object as a collection read the words after it
// in the heap, which held whatever the image layout put there. That decoded as
// a harmless integer for as long as it happened to, and aborted the whole of
// `st -f` the day a new class in the bootstrap moved the neighbor.
static inline void *astSlotOrNull(Value slot)
{
	return valueTypeOf(slot, VALUE_POINTER) && !isTaggedNil(slot)
		? scopeHandle(asObject(slot)) : NULL;
}

typedef struct
{
	OBJECT_HEADER;
	Value name;
	Value superName;
	Value pragmas;
	Value vars;
	Value methods;
	Value sourceCode;
	Value isExtension; // true for `Name extend [ ... ]` (no super, no vars)
	Value members;     // `Name := Namespace [ ... ]`: OrderedCollection of the
	                   // enclosed ClassNodes; nil for ordinary class nodes
} RawClassNode;
OBJECT_HANDLE(ClassNode);

typedef struct
{
	OBJECT_HEADER;
	Value className;
	Value selector;
	Value pragmas;
	Value body;
	Value sourceCode;
} RawMethodNode;
OBJECT_HANDLE(MethodNode);

typedef struct
{
	OBJECT_HEADER;
	Value args;
	Value tempVars;
	Value expressions;
	Value scope;
	Value sourceCode;
} RawBlockNode;
OBJECT_HANDLE(BlockNode);

typedef struct
{
	OBJECT_HEADER;
	Value returns;
	Value assigments;
	Value receiver;
	Value messageExpressions;
	Value sourceCode;
} RawExpressionNode;
OBJECT_HANDLE(ExpressionNode);

typedef struct
{
	OBJECT_HEADER;
	Value selector;
	Value args;
	Value sourceCode;
} RawMessageExpressionNode;
OBJECT_HANDLE(MessageExpressionNode);

typedef struct
{
	OBJECT_HEADER;
	Value value;
	Value sourceCode;
} RawLiteralNode;
OBJECT_HANDLE(LiteralNode);

typedef struct {
	OBJECT_HEADER;
	Value sourceOrFileName;
	Value position;
	Value sourceSize;
	Value line;
	Value column;
} RawSourceCode;
OBJECT_HANDLE(SourceCode);

union BlockScope;

static inline void classNodeSetName(ClassNode *class, LiteralNode *node);
static inline LiteralNode *classNodeGetName(ClassNode *class);
static inline void classNodeSetSuperName(ClassNode *class, LiteralNode *superName);
static inline LiteralNode *classNodeGetSuperName(ClassNode *class);
static inline void classNodeSetPragmas(ClassNode *class, OrderedCollection *pragmas);
static inline OrderedCollection *classNodeGetPragmas(ClassNode *class);
static inline void classNodeSetVars(ClassNode *class, OrderedCollection *vars);
static inline OrderedCollection *classNodeGetVars(ClassNode *class);
static inline void classNodeSetMethods(ClassNode *class, OrderedCollection *methods);
static inline OrderedCollection *classNodeGetMethods(ClassNode *class);
static inline void classNodeSetSourceCode(ClassNode *class, SourceCode *sourceCode);
static inline SourceCode *classNodeGetSourceCode(ClassNode *class);
static inline void classNodeSetIsExtension(ClassNode *class, _Bool isExtension);
static inline _Bool classNodeIsExtension(ClassNode *class);
static inline void classNodeSetMembers(ClassNode *class, OrderedCollection *members);
static inline OrderedCollection *classNodeGetMembers(ClassNode *class);
static inline _Bool classNodeIsNamespace(ClassNode *class);

static inline void methodNodeSetClassName(MethodNode *method, String *className);
static inline String *methodNodeGetClassName(MethodNode *method);
static inline void methodNodeSetSelector(MethodNode *method, String *selector);
static inline String *methodNodeGetSelector(MethodNode *method);
static inline void methodNodeSetPragmas(MethodNode *method, OrderedCollection *pragmas);
static inline OrderedCollection *methodNodeGetPragmas(MethodNode *method);
static inline void methodNodeSetBody(MethodNode *method, BlockNode *body);
static inline BlockNode *methodNodeGetBody(MethodNode *method);
static inline void methodNodeSetSourceCode(MethodNode *method, SourceCode *sourceCode);
static inline SourceCode *methodNodeGetSourceCode(MethodNode *method);

static inline void blockNodeSetArgs(BlockNode *block, OrderedCollection *args);
static inline OrderedCollection *blockNodeGetArgs(BlockNode *block);
static inline void blockNodeSetTempVars(BlockNode *block, OrderedCollection *vars);
static inline OrderedCollection *blockNodeGetTempVars(BlockNode *block);
static inline void blockNodeSetExpressions(BlockNode *block, OrderedCollection *expressions);
static inline OrderedCollection *blockNodeGetExpressions(BlockNode *block);
static inline void blockNodeSetScope(BlockNode *block, union BlockScope *scope);
static inline union BlockScope *blockNodeGetScope(BlockNode *block);
static inline void blockNodeSetSourceCode(BlockNode *block, SourceCode *sourceCode);
static inline SourceCode *blockNodeGetSourceCode(BlockNode *block);

static inline void expressionNodeEnableReturn(ExpressionNode *expression);
static inline void expressionNodeDisableReturn(ExpressionNode *expression);
static inline _Bool expressionNodeReturns(ExpressionNode *expression);
static inline void expressionNodeSetAssigments(ExpressionNode *expression, OrderedCollection *assigments);
static inline OrderedCollection *expressionNodeGetAssigments(ExpressionNode *expression);
static inline void expressionNodeSetReceiver(ExpressionNode *expression, LiteralNode *receiver);
static inline LiteralNode *expressionNodeGetReceiver(ExpressionNode *expression);
static inline void expressionNodeSetMessageExpressions(ExpressionNode *expression, OrderedCollection *messageExpressions);
static inline OrderedCollection *expressionNodeGetMessageExpressions(ExpressionNode *expression);
static inline void expressionNodeSetSourceCode(ExpressionNode *expression, SourceCode *sourceCode);
static inline SourceCode *expressionNodeGetSourceCode(ExpressionNode *expression);

static inline void messageExpressionNodeSetSelector(MessageExpressionNode *messageExpression, String *selector);
static inline String *messageExpressionNodeGetSelector(MessageExpressionNode *messageExpression);
static inline void messageExpressionNodeSetArgs(MessageExpressionNode *messageExpression, OrderedCollection *args);
static inline OrderedCollection *messageExpressionNodeGetArgs(MessageExpressionNode *messageExpression);
static inline void messageExpressionNodeSetSourceCode(MessageExpressionNode *messageExpression, SourceCode *sourceCode);
static inline SourceCode *messageExpressionNodeGetSourceCode(MessageExpressionNode *messageExpression);

static inline void literalNodeSetValue(LiteralNode *literal, Object *value);
static inline void literalNodeSetIntValue(LiteralNode *literal, intptr_t value);
static inline void literalNodeSetCharValue(LiteralNode *literal, char value);
static inline Value literalNodeGetValue(LiteralNode *literal);
static inline intptr_t literalNodeGetIntValue(LiteralNode *literal);
static inline String *literalNodeGetStringValue(LiteralNode *literal);
static inline OrderedCollection *literalNodeGetOrdCollValue(LiteralNode *literal);
static inline void literalNodeSetSourceCode(LiteralNode *literal, SourceCode *sourceCode);
static inline SourceCode *literalNodeGetSourceCode(LiteralNode *literal);

static inline void sourceCodeSetSourceOrFileName(SourceCode *sourceCode, String *sourceOrFileName);
static inline String *sourceCodeGetSourceOrFileName(SourceCode *sourceCode);
static inline void sourceCodeSetPosition(SourceCode *sourceCode, uintptr_t position);
static inline uintptr_t sourceCodeGetPosition(SourceCode *sourceCode);
static inline void sourceCodeSetSourceSize(SourceCode *sourceCode, uintptr_t sourceSize);
static inline uintptr_t sourceCodeGetSourceSize(SourceCode *sourceCode);
static inline void sourceCodeSetLine(SourceCode *sourceCode, uintptr_t line);
static inline uintptr_t sourceCodeGetLine(SourceCode *sourceCode);
static inline void sourceCodeSetColumn(SourceCode *sourceCode, uintptr_t column);
static inline uintptr_t sourceCodeGetColumn(SourceCode *sourceCode);


static inline void classNodeSetName(ClassNode *class, LiteralNode *name)
{
	objectStorePtr((Object *) class,  &class->raw->name, (Object *) name);
}


static inline LiteralNode *classNodeGetName(ClassNode *class)
{
	return (LiteralNode *) scopeHandle(asObject(class->raw->name));
}


static inline void classNodeSetIsExtension(ClassNode *class, _Bool isExtension)
{
	objectStorePtr((Object *) class,  &class->raw->isExtension, asBool(isExtension));
}


static inline _Bool classNodeIsExtension(ClassNode *class)
{
	return asObject(class->raw->isExtension) == Handles.true_.raw;
}


static inline void classNodeSetMembers(ClassNode *class, OrderedCollection *members)
{
	objectStorePtr((Object *) class,  &class->raw->members, (Object *) members);
}


static inline OrderedCollection *classNodeGetMembers(ClassNode *class)
{
	// Unset for every class that is not a namespace declaration.
	return valueTypeOf(class->raw->members, VALUE_POINTER)
		? (OrderedCollection *) scopeHandle(asObject(class->raw->members)) : NULL;
}


// A field NOBODY SET reads as 0, which is tagInt(0) and therefore not a pointer.
// That is what ABSENT looks like in this object model (memory/Heap.c: the
// allocator writes zero, deliberately, and nil is an object rather than the
// absence of one). Every accessor here that can be asked about a field the
// parser leaves alone says so by answering NULL instead of dereferencing zero.
//
// It mattered the first time the class builder ran: `members` is set only for a
// namespace, the old test read "is it not nil", and EVERY ordinary class in
// packages/Core came back claiming to be one.
static inline _Bool classNodeIsNamespace(ClassNode *class)
{
	return valueTypeOf(class->raw->members, VALUE_POINTER)
		&& !isTaggedNil(class->raw->members);
}


static inline void classNodeSetSuperName(ClassNode *class, LiteralNode *superName)
{
	objectStorePtr((Object *) class,  &class->raw->superName, (Object *) superName);
}


static inline LiteralNode *classNodeGetSuperName(ClassNode *class)
{
	// Unset for `Name extend [ ... ]`, which names no superclass at all.
	return valueTypeOf(class->raw->superName, VALUE_POINTER)
		? (LiteralNode *) scopeHandle(asObject(class->raw->superName)) : NULL;
}


static inline void classNodeSetPragmas(ClassNode *class, OrderedCollection *pragmas)
{
	objectStorePtr((Object *) class,  &class->raw->pragmas, (Object *) pragmas);
}


static inline OrderedCollection *classNodeGetPragmas(ClassNode *class)
{
	return (OrderedCollection *) scopeHandle(asObject(class->raw->pragmas));
}


static inline void classNodeSetVars(ClassNode *class, OrderedCollection *vars)
{
	objectStorePtr((Object *) class,  &class->raw->vars, (Object *) vars);
}


static inline OrderedCollection *classNodeGetVars(ClassNode *class)
{
	return (OrderedCollection *) scopeHandle(asObject(class->raw->vars));
}


static inline void classNodeSetMethods(ClassNode *class, OrderedCollection *methods)
{
	objectStorePtr((Object *) class,  &class->raw->methods, (Object *) methods);
}


static inline OrderedCollection *classNodeGetMethods(ClassNode *class)
{
	return (OrderedCollection *) scopeHandle(asObject(class->raw->methods));
}


static inline void classNodeSetSourceCode(ClassNode *class, SourceCode *sourceCode)
{
	objectStorePtr((Object *) class,  &class->raw->sourceCode, (Object *) sourceCode);
}


static inline SourceCode *classNodeGetSourceCode(ClassNode *class)
{
	return (SourceCode *) scopeHandle(asObject(class->raw->sourceCode));
}


static inline void methodNodeSetClassName(MethodNode *method, String *className)
{
	objectStorePtr((Object *) method,  &method->raw->className, (Object *) className);
}


static inline String *methodNodeGetClassName(MethodNode *method)
{
	// Set only for a CLASS-SIDE method, written `class foo [ ... ]`, so an
	// instance-side method leaves it unset and this is how that is asked.
	return (String *) astSlotOrNull(method->raw->className);
}


static inline void methodNodeSetSelector(MethodNode *method, String *selector)
{
	objectStorePtr((Object *) method,  &method->raw->selector, (Object *) selector);
}


static inline String *methodNodeGetSelector(MethodNode *method)
{
	return (String *) scopeHandle(asObject(method->raw->selector));
}


static inline void methodNodeSetPragmas(MethodNode *method, OrderedCollection *pragmas)
{
	objectStorePtr((Object *) method,  &method->raw->pragmas, (Object *) pragmas);
}


static inline OrderedCollection *methodNodeGetPragmas(MethodNode *method)
{
	// A node the PARSER built always has one, even empty; a node built by hand
	// (`st -f` wrapping a top-level block) leaves it nil.
	return (OrderedCollection *) astSlotOrNull(method->raw->pragmas);
}


static inline void methodNodeSetBody(MethodNode *method, BlockNode *body)
{
	objectStorePtr((Object *) method,  &method->raw->body, (Object *) body);
}


static inline BlockNode *methodNodeGetBody(MethodNode *method)
{
	return (BlockNode *) scopeHandle(asObject(method->raw->body));
}


static inline void methodNodeSetSourceCode(MethodNode *method, SourceCode *sourceCode)
{
	objectStorePtr((Object *) method,  &method->raw->sourceCode, (Object *) sourceCode);
}


static inline SourceCode *methodNodeGetSourceCode(MethodNode *method)
{
	// nil for a node built by hand (main.c runBody): the unit then keeps the
	// source compileUnit read off the body block, which for a wrapped
	// top-level block is the same text.
	return (SourceCode *) astSlotOrNull(method->raw->sourceCode);
}


static inline void blockNodeSetArgs(BlockNode *block, OrderedCollection *args)
{
	objectStorePtr((Object *) block,  &block->raw->args, (Object *) args);
}


static inline OrderedCollection *blockNodeGetArgs(BlockNode *block)
{
	return (OrderedCollection *) scopeHandle(asObject(block->raw->args));
}


static inline void blockNodeSetTempVars(BlockNode *block, OrderedCollection *vars)
{
	objectStorePtr((Object *) block,  &block->raw->tempVars, (Object *) vars);
}


static inline OrderedCollection *blockNodeGetTempVars(BlockNode *block)
{
	return (OrderedCollection *) scopeHandle(asObject(block->raw->tempVars));
}


static inline void blockNodeSetExpressions(BlockNode *block, OrderedCollection *expressions)
{
	objectStorePtr((Object *) block,  &block->raw->expressions, (Object *) expressions);
}


static inline OrderedCollection *blockNodeGetExpressions(BlockNode *block)
{
	return (OrderedCollection *) scopeHandle(asObject(block->raw->expressions));
}


static inline void blockNodeSetScope(BlockNode *block, union BlockScope *scope)
{
	objectStorePtr((Object *) block,  &block->raw->scope, (Object *) scope);
}


static inline union BlockScope *blockNodeGetScope(BlockNode *block)
{
	// Never set in v2: the front end keeps its capture analysis in its own
	// tables rather than on the tree (vm/compiler/Compile.c).
	return (union BlockScope *) astSlotOrNull(block->raw->scope);
}


static inline void blockNodeSetSourceCode(BlockNode *block, SourceCode *sourceCode)
{
	objectStorePtr((Object *) block,  &block->raw->sourceCode, (Object *) sourceCode);
}


static inline SourceCode *blockNodeGetSourceCode(BlockNode *block)
{
	// nil for the BODY block of a parsed method: parseMethod records the
	// source (pattern included) on the MethodNode, not here, and
	// compileMethod reads it from there over anything taken from the body.
	return (SourceCode *) astSlotOrNull(block->raw->sourceCode);
}


static inline void expressionNodeEnableReturn(ExpressionNode *expression)
{
	objectStorePtr((Object *) expression,  &expression->raw->returns, (Object *) &Handles.true_);
}


static inline void expressionNodeDisableReturn(ExpressionNode *expression)
{
	objectStorePtr((Object *) expression,  &expression->raw->returns, (Object *) &Handles.false_);
}


static inline _Bool expressionNodeReturns(ExpressionNode *expression)
{
	return isTaggedTrue(expression->raw->returns);
}


static inline void expressionNodeSetAssigments(ExpressionNode *expression, OrderedCollection *assigments)
{
	objectStorePtr((Object *) expression,  &expression->raw->assigments, (Object *) assigments);
}


static inline OrderedCollection *expressionNodeGetAssigments(ExpressionNode *expression)
{
	return (OrderedCollection *) scopeHandle(asObject(expression->raw->assigments));
}


static inline void expressionNodeSetReceiver(ExpressionNode *expression, LiteralNode *receiver)
{
	objectStorePtr((Object *) expression,  &expression->raw->receiver, (Object *) receiver);
}


static inline LiteralNode *expressionNodeGetReceiver(ExpressionNode *expression)
{
	return (LiteralNode *) scopeHandle(asObject(expression->raw->receiver));
}


static inline void expressionNodeSetMessageExpressions(ExpressionNode *expression, OrderedCollection *messageExpressions)
{
	objectStorePtr((Object *) expression,  &expression->raw->messageExpressions, (Object *) messageExpressions);
}


static inline OrderedCollection *expressionNodeGetMessageExpressions(ExpressionNode *expression)
{
	return (OrderedCollection *) scopeHandle(asObject(expression->raw->messageExpressions));
}


static inline void expressionNodeSetSourceCode(ExpressionNode *expression, SourceCode *sourceCode)
{
	objectStorePtr((Object *) expression,  &expression->raw->sourceCode, (Object *) sourceCode);
}


static inline SourceCode *expressionNodeGetSourceCode(ExpressionNode *expression)
{
	return (SourceCode *) scopeHandle(asObject(expression->raw->sourceCode));
}


static inline void messageExpressionNodeSetSelector(MessageExpressionNode *messageExpression, String *selector)
{
	objectStorePtr((Object *) messageExpression,  &messageExpression->raw->selector, (Object *) selector);
}


static inline String *messageExpressionNodeGetSelector(MessageExpressionNode *messageExpression)
{
	return (String *) scopeHandle(asObject(messageExpression->raw->selector));
}


static inline void messageExpressionNodeSetArgs(MessageExpressionNode *messageExpression, OrderedCollection *args)
{
	objectStorePtr((Object *) messageExpression,  &messageExpression->raw->args, (Object *) args);
}


static inline OrderedCollection *messageExpressionNodeGetArgs(MessageExpressionNode *messageExpression)
{
	return (OrderedCollection *) scopeHandle(asObject(messageExpression->raw->args));
}


static inline void messageExpressionNodeSetSourceCode(MessageExpressionNode *messageExpression, SourceCode *sourceCode)
{
	objectStorePtr((Object *) messageExpression,  &messageExpression->raw->sourceCode, (Object *) sourceCode);
}


static inline SourceCode *messageExpressionNodeGetSourceCode(MessageExpressionNode *messageExpression)
{
	return (SourceCode *) scopeHandle(asObject(messageExpression->raw->sourceCode));
}


static inline void literalNodeSetValue(LiteralNode *literal, Object *value)
{
	objectStorePtr((Object *) literal,  &literal->raw->value, (Object *) value);
}


static inline void literalNodeSetIntValue(LiteralNode *literal, intptr_t value)
{
	literal->raw->value = tagInt(value);
}


static inline void literalNodeSetCharValue(LiteralNode *literal, char value)
{
	literal->raw->value = tagChar(value);
}


static inline void literalNodeSetRawValue(LiteralNode *literal, Value value)
{
	if (valueTypeOf(value, VALUE_POINTER)) {
		rawObjectStorePtr((RawObject *) literal->raw, &literal->raw->value, asObject(value));
	} else {
		literal->raw->value = value;
	}
}


static inline Value literalNodeGetValue(LiteralNode *literal)
{
	return literal->raw->value;
}


static inline intptr_t literalNodeGetIntValue(LiteralNode *literal)
{
	return asCInt(literalNodeGetValue(literal));
}


static inline String *literalNodeGetStringValue(LiteralNode *literal)
{
	return (String *) scopeHandle(asObject(literalNodeGetValue(literal)));
}


static inline OrderedCollection *literalNodeGetOrdCollValue(LiteralNode *literal)
{
	return scopeHandle(asObject(literalNodeGetValue(literal)));
}


static inline void literalNodeSetSourceCode(LiteralNode *literal, SourceCode *sourceCode)
{
	objectStorePtr((Object *) literal,  &literal->raw->sourceCode, (Object *) sourceCode);
}


static inline SourceCode *literalNodeGetSourceCode(LiteralNode *literal)
{
	return (SourceCode *) scopeHandle(asObject(literal->raw->sourceCode));
}


static inline void sourceCodeSetSourceOrFileName(SourceCode *sourceCode, String *sourceOrFileName)
{
	objectStorePtr((Object *) sourceCode,  &sourceCode->raw->sourceOrFileName, (Object *) sourceOrFileName);
}


static inline String *sourceCodeGetSourceOrFileName(SourceCode *sourceCode)
{
	return (String *) scopeHandle(asObject(sourceCode->raw->sourceOrFileName));
}


static inline void sourceCodeSetPosition(SourceCode *sourceCode, uintptr_t position)
{
	sourceCode->raw->position = tagInt(position);
}


static inline uintptr_t sourceCodeGetPosition(SourceCode *sourceCode)
{
	return asCInt(sourceCode->raw->position);
}


static inline void sourceCodeSetSourceSize(SourceCode *sourceCode, uintptr_t sourceSize)
{
	sourceCode->raw->sourceSize = tagInt(sourceSize);
}


static inline uintptr_t sourceCodeGetSourceSize(SourceCode *sourceCode)
{
	return asCInt(sourceCode->raw->sourceSize);
}


static inline void sourceCodeSetLine(SourceCode *sourceCode, uintptr_t line)
{
	sourceCode->raw->line = tagInt(line);
}


static inline uintptr_t sourceCodeGetLine(SourceCode *sourceCode)
{
	return asCInt(sourceCode->raw->line);
}


static inline void sourceCodeSetColumn(SourceCode *sourceCode, uintptr_t column)
{
	sourceCode->raw->column = tagInt(column);
}


static inline uintptr_t sourceCodeGetColumn(SourceCode *sourceCode)
{
	return asCInt(sourceCode->raw->column);
}


#endif
