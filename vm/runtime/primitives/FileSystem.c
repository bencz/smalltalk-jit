// The filesystem: metadata, directories, and opening a file.
//
// THE POSIX IS NOT HERE. Every call below goes through the OS seam
// (os/OsFile.h), which is where `stat`, `mkdir`, `opendir` and `realpath` live
// in jit-v2. The old VM had them in runtime/FileSystem.c calling POSIX
// directly, and its directory lister even answered an OrderedCollection, so the
// platform layer had to know the object model. Here the seam answers plain C
// and this file builds the Smalltalk.
//
// FAILING AND ANSWERING NIL ARE DIFFERENT THINGS HERE, and getting them the
// wrong way round is not cosmetic. Every wrapper in Files/File.st and
// Files/Directory.st has an EMPTY body, so a primitive that fails does not fall
// through to a Smalltalk fallback -- it raises `self primitiveFailed:`. But the
// kernel's own comments promise `nil` for a missing path and `false` for a
// refused operation:
//
//     class exists: aPath [ stat := self statPath: aPath.
//                           ^stat notNil and: [(stat at: 1) = 1] ]
//
// so `File exists: '/nope'` must answer FALSE, not raise. The rule this file
// follows, stated once:
//
//   * a path that does not exist, or an operation the OS refuses  -> nil/false;
//   * an ARGUMENT THAT IS NOT A STRING                            -> fail.
//
// The second is a programmer error and has no sensible answer, so it gets the
// loud one. The first is an ordinary fact about a filesystem.
//
// The one exception is realpath, and it is an exception because its kernel
// method DOES carry a fallback (`^aPath`); failing there is how that fallback
// gets its turn.
//
// EVERY PATH IS COPIED TO A C BUFFER BEFORE ANYTHING ALLOCATES. A Smalltalk
// String is a heap object and building the answer moves it, so reading the
// argument afterwards would read a corpse. Copy first, call the OS, then
// allocate.

#include "runtime/primitives/Shared.h"
#include "runtime/Collection.h"
#include "runtime/String.h"
#include "os/OsFile.h"
#include <string.h>

// Longer than any path Linux will accept, and the same bound tools/Project.h
// uses, so the C side of the project tooling and this agree on what is too long.
#define FS_PATH_MAX 4096


// Copy a Smalltalk String argument into a NUL-terminated C buffer.
//
// Answers 0 when the value is not a byte object, when it does not fit, or when
// it CONTAINS A NUL. That last one matters: a counted Smalltalk String may hold
// a zero byte, and handing it to a C API silently truncates the path, which
// addresses a different file. Refusing is the only answer that is not wrong.
static _Bool pathOf(Value value, char *buffer, size_t size)
{
	if (!valueTypeOf(value, VALUE_POINTER)) {
		return 0;
	}
	RawObject *object = asObject(value);
	if (rawObjectFormat(object) != FORMAT_BYTES) {
		return 0;
	}
	size_t length = rawObjectElementCount(object);
	if (length + 1 > size) {
		return 0;
	}
	const uint8_t *bytes = rawObjectBytes(object);
	if (memchr(bytes, 0, length) != NULL) {
		return 0;
	}
	memcpy(buffer, bytes, length);
	buffer[length] = '\0';
	return 1;
}


static Value nilResult(void)
{
	return tagPtr(Handles.nil.raw);
}


// stat, as the three numbers Files/File.st and Files/Directory.st index by
// position: kind, size in bytes, modification time in epoch milliseconds.
//
// The ORDER is the contract with those two files and it is stated in
// os/OsFile.h beside the struct. A missing path answers nil, which is what both
// `exists:` and `size:` are written to test for.
Value primFileStat(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	OsPathInfo info;
	if (!osPathInfo(path, &info)) {
		return nilResult(); // no such path: an answer, not a failure
	}
	// The three fit a SmallInteger by construction: a kind is 1 to 3, a size is
	// an off_t and a time in milliseconds is about 2^41 today, both far inside
	// the 62-bit payload.
	//
	// Stored STRAIGHT INTO vars and not through arrayAtPutObject, which takes an
	// Object and goes through the write barrier. These are immediates: there is
	// no young pointer for the barrier to record, which is the same reason
	// newArray fills itself with nil directly (runtime/Collection.c).
	PRIMITIVE_ALLOCATES(args);
	Array *array = newArray(3);
	array->raw->vars[0] = tagInt((intptr_t) info.kind);
	array->raw->vars[1] = tagInt((intptr_t) info.sizeInBytes);
	array->raw->vars[2] = tagInt((intptr_t) info.modifiedMillis);
	Value answer = objectTagged((Object *) array);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// The mutating operations. All four answer true or false and never raise: their
// kernel methods say "Answer true on success, false on failure", and a caller
// that wanted the reason would have asked for it.
Value primFileMkdir(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(osPathCreateDirectory(path));
}


Value primFileRmdir(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(osPathRemoveDirectory(path));
}


Value primFileUnlink(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(osPathRemoveFile(path));
}


Value primFileRename(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	char from[FS_PATH_MAX];
	char to[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), from, sizeof from)
			|| !pathOf(primitiveArgument(args, 1), to, sizeof to)) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(osPathRename(from, to));
}


