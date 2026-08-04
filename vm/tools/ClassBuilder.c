#include "tools/ClassBuilder.h"
#include "core/Assert.h"
#include "core/ClassTable.h"
#include "core/Handle.h"
#include "core/Namespace.h"
#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "jit/CompiledMethod.h"
#include "jit/Jit.h"
#include "memory/Heap.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"
#include "runtime/String.h"
#include <string.h>
#include "jit/CompiledMethod.h"
#include "runtime/Closure.h"

// A class the system ALREADY HAS under this name, or NULL.
//
// A global that exists holding NIL is not a class: it is a FORWARD REFERENCE
// that the compiler created when an earlier method mentioned the name
// (vm/compiler/Compile.c), and the definition being built now is what fills it
// in. Reading one as an existing class is how `Integer` came back with the wrong
// shape and how its subclasses then walked nil looking for instance variables.
//
// OWN BINDINGS ONLY, never the import chain. Resolving through the chain would
// find an imported package's class of the same name and REOPEN it, so two
// packages that both define `Formatter` would silently become one -- which is
// precisely the shadowing the modules sample exists to demonstrate working.
static Class *classFromBinding(Association *binding)
{
	if (binding == NULL) {
		return NULL;
	}
	Value value = binding->raw->value;
	if (!valueTypeOf(value, VALUE_POINTER) || asObject(value) == Handles.nil.raw) {
		return NULL;
	}
	return (Class *) scopeHandle(asObject(value));
}


// The class this DEFINITION is reopening, if any: own bindings only, for the
// reason above.
static Class *definedClassNamed(Namespace *namespace, String *name)
{
	return classFromBinding(namespaceOwnAssocAt(namespace, name));
}


// A class a definition REFERS to -- its superclass, or the target of
// `Name extend [ ... ]` -- through the full chain: own bindings, then imports
// in declaration order, then Core.
//
// It has to be the chain and not own bindings: a package subclasses `Object`,
// which lives in Core, so an own-only probe answers nothing and the build stops
// with "superclass is not defined yet: Object". Referring to a name and
// defining one are different questions, and this is the pair that answers them.
static Class *resolvedClassNamed(Namespace *namespace, String *name)
{
	return classFromBinding(namespaceResolveAssoc(namespace, name));
}


static void fail(ClassBuildError *error, const char *message, String *what)
{
	if (error->message == NULL) {
		error->message = message;
		error->what = what;
	}
}


// The same, naming the kernel exception class the image should raise. Plain
// `fail` leaves it NULL, which the reflective primitive reads as Error.
static void failAs(ClassBuildError *error, const char *errorClass,
	const char *message, String *what)
{
	if (error->message == NULL) {
		fail(error, message, what);
		error->errorClass = errorClass;
	}
}


static Dictionary *methodsOf(Class *class)
{
	Value dictionary = class->raw->methodDictionary;
	if (valueTypeOf(dictionary, VALUE_POINTER)) {
		return scopeHandle(asObject(dictionary));
	}
	Dictionary *methods = newDictionary(16);
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->methodDictionary,
		(RawObject *) methods->raw);
	return methods;
}


// ---------------------------------------------------------------------------
// Metaclasses
// ---------------------------------------------------------------------------
//
// A class-side method belongs in the method dictionary of the class's OWN class.
// Until a class has one of its own, every class is an instance of the single
// class-of-classes, and installing a class-side method there would put it on
// EVERY class in the system.
//
// EVERY class gets one, and not lazily. Creating it only for classes that
// declare a class-side method looked like a saving and was a bug: the metaclass
// chain is what makes a class-side method INHERITED, so a class with none of its
// own stayed an instance of the shared class-of-classes and the lookup for
// `Array with: 1 with: 2` never reached ArrayedCollection's metaclass, where
// with:with: is defined.

