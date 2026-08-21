// SPDX-License-Identifier: GPL-2.0-only
/* OnePlus MSM8998 DASH companion, adapted from X-FT 45587b80. */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power/oem_external_fg.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <linux/pmic-voter.h>
#include "smb-lib.h"
#include "smb-reg.h"
#include "smb2-oneplus.h"

#if IS_ENABLED(CONFIG_ARCH_ONEPLUS_MSM8998)

#define ONEPLUS_HEARTBEAT_MS		6000
#define ONEPLUS_RETRY_MS		1000
#define ONEPLUS_CHG_TIMEOUT_COUNT	6000
#define ONEPLUS_TEMP_HYST_DECIDEGC	20
#define ONEPLUS_BATT_OVP_MV		4500
#define ONEPLUS_CHG_OVP_MV		5800
#define ONEPLUS_CHG_UVP_MV		4300
#define ONEPLUS_CHG_HYST_MV		100
#define ONEPLUS_UOVP_DETECT_COUNT	3
#define ONEPLUS_ICL_CEIL_UA		1500000
#define ONEPLUS_ICL_FLOOR_UA		500000
#define ONEPLUS_ICL_DELTA_UA		250000
#define ONEPLUS_SDP_CEIL_UA		500000
#define ONEPLUS_SDP_FLOOR_UA		100000

#define ONEPLUS_POLICY_VOTER		"ONEPLUS_POLICY_VOTER"
#define ONEPLUS_UOVP_VOTER		"ONEPLUS_UOVP_VOTER"
#define ONEPLUS_DASH_VOTER		"ONEPLUS_DASH_VOTER"

struct oneplus_smb2 {
	struct smb_charger *chg;
	struct mutex policy_lock;
	struct mutex uovp_lock;
	struct delayed_work heartbeat_work;
	struct delayed_work retry_work;
	struct delayed_work recovery_work;
	struct delayed_work watchdog_work;
	struct wakeup_source *dash_ws;

	int temp_bound[7];
	int ibatmax_ma[BATT_TEMP_INVALID];
	int vbatmax_mv[BATT_TEMP_INVALID];
	int vbatdet_mv[BATT_TEMP_INVALID];
	enum temp_region_type temp_region;
	enum batt_status_type batt_status;
	int heartbeat_count;
	int ov_count;
	int uv_count;
	int retry_count;
	int term_count;
	bool usb_present;
	bool usb_enumerated;
	bool dash_present;
	bool dash_starting;
	bool dash_disabled_normal;
	bool time_out;
	bool recharge;
	bool chg_ovp;
	bool batt_ovp;
	bool lcd_off;
	bool otg_switch;
	bool removed;
};

static bool oneplus_prop(enum power_supply_property prop)
{
	return prop >= POWER_SUPPLY_PROP_SET_ALLOW_READ_EXTERN_FG_IIC &&
		prop <= POWER_SUPPLY_PROP_NOTIFY_CHARGER_SET_PARAMETER;
}

static int oneplus_bms_get(struct oneplus_smb2 *op,
		enum power_supply_property prop, int fallback)
{
	union power_supply_propval val = { .intval = fallback };

	if (!smblib_get_prop_from_bms(op->chg, prop, &val))
		return val.intval;
	return fallback;
}

static enum temp_region_type oneplus_temp_region(struct oneplus_smb2 *op,
		int temp)
{
	int i;
	enum temp_region_type region = BATT_TEMP_HOT;

	for (i = 0; i < ARRAY_SIZE(op->temp_bound); i++) {
		if (temp < op->temp_bound[i]) {
			region = i;
			break;
		}
	}

	if (op->temp_region < BATT_TEMP_INVALID &&
		region != op->temp_region) {
		if (region > op->temp_region &&
			temp < op->temp_bound[op->temp_region] +
				ONEPLUS_TEMP_HYST_DECIDEGC)
			return op->temp_region;
		if (region < op->temp_region && region < ARRAY_SIZE(op->temp_bound) &&
			temp >= op->temp_bound[region] -
				ONEPLUS_TEMP_HYST_DECIDEGC)
			return op->temp_region;
	}

	return region;
}

