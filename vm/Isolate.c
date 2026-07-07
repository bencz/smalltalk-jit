#include "Isolate.h"
#include "Thread.h"
#include "Handle.h"
#include "Scheduler.h"
#include "Snapshot.h"
#include "Entry.h"
#include "Object.h"
#include <pthread.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- cross-isolate inbox transport --------------------------------------
//
// Each isolate owns one inbox in the shared `gIsolates` registry: an eventfd
// plus a mutex-protected singly-linked MPSC queue of message buffers. Any OS
// thread posts a buffer with isolatePostBytes(target,..) — it copies the bytes
// under the target's lock and pokes the target's eventfd. A consumer fiber in
// the target isolate waits on isolateInboxFd() (via the ordinary scheduler
// fd-wait) and drains with isolateInboxPop(). The VM never interprets the bytes.

typedef struct InboxMsg {
	struct InboxMsg *next;
	size_t size;
	uint8_t bytes[];
} InboxMsg;

typedef struct {
	int eventFd;               // >0 once initialised
	volatile int ready;        // set after full init; peers spin on this
	pthread_mutex_t mutex;
	InboxMsg *head;
	InboxMsg *tail;
} IsolateInbox;

static IsolateInbox gIsolates[MAX_ISOLATES];       // SHARED across all isolates
static PER_ISOLATE int gMyIsolateId = 0;

int isolateCurrentId(void)
{
	return gMyIsolateId;
}

void isolateInboxInit(int id)
{
	gMyIsolateId = id;
	IsolateInbox *inbox = &gIsolates[id];
	pthread_mutex_init(&inbox->mutex, NULL);
	inbox->head = inbox->tail = NULL;
	inbox->eventFd = eventfd(0, EFD_NONBLOCK);
	// Release store, paired with the acquire loads below: a peer that observes
	// ready==1 is guaranteed to see the initialised mutex/eventfd/queue too (the
	// bare `volatile` gives no such ordering on weakly-ordered CPUs).
	__atomic_store_n(&inbox->ready, 1, __ATOMIC_RELEASE);
}

int isolatePostBytes(int target, const uint8_t *bytes, size_t size)
{
	if (target < 0 || target >= MAX_ISOLATES
	    || !__atomic_load_n(&gIsolates[target].ready, __ATOMIC_ACQUIRE)) {
		return -1;
	}
	InboxMsg *msg = malloc(sizeof(InboxMsg) + size);
	if (msg == NULL) {
		return -1;
	}
	msg->next = NULL;
	msg->size = size;
	memcpy(msg->bytes, bytes, size);

	IsolateInbox *inbox = &gIsolates[target];
	pthread_mutex_lock(&inbox->mutex);
	if (inbox->tail == NULL) {
		inbox->head = inbox->tail = msg;
	} else {
		inbox->tail->next = msg;
		inbox->tail = msg;
	}
	pthread_mutex_unlock(&inbox->mutex);

	uint64_t one = 1;
	ssize_t w = write(inbox->eventFd, &one, sizeof(one)); // wake the target's waiter
	(void) w;
	return 0;
}

int isolateInboxPop(uint8_t **outBytes, size_t *outSize)
{
	IsolateInbox *inbox = &gIsolates[gMyIsolateId];
	pthread_mutex_lock(&inbox->mutex);
	InboxMsg *msg = inbox->head;
	if (msg != NULL) {
		inbox->head = msg->next;
		if (inbox->head == NULL) {
			inbox->tail = NULL;
		}
	}
	pthread_mutex_unlock(&inbox->mutex);
	if (msg == NULL) {
		return 0;
	}
	uint8_t *buf = malloc(msg->size ? msg->size : 1);
	if (buf == NULL) {
		free(msg); // out of memory: drop the message rather than deref NULL
		return 0;
	}
	memcpy(buf, msg->bytes, msg->size);
	*outBytes = buf;
	*outSize = msg->size;
	free(msg);
	return 1;
}

int isolateInboxFd(void)
{
	return gIsolates[gMyIsolateId].eventFd;
}

void isolateInboxClearSignal(void)
{
	uint64_t counter;
	ssize_t r = read(gIsolates[gMyIsolateId].eventFd, &counter, sizeof(counter));
	(void) r; // EAGAIN when already drained is fine (eventfd is non-blocking)
}

// ---- isolate lifecycle (runtime worker spawn) ---------------------------
//
// A program on the main isolate spawns WORKER isolates at runtime with
// isolateSpawn (`Isolate spawn:` in Smalltalk) — like Thread/Worker/Ractor, NOT
// a second copy of the process. Each worker is a fresh VM on its own OS thread
// that reloads the shared image and runs a source program; the spawner keeps
// running. Ids are handed out atomically (main = 0, workers = 1,2,...). Spawned
// threads are tracked so the process can join them at exit.

static const char *gSnapshotPath = NULL;              // image workers reload (shared code)
static int gNextIsolateId = 1;                        // 0 is the main isolate
static pthread_mutex_t gSpawnMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gWorkerThreads[MAX_ISOLATES];
static int gWorkerCount = 0;

void isolateSetSnapshotPath(const char *path)
{
	gSnapshotPath = path;
}