Class *classMetaclassOf(Class *class)
{
	HandleScope scope;
	openHandleScope(&scope);
	Class *held = scopeHandle(class->raw);

	uint32_t current = rawObjectClassIndex((RawObject *) held->raw);
	if (current != classIndexOf(&Handles.ClassClass)) {
		Class *existing = scopeHandle(classTableAt(&CurrentThread.heap->classes, current));
		return closeHandleScope(&scope, existing);
	}

	// The metaclass chain PARALLELS the class chain: the metaclass of a class
	// inherits from the metaclass of its superclass, which is what makes a
	// class-side method inherited by subclasses. At the root it lands on the
	// class-of-classes -- `Object class superClass == Class` -- where the
	// class-side methods every class has (new, new:) already live.
	RawClass *super = rawClassSuperclass(held->raw);
	Class *superMeta = super == NULL
		? &Handles.ClassClass
		: classMetaclassOf(scopeHandle((RawObject *) super));

	// NO NAME IS STORED. A metaclass answers `instanceClass name`, which
	// packages/Core/src/MetaClass.st defines and which is the only spelling that
	// stays right when a class is renamed. This used to build the Symbol
	// `#'MetaA class'` and store it, so `MetaA class name` answered that instead
	// of `#MetaA`, and printing was the only place the difference showed.
	Class *meta = classCreate(superMeta, NULL, CLASS_OF_CLASSES_SHAPE);
	methodsOf(meta);
	// THE METACLASS IS AN INSTANCE OF MetaClass, which is what makes
	// `MetaA class class == MetaClass` true and what separates the two kinds of
	// Behavior for `isKindOf:`: a metaclass is a ClassDescription and a Behavior
	// but NOT a Class. classCreate stamps every new class as an instance of the
	// class-of-classes, which is right for a class and wrong for this.
	rawObjectSetClassIndex((RawObject *) meta->raw, classIndexOf(&Handles.MetaClass));
	// And it points BACK at its class, in the field a class uses for its name
	// (core/Object.h, RawMetaClass). Through the barrier: the metaclass was just
	// allocated and the class may already be old.
	rawObjectStorePtr((RawObject *) meta->raw,
		&((RawMetaClass *) meta->raw)->instanceClass, (RawObject *) held->raw);
	// The class is now an instance of its metaclass. Only the class OBJECT's
	// header changes; the class's own index in the table, which is what every
	// baked immediate and every inline cache compares, is untouched.
	rawObjectSetClassIndex((RawObject *) held->raw, classIndexOf(meta));
	return closeHandleScope(&scope, meta);
}


// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------
//
// `<shape: BytesShape>` says how instances are laid out, and the mapping is
// explicit rather than defaulted: a shape guessed wrong is a collector walking
// the wrong words, which is the most expensive kind of quiet mistake in this
// system. A pragma this does not know is a NAMED failure.

typedef struct {
	const char *pragma;
	uint8_t format;
	_Bool countsFixedSlots; // does the instance variable count go in fixedSlots
	// A layout the VM OWNS, used exactly as given. A class carrying one of these
	// is a Smalltalk MIRROR of a C struct, and the pragma is how the mirror says
	// so out loud instead of a builder inferring it from a field list.
	_Bool exact;
	InstanceShape shape;
} ShapeMapping;

#define SHAPE_FROM_FORMAT(f, counts) { NULL, (f), (counts), 0, { 0, 0, 0, 0 } }

static ShapeMapping gShapes[] = {
	{ "FixedShape", FORMAT_POINTERS, 1, 0, { 0, 0, 0, 0 } },
	{ "IndexedShape", FORMAT_INDEXED_POINTERS, 1, 0, { 0, 0, 0, 0 } },
	{ "BytesShape", FORMAT_BYTES, 0, 0, { 0, 0, 0, 0 } },
	{ "StringShape", FORMAT_BYTES, 0, 0, { 0, 0, 0, 0 } },
	// One raw word holding an IEEE double, and no pointers in it: getting this
	// wrong would have the collector chase a mantissa as an address.
	{ "FloatShape", FORMAT_NO_POINTERS, 0, 1,
		{ FORMAT_NO_POINTERS, 0, 0, 1 } },
	{ "ClassShape", FORMAT_MIXED_BYTES, 0, 1, { 0, 0, 0, 0 } },
	{ "CompiledMethodShape", FORMAT_MIXED_BYTES, 0, 1, { 0, 0, 0, 0 } },
	{ "ClosureShape", FORMAT_INDEXED_POINTERS, 0, 1, { 0, 0, 0, 0 } },
};


// The three shapes above whose layout comes from a C header are filled in here
// rather than written twice: DEFINE_SHAPE is not a constant expression the
// initialiser above can use, and a second copy of these numbers is exactly the
// drift the mirror check exists to prevent.
static void shapesResolve(void)
{
	for (size_t i = 0; i < sizeof(gShapes) / sizeof(gShapes[0]); i++) {
		if (strcmp(gShapes[i].pragma, "ClassShape") == 0) {
			gShapes[i].shape = CLASS_OF_CLASSES_SHAPE;
		} else if (strcmp(gShapes[i].pragma, "CompiledMethodShape") == 0) {
			gShapes[i].shape = COMPILED_METHOD_SHAPE;
		} else if (strcmp(gShapes[i].pragma, "ClosureShape") == 0) {
			gShapes[i].shape = CLOSURE_SHAPE;
		}
	}
}


