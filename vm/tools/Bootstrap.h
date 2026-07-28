#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

// The BUILT-IN kernel: the smallest set of classes and methods that lets the VM
// run a program at all, built in C.
//
// It exists because of an ordering problem. The real kernel is packages/Core,
// which is Smalltalk source, and reading Smalltalk source needs a parser, a
// compiler, a heap, classes to compile against and primitives to send to. So
// SOMETHING has to exist before any image does, and this is that something: the
// class-of-classes, the classes the parser and the compiler instantiate, the
// immediates' classes, and the primitives that are already implemented.
//
// It is deliberately small, and it is NOT a subset of packages/Core that has to
// stay in step with it. It is scaffolding: what it defines are the things that
// could not have come from anywhere else.
//
// PENDING: loading packages/ is not here. That needs the class builder (a
// ClassNode becomes a real class with a shape, instance variables and compiled
// methods), which is the next milestone after this one.

void bootstrapBuiltinKernel(void);

#endif
