/* Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#define pr_fmt(fmt) "BQ: %s: " fmt, __func__

#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <asm/unaligned.h>
#include <linux/time.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/machine.h>
#include <linux/err.h>
#include <linux/rtc.h>
#include <linux/pm_wakeup.h>

#ifdef CONFIG_OF
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#endif
#include <linux/proc_fs.h>
#include <linux/power/oem_external_fg.h>

/* david.liu@bsp, 20161004 Add BQ27411 support */
#define CONFIG_GAUGE_BQ27411		1
#define DEVICE_TYPE_BQ27541		0x0541
#define DEVICE_TYPE_BQ27411		0x0421
#define DEVICE_BQ27541			0
#define DEVICE_BQ27411			1

#define DEVICE_ROM_SLAVE_ADDR		0x0b
#define DEVICE_NORMAL_SLAVE_ADDR	0x55

#define DRIVER_VERSION			"1.1.0"
/* Bq27541 standard data commands */
#define BQ27541_REG_CNTL		0x00
#define BQ27541_REG_AR			0x02
#define BQ27541_REG_ARTTE		0x04
#define BQ27541_REG_TEMP		0x06
#define BQ27541_REG_VOLT		0x08
#define BQ27541_REG_FLAGS		0x0A
#define BQ27541_REG_NAC			0x0C
#define BQ27541_REG_FAC			0x0e
#define BQ27541_REG_RM			0x10
#define BQ27541_REG_FCC			0x12
#define BQ27541_REG_AI			0x14
#define BQ27541_REG_TTE			0x16
#define BQ27541_REG_TTF			0x18
#define BQ27541_REG_SI			0x1a
#define BQ27541_REG_STTE		0x1c
#define BQ27541_REG_MLI			0x1e
#define BQ27541_REG_MLTTE		0x20
#define BQ27541_REG_AE			0x22
#define BQ27541_REG_AP			0x24
#define BQ27541_REG_TTECP		0x26
#define BQ27541_REG_SOH			0x28
#define BQ27541_REG_SOC			0x2c
#define BQ27541_REG_NIC			0x2e
#define BQ27541_REG_ICR			0x30
#define BQ27541_REG_LOGIDX		0x32
#define BQ27541_REG_LOGBUF		0x34
#define BQ27541_REG_DCAP		0x3c

#define BQ27541_FLAG_DSC		BIT(0)
#define BQ27541_FLAG_FC			BIT(9)

#define BQ27541_CS_DLOGEN		BIT(15)
#define BQ27541_CS_SS			BIT(13)

#ifdef CONFIG_GAUGE_BQ27411
/* david.liu@bsp, 20161004 Add BQ27411 support */
/* Bq27411 standard data commands */
#define BQ27411_REG_TEMP		0x02
#define BQ27411_REG_VOLT		0x04
#define BQ27411_REG_RM			0x0c
#define BQ27411_REG_AI			0x10
#define BQ27411_REG_SOC			0x1c
#define BQ27411_REG_HEALTH		0x20
#define BQ27411_REG_FCC			0x0e

#define CONTROL_CMD			0x00
#define CONTROL_STATUS			0x00
#define SEAL_POLLING_RETRY_LIMIT	100
#define BQ27541_UNSEAL_KEY		0x11151986
#define BQ27411_UNSEAL_KEY		0x80008000

#define BQ27541_RESET_SUBCMD		0x0041
#define BQ27411_RESET_SUBCMD		0x0042
#define SEAL_SUBCMD			0x0020

#define BQ27411_CONFIG_MODE_POLLING_LIMIT	60
#define BQ27411_CONFIG_MODE_BIT			BIT(4)
#define BQ27411_BLOCK_DATA_CONTROL		0x61
#define BQ27411_DATA_CLASS_ACCESS		0x003e
#define BQ27411_CC_DEAD_BAND_ID			0x006b
#define BQ27411_CC_DEAD_BAND_ADDR		0x42
#define BQ27411_CHECKSUM_ADDR			0x60
#define BQ27411_CC_DEAD_BAND_POWERUP_VALUE	0x11
#define BQ27411_CC_DEAD_BAND_SHUTDOWN_VALUE	0x71

#define BQ27411_OPCONFIGB_ID			0x0040
#define BQ27411_OPCONFIGB_ADDR			0x42
#define BQ27411_OPCONFIGB_POWERUP_VALUE		0x07
#define BQ27411_OPCONFIGB_SHUTDOWN_VALUE	0x0f

#define BQ27411_DODATEOC_ID			0x0024
#define BQ27411_DODATEOC_ADDR			0x48
#define BQ27411_DODATEOC_POWERUP_VALUE		0x32
#define BQ27411_DODATEOC_SHUTDOWN_VALUE		0x32
#endif

/* BQ27541 Control subcommands */
#define BQ27541_SUBCMD_CTNL_STATUS		0x0000
#define BQ27541_SUBCMD_DEVCIE_TYPE		0x0001
#define BQ27541_SUBCMD_FW_VER			0x0002
#define BQ27541_SUBCMD_HW_VER			0x0003
#define BQ27541_SUBCMD_DF_CSUM			0x0004
#define BQ27541_SUBCMD_PREV_MACW		0x0007
#define BQ27541_SUBCMD_CHEM_ID			0x0008
#define BQ27541_SUBCMD_BD_OFFSET		0x0009
#define BQ27541_SUBCMD_INT_OFFSET		0x000a
#define BQ27541_SUBCMD_CC_VER			0x000b
#define BQ27541_SUBCMD_OCV			0x000c
#define BQ27541_SUBCMD_BAT_INS			0x000d
#define BQ27541_SUBCMD_BAT_REM			0x000e
#define BQ27541_SUBCMD_SET_HIB			0x0011
#define BQ27541_SUBCMD_CLR_HIB			0x0012
#define BQ27541_SUBCMD_SET_SLP			0x0013
#define BQ27541_SUBCMD_CLR_SLP			0x0014
#define BQ27541_SUBCMD_FCT_RES			0x0015
#define BQ27541_SUBCMD_ENABLE_DLOG		0x0018
#define BQ27541_SUBCMD_DISABLE_DLOG		0x0019
#define BQ27541_SUBCMD_SEALED			0x0020
#define BQ27541_SUBCMD_ENABLE_IT		0x0021
#define BQ27541_SUBCMD_DISABLE_IT		0x0023
#define BQ27541_SUBCMD_CAL_MODE			0x0040
#define BQ27541_SUBCMD_RESET			0x0041
#define ZERO_DEGREE_CELSIUS_IN_TENTH_KELVIN	(-2731)
#define BQ27541_INIT_DELAY			((HZ)*1)
#define SET_BQ_PARAM_DELAY_MS			6000

/* Bq27411 sub commands */
#define BQ27411_SUBCMD_CNTL_STATUS	0x0000
#define BQ27411_SUBCMD_DEVICE_TYPE	0x0001
#define BQ27411_SUBCMD_FW_VER		0x0002
#define BQ27411_SUBCMD_DM_CODE		0x0004
#define BQ27411_SUBCMD_CONFIG_MODE	0x0006
#define BQ27411_SUBCMD_PREV_MACW	0x0007
#define BQ27411_SUBCMD_CHEM_ID		0x0008
#define BQ27411_SUBCMD_SET_HIB		0x0011
#define BQ27411_SUBCMD_CLR_HIB		0x0012
#define BQ27411_SUBCMD_SET_CFG		0x0013
#define BQ27411_SUBCMD_SEALED		0x0020
#define BQ27411_SUBCMD_RESET		0x0041
#define BQ27411_SUBCMD_SOFTRESET	0x0042
#define BQ27411_SUBCMD_EXIT_CFG		0x0043

#define BQ27411_SUBCMD_ENABLE_DLOG	0x0018
#define BQ27411_SUBCMD_DISABLE_DLOG	0x0019
#define BQ27411_SUBCMD_ENABLE_IT	0x0021
#define BQ27411_SUBCMD_DISABLE_IT	0x0023

#define BQ27541_BQ27411_CMD_INVALID	0xFF

#define ERROR_SOC  33
#define ERROR_BATT_VOL  (3800 * 1000)
/* If the system has several batteries we need a different name for each
 * of them...
 */
static DEFINE_IDR(battery_id);
static DEFINE_MUTEX(battery_id_lock);

struct bq27541_device_info;
struct bq27541_access_methods {
	int (*read)(u8 reg, int *rt_value, int b_single,
			struct bq27541_device_info *di);
};

struct update_pre_capacity_data {
	struct delayed_work work;
	struct workqueue_struct *workqueue;
	int suspend_time;
};

#ifdef CONFIG_GAUGE_BQ27411
/* david.liu@bsp, 20161004 Add BQ27411 support */
struct cmd_address {
	u8	reg_temp;
	u8	reg_volt;
	u8	reg_rm;
	u8	reg_ai;
	u8	reg_soc;
	u8	reg_helth;
};
#endif

