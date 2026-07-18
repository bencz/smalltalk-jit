#ifndef BYTECODES_H
#define BYTECODES_H

#include "core/Object.h"
#include "jit/Assembler.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <stdint.h>

typedef enum {
	BYTECODE_COPY, // destination:op, source:op
	BYTECODE_SEND, // selector:literal, noOfArgs:byte, receiver:op, arg:op[0..noOfArgs]
	BYTECODE_SEND_WITH_STORE, // selector:literal, noOfArgs:byte, receiver:op, arg:op[0..noOfArgs], result:op
	BYTECODE_RETURN, // source:op
	BYTECODE_OUTER_RETURN, // source:op
	BYTECODE_JUMP, // target:int32
	BYTECODE_JUMP_NOT_MEMBER_OF // class:literal, arg:op, target:int32
} Bytecode;

typedef enum {
	OPERAND_VALUE, // 64b int
	OPERAND_NIL,
	OPERAND_TRUE,
	OPERAND_FALSE,
	OPERAND_THIS_CONTEXT,
	OPERAND_TEMP_VAR, // index
	OPERAND_ARG_VAR, // index
	OPERAND_SUPER,
	OPERAND_CONTEXT_VAR, // index level
	OPERAND_INST_VAR, // index
	OPERAND_INST_VAR_OF, // index operandType index [level]
	OPERAND_LITERAL, // index
	OPERAND_ASSOC, // index
	OPERAND_BLOCK, // index
} OperandType;

typedef struct {
	_Bool isValid;
	OperandType type;
	union {
		struct {
			uint16_t index;   // 16-bit: literal/selector pools may exceed 255 entries
			uint8_t level;
			struct {
				OperandType type;
				uint16_t index;
				uint8_t level;
			} instance;
		};
		int64_t int64;
		uint64_t uint64;
		Value value;
		// double
	};
} Operand;

typedef struct {
	uint8_t *p;
	uint8_t *start;
	uint8_t *end;
	ptrdiff_t bytecodeNumber;
} BytecodesIterator;

static void bytecodeCopy(AssemblerBuffer *buffer, Operand *source, Operand *dest);
static void bytecodeSend(AssemblerBuffer *buffer, uint16_t selector, Operand *receiver, Operand *args, uint8_t numArgs);
static void bytecodeSendWithStore(AssemblerBuffer *buffer, uint16_t selector, Operand *receiver, Operand *result, Operand *args, uint8_t numArgs);
static void bytecodeReturn(AssemblerBuffer *buffer, Operand *operand, _Bool outer);
static void bytecodeJump(AssemblerBuffer *buffer, AssemblerLabel *label);
static void bytecodeJumpNotMemberOf(AssemblerBuffer *buffer, Operand *operand, uint16_t class, AssemblerLabel *label);
static void bytecodeOperand(AssemblerBuffer *buffer, Operand *operand);

static void bytecodeInitIterator(BytecodesIterator *iterator, uint8_t *bytecodes, size_t size);
static ptrdiff_t bytecodeNumber(BytecodesIterator *iterator);
static ptrdiff_t bytecodeOffset(BytecodesIterator *iterator);
static Bytecode bytecodeNext(BytecodesIterator *iterator);
static Operand bytecodeNextOperand(BytecodesIterator *iterator);
static uint8_t bytecodeNextByte(BytecodesIterator *iterator);
static uint16_t bytecodeNextUint16(BytecodesIterator *iterator);
static _Bool bytecodeHasNext(BytecodesIterator *iterator);


static void bytecodeCopy(AssemblerBuffer *buffer, Operand *source, Operand *dest)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, BYTECODE_COPY);
	bytecodeOperand(buffer, source);
	// INST_VAR_OF destinations come only from the tier-1 inliner (a callee
	// ivar store rewritten against the spilled receiver temp).
	ASSERT(dest->type == OPERAND_TEMP_VAR || dest->type == OPERAND_CONTEXT_VAR
		|| dest->type == OPERAND_INST_VAR || dest->type == OPERAND_ASSOC
		|| dest->type == OPERAND_INST_VAR_OF);
	bytecodeOperand(buffer, dest);
	buffer->instOffset++;
}


static void bytecodeSend(AssemblerBuffer *buffer, uint16_t selector, Operand *receiver, Operand *args, uint8_t numArgs)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, BYTECODE_SEND);
	asmEmitUint16(buffer, selector);
	asmEmitUint8(buffer, numArgs);
	bytecodeOperand(buffer, receiver);
	for (ptrdiff_t i = numArgs - 1; i >= 0; i--) {
		bytecodeOperand(buffer, args + i);
	}
	buffer->instOffset++;
}


