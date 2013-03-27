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
