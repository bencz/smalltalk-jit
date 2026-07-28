// The process the VM is running in: its environment, and what it was told.
//
// Everything here goes through the OS seam (os/Os.h); nothing calls libc
// directly, for the same reason the filesystem domain does not.

#include "runtime/primitives/Shared.h"
#include "runtime/String.h"
#include "os/Os.h"

// Longer than any environment value worth putting in one, and the same bound
// the project tooling uses for a path list.
#define ENVIRONMENT_VALUE_MAX 8192


// System class primitiveGetEnv: aString
//
// FAILS when the variable is unset, and the kernel's `^nil` fallback is the
// answer for that. Failing rather than answering nil keeps "unset" and "set to
// the empty string" distinguishable, which is what `System env:ifAbsent:` is
// written to tell apart.
Value primGetEnv(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value nameValue = primitiveArgument(args, 0);
	if (!valueTypeOf(nameValue, VALUE_POINTER)
			|| rawObjectFormat(asObject(nameValue)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	RawObject *nameObject = asObject(nameValue);
	size_t nameLength = rawObjectElementCount(nameObject);
	char name[256];
	if (nameLength + 1 > sizeof name) {
		return PRIMITIVE_FAILED;
	}
	memcpy(name, rawObjectBytes(nameObject), nameLength);
	name[nameLength] = '\0';

	// Read into C storage BEFORE allocating: building the answer String can move
	// the name argument, and getenv's own memory is not stable across a setenv.
	char value[ENVIRONMENT_VALUE_MAX];
	if (!osEnvironmentValue(name, value, sizeof value)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(value));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
