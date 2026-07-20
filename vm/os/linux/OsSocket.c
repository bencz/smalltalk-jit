// Linux/BSD-sockets implementation of the OsSocket contract
// (vm/os/OsSocket.h). Pure syscall wrapping: readiness waiting and retry
// loops live in the VM (vm/runtime/Socket.c), which parks fibers on the
// scheduler — never here.
#include "os/OsSocket.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>


static OsIoStatus statusFromErrno(void)
{
	if (errno == EAGAIN || errno == EWOULDBLOCK) {
		return OS_IO_WOULD_BLOCK;
	}
	if (errno == EINTR) {
		return OS_IO_INTERRUPTED;
	}
	return OS_IO_ERROR;
}


static void socketSetNonBlockingFd(int descriptor)
{
	int flags = fcntl(descriptor, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	}
}


static void socketSetNoDelayFd(int descriptor)
{
	int one = 1;
	setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}


static struct sockaddr_in socketAddress(uint32_t ip, uint16_t port)
{
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	memcpy(&address.sin_addr, &ip, sizeof(ip));
	return address;
}


OsIoStatus osSocketConnectBegin(uint32_t ip, uint16_t port, OsFd *fd)
{
	int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (descriptor < 0) {
		return OS_IO_ERROR;
	}
	socketSetNonBlockingFd(descriptor);
	socketSetNoDelayFd(descriptor);

	struct sockaddr_in address = socketAddress(ip, port);
	if (connect(descriptor, (struct sockaddr *) &address, sizeof(address)) == 0) {
		*fd = descriptor;
		return OS_IO_OK;
	}
	if (errno == EINPROGRESS) {
		*fd = descriptor;
		return OS_IO_WOULD_BLOCK;
	}
	int saved = errno;
	close(descriptor);
	errno = saved;
	return OS_IO_ERROR;
}


OsIoStatus osSocketConnectFinish(OsFd fd)
{
	int error = 0;
	socklen_t length = sizeof(error);
	if (getsockopt((int) fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
		return OS_IO_ERROR;
	}
	if (error != 0) {
		errno = error;
		return OS_IO_ERROR;
	}
	return OS_IO_OK;
}


OsIoStatus osSocketListen(uint32_t ip, uint16_t port, int backlog, OsFd *fd)
{
	int descriptor = socket(AF_INET, SOCK_STREAM, 0);
	if (descriptor < 0) {
		return OS_IO_ERROR;
	}
	int reuse = 1;
	setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in address = socketAddress(ip, port);
	if (bind(descriptor, (struct sockaddr *) &address, sizeof(address)) != 0
			|| listen(descriptor, backlog) != 0) {
		int saved = errno;
		close(descriptor);
		errno = saved;
		return OS_IO_ERROR;
	}
	socketSetNonBlockingFd(descriptor);
	*fd = descriptor;
	return OS_IO_OK;
}


OsIoStatus osSocketAccept(OsFd listener, OsFd *client)
{
	int descriptor = accept((int) listener, NULL, 0);
	if (descriptor < 0) {
		return statusFromErrno();
	}
	socketSetNonBlockingFd(descriptor);
	socketSetNoDelayFd(descriptor);
	*client = descriptor;
	return OS_IO_OK;
}


OsIoStatus osSocketRead(OsFd fd, void *buffer, size_t size, size_t *bytesRead)
{
	ssize_t result = read((int) fd, buffer, size);
	if (result < 0) {
		return statusFromErrno();
	}
	*bytesRead = (size_t) result;
	return OS_IO_OK;
}


OsIoStatus osSocketWrite(OsFd fd, const void *buffer, size_t size, size_t *bytesWritten)
{
	ssize_t result = write((int) fd, buffer, size);
	if (result < 0) {
		return statusFromErrno();
	}
	*bytesWritten = (size_t) result;
	return OS_IO_OK;
}


OsIoStatus osSocketSetNoDelay(OsFd fd)
{
	socketSetNoDelayFd((int) fd);
	return OS_IO_OK;
}


void osSocketClose(OsFd fd)
{
	close((int) fd);
}


_Bool osSocketHostLookup(const char *host, uint32_t *ip, char *errorBuffer, size_t errorBufferSize)
{
	struct addrinfo hints;
	struct addrinfo *info;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	int result = getaddrinfo(host, NULL, &hints, &info);
	if (result != 0) {
		snprintf(errorBuffer, errorBufferSize, "%s", gai_strerror(result));
		return 0;
	}
	memcpy(ip, &((struct sockaddr_in *) info->ai_addr)->sin_addr, sizeof(*ip));
	freeaddrinfo(info);
	return 1;
}
