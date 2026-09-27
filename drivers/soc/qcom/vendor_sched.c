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
 * userspace writes to, and translates every node write into a write on the
 * cgroup files this kernel already exposes: the "cpu" controller carries
 * cpu.uclamp.min, cpu.uclamp.max and cpu.uclamp.latency_sensitive, and its
 * cgroup.procs and tasks files accept a pid.
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
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/uaccess.h>

#define VS_CPUCTL_ROOT		"/dev/cpuctl"
#define VS_PROC_DIR		"vendor_sched"
#define VS_MAX_VALUE		32

/*
 * The nodes are world-unwritable and owned by system, matching the Pixel
 * interface that the power HAL writes as uid 1000. Init runs as root and does
 * not need the mode relaxed.
 */
#define VS_MODE			0220
#define VS_UID			1000
#define VS_GID			1000

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

struct vs_node {
	const char *name;
	enum vs_kind kind;
	int group;		/* index into vs_groups, -1 for the globals */
	struct mutex lock;
	char value[VS_MAX_VALUE];
};

static struct vs_node *vs_nodes;
static unsigned int vs_node_count;
static struct proc_dir_entry *vs_root;
static struct proc_dir_entry *vs_sched_qos;

/*
 * The power HAL tags an ADPF session by writing a tgid to check_tgid_type and
 * reading the tag out of the write return value: 1 is SystemUI, 2 is Chrome,
 * anything else is the default. Pixel decides that inside vendor_sched; here it
 * is decided from the task name, which is all this shim can see.
 */
#define VS_TAG_SYSTEM_UI	1
#define VS_TAG_CHROME		2

static int vs_tgid_type(unsigned long tgid, int fallback)
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
	ssize_t ret;

	file = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = kernel_write(file, buf, count, &file->f_pos);
	filp_close(file, NULL);

	return ret < 0 ? ret : (int)count;
}

static int vs_write_attr(int group, const char *attr, const char *buf,
			 size_t count)
{
	char path[128];

	snprintf(path, sizeof(path), "%s/%s/cpu.uclamp.%s", VS_CPUCTL_ROOT,
		 vs_groups[group].cpuctl, attr);

	return vs_write_file(path, buf, count);
}

static int vs_write_pid(const char *file, const char *buf, size_t count)
{
	char path[128];
	char pid[VS_MAX_VALUE];
	unsigned long val;
	int ret;

	ret = kstrtoul(strim((char *)buf), 10, &val);
	if (ret)
		return ret;

	snprintf(pid, sizeof(pid), "%lu", val);
	snprintf(path, sizeof(path), "%s/%s", VS_CPUCTL_ROOT, file);

	return vs_write_file(path, pid, strlen(pid));
}

/*
 * The cpu controller parses cpu.uclamp.min and cpu.uclamp.max as a percentage
 * and rejects anything above 100 with -ERANGE, while the Pixel interface takes
 * these values on the 0 to SCHED_CAPACITY_SCALE scale it uses everywhere else.
 * Values above 100 are therefore read as that scale and converted; values of
 * 100 or below are already percentages and pass through unchanged.
 */
