// Sockets: TCP/IPv4, and the reason they are not just syscalls.
//
// EVERY ONE OF THESE CAN PARK THE FIBER. os/OsSocket.h states the split and it
// is the whole design: the OS layer never blocks on readiness and never calls
// the scheduler, it answers OS_IO_WOULD_BLOCK; the caller parks on
// schedulerWaitFd and retries. A fiber that instead blocked in `read` would
// stop every other fiber on this OS thread, because they share it -- so these
// loops are the difference between a VM that can serve two connections and one
// that cannot, and packages/Core/src/Streams/Socket.st says so at the methods
// ("park the fiber until the socket is ready").
//
// THE DESCRIPTOR IS A SmallInteger IN THE IMAGE, which is why the kernel's
// socket protocol is mostly class-side: `Socket class read: descriptor into:
// ...`. That is deliberate and predates this file -- a Smalltalk object holding
// a raw fd could outlive the close and name a descriptor the OS has since
// handed to someone else, so the image holds a number and the VM checks it on
// every call.
//
// FAILURE IS ALWAYS THE KERNEL'S TO EXPLAIN. Each of these fails into
// `IoError last signal`, and `IoError class last` reads errno through its own
// primitive; nothing here formats a message.

#include "runtime/primitives/Shared.h"
#include "concurrency/Scheduler.h"
#include "os/OsSocket.h"
#include "runtime/String.h"

// The 32-bit address out of an InternetAddress.
//
// AN OBJECT AND NOT A NUMBER, which is what the kernel passes:
// `InternetAddress class>>lookup:` answers an INSTANCE and
// `ServerSocket bindTo: address port:` hands that instance straight through.
// The integer is its first named slot, which is what
// packages/Core/src/Streams/InternetAddress.st declares -- `| address |` and
// nothing before it -- and what its printOn: takes apart byte by byte.
//
// THE BYTE ORDER IS THE IMAGE'S. printOn: reads the LOW byte as the first
// octet, so 127.0.0.1 is 0x0100007F, and the OS layer memcpy's the word into
// sin_addr unchanged (os/linux/OsSocket.c). Nothing here swaps it; the two ends
// simply agree, and a swap in the middle would be a third opinion.
static _Bool internetAddress(Value value, uint32_t *ip)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_POINTERS) {
		return 0;
	}
	RawClass *class = classOf(value);
	if (class == NULL || class->instanceShape.fixedSlots < 1) {
		return 0;
	}
	Value slot = ((Value *) object->body)[0];
	if (!valueTypeOf(slot, VALUE_INT)) {
		return 0;
	}
	*ip = (uint32_t) asCInt(slot);
	return 1;
}


// A descriptor argument, checked. A negative one is not merely invalid, it is
// OS_FD_INVALID, and passing it to the OS layer would be asking about a
// descriptor this process never had.
static _Bool socketDescriptor(Value value, OsFd *fd)
{
	if (!valueTypeOf(value, VALUE_INT) || asCInt(value) < 0) {
		return 0;
	}
	*fd = (OsFd) asCInt(value);
	return 1;
}


// The byte payload of a String or ByteArray, with `size` bytes available from
// `start` (1-based). Both the read and the write side check the span, because
// the kernel passes a buffer that is routinely LONGER than the transfer:
// `nextPutAll:count:` hands over a stream's oversized backing buffer on purpose,
// to avoid copying an exact-length slice on the HTTP hot path.
static _Bool socketSpan(Value value, intptr_t start, intptr_t size, uint8_t **bytes)
{
	if (!valueTypeOf(value, VALUE_POINTER) || size < 0 || start < 1) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return 0;
	}
	size_t available = rawObjectElementCount(object);
	if ((size_t) (start - 1) + (size_t) size > available) {
		return 0;
	}
	*bytes = rawObjectBytes(object) + (start - 1);
	return 1;
}


