/****************************************************
 **Description:fastchg update firmware and driver
 *****************************************************/
#define pr_fmt(fmt) "FASTCHG: %s: " fmt, __func__

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/power_supply.h>
#include <linux/interrupt.h>
#include <linux/power/oem_external_fg.h>
#include <linux/clk.h>
#include <linux/kref.h>
#include <linux/of_clk.h>
#include <linux/pm_wakeup.h>
#include <linux/sizes.h>
#include <dt-bindings/clock/qcom,rpmcc.h>

#define BYTE_OFFSET			2
#define BYTES_TO_WRITE		16
#define ERASE_COUNT		384	/* 0x8800-0x9FFF */
#define READ_COUNT			192
#define	FW_CHECK_FAIL		0
#define	FW_CHECK_SUCCESS	1

#define SHOW_FW_VERSION_DELAY_MS 18000
struct fastchg_device_info {
	struct i2c_client		*client;
	struct miscdevice   dash_device;
	struct mutex        read_mutex;
	struct mutex	 firmware_lock;
	struct mutex	 update_lock;
	wait_queue_head_t   read_wq;
	struct kref	 refcount;
	bool		 removed;
	bool		 misc_registered;
	bool		 callbacks_registered;

	struct pinctrl_state *pinctrl_state_active;
	struct pinctrl_state *pinctrl_state_suspended;
	struct pinctrl_state *pinctrl_mcu_data_state_active;
	struct pinctrl_state *pinctrl_mcu_data_state_suspended;
	struct pinctrl *pinctrl;
	bool fast_chg_started;
	bool fast_low_temp_full;
	bool fast_chg_ing;
	bool fast_switch_to_normal;
	bool fast_normal_to_warm;
	bool irq_enabled;
	bool irq_requested;
	bool usb_sw_1_requested;
	bool usb_sw_2_requested;
	bool ap_clk_requested;
	bool ap_data_requested;
	bool mcu_en_requested;
	bool fast_chg_allow;
	bool firmware_already_updated;
	int adapter_update_report;
	int adapter_update_real;
	int battery_type;
	int irq;
	int mcu_en_gpio;
	int usb_sw_1_gpio;
	int usb_sw_2_gpio;
	int ap_clk;
	int ap_data;
	int dashchg_fw_ver_count;
	unsigned char *firmware_data;
	size_t firmware_size;
	struct clk *snoc_clk;
	struct clk *cnoc_clk;
	struct op_adapter_chip *adapter_chip;
	struct power_supply *smb_psy;

	struct power_supply		*batt_psy;
	struct work_struct fastcg_work;
	struct work_struct charger_present_status_work;
	struct timer_list watchdog;
	struct wakeup_source *fastchg_wake_lock;
	struct wakeup_source *fastchg_update_fireware_lock;

	struct delayed_work		update_firmware;
	struct delayed_work update_fireware_version_work;
	struct delayed_work adapter_update_work;
	char fw_id[12];
	char manu_name[12];
};

static struct op_adapter_chip *adapter_chip __ro_after_init;

static void dash_dev_release_ref(struct kref *ref)
{
	struct fastchg_device_info *di = container_of(ref,
			struct fastchg_device_info, refcount);

	kfree(di);
}

void opchg_set_data_active(struct fastchg_device_info *chip)
{
	gpio_direction_input(chip->ap_data);
	pinctrl_select_state(chip->pinctrl,
		chip->pinctrl_mcu_data_state_active);
}

static void mcu_en_reset(struct fastchg_device_info *di)
{
	if (gpio_is_valid(di->mcu_en_gpio))
		gpio_direction_output(di->mcu_en_gpio, 1);
}

static void mcu_en_gpio_set(struct fastchg_device_info *di, int value)
{
	if (value) {
		if (gpio_is_valid(di->mcu_en_gpio))
			gpio_direction_output(di->mcu_en_gpio, 0);
	} else {
		if (gpio_is_valid(di->mcu_en_gpio)) {
			gpio_direction_output(di->mcu_en_gpio, 1);
			usleep_range(10000, 10001);
			gpio_direction_output(di->mcu_en_gpio, 0);
		}
	}
}
#define ADAPTER_UPDATE_DELAY              1400

static void usb_sw_gpio_set(struct fastchg_device_info *di, int value)
{
	pr_info("set usb_sw_gpio=%d\n", value);
	if (!gpio_is_valid(di->usb_sw_1_gpio)
		&& !gpio_is_valid(di->usb_sw_2_gpio)) {
		pr_err("gpio is invalid\n");
		return;
	}

	if (value) {
		gpio_direction_output(di->usb_sw_1_gpio, 1);
		gpio_direction_output(di->usb_sw_2_gpio, 1);
	} else {
		gpio_direction_output(di->usb_sw_1_gpio, 0);
		gpio_direction_output(di->usb_sw_2_gpio, 0);
	}
	di->fast_chg_allow = value;
	/* david@bsp add log */
	pr_info("get usb_sw_gpio=%d&%d\n"
		, gpio_get_value(di->usb_sw_1_gpio)
		, gpio_get_value(di->usb_sw_2_gpio));
}

static int set_property_on_smbcharger(
	struct fastchg_device_info *di, enum power_supply_property prop, bool data)
{
	union power_supply_propval value = {data, };
	int ret;

	if (!di->smb_psy) {
		di->smb_psy = power_supply_get_by_name("battery");
		if (!di->smb_psy) {
			pr_err("failed to get ps battery\n");
			return -EINVAL;
		}
	}
	ret = power_supply_set_property(di->smb_psy, prop, &value);
	/* david@bsp modified */
	if (ret)
		return -EINVAL;

	return 0;
}


static int oneplus_dash_i2c_read(
	struct i2c_client *client, u8 addr, s32 len, u8 *rxbuf)
{
	return i2c_smbus_read_i2c_block_data(client, addr, len, rxbuf);
}

static int oneplus_dash_i2c_write(
	struct i2c_client *client, u8 addr, s32 len, u8 *txbuf)
{
	return i2c_smbus_write_i2c_block_data(client, addr, len, txbuf);
}

