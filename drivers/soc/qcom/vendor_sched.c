// SPDX-License-Identifier: GPL-2.0-only
/*
 * A compatibility front end for the "/proc/vendor_sched" interface that Pixel
 * userspace writes to.
 *
 * Copyright (c) 2026, Edrick Sinsuan <evcsinsuan@gmail.com>
 *
 * The node set emulated here is the one Pixel's vh_sched module provides in
 * drivers/soc/google/vh/kernel/sched; no code from that module is used, every
 * write is translated into a write on the cgroup files this kernel already
 * exposes.
 *
 * Pixel keeps its scheduling policy in the vh_sched module, which is built on
 * about two dozen scheduler vendor hooks that this tree does not have. This
 * driver provides only the subset of the vendor_sched node set that Pixel
 * userspace writes to. The "cpu" controller carries cpu.uclamp.min,
 * cpu.uclamp.max and cpu.uclamp.latency_sensitive, and its cgroup.procs and
 * tasks files accept a pid, so a write on a node becomes a write on the
 * matching cgroup file.
 *
 * uclamp nodes keep the Pixel scale: a read returns exactly the value that was
 * written, while the cgroup file underneath is given the equivalent percentage.
 *
 * Nothing here hooks the scheduler. Task grouping is therefore at cgroup
 * granularity, which is also how the AOSP task profiles work, and the
 * uclamp_fork_reset_* pair is accepted and remembered but has no effect,
 * because a fork reset needs a scheduler hook. Readback nodes report the last
 * value written so configuration tools observe their own writes succeed.
 */

#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/uaccess.h>

#define VS_CPUCTL_ROOT		"/dev/cpuctl"
#define VS_PROC_DIR		"vendor_sched"
#define VS_SCHED_QOS_DIR	"sched_qos"
#define VS_MAX_VALUE		32
#define VS_MAX_NAME		64
#define VS_MAX_PATH		128

/*
 * The nodes are writable by their owner and its group, which is the system uid
 * the power HAL runs as. Init runs as root and does not need the mode relaxed.
 */
#define VS_MODE			0220
#define VS_SYSTEM_ID		1000

/*
 * The Pixel interface carries uclamp on the 0..SCHED_CAPACITY_SCALE scale,
 * while the cgroup files take the percentage described by UCLAMP_PERCENT_SHIFT
 * and UCLAMP_PERCENT_SCALE (linux/sched.h). A write is therefore converted with
 * the inverse of the kernel's capacity_from_percent().
 */

/* A group index of -1 stands for the nodes that are not per group. */
#define VS_GLOBAL_GROUP		(-1)

/*
 * Pixel group names and the cpuctl group each maps to on this platform. Groups
 * with no cpuctl directory of their own are folded into the closest group that
 * exists, and the mapping stays explicit so it is obvious which are real.
 */
static const struct vs_group {
	const char *name;
	const char *cpuctl;
} vs_groups[] = {
	{ "ta",		"top-app" },
	{ "fg",		"foreground" },
	{ "sys",	"system" },
	{ "bg",		"background" },
	{ "sysbg",	"system-background" },
	{ "rt",		"rt" },
	{ "dex2oat",	"dex2oat" },
	{ "sf",		"foreground" },
	{ "cam",	"foreground" },
	{ "cam_power",	"foreground" },
	{ "nnapi",	"foreground" },
	{ "ota",	"background" },
};

enum vs_kind {
	VS_UCLAMP_MIN,
	VS_UCLAMP_MAX,
	VS_PREFER_IDLE,
	VS_SET_TASKS,
	VS_SET_PROCS,
	VS_FORK_RESET_SET,
	VS_FORK_RESET_CLEAR,
	VS_TGID_TYPE,
	VS_RAMPUP_SET,
};

/* One node: a name format for the per group nodes, a literal one otherwise. */
struct vs_desc {
	const char *name;
	enum vs_kind kind;
};

struct vs_node {
	const char *name;
	enum vs_kind kind;
	int group;		/* index into vs_groups */
	struct mutex lock;
	char value[VS_MAX_VALUE];
};

static struct vs_node *vs_nodes;
static unsigned int vs_node_count;
static unsigned int vs_node_max;
static struct proc_dir_entry *vs_root;

/*
 * The power HAL tags an ADPF session by writing a tgid to check_tgid_type and
 * reading the tag out of the write return value: 1 is SystemUI, 2 is Chrome,
 * anything else is the default. Pixel decides that inside vendor_sched; here it
 * is decided from the task name, which is all this shim can see.
 */
