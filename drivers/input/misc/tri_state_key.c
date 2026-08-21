// SPDX-License-Identifier: GPL-2.0
/* OnePlus tri-state alert slider driver. */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/switch.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define DRV_NAME		"tri-state-key"
#define OPLUS_TRI_KEY		"oplus,tri-state-key"
#define TRI_STATE_KEYS		3
#define TRI_STATE_RETRIES	5

enum tri_mode {
	MODE_UNKNOWN,
	MODE_MUTE,
	MODE_DO_NOT_DISTURB,
	MODE_NORMAL,
	MODE_MAX_NUM,
};

struct tri_state_key_data {
	struct device *dev;
	int gpio[TRI_STATE_KEYS];
	int irq[TRI_STATE_KEYS];
	bool gpio_requested[TRI_STATE_KEYS];
	bool irq_requested[TRI_STATE_KEYS];
	bool irq_wake_enabled[TRI_STATE_KEYS];
	short stored_current_mode;
	struct workqueue_struct *wq;
	struct work_struct work;
	struct input_dev *input;
	bool input_registered;
	struct switch_dev sdev;
	bool switch_registered;
	struct timer_list timer;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active_state;
	struct pinctrl_state *suspend_state;
	struct proc_dir_entry *proc_dir;
	struct proc_dir_entry *proc_file;
};

static const char * const tri_state_gpio_names[TRI_STATE_KEYS] = {
	"tristate_key1-int", "tristate_key2-int", "tristate_key3-int",
};

static const char * const tri_state_irq_names[TRI_STATE_KEYS] = {
	"tristate_key1", "tristate_key2", "tristate_key3",
};

static const char * const tri_state_gpio_properties[TRI_STATE_KEYS] = {
	"tristate,gpio_key1", "tristate,gpio_key2", "tristate,gpio_key3",
};

static void tri_state_report_input(struct input_dev *input, int mode)
{
	input_report_key(input, KEY_F3, mode);
	input_sync(input);
	input_report_key(input, KEY_F3, 0);
	input_sync(input);
}

static void tri_state_work(struct work_struct *work)
{
	struct tri_state_key_data *data =
		container_of(work, struct tri_state_key_data, work);
	bool switch_state[MODE_MAX_NUM] = { false };
	int candidate = MODE_UNKNOWN;
	int switch_count;
	int retries = TRI_STATE_RETRIES;
	int i;

	do {
		switch_state[MODE_MUTE] = !gpio_get_value(data->gpio[0]);
		switch_state[MODE_DO_NOT_DISTURB] =
			!gpio_get_value(data->gpio[1]);
		switch_state[MODE_NORMAL] = !gpio_get_value(data->gpio[2]);

		switch_count = 0;
		candidate = MODE_UNKNOWN;
		for (i = MODE_MUTE; i < MODE_MAX_NUM; i++) {
			if (!switch_state[i])
				continue;
			candidate = i;
			if (++switch_count > 1) {
				msleep(100);
				break;
			}
		}
	} while (--retries && switch_count != 1);

	/* Retain the current mode after an indeterminate retry window. */
	if (switch_count != 1 || candidate == data->stored_current_mode)
		return;

	data->stored_current_mode = candidate;
	switch_set_state(&data->sdev, candidate);
	tri_state_report_input(data->input, candidate);
	dev_info(data->dev, "tri-state set to state %d\n", candidate);
}

static irqreturn_t tri_state_interrupt(int irq, void *dev_id)
{
	struct tri_state_key_data *data = dev_id;

	queue_work(data->wq, &data->work);
	return IRQ_HANDLED;
}

static void tri_state_timer(struct timer_list *timer)
{
	struct tri_state_key_data *data = from_timer(data, timer, timer);

	queue_work(data->wq, &data->work);
}

