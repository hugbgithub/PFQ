/***************************************************************
 *
 * (C) 2011-16 Nicola Bonelli <nicola@pfq.io>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <core/global.h>
#include <core/sock.h>

#include <pfq/printk.h>
#include <pfq/kcompat.h>
#include <pfq/thread.h>
#include <pfq/atomic.h>
#include <pfq/pool.h>


/* vector of pointers to core_sock */

pfq_id_t
core_sock_get_free_id(struct core_sock * so)
{
        int n = 0;

        for(; n < (__force int)Q_CORE_MAX_ID; n++)
        {
                if (atomic_long_cmpxchg(&global->socket_ptr[n], (long)0, (long)so) == 0) {
			if(atomic_inc_return(&global->socket_count) == 1)
				pfq_sock_init_once();
			return (__force pfq_id_t)n;
                }
        }
        return (__force pfq_id_t)-ENOMEM;
}


int core_sock_get_socket_count(void)
{
        return atomic_read(&global->socket_count);
}


struct core_sock *
core_sock_get_by_id(pfq_id_t id)
{
        struct core_sock *so;
        if (unlikely((__force int)id >= Q_CORE_MAX_ID)) {
                pr_devel("[PFQ] core_get_sock_by_id: bad id=%d!\n", id);
                return NULL;
        }
	so = (struct core_sock *)atomic_long_read(&global->socket_ptr[(__force int)id]);
	// smp_read_barrier_depends();
	return so;
}


void core_sock_release_id(pfq_id_t id)
{
        if (unlikely((__force int)id >= Q_CORE_MAX_ID ||
		     (__force int)id < 0)) {
                pr_devel("[PFQ] core_release_sock_by_id: bad id=%d!\n", id);
                return;
        }

        atomic_long_set(global->socket_ptr + (__force int)id, 0);
        if (atomic_dec_return(&global->socket_count) == 0)
		pfq_sock_fini_once();
}


void core_sock_opt_init(struct core_sock_opt *that, size_t caplen, size_t maxlen)
{
        int n;

        /* disable tiemstamping by default */

        that->tstamp = false;

        /* initialize waitqueue */

        pfq_sock_init_waitqueue_head(&that->waitqueue);

	/* Rx queue setup */

	core_rx_info_init(&that->rx_info);

        that->caplen = caplen;
        that->rx_queue_len = 0;
        that->rx_slot_size = 0;

	/* Tx queues setup */

	core_tx_info_init(&that->tx_info);

        that->tx_queue_len  = 0;
        that->tx_slot_size  = PFQ_SHARED_QUEUE_SLOT_SIZE(maxlen);
	that->tx_num_async_queues = 0;

	/* Tx async queues setup */

	for(n = 0; n < Q_MAX_TX_QUEUES; ++n)
	{
		core_tx_info_init(&that->tx_info_async[n]);
	}
}


int core_sock_init(struct core_sock *so, pfq_id_t id)
{
	int i;

	/* setup stats */

	so->stats = alloc_percpu(core_sock_stats_t);
	if (!so->stats)
		return -ENOMEM;

	for_each_possible_cpu(i)
	{
		core_sock_stats_t * stat = per_cpu_ptr(so->stats, i);

		local_set(&stat->recv, 0);
		local_set(&stat->lost, 0);
		local_set(&stat->drop, 0);
		local_set(&stat->sent, 0);
		local_set(&stat->disc, 0);
		local_set(&stat->fail, 0);
		local_set(&stat->frwd, 0);
		local_set(&stat->kern, 0);
	}

	/* setup id */

	so->id = id;

        /* memory mapped queues are allocated later, when the socket is enabled */

	so->egress_type = Q_CORE_ENDPOINT_SOCKET;
	so->egress_index = 0;
	so->egress_queue = 0;

	/* default weight */

	so->weight = 1;

        so->shmem.addr = NULL;
        so->shmem.size = 0;
        so->shmem.kind = 0;
        so->shmem.hugepages = NULL;

        return 0;
}


int
core_sock_tx_bind(struct core_sock *so, int tid, int ifindex, int qindex)
{
	int queue = (int)so->opt.tx_num_async_queues;
	int err = 0;

	if (queue >= Q_MAX_TX_QUEUES) {
		printk(KERN_INFO "[PFQ|%d] could not bind Tx[%d] thread to queue %d (out of range)!\n", so->id, tid, queue);
		return -EPERM;
	}

	so->opt.tx_info_async[queue].def_ifindex = ifindex;
	so->opt.tx_info_async[queue].def_queue = qindex;
	so->opt.tx_num_async_queues++;

	smp_wmb();

	if ((err = pfq_bind_tx_thread(tid, so, queue)) < 0)
	{
		so->opt.tx_info_async[queue].def_ifindex = -1;
		so->opt.tx_info_async[queue].def_queue = -1;
		so->opt.tx_num_async_queues--;
		return err;
	}

	return 0;
}


int
core_sock_tx_unbind(struct core_sock *so)
{
	size_t n;

	so->opt.tx_info.def_ifindex = -1;
	so->opt.tx_info.def_queue = -1;

	/* unbind async Tx queue */

	if (pfq_unbind_tx_thread(so) < 0)
		return -EPERM;

	for(n = 0; n < Q_MAX_TX_QUEUES; ++n)
	{
		so->opt.tx_info_async[n].def_ifindex = -1;
		so->opt.tx_info_async[n].def_queue = -1;
	}

	return 0;
}
