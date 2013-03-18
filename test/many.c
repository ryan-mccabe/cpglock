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
#include <signal.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
	cpg_lock_handle_t h;
	int n = 0, count = 0;
	char buf[32];
	struct cpg_lock *l;

	if (argc < 2) {
		printf("usage: %s count\n", argv[0]);
		return 1;
	}

	count = atoi(argv[1]);
	if (count <= 0) {
		printf("count must be > 0\n");
		return 1;
	}

	if (cpg_lock_init(&h) < 0) {
		perror("cpg_lock_init");
		return 1;
	}

	l = calloc(count, sizeof(*l));

	for (n = 0; n < count; n++) {
		snprintf(buf, sizeof(buf), "%d", n);

		if (cpg_lock(h, buf, 0, &l[n]) < 0) {
			perror("cpg_lock");
			return 1;
		}
	}

	printf("%d locks taken, press <enter> to release\n", count);
	getc(stdin);

	for (n = 0; n < count; n++) {
		snprintf(buf, sizeof(buf), "%d", n);

		if (cpg_unlock(h, &l[n]) < 0) {
			perror("cpg_unlock");
			return 1;
		}
	}

	sleep(1);
	cpg_lock_fin(h);

	return 0;
}