static void oneplus_vote_policy(struct oneplus_smb2 *op,
		enum temp_region_type region)
{
	struct smb_charger *chg = op->chg;
	bool bad_temp = region == BATT_TEMP_COLD || region == BATT_TEMP_HOT;

	vote(chg->chg_disable_votable, ONEPLUS_POLICY_VOTER, bad_temp, 0);
	if (bad_temp)
		return;

	if (op->ibatmax_ma[region] > 0)
		vote(chg->fcc_votable, ONEPLUS_POLICY_VOTER, true,
			op->ibatmax_ma[region] * 1000);
	if (op->vbatmax_mv[region] > 0)
		vote(chg->fv_votable, ONEPLUS_POLICY_VOTER, true,
			op->vbatmax_mv[region] * 1000);
}

static void oneplus_uovp(struct oneplus_smb2 *op)
{
	union power_supply_propval val = { };
	int rc, vbus_mv, icl_ua, ceil_ua, floor_ua;

	rc = smblib_get_prop_usb_voltage_now(op->chg, &val);
	if (rc < 0)
		return;

	/* SMB2 reports microvolts.  X-FT thresholds are millivolts. */
	vbus_mv = val.intval / 1000;
	if (op->chg->real_charger_type == POWER_SUPPLY_TYPE_USB) {
		ceil_ua = ONEPLUS_SDP_CEIL_UA;
		floor_ua = ONEPLUS_SDP_FLOOR_UA;
	} else {
		ceil_ua = ONEPLUS_ICL_CEIL_UA;
		floor_ua = ONEPLUS_ICL_FLOOR_UA;
	}
	mutex_lock(&op->uovp_lock);
	if (vbus_mv >= ONEPLUS_CHG_OVP_MV) {
		op->ov_count++;
		op->uv_count = 0;
	} else if (vbus_mv <= ONEPLUS_CHG_UVP_MV) {
		op->uv_count++;
		op->ov_count = 0;
	} else if ((!op->chg_ovp ||
		vbus_mv < ONEPLUS_CHG_OVP_MV - ONEPLUS_CHG_HYST_MV) &&
		vbus_mv > ONEPLUS_CHG_UVP_MV + ONEPLUS_CHG_HYST_MV) {
		op->ov_count = 0;
		op->uv_count = 0;
		op->chg_ovp = false;
		vote(op->chg->chg_disable_votable, ONEPLUS_UOVP_VOTER,
			false, 0);
		vote(op->chg->usb_icl_votable, ONEPLUS_UOVP_VOTER,
			false, 0);
	}

	if (op->ov_count >= ONEPLUS_UOVP_DETECT_COUNT ||
		op->uv_count >= ONEPLUS_UOVP_DETECT_COUNT) {
		op->chg_ovp = true;
		icl_ua = get_effective_result(op->chg->usb_icl_votable);
		if (icl_ua < 0 || icl_ua > ceil_ua)
			icl_ua = ceil_ua;
		icl_ua -= ONEPLUS_ICL_DELTA_UA;
		if (icl_ua >= floor_ua) {
			vote(op->chg->usb_icl_votable, ONEPLUS_UOVP_VOTER,
				true, icl_ua);
		} else {
			vote(op->chg->chg_disable_votable, ONEPLUS_UOVP_VOTER,
				true, 0);
		}
	}
	mutex_unlock(&op->uovp_lock);
}