// Socket class>>connect: address port: port
//
// The address arrives as the 32-bit integer InternetAddress carries, not as a
// name: resolution is a separate primitive because it is a separate failure
// with its own error namespace (os/OsSocket.h).
Value primSocketConnect(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	uint32_t ip;
	Value port = primitiveArgument(args, 1);
	if (!internetAddress(primitiveArgument(args, 0), &ip)
			|| !valueTypeOf(port, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	// Connecting parks, and parking runs other fibers, which allocate.
	PRIMITIVE_ALLOCATES(args);
	OsFd fd = OS_FD_INVALID;
	OsIoStatus status = osSocketConnectBegin(ip, (uint16_t) asCInt(port), &fd);
	if (status == OS_IO_WOULD_BLOCK) {
		// IN PROGRESS: the connect completes when the socket becomes WRITABLE,
		// and the pending error has to be collected afterwards -- a refused
		// connection also makes it writable, and reading the error is the only
		// way to tell the two apart.
		schedulerWaitFd(fd, 1);
		status = osSocketConnectFinish(fd);
	}
	PRIMITIVE_DONE_ALLOCATING();
	if (status != OS_IO_OK) {
		if (fd != OS_FD_INVALID) {
			schedulerForgetFd(fd);
			osSocketClose(fd);
		}
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) fd);
}


// ServerSocket class>>bind: address port: port queueSize: queueSize
//
// Does not park: bind and listen are immediate. What parks is `accept`.
Value primSocketBind(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	uint32_t ip;
	Value port = primitiveArgument(args, 1);
	Value queueSize = primitiveArgument(args, 2);
	if (!internetAddress(primitiveArgument(args, 0), &ip)
			|| !valueTypeOf(port, VALUE_INT)
			|| !valueTypeOf(queueSize, VALUE_INT) || asCInt(queueSize) < 0) {
		return PRIMITIVE_FAILED;
	}
	OsFd fd = OS_FD_INVALID;
	if (osSocketListen(ip, (uint16_t) asCInt(port),
			(int) asCInt(queueSize), &fd) != OS_IO_OK) {
		return PRIMITIVE_FAILED;
	}
	return tagInt((intptr_t) fd);
}


// ServerSocket>>basicAccept
//
// LOOPS, because readiness is not a promise: the listener can report readable
// and the connection be gone by the time accept runs, and another fiber may
// have taken it first. Answering an error there would make a server die of a
// client that changed its mind.
Value primSocketAccept(Value *args, uint64_t argc)
{
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	// The receiver is a ServerSocket and its descriptor is its first named
	// slot, which is what packages/Core/src/Streams/ServerSocket.st declares:
	// `| descriptor |` and nothing before it.
	Value receiver = primitiveReceiver(args);
	if (!valueTypeOf(receiver, VALUE_POINTER)
			|| rawObjectFormat(asObject(receiver)) != FORMAT_POINTERS) {
		return PRIMITIVE_FAILED;
	}
	OsFd listener;
	if (!socketDescriptor(((Value *) asObject(receiver)->body)[0], &listener)) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	OsFd client = OS_FD_INVALID;
	for (;;) {
		OsIoStatus status = osSocketAccept(listener, &client);
		if (status == OS_IO_OK) {
			PRIMITIVE_DONE_ALLOCATING();
			return tagInt((intptr_t) client);
		}
		if (status == OS_IO_ERROR) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		if (status == OS_IO_WOULD_BLOCK && !schedulerWaitFd(listener, 0)) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED; // no scheduler to park on
		}
		// INTERRUPTED retries immediately, which is what EINTR means.
	}
}