static bool dashchg_fw_check(struct fastchg_device_info *di)
{
	unsigned char addr_buf[2] = {0x88, 0x00};
	unsigned char data_buf[32] = {0x0};
	int rc, i, j, addr;
	int fw_line = 0;
	int fw_records = di->firmware_size / 34;

	if (!fw_records)
		return FW_CHECK_FAIL;
	rc = oneplus_dash_i2c_write(di->client, 0x01, 2, &addr_buf[0]);
	if (rc < 0) {
		pr_err("%s i2c_write 0x01 error\n", __func__);
		goto i2c_err;
	}

	usleep_range(2000, 2001);
	for (i = 0; i < READ_COUNT; i++) {
		rc = oneplus_dash_i2c_read(di->client, 0x03, 16,
			&data_buf[0]);
		if (rc < 0)
			goto i2c_err;
		usleep_range(2000, 2001);
		rc = oneplus_dash_i2c_read(di->client, 0x03, 16,
			&data_buf[16]);
		if (rc < 0)
			goto i2c_err;
		addr = 0x8800 + i * 32;

		/* compare recv_buf with dashchg_firmware_data[] begin */
		if (fw_line < fw_records &&
			addr == ((di->firmware_data[fw_line * 34 + 1] << 8)
			| di->firmware_data[fw_line * 34])) {
			for (j = 0; j < 32; j++) {
			if (data_buf[j] != di->firmware_data[fw_line * 34 + 2 + j]) {
				pr_info("%s fail,data_buf[%d]:0x%x!=dashchg_firmware_data[%d]:0x%x\n",
				__func__, j, data_buf[j],
				(fw_line * 34 + 2 + j),
				di->firmware_data[fw_line * 34 + 2 + j]);
				pr_info("%s addr = 0x%x", __func__, addr);
				for (j = 0; j <= 31; j++)
					pr_info("%x\n", data_buf[j]);
				return FW_CHECK_FAIL;
				}
			}
			fw_line++;
		} else {
	/*pr_err("%s addr dismatch,addr:0x%x,stm_data:0x%x\n",__func__,*/
	/*addr,(dashchg_firmware_data[fw_line * 34 + 1] << 8) | */
	/*dashchg_firmware_data[fw_line * 34]);*/
		}
		/* compare recv_buf with dashchg_firmware_data[] end */
	}
	if (fw_line != fw_records)
		return FW_CHECK_FAIL;
	pr_info("result=success\n");
	return FW_CHECK_SUCCESS;
i2c_err:
	pr_err("result=fail\n");
	return FW_CHECK_FAIL;
}

static int dashchg_fw_write(struct fastchg_device_info *di,
	unsigned char *data_buf,
	unsigned int offset, unsigned int length)
{
	unsigned int count = 0;
	unsigned char zero_buf[1] = {0};
	unsigned char temp_buf[1] = {0};
	unsigned char addr_buf[2] = {0x88, 0x00};
	int rc;

	if (!data_buf || !length || offset % 34 || length % 34 ||
		offset > di->firmware_size ||
		length > di->firmware_size - offset)
		return -EINVAL;

	count = offset;
	/* write data begin */
	while (count < (offset + length)) {
		addr_buf[0] = data_buf[count + 1];
		addr_buf[1] = data_buf[count];

		rc = oneplus_dash_i2c_write(di->client, 0x01, 2, &addr_buf[0]);
		if (rc < 0) {
			pr_err("i2c_write 0x01 error\n");
			return -EFAULT;
		}

		/* write 16 bytes data to dashchg */
		oneplus_dash_i2c_write(di->client,
		0x02, BYTES_TO_WRITE, &data_buf[count+BYTE_OFFSET]);
		oneplus_dash_i2c_write(di->client, 0x05, 1, &zero_buf[0]);
		oneplus_dash_i2c_read(di->client, 0x05, 1, &temp_buf[0]);

		/* write 16 bytes data to dashchg again */
		oneplus_dash_i2c_write(di->client,
		0x02, BYTES_TO_WRITE,
		&data_buf[count+BYTE_OFFSET+BYTES_TO_WRITE]);
		oneplus_dash_i2c_write(di->client, 0x05, 1, &zero_buf[0]);
		oneplus_dash_i2c_read(di->client, 0x05, 1, &temp_buf[0]);

		count = count + BYTE_OFFSET + 2 * BYTES_TO_WRITE;

		usleep_range(2000, 2001);
		if (count > (offset + length - 1))
			break;
	}
	return 0;
}

static irqreturn_t irq_rx_handler(int irq, void *dev_id);
static void reset_mcu_and_request_irq(struct fastchg_device_info *di)
{
	int ret;

	pr_info("\n");
	gpio_direction_output(di->ap_clk, 1);
	usleep_range(10000, 10001);
	gpio_direction_output(di->mcu_en_gpio, 1);
	usleep_range(10000, 10001);
	gpio_direction_output(di->mcu_en_gpio, 0);
	usleep_range(5000, 5001);
	opchg_set_data_active(di);
	di->irq = gpio_to_irq(di->ap_data);

	/* 0x01:rising edge, 0x02:falling edge */
	if (di->irq_requested) {
		if (!di->irq_enabled) {
			enable_irq(di->irq);
			di->irq_enabled = true;
		}
		return;
	}
	ret = request_irq(di->irq, irq_rx_handler,
			IRQF_TRIGGER_RISING, "mcu_data", di);
	if (ret < 0)
		pr_err("request ap rx irq failed.\n");
	else {
		di->irq_requested = true;
		di->irq_enabled = true;
	}
}


static void dashchg_fw_update(struct work_struct *work)
{
	unsigned char zero_buf[1] = {0};
	unsigned char addr_buf[2] = {0x88, 0x00};
	unsigned char temp_buf[1] = {0};
	int i, rc = 0;
	unsigned int addr = 0x8800;
	int download_again = 0;
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			update_firmware.work);

	mutex_lock(&di->update_lock);
	mutex_lock(&di->firmware_lock);
	if (di->removed || !di->firmware_data) {
		mutex_unlock(&di->firmware_lock);
		mutex_unlock(&di->update_lock);
		return;
	}
	__pm_stay_awake(di->fastchg_update_fireware_lock);
	rc = dashchg_fw_check(di);
	if (rc == FW_CHECK_SUCCESS) {
		di->firmware_already_updated = true;
		reset_mcu_and_request_irq(di);
		__pm_relax(di->fastchg_update_fireware_lock);
		mutex_unlock(&di->firmware_lock);
		mutex_unlock(&di->update_lock);
		set_property_on_smbcharger(di, POWER_SUPPLY_PROP_SWITCH_DASH, true);
		pr_info("FW check success\n"); /* david@bsp add log */
		return;
	}
	pr_info("start erasing data.......\n");

