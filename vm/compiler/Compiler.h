#ifndef COMPILER_H
#define COMPILER_H

#include "core/Object.h"
#include "core/CompiledCode.h"
#include "compiler/Parser.h"

enum {
	CONTEXT_INDEX = 0,
	SELF_INDEX = 1,
	SUPER_INDEX	= 1,
};

typedef struct {
	OBJECT_HEADER;
	Value messageText;
	Value variable;
} RawCompileError;
OBJECT_HANDLE(CompileError);

Object *compileMethod(MethodNode *node, Class *class);
// Compile resolving globals in `ns`; NULL defaults to the class's home
// namespace, then DefaultNamespace (see analyzeMethodIn).
Object *compileMethodIn(MethodNode *node, Class *class, Namespace *ns);
CompileError *createUndefinedVariableError(LiteralNode *node);
CompileError *createRedefinitionError(LiteralNode *var);
void compileErrorSetVariable(CompileError *error, LiteralNode *node);
LiteralNode *compileErrorGetVariable(CompileError *error);
_Bool isCompileError(Object *object);
void printCompileError(CompileError *error);

#endif
