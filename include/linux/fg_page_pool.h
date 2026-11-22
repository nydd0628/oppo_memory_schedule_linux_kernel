/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FG_PAGE_POOL_H
#define _LINUX_FG_PAGE_POOL_H

#include <linux/types.h>

#ifdef CONFIG_FG_PAGE_POOL
bool fg_pool_active(void);
#else
static inline bool fg_pool_active(void)
{
	return false;
}
#endif

#endif
