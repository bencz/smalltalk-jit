#ifndef SEND_CLASSIFY_H
#define SEND_CLASSIFY_H

// Send-site classification shared by BOTH backends' generateSend and by the
// tier-1 bytecode inliner (compiler/Optimizer.c). The inliner must know which
// sends the codegen turns into IC sites (every dynamic send EXCEPT the
// identity family, which emits no dispatch at all) to keep the send-to-cell
// correspondence exact: a divergence here is not a slowdown, it is feedback
// attached to the WRONG selector, which a promoted guard would then dispatch
// to the wrong method. One copy, three consumers, zero drift.

#include "core/Object.h"
#include <string.h>

static _Bool rawSelectorIs(RawObject *selector, const char *name, size_t len)
{
	return rawObjectSize(selector) == len
		&& memcmp(getRawObjectIndexedVars(selector), name, len) == 0;
}

// SmallInteger selectors inlined at the call site (fast path in generateSend).
// The bit-op kinds are int-only (never taken by the Float fast path).
enum { ARITH_NONE = 0, ARITH_ADD, ARITH_SUB, ARITH_MUL, ARITH_DIV,
       ARITH_LT, ARITH_LE, ARITH_GT, ARITH_GE, ARITH_EQ, ARITH_NE,
       ARITH_BITAND, ARITH_BITOR, ARITH_BITXOR };

static int classifyArith(RawObject *selector)
{
	size_t n = rawObjectSize(selector);
	char *s = (char *) getRawObjectIndexedVars(selector);
	if (n == 1) {
		switch (s[0]) {
		case '+': return ARITH_ADD;
		case '-': return ARITH_SUB;
		case '*': return ARITH_MUL;
		case '/': return ARITH_DIV;   // Float only (single-instruction divsd); SmallInteger keeps dispatching
		case '<': return ARITH_LT;
		case '>': return ARITH_GT;
		case '=': return ARITH_EQ;
		}
	} else if (n == 2 && s[1] == '=') {
		switch (s[0]) {
		case '<': return ARITH_LE;
		case '>': return ARITH_GE;
		case '~': return ARITH_NE;
		}
	} else {
		if (rawSelectorIs(selector, "bitAnd:", 7)) return ARITH_BITAND;
		if (rawSelectorIs(selector, "bitOr:", 6)) return ARITH_BITOR;
		if (rawSelectorIs(selector, "bitXor:", 7)) return ARITH_BITXOR;
	}
	return ARITH_NONE;
}

static _Bool arithIsCompare(int kind) { return kind >= ARITH_LT && kind <= ARITH_NE; }
static _Bool arithIsBitOp(int kind) { return kind >= ARITH_BITAND; }


// Identity / nil tests inlined at the call site. `==`/`~~` are non-overridable
// identity; `isNil`/`notNil` compile to `== nil`/`~~ nil` (the only kernel
// definitions are identity-to-nil). Unlike the arithmetic fast paths these
// ALWAYS resolve, so no class guard, no dispatch and no IC site are emitted.
enum { IDENT_NONE = 0, IDENT_EQ, IDENT_NE, IDENT_ISNIL, IDENT_NOTNIL };

static int classifyIdentity(RawObject *selector, uint8_t argsSize)
{
	if (argsSize == 1) {
		if (rawSelectorIs(selector, "==", 2)) return IDENT_EQ;
		if (rawSelectorIs(selector, "~~", 2)) return IDENT_NE;
	} else if (argsSize == 0) {
		if (rawSelectorIs(selector, "isNil", 5)) return IDENT_ISNIL;
		if (rawSelectorIs(selector, "notNil", 6)) return IDENT_NOTNIL;
	}
	return IDENT_NONE;
}

#endif