typedef struct {
	int id;
	char *source; // owned; freed by the worker thread
} WorkerArg;

// Runs as the worker's top-level fiber. Source (not a live block) crosses the
// isolate boundary, so a worker can define classes and run blocks freshly.
static void workerProgram(void *arg)
{
	WorkerArg *wa = arg;
	Value result;
	parseSourceAndInitialize(wa->source, &result);
}

// pthread entry for a worker isolate: a full independent VM boot + its inbox.
static void *isolateWorkerMain(void *arg)
{
	WorkerArg *wa = arg;

	initThread(&CurrentThread);

	FILE *snapshot = gSnapshotPath != NULL ? fopen(gSnapshotPath, "r") : NULL;
	if (snapshot == NULL) {
		fprintf(stderr, "isolate %d: cannot open snapshot '%s'\n",
			wa->id, gSnapshotPath ? gSnapshotPath : "(null)");
		free(wa->source);
		free(wa);
		return NULL;
	}
	snapshotRead(snapshot);
	fclose(snapshot);

	schedulerInit();
	isolateInboxInit(wa->id);
	schedulerSpawnC(workerProgram, wa, 0);
	schedulerRun(); // exits once the worker's fibers (incl. any receiver) drain

	freeHandles();
	freeThread(&CurrentThread);
	free(wa->source);
	free(wa);
	return NULL;
}

int isolateSpawn(const char *source)
{
	pthread_mutex_lock(&gSpawnMutex);
	if (gNextIsolateId >= MAX_ISOLATES) {
		pthread_mutex_unlock(&gSpawnMutex);
		return -1;
	}
	int id = gNextIsolateId++;

	WorkerArg *wa = malloc(sizeof(WorkerArg));
	wa->id = id;
	wa->source = strdup(source);

	pthread_t thread;
	if (pthread_create(&thread, NULL, isolateWorkerMain, wa) != 0) {
		gNextIsolateId--; // hand the id back
		pthread_mutex_unlock(&gSpawnMutex);
		fprintf(stderr, "isolate %d: pthread_create failed\n", id);
		free(wa->source);
		free(wa);
		return -1;
	}
	gWorkerThreads[gWorkerCount++] = thread;
	pthread_mutex_unlock(&gSpawnMutex);
	return id;
}

void isolateJoinWorkers(void)
{
	// Re-read the count each iteration so workers that spawn sub-workers are
	// joined too; read each handle under the lock, join outside it.
	int joined = 0;
	for (;;) {
		pthread_mutex_lock(&gSpawnMutex);
		if (joined >= gWorkerCount) {
			pthread_mutex_unlock(&gSpawnMutex);
			break;
		}
		pthread_t thread = gWorkerThreads[joined];
		pthread_mutex_unlock(&gSpawnMutex);
		pthread_join(thread, NULL);
		joined++;
	}
}

// ---- transport self-test ------------------------------------------------
// Pure C (no heap/image): proves the eventfd wakeup + MPSC queue deliver buffers
// from one OS thread to another isolate's inbox, in order, via the pop API.

typedef struct { int id; int count; int ordered; } TransportTestArg;

static void *transportReceiver(void *arg)
{
	TransportTestArg *ta = arg;
	isolateInboxInit(ta->id);
	int count = 0, expect = 0, ordered = 1;
	for (;;) {
		uint8_t *bytes;
		size_t size;
		if (isolateInboxPop(&bytes, &size)) {
			if (size >= 1 && bytes[0] == 0) { free(bytes); break; } // stop sentinel
			int seq = (size >= 5)
				? (bytes[1] | (bytes[2] << 8) | (bytes[3] << 16) | (bytes[4] << 24))
				: -1;
			if (seq != expect) ordered = 0;
			expect++; count++;
			free(bytes);
		} else {
			struct pollfd pfd = { .fd = isolateInboxFd(), .events = POLLIN };
			poll(&pfd, 1, -1);          // block until a buffer arrives
			isolateInboxClearSignal();  // drain the eventfd counter
		}
	}
	ta->count = count;
	ta->ordered = ordered;
	return NULL;
}

int isolateTransportSelfTest(void)
{
	const int N = 1000;
	pthread_t recv;
	TransportTestArg ta = { .id = 1, .count = 0, .ordered = 0 };
	pthread_create(&recv, NULL, transportReceiver, &ta);

	while (!__atomic_load_n(&gIsolates[1].ready, __ATOMIC_ACQUIRE)) { /* wait for the receiver's inbox */ }

	for (int i = 0; i < N; i++) {
		uint8_t buf[8] = { 1, (uint8_t) i, (uint8_t) (i >> 8), (uint8_t) (i >> 16), (uint8_t) (i >> 24), 0xAA, 0xBB, 0xCC };
		isolatePostBytes(1, buf, sizeof(buf));
	}
	uint8_t stop[1] = { 0 };
	isolatePostBytes(1, stop, 1);

	pthread_join(recv, NULL);

	fprintf(stderr, "transport self-test: received=%d/%d ordered=%d\n", ta.count, N, ta.ordered);
	return (ta.count == N && ta.ordered) ? 0 : 1;
}
