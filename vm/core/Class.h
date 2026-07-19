#ifndef CLASS_H
#define CLASS_H

#include "core/Object.h"
#include "core/Thread.h"
#include "compiler/Parser.h"
#include "runtime/Collection.h"
#include "runtime/Dictionary.h"

union CompiledMethod;

Object *buildClass(ClassNode *node);
// Build into `ns` (NULL = DefaultNamespace): superclass and shape names
// resolve through the namespace chain, methods compile with it, and the
// class installs into the namespace's own bindings.
Object *buildClassIn(ClassNode *node, Namespace *ns);
// Remove a selector from a Class/MetaClass's own method dictionary under a
// full send-cache invalidation; 0 when the selector is not defined there.
_Bool classRemoveSelector(Object *holder, String *selector);
union CompiledMethod *lookupSelector(Class *startClass, String *selector);
void printClassName(RawClass *class);
static RawClass *getClassOf(Value value);

static void classSetMetaClass(Class *class, MetaClass *metaClass);
static MetaClass *classGetMetaClass(Class *class);
static void classSetSuperClass(Class *class, Class *superClass);
static Class *classGetSuperClass(Class *class);
static void classSetSubClasses(Class *class, OrderedCollection *subClasses);
static OrderedCollection *classGetSubClasses(Class *class);
static void classSetMethodDictionary(Class *class, Dictionary *dictionary);
static Dictionary *classGetMethodDictionary(Class *class);
static void classSetInstanceShape(Class *class, InstanceShape shape);
static InstanceShape classGetInstanceShape(Class *class);
static void classSetInstanceVariables(Class *class, Array *instVars);
static Array *classGetInstanceVariables(Class *class);
static void classSetName(Class *class, String *name);
static String *classGetName(Class *class);
static void classSetComment(Class *class, String *comment);
static String *classGetComment(Class *class);
static void classSetCategory(Class *class, String *category);
static String *classGetCategory(Class *class);
static void classSetClassVariables(Class *class, Dictionary *classVars);
static Dictionary *classGetClassVariables(Class *class);

static void metaClassSetInstanceClass(MetaClass *class, Class *instanceClass);
static Class *metaClassGetInstanceClass(MetaClass *class);
static void metaClassSetSuperClass(MetaClass *class, MetaClass *superClass);
static MetaClass *metaClassGetSuperClass(MetaClass *class);
static void metaClassSetSubClasses(MetaClass *class, OrderedCollection *subClasses);
static OrderedCollection *metaClassGetSubClasses(MetaClass *class);
static void metaClassSetInstanceShape(MetaClass *class, InstanceShape shape);
static InstanceShape metaClassGetInstanceShape(MetaClass *class);
static void metaClassSetInstanceVariables(MetaClass *class, Array *instVars);
static Array *metaClassGetInstanceVariables(MetaClass *class);
static void metaClassSetMethodDictionary(MetaClass *class, Dictionary *dictionary);
static Dictionary *metaClassGetMethodDictionary(MetaClass *class);


static RawClass *getClassOf(Value value)
{
	switch (value & 3) {
	case VALUE_INT:
		return Handles.SmallInteger->raw;
	case VALUE_CHAR:
		return Handles.Character->raw;
	case VALUE_FLOAT:
		return Handles.SmallFloat64->raw;
	case VALUE_POINTER:
		return asObject(value)->class;
	default:
		return NULL;
	}
}


static void classSetMetaClass(Class *class, MetaClass *metaClass)
{
	class->raw->class = (RawClass *) metaClass->raw;
}


static MetaClass *classGetMetaClass(Class *class)
{
	return (MetaClass *) scopeHandle(class->raw->class);
}


static void classSetSuperClass(Class *class, Class *superClass)
{
	objectStorePtr((Object *) class,  &class->raw->superClass, (Object *) superClass);
}


static Class *classGetSuperClass(Class *class)
{
	return (Class *) scopeHandle(asObject(class->raw->superClass));
}


static void classSetSubClasses(Class *class, OrderedCollection *subClasses)
{
	objectStorePtr((Object *) class,  &class->raw->subClasses, (Object *) subClasses);
}


static OrderedCollection *classGetSubClasses(Class *class)
{
	return (OrderedCollection *) scopeHandle(asObject(class->raw->subClasses));
}


