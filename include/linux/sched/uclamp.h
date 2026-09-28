/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Utilization clamp entry points for code outside the scheduler.
 *
 * Copyright (c) 2026, Edrick Sinsuan <evcsinsuan@gmail.com>
 */
#ifndef _LINUX_SCHED_UCLAMP_H
#define _LINUX_SCHED_UCLAMP_H

#include <linux/sched.h>

struct dentry;

#ifdef CONFIG_UCLAMP_TASK
int sched_uclamp_set_dir(struct dentry *dir, enum uclamp_id clamp_id,
			 unsigned int util);
#else
static inline int sched_uclamp_set_dir(struct dentry *dir,
				       enum uclamp_id clamp_id,
				       unsigned int util)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_UCLAMP_TASK */

#endif /* _LINUX_SCHED_UCLAMP_H */
