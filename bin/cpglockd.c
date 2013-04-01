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
#include <sys/time.h>
#include <errno.h>
#include <poll.h>

#include <corosync/corodefs.h>
#include <corosync/cpg.h>

#include <qb/qbloop.h>
#include <qb/qblog.h>
#include <qb/qblist.h>
#include <qb/qbmap.h>

#include "platform.h"
#include "daemon_init.h"
#include "sock.h"
#include "cpglock.h"
#include "cpglock-internal.h"
#include "cpglock_util.h"

struct request_item {
	struct qb_list_head list;
	struct cpg_lock l;
	struct timespec req_time;
};

struct client_item {
	struct qb_list_head list;
	int fd;
	int pid;
};

struct member_item {
	struct qb_list_head list;
	uint32_t nodeid;
	uint32_t pid;
};

struct msg_item {
	struct qb_list_head list;
	struct cpg_lock_msg m;
	struct timespec msg_time;
};

/* Local vars */
static cpg_handle_t cpg;
static uint32_t my_node_id = 0;


static uint32_t message_count = 0;
static unsigned long max_saved_msgs = 20;

/* Increases monotonically*/
static uint64_t local_lockid = 0;

static QB_LIST_DECLARE(requests);
static QB_LIST_DECLARE(messages);
static QB_LIST_DECLARE(clients);
static QB_LIST_DECLARE(group_members);

static qb_map_t *locks = NULL;

static int total_members = 0;
static int joined = 0;
static int shutdown_pending = 0;
static int nofork = 0;
static int debug_mode = 0;
static char *cpglockd_sock_path = NULL;

static qb_loop_t *main_loop = NULL;

static void insert_client(int fd);
static void del_client(int fd);
static void dump_state(FILE *fp);
static int send_lock(struct cpg_lock_msg *m);
static int send_unlock(struct cpg_lock_msg *m);
static int grant_next(struct cpg_lock_msg *m);
static int find_lock_by_id(struct cpg_lock_msg *m);

static inline struct member_item *get_oldest_node(void) {
	return qb_list_first_entry(&group_members, struct member_item, list);
}

static int32_t
client_connect_cb(int32_t fd, int32_t revents, void *data)
{
	int client_fd;

	if (revents & (POLLERR | POLLNVAL)) {
		qb_log(LOG_ERR, "Error on listen socket: revents %d\n", revents);
		return 0;
	}

	if (shutdown_pending)
		return 0;

	do {
		client_fd = accept(fd, NULL, NULL);
	} while (client_fd < 0 && errno == EINTR);

	qb_log(LOG_TRACE, "Accepted new client on fd %d\n", client_fd);

	if (client_fd < 0)
		qb_log(LOG_DEBUG, "Error accepting new client: %s\n", strerror(errno));
	else
		insert_client(client_fd);

	return 0;
}

static int32_t
client_activity_cb(int32_t fd, int32_t revents, void *data)
{
	struct cpg_lock_msg m;
	struct client_item *client = (struct client_item *) data;

	if (revents & (POLLERR | POLLNVAL)) {
		qb_log(LOG_DEBUG, "Closing client fd %d pid %u: revents %d\n",
			client->fd, client->pid, revents);

		del_client(fd);
		return 0;
	}

	if (read_timeout(fd, &m, sizeof(m), -1) < 0) {
		qb_log(LOG_DEBUG, "Closing client fd %d pid %u: %d\n",
			client->fd, client->pid, errno);

		del_client(fd);
		return 0;
	}

	if (m.request == MSG_LOCK) {
		client->pid = m.owner_pid;
		send_lock(&m);
	} else if (m.request == MSG_UNLOCK) {
		qb_log(LOG_TRACE,
			"Unlock %lu from fd %d\n", m.lockid, client->fd);

		if (find_lock_by_id(&m) != 0)
			qb_log(LOG_DEBUG, "Unable to find lock %lu\n", m.lockid);
		else {
			if (grant_next(&m) == 0)
				send_unlock(&m);
		}
	} else if (m.request == MSG_DUMP) {
		FILE *fp;

		qb_list_del(&client->list);
		qb_loop_poll_del(main_loop, fd);

		fp = fdopen(fd, "w");
		if (fp) {
			dump_state(fp);
			fclose(fp);
		} else {
			qb_log(LOG_DEBUG, "Error: fdopen(%d): %s\n",
				fd, strerror(errno));
		}

		close(client->fd);
		free(client);
	} else {
		qb_log(LOG_DEBUG, "Unknown request %d from fd %d\n", m.request, fd);
	}

	return 0;
}

static int32_t cpg_event_cb(int32_t fd, int32_t revents, void *data) {
	if (revents & (POLLERR | POLLNVAL)) {
		qb_log(LOG_ERR, "Error on cpg fd: revents %d\n", revents);
		goto out_err;
	}

	if (revents & POLLIN && cpg_dispatch(cpg, CS_DISPATCH_ALL) != CS_OK) {
		qb_log(LOG_ERR, "Fatal: Lost CPG connection.\n");
		goto out_err;
	}

	return 0;

out_err:
	qb_loop_stop(main_loop);
	return -1;
}

static int32_t
dummy_sigcb(int32_t sig, void *data) {
	return 0;
}

static int32_t
flag_shutdown(int32_t sig, void *data)
{
	if (!qb_list_empty(&clients)) {
		qb_log(LOG_INFO,
			"Clients are connected to cpglockd. Refusing to shutdown.\n");
		return 0;
	}

	qb_log(LOG_INFO, "Caught signal %d. Shutting Down.\n", sig);
	qb_loop_stop(main_loop);
	shutdown_pending = 1;
	return 0;
}



static int
is_member(uint32_t nodeid)
{
	struct member_item *n;

	qb_list_for_each_entry(n, &group_members, list) {
		if (n->nodeid == nodeid)
			return 1;
	}

	return 0;
}


static const char *
ls2str(int x)
{
	switch (x) {
		case LOCK_FREE:
			return "FREE";
		case LOCK_HELD:
			return "HELD";
		case LOCK_PENDING:
			return "PENDING";
	}

	return "UNKNOWN";
}