static int oneplus_set_otg_switch(struct oneplus_smb2 *op, bool enable)
{
	u8 role = enable ? 0 : UFP_EN_CMD_BIT;
	u8 ctrl;
	int i, rc = 0;

	if (op->otg_switch == enable)
		return 0;
	for (i = 0; i < 10; i++) {
		rc = smblib_masked_write(op->chg,
			TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			TYPEC_POWER_ROLE_CMD_MASK, role);
		if (rc < 0)
			return rc;
		usleep_range(30000, 31000);
		rc = smblib_read(op->chg,
			TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG, &ctrl);
		if (rc < 0)
			return rc;
		if ((!role && !(ctrl & TYPEC_POWER_ROLE_CMD_MASK)) ||
			(role && (ctrl & UFP_EN_CMD_BIT)))
			break;
	}
	op->otg_switch = enable;
	return i == 10 ? -ETIMEDOUT : 0;
}

static void oneplus_reset_session(struct oneplus_smb2 *op)
{
	op->usb_enumerated = false;
	op->dash_present = false;
	op->dash_starting = false;
	op->time_out = false;
	op->recharge = false;
	op->chg_ovp = false;
	op->batt_ovp = false;
	op->heartbeat_count = 0;
	op->ov_count = 0;
	op->uv_count = 0;
	op->retry_count = 0;
	op->term_count = 0;
	oem_fastchg_set_fast_chg_allow(false);
	oem_fastchg_set_switch_to_noraml_false();
	oem_fastchg_set_usb_switch(false);
	oneplus_set_otg_switch(op, true);
	__pm_relax(op->dash_ws);
	vote(op->chg->chg_disable_votable, ONEPLUS_DASH_VOTER, false, 0);
	vote(op->chg->chg_disable_votable, ONEPLUS_UOVP_VOTER, false, 0);
	vote(op->chg->usb_icl_votable, ONEPLUS_UOVP_VOTER, false, 0);
}

static void oneplus_policy_update(struct oneplus_smb2 *op)
{
	enum temp_region_type region;
	int temp, vbat_mv, current_ua;
	union power_supply_propval status = { };
	bool fast_started, fast_ing;

	mutex_lock(&op->policy_lock);
	if (op->removed || !op->usb_present)
		goto out;

	fast_started = oem_fastchg_started();
	fast_ing = oem_fastchg_get_fast_chg_ing();
	op->dash_starting = fast_started && !fast_ing;
	if (fast_started) {
		op->dash_present = true;
		if (op->chg->batt_psy)
			power_supply_changed(op->chg->batt_psy);
		if (fast_ing && !op->dash_disabled_normal) {
			vote(op->chg->chg_disable_votable,
				ONEPLUS_DASH_VOTER, true, 0);
			op->dash_disabled_normal = true;
		}
		goto out;
	}
	if (op->dash_disabled_normal) {
		vote(op->chg->chg_disable_votable, ONEPLUS_DASH_VOTER,
			false, 0);
		op->dash_disabled_normal = false;
	}

	temp = oneplus_bms_get(op, POWER_SUPPLY_PROP_TEMP,
		oem_bq_get_battery_temperature(250));
	region = oneplus_temp_region(op, temp);
	if (region != op->temp_region) {
		op->temp_region = region;
		oneplus_vote_policy(op, region);
	}

	vbat_mv = oneplus_bms_get(op, POWER_SUPPLY_PROP_VOLTAGE_NOW,
		oem_bq_get_battery_mvolts(4000) * 1000) / 1000;
	if (vbat_mv > ONEPLUS_BATT_OVP_MV) {
		op->batt_ovp = true;
		vote(op->chg->chg_disable_votable, ONEPLUS_UOVP_VOTER,
			true, 0);
	} else if (op->batt_ovp &&
		vbat_mv < ONEPLUS_BATT_OVP_MV - ONEPLUS_CHG_HYST_MV) {
		op->batt_ovp = false;
		vote(op->chg->chg_disable_votable, ONEPLUS_UOVP_VOTER,
			false, 0);
	}

	current_ua = oneplus_bms_get(op, POWER_SUPPLY_PROP_CURRENT_NOW, 0);
	if (!smblib_get_prop_batt_status(op->chg, &status) &&
		status.intval == POWER_SUPPLY_STATUS_CHARGING &&
		abs(current_ua) <= 100000 &&
		op->vbatmax_mv[region] > 0 &&
		vbat_mv >= op->vbatmax_mv[region]) {
		if (++op->term_count >= 5) {
			op->chg->chg_done = true;
			op->term_count = 0;
			vote(op->chg->chg_disable_votable,
				ONEPLUS_POLICY_VOTER, true, 0);
		}
	} else {
		op->term_count = 0;
	}
	if (op->chg->chg_done && op->vbatdet_mv[region] > 0 &&
		vbat_mv <= op->vbatdet_mv[region]) {
		op->chg->chg_done = false;
		op->recharge = true;
		vote(op->chg->chg_disable_votable, ONEPLUS_POLICY_VOTER,
			false, 0);
	}

	if (status.intval == POWER_SUPPLY_STATUS_CHARGING)
		op->heartbeat_count++;
	else
		op->heartbeat_count = 0;
	if (op->heartbeat_count >= ONEPLUS_CHG_TIMEOUT_COUNT) {
		op->time_out = true;
		vote(op->chg->chg_disable_votable, ONEPLUS_POLICY_VOTER,
			true, 0);
	}
	oneplus_uovp(op);
out:
	mutex_unlock(&op->policy_lock);
}

