/* 
 * Copyright (C) Shivaram Upadhyayula <shivaram.u@quadstor.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * Version 2 as published by the Free Software Foundation
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, 
 * Boston, MA  02110-1301, USA.
 */

#include "cluster.h"
#include "bdevmgr.h"
#include "tdisk.h"
#include "../common/cluster_common.h" 
#include "qs_lib.h"
#include "ddthread.h"
#include "node_sock.h"
#include "node_mirror.h"
#include "bdevgroup.h"
#include "tcache.h"

static int node_type;
uint64_t ntransaction_id;
mtx_t *node_transaction_lock;

int
node_type_client(void)
{
	return atomic_test_bit(NODE_TYPE_CLIENT, &node_type);
}

int
node_type_master(void)
{
	return atomic_test_bit(NODE_TYPE_MASTER, &node_type);
}

int
node_type_controller(void)
{
	return atomic_test_bit(NODE_TYPE_CONTROLLER, &node_type);
}

int
node_type_receiver(void)
{
	return atomic_test_bit(NODE_TYPE_RECEIVER, &node_type);
}

void
node_sock_free(struct node_sock *sock, int linger)
{
	if (sock->lsock)
		sock_close(sock->lsock, linger);

	if (sock->task)
		kernel_thread_stop(sock->task, &sock->flags, sock->sock_wait, NODE_SOCK_EXIT);

	if (sock->lsock)
		sock_free(sock->lsock);

	wait_chan_free(sock->sock_wait);
	mtx_free(sock->sock_lock);
	uma_zfree(node_sock_cache, sock);
}

struct node_sock *
__node_sock_alloc(struct node_comm *comm, int (*sock_callback) (struct node_sock *))
{
	struct node_sock *sock;

	sock = __uma_zalloc(node_sock_cache, Q_WAITOK);
	SLIST_INIT(&sock->accept_list);
	sock->comm = comm;
	sock->sock_callback = sock_callback;
	sock->sock_wait = wait_chan_alloc("node sock wait");
	sock->sock_lock = mtx_alloc("node sock lock");
	return sock;
}

struct node_sock *
node_sock_alloc(struct node_comm *comm, int (*sock_callback) (struct node_sock *), sock_t *lsock, char *name)
{
	struct node_sock *sock;
	int retval;

	sock = __node_sock_alloc(comm, sock_callback);
	if (!lsock) {
		sock->lsock = sock_create(sock);
		if (unlikely(!sock->lsock)) {
			node_sock_free(sock, 1);
			return NULL;
		}
	}
	else
		sock->lsock = lsock;

	if (sock_callback) {
		retval = kernel_thread_create(node_sock_recv_thr, sock, sock->task, name);
		if (unlikely(retval != 0)) {
			node_sock_free(sock, 1);
			return NULL;
		}
	}
	TAILQ_INSERT_TAIL(&comm->sock_list, sock, s_list);

	return sock;
}

static void
comm_free_sock_list(struct node_comm *comm)
{
	struct node_sock *iter;
	int linger = atomic_test_bit(NODE_COMM_LINGER, &comm->flags);

	while ((iter = TAILQ_FIRST(&comm->sock_list)) != NULL) {
		TAILQ_REMOVE(&comm->sock_list, iter, s_list);
		node_sock_free(iter, linger);
		if (!linger)
			pause("psg", 20);
	}
}

void
node_comm_free(struct node_comm *comm)
{
	/* free sock list */
	comm_free_sock_list(comm);

	sx_free(comm->comm_lock);
	wait_chan_free(comm->comm_wait);
	uma_zfree(node_comm_cache, comm);
}

void
node_comm_put(struct node_comm *comm)
{
	debug_check(!atomic_read(&comm->refs));
	if (atomic_dec_and_test(&comm->refs))
		node_comm_free(comm);
}

