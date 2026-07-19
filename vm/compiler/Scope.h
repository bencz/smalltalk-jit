#ifndef SCOPE_H
#define SCOPE_H

#include "core/Object.h"
#include "compiler/Parser.h"
#include "core/CompiledCode.h"
#include "compiler/Compiler.h"

typedef struct RawBlockScope {
	OBJECT_HEADER;
	CompiledCodeHeader header;
	Value parent;
	Value vars;
	Value ownerClass;
	Value literals;
	Value error;
	Value namespace; // resolution context for globals; nil means Core
} RawBlockScope;
OBJECT_HANDLE(BlockScope);

BlockScope *analyzeMethod(MethodNode *node, Class *class);
// Analyze resolving globals in `ns` (own bindings -> imports -> core).
// ns == NULL falls back to the class's home namespace, then DefaultNamespace.
BlockScope *analyzeMethodIn(MethodNode *node, Class *class, Namespace *ns);


static void blockScopeSetHeader(BlockScope *blockScope, CompiledCodeHeader header)
{
	blockScope->raw->header = header;
}


static CompiledCodeHeader blockScopeGetHeader(BlockScope *blockScope)
{
	return blockScope->raw->header;
}


static void blockScopeSetParent(BlockScope *blockScope, BlockScope *parent)
{
	objectStorePtr((Object *) blockScope,  &blockScope->raw->parent, (Object *) parent);
}


static BlockScope *blockScopeGetParent(BlockScope *blockScope)
{
	return scopeHandle(asObject(blockScope->raw->parent));
}


static void blockScopeSetVars(BlockScope *blockScope, Dictionary *vars)
{
	objectStorePtr((Object *) blockScope,  &blockScope->raw->vars, (Object *) vars);
}


static Dictionary *blockScopeGetVars(BlockScope *blockScope)
{
	return scopeHandle(asObject(blockScope->raw->vars));
}


static void blockScopeSetOwnerClass(BlockScope *blockScope, Class *class)
{
	objectStorePtr((Object *) blockScope,  &blockScope->raw->ownerClass, (Object *) class);
}


static Class *blockScopeGetOwnerClass(BlockScope *blockScope)
{
	return scopeHandle(asObject(blockScope->raw->ownerClass));
}


static void blockScopeSetLiterals(BlockScope *blockScope, OrderedCollection *literals)
{
	objectStorePtr((Object *) blockScope,  &blockScope->raw->literals, (Object *) literals);
}


static OrderedCollection *blockScopeGetLiterals(BlockScope *blockScope)
{
	return scopeHandle(asObject(blockScope->raw->literals));
}


static void blockScopeSetError(BlockScope *blockScope, CompileError *error)
{
	objectStorePtr((Object *) blockScope,  &blockScope->raw->error, (Object *) error);
}


static CompileError *blockScopeGetError(BlockScope *blockScope)
{
	return scopeHandle(asObject(blockScope->raw->error));
}


static _Bool blockScopeHasError(BlockScope *blockScope)
{
	return !isTaggedNil(blockScope->raw->error);
}


static void blockScopeSetNamespace(BlockScope *blockScope, Namespace *ns)
{
	objectStorePtr((Object *) blockScope,  &blockScope->raw->namespace, (Object *) ns);
}


static Namespace *blockScopeGetNamespace(BlockScope *blockScope)
{
	return (Namespace *) scopeHandle(asObject(blockScope->raw->namespace));
}

#endif
