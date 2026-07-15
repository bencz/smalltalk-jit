#ifndef TARGET_FIBER_H
#define TARGET_FIBER_H

// CPU-specific stackful-coroutine support. Bound at link time: the CMake
// ST_ARCH selection compiles exactly one vm/jit/<arch>/Fiber<Arch>.c that
// provides these. All knowledge of the callee-saved set, the switch frame
// layout and the ABI's stack-entry alignment lives arch-side.

// Save the current context's callee-saved registers on its own stack, store
// the resulting sp to *saveSp, switch to newSp, restore that context's
// registers and return into it. Classic stackful coroutine swap.
void fiberSwitchAsm(void **saveSp, void *newSp);

// Prime a fresh stack (highest usable address `top`; at least the top page is
// committed) so the FIRST fiberSwitchAsm into the returned sp pops a zeroed
// callee-saved frame and returns into `entry` with the ABI's required entry
// alignment. Returns the sp to store in Fiber.sp.
void *fiberTargetPrimeStack(void *top, void (*entry)(void));

#endif