struct node_comm *
node_comm_locate(struct node_msg_list *node_hash, uint32_t ipaddr, struct node_comm *root)
{
	struct node_comm *comm;

	node_comm_lock(root);

	SLIST_FOREACH(comm, &root->comm_list, c_list) {
		if (comm->node_ipaddr == ipaddr && !atomic_test_bit(NODE_COMM_UNREGISTERED, &comm->flags)) {
			node_comm_unlock(root);
			return comm;
		}
	}

	comm = node_comm_alloc(node_hash, root->node_ipaddr, ipaddr);
	SLIST_INSERT_HEAD(&root->comm_list, comm, c_list);
	node_comm_unlock(root);
	return comm;
}

struct node_comm * 
node_comm_alloc(struct node_msg_list *node_hash, uint32_t controller_ipaddr, uint32_t node_ipaddr)
{
	struct node_comm *comm;

	comm = __uma_zalloc(node_comm_cache, Q_WAITOK);
	comm->comm_wait = wait_chan_alloc("node comm wait");
	comm->controller_ipaddr = controller_ipaddr;
	comm->node_ipaddr = node_ipaddr;
	comm->comm_lock = sx_alloc("node comm lock");
	comm->node_hash = node_hash;
	atomic_set(&comm->refs, 1);
	atomic_set_bit(NODE_COMM_LINGER, &comm->flags);
	TAILQ_INIT(&comm->sock_list);
	TAILQ_INIT(&comm->free_sock_list);
	return comm;
}

void
node_resp_free(struct node_msg *msg)
{
	if (msg->resp) {
		node_msg_free(msg->resp);
		msg->resp = NULL;
	}
}

void
page_list_free(pagestruct_t **pages, int pg_count)
{
	pagestruct_t *page;
	int i;

	for (i = 0; i < pg_count; i++) {
		page = pages[i];
		vm_pg_free(page);
	}
	free(pages, M_PAGE_LIST);
}

void
node_msg_free(struct node_msg *msg)
{
	if (msg->resp)
		node_msg_free(msg->resp);

	if (msg->pages)
		page_list_free(msg->pages, msg->pg_count);

	if (msg->tdisk)
		tdisk_put(msg->tdisk);

	if (msg->raw)
		free(msg->raw, M_NODE_RMSG);
	wait_completion_free(msg->completion);
	uma_zfree(node_msg_cache, msg);
}

struct node_msg *
node_msg_alloc(int msg_len)
{
	struct node_msg *msg;

	msg = __uma_zalloc(node_msg_cache, Q_WAITOK);
	msg->raw = malloc(msg_len + sizeof(struct raw_node_msg), M_NODE_RMSG, Q_WAITOK);
	msg->completion = wait_completion_alloc("node msg");
	return msg;
}

#define NODE_RECV_HASH_BUCKETS		4096
#define NODE_RECV_HASH_MASK		(NODE_RECV_HASH_BUCKETS - 1)

struct node_msg_list node_client_hash[NODE_RECV_HASH_BUCKETS];
struct node_msg_list node_client_accept_hash[NODE_RECV_HASH_BUCKETS];
struct node_msg_list node_usr_hash[NODE_RECV_HASH_BUCKETS];
struct node_msg_list node_master_hash[NODE_RECV_HASH_BUCKETS];
struct node_msg_list node_rep_send_hash[NODE_RECV_HASH_BUCKETS];
struct node_msg_list node_rep_recv_hash[NODE_RECV_HASH_BUCKETS];

void
node_msg_hash_cancel(struct node_msg_list *node_hash)
{
	int i;
	struct node_msg_list *msg_list;
	struct node_msg *iter, *next;

	for (i = 0; i < NODE_RECV_HASH_BUCKETS; i++) {
		msg_list = &node_hash[i];
		mtx_lock(msg_list->list_lock);
		LIST_FOREACH_SAFE(iter, &msg_list->msgs, c_list, next) {
			LIST_REMOVE_INIT(iter, c_list);
			wait_complete_all(iter->completion);
		}
		mtx_unlock(msg_list->list_lock);
	}
}