// Socket class>>read: descriptor into: aString size: anInteger startingAt: start
//
// Answers HOW MANY bytes arrived, and ZERO MEANS THE PEER CLOSED, which is what
// the kernel's `directNext:into:startingAt:` is written against. It parks until
// at least one byte is there rather than looping to fill the buffer: a reader
// that waited for a full buffer would deadlock against a peer waiting for a
// reply to what it already sent.
Value primSocketRead(Value *args, uint64_t argc)
{
	if (argc != 4) {
		return PRIMITIVE_FAILED;
	}
	OsFd fd;
	Value sizeValue = primitiveArgument(args, 2);
	Value startValue = primitiveArgument(args, 3);
	if (!socketDescriptor(primitiveArgument(args, 0), &fd)
			|| !valueTypeOf(sizeValue, VALUE_INT)
			|| !valueTypeOf(startValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t size = asCInt(sizeValue);
	intptr_t start = asCInt(startValue);
	PRIMITIVE_ALLOCATES(args);
	for (;;) {
		// THE BUFFER IS RE-RESOLVED EVERY TURN. Parking runs other fibers, which
		// allocate, which can move the String; a pointer taken before the park
		// would be the address it used to have.
		uint8_t *bytes;
		if (!socketSpan(primitiveArgument(args, 1), start, size, &bytes)) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		size_t read = 0;
		OsIoStatus status = osSocketRead(fd, bytes, (size_t) size, &read);
		if (status == OS_IO_OK) {
			PRIMITIVE_DONE_ALLOCATING();
			return tagInt((intptr_t) read);
		}
		if (status == OS_IO_ERROR) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		if (status == OS_IO_WOULD_BLOCK && !schedulerWaitFd(fd, 0)) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
	}
}


// Socket class>>write: descriptor from: aString size: anInteger
//
// WRITES FULLY, and that is the asymmetry with read: a short write is normal
// and the loop belongs here, parked on writability between attempts, because
// nothing above could resume a half-sent message meaningfully. os/OsSocket.h
// says the same thing from its side: one attempt per call, the loop in the VM.
Value primSocketWrite(Value *args, uint64_t argc)
{
	if (argc != 3) {
		return PRIMITIVE_FAILED;
	}
	OsFd fd;
	Value sizeValue = primitiveArgument(args, 2);
	if (!socketDescriptor(primitiveArgument(args, 0), &fd)
			|| !valueTypeOf(sizeValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	intptr_t size = asCInt(sizeValue);
	intptr_t sent = 0;
	PRIMITIVE_ALLOCATES(args);
	while (sent < size) {
		uint8_t *bytes;
		// Re-resolved every turn, for the same reason the read loop does it.
		if (!socketSpan(primitiveArgument(args, 1), sent + 1, size - sent, &bytes)) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		size_t written = 0;
		OsIoStatus status = osSocketWrite(fd, bytes, (size_t) (size - sent), &written);
		if (status == OS_IO_OK) {
			sent += (intptr_t) written;
			continue;
		}
		if (status == OS_IO_ERROR) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
		if (status == OS_IO_WOULD_BLOCK && !schedulerWaitFd(fd, 1)) {
			PRIMITIVE_DONE_ALLOCATING();
			return PRIMITIVE_FAILED;
		}
	}
	PRIMITIVE_DONE_ALLOCATING();
	return tagInt(sent);
}


// Socket class>>setNoDelay: descriptor
//
// Connected and accepted sockets get this from the OS layer already; the
// kernel keeps the selector for explicit control, and so does this.
Value primSocketSetNoDelay(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	OsFd fd;
	if (!socketDescriptor(primitiveArgument(args, 0), &fd)) {
		return PRIMITIVE_FAILED;
	}
	return osSocketSetNoDelay(fd) == OS_IO_OK
		? primitiveReceiver(args) : PRIMITIVE_FAILED;
}


// InternetAddress class>>lookup: aString
//
// Answers the 32-bit address InternetAddress>>printOn: takes apart byte by
// byte. It FAILS rather than reporting the reason, and the reason is that
// getaddrinfo has its OWN error namespace (os/OsSocket.h): errno says nothing
// about it, so `IoError last` would report an unrelated failure with
// confidence. The kernel's fallback signals a plain Error.
Value primSocketHostLookup(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	Value name = primitiveArgument(args, 0);
	if (!valueTypeOf(name, VALUE_POINTER)
			|| rawObjectFormat(asObject(name)) != FORMAT_BYTES) {
		return PRIMITIVE_FAILED;
	}
	RawObject *object = asObject(name);
	size_t length = rawObjectElementCount(object);
	char host[256];
	if (length + 1 > sizeof host) {
		return PRIMITIVE_FAILED;
	}
	memcpy(host, rawObjectBytes(object), length);
	host[length] = '\0';
	uint32_t ip = 0;
	char reason[256];
	if (!osSocketHostLookup(host, &ip, reason, sizeof reason)) {
		return PRIMITIVE_FAILED;
	}
	// AN INSTANCE, not the number. The receiver is InternetAddress itself (a
	// class-side method), so the class to instantiate is the receiver, and every
	// caller passes what comes back straight to bind: or connect:.
	Class *class = receiverAsClass(primitiveReceiver(args));
	if (class == NULL) {
		return PRIMITIVE_FAILED;
	}
	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	Object *address = newObject(scopeHandle((RawObject *) class->raw), 0);
	// An immediate, so no write barrier: the slot never names a heap object.
	((Value *) address->raw->body)[0] = tagInt((intptr_t) ip);
	Value answer = objectTagged(address);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}