update_fw:
	/* erase address 0x200-0x7FF */
	for (i = 0; i < ERASE_COUNT; i++) {
		/* first:set address */
		rc = oneplus_dash_i2c_write(di->client, 0x01, 2, &addr_buf[0]);
		if (rc < 0) {
			pr_err("dashchg_update_fw, i2c_write 0x01 error\n");
			goto update_fw_err;
		}

		/* erase data:0x10 words once */
		oneplus_dash_i2c_write(di->client, 0x04, 1, &zero_buf[0]);
		usleep_range(1000, 1001);
		oneplus_dash_i2c_read(di->client, 0x04, 1, &temp_buf[0]);

		/* erase data:0x10 words once */
		addr = addr + 0x10;
		addr_buf[0] = addr >> 8;
		addr_buf[1] = addr & 0xFF;
	}
	usleep_range(10000, 10001);
	dashchg_fw_write(di, di->firmware_data, 0, di->firmware_size);

	/* fw check begin:read data from mcu and compare*/
	/*it with dashchg_firmware_data[] */
	rc = dashchg_fw_check(di);
	if (rc == FW_CHECK_FAIL) {
		download_again++;
		if (download_again > 3)
			goto update_fw_err;
		mcu_en_gpio_set(di, 0);
		msleep(1000);
		pr_err("fw check fail, download fw again\n");
		goto update_fw;
	}
	/* fw check end */

	usleep_range(2000, 2001);
	/* jump to app code begin */
	oneplus_dash_i2c_write(di->client, 0x06, 1, &zero_buf[0]);
	oneplus_dash_i2c_read(di->client, 0x06, 1, &temp_buf[0]);
	/* jump to app code end */
	di->firmware_already_updated = true;
	reset_mcu_and_request_irq(di);
	__pm_relax(di->fastchg_update_fireware_lock);
	mutex_unlock(&di->firmware_lock);
	mutex_unlock(&di->update_lock);
	set_property_on_smbcharger(di, POWER_SUPPLY_PROP_SWITCH_DASH, true);
	pr_info("result=success\n");
	return;

update_fw_err:
	di->firmware_already_updated = false;
	reset_mcu_and_request_irq(di);
	__pm_relax(di->fastchg_update_fireware_lock);
	mutex_unlock(&di->firmware_lock);
	mutex_unlock(&di->update_lock);
	set_property_on_smbcharger(di, POWER_SUPPLY_PROP_SWITCH_DASH, true);
	pr_err("result=fail\n");
}

static bool bq27541_fast_chg_started(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->fast_chg_started;
}

static bool bq27541_get_fast_low_temp_full(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->fast_low_temp_full;
}

static int bq27541_set_fast_chg_allow(void *ctx, bool enable)
{
	((struct fastchg_device_info *)ctx)->fast_chg_allow = enable;
	return 0;
}

static bool bq27541_get_fast_chg_allow(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->fast_chg_allow;
}

static bool bq27541_fast_switch_to_normal(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->fast_switch_to_normal;
}

static bool bq27541_get_fast_chg_ing(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->fast_chg_ing;
}

static int bq27541_set_switch_to_noraml_false(void *ctx)
{
	((struct fastchg_device_info *)ctx)->fast_switch_to_normal = false;
	return 0;
}

static bool get_fastchg_firmware_already_updated(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->firmware_already_updated;
}

static bool fastchg_is_usb_switch_on(void *ctx)
{
	struct fastchg_device_info *di = ctx;

	return gpio_get_value(di->usb_sw_1_gpio);
}

static int dash_get_adapter_update_status(void *ctx)
{
	return ((struct fastchg_device_info *)ctx)->adapter_update_report;
}

static int fastchg_set_usb_switch(void *ctx, bool enable)
{
	struct fastchg_device_info *di = ctx;

	if (READ_ONCE(di->removed))
		return -ENODEV;
	if (enable) {
		mcu_en_reset(di);
		usleep_range(10000, 10002);
		usb_sw_gpio_set(di, 1);
		usleep_range(10000, 10002);
		mcu_en_gpio_set(di, 1);
	} else {
		usb_sw_gpio_set(di, 0);
		mcu_en_reset(di);
	}
	return 0;
}

static int fastchg_reset_mcu(void *ctx)
{
	struct fastchg_device_info *di = ctx;

	if (READ_ONCE(di->removed))
		return -ENODEV;
	mcu_en_gpio_set(di, 0);
	return 0;
}

static const struct oem_battery_gauge_ops fastcharge_information_ops = {
	.fast_chg_started =
		bq27541_fast_chg_started,
	.get_fast_low_temp_full =
		bq27541_get_fast_low_temp_full,
	.fast_switch_to_normal =
		bq27541_fast_switch_to_normal,
	.get_fast_chg_ing =
		bq27541_get_fast_chg_ing,
	.set_fast_chg_allow =
		bq27541_set_fast_chg_allow,
	.get_fast_chg_allow =
		bq27541_get_fast_chg_allow,
	.set_switch_to_noraml_false =
		bq27541_set_switch_to_noraml_false,
	.get_fastchg_firmware_already_updated =
		get_fastchg_firmware_already_updated,
	.is_usb_switch_on = fastchg_is_usb_switch_on,
	.get_adapter_update = dash_get_adapter_update_status,
	.set_usb_switch = fastchg_set_usb_switch,
	.reset_mcu = fastchg_reset_mcu,
};

static void mcu_init(struct fastchg_device_info *di)
{
	gpio_direction_output(di->ap_clk, 0);
	usleep_range(1000, 1001);
	gpio_direction_output(di->mcu_en_gpio, 1);
	usleep_range(1000, 1001);
	gpio_direction_output(di->mcu_en_gpio, 0);
}

static irqreturn_t irq_rx_handler(int irq, void *dev_id)
{
	struct fastchg_device_info *di = dev_id;

	if (READ_ONCE(di->removed))
		return IRQ_HANDLED;
	pr_debug("triggered\n");
	schedule_work(&di->fastcg_work);
	return IRQ_HANDLED;
}

static void oneplus_notify_dash_charger_present(bool status)
{
	oem_dash_notify_charger_present(status);
}

static void oneplus_notify_pmic_check_charger_present(void)
{
	oem_dash_notify_event();
}

static void notify_check_usb_suspend(bool status, bool check_power_ok)
{
	oem_dash_op_contrl(status, check_power_ok);
}

static void update_charger_present_status(struct work_struct *work)
{
	notify_check_usb_suspend(true, true);
	oneplus_notify_dash_charger_present(false);
	oneplus_notify_pmic_check_charger_present();
}

static int op_get_device_type(void)
{
	return oem_bq_get_device_type(0);
}

static int onplus_get_battery_mvolts(void)
{
	return oem_bq_get_battery_mvolts(4010 * 1000);
}

static int onplus_get_battery_temperature(void)
{
	return oem_bq_get_battery_temperature(255);
}

static int onplus_get_batt_remaining_capacity(void)
{
	return oem_bq_get_batt_remaining_capacity(5);
}

static int onplus_get_battery_soc(void)
{
	return oem_bq_get_battery_soc(50);
}

static int onplus_get_average_current(void)
{
	return oem_bq_get_average_current(666 * 1000);
}

static void switch_mode_to_normal(struct fastchg_device_info *di)
{
	usb_sw_gpio_set(di, 0);
	mcu_en_gpio_set(di, 1);
}

static void update_fast_chg_started(struct fastchg_device_info *di)
{
	oem_external_fg_fast_chg_started_status(di->fast_chg_started);
}

