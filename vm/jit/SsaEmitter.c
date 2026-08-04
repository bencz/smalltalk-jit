// The architecture-neutral half of SSA emission: the walk.
//
// It decides the ORDER of blocks, which label belongs to which, and which
// successor is the fall-through. It never learns what a register is; every
// instruction goes to the backend's hook (ADR 0009).

#include "jit/SsaEmitter.h"
#include "jit/CodeBuffer.h"
#include "core/Assert.h"
#include <stdlib.h>
#include <string.h>

struct SsaEmitter {
	const SsaEmitterOps *ops;
	LirFunction *function;
	CodeBuffer buffer;
	uint8_t state[];
};


CodeBuffer *ssaEmitterBuffer(SsaEmitter *emitter) { return &emitter->buffer; }
void *ssaEmitterState(SsaEmitter *emitter) { return emitter->state; }
LirFunction *ssaEmitterFunction(SsaEmitter *emitter) { return emitter->function; }
const Abi *ssaEmitterAbi(SsaEmitter *emitter) { return emitter->ops->abi; }


const uint8_t *ssaEmitterBytes(SsaEmitter *emitter, size_t *size)
{
	if (size != NULL) {
		*size = emitter->buffer.size;
	}
	return emitter->buffer.bytes;
}


void ssaEmitterDestroy(SsaEmitter *emitter)
{
	if (emitter == NULL) {
		return;
	}
	emitter->ops->end(emitter);
	codeBufferFree(&emitter->buffer);
	free(emitter);
}


SsaEmitter *ssaEmit(const SsaEmitterOps *ops, LirFunction *function,
	Value nilValue)
{
	SsaEmitter *emitter = calloc(1, sizeof(SsaEmitter) + ops->stateBytes);
	ASSERT(emitter != NULL);
	emitter->ops = ops;
	emitter->function = function;
	codeBufferInit(&emitter->buffer);
	ops->begin(emitter);

	// One label per block, made up front: a backward branch names a block whose
	// code has already been emitted and a forward one names a block that has
	// not, and the label machinery does not care which as long as it exists.
	SsaLabel **labels = calloc(function->blockCount == 0 ? 1 : function->blockCount,
		sizeof(SsaLabel *));
	ASSERT(labels != NULL);
	for (uint32_t i = 0; i < function->blockCount; i++) {
		labels[i] = ops->newLabel(emitter);
	}

	ops->prologue(emitter, nilValue);

	for (LirBlock *block = function->blocks; block != NULL; block = block->next) {
		ops->bind(emitter, labels[block->id]);
		for (LirInstruction *it = block->first; it != NULL; it = it->next) {
			it->codeOffset = (uint32_t) emitter->buffer.size;
			SsaLabel *taken = NULL;
			SsaLabel *notTaken = NULL;
			if (lirOpIsTerminator((LirOp) it->op)) {
				// succs[0] is the taken arm and succs[1] the fall-through, an
				// order the lowering carried through from the SSA CFG so the
				// two ends cannot disagree about which is which.
				taken = block->succCount > 0 ? labels[block->succs[0]->id] : NULL;
				notTaken = block->succCount > 1 ? labels[block->succs[1]->id]
					: NULL;
				// The block laid out NEXT needs no jump to reach. Checked
				// against the layout rather than assumed from the CFG, because
				// the ordering pass is what decides layout and it is free to
				// change its mind.
				if (block->succCount == 1 && block->next == block->succs[0]) {
					it->codeOffset = (uint32_t) emitter->buffer.size;
					continue;
				}
			}
			ops->instruction(emitter, it, taken, notTaken);
		}
	}

	free(labels);
	return emitter;
}