#define VS_TAG_SYSTEM_UI	1
#define VS_TAG_CHROME		2

static int vs_tgid_tag(unsigned long tgid, int fallback)
{
	struct task_struct *task;
	char comm[TASK_COMM_LEN];

	task = find_get_task_by_vpid(tgid);
	if (!task)
		return fallback;

	get_task_comm(comm, task);
	put_task_struct(task);

	if (strnstr(comm, "systemui", TASK_COMM_LEN))
		return VS_TAG_SYSTEM_UI;
	if (strnstr(comm, "chrome", TASK_COMM_LEN) ||
	    strnstr(comm, "CrBrowserMain", TASK_COMM_LEN) ||
	    strnstr(comm, "CrRendererMain", TASK_COMM_LEN))
		return VS_TAG_CHROME;

	return fallback;
}

/*
 * Translate one node write into a write on a cgroup file. kernfs parses the
 * same buffer userspace would have written, so the cgroup validation and the
 * percent handling run exactly as they do for a direct write.
 */
static int vs_write_file(const char *path, const char *buf, size_t count)
{
	struct file *file;
	ssize_t written;

	file = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	written = kernel_write(file, buf, count, &file->f_pos);
	filp_close(file, NULL);

	if (written < 0)
		return written;

	return (int)count;
}

static int vs_write_attr(int group, const char *attr, const char *buf,
			 size_t count)
{
	char path[VS_MAX_PATH];

	snprintf(path, sizeof(path), "%s/%s/cpu.uclamp.%s", VS_CPUCTL_ROOT,
		 vs_groups[group].cpuctl, attr);

	return vs_write_file(path, buf, count);
}

static int vs_write_pid(const char *file, char *buf)
{
	char path[VS_MAX_PATH];
	char pid[VS_MAX_VALUE];
	unsigned long val;
	int ret;

	ret = kstrtoul(strim(buf), 10, &val);
	if (ret)
		return ret;

	snprintf(pid, sizeof(pid), "%lu", val);
	snprintf(path, sizeof(path), "%s/%s", VS_CPUCTL_ROOT, file);

	return vs_write_file(path, pid, strlen(pid));
}

/*
 * capacity_from_percent() in kernel/sched/core.c turns the percentage the
 * cgroup files take into a capacity value, and cpu_uclamp_print() formats that
 * percentage again. Both are inverted here with the same definitions: the value
 * is scaled into UCLAMP_PERCENT_SCALE, divided by the capacity scale, and
 * printed with UCLAMP_PERCENT_SHIFT decimals, so what the kernel derives from
 * this driver's write is the value that was asked for.
 */
static void vs_uclamp_percent(unsigned long util, char *out, size_t len)
{
	u64 percent = (u64)util * UCLAMP_PERCENT_SCALE;
	u64 whole;
	u32 rem;

	percent = DIV_ROUND_CLOSEST_ULL(percent, SCHED_CAPACITY_SCALE);
	whole = div_u64_rem(percent, SCHED_POW10(UCLAMP_PERCENT_SHIFT), &rem);
	snprintf(out, len, "%llu.%0*u", whole, UCLAMP_PERCENT_SHIFT, rem);
}

/*
 * Parse a uclamp node write. The interface carries the value on the
 * 0..SCHED_CAPACITY_SCALE scale, and "max" is accepted for cpu.uclamp.max the
 * way the cgroup file itself accepts it. A value above the scale is rejected
 * instead of being handed to capacity_from_percent(), which refuses anything
 * above a full percentage with -ERANGE.
 */
static int vs_uclamp_value(char *buf, bool is_max, unsigned long *value)
{
	char *str = strim(buf);

	if (is_max && !strcmp(str, "max")) {
		*value = SCHED_CAPACITY_SCALE;
		return 0;
	}

	if (kstrtoul(str, 10, value))
		return -EINVAL;
	if (*value > SCHED_CAPACITY_SCALE)
		return -ERANGE;

	return 0;
}

/*
 * What the kernel makes of a write has to give back the value that was asked
 * for, or the powerhint ladder quietly lands somewhere else. Walk the ladder's
 * boundaries at init, deriving both directions instead of comparing against
 * expected strings, and warn if any value fails to survive the round trip.
 */
static const unsigned long vs_uclamp_bounds[] = {
	0, 1, 40, 80, 100, 101, 308, 410, 512, 1024,
};

