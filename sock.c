/*
  Copyright Red Hat, Inc. 2002-2003, 2012-2013
  Copyright Mission Critical Linux, 2000

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include "sock.h"

void *
do_alloc(size_t n)
{
	void *p;

	do {
		p = calloc(1, n);
		if (!p)
			usleep(10000);
	} while (!p);

	return p;
}


/**
 * This is a wrapper around select which will retry in the case we receive
 * EINTR.  This is necessary for read_retry, since it wouldn't make sense
 * to have read_retry terminate if and only if two EINTRs were received
 * in a row - one during the read() call, one during the select call...
 *
 * See select(2) for description of parameters.
 */
int
select_retry(int fdmax, fd_set * rfds, fd_set * wfds, fd_set * xfds,
		   struct timeval *timeout)
{
	int rv;

	while (1) {
		rv = select(fdmax, rfds, wfds, xfds, timeout);
		if ((rv == -1) && (errno == EINTR))
			/* return on EBADF/EINVAL/ENOMEM; continue on EINTR */
			continue;
		return rv;
	}
}

/**
 * Retries a write in the event of a non-blocked interrupt signal.
 *
 * @param fd		File descriptor to which we are writing.
 * @param buf		Data buffer to send.
 * @param count		Number of bytes in buf to send.
 * @param timeout	(struct timeval) telling us how long we should retry.
 * @return		The number of bytes written to the file descriptor,
 * 			or -1 on error (with errno set appropriately).
 */
ssize_t
write_retry(int fd, void *buf, size_t count, struct timeval * timeout)
{
	int n, rv = 0;
	size_t total = 0;
	ssize_t remain = count;
	fd_set wfds, xfds;

	while (total < count) {
		/* Create the write FD set of 1... */
		FD_ZERO(&wfds);
		FD_SET(fd, &wfds);
		FD_ZERO(&xfds);
		FD_SET(fd, &xfds);

		/* wait for the fd to be available for writing */
		rv = select_retry(fd + 1, NULL, &wfds, &xfds, timeout);
		if (rv == -1)
			return -1;
		else if (rv == 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		if (FD_ISSET(fd, &xfds)) {
			errno = EPIPE;
			return -1;
		}

		/* 
		 * Attempt to write to fd
		 */
		n = write(fd, (char *) buf + (off_t) total, remain);

		/*
		 * When we know our fd was select()ed and we receive 0 bytes
		 * when we write, the fd was closed.
		 */
		if (n == 0 && rv == 1) {
			errno = EPIPE;
			return -1;
		}

		if (n == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				/* 
				 * Not ready?
				 */
				continue;
			}

			/* Other errors: EIO, EINVAL, etc */
			return -1;
		}

		total += n;
		remain -= n;
	}

	return (ssize_t) total;
}

/**
 * Retry reads until we (a) time out or (b) get our data.  Of course, if
 * timeout is NULL, it'll wait forever.
 *
 * @param sockfd	File descriptor we want to read from.
 * @param buf		Preallocated buffer into which we will read data.
 * @param count		Number of bytes to read.
 * @param timeout	(struct timeval) describing how long we should retry.
 * @return 		The number of bytes read on success, or -1 on failure.
 			Note that we will always return (count) or (-1).
 */
ssize_t
read_retry(int sockfd, void *buf, size_t count, struct timeval * timeout)
{
	int n, rv = 0;
	size_t total = 0;
	ssize_t remain = count;
	fd_set rfds, xfds;

	memset(buf, 0, count);

	while (total < count) {
		FD_ZERO(&rfds);
		FD_SET(sockfd, &rfds);
		FD_ZERO(&xfds);
		FD_SET(sockfd, &xfds);
		
		/*
		 * Select on the socket, in case it closes while we're not
		 * looking...
		 */
		rv = select_retry(sockfd + 1, &rfds, NULL, &xfds, timeout);
		if (rv == -1)
			return -1;
		else if (rv == 0) {
			errno = ETIMEDOUT;
			return -1;
		}

		if (FD_ISSET(sockfd, &xfds)) {
			errno = EPIPE;
			return -1;
		}

		/* 
		 * Attempt to read off the socket 
		 */
		n = read(sockfd, (char *) buf + (off_t) total, remain);

		/*
		 * When we know our socket was select()ed and we receive 0 bytes
		 * when we read, the socket was closed.
		 */
		if (n == 0 && rv == 1) {
			errno = EPIPE;
			return -1;
		}

		if (n == -1) {
			if (errno == EAGAIN || errno == EINTR) {
				/* 
				 * Not ready? Wait for data to become available
				 */
				continue;
			}

			/* Other errors: EPIPE, EINVAL, etc */
			return -1;
		}

		total += n;
		remain -= n;

		/*printf("read-retry %d/%d remain %d \n", total, count, remain);*/
	}

	return (ssize_t) total;
}