static const char *
rq2str(int x)
{
	switch (x) {
		case MSG_LOCK:
			return "LOCK";
		case MSG_UNLOCK:
			return "UNLOCK";
		case MSG_GRANT:
			return "GRANT";
		case MSG_NAK:
			return "NAK";
		case MSG_PURGE:
			return "PURGE";
		case MSG_CONFCHG:
			return "CONFCHG";
		case MSG_JOIN:
			return "JOIN";
		case MSG_HALT:
			return "HALT";
	}

	return "UNKNOWN";
}


static void
dump_state(FILE *fp)
{
	fprintf(fp, "cpglockd state\n");
	fprintf(fp, "======== =====\n");

	fprintf(fp, "Node ID: %d\n", my_node_id);

	if (!qb_list_empty(&group_members)) {
		struct member_item *m = NULL;

		fprintf(fp, "Participants:");
		qb_list_for_each_entry(m, &group_members, list) {
			fprintf(fp, " %u.%u", m->nodeid, m->pid);
		}
		fprintf(fp, "\n");
	}

	if (!qb_list_empty(&clients)) {
		struct client_item *c = NULL;

		fprintf(fp, "Clients:");
		qb_list_for_each_entry(c, &clients, list) {
			fprintf(fp, " %u.%d", c->pid, c->fd);
		}
		fprintf(fp, "\n");
	}
	fprintf(fp, "\n");

	if (qb_map_count_get(locks) > 0) {
		qb_map_iter_t *iter;

		fprintf(fp, "Locks\n");
		fprintf(fp, "=====\n");

		iter = qb_map_iter_create(locks);
		if (iter) {
			void *data;

			while (qb_map_iter_next(iter, &data)) {
				const struct cpg_lock *l = (struct cpg_lock *) data;

				fprintf(fp, "  %s: %s", l->resource, ls2str(l->state));
				if (l->owner_nodeid) {
					fprintf(fp, ", owner %u:%u:%u",
						l->owner_nodeid, l->owner_pid, l->owner_tid);

					if (l->owner_nodeid == my_node_id && l->state == LOCK_HELD)
						fprintf(fp, ", Local ID %lu", l->local_id);
				}
				fprintf(fp, "\n");
			}
			qb_map_iter_free(iter);
		}
		fprintf(fp, "\n");
	}

	if (!qb_list_empty(&requests)) {
		struct request_item *r = NULL;

		fprintf(fp, "Requests\n");
		fprintf(fp, "========\n");

		qb_list_for_each_entry(r, &requests, list) {
			fprintf(fp, "  [%ld.%09ld] %s: %s",
				r->req_time.tv_sec, r->req_time.tv_nsec,
				r->l.resource, rq2str(r->l.state));
			if (r->l.owner_nodeid)
				fprintf(fp, ", from %u:%u:%u",
					r->l.owner_nodeid, r->l.owner_pid, r->l.owner_tid);
			fprintf(fp, "\n");
		}
		fprintf(fp, "\n");
	}

	if (!qb_list_empty(&messages)) {
		struct msg_item *s = NULL;

		fprintf(fp, "Message History\n");
		fprintf(fp, "======= =======\n");

		qb_list_for_each_entry(s, &messages, list) {
			switch (s->m.request) {
				case MSG_CONFCHG:
					fprintf(fp, "  [%ld.%09ld] CONFIG CHANGE\n",
						s->msg_time.tv_sec, s->msg_time.tv_nsec);
					break;
				case MSG_PURGE:
					fprintf(fp, "  [%ld.%09ld] PURGE for %u:%u\n",
						s->msg_time.tv_sec, s->msg_time.tv_nsec,
						s->m.owner_nodeid, s->m.owner_pid);
					break;
				case MSG_JOIN:
					fprintf(fp, "  [%ld.%09ld] JOIN %u\n",
						s->msg_time.tv_sec, s->msg_time.tv_nsec,
						s->m.owner_nodeid);
					break;
				case MSG_LOCK:
				case MSG_UNLOCK:
				case MSG_GRANT:
				case MSG_NAK:
				case MSG_HALT:
					fprintf(fp, "  [%ld.%09ld] %s: %s %u:%u:%u\n",
						s->msg_time.tv_sec, s->msg_time.tv_nsec,
						rq2str(s->m.request), s->m.resource, s->m.owner_nodeid,
						s->m.owner_pid, s->m.owner_tid);
					break;
				default:
					fprintf(fp, "  [%ld.%09ld] %s (%d): %s %u:%u:%u\n",
						s->msg_time.tv_sec, s->msg_time.tv_nsec,
						rq2str(s->m.request), s->m.request,
						s->m.resource, s->m.owner_nodeid,
						s->m.owner_pid, s->m.owner_tid);
					break;
			}
		}
		fprintf(fp, "\n");
	}
}


static void
old_msg(struct cpg_lock_msg *m)
{
	struct msg_item *n;
	int ret = -1;

	n = wait_calloc(sizeof(*n));
	do {
		ret = clock_gettime(CLOCK_MONOTONIC, &n->msg_time);
	} while (ret == -1 && errno == EINTR);

	memcpy(&n->m, m, sizeof(n->m));
	qb_list_add_tail(&n->list, &messages);
	if (message_count < max_saved_msgs) {
		++message_count;
	} else {
		n = qb_list_first_entry(&messages, struct msg_item, list);
		qb_list_del(&n->list);
		free(n);
	}
}


static void
insert_client(int fd)
{
	struct client_item *n = NULL;

	n = wait_calloc(sizeof(*n));
	n->fd = fd;
	qb_list_init(&n->list);

	qb_list_add_tail(&n->list, &clients);
	qb_loop_poll_add(main_loop, QB_LOOP_MED, fd, POLLIN, n, client_activity_cb);
}

static void
swab_cpg_lock_msg(struct cpg_lock_msg *m)
{
	swab32(m->request);
	swab32(m->owner_nodeid);
	swab32(m->owner_pid);
	swab32(m->flags);
	swab32(m->owner_tid);
	swab64(m->lockid);
}