/* capacity_from_percent(), applied the way kernel/sched/core.c applies it. */
static unsigned long vs_percent_util(char *percent)
{
	s64 fixed = 0;

	if (cgroup_parse_float(percent, UCLAMP_PERCENT_SHIFT, &fixed))
		return SCHED_CAPACITY_SCALE + 1;
	if (fixed < 0 || (u64)fixed > UCLAMP_PERCENT_SCALE)
		return SCHED_CAPACITY_SCALE + 1;

	return DIV_ROUND_CLOSEST_ULL((u64)fixed << SCHED_CAPACITY_SHIFT,
				     UCLAMP_PERCENT_SCALE);
}

static void vs_uclamp_check(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vs_uclamp_bounds); i++) {
		unsigned long util = vs_uclamp_bounds[i];
		char percent[VS_MAX_VALUE];

		vs_uclamp_percent(util, percent, sizeof(percent));
		WARN_ONCE(vs_percent_util(percent) != util,
			  "vendor_sched: uclamp %lu does not survive %s\n",
			  util, percent);
	}
}

static const char *vs_uclamp_attr(enum vs_kind kind)
{
	return kind == VS_UCLAMP_MIN ? "min" : "max";
}

/*
 * Carry out one node write and hand back the value the node reports on
 * readback. check_tgid_type is not one of these: its answer is the write
 * return value itself, so vs_node_write routes it out first.
 */
static int vs_node_apply(struct vs_node *node, char *buf, size_t count,
			 char *value, size_t len)
{
	char conv[VS_MAX_VALUE];
	const char *stored;
	int ret;

	switch (node->kind) {
	case VS_UCLAMP_MIN:
	case VS_UCLAMP_MAX: {
		unsigned long util;

		ret = vs_uclamp_value(buf, node->kind == VS_UCLAMP_MAX,
				      &util);
		if (ret)
			return ret;

		/* The node reports the capacity value it was given. */
		stored = buf;
		vs_uclamp_percent(util, conv, sizeof(conv));
		ret = vs_write_attr(node->group, vs_uclamp_attr(node->kind),
				    conv, strlen(conv));
		break;
	}
	case VS_PREFER_IDLE: {
		unsigned long idle;

		if (kstrtoul(strim(buf), 10, &idle) || idle > 1)
			return -EINVAL;

		stored = idle ? "1" : "0";
		ret = vs_write_attr(node->group, "latency_sensitive", stored,
				    strlen(stored));
		break;
	}
	case VS_SET_TASKS:
		stored = buf;
		ret = vs_write_pid("tasks", buf);
		break;
	case VS_SET_PROCS:
		stored = buf;
		ret = vs_write_pid("cgroup.procs", buf);
		break;
	case VS_FORK_RESET_SET:
		stored = "1";
		ret = count;
		break;
	case VS_FORK_RESET_CLEAR:
		stored = "0";
		ret = count;
		break;
	case VS_RAMPUP_SET:
		/*
		 * Pixel applies this multiplier in its rampup machinery. This
		 * shim has none, so the value is accepted and dropped.
		 */
		stored = buf;
		ret = count;
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	strscpy(value, stored, len);
	strim(value);

	return ret;
}

static ssize_t vs_node_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	struct vs_node *node = PDE_DATA(file_inode(file));
	size_t len;

	if (!node)
		return -EINVAL;

	mutex_lock(&node->lock);
	len = strlen(node->value);
	mutex_unlock(&node->lock);

	return simple_read_from_buffer(ubuf, count, ppos, node->value, len);
}

static ssize_t vs_node_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct vs_node *node = PDE_DATA(file_inode(file));
	char buf[VS_MAX_VALUE];
	char value[VS_MAX_VALUE];
	int ret;

	if (!node || !count)
		return -EINVAL;

	if (count >= sizeof(buf))
		count = sizeof(buf) - 1;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (node->kind == VS_TGID_TYPE) {
		unsigned long tgid;

		if (kstrtoul(strim(buf), 10, &tgid))
			return -EINVAL;

		return vs_tgid_tag(tgid, count);
	}

	ret = vs_node_apply(node, buf, count, value, sizeof(value));
	if (ret < 0)
		return ret;

	mutex_lock(&node->lock);
	strscpy(node->value, value, sizeof(node->value));
	mutex_unlock(&node->lock);

	return ret;
}

static const struct file_operations vs_node_ops = {
	.read	= vs_node_read,
	.write	= vs_node_write,
	.llseek	= default_llseek,
};

static void vs_add_node(struct proc_dir_entry *parent, const char *name,
			enum vs_kind kind, int group)
{
	struct vs_node *node = &vs_nodes[vs_node_count];
	struct proc_dir_entry *entry;

