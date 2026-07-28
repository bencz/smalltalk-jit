#include "core/Namespace.h"
#include "core/Smalltalk.h"

// A name arrives either as a parser identifier (a String) or already interned
// (a Symbol, from getSymbol or from image code). asSymbol on an interned Symbol
// would intern it again, so the class is checked first.
static String *toSymbol(String *name)
{
	if (Handles.Symbol.raw != NULL
			&& rawObjectClassIndex((RawObject *) name->raw)
				== classIndexOf(&Handles.Symbol)) {
		return name;
	}
	return asSymbol(name);
}


Dictionary *namespaceBindings(Namespace *namespace)
{
	if (namespace == NULL) {
		return smalltalkGlobals();
	}
	Value bindings = namespace->raw->bindings;
	if (!valueTypeOf(bindings, VALUE_POINTER)) {
		return smalltalkGlobals(); // a half-built namespace: core is the safe read
	}
	return (Dictionary *) scopeHandle(asObject(bindings));
}


// Is this the Core namespace? Asked by IDENTITY of the bindings dictionary
// rather than by name.
//
// packages/Core/src/Namespace.st states the invariant that makes this work:
// "(Namespaces at: #Core) bindings == Smalltalk". Comparing the name against
// the string "Core" would be a second encoding of the same fact, and one a
// package could spoof by calling itself Core.
static _Bool isCoreNamespace(Namespace *namespace)
{
	if (namespace == NULL) {
		return 1;
	}
	Value bindings = namespace->raw->bindings;
	return valueTypeOf(bindings, VALUE_POINTER)
		&& asObject(bindings) == (RawObject *) smalltalkGlobals()->raw;
}


Association *namespaceResolveAssoc(Namespace *namespace, String *name)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *symbol = toSymbol(name);

	// Core's own bindings ARE the globals dictionary, so going straight to the
	// fallback keeps the common case at exactly one probe.
	if (!isCoreNamespace(namespace)) {
		Association *own = symbolDictAssocAt(namespaceBindings(namespace), symbol);
		if (own != NULL) {
			return closeHandleScope(&scope, own);
		}
		Value importsValue = namespace->raw->imports;
		if (valueTypeOf(importsValue, VALUE_POINTER)) {
			Array *imports = scopeHandle(asObject(importsValue));
			size_t count = rawArraySize(imports->raw);
			for (size_t i = 0; i < count; i++) {
				// FIRST IMPORT WINS, which is why this is a loop in declaration
				// order and not a set. Two packages exporting the same name is
				// ordinary, and the manifest's order is the answer to which one
				// the importer meant.
				Value entry = rawArrayAt(imports->raw, i);
				if (!valueTypeOf(entry, VALUE_POINTER)) {
					continue;
				}
				Namespace *import = scopeHandle(asObject(entry));
				Association *found =
					symbolDictAssocAt(namespaceBindings(import), symbol);
				if (found != NULL) {
					return closeHandleScope(&scope, found);
				}
			}
		}
	}
	return closeHandleScope(&scope,
		symbolDictAssocAt(smalltalkGlobals(), symbol));
}


Association *namespaceOwnAssocAt(Namespace *namespace, String *name)
{
	HandleScope scope;
	openHandleScope(&scope);
	return closeHandleScope(&scope,
		symbolDictAssocAt(namespaceBindings(namespace), toSymbol(name)));
}


Association *namespaceAtPutObject(Namespace *namespace, String *name,
	Object *value)
{
	HandleScope scope;
	openHandleScope(&scope);
	return closeHandleScope(&scope,
		symbolDictAtPutObject(namespaceBindings(namespace), toSymbol(name), value));
}
