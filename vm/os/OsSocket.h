#ifndef OS_SOCKET_H
#define OS_SOCKET_H

#include "Os.h"

// The socket slice of the OS seam: TCP/IPv4, 1:1 with what the VM uses
// (vm/runtime/Socket.c and the socket primitives). Every socket handed back
// is NON-BLOCKING (and NODELAY where noted). This layer never blocks on
// readiness and never calls the scheduler — see OsIoStatus in Os.h: it
// answers OS_IO_WOULD_BLOCK and the CALLER parks the fiber on schedulerWaitFd
// and retries, which keeps vm/os/ scheduler-free.

// OK: connected immediately. WOULD_BLOCK: in progress — wait until fd is
// WRITABLE, then osSocketConnectFinish. ERROR: nothing was left open
// (*fd untouched). The new socket comes back non-blocking + nodelay.
OsIoStatus osSocketConnectBegin(uint32_t ip, uint16_t port, OsFd *fd);

// The pending-error check after writability; ERROR => the caller closes fd.
OsIoStatus osSocketConnectFinish(OsFd fd);

// Bound + listening (SO_REUSEADDR), non-blocking. ERROR: nothing left open.
OsIoStatus osSocketListen(uint32_t ip, uint16_t port, int backlog, OsFd *fd);

// The accepted client comes back non-blocking + nodelay. WOULD_BLOCK: wait
// until the LISTENER is readable and retry.
OsIoStatus osSocketAccept(OsFd listener, OsFd *client);

// One read attempt. OK with *bytesRead == 0 means the peer closed (EOF).
OsIoStatus osSocketRead(OsFd fd, void *buffer, size_t size, size_t *bytesRead);

// ONE write attempt (short writes are normal); the write-fully loop stays in
// the VM, parked on writability between attempts.
OsIoStatus osSocketWrite(OsFd fd, const void *buffer, size_t size, size_t *bytesWritten);

OsIoStatus osSocketSetNoDelay(OsFd fd);

void osSocketClose(OsFd fd);

// Resolve a host name to an IPv4 address. getaddrinfo has its OWN error
// namespace (not errno/osLastError), so failures answer 0 with the
// human-readable reason copied into errorBuffer.
_Bool osSocketHostLookup(const char *host, uint32_t *ip, char *errorBuffer, size_t errorBufferSize);

#endif
