/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018 Vladimir Medvedkin <medvedkinv@gmail.com>
 * Copyright(c) 2019 Intel Corporation
 */

#include <stdint.h>
#include <stdio.h>

#include <rte_debug.h>
#include <rte_malloc.h>
#include <rte_cpuflags.h>
#include <rte_errno.h>

#include <rte_rib6.h>
#include <rte_fib6.h>
#include <rib6_internal.h>
#include "fib_log.h"
#include "trie.h"

static_assert((int)FIB_NH_SZ_2B == (int)RTE_FIB6_TRIE_2B, "nh_sz 2B mismatch");
static_assert((int)FIB_NH_SZ_4B == (int)RTE_FIB6_TRIE_4B, "nh_sz 4B mismatch");
static_assert((int)FIB_NH_SZ_8B == (int)RTE_FIB6_TRIE_8B, "nh_sz 8B mismatch");

#ifdef CC_AVX512_SUPPORT

#include "trie_avx512.h"

#endif /* CC_AVX512_SUPPORT */

#define TRIE_NAMESIZE		64

enum edge {
	LEDGE,
	REDGE
};

static inline rte_fib6_lookup_fn_t
get_scalar_fn(enum rte_fib_trie_nh_sz nh_sz)
{
	switch (nh_sz) {
	case RTE_FIB6_TRIE_2B:
		return rte_trie_lookup_bulk_2b;
	case RTE_FIB6_TRIE_4B:
		return rte_trie_lookup_bulk_4b;
	case RTE_FIB6_TRIE_8B:
		return rte_trie_lookup_bulk_8b;
	default:
		return NULL;
	}
}

static inline rte_fib6_lookup_fn_t
get_vector_fn(enum rte_fib_trie_nh_sz nh_sz)
{
#ifdef CC_AVX512_SUPPORT
	if (rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512F) <= 0 ||
			rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512DQ) <= 0 ||
			rte_cpu_get_flag_enabled(RTE_CPUFLAG_AVX512BW) <= 0 ||
			rte_vect_get_max_simd_bitwidth() < RTE_VECT_SIMD_512)
		return NULL;
	switch (nh_sz) {
	case RTE_FIB6_TRIE_2B:
		return rte_trie_vec_lookup_bulk_2b;
	case RTE_FIB6_TRIE_4B:
		return rte_trie_vec_lookup_bulk_4b;
	case RTE_FIB6_TRIE_8B:
		return rte_trie_vec_lookup_bulk_8b;
	default:
		return NULL;
	}
#else
	RTE_SET_USED(nh_sz);
#endif
	return NULL;
}

rte_fib6_lookup_fn_t
trie_get_lookup_fn(void *p, enum rte_fib6_lookup_type type)
{
	enum rte_fib_trie_nh_sz nh_sz;
	rte_fib6_lookup_fn_t ret_fn;
	struct rte_trie_tbl *dp = p;

	if (dp == NULL)
		return NULL;

	nh_sz = dp->nh_sz;

	switch (type) {
	case RTE_FIB6_LOOKUP_TRIE_SCALAR:
		return get_scalar_fn(nh_sz);
	case RTE_FIB6_LOOKUP_TRIE_VECTOR_AVX512:
		return get_vector_fn(nh_sz);
	case RTE_FIB6_LOOKUP_DEFAULT:
		ret_fn = get_vector_fn(nh_sz);
		return (ret_fn != NULL) ? ret_fn : get_scalar_fn(nh_sz);
	default:
		return NULL;
	}
	return NULL;
}

static int
tbl8_alloc(struct rte_trie_tbl *dp, uint64_t nh)
{
	return fib_tbl8_pool_alloc(dp->pool, nh);
}