struct bq27541_device_info {
	struct device			*dev;
	int				id;
	struct bq27541_access_methods	*bus;
	struct i2c_client		*client;
	struct mutex			io_lock;
	struct work_struct		counter;
	/* 300ms delay is needed after bq27541 is powered up
	 * and before any successful I2C transaction
	 */
	struct  delayed_work		bq_reset;
	struct  delayed_work		hw_config;
	struct  delayed_work		modify_soc_smooth_parameter;
	struct  delayed_work		battery_soc_work;
	struct wakeup_source *update_soc_wake_lock;
	struct power_supply	*batt_psy;
	struct power_supply	*usb_psy;
	struct update_pre_capacity_data update_pre_capacity;
	bool callbacks_registered;
	bool first_soc_read;
	bool registered;
	int charging_status;
	int charging_status_pre;
	unsigned long high_temp_start;
	int debug_reg;
	int debug_subcmd;
	int saltate_counter;
	int temp_err_count;
	int raw_soc_pre;
	/*  Add for retry when config fail */
	int retry_count;
	/*  Add for get right soc when sleep long time */
	int soc_pre;
	int  batt_vol_pre;
	int current_pre;
	int dcap_pre;
	int rmcap_pre;
	int fcc_pre;
	int health_pre;
	unsigned long rtc_resume_time;
	unsigned long rtc_suspend_time;
	atomic_t suspended;
	int temp_pre;
	int lcd_off_delt_soc;
	int  t_count;
	bool lcd_is_off;
	bool allow_reading;
	bool fastchg_started;
	bool bq_present;
	bool set_smoothing;
	bool disable_calib_soc;
	bool recovery_rom_mode;
	unsigned long	lcd_off_time;
	unsigned long	soc_pre_time;
	/* david.liu@oneplus.tw, 2016/05/16  Fix capacity won't udate */
	unsigned long	soc_store_time;
#ifdef CONFIG_GAUGE_BQ27411
	/* david.liu@bsp, 20161004 Add BQ27411 support */
	int device_type;
	struct cmd_address cmd_addr;
	bool modify_soc_smooth;
	bool already_modify_smooth;
#endif
};

#define VENDOR_EDIT_OPT_BQ_RESUME_TIME

#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
#include <linux/workqueue.h>

#endif
static void op_bq_reset(struct bq27541_device_info *di);
static int sealed(struct bq27541_device_info *di);
static int seal(struct bq27541_device_info *di);
static int unseal(struct bq27541_device_info *di, u32 key);

static void bq27411_modify_soc_smooth_parameter(
	struct bq27541_device_info *di, bool is_powerup);

static int bq27541_i2c_txsubcmd(u8 reg, unsigned short subcmd,
		struct bq27541_device_info *di);
static int bq27541_average_current(struct bq27541_device_info *di);

static int bq27541_read(u8 reg, int *rt_value, int b_single,
		struct bq27541_device_info *di)
{
	return di->bus->read(reg, rt_value, b_single, di);
}

/*
 * Return the battery temperature in tenths of degree Celsius
 * Or < 0 if something fails.
 */
static int bq27541_battery_temperature(struct bq27541_device_info *di)
{
	int ret;
	int temp = 0;
	int error_temp;

	/* Add for get right*/
	/*soc when sleep long time */
	if (atomic_read(&di->suspended) == 1)
		return di->temp_pre + ZERO_DEGREE_CELSIUS_IN_TENTH_KELVIN;

	if (READ_ONCE(di->allow_reading)) {

#ifdef CONFIG_GAUGE_BQ27411
		/* david.liu@bsp, 20161004 Add BQ27411 support */
		ret = bq27541_read(di->cmd_addr.reg_temp,
				&temp, 0, di);
#else
		ret = bq27541_read(BQ27541_REG_TEMP, &temp, 0, di);
#endif
		/* Add for don't report battery*/
		/*not connect when reading error once. */
		if (ret) {
			di->temp_err_count++;
			pr_err("error reading temperature\n");
			if (di->temp_err_count > 1) {
				di->temp_err_count = 0;
				/* Add for it report bad*/
				/*status when plug out battery */
				di->temp_pre =
				-400 - ZERO_DEGREE_CELSIUS_IN_TENTH_KELVIN;
				error_temp = -400;
				return error_temp;
			} else {
				return di->temp_pre
				+ZERO_DEGREE_CELSIUS_IN_TENTH_KELVIN;
			}
		}
		di->temp_err_count = 0;
	} else {
		return di->temp_pre + ZERO_DEGREE_CELSIUS_IN_TENTH_KELVIN;
	}
	di->temp_pre = temp;
	return temp + ZERO_DEGREE_CELSIUS_IN_TENTH_KELVIN;
}

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */
static int bq27541_battery_voltage(struct bq27541_device_info *di)
{
	int ret;
	int volt = 0;

	/* Add for get right soc when sleep long time */
	if (atomic_read(&di->suspended) == 1)
		return di->batt_vol_pre;

	if (READ_ONCE(di->allow_reading)) {
#ifdef CONFIG_GAUGE_BQ27411
		/* david.liu@bsp, 20161004 Add BQ27411 support */
		ret = bq27541_read(di->cmd_addr.reg_volt,
				&volt, 0, di);
#else
		ret = bq27541_read(BQ27541_REG_VOLT, &volt, 0, di);
#endif
		if (ret) {
			pr_err("error reading voltage,ret:%d\n", ret);
			return ret;
		}
	} else {
		return di->batt_vol_pre;
	}
	di->batt_vol_pre = volt * 1000;
	return volt * 1000;
}

static void bq27541_cntl_cmd(struct bq27541_device_info *di,
		int subcmd)
{
	mutex_lock(&di->io_lock);
	bq27541_i2c_txsubcmd(BQ27541_REG_CNTL, subcmd, di);
	mutex_unlock(&di->io_lock);

}

/*
 * i2c specific code
 */
static int bq27541_i2c_txsubcmd(u8 reg, unsigned short subcmd,
		struct bq27541_device_info *di)
{
	struct i2c_msg msg;
	unsigned char data[3];
	int ret;

	if (!di->client)
		return -ENODEV;

	memset(data, 0, sizeof(data));
	data[0] = reg;
	data[1] = subcmd & 0x00FF;
	data[2] = (subcmd & 0xFF00) >> 8;

	msg.addr = di->client->addr;
	msg.flags = 0;
	msg.len = 3;
	msg.buf = data;
	ret = i2c_transfer(di->client->adapter, &msg, 1);
	if (ret < 0)
		return -EIO;

	return 0;
}

static int bq27541_chip_config(struct bq27541_device_info *di)
{
	int flags = 0, ret = 0;

	bq27541_cntl_cmd(di, BQ27541_SUBCMD_CTNL_STATUS);
	udelay(66);
	ret = bq27541_read(BQ27541_REG_CNTL, &flags, 0, di);
	if (ret < 0) {
		pr_err("error reading register %02x ret = %d\n",
				BQ27541_REG_CNTL, ret);
		return ret;
	}
	udelay(66);

	bq27541_cntl_cmd(di, BQ27541_SUBCMD_ENABLE_IT);
	udelay(66);

	if (!(flags & BQ27541_CS_DLOGEN)) {
		bq27541_cntl_cmd(di, BQ27541_SUBCMD_ENABLE_DLOG);
		udelay(66);
	}

	return 0;
}

#define TEN_PERCENT				10
#define SOC_SHUTDOWN_VALID_LIMITS		30
#define TEN_MINUTES				600
#define FIVE_MINUTES				300
#define TWO_POINT_FIVE_MINUTES			150
#define ONE_MINUTE				60
#define TWENTY_MINUTES				1200
#define TWENTY_PERCENT				20
#define TWENTY_SECS				20


#define CAPACITY_SALTATE_COUNTER_60		38 /* 40 1min */
#define CAPACITY_SALTATE_COUNTER_95		78 /* 60 2.5min */
#define CAPACITY_SALTATE_COUNTER_FULL		200 /* 150 120 5min */
#define CAPACITY_SALTATE_COUNTER_CHARGING_TERM	30 /* 30 1min */
#define CAPACITY_SALTATE_COUNTER		4
#define CAPACITY_SALTATE_COUNTER_NOT_CHARGING	24 /* >=40sec */
#define LOW_BATTERY_PROTECT_VOLTAGE		3250000
#define CAPACITY_CALIBRATE_TIME_60_PERCENT	45 /* 45s */
#define LOW_BATTERY_CAPACITY_THRESHOLD		20

static bool is_rom_mode(struct bq27541_device_info *di)
{
	int rc = 0;
	int flags = 0;

	di->client->addr = DEVICE_ROM_SLAVE_ADDR;
	rc = bq27541_read(BQ27541_REG_CNTL, &flags, 0, di);
	di->client->addr = DEVICE_NORMAL_SLAVE_ADDR;
	if (rc) {
		pr_info("bq27541_rom_mode false\n");
		return false;
	} else {
		pr_info("bq27541_rom_mode true\n");
		return true;
	}
}

static bool exit_rom_mode(struct bq27541_device_info *di)
{
	if (!is_rom_mode(di))
		return false;
	mutex_lock(&di->io_lock);
	di->client->addr = DEVICE_ROM_SLAVE_ADDR;
	bq27541_i2c_txsubcmd(0x00, 0x0f, di);
	bq27541_i2c_txsubcmd(0x64, 0x0f, di);
	bq27541_i2c_txsubcmd(0x65, 0x00, di);
	di->client->addr = DEVICE_NORMAL_SLAVE_ADDR;
	mutex_unlock(&di->io_lock);
	mdelay(1000);
	if (!is_rom_mode(di))
		return true;
	else
		return false;
}