static const char *vs_uclamp_value(const char *buf, char *out, size_t len)
{
	unsigned long raw;
	char *val = strim((char *)buf);

	if (strcmp(val, "max") && !kstrtoul(val, 10, &raw) && raw > 100)
		snprintf(out, len, "%d", (int)DIV_ROUND_CLOSEST(raw * 100, 1024));
	else
		strscpy(out, val, len);

	return out;
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
	char conv[VS_MAX_VALUE];
	const char *stored = buf;
	int ret;

	if (!node || count == 0)
		return -EINVAL;
	if (count >= sizeof(buf))
		count = sizeof(buf) - 1;

	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	switch (node->kind) {
	case VS_UCLAMP_MIN:
		stored = vs_uclamp_value(buf, conv, sizeof(conv));
		ret = vs_write_attr(node->group, "min", stored, strlen(stored));
		break;
	case VS_UCLAMP_MAX:
		stored = vs_uclamp_value(buf, conv, sizeof(conv));
		ret = vs_write_attr(node->group, "max", stored, strlen(stored));
		break;
	case VS_PREFER_IDLE:
		ret = vs_write_attr(node->group, "latency_sensitive", buf, count);
		break;
	case VS_SET_TASKS:
		ret = vs_write_pid("tasks", buf, count);
		break;
	case VS_SET_PROCS:
		ret = vs_write_pid("cgroup.procs", buf, count);
		break;
	case VS_TGID_TYPE: {
		unsigned long tgid;

		if (kstrtoul(strim(buf), 10, &tgid))
			return -EINVAL;

		return vs_tgid_type(tgid, count);
	}
	case VS_RAMPUP_SET:
		/*
		 * Pixel applies this multiplier in its rampup machinery; this shim
		 * has no such machinery, so the value is accepted and dropped.
		 */
		ret = count;
		break;
	case VS_FORK_RESET_SET:
	case VS_FORK_RESET_CLEAR:
		ret = count;
		break;
	default:
		return -EINVAL;
	}

	if (ret < 0)
		return ret;

	mutex_lock(&node->lock);
	switch (node->kind) {
	case VS_FORK_RESET_SET:
		strscpy(node->value, "1", sizeof(node->value));
		break;
	case VS_FORK_RESET_CLEAR:
		strscpy(node->value, "0", sizeof(node->value));
		break;
	default:
		strscpy(node->value, strim((char *)stored), sizeof(node->value));
		break;
	}
	mutex_unlock(&node->lock);

	return count;
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

	node->name = kstrdup(name, GFP_KERNEL);
	if (!node->name)
		return;

	node->kind = kind;
	node->group = group;
	mutex_init(&node->lock);
	strscpy(node->value, "0", sizeof(node->value));

	entry = proc_create_data(node->name, VS_MODE, parent, &vs_node_ops, node);
	if (!entry) {
		pr_warn("vendor_sched: failed to create %s\n", node->name);
		kfree(node->name);
		node->name = NULL;
		return;
	}

	proc_set_user(entry, KUIDT_INIT(VS_UID), KGIDT_INIT(VS_GID));
	vs_node_count++;
}

static const struct {
	const char *fmt;
	enum vs_kind kind;
} vs_group_nodes[] = {
	{ "%s_uclamp_min",	VS_UCLAMP_MIN },
	{ "%s_uclamp_max",	VS_UCLAMP_MAX },
	{ "%s_prefer_idle",	VS_PREFER_IDLE },
	{ "set_task_group_%s",	VS_SET_TASKS },
	{ "set_proc_group_%s",	VS_SET_PROCS },
};

static int __init vs_init(void)
{
	unsigned int g, i;
	char name[64];

	vs_nodes = kcalloc(ARRAY_SIZE(vs_groups) * ARRAY_SIZE(vs_group_nodes) + 4,
			   sizeof(*vs_nodes), GFP_KERNEL);
	if (!vs_nodes)
		return -ENOMEM;

	vs_root = proc_mkdir(VS_PROC_DIR, NULL);
	if (!vs_root) {
		kfree(vs_nodes);
		vs_nodes = NULL;
		return -ENOMEM;
	}

	for (g = 0; g < ARRAY_SIZE(vs_groups); g++) {
		for (i = 0; i < ARRAY_SIZE(vs_group_nodes); i++) {
			snprintf(name, sizeof(name), vs_group_nodes[i].fmt,
				 vs_groups[g].name);
			vs_add_node(vs_root, name, vs_group_nodes[i].kind, g);
		}
	}

	vs_add_node(vs_root, "uclamp_fork_reset_set", VS_FORK_RESET_SET, -1);
	vs_add_node(vs_root, "uclamp_fork_reset_clear", VS_FORK_RESET_CLEAR, -1);

	/* Nodes the power HAL opens directly, outside powerhint.json. */
	vs_add_node(vs_root, "check_tgid_type", VS_TGID_TYPE, -1);

	vs_sched_qos = proc_mkdir("sched_qos", vs_root);
	if (vs_sched_qos)
		proc_set_user(vs_sched_qos, KUIDT_INIT(VS_UID), KGIDT_INIT(VS_GID));
	vs_add_node(vs_sched_qos, "rampup_multiplier_set", VS_RAMPUP_SET, -1);

	pr_info("vendor_sched: %u compatibility nodes created\n", vs_node_count);
	return 0;
}

static void __exit vs_exit(void)
{
	unsigned int i;

	proc_remove(vs_root);

	for (i = 0; i < vs_node_count; i++)
		kfree(vs_nodes[i].name);

	kfree(vs_nodes);
	vs_nodes = NULL;
	vs_node_count = 0;
}

module_init(vs_init);
module_exit(vs_exit);

MODULE_DESCRIPTION("Compatibility front end for the Pixel /proc/vendor_sched interface");
MODULE_LICENSE("GPL v2");