static void
tbl8_recycle(struct rte_trie_tbl *dp, void *par, uint64_t tbl8_idx)
{
	uint32_t i;
	uint64_t nh;
	uint16_t *ptr16;
	uint32_t *ptr32;
	uint64_t *ptr64;

	switch (dp->nh_sz) {
	case RTE_FIB6_TRIE_2B:
		ptr16 = &((uint16_t *)dp->tbl8)[tbl8_idx *
				FIB_TBL8_GRP_NUM_ENT];
		nh = *ptr16;
		if (nh & TRIE_EXT_ENT)
			return;
		for (i = 1; i < FIB_TBL8_GRP_NUM_ENT; i++) {
			if (nh != ptr16[i])
				return;
		}
		fib_tbl8_write(par, nh, dp->nh_sz, 1);
		break;
	case RTE_FIB6_TRIE_4B:
		ptr32 = &((uint32_t *)dp->tbl8)[tbl8_idx *
				FIB_TBL8_GRP_NUM_ENT];
		nh = *ptr32;
		if (nh & TRIE_EXT_ENT)
			return;
		for (i = 1; i < FIB_TBL8_GRP_NUM_ENT; i++) {
			if (nh != ptr32[i])
				return;
		}
		fib_tbl8_write(par, nh, dp->nh_sz, 1);
		break;
	case RTE_FIB6_TRIE_8B:
		ptr64 = &((uint64_t *)dp->tbl8)[tbl8_idx *
				FIB_TBL8_GRP_NUM_ENT];
		nh = *ptr64;
		if (nh & TRIE_EXT_ENT)
			return;
		for (i = 1; i < FIB_TBL8_GRP_NUM_ENT; i++) {
			if (nh != ptr64[i])
				return;
		}
		fib_tbl8_write(par, nh, dp->nh_sz, 1);
		break;
	}

	dp->pool->cur_tbl8s--;
	if (dp->v == NULL) {
		fib_tbl8_pool_cleanup_and_free(dp->pool, tbl8_idx);
	} else if (dp->rcu_mode == RTE_FIB6_QSBR_MODE_SYNC) {
		rte_rcu_qsbr_synchronize(dp->v, RTE_QSBR_THRID_INVALID);
		fib_tbl8_pool_cleanup_and_free(dp->pool, tbl8_idx);
	} else { /* RTE_FIB6_QSBR_MODE_DQ */
		if (rte_rcu_qsbr_dq_enqueue(dp->dq, &tbl8_idx))
			FIB_LOG(ERR, "Failed to push QSBR FIFO");
	}
}

#define BYTE_SIZE	8
static inline uint64_t
get_idx(const struct rte_ipv6_addr *ip, uint64_t prev_idx, int bytes, int first_byte)
{
	int i;
	uint32_t idx = 0;
	uint8_t bitshift;

	for (i = first_byte; i < (first_byte + bytes); i++) {
		bitshift = (int8_t)(((first_byte + bytes - 1) - i)*BYTE_SIZE);
		idx |= ip->a[i] <<  bitshift;
	}
	return (prev_idx * FIB_TBL8_GRP_NUM_ENT) + idx;
}

static inline uint64_t
get_val_by_p(void *p, uint8_t nh_sz)
{
	uint64_t val = 0;

	switch (nh_sz) {
	case RTE_FIB6_TRIE_2B:
		val = *(uint16_t *)p;
		break;
	case RTE_FIB6_TRIE_4B:
		val = *(uint32_t *)p;
		break;
	case RTE_FIB6_TRIE_8B:
		val = *(uint64_t *)p;
		break;
	}
	return val;
}

/*
 * recursively recycle tbl8's
 */
static void
recycle_root_path(struct rte_trie_tbl *dp, const uint8_t *ip_part,
	uint8_t common_tbl8, void *prev)
{
	void *p;
	uint64_t val;

	val = get_val_by_p(prev, dp->nh_sz);
	if (unlikely((val & TRIE_EXT_ENT) != TRIE_EXT_ENT))
		return;

	if (common_tbl8 != 0) {
		p = get_tbl_p_by_idx(dp->tbl8, (val >> 1) *
			FIB_TBL8_GRP_NUM_ENT + *ip_part, dp->nh_sz);
		recycle_root_path(dp, ip_part + 1, common_tbl8 - 1, p);
	}
	tbl8_recycle(dp, prev, val >> 1);
}