static int get_current_time(unsigned long *now_tm_sec)
{
	struct rtc_time tm;
	struct rtc_device *rtc;
	int rc;

	rtc = rtc_class_open(CONFIG_RTC_HCTOSYS_DEVICE);
	if (rtc == NULL) {
		pr_err("%s: unable to open rtc device (%s)\n",
				__FILE__, CONFIG_RTC_HCTOSYS_DEVICE);
		return -EINVAL;
	}

	rc = rtc_read_time(rtc, &tm);
	if (rc) {
		pr_err("Error reading rtc device (%s) : %d\n",
				CONFIG_RTC_HCTOSYS_DEVICE, rc);
		goto close_time;
	}

	rc = rtc_valid_tm(&tm);
	if (rc) {
		pr_err("Invalid RTC time (%s): %d\n",
				CONFIG_RTC_HCTOSYS_DEVICE, rc);
		goto close_time;
	}
	rtc_tm_to_time(&tm, now_tm_sec);

close_time:
	rtc_class_close(rtc);
	return rc;
}

int get_prop_pre_shutdown_soc(void)
{
	int soc_load;

	soc_load = load_soc();
	if (soc_load == -1)
		return 50;
	else
		return soc_load;
}

static int fg_soc_calibrate(struct  bq27541_device_info *di, int soc)
{
	union power_supply_propval ret = {0,};
	unsigned int soc_calib;
	unsigned long soc_current_time, time_last;
	bool chg_done;
	int temp_region, vbat_mv, ibat_ma, soc_load, soc_temp, counter_temp = 0;

	if (!di->first_soc_read) {
		di->batt_psy = power_supply_get_by_name("battery");
		if (di->batt_psy) {
			di->first_soc_read = true;
			soc_load = load_soc();
			pr_info("soc=%d, soc_load=%d\n", soc, soc_load);
			if (soc_load < 0) {
				/* get last soc error */
				di->soc_pre = soc;
			} else if (soc_load > 0 && soc_load < 100) {
				if (abs(soc_load - soc) > SOC_SHUTDOWN_VALID_LIMITS)
					di->soc_pre = soc;
				else if (soc_load > soc)
					di->soc_pre = soc_load - 1;
				else
					di->soc_pre = soc_load;
			} else if (soc_load == 100
					&& abs(soc_load - soc) > TEN_PERCENT) {
				/* decrease soc when gap between soc_load and */
				/* real_soc is over 10% */
				di->soc_pre = soc_load - 1;
			} else {
				di->soc_pre = soc_load;
			}

			if (!di->batt_psy) {
				pr_err(
				"batt_psy is absent, soc_pre=%d\n",
				di->soc_pre);
				return di->soc_pre;
			}
			/* store the soc when boot first time */
			get_current_time(&di->soc_pre_time);
			clean_backup_soc_ex();
		} else {
			return soc;
		}
	}
	soc_temp = di->soc_pre;

	if (!di->batt_psy) {
		soc_calib = soc;
		goto out;
	}

	ret.intval = get_charging_status();
	di->batt_vol_pre = bq27541_battery_voltage(di);
	chg_done = get_oem_charge_done_status();
	temp_region = fuelgauge_battery_temp_region_get();
	if ((temp_region == BATT_TEMP_LITTLE_COOL
			|| temp_region == BATT_TEMP_COOL
			|| temp_region == BATT_TEMP_NORMAL
			|| temp_region == BATT_TEMP_PRE_NORMAL)
			&& chg_done) {
		ret.intval = POWER_SUPPLY_STATUS_FULL;
	}

	if (ret.intval == POWER_SUPPLY_STATUS_CHARGING
			|| ret.intval == POWER_SUPPLY_STATUS_FULL
			|| di->fastchg_started)
		di->charging_status = 1;
	else
		di->charging_status = 0;

	if (di->charging_status ^ di->charging_status_pre) {
		if (di->charging_status_pre) {
			get_current_time(&soc_current_time);
			di->soc_store_time =
			soc_current_time - di->soc_pre_time;
		}

		get_current_time(&di->soc_pre_time);
		if (!di->charging_status_pre && di->soc_store_time)
			di->soc_pre_time -= di->soc_store_time;
		di->charging_status_pre = di->charging_status;
		di->saltate_counter = 0;
	}

	get_current_time(&soc_current_time);
	time_last = soc_current_time - di->soc_pre_time;
	if (di->charging_status) { /* is charging */
		if (ret.intval == POWER_SUPPLY_STATUS_FULL) {
			soc_calib = di->soc_pre;
			if (di->soc_pre < 100
					&& (temp_region == BATT_TEMP_LITTLE_COOL
					|| temp_region == BATT_TEMP_NORMAL
					|| temp_region == BATT_TEMP_PRE_NORMAL
					|| temp_region == BATT_TEMP_COOL)) {
				if (time_last > TWENTY_SECS)
					soc_calib = di->soc_pre + 1;
			}
		} else {
			if (soc - di->soc_pre > 0) {
				di->saltate_counter++;
				if ((di->fastchg_started
					&& time_last < 20)
				|| (!di->fastchg_started
				&& time_last < 30))
					return di->soc_pre;
				di->saltate_counter = 0;
				soc_calib = di->soc_pre + 1;
			} else if (soc < (di->soc_pre - 1)) {
				di->saltate_counter++;
				if (di->soc_pre == 100) {
					counter_temp =
					CAPACITY_SALTATE_COUNTER_FULL;
					/* t>=5min */
				} else if (di->soc_pre > 95) {
					counter_temp =
					CAPACITY_SALTATE_COUNTER_95;
					/* t>=2.5min */
				} else if (di->soc_pre > 60) {
					counter_temp =
					CAPACITY_SALTATE_COUNTER_60;
					/* t>=1min */
				} else {
					if (time_last >
					CAPACITY_CALIBRATE_TIME_60_PERCENT
						&& (soc - di->soc_pre) < 0)
						counter_temp = 0;
					else
						/* t>=40sec */
						counter_temp =
					CAPACITY_SALTATE_COUNTER_NOT_CHARGING;
				}

				/* avoid dead battery shutdown */
				if (di->batt_vol_pre <=
					LOW_BATTERY_PROTECT_VOLTAGE
					&& di->batt_vol_pre > 2500 * 1000
					&& di->soc_pre
					<= LOW_BATTERY_CAPACITY_THRESHOLD) {
					/* check again */
					vbat_mv =
					bq27541_battery_voltage(di);
					if (vbat_mv <=
						LOW_BATTERY_PROTECT_VOLTAGE
						&& vbat_mv > 2500 * 1000) {
						/* about 9s */
						counter_temp =
						CAPACITY_SALTATE_COUNTER - 1;
					}
				}

				ibat_ma = bq27541_average_current(di);
				/* don't allow soc down*/
				/*if chg current > -200mA */
				if (di->saltate_counter < counter_temp
						|| ibat_ma < -200 * 1000)
					return di->soc_pre;
				di->saltate_counter = 0;

				soc_calib = di->soc_pre - 1;
			} else if ((soc == 0 && soc < di->soc_pre)
					&& di->soc_pre <= 2) {
				di->saltate_counter++;
				if (time_last >
					CAPACITY_CALIBRATE_TIME_60_PERCENT
						&& (soc - di->soc_pre) < 0)
					counter_temp = 0;
				else
					/* t>=40sec */
					counter_temp =
					CAPACITY_SALTATE_COUNTER_NOT_CHARGING;

				if (di->saltate_counter < counter_temp)
					return di->soc_pre;
				di->saltate_counter = 0;
				soc_calib = di->soc_pre - 1;
			} else {
				soc_calib = di->soc_pre;
			}
		}
	} else { /* not charging */
		if ((soc < di->soc_pre)
			|| (di->batt_vol_pre <= LOW_BATTERY_PROTECT_VOLTAGE
			&& di->batt_vol_pre > 2500 * 1000)) {
			if (di->soc_pre == 100) {
				counter_temp = FIVE_MINUTES;
			} else if (di->soc_pre >= 95) {
				counter_temp = TWO_POINT_FIVE_MINUTES;
			} else if (di->soc_pre >= 60) {
				counter_temp = ONE_MINUTE;
			} else {
				if (time_last >=
					CAPACITY_CALIBRATE_TIME_60_PERCENT
						&& (soc - di->soc_pre) < 0)
					counter_temp = 0;
				else
					/* t>=40sec */
				counter_temp =
				CAPACITY_SALTATE_COUNTER_NOT_CHARGING
				+ 20;
			}
			/* avoid dead battery shutdown */
			if (di->batt_vol_pre <=
				LOW_BATTERY_PROTECT_VOLTAGE
				&& di->batt_vol_pre > 2500 * 1000
				&& di->soc_pre <=
				LOW_BATTERY_CAPACITY_THRESHOLD) {
				/* check again */
				vbat_mv = bq27541_battery_voltage(di);
				if (vbat_mv <= LOW_BATTERY_PROTECT_VOLTAGE
				&& vbat_mv > 2500 * 1000 && time_last > 9)
				counter_temp = 0;
			}

			if (time_last < counter_temp)
				return di->soc_pre;
		}

		if (soc < di->soc_pre)
			soc_calib = di->soc_pre - 1;
		else if (di->batt_vol_pre <= LOW_BATTERY_PROTECT_VOLTAGE
				&& di->batt_vol_pre > 2500 * 1000
				&& di->soc_pre > 0 && time_last > 9)
			soc_calib = di->soc_pre - 1;
		else
			soc_calib = di->soc_pre;
	}

out:
	if (soc_calib > 100)
		soc_calib = 100;
	if (soc_calib < 0)
		soc_calib = 0;
	if (soc_calib == 0) {
		if ((di->batt_vol_pre/1000) > 3400)
			soc_calib = 1;
	}
	di->soc_pre = soc_calib;

	if (soc_temp != soc_calib) {
		get_current_time(&di->soc_pre_time);
		/* store when soc changed */
		power_supply_changed(di->batt_psy);
		pr_info("soc:%d, soc_calib:%d, VOLT:%d, current:%d\n",
		soc, soc_calib, bq27541_battery_voltage(di) / 1000,
		bq27541_average_current(di) / 1000);
	}

	return soc_calib;
}

