#include "core/Namespace.h"
#include "core/Smalltalk.h"


// asSymbol FAILs on an already-interned Symbol; the namespace API accepts
// either a String (parser identifiers) or a Symbol (getSymbol, image code).
static String *toSymbol(String *name)
{
	return name->raw->class == Handles.Symbol->raw ? name : asSymbol(name);
}


Namespace *newNamespace(String *name, Dictionary *bindings, Array *imports)
{
	HandleScope scope;
	openHandleScope(&scope);
	Namespace *ns = (Namespace *) newObject(Handles.Namespace, 0);
	namespaceSetName(ns, toSymbol(name));
	namespaceSetBindings(ns, bindings);
	namespaceSetImports(ns, imports);
	return closeHandleScope(&scope, ns);
}


Association *namespaceResolveAssoc(Namespace *ns, String *name)
{
	HandleScope scope;
	openHandleScope(&scope);
	String *symbol = toSymbol(name);

	// The Core namespace's own bindings ARE the core dictionary, so skipping
	// straight to the fallback keeps the common (core-only) path at exactly
	// one probe.
	if (ns != NULL && ns->raw != (RawNamespace *) Handles.CoreNamespace->raw) {
		Association *assoc = symbolDictAssocAt(namespaceGetBindings(ns), symbol);
		if (!isNil(assoc)) {
			return closeHandleScope(&scope, assoc);
		}
		Array *imports = namespaceGetImports(ns);
		size_t importsSize = imports->raw->size;
		for (size_t i = 0; i < importsSize; i++) {
			Namespace *import = (Namespace *) arrayObjectAt(imports, i);
			assoc = symbolDictAssocAt(namespaceGetBindings(import), symbol);
			if (!isNil(assoc)) {
				return closeHandleScope(&scope, assoc);
			}
		}
	}
	return closeHandleScope(&scope, symbolDictAssocAt(Handles.Smalltalk, symbol));
}


Association *namespaceOwnAssocAt(Namespace *ns, String *name)
{
	HandleScope scope;
	openHandleScope(&scope);
	Dictionary *bindings = ns == NULL
		? Handles.Smalltalk
		: namespaceGetBindings(ns);
	return closeHandleScope(&scope, symbolDictAssocAt(bindings, toSymbol(name)));
}


Association *namespaceAtPutObject(Namespace *ns, String *name, Object *value)
{
	HandleScope scope;
	openHandleScope(&scope);
	Dictionary *bindings = ns == NULL
		? Handles.Smalltalk
		: namespaceGetBindings(ns);
	return closeHandleScope(&scope, symbolDictAtPutObject(bindings, toSymbol(name), value));
}