/* forward request from client */
static int
send_lock_msg(struct cpg_lock_msg *m)
{
	struct iovec iov;
	int ret;
	struct cpg_lock_msg out_msg;

	memcpy(&out_msg, m, sizeof(out_msg));
	swab_cpg_lock_msg(&out_msg);

	iov.iov_base = &out_msg;
	iov.iov_len = sizeof(out_msg);

	do {
		ret = cpg_mcast_joined(cpg, CPG_TYPE_AGREED, &iov, 1);
		if (ret != CS_OK) {
			qb_log(LOG_DEBUG, "send_lock_msg() failed %d: %s\n",
				ret, strerror(errno));
			usleep(250000);
		}
	} while (ret != CS_OK && !shutdown_pending);

	return 0;
}


/* forward request from client */
static int
send_lock(struct cpg_lock_msg *m)
{
	m->owner_nodeid = my_node_id;
	return send_lock_msg(m);
}


static int
send_grant(struct request_item *n)
{
	struct cpg_lock_msg m;

	qb_log(LOG_TRACE, "-> sending grant %s to %u:%u:%u\n",
		n->l.resource, n->l.owner_nodeid, n->l.owner_pid, n->l.owner_tid);

	memset(&m, 0, sizeof(m));
	strncpy(m.resource, n->l.resource, sizeof(m.resource));
	m.request = MSG_GRANT;
	m.owner_nodeid = n->l.owner_nodeid;
	m.owner_pid = n->l.owner_pid;
	m.owner_tid = n->l.owner_tid;

	return send_lock_msg(&m);
}


static int
send_nak(struct cpg_lock_msg *m)
{
	qb_log(LOG_TRACE, "-> sending NAK %s to %u:%u:%u\n",
		m->resource, m->owner_nodeid, m->owner_pid, m->owner_tid);

	m->request = MSG_NAK;
	return send_lock_msg(m);
}


static int
send_join(void)
{
	struct cpg_lock_msg m;

	memset(&m, 0, sizeof(m));
	m.request = MSG_JOIN;
	m.owner_nodeid = my_node_id;
	return send_lock_msg(&m);
}



static int
send_unlock(struct cpg_lock_msg *m)
{
	m->request = MSG_UNLOCK;
	return send_lock_msg(m);
}


/*
 * Grant the lock in this request node to the next
 * waiting client.
 */
static int
grant_next(struct cpg_lock_msg *m)
{
	struct request_item *r;

	qb_list_for_each_entry(r, &requests, list) {
		/* Send grant */
		if (!strcmp(m->resource, r->l.resource) &&
			r->l.state == LOCK_PENDING)
		{
			qb_log(LOG_TRACE, "LOCK %s: grant to %u:%u:%u\n", m->resource,
				r->l.owner_nodeid, r->l.owner_pid, r->l.owner_tid);
			/* don't send dup grants */
			r->l.state = LOCK_HELD;
			send_grant(r);
			return 1;
		}
	}

	return 0;
}


static void
purge_requests(uint32_t nodeid, uint32_t pid)
{
	struct request_item *r, *n;
	uint32_t count = 0;

	qb_list_for_each_entry_safe(r, n, &requests, list) {
		if (r->l.owner_nodeid == nodeid && (!pid || r->l.owner_pid == pid)) {
			qb_list_del(&r->list);
			free(r);
			++count;
		}
	}

	if (count) {
		if (pid) {
			qb_log(LOG_TRACE, "RECOVERY: purged %u requests from %u:%u\n",
				count, nodeid, pid);
		} else {
			qb_log(LOG_TRACE, "RECOVERY: purged %u requests from node %u\n",
				count, nodeid);
		}
	}
}

static void
del_client(int fd)
{
	struct cpg_lock_msg m;
	struct client_item *n;
	uint32_t pid = 0;
	uint32_t recovered = 0;
	int found = 0;
	qb_map_iter_t *iter;
	void *data;

	qb_loop_poll_del(main_loop, fd);

	qb_list_for_each_entry(n, &clients, list) {
		if (n->fd == fd) {
			qb_list_del(&n->list);
			pid = n->pid;
			found = 1;
			close(n->fd);
			free(n);
			break;
		}
	}

	if (!found)
		qb_log(LOG_DEBUG, "No client with fd %d in client list\n", fd);

	if (!pid) {
		/* Clients for dump requests will have no PID set */
		return;
	}

	qb_log(LOG_TRACE, "RECOVERY: Looking for locks held by PID %u\n", pid);

	/* This may not be needed */
	purge_requests(my_node_id, pid);

	memset(&m, 0, sizeof(m));
	m.request = MSG_PURGE;
	m.owner_nodeid = my_node_id;
	m.owner_pid = pid;
	send_lock_msg(&m);

	iter = qb_map_iter_create(locks);
	if (!iter) {
		qb_log(LOG_TRACE,
			"[%d] Failed to create iterator for lock map\n",
			__LINE__);
		return;
	}

	while (qb_map_iter_next(iter, &data)) {
		struct cpg_lock *l = (struct cpg_lock *) data;

		if (l->owner_nodeid == my_node_id &&
			l->owner_pid == pid &&
			l->state == LOCK_HELD)
		{
			qb_log(LOG_TRACE, "RECOVERY: Releasing %s\n", l->resource);
			l->state = LOCK_FREE;
			strncpy(m.resource, l->resource, sizeof(m.resource));
			++recovered;
			if (grant_next(&m) == 0)
				send_unlock(&m);
		}
	}
	qb_map_iter_free(iter);

	if (recovered) {
		qb_log(LOG_TRACE, "RECOVERY: %u locks from local PID %u\n",
			recovered, pid);
	}

	qb_log(LOG_TRACE, "RECOVERY: Complete\n");
}