static void oneplus_heartbeat_work(struct work_struct *work)
{
	struct oneplus_smb2 *op = container_of(to_delayed_work(work),
		struct oneplus_smb2, heartbeat_work);

	oneplus_policy_update(op);
	if (!READ_ONCE(op->removed))
		schedule_delayed_work(&op->heartbeat_work,
			round_jiffies_relative(
				msecs_to_jiffies(ONEPLUS_HEARTBEAT_MS)));
}

static void oneplus_retry_work(struct work_struct *work)
{
	struct oneplus_smb2 *op = container_of(to_delayed_work(work),
		struct oneplus_smb2, retry_work);
	int temp;
	bool allowed;

	mutex_lock(&op->policy_lock);
	temp = oneplus_bms_get(op, POWER_SUPPLY_PROP_TEMP,
		oem_bq_get_battery_temperature(250));
	allowed = !op->removed && op->usb_present && !op->usb_enumerated &&
		!oem_fastchg_started() && !oem_fastchg_switch_to_normal() &&
		oem_fastchg_get_fastchg_firmware_already_updated() &&
		!oem_fastchg_get_fast_low_temp_full() && temp >= 165 &&
		temp <= 430 &&
		!get_effective_result(op->chg->chg_disable_votable);
	if (allowed && op->retry_count++ < 3) {
		oneplus_set_otg_switch(op, false);
		oem_fastchg_set_usb_switch(true);
		oem_fastchg_set_fast_chg_allow(true);
		op->dash_starting = true;
		__pm_stay_awake(op->dash_ws);
		smb2_oneplus_switcher_power(op->chg);
		schedule_delayed_work(&op->watchdog_work,
			msecs_to_jiffies(ONEPLUS_HEARTBEAT_MS));
		schedule_delayed_work(&op->retry_work,
			msecs_to_jiffies(ONEPLUS_RETRY_MS));
	}
	mutex_unlock(&op->policy_lock);
}

static void oneplus_recovery_work(struct work_struct *work)
{
	struct oneplus_smb2 *op = container_of(to_delayed_work(work),
		struct oneplus_smb2, recovery_work);

	if (!READ_ONCE(op->removed) && op->usb_present)
		smblib_rerun_aicl(op->chg);
}

static void oneplus_watchdog_work(struct work_struct *work)
{
	struct oneplus_smb2 *op = container_of(to_delayed_work(work),
		struct oneplus_smb2, watchdog_work);

	mutex_lock(&op->policy_lock);
	if (!READ_ONCE(op->removed) && op->dash_present &&
		!oem_fastchg_get_fast_chg_ing()) {
		oem_fastchg_set_usb_switch(false);
		op->dash_present = false;
		schedule_delayed_work(&op->recovery_work,
			msecs_to_jiffies(ONEPLUS_RETRY_MS));
	}
	mutex_unlock(&op->policy_lock);
}