static void
node_msg_queue_lock(mtx_t *queue_lock)
{
	if (queue_lock)
		mtx_lock(queue_lock);
}

static void
node_msg_queue_unlock(mtx_t *queue_lock)
{
	if (queue_lock)
		mtx_unlock(queue_lock);
}

static void
node_msg_queue_remove(struct node_msg *msg)
{
	if (!msg->queue_list)
		return;

	TAILQ_REMOVE_INIT(msg->queue_list, msg, q_list);
}

static void
node_msg_timeout(struct node_msg *msg)
{
	struct qsio_scsiio *ctio;
	uint8_t *cdb;

	ctio = msg->ctio;
	debug_check(!ctio);
	cdb = ctio->cdb;
	debug_warn("Timing out ctio for cmd %x tdisk %s msg timestamp %llu current %llu msg cmd %d msg id %llx xchg id %llx\n", cdb[0], tdisk_name(msg->tdisk), (unsigned long long)msg->timestamp, (unsigned long long)ticks, msg->raw->msg_cmd, (unsigned long long)msg->raw->msg_id, (unsigned long long)msg->raw->xchg_id);

	if (cdb[0] == WRITE_16) {
		node_master_write_error(msg->tdisk, msg->wlist, ctio);
		if (tdisk_mirroring_configured(msg->tdisk))
			tdisk_mirror_peer_failure(msg->tdisk, 0);
	}
	else if (cdb[0] == READ_16) {
		node_master_read_error(msg->tdisk, msg->wlist, ctio);
	}
	else if (cdb[0] == EXTENDED_COPY) {
		free_block_refs(msg->tdisk, &msg->wlist->index_info_list);
		node_master_read_error(msg->tdisk, msg->wlist, ctio);
	}

	if (msg->tcache) {
		wait_for_done(msg->tcache->completion);
		tcache_put(msg->tcache);
	}

	node_master_end_ctio(ctio);
	node_msg_cleanup(msg);
}


static int
__node_cmd_hash_remove(struct node_msg_list msg_hash[], struct node_msg *msg, uint32_t id)
{
	int idx = (id & NODE_RECV_HASH_MASK);
	struct node_msg_list *msg_list;
	int removed = 0;

	msg_list = &msg_hash[idx];
	mtx_lock(msg_list->list_lock);
	if (msg->c_list.le_next != NULL || msg->c_list.le_prev != NULL) {
		LIST_REMOVE_INIT(msg, c_list);
		removed = 1;
	}
	mtx_unlock(msg_list->list_lock);

	return removed;
}

static int
sock_in_delete_list(struct sock_list *sock_list, struct node_sock *sock)
{
	struct node_sock *iter;

	TAILQ_FOREACH(iter, sock_list, s_list) {
		if (iter == sock)
			return 1;
	}
	return 0;
}

void
node_clear_comm_msgs(struct node_msg_list *node_hash, struct queue_list *queue_list, mtx_t *queue_lock, struct node_comm *comm, struct sock_list *sock_list)
{
	struct queue_list tmp_list;
	struct node_msg *msg, *next;
	int removed;

	TAILQ_INIT(&tmp_list);
	mtx_lock(queue_lock);
	TAILQ_FOREACH_SAFE(msg, queue_list, q_list, next) {
		if ((msg->sock->comm != comm) && (!sock_list || !sock_in_delete_list(sock_list, msg->sock)))
			continue;

		removed = __node_cmd_hash_remove(node_hash, msg, msg->raw->xchg_id);
		if (!removed)
			continue;
		TAILQ_REMOVE(queue_list, msg, q_list);
		TAILQ_INSERT_TAIL(&tmp_list, msg, q_list);
	}
	mtx_unlock(queue_lock);

	while ((msg = TAILQ_FIRST(&tmp_list)) != NULL) {
		TAILQ_REMOVE(&tmp_list, msg, q_list);
		node_msg_timeout(msg);
	}
}