static int bq27541_battery_soc(
struct bq27541_device_info *di, int suspend_time_ms)
{
	int ret;
	int soc = 0;
	int soc_delt = 0;
	bool fg_soc_changed = false;

	/* Add for get right soc when sleep long time */
	if (atomic_read(&di->suspended) == 1) {
		dev_warn(di->dev,
		"di->suspended di->soc_pre=%d\n", di->soc_pre);
		return di->soc_pre;
	}

	if (READ_ONCE(di->allow_reading)) {
#ifdef CONFIG_GAUGE_BQ27411
		/* david.liu@bsp, 20161004 Add BQ27411 support */
		ret = bq27541_read(di->cmd_addr.reg_soc,
				&soc, 0, di);
#else
		ret = bq27541_read(BQ27541_REG_SOC, &soc, 0, di);
#endif
		if (ret) {
			pr_err("error reading soc=%d, ret:%d\n", soc, ret);
			goto read_soc_err;
		}
		if (di->raw_soc_pre != soc)
			pr_err("bq27541_battery_soc = %d\n", soc);

		di->raw_soc_pre = soc;
	} else {
		if (di->soc_pre)
			return di->soc_pre;
		else
			return 0;
	}
	/* Add for get right soc when sleep long time */
	if (suspend_time_ms && di->lcd_is_off) {
		if (soc < di->soc_pre) {
			soc_delt  =  di->soc_pre - soc;
			fg_soc_changed = (soc < TWENTY_PERCENT
					|| soc_delt > di->lcd_off_delt_soc
					|| suspend_time_ms > TEN_MINUTES);
			pr_info("suspend_time_ms=%d,soc_delt=%d,di->lcd_off_delt_soc=%d\n",
			suspend_time_ms, soc_delt, di->lcd_off_delt_soc);
			if (fg_soc_changed) {
				if (suspend_time_ms/TEN_MINUTES) {
				di->soc_pre -=
				(suspend_time_ms / TEN_MINUTES < soc_delt
				? suspend_time_ms / TEN_MINUTES : soc_delt);
				} else {
					di->soc_pre -= 1;
				}
				/* store when soc changed */
				get_current_time(&di->soc_pre_time);
				power_supply_changed(di->batt_psy);
				pr_err("system resume,soc:%d, soc_calib:%d,VOLT:%d,current:%d\n",
				soc, di->soc_pre,
				bq27541_battery_voltage(di) / 1000,
				bq27541_average_current(di) / 1000);
			}
		}
		goto read_soc_err;
	}
	if (di->disable_calib_soc)
		return soc;
	soc = fg_soc_calibrate(di, soc);
	return soc;

read_soc_err:
	if (di->soc_pre) {
		dev_warn(di->dev,
		"read_soc_exit ,di->soc_pre=%d\n", di->soc_pre);
		return di->soc_pre;
	} else
		return 0;
}

static int bq27541_average_current(struct bq27541_device_info *di)
{
	int ret;
	int curr = 0;

	/* Add for get right soc when sleep long time */
	if (atomic_read(&di->suspended) == 1)
		return -di->current_pre;

	if (READ_ONCE(di->allow_reading)) {
#ifdef CONFIG_GAUGE_BQ27411
		/* david.liu@bsp, 20161004 Add BQ27411 support */
		ret = bq27541_read(di->cmd_addr.reg_ai,
				&curr, 0, di);
#else
		ret = bq27541_read(BQ27541_REG_AI, &curr, 0, di);
#endif
		if (ret) {
			pr_err("error reading current.\n");
			return ret;
		}
	} else {
		return -di->current_pre;
	}
	/* negative current */
	if (curr & 0x8000)
		curr = -((~(curr-1)) & 0xFFFF);
	di->current_pre = 1000 * curr;
	return -curr * 1000;
}

static int bq27541_remaining_capacity(struct bq27541_device_info *di)
{
	int ret;
	int cap = 0;

	if (atomic_read(&di->suspended))
		return di->rmcap_pre;

	if (READ_ONCE(di->allow_reading)) {
#ifdef CONFIG_GAUGE_BQ27411
		/* david.liu@bsp, 20161004 Add BQ27411 support */
		ret = bq27541_read(di->cmd_addr.reg_rm,
				&cap, 0, di);
#else
		ret = bq27541_read(BQ27541_REG_RM, &cap, 0, di);
#endif
		if (ret) {
			pr_err("error reading capacity.\n");
			return ret;
		}
		di->rmcap_pre = cap * 1000;
	}

	return di->rmcap_pre;
}

static int bq27541_design_capacity(struct bq27541_device_info *di)
{
	int cap, ret;

	if (atomic_read(&di->suspended))
		return di->dcap_pre;
	if (READ_ONCE(di->allow_reading)) {
		ret = bq27541_read(BQ27541_REG_DCAP, &cap, 0, di);
		if (ret)
			return ret;
		di->dcap_pre = cap * 1000;
	}
	return di->dcap_pre;
}

static int bq27541_full_chg_capacity(struct bq27541_device_info *di)
{
	int cap, ret;

	if (atomic_read(&di->suspended))
		return di->fcc_pre;
	if (READ_ONCE(di->allow_reading)) {
		ret = bq27541_read(di->device_type == DEVICE_BQ27411 ?
				BQ27411_REG_FCC : BQ27541_REG_FCC, &cap, 0, di);
		if (ret)
			return ret;
		di->fcc_pre = cap * 1000;
	}
	return di->fcc_pre;
}

static int bq27541_batt_health(struct bq27541_device_info *di)
{
	int ret;
	int health = 0;

	if (READ_ONCE(di->allow_reading)) {
		ret = bq27541_read(di->cmd_addr.reg_helth,
				&health, 0, di);
		if (ret) {
			pr_err("error reading health\n");
			return ret;
		}
		if (di->device_type == DEVICE_BQ27411)
			di->health_pre = (health & 0xFF);
		else
			di->health_pre = health;
	}

	return di->health_pre;
}

static int bq27541_get_battery_mvolts(void *ctx)
{
	return bq27541_battery_voltage(ctx);
}

static int bq27541_get_batt_remaining_capacity(void *ctx)
{
	return bq27541_remaining_capacity(ctx);
}

static int bq27541_get_batt_design_capacity(void *ctx)
{
	return bq27541_design_capacity(ctx);
}

static int bq27541_get_batt_full_chg_capacity(void *ctx)
{
	return bq27541_full_chg_capacity(ctx);
}

static int bq27541_get_batt_health(void *ctx)
{
	return bq27541_batt_health(ctx);
}
static int bq27541_get_batt_bq_soc(void *ctx)
{
	struct bq27541_device_info *di = ctx;
	int soc;

	di->disable_calib_soc = true;
	soc = bq27541_battery_soc(di, 0);
	di->disable_calib_soc = false;
	return soc;
}
#define SHUTDOWN_TBAT 680
static int bq27541_get_battery_temperature(void *ctx)
{
	struct bq27541_device_info *di = ctx;
	int ret;
	unsigned long current_time, time_last;

	ret = bq27541_battery_temperature(di);
	if (ret >= SHUTDOWN_TBAT) {
		di->t_count++;
		if (di->t_count == 1)
			get_current_time(&di->high_temp_start);
		get_current_time(&current_time);
		time_last = current_time - di->high_temp_start;
		if (time_last < 8)
			return SHUTDOWN_TBAT - 1;
		else {
			pr_info("Tbat =%d T_tol=%d\n",
				ret, (int)(current_time - di->high_temp_start));
		}
	}
	di->t_count = 0;
	return ret;
}
static bool bq27541_is_battery_present(void *ctx)
{
	struct bq27541_device_info *di = ctx;

	return di->bq_present;
}

static bool bq27541_is_battery_temp_within_range(void *ctx)
{
	return true;
}

static bool bq27541_is_battery_id_valid(void *ctx)
{
	return true;
}

#ifdef CONFIG_GAUGE_BQ27411
/* david.liu@bsp, 20161025 Add BQ27411 dash charging */
static int bq27541_get_device_type(void *ctx)
{
	struct bq27541_device_info *di = ctx;

	return di->device_type;
}
#endif

static int bq27541_get_battery_soc(void *ctx)
{
	return bq27541_battery_soc(ctx, 0);
}

static int bq27541_get_average_current(void *ctx)
{
	return bq27541_average_current(ctx);
}

static int bq27541_set_allow_reading(void *ctx, int enable)
{
	struct bq27541_device_info *di = ctx;

	WRITE_ONCE(di->allow_reading, enable);

	return 0;
}

static int bq27541_set_lcd_off_status(void *ctx, int off)
{
	struct bq27541_device_info *di = ctx;
	int soc;

	pr_info("off=%d\n", off);
	if (off) {
		soc = bq27541_get_batt_bq_soc(di);
		di->lcd_off_delt_soc = di->soc_pre - soc;
			pr_info("lcd_off_delt_soc:%d,soc=%d,soc_pre=%d\n",
			di->lcd_off_delt_soc, soc, di->soc_pre);
		get_current_time(&di->lcd_off_time);
		di->lcd_is_off = true;
	} else {
		di->lcd_is_off = false;
		di->lcd_off_delt_soc = 0;
	}
	return 0;
}

