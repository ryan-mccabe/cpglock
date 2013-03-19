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

#ifndef _CPGLOCK_INT_H
#define _CPGLOCK_INT_H

#ifndef CPG_LOCKD_SOCK
#define CPG_LOCKD_SOCK "/var/run/cpglockd.sk"
#endif

#define CPG_LOCKD_NAME "cpglockd"

#include <stdint.h>

typedef enum {
	MSG_LOCK    = 1,
	MSG_NAK     = 2,
	MSG_GRANT   = 3,
	MSG_UNLOCK  = 4,
	MSG_PURGE   = 5,
	MSG_CONFCHG = 6,
	MSG_JOIN    = 7,
	MSG_DUMP    = 998,
	MSG_HALT    = 999
} cpg_lock_req_t;

struct cpg_lock_msg {
	int32_t request;
	uint32_t owner_nodeid;
	uint32_t owner_pid;
	uint32_t flags;
	uint32_t owner_tid;
	uint64_t lockid;
	char resource[96];
	char pad[4];
} __attribute__((packed)); /* 128 */

#endif
