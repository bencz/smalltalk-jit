// Streams: the bytes actually leaving the process.
//
// These are the bottom of the kernel's IO stack. `Transcript` is an
// ExternalStream on descriptor 1, `printNl` fills its buffer, and the buffer
// arrives here. Everything above them is Smalltalk.
//
// They are CLASS-SIDE methods, so the receiver is the class and the descriptor
// is an ordinary argument. None of them allocates.
//
// A short write is not an error and not silently accepted either: the count
// WRITTEN is answered, and the Smalltalk side loops. That is the contract a
// caller can act on; answering "done" after a partial write is how data goes
// missing without anybody noticing.

#include "runtime/primitives/Shared.h"
#include "os/OsFile.h"


static _Bool streamDescriptorOf(Value value, int *descriptor)
{
	if (!valueTypeOf(value, VALUE_INT)) {
		return 0;
	}
	intptr_t number = asCInt(value);
	if (number < 0 || number > 65535) {
		return 0;
	}
	*descriptor = (int) number;
	return 1;
}


Value primStreamWrite(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	Value count = primitiveArgument(args, 1);
	Value bytes = primitiveArgument(args, 2);
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)
			|| !valueTypeOf(count, VALUE_INT) || !valueTypeOf(bytes, VALUE_POINTER)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(bytes);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	intptr_t wanted = asCInt(count);
	if (wanted < 0 || (size_t) wanted > rawObjectElementCount(object)) {
		return PRIMITIVE_FAILED; // out of range fails; the fallback signals
	}
	int64_t written = osFileWrite(descriptor, rawObjectBytes(object),
		(size_t) wanted);
	if (written < 0) {
		return PRIMITIVE_FAILED; // the fallback reads the errno through IoError
	}
	return tagInt((intptr_t) written);
}


Value primStreamRead(Value *args, uint64_t argc)
{
	if (argc != 4) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	Value count = primitiveArgument(args, 1);
	Value into = primitiveArgument(args, 2);
	Value start = primitiveArgument(args, 3);
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)
			|| !valueTypeOf(count, VALUE_INT) || !valueTypeOf(into, VALUE_POINTER)
			|| !valueTypeOf(start, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(into);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	// One-based, like every index in Smalltalk.
	intptr_t from = asCInt(start) - 1;
	intptr_t wanted = asCInt(count);
	size_t size = rawObjectElementCount(object);
	if (from < 0 || wanted < 0 || (size_t) from + (size_t) wanted > size) {
		return PRIMITIVE_FAILED;
	}
	int64_t got = osFileRead(descriptor, rawObjectBytes(object) + from,
		(size_t) wanted);
	if (got < 0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) got);
}


Value primStreamClose(Value *args, uint64_t argc)
{
	int descriptor;
	if (argc != 1 || !streamDescriptorOf(primitiveArgument(args, 0), &descriptor)) {
		return PRIMITIVE_FAILED;
	}
	return osFileClose(descriptor) ? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


Value primStreamFlush(Value *args, uint64_t argc)
{
	int descriptor;
	if (argc != 1 || !streamDescriptorOf(primitiveArgument(args, 0), &descriptor)) {
		return PRIMITIVE_FAILED;
	}
	// A NO-OP that answers the receiver, and deliberately so. Nothing is buffered
	// on the C side; the buffering that exists is the kernel's own, in Smalltalk,
	// and it has already been written out by the time this is sent.
	//
	// Calling through to the OS was the obvious version and it was wrong:
	// fsync on a terminal or a pipe answers EINVAL, so `Transcript flush` failed,
	// the Smalltalk fallback raised an IoError, and reporting THAT needed the
	// Transcript. Measured as an infinite recursion on `last`.
	(void) descriptor;
	return primitiveReceiver(args);
}