static void
del_node(uint32_t nodeid)
{
	struct cpg_lock_msg m;
	struct cpg_lock *l;
	uint32_t recovered = 0, granted = 0;
	qb_map_iter_t *iter;
	void *data;
	struct member_item *oldest = get_oldest_node();

	iter = qb_map_iter_create(locks);
	if (!iter) {
		qb_log(LOG_TRACE,
			"[%d] Failed to create iterator for lock map\n",
			__LINE__);
			return;
	}

	if (oldest->nodeid != my_node_id) {
		/*
		** Update the owner and pid of any locks owned by the deleted
		** node to those of the oldest node in the group.
		*/
		 while (qb_map_iter_next(iter, &data)) {
			l = (struct cpg_lock *) data;

			if (l->owner_nodeid == nodeid) {
				l->owner_nodeid = oldest->nodeid;
				l->owner_pid = oldest->pid;

				qb_log(LOG_TRACE,
					"RECOVERY: LOCK UPDATED: %s [%u:%u]=>[%u:%u]\n",
					l->resource,
					l->owner_nodeid, l->owner_pid,
					oldest->nodeid, oldest->pid);
			}
		}

		qb_map_iter_free(iter);
		return;
	}

	qb_log(LOG_TRACE,
		"RECOVERY: I am the oldest node in the group, recovering locks\n");

	/* pass 1: purge outstanding requests from this node. */

	/* This may not be needed */
	purge_requests(nodeid, 0);

	memset(&m, 0, sizeof(m));
	m.request = MSG_PURGE;
	m.owner_nodeid = nodeid;
	m.owner_pid = 0;

	send_lock_msg(&m);

	while (qb_map_iter_next(iter, &data)) {
		l = (struct cpg_lock *) data;

		if (l->owner_nodeid == nodeid && l->state == LOCK_HELD) {
			qb_log(LOG_TRACE,
				"RECOVERY: Releasing %s held by dead node %u\n",
				l->resource, nodeid);

			l->state = LOCK_FREE;
			strncpy(m.resource, l->resource, sizeof(m.resource));
			if (grant_next(&m) == 0) {
				qb_log(LOG_TRACE,
					"RECOVERY: HELD LOCK UPDATED: %s [%u:%u:%u]=>[%u:%d]\n",
					l->resource,
					l->owner_nodeid, l->owner_pid, l->owner_tid,
					my_node_id, getpid());

				l->owner_nodeid = my_node_id;
				l->owner_pid = getpid();
				m.owner_nodeid = my_node_id;
				m.owner_pid = getpid();
				send_unlock(&m);
			}
			++recovered;
		} else if (l->owner_nodeid == nodeid && l->state == LOCK_FREE) {
			strncpy(m.resource, l->resource, sizeof(m.resource));
			if (grant_next(&m) == 0) {
				l->owner_nodeid = my_node_id;
				l->owner_pid = getpid();

				qb_log(LOG_TRACE,
					"RECOVERY: FREE LOCK UPDATED: %s [%u:%u:%u]=>[%u:%d]\n",
					l->resource,
					l->owner_nodeid, l->owner_pid, l->owner_tid,
					my_node_id, getpid());
			}
			++granted;
		}
	}
	qb_map_iter_free(iter);

	if (recovered) {
		qb_log(LOG_TRACE,
			"RECOVERY: %u locks from node %u\n", recovered, nodeid);
	}

	if (granted) {
		qb_log(LOG_TRACE, "RECOVERY: %u pending locks granted\n", granted);
	}

	qb_log(LOG_TRACE, "RECOVERY: Complete\n");
}


static struct client_item *
find_client(int pid)
{
	struct client_item *n;

	qb_list_for_each_entry(n, &clients, list) {
		if (n->pid == pid)
			return n;
	}

	return NULL;
}


#if 0
static void
send_fault(const char *resource)
{
	struct cpg_lock_msg m;

	memset(&m, 0, sizeof(m));
	strncpy(m.resource, resource, sizeof(m.resource));
	m.request = MSG_HALT;
	m.owner_pid = 0;
	m.owner_nodeid = my_node_id;

	send_lock_msg(&m);
}
#endif


static int
grant_client(struct cpg_lock *l)
{
	struct client_item *c;
	struct cpg_lock_msg m;

	memset(&m, 0, sizeof(m));
	strncpy(m.resource, l->resource, sizeof(m.resource));
	m.request = MSG_GRANT;
	m.owner_pid = l->owner_pid;
	m.owner_tid = l->owner_tid;

	if (local_lockid == (typeof(local_lockid)) ~0) {
		qb_log(LOG_DEBUG, "local_lockid about to overflow for %s %u:%u\n",
			m.resource, m.owner_pid, m.owner_tid);
	}

	l->local_id = ++local_lockid;
	m.lockid = l->local_id;
	m.owner_nodeid = my_node_id;

	c = find_client(l->owner_pid);
	if (!c) {
		qb_log(LOG_DEBUG, "can't find client for pid %u\n",
			l->owner_pid);
		return 1;
	}

	if (c->fd < 0) {
		qb_log(LOG_DEBUG, "Client with PID %u has bad fd %d\n",
			l->owner_pid, c->fd);
		return -1;
	}

	if (write_timeout(c->fd, &m, sizeof(m), -1) < 0) {
		/* no client anymore; drop and send to next guy
		** This should be handled by our main loop
		** qb_log(LOG_DEBUG, "Failed to notify client!\n");
		*/
	 }

	return 0;
}


static int
nak_client(struct request_item *r)
{
	struct client_item *c;
	struct cpg_lock_msg m;

	memset(&m, 0, sizeof(m));
	strncpy(m.resource, r->l.resource, sizeof(m.resource));
	m.request = MSG_NAK;
	m.owner_pid = r->l.owner_pid;
	m.owner_tid = r->l.owner_tid;
	m.owner_nodeid = my_node_id;

	c = find_client(r->l.owner_pid);
	if (!c) {
		qb_log(LOG_DEBUG, "Can't find client for pid %u\n",
			r->l.owner_pid);
		return 1;
	}

	if (c->fd < 0) {
		qb_log(LOG_DEBUG, "Client with PID %u has bad fd %d\n",
			r->l.owner_pid, c->fd);
		return -1;
	}

	if (write_timeout(c->fd, &m, sizeof(m), -1) < 0) {
		/* no client anymore; drop and send to next guy XXX
		** This should be handled by our main loop
		** qb_log(LOG_DEBUG, "Failed to notify client!\n");
		*/
	}

	return 0;
}

