/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Maxime Leroy, Free Mobile
 */

#ifndef _RTE_FIB_TBL8_POOL_H_
#define _RTE_FIB_TBL8_POOL_H_

/**
 * @file
 * Shared tbl8 pool for FIB backends.
 *
 * A tbl8 pool manages a shared array of tbl8 groups that can be used
 * across multiple FIB instances (e.g., one per VRF).
 *
 * Two modes of operation:
 *  - Internal pool: set num_tbl8 in the FIB config and leave tbl8_pool
 *    NULL. The pool is created and destroyed with the FIB.
 *  - External shared pool: create with rte_fib_tbl8_pool_create(), pass
 *    the handle via the tbl8_pool config field. Each FIB holds a
 *    reference; the creator releases its reference with
 *    rte_fib_tbl8_pool_free(). The pool is freed when the last
 *    reference is dropped.
 *
 * Resizing: if max_tbl8 is set in the pool configuration, the pool
 * can grow on demand up to that limit. This requires an RCU QSBR
 * variable (rte_fib_tbl8_pool_rcu_qsbr_add). When max_tbl8 is 0
 * (default), the pool has a fixed capacity and no RCU is needed
 * for pool operation.
 *
 * Thread safety: none. The pool is not thread-safe. All operations
 * on FIBs sharing the same pool (route updates, FIB creation and
 * destruction, pool create/free) must be serialized by the caller.
 *
 * Reservation: each route reserves in the pool the tbl8 groups it may
 * need. An add is refused with -ENOSPC when the reservations of all the
 * FIBs of the pool would exceed its capacity, so that an accepted route
 * never fails later for lack of tbl8 groups. Freeing a FIB returns its
 * groups and its reservation to the pool.
 * When the pool is shared and the FIB has an RCU defer queue, freeing
 * the FIB waits for the readers to report a quiescent state.
 */

#include <stdint.h>

#include <rte_rcu_qsbr.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rte_fib_tbl8_pool;

/** tbl8 pool configuration */
struct rte_fib_tbl8_pool_conf {
	uint32_t num_tbl8;	/**< Initial number of tbl8 groups */
	uint32_t max_tbl8;	/**< Max tbl8 groups (0 = fixed, no resize) */
	uint8_t  nh_sz;		/**< Nexthop size: 0=1B, 1=2B, 2=4B, 3=8B */
	int      socket_id;	/**< NUMA socket for memory allocation */
};

/** RCU QSBR configuration for tbl8 pool resize. */
struct rte_fib_tbl8_pool_rcu_config {
	struct rte_rcu_qsbr *v;	/**< RCU QSBR variable */
};

/**
 * Create a tbl8 pool.
 *
 * @param name
 *   Pool name (for memory allocation tracking)
 * @param conf
 *   Pool configuration
 * @return
 *   Pool handle on success, NULL on failure with rte_errno set
 */
__rte_experimental
struct rte_fib_tbl8_pool *
rte_fib_tbl8_pool_create(const char *name,
			 const struct rte_fib_tbl8_pool_conf *conf);

/**
 * Release the creator's reference on a tbl8 pool.
 *
 * The pool is freed when the last reference is dropped (i.e. after
 * all FIBs using this pool have been destroyed).
 *
 * @param pool
 *   Pool handle (NULL is allowed)
 */
__rte_experimental
void
rte_fib_tbl8_pool_free(struct rte_fib_tbl8_pool *pool);

/**
 * Associate an RCU QSBR variable with the pool.
 *
 * Required for resizable pools so that the old tbl8 array can be
 * reclaimed safely after a resize.
 *
 * @param pool
 *   Pool handle
 * @param cfg
 *   RCU configuration
 * @return
 *   0 on success, negative errno on failure
 */
__rte_experimental
int
rte_fib_tbl8_pool_rcu_qsbr_add(struct rte_fib_tbl8_pool *pool,
			       const struct rte_fib_tbl8_pool_rcu_config *cfg);

/**
 * Resize the tbl8 pool to a given capacity.
 *
 * The new capacity must be greater than the current capacity and
 * must not exceed max_tbl8 (if set). Requires RCU to be configured.
 *
 * @param pool
 *   Pool handle
 * @param new_num_tbl8
 *   Target number of tbl8 groups
 * @return
 *   0 on success
 *   -EINVAL if RCU is not configured (see rte_fib_tbl8_pool_rcu_qsbr_add)
 *   -ENOSPC if pool cannot grow (at max capacity or
 *     new_num_tbl8 <= current capacity)
 *   -ENOMEM if memory allocation failed
 */
__rte_experimental
int
rte_fib_tbl8_pool_resize(struct rte_fib_tbl8_pool *pool,
			 uint32_t new_num_tbl8);

/**
 * Retrieve tbl8 pool statistics.
 *
 * @param pool
 *   Pool handle
 * @param used
 *   Number of tbl8 groups taken from the pool, including those waiting
 *   in an RCU defer queue (can be NULL)
 * @param total
 *   Total number of tbl8 groups (current capacity, can be NULL)
 * @param max
 *   Maximum number of tbl8 groups (0 = fixed, can be NULL)
 * @return
 *   0 on success, -EINVAL if pool is NULL
 */
__rte_experimental
int
rte_fib_tbl8_pool_get_stats(struct rte_fib_tbl8_pool *pool,
			    uint32_t *used, uint32_t *total, uint32_t *max);

#ifdef __cplusplus
}
#endif

#endif /* _RTE_FIB_TBL8_POOL_H_ */
