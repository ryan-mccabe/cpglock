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

#ifndef _CPGLOCK_H
#define _CPGLOCK_H

#include <stdint.h>

typedef enum {
	LOCK_FREE		= 0,
	LOCK_PENDING	= 1,
	LOCK_HELD		= 2
} lock_state_t;

typedef enum {
	FL_TRY			= 0x1
} lock_flag_t;

struct cpg_lock {
	int local_fd;
	lock_state_t state;
	uint32_t owner_nodeid;
	uint32_t owner_pid;
	uint32_t owner_tid;
	uint64_t local_id;
	char resource[96];
};

typedef void * cpg_lock_handle_t;

int cpg_lock_init(cpg_lock_handle_t *h);

/* 0 if successful, -1 if error */
int cpg_lock(	cpg_lock_handle_t h,
				const char *resource,
				lock_flag_t flags,
				struct cpg_lock *lock);

int cpg_unlock(cpg_lock_handle_t h, struct cpg_lock *lock);

/* Warning: drops all locks with this client */
int cpg_lock_fin(cpg_lock_handle_t h);

int cpg_lock_dump(FILE *fp);

#endif
