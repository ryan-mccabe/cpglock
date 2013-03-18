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

#include <unistd.h>
#include <stdio.h>
#include <cpglock.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

static pthread_t th;
static cpg_lock_handle_t h;

static void *
bg(void *arg)
{
	struct cpg_lock l;
	char *ln = (char *)arg;

	memset(&l, 0, sizeof(l));

	printf("BG lock requested\n");
	fflush(stdout);

	if (cpg_lock(h, ln, 0, &l) < 0) {
		perror("cpg_lock[bg]");
		return NULL;
	}

	printf("BG lock granted\n");
	fflush(stdout);
	
	if (cpg_unlock(h, &l) < 0) {
		perror("cpg_unlock");
		return NULL;
	}

	printf("BG lock released\n");
	fflush(stdout);

	return NULL;
}
	


int
main(int argc, char **argv)
{
	struct cpg_lock l;

	memset(&l, 0, sizeof(l));

	if (argc < 2) {
		printf("usage: %s lockname\n", argv[0]);
		return 1;
	}

	if (cpg_lock_init(&h) < 0) {
		perror("cpg_lock_init");
		return 1;
	}

	printf("Acquiring lock on %s...", argv[1]);
	fflush(stdout);

	if (cpg_lock(h, argv[1], 0, &l) < 0) {
		perror("cpg_lock");
		return 1;
	}

	printf("OK\npress <enter> to unlock\n");

	pthread_create(&th, NULL, bg, argv[1]);

	getc(stdin);

	if (cpg_unlock(h, &l) < 0) {
		perror("cpg_unlock");
		return 1;
	}

	pthread_join(th, NULL);
	sleep(1);
	cpg_lock_fin(h);

	return 0;
}