void
node_check_timedout_msgs(struct node_msg_list *node_hash, struct queue_list *queue_list, mtx_t *queue_lock, uint32_t timeout_msecs)
{
	struct queue_list tmp_list;
	struct node_msg *msg, *next;
	int removed;
	unsigned long elapsed;

	TAILQ_INIT(&tmp_list);
	mtx_lock(queue_lock);
	TAILQ_FOREACH_SAFE(msg, queue_list, q_list, next) {
		elapsed = get_elapsed(msg->timestamp);
		if (ticks_to_msecs(elapsed) < timeout_msecs)
			break;
		debug_warn("elapsed %llu timeout msecs %u msg id %llx msg cmd %d\n", (unsigned long long)ticks_to_msecs(elapsed), timeout_msecs, (unsigned long long)msg->raw->msg_id, msg->raw->msg_cmd);
		removed = __node_cmd_hash_remove(node_hash, msg, msg->raw->xchg_id);
		if (!removed)
			continue;
		TAILQ_REMOVE(queue_list, msg, q_list);
		TAILQ_INSERT_TAIL(&tmp_list, msg, q_list);
	}
	mtx_unlock(queue_lock);

	while ((msg = TAILQ_FIRST(&tmp_list)) != NULL) {
		TAILQ_REMOVE(&tmp_list, msg, q_list);
		node_msg_timeout(msg);
	}
}

static void
node_msg_queue_insert(struct node_msg *msg)
{
	if (!msg->queue_list)
		return;

	TAILQ_INSERT_TAIL(msg->queue_list, msg, q_list);
}

int
node_cmd_hash_remove(struct node_msg_list *node_hash, struct node_msg *msg, uint32_t id)
{
	int removed;

	node_msg_queue_lock(msg->queue_lock);
	removed = __node_cmd_hash_remove(node_hash, msg, id);
	node_msg_queue_remove(msg);
	node_msg_queue_unlock(msg->queue_lock);
	return removed;
}

struct node_msg *
node_cmd_lookup(struct node_msg_list *node_hash, uint64_t id, struct queue_list *queue_list, mtx_t *queue_lock)
{
	int idx = (id & NODE_RECV_HASH_MASK);
	struct node_msg_list *msg_list;
	struct node_msg *ret = NULL, *iter;

	node_msg_queue_lock(queue_lock);
	msg_list = &node_hash[idx];
	mtx_lock(msg_list->list_lock);
	LIST_FOREACH(iter, &msg_list->msgs, c_list) {
		if (iter->id == id) {
			LIST_REMOVE_INIT(iter, c_list);
			ret = iter;
			break;
		}
	}
	mtx_unlock(msg_list->list_lock);

	if (ret)
		node_msg_queue_remove(ret);
	node_msg_queue_unlock(queue_lock);

	return ret;
}

void
node_cmd_hash_insert(struct node_msg_list *node_hash, struct node_msg *msg, uint32_t id)
{
	int idx = (id & NODE_RECV_HASH_MASK);
	struct node_msg_list *msg_list;

	node_msg_queue_lock(msg->queue_lock);
	msg->id = id;
	msg->timestamp = ticks;
	msg_list = &node_hash[idx];
	mtx_lock(msg_list->list_lock);
	LIST_INSERT_HEAD(&msg_list->msgs, msg, c_list); 
	mtx_unlock(msg_list->list_lock);
	node_msg_queue_insert(msg);
	node_msg_queue_unlock(msg->queue_lock);
}

static void
node_hash_init(struct node_msg_list *node_hash)
{
	int i;
	struct node_msg_list *msg_list;

	for (i = 0; i < NODE_RECV_HASH_BUCKETS; i++) {
		msg_list = &node_hash[i];
		LIST_INIT(&msg_list->msgs);
		msg_list->list_lock = mtx_alloc("msg list lock");
	}
}

