// Linux signal handling (vm/os/Os.h): the SIGSEGV-driven fiber stack growth
// and SIGPIPE suppression.
#include "os/Os.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
static __thread char *gAltStack = NULL;               // per-thread signal stack
static int gSegvHandlerInstalled = 0;                 // process-global, once
static _Bool (*gSegvCallback)(uintptr_t faultAddr);   // set before the handler installs

static void segvHandler(int sig, siginfo_t *si, void *ucontext)
{
	(void) sig;
	(void) ucontext;
	// No si_code filter: a fault in a reserved-but-PROT_NONE window is
	// SEGV_ACCERR natively, but qemu-user (the ppc64 bring-up vehicle)
	// reports it as SEGV_MAPERR. The callback itself validates the fault
	// address against the fiber stack reservations, so wild accesses still
	// fall through to the fatal path below.
	if (gSegvCallback((uintptr_t) si->si_addr)) {
		return; // consumed (e.g. stack grown) -> retry the faulting instruction
	}
	static const char msg[] = "fatal: stack overflow past reservation / invalid memory access\n";
	ssize_t w = write(2, msg, sizeof(msg) - 1);
	(void) w;
	signal(SIGSEGV, SIG_DFL); // return re-faults into the default handler -> core at real PC
}

void osInstallSegvHandler(_Bool (*cb)(uintptr_t faultAddr))
{
	if (gAltStack == NULL) {
		size_t altSize = 32 * 1024; // handler is tiny; SIGSTKSZ headroom
		gAltStack = malloc(altSize);
		if (gAltStack != NULL) {
			stack_t ss;
			ss.ss_sp = gAltStack;
			ss.ss_size = altSize;
			ss.ss_flags = 0;
			sigaltstack(&ss, NULL); // per-thread
		}
	}
	gSegvCallback = cb;
	if (__sync_bool_compare_and_swap(&gSegvHandlerInstalled, 0, 1)) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = segvHandler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // NOT SA_NODEFER / SA_RESETHAND
		sigaction(SIGSEGV, &sa, NULL);         // process-global, once
	}
}
#else
// A sanitizer owns SIGSEGV and intercepts mmap/mprotect; the growable-stack
// machinery is disabled under sanitizers (stacks are fully committed instead).
void osInstallSegvHandler(_Bool (*cb)(uintptr_t faultAddr))
{
	(void) cb;
}
#endif


void osIgnoreBrokenPipe(void)
{
	signal(SIGPIPE, SIG_IGN);
}
