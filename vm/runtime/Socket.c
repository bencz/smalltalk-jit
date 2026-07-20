// Fiber integration for TCP sockets: the OS work (create/connect/accept,
// non-blocking flags, the syscalls themselves) lives behind vm/os/OsSocket.h;
// this layer owns the RETRY POLICY — park the calling fiber on the scheduler's
// event loop whenever the OS answers OS_IO_WOULD_BLOCK, retry on
// OS_IO_INTERRUPTED — which is exactly the piece the OS seam must never know
// about (it never calls the scheduler).
#include "runtime/Socket.h"
#include "os/OsSocket.h"
#include "concurrency/Scheduler.h"


OsFd socketConnect(uint32_t ip, uint16_t port)
{
	OsFd fd;
	OsIoStatus status = osSocketConnectBegin(ip, port, &fd);
	if (status == OS_IO_ERROR) {
		return OS_FD_INVALID;
	}
	if (status == OS_IO_WOULD_BLOCK) {
		// connection in progress: park until the socket is writable, then check
		schedulerWaitFd(fd, 1);
		if (osSocketConnectFinish(fd) != OS_IO_OK) {
			osSocketClose(fd);
			return OS_FD_INVALID;
		}
	}
	return fd;
}


OsFd socketBind(uint32_t ip, uint16_t port, int backlog)
{
	OsFd fd;
	if (osSocketListen(ip, port, backlog, &fd) != OS_IO_OK) {
		return OS_FD_INVALID;
	}
	return fd;
}


// Accept a connection, parking the fiber until one arrives. The returned
// client descriptor is itself non-blocking (and nodelay).
OsFd socketAccept(OsFd listener)
{
	for (;;) {
		OsFd client;
		switch (osSocketAccept(listener, &client)) {
		case OS_IO_OK:
			return client;
		case OS_IO_WOULD_BLOCK:
			schedulerWaitFd(listener, 0);
			continue;
		case OS_IO_INTERRUPTED:
			continue;
		default:
			return OS_FD_INVALID;
		}
	}
}


uint32_t socketHostLookup(char *host, const char **error)
{
	static _Thread_local char errorBuffer[256];
	uint32_t ip;
	if (!osSocketHostLookup(host, &ip, errorBuffer, sizeof(errorBuffer))) {
		*error = errorBuffer;
		return 0;
	}
	*error = NULL;
	return ip;
}
