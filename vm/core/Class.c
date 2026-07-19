#include "core/Class.h"
#include "memory/Heap.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/Iterator.h"
#include "compiler/Compiler.h"
#include "core/Assert.h"
#include "core/Namespace.h"
#include "core/Entry.h"
#include "jit/InlineCache.h"
#include <string.h>
#include <stdarg.h>

static _Bool resolveSuperClass(ClassNode *node, MetaClass **superMetaClass, Class **superClass, _Bool *isRoot, Namespace *ns);
static Class *subClass(ClassNode *node, MetaClass *superMetaClass, Class *superClass);
static CompileError *processPragmas(Class *class, OrderedCollection *pragmas, Namespace *ns);
static CompileError *processShapePragma(MessageExpressionNode *pragma, Class *class, Namespace *ns);
static CompileError *declareVariables(Class *class, OrderedCollection *vars, Array *superInstVars);
static _Bool isClassVar(String *name);
static CompileError *compileMethods(Class *class, OrderedCollection *methods, Namespace *ns);
static CompileError *compileAndInstallMethod(MethodNode *node, Class *class, Dictionary *methodDict, Namespace *ns);
static CompileError *createMethodRedefinitionError(MethodNode *node);
static Class *installClass(Class *class, Namespace *ns);
static void addSubClass(Class *class, Class *subClass);
static Object *extendClass(ClassNode *node, Namespace *ns);
static Object *declareNamespace(ClassNode *node, Namespace *ns);
static Dictionary *dictionaryMerging(Dictionary *live, Dictionary *adds);
static size_t dictionaryCapacityFor(size_t entries);


Object *buildClass(ClassNode *node)
{
	return buildClassIn(node, NULL);
}


Object *buildClassIn(ClassNode *node, Namespace *ns)
{
	HandleScope handleScope;
	openHandleScope(&handleScope);

	if (ns == NULL) {
		ns = defaultNamespace();
	}

	if (classNodeIsNamespace(node)) {
		return (Object *) closeHandleScope(&handleScope, declareNamespace(node, ns));
	}
	if (classNodeIsExtension(node)) {
		return (Object *) closeHandleScope(&handleScope, extendClass(node, ns));
	}

	CompileError *error;
	MetaClass *superMetaClass;
	Class *superClass;
	_Bool isRoot;
	if (!resolveSuperClass(node, &superMetaClass, &superClass, &isRoot, ns)) {
		error = createUndefinedVariableError(classNodeGetSuperName(node));
		return (Object *) closeHandleScope(&handleScope, error);
	}

	Class *class = subClass(node, superMetaClass, superClass);
	MetaClass *metaClass = classGetMetaClass(class);
	// The home namespace rides the class object from birth, so the
	// redefinition memcpy in installClass carries it to the live class.
	classSetNamespace(class, ns);

	OrderedCollection *pragmas = classNodeGetPragmas(node);
	error = processPragmas(class, pragmas, ns);
	if (error != NULL) {
		return (Object *) closeHandleScope(&handleScope, error);
	}

	OrderedCollection *vars = classNodeGetVars(node);
	Array *superInstVars = classGetInstanceVariables(superClass);
	error = declareVariables(class, vars, superInstVars);
	if (error != NULL) {
		return (Object *) closeHandleScope(&handleScope, error);
	}

	OrderedCollection *methods = classNodeGetMethods(node);
	error = compileMethods(class, methods, ns);
	if (error != NULL) {
		return (Object *) closeHandleScope(&handleScope, error);
	}

	class = installClass(class, ns);
	if (isRoot) {
		classSetSuperClass(class, (Class *) Handles.nil);
	} else {
		addSubClass(superClass, class);
	}
	metaClassSetInstanceClass(metaClass, class);
	return (Object *) closeHandleScope(&handleScope, class);
}


static _Bool resolveSuperClass(ClassNode *node, MetaClass **superMetaClass, Class **superClass, _Bool *isRoot, Namespace *ns)
{
	String *name = literalNodeGetStringValue(classNodeGetSuperName(node));
	if (stringEqualsC(name, "nil")) {
		*superMetaClass = (MetaClass *) Handles.Class;
		*superClass = Handles.UndefinedObject;
		*isRoot = 1;
		return 1;
	}

	Association *assoc = namespaceResolveAssoc(ns, name);
	if (isNil(assoc)) {
		return 0;
	}
	Class *super = (Class *) scopeHandle(asObject(assoc->raw->value));
	if (!isNil(super)) {
		*superMetaClass = classGetMetaClass(super);
		*superClass = super;
		*isRoot = 0;
		return 1;
	}

	return 0;
}