static inline int
build_common_root(struct rte_trie_tbl *dp, const struct rte_ipv6_addr *ip,
	int common_bytes, void **tbl)
{
	void *tbl_ptr = NULL;
	uint64_t *cur_tbl;
	uint64_t val;
	uint64_t idx, prev_idx = 0;
	int i, j, tbl8_idx;

	cur_tbl = dp->tbl24;
	for (i = 3, j = 0; i <= common_bytes; i++) {
		idx = get_idx(ip, prev_idx, i - j, j);
		val = get_tbl_val_by_idx(cur_tbl, idx, dp->nh_sz);
		tbl_ptr = get_tbl_p_by_idx(cur_tbl, idx, dp->nh_sz);
		if ((val & TRIE_EXT_ENT) != TRIE_EXT_ENT) {
			tbl8_idx = tbl8_alloc(dp, val);
			if (unlikely(tbl8_idx < 0))
				return tbl8_idx;
			fib_tbl8_write(tbl_ptr, ((uint64_t)tbl8_idx << 1) |
				TRIE_EXT_ENT, dp->nh_sz, 1);
			prev_idx = (uint64_t)tbl8_idx;
		} else
			prev_idx = val >> 1;

		j = i;
		cur_tbl = dp->tbl8;
	}
	*tbl = get_tbl_p_by_idx(cur_tbl, prev_idx * FIB_TBL8_GRP_NUM_ENT,
		dp->nh_sz);
	return 0;
}

static int
write_edge(struct rte_trie_tbl *dp, const uint8_t *ip_part, uint64_t next_hop,
	int len, enum edge edge, void *ent)
{
	uint64_t val = next_hop << 1;
	int tbl8_idx;
	int ret = 0;
	void *p;

	if (len != 0) {
		val = get_val_by_p(ent, dp->nh_sz);
		if ((val & TRIE_EXT_ENT) == TRIE_EXT_ENT)
			tbl8_idx = val >> 1;
		else {
			tbl8_idx = tbl8_alloc(dp, val);
			if (tbl8_idx < 0)
				return tbl8_idx;
			val = ((uint64_t)tbl8_idx << 1) | TRIE_EXT_ENT;
		}
		p = get_tbl_p_by_idx(dp->tbl8, (tbl8_idx *
			FIB_TBL8_GRP_NUM_ENT) + *ip_part, dp->nh_sz);
		ret = write_edge(dp, ip_part + 1, next_hop, len - 1, edge, p);
		if (ret < 0)
			return ret;
		if (edge == LEDGE) {
			fib_tbl8_write(RTE_PTR_ADD(p, (uintptr_t)(1) << dp->nh_sz),
				next_hop << 1, dp->nh_sz, UINT8_MAX - *ip_part);
		} else {
			fib_tbl8_write(get_tbl_p_by_idx(dp->tbl8, tbl8_idx *
				FIB_TBL8_GRP_NUM_ENT, dp->nh_sz),
				next_hop << 1, dp->nh_sz, *ip_part);
		}
		tbl8_recycle(dp, &val, tbl8_idx);
	}

	fib_tbl8_write(ent, val, dp->nh_sz, 1);
	return ret;
}

#define IPV6_MAX_IDX	(RTE_IPV6_ADDR_SIZE - 1)
#define TBL24_BYTES	3
#define TBL8_LEN	(RTE_IPV6_ADDR_SIZE - TBL24_BYTES)

