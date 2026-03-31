/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Maxime Leroy, Free Mobile
 */

#ifndef _FIB_TBL8_H_
#define _FIB_TBL8_H_

/**
 * @file
 * Common tbl8 definitions shared by dir24_8 and trie backends.
 */

#include <stdint.h>

#define FIB_TBL8_GRP_NUM_ENT	256ULL

/** Nexthop size (log2 of byte width) */
enum fib_nh_sz {
	FIB_NH_SZ_1B = 0,
	FIB_NH_SZ_2B = 1,
	FIB_NH_SZ_4B = 2,
	FIB_NH_SZ_8B = 3,
};

static inline void
fib_tbl8_write(void *ptr, uint64_t val, uint8_t nh_sz, int n)
{
	int i;

	switch (nh_sz) {
	case FIB_NH_SZ_1B:
		for (i = 0; i < n; i++)
			((uint8_t *)ptr)[i] = (uint8_t)val;
		break;
	case FIB_NH_SZ_2B:
		for (i = 0; i < n; i++)
			((uint16_t *)ptr)[i] = (uint16_t)val;
		break;
	case FIB_NH_SZ_4B:
		for (i = 0; i < n; i++)
			((uint32_t *)ptr)[i] = (uint32_t)val;
		break;
	case FIB_NH_SZ_8B:
		for (i = 0; i < n; i++)
			((uint64_t *)ptr)[i] = (uint64_t)val;
		break;
	}
}

#endif /* _FIB_TBL8_H_ */
