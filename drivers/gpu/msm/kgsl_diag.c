// SPDX-License-Identifier: GPL-2.0-only
/* OP5 V12 metadata-only observer. Never dereference GPU command contents. */
#include <linux/atomic.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include "kgsl_device.h"
#include "kgsl_drawobj.h"
#include "kgsl_diag.h"

enum { ALLOC, IMPORT, DESTROY, SUBMIT, FAULT, EVENTS };
static const unsigned int caps[EVENTS] = { 128, 128, 128, 32, 1 };
static atomic64_t seen[EVENTS];
static atomic_t emitted[EVENTS];
static atomic64_t cookies = ATOMIC64_INIT(0);

static bool selected(const struct kgsl_process_private *p)
{
	return p && !strcmp(p->comm, "surfaceflinger");
}

/* Telemetry may wrap; saturating admission never reopens a print budget. */
static bool admit(unsigned int event)
{
	atomic64_inc(&seen[event]);
	return atomic_add_unless(&emitted[event], 1, caps[event]);
}

static void summary(const char *phase)
{
	u64 n[EVENTS];
	unsigned int i;

	for (i = 0; i < EVENTS; i++)
		n[i] = atomic64_read(&seen[i]);
	pr_info("OP5KGSL summary=%s seen=%llu,%llu,%llu,%llu,%llu dropped=%llu,%llu,%llu,%llu,%llu caps=128,128,128,32,1 order=alloc,import,destroy,submit,fault\n",
		phase, n[0], n[1], n[2], n[3], n[4],
		n[0] > caps[0] ? n[0] - caps[0] : 0,
		n[1] > caps[1] ? n[1] - caps[1] : 0,
		n[2] > caps[2] ? n[2] - caps[2] : 0,
		n[3] > caps[3] ? n[3] - caps[3] : 0,
		n[4] > caps[4] ? n[4] - caps[4] : 0);
}

static void summary_work(struct work_struct *work)
{
	summary("60s-after-kgsl-init");
}
static DECLARE_DELAYED_WORK(diag_summary, summary_work);

void kgsl_diag_init(void)
{
	queue_delayed_work(system_unbound_wq, &diag_summary, 60 * HZ);
}

void kgsl_diag_exit(void)
{
	cancel_delayed_work_sync(&diag_summary);
}

void kgsl_diag_mem(struct kgsl_mem_entry *entry, int import, int fd,
		u64 offset, u64 requested)
{
	struct kgsl_memdesc *m = &entry->memdesc;

	if (!selected(entry->priv))
		return;
	/* Called once, while creator owns the entry, before ID publication. */
	entry->op5_diag_cookie = atomic64_inc_return(&cookies);
	if (!admit(import ? IMPORT : ALLOC))
		return;
	pr_info("OP5KGSL %s pid=%d cookie=%llu id=%u flags=%llx priv=%x gpu=%llx size=%llu dma_buf_raw_sg_size=%llu nents=%u orig_nents=%u fd=%d offset=%llu requested=%llu\n",
		import ? "import" : "alloc", pid_nr(entry->priv->pid),
		entry->op5_diag_cookie, entry->id, m->flags, m->priv,
		m->gpuaddr, m->size, entry->op5_diag_sg_size,
		m->sgt ? m->sgt->nents : 0,
		m->sgt ? m->sgt->orig_nents : 0, fd, offset, requested);
}

void kgsl_diag_destroy(struct kgsl_mem_entry *entry)
{
	if (!selected(entry->priv) || !entry->op5_diag_cookie ||
			!admit(DESTROY))
		return;
	pr_info("OP5KGSL destroy pid=%d cookie=%llu id=%u flags=%llx gpu=%llx size=%llu pending_free=%d\n",
		pid_nr(entry->priv->pid), entry->op5_diag_cookie, entry->id,
		entry->memdesc.flags, entry->memdesc.gpuaddr,
		entry->memdesc.size, entry->pending_free);
}

void kgsl_diag_submit(struct kgsl_drawobj_cmd *cmd, unsigned int numibs,
		int result)
{
	struct kgsl_context *ctx = cmd->base.context;
	struct kgsl_memobj_node *ib;
	unsigned int i = 0;

	if (!selected(ctx->proc_priv) || !admit(SUBMIT))
		return;
	pr_info("OP5KGSL submit pid=%d context=%u timestamp=%u flags=%lx numibs=%u result=%d\n",
		pid_nr(ctx->proc_priv->pid), ctx->id, cmd->base.timestamp,
		cmd->base.flags, numibs, result);
	list_for_each_entry(ib, &cmd->cmdlist, node) {
		pr_info("OP5KGSL ib pid=%d context=%u timestamp=%u index=%u id=%u offset=%llu gpu=%llx size=%llu flags=%lx\n",
			pid_nr(ctx->proc_priv->pid), ctx->id,
			cmd->base.timestamp, i++, ib->id, ib->offset,
			ib->gpuaddr, ib->size, ib->flags);
		if (i == 2)
			break;
	}
}

void kgsl_diag_fault(struct kgsl_process_private *private, u64 addr,
		unsigned int flags, u32 contextidr, u64 ptbase)
{
	if (!selected(private) || !admit(FAULT))
		return;
	pr_info("OP5KGSL first_fault pid=%d addr=%llx flags=%x contextidr=%x ttbr0=%llx\n",
		pid_nr(private->pid), addr, flags, contextidr, ptbase);
	summary("first-fault");
}