static void
queue_request(struct cpg_lock_msg *m)
{
	struct request_item *r;
	int ret;

	qb_log(LOG_TRACE, "LOCK %s: queue for %u:%u:%u\n", m->resource,
		m->owner_nodeid, m->owner_pid, m->owner_tid);

	r = wait_calloc(sizeof(*r));
	do {
		ret = clock_gettime(CLOCK_MONOTONIC, &r->req_time);
	} while (ret == -1 && errno == EINTR);

	strncpy(r->l.resource, m->resource, sizeof(r->l.resource));
	r->l.owner_nodeid = m->owner_nodeid;
	r->l.owner_pid = m->owner_pid;
	r->l.owner_tid = m->owner_tid;
	r->l.state = LOCK_PENDING;
	qb_list_init(&r->list);
	qb_list_add_tail(&r->list, &requests);
}


static int
process_lock(struct cpg_lock_msg *m)
{
	struct cpg_lock *l;
	struct member_item *oldest = NULL;

	if (!joined)
		return 0;

	queue_request(m);

	l = qb_map_get(locks, m->resource);
	if (l) {
		/* if it's owned locally, we need send a
				GRANT to the first node on the request queue */
		if (l->owner_nodeid == my_node_id) {
			if (l->state == LOCK_FREE) {
				/* Set local state to PENDING to avoid double-grants */
				l->state = LOCK_PENDING;
				if (grant_next(m) == 0)
					l->state = LOCK_FREE;
			} else {
				/* state is PENDING or HELD */
				if (m->flags & FL_TRY) {
					/* nack to client if needed */
					send_nak(m);
				}
			}
		}
	} else {
		/* New lock */
		l = wait_calloc(sizeof(*l));
		strncpy(l->resource, m->resource, sizeof(l->resource));
		l->state = LOCK_FREE;

		qb_map_put(locks, m->resource, l);

		oldest = get_oldest_node();
		if (oldest->nodeid == my_node_id) {
			l->state = LOCK_PENDING;
			/* immediately grant */
			if (grant_next(m) == 0) {
				/* Should not happen */
				l->state = LOCK_FREE;
			}
		}
	}

	return 0;
}


static int
process_grant(struct cpg_lock_msg *m, uint32_t nodeid)
{
	struct cpg_lock *l;
	struct request_item *r;
	struct member_item *oldest = NULL;

	if (!joined)
		return 0;

	l = qb_map_get(locks, m->resource);
	if (l) {
		if (l->state == LOCK_HELD) {
			if (m->owner_pid == 0 || m->owner_nodeid == 0) {
				qb_log(LOG_TRACE, "GRANT averted: %s %u:%u:%u\n",
					m->resource, m->owner_nodeid,
					m->owner_pid, m->owner_tid);
				return 0;
			}
		} else {
			l->state = LOCK_HELD;
		}

		qb_log(LOG_TRACE, "GRANT %s: to %u:%u:%u\n",
			m->resource, m->owner_nodeid, m->owner_pid, m->owner_tid);

		l->owner_nodeid = m->owner_nodeid;
		l->owner_pid = m->owner_pid;
		l->owner_tid = m->owner_tid;

		qb_list_for_each_entry(r, &requests, list) {
			if (!strcmp(r->l.resource, m->resource) &&
				r->l.owner_nodeid == m->owner_nodeid &&
				r->l.owner_pid == m->owner_pid &&
				r->l.owner_tid == m->owner_tid)
			{
				qb_list_del(&r->list);
				free(r);
				break;
			}
		}

		/* granted lock */
		if (l->owner_nodeid == my_node_id) {
			if (grant_client(l) != 0) {
				/*
				** A Grant to a nonexistent PID can
				** happen because we may have a pending
				** request after a fd was closed.
				** since we process on delivery, we
				** now simply make an unlock request
				** and move on
				*/
				purge_requests(my_node_id, l->owner_pid);
				if (grant_next(m) == 0)
					send_unlock(m);
				return 0;
			}
		}

		/* What if node has died with a GRANT in flight? */
		oldest = get_oldest_node();
		if (oldest->nodeid == my_node_id && !is_member(l->owner_nodeid)) {
			qb_log(LOG_DEBUG,
				"GRANT to non-member %u; giving to next requestor\n",
				l->owner_nodeid);

			l->state = LOCK_FREE;
			if (grant_next(m) == 0)
				send_unlock(m);
			return 0;
		}
	} else {
		/* Record the lock state since we now know it */
		l = wait_calloc(sizeof(*l));
		strncpy(l->resource, m->resource, sizeof(l->resource));
		l->state = LOCK_HELD;
		l->owner_nodeid = m->owner_nodeid;
		l->owner_pid = m->owner_pid;
		l->owner_tid = m->owner_tid;

		qb_map_put(locks, m->resource, l);
	}

	return 0;
}


static int
process_nak(struct cpg_lock_msg *m, uint32_t nodeid)
{
	struct request_item *r = NULL;

	if (!joined)
		return 0;

	qb_log(LOG_TRACE, "NAK %s for %u:%u:%u\n", m->resource,
		m->owner_nodeid, m->owner_pid, m->owner_tid);

	qb_list_for_each_entry(r, &requests, list) {
		if (!strcmp(r->l.resource, m->resource) &&
			r->l.owner_nodeid == m->owner_nodeid &&
			r->l.owner_pid == m->owner_pid &&
			r->l.owner_tid == m->owner_tid)
		{
			qb_list_del(&r->list);
			if (r->l.owner_nodeid == my_node_id) {
				if (nak_client(r) != 0) {
					purge_requests(my_node_id, r->l.owner_pid);
				}
			}
			free(r);
			break;
		}
	}

	return 0;
}


