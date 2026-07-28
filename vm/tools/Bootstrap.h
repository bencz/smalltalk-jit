#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include <stddef.h>

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
void bootstrapBuiltinKernel(void);

// Load a package directory on top of the built-in kernel: read its manifest,
// then build every file it lists, in the order it lists them.
//
// The manifest is read with a MINIMAL SCANNER and not with the DSL it is written
// in, for the same ordering reason the built-in kernel exists: evaluating
// `PackageSpec new name: 'Core'; ...` needs a PackageSpec class, which is in the
// package being loaded.
//
// Answers how far it got. A failure names the file, the class and what was
// wrong, because the whole value of running this over packages/Core today is
// finding out precisely what is still missing.
typedef struct {
	size_t filesRead;
	size_t classesBuilt;
	size_t methodsBuilt;
	size_t classesFailed;
	// A load KEEPS GOING past a class it cannot build, because the question this
	// answers today is "what is still missing", and the first answer is worth far
	// less than all of them. Each failure is printed as it happens; this is the
	// tally.
	const char *error;    // the FIRST failure, NULL when everything loaded
	char errorFile[512];
	char errorDetail[256];
} BootstrapReport;

void bootstrapLoadPackage(const char *directory, BootstrapReport *report);

#endif