static int bq27541_get_fastchg_started_status(void *ctx,
		bool fastchg_started_status)
{
	struct bq27541_device_info *di = ctx;

	di->fastchg_started = fastchg_started_status;

	return 0;
}

static const struct oem_battery_gauge_ops bq27541_batt_gauge_ops = {
	.get_battery_mvolts		= bq27541_get_battery_mvolts,
	.get_battery_temperature	= bq27541_get_battery_temperature,
	.is_battery_present		= bq27541_is_battery_present,
	.is_battery_temp_within_range	= bq27541_is_battery_temp_within_range,
	.is_battery_id_valid		= bq27541_is_battery_id_valid,
	.get_batt_design_capacity	= bq27541_get_batt_design_capacity,
	.get_batt_remaining_capacity	= bq27541_get_batt_remaining_capacity,
	.get_batt_full_chg_capacity	= bq27541_get_batt_full_chg_capacity,
	.get_batt_health		= bq27541_get_batt_health,
	.get_batt_bq_soc		= bq27541_get_batt_bq_soc,
#ifdef CONFIG_GAUGE_BQ27411
	.get_device_type		= bq27541_get_device_type,
#endif
	.get_battery_soc		= bq27541_get_battery_soc,
	.get_average_current		= bq27541_get_average_current,
	.set_allow_reading		= bq27541_set_allow_reading,
	.set_lcd_off_status		= bq27541_set_lcd_off_status,
	.fast_chg_started_status	= bq27541_get_fastchg_started_status,
};
#define BATTERY_SOC_UPDATE_MS 12000
#define LOW_BAT_SOC_UPDATE_MS 6000
#define RESUME_SCHDULE_SOC_UPDATE_WORK_MS 60000

static int is_usb_pluged(struct bq27541_device_info *di)
{
	union power_supply_propval ret = {0,};
	int Vbus, rc; /* david@bsp modified */

	if (!di->usb_psy) {
		di->usb_psy = power_supply_get_by_name("battery");
		if (!di->usb_psy) {
			pr_err("failed to get ps battery\n");
			return -EINVAL;
		}
	}

	/* david@bsp modified */
	rc = power_supply_get_property(di->usb_psy,
			POWER_SUPPLY_PROP_CHARGE_NOW, &ret);
	if (rc) {
		pr_err("failed to get POWER_SUPPLY_PROP_CHARGE_NOW\n");
		return -EINVAL;
	}
	if (ret.intval <= 0)
		return false;

	Vbus = ret.intval;
	return (Vbus > 2500) ? true : false;
}

static bool get_dash_started(struct bq27541_device_info *di)
{
	return di->fastchg_started;
}

static void update_battery_soc_work(struct work_struct *work)
{
	int schedule_time, vbat, flags, rc;
	struct bq27541_device_info *di = container_of(work,
			struct bq27541_device_info, battery_soc_work.work);

	bq27541_set_allow_reading(di, true);
	rc = bq27541_read(BQ27541_REG_CNTL, &flags, 0, di);
	bq27541_set_allow_reading(di, false);
	if (rc || di->recovery_rom_mode) {
		rc = exit_rom_mode(di);
		if (rc || di->recovery_rom_mode) {
			op_bq_reset(di);
			di->recovery_rom_mode = false;
		}
	}
	if (is_usb_pluged(di) || get_dash_started(di)) {
		schedule_delayed_work(
				&di->battery_soc_work,
				msecs_to_jiffies(BATTERY_SOC_UPDATE_MS));
		if (get_dash_started(di))
			return;
		if (di->set_smoothing)
			return;
		if (!READ_ONCE(di->allow_reading))
			bq27541_set_allow_reading(di, true);
		return;
	}
	bq27541_set_allow_reading(di, true);
	vbat = bq27541_get_battery_mvolts(di) / 1000;
	bq27541_get_average_current(di);
	bq27541_get_battery_temperature(di);
	bq27541_get_battery_soc(di);
	bq27541_get_batt_design_capacity(di);
	bq27541_get_batt_remaining_capacity(di);
	bq27541_get_batt_full_chg_capacity(di);
	pr_debug("battery remain capacity:%d\n",
				bq27541_get_batt_health(di));
	bq27541_set_allow_reading(di, false);
	if (!di->already_modify_smooth)
		schedule_delayed_work(
		&di->modify_soc_smooth_parameter, 1000);
	schedule_time =
		vbat < 3600 ? LOW_BAT_SOC_UPDATE_MS : BATTERY_SOC_UPDATE_MS;
	schedule_delayed_work(&di->battery_soc_work,
			msecs_to_jiffies(schedule_time));
}

bool get_extern_fg_regist_done(void)
{
	return oem_external_fg_registered();
}
bool get_extern_bq_present(void)
{
	return oem_external_fg_is_battery_present(false);
}

#ifdef CONFIG_GAUGE_BQ27411
/* david.liu@bsp, 20161004 Add BQ27411 support */
static void gauge_set_cmd_addr(struct bq27541_device_info *di,
		int device_type)
{
	if (device_type == DEVICE_BQ27541) {
		di->cmd_addr.reg_temp = BQ27541_REG_TEMP;
		di->cmd_addr.reg_volt = BQ27541_REG_VOLT;
		di->cmd_addr.reg_rm = BQ27541_REG_RM;
		di->cmd_addr.reg_ai = BQ27541_REG_AI;
		di->cmd_addr.reg_soc = BQ27541_REG_SOC;
		di->cmd_addr.reg_helth = BQ27541_REG_NIC;
	} else { /* device_bq27411 */
		di->cmd_addr.reg_temp = BQ27411_REG_TEMP;
		di->cmd_addr.reg_volt = BQ27411_REG_VOLT;
		di->cmd_addr.reg_rm = BQ27411_REG_RM;
		di->cmd_addr.reg_ai = BQ27411_REG_AI;
		di->cmd_addr.reg_soc = BQ27411_REG_SOC;
		di->cmd_addr.reg_helth = BQ27411_REG_HEALTH;
	}
}
#endif

static void op_bq_reset(struct bq27541_device_info *di)
{

	if (di->device_type == DEVICE_BQ27541)
		return;
	if (get_dash_started(di))
		return;
	pr_info("%s begin\n", __func__);
	bq27541_set_allow_reading(di, false);
	if (sealed(di)) {
		if (!unseal(di, BQ27411_UNSEAL_KEY))
			return;
		msleep(50);
	}
	bq27541_i2c_txsubcmd(BQ27541_SUBCMD_CTNL_STATUS,
						BQ27541_SUBCMD_RESET, di);
	usleep_range(1000, 1001);
	if (sealed(di) == 0) {
		usleep_range(1000, 1001);
		seal(di);
	}
	mdelay(2000);
	bq27541_set_allow_reading(di, true);
	pr_info("%s end\n", __func__);
}

static void bq_modify_soc_smooth_parameter(struct work_struct *work)
{
	struct bq27541_device_info *di;

	di = container_of(work, struct bq27541_device_info,
			modify_soc_smooth_parameter.work);
	if (get_dash_started(di))
		return;
	if (di->already_modify_smooth)
		return;
	bq27541_set_allow_reading(di, false);
	di->set_smoothing = true;
	bq27411_modify_soc_smooth_parameter(di, true);
	di->set_smoothing = false;
	bq27541_set_allow_reading(di, true);
}

static bool check_bat_present(struct bq27541_device_info *di)
{
	int flags = 0, ret = 0;

	ret = bq27541_read(BQ27541_SUBCMD_CHEM_ID, &flags, 0, di);
	if (ret < 0) {
		pr_err("read bq27541  fail\n");
		di->bq_present = false;
		return false;
	}
	di->bq_present = true;
	return true;
}
static void op_bq_reset_work(struct work_struct *work)
{
	struct bq27541_device_info *di = container_of(work,
			struct bq27541_device_info, bq_reset.work);

	op_bq_reset(di);
	di->recovery_rom_mode = false;
}
static int bq27541_configure(struct bq27541_device_info *di)
{
	int ret = 0, flags = 0, type = 0, fw_ver = 0;

	ret = bq27541_chip_config(di);
	if (ret) {
		pr_err("Failed to config Bq27541\n");
		di->recovery_rom_mode = exit_rom_mode(di);
		return ret;
	}
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_CTNL_STATUS);
	udelay(66);
	bq27541_read(BQ27541_REG_CNTL, &flags, 0, di);
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_DEVCIE_TYPE);
	udelay(66);
	bq27541_read(BQ27541_REG_CNTL, &type, 0, di);
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_FW_VER);
	udelay(66);
	bq27541_read(BQ27541_REG_CNTL, &fw_ver, 0, di);

#ifdef CONFIG_GAUGE_BQ27411
	/* david.liu@bsp, 20161004 Add BQ27411 support */
	if (type == DEVICE_TYPE_BQ27411) {
		di->device_type = DEVICE_BQ27411;
		pr_info("DEVICE_BQ27411\n");
	} else {
		di->device_type = DEVICE_BQ27541;
		pr_info("DEVICE_BQ27541\n");
	}
	gauge_set_cmd_addr(di, di->device_type);
	WRITE_ONCE(di->allow_reading, true);
#endif

	di->registered = true;
	pr_info("DEVICE_TYPE is 0x%02X, FIRMWARE_VERSION is 0x%02X\n",
			type, fw_ver);
	pr_info("Complete bq27541 configuration 0x%02X\n", flags);
	return 0;
}

