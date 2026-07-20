// Thread spawn/join for the Linux OsThread contract. The hot mutex/cond
// wrappers are static inline in OsThreadImpl.h; only thread lifetime needs
// to be out of line (the void-returning entry thunk).
#include "os/OsThread.h"

_Static_assert(sizeof(OsMutex) == sizeof(pthread_mutex_t),
	"OsMutex must stay layout-identical to pthread_mutex_t (Heap embeds it by value)");
_Static_assert(sizeof(OsCond) == sizeof(pthread_cond_t),
	"OsCond must stay layout-identical to pthread_cond_t");


static void *osThreadThunk(void *arg)
{
	OsThread *thread = arg;
	thread->entry(thread->arg);
	return NULL;
}


_Bool osThreadSpawn(OsThread *thread, void (*entry)(void *), void *arg)
{
	thread->entry = entry;
	thread->arg = arg;
	return pthread_create(&thread->impl, NULL, osThreadThunk, thread) == 0;
}


void osThreadJoin(OsThread *thread)
{
	pthread_join(thread->impl, NULL);
}