static Class *subClass(ClassNode *node, MetaClass *superMetaClass, Class *superClass)
{
	MetaClass *metaClass = (MetaClass *) newObject(Handles.MetaClass, 0);
	metaClassSetSuperClass(metaClass, superMetaClass);
	metaClassSetSubClasses(metaClass, newOrdColl(16));
	metaClassSetInstanceShape(metaClass, metaClassGetInstanceShape(superMetaClass));
	metaClassSetInstanceVariables(metaClass, metaClassGetInstanceVariables(superMetaClass));

	Class *class = (Class *) newObject((Class *) metaClass, 0);
	classSetSuperClass(class, superClass);
	classSetSubClasses(class, newOrdColl(16));
	classSetName(class, asSymbol(literalNodeGetStringValue(classNodeGetName(node))));
	classSetInstanceShape(class, classGetInstanceShape(superClass));

	metaClassSetInstanceClass(metaClass, class);
	return class;
}


static CompileError *processPragmas(Class *class, OrderedCollection *pragmas, Namespace *ns)
{
	MessageExpressionNode *pragma;
	CompileError *error;
	Iterator iterator;

	initOrdCollIterator(&iterator, pragmas, 0, 0);
	while (iteratorHasNext(&iterator)) {
		MessageExpressionNode *pragma = (MessageExpressionNode *) iteratorNextObject(&iterator);
		String *selector = messageExpressionNodeGetSelector(pragma);
		if (stringEqualsC(selector, "shape:")) {
			error = processShapePragma(pragma, class, ns);
			if (error != NULL) {
				return error;
			}
		}
	}

	return NULL;
}


static CompileError *processShapePragma(MessageExpressionNode *pragma, Class *class, Namespace *ns)
{
	OrderedCollection *args = messageExpressionNodeGetArgs(pragma);
	ASSERT(ordCollSize(args) == 1);
	LiteralNode *arg = (LiteralNode *) ordCollObjectAt(args, 0);
	Value value;
	CompileError *error;

	if (arg->raw->class == Handles.VariableNode->raw) {
		// Shape names are core globals; the chain finds them from any namespace.
		Association *assoc = namespaceResolveAssoc(ns, literalNodeGetStringValue(arg));
		if (isNil(assoc)) {
			goto error;
		}
		value = assoc->raw->value;
	} else if (arg->raw->class == Handles.IntegerNode->raw) {
		value = literalNodeGetValue(arg);
	} else {
		goto error;
	}

	classSetInstanceShape(class, *(InstanceShape *) &value);
	return NULL;

error:
	error = (CompileError *) newObject(Handles.InvalidPragmaError, 0);
	compileErrorSetVariable(error, (LiteralNode *) pragma);
	return error;
}


static CompileError *declareVariables(Class *class, OrderedCollection *vars, Array *superInstVars)
{
	OrderedCollection *tmpInstVars = newOrdColl(8);
	Dictionary *classVars = newDictionary(8);
	Iterator iterator;

	initOrdCollIterator(&iterator, vars, 0, 0);
	while (iteratorHasNext(&iterator)) {
		String *name = literalNodeGetStringValue((LiteralNode *) iteratorNextObject(&iterator));
		if (isClassVar(name)) {
			symbolDictAtPutObject(classVars, asSymbol(name), Handles.nil);
		} else {
			ordCollAddObject(tmpInstVars, (Object *) name);
		}
	}

	Array *instVars;
	size_t size = objectSize((Object *) superInstVars);
	if (size == 0) {
		instVars = ordCollAsArray(tmpInstVars);
	} else {
		instVars = (Array *) copyResizedObject((Object *) superInstVars, size + ordCollSize(tmpInstVars));
		initOrdCollIterator(&iterator, tmpInstVars, 0, 0);
		while (iteratorHasNext(&iterator)) {
			ptrdiff_t index = size + iteratorIndex(&iterator);
			arrayAtPutObject(instVars, index, iteratorNextObject(&iterator));
		}
	}

	InstanceShape shape = classGetInstanceShape(class);
	shape.varsSize = instVars->raw->size;
	shape.size = COMPUTE_INST_SHAPE_SIZE(shape.payloadSize, shape.varsSize, shape.isIndexed);
	classSetInstanceShape(class, shape);

	classSetInstanceVariables(class, instVars);
	classSetClassVariables(class, classVars);
	return NULL;
}


