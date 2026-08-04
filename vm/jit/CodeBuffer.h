#ifndef CODE_BUFFER_H
#define CODE_BUFFER_H

// A growable byte buffer for machine code, with no knowledge of any
// architecture. Code is built here and only afterwards copied into executable
// memory, so nothing is ever written to a page that is simultaneously
// executable, and a foreign target's code can be emitted and inspected without
// ever being mapped executable at all.

#include "core/Assert.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct CodeBuffer {
	uint8_t *bytes;
	size_t size;
	size_t capacity;
} CodeBuffer;


static inline void codeBufferInit(CodeBuffer *buffer)
{
	buffer->capacity = 256;
	buffer->bytes = malloc(buffer->capacity);
	ASSERT(buffer->bytes != NULL);
	buffer->size = 0;
}


static inline void codeBufferFree(CodeBuffer *buffer)
{
	free(buffer->bytes);
	buffer->bytes = NULL;
	buffer->size = 0;
	buffer->capacity = 0;
}


static inline void codeBufferReserve(CodeBuffer *buffer, size_t extra)
{
	if (buffer->size + extra <= buffer->capacity) {
		return;
	}
	while (buffer->capacity < buffer->size + extra) {
		buffer->capacity *= 2;
	}
	buffer->bytes = realloc(buffer->bytes, buffer->capacity);
	ASSERT(buffer->bytes != NULL);
}


static inline void emit8(CodeBuffer *buffer, uint8_t byte)
{
	codeBufferReserve(buffer, 1);
	buffer->bytes[buffer->size++] = byte;
}


// Little-endian by memcpy, which is what x86 and ARM want. A big-endian
// instruction encoding (POWER in BE mode) emits through emit32be instead: the
// choice belongs to the backend, not to the buffer.
static inline void emit32(CodeBuffer *buffer, uint32_t word)
{
	codeBufferReserve(buffer, 4);
	memcpy(buffer->bytes + buffer->size, &word, 4);
	buffer->size += 4;
}


static inline void emit32be(CodeBuffer *buffer, uint32_t word)
{
	codeBufferReserve(buffer, 4);
	buffer->bytes[buffer->size + 0] = (uint8_t) (word >> 24);
	buffer->bytes[buffer->size + 1] = (uint8_t) (word >> 16);
	buffer->bytes[buffer->size + 2] = (uint8_t) (word >> 8);
	buffer->bytes[buffer->size + 3] = (uint8_t) word;
	buffer->size += 4;
}


static inline void emit64(CodeBuffer *buffer, uint64_t word)
{
	codeBufferReserve(buffer, 8);
	memcpy(buffer->bytes + buffer->size, &word, 8);
	buffer->size += 8;
}

#endif
