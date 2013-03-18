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

int
main(int argc, char **argv)
{
	cpg_lock_handle_t h;
	struct cpg_lock l;

	if (argc < 2)
		return 1;

	if (cpg_lock_init(&h) < 0) {
		perror("cpg_lock_init");
		return 1;
	}

	printf("Acquiring lock on %s...", argv[1]);
	fflush(stdout);

	if (cpg_lock(h, argv[1], 1, &l) < 0) {
		perror("cpg_lock");
		return 1;
	}

	printf("OK, local id %lu\npress <enter> to unlock\n", l.local_id);
	getc(stdin);

	memset(l.resource, 0, sizeof(l.resource));

	if (cpg_unlock(h, &l) < 0) {
		perror("cpg_unlock");
		return 1;
	}

	cpg_lock_fin(h);
	return 0;
}
