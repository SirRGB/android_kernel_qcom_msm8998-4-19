/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SMB2_ONEPLUS_H
#define __SMB2_ONEPLUS_H

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>

struct smb_charger;

#if IS_ENABLED(CONFIG_ARCH_ONEPLUS_MSM8998)
int smb2_oneplus_init(struct smb_charger *chg);
void smb2_oneplus_deinit(struct smb_charger *chg);
int smb2_oneplus_batt_get_property(struct smb_charger *chg,
		enum power_supply_property prop, union power_supply_propval *val);
int smb2_oneplus_batt_set_property(struct smb_charger *chg,
		enum power_supply_property prop,
		const union power_supply_propval *val);
void smb2_oneplus_usb_event(struct smb_charger *chg, bool present);
void smb2_oneplus_switcher_power(struct smb_charger *chg);
void smb2_oneplus_heartbeat(struct smb_charger *chg);
void smb2_oneplus_enumeration(struct smb_charger *chg, bool configured);
#else
static inline int smb2_oneplus_init(struct smb_charger *chg)
{
	return 0;
}

static inline void smb2_oneplus_deinit(struct smb_charger *chg) { }

static inline int smb2_oneplus_batt_get_property(struct smb_charger *chg,
		enum power_supply_property prop, union power_supply_propval *val)
{
	return -EINVAL;
}

static inline int smb2_oneplus_batt_set_property(struct smb_charger *chg,
		enum power_supply_property prop,
		const union power_supply_propval *val)
{
	return -EINVAL;
}

static inline void smb2_oneplus_usb_event(struct smb_charger *chg,
		bool present) { }
static inline void smb2_oneplus_switcher_power(struct smb_charger *chg) { }
static inline void smb2_oneplus_heartbeat(struct smb_charger *chg) { }
static inline void smb2_oneplus_enumeration(struct smb_charger *chg,
		bool configured) { }
#endif

#endif