static void request_mcu_irq(struct fastchg_device_info *di)
{
	int retval;

	opchg_set_data_active(di);
	gpio_set_value(di->ap_clk, 0);
	usleep_range(10000, 10001);
	gpio_set_value(di->ap_clk, 1);
	if (di->adapter_update_real
		!= ADAPTER_FW_NEED_UPDATE) {
		pr_info("%s\n", __func__);
	if (!di->irq_requested) {
		retval = request_irq(di->irq, irq_rx_handler,
				IRQF_TRIGGER_RISING, "mcu_data", di);
		if (retval < 0)
			pr_err("request ap rx irq failed.\n");
		else {
			di->irq_requested = true;
			di->irq_enabled = true;
		}
	} else if (!di->irq_enabled) {
		enable_irq(di->irq);
		di->irq_enabled = true;
	}
	} else {
		di->irq_enabled = true;
}
}
static void fastcg_work_func(struct work_struct *work)
{
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			fastcg_work);
	pr_info("\n");
	if (di->irq_enabled) {
		disable_irq_nosync(di->irq);
		msleep(25);
		di->irq_enabled = false;
		wake_up(&di->read_wq);
	}
}

static void update_fireware_version_func(struct work_struct *work)
{
	struct fastchg_device_info *di = container_of(work,
			struct fastchg_device_info,
			update_fireware_version_work.work);

	if (!di->firmware_data || di->firmware_size < 4)
		return;

	snprintf(di->fw_id, 255, "0x%x",
	di->firmware_data[di->firmware_size - 4]);
	snprintf(di->manu_name, 255, "%s", "ONEPLUS");
	//push_component_info(FAST_CHARGE, di->fw_id, di->manu_name);
}
static void di_watchdog(struct timer_list *timer)
{
	struct fastchg_device_info *di = from_timer(di, timer, watchdog);

	pr_err("di_watchdog can't receive mcu data\n");
	oem_external_fg_set_allow_reading(true);
	di->fast_chg_started = false;
	di->fast_switch_to_normal = false;
	di->fast_low_temp_full = false;
	di->fast_chg_allow = false;
	di->fast_normal_to_warm = false;
	di->fast_chg_ing = false;

	/* switch off fast chg */
	switch_mode_to_normal(di);
	schedule_work(&di->charger_present_status_work);
	pr_err("switch off fastchg\n");

	__pm_relax(di->fastchg_wake_lock);
}

#define MAX_BUFFER_SIZE 1024
#define ALLOW_DATA 0x2
#define REJECT_DATA 0x11
static void dash_write(struct fastchg_device_info *di, int data)
{
	int i;
	int device_type = op_get_device_type();

	usleep_range(2000, 2001);
	gpio_direction_output(di->ap_data, 0);
	pinctrl_select_state(di->pinctrl,
		di->pinctrl_mcu_data_state_suspended);
	for (i = 0; i < 3; i++) {
		if (i == 0)
			gpio_set_value(di->ap_data, data >> 1);
		else if (i == 1)
			gpio_set_value(di->ap_data, data & 0x1);
		else
			gpio_set_value(di->ap_data, device_type);
		gpio_set_value(di->ap_clk, 0);
		usleep_range(1000, 1001);
		gpio_set_value(di->ap_clk, 1);
		usleep_range(19000, 19001);
	}
}

static int dash_read(struct fastchg_device_info *di)
{
	int i;
	int bit = 0;
	int data = 0;

	for (i = 0; i < 7; i++) {
		gpio_set_value(di->ap_clk, 0);
		usleep_range(1000, 1001);
		gpio_set_value(di->ap_clk, 1);
		usleep_range(19000, 19001);
		bit = gpio_get_value(di->ap_data);
		data |= bit<<(6-i);
	}
	pr_err("recv data:0x%x\n", data);
	return data;
}

static int dash_dev_open(struct inode *inode, struct file *filp)
{
	struct fastchg_device_info *dash_dev = container_of(filp->private_data,
			struct fastchg_device_info, dash_device);

	if (!kref_get_unless_zero(&dash_dev->refcount))
		return -ENODEV;
	if (READ_ONCE(dash_dev->removed)) {
		kref_put(&dash_dev->refcount, dash_dev_release_ref);
		return -ENODEV;
	}
	filp->private_data = dash_dev;
	pr_debug("%d,%d\n", imajor(inode), iminor(inode));
	return 0;
}

static int dash_dev_release(struct inode *inode, struct file *filp)
{
	struct fastchg_device_info *di = filp->private_data;

	kref_put(&di->refcount, dash_dev_release_ref);
	return 0;
}

static ssize_t dash_dev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *offset)
{
	struct fastchg_device_info *di = filp->private_data;

	int data;
	int ret = 0;

	if (!count)
		return 0;

	mutex_lock(&di->read_mutex);
	while (1) {
		ret = wait_event_interruptible(di->read_wq,
				!di->irq_enabled || READ_ONCE(di->removed));
		if (ret)
			goto fail;
		if (READ_ONCE(di->removed)) {
			ret = -ENODEV;
			goto fail;
		}
		if (di->irq_enabled)
			pr_err("dash false wakeup,ret=%d\n", ret);
		data = dash_read(di);
		mutex_unlock(&di->read_mutex);
		if (copy_to_user(buf, &data, 1)) {
			pr_err("failed to copy to user space\n");
			return -EFAULT;
		}
		break;
	}
	return ret;
fail:
	mutex_unlock(&di->read_mutex);
	return ret;
}
static void adapter_update_work_func(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fastchg_device_info *chip =
		container_of(dwork,
		struct fastchg_device_info, adapter_update_work);
	bool update_result = false;
	int i = 0;

	mutex_lock(&chip->update_lock);
	if (chip->removed || !chip->adapter_chip) {
		pr_info("%s g_adapter_chip NULL\n", __func__);
		mutex_unlock(&chip->update_lock);
		return;
	}
	if (!chip->adapter_chip->vops ||
		!chip->adapter_chip->vops->adapter_update) {
		__pm_relax(chip->fastchg_wake_lock);
		mutex_unlock(&chip->update_lock);
		return;
	}
	pr_info("%s begin\n", __func__);
	opchg_set_data_active(chip);
	if (clk_set_rate(chip->snoc_clk, 200000000) ||
			clk_prepare_enable(chip->snoc_clk))
		goto out_unlock;
	if (clk_set_rate(chip->cnoc_clk, 75000000) ||
			clk_prepare_enable(chip->cnoc_clk))
		goto disable_snoc;
	msleep(1000);
	for (i = 0; i < 3; i++) {
		update_result =
			chip->adapter_chip->vops->adapter_update(chip->adapter_chip,
			chip->ap_clk, chip->ap_data);
		if (update_result == true)
			break;
		if (i < 1)
			msleep(1650);
	}
	msleep(5000);
	if (update_result) {
		chip->adapter_update_real = ADAPTER_FW_UPDATE_SUCCESS;
	} else {
		chip->adapter_update_real = ADAPTER_FW_UPDATE_FAIL;
		chip->adapter_update_report = chip->adapter_update_real;
	}
	msleep(20);
	mcu_en_gpio_set(chip, 1);
	chip->fast_chg_started = false;
	chip->fast_chg_allow = false;
	chip->fast_chg_ing = false;
	msleep(1000);
	if (update_result) {
		msleep(2000);
		chip->adapter_update_report = ADAPTER_FW_UPDATE_SUCCESS;
	}
	notify_check_usb_suspend(true, false);
	oneplus_notify_pmic_check_charger_present();
	oneplus_notify_dash_charger_present(false);
	reset_mcu_and_request_irq(chip);
	//pm_qos_update_request(&big_cpu_update_freq, MIN_CPUFREQ);
	clk_disable_unprepare(chip->cnoc_clk);
	clk_disable_unprepare(chip->snoc_clk);

	pr_info("%s end update_result:%d\n",
		__func__, update_result);
	__pm_relax(chip->fastchg_wake_lock);
	mutex_unlock(&chip->update_lock);
	return;

disable_snoc:
	clk_disable_unprepare(chip->snoc_clk);
out_unlock:
	__pm_relax(chip->fastchg_wake_lock);
	mutex_unlock(&chip->update_lock);
}