static void bq27541_hw_config(struct work_struct *work)
{
	struct bq27541_device_info *di = container_of(work,
		struct bq27541_device_info, hw_config.work);

	if (!bq27541_configure(di))
		return;
	if (--di->retry_count > 0)
		schedule_delayed_work(&di->hw_config, HZ);
}

static int bq27541_read_i2c(u8 reg, int *rt_value, int b_single,
		struct bq27541_device_info *di)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[2];
	unsigned char data[2];
	int err;

	if (!client->adapter)
		return -ENODEV;
	mutex_lock(&di->io_lock);

	/* Write register */
	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = data;
	data[0] = reg;
	/* Read data */
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	if (!b_single)
		msg[1].len = 2;
	else
		msg[1].len = 1;
	msg[1].buf = data;
	err = i2c_transfer(client->adapter, msg, 2);
	if (err >= 0) {
		if (!b_single)
			*rt_value = get_unaligned_le16(data);
		else
			*rt_value = data[0];
		mutex_unlock(&di->io_lock);
		return 0;
	}
	mutex_unlock(&di->io_lock);
	return err;
}

#ifdef CONFIG_BQ27541_TEST_ENABLE
static ssize_t bq27541_read_stdcmd(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;
	int temp = 0;
	struct bq27541_device_info *di;

	di = i2c_get_clientdata(to_i2c_client(dev));

	if (di->debug_reg <= BQ27541_REG_ICR && di->debug_reg > 0x00) {
		ret = bq27541_read(di->debug_reg, &temp, 0, di);
		if (ret)
			ret = snprintf(buf, PAGE_SIZE, "Read Error!\n");
		else
			ret = snprintf(buf, PAGE_SIZE, "0x%02x\n", temp);
	} else
		ret = snprintf(buf, PAGE_SIZE, "Register Error!\n");

	return ret;
}

static ssize_t bq27541_write_stdcmd(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct bq27541_device_info *di =
			i2c_get_clientdata(to_i2c_client(dev));
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	int cmd;
	int rc;

	rc = kstrtou32(buf, 0, &cmd);
	if (rc != 1)
		pr_err("%s,scanf error\n");
	di->debug_reg = cmd;
	return ret;
}

static ssize_t bq27541_read_subcmd(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret;
	int temp = 0;
	struct bq27541_device_info *di;

	di = i2c_get_clientdata(to_i2c_client(dev));

	if (di->debug_subcmd == BQ27541_SUBCMD_DEVCIE_TYPE ||
			di->debug_subcmd == BQ27541_SUBCMD_FW_VER ||
			di->debug_subcmd == BQ27541_SUBCMD_HW_VER ||
			di->debug_subcmd == BQ27541_SUBCMD_CHEM_ID) {

		bq27541_cntl_cmd(di, di->debug_subcmd);
		udelay(66);
		ret = bq27541_read(BQ27541_REG_CNTL, &temp, 0, di);

		if (ret)
			ret = snprintf(buf, PAGE_SIZE, "Read Error!\n");
		else
			ret = snprintf(buf, PAGE_SIZE, "0x%02x\n", temp);
	} else
		ret = snprintf(buf, PAGE_SIZE, "Register Error!\n");

	return ret;
}

static ssize_t bq27541_write_subcmd(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct bq27541_device_info *di =
			i2c_get_clientdata(to_i2c_client(dev));
	ssize_t ret = strnlen(buf, PAGE_SIZE);
	int cmd, rc;

	rc = kstrtou32(buf, 0, &cmd);
	if (rc != 1)
		pr_err("%s,scanf error\n");
	di->debug_subcmd = cmd;
	return ret;
}

static DEVICE_ATTR(std_cmd, 0644, bq27541_read_stdcmd,
		bq27541_write_stdcmd);
static DEVICE_ATTR(sub_cmd, 0644, bq27541_read_subcmd,
		bq27541_write_subcmd);
static struct attribute *fs_attrs[] = {
	&dev_attr_std_cmd.attr,
	&dev_attr_sub_cmd.attr,
	NULL,
};
static struct attribute_group fs_attr_group = {
	.attrs = fs_attrs,
};

#endif

#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
static void update_pre_capacity_func(struct work_struct *w)
{
	struct update_pre_capacity_data *update = container_of(w,
			struct update_pre_capacity_data, work.work);
	struct bq27541_device_info *di = container_of(update,
			struct bq27541_device_info, update_pre_capacity);

	pr_info("enter\n");
	bq27541_set_allow_reading(di, true);
	bq27541_get_battery_temperature(di);
	bq27541_battery_soc(di, update->suspend_time);
	bq27541_set_allow_reading(di, false);
	__pm_relax(di->update_soc_wake_lock);
	pr_info("exit\n");
}
#endif

#define MAX_RETRY_COUNT	5
#define DEFAULT_INVALID_SOC_PRE  -22

static void bq27541_parse_dt(struct bq27541_device_info *di)
{
	struct device_node *node = di->dev->of_node;

	di->modify_soc_smooth = of_property_read_bool(node,
				"qcom,modify-soc-smooth");
	pr_err("di->modify_soc_smooth=%d\n", di->modify_soc_smooth);
}
static int sealed(struct bq27541_device_info *di)
{
	/*return control_cmd_read(di, CONTROL_STATUS) & (1 << 13);*/
	int value = 0;

	bq27541_cntl_cmd(di, CONTROL_STATUS);
	/*bq27541_cntl_cmd(di,CONTROL_STATUS);*/
	usleep_range(10000, 10001);
	bq27541_read_i2c(CONTROL_STATUS, &value, 0, di);
	pr_info(" REG_CNTL: 0x%x\n", value);

	if (di->device_type == DEVICE_BQ27541)
		return value & BIT(14);
	else if (di->device_type == DEVICE_BQ27411)
		return value & BIT(13);
	else
		return 1;
}

static int seal(struct bq27541_device_info *di)
{
	int i = 0;

	if (sealed(di)) {
		pr_err("bq27541/27411 sealed,return\n");
		return 1;
	}
	bq27541_cntl_cmd(di, SEAL_SUBCMD);
	usleep_range(10000, 10001);
	for (i = 0; i < SEAL_POLLING_RETRY_LIMIT; i++) {
		if (sealed(di))
			return 1;
		usleep_range(10000, 10001);
	}
	return 0;
}


static int unseal(struct bq27541_device_info *di, u32 key)
{
	int i = 0;

	if (!sealed(di))
		goto out;
re_unseal:
	 if (di->device_type == DEVICE_BQ27411) {
		usleep_range(10000, 10001);
	/*bq27541_write(CONTROL_CMD, key & 0xFFFF, false, di);*/
		bq27541_cntl_cmd(di, 0x8000);
		usleep_range(10000, 10001);
		bq27541_cntl_cmd(di, 0x8000);
		usleep_range(10000, 10001);
	}
	bq27541_cntl_cmd(di, 0xffff);
	usleep_range(10000, 10001);
	bq27541_cntl_cmd(di, 0xffff);
	usleep_range(10000, 10001);

	while (i < SEAL_POLLING_RETRY_LIMIT) {
		i++;
		if (!sealed(di))
			break;
		usleep_range(10000, 10001);
		goto re_unseal;
	}

out:
	pr_info("bq27541 : i=%d,bq27541_di->device_type=%d\n",
		i, di->device_type);

	if (i == SEAL_POLLING_RETRY_LIMIT) {
		pr_err("bq27541 failed\n");
		return 0;
	} else {
		return 1;
	}
}

static int bq27541_read_i2c_onebyte(struct bq27541_device_info *di,
		u8 cmd, u8 *returnData)
{
	if (cmd == BQ27541_BQ27411_CMD_INVALID)
		return 0;

	mutex_lock(&di->io_lock);
	*returnData = i2c_smbus_read_byte_data(di->client, cmd);

	mutex_unlock(&di->io_lock);
	/*pr_err(" cmd = 0x%x, returnData = 0x%x\r\n",cmd,*returnData)  ;*/
	if (*returnData < 0)
		return 1;
	else
		return 0;
}

static int bq27541_i2c_txsubcmd_onebyte(struct bq27541_device_info *di,
		u8 cmd, u8 writeData)
{
	if (cmd == BQ27541_BQ27411_CMD_INVALID)
		return 0;

	mutex_lock(&di->io_lock);
	i2c_smbus_write_byte_data(di->client, cmd, writeData);
	mutex_unlock(&di->io_lock);
	return 0;
}


static int bq27411_write_block_data_cmd(struct bq27541_device_info *di,
				int block_id, u8 reg_addr, u8 new_value)
{
	int rc = 0;
	u8 old_value = 0, old_csum = 0, new_csum = 0;
	/*u8 new_csum_test = 0, csum_temp = 0;*/

	usleep_range(1000, 1001);
	bq27541_i2c_txsubcmd(BQ27411_DATA_CLASS_ACCESS, block_id, di);
	usleep_range(10000, 10001);
	rc = bq27541_read_i2c_onebyte(di, reg_addr, &old_value);
	if (rc) {
		pr_err("%s read reg_addr = 0x%x fail\n", __func__, reg_addr);
		return 1;
	}
	if (old_value == new_value)
		return 0;
	usleep_range(1000, 1001);
	rc = bq27541_read_i2c_onebyte(di, BQ27411_CHECKSUM_ADDR, &old_csum);
	if (rc) {
		pr_err("%s read checksum fail\n", __func__);
		return 1;
	}
	usleep_range(1000, 1001);
	bq27541_i2c_txsubcmd_onebyte(di, reg_addr, new_value);
	usleep_range(1000, 1001);
	new_csum = (old_value + old_csum - new_value) & 0xff;
	usleep_range(1000, 1001);
	bq27541_i2c_txsubcmd_onebyte(di, BQ27411_CHECKSUM_ADDR, new_csum);
	pr_err("bq27411 write blk_id = 0x%x, addr = 0x%x, old_val = 0x%x, new_val = 0x%x, old_csum = 0x%x, new_csum = 0x%x\n",
		block_id, reg_addr, old_value, new_value, old_csum, new_csum);
	return 0;
}

