// SPDX-License-Identifier: GPL-2.0-only
/* OP5 V12 metadata-only observer. Never dereference GPU command contents. */
#include <linux/atomic.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "kgsl_device.h"
#include "kgsl_drawobj.h"
#include "kgsl_diag.h"

enum { ALLOC, IMPORT, DESTROY, SUBMIT, FAULT, EVENTS };
static const unsigned int caps[EVENTS] = { 128, 128, 128, 32, 1 };
static atomic64_t seen[EVENTS];
static atomic_t emitted[EVENTS];
static atomic64_t cookies = ATOMIC64_INIT(0);

#define RECENT_COUNT 32
struct recent_ib {
	u64 offset, gpuaddr, size;
	unsigned long flags;
	unsigned int id;
};
struct recent_submit {
	u64 sequence;
	int pid, result;
	unsigned int context, timestamp, numibs, copied;
	unsigned long flags;
	struct recent_ib ib[2];
};
static DEFINE_RAW_SPINLOCK(recent_lock);
static struct recent_submit recent[RECENT_COUNT], fault_snapshot[RECENT_COUNT];
static unsigned int recent_next, recent_count;
static u64 recent_sequence;
static atomic64_t recent_skipped = ATOMIC64_INIT(0);

/* Separate trylock: never wait on a submitting CPU from fault context.
 * Records contain values only; no object pointer escapes the submit hook.
 */
static void remember_submit(struct kgsl_drawobj_cmd *cmd, unsigned int numibs,
		int result)
{
	struct recent_submit r = { 0 };
	struct kgsl_context *ctx = cmd->base.context;
	struct kgsl_memobj_node *ib;
	unsigned long irqflags;

	r.pid = pid_nr(ctx->proc_priv->pid);
	r.context = ctx->id;
	r.timestamp = cmd->base.timestamp;
	r.flags = cmd->base.flags;
	r.numibs = numibs;
	r.result = result;
	list_for_each_entry(ib, &cmd->cmdlist, node) {
		struct recent_ib *out = &r.ib[r.copied++];

		out->id = ib->id;
		out->offset = ib->offset;
		out->gpuaddr = ib->gpuaddr;
		out->size = ib->size;
		out->flags = ib->flags;
		if (r.copied == 2)
			break;
	}
	if (!raw_spin_trylock_irqsave(&recent_lock, irqflags)) {
		atomic64_inc(&recent_skipped);
		return;
	}
	r.sequence = ++recent_sequence;
	recent[recent_next] = r;
	recent_next = (recent_next + 1) % RECENT_COUNT;
	if (recent_count < RECENT_COUNT)
		recent_count++;
	raw_spin_unlock_irqrestore(&recent_lock, irqflags);
}

/* FAULT admission guarantees one writer to this static snapshot. No large
 * fault stack object, sleeping/device locks, allocation or printk under lock.
 * Snapshot is coherent at lock acquisition, not an atomic fault-time history.
 */
static void dump_recent(void)
{
	unsigned long irqflags;
	unsigned int count, start, i, j;
	u64 last, skipped;

	if (!raw_spin_trylock_irqsave(&recent_lock, irqflags)) {
		pr_info("OP5KGSL recent snapshot=skipped-lock-busy submit_skipped=%llu\n",
			atomic64_read(&recent_skipped));
		return;
	}
	count = recent_count;
	start = (recent_next + RECENT_COUNT - count) % RECENT_COUNT;
	for (i = 0; i < count; i++)
		fault_snapshot[i] = recent[(start + i) % RECENT_COUNT];
	last = recent_sequence;
	skipped = atomic64_read(&recent_skipped);
	raw_spin_unlock_irqrestore(&recent_lock, irqflags);
	pr_info("OP5KGSL recent snapshot=complete count=%u last=%llu submit_skipped=%llu scope=latest-successful-lock-acquisitions\n",
		count, last, skipped);
	for (i = 0; i < count; i++) {
		const struct recent_submit *r = &fault_snapshot[i];

		pr_info("OP5KGSL recent_submit sequence=%llu pid=%d context=%u timestamp=%u flags=%lx numibs=%u result=%d copied=%u\n",
			r->sequence, r->pid, r->context, r->timestamp,
			r->flags, r->numibs, r->result, r->copied);
		for (j = 0; j < r->copied; j++) {
			const struct recent_ib *ib = &r->ib[j];

			pr_info("OP5KGSL recent_ib sequence=%llu index=%u id=%u offset=%llu gpu=%llx size=%llu flags=%lx\n",
				r->sequence, j, ib->id, ib->offset,
				ib->gpuaddr, ib->size, ib->flags);
		}
	}
}

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

	if (!selected(ctx->proc_priv))
		return;
	remember_submit(cmd, numibs, result);
	if (!admit(SUBMIT))
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
	dump_recent();
	pr_info("OP5KGSL first_fault pid=%d addr=%llx flags=%x contextidr=%x ttbr0=%llx\n",
		pid_nr(private->pid), addr, flags, contextidr, ptbase);
	summary("first-fault");
}
