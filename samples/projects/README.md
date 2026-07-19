# Project samples

Runnable example PROJECTS for the package system (`docs/packages.md`,
`docs/namespaces.md`). Unlike the script samples one directory up (which run
with `st -f` against the dev image), these are built and run by the project
tooling itself.

From the repository root, with a bootstrapped `./snapshot`:

```sh
export LD_LIBRARY_PATH=build
cd samples/projects/hello
ST_IMAGE=../../../snapshot ../../../build/st run alpha beta
ST_IMAGE=../../../snapshot ../../../build/st test
```

(`ST_IMAGE` points at the base image; an installed `st` with a `snapshot`
beside it needs no variable at all.)

## hello/

The smallest application project: exactly what `st new hello` scaffolds. One
manifest, one class with the `main:` entry point, one test file.

## namespaces/

ONE project, several namespaces, declared straight in source with the
`Name := Namespace [ classDefs ]` form:

```
src/Greetings.st   Greetings := Namespace [ Greeter := Object [...] ]
src/Numbers.st     Numbers := Namespace [ Stats := Object [...] ]
src/Text.st        Text := Namespace [ String extend [ shout ] ]
src/Banner.st      Banner := Namespace [ Formatter := Object [...] ]
src/Plain.st       Plain := Namespace [ Formatter := Object [...] ]  (same name!)
src/Main.st        uses every module, then LISTS them reflectively
```

The declared namespace object is bound as a global where it was declared, so
`(Banner at: #Formatter) format: 'x'` just works; Banner's and Plain's
`Formatter` are fully independent classes; the Text module only contributes
a core-class extension. `Main` closes the loop by walking the `Namespaces`
registry and printing every non-core namespace with its classes, their
superclasses and their sorted class-side and instance-side selectors: the
whole module structure is ordinary reflective data:

```
namespace Banner
  Formatter (superclass: Object)
    class format:
namespace Greetings
  Greeter (superclass: Object)
    class greet:
namespace NsShowcase
  Main (superclass: Object)
    class listClass:
    ...
```

```sh
cd samples/projects/namespaces
ST_IMAGE=../../../snapshot ../../../build/st run
ST_IMAGE=../../../snapshot ../../../build/st test
```

## store/

The realistic one: a coffee store's business day as a LAYERED application,
five namespaces whose instances flow across every boundary:

```
Domain    Money value object (+ - * percent: printOn:), Product, Customer,
          Order/OrderLine, and an Integer extension so 1990 cents is Money
Storage   Repository with block queries, Inventory whose reserve: raises a
          custom OutOfStockError (an Error subclass with its own state)
Events    a publish/subscribe bus whose handlers are plain blocks
Billing   TaxPolicy hierarchy (rate is subclassResponsibility: food 0%,
          standard 10%, luxury 25%), InvoiceBuilder taxing Domain orders
          line by line
Reports   revenue-by-category and best-seller aggregation, padded tables
Main      wires the day together and cross-checks the revenue with three
          fibers over a Channel
```

Things to watch for while reading it: classes are fetched from sibling
namespaces as objects (`(Domain at: #Product) sku: ...`), the exception
HANDLER class is fetched the same way (`on: (Storage at: #OutOfStockError)
do: ...`), one order is rejected mid-day and turns into an event instead of
a crash, and the same business total is computed three ways (orders,
invoices minus tax, parallel fibers) and agrees.

```sh
cd samples/projects/store
ST_IMAGE=../../../snapshot ../../../build/st run
ST_IMAGE=../../../snapshot ../../../build/st test
```

## modules/

The same five modules as PACKAGES: a workspace where each module is its own
package with a manifest, wired into the app with `requires:path:`. This is
the shape to reach for when a module has its own dependencies, version or
tests, or is shared between projects. It also demonstrates what declared
namespaces do not have: import-based UNQUALIFIED resolution (`Formatter`
resolves through the app's requires, first import wins on the shadowed
name), while the shadowed class stays reachable reflectively.

```sh
cd samples/projects/modules/app
ST_IMAGE=../../../../snapshot ../../../../build/st run
ST_IMAGE=../../../../snapshot ../../../../build/st test
```