static _Bool isClassVar(String *name)
{
	return name->raw->contents[0] >= 'A' && name->raw->contents[0] <= 'Z';
}


static CompileError *compileMethods(Class *class, OrderedCollection *methods, Namespace *ns)
{
	HandleScope scope;
	openHandleScope(&scope);

	MetaClass *metaClass = classGetMetaClass(class);
	Dictionary *methodDict = newDictionary(64);
	Dictionary *classMethodDict = newDictionary(32);
	CompileError *error;
	Iterator iterator;

	initOrdCollIterator(&iterator, methods, 0, 0);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope2;
		openHandleScope(&scope2);

		MethodNode *methodNode = (MethodNode *) iteratorNextObject(&iterator);
		if (isNil(methodNodeGetClassName(methodNode))) {
			error = compileAndInstallMethod(methodNode, class, methodDict, ns);
		} else { // TODO: check for class name: methodNode->className == "class"
			error = compileAndInstallMethod(methodNode, (Class *) metaClass, classMethodDict, ns);
		}
		if (error != NULL) {
			return closeHandleScope(&scope, closeHandleScope(&scope2, error));
		}

		closeHandleScope(&scope2, NULL);
	}

	classSetMethodDictionary(class, methodDict);
	metaClassSetMethodDictionary(metaClass, classMethodDict);
	closeHandleScope(&scope, NULL);
	return NULL;
}


static CompileError *compileAndInstallMethod(MethodNode *node, Class *class, Dictionary *methodDict, Namespace *ns)
{
	if (!isNil(symbolDictObjectAt(methodDict, asSymbol(methodNodeGetSelector(node))))) {
		return createMethodRedefinitionError(node);
	}

	Object *method = compileMethodIn(node, class, ns);
	if (method->raw->class == Handles.CompiledMethod->raw) {
		symbolDictAtPutObject(methodDict, compiledMethodGetSelector((CompiledMethod *) method), method);
		return NULL;
	} else {
		return (CompileError *) method;
	}
}


static CompileError *createMethodRedefinitionError(MethodNode *node)
{
	LiteralNode *literal = newObject(Handles.VariableNode, 0);
	literalNodeSetValue(literal, (Object *) methodNodeGetSelector(node));
	literalNodeSetSourceCode(literal, methodNodeGetSourceCode(node));
	return createRedefinitionError(literal)	;
}


typedef struct {
	RawObject *target;
	RawObject *replacement;
} RedefineContext;


typedef struct {
	Class *target;
	MetaClass *metaClass;
	Dictionary *instDict;
	Dictionary *classDict;
} ExtendContext;


// Runs inside the icInvalidateAllSends STW bracket: two pointer stores, no
// allocation. The replacement dictionaries were fully built OUTSIDE the
// pause (a dictionary insert can grow and therefore allocate); swapping the
// whole dictionary here means lock-free lookup readers never see a
// half-grown table, and every send cache clears in the same pause.
static void extendMutate(void *arg)
{
	ExtendContext *context = arg;
	objectStorePtr((Object *) context->target,
		&context->target->raw->methodDictionary, (Object *) context->instDict);
	objectStorePtr((Object *) context->metaClass,
		&context->metaClass->raw->methodDictionary, (Object *) context->classDict);
}


typedef struct {
	Object *holder;
	Dictionary *replacement;
} RemoveSelectorContext;


static void removeSelectorMutate(void *arg)
{
	RemoveSelectorContext *context = arg;
	// Class and MetaClass share the methodDictionary field position.
	objectStorePtr(context->holder,
		&((Class *) context->holder)->raw->methodDictionary, (Object *) context->replacement);
}


