#ifndef ASSERT_H
#define ASSERT_H

#include <stdlib.h>
#include <stdio.h>

// stderr, not stdout: abort() does not flush stdio, so a message printf'd to
// a redirected (block-buffered) stdout dies with the process. That is how the
// POWER7 64KB-code-ceiling abort shipped silent: the assertion text was only
// recoverable from the stdio buffer inside the core dump. stderr is unbuffered
// by default; the fflush covers a redirected stderr too.
#ifdef NDEBUG
#define ASSERT(cond) while(0 && (cond)) {}
#define FAIL() { \
		fprintf(stderr, "Fatal error in %s:%u\n", __FILE__, __LINE__); \
		fflush(NULL); \
		exit(EXIT_FAILURE); \
	}
#else
#define ASSERT(cond) \
	if (!(cond)) { \
		fprintf(stderr, "Assertion '%s' failed in %s:%u\n", #cond, __FILE__, __LINE__); \
		fflush(NULL); \
		abort(); \
	}
#define FAIL() { \
		fprintf(stderr, "Fatal error in %s:%u\n", __FILE__, __LINE__); \
		fflush(NULL); \
		abort(); \
	}
#endif

#endif