static int tri_state_parse_dt(struct tri_state_key_data *data)
{
	struct device_node *node = data->dev->of_node;
	int i;

	if (!node)
		return -EINVAL;
	for (i = 0; i < TRI_STATE_KEYS; i++) {
		data->gpio[i] = of_get_named_gpio(node,
						    tri_state_gpio_properties[i], 0);
		if (!gpio_is_valid(data->gpio[i]))
			return data->gpio[i] < 0 ? data->gpio[i] : -EINVAL;
	}
	return 0;
}

static ssize_t tri_state_proc_read(struct file *file, char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	struct tri_state_key_data *data = PDE_DATA(file_inode(file));
	char page[6];
	int length;

	length = scnprintf(page, sizeof(page), "%d\n",
			   data->stored_current_mode);
	return simple_read_from_buffer(user_buf, count, ppos, page, length);
}

static const struct file_operations tri_state_proc_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = tri_state_proc_read,
	.llseek = default_llseek,
};

static int tri_state_create_proc(struct tri_state_key_data *data)
{
	data->proc_dir = proc_mkdir("tristatekey", NULL);
	if (!data->proc_dir)
		return -ENOMEM;
	data->proc_file = proc_create_data("tri_state", 0644, data->proc_dir,
					  &tri_state_proc_ops, data);
	if (!data->proc_file) {
		proc_remove(data->proc_dir);
		data->proc_dir = NULL;
		return -ENOMEM;
	}
	return 0;
}

static void tri_state_remove_proc(struct tri_state_key_data *data)
{
	if (data->proc_file) {
		proc_remove(data->proc_file);
		data->proc_file = NULL;
	}
	if (data->proc_dir) {
		proc_remove(data->proc_dir);
		data->proc_dir = NULL;
	}
}

static int tri_state_register_input(struct tri_state_key_data *data)
{
	int error;

	data->input = input_allocate_device();
	if (!data->input)
		return -ENOMEM;
	data->input->name = OPLUS_TRI_KEY;
	data->input->dev.parent = data->dev;
	data->input->id.vendor = 0x22d9;
	input_set_capability(data->input, EV_KEY, KEY_F3);
	input_set_drvdata(data->input, data);
	error = input_register_device(data->input);
	if (error) {
		input_free_device(data->input);
		data->input = NULL;
		return error;
	}
	data->input_registered = true;
	return 0;
}

static void tri_state_unregister_input(struct tri_state_key_data *data)
{
	if (!data->input)
		return;
	if (data->input_registered)
		input_unregister_device(data->input);
	else
		input_free_device(data->input);
	data->input = NULL;
	data->input_registered = false;
}

static void tri_state_release_irqs(struct tri_state_key_data *data)
{
	int i;

	for (i = TRI_STATE_KEYS - 1; i >= 0; i--) {
		if (data->irq_wake_enabled[i]) {
			disable_irq_wake(data->irq[i]);
			data->irq_wake_enabled[i] = false;
		}
		if (data->irq_requested[i]) {
			free_irq(data->irq[i], data);
			data->irq_requested[i] = false;
		}
	}
}

static void tri_state_release_gpios(struct tri_state_key_data *data)
{
	int i;

	for (i = TRI_STATE_KEYS - 1; i >= 0; i--) {
		if (data->gpio_requested[i]) {
			gpio_free(data->gpio[i]);
			data->gpio_requested[i] = false;
		}
	}
}

static int tri_state_request_gpios(struct tri_state_key_data *data)
{
	int error;
	int i;

	for (i = 0; i < TRI_STATE_KEYS; i++) {
		error = gpio_request(data->gpio[i], tri_state_gpio_names[i]);
		if (error)
			return error;
		data->gpio_requested[i] = true;
		error = gpio_direction_input(data->gpio[i]);
		if (error)
			return error;
	}
	return 0;
}

