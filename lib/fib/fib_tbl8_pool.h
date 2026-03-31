/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Maxime Leroy, Free Mobile
 */

#ifndef _FIB_TBL8_POOL_H_
#define _FIB_TBL8_POOL_H_

/**
 * @file
 * Internal tbl8 pool header.
 *
 * The pool is not thread-safe. When multiple FIBs share a pool,
 * all operations (route modifications, FIB creation/destruction)
 * must be serialized by the caller.
 */

#include <stdint.h>
#include <string.h>

#include <sys/queue.h>

#include <rte_common.h>

#include "fib_tbl8.h"
#include "rte_fib_tbl8_pool.h"

/** Consumer entry -- tracks each FIB's tbl8 pointer for resize updates. */
struct fib_tbl8_consumer {
	SLIST_ENTRY(fib_tbl8_consumer) next;
	uint64_t **tbl8_ptr;	/**< Points to the FIB's dp->tbl8 field */
	struct rte_rcu_qsbr_dq **dq;	/**< Points to the FIB's dp->dq field */
};

struct rte_fib_tbl8_pool {
	uint64_t	*tbl8;		/**< tbl8 group array */
	uint32_t	*free_list;	/**< Stack of free group indices */
	uint32_t	pool_pos;	/**< Number of groups taken from the free list */
	uint32_t	cur_tbl8s;	/**< Number of groups used by the dataplanes */
	uint32_t	rsvd_tbl8s;	/**< Number of groups reserved by the routes */
	uint32_t	num_tbl8s;	/**< Current capacity */
	uint32_t	max_tbl8s;	/**< Maximum capacity (0 = fixed) */
	uint8_t		nh_sz;		/**< Nexthop entry size (0-3) */
	int		socket_id;
	uint32_t	refcnt;		/**< Reference count */
	struct rte_rcu_qsbr *v;		/**< RCU QSBR variable (for resize) */
	SLIST_HEAD(, fib_tbl8_consumer) consumers; /**< Registered FIBs */
};

/**
 * Get a free tbl8 group index from the pool.
 * @return index on success, -ENOSPC if pool is full
 */
int32_t
fib_tbl8_pool_get(struct rte_fib_tbl8_pool *pool);

/**
 * Return a tbl8 group index to the pool.
 */
void
fib_tbl8_pool_put(struct rte_fib_tbl8_pool *pool, uint32_t idx);

/**
 * Clear a tbl8 group and return its index to the pool.
 */
void
fib_tbl8_pool_cleanup_and_free(struct rte_fib_tbl8_pool *pool, uint64_t idx);

/**
 * RCU defer queue callback for tbl8 group reclamation.
 * Shared by dir24_8 and trie backends.
 * Use as params.free_fn with params.p = pool.
 */
void
fib_tbl8_pool_rcu_free_cb(void *p, void *data, unsigned int n);

/**
 * Increment pool reference count.
 */
void
fib_tbl8_pool_ref(struct rte_fib_tbl8_pool *pool);

/**
 * Decrement pool reference count. Free the pool if it reaches 0.
 */
void
fib_tbl8_pool_unref(struct rte_fib_tbl8_pool *pool);

/**
 * Reclaim up to @p n tbl8 groups from the RCU defer queues of the FIBs
 * using the pool.
 *
 * @return number of groups returned to the pool.
 */
uint32_t
fib_tbl8_pool_reclaim(struct rte_fib_tbl8_pool *pool, uint32_t n);

/**
 * Make sure at least @p n tbl8 groups are free.
 *
 * Reclaims the RCU-deferred groups, then grows a resizable pool.
 * To be called before an update keeps pointers into the tbl8 array.
 *
 * @return 0 on success, -ENOSPC if @p n groups can not be made available.
 */
int
fib_tbl8_pool_reserve(struct rte_fib_tbl8_pool *pool, uint32_t n);

/**
 * Allocate a tbl8 group and initialise it with @p nh.
 *
 * Tries fib_tbl8_pool_get() first; on ENOSPC, tries RCU reclaim.
 * Never resizes the pool, see fib_tbl8_pool_reserve().
 *
 * @return group index on success, negative errno on failure.
 */
int
fib_tbl8_pool_alloc(struct rte_fib_tbl8_pool *pool, uint64_t nh);

/**
 * Register a FIB consumer: its tbl8 pointer is updated on resize and its
 * RCU defer queue is reclaimed when the pool runs out of groups.
 * The consumer is embedded in the dataplane struct of the FIB.
 */
void
fib_tbl8_pool_register(struct rte_fib_tbl8_pool *pool,
		       struct fib_tbl8_consumer *c);

/**
 * Unregister a FIB consumer.
 */
void
fib_tbl8_pool_unregister(struct rte_fib_tbl8_pool *pool,
			 struct fib_tbl8_consumer *c);

#endif /* _FIB_TBL8_POOL_H_ */