static void dash_adapter_update(struct fastchg_device_info *chip)
{
	pr_err("%s\n", __func__);
	/*schedule_delayed_work_on(5,*/
	/*&chip->adapter_update_work,*/
	/*round_jiffies_relative(*/
	/*msecs_to_jiffies(ADAPTER_UPDATE_DELAY)));*/
	schedule_delayed_work(&chip->adapter_update_work,
			msecs_to_jiffies(ADAPTER_UPDATE_DELAY));
}
void op_adapter_init(struct op_adapter_chip *chip)
{
	adapter_chip = chip;
}

#define DASH_IOC_MAGIC					0xff
#define DASH_NOTIFY_FIRMWARE_UPDATE		_IO(DASH_IOC_MAGIC, 1)
#define DASH_NOTIFY_FAST_PRESENT		_IOW(DASH_IOC_MAGIC, 2, int)
#define DASH_NOTIFY_FAST_ABSENT			_IOW(DASH_IOC_MAGIC, 3, int)
#define DASH_NOTIFY_NORMAL_TEMP_FULL	_IOW(DASH_IOC_MAGIC, 4, int)
#define DASH_NOTIFY_LOW_TEMP_FULL		_IOW(DASH_IOC_MAGIC, 5, int)
#define DASH_NOTIFY_BAD_CONNECTED		_IOW(DASH_IOC_MAGIC, 6, int)
#define DASH_NOTIFY_TEMP_OVER			_IOW(DASH_IOC_MAGIC, 7, int)
#define DASH_NOTIFY_ADAPTER_FW_UPDATE	_IOW(DASH_IOC_MAGIC, 8, int)
#define DASH_NOTIFY_BTB_TEMP_OVER		_IOW(DASH_IOC_MAGIC, 9, int)
#define DASH_NOTIFY_ALLOW_READING_IIC	_IOW(DASH_IOC_MAGIC, 10, int)
#define DASH_NOTIFY_UNDEFINED_CMD		_IO(DASH_IOC_MAGIC, 11)
#define DASH_NOTIFY_INVALID_DATA_CMD	_IO(DASH_IOC_MAGIC, 12)
#define DASH_NOTIFY_REQUEST_IRQ			_IO(DASH_IOC_MAGIC, 13)
#define DASH_NOTIFY_UPDATE_DASH_PRESENT	_IOW(DASH_IOC_MAGIC, 14, int)