void
node_init(void)
{
	node_transaction_lock = mtx_alloc("node trans lock");
	node_hash_init(node_client_hash);
	node_hash_init(node_client_accept_hash);
	node_hash_init(node_usr_hash);
	node_hash_init(node_master_hash);
	node_hash_init(node_rep_send_hash);
	node_hash_init(node_rep_recv_hash);
}

uint64_t sock_page_writes;

int
node_sock_write_page(struct node_sock *sock, pagestruct_t *page, int dxfer_len)
{
	int retval, offset = 0;

	while (1) {
		if (atomic_test_bit(NODE_SOCK_EXIT, &sock->flags) || sock_state_error(sock))
			return -1;
		retval = sock_write_page(sock->lsock, page, offset, dxfer_len);
		if (unlikely(retval < 0)) {
			debug_warn("write of %d bytes offset %d failed\n", dxfer_len, offset);
			node_sock_write_error(sock);
			return retval;
		}

		GLOB_INC(sock_page_writes, retval);
		dxfer_len -= retval;
		if (!dxfer_len)
			return 0;
		offset += retval;
		node_sock_wait_for_write(sock);
		if (atomic_test_bit(NODE_SOCK_WRITE_WAIT, &sock->flags))
			return -1;
	}
	return 0;
}

uint64_t sock_writes;

int
node_sock_write_data(struct node_sock *sock, uint8_t *buffer, int dxfer_len)
{
	int retval;

	while (1) {
		if (atomic_test_bit(NODE_SOCK_EXIT, &sock->flags) || sock_state_error(sock))
			return -1;

		retval = sock_write(sock->lsock, buffer, dxfer_len);
		if (unlikely(retval < 0)) {
			debug_warn("write of %d bytes failed\n", dxfer_len);
			node_sock_write_error(sock);
			return retval;
		}

		dxfer_len -= retval;
		GLOB_INC(sock_writes, retval);
		if (!dxfer_len)
			return 0;
		buffer += retval;
		node_sock_wait_for_write(sock);
		if (atomic_test_bit(NODE_SOCK_WRITE_WAIT, &sock->flags))
			return -1;
	}
	return 0;

}

int 
node_sock_write(struct node_sock *sock, struct raw_node_msg *raw)
{
	uint8_t *buffer = (uint8_t *)raw;
	int dxfer_len = (raw->dxfer_len + sizeof(*raw));

	return node_sock_write_data(sock, buffer, dxfer_len);
}

void
node_sock_read_error(struct node_sock *sock)
{
	atomic_set_bit(NODE_SOCK_READ_ERROR, &sock->flags);
	chan_wakeup(sock->sock_wait);
}

void
node_sock_write_error(struct node_sock *sock)
{
	atomic_set_bit(NODE_SOCK_READ_ERROR, &sock->flags);
	chan_wakeup(sock->sock_wait);
}

void
node_sock_finish(struct node_sock *sock)
{
	struct node_comm *comm = sock->comm;

	node_comm_lock(comm);
	atomic_clear_bit(NODE_SOCK_BUSY, &sock->flags);
	if (!sock_state_error(sock))
		TAILQ_INSERT_HEAD(&comm->free_sock_list, sock, f_list);
	chan_wakeup_nointr(comm->comm_wait);
	node_comm_unlock(comm);
}

int
node_comm_free_sock_count(struct node_comm *comm)
{
	int count = 0;
	struct node_sock *iter;

	TAILQ_FOREACH(iter, &comm->sock_list, s_list) {
		if (sock_state_error(iter))
			continue;
		count++;
	}
	return count;
}

void
node_comm_cleanup(struct node_comm *comm, struct sock_list *sock_list)
{
	struct node_sock *iter, *next;

	node_comm_lock(comm);
	TAILQ_FOREACH_SAFE(iter, &comm->sock_list, s_list, next) {
		if (!sock_state_error(iter))
			continue;

		TAILQ_REMOVE(&comm->sock_list, iter, s_list);
		TAILQ_INSERT_TAIL(sock_list, iter, s_list);
	}
	node_comm_unlock(comm);
	atomic_clear_bit(NODE_COMM_CLEANUP, &comm->flags);
}

