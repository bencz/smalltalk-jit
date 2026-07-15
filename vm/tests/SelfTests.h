#ifndef SELF_TESTS_H
#define SELF_TESTS_H

// Runtime-dispatched C self-test battery (ST_*_TEST env vars). Returns the
// test's exit code, or -1 when no self-test was requested. `bootstrap` loads
// or builds the image for the tests that need one (main.c passes its own
// snapshot/bootstrap routine).
int selfTestFromEnv(char *snapshotFileName, char *bootstrapDir,
	void (*bootstrap)(char *snapshotFileName, char *bootstrapDir));

// Golden-byte emission test for the selected backend's ABI-sensitive emitters;
// provided by the arch's golden TU (vm/tests/EmitGolden<Arch>.c, wired via
// vmArchTestSources in CMakeLists.txt). mode "print" regenerates the vectors.
int abiEmitGoldenSelfTest(const char *mode);

// The ppc64 (big-endian) golden, runnable on ANY build host (the encoders
// emit explicitly big-endian words — nothing executes): the ppc64 port's
// rung-1 bring-up vehicle, developed natively on x86. On ppc64 builds
// EmitGoldenPpc64Bind.c aliases abiEmitGoldenSelfTest to it; ppc64le builds
// get a stub (separate backend).
int ppc64EmitGoldenSelfTest(const char *mode);

#endif
