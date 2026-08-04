// Every SSA backend compiled into this build, and which one the host is.
//
// The same arrangement as jit/Backends.c one tier down, for the same reason:
// with every backend present, emitting for a FOREIGN target and comparing the
// bytes against an oracle is an ordinary test rather than a second build.

#include "jit/SsaEmitter.h"
#include <string.h>

extern const SsaEmitterOps gSsaEmitterX64SysV;

const SsaEmitterOps *const gSsaEmitterBackends[] = {
	&gSsaEmitterX64SysV,
	NULL,
};


// CACHED, and that is not premature: the tier-1 equivalent was measured
// walking this table with strcmp on every send and costing 14% of Richards.
const SsaEmitterOps *ssaHostBackend(void)
{
	static const SsaEmitterOps *host;
	if (host == NULL) {
#if defined(__x86_64__)
		host = &gSsaEmitterX64SysV;
#else
#error "no SSA backend for this architecture"
#endif
	}
	return host;
}
