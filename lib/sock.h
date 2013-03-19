/*
** Copyright (C) 2012-2013 Red Hat, Inc. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef _SOCK_H
#define _SOCK_H

ssize_t read_retry(int sockfd, void *buf, size_t count,
		   struct timeval * timeout);
int select_retry(int fdmax, fd_set * rfds, fd_set * wfds, fd_set * xfds,
		 struct timeval *timeout);
ssize_t write_retry(int fd, void *buf, size_t count,
		   struct timeval *timeout);

int sock_listen(const char *sockpath);
int sock_connect(const char *sockpath, int tout);
int sock_accept(int fd);
void hexdump(const void *buf, size_t len);

void *do_alloc(size_t);

#endif
