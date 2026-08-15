// SPDX-License-Identifier: GPL-2.0
#include <linux/input.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <soc/qcom/restart.h>

#define POWERKEY_PANIC_DELAY_MS 5000

static struct proc_dir_entry *powerkey_dir;

static int powerkey_show(struct seq_file *seq, void *unused)
{
	bool reboot_recovery;
	bool warm_reboot;

	msm_powerkey_get_reboot_mode(&warm_reboot, &reboot_recovery);
	seq_printf(seq, "warm_reboot: %s reboot_rec: %s\n",
		   warm_reboot ? "enabled" : "disabled",
		   reboot_recovery ? "enabled" : "disabled");

	return 0;
}

static int powerkey_open(struct inode *inode, struct file *file)
{
	return single_open(file, powerkey_show, NULL);
}

static ssize_t powerkey_write(struct file *file, const char __user *userbuf,
			      size_t count, loff_t *data)
{
	char buf[16];
	int reboot_recovery;
	int warm_reboot;

	if (!count || count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, userbuf, count))
		return -EFAULT;

	buf[count] = '\0';
	if (sscanf(buf, "%d,%d", &warm_reboot, &reboot_recovery) != 2)
		return -EINVAL;
	if (warm_reboot < 0 || warm_reboot > 1 ||
	    reboot_recovery < 0 || reboot_recovery > 1)
		return -EINVAL;

	msm_powerkey_set_reboot_mode(warm_reboot, reboot_recovery);
	return count;
}

static void powerkey_panic_work(struct work_struct *unused)
{
	bool reboot_recovery;

	msm_powerkey_get_reboot_mode(NULL, &reboot_recovery);
	msm_powerkey_set_reboot_mode(true, reboot_recovery);
	pr_emerg("Power key held: forcing warm restart\n");
	kernel_restart("powerkey_panic");
}

static DECLARE_DELAYED_WORK(powerkey_panic_worker, powerkey_panic_work);

static void powerkey_panic_event(struct input_handle *handle,
				 unsigned int type, unsigned int code, int value)
{
	if (type != EV_KEY || code != KEY_POWER)
		return;

	if (value == 1)
		queue_delayed_work(system_highpri_wq, &powerkey_panic_worker,
				   msecs_to_jiffies(POWERKEY_PANIC_DELAY_MS));
	else if (value == 0)
		cancel_delayed_work(&powerkey_panic_worker);
}

static int powerkey_panic_connect(struct input_handler *handler,
				  struct input_dev *dev,
				  const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "powerkey_panic";

	error = input_register_handle(handle);
	if (error)
		goto err_free_handle;

	error = input_open_device(handle);
	if (error)
		goto err_unregister_handle;

	return 0;

err_unregister_handle:
	input_unregister_handle(handle);
err_free_handle:
	kfree(handle);
	return error;
}

static void powerkey_panic_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct file_operations powerkey_ops = {
	.open = powerkey_open,
	.read = seq_read,
	.write = powerkey_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct input_device_id powerkey_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_KEYBIT,
		.evbit = { BIT_MASK(EV_KEY) },
		.keybit = { [BIT_WORD(KEY_POWER)] = BIT_MASK(KEY_POWER) },
	},
	{ },
};

static struct input_handler powerkey_panic_handler = {
	.event = powerkey_panic_event,
	.connect = powerkey_panic_connect,
	.disconnect = powerkey_panic_disconnect,
	.name = "powerkey_panic_handler",
	.id_table = powerkey_ids,
};

static int __init powerkey_panic_init(void)
{
	struct proc_dir_entry *reboot_entry;
	bool reboot_recovery;
	bool warm_reboot;
	int error;

	powerkey_dir = proc_mkdir("powerkey", NULL);
	if (!powerkey_dir)
		return -ENOMEM;

	reboot_entry = proc_create("reboot", 0664, powerkey_dir,
				   &powerkey_ops);
	if (!reboot_entry) {
		remove_proc_entry("powerkey", NULL);
		return -ENOMEM;
	}

	error = input_register_handler(&powerkey_panic_handler);
	if (error) {
		remove_proc_entry("reboot", powerkey_dir);
		remove_proc_entry("powerkey", NULL);
		return error;
	}

	warm_reboot = IS_ENABLED(CONFIG_POWERKEY_FORCE_WARM_REBOOT);
	reboot_recovery = IS_ENABLED(CONFIG_POWERKEY_FORCE_REBOOT_REC);
	msm_powerkey_set_reboot_mode(warm_reboot, reboot_recovery);

	return 0;
}
module_init(powerkey_panic_init);

MODULE_DESCRIPTION("Power-key warm restart debugger");
MODULE_LICENSE("GPL");
