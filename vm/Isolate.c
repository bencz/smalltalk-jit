#include "Isolate.h"
#include "Thread.h"
#include "Handle.h"
#include "Scheduler.h"
#include "Snapshot.h"
#include "Entry.h"
#include "Object.h"
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- cross-isolate inbox (Phase 2 transport) ----------------------------
//
// Each isolate owns one inbox in the shared `gIsolates` registry: an eventfd it
// registers in its own epoll, plus a mutex-protected singly-linked MPSC queue of
// message buffers. Any OS thread posts a buffer with isolatePostBytes(target,..)
// — it copies the bytes under the target's lock and pokes the target's eventfd,
// which wakes the target scheduler's epoll loop into isolateDrainInbox().

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
static PER_ISOLATE IsolateReceiveFn gReceiveHandler = NULL;

int isolateCurrentId(void)
{
	return gMyIsolateId;
}

void isolateSetReceiveHandler(IsolateReceiveFn handler)
{
	gReceiveHandler = handler;
}

void isolateInboxInit(int id)
{
	gMyIsolateId = id;
	IsolateInbox *inbox = &gIsolates[id];
	pthread_mutex_init(&inbox->mutex, NULL);
	inbox->head = inbox->tail = NULL;
	inbox->eventFd = eventfd(0, EFD_NONBLOCK);
	schedulerRegisterInboxFd(inbox->eventFd);
	inbox->ready = 1; // publish last: peers wait on this before posting
}

int isolatePostBytes(int target, const uint8_t *bytes, size_t size)
{
	if (target < 0 || target >= MAX_ISOLATES || !gIsolates[target].ready) {
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
	ssize_t w = write(inbox->eventFd, &one, sizeof(one)); // wake the target's epoll
	(void) w;
	return 0;
}

void isolateDrainInbox(void)
{
	IsolateInbox *inbox = &gIsolates[gMyIsolateId];
	uint64_t counter;
	ssize_t r = read(inbox->eventFd, &counter, sizeof(counter)); // clear readiness
	(void) r;

	pthread_mutex_lock(&inbox->mutex);
	InboxMsg *list = inbox->head;
	inbox->head = inbox->tail = NULL;
	pthread_mutex_unlock(&inbox->mutex);

	while (list != NULL) {
		InboxMsg *next = list->next;
		if (gReceiveHandler != NULL) {
			gReceiveHandler(-1, list->bytes, list->size);
		}
		free(list);
		list = next;
	}
}

// ---- isolate lifecycle (Phase 1) ----------------------------------------

typedef struct {
	int id;
	const char *snapshotFile;
	const char *program;
	int booted;   // 1 once the image loaded successfully
	long value;   // the doit's result, if it is a SmallInteger
} Isolate;

// Runs as the isolate's top-level fiber (fiber #0 on this OS thread).
static void isolateProgram(void *arg)
{
	Isolate *iso = arg;
	if (iso->program == NULL) {
		return;
	}
	Value result = evalCode((char *) iso->program);
	iso->value = valueTypeOf(result, VALUE_INT) ? asCInt(result) : 0;
}

// pthread entry: replicate the single-VM boot path (main.c) on a worker thread.
static void *isolateThreadMain(void *arg)
{
	Isolate *iso = arg;

	initThread(&CurrentThread);

	FILE *snapshot = fopen(iso->snapshotFile, "r");
	if (snapshot == NULL) {
		fprintf(stderr, "isolate %d: cannot open snapshot '%s'\n", iso->id, iso->snapshotFile);
		return NULL;
	}
	snapshotRead(snapshot);
	fclose(snapshot);
	iso->booted = 1;

	schedulerInit();
	// NB: the cross-isolate inbox (isolateInboxInit) is intentionally NOT started
	// here — it keeps the scheduler loop alive, which is right for an actor-hosting
	// isolate (Phase 3) but would hang this "run a program then exit" isolate.
	schedulerSpawnC(isolateProgram, iso, 0);
	schedulerRun();

	freeHandles();
	freeThread(&CurrentThread);
	return NULL;
}

int isolatesRun(int count, const char *snapshotFile, const char *program)
{
	if (count < 1) {
		count = 1;
	}
	pthread_t *threads = malloc(count * sizeof(pthread_t));
	Isolate *isolates = malloc(count * sizeof(Isolate));

	for (int i = 0; i < count; i++) {
		isolates[i] = (Isolate) {
			.id = i,
			.snapshotFile = snapshotFile,
			.program = program,
			.booted = 0,
			.value = 0,
		};
		if (pthread_create(&threads[i], NULL, isolateThreadMain, &isolates[i]) != 0) {
			fprintf(stderr, "isolate %d: pthread_create failed\n", i);
			isolates[i].booted = -1;
		}
	}

	int failures = 0;
	for (int i = 0; i < count; i++) {
		pthread_join(threads[i], NULL);
		if (isolates[i].booted != 1) {
			failures++;
		}
		fprintf(stderr, "isolate %d: booted=%d result=%ld\n",
			isolates[i].id, isolates[i].booted, isolates[i].value);
	}

	free(threads);
	free(isolates);
	return failures;
}

// ---- Phase 2 transport self-test ----------------------------------------
// Pure C: no heap/image needed — proves the eventfd wakeup + MPSC queue deliver
// buffers from one OS thread to another isolate's scheduler loop, in order.

static PER_ISOLATE int gTestCount = 0;
static PER_ISOLATE size_t gTestBytes = 0;
static PER_ISOLATE int gTestOrdered = 1;
static PER_ISOLATE int gTestExpect = 0;

static void transportTestHandler(int fromHint, const uint8_t *bytes, size_t size)
{
	(void) fromHint;
	if (size >= 1 && bytes[0] == 0) { // type 0 = stop sentinel
		schedulerStopInbox();
		return;
	}
	// type 1 = data: bytes[1..4] hold a 32-bit little-endian sequence number
	int seq = (size >= 5)
		? (bytes[1] | (bytes[2] << 8) | (bytes[3] << 16) | (bytes[4] << 24))
		: -1;
	if (seq != gTestExpect) {
		gTestOrdered = 0;
	}
	gTestExpect++;
	gTestCount++;
	gTestBytes += size;
}

typedef struct { int id; int count; int ordered; } TransportTestArg;

static void *transportReceiver(void *arg)
{
	TransportTestArg *ta = arg;
	schedulerInit();
	isolateInboxInit(ta->id);
	isolateSetReceiveHandler(transportTestHandler);
	schedulerRun(); // blocks on inbox, drains, exits when the stop sentinel arrives
	ta->count = gTestCount;
	ta->ordered = gTestOrdered;
	return NULL;
}

int isolateTransportSelfTest(void)
{
	const int N = 1000;
	pthread_t recv;
	TransportTestArg ta = { .id = 1, .count = 0, .ordered = 0 };
	pthread_create(&recv, NULL, transportReceiver, &ta);

	while (!gIsolates[1].ready) { /* wait for the receiver's inbox */ }

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