static int
install_to_dp(struct rte_trie_tbl *dp, const struct rte_ipv6_addr *ledge,
	const struct rte_ipv6_addr *r, uint64_t next_hop)
{
	void *common_root_tbl;
	void *ent;
	int ret;
	int i;
	int common_bytes;
	int llen, rlen;
	struct rte_ipv6_addr redge;

	/* decrement redge by 1*/
	redge = *r;
	for (i = 15; i >= 0; i--) {
		redge.a[i]--;
		if (redge.a[i] != 0xff)
			break;
	}

	for (common_bytes = 0; common_bytes < 15; common_bytes++) {
		if (ledge->a[common_bytes] != redge.a[common_bytes])
			break;
	}

	/*
	 * A range covering a whole tbl8 is a single entry one level up.
	 * Write that entry rather than build a tbl8 only to recycle it.
	 */
	if (common_bytes >= TBL24_BYTES) {
		for (i = common_bytes; i <= IPV6_MAX_IDX; i++) {
			if (ledge->a[i] != 0 || redge.a[i] != UINT8_MAX)
				break;
		}
		if (i > IPV6_MAX_IDX)
			common_bytes--;
	}

	ret = build_common_root(dp, ledge, common_bytes, &common_root_tbl);
	if (unlikely(ret != 0))
		return ret;
	/*first uncommon tbl8 byte idx*/
	uint8_t first_tbl8_byte = RTE_MAX(common_bytes, TBL24_BYTES);

	for (i = IPV6_MAX_IDX; i >= first_tbl8_byte + !(common_bytes < 3); i--) {
		if (ledge->a[i] != 0)
			break;
	}

	llen = i - first_tbl8_byte + (common_bytes < 3);

	for (i = IPV6_MAX_IDX; i >= first_tbl8_byte + !(common_bytes < 3); i--) {
		if (redge.a[i] != UINT8_MAX)
			break;
	}
	rlen = i - first_tbl8_byte + (common_bytes < 3);

	/*first noncommon byte*/
	uint8_t first_byte_idx = (common_bytes < 3) ? 0 : common_bytes;
	uint8_t first_idx_len = (common_bytes < 3) ? 3 : 1;

	uint64_t left_idx = get_idx(ledge, 0, first_idx_len, first_byte_idx);
	uint64_t right_idx = get_idx(&redge, 0, first_idx_len, first_byte_idx);

	ent = get_tbl_p_by_idx(common_root_tbl, left_idx, dp->nh_sz);
	ret = write_edge(dp, &ledge->a[first_tbl8_byte + !(common_bytes < 3)],
		next_hop, llen, LEDGE, ent);
	if (ret < 0)
		return ret;

	if (right_idx > left_idx + 1) {
		ent = get_tbl_p_by_idx(common_root_tbl, left_idx + 1,
			dp->nh_sz);
		fib_tbl8_write(ent, next_hop << 1, dp->nh_sz,
			right_idx - (left_idx + 1));
	}
	ent = get_tbl_p_by_idx(common_root_tbl, right_idx, dp->nh_sz);
	ret = write_edge(dp, &redge.a[first_tbl8_byte + !((common_bytes < 3))],
		next_hop, rlen, REDGE, ent);
	if (ret < 0)
		return ret;

	uint8_t	common_tbl8 = (common_bytes < TBL24_BYTES) ?
			0 : common_bytes - (TBL24_BYTES - 1);
	ent = get_tbl24_p(dp, ledge, dp->nh_sz);
	recycle_root_path(dp, ledge->a + TBL24_BYTES, common_tbl8, ent);
	return 0;
}

static void
get_nxt_net(struct rte_ipv6_addr *ip, uint8_t depth)
{
	int i;
	uint8_t part_depth;
	uint8_t prev_byte;

	for (i = 0, part_depth = depth; part_depth > 8; part_depth -= 8, i++)
		;

	prev_byte = ip->a[i];
	ip->a[i] += 1 << (8 - part_depth);
	if (ip->a[i] < prev_byte) {
		while (i > 0) {
			ip->a[--i] += 1;
			if (ip->a[i] != 0)
				break;
		}
	}
}

