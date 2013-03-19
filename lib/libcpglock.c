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
#include <stdlib.h>
#include <sys/socket.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <qb/qblist.h>

#include "cpglock.h"
#include "cpglock-internal.h"
#include "sock.h"

struct pending_node {
	struct qb_list_head list;
	struct cpg_lock_msg m;
};

struct cpg_lock_handle {
	pthread_mutex_t mutex;
	struct qb_list_head pending;
	int fd;
	pid_t pid;
	uint32_t seq;
};

static int
check_pending(	struct qb_list_head *pending,
				struct cpg_lock_msg *exp,
				struct cpg_lock_msg *ret)
{
	struct pending_node *p;

	if (!pending) {
		errno = EINVAL;
		return -1;
	}

	qb_list_for_each_entry(p, pending, list) {
		if (!strcmp(exp->resource, p->m.resource) &&
			exp->owner_tid == p->m.owner_tid)
		{
			memcpy(ret, &p->m, sizeof(*ret));
			qb_list_del(&p->list);
			free(p);
			return 0;
		}
	}

	return 1;
}


static void
add_pending(struct qb_list_head *pending, struct cpg_lock_msg *m)
{
	struct pending_node *p = do_alloc(sizeof(*p));

	memcpy(&p->m, m, sizeof(p->m));
	qb_list_init(&p->list);
	qb_list_add_tail(&p->list, pending);
}


/* Not thread safe */
int
cpg_lock_init(void **handle)
{
	struct cpg_lock_handle *h;
	int esv;

	h = do_alloc(sizeof (*h));
	if (!h)
		return -1;

	h->fd = sock_connect(CPG_LOCKD_SOCK, 3);
	if (h->fd < 0) {
		esv = errno;
		free(h);
		errno = esv;
		return -1;
	}

	h->pid = getpid();
	pthread_mutex_init(&h->mutex, NULL);
	qb_list_init(&h->pending);

	*handle = (void *)h;
	return 0;
}


int
cpg_lock(	void *handle,
			const char *resource,
			lock_flag_t flags,
			struct cpg_lock *lock)
{
	struct cpg_lock_handle *h = handle;
	struct cpg_lock_msg l, r;
	struct timeval tv;
	int ret = -1;

	if (!h) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&h->mutex);

	if (h->pid != getpid()) {
		errno = EBADF;
		goto out;
	}

	if (strlen(resource) >= sizeof(l.resource)) {
		errno = ENAMETOOLONG;
		goto out;
	}

	memset(&l, 0, sizeof(l));
	memset(&r, 0, sizeof(r));
	strncpy(l.resource, resource, sizeof(l.resource));
	strncpy(lock->resource, resource, sizeof(lock->resource));
	l.owner_pid = h->pid;
	++h->seq;
	l.owner_tid = h->seq;
	l.request = MSG_LOCK;
	l.flags = (uint32_t) flags;
	
	if (write_retry(h->fd, &l, sizeof(l), NULL) < 0)
		goto out;

	/* Thread concurrency: in case multiple threads wake up
	   from select, peek at the message to see if it's ours */
	do {
		if (check_pending(&h->pending, &l, &r) == 0)
			break;

		tv.tv_sec = 0;
		tv.tv_usec = random() & 16383;

		if (read_retry(h->fd, &r, sizeof(r), &tv) < 0) {
			if (errno == ETIMEDOUT) {
				pthread_mutex_unlock(&h->mutex);
				usleep(random() & 16383);
				pthread_mutex_lock(&h->mutex);
				continue;
			}
			goto out;
		}

		if (strcmp(r.resource, l.resource)) {
			add_pending(&h->pending, &r);
			pthread_mutex_unlock(&h->mutex);
			usleep(random() & 16383);
			pthread_mutex_lock(&h->mutex);
			continue;
		}

		if (r.owner_tid != l.owner_tid) {
			add_pending(&h->pending, &r);
			pthread_mutex_unlock(&h->mutex);
			usleep(random() & 16383);
			pthread_mutex_lock(&h->mutex);
			continue;
		}
		break;
	} while (1);
	/* locked */

	if (r.owner_nodeid == 0)
		goto out;

	if (r.request == MSG_NAK) {
		errno = EAGAIN;
		ret = -1;
		goto out;
	}

	if (r.request != MSG_GRANT)
		goto out;

	lock->state = LOCK_HELD;
	lock->owner_nodeid = r.owner_nodeid;
	lock->owner_pid = h->pid; /* XXX */
	lock->local_id = r.lockid;

	ret = 0;

out:
	pthread_mutex_unlock(&h->mutex);
	return ret;
}


int
cpg_unlock(void *handle, struct cpg_lock *lock)
{
	struct cpg_lock_handle *h = handle;
	struct cpg_lock_msg l;
	int ret = -1;

	if (!h) {
		errno = EINVAL;
		goto out;
	}

	/* Only block on lock requests, not unlock */
	if (h->pid != getpid()) {
		errno = EBADF;
		goto out;
	}

	if (lock->state != LOCK_HELD) {
		errno = EINVAL;
		goto out;
	}

	if (!lock->local_id) {
		errno = EINVAL;
		goto out;
	}

	memset(&l, 0, sizeof(l));
	strncpy(l.resource, lock->resource, sizeof(l.resource));
	l.request = MSG_UNLOCK;
	l.owner_nodeid = lock->owner_nodeid;
	l.owner_pid = lock->owner_pid;
	l.lockid = lock->local_id;
	l.owner_tid = 0;
	lock->state = LOCK_FREE;
	
	ret = write_retry(h->fd, &l, sizeof(l), NULL);
out:
	return ret;
}


int
cpg_lock_dump(FILE *fp)
{
	struct cpg_lock_msg l;
	int fd;
	char c;

	fd = sock_connect(CPG_LOCKD_SOCK, 3);
	if (fd < 0)
		return -1;

	memset(&l, 0, sizeof(l));
	l.request = MSG_DUMP;
	
	if (write_retry(fd, &l, sizeof(l), NULL) < 0) {
		close(fd);
		return -1;
	}

	while (read_retry(fd, &c, 1, NULL) == 1)
		fprintf(fp, "%c", c);
	
	close(fd);
	return 0;
}



/* Not thread safe */
int
cpg_lock_fin(void *handle)
{
	struct cpg_lock_handle *h = handle;
	struct pending_node *p, *n;

	if (!h) {
		errno = EINVAL;
		return -1;
	}

	pthread_mutex_lock(&h->mutex);

	if (h->pid != getpid()) {
		errno = EBADF;
		return -1;
	}

	shutdown(h->fd, SHUT_RDWR);
	close(h->fd);

	qb_list_for_each_entry_safe(p, n, &h->pending, list) {
		qb_list_del(&p->list);
		free(p);
	}

	pthread_mutex_destroy(&h->mutex);
	free(h);
	return 0;
}
