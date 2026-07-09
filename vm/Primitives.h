#ifndef PRIMITIVES_H
#define PRIMITIVES_H

#include "CodeGenerator.h"

void registerPrimitives(void);
void generatePrimitive(CodeGenerator *generator, uint16_t primitive);
uint16_t primitiveCount(void);   // valid primitive numbers are 1..primitiveCount()

#endif
