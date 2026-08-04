#ifndef NAMESPACE_H
#define NAMESPACE_H

// Namespaces: one per loaded package, plus Core.
//
// A Namespace (RawNamespace in core/Object.h, mirrored by
// packages/Core/src/Namespace.st) is a name, a bindings Dictionary and an
// imports Array. THE CORE NAMESPACE'S BINDINGS *IS* THE GLOBALS DICTIONARY,
// same object identity, which is what keeps every pre-namespace path -- the C
// getGlobal/setGlobal family, `Smalltalk at:`, the bootstrap -- meaning exactly
// "the core namespace" without any of them being changed.
//
// NULL MEANS CORE. Every call site that has no namespace passes NULL and gets
// core-only behaviour, so adding namespaces did not require touching the
// callers that do not have one.

#include "core/Class.h"
#include "core/Handle.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"

// The compile-time resolution chain: own bindings, then imports in DECLARATION
// ORDER (first import wins), then the core globals as the implicit final
// fallback. Core is never stored in imports; it is the fallback by
// construction. Answers the Association, or NULL when the whole chain missed.
Association *namespaceResolveAssoc(Namespace *namespace, String *name);

// Own bindings only, no chain. This is what a class DEFINITION uses, so that
// the same class name in two namespaces never collides: resolving through the
// chain would find the import's class and reopen THAT one.
Association *namespaceOwnAssocAt(Namespace *namespace, String *name);

// Insert or update a binding in the namespace's OWN dictionary.
Association *namespaceAtPutObject(Namespace *namespace, String *name,
	Object *value);

// The bindings dictionary, or the globals when `namespace` is NULL or Core.
Dictionary *namespaceBindings(Namespace *namespace);

#endif