// The `<shape: X>` pragma of a class node, or NULL when it has none.
// The name in `<shape: Something>`, or NULL when there is no shape pragma.
//
// THE ARGUMENT HAS TO BE AN IDENTIFIER, and that is checked rather than
// assumed. The parser answers a VariableNode for `BytesShape`, a StringNode for
// `'hi'` and a CharacterNode for `$a`, and only the first holds a String: a
// CharacterNode holds a Character IMMEDIATE, so reading it as a String went
// through asObject on an immediate and ABORTED THE VM while compiling. That is
// the tag-check-before-asObject rule, and a malformed pragma in someone's source
// is exactly the input that must not crash a compiler.
static String *shapePragmaOf(ClassNode *node, ClassBuildError *error)
{
	OrderedCollection *pragmas = classNodeGetPragmas(node);
	size_t count = pragmas == NULL ? 0 : ordCollSize(pragmas);
	for (size_t i = 0; i < count; i++) {
		MessageExpressionNode *pragma = scopeHandle(asObject(ordCollAt(pragmas, i)));
		if (!stringEqualsC(messageExpressionNodeGetSelector(pragma), "shape:")) {
			continue;
		}
		OrderedCollection *arguments = messageExpressionNodeGetArgs(pragma);
		if (arguments == NULL || ordCollSize(arguments) != 1) {
			failAs(error, "InvalidPragmaError",
				"shape: takes exactly one argument", classNodeGetName(node) == NULL
					? NULL : literalNodeGetStringValue(classNodeGetName(node)));
			return NULL;
		}
		Object *argument = scopeHandle(asObject(ordCollAt(arguments, 0)));
		if (Handles.VariableNode.raw == NULL
				|| rawObjectClassIndex(argument->raw)
					!= classIndexOf(&Handles.VariableNode)) {
			failAs(error, "InvalidPragmaError",
				"shape: takes a shape NAME, not a literal", NULL);
			return NULL;
		}
		return literalNodeGetStringValue((LiteralNode *) argument);
	}
	return NULL;
}


// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

CompiledMethod *classCompileMethodInto(MethodNode *node, Class *target,
	Class *classVariableScope, Namespace *namespace, ClassBuildError *error)
{
	// Instance variables and super come from `target`, which for a class-side
	// method is the metaclass; class variables come from the scope, which is
	// where they were declared and where both sides have to find the same
	// Association.
	CompileContext context = { target, smalltalkGlobals(),
		classVariableScope != NULL ? classVariableScope : target, namespace };
	CompileError compileError;
	CodeUnit *unit = compileMethod(node, &context, &compileError);
	if (unit == NULL) {
		// BOTH names: what failed and where. The selector alone sends a
		// reader looking through a file for a name the message never gave.
		error->status = compileError.status;
		error->inMethod = methodNodeGetSelector(node);
		fail(error, compileStatusName(compileError.status), compileError.what);
		// The compile statuses that the image catches BY KIND carry their
		// class and the offending node up to the reflective primitive, which
		// is what turns `zzzTypo foo` into an UndefinedVariableError instead
		// of a plain Error (compileStatusErrorClass in compiler/Compile.c).
		error->errorClass = compileStatusErrorClass(compileError.status);
		error->identifier = compileError.node;
		return NULL;
	}
	String *selector = scopeHandle(asObject(unit->selector));
	CompiledMethod *method = compiledMethodCreate(unit, selector, target);
	// The blocks inside it can only learn which method they belong to once that
	// method object exists, which is here (jit/CompiledMethod.h).
	compiledMethodBindBlocks(method);
	symbolDictAtPutObject(methodsOf(target), selector, (Object *) method);
	// INSTALLING A METHOD INVALIDATES EVERY RESOLVED SEND, and removing one
	// already knew that (primClassRemoveSelector). Installing did not, and got
	// away with it while the runtime looked the answer up on every send: the
	// cached target was only ever read by the runtime that had just written it.
	// A send site with an inline cache reads it in COMPILED CODE, so a class
	// whose method is replaced -- or one that starts defining a selector it used
	// to inherit -- keeps calling the old method until something else evicts the
	// way. Wrong ANSWER, not a crash, and only at sites warm enough to be armed.
	//
	// CONSERVATIVE ON PURPOSE. Which sites a definition can affect is "every site
	// that dispatches to this selector for any class that inherits from this
	// one", and computing that costs more than flushing does at the frequency
	// definitions happen.
	jitFlushSendCaches();
	return method;
}


