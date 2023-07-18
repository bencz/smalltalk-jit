#include "Lookup.h"
#include "CodeGenerator.h"
#include "Object.h"
#include "Class.h"
#include "String.h"
#include "Smalltalk.h"
#include "Heap.h"
#include "Handle.h"
#include "CompiledCode.h"
#include "CodeDescriptors.h"
#include "Thread.h"
#include "StackFrame.h"

LookupTable LookupCache = {
	.classes = { NULL },
	.selectors = { NULL },
	.codes = { NULL },
};

static void feedbackType(Class *class);
static NativeCodeEntry doesNotUnderstand(Class *class, String *selector);

NativeCodeEntry lookupNativeCode(RawClass *class, RawString *selector)
{
    HandleScope scope;
    openHandleScope(&scope);

    Class *classHandle = scopeHandle(class);
    String *selectorHandle = scopeHandle(selector);
    CompiledMethod *method = lookupSelector(classHandle, selectorHandle);

    NativeCodeEntry entry;
    if (method == NULL) {
        entry = doesNotUnderstand(classHandle, selectorHandle);
    } else {
        union PointerConverter converter;
        converter.object_pointer = getNativeCode(classHandle, method)->insts;
        entry = converter.function_pointer;
        feedbackType(classHandle);
    }

    intptr_t hash = lookupHash((intptr_t) classHandle->raw, (intptr_t) selectorHandle->raw);
    LookupCache.classes[hash] = classHandle->raw;
    LookupCache.selectors[hash] = selectorHandle->raw;

    union PointerConverter converter;
    converter.function_pointer = entry;
    LookupCache.codes[hash] = converter.object_pointer;

    closeHandleScope(&scope, NULL);
    return entry;
}

static void feedbackType(Class *class)
{
	EntryStackFrame *entryFrame = CurrentThread.stackFramesTail;
	if (entryFrame == NULL) {
		return;
	}

	StackFrame *frame = stackFrameGetParent(entryFrame->exit, entryFrame);
	NativeCode *code = stackFrameGetNativeCode(frame);
	OrderedCollection *typeFeedback;
	if (code->typeFeedback == NULL) {
		typeFeedback = newOrdColl(8);
		code->typeFeedback = typeFeedback->raw;
	} else {
		typeFeedback = scopeHandle(code->typeFeedback);
		if (ordCollSize(typeFeedback) > 16) {
			typeFeedback = newOrdColl(8);
			code->typeFeedback = typeFeedback->raw;
		}
	}

	TypeFeedback *type = newObject(Handles.TypeFeedback, 0);
	type->raw->ic = tagInt(entryFrame->exit->parentIc - code->insts);
	type->raw->hintedClass = getTaggedPtr(class);
	ordCollAddObject(typeFeedback, (Object *) type);
}


static NativeCodeEntry doesNotUnderstand(Class *class, String *selector)
{
    intptr_t hash = lookupHash((intptr_t) class->raw, (intptr_t) selector->raw);
    NativeCode *code = generateDoesNotUnderstand(selector);
    code->compiledCode = lookupSelector(class, Handles.doesNotUnderstandSymbol)->raw;

    union PointerConverter converter;
    converter.object_pointer = code->insts;
    return converter.function_pointer;
}


NativeCode *getNativeCode(Class *class, CompiledMethod *method)
{
	NativeCode *code = compiledMethodGetNativeCode(method);
	if (code == NULL) {
		String *selector = compiledMethodGetSelector(method);
		code = generateMethodCode(method);
		compiledMethodSetNativeCode(method, code);
	}
	return code;
}