static int
process_unlock(struct cpg_lock_msg *m, uint32_t nodeid)
{
	qb_map_iter_t *iter;
	void *data;

	if (!joined)
		return 0;
	/*
	** We could create another hash table and
	** hash by lockid to speed this up
	*/
	iter = qb_map_iter_create(locks);
	if (!iter) {
		qb_log(LOG_TRACE,
			"[%d] Failed to create iterator for lock map\n",
			__LINE__);
		return 0;
	}

	while (qb_map_iter_next(iter, &data)) {
		struct cpg_lock *l = (struct cpg_lock *) data;

		/* Held lock... if it's local, we need to send a
				GRANT to the first node on the request queue */
		if (l &&
			l->state == LOCK_HELD &&
			!strcmp(l->resource, m->resource) &&
			l->owner_nodeid == m->owner_nodeid &&
			l->owner_pid == m->owner_pid)
		{
			qb_log(LOG_TRACE, "UNLOCK %s: %u:%u:%u\n",
				m->resource, m->owner_nodeid, m->owner_pid, m->owner_tid);
			l->state = LOCK_FREE;
			if (l->owner_nodeid == my_node_id) {
				if (grant_next(m) != 0)
					l->state = LOCK_PENDING;
			}
		}
	}
	qb_map_iter_free(iter);

	return 0;
}

static int
find_lock_by_id(struct cpg_lock_msg *m)
{
	qb_map_iter_t *iter;
	int ret = 1;
	void *data;

#if 0
	if (m->resource[0] != 0) {
		qb_log(LOG_DEBUG, "find_lock_by_id with non-empty resource: %s %lu\n",
			m->resource, m->lockid);
		return 0;
	}
#endif

	iter = qb_map_iter_create(locks);
	if (!iter) {
		qb_log(LOG_TRACE,
			"find_lock_by_id failed to create lock iterator for %lu\n",
			m->lockid);
		return 1;
	}

	while (qb_map_iter_next(iter, &data)) {
		const struct cpg_lock *l = (const struct cpg_lock *) data;

		if (l && m->lockid == l->local_id) {
			qb_log(LOG_TRACE, "LOCK ID %lu -> LOCK NAME %s\n",
				m->lockid, m->resource);
			strncpy(m->resource, l->resource, sizeof(m->resource));
			m->owner_nodeid = l->owner_nodeid;
			m->owner_pid = l->owner_pid;
			m->owner_tid = l->owner_tid;
			m->lockid = 0;

			ret = 0;
			break;
		}
	}
	qb_map_iter_free(iter);

	return ret;
}


static int
process_join(struct cpg_lock_msg *m, uint32_t nodeid, uint32_t pid)
{
	struct member_item *n;

	qb_list_for_each_entry(n, &group_members, list) {
		if (n->nodeid == nodeid) {
			if (n->pid == pid) {
				qb_log(LOG_DEBUG, "Saw JOIN from self (%u.%u)\n",
					nodeid, pid);
				qb_list_del(&n->list);
				qb_list_add_tail(&n->list, &group_members);
			} else {
				qb_log(LOG_DEBUG,
					"IGNORING JOIN from existing member %u.%u (%u.%u)\n",
					nodeid, pid, nodeid, n->pid);
			}
			return 0;
		}
	}

	if (nodeid == my_node_id) {
		joined = 1;
		qb_log(LOG_TRACE, "JOIN: node %u.%u (self)\n", nodeid, pid);
	} else
		qb_log(LOG_TRACE, "JOIN: node %u.%u\n", nodeid, pid);

	n = wait_calloc(sizeof(*n));
	n->nodeid = nodeid;
	n->pid = pid;
	qb_list_init(&n->list);
	qb_list_add_tail(&n->list, &group_members);
	total_members++;

	return 0;
}


static int
process_request(struct cpg_lock_msg *m, uint32_t nodeid, uint32_t pid)
{
	swab_cpg_lock_msg(m);
	old_msg(m);

	switch (m->request) {
		case MSG_HALT:
			qb_log(LOG_INFO,
				"FAULT: Halting operations; see node %u\n", m->owner_nodeid);
			while (1)
				sleep(30);
			break;
		case MSG_LOCK:
			process_lock(m);
			break;
		case MSG_NAK:
			process_nak(m, nodeid);
			break;
		case MSG_GRANT:
			process_grant(m, nodeid);
			break;
		case MSG_UNLOCK:
			process_unlock(m, nodeid);
			break;
		case MSG_PURGE:
			purge_requests(m->owner_nodeid, m->owner_pid);
			break;
		case MSG_JOIN:
			process_join(m, nodeid, pid);
			break;
		default:
			qb_log(LOG_DEBUG, "Unknown request: %d from %u:%u\n",
				m->request, nodeid, pid);
			break;
	}

	return 0;
}


static void
cpg_deliver_func(cpg_handle_t h,
		 const struct cpg_name *group_name,
		 uint32_t nodeid,
		 uint32_t pid,
		 void *msg,
		 size_t msglen)
{
	if (msglen == sizeof(struct cpg_lock_msg))
		process_request((struct cpg_lock_msg *) msg, nodeid, pid);
	else
		qb_log(LOG_DEBUG, "Invalid message size %lu\n", msglen);
}