// Does this extension redefine a selector the class defines ITSELF?
//
// OWN DICTIONARY ONLY, on whichever side the method is written for. An
// INHERITED selector is a legal override -- `ExtChild extend [ probe [ ... ] ]`
// over `ExtParent>>probe` is the ordinary thing extensions are for -- and only a
// selector already in this class's own dictionary is a redefinition. Asking the
// full lookup chain instead would refuse every override, which is the same
// mistake in the other direction and just as silent.
static void checkNoOwnSelectorCollision(Class *class, ClassNode *node,
	ClassBuildError *error)
{
	OrderedCollection *methods = classNodeGetMethods(node);
	size_t count = methods == NULL ? 0 : ordCollSize(methods);
	for (size_t i = 0; i < count; i++) {
		HandleScope scope;
		openHandleScope(&scope);
		MethodNode *methodNode = scopeHandle(asObject(ordCollAt(methods, i)));
		String *side = methodNodeGetClassName(methodNode);
		Class *target = side != NULL && stringEqualsC(side, "class")
			? classMetaclassOf(class) : class;
		String *selector = methodNodeGetSelector(methodNode);
		Value existing = selector == NULL ? 0
			: symbolDictAt(methodsOf(target), asSymbol(selector));
		if (valueTypeOf(existing, VALUE_POINTER)) {
			failAs(error, "RedefinitionError",
				"extension redefines a selector the class defines itself", selector);
			closeHandleScope(&scope, NULL);
			return;
		}
		closeHandleScope(&scope, NULL);
	}
}


// Does one DEFINITION spell the same selector twice, on the same side?
//
// Refused rather than resolved: installing both would have the second silently
// replace the first in the dictionary, and half of what the file says would
// never run. It is a per-NODE question -- reopening a class, or extending it
// from another file, installs over the dictionary on purpose and is not this.
static void checkNoDuplicateSelector(ClassNode *node, ClassBuildError *error)
{
	OrderedCollection *methods = classNodeGetMethods(node);
	size_t count = methods == NULL ? 0 : ordCollSize(methods);
	for (size_t i = 0; i < count && error->message == NULL; i++) {
		for (size_t j = i + 1; j < count && error->message == NULL; j++) {
			HandleScope scope;
			openHandleScope(&scope);
			MethodNode *first = scopeHandle(asObject(ordCollAt(methods, i)));
			MethodNode *second = scopeHandle(asObject(ordCollAt(methods, j)));
			String *firstSide = methodNodeGetClassName(first);
			String *secondSide = methodNodeGetClassName(second);
			_Bool firstClassSide = firstSide != NULL && stringEqualsC(firstSide, "class");
			_Bool secondClassSide = secondSide != NULL && stringEqualsC(secondSide, "class");
			if (firstClassSide == secondClassSide
					&& stringEquals(methodNodeGetSelector(first),
						methodNodeGetSelector(second))) {
				// Re-homed to the outermost scope: this scope closes on the
				// next line, and the name is read by whoever reports the error
				// long after every build scope is gone.
				failAs(error, "RedefinitionError",
					"one definition spells this selector twice", (String *)
					outermostHandle(methodNodeGetSelector(second)->raw));
			}
			closeHandleScope(&scope, NULL);
		}
	}
}


static void buildMethods(Class *class, ClassNode *node, Namespace *namespace,
	ClassBuildError *error)
{
	OrderedCollection *methods = classNodeGetMethods(node);
	size_t count = methods == NULL ? 0 : ordCollSize(methods);
	checkNoDuplicateSelector(node, error);
	for (size_t i = 0; i < count && error->message == NULL; i++) {
		HandleScope scope;
		openHandleScope(&scope);
		MethodNode *methodNode = scopeHandle(asObject(ordCollAt(methods, i)));

		// `class foo [ ... ]` goes on the metaclass. The parser records the word
		// that preceded the pattern, so this is where it is read back.
		String *side = methodNodeGetClassName(methodNode);
		_Bool classSide = side != NULL && stringEqualsC(side, "class");
		Class *target = classSide ? classMetaclassOf(class) : class;

		classCompileMethodInto(methodNode, target, class, namespace, error);
		closeHandleScope(&scope, NULL);
		if (error->message != NULL) {
			return;
		}
	}
}


// The instance variables a class DECLARES, as a collection of Symbols. Only its
// own: the compiler walks the superclass chain to build the slot numbering
// (vm/compiler/Compile.c), so repeating inherited ones here would number them
// twice.
// A name in the variable section that starts with a CAPITAL is a CLASS
// VARIABLE, not an instance variable. That is Smalltalk's own convention and the
// kernel depends on it: `Character` declares `| Table |` and fills it from a
// class-side method, and Character's instances are IMMEDIATES with no slots at
// all, so there is nowhere for an instance variable to live.
//
// It becomes an Association in the class's classVariables dictionary, which is
// the same thing a global is, so the compiler reads and writes it with the
// instructions it already has.
static _Bool isClassVariableName(String *name)
{
	return rawStringSize(name->raw) > 0 && name->raw->contents[0] >= 'A'
		&& name->raw->contents[0] <= 'Z';
}