static int
modify_dp(struct rte_trie_tbl *dp, struct rte_rib6 *rib,
	const struct rte_ipv6_addr *ip,
	uint8_t depth, uint64_t next_hop)
{
	struct rte_rib6_node *tmp = NULL;
	struct rte_ipv6_addr ledge, redge;
	int ret;
	uint8_t tmp_depth;

	if (next_hop > get_max_nh(dp->nh_sz))
		return -EINVAL;

	/*
	 * Check if we can safely allocate new tbl8s further in process of modifying the dataplane.
	 */
	if (dp->pool->rsvd_tbl8s > dp->pool->cur_tbl8s) {
		ret = fib_tbl8_pool_reserve(dp->pool,
			dp->pool->rsvd_tbl8s - dp->pool->cur_tbl8s);
		if (ret != 0)
			return ret;
	}

	ledge = *ip;
	do {
		tmp = rte_rib6_get_nxt(rib, ip, depth, tmp,
			RTE_RIB6_GET_NXT_COVER);
		if (tmp != NULL) {
			rte_rib6_get_depth(tmp, &tmp_depth);
			if (tmp_depth == depth)
				continue;
			rte_rib6_get_ip(tmp, &redge);
			if (rte_ipv6_addr_eq(&ledge, &redge)) {
				get_nxt_net(&ledge, tmp_depth);
				/*
				 * we got to the end of address space
				 * and wrapped around
				 */
				if (rte_ipv6_addr_is_unspec(&ledge))
					break;
				continue;
			}
			ret = install_to_dp(dp, &ledge, &redge, next_hop);
			if (ret != 0)
				return ret;
			get_nxt_net(&redge, tmp_depth);
			ledge = redge;
			/*
			 * we got to the end of address space
			 * and wrapped around
			 */
			if (rte_ipv6_addr_is_unspec(&ledge))
				break;
		} else {
			redge = *ip;
			get_nxt_net(&redge, depth);
			if (rte_ipv6_addr_eq(&ledge, &redge) &&
					!rte_ipv6_addr_is_unspec(&ledge))
				break;

			ret = install_to_dp(dp, &ledge, &redge, next_hop);
			if (ret != 0)
				return ret;
		}
	} while (tmp);

	return 0;
}

/*
 * Count number of TBL8s that can be freed after deleting a prefix or allocated
 * after adding a prefix.
 */
static uint8_t
count_empty_levels(const struct rte_rib6_node *node)
{
	struct rte_rib6_node *parent;
	uint8_t depth, parent_depth = 24;

	/* more specifics present */
	if (rte_rib6_node_has_children(node))
		return 0;

	rte_rib6_get_depth(node, &depth);
	depth = RTE_MAX(depth, 24);

	/* we know parent depth lt a target node depth
	 * also, there exists tbl8 path up to RTE_ALIGN_CEIL(parent depth, 8)
	 */
	parent = rte_rib6_get_parent(node);
	if (parent != NULL) {
		rte_rib6_get_depth(parent, &parent_depth);
		parent_depth = RTE_MAX(parent_depth, 24);
	}

	return (RTE_ALIGN_CEIL(depth, 8) - RTE_ALIGN_CEIL(parent_depth, 8)) >> 3;
}

int
trie_modify(struct rte_fib6 *fib, const struct rte_ipv6_addr *ip,
	uint8_t depth, uint64_t next_hop, int op)
{
	struct rte_trie_tbl *dp;
	struct rte_rib6 *rib;
	struct rte_rib6_node *node;
	struct rte_rib6_node *parent;
	struct rte_ipv6_addr ip_masked;
	int ret = 0;
	uint64_t par_nh, node_nh;
	uint8_t new_levels;

	if ((fib == NULL) || (ip == NULL) || (depth > RTE_IPV6_MAX_DEPTH))
		return -EINVAL;

	dp = rte_fib6_get_dp(fib);
	RTE_ASSERT(dp);
	rib = rte_fib6_get_rib(fib);
	RTE_ASSERT(rib);

	ip_masked = *ip;
	rte_ipv6_addr_mask(&ip_masked, depth);

	node = rte_rib6_lookup_exact(rib, &ip_masked, depth);
	switch (op) {
	case RTE_FIB6_ADD:
		if (node != NULL) {
			rte_rib6_get_nh(node, &node_nh);
			if (node_nh == next_hop)
				return 0;
			ret = modify_dp(dp, rib, &ip_masked, depth, next_hop);
			if (ret == 0)
				rte_rib6_set_nh(node, next_hop);

			return ret;
		}

		node = rte_rib6_insert(rib, &ip_masked, depth);
		if (node == NULL)
			return -rte_errno;

		new_levels = count_empty_levels(node);
		if (dp->pool->rsvd_tbl8s + new_levels >
				(dp->pool->max_tbl8s ? dp->pool->max_tbl8s :
					dp->pool->num_tbl8s)) {
			rte_rib6_remove(rib, &ip_masked, depth);
			return -ENOSPC;
		}
		dp->rsvd_tbl8s += new_levels;
		dp->pool->rsvd_tbl8s += new_levels;

		rte_rib6_set_nh(node, next_hop);
		parent = rte_rib6_lookup_parent(node);
		if (parent != NULL) {
			rte_rib6_get_nh(parent, &par_nh);
			if (par_nh == next_hop)
				goto successfully_added;
		}
		ret = modify_dp(dp, rib, &ip_masked, depth, next_hop);
		if (ret != 0) {
			dp->rsvd_tbl8s -= new_levels;
			dp->pool->rsvd_tbl8s -= new_levels;
			rte_rib6_remove(rib, &ip_masked, depth);
			return ret;
		}
successfully_added:
		return 0;
	case RTE_FIB6_DEL:
		if (node == NULL)
			return -ENOENT;

		parent = rte_rib6_lookup_parent(node);
		if (parent != NULL) {
			rte_rib6_get_nh(parent, &par_nh);
			rte_rib6_get_nh(node, &node_nh);
			if (par_nh != node_nh)
				ret = modify_dp(dp, rib, &ip_masked, depth,
					par_nh);
		} else
			ret = modify_dp(dp, rib, &ip_masked, depth, dp->def_nh);

		if (ret != 0)
			return ret;

		new_levels = count_empty_levels(node);
		dp->rsvd_tbl8s -= new_levels;
		dp->pool->rsvd_tbl8s -= new_levels;
		rte_rib6_remove(rib, &ip_masked, depth);

		return 0;
	default:
		break;
	}
	return -EINVAL;
}