	if (!parent)
		return;

	if (vs_node_count >= vs_node_max) {
		WARN_ONCE(1, "vendor_sched: no room for %s\n", name);
		return;
	}

	node->name = kstrdup(name, GFP_KERNEL);
	if (!node->name)
		return;

	node->kind = kind;
	node->group = group;
	mutex_init(&node->lock);
	strscpy(node->value, "0", sizeof(node->value));

	entry = proc_create_data(node->name, VS_MODE, parent, &vs_node_ops,
				 node);
	if (!entry)
		goto err_free_name;

	proc_set_user(entry, KUIDT_INIT(VS_SYSTEM_ID), KGIDT_INIT(VS_SYSTEM_ID));
	vs_node_count++;
	return;

err_free_name:
	kfree(node->name);
	node->name = NULL;
}

static const struct vs_desc vs_group_nodes[] = {
	{ "%s_uclamp_min",	VS_UCLAMP_MIN },
	{ "%s_uclamp_max",	VS_UCLAMP_MAX },
	{ "%s_prefer_idle",	VS_PREFER_IDLE },
	{ "set_task_group_%s",	VS_SET_TASKS },
	{ "set_proc_group_%s",	VS_SET_PROCS },
};

static const struct vs_desc vs_global_nodes[] = {
	{ "uclamp_fork_reset_set",	VS_FORK_RESET_SET },
	{ "uclamp_fork_reset_clear",	VS_FORK_RESET_CLEAR },
	{ "check_tgid_type",		VS_TGID_TYPE },
};

/* Nodes the power HAL opens directly, outside powerhint.json. */
static const struct vs_desc vs_sched_qos_nodes[] = {
	{ "rampup_multiplier_set",	VS_RAMPUP_SET },
};

static void vs_add_nodes(struct proc_dir_entry *parent,
			 const struct vs_desc *descs, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		vs_add_node(parent, descs[i].name, descs[i].kind,
			    VS_GLOBAL_GROUP);
}

static void vs_add_group_nodes(struct proc_dir_entry *parent, const char *group,
			       int index)
{
	char name[VS_MAX_NAME];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vs_group_nodes); i++) {
		snprintf(name, sizeof(name), vs_group_nodes[i].name, group);
		vs_add_node(parent, name, vs_group_nodes[i].kind, index);
	}
}

static int __init vs_init(void)
{
	struct proc_dir_entry *parent;
	unsigned int i;
	int ret = -ENOMEM;

	vs_node_max = ARRAY_SIZE(vs_groups) * ARRAY_SIZE(vs_group_nodes) +
		      ARRAY_SIZE(vs_global_nodes) +
		      ARRAY_SIZE(vs_sched_qos_nodes);

	vs_uclamp_check();

	vs_nodes = kcalloc(vs_node_max, sizeof(*vs_nodes), GFP_KERNEL);
	if (!vs_nodes)
		return ret;

	vs_root = proc_mkdir(VS_PROC_DIR, NULL);
	if (!vs_root)
		goto err_free_nodes;

	for (i = 0; i < ARRAY_SIZE(vs_groups); i++)
		vs_add_group_nodes(vs_root, vs_groups[i].name, i);

	vs_add_nodes(vs_root, vs_global_nodes, ARRAY_SIZE(vs_global_nodes));

	parent = proc_mkdir(VS_SCHED_QOS_DIR, vs_root);
	if (parent) {
		proc_set_user(parent, KUIDT_INIT(VS_SYSTEM_ID),
			      KGIDT_INIT(VS_SYSTEM_ID));
		vs_add_nodes(parent, vs_sched_qos_nodes,
			     ARRAY_SIZE(vs_sched_qos_nodes));
	}

	pr_info("vendor_sched: %u compatibility nodes created\n", vs_node_count);
	return 0;

err_free_nodes:
	kfree(vs_nodes);
	vs_nodes = NULL;
	return ret;
}

static void __exit vs_exit(void)
{
	unsigned int i;

	proc_remove(vs_root);
	vs_root = NULL;

	for (i = 0; i < vs_node_count; i++)
		kfree(vs_nodes[i].name);

	kfree(vs_nodes);
	vs_nodes = NULL;
	vs_node_count = 0;
}

module_init(vs_init);
module_exit(vs_exit);

MODULE_DESCRIPTION("Pixel /proc/vendor_sched compatibility front end");
MODULE_LICENSE("GPL v2");
