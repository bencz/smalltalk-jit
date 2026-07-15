#include "runtime/Socket.h"
#include "concurrency/Scheduler.h"
#include "core/Assert.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>


// Put a descriptor into non-blocking mode so its I/O parks the fiber (via the
// scheduler's epoll loop) instead of stalling the whole VM.
void socketSetNonBlocking(int descriptor)
{
	int flags = fcntl(descriptor, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	}
}


// Disable Nagle's algorithm so small HTTP responses are sent immediately instead
// of being coalesced. Applied automatically to every connected/accepted socket.
void socketSetNoDelay(int descriptor)
{
	int one = 1;
	setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}


int socketConnect(uint32_t ip, uint16_t port)
{
	int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address;

	if (descriptor < 0) {
		return -1;
	}
	socketSetNonBlocking(descriptor);
	socketSetNoDelay(descriptor);

	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	memcpy(&address.sin_addr, &ip, sizeof(ip));

	int result = connect(descriptor, (struct sockaddr *) &address, sizeof(address));
	if (result != 0 && errno == EINPROGRESS) {
		// connection in progress: park until the socket is writable, then check
		schedulerWaitFd(descriptor, 1);
		int error = 0;
		socklen_t len = sizeof(error);
		if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
			close(descriptor);
			return -1;
		}
		return descriptor;
	}
	if (result != 0) {
		close(descriptor);
		return -1;
	}
	return descriptor;
}


int socketBind(uint32_t ip, uint16_t port, int backlog)
{
	int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in address;

	if (descriptor < 0) {
		return -1;
	}

	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	memcpy(&address.sin_addr, &ip, sizeof(ip));
	//address.sin_addr.s_addr = INADDR_ANY;

	int reuse = 1;
	setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	if (bind(descriptor, (struct sockaddr *) &address, sizeof(address)) != 0) {
		close(descriptor);
		return -1;
	}
	if (listen(descriptor, backlog) != 0) {
		close(descriptor);
		return -1;
	}
	socketSetNonBlocking(descriptor);
	return descriptor;
}


// Accept a connection, parking the fiber until one arrives. The returned
// client descriptor is itself non-blocking.
int socketAccept(int descriptor)
{
	for (;;) {
		int client = accept(descriptor, NULL, 0);
		if (client >= 0) {
			socketSetNonBlocking(client);
			socketSetNoDelay(client);
			return client;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			schedulerWaitFd(descriptor, 0);
			continue;
		}
		if (errno == EINTR) {
			continue;
		}
		return -1;
	}
}


uint32_t socketHostLookup(char *host, const char **error)
{
	struct addrinfo hints;
	struct addrinfo *addr;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

	int result = getaddrinfo(host, NULL, &hints, &addr);
	if (result == 0) {
		ASSERT(addr->ai_addr->sa_family == AF_INET);
		uint32_t ip;
		memcpy(&ip, &((struct sockaddr_in *) addr->ai_addr)->sin_addr, sizeof(ip));
		*error = NULL;
		freeaddrinfo(addr);
		return ip;
	} else {
		*error = gai_strerror(result);
		return 0;
	}
}
