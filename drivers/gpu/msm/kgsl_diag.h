/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KGSL_OP5_DIAG_H
#define _KGSL_OP5_DIAG_H
#include <linux/types.h>

struct kgsl_mem_entry;
struct kgsl_process_private;
struct kgsl_drawobj_cmd;
#ifdef CONFIG_KGSL_OP5_DIAG
void kgsl_diag_init(void);
void kgsl_diag_exit(void);
void kgsl_diag_mem(struct kgsl_mem_entry *entry, int import, int fd,
		u64 offset, u64 requested);
void kgsl_diag_destroy(struct kgsl_mem_entry *entry);
void kgsl_diag_submit(struct kgsl_drawobj_cmd *cmd, unsigned int numibs,
		int result);
void kgsl_diag_fault(struct kgsl_process_private *private, u64 addr,
		unsigned int flags, u32 contextidr, u64 ptbase);
#else
static inline void kgsl_diag_init(void) {}
static inline void kgsl_diag_exit(void) {}
static inline void kgsl_diag_mem(struct kgsl_mem_entry *entry, int import,
		int fd, u64 offset, u64 requested) {}
static inline void kgsl_diag_destroy(struct kgsl_mem_entry *entry) {}
static inline void kgsl_diag_submit(struct kgsl_drawobj_cmd *cmd,
		unsigned int numibs, int result) {}
static inline void kgsl_diag_fault(struct kgsl_process_private *private,
		u64 addr, unsigned int flags, u32 contextidr, u64 ptbase) {}
#endif
#endif