static OrderedCollection *ownInstanceVariables(ClassNode *node)
{
	OrderedCollection *declared = classNodeGetVars(node);
	size_t count = declared == NULL ? 0 : ordCollSize(declared);
	OrderedCollection *names = newOrdColl(count == 0 ? 1 : count);
	for (size_t i = 0; i < count; i++) {
		LiteralNode *variable = scopeHandle(asObject(ordCollAt(declared, i)));
		String *name = literalNodeGetStringValue(variable);
		if (!isClassVariableName(name)) {
			ordCollAdd(names, objectTagged(asSymbol(name)));
		}
	}
	return names;
}


// The class variables a class declares, into its own dictionary, each holding
// nil until something assigns it.
static void declareClassVariables(Class *class, ClassNode *node)
{
	OrderedCollection *declared = classNodeGetVars(node);
	size_t count = declared == NULL ? 0 : ordCollSize(declared);
	Dictionary *variables = NULL;
	for (size_t i = 0; i < count; i++) {
		LiteralNode *variable = scopeHandle(asObject(ordCollAt(declared, i)));
		String *name = literalNodeGetStringValue(variable);
		if (!isClassVariableName(name)) {
			continue;
		}
		if (variables == NULL) {
			Value existing = class->raw->classVariables;
			variables = valueTypeOf(existing, VALUE_POINTER)
				? scopeHandle(asObject(existing)) : newDictionary(8);
			rawObjectStorePtr((RawObject *) class->raw, &class->raw->classVariables,
				(RawObject *) variables->raw);
		}
		symbolDictAtPut(variables, asSymbol(name), tagPtr(Handles.nil.raw));
	}
}


// How many instance variables a class and its superclasses DECLARE. The mirror
// check needs the count the compiler will number against, and that comes from
// the declarations rather than from the shape, because a mirror's shape is the
// C struct's and says nothing about how many names Smalltalk gave it.
static size_t declaredVariableCount(Class *class)
{
	size_t count = 0;
	for (Class *level = class; level != NULL; ) {
		Value declared = level->raw->instanceVariables;
		if (valueTypeOf(declared, VALUE_POINTER)) {
			count += ordCollSize(scopeHandle(asObject(declared)));
		}
		RawClass *super = rawClassSuperclass(level->raw);
		level = super == NULL ? NULL : scopeHandle((RawObject *) super);
	}
	return count;
}


static uint16_t inheritedSlots(Class *super)
{
	return super == NULL ? 0 : (uint16_t) super->raw->instanceShape.fixedSlots;
}


// Record `class` among its superclass's DIRECT subclasses.
//
// Nothing maintained this field before, so `Behavior>>subClasses` answered nil
// for every class in the system and the whole reflective protocol built on it
// -- subClasses, allSubClassesDo:, a class browser -- had nothing to walk.
//
// DIRECT ONLY, which is what the collection means and what the test that
// separates a right implementation from a plausible one checks: a grandchild
// must NOT appear in its grandparent's list. The transitive walk is
// allSubClassesDo:, and it is already written in Smalltalk on top of this.
//
// IDEMPOTENT, because reopening a class is the common case in this system: the
// built-in kernel defines Object, Array and the rest, and packages/Core defines
// them again with their real contents. Appending without checking would give
// Object a second entry for every class in the kernel.
static void recordSubclass(Class *super, Class *class)
{
	if (super == NULL) {
		return; // Object, whose superclass is nil, is nobody's subclass
	}
	HandleScope scope;
	openHandleScope(&scope);
	Class *held = scopeHandle((RawObject *) class->raw);
	Class *parent = scopeHandle((RawObject *) super->raw);
	Value existing = parent->raw->subClasses;
	OrderedCollection *list;
	if (valueTypeOf(existing, VALUE_POINTER) && asObject(existing) != Handles.nil.raw) {
		list = scopeHandle(asObject(existing));
		size_t count = ordCollSize(list);
		for (size_t i = 0; i < count; i++) {
			if (asObject(ordCollAt(list, i)) == (RawObject *) held->raw) {
				closeHandleScope(&scope, NULL);
				return;
			}
		}
	} else {
		list = newOrdColl(4);
		// Through the barrier: the collection is young and the superclass may
		// already have been promoted.
		rawObjectStorePtr((RawObject *) parent->raw, &parent->raw->subClasses,
			(RawObject *) list->raw);
	}
	ordCollAddObject(list, (Object *) held);
	closeHandleScope(&scope, NULL);
}