// Remove `selector` from the receiver's OWN method dictionary (the receiver
// is a Class or a MetaClass), under the same build-outside/swap-under-STW
// discipline as extendClass. Answers 0 when the selector is not defined
// there (the image-side wrapper turns that into an error).
_Bool classRemoveSelector(Object *holder, String *selector)
{
	HandleScope scope;
	openHandleScope(&scope);

	Dictionary *live = (Dictionary *) scopeHandle(asObject(((Class *) holder)->raw->methodDictionary));
	// asSymbol FAILs on an already-interned Symbol; the wrapper passes one
	String *symbol = selector->raw->class == Handles.Symbol->raw
		? selector
		: asSymbol(selector);
	if (isNil(live) || isNil(symbolDictObjectAt(live, symbol))) {
		closeHandleScope(&scope, NULL);
		return 0;
	}

	size_t total = dictSize(live) + 8;
	Dictionary *fresh = newDictionary(dictionaryCapacityFor(total));
	// index + handle re-read per step: inserts can scavenge and move the
	// contents array (see dictionaryCopyInto)
	Array *contents = dictGetContents(live);
	size_t capacity = contents->raw->size;
	for (size_t i = 0; i < capacity; i++) {
		HandleScope scope2;
		openHandleScope(&scope2);
		Association *assoc = (Association *) scopeHandle(asObject(contents->raw->vars[i]));
		if (!isNil(assoc) && asObject(assoc->raw->key) != (RawObject *) symbol->raw) {
			symbolDictAtPut(fresh,
				(String *) scopeHandle(asObject(assoc->raw->key)), assoc->raw->value);
		}
		closeHandleScope(&scope2, NULL);
	}

	RemoveSelectorContext context = { holder, fresh };
	icInvalidateAllSends(removeSelectorMutate, &context);
	closeHandleScope(&scope, NULL);
	return 1;
}


// Dictionary probing masks with `hash & (capacity - 1)`: capacity MUST be a
// power of two, like every newDictionary call site in the VM.
static size_t dictionaryCapacityFor(size_t entries)
{
	size_t capacity = 16;
	while (capacity < entries * 2) {
		capacity <<= 1;
	}
	return capacity;
}


// Copy every association of `source` into `fresh`. Inserting allocates (and
// can scavenge, moving the source's contents array), so NO raw-pointer
// Iterator here: index into the contents through a handle, re-reading the
// raw array every step, the same discipline as parseFileAndInitialize.
static void dictionaryCopyInto(Dictionary *fresh, Dictionary *source)
{
	HandleScope scope;
	openHandleScope(&scope);

	Array *contents = dictGetContents(source);
	size_t capacity = contents->raw->size;
	for (size_t i = 0; i < capacity; i++) {
		HandleScope scope2;
		openHandleScope(&scope2);
		Association *assoc = (Association *) scopeHandle(asObject(contents->raw->vars[i]));
		if (!isNil(assoc)) {
			symbolDictAtPut(fresh,
				(String *) scopeHandle(asObject(assoc->raw->key)), assoc->raw->value);
		}
		closeHandleScope(&scope2, NULL);
	}
	closeHandleScope(&scope, NULL);
}


// A fresh dictionary holding every association of `live` plus every one of
// `adds`, sized so the copy itself never grows mid-insert.
static Dictionary *dictionaryMerging(Dictionary *live, Dictionary *adds)
{
	HandleScope scope;
	openHandleScope(&scope);

	size_t total = (isNil(live) ? 0 : dictSize(live)) + dictSize(adds) + 8;
	Dictionary *fresh = newDictionary(dictionaryCapacityFor(total));
	if (!isNil(live)) {
		dictionaryCopyInto(fresh, live);
	}
	dictionaryCopyInto(fresh, adds);
	return closeHandleScope(&scope, fresh);
}


