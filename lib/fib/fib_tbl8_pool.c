/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Maxime Leroy, Free Mobile
 */

#include <stdint.h>
#include <string.h>

#include <eal_export.h>
#include <rte_debug.h>
#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_rcu_qsbr.h>

#include "fib_tbl8_pool.h"

static inline uint64_t
pool_max_tbl8(uint8_t nh_sz)
{
	return (1ULL << ((8 << nh_sz) - 1)) - 1;
}

static void
pool_init_free_list(struct rte_fib_tbl8_pool *pool)
{
	uint32_t i;

	/* put entire range of indexes to the tbl8 pool */
	for (i = 0; i < pool->num_tbl8s; i++)
		pool->free_list[i] = i;

	pool->pool_pos = 0;
}

int32_t
fib_tbl8_pool_get(struct rte_fib_tbl8_pool *pool)
{
	if (pool->pool_pos == pool->num_tbl8s)
		/* no more free tbl8 */
		return -ENOSPC;

	/* next index */
	return pool->free_list[pool->pool_pos++];
}

void
fib_tbl8_pool_put(struct rte_fib_tbl8_pool *pool, uint32_t idx)
{
	RTE_ASSERT(pool->pool_pos > 0);
	pool->free_list[--pool->pool_pos] = idx;
}

void
fib_tbl8_pool_cleanup_and_free(struct rte_fib_tbl8_pool *pool, uint64_t idx)
{
	uint8_t *ptr = (uint8_t *)pool->tbl8 +
		((idx * FIB_TBL8_GRP_NUM_ENT) << pool->nh_sz);

	memset(ptr, 0, FIB_TBL8_GRP_NUM_ENT << pool->nh_sz);
	fib_tbl8_pool_put(pool, idx);
}

void
fib_tbl8_pool_rcu_free_cb(void *p, void *data,
			  unsigned int n __rte_unused)
{
	struct rte_fib_tbl8_pool *pool = p;
	uint64_t tbl8_idx = *(uint64_t *)data;

	fib_tbl8_pool_cleanup_and_free(pool, tbl8_idx);
}

uint32_t
fib_tbl8_pool_reclaim(struct rte_fib_tbl8_pool *pool, uint32_t n)
{
	struct fib_tbl8_consumer *c;
	unsigned int freed;
	uint32_t total = 0;

	SLIST_FOREACH(c, &pool->consumers, next) {
		if (total >= n)
			break;
		if (*c->dq == NULL)
			continue;
		freed = 0;
		rte_rcu_qsbr_dq_reclaim(*c->dq, n - total, &freed, NULL, NULL);
		total += freed;
	}

	return total;
}

void
fib_tbl8_pool_register(struct rte_fib_tbl8_pool *pool,
		       struct fib_tbl8_consumer *c)
{
	SLIST_INSERT_HEAD(&pool->consumers, c, next);
}

void
fib_tbl8_pool_unregister(struct rte_fib_tbl8_pool *pool,
			 struct fib_tbl8_consumer *c)
{
	SLIST_REMOVE(&pool->consumers, c, fib_tbl8_consumer, next);
}

void
fib_tbl8_pool_ref(struct rte_fib_tbl8_pool *pool)
{
	pool->refcnt++;
}

static void
pool_free(struct rte_fib_tbl8_pool *pool)
{
	RTE_ASSERT(SLIST_EMPTY(&pool->consumers));
	rte_free(pool->free_list);
	rte_free(pool->tbl8);
	rte_free(pool);
}

void
fib_tbl8_pool_unref(struct rte_fib_tbl8_pool *pool)
{
	if (--pool->refcnt == 0)
		pool_free(pool);
}

RTE_EXPORT_EXPERIMENTAL_SYMBOL(rte_fib_tbl8_pool_create, 27.03)
struct rte_fib_tbl8_pool *
rte_fib_tbl8_pool_create(const char *name,
			 const struct rte_fib_tbl8_pool_conf *conf)
{
	struct rte_fib_tbl8_pool *pool;
	char mem_name[64];

	if (name == NULL || conf == NULL || conf->num_tbl8 == 0 ||
	    conf->nh_sz > 3 ||
	    conf->num_tbl8 > pool_max_tbl8(conf->nh_sz)) {
		rte_errno = EINVAL;
		return NULL;
	}

	snprintf(mem_name, sizeof(mem_name), "TBL8_POOL_%s", name);
	pool = rte_zmalloc_socket(mem_name, sizeof(*pool),
		RTE_CACHE_LINE_SIZE, conf->socket_id);
	if (pool == NULL) {
		rte_errno = ENOMEM;
		return NULL;
	}

	pool->nh_sz = conf->nh_sz;
	pool->num_tbl8s = conf->num_tbl8;
	pool->socket_id = conf->socket_id;
	pool->refcnt = 1;

	snprintf(mem_name, sizeof(mem_name), "TBL8_%s", name);
	pool->tbl8 = rte_zmalloc_socket(mem_name,
		FIB_TBL8_GRP_NUM_ENT * (1ULL << pool->nh_sz) *
		(pool->num_tbl8s + 1),
		RTE_CACHE_LINE_SIZE, conf->socket_id);
	if (pool->tbl8 == NULL) {
		rte_errno = ENOMEM;
		rte_free(pool);
		return NULL;
	}

	snprintf(mem_name, sizeof(mem_name), "TBL8_FL_%s", name);
	pool->free_list = rte_zmalloc_socket(mem_name,
		sizeof(uint32_t) * pool->num_tbl8s,
		RTE_CACHE_LINE_SIZE, conf->socket_id);
	if (pool->free_list == NULL) {
		rte_errno = ENOMEM;
		rte_free(pool->tbl8);
		rte_free(pool);
		return NULL;
	}

	pool_init_free_list(pool);
	SLIST_INIT(&pool->consumers);

	return pool;
}

RTE_EXPORT_EXPERIMENTAL_SYMBOL(rte_fib_tbl8_pool_free, 27.03)
void
rte_fib_tbl8_pool_free(struct rte_fib_tbl8_pool *pool)
{
	if (pool == NULL)
		return;

	fib_tbl8_pool_unref(pool);
}
