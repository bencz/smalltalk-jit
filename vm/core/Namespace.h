#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "core/Object.h"
#include "core/Thread.h"
#include "core/Handle.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"

// Namespaces (RawNamespace in core/Object.h). One per loaded package, plus
// the Core namespace whose `bindings` IS Handles.Smalltalk (same Dictionary
// identity), so every pre-namespace reflective path (`Smalltalk at:`, the C
// setGlobal/getGlobal family) keeps meaning "the core namespace". The
// registry Dictionary name -> Namespace lives in Handles.Namespaces and is
// the image-side `Namespaces` global.

Namespace *newNamespace(String *name, Dictionary *bindings, Array *imports);

// The compile-time resolution chain: own bindings, then imports in
// declaration order (first import wins), then the core globals as the
// implicit final fallback (core is NEVER stored in imports). Answers the
// binding Association, or a nil handle when the whole chain missed.
// ns == NULL means "core only": every pre-namespace call site passes that.
Association *namespaceResolveAssoc(Namespace *ns, String *name);

// Own bindings only, no chain: what installClass uses so that the same class
// name in two namespaces never collides. Nil handle when absent.
Association *namespaceOwnAssocAt(Namespace *ns, String *name);

// Insert or update a binding in the namespace's OWN dictionary.
Association *namespaceAtPutObject(Namespace *ns, String *name, Object *value);


// The session's default compile namespace (Core until a project image changes
// it): what eval, parseFile and class builds without an explicit namespace
// resolve against. Stored behind the Handles.DefaultNamespace indirection cell.
static Namespace *defaultNamespace(void)
{
	return (Namespace *) scopeHandle(asObject(Handles.DefaultNamespace->raw->value));
}


static void namespaceSetName(Namespace *ns, String *name)
{
	objectStorePtr((Object *) ns, &ns->raw->name, (Object *) name);
}


static String *namespaceGetName(Namespace *ns)
{
	return (String *) scopeHandle(asObject(ns->raw->name));
}


static void namespaceSetBindings(Namespace *ns, Dictionary *bindings)
{
	objectStorePtr((Object *) ns, &ns->raw->bindings, (Object *) bindings);
}


static Dictionary *namespaceGetBindings(Namespace *ns)
{
	return (Dictionary *) scopeHandle(asObject(ns->raw->bindings));
}


static void namespaceSetImports(Namespace *ns, Array *imports)
{
	objectStorePtr((Object *) ns, &ns->raw->imports, (Object *) imports);
}


static Array *namespaceGetImports(Namespace *ns)
{
	return (Array *) scopeHandle(asObject(ns->raw->imports));
}

#endif
