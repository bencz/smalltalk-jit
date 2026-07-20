#ifndef OS_THREAD_IMPL_H
#define OS_THREAD_IMPL_H
#define OS_THREAD_IMPL 1

// Linux/POSIX types for the OsThread contract (vm/os/OsThread.h): thin
// by-value wrappers over pthreads. Layout-identical to the native types
// (canary _Static_asserts in OsThread.c), so embedding structs (Heap,
// Scheduler) keep their exact field offsets, and every inline below compiles
// to the bare pthread call.

#include <pthread.h>

typedef struct {
	pthread_mutex_t impl;
} OsMutex;

typedef struct {
	pthread_cond_t impl;
} OsCond;

typedef struct {
	pthread_t impl;
	void (*entry)(void *);
	void *arg;
} OsThread;

#define OS_MUTEX_STATIC_INIT { PTHREAD_MUTEX_INITIALIZER }
#define OS_COND_STATIC_INIT { PTHREAD_COND_INITIALIZER }


static inline void osMutexInit(OsMutex *mutex)
{
	pthread_mutex_init(&mutex->impl, NULL);
}


static inline void osMutexDestroy(OsMutex *mutex)
{
	pthread_mutex_destroy(&mutex->impl);
}


static inline void osMutexLock(OsMutex *mutex)
{
	pthread_mutex_lock(&mutex->impl);
}


static inline void osMutexUnlock(OsMutex *mutex)
{
	pthread_mutex_unlock(&mutex->impl);
}


static inline void osCondInit(OsCond *cond)
{
	pthread_cond_init(&cond->impl, NULL);
}


static inline void osCondDestroy(OsCond *cond)
{
	pthread_cond_destroy(&cond->impl);
}


static inline void osCondWait(OsCond *cond, OsMutex *mutex)
{
	pthread_cond_wait(&cond->impl, &mutex->impl);
}


static inline void osCondBroadcast(OsCond *cond)
{
	pthread_cond_broadcast(&cond->impl);
}

#endif