// `Name := Namespace [ classDefs ]`: declare (or REOPEN) a namespace from
// source, decoupling namespaces from packages: one file can declare several,
// several files can grow one. The enclosed definitions (classes, extensions,
// even nested namespace declarations) build INTO the declared namespace,
// which imports the DECLARING namespace (lexical visibility: members see the
// surrounding project's names, plus core as always). The namespace OBJECT is
// bound as a global in the declaring namespace, so `(Banner at: #Formatter)`
// works wherever the declaration is visible. Freshly built member classes
// get their class-side initialize at the end of the declaration; loaders
// therefore skip namespace nodes in their own initialize passes.
static Object *declareNamespace(ClassNode *node, Namespace *ns)
{
	HandleScope scope;
	openHandleScope(&scope);

	String *name = literalNodeGetStringValue(classNodeGetName(node));
	String *symbol = asSymbol(name);
	Namespace *target;
	Object *existing = symbolDictObjectAt(Handles.Namespaces, symbol);
	if (isNil(existing)) {
		Array *imports;
		if (ns == NULL || ns->raw == (RawNamespace *) Handles.CoreNamespace->raw) {
			imports = newArray(0); // core is the implicit fallback anyway
		} else {
			imports = newArray(1);
			arrayAtPutObject(imports, 0, (Object *) ns);
		}
		target = newNamespace(symbol, newDictionary(32), imports);
		symbolDictAtPutObject(Handles.Namespaces, symbol, (Object *) target);
	} else if (((Object *) existing)->raw->class == Handles.Namespace->raw) {
		target = (Namespace *) existing; // reopen: add more members
	} else {
		return (Object *) closeHandleScope(&scope,
			createUndefinedVariableError(classNodeGetName(node)));
	}
	namespaceAtPutObject(ns, name, (Object *) target);

	OrderedCollection *members = classNodeGetMembers(node);
	size_t size = ordCollSize(members);
	for (size_t i = 0; i < size; i++) {
		HandleScope scope2;
		openHandleScope(&scope2);
		Object *built = buildClassIn((ClassNode *) ordCollObjectAt(members, i), target);
		if (isCompileError(built)) {
			return (Object *) closeHandleScope(&scope, closeHandleScope(&scope2, built));
		}
		closeHandleScope(&scope2, NULL);
	}

	// class-side initialize for the freshly built member CLASSES, after the
	// whole body built (forward references between members resolved by then)
	for (size_t i = 0; i < size; i++) {
		HandleScope scope2;
		openHandleScope(&scope2);
		ClassNode *member = (ClassNode *) ordCollObjectAt(members, i);
		if (!classNodeIsExtension(member) && !classNodeIsNamespace(member)) {
			Association *assoc = namespaceOwnAssocAt(target,
				literalNodeGetStringValue(classNodeGetName(member)));
			if (!isNil(assoc) && !isTaggedNil(assoc->raw->value)) {
				invokeInititalize(scopeHandle(asObject(assoc->raw->value)));
			}
		}
		closeHandleScope(&scope2, NULL);
	}

	return (Object *) closeHandleScope(&scope, target);
}


// `Name extend [ methods ]`: additive extension of an EXISTING class. The
// target resolves through the extending namespace's chain, so a package can
// extend core or imported classes; method bodies compile against the
// EXTENDING namespace (their globals resolve there) with the target as owner
// class (its instance variables stay visible). A selector already present in
// the target's OWN dictionaries is a RedefinitionError and nothing installs
// (two packages extending one class with one selector is deterministic);
// overriding an INHERITED selector is ordinary Smalltalk practice and is
// exactly why the commit runs under icInvalidateAllSends: IC cells bound to
// the superclass method for this receiver class must rebind. Shape or ivar
// changes are impossible by grammar (extensions have no variable section).
static Object *extendClass(ClassNode *node, Namespace *ns)
{
	HandleScope scope;
	openHandleScope(&scope);

	String *name = literalNodeGetStringValue(classNodeGetName(node));
	Association *assoc = namespaceResolveAssoc(ns, name);
	if (isNil(assoc) || isTaggedNil(assoc->raw->value)) {
		return (Object *) closeHandleScope(&scope,
			createUndefinedVariableError(classNodeGetName(node)));
	}
	Class *target = (Class *) scopeHandle(asObject(assoc->raw->value));
	if (target->raw->class == NULL || target->raw->class->class != Handles.MetaClass->raw) {
		return (Object *) closeHandleScope(&scope,
			createUndefinedVariableError(classNodeGetName(node)));
	}

	MetaClass *metaClass = classGetMetaClass(target);
	Dictionary *instAdds = newDictionary(16);
	Dictionary *classAdds = newDictionary(8);
	CompileError *error;
	Iterator iterator;

	initOrdCollIterator(&iterator, classNodeGetMethods(node), 0, 0);
	while (iteratorHasNext(&iterator)) {
		HandleScope scope2;
		openHandleScope(&scope2);

		MethodNode *methodNode = (MethodNode *) iteratorNextObject(&iterator);
		_Bool classSide = !isNil(methodNodeGetClassName(methodNode));
		Class *owner = classSide ? (Class *) metaClass : target;
		Dictionary *live = classSide
			? metaClassGetMethodDictionary(metaClass)
			: classGetMethodDictionary(target);
		if (!isNil(live)
				&& !isNil(symbolDictObjectAt(live, asSymbol(methodNodeGetSelector(methodNode))))) {
			error = createMethodRedefinitionError(methodNode);
			return (Object *) closeHandleScope(&scope, closeHandleScope(&scope2, error));
		}
		error = compileAndInstallMethod(methodNode, owner,
			classSide ? classAdds : instAdds, ns);
		if (error != NULL) {
			return (Object *) closeHandleScope(&scope, closeHandleScope(&scope2, error));
		}
		closeHandleScope(&scope2, NULL);
	}

	ExtendContext context = {
		target,
		metaClass,
		dictionaryMerging(classGetMethodDictionary(target), instAdds),
		dictionaryMerging(metaClassGetMethodDictionary(metaClass), classAdds),
	};
	icInvalidateAllSends(extendMutate, &context);

	return (Object *) closeHandleScope(&scope, target);
}


