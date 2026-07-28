// Showing a value, before there is a kernel that can.
//
// The ONE primitive the kernel does not name (runtime/Primitives.def says so at
// its line). The VM's built-in kernel has no streams, no printOn: and no
// Transcript, so without this it can run a program and have no way to say what
// came out. It is deliberately NOT wired into packages/Core: that kernel's
// printNl goes through a buffered stream, and a C primitive writing straight to
// the descriptor would interleave with the buffer.
//
// It answers the RECEIVER, like printNl does, so it composes in a cascade.

#include "runtime/primitives/Shared.h"
#include "runtime/String.h"
#include <stdio.h>
#include <string.h>


Value primPrintValue(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Value receiver = primitiveReceiver(args);
	if (valueTypeOf(receiver, VALUE_POINTER)) {
		RawObject *object = asObject(receiver);
		if (object == Handles.nil.raw) {
			printf("nil\n");
			return receiver;
		}
		if (object == Handles.true_.raw || object == Handles.false_.raw) {
			printf("%s\n", object == Handles.true_.raw ? "true" : "false");
			return receiver;
		}
		// A String or a Symbol prints its characters. Anything else has no
		// printOn: to reach from C, so it says what it IS rather than guessing:
		// the built-in kernel is minimal on purpose and this is where that shows.
		if (Handles.String.raw != NULL
				&& (rawObjectClassIndex(object) == classIndexOf(&Handles.String)
					|| rawObjectClassIndex(object) == classIndexOf(&Handles.Symbol))) {
			printRawString((RawString *) object);
			printf("\n");
			return receiver;
		}
		printf("a <class %u>\n", rawObjectClassIndex(object));
		return receiver;
	}
	if (valueTypeOf(receiver, VALUE_FLOAT)) {
		// A float PRINTS AS A FLOAT: `7.0`, not `7`. %g drops a trailing `.0`,
		// and a number that answers `7` to printNl and `false` to `= 7` would be
		// a small mystery every time it happened.
		char text[32];
		snprintf(text, sizeof text, "%g", floatValueOf(receiver));
		printf("%s%s\n", text, strpbrk(text, ".eni") == NULL ? ".0" : "");
		return receiver;
	}
	printValue(receiver);
	printf("\n");
	return receiver;
}