static int oneplus_notify_event(void *ctx)
{
	struct oneplus_smb2 *op = ctx;
	union power_supply_propval vbus = { };

	if (READ_ONCE(op->removed))
		return -ENODEV;
	/* Restore the normal AICL path before rechecking a DASH unplug. */
	smblib_rerun_aicl(op->chg);
	if (!smblib_get_prop_usb_voltage_now(op->chg, &vbus) &&
		vbus.intval <= 2500000)
		smb2_oneplus_usb_event(op->chg, false);
	else
		schedule_delayed_work(&op->recovery_work, 0);
	return 0;
}

static int oneplus_op_control(void *ctx, int status, bool check_power_ok)
{
	struct oneplus_smb2 *op = ctx;

	if (READ_ONCE(op->removed))
		return -ENODEV;
	mutex_lock(&op->policy_lock);
	op->dash_starting = !!status;
	if (status) {
		__pm_stay_awake(op->dash_ws);
		if (check_power_ok)
			smb2_oneplus_switcher_power(op->chg);
	} else {
		smblib_masked_write(op->chg,
			USBIN_5V_AICL_THRESHOLD_CFG_REG, BIT(0) | BIT(1),
			BIT(0) | BIT(1));
		smblib_masked_write(op->chg,
			USBIN_CONT_AICL_THRESHOLD_CFG_REG, BIT(0) | BIT(1),
			BIT(0) | BIT(1));
		__pm_relax(op->dash_ws);
	}
	if (status && check_power_ok)
		schedule_delayed_work(&op->watchdog_work,
			msecs_to_jiffies(ONEPLUS_HEARTBEAT_MS));
	mutex_unlock(&op->policy_lock);
	return 0;
}

static int oneplus_notify_present(void *ctx, int present)
{
	struct oneplus_smb2 *op = ctx;

	if (READ_ONCE(op->removed))
		return -ENODEV;
	mutex_lock(&op->policy_lock);
	op->dash_present = !!present && op->usb_present;
	if (op->dash_present) {
		op->chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		vote(op->chg->usb_icl_votable, PD_VOTER, true, 1800000);
	} else {
		vote(op->chg->usb_icl_votable, PD_VOTER, false, 0);
	}
	if (op->chg->batt_psy)
		power_supply_changed(op->chg->batt_psy);
	mutex_unlock(&op->policy_lock);
	return 0;
}

static int oneplus_get_temp_region(void *ctx)
{
	struct oneplus_smb2 *op = ctx;

	return READ_ONCE(op->temp_region);
}

static bool oneplus_get_charge_done(void *ctx)
{
	struct oneplus_smb2 *op = ctx;

	return READ_ONCE(op->recharge) || READ_ONCE(op->chg->chg_done);
}

static int oneplus_get_charging_status(void *ctx)
{
	struct oneplus_smb2 *op = ctx;
	union power_supply_propval status = {
		.intval = POWER_SUPPLY_STATUS_UNKNOWN,
	};

	smblib_get_prop_batt_status(op->chg, &status);
	return status.intval;
}

static int oneplus_notify_enumeration(void *ctx, int status)
{
	struct oneplus_smb2 *op = ctx;

	if (READ_ONCE(op->removed))
		return -ENODEV;
	smb2_oneplus_enumeration(op->chg, !!status);
	return 0;
}

static const struct oem_dash_notify_ops oneplus_notify_ops = {
	.notify_event = oneplus_notify_event,
	.op_contrl = oneplus_op_control,
	.notify_dash_charger_present = oneplus_notify_present,
	.get_temp_region = oneplus_get_temp_region,
	.get_charge_done = oneplus_get_charge_done,
	.get_charging_status = oneplus_get_charging_status,
};

static const struct oem_usb_enumeration_ops oneplus_enum_ops = {
	.notify_usb_enumeration = oneplus_notify_enumeration,
};