static long  dash_dev_ioctl(struct file *filp, unsigned int cmd,
		unsigned long arg)
{
	struct fastchg_device_info *di = filp->private_data;
	int volt = 0;
	int temp = 0;
	int soc = 0;
	int current_now = 0;
	int remain_cap = 0;

	if (READ_ONCE(di->removed))
		return -ENODEV;
	if (_IOC_TYPE(cmd) != DASH_IOC_MAGIC ||
			_IOC_NR(cmd) < 1 || _IOC_NR(cmd) > 14)
		return -ENOTTY;
	mutex_lock(&di->update_lock);
	if (di->removed) {
		mutex_unlock(&di->update_lock);
		return -ENODEV;
	}

		switch (cmd) {
		case DASH_NOTIFY_FIRMWARE_UPDATE:
			schedule_delayed_work(&di->update_firmware,
					msecs_to_jiffies(2200));
			break;
		case DASH_NOTIFY_FAST_PRESENT:
			oneplus_notify_dash_charger_present(true);
			if (arg == DASH_NOTIFY_FAST_PRESENT + 1) {
				__pm_stay_awake(di->fastchg_wake_lock);
				oem_external_fg_set_allow_reading(false);
				di->fast_chg_allow = false;
				di->fast_normal_to_warm = false;
				mod_timer(&di->watchdog,
				jiffies + msecs_to_jiffies(15000));
			} else if (arg == DASH_NOTIFY_FAST_PRESENT + 2) {
				pr_err("REJECT_DATA\n");
				dash_write(di, REJECT_DATA);
			} else if (arg == DASH_NOTIFY_FAST_PRESENT + 3) {
				notify_check_usb_suspend(false, false);
				dash_write(di, ALLOW_DATA);
				di->fast_chg_started = true;
			}
			break;
		case DASH_NOTIFY_FAST_ABSENT:
			if (arg == DASH_NOTIFY_FAST_ABSENT + 1) {
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_switch_to_normal = false;
				di->fast_normal_to_warm = false;
				di->fast_chg_ing = false;
				pr_err("fastchg stop unexpectly, switch off fastchg\n");
				switch_mode_to_normal(di);
				del_timer(&di->watchdog);
				dash_write(di, REJECT_DATA);
			} else if (arg == DASH_NOTIFY_FAST_ABSENT + 2) {
				notify_check_usb_suspend(true, true);
				oneplus_notify_dash_charger_present(false);
				oneplus_notify_pmic_check_charger_present();
				__pm_relax(di->fastchg_wake_lock);
			}
			break;
		case DASH_NOTIFY_ALLOW_READING_IIC:
			if (arg == DASH_NOTIFY_ALLOW_READING_IIC + 1) {
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = true;
				di->fast_chg_ing = true;
				volt = onplus_get_battery_mvolts();
				temp = onplus_get_battery_temperature();
				remain_cap =
				onplus_get_batt_remaining_capacity();
				soc = onplus_get_battery_soc();
				current_now = onplus_get_average_current();
				pr_err("volt:%d,temp:%d,remain_cap:%d,soc:%d,current:%d\n",
				volt, temp, remain_cap, soc, current_now);
				if (!di->batt_psy)
					di->batt_psy =
					power_supply_get_by_name("battery");
				if (di->batt_psy)
					power_supply_changed(di->batt_psy);
				oem_external_fg_set_allow_reading(false);
				mod_timer(&di->watchdog,
				jiffies + msecs_to_jiffies(15000));
				dash_write(di, ALLOW_DATA);
			}
			break;
		case DASH_NOTIFY_BTB_TEMP_OVER:
			mod_timer(&di->watchdog,
					jiffies + msecs_to_jiffies(15000));
			dash_write(di, ALLOW_DATA);
			break;
		case DASH_NOTIFY_BAD_CONNECTED:
		case DASH_NOTIFY_NORMAL_TEMP_FULL:
			if (arg == DASH_NOTIFY_NORMAL_TEMP_FULL + 1) {
				pr_err("fastchg full, switch off fastchg, set usb_sw_gpio 0\n");
				switch_mode_to_normal(di);
				del_timer(&di->watchdog);
			} else if (arg == DASH_NOTIFY_NORMAL_TEMP_FULL + 2) {
				di->fast_switch_to_normal = true;
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_chg_ing = false;
				notify_check_usb_suspend(true, false);
				oneplus_notify_pmic_check_charger_present();
				__pm_relax(di->fastchg_wake_lock);
			}
			break;
		case DASH_NOTIFY_TEMP_OVER:
			if (arg == DASH_NOTIFY_TEMP_OVER + 1) {
				pr_err("fastchg temp over\n");
				switch_mode_to_normal(di);
				del_timer(&di->watchdog);
			} else if (arg == DASH_NOTIFY_TEMP_OVER + 2) {
				di->fast_normal_to_warm = true;
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_chg_ing = false;
				notify_check_usb_suspend(true, false);
				oneplus_notify_pmic_check_charger_present();
				oneplus_notify_dash_charger_present(false);
				__pm_relax(di->fastchg_wake_lock);
			}
			break;
		case DASH_NOTIFY_ADAPTER_FW_UPDATE:
			if (arg == DASH_NOTIFY_ADAPTER_FW_UPDATE + 1) {
				di->adapter_update_real
					= ADAPTER_FW_NEED_UPDATE;
				di->adapter_update_report
					= di->adapter_update_real;
			} else if (arg == DASH_NOTIFY_ADAPTER_FW_UPDATE + 2) {
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = false;
				oneplus_notify_dash_charger_present(true);
				dash_write(di, ALLOW_DATA);
				__pm_stay_awake(di->fastchg_wake_lock);
				dash_adapter_update(di);
			}
			break;
		case DASH_NOTIFY_UNDEFINED_CMD:
			if (di->fast_chg_started) {
				pr_err("UNDEFINED_CMD, switch off fastchg\n");
				switch_mode_to_normal(di);
				msleep(500); /* avoid i2c conflict */
				/* data err */
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = false;
				__pm_relax(di->fastchg_wake_lock);
				di->fast_chg_allow = false;
				di->fast_switch_to_normal = false;
				di->fast_normal_to_warm = false;
				di->fast_chg_ing = false;
				notify_check_usb_suspend(true, false);
			}
			break;
		case DASH_NOTIFY_INVALID_DATA_CMD:
			if (di->fast_chg_started) {
				oem_external_fg_set_allow_reading(true);
				di->fast_chg_started = false;
				di->fast_chg_allow = false;
				di->fast_switch_to_normal = false;
				di->fast_normal_to_warm = false;
				di->fast_chg_ing = false;
				pr_err("DASH_NOTIFY_INVALID_DATA_CMD, switch off fastchg\n");
				switch_mode_to_normal(di);
				del_timer(&di->watchdog);
				__pm_relax(di->fastchg_wake_lock);
				notify_check_usb_suspend(true, true);
				oneplus_notify_pmic_check_charger_present();
			}
			break;
		case DASH_NOTIFY_REQUEST_IRQ:
			request_mcu_irq(di);
			break;
		case DASH_NOTIFY_UPDATE_DASH_PRESENT:
			if (arg == DASH_NOTIFY_UPDATE_DASH_PRESENT+1)
				update_fast_chg_started(di);
			break;
	default:
			pr_err("bad ioctl %u\n", cmd);
			mutex_unlock(&di->update_lock);
			return -ENOTTY;
	}
	mutex_unlock(&di->update_lock);
	return 0;
}

static ssize_t dash_dev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *offset)
{
	struct fastchg_device_info *di = filp->private_data;
	unsigned char *firmware;
	unsigned int i, records;
	u16 address, expected;

	if (READ_ONCE(di->removed))
		return -ENODEV;
	if (di->firmware_already_updated)
		return 0;
	if (!count || count > 34 * READ_COUNT || count % 34)
		return -EINVAL;
	firmware = memdup_user(buf, count);
	if (IS_ERR(firmware))
		return PTR_ERR(firmware);
	records = count / 34;
	expected = 0x8800;
	for (i = 0; i < records; i++, expected += 32) {
		address = firmware[i * 34] | firmware[i * 34 + 1] << 8;
		if (address != expected) {
			kfree(firmware);
			return -EINVAL;
		}
	}
	mutex_lock(&di->update_lock);
	mutex_lock(&di->firmware_lock);
	if (di->removed) {
		mutex_unlock(&di->firmware_lock);
		mutex_unlock(&di->update_lock);
		kfree(firmware);
		return -ENODEV;
	}
	kfree(di->firmware_data);
	di->firmware_data = firmware;
	di->firmware_size = count;
	di->dashchg_fw_ver_count = count;
	mutex_unlock(&di->firmware_lock);
	mutex_unlock(&di->update_lock);
	pr_info("fw_ver_count=%d\n", di->dashchg_fw_ver_count);
	return count;
}

static const struct file_operations dash_dev_fops = {
	.owner			= THIS_MODULE,
	.llseek			= no_llseek,
	.write			= dash_dev_write,
	.read			= dash_dev_read,
	.open			= dash_dev_open,
	.release		= dash_dev_release,
	.unlocked_ioctl	= dash_dev_ioctl,
};

static int dash_parse_dt(struct fastchg_device_info *di)
{
	u32 flags;
	struct device_node *dev_node = di->client->dev.of_node;

	if (!dev_node) {
		pr_err("device tree info. missing\n");
		return -EINVAL;
	}

	di->usb_sw_1_gpio = of_get_named_gpio_flags(dev_node,
			"microchip,usb-sw-1-gpio", 0, &flags);
	di->usb_sw_2_gpio = of_get_named_gpio_flags(dev_node,
			"microchip,usb-sw-2-gpio", 0, &flags);
	di->ap_clk = of_get_named_gpio_flags(dev_node,
			"microchip,ap-clk", 0, &flags);
	di->ap_data = of_get_named_gpio_flags(dev_node,
			"microchip,ap-data", 0, &flags);
	di->mcu_en_gpio = of_get_named_gpio_flags(dev_node,
			"microchip,mcu-en-gpio", 0, &flags);
	if (!gpio_is_valid(di->usb_sw_1_gpio) ||
			!gpio_is_valid(di->usb_sw_2_gpio) ||
			!gpio_is_valid(di->ap_clk) ||
			!gpio_is_valid(di->ap_data) ||
			!gpio_is_valid(di->mcu_en_gpio))
		return -EINVAL;
	return 0;
}

