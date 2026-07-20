#ifndef SOCKET_H
#define SOCKET_H

#include "core/Object.h"
#include "os/Os.h"
#include <stdint.h>

typedef struct {
	OBJECT_HEADER;
	Value descriptor;
} RawServerSocket;
OBJECT_HANDLE(ServerSocket);

typedef struct {
	OBJECT_HEADER;
	Value address;
} RawInternetAddress;
OBJECT_HANDLE(InternetAddress);

// Fiber-parking socket operations (retry policy over vm/os/OsSocket.h).
// All answer OS_FD_INVALID on failure.
OsFd socketConnect(uint32_t ip, uint16_t port);
OsFd socketBind(uint32_t ip, uint16_t port, int backlog);
OsFd socketAccept(OsFd listener);
uint32_t socketHostLookup(char *host, const char **error);

#endif
