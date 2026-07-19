#include "runtime/FileSystem.h"
#include "runtime/String.h"
#include "core/Handle.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


_Bool fsStatPath(const char *path, int64_t info[3])
{
	struct stat status;
	if (stat(path, &status) != 0) {
		return 0;
	}
	info[0] = S_ISREG(status.st_mode) ? 1 : S_ISDIR(status.st_mode) ? 2 : 3;
	info[1] = (int64_t) status.st_size;
	info[2] = (int64_t) status.st_mtim.tv_sec * 1000 + status.st_mtim.tv_nsec / 1000000;
	return 1;
}


_Bool fsMkdir(const char *path)
{
	return mkdir(path, 0777) == 0;
}


_Bool fsUnlink(const char *path)
{
	return unlink(path) == 0;
}


_Bool fsRmdir(const char *path)
{
	return rmdir(path) == 0;
}


_Bool fsRename(const char *from, const char *to)
{
	return rename(from, to) == 0;
}


_Bool fsChdir(const char *path)
{
	return chdir(path) == 0;
}


_Bool fsGetCwd(char *buffer, size_t capacity)
{
	return getcwd(buffer, capacity) != NULL;
}


_Bool fsRealpath(const char *path, char *resolved)
{
	return realpath(path, resolved) != NULL;
}


OrderedCollection *fsListDir(const char *path)
{
	DIR *directory = opendir(path);
	if (directory == NULL) {
		return NULL;
	}

	OrderedCollection *result = newOrdColl(16);
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		HandleScope scope;
		openHandleScope(&scope);
		// asString may GC; `result` is a handle and readdir state is C memory
		ordCollAddObject(result, (Object *) asString(entry->d_name));
		closeHandleScope(&scope, NULL);
	}
	closedir(directory);
	return result;
}