static void oneplus_read_u32(struct device_node *node, const char *name,
		int *value, int fallback)
{
	u32 tmp;

	*value = of_property_read_u32(node, name, &tmp) ? fallback : tmp;
}

static void oneplus_parse_dt(struct oneplus_smb2 *op)
{
	struct device_node *node = op->chg->dev->of_node;

	oneplus_read_u32(node, "cold-bat-decidegc", &op->temp_bound[0], 30);
	op->temp_bound[0] = -op->temp_bound[0];
	oneplus_read_u32(node, "little-cold-bat-decidegc", &op->temp_bound[1], 0);
	oneplus_read_u32(node, "cool-bat-decidegc", &op->temp_bound[2], 50);
	oneplus_read_u32(node, "little-cool-bat-decidegc", &op->temp_bound[3], 120);
	oneplus_read_u32(node, "pre-normal-bat-decidegc", &op->temp_bound[4], 160);
	oneplus_read_u32(node, "warm-bat-decidegc", &op->temp_bound[5], 450);
	oneplus_read_u32(node, "hot-bat-decidegc", &op->temp_bound[6], 530);

#define OP_DT_REGION(_region, _name, _ibat, _vmax, _vdet) do { \
	oneplus_read_u32(node, "ibatmax-" _name "-ma", \
		&op->ibatmax_ma[_region], _ibat); \
	oneplus_read_u32(node, "vbatmax-" _name "-mv", \
		&op->vbatmax_mv[_region], _vmax); \
	oneplus_read_u32(node, "vbatdet-" _name "-mv", \
		&op->vbatdet_mv[_region], _vdet); \
} while (0)
	OP_DT_REGION(BATT_TEMP_LITTLE_COLD, "little-cold", 325, 3975, 3700);
	OP_DT_REGION(BATT_TEMP_COOL, "cool", 450, 4380, 4150);
	OP_DT_REGION(BATT_TEMP_LITTLE_COOL, "little-cool", 775, 4380, 4270);
	OP_DT_REGION(BATT_TEMP_PRE_NORMAL, "pre-normal", 1425, 4380, 4270);
	OP_DT_REGION(BATT_TEMP_NORMAL, "normal", 1950, 4380, 4270);
	OP_DT_REGION(BATT_TEMP_WARM, "warm", 750, 4080, 3980);
#undef OP_DT_REGION
}

int smb2_oneplus_init(struct smb_charger *chg)
{
	struct oneplus_smb2 *op;
	int rc;

	if (chg->mode != PARALLEL_MASTER)
		return 0;
	op = devm_kzalloc(chg->dev, sizeof(*op), GFP_KERNEL);
	if (!op)
		return -ENOMEM;
	op->chg = chg;
	op->temp_region = BATT_TEMP_INVALID;
	op->batt_status = BATT_STATUS_GOOD;
	mutex_init(&op->policy_lock);
	mutex_init(&op->uovp_lock);
	INIT_DELAYED_WORK(&op->heartbeat_work, oneplus_heartbeat_work);
	INIT_DELAYED_WORK(&op->retry_work, oneplus_retry_work);
	INIT_DELAYED_WORK(&op->recovery_work, oneplus_recovery_work);
	INIT_DELAYED_WORK(&op->watchdog_work, oneplus_watchdog_work);
	oneplus_parse_dt(op);
	op->dash_ws = wakeup_source_register(chg->dev, "oneplus_dash");
	if (!op->dash_ws)
		return -ENOMEM;
	chg->oneplus = op;
	rc = notify_dash_unplug_register_ctx(&oneplus_notify_ops, op);
	if (rc)
		goto err_clear;
	rc = register_notify_usb_enumeration_status_ctx(&oneplus_enum_ops, op);
	if (rc)
		goto err_notify;
	schedule_delayed_work(&op->heartbeat_work,
		msecs_to_jiffies(ONEPLUS_HEARTBEAT_MS));
	return 0;

err_notify:
	notify_dash_unplug_unregister_ctx(op);
err_clear:
	chg->oneplus = NULL;
	wakeup_source_unregister(op->dash_ws);
	return rc;
}