struct node_sock *
node_comm_get_sock(struct node_comm *comm, int wait)
{
	struct node_sock *sock, *next;
	int have_free_socks;

	node_comm_lock(comm);

retry:
	/* First check if there is a free socket to use */
	have_free_socks = 0;

	if (!TAILQ_EMPTY(&comm->free_sock_list)) {	
		sock = TAILQ_FIRST(&comm->free_sock_list);
		TAILQ_REMOVE(&comm->free_sock_list, sock, f_list);
		if (sock_state_error(sock))
			goto retry;
		atomic_set_bit(NODE_SOCK_BUSY, &sock->flags);
		node_comm_unlock(comm);
		return sock;
	}

	TAILQ_FOREACH_SAFE(sock, &comm->sock_list, s_list, next) {
		if (atomic_test_bit(NODE_SOCK_BUSY, &sock->flags))
			continue;

		if (!sock_state_error(sock)) {
			if (sock->state == SOCK_STATE_CONNECTED) {
				TAILQ_INSERT_TAIL(&comm->free_sock_list, sock, f_list);
				have_free_socks = 1;
			}
			continue;
		}

		TAILQ_REMOVE(&comm->sock_list, sock, s_list);
		node_sock_free(sock, atomic_test_bit(NODE_COMM_LINGER, &comm->flags));
	}

	if (have_free_socks) {
		chan_wakeup_nointr(comm->comm_wait);
		goto retry;
	}

	if (!TAILQ_EMPTY(&comm->sock_list) && wait) {
		node_comm_unlock(comm);
		wait_on_chan_timeout(comm->comm_wait, !TAILQ_EMPTY(&comm->free_sock_list) || TAILQ_EMPTY(&comm->sock_list), 1000);
		wait -= 1000;
		node_comm_lock(comm);
		goto retry;
	}

	/* We are out of socket connections */
	/* Try to reconnect to master, returning NULL on a connect failure */
	node_comm_unlock(comm);
	return NULL;
}

void
node_msg_compute_csum(struct raw_node_msg *raw)
{
	uint8_t *ptr = (uint8_t *)(raw);

	raw->data_csum = net_calc_csum16(raw->data, raw->dxfer_len);
	raw->csum = net_calc_csum16((ptr + sizeof(uint16_t)), sizeof(*raw) - sizeof(uint16_t));
}

int
node_msg_csum_valid(struct raw_node_msg *raw)
{
	uint8_t *ptr = (uint8_t *)(raw);
	uint16_t csum;

	csum = net_calc_csum16((ptr + sizeof(uint16_t)), sizeof(*raw) - sizeof(uint16_t));
	return (csum == raw->csum);
}

int
node_send_msg(struct node_sock *sock, struct node_msg *msg, uint64_t id, int resp)
{
	int retval;

	if (resp)
		node_cmd_hash_insert(sock->comm->node_hash, msg, id);

	node_msg_compute_csum(msg->raw);
	retval = node_sock_write(sock, msg->raw);
	if (retval != 0 && resp)
		node_cmd_hash_remove(sock->comm->node_hash, msg, id);

	return retval;
}

char discard_buf[262144]; /* None of our messages are greater than 256 K */

int
node_msg_discard_pages(struct node_sock *sock, int pg_count)
{
	int i, retval;

	for (i = 0; i < pg_count; i++) {
		retval =  node_sock_read_nofail(sock, discard_buf, LBA_SIZE);
		if (unlikely(retval != 0))
			return retval;
	}
	return 0;
}

