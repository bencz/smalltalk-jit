#ifndef CLASS_BUILDER_H
#define CLASS_BUILDER_H

// A parsed class definition becomes a REAL CLASS: shape, superclass, instance
// variables, and every method compiled into it.
//
// This is the piece that turns packages/ from text into a running system, and
// the only piece between the front end (which compiles ONE method against a
// class that already exists) and an image.
//
// Everything it cannot do is a NAMED failure rather than a guess, because the
// point of running it over packages/Core is to find out precisely what is
// missing, and a builder that silently approximated a shape would answer that
// question wrong.

#include "compiler/Ast.h"
#include "compiler/Compile.h"
#include "core/Class.h"
#include "core/Namespace.h"
#include "jit/CompiledMethod.h"

typedef struct {
	// What went wrong, in words, or NULL. Not an enum: these are diagnostics for
	// a human reading a load report, and the set of them is still growing.
	const char *message;
	String *what;          // the name involved, when there is one
	String *inMethod;      // the method being compiled, when it was one
	CompileStatus status;  // when the failure came from compiling a method
	// Which kernel exception class the reflective compiler should raise, or NULL
	// for plain Error. It is a NAME and not a Handle because the classes it can
	// name live in packages/Core and this header is compiled long before any of
	// them exists; the reflective primitive looks it up when it needs it.
	//
	// It exists because `on: RedefinitionError do: [...]` in the image cannot
	// work if every build failure arrives as Error. A caller that wants to
	// distinguish "you redefined something" from "the shape pragma is wrong"
	// needs the class, and a message string is not one.
	const char *errorClass;
	// The AST node the failure is about, or NULL: it becomes the `identifier`
	// of the typed error, whose defaultMessageText prints its name and source
	// position. A handle in the OUTERMOST scope (outermostHandle), because the
	// error is read back after every build scope has closed.
	LiteralNode *identifier;
} ClassBuildError;

// A zeroed one. Every field means "not set", so one initializer, and adding a
// field does not make four call sites list one fewer than the struct has.
#define CLASS_BUILD_ERROR_NONE \
	((ClassBuildError) { NULL, NULL, NULL, COMPILE_OK, NULL, NULL })

// Build or REOPEN the class this node describes. Reopening matters: the
// built-in kernel already defines Object, Array, SmallInteger and the rest, and
// packages/Core defines them again with their real contents. A second class
// object would leave every immediate's class index pointing at the first.
Class *classBuildIn(ClassNode *node, Namespace *namespace, ClassBuildError *error);

// Core only, which is what the bootstrap and `st -b` want. NULL is the
// core-only namespace everywhere (core/Namespace.h).
static inline Class *classBuild(ClassNode *node, ClassBuildError *error)
{
	return classBuildIn(node, NULL, error);
}

// The metaclass of a class, created on first use. A class-side method belongs in
// the method dictionary of the class's OWN class, which is what makes
// `Foo bar` find something different from `Foo new bar`.
Class *classMetaclassOf(Class *class);

// Compile ONE parsed method into `target` and install it in that class's method
// dictionary. Answers NULL with `error` filled in.
//
// `classVariableScope` is where class variables are looked up, and it differs
// from `target` for a class-side method: the method is compiled with the
// METACLASS as its owner, but a metaclass holds none of the class's class
// variables, so both sides have to be pointed at the class itself or they reach
// different Associations for the same name. NULL means "same as target".
//
// EXPORTED so the reflective compiler primitive (runtime/primitives/Reflect.c)
// and the class builder share one path. Two copies of this would be two answers
// to "where does a class variable live", which is a question this project has
// already got wrong once, silently.
CompiledMethod *classCompileMethodInto(MethodNode *node, Class *target,
	Class *classVariableScope, Namespace *namespace, ClassBuildError *error);

#endif