static void bytecodeSendWithStore(AssemblerBuffer *buffer, uint16_t selector, Operand *receiver, Operand *result, Operand *args, uint8_t numArgs)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, BYTECODE_SEND_WITH_STORE);
	asmEmitUint16(buffer, selector);
	asmEmitUint8(buffer, numArgs);
	bytecodeOperand(buffer, receiver);
	for (ptrdiff_t i = numArgs - 1; i >= 0; i--) {
		bytecodeOperand(buffer, args + i);
	}
	bytecodeOperand(buffer, result);
	buffer->instOffset++;
}


static void bytecodeReturn(AssemblerBuffer *buffer, Operand *operand, _Bool outer)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, outer ? BYTECODE_OUTER_RETURN : BYTECODE_RETURN);
	bytecodeOperand(buffer, operand);
	buffer->instOffset++;
}


static void bytecodeJump(AssemblerBuffer *buffer, AssemblerLabel *label)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, BYTECODE_JUMP);
	asmEmitLabel32(buffer, label);
	buffer->instOffset++;
}


static void bytecodeJumpNotMemberOf(AssemblerBuffer *buffer, Operand *operand, uint16_t class, AssemblerLabel *label)
{
	asmEnsureCapacity(buffer);
	asmEmitUint8(buffer, BYTECODE_JUMP_NOT_MEMBER_OF);
	asmEmitUint16(buffer, class);
	bytecodeOperand(buffer, operand);
	asmEmitLabel32(buffer, label);
	buffer->instOffset++;
}


static void bytecodeOperand(AssemblerBuffer *buffer, Operand *operand)
{
	switch (operand->type) {
	case OPERAND_VALUE:
		asmEmitUint8(buffer, operand->type);
		storeU64(buffer->p, operand->value); // unaligned-safe (core/Endian.h)
		buffer->p += sizeof(operand->value);
		break;

	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_INST_VAR:
	case OPERAND_LITERAL:
	case OPERAND_ASSOC:
	case OPERAND_BLOCK:
		asmEmitUint8(buffer, operand->type);
		asmEmitUint16(buffer, operand->index);
		break;

	case OPERAND_INST_VAR_OF: {
		asmEmitUint8(buffer, operand->type);
		asmEmitUint16(buffer, operand->index);
		ASSERT(operand->instance.type != OPERAND_VALUE)
		ASSERT(operand->instance.type != OPERAND_INST_VAR);
		ASSERT(operand->instance.type != OPERAND_INST_VAR_OF);
		// A REAL Operand, not a cast of the nested struct: the layouts differ
		// (Operand starts with isValid + padding), so the old cast emitted
		// bytes read from beyond the instance fields.
		Operand instance = { .isValid = 1, .type = operand->instance.type,
			.index = operand->instance.index, .level = operand->instance.level };
		bytecodeOperand(buffer, &instance);
		break;
	}

	case OPERAND_CONTEXT_VAR:
		asmEmitUint8(buffer, operand->type);
		asmEmitUint16(buffer, operand->index);
		asmEmitUint8(buffer, operand->level);
		break;

	default:
		asmEmitUint8(buffer, operand->type);
	}
}


static void bytecodeInitIterator(BytecodesIterator *iterator, uint8_t *bytecodes, size_t size)
{
	iterator->p = bytecodes;
	iterator->start = bytecodes;
	iterator->end = bytecodes + size;
	iterator->bytecodeNumber = -1;
}


static ptrdiff_t bytecodeNumber(BytecodesIterator *iterator)
{
	return iterator->bytecodeNumber;
}


static ptrdiff_t bytecodeOffset(BytecodesIterator *iterator)
{
	return iterator->p - iterator->start;
}


static Bytecode bytecodeNext(BytecodesIterator *iterator)
{
	iterator->bytecodeNumber++;
	return (Bytecode) bytecodeNextByte(iterator);
}


static Operand bytecodeNextOperand(BytecodesIterator *iterator)
{
	Operand operand = { .isValid = 1, .type = bytecodeNextByte(iterator) };
	switch (operand.type) {
	case OPERAND_VALUE:
		operand.value = loadU64(iterator->p); // unaligned-safe (core/Endian.h)
		iterator->p += sizeof(Value);
		break;
	case OPERAND_TEMP_VAR:
	case OPERAND_ARG_VAR:
	case OPERAND_INST_VAR:
	case OPERAND_LITERAL:
	case OPERAND_ASSOC:
	case OPERAND_BLOCK:
		operand.index = bytecodeNextUint16(iterator);
		break;
	case OPERAND_INST_VAR_OF:
		operand.index = bytecodeNextUint16(iterator);
		Operand instance = bytecodeNextOperand(iterator);
		ASSERT(instance.type != OPERAND_VALUE)
		ASSERT(instance.type != OPERAND_INST_VAR);
		ASSERT(instance.type != OPERAND_INST_VAR_OF);
		operand.instance.type = instance.type;
		operand.instance.index = instance.index;
		operand.instance.level = instance.level;
		break;
	case OPERAND_CONTEXT_VAR:
		operand.index = bytecodeNextUint16(iterator);
		operand.level = bytecodeNextByte(iterator);
		break;
	default:;
	}
	return operand;
}


