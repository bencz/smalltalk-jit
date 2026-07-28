#include "tools/ClassBuilder.h"
#include "core/Assert.h"
#include "core/ClassTable.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "core/Thread.h"
#include "jit/CompiledMethod.h"
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
static Class *definedClassNamed(String *name)
{
	Object *found = globalObjectAt(name);
	if (found == NULL || found->raw == Handles.nil.raw) {
		return NULL;
	}
	return (Class *) found;
}


static void fail(ClassBuildError *error, const char *message, String *what)
{
	if (error->message == NULL) {
		error->message = message;
		error->what = what;
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
// The metaclass is created on first use rather than for every class, because
// most classes have no class-side methods and a metaclass each would double the
// class table for nothing.

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
	// class-of-classes, where the class-side methods every class has (new, new:)
	// already live.
	Value super = held->raw->superClass;
	Class *superMeta = valueTypeOf(super, VALUE_POINTER)
		? classMetaclassOf(scopeHandle(asObject(super)))
		: &Handles.ClassClass;

	String *name = NULL;
	if (valueTypeOf(held->raw->name, VALUE_POINTER)) {
		String *own = scopeHandle(asObject(held->raw->name));
		char buffer[256];
		size_t size = rawStringSize(own->raw);
		if (size > sizeof(buffer) - sizeof(" class")) {
			size = sizeof(buffer) - sizeof(" class");
		}
		memcpy(buffer, own->raw->contents, size);
		memcpy(buffer + size, " class", sizeof(" class"));
		name = asSymbol(stringFromC(buffer));
	}

	Class *meta = classCreate(superMeta, (union String *) name, CLASS_OF_CLASSES_SHAPE);
	methodsOf(meta);
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
static String *shapePragmaOf(ClassNode *node)
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
			continue;
		}
		LiteralNode *named = scopeHandle(asObject(ordCollAt(arguments, 0)));
		return literalNodeGetStringValue(named);
	}
	return NULL;
}


// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static void buildMethods(Class *class, ClassNode *node, ClassBuildError *error)
{
	OrderedCollection *methods = classNodeGetMethods(node);
	size_t count = methods == NULL ? 0 : ordCollSize(methods);
	for (size_t i = 0; i < count && error->message == NULL; i++) {
		HandleScope scope;
		openHandleScope(&scope);
		MethodNode *methodNode = scopeHandle(asObject(ordCollAt(methods, i)));

		// `class foo [ ... ]` goes on the metaclass. The parser records the word
		// that preceded the pattern, so this is where it is read back.
		String *side = methodNodeGetClassName(methodNode);
		_Bool classSide = side != NULL && stringEqualsC(side, "class");
		Class *target = classSide ? classMetaclassOf(class) : class;

		CompileContext context = { target, smalltalkGlobals() };
		CompileError compileError;
		CodeUnit *unit = compileMethod(methodNode, &context, &compileError);
		if (unit == NULL) {
			// BOTH names: what failed and where. The selector alone sends a
			// reader looking through a file for a name the message never gave.
			error->status = compileError.status;
			error->inMethod = methodNodeGetSelector(methodNode);
			fail(error, compileStatusName(compileError.status), compileError.what);
			closeHandleScope(&scope, NULL);
			return;
		}
		String *selector = scopeHandle(asObject(unit->selector));
		CompiledMethod *method = compiledMethodCreate(unit, selector, target);
		symbolDictAtPutObject(methodsOf(target), selector, (Object *) method);
		closeHandleScope(&scope, NULL);
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
		Value super = level->raw->superClass;
		level = valueTypeOf(super, VALUE_POINTER) ? scopeHandle(asObject(super)) : NULL;
	}
	return count;
}


static uint16_t inheritedSlots(Class *super)
{
	return super == NULL ? 0 : (uint16_t) super->raw->instanceShape.fixedSlots;
}


Class *classBuild(ClassNode *node, ClassBuildError *error)
{
	shapesResolve();
	HandleScope scope;
	openHandleScope(&scope);
	error->message = NULL;
	error->what = NULL;
	error->inMethod = NULL;
	error->status = COMPILE_OK;

	String *name = literalNodeGetStringValue(classNodeGetName(node));

	// A NAMESPACE declaration is a container of class definitions. The container
	// itself is not built yet, so its members are built into the system
	// dictionary; that is wrong for a namespace and right for getting the
	// classes to exist, and it is called out here rather than hidden.
	if (classNodeIsNamespace(node)) {
		OrderedCollection *members = classNodeGetMembers(node);
		size_t count = members == NULL ? 0 : ordCollSize(members);
		for (size_t i = 0; i < count && error->message == NULL; i++) {
			ClassNode *member = scopeHandle(asObject(ordCollAt(members, i)));
			classBuild(member, error);
		}
		closeHandleScope(&scope, NULL);
		return NULL;
	}

	// `Name extend [ ... ]` adds methods to a class that must already exist. An
	// extension cannot change a shape by construction, so nothing else happens.
	if (classNodeIsExtension(node)) {
		Class *class = definedClassNamed(name);
		if (class == NULL) {
			fail(error, "extending a class that does not exist", name);
			closeHandleScope(&scope, NULL);
			return NULL;
		}
		buildMethods(class, node, error);
		return closeHandleScope(&scope, error->message == NULL ? class : NULL);
	}

	// The superclass. `nil` is the metacircular root and the ONE name accepted
	// in this position that is not a class.
	String *superName = literalNodeGetStringValue(classNodeGetSuperName(node));
	Class *super = NULL;
	if (!stringEqualsC(superName, "nil")) {
		super = definedClassNamed(superName);
		if (super == NULL) {
			fail(error, "superclass is not defined yet", superName);
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
	String *shapeName = shapePragmaOf(node);
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
			fail(error, "unknown instance shape", shapeName);
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
	Class *class = definedClassNamed(name);
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
		}
	} else {
		class = classCreate(super, (union String *) asSymbol(name), shape);
		globalAtPut(name, objectTagged(class));
	}
	rawObjectStorePtr((RawObject *) class->raw, &class->raw->instanceVariables,
		(RawObject *) variables->raw);
	declareClassVariables(class, node);
	methodsOf(class);

	buildMethods(class, node, error);
	return closeHandleScope(&scope, error->message == NULL ? class : NULL);
}
