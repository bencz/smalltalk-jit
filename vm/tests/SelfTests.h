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

#endif