static int request_dash_gpios(struct fastchg_device_info *di)
{
	int ret;

	ret = gpio_request(di->usb_sw_1_gpio, "usb_sw_1_gpio");
	if (ret)
		return ret;
	di->usb_sw_1_requested = true;
	ret = gpio_direction_output(di->usb_sw_1_gpio, 0);
	if (ret)
		goto free_usb_sw_1;

	ret = gpio_request(di->usb_sw_2_gpio, "usb_sw_2_gpio");
	if (ret)
		goto free_usb_sw_1;
	di->usb_sw_2_requested = true;
	ret = gpio_direction_output(di->usb_sw_2_gpio, 0);
	if (ret)
		goto free_usb_sw_2;

	ret = gpio_request(di->ap_clk, "ap_clk");
	if (ret)
		goto free_usb_sw_2;
	di->ap_clk_requested = true;
	ret = gpio_direction_output(di->ap_clk, 0);
	if (ret)
		goto free_ap_clk;

	ret = gpio_request(di->mcu_en_gpio, "mcu_en_gpio");
	if (ret)
		goto free_ap_clk;
	di->mcu_en_requested = true;
	ret = gpio_direction_output(di->mcu_en_gpio, 0);
	if (ret)
		goto free_mcu_en;

	ret = gpio_request(di->ap_data, "mcu_data");
	if (ret)
		goto free_mcu_en;
	di->ap_data_requested = true;
	ret = gpio_direction_input(di->ap_data);
	if (!ret)
		return 0;
	gpio_free(di->ap_data);
	di->ap_data_requested = false;
free_mcu_en:
	gpio_free(di->mcu_en_gpio);
	di->mcu_en_requested = false;
free_ap_clk:
	gpio_free(di->ap_clk);
	di->ap_clk_requested = false;
free_usb_sw_2:
	gpio_free(di->usb_sw_2_gpio);
	di->usb_sw_2_requested = false;
free_usb_sw_1:
	gpio_free(di->usb_sw_1_gpio);
	di->usb_sw_1_requested = false;
	return ret;
}

static void free_dash_gpios(struct fastchg_device_info *di)
{
	if (di->ap_data_requested)
		gpio_free(di->ap_data);
	if (di->mcu_en_requested)
		gpio_free(di->mcu_en_gpio);
	if (di->ap_clk_requested)
		gpio_free(di->ap_clk);
	if (di->usb_sw_2_requested)
		gpio_free(di->usb_sw_2_gpio);
	if (di->usb_sw_1_requested)
		gpio_free(di->usb_sw_1_gpio);
}

static int dash_pinctrl_init(struct fastchg_device_info *di)
{
	di->pinctrl = devm_pinctrl_get(&di->client->dev);
	if (IS_ERR_OR_NULL(di->pinctrl)) {
		int rc = IS_ERR(di->pinctrl) ? PTR_ERR(di->pinctrl) : -ENODEV;

		dev_err(&di->client->dev,
				"Unable to acquire pinctrl\n");
		di->pinctrl = NULL;
		return rc;
	}
	di->pinctrl_state_active =
		pinctrl_lookup_state(di->pinctrl, "mux_fastchg_active");
	if (IS_ERR_OR_NULL(di->pinctrl_state_active)) {
		dev_err(&di->client->dev,
				"Can not fastchg_active state\n");
		return PTR_ERR(di->pinctrl_state_active);
	}
	di->pinctrl_state_suspended =
		pinctrl_lookup_state(di->pinctrl,
				"mux_fastchg_suspend");
	if (IS_ERR_OR_NULL(di->pinctrl_state_suspended)) {
		dev_err(&di->client->dev,
				"Can not fastchg_suspend state\n");
		return PTR_ERR(di->pinctrl_state_suspended);
	}

		di->pinctrl_mcu_data_state_active =
			pinctrl_lookup_state(di->pinctrl,
					"mcu_data_active");
		if (IS_ERR_OR_NULL(di->pinctrl_mcu_data_state_active)) {
			dev_err(&di->client->dev,
					"Can not mcu_data_active state\n");
			return PTR_ERR(di->pinctrl_mcu_data_state_active);
		}
		di->pinctrl_mcu_data_state_suspended =
					pinctrl_lookup_state(di->pinctrl,
							"mcu_data_suspend");
		if (IS_ERR_OR_NULL(di->pinctrl_mcu_data_state_suspended)) {
			dev_err(&di->client->dev,
					"Can not fastchg_suspend state\n");
			return PTR_ERR(di->pinctrl_mcu_data_state_suspended);
		}

	if (pinctrl_select_state(di->pinctrl,
				di->pinctrl_state_active) < 0)
		return -EINVAL;

	if (pinctrl_select_state(di->pinctrl,
				di->pinctrl_mcu_data_state_active) < 0)
		return -EINVAL;

	return 0;

}

static int dash_get_rpm_clocks(struct fastchg_device_info *di)
{
	struct device_node *rpmcc;
	struct of_phandle_args clkspec = { };

	rpmcc = of_find_compatible_node(NULL, NULL, "qcom,rpmcc-msm8998");
	if (!rpmcc)
		return -EPROBE_DEFER;
	di->snoc_clk = of_clk_get(rpmcc, SNOC_MSMBUS_A_CLK);
	if (IS_ERR(di->snoc_clk)) {
		clkspec.np = rpmcc;
		clkspec.args_count = 1;
		clkspec.args[0] = SNOC_MSMBUS_A_CLK;
		di->snoc_clk = of_clk_get_from_provider(&clkspec);
	}
	if (IS_ERR(di->snoc_clk)) {
		di->snoc_clk = NULL;
		of_node_put(rpmcc);
		return -EPROBE_DEFER;
	}
	di->cnoc_clk = of_clk_get(rpmcc, CNOC_MSMBUS_A_CLK);
	if (IS_ERR(di->cnoc_clk)) {
		clkspec.args[0] = CNOC_MSMBUS_A_CLK;
		di->cnoc_clk = of_clk_get_from_provider(&clkspec);
	}
	of_node_put(rpmcc);
	if (IS_ERR(di->cnoc_clk)) {
		di->cnoc_clk = NULL;
		clk_put(di->snoc_clk);
		di->snoc_clk = NULL;
		return -EPROBE_DEFER;
	}
	return 0;
}