// Runs inside the icInvalidateAllSends STW bracket: raw pointers stay stable
// (nothing moves, nothing allocates) for the duration.
static void redefineMutate(void *arg)
{
	RedefineContext *ctx = arg;
	// TODO: temporarily do memcpy() instead of #become:
	memcpy(ctx->target, ctx->replacement, sizeof(RawClass));
}


// Install looks at the namespace's OWN bindings only: the same class name in
// two namespaces never collides. A nil-valued binding (vivified forward
// reference) is filled in place, snapping earlier-compiled references to the
// class. A live binding is a redefinition: dev/reload semantics, the new raw
// class is copied over the existing object so every reference stays valid,
// under a full send-cache invalidation: IC cells are keyed by the class
// OBJECT (whose identity survives the copy), so without the reset a hot call
// site would keep dispatching into the replaced method dictionary's code
// until the next scavenge.
static Class *installClass(Class *class, Namespace *ns)
{
	String *name = classGetName(class);
	Association *assoc = namespaceOwnAssocAt(ns, name);
	if (isNil(assoc) || isTaggedNil(assoc->raw->value)) {
		namespaceAtPutObject(ns, name, (Object *) class);
		return class;
	} else {
		Object *currentClass = scopeHandle(asObject(assoc->raw->value));
		RedefineContext ctx = { currentClass->raw, (RawObject *) class->raw };
		icInvalidateAllSends(redefineMutate, &ctx);
		return (Class *) currentClass;
	}
}


static void addSubClass(Class *class, Class *subClass)
{
	ordCollAddObject(classGetSubClasses(class), (Object *) subClass);
	ordCollAddObject(metaClassGetSubClasses(classGetMetaClass(class)), (Object *) classGetMetaClass(subClass));
}


CompiledMethod *lookupSelector(Class *startClass, String *selector)
{
	HandleScope scope;
	openHandleScope(&scope);

	Class *class = startClass;
	CompiledMethod *method = (CompiledMethod *) symbolDictObjectAt(classGetMethodDictionary(class), selector);

	while (isNil(method)) {
		class = classGetSuperClass(class);
		if (isNil(class)) {
			return closeHandleScope(&scope, NULL);
		}
		method = (CompiledMethod *) symbolDictObjectAt(classGetMethodDictionary(class), selector);
	}

	return closeHandleScope(&scope, method);
}


void printClassName(RawClass *class)
{
	_Bool isMetaClass = class->class == Handles.MetaClass->raw;
	if (isMetaClass) {
		class = (RawClass *) asObject(((RawMetaClass *) class)->instanceClass);
	}
	// Qualify with the home namespace for anything outside Core, so VM-level
	// backtraces disambiguate same-named classes across packages.
	RawObject *ns = asObject(class->namespace);
	if (ns != Handles.nil->raw && ns != (RawObject *) Handles.CoreNamespace->raw) {
		RawString *nsName = (RawString *) asObject(((RawNamespace *) ns)->name);
		printf("%.*s.", (int) nsName->size, nsName->contents);
	}
	RawString *className = (RawString *) asObject(class->name);
	printf("%.*s", (int) className->size, className->contents);
	if (isMetaClass) {
		printf(" class");
	}
}