static int bq27411_read_block_data_cmd(struct bq27541_device_info *di,
				int block_id, u8 reg_addr)
{
	u8 value = 0;

	usleep_range(1000, 1001);
	bq27541_i2c_txsubcmd(BQ27411_DATA_CLASS_ACCESS, block_id, di);
	usleep_range(10000, 10001);
	bq27541_read_i2c_onebyte(di, reg_addr, &value);
	return value;
}

static int bq27411_enable_config_mode(
	struct bq27541_device_info *di, bool enable)
{
	int config_mode = 0, i = 0, rc = 0;

	if (enable) {		/*enter config mode*/
		usleep_range(1000, 1001);
		bq27541_cntl_cmd(di, BQ27411_SUBCMD_SET_CFG);
		usleep_range(1000, 1001);
		for (i = 0; i < BQ27411_CONFIG_MODE_POLLING_LIMIT; i++) {
			i++;
			rc = bq27541_read_i2c(BQ27411_SUBCMD_CONFIG_MODE,
				&config_mode, 0, di);
			if (rc < 0) {
				pr_err("%s i2c read error\n", __func__);
				return 1;
			}
			if (config_mode & BIT(4))
				break;
			msleep(50);
		}
	} else {		/* exit config mode*/
		usleep_range(1000, 1001);
		bq27541_cntl_cmd(di, BQ27411_SUBCMD_EXIT_CFG);
		usleep_range(1000, 1001);
		for (i = 0; i < BQ27411_CONFIG_MODE_POLLING_LIMIT; i++) {
			i++;
			rc = bq27541_read_i2c(BQ27411_SUBCMD_CONFIG_MODE,
				&config_mode, 0, di);
			if (rc < 0) {
				pr_err("%s i2c read error\n", __func__);
				return 1;
			}
			if ((config_mode & BIT(4)) == 0)
				break;
			msleep(50);
		}
	}
	if (i == BQ27411_CONFIG_MODE_POLLING_LIMIT) {
		pr_err("%s fail config_mode = 0x%x, enable = %d\n",
			__func__, config_mode, enable);
		return 1;
	}
		pr_err("%s success i = %d, config_mode = 0x%x, enable = %d\n",
			__func__, i, config_mode, enable);
		return 0;
}

static bool bq27411_check_soc_smooth_parameter(
	struct bq27541_device_info *di, bool is_powerup)
{
	int value_read = 0;
	u8 dead_band_val = 0, op_cfgb_val = 0, dodat_val = 0, rc = 0;

	return true;	/*not check because it costs 5.5 seconds*/

	msleep(4000);
	if (sealed(di)) {
		if (!unseal(di, BQ27411_UNSEAL_KEY))
			return false;
		msleep(50);
	}

	if (is_powerup) {
		dead_band_val = BQ27411_CC_DEAD_BAND_POWERUP_VALUE;
		op_cfgb_val = BQ27411_OPCONFIGB_POWERUP_VALUE;
		dodat_val = BQ27411_DODATEOC_POWERUP_VALUE;
	} else {	/*shutdown*/
		dead_band_val = BQ27411_CC_DEAD_BAND_SHUTDOWN_VALUE;
		op_cfgb_val = BQ27411_OPCONFIGB_SHUTDOWN_VALUE;
		dodat_val = BQ27411_DODATEOC_SHUTDOWN_VALUE;
	}
	rc = bq27411_enable_config_mode(di, true);
	if (rc) {
		pr_err("%s enable config mode fail\n", __func__);
		return false;
	}
	/*enable block data control*/
	rc = bq27541_i2c_txsubcmd_onebyte(di,
			BQ27411_BLOCK_DATA_CONTROL, 0x00);
	if (rc) {
		pr_err("%s enable block data control fail\n", __func__);
		goto check_error;
	}
	usleep_range(5000, 5001);

	/*check cc-dead-band*/
	value_read = bq27411_read_block_data_cmd(di,
	BQ27411_CC_DEAD_BAND_ID, BQ27411_CC_DEAD_BAND_ADDR);
	if (value_read != dead_band_val) {
		pr_err("%s cc_dead_band error, value_read = 0x%x\n",
			__func__, value_read);
		goto check_error;
	}

	/*check opconfigB*/
	value_read = bq27411_read_block_data_cmd(di,
	BQ27411_OPCONFIGB_ID,
	BQ27411_OPCONFIGB_ADDR);
	if (value_read != op_cfgb_val) {
		pr_err("%s opconfigb error, value_read = 0x%x\n",
			__func__, value_read);
		goto check_error;
	}

	/*check dodateoc*/
	value_read = bq27411_read_block_data_cmd(di,
				BQ27411_DODATEOC_ID, BQ27411_DODATEOC_ADDR);
	if (value_read != dodat_val) {
		pr_err("%s dodateoc error, value_read = 0x%x\n",
			__func__, value_read);
		goto check_error;
	}
	bq27411_enable_config_mode(di, false);
	return true;

check_error:
	bq27411_enable_config_mode(di, false);
	return false;
}

static int bq27411_write_soc_smooth_parameter(
	struct bq27541_device_info *di, bool is_powerup)
{
	int rc = 0;
	u8 dead_band_val = 0, op_cfgb_val = 0, dodat_val = 0;

	if (is_powerup) {
		dead_band_val = BQ27411_CC_DEAD_BAND_POWERUP_VALUE;
		op_cfgb_val = BQ27411_OPCONFIGB_POWERUP_VALUE;
		dodat_val = BQ27411_DODATEOC_POWERUP_VALUE;
	} else {	/*shutdown*/
		dead_band_val = BQ27411_CC_DEAD_BAND_SHUTDOWN_VALUE;
		op_cfgb_val = BQ27411_OPCONFIGB_SHUTDOWN_VALUE;
		dodat_val = BQ27411_DODATEOC_SHUTDOWN_VALUE;
	}

	/*enter config mode*/
	rc = bq27411_enable_config_mode(di, true);
	if (rc) {
		pr_err("%s enable config mode fail\n", __func__);
		return 1;
	}
	/*enable block data control*/
	bq27541_i2c_txsubcmd_onebyte(di, BQ27411_BLOCK_DATA_CONTROL, 0x00);

	usleep_range(5000, 5001);
	/*step1: update cc-dead-band*/
	rc = bq27411_write_block_data_cmd(di, BQ27411_CC_DEAD_BAND_ID,
			BQ27411_CC_DEAD_BAND_ADDR, dead_band_val);
	if (rc) {
		pr_err("%s cc_dead_band fail\n", __func__);
		goto exit_config_mode;
	}
	/*step2: update opconfigB*/
	rc = bq27411_write_block_data_cmd(di, BQ27411_OPCONFIGB_ID,
			BQ27411_OPCONFIGB_ADDR, op_cfgb_val);
	if (rc) {
		pr_err("%s opconfigB fail\n", __func__);
		goto exit_config_mode;
	}
	/*step3: update dodateoc*/
	rc = bq27411_write_block_data_cmd(di, BQ27411_DODATEOC_ID,
			BQ27411_DODATEOC_ADDR, dodat_val);
	if (rc) {
		pr_err("%s dodateoc fail\n", __func__);
		goto exit_config_mode;
	}
	bq27411_enable_config_mode(di, false);
	return 0;

exit_config_mode:
	bq27411_enable_config_mode(di, false);
	return 1;
}

static void bq27411_modify_soc_smooth_parameter(
	struct bq27541_device_info *di, bool is_powerup)
{
	int rc = 0;
	bool check_result = false, tried_again = false;

	if (di->modify_soc_smooth == false
		|| di->device_type == DEVICE_BQ27541) {
		return;
	}

	pr_err("%s begin\n", __func__);
	if (sealed(di)) {
		if (!unseal(di, BQ27411_UNSEAL_KEY))
			return;
		msleep(50);
	}
write_parameter:
	rc = bq27411_write_soc_smooth_parameter(di, is_powerup);
	if (rc && tried_again == false) {
		tried_again = true;
		goto write_parameter;
	} else {
		check_result =
		bq27411_check_soc_smooth_parameter(di, is_powerup);
		if (check_result == false && tried_again == false) {
			tried_again = true;
			goto write_parameter;
		}
	}
	usleep_range(1000, 1001);
	if (sealed(di) == 0) {
		usleep_range(1000, 1001);
		seal(di);
	}
	di->already_modify_smooth = true;
	if (di->recovery_rom_mode)
		schedule_delayed_work(&di->bq_reset, 2000);
	pr_err("%s end\n", __func__);
}

static int bq27541_battery_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct bq27541_device_info *di;
	struct bq27541_access_methods *bus;
	int num, retval;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENODEV;

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		return -ENOMEM;
	}

	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (!bus) {
		retval = -ENOMEM;
		goto free_di;
	}

	mutex_lock(&battery_id_lock);
	num = idr_alloc(&battery_id, client, 0, 0, GFP_KERNEL);
	mutex_unlock(&battery_id_lock);
	if (num < 0) {
		retval = num;
		goto free_bus;
	}
	di->id = num;

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	bus->read = &bq27541_read_i2c;
	di->bus = bus;
	di->client = client;
	mutex_init(&di->io_lock);

	di->update_soc_wake_lock = wakeup_source_register(di->dev,
			"bq_delt_soc_wake_lock");
	if (!di->update_soc_wake_lock) {
		retval = -ENOMEM;
		goto remove_idr;
	}
