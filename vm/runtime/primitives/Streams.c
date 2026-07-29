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
#include "runtime/String.h"
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


// ---------------------------------------------------------------------------
// Position, readiness, and the last error
// ---------------------------------------------------------------------------

// ExternalStream class>>position: descriptor
Value primStreamGetPosition(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)) {
		return PRIMITIVE_FAILED;
	}
	int64_t position = osFileGetPosition((OsFd) descriptor);
	if (position < 0) {
		return PRIMITIVE_FAILED; // a pipe or a terminal: the fallback signals
	}
	return tagInt((intptr_t) position);
}


// ExternalStream class>>position: descriptor to: anInteger
Value primStreamSetPosition(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	Value position = primitiveArgument(args, 1);
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)
			|| !valueTypeOf(position, VALUE_INT) || asCInt(position) < 0) {
		return PRIMITIVE_FAILED;
	}
	if (!osFileSetPosition((OsFd) descriptor, (int64_t) asCInt(position))) {
		return PRIMITIVE_FAILED;
	}
	return primitiveReceiver(args);
}


// ExternalStream class>>available: descriptor
//
// HOW MANY BYTES CAN BE READ WITHOUT BLOCKING, which is not the same question
// as how long the file is: on a pipe or a socket it is what has arrived, and on
// a regular file it is what is left from here to the end.
Value primStreamAvailable(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	int descriptor;
	if (!streamDescriptorOf(primitiveArgument(args, 0), &descriptor)) {
		return PRIMITIVE_FAILED;
	}
	int64_t available = osFileAvailable((OsFd) descriptor);
	if (available < 0) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) available);
}


// IoError class>>last
//
// THE LAST OS ERROR AS AN EXCEPTION, carrying the system's own words. Every
// stream and socket primitive in this VM fails silently and lets the kernel
// signal `IoError last`, so this is the one place the errno actually becomes
// readable -- and without it every I/O failure in the system reported the same
// sentence, 'input/output error', whatever had gone wrong.
//
// It builds the exception rather than answering a String because that is what
// the kernel's fallback does, and the two have to be interchangeable: a caller
// writes `IoError last signal` and cannot be made to care which one answered.
Value primLastIoError(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	Class *ioError = receiverAsClass(primitiveReceiver(args));
	if (ioError == NULL) {
		return PRIMITIVE_FAILED;
	}
	// READ THE ERROR FIRST, before anything can allocate: allocating runs the
	// collector, and a collection is entitled to make syscalls of its own.
	char text[256];
	osErrorMessage(osLastError(), text, sizeof text);

	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Class *held = scopeHandle((RawObject *) ioError->raw);
	Object *error = newObject(held, 0);
	// messageText is the FIRST instance variable of every exception in this
	// kernel: Exception.st declares `| messageText |` and nothing above it
	// does. Through the barrier, because the String is young and the error may
	// already have been promoted.
	rawObjectStorePtr((RawObject *) error->raw, (Value *) error->raw->body,
		(RawObject *) stringFromC(text)->raw);
	Value answer = objectTagged(error);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