Value primFileChdir(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	return booleanResult(osPathSetCurrentDirectory(path));
}


Value primFileGetCwd(Value *args, uint64_t argc)
{
	(void) args;
	if (argc != 0) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!osPathCurrentDirectory(path, sizeof path)) {
		return nilResult();
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(path));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// The one that is SUPPOSED to fail when it cannot answer: File>>realpath: has
// `^aPath` under the pragma, so a path the OS will not resolve falls through to
// the kernel and comes back unchanged, which is exactly what its comment
// promises.
Value primFileRealpath(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	char resolved[FS_PATH_MAX];
	if (!osPathCanonical(path, resolved, sizeof resolved)) {
		return PRIMITIVE_FAILED; // the fallback answers the path unchanged
	}
	PRIMITIVE_ALLOCATES(args);
	Value answer = objectTagged(stringFromC(resolved));
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// An OrderedCollection of the entry names, or nil when the path is not a
// readable directory. `.` and `..` are already skipped by the seam.
//
// THE DIRECTORY STAYS OPEN ACROSS ALLOCATIONS, and that is safe in the one
// direction that matters: the OsDirectory is C memory that no collection moves,
// and the name is copied into a C buffer before each String is built. The
// collection itself is a handle, so it survives the collection that adding to
// it may trigger.
Value primFileListDir(Value *args, uint64_t argc)
{
	if (argc != 1) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)) {
		return PRIMITIVE_FAILED;
	}
	OsDirectory *directory = osDirectoryOpen(path);
	if (directory == NULL) {
		return nilResult(); // not a readable directory: an answer, not a failure
	}

	PRIMITIVE_ALLOCATES(args);
	HandleScope scope;
	openHandleScope(&scope);
	OrderedCollection *entries = newOrdColl(16);
	char name[FS_PATH_MAX];
	while (osDirectoryNext(directory, name, sizeof name)) {
		HandleScope each;
		openHandleScope(&each);
		ordCollAddObject(entries, (Object *) stringFromC(name));
		closeHandleScope(&each, NULL);
	}
	osDirectoryClose(directory);
	Value answer = objectTagged((Object *) entries);
	closeHandleScope(&scope, NULL);
	PRIMITIVE_DONE_ALLOCATING();
	return answer;
}


// FileStream class open: aPath mode: anInteger
//
// The mode numbers are the kernel's, from Streams/FileStream.st: 1 read, 2
// write (create + truncate), 4 read/write (create, no truncate). They are
// POWERS OF TWO with 3 deliberately unused -- FileStream.st carries a comment
// about 3 having once matched no branch and always failing to open -- so an
// unknown mode FAILS here rather than picking a neighbour.
//
// Failure runs the kernel's `IoError last appendName: aString; signal`, which
// is where a missing file turns into an exception a handler can catch.
Value primStreamOpen(Value *args, uint64_t argc)
{
	if (argc != 2) {
		return PRIMITIVE_FAILED;
	}
	char path[FS_PATH_MAX];
	Value modeValue = primitiveArgument(args, 1);
	if (!pathOf(primitiveArgument(args, 0), path, sizeof path)
			|| !valueTypeOf(modeValue, VALUE_INT)) {
		return PRIMITIVE_FAILED;
	}
	OsFileMode mode;
	switch (asCInt(modeValue)) {
	case 1: mode = OS_FILE_READ; break;
	case 2: mode = OS_FILE_WRITE_TRUNC; break;
	case 4: mode = OS_FILE_READ_WRITE_CREATE; break;
	default: return PRIMITIVE_FAILED;
	}
	OsFd fd = osFileOpen(path, mode);
	if (fd == OS_FD_INVALID) {
		return PRIMITIVE_FAILED; // the kernel raises an IoError naming the path
	}
	return tagInt((intptr_t) fd);
}