void smb2_oneplus_deinit(struct smb_charger *chg)
{
	struct oneplus_smb2 *op = chg->oneplus;

	if (!op)
		return;
	WRITE_ONCE(op->removed, true);
	unregister_notify_usb_enumeration_status_ctx(op);
	notify_dash_unplug_unregister_ctx(op);
	cancel_delayed_work_sync(&op->watchdog_work);
	cancel_delayed_work_sync(&op->recovery_work);
	cancel_delayed_work_sync(&op->retry_work);
	cancel_delayed_work_sync(&op->heartbeat_work);
	oneplus_reset_session(op);
	__pm_relax(op->dash_ws);
	wakeup_source_unregister(op->dash_ws);
	chg->oneplus = NULL;
}

void smb2_oneplus_usb_event(struct smb_charger *chg, bool present)
{
	struct oneplus_smb2 *op = chg->oneplus;

	if (!op || READ_ONCE(op->removed))
		return;
	mutex_lock(&op->policy_lock);
	op->usb_present = present;
	if (!present)
		oneplus_reset_session(op);
	else
		schedule_delayed_work(&op->retry_work,
			msecs_to_jiffies(ONEPLUS_RETRY_MS));
	mutex_unlock(&op->policy_lock);
}

void smb2_oneplus_switcher_power(struct smb_charger *chg)
{
	struct oneplus_smb2 *op = chg->oneplus;

	if (op && op->usb_present && !op->dash_present)
		schedule_delayed_work(&op->recovery_work,
			msecs_to_jiffies(ONEPLUS_RETRY_MS));
}

void smb2_oneplus_heartbeat(struct smb_charger *chg)
{
	if (chg->oneplus)
	oneplus_policy_update(chg->oneplus);
}

void smb2_oneplus_enumeration(struct smb_charger *chg, bool configured)
{
	struct oneplus_smb2 *op = chg->oneplus;

	if (!op)
		return;
	mutex_lock(&op->policy_lock);
	op->usb_enumerated = configured;
	if (configured) {
		oem_fastchg_set_fast_chg_allow(false);
		cancel_delayed_work(&op->retry_work);
	} else if (op->usb_present) {
		schedule_delayed_work(&op->retry_work,
			msecs_to_jiffies(ONEPLUS_RETRY_MS));
	}
	mutex_unlock(&op->policy_lock);
}

int smb2_oneplus_batt_get_property(struct smb_charger *chg,
		enum power_supply_property prop, union power_supply_propval *val)
{
	struct oneplus_smb2 *op = chg->oneplus;

	if (!op)
		return -EOPNOTSUPP;
	if (prop == POWER_SUPPLY_PROP_PRESENT) {
		if (!oem_external_fg_registered())
			return -EOPNOTSUPP;
		val->intval = oem_external_fg_is_battery_present(true);
		return 0;
	}
	if (prop == POWER_SUPPLY_PROP_HEALTH) {
		if (!oem_external_fg_registered())
			return -EOPNOTSUPP;
		val->intval = oem_external_fg_get_batt_health(
			POWER_SUPPLY_HEALTH_UNKNOWN);
		return 0;
	}
	if (!oneplus_prop(prop))
		return -EOPNOTSUPP;

