#ifndef SELF_TESTS_H
#define SELF_TESTS_H

// Runtime-dispatched C self-test battery (ST_*_TEST env vars). Returns the
// test's exit code, or -1 when no self-test was requested. `bootstrap` loads
// or builds the image for the tests that need one (main.c passes its own
// snapshot/bootstrap routine).
int selfTestFromEnv(char *snapshotFileName, char *bootstrapDir,
	void (*bootstrap)(char *snapshotFileName, char *bootstrapDir));

#endif
