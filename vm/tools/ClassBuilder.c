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
} ShapeMapping;

static const ShapeMapping gShapes[] = {
	{ "FixedShape", FORMAT_POINTERS, 1 },
	{ "IndexedShape", FORMAT_INDEXED_POINTERS, 1 },
	{ "BytesShape", FORMAT_BYTES, 0 },
	{ "StringShape", FORMAT_BYTES, 0 },
	{ "FloatShape", FORMAT_NO_POINTERS, 1 },
};


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
static OrderedCollection *ownInstanceVariables(ClassNode *node)
{
	OrderedCollection *declared = classNodeGetVars(node);
	size_t count = declared == NULL ? 0 : ordCollSize(declared);
	OrderedCollection *names = newOrdColl(count == 0 ? 1 : count);
	for (size_t i = 0; i < count; i++) {
		LiteralNode *variable = scopeHandle(asObject(ordCollAt(declared, i)));
		ordCollAdd(names, objectTagged(asSymbol(literalNodeGetStringValue(variable))));
	}
	return names;
}


static uint16_t inheritedSlots(Class *super)
{
	return super == NULL ? 0 : (uint16_t) super->raw->instanceShape.fixedSlots;
}


Class *classBuild(ClassNode *node, ClassBuildError *error)
{
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

	// The shape. No pragma means the ordinary case: named slots, all tagged.
	String *shapeName = shapePragmaOf(node);
	const ShapeMapping *mapping = &gShapes[0];
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
	InstanceShape shape = (InstanceShape)
		DEFINE_SHAPE(mapping->format, 0, 0, slots);

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
	methodsOf(class);

	buildMethods(class, node, error);
	return closeHandleScope(&scope, error->message == NULL ? class : NULL);
}