void *
trie_create(const char *name, int socket_id,
	struct rte_fib6_conf *conf)
{
	char mem_name[TRIE_NAMESIZE];
	struct rte_trie_tbl *dp = NULL;
	struct rte_fib_tbl8_pool *pool;
	uint64_t	def_nh;
	enum rte_fib_trie_nh_sz	nh_sz;

	if ((name == NULL) || (conf == NULL) ||
			(conf->trie.nh_sz < RTE_FIB6_TRIE_2B) ||
			(conf->trie.nh_sz > RTE_FIB6_TRIE_8B) ||
			(conf->default_nh >
			get_max_nh(conf->trie.nh_sz))) {
		rte_errno = EINVAL;
		return NULL;
	}

	def_nh = conf->default_nh;
	nh_sz = conf->trie.nh_sz;

	if (conf->trie.tbl8_pool != NULL) {
		/* External shared pool: validate nh_sz matches. */
		pool = conf->trie.tbl8_pool;
		if (pool->nh_sz != nh_sz) {
			rte_errno = EINVAL;
			return NULL;
		}
		fib_tbl8_pool_ref(pool);
	} else {
		/* Internal pool: create from config. */
		struct rte_fib_tbl8_pool_conf pool_conf = {
			.num_tbl8 = conf->trie.num_tbl8,
			.nh_sz = nh_sz,
			.socket_id = socket_id,
		};

		if (conf->trie.num_tbl8 == 0 ||
		    conf->trie.num_tbl8 >
		    get_max_nh(nh_sz)) {
			rte_errno = EINVAL;
			return NULL;
		}

		pool = rte_fib_tbl8_pool_create(name, &pool_conf);
		if (pool == NULL)
			return NULL;
	}

	snprintf(mem_name, sizeof(mem_name), "DP_%s", name);
	dp = rte_zmalloc_socket(name, sizeof(struct rte_trie_tbl) +
		TRIE_TBL24_NUM_ENT * (1 << nh_sz) + sizeof(uint32_t),
		RTE_CACHE_LINE_SIZE, socket_id);
	if (dp == NULL) {
		fib_tbl8_pool_unref(pool);
		rte_errno = ENOMEM;
		return NULL;
	}

	dp->def_nh = def_nh;
	dp->nh_sz = nh_sz;
	dp->pool = pool;
	dp->tbl8 = pool->tbl8;

	dp->consumer.tbl8_ptr = &dp->tbl8;
	dp->consumer.dq = &dp->dq;
	fib_tbl8_pool_register(pool, &dp->consumer);

	fib_tbl8_write(&dp->tbl24, (def_nh << 1), nh_sz, 1 << 24);

	return dp;
}