static int dash_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct fastchg_device_info *di;
	int ret;

	pr_info("dash_probe\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("i2c_func error\n");
		return -ENODEV;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		ret = -ENOMEM;
		return ret;
	}

	di->client = client;
	i2c_set_clientdata(client, di);
	kref_init(&di->refcount);
	mutex_init(&di->read_mutex);
	mutex_init(&di->firmware_lock);
	mutex_init(&di->update_lock);
	init_waitqueue_head(&di->read_wq);
	di->firmware_already_updated = false;
	di->irq_enabled = false;
	di->fast_chg_ing = false;
	di->fast_low_temp_full = false;
	di->fast_chg_started = false;

	ret = dash_parse_dt(di);
	if (ret)
		goto err_free_di;

	ret = request_dash_gpios(di);
	if (ret)
		goto err_free_gpios;
	ret = dash_pinctrl_init(di);
	if (ret)
		goto err_free_gpios;
	ret = dash_get_rpm_clocks(di);
	if (ret)
		goto err_free_gpios;

	di->fastchg_wake_lock = wakeup_source_register(&client->dev,
			"fastcg_wake_lock");
	if (!di->fastchg_wake_lock) {
		ret = -ENOMEM;
		goto err_put_clocks;
	}
	di->fastchg_update_fireware_lock = wakeup_source_register(&client->dev,
			"fastchg_fireware_lock");
	if (!di->fastchg_update_fireware_lock) {
		ret = -ENOMEM;
		goto err_unregister_wake;
	}

	INIT_WORK(&di->fastcg_work, fastcg_work_func);
	INIT_WORK(&di->charger_present_status_work,
		update_charger_present_status);
	INIT_DELAYED_WORK(&di->update_fireware_version_work,
		update_fireware_version_func);
	INIT_DELAYED_WORK(&di->update_firmware, dashchg_fw_update);
	INIT_DELAYED_WORK(&di->adapter_update_work, adapter_update_work_func);

	timer_setup(&di->watchdog, di_watchdog, 0);
	di->irq = gpio_to_irq(di->ap_data);
	if (di->irq < 0) {
		ret = di->irq;
		goto err_unregister_update_wake;
	}
	ret = request_irq(di->irq, irq_rx_handler, IRQF_TRIGGER_RISING,
			"mcu_data", di);
	if (ret)
		goto err_unregister_update_wake;
	di->irq_requested = true;
	disable_irq(di->irq);
	di->irq_enabled = false;
	di->adapter_chip = adapter_chip;

	di->dash_device.minor = MISC_DYNAMIC_MINOR;
	di->dash_device.name = "dash";
	di->dash_device.fops = &dash_dev_fops;
	ret = misc_register(&di->dash_device);
	/*when everything is ok, regist /dev/dash*/
	if (ret) {
		pr_err("%s : misc_register failed\n", __FILE__);
		goto err_free_irq;
	}
	di->misc_registered = true;

	mcu_init(di);

	ret = fastcharge_information_register_ctx(&fastcharge_information_ops, di);
	if (ret)
		goto err_misc_deregister;
	di->callbacks_registered = true;
	schedule_delayed_work(&di->update_fireware_version_work,
			msecs_to_jiffies(SHOW_FW_VERSION_DELAY_MS));
	pr_info("dash_probe success\n");

	return 0;

err_misc_deregister:
	misc_deregister(&di->dash_device);
	di->misc_registered = false;
err_free_irq:
	free_irq(di->irq, di);
	di->irq_requested = false;
err_unregister_update_wake:
	wakeup_source_unregister(di->fastchg_update_fireware_lock);
err_unregister_wake:
	wakeup_source_unregister(di->fastchg_wake_lock);
err_put_clocks:
	clk_put(di->cnoc_clk);
	clk_put(di->snoc_clk);
err_free_gpios:
	free_dash_gpios(di);
err_free_di:
	i2c_set_clientdata(client, NULL);
	kfree(di);
	pr_err("dash_probe fail\n");
	return ret;
}

static int dash_remove(struct i2c_client *client)
{
	struct fastchg_device_info *di = i2c_get_clientdata(client);

	if (di->misc_registered) {
		misc_deregister(&di->dash_device);
		di->misc_registered = false;
	}
	mutex_lock(&di->update_lock);
	mutex_lock(&di->firmware_lock);
	WRITE_ONCE(di->removed, true);
	mutex_unlock(&di->firmware_lock);
	mutex_unlock(&di->update_lock);
	wake_up_interruptible_all(&di->read_wq);
	if (di->callbacks_registered) {
		fastcharge_information_unregister_ctx(di);
		di->callbacks_registered = false;
	}
	if (di->irq_requested && di->irq_enabled) {
		disable_irq(di->irq);
		di->irq_enabled = false;
	}
	del_timer_sync(&di->watchdog);
	cancel_delayed_work_sync(&di->adapter_update_work);
	cancel_delayed_work_sync(&di->update_firmware);
	cancel_delayed_work_sync(&di->update_fireware_version_work);
	cancel_work_sync(&di->charger_present_status_work);
	cancel_work_sync(&di->fastcg_work);
	if (di->irq_requested) {
		free_irq(di->irq, di);
		di->irq_requested = false;
	}
	/* Drain an active GPIO read or a reader woken by removal. */
	mutex_lock(&di->read_mutex);
	mutex_unlock(&di->read_mutex);
	wakeup_source_unregister(di->fastchg_update_fireware_lock);
	wakeup_source_unregister(di->fastchg_wake_lock);
	clk_put(di->cnoc_clk);
	clk_put(di->snoc_clk);
	free_dash_gpios(di);
	if (di->batt_psy)
		power_supply_put(di->batt_psy);
	if (di->smb_psy)
		power_supply_put(di->smb_psy);
	kfree(di->firmware_data);
	di->firmware_data = NULL;
	i2c_set_clientdata(client, NULL);
	kref_put(&di->refcount, dash_dev_release_ref);

	return 0;
}

static void dash_shutdown(struct i2c_client *client)
{
	struct fastchg_device_info *di = i2c_get_clientdata(client);

	if (!di)
		return;
	usb_sw_gpio_set(di, 0);
	mcu_en_reset(di);
}

static const struct of_device_id dash_match[] = {
	{ .compatible = "microchip,oneplus_fastcg" },
	{ },
};

static const struct i2c_device_id dash_id[] = {
	{ "dash_fastcg", 1 },
	{},
};
MODULE_DEVICE_TABLE(i2c, dash_id);

static struct i2c_driver dash_fastcg_driver = {
	.driver		= {
		.name = "dash_fastcg",
		.owner	= THIS_MODULE,
		.of_match_table = dash_match,
	},
	.probe		= dash_probe,
	.remove		= dash_remove,
	.shutdown	= dash_shutdown,
	.id_table	= dash_id,
};

static int __init dash_fastcg_init(void)
{
	return i2c_add_driver(&dash_fastcg_driver);
}
module_init(dash_fastcg_init);

static void __exit dash_fastcg_exit(void)
{
	i2c_del_driver(&dash_fastcg_driver);
}
module_exit(dash_fastcg_exit);