static uint8_t bytecodeNextByte(BytecodesIterator *iterator)
{
	return *iterator->p++;
}


// 16-bit literal/selector index (unaligned-safe, mirrors asmEmitUint16).
static uint16_t bytecodeNextUint16(BytecodesIterator *iterator)
{
	uint16_t v = loadU16(iterator->p);
	iterator->p += sizeof(uint16_t);
	return v;
}


static int32_t bytecodeNextInt32(BytecodesIterator *iterator)
{
	// The stream mixes 1-byte opcodes with these 4-byte fields, so the cursor
	// is frequently unaligned: memcpy-based load (core/Endian.h).
	int32_t result = loadI32(iterator->p);
	iterator->p += sizeof(int32_t);
	return result;
}


static _Bool bytecodeHasNext(BytecodesIterator *iterator)
{
	return iterator->p < iterator->end;
}


static void printOperand(Operand operand, RawArray *literals)
{
	switch (operand.type) {
	case OPERAND_VALUE:
		printf(" %zx", operand.value);
		break;
	case OPERAND_NIL:
		printf(" nil");
		break;
	case OPERAND_TRUE:
		printf(" true");
		break;
	case OPERAND_FALSE:
		printf(" false");
		break;
	case OPERAND_THIS_CONTEXT:
		printf(" thisContext");
		break;
	case OPERAND_TEMP_VAR:
		printf(" temp#%i", operand.index);
		break;
	case OPERAND_ARG_VAR:
		printf(" arg#%i", operand.index);
		break;
	case OPERAND_SUPER:
		printf(" super");
		break;
	case OPERAND_CONTEXT_VAR:
		printf(" context#%i.%i", operand.level, operand.index);
		break;
	case OPERAND_INST_VAR:
		printf(" instVar#%i", operand.index);
		break;
	case OPERAND_INST_VAR_OF:
		printf(" instVar#%i of", operand.index);
		printOperand(*(Operand *) &operand.instance, literals);
		break;
	case OPERAND_LITERAL:
	case OPERAND_ASSOC:
	case OPERAND_BLOCK:
		printf(" ");
		printValue(literals->vars[operand.index]);
		printf("#%i", operand.index);
		break;
	}
}


static void printBytecodes(uint8_t *bytecodes, size_t size, RawArray *literals)
{
	BytecodesIterator iterator;
	bytecodeInitIterator(&iterator, bytecodes, size);

	while (bytecodeHasNext(&iterator)) {
		printf("<%zX>\t", iterator.p - bytecodes);
		Bytecode bytecode = bytecodeNext(&iterator);

		switch (bytecode) {
		case BYTECODE_COPY:
			printf("COPY");
			printOperand(bytecodeNextOperand(&iterator), literals);
			printOperand(bytecodeNextOperand(&iterator), literals);
			break;
		case BYTECODE_SEND:
		case BYTECODE_SEND_WITH_STORE:
			printf("SEND #");
			printRawString((RawString *) asObject(literals->vars[bytecodeNextUint16(&iterator)]));
			uint8_t argsSize = bytecodeNextByte(&iterator);
			printf(" TO");
			printOperand(bytecodeNextOperand(&iterator), literals);
			if (argsSize > 0) {
				printf(" ARGUMENTS");
			}
			for (uint8_t i = 0; i < argsSize; i++) {
				printOperand(bytecodeNextOperand(&iterator), literals);
				printf(",");
			}
			if (bytecode == BYTECODE_SEND_WITH_STORE) {
				printf(" STORE IN");
				printOperand(bytecodeNextOperand(&iterator), literals);
			}
			break;
		case BYTECODE_RETURN:
			printf("RETURN");
			printOperand(bytecodeNextOperand(&iterator), literals);
			break;
		case BYTECODE_OUTER_RETURN:
			printf("OUTER RETURN");
			printOperand(bytecodeNextOperand(&iterator), literals);
			break;
		case BYTECODE_JUMP:
			printf("JUMP 0x%X", bytecodeNextInt32(&iterator));
			break;
		case BYTECODE_JUMP_NOT_MEMBER_OF: {
			RawClass *class = (RawClass *) asObject(literals->vars[bytecodeNextUint16(&iterator)]);
			Operand operand = bytecodeNextOperand(&iterator);
			printf("JUMP 0x%X IF", bytecodeNextInt32(&iterator));
			printOperand(operand, literals);
			printf(" IS NOT MEMBER OF ");
			printClassName(class);
			break;
		}
		default:
			FAIL();
		}

		printf("\n");
	}
}

#endif