#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
	di->update_pre_capacity.workqueue =
		create_singlethread_workqueue("update_pre_capacity");
	if (!di->update_pre_capacity.workqueue) {
		retval = -ENOMEM;
		goto trash_wakeup;
	}
	INIT_DELAYED_WORK(&di->update_pre_capacity.work,
		update_pre_capacity_func);
#endif
	di->soc_pre = DEFAULT_INVALID_SOC_PRE;
	di->temp_pre = 0;
#ifndef CONFIG_GAUGE_BQ27411
	/* david.liu@bsp, 20161004 Add BQ27411 support */
	WRITE_ONCE(di->allow_reading, true);
#endif
	/* Add for retry when config fail */
	di->retry_count = MAX_RETRY_COUNT;
	atomic_set(&di->suspended, 0);

#ifdef CONFIG_BQ27541_TEST_ENABLE
	retval = sysfs_create_group(&client->dev.kobj, &fs_attr_group);
	if (retval)
		goto destroy_workqueue;
#endif
	bq27541_parse_dt(di);
	di->t_count = 0;
	di->lcd_is_off = false;
	INIT_DELAYED_WORK(&di->hw_config, bq27541_hw_config);
	INIT_DELAYED_WORK(&di->bq_reset, op_bq_reset_work);
	INIT_DELAYED_WORK(&di->modify_soc_smooth_parameter,
		bq_modify_soc_smooth_parameter);
	INIT_DELAYED_WORK(&di->battery_soc_work, update_battery_soc_work);
	/* The gauge needs its documented power-up settling interval. */
	msleep(jiffies_to_msecs(BQ27541_INIT_DELAY));
	retval = bq27541_configure(di);
	if (retval)
		goto remove_test_sysfs;
	check_bat_present(di);
	retval = external_battery_gauge_register_ctx(
			&bq27541_batt_gauge_ops, di);
	if (retval)
		goto remove_test_sysfs;
	retval = bq27541_information_register_ctx(
			&bq27541_batt_gauge_ops, di);
	if (retval)
		goto unregister_external_fg;
	di->callbacks_registered = true;
	schedule_delayed_work(&di->modify_soc_smooth_parameter,
		SET_BQ_PARAM_DELAY_MS);
	schedule_delayed_work(&di->battery_soc_work, BATTERY_SOC_UPDATE_MS);
	pr_info("probe success\n");
	return 0;

unregister_external_fg:
	external_battery_gauge_unregister_ctx(di);
remove_test_sysfs:
#ifdef CONFIG_BQ27541_TEST_ENABLE
	sysfs_remove_group(&client->dev.kobj, &fs_attr_group);
destroy_workqueue:
#endif
#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
	destroy_workqueue(di->update_pre_capacity.workqueue);
#endif
trash_wakeup:
	wakeup_source_unregister(di->update_soc_wake_lock);
remove_idr:
	mutex_lock(&battery_id_lock);
	idr_remove(&battery_id, num);
	mutex_unlock(&battery_id_lock);
free_bus:
	kfree(bus);
free_di:
	kfree(di);
	return retval;
}

static int bq27541_battery_remove(struct i2c_client *client)
{
	struct bq27541_device_info *di = i2c_get_clientdata(client);

	if (di->callbacks_registered) {
		bq27541_information_unregister_ctx(di);
		external_battery_gauge_unregister_ctx(di);
		di->callbacks_registered = false;
	}
#ifdef CONFIG_BQ27541_TEST_ENABLE
	sysfs_remove_group(&client->dev.kobj, &fs_attr_group);
#endif
	cancel_delayed_work_sync(&di->battery_soc_work);
	cancel_delayed_work_sync(&di->bq_reset);
	cancel_delayed_work_sync(&di->hw_config);
	cancel_delayed_work_sync(&di->modify_soc_smooth_parameter);
#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
	cancel_delayed_work_sync(&di->update_pre_capacity.work);
	destroy_workqueue(di->update_pre_capacity.workqueue);
#endif
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_DISABLE_DLOG);
	udelay(66);
	bq27541_cntl_cmd(di, BQ27541_SUBCMD_DISABLE_IT);
	wakeup_source_unregister(di->update_soc_wake_lock);
	if (di->usb_psy)
		power_supply_put(di->usb_psy);
	if (di->batt_psy)
		power_supply_put(di->batt_psy);
	kfree(di->bus);

	mutex_lock(&battery_id_lock);
	idr_remove(&battery_id, di->id);
	mutex_unlock(&battery_id_lock);

	kfree(di);
	return 0;
}


static int bq27541_battery_suspend(struct device *dev)
{
	int ret = 0;
	struct bq27541_device_info *di = dev_get_drvdata(dev);
#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
	cancel_delayed_work_sync(&di->battery_soc_work);
#endif
	/* Snapshot all fallback values while I2C access is still permitted. */
	bq27541_set_allow_reading(di, true);
	bq27541_get_battery_temperature(di);
	bq27541_get_battery_mvolts(di);
	bq27541_get_average_current(di);
	bq27541_get_battery_soc(di);
	bq27541_get_batt_design_capacity(di);
	bq27541_get_batt_remaining_capacity(di);
	bq27541_get_batt_full_chg_capacity(di);
	bq27541_get_batt_health(di);
	atomic_set(&di->suspended, 1);
	ret = get_current_time(&di->rtc_suspend_time);
	if (ret) {
		pr_err("Failed to read RTC time\n");
		return 0;
	}
	return 0;
}


/*1 minute*/

#define RESUME_TIME  60
static int bq27541_battery_resume(struct device *dev)
{
	int ret = 0;
	int suspend_time;
	struct bq27541_device_info *di =  dev_get_drvdata(dev);

	ret = get_current_time(&di->rtc_resume_time);
	if (ret) {
		pr_err("Failed to read RTC time\n");
		di->rtc_resume_time = di->rtc_suspend_time;
	}
	suspend_time =  di->rtc_resume_time - di->rtc_suspend_time;
	pr_info("suspend_time=%d\n", suspend_time);
#ifdef VENDOR_EDIT_OPT_BQ_RESUME_TIME
	di->update_pre_capacity.suspend_time = suspend_time;

	if (di->rtc_resume_time - di->lcd_off_time >= TWO_POINT_FIVE_MINUTES) {
		pr_err("di->rtc_resume_time - di->lcd_off_time=%ld\n",
				di->rtc_resume_time - di->lcd_off_time);
		__pm_stay_awake(di->update_soc_wake_lock);
		get_current_time(&di->lcd_off_time);
		queue_delayed_work_on(0,
				di->update_pre_capacity.workqueue,
				&di->update_pre_capacity.work,
				msecs_to_jiffies(1000));
	}
	/* Refresh cached readings before normal external reads resume. */
	atomic_set(&di->suspended, 0);
	bq27541_set_allow_reading(di, true);
	bq27541_get_battery_temperature(di);
	bq27541_get_battery_mvolts(di);
	bq27541_get_average_current(di);
	bq27541_get_battery_soc(di);
	bq27541_get_batt_design_capacity(di);
	bq27541_get_batt_remaining_capacity(di);
	bq27541_get_batt_full_chg_capacity(di);
	bq27541_get_batt_health(di);
	schedule_delayed_work(&di->battery_soc_work,
			msecs_to_jiffies(RESUME_SCHDULE_SOC_UPDATE_WORK_MS));
#else
	/* update pre capacity when sleep time more than 1minutes */
	bq27541_battery_soc(di, suspend_time);
	atomic_set(&di->suspended, 0);
#endif
	return 0;
}


static void bq27541_shutdown(struct i2c_client *client)
{
	struct bq27541_device_info *di = i2c_get_clientdata(client);

	if (di->already_modify_smooth)
		bq27411_modify_soc_smooth_parameter(di, false);

	if (di->soc_pre != DEFAULT_INVALID_SOC_PRE)
		backup_soc_ex(di->soc_pre);
}

static const struct of_device_id bq27541_match[] = {
	{ .compatible = "ti,bq27541-battery" },
	{ },
};

static const struct i2c_device_id bq27541_id[] = {
	{ "bq27541-battery", 1 },
	{},
};
MODULE_DEVICE_TABLE(i2c, BQ27541_id);
static const struct dev_pm_ops bq27541_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(bq27541_battery_suspend, bq27541_battery_resume)
};

static struct i2c_driver bq27541_battery_driver = {
	.driver		= {
		.name = "bq27541-battery",
		.pm = &bq27541_pm,
	},
	.probe		= bq27541_battery_probe,
	.remove		= bq27541_battery_remove,
	.shutdown	= bq27541_shutdown,
	.id_table	= bq27541_id,
};

static int __init bq27541_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&bq27541_battery_driver);
	if (ret)
		pr_err("Unable to register BQ27541 driver\n");

	return ret;
}
module_init(bq27541_battery_init);

static void __exit bq27541_battery_exit(void)
{
	i2c_del_driver(&bq27541_battery_driver);
}
module_exit(bq27541_battery_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Qualcomm Innovation Center, Inc.");
MODULE_DESCRIPTION("BQ27541 battery monitor driver");
