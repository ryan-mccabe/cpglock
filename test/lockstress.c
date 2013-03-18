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

#include <stdio.h>
#include <cpglock.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

static int running = 1;
static void
inter(int sig)
{
	running = 0;
}


int
main(int argc, char **argv)
{
	cpg_lock_handle_t h;
	int n = 0;

	if (argc < 2) {
		printf("usage: %s lockname\n", argv[0]);
		return 1;
	}

	signal(SIGINT, inter);

	if (cpg_lock_init(&h) < 0) {
		perror("cpg_lock_init");
		return 1;
	}

	while (running) {
		struct cpg_lock l;

		memset(&l, 0, sizeof(l));
		printf("lock ... ");
		fflush(stdout);

		if (cpg_lock(h, argv[1], 0, &l) < 0) {
			perror("cpg_lock");
			return 1;
		}
		printf(" [%d:%d:%u:%u:%u:%lu:%s] ",
			l.local_fd, l.state, l.owner_nodeid,
			l.owner_pid, l.owner_tid, l.local_id, l.resource);
		printf("unlock");

		if (cpg_unlock(h, &l) < 0) {
			perror("cpg_unlock");
			return 1;
		}
		++n;

		printf("\n");
		fflush(stdout);
	}

	cpg_lock_fin(h);

	printf("%s taken/released %d times\n", argv[0], n);

	return 0;
}
