// SPDX-License-Identifier: GPL-2.0
/*
 * NXP PN547 NFC controller driver
 *
 * Copyright (C) 2010 Trusted Logic S.A.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "pn547.h"

#define FRAME_SIZE_COMPAT	(255 + 3)
#define FRAME_SIZE		(1023 + 5)
#define MAX_TRY_I2C_READ	10
#define I2C_ADDR_READ_L		0x51
#define I2C_ADDR_READ_H		0x57

struct pn547_dev {
	struct kref ref;
	struct i2c_client *client;
	struct miscdevice miscdev;
	struct mutex read_mutex;
	struct mutex io_mutex;
	wait_queue_head_t read_wq;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_inactive;
	struct clk *nfc_clk;
	enum pn547_state state;
	unsigned int irq_gpio;
	unsigned int ven_gpio;
	unsigned int firm_gpio;
	unsigned int clk_req_gpio;
	unsigned int pvdd_en_gpio;
	atomic_t irq_enabled;
	atomic_t read_flag;
	bool irq_wake_enabled;
	bool cancel_read;
	bool is_pn544;
	bool removed;
};

static void pn547_release(struct kref *ref)
{
	struct pn547_dev *dev = container_of(ref, struct pn547_dev, ref);

	kfree(dev->miscdev.name);
	mutex_destroy(&dev->io_mutex);
	mutex_destroy(&dev->read_mutex);
	kfree(dev);
}

static void pn547_irq_toggle(struct pn547_dev *dev, bool enable)
{
	int ret;

	if (atomic_read(&dev->irq_enabled) == enable)
		return;

	if (enable) {
		atomic_set(&dev->irq_enabled, 1);
		enable_irq(dev->client->irq);
		ret = enable_irq_wake(dev->client->irq);
		if (!ret)
			dev->irq_wake_enabled = true;
		else
			dev_warn(&dev->client->dev,
				 "failed to enable IRQ wake: %d\n", ret);
	} else {
		if (dev->irq_wake_enabled) {
			disable_irq_wake(dev->client->irq);
			dev->irq_wake_enabled = false;
		}
		disable_irq_nosync(dev->client->irq);
		atomic_set(&dev->irq_enabled, 0);
	}
}

static irqreturn_t pn547_irq_handler(int irq, void *data)
{
	struct pn547_dev *dev = data;

	atomic_set(&dev->read_flag, 1);
	wake_up_interruptible(&dev->read_wq);
	pm_wakeup_event(&dev->client->dev, 2000);

	return IRQ_HANDLED;
}

static ssize_t pn547_read(struct file *file, char __user *buf, size_t count,
			  loff_t *offset)
{
	struct pn547_dev *dev = file->private_data;
	bool fwdl;
	char *tmp;
	int ret = 0;
	int watchdog = 0;
	size_t maxlen;

	if (READ_ONCE(dev->removed))
		return -ENODEV;

	if (mutex_lock_interruptible(&dev->read_mutex))
		return -ERESTARTSYS;
	if (READ_ONCE(dev->removed)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	fwdl = READ_ONCE(dev->state) == PN547_STATE_FWDL;
	maxlen = fwdl ? FRAME_SIZE : FRAME_SIZE_COMPAT;
	count = min(count, maxlen);
	tmp = kzalloc(count, GFP_KERNEL);
	if (!tmp) {
		ret = -ENOMEM;
		goto out_unlock;
	}

wait_irq:
	if (!gpio_get_value(dev->irq_gpio)) {
		atomic_set(&dev->read_flag, 0);
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_free;
		}

		for (;;) {
			pn547_irq_toggle(dev, true);
			ret = wait_event_interruptible(dev->read_wq,
				atomic_read(&dev->read_flag) ||
				READ_ONCE(dev->removed));
			pn547_irq_toggle(dev, false);

			if (READ_ONCE(dev->removed)) {
				ret = -ENODEV;
				goto out_free;
			}
			if (dev->cancel_read) {
				dev->cancel_read = false;
				ret = -1;
				goto out_free;
			}
			if (ret)
				goto out_free;
			if (gpio_get_value(dev->irq_gpio))
				break;
		}
	}

	ret = i2c_master_recv(dev->client, tmp, count);
	if (dev->is_pn544 && ret > 0 && tmp[0] >= I2C_ADDR_READ_L &&
	    tmp[0] <= I2C_ADDR_READ_H && watchdog < MAX_TRY_I2C_READ) {
		dev_warn(&dev->client->dev,
			 "data not ready: 0x%x, retry %d\n", tmp[0], watchdog);
		usleep_range(2000, 2000);
		watchdog++;
		goto wait_irq;
	}
	if (ret < 0) {
		dev_err(&dev->client->dev, "I2C read failed: %d\n", ret);
		goto out_free;
	}
	if (ret > count) {
		ret = -EIO;
		goto out_free;
	}
	if (copy_to_user(buf, tmp, ret))
		ret = -EFAULT;

out_free:
	kfree(tmp);
out_unlock:
	mutex_unlock(&dev->read_mutex);
	return ret;
}

static ssize_t pn547_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *offset)
{
	struct pn547_dev *dev = file->private_data;
	char *tmp;
	int ret;
	int retry = 5;
	size_t maxlen;

	if (READ_ONCE(dev->removed))
		return -ENODEV;
	if (mutex_lock_interruptible(&dev->io_mutex))
		return -ERESTARTSYS;
	if (READ_ONCE(dev->removed)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	maxlen = READ_ONCE(dev->state) == PN547_STATE_FWDL ?
		 FRAME_SIZE : FRAME_SIZE_COMPAT;
	count = min(count, maxlen);
	tmp = memdup_user(buf, count);
	if (IS_ERR(tmp)) {
		ret = PTR_ERR(tmp);
		goto out_unlock;
	}

	do {
		ret = i2c_master_send(dev->client, tmp, count);
		if (ret == count)
			break;
		usleep_range(6000, 10000);
	} while (--retry);
	if (ret != count) {
		dev_err(&dev->client->dev, "I2C write failed: %d\n", ret);
		ret = -EIO;
	}
	kfree(tmp);

out_unlock:
	mutex_unlock(&dev->io_mutex);
	return ret;
}

static int pn547_open(struct inode *inode, struct file *file)
{
	struct pn547_dev *dev = container_of(file->private_data,
					     struct pn547_dev, miscdev);

	if (READ_ONCE(dev->removed))
		return -ENODEV;
	kref_get(&dev->ref);
	if (READ_ONCE(dev->removed)) {
		kref_put(&dev->ref, pn547_release);
		return -ENODEV;
	}
	file->private_data = dev;

	return 0;
}

static int pn547_release_file(struct inode *inode, struct file *file)
{
	struct pn547_dev *dev = file->private_data;

	kref_put(&dev->ref, pn547_release);
	return 0;
}

static long pn547_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct pn547_dev *dev = file->private_data;
	int ret;

	if (cmd != PN547_SET_PWR)
		return -EINVAL;
	if (READ_ONCE(dev->removed))
		return -ENODEV;
	if (mutex_lock_interruptible(&dev->io_mutex))
		return -ERESTARTSYS;
	if (READ_ONCE(dev->removed)) {
		ret = -ENODEV;
		goto out;
	}

	ret = pinctrl_select_state(dev->pinctrl, dev->pins_active);
	if (ret)
		dev_err(&dev->client->dev,
			"failed to select active pins: %d\n", ret);
	ret = 0;

	switch (arg) {
	case 2:
		gpio_set_value_cansleep(dev->ven_gpio, 1);
		gpio_set_value(dev->firm_gpio, 1);
		usleep_range(10000, 10050);
		gpio_set_value_cansleep(dev->ven_gpio, 0);
		usleep_range(10000, 10050);
		gpio_set_value_cansleep(dev->ven_gpio, 1);
		usleep_range(10000, 10050);
		dev->state = PN547_STATE_FWDL;
		break;
	case 1:
		gpio_set_value(dev->firm_gpio, 0);
		gpio_set_value_cansleep(dev->ven_gpio, 1);
		usleep_range(10000, 10050);
		dev->state = PN547_STATE_ON;
		break;
	case 0:
		gpio_set_value(dev->firm_gpio, 0);
		gpio_set_value_cansleep(dev->ven_gpio, 0);
		usleep_range(10000, 10050);
		dev->state = PN547_STATE_OFF;
		ret = pinctrl_select_state(dev->pinctrl, dev->pins_inactive);
		if (ret)
			dev_err(&dev->client->dev,
				"failed to select inactive pins: %d\n", ret);
		ret = 0;
		break;
	case 3:
		dev->cancel_read = true;
		atomic_set(&dev->read_flag, 1);
		wake_up_interruptible(&dev->read_wq);
		ret = 0;
		break;
	default:
		ret = -EINVAL;
		break;
	}

out:
	mutex_unlock(&dev->io_mutex);
	return ret;
}

static const struct file_operations pn547_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.read = pn547_read,
	.write = pn547_write,
	.open = pn547_open,
	.release = pn547_release_file,
	.unlocked_ioctl = pn547_ioctl,
};

static int pn547_get_gpio(struct device *dev, const char *property)
{
	int gpio;

	gpio = of_get_named_gpio(dev->of_node, property, 0);
	if (gpio == -EPROBE_DEFER)
		return gpio;
	if (!gpio_is_valid(gpio)) {
		dev_err(dev, "invalid %s GPIO: %d\n", property, gpio);
		return gpio < 0 ? gpio : -EINVAL;
	}

	return gpio;
}

static int pn547_detect_address(struct pn547_dev *dev)
{
	static const u8 probe[] = { 0x20, 0x00, 0x01, 0x01 };
	int addr;
	int tries;
	int ret = -ENODEV;

	gpio_set_value(dev->ven_gpio, 1);
	usleep_range(10000, 11000);
	for (addr = 0x28; addr <= 0x2b; addr++) {
		dev->client->addr = addr;
		for (tries = 0; tries < 3; tries++) {
			ret = i2c_master_send(dev->client, probe, sizeof(probe));
			if (ret == sizeof(probe))
				goto found;
			msleep(20);
		}
	}
	ret = ret < 0 ? ret : -ENODEV;

found:
	gpio_set_value(dev->ven_gpio, 0);
	return ret == sizeof(probe) ? 0 : ret;
}

static int pn547_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct pn547_dev *dev;
	const char *name = "pn54x";
	int gpio_irq;
	int ret;

	if (!client->dev.of_node)
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->irq <= 0)
		return client->irq ? client->irq : -EINVAL;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	kref_init(&dev->ref);
	dev->client = client;
	dev->state = PN547_STATE_UNKNOWN;
	mutex_init(&dev->read_mutex);
	mutex_init(&dev->io_mutex);
	init_waitqueue_head(&dev->read_wq);
	atomic_set(&dev->irq_enabled, 0);
	atomic_set(&dev->read_flag, 0);

	dev->irq_gpio = pn547_get_gpio(&client->dev, "nxp,irq_gpio");
	if ((int)dev->irq_gpio < 0) {
		ret = (int)dev->irq_gpio;
		goto err_put;
	}
	dev->ven_gpio = pn547_get_gpio(&client->dev, "nxp,ven");
	if ((int)dev->ven_gpio < 0) {
		ret = (int)dev->ven_gpio;
		goto err_put;
	}
	dev->firm_gpio = pn547_get_gpio(&client->dev, "nxp,dwld_en");
	if ((int)dev->firm_gpio < 0) {
		ret = (int)dev->firm_gpio;
		goto err_put;
	}
	dev->clk_req_gpio = pn547_get_gpio(&client->dev, "nxp,clk_req");
	if ((int)dev->clk_req_gpio < 0) {
		ret = (int)dev->clk_req_gpio;
		goto err_put;
	}
	dev->pvdd_en_gpio = pn547_get_gpio(&client->dev, "nxp,pvdd_en");
	if ((int)dev->pvdd_en_gpio < 0) {
		ret = (int)dev->pvdd_en_gpio;
		goto err_put;
	}
	dev->is_pn544 = of_property_read_bool(client->dev.of_node,
					      "nxp,use_pn544");

	gpio_irq = gpio_to_irq(dev->irq_gpio);
	if (gpio_irq < 0) {
		ret = gpio_irq;
		goto err_put;
	}
	if (gpio_irq != client->irq) {
		dev_err(&client->dev, "IRQ GPIO mismatch: %d != %d\n",
			gpio_irq, client->irq);
		ret = -EINVAL;
		goto err_put;
	}

	ret = gpio_request(dev->irq_gpio, "nfc_int");
	if (ret)
		goto err_put;
	ret = gpio_request(dev->ven_gpio, "nfc_ven");
	if (ret)
		goto err_free_irq_gpio;
	ret = gpio_request(dev->firm_gpio, "nfc_firm");
	if (ret)
		goto err_free_ven;
	ret = gpio_request(dev->clk_req_gpio, "nfc_clk_req");
	if (ret)
		goto err_free_firm;
	ret = gpio_request(dev->pvdd_en_gpio, "nfc_pvdd_en");
	if (ret)
		goto err_free_clk_req;

	ret = gpio_direction_input(dev->irq_gpio);
	if (ret)
		goto err_free_pvdd;
	ret = gpio_direction_output(dev->ven_gpio, 0);
	if (ret)
		goto err_free_pvdd;
	ret = gpio_direction_output(dev->firm_gpio, 0);
	if (ret)
		goto err_free_pvdd;
	ret = gpio_direction_input(dev->clk_req_gpio);
	if (ret)
		goto err_free_pvdd;
	ret = gpio_direction_output(dev->pvdd_en_gpio, 0);
	if (ret)
		goto err_free_pvdd;

	dev->pinctrl = pinctrl_get(&client->dev);
	if (IS_ERR(dev->pinctrl)) {
		ret = PTR_ERR(dev->pinctrl);
		goto err_free_pvdd;
	}
	dev->pins_active = pinctrl_lookup_state(dev->pinctrl, "pn547-active");
	if (IS_ERR(dev->pins_active)) {
		ret = PTR_ERR(dev->pins_active);
		goto err_put_pinctrl;
	}
	dev->pins_inactive = pinctrl_lookup_state(dev->pinctrl,
						  "pn547-inactive");
	if (IS_ERR(dev->pins_inactive)) {
		ret = PTR_ERR(dev->pins_inactive);
		goto err_put_pinctrl;
	}
	ret = pinctrl_select_state(dev->pinctrl, dev->pins_inactive);
	if (ret)
		goto err_put_pinctrl;

	dev->nfc_clk = clk_get(&client->dev, "nfc_clk");
	if (IS_ERR(dev->nfc_clk)) {
		ret = PTR_ERR(dev->nfc_clk);
		goto err_put_pinctrl;
	}
	ret = clk_prepare_enable(dev->nfc_clk);
	if (ret)
		goto err_put_clk;

	ret = request_irq(client->irq, pn547_irq_handler, IRQF_TRIGGER_RISING,
			  "pn547", dev);
	if (ret)
		goto err_disable_clk;
	disable_irq(client->irq);

	ret = pn547_detect_address(dev);
	if (ret)
		goto err_free_irq;

	if (!of_property_read_string(client->dev.of_node, "nxp,dev_name",
				     &name))
		dev_dbg(&client->dev, "using misc device name %s\n", name);
	dev->miscdev.name = kstrdup(name, GFP_KERNEL);
	if (!dev->miscdev.name) {
		ret = -ENOMEM;
		goto err_free_irq;
	}
	dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	dev->miscdev.fops = &pn547_fops;
	dev->miscdev.parent = &client->dev;
	ret = misc_register(&dev->miscdev);
	if (ret)
		goto err_free_name;

	i2c_set_clientdata(client, dev);
	ret = device_init_wakeup(&client->dev, true);
	if (ret)
		goto err_misc;

	return 0;

err_misc:
	misc_deregister(&dev->miscdev);
err_free_name:
	kfree(dev->miscdev.name);
	dev->miscdev.name = NULL;
err_free_irq:
	free_irq(client->irq, dev);
err_disable_clk:
	clk_disable_unprepare(dev->nfc_clk);
err_put_clk:
	clk_put(dev->nfc_clk);
err_put_pinctrl:
	pinctrl_put(dev->pinctrl);
err_free_pvdd:
	gpio_free(dev->pvdd_en_gpio);
err_free_clk_req:
	gpio_free(dev->clk_req_gpio);
err_free_firm:
	gpio_free(dev->firm_gpio);
err_free_ven:
	gpio_free(dev->ven_gpio);
err_free_irq_gpio:
	gpio_free(dev->irq_gpio);
err_put:
	kref_put(&dev->ref, pn547_release);
	return ret;
}

static int pn547_remove(struct i2c_client *client)
{
	struct pn547_dev *dev = i2c_get_clientdata(client);
	int ret;

	WRITE_ONCE(dev->removed, true);
	misc_deregister(&dev->miscdev);
	atomic_set(&dev->read_flag, 1);
	wake_up_interruptible(&dev->read_wq);

	mutex_lock(&dev->read_mutex);
	mutex_lock(&dev->io_mutex);
	pn547_irq_toggle(dev, false);
	free_irq(client->irq, dev);
	device_init_wakeup(&client->dev, false);
	gpio_set_value(dev->firm_gpio, 0);
	gpio_set_value_cansleep(dev->ven_gpio, 0);
	ret = pinctrl_select_state(dev->pinctrl, dev->pins_inactive);
	if (ret)
		dev_warn(&client->dev, "failed to select inactive pins: %d\n",
			 ret);
	clk_disable_unprepare(dev->nfc_clk);
	clk_put(dev->nfc_clk);
	pinctrl_put(dev->pinctrl);
	gpio_free(dev->pvdd_en_gpio);
	gpio_free(dev->clk_req_gpio);
	gpio_free(dev->firm_gpio);
	gpio_free(dev->ven_gpio);
	gpio_free(dev->irq_gpio);
	mutex_unlock(&dev->io_mutex);
	mutex_unlock(&dev->read_mutex);
	i2c_set_clientdata(client, NULL);
	kref_put(&dev->ref, pn547_release);

	return 0;
}

static const struct i2c_device_id pn547_id[] = {
	{ "pn547", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pn547_id);

static const struct of_device_id pn547_of_match[] = {
	{ .compatible = "nxp,pn547" },
	{ }
};
MODULE_DEVICE_TABLE(of, pn547_of_match);

static struct i2c_driver pn547_driver = {
	.probe = pn547_probe,
	.remove = pn547_remove,
	.id_table = pn547_id,
	.driver = {
		.name = "pn547",
		.of_match_table = pn547_of_match,
	},
};
module_i2c_driver(pn547_driver);

MODULE_AUTHOR("Sylvain Fonteneau");
MODULE_DESCRIPTION("NFC PN547 driver");
MODULE_LICENSE("GPL v2");