static void
cpg_config_change(	cpg_handle_t h,
					const struct cpg_name *group_name,
					const struct cpg_address *members, size_t memberlen,
					const struct cpg_address *left, size_t leftlen,
					const struct cpg_address *join, size_t joinlen)
{
	struct member_item *n;
	size_t x, y;
	struct cpg_lock_msg m;
	int cpglock_members_removed = 0;

	memset(&m, 0, sizeof(m));
	strncpy(m.resource, "(none)", sizeof(m.resource));
	m.request = MSG_CONFCHG;

	old_msg(&m);

	if (total_members == 0) {
		qb_log(LOG_TRACE, "JOIN: Setting up initial node list\n");
		for (x = 0; x < memberlen; x++) {
			for (y = 0; y < joinlen; y++) {
				if (join[y].nodeid == members[x].nodeid)
					continue;
				if (members[x].nodeid == my_node_id)
					continue;

				n = wait_calloc(sizeof(*n));
				n->nodeid = members[x].nodeid;
				n->pid = members[x].pid;
				qb_list_init(&n->list);
				qb_list_add_tail(&n->list, &group_members);
				qb_log(LOG_TRACE, "JOIN: node %u.%u\n", n->nodeid, n->pid);
			}
		}
		qb_log(LOG_TRACE, "JOIN: Done\n");

		total_members = memberlen;
	}

	/*
	** process join on receipt of JOIN message rather than here
	** since ordered delivery is agreed, this prevents >1 member from
	** believing it is the oldest host
	*/

	for (x = 0; x < leftlen; x++) {
		qb_list_for_each_entry(n, &group_members, list) {
			if (n->nodeid == left[x].nodeid) {
				if (n->pid == left[x].pid) {
					if (n->nodeid == my_node_id) {
						qb_log(LOG_ERR,
							"Received DELETE message for self. Exiting.\n");
						exit(0);
					}
					qb_log(LOG_TRACE, "DELETE: node %u.%u\n",
						n->nodeid, n->pid);
					qb_list_del(&n->list);
					del_node(n->nodeid);
					cpglock_members_removed++;
					free(n);
					break;
				} else {
					qb_log(LOG_TRACE, "DUPE NODE %u LEFT (%u != %u)\n",
						n->nodeid, n->pid, left[x].pid);
				}
			}
		}
	}

	total_members -= cpglock_members_removed;
	if (total_members < 0) {
		qb_log(LOG_DEBUG, "Invalid group member count: %d\n", total_members);
		total_members = 0;
	}

	return;
}


static cpg_callbacks_t my_callbacks = {
	.cpg_deliver_fn = cpg_deliver_func,
	.cpg_confchg_fn = cpg_config_change
};

