#ifndef VARIABLE_H
#define VARIABLE_H

#include "core/Object.h"

typedef struct {
	uint8_t tag;
	uint8_t type;
	uint16_t index;   // 16-bit: a method can reference up to 65535 literals/selectors
	uint8_t level;
	uint8_t ctxCopy;
	uint8_t unused1;
	uint8_t unused2;
} Variable;


static Value defineVariable(uint8_t type, uint16_t index, uint8_t level)
{
	Variable tmp;
	tmp.tag = 0;
	tmp.type = type;
	tmp.index = index;
	tmp.level = level;
	tmp.ctxCopy = 0;
	tmp.unused1 = 0;
	tmp.unused2 = 0;
	return *(Value *) &tmp;
}


static void setVarType(Value *var, uint8_t type)
{
	((Variable *) var)->type = type;
}


static uint8_t getVarType(Value var)
{
	return ((Variable *) &var)->type;
}


static void setVarIndex(Value *var, uint16_t index)
{
	((Variable *) var)->index = index;
}


static uint16_t getVarIndex(Value var)
{
	return ((Variable *) &var)->index;
}


static void setVarLevel(Value *var, uint8_t level)
{
	((Variable *) var)->level = level;
}


static uint8_t getVarLevel(Value var)
{
	return ((Variable *) &var)->level;
}


static void setVarCtxCopy(Value *var, uint8_t index)
{
	((Variable *) var)->ctxCopy = index + 1;
}


static uint8_t getVarCtxCopy(Value var)
{
	return ((Variable *) &var)->ctxCopy - 1;
}


static _Bool hasVarCtxCopy(Value var)
{
	return ((Variable *) &var)->ctxCopy != 0;
}


#endif
