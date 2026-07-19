#ifndef PROJECT_H
#define PROJECT_H

// CLI-side plumbing for the st project subcommands (main.c): locate the
// project root, decide staleness from .stbuild/build.deps, and shuttle eval
// results across the C boundary. All project SEMANTICS (manifest evaluation,
// dependency graph, entry point, scaffolding) live in-image in
// smalltalk/Packages/; this file only walks directories, stats files and
// copies strings, keeping the VM generic.

#include "core/Entry.h"
#include "core/Handle.h"
#include "core/Smalltalk.h"
#include "runtime/String.h"
#include "tools/Snapshot.h"
#include "vm/os/Os.h"
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PROJECT_MANIFEST "package.st"
#define PROJECT_BUILD_DIR ".stbuild"
#define PROJECT_IMAGE "program.img"
#define PROJECT_DEPS "build.deps"
#define PROJECT_PATH_MAX 4096

typedef struct {
	_Bool hasProject;
	_Bool stale;
	_Bool force;
	char root[PROJECT_PATH_MAX];
	char image[PROJECT_PATH_MAX];
} ProjectPlan;


// Walk upward from the working directory to the first dir with a package.st.
static _Bool projectFindRoot(char *buffer, size_t size)
{
	if (getcwd(buffer, size) == NULL) {
		return 0;
	}
	for (;;) {
		size_t length = strlen(buffer);
		char probe[length + sizeof(PROJECT_MANIFEST) + 1];
		struct stat info;
		snprintf(probe, sizeof(probe), "%s/%s", buffer, PROJECT_MANIFEST);
		if (stat(probe, &info) == 0) {
			return 1;
		}
		char *slash = strrchr(buffer, '/');
		if (slash == NULL || slash == buffer) {
			return 0; // reached the filesystem root without a manifest
		}
		*slash = '\0';
	}
}


static void projectBuildPath(char *out, size_t size, const char *root, const char *tail)
{
	snprintf(out, size, "%s/%s/%s", root, PROJECT_BUILD_DIR, tail);
}


// Same epoch-millis derivation as FileStatPrimitive (vm/runtime/FileSystem.c),
// so the C probe agrees exactly with the in-image build.deps writer.
// -1 when the path does not stat.
static int64_t projectMtimeMillis(const char *path)
{
	struct stat info;
	if (stat(path, &info) != 0) {
		return -1;
	}
	return (int64_t) info.st_mtim.tv_sec * 1000 + info.st_mtim.tv_nsec / 1000000;
}


// Stale when: the project image is missing or its STIM header is rejected
// (VM upgrade across format versions), the image predates the st binary or
// the base image (conservative guard for VM/core changes that keep the
// format), the deps file is missing, or any recorded file's mtime moved.
// build.deps lines are "<mtimeMillis><TAB><absolutePath>".
static _Bool projectIsStale(const char *root, const char *baseImage)
{
	char imagePath[PROJECT_PATH_MAX];
	char depsPath[PROJECT_PATH_MAX];
	projectBuildPath(imagePath, sizeof(imagePath), root, PROJECT_IMAGE);
	projectBuildPath(depsPath, sizeof(depsPath), root, PROJECT_DEPS);

	int64_t imageMtime = projectMtimeMillis(imagePath);
	if (imageMtime < 0) {
		return 1;
	}
	FILE *image = fopen(imagePath, "r");
	if (image == NULL) {
		return 1;
	}
	char err[256];
	int headerBad = snapshotCheckHeader(image, err, sizeof(err));
	fclose(image);
	if (headerBad != 0) {
		return 1;
	}

	char exe[PROJECT_PATH_MAX];
	if (osExecutablePath(exe, sizeof(exe)) && projectMtimeMillis(exe) > imageMtime) {
		return 1;
	}
	if (baseImage != NULL) {
		int64_t baseMtime = projectMtimeMillis(baseImage);
		if (baseMtime >= 0 && baseMtime > imageMtime) {
			return 1;
		}
	}

	FILE *deps = fopen(depsPath, "r");
	if (deps == NULL) {
		return 1;
	}
	char line[PROJECT_PATH_MAX + 32];
	while (fgets(line, sizeof(line), deps) != NULL) {
		char *tab = strchr(line, '\t');
		if (tab == NULL) {
			continue;
		}
		*tab = '\0';
		char *path = tab + 1;
		path[strcspn(path, "\n")] = '\0';
		if (strtoll(line, NULL, 10) != projectMtimeMillis(path)) {
			fclose(deps);
			return 1;
		}
	}
	fclose(deps);
	return 0;
}


// Evaluate `code` and copy its String result out. Returns 1 when the result
// was a String/Symbol; 0 otherwise (buffer untouched). The bytes are copied
// before any further allocation, so the raw pointer cannot move under us
// (the Repl.c autocomplete pattern).
static _Bool projectEvalToCString(char *code, char *buffer, size_t size)
{
	HandleScope scope;
	openHandleScope(&scope);
	Value value = evalObject(code);
	if (!valueTypeOf(value, VALUE_POINTER)) {
		closeHandleScope(&scope, NULL);
		return 0;
	}
	Object *object = scopeHandle(asObject(value));
	if (object->raw->class != Handles.String->raw
			&& object->raw->class != Handles.Symbol->raw) {
		closeHandleScope(&scope, NULL);
		return 0;
	}
	RawString *raw = (RawString *) object->raw;
	size_t length = (size_t) raw->size < size - 1 ? (size_t) raw->size : size - 1;
	memcpy(buffer, raw->contents, length);
	buffer[length] = '\0';
	closeHandleScope(&scope, NULL);
	return 1;
}


// Evaluate `code` expecting an integer exit code; EXIT_FAILURE otherwise.
static int projectEvalToInt(char *code)
{
	Value value = evalObject(code);
	return valueTypeOf(value, VALUE_INT) ? (int) asCInt(value) : EXIT_FAILURE;
}

#endif