static int
cpg_fin(void)
{
	struct cpg_name gname;

	gname.length = snprintf(gname.value,
					sizeof(gname.value), "%s", CPG_LOCKD_NAME);

	if (gname.length >= sizeof(gname.value)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (gname.length <= 0) {
		errno = EINVAL;
		return -1;
	}

	if (cpg_leave(cpg, &gname) != CS_OK) {
		qb_log(LOG_ERR, "cpg_leave failed for %s: %s\n",
			CPG_LOCKD_NAME, strerror(errno));
		return -1;
	}

	if (cpg_finalize(cpg) != CS_OK) {
		qb_log(LOG_ERR, "cpg_finalize failed for %s: %s\n",
			CPG_LOCKD_NAME, strerror(errno));
		return -1;
	}

	return 0;
}

static int
cpg_init(void)
{
	struct cpg_name gname;
	struct cpg_address member_list[64];
	int cpg_member_list_len = 0;
	int i, ret;

	errno = EINVAL;

	gname.length = snprintf(gname.value, sizeof(gname.value), CPG_LOCKD_NAME);
	if (gname.length >= sizeof(gname.value)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	if (gname.length <= 0)
		return -1;

	memset(&cpg, 0, sizeof(cpg));
	if (cpg_initialize(&cpg, &my_callbacks) != CS_OK) {
		qb_log(LOG_ERR, "cpg_initialize failed\n");
		return -1;
	}

	if (cpg_join(cpg, &gname) != CS_OK) {
		qb_log(LOG_ERR, "cpg_join failed\n");
		return -1;
	}

	if (cpg_local_get(cpg, &my_node_id) != CS_OK) {
		qb_log(LOG_ERR, "cpg_local_get failed\n");
		return -1;
	}

	ret = cpg_membership_get(cpg, &gname, member_list, &cpg_member_list_len);
	if (ret != CS_OK) {
		qb_log(LOG_ERR,
			"cpg_membership_get() failed: %s\n", strerror(errno));
		cpg_fin();
		return -1;
	}

	for (i = 0 ; i < cpg_member_list_len ; i++) {
		if (member_list[i].nodeid == my_node_id) {
			if (member_list[i].pid != (uint32_t) getpid()) {
				qb_log(LOG_ERR,
					"nodeid %u is already in the CPG group with PID %u %u\n",
					member_list[i].nodeid, member_list[i].pid, getpid());
				cpg_fin();
				return -1;
			}
		}
	}

	return 0;
}

static uint32_t drop_all_locks(void) {
	qb_map_iter_t *iter;
	uint32_t dropped_locks = 0;

	iter = qb_map_iter_create(locks);
	if (iter) {
		void *data;

		while (qb_map_iter_next(iter, &data)) {
			struct cpg_lock *l = (struct cpg_lock *) data;

			qb_log(LOG_TRACE,
				"CLEAR STATE: Dropping lock %s [%u:%u:%u]\n",
				l->resource, l->owner_nodeid, l->owner_pid, l->owner_tid);
			qb_map_rm(locks, l->resource);
			free(l);
			++dropped_locks;
		}
	}

	qb_log(LOG_TRACE, "CLEAR STATE: Dropped %u locks\n",
		dropped_locks);

	return dropped_locks;
}

static uint32_t drop_all_requests(void) {
	uint32_t dropped_reqs = 0;
	struct request_item *r, *n;

	qb_list_for_each_entry_safe(r, n, &requests, list) {
		if (r->l.owner_nodeid == my_node_id) {
			nak_client(r);
			qb_log(LOG_TRACE, "CLEAR STATE: sent NAK %s [%u:%u:%u]\n",
				r->l.resource, r->l.owner_nodeid,
				r->l.owner_pid, r->l.owner_tid);
		}
		qb_log(LOG_TRACE, "CLEAR STATE: Dropping request %s [%u:%u:%u]\n",
			r->l.resource, r->l.owner_nodeid,
			r->l.owner_pid, r->l.owner_tid);
		qb_list_del(&r->list);
		free(r);
		++dropped_reqs;
	}

	qb_log(LOG_TRACE, "CLEAR STATE: Dropped %u requests\n",
		dropped_reqs);

	return dropped_reqs;
}

static uint32_t drop_all_clients(void) {
	struct client_item *c, *n;
	uint32_t dropped_clients = 0;

	qb_list_for_each_entry_safe(c, n, &clients, list) {
		qb_loop_poll_del(main_loop, c->fd);
		qb_list_del(&c->list);
		close(c->fd);
		free(c);
		++dropped_clients;
	}

	qb_log(LOG_TRACE, "CLEAR STATE: Dropped %u clients\n",
		dropped_clients);

	return dropped_clients;
}

static uint32_t drop_all_saved_msgs(void) {
	struct msg_item *m, *n;
	uint32_t dropped_msgs = 0;

	qb_list_for_each_entry_safe(m, n, &messages, list) {
		qb_list_del(&m->list);
		free(m);
		++dropped_msgs;
	}

	qb_log(LOG_TRACE, "CLEAR STATE: Dropped %u saved messages\n",
		dropped_msgs);

	return dropped_msgs;
}

static uint32_t drop_all_members(void) {
	struct member_item *m, *n;
	uint32_t dropped_members = 0;

	qb_list_for_each_entry_safe(m, n, &group_members, list) {
		qb_list_del(&m->list);
		free(m);
		++dropped_members;
	}

	qb_log(LOG_TRACE, "CLEAR STATE: Dropped %u group members\n",
		dropped_members);

	return dropped_members;
}

static int parse_opts(int argc, char **argv) {
	int opt;

	while ((opt = getopt(argc, argv, "fm:s:hd")) != EOF) {
		switch (opt) {
			case 'f':
				nofork = 1;
				break;
			case 'd':
				debug_mode = 1;
				break;
			case 's':
				cpglockd_sock_path = strdup(optarg);
				break;
			case 'm': {
				unsigned long msgs;
				char *p = NULL;

				msgs = strtoul(optarg, &p, 10);
				if (*p != '\0' || (msgs == ULONG_MAX && errno == ERANGE)) {
					fprintf(stderr,
						"Invalid value for maximum saved messages: %s\n",
						optarg);
					return -1;
				}

				max_saved_msgs = msgs;
				break;
			}
			case 'h':
				printf("Usage: %s [options]\n\
 -f          Don't daemonize\n\
 -m <msgs>   Number of old messages to store (default: %lu)\n\
 -s <path>   Path to the cpglockd socket (default: %s)\n\
 -d          Enable debug mode\n\
 -h          Print this help message\n",
					argv[0], max_saved_msgs, CPG_LOCKD_SOCK);
				return -1;

			default:
				return -1;
		}
	}

	if (!cpglockd_sock_path)
		cpglockd_sock_path = strdup(CPG_LOCKD_SOCK);

	return 0;
}

int
main(int argc, char **argv)
{
	int fd;
	int cpgfd;
	int exit_status = 0;

	if (parse_opts(argc, argv) != 0)
		exit(1);

	if (!nofork) {
		daemon_init((char *) "cpglockd");
	} else {
		pid_t pid;

		if (check_process_running((char *) "cpglockd", &pid) &&
			pid != getpid())
		{
			fprintf(stderr, "cpglockd is already running\n");
			return -1;
		}
		update_pidfile((char *) "cpglockd");
	}

	qb_log_init("cpglockd", LOG_DAEMON, LOG_INFO);

	main_loop = qb_loop_create();

	if (nofork) {
		qb_log_ctl(QB_LOG_STDERR, QB_LOG_CONF_ENABLED, QB_TRUE);
		qb_log_filter_ctl(QB_LOG_STDERR, QB_LOG_FILTER_ADD,
			QB_LOG_FILTER_FILE, "*", LOG_TRACE);
	}

	if (debug_mode) {
		qb_log_ctl(QB_LOG_SYSLOG,
			QB_LOG_CONF_PRIORITY_BUMP, LOG_TRACE - LOG_INFO);
	}

	qb_loop_signal_add(main_loop, QB_LOOP_HIGH, SIGTERM,
			NULL, flag_shutdown, NULL);
	qb_loop_signal_add(main_loop, QB_LOOP_HIGH, SIGINT,
			NULL, flag_shutdown, NULL);
	qb_loop_signal_add(main_loop, QB_LOOP_HIGH, SIGPIPE,
			NULL, dummy_sigcb, NULL);

	if (shutdown_pending)
		goto out2;

	if (cpg_init() < 0) {
		qb_log(LOG_ERR, "Unable to join CPG group: %s\n", strerror(errno));
		exit_status = -1;
		goto out2;
	}

	fd = sock_listen(cpglockd_sock_path);
	if (fd < 0) {
		qb_log(LOG_ERR, "Error connecting to %s: %s\n",
			cpglockd_sock_path, strerror(errno));
		exit_status = -1;
		goto out1;
	}

	cpg_fd_get(cpg, &cpgfd);
	if (cpgfd < 0 || send_join() < 0) {
		qb_log(LOG_ERR, "Unable to complete join to CPG group\n");
		exit_status = -1;
		goto out;
	}

	qb_loop_poll_add(main_loop, QB_LOOP_HIGH, cpgfd,
		POLLIN, NULL, cpg_event_cb);
	qb_loop_poll_add(main_loop, QB_LOOP_MED, fd,
		POLLIN, NULL, client_connect_cb);

	/* XXX - using trie because hash and skiplist seem to be broken */
	locks = qb_trie_create();
	if (!locks) {
		qb_log(LOG_ERR, "Unable to create lock map\n");
		goto out;
	}

	qb_log(LOG_INFO, "cpglockd entering normal operation\n");

	if (!shutdown_pending)
		qb_loop_run(main_loop);

	drop_all_locks();
	drop_all_requests();
	drop_all_clients();
	drop_all_saved_msgs();
	drop_all_members();

	qb_map_destroy(locks);

out:
	close(fd);
	unlink(cpglockd_sock_path);
out1:
	cpg_fin();
out2:
	qb_loop_destroy(main_loop);
	qb_log_fini();

	daemon_cleanup();
	free(cpglockd_sock_path);

	return exit_status;
}
