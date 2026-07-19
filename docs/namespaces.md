# Namespaces

Every package loads into its own namespace: a name universe holding the
bindings of the classes and globals the package defines. The kernel lives in
the `Core` namespace, whose bindings ARE the `Smalltalk` system dictionary
(same object identity), so all pre-namespace reflection keeps working.

## Resolution

When a method body references an uppercase name, the compiler resolves it
through the ACTIVE namespace's chain:

1. the namespace's own bindings;
2. the namespaces of the package's DIRECT requirements, in manifest order
   (first import wins on a name both export);
3. Core, implicitly, always last.

Transitive dependencies are NOT visible unless required directly, which
keeps dependency graphs honest. Bindings are the classic Smalltalk-80
Association model: a compiled reference points at the binding, so defining a
class later fills earlier references in place.

A forward reference (a name used before its class appears later in the same
package) auto-creates a nil binding in the ACTIVE namespace. Because that
only happens after the whole chain missed, a typo can never shadow a core or
imported name. At the end of a package load, bindings that never received a
value fail the load with the list of names (`allowUnresolved:` in the
manifest exempts legitimately late-bound globals).

Same name, different namespaces = fully independent classes. A package may
even shadow a core name for its own compiles; core itself is untouched.

## Declaring namespaces in source

A namespace does not need a package of its own: any source file can declare
one (or several) with the class-definition shape:

```smalltalk
Banner := Namespace [

	Formatter := Object [
		class format: aString [ ^'== ', aString, ' ==' ]
	]

	String extend [ bannered [ ^'** ', self ] ]

]
```

- The body holds class definitions, extensions, or further declarations;
  they all build INTO the declared namespace.
- The declared namespace imports the DECLARING one (its members see the
  surrounding project's names unqualified, plus core as always).
- The namespace OBJECT binds as a global where it was declared, so members
  are reached through it: `(Banner at: #Formatter) format: 'x'`.
- Declaring an existing name REOPENS the namespace: several files can grow
  one namespace, and members redefine on reload like any class.
- Member class-side `initialize` runs when the declaration completes.
- Like the `nil` superclass of the metacircular roots, the name `Namespace`
  is recognized only in the superclass position of this form.

Packages remain the LOAD unit (dependencies, versions, testFiles); declared
namespaces are how one project organizes itself internally. Sibling declared
namespaces do not import each other: access across them goes through the
namespace objects, which is explicit and reads well.

There is no qualified-name syntax in source code (the `.` is the statement
separator; `::` is reserved for a possible future). Cross-namespace access
beyond imports is reflective:

```smalltalk
(Namespaces at: #'Std.Http') at: #HttpServer
Namespace default          "the session's compile namespace"
someClass namespace        "a class's home"
someClass qualifiedName    "'Std.Http.HttpServer' outside Core"
```

`Namespace default` is what eval, the REPL and `-f` scripts compile against:
Core in a plain session, the project's namespace inside a project image
(which is how `st repl` sees your classes unqualified).

## Class extension

A package can add methods to a class it can see (core or imported):

```smalltalk
String extend [

	shout [ ^self asUppercase, '!' ]

	class fromShout: aString [ ^aString copyFrom: 1 to: aString size - 1 ]

]
```

The same form works at the top level of any `-f` script. Rules:

- The extension's method bodies resolve THEIR globals in the extending
  package's namespace; the target class's instance variables stay visible.
- No pragmas and no variable section: an extension cannot change the
  target's shape.
- A selector already present in the target's OWN dictionary is a
  `RedefinitionError` and the whole extension aborts, nothing installs (two
  packages extending one class with one selector is deterministic).
  Overriding an INHERITED selector is allowed, as in any Smalltalk.
- `SomeClass removeSelector: #sel` removes from the own dictionary (dev
  reload).
- Extensions never re-run the target's class-side `initialize`.

Extension, class redefinition and `removeSelector:` all commit under a
stop-the-world pause that swaps the method dictionaries, resets every JIT
inline-cache cell and flushes the per-worker lookup caches, so the very next
send anywhere observes the change (`System flushSendCaches` is the manual
escape hatch for raw reflective dictionary surgery).

## Loader contract (for tooling authors)

- Dependencies are fully loaded before a dependent package compiles.
- Package loading runs single-mutator (build happens pre-scheduler).
- The unresolved-names scan runs after the package's class initializers.
- `Namespace named:imports:` creates, `Namespaces at:put:` registers; the
  loader owns both steps plus the import wiring.