// ---------------------------------------------------------------------------
// `Name := Namespace [ ... ]`
// ---------------------------------------------------------------------------
//
// EVERYTHING THIS NEEDS IS ALREADY IN packages/Core, and none of it is rebuilt
// here. `Namespace named:imports:` makes one, the `Namespaces` global is the
// registry, and `PackageLoader>>loadPackage:` is this exact sequence for a
// package: make the namespace, register it, build the sources into it, then
// initialize what was built. A Namespace assembled in C would be a second
// encoding of a shape Namespace.st owns.
//
// IT LIVES IN C rather than in Compiler.st because a class definition reaches
// this builder three ways -- the reflective compiler, `st -f`, and the package
// bootstrap -- and only the first goes through Smalltalk. One implementation,
// every path.

// The registry, or NULL before packages/Core has been loaded. Before that there
// is no Namespace class either, so a declaration cannot be built and says so.
static Dictionary *namespaceRegistry(void)
{
	Object *registry = getGlobalObject("Namespaces");
	if (registry == NULL || registry->raw == Handles.nil.raw) {
		return NULL;
	}
	return (Dictionary *) registry;
}


// Declare, or REOPEN. Reopening is the same object: the test that separates a
// right implementation from a plausible one is that a second declaration adds
// members and keeps the ones already there, which a fresh Namespace would not.
static Namespace *declareNamespace(String *name, Namespace *declarant,
	ClassBuildError *error)
{
	HandleScope scope;
	openHandleScope(&scope);
	Dictionary *registry = namespaceRegistry();
	Class *namespaceClass = getClass("Namespace");
	if (registry == NULL || namespaceClass == NULL) {
		fail(error, "namespaces need packages/Core loaded", name);
		return closeHandleScope(&scope, NULL);
	}

	String *symbol = asSymbol(name);
	Value existing = symbolDictAt(registry, symbol);
	if (valueTypeOf(existing, VALUE_POINTER) && asObject(existing) != Handles.nil.raw) {
		return closeHandleScope(&scope, scopeHandle(asObject(existing)));
	}

	// A namespace declared inside another one IMPORTS its declarant, so its
	// members see the declarant's classes unqualified. Core is never in imports:
	// it is the fallback by construction (core/Namespace.h), so a top-level
	// declaration imports nothing.
	Array *imports = newArray(declarant == NULL ? 0 : 1);
	if (declarant != NULL) {
		arrayAtPutObject(imports, 0, (Object *) declarant);
	}
	Value arguments[2] = { tagPtr(symbol->raw), tagPtr(imports->raw) };
	_Bool understood = 0;
	Value made = jitSend(objectTagged(namespaceClass), "named:imports:", 2,
		arguments, &understood);
	if (!understood || !valueTypeOf(made, VALUE_POINTER)) {
		fail(error, "Namespace class did not answer named:imports:", name);
		return closeHandleScope(&scope, NULL);
	}
	Namespace *declared = scopeHandle(asObject(made));

	symbolDictAtPutObject(namespaceRegistry(), symbol, (Object *) declared);
	// Bound in the DECLARING namespace and not in core, which is what makes a
	// namespace declared inside a package invisible from outside it.
	namespaceAtPutObject(declarant, symbol, (Object *) declared);
	return closeHandleScope(&scope, declared);
}


// Build the members into the declared namespace and initialize them.
//
// THE INITIALIZE RULE IS THE ONE THE BOOTSTRAP ALREADY USES, and it is not
// optional: only the class that DEFINES `initialize` runs it. Sending it to
// every member would run an inherited one once per subclass, with a different
// `self` each time, which is how Transcript once became a Socket
// (docs/jit-v2/01-gate.md). An extension or a nested declaration is skipped for
// the same reason the image skips it: neither defines a class to initialize.
static void buildMembersOf(ClassNode *node, Namespace *declared,
	ClassBuildError *error)
{
	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *members = classNodeGetMembers(node);
	size_t count = members == NULL ? 0 : ordCollSize(members);
	OrderedCollection *built = newOrdColl(count == 0 ? 1 : count);
	for (size_t i = 0; i < count && error->message == NULL; i++) {
		ClassNode *member = scopeHandle(asObject(ordCollAt(members, i)));
		Class *class = classBuildIn(member, declared, error);
		if (class != NULL && !classNodeIsExtension(member)
				&& !classNodeIsNamespace(member)) {
			ordCollAddObject(built, (Object *) class);
		}
	}
	if (error->message != NULL) {
		closeHandleScope(&scope, NULL);
		return;
	}

	// AFTER every member exists, not as each one is built: an initializer
	// routinely names a sibling declared further down the body, and the forward
	// reference that made it compile is only filled in when that sibling is
	// built.
	String *initialize = asSymbol(stringFromC("initialize"));
	size_t builtCount = ordCollSize(built);
	for (size_t i = 0; i < builtCount; i++) {
		HandleScope each;
		openHandleScope(&each);
		Object *class = ordCollObjectAt(built, i);
		RawClass *metaclass = classOf(objectTagged(class));
		Value methods = metaclass == NULL ? 0 : metaclass->methodDictionary;
		if (valueTypeOf(methods, VALUE_POINTER)
				&& valueTypeOf(symbolDictAt(scopeHandle(asObject(methods)), initialize),
					VALUE_POINTER)) {
			_Bool understood = 0;
			jitSendUnary(objectTagged(class), "initialize", &understood);
		}
		closeHandleScope(&each, NULL);
	}
	closeHandleScope(&scope, NULL);
}