int
sock_listen(const char *sockpath)
{
	int sock = -1;
	struct sockaddr_un su;
	mode_t om;
	int ret;
	int saved_errno;

	sock = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (sock < 0)
		goto fail;
		
	su.sun_family = PF_LOCAL;
	ret = snprintf(su.sun_path, sizeof(su.sun_path), "%s", sockpath);
	if (ret < 0 || (size_t) ret >= sizeof(su.sun_path)) {
		errno = ENAMETOOLONG;
		goto fail;
	}

	unlink(su.sun_path);
	om = umask(077);

	if (bind(sock, (struct sockaddr *) &su, sizeof(su)) < 0) {
		saved_errno = errno;
		umask(om);
		errno = saved_errno;
		goto fail;
	}
	umask(om);

	if (listen(sock, SOMAXCONN) < 0)
		goto fail;

	return sock;

fail:
	saved_errno = errno;
	if (sock >= 0) {
		close(sock);
		unlink(su.sun_path);
	}
	errno = saved_errno;
	return -1;
}


int
sock_connect(const char *sockpath, int tout)
{
	struct timeval timeout;
	int sock, flags, error, ret;
	socklen_t len;
	struct sockaddr_un sun;
	fd_set rfds, wfds;
	int saved_errno;

	sock = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (sock < 0)
		return -1;
		
	sun.sun_family = PF_LOCAL;
	ret = snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", sockpath);
	if (ret < 0 || (size_t) ret >= sizeof(sun.sun_path)) {
		close(sock);
		errno = ENAMETOOLONG;
		return -1;
	}
	
	flags = fcntl(sock, F_GETFL, 0);
	if (flags < 0)
		return -1;

	if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	ret = connect(sock, (struct sockaddr *) &sun, sizeof(sun));
	if (ret < 0 && (errno != EINPROGRESS)) {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

	if (ret == 0)
		goto done;

	FD_ZERO(&rfds);
	FD_SET(sock, &rfds);
	wfds = rfds;

    timeout.tv_sec = tout;
    timeout.tv_usec = 0;

	ret = select_retry(sock + 1, &rfds, &wfds, NULL, &timeout);
	if (ret == 0) {
		close(sock);
		errno = ETIMEDOUT;
		return -1;
	}
	
	if (FD_ISSET(sock, &rfds) || FD_ISSET(sock, &wfds)) {
		len = sizeof(error);
		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
			saved_errno = errno;
			close(sock);
			errno = saved_errno;
			return -1;
		}
	} else {
		saved_errno = errno;
		close(sock);
		errno = saved_errno;
		return -1;
	}

done:
	return sock;		
}


int
sock_accept(int sockfd)
{
	int acceptfd;

	if (sockfd < 0) {
		errno = EBADF;
		return -1;
	}

	while ((acceptfd = accept(sockfd, (struct sockaddr *) NULL, NULL)) < 0) {
		if (errno == EINTR)
			continue;
		return -1;
	}

	return acceptfd;
}


void
hexdump(const void *buf, size_t len)
{
	size_t x;

	printf("%d bytes @ %p \n", (int)len, buf);

	for (x = 0; x < len; x++) {
		printf(" %02x", (((char *)buf)[x])&0xff);
		if (((x+1) % 16) == 0)
			printf("\n");
	}

	printf("\n");
}
