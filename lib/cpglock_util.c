/*
** Copyright (C) 2013 Red Hat, Inc. All rights reserved.
**
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU Lesser General Public License as published by
** the Free Software Foundation, either version 2.1 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with This program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

static ssize_t io_op_timeout(	struct pollfd *fds,
								ssize_t (*io_op)(int, void *, size_t),
								void *buf,
								size_t buflen,
								int timeout_ms)
{
	int ret;
	int has_timeout = timeout_ms > 0;
	size_t n = buflen;

	while (n > 0) {
		ssize_t io_bytes;
		struct timespec time_start;
		struct timespec time_end;
		int io_errno;
		int poll_ret;

		if (has_timeout) {
			if (timeout_ms <= 0) {
				errno = ETIMEDOUT;
				return -1;
			}

			do {
				ret = clock_gettime(CLOCK_MONOTONIC, &time_start);
			} while (ret == -1 && errno == EAGAIN);
		}

		do {
			poll_ret = poll(fds, 1, timeout_ms);
		} while (poll_ret == -1 && errno == EINTR);

		if (poll_ret == -1)
			return -1;

		if (poll_ret == 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		if (fds->revents & (POLLERR | POLLNVAL)) {
			errno = EPIPE;
			return -1;
		}

		io_bytes = io_op(fds->fd, buf, n);
		io_errno = errno;

		if (has_timeout) {
			uint64_t iteration_ms;

			do {
				ret = clock_gettime(CLOCK_MONOTONIC, &time_end);
			} while (ret == -1 && errno == EAGAIN);

			iteration_ms = (time_end.tv_sec - time_start.tv_sec) * 1000 +
						(time_end.tv_nsec - time_start.tv_nsec) / 1000000;
			timeout_ms -= iteration_ms;
		}

		errno = io_errno;

		if (io_bytes < 0) {
			if (io_errno == EINTR || io_errno == EAGAIN)
				continue;
			return -1;
		}

		if (io_bytes == 0 && poll_ret > 0) {
			errno = EPIPE;
			return -1;
		}

		buf = (char *) buf + io_bytes;
		n -= (size_t) io_bytes;
	}

	return (size_t) buflen;
}

ssize_t read_timeout(int fd, void *buf, size_t buflen, int timeout_ms) {
	struct pollfd fds;

	fds.fd = fd;
	fds.events = POLLIN;

	return io_op_timeout(&fds, read, buf, buflen, timeout_ms);
}

ssize_t write_timeout(int fd, void *buf, size_t buflen, int timeout_ms) {
	struct pollfd fds;
	int ret;

	fds.fd = fd;
	fds.events = POLLOUT;

	ret = io_op_timeout(&fds,
			(ssize_t (*)(int, void *, size_t)) write,
			buf, buflen, timeout_ms);
	return ret;
}

int sock_connect(const char *path, int timeout_ms) {
	int ret;
	int sock;
	int flags;
	struct pollfd pfd;
	struct sockaddr_un sun;
	int saved_errno;

	if (!path) {
		errno = EINVAL;
		return -1;
	}

	sock = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (sock < 0)
		return -1;
		
	if (strlen(path) >= sizeof(sun.sun_path)) {
		close(sock);
		errno = ENAMETOOLONG;
		return -1;
	}

	sun.sun_family = PF_LOCAL;
	strcpy(sun.sun_path, path);
	
	do {
		flags = fcntl(sock, F_GETFL, 0);
	} while (flags < 0 && errno == EINTR);

	if (flags < 0) {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

	do {
		ret = fcntl(sock, F_SETFL, flags | O_NONBLOCK);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

	do {
		ret = connect(sock, (struct sockaddr *) &sun, sizeof(sun));
	} while (ret < 0 && errno == EINTR);

	if (ret < 0 && errno != EINPROGRESS) {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

	if (ret == 0)
		return sock;

	pfd.fd = sock;
	pfd.events = POLLIN | POLLOUT;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

	if (pfd.revents & (POLLHUP | POLLNVAL | POLLERR)) {
		close(sock);
		errno = EPIPE;
		return -1;
	}

	if (ret == 0) {
		close(sock);
		errno = ETIMEDOUT;
		return -1;
	}

	return sock;		
}

void *wait_calloc(size_t len) {
	void *ret;

	do {
		ret = calloc(1, len);
		if (!ret)
			usleep(10000);
	} while (!ret);

	return ret;
}