static void classSetMethodDictionary(Class *class, Dictionary *dictionary)
{
	objectStorePtr((Object *) class,  &class->raw->methodDictionary, (Object *) dictionary);
}


static Dictionary *classGetMethodDictionary(Class *class)
{
	return (Dictionary *) scopeHandle(asObject(class->raw->methodDictionary));
}


static void classSetInstanceShape(Class *class, InstanceShape shape)
{
	class->raw->instanceShape = shape;
}


static InstanceShape classGetInstanceShape(Class *class)
{
	return class->raw->instanceShape;
}


static void classSetInstanceVariables(Class *class, Array *instVars)
{
	objectStorePtr((Object *) class,  &class->raw->instanceVariables, (Object *) instVars);
}


static Array *classGetInstanceVariables(Class *class)
{
	return (Array *) scopeHandle(asObject(class->raw->instanceVariables));
}


static void classSetName(Class *class, String *name)
{
	objectStorePtr((Object *) class,  &class->raw->name, (Object *) name);
}


static String *classGetName(Class *class)
{
	return (String *) scopeHandle(asObject(class->raw->name));
}


static void classSetComment(Class *class, String *comment)
{
	objectStorePtr((Object *) class,  &class->raw->comment, (Object *) comment);
}


static String *classGetComment(Class *class)
{
	return (String *) scopeHandle(asObject(class->raw->comment));
}


static void classSetCategory(Class *class, String *category)
{
	objectStorePtr((Object *) class,  &class->raw->category, (Object *) category);
}


static String *classGetCategory(Class *class)
{
	return (String *) scopeHandle(asObject(class->raw->category));
}


static void classSetClassVariables(Class *class, Dictionary *classVars)
{
	objectStorePtr((Object *) class,  &class->raw->classVariables, (Object *) classVars);
}


static Dictionary *classGetClassVariables(Class *class)
{
	return (Dictionary *) scopeHandle(asObject(class->raw->classVariables));
}


static void classSetNamespace(Class *class, Namespace *ns)
{
	objectStorePtr((Object *) class,  &class->raw->namespace, (Object *) ns);
}


static Namespace *classGetNamespace(Class *class)
{
	return (Namespace *) scopeHandle(asObject(class->raw->namespace));
}


static void metaClassSetInstanceClass(MetaClass *class, Class *instanceClass)
{
	objectStorePtr((Object *) class,  &class->raw->instanceClass, (Object *) instanceClass);
}


static Class *metaClassGetInstanceClass(MetaClass *class)
{
	return scopeHandle(asObject(class->raw->instanceClass));
}


static void metaClassSetSuperClass(MetaClass *class, MetaClass *superClass)
{
	objectStorePtr((Object *) class,  &class->raw->superClass, (Object *) superClass);
}


static MetaClass *metaClassGetSuperClass(MetaClass *class)
{
	return scopeHandle(asObject(class->raw->superClass));
}


static void metaClassSetSubClasses(MetaClass *class, OrderedCollection *subClasses)
{
	objectStorePtr((Object *) class,  &class->raw->subClasses, (Object *) subClasses);
}


static OrderedCollection *metaClassGetSubClasses(MetaClass *class)
{
	return scopeHandle(asObject(class->raw->subClasses));
}


static void metaClassSetInstanceShape(MetaClass *class, InstanceShape shape)
{
	class->raw->instanceShape = shape;
}


static InstanceShape metaClassGetInstanceShape(MetaClass *class)
{
	return class->raw->instanceShape;
}


static void metaClassSetInstanceVariables(MetaClass *class, Array *instVars)
{
	objectStorePtr((Object *) class,  &class->raw->instanceVariables, (Object *) instVars);
}


static Array *metaClassGetInstanceVariables(MetaClass *class)
{
	return scopeHandle(asObject(class->raw->instanceVariables));
}


static void metaClassSetMethodDictionary(MetaClass *class, Dictionary *dictionary)
{
	objectStorePtr((Object *) class,  &class->raw->methodDictionary, (Object *) dictionary);
}


static Dictionary *metaClassGetMethodDictionary(MetaClass *class)
{
	return scopeHandle(asObject(class->raw->methodDictionary));
}

#endif