Class *classBuildIn(ClassNode *node, Namespace *namespace,
	ClassBuildError *error)
{
	shapesResolve();
	HandleScope scope;
	openHandleScope(&scope);
	error->message = NULL;
	error->what = NULL;
	error->inMethod = NULL;
	error->status = COMPILE_OK;

	String *name = literalNodeGetStringValue(classNodeGetName(node));

	// `Name := Namespace [ ... ]`: a container of class definitions. It answers
	// NO CLASS, because there is no single one to answer; the image is written
	// for that (PackageLoader>>loadPackage: and Compiler>>compileStream: both
	// skip the answer when `node isNamespace`), and runtime/primitives/Reflect.c
	// turns it into nil.
	if (classNodeIsNamespace(node)) {
		Namespace *declared = declareNamespace(name, namespace, error);
		if (declared == NULL) {
			closeHandleScope(&scope, NULL);
			return NULL;
		}
		buildMembersOf(node, declared, error);
		closeHandleScope(&scope, NULL);
		return NULL;
	}

	// `Name extend [ ... ]` adds methods to a class that must already exist. An
	// extension cannot change a shape by construction, so nothing else happens.
	if (classNodeIsExtension(node)) {
		// Through the CHAIN: `Object extend [ ... ]` from inside a package means
		// Core's Object, and an extension adds methods to a class that already
		// exists rather than declaring one of its own.
		Class *class = resolvedClassNamed(namespace, name);
		if (class == NULL) {
			fail(error, "extending a class that does not exist", name);
			closeHandleScope(&scope, NULL);
			return NULL;
		}
		// EVERY SELECTOR IS CHECKED BEFORE ANY IS INSTALLED, and that ordering is
		// the whole guarantee. An extension is ADDITIVE: it may override an
		// INHERITED selector, which is an ordinary subclass-style override, but
		// it may not quietly replace one the class defines ITSELF. Checking as
		// each method was installed would leave the ones before the collision in
		// place, so a failed extension would half-apply -- and half of an
		// extension is a class nobody wrote.
		checkNoOwnSelectorCollision(class, node, error);
		if (error->message != NULL) {
			closeHandleScope(&scope, NULL);
			return NULL;
		}
		buildMethods(class, node, namespace, error);
		return closeHandleScope(&scope, error->message == NULL ? class : NULL);
	}

	// The superclass. `nil` is the metacircular root and the ONE name accepted
	// in this position that is not a class.
	String *superName = literalNodeGetStringValue(classNodeGetSuperName(node));
	Class *super = NULL;
	if (!stringEqualsC(superName, "nil")) {
		super = resolvedClassNamed(namespace, superName);
		if (super == NULL) {
			// The same kind of failure an undefined name in a method body is,
			// and raised as the same class: the name after `:=` resolves to no
			// class, and the node carries where it was written.
			failAs(error, "UndefinedVariableError",
				"superclass is not defined yet", superName);
			error->identifier = outermostHandle(classNodeGetSuperName(node)->raw);
			closeHandleScope(&scope, NULL);
			return NULL;
		}
	}

	OrderedCollection *variables = ownInstanceVariables(node);
	uint16_t ownCount = (uint16_t) ordCollSize(variables);

	// THE SHAPE IS INHERITED unless the class declares one, which is what makes
	// `Array := ArrayedCollection [ ]` an indexed class and `Symbol := String`
	// a byte class without either of them repeating a pragma its parent already
	// carries. Defaulting to named-slots instead was wrong in exactly those
	// places, and wrong in the expensive direction: the collector would have
	// walked an Array's elements as if they were named fields.
	String *shapeName = shapePragmaOf(node, error);
	if (error->message != NULL) {
		closeHandleScope(&scope, NULL);
		return NULL;
	}
	const ShapeMapping *mapping = NULL;
	if (shapeName == NULL) {
		for (size_t i = 0; i < sizeof(gShapes) / sizeof(gShapes[0]); i++) {
			if (super != NULL && gShapes[i].format == super->raw->instanceShape.format) {
				mapping = &gShapes[i];
				break;
			}
		}
		if (mapping == NULL) {
			mapping = &gShapes[0]; // no superclass: named slots, all tagged
		}
	}
	if (shapeName != NULL) {
		mapping = NULL;
		for (size_t i = 0; i < sizeof(gShapes) / sizeof(gShapes[0]); i++) {
			if (stringEqualsC(shapeName, gShapes[i].pragma)) {
				mapping = &gShapes[i];
				break;
			}
		}
		if (mapping == NULL) {
			// Every shape this does not know belongs to an object the VM itself
			// lays out (a context, a compiled method, an exception handler), and
			// those are decisions for the layer that owns them, not defaults to
			// be guessed here.
			failAs(error, "InvalidPragmaError", "unknown instance shape", shapeName);
			closeHandleScope(&scope, NULL);
			return NULL;
		}
	}
	uint16_t slots = mapping->countsFixedSlots
		? (uint16_t) (inheritedSlots(super) + ownCount) : 0;
	InstanceShape shape = mapping->exact
		? mapping->shape : (InstanceShape) DEFINE_SHAPE(mapping->format, 0, 0, slots);

	// A MIRROR of a C struct has to agree with the struct, field for field, and
	// this is where that is checked rather than hoped for. The Smalltalk side
	// declares the tagged fields in order and the C side lays them out; adding
	// one to either without the other shows up as a method reading the wrong
	// slot, which is a wrong answer and not a crash.
	if (shapeName != NULL && stringEqualsC(shapeName, "ClassShape")) {
		size_t declared = ownCount + declaredVariableCount(super);
		if (declared > CLASS_TAGGED_FIELDS) {
			fail(error, "the class mirror declares more fields than RawClass has",
				name);
			closeHandleScope(&scope, NULL);
			return NULL;
		}
	}

	// REOPENING, which is the common case for the kernel: the built-in classes
	// already exist, and packages/Core defines them again with their real
	// contents. Making a second class object would leave every immediate's class
	// index, and every inline cache that has run, pointing at the first one.
	Class *class = definedClassNamed(namespace, name);
	if (class != NULL) {
		InstanceShape existing = class->raw->instanceShape;
		if (existing.format != shape.format || existing.fixedSlots != shape.fixedSlots) {
			// Silently restamping would leave every instance already alive laid
			// out one way and every instance made afterwards laid out another.
			fail(error, "class already exists with a different shape", name);
			closeHandleScope(&scope, NULL);
			return NULL;
		}
		if (super != NULL) {
			rawObjectStorePtr((RawObject *) class->raw, &class->raw->superClass,
				(RawObject *) super->raw);
		} else {
			// `Object := nil [ ... ]` SAYS its superclass is nil, and the field is
			// read from Smalltalk as an ordinary instance variable. Dropping the
			// declaration left the allocator's zero there, so `Object superClass`
			// answered the SmallInteger 0: not nil, so every `superClass == nil`
			// loop ran one step too far and sent `superClass` to a 0. That is what
			// `3 isKindOf: Fraction` did, and it is the whole isKindOf: protocol.
			rawObjectStorePtr((RawObject *) class->raw, &class->raw->superClass,
				Handles.nil.raw);
		}
	} else {
		class = classCreate(super, (union String *) asSymbol(name), shape);
		// Into the NAMESPACE's own bindings. With no namespace this is the
		// globals dictionary, byte for byte what it used to be.
		namespaceAtPutObject(namespace, name, (Object *) class);
	}
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->instanceVariables,
		(RawObject *) variables->raw);
	// The built-in classes were made during bootstrap, before there was a nil to
	// put in them, and this is the pass that reaches every one of them.
	classFillAbsentSmalltalkFields(class);
	recordSubclass(super, class);
	// THE HOME NAMESPACE, which nothing wrote until now: every class in the
	// system answered nil there, so `Class>>qualifiedName` answered the plain
	// name for a package class as readily as for a kernel one, and the only
	// thing that reads the field could never tell the two apart.
	//
	// Core stays NIL rather than holding the Core namespace, because that is
	// what qualifiedName already treats as "no prefix" and what every class made
	// before namespaces existed already has. Two spellings of the same answer,
	// and the one already in the image is the one to keep.
	if (namespace != NULL) {
		rawObjectStorePtr((RawObject *) class->raw, &class->raw->namespace,
			(RawObject *) namespace->raw);
	}
	declareClassVariables(class, node);
	methodsOf(class);
	classMetaclassOf(class); // established here so the chain is never partial

	buildMethods(class, node, namespace, error);
	return closeHandleScope(&scope, error->message == NULL ? class : NULL);
}