static int tri_state_request_irqs(struct tri_state_key_data *data)
{
	int error;
	int i;

	for (i = 0; i < TRI_STATE_KEYS; i++) {
		data->irq[i] = gpio_to_irq(data->gpio[i]);
		if (data->irq[i] <= 0)
			return data->irq[i] ?: -EINVAL;
		error = request_irq(data->irq[i], tri_state_interrupt,
				    IRQF_TRIGGER_FALLING,
				    tri_state_irq_names[i], data);
		if (error)
			return error;
		data->irq_requested[i] = true;
		error = enable_irq_wake(data->irq[i]);
		if (error)
			return error;
		data->irq_wake_enabled[i] = true;
	}
	return 0;
}

static void tri_state_cleanup(struct tri_state_key_data *data)
{
	del_timer_sync(&data->timer);
	tri_state_release_irqs(data);
	if (data->wq)
		cancel_work_sync(&data->work);
	tri_state_remove_proc(data);
	if (data->switch_registered) {
		switch_dev_unregister(&data->sdev);
		data->switch_registered = false;
	}
	tri_state_unregister_input(data);
	tri_state_release_gpios(data);
	if (data->wq) {
		destroy_workqueue(data->wq);
		data->wq = NULL;
	}
}

static int tri_state_probe(struct platform_device *pdev)
{
	struct tri_state_key_data *data;
	int error;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = &pdev->dev;
	platform_set_drvdata(pdev, data);
	INIT_WORK(&data->work, tri_state_work);
	timer_setup(&data->timer, tri_state_timer, 0);
	data->wq = alloc_workqueue("tsk_switch_wq", WQ_HIGHPRI, 1);
	if (!data->wq) {
		error = -ENOMEM;
		goto err_cleanup;
	}
	error = tri_state_register_input(data);
	if (error)
		goto err_cleanup;
	data->pinctrl = devm_pinctrl_get(data->dev);
	if (IS_ERR(data->pinctrl)) {
		error = PTR_ERR(data->pinctrl);
		goto err_cleanup;
	}
	data->active_state = pinctrl_lookup_state(data->pinctrl,
						  "pmx_tri_state_key_active");
	if (IS_ERR(data->active_state)) {
		error = PTR_ERR(data->active_state);
		goto err_cleanup;
	}
	data->suspend_state = pinctrl_lookup_state(data->pinctrl,
						   "pmx_tri_state_key_suspend");
	if (IS_ERR(data->suspend_state)) {
		error = PTR_ERR(data->suspend_state);
		goto err_cleanup;
	}
	error = pinctrl_select_state(data->pinctrl, data->active_state);
	if (error)
		goto err_cleanup;
	error = tri_state_parse_dt(data);
	if (error)
		goto err_cleanup;
	error = tri_state_request_gpios(data);
	if (error)
		goto err_cleanup;
	data->sdev.name = DRV_NAME;
	error = switch_dev_register(&data->sdev);
	if (error)
		goto err_cleanup;
	data->switch_registered = true;
	error = tri_state_create_proc(data);
	if (error)
		goto err_cleanup;
	error = tri_state_request_irqs(data);
	if (error)
		goto err_cleanup;
	mod_timer(&data->timer, jiffies + 5 * HZ);
	return 0;

err_cleanup:
	tri_state_cleanup(data);
	platform_set_drvdata(pdev, NULL);
	kfree(data);
	dev_err(&pdev->dev, "probe failed: %d\n", error);
	return error;
}

static int tri_state_remove(struct platform_device *pdev)
{
	struct tri_state_key_data *data = platform_get_drvdata(pdev);

	tri_state_cleanup(data);
	platform_set_drvdata(pdev, NULL);
	kfree(data);
	return 0;
}

static const struct of_device_id tri_state_of_match[] = {
	{ .compatible = "oneplus,tri-state-key" },
	{ }
};
MODULE_DEVICE_TABLE(of, tri_state_of_match);

static struct platform_driver tri_state_driver = {
	.probe = tri_state_probe,
	.remove = tri_state_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = tri_state_of_match,
	},
};
module_platform_driver(tri_state_driver);

MODULE_DESCRIPTION("OnePlus tri-state alert slider driver");
MODULE_LICENSE("GPL");
