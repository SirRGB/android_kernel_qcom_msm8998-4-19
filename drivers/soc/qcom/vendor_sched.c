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
 * Nothing here hooks the scheduler. Task grouping is therefore at cgroup
 * granularity, which is also how the AOSP task profiles work, and the
 * uclamp_fork_reset_* pair is accepted and remembered but has no effect,
 * because a fork reset needs a scheduler hook. Readback nodes report the last
 * value written so configuration tools observe their own writes succeed.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
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
 * cpu.uclamp.min and cpu.uclamp.max are percentages, while the Pixel interface
 * carries the same value on the SCHED_CAPACITY_SCALE scale it uses everywhere
 * else; kernel/sched/core.c converts between the two the same way.
 */
#define VS_PERCENT_SCALE	100

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
 * The cpu controller rejects a value above 100 with -ERANGE, so a value above
 * the percent scale is read as a SCHED_CAPACITY_SCALE one and converted, and
 * anything else already is a percentage and passes through.
 */
static const char *vs_uclamp_value(char *buf, char *out, size_t len)
{
	unsigned long raw;
	char *value = strim(buf);

	if (strcmp(value, "max") && !kstrtoul(value, 10, &raw) &&
	    raw > VS_PERCENT_SCALE)
		snprintf(out, len, "%lu",
			 DIV_ROUND_CLOSEST(raw * VS_PERCENT_SCALE,
					   SCHED_CAPACITY_SCALE));
	else
		strscpy(out, value, len);

	return out;
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
	case VS_UCLAMP_MAX:
		stored = vs_uclamp_value(buf, conv, sizeof(conv));
		ret = vs_write_attr(node->group, vs_uclamp_attr(node->kind),
				    stored, strlen(stored));
		break;
	case VS_PREFER_IDLE:
		stored = buf;
		ret = vs_write_attr(node->group, "latency_sensitive", buf,
				    count);
		break;
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
