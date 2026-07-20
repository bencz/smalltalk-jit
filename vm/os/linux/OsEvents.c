// Linux readiness event loop (vm/os/Os.h): epoll plus a wake-eventfd that
// kicks the sole blocked waiter without surfacing as a caller event.
#include "os/Os.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>


struct OsEventLoop {
	int epollFd;
	// eventfd registered level-triggered with the reserved tag: writing to it
	// kicks the sole blocked waiter out of epoll_wait; the wait loop drains it
	// internally and never surfaces it as a caller tag.
	int wakeFd;
};

OsEventLoop *osEventLoopCreate(void)
{
	OsEventLoop *loop = calloc(1, sizeof(OsEventLoop));
	loop->epollFd = epoll_create1(0);
	if (loop->epollFd < 0) {
		free(loop);
		return NULL;
	}
	loop->wakeFd = eventfd(0, EFD_NONBLOCK);
	if (loop->wakeFd >= 0) {
		struct epoll_event wev;
		wev.events = EPOLLIN;
		wev.data.u64 = OS_EVENT_TAG_RESERVED;
		epoll_ctl(loop->epollFd, EPOLL_CTL_ADD, loop->wakeFd, &wev);
	}
	return loop;
}


void osEventLoopArm(OsEventLoop *loop, OsFd fd, _Bool forWrite, uint64_t tag)
{
	struct epoll_event ev;
	ev.events = (forWrite ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
	ev.data.u64 = tag;
	// Re-arm if the fd is already known to epoll, otherwise register it.
	if (epoll_ctl(loop->epollFd, EPOLL_CTL_MOD, fd, &ev) < 0 && errno == ENOENT) {
		epoll_ctl(loop->epollFd, EPOLL_CTL_ADD, fd, &ev);
	}
}


void osEventLoopDisarm(OsEventLoop *loop, OsFd fd)
{
	epoll_ctl(loop->epollFd, EPOLL_CTL_DEL, fd, NULL);
}


int osEventLoopWait(OsEventLoop *loop, uint64_t *tags, int maxTags, int timeoutMs)
{
	struct epoll_event events[64];
	if (maxTags > 64) {
		maxTags = 64;
	}
	int n = epoll_wait(loop->epollFd, events, maxTags, timeoutMs);
	int count = 0;
	for (int i = 0; i < n; i++) {
		if (events[i].data.u64 == OS_EVENT_TAG_RESERVED) {
			// Just a kick to make the waiter re-check its queues: drain the
			// eventfd counter and move on — it is not a caller's waiter.
			uint64_t drain;
			ssize_t r = read(loop->wakeFd, &drain, sizeof(drain));
			(void) r;
			continue;
		}
		tags[count++] = events[i].data.u64;
	}
	return count;
}


void osEventLoopWake(OsEventLoop *loop)
{
	if (loop->wakeFd >= 0) {
		uint64_t one = 1;
		ssize_t w = write(loop->wakeFd, &one, sizeof(one));
		(void) w;
	}
}