int
node_msg_discard(struct node_sock *sock, struct raw_node_msg *raw)
{
	int retval;

	retval = node_msg_discard_data(sock, raw->dxfer_len);
	if (unlikely(retval != 0))
		return -1;

	if (raw->pg_count) {
		retval = node_msg_discard_pages(sock, raw->pg_count);
		if (unlikely(retval != 0))
			return -1;
	}
	return 0;
}

int
node_msg_discard_data(struct node_sock *sock, int dxfer_len)
{
	int retval, min_len;

	debug_check(dxfer_len > sizeof(discard_buf));

	while (dxfer_len) {
		min_len = min_t(int, sizeof(discard_buf), dxfer_len);
		retval = node_sock_read_nofail(sock, discard_buf, min_len);
		if (retval != 0)
			return retval;
		dxfer_len -= min_len;
	}
	return 0;
}


void
node_resp_msg(struct node_sock *sock, struct raw_node_msg *msg, int msg_status)
{
	struct node_msg *resp;

	resp = node_msg_alloc(0);
	memcpy(resp->raw, msg, sizeof(*msg));
	resp->raw->dxfer_len = 0;
	resp->raw->pg_count = 0;
	resp->raw->msg_status = msg_status;
	node_send_msg(sock, resp, 0, 0);
	node_msg_free(resp);
}

void
node_error_resp_msg(struct node_sock *sock, struct raw_node_msg *msg, int msg_status)
{
	struct node_msg *resp;
	int retval;

	retval = node_msg_discard_data(sock, msg->dxfer_len);
	if (unlikely(retval != 0))
		return;

	resp = node_msg_alloc(0);
	memcpy(resp->raw, msg, sizeof(*msg));
	resp->raw->dxfer_len = 0;
	resp->raw->pg_count = 0;
	resp->raw->msg_status = msg_status;
	node_send_msg(sock, resp, 0, 0);
	node_msg_free(resp);
}

static void
node_hash_free(struct node_msg_list *node_hash, char *name)
{
	struct node_msg_list *msg_list;
	int i;

	for (i = 0; i < NODE_RECV_HASH_BUCKETS; i++) {
		msg_list = &node_hash[i];
		if (!LIST_EMPTY(&msg_list->msgs))
			debug_warn("hash %s not empty\n", name);
		mtx_free(msg_list->list_lock);
	}
}

void
node_exit(void)
{
	if (!node_transaction_lock)
		return;

	mtx_free(node_transaction_lock);
	node_hash_free(node_client_hash, "node_client_hash");
	node_hash_free(node_client_accept_hash, "node_client_accept_hash");
	node_hash_free(node_usr_hash, "node_usr_hash");
	node_hash_free(node_master_hash, "node_master_hash");
	node_hash_free(node_rep_send_hash, "node_rep_send_hash");
	node_hash_free(node_rep_recv_hash, "node_rep_recv_hash");
	node_transaction_lock = NULL;
}

extern int recv_flags;

uint64_t
node_get_tprt(void)
{
	if (node_type_receiver())
		return recv_config.recv_ipaddr;
	else
		return 1;
}

int
node_status(struct node_config *node_config)
{
	bzero(node_config, sizeof(*node_config));
	if (node_type_receiver()) {
		if (atomic_test_bit(MASTER_BIND_ERROR, &recv_flags))
			node_config->recv_flags = MASTER_BIND_ERROR;
		else if (atomic_test_bit(MASTER_INITED, &recv_flags))
			node_config->recv_flags = MASTER_INITED;
		strcpy(node_config->recv_host, recv_config.recv_host);
		node_config->recv_ipaddr = recv_config.recv_ipaddr;
	}
	return 0;
}

int
node_config(struct node_config *node_config)
{
	int retval = -1;

	if (node_config->node_type == NODE_TYPE_RECEIVER)
		retval = node_recv_init(node_config);

	return (retval >= 0) ? 0 : retval;
}

uint16_t
pgdata_csum(struct pgdata *pgdata, int len)
{
	return (calc_csum16(pgdata_page_address(pgdata), len));
}
