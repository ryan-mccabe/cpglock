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
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static cpg_lock_handle_t h;


struct arg_s {
	char *outer;
	char *inner;
};


static void *
bg(void *arg)
{
	char *outer = ((struct arg_s *)arg)->outer;
	char *inner = ((struct arg_s *)arg)->inner;

	while (1) {
		struct cpg_lock i, o;

		memset(&i, 0, sizeof(i));
		memset(&o, 0, sizeof(o));

		printf("BG inner lock requested\n");
		fflush(stdout);

		if (cpg_lock(h, inner, 0, &i) < 0) {
			perror("cpg_lock[inner bg]");
			return NULL;
		}

		printf("BG inner lock granted\n");
		printf("BG outer lock requested\n");
		fflush(stdout);

		if (cpg_lock(h, outer, 0, &o) < 0) {
			perror("cpg_lock[outer bg]");
			return NULL;
		}

		printf("BG outer lock granted\n");
		printf("Releasing BG outer lock\n");
		fflush(stdout);

		usleep(random()&32767);

		if (cpg_unlock(h, &o) < 0) {
			perror("cpg_unlock[outer bg]");
			return NULL;
		}

		printf("BG outer lock released\n");
		printf("Releasing BG inner lock\n");
		fflush(stdout);

		if (cpg_unlock(h, &i) < 0) {
			perror("cpg_lock[inner bg]");
			return NULL;
		}

		printf("BG inner lock released\n");
		fflush(stdout);
	}

	return NULL;
}


int
main(int argc, char **argv)
{
	struct cpg_lock l;
	struct arg_s *args;
	int x, y;

	pthread_t *th;

	if (argc < 2) {
		printf("usage: %s count\n", argv[0]);
		return 1;
	}

	y = atoi(argv[1]);
	if (y <= 0) {
		printf("Invalid count %s\n", argv[1]);
		return 1;
	}

	th = malloc(y * sizeof(pthread_t));

	if (cpg_lock_init(&h) < 0) {
		perror("cpg_lock_init");
		return 1;
	}

	for (x = 0; x < y; x++) {
		args = malloc(sizeof(*args));

		args->outer = strdup("outer");
		args->inner = malloc(32);
		snprintf(args->inner, 32, "inner%d", x);

		pthread_create(&th[x], NULL, bg, args);
	}

	while (1) {
		printf("FG outer lock requested\n");
		fflush(stdout);

		if (cpg_lock(h, "outer", 0, &l) < 0) {
			perror("cpg_lock");
			return -1;
		}

		printf("FG outer lock granted\n");
		printf("Releasing FG outer lock\n");
		fflush(stdout);

		if (cpg_unlock(h, &l) < 0) {
			perror("cpg_unlock");
			return 1;
		}
		printf("FG outer lock released\n");
		fflush(stdout);
	}

	for (x = 0; x < y; x++) {
		pthread_join(th[x], NULL);
	}

	sleep(1);
	cpg_lock_fin(h);

	return 0;
}
