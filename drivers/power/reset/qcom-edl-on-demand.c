// SPDX-License-Identifier: GPL-2.0-only
/* Explicit, privileged control for the built-in Qualcomm full-dump backend. */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include <soc/qcom/edl-on-demand.h>

#define QCOM_EDL_CONFIRM "CONFIRM_QCOM_FULL_DDR_ONCE\n"

enum qcom_edl_control_state {
	QCOM_EDL_IDLE,
	QCOM_EDL_ACCEPTED,
};

static atomic_t qcom_edl_state = ATOMIC_INIT(QCOM_EDL_IDLE);

static ssize_t qcom_edl_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char confirmation[sizeof(QCOM_EDL_CONFIRM) - 1];
	int ret;

	/* Neither a namespaced root nor DAC access alone authorizes a reset. */
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) ||
	    !ns_capable(&init_user_ns, CAP_SYS_BOOT))
		return -EPERM;

	if (count != sizeof(confirmation))
		return -EINVAL;
	if (copy_from_user(confirmation, buf, sizeof(confirmation)))
		return -EFAULT;
	/* Compare every byte, including the required newline; no NUL accepted. */
	if (memcmp(confirmation, QCOM_EDL_CONFIRM, sizeof(confirmation)))
		return -EINVAL;

	if (atomic_cmpxchg(&qcom_edl_state, QCOM_EDL_IDLE,
			   QCOM_EDL_ACCEPTED) != QCOM_EDL_IDLE)
		return -EALREADY;

	pr_emerg("qcom-edl-on-demand: explicit full-dump intent uid=%u pid=%d\n",
		 __kuid_val(current_euid()), task_pid_nr(current));

	/*
	 * fops.owner protects this write. Take an additional reference before
	 * handing off so a successful, irreversible request prevents unload
	 * even after close. try_module_get(NULL) also supports built-in use.
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	ret = qcom_edl_request_full_dump();
	if (ret) {
		module_put(THIS_MODULE);
		/* The attempt remains consumed; rejection never silently rearms. */
		return ret;
	}

	/* Success acknowledges scheduling only; retain the pin until reset. */
	return count;
}

static const struct file_operations qcom_edl_fops = {
	.owner = THIS_MODULE,
	.write = qcom_edl_write,
	.llseek = no_llseek,
};

static struct miscdevice qcom_edl_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "qcom-edl-on-demand",
	.fops = &qcom_edl_fops,
	.mode = 0200,
};

static int __init qcom_edl_init(void)
{
	/* Loading only publishes the control endpoint; it never requests EDL. */
	return misc_register(&qcom_edl_device);
}

static void __exit qcom_edl_exit(void)
{
	/* No module-owned asynchronous callbacks exist to drain. */
	misc_deregister(&qcom_edl_device);
}

module_init(qcom_edl_init);
module_exit(qcom_edl_exit);

MODULE_DESCRIPTION("Explicit one-shot Qualcomm full-dump control");
MODULE_LICENSE("GPL");