	switch (prop) {
	case POWER_SUPPLY_PROP_SET_ALLOW_READ_EXTERN_FG_IIC:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_CC_TO_CV_POINT:
		val->intval = op->vbatmax_mv[op->temp_region < BATT_TEMP_INVALID ?
			op->temp_region : BATT_TEMP_NORMAL];
		break;
	case POWER_SUPPLY_PROP_CHG_PROTECT_STATUS:
		if (op->chg_ovp)
			val->intval = 1;
		else if (!oem_external_fg_is_battery_present(true))
			val->intval = 2;
		else if (op->time_out)
			val->intval = 3;
		else if (op->batt_ovp)
			val->intval = 4;
		else if (op->temp_region == BATT_TEMP_HOT)
			val->intval = 5;
		else if (op->temp_region == BATT_TEMP_COLD)
			val->intval = 6;
		else if (op->temp_region == BATT_TEMP_LITTLE_COLD &&
			(op->chg->chg_done || op->recharge))
			val->intval = 7;
		else if (op->temp_region == BATT_TEMP_COOL &&
			(op->chg->chg_done || op->recharge))
			val->intval = 8;
		else if (op->temp_region == BATT_TEMP_WARM &&
			(op->chg->chg_done || op->recharge))
			val->intval = 9;
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_FASTCHG_STATUS:
		val->intval = op->dash_present;
		if (!val->intval &&
			(op->chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP ||
			op->chg->real_charger_type ==
				POWER_SUPPLY_TYPE_USB_HVDCP_3)) {
			int capacity = oneplus_bms_get(op,
				POWER_SUPPLY_PROP_CAPACITY, 0);

			val->intval = capacity >= 1 && capacity <= 85;
		}
		break;
	case POWER_SUPPLY_PROP_FASTCHG_STARTING:
		val->intval = op->dash_starting;
		break;
	case POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF:
		val->intval = op->lcd_off;
		break;
	case POWER_SUPPLY_PROP_CHECK_USB_UNPLUG:
		val->intval = !op->usb_present;
		break;
	case POWER_SUPPLY_PROP_OTG_SWITCH:
		val->intval = op->otg_switch;
		break;
	case POWER_SUPPLY_PROP_SWITCH_DASH:
		val->intval = oem_fastchg_get_fast_chg_allow();
		break;
	case POWER_SUPPLY_PROP_NOTIFY_CHARGER_SET_PARAMETER:
		val->intval = op->temp_region;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int smb2_oneplus_batt_set_property(struct smb_charger *chg,
		enum power_supply_property prop,
		const union power_supply_propval *val)
{
	struct oneplus_smb2 *op = chg->oneplus;

	if (!op || !oneplus_prop(prop))
		return -EOPNOTSUPP;

	switch (prop) {
	case POWER_SUPPLY_PROP_SET_ALLOW_READ_EXTERN_FG_IIC:
		return oem_external_fg_set_allow_reading(val->intval);
	case POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF:
		op->lcd_off = !!val->intval;
		return oem_external_fg_set_lcd_off_status(val->intval);
	case POWER_SUPPLY_PROP_OTG_SWITCH:
		return oneplus_set_otg_switch(op, !!val->intval);
	case POWER_SUPPLY_PROP_SWITCH_DASH:
		return oem_fastchg_set_fast_chg_allow(!!val->intval);
	case POWER_SUPPLY_PROP_NOTIFY_CHARGER_SET_PARAMETER:
		smb2_oneplus_heartbeat(chg);
		return 0;
	case POWER_SUPPLY_PROP_CC_TO_CV_POINT:
	case POWER_SUPPLY_PROP_CHG_PROTECT_STATUS:
	case POWER_SUPPLY_PROP_FASTCHG_STATUS:
	case POWER_SUPPLY_PROP_FASTCHG_STARTING:
	case POWER_SUPPLY_PROP_CHECK_USB_UNPLUG:
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

int fuelgauge_battery_temp_region_get(void)
{
	return oem_dash_get_temp_region(BATT_TEMP_INVALID);
}
EXPORT_SYMBOL(fuelgauge_battery_temp_region_get);

bool get_oem_charge_done_status(void)
{
	return oem_dash_get_charge_done(false);
}
EXPORT_SYMBOL(get_oem_charge_done_status);

int get_charging_status(void)
{
	return oem_dash_get_charging_status(POWER_SUPPLY_STATUS_UNKNOWN);
}
EXPORT_SYMBOL(get_charging_status);

#endif