static void
tbl8_free_subtree(struct rte_trie_tbl *dp, uint64_t tbl8_idx)
{
	uint64_t val;
	uint32_t i;

	for (i = 0; i < FIB_TBL8_GRP_NUM_ENT; i++) {
		val = get_tbl_val_by_idx(dp->tbl8,
			tbl8_idx * FIB_TBL8_GRP_NUM_ENT + i, dp->nh_sz);
		if ((val & TRIE_EXT_ENT) == TRIE_EXT_ENT)
			tbl8_free_subtree(dp, val >> 1);
	}
	dp->pool->cur_tbl8s--;
	fib_tbl8_pool_cleanup_and_free(dp->pool, tbl8_idx);
}

void
trie_free(void *p)
{
	struct rte_trie_tbl *dp = (struct rte_trie_tbl *)p;
	bool shared = dp->pool->refcnt > 1;
	uint64_t val;
	uint32_t i;

	fib_tbl8_pool_unregister(dp->pool, &dp->consumer);
	if (shared && dp->dq != NULL)
		rte_rcu_qsbr_synchronize(dp->v, RTE_QSBR_THRID_INVALID);
	rte_rcu_qsbr_dq_delete(dp->dq);
	if (shared) {
		for (i = 0; i < TRIE_TBL24_NUM_ENT; i++) {
			val = get_tbl_val_by_idx(dp->tbl24, i, dp->nh_sz);
			if ((val & TRIE_EXT_ENT) == TRIE_EXT_ENT)
				tbl8_free_subtree(dp, val >> 1);
		}
		dp->pool->rsvd_tbl8s -= dp->rsvd_tbl8s;
	}
	fib_tbl8_pool_unref(dp->pool);
	rte_free(dp);
}

int
trie_rcu_qsbr_add(struct rte_trie_tbl *dp, struct rte_fib6_rcu_config *cfg,
	const char *name)
{
	struct rte_rcu_qsbr_dq_parameters params = {0};
	char rcu_dq_name[RTE_RCU_QSBR_DQ_NAMESIZE];
	bool pool_rcu_added = false;

	if (dp == NULL || cfg == NULL)
		return -EINVAL;

	if (dp->v != NULL)
		return -EEXIST;

	/* Propagate RCU to the pool for resize if it is resizable */
	if (dp->pool->max_tbl8s > 0) {
		if (dp->pool->v != NULL && dp->pool->v != cfg->v)
			return -EINVAL;
		if (dp->pool->v == NULL) {
			struct rte_fib_tbl8_pool_rcu_config pool_rcu = {
				.v = cfg->v,
			};
			int rc = rte_fib_tbl8_pool_rcu_qsbr_add(
				dp->pool, &pool_rcu);
			if (rc != 0)
				return rc;
			pool_rcu_added = true;
		}
	}

	switch (cfg->mode) {
	case RTE_FIB6_QSBR_MODE_DQ:
		/* Init QSBR defer queue. */
		snprintf(rcu_dq_name, sizeof(rcu_dq_name),
			"FIB_RCU_%s", name);
		params.name = rcu_dq_name;
		params.size = cfg->dq_size;
		if (params.size == 0)
			params.size = RTE_FIB6_RCU_DQ_RECLAIM_SZ;
		params.trigger_reclaim_limit = cfg->reclaim_thd;
		params.max_reclaim_size = cfg->reclaim_max;
		if (params.max_reclaim_size == 0)
			params.max_reclaim_size = RTE_FIB6_RCU_DQ_RECLAIM_MAX;
		params.esize = sizeof(uint64_t);
		params.free_fn = fib_tbl8_pool_rcu_free_cb;
		params.p = dp->pool;
		params.v = cfg->v;
		dp->dq = rte_rcu_qsbr_dq_create(&params);
		if (dp->dq == NULL) {
			FIB_LOG(ERR, "FIB6 defer queue creation failed");
			if (pool_rcu_added)
				dp->pool->v = NULL;
			return -ENOMEM;
		}
		break;
	case RTE_FIB6_QSBR_MODE_SYNC:
		/* No other things to do. */
		break;
	default:
		if (pool_rcu_added)
			dp->pool->v = NULL;
		return -EINVAL;
	}
	dp->rcu_mode = cfg->mode;
	dp->v = cfg->v;

	return 0;
}
