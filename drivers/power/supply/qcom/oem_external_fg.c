// SPDX-License-Identifier: GPL-2.0-only
/* Lifetime-safe OnePlus charging callback registry. */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power/oem_external_fg.h>
#include <linux/slab.h>
#include <linux/srcu.h>

#define OEM_SOC_DATA_REG	0x088d
#define OEM_SOC_VALID_BIT	BIT(7)
#define OEM_SOC_DATA_MASK	GENMASK(7, 1)
#define OEM_SOC_INVALID		0x7e

enum oem_provider_kind {
	OEM_PROVIDER_CONTEXT,
	OEM_PROVIDER_LEGACY,
};

struct oem_fg_provider {
	enum oem_provider_kind kind;
	const void *owner;
	void *ctx;
	union {
		const struct oem_battery_gauge_ops *ops;
		const struct external_battery_gauge *legacy;
	};
};

struct oem_dash_provider {
	enum oem_provider_kind kind;
	const void *owner;
	void *ctx;
	union {
		const struct oem_dash_notify_ops *ops;
		const struct notify_dash_event *legacy;
	};
};

struct oem_usb_provider {
	enum oem_provider_kind kind;
	const void *owner;
	void *ctx;
	union {
		const struct oem_usb_enumeration_ops *ops;
		const struct notify_usb_enumeration_status *legacy;
	};
};

struct oem_pon_provider {
	const void *owner;
	void *ctx;
	const struct oem_pon_soc_ops *ops;
};

#define OEM_SLOT(_name, _type) \
	static DEFINE_MUTEX(_name##_lock); \
	static DEFINE_STATIC_SRCU(_name##_srcu); \
	static struct _type __rcu *_name

OEM_SLOT(external_fg, oem_fg_provider);
OEM_SLOT(bq_data, oem_fg_provider);
OEM_SLOT(fastchg, oem_fg_provider);
OEM_SLOT(dash_notify, oem_dash_provider);
OEM_SLOT(usb_enum, oem_usb_provider);
OEM_SLOT(pon_soc, oem_pon_provider);

#define OEM_REGISTER(_slot, _provider) ({ \
	int __rc = 0; \
	mutex_lock(&_slot##_lock); \
	if (rcu_access_pointer(_slot)) { \
		__rc = -EBUSY; \
	} else { \
		rcu_assign_pointer(_slot, (_provider)); \
		(_provider) = NULL; \
	} \
	mutex_unlock(&_slot##_lock); \
	kfree(_provider); \
	__rc; \
})

#define OEM_UNREGISTER(_slot, _owner) ({ \
	typeof(_slot) __provider = NULL; \
	int __rc = 0; \
	mutex_lock(&_slot##_lock); \
	__provider = rcu_dereference_protected(_slot, \
			lockdep_is_held(&_slot##_lock)); \
	if (!__provider) \
		__rc = -ENOENT; \
	else if (__provider->owner != (_owner)) \
		__rc = -EINVAL; \
	else \
		RCU_INIT_POINTER(_slot, NULL); \
	mutex_unlock(&_slot##_lock); \
	if (!__rc) { \
		synchronize_srcu(&_slot##_srcu); \
		kfree(__provider); \
	} \
	__rc; \
})

static struct oem_fg_provider *oem_fg_provider_alloc(
		const struct oem_battery_gauge_ops *ops, void *ctx)
{
	struct oem_fg_provider *provider;

	if (!ops || !ctx)
		return ERR_PTR(-EINVAL);
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return ERR_PTR(-ENOMEM);
	provider->kind = OEM_PROVIDER_CONTEXT;
	provider->owner = ctx;
	provider->ctx = ctx;
	provider->ops = ops;
	return provider;
}

static struct oem_fg_provider *oem_legacy_fg_provider_alloc(
		const struct external_battery_gauge *ops)
{
	struct oem_fg_provider *provider;

	if (!ops)
		return ERR_PTR(-EINVAL);
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return ERR_PTR(-ENOMEM);
	provider->kind = OEM_PROVIDER_LEGACY;
	provider->owner = ops;
	provider->legacy = ops;
	return provider;
}

#define OEM_FG_CTX_REGISTER(_name, _slot) \
	int _name(const struct oem_battery_gauge_ops *ops, void *ctx) \
	{ \
		struct oem_fg_provider *provider = \
			oem_fg_provider_alloc(ops, ctx); \
		if (IS_ERR(provider)) \
			return PTR_ERR(provider); \
		return OEM_REGISTER(_slot, provider); \
	} \
	EXPORT_SYMBOL(_name)

#define OEM_FG_CTX_UNREGISTER(_name, _slot) \
	int _name(void *ctx) \
	{ \
		if (!ctx) \
			return -EINVAL; \
		return OEM_UNREGISTER(_slot, ctx); \
	} \
	EXPORT_SYMBOL(_name)

OEM_FG_CTX_REGISTER(external_battery_gauge_register_ctx, external_fg);
OEM_FG_CTX_UNREGISTER(external_battery_gauge_unregister_ctx, external_fg);
OEM_FG_CTX_REGISTER(bq27541_information_register_ctx, bq_data);
OEM_FG_CTX_UNREGISTER(bq27541_information_unregister_ctx, bq_data);
OEM_FG_CTX_REGISTER(fastcharge_information_register_ctx, fastchg);
OEM_FG_CTX_UNREGISTER(fastcharge_information_unregister_ctx, fastchg);

#define OEM_LEGACY_FG_REGISTER(_name, _slot) \
	void _name(struct external_battery_gauge *ops) \
	{ \
		struct oem_fg_provider *provider = \
			oem_legacy_fg_provider_alloc(ops); \
		if (!IS_ERR(provider)) \
			OEM_REGISTER(_slot, provider); \
	} \
	EXPORT_SYMBOL(_name)

#define OEM_LEGACY_FG_UNREGISTER(_name, _slot) \
	void _name(struct external_battery_gauge *ops) \
	{ \
		if (ops) \
			OEM_UNREGISTER(_slot, ops); \
	} \
	EXPORT_SYMBOL(_name)

OEM_LEGACY_FG_REGISTER(external_battery_gauge_register, external_fg);
OEM_LEGACY_FG_UNREGISTER(external_battery_gauge_unregister, external_fg);
OEM_LEGACY_FG_REGISTER(bq27541_information_register, bq_data);
OEM_LEGACY_FG_UNREGISTER(bq27541_information_unregister, bq_data);
OEM_LEGACY_FG_REGISTER(fastcharge_information_register, fastchg);
OEM_LEGACY_FG_UNREGISTER(fastcharge_information_unregister, fastchg);

#define OEM_FG_INT_HELPER(_name, _slot, _member) \
	int _name(int fallback) \
	{ \
		struct oem_fg_provider *p; \
		int idx, ret = fallback; \
		idx = srcu_read_lock(&_slot##_srcu); \
		p = srcu_dereference(_slot, &_slot##_srcu); \
		if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->_member) \
			ret = p->ops->_member(p->ctx); \
		else if (p && p->kind == OEM_PROVIDER_LEGACY && \
			 p->legacy->_member) \
			ret = p->legacy->_member(); \
		srcu_read_unlock(&_slot##_srcu, idx); \
		return ret; \
	} \
	EXPORT_SYMBOL(_name)

#define OEM_FG_BOOL_HELPER(_name, _slot, _member) \
	bool _name(void) \
	{ \
		struct oem_fg_provider *p; \
		int idx; \
		bool ret = false; \
		idx = srcu_read_lock(&_slot##_srcu); \
		p = srcu_dereference(_slot, &_slot##_srcu); \
		if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->_member) \
			ret = p->ops->_member(p->ctx); \
		else if (p && p->kind == OEM_PROVIDER_LEGACY && \
			 p->legacy->_member) \
			ret = p->legacy->_member(); \
		srcu_read_unlock(&_slot##_srcu, idx); \
		return ret; \
	} \
	EXPORT_SYMBOL(_name)

#define OEM_FG_BOOL_FALLBACK_HELPER(_name, _slot, _member) \
	int _name(bool fallback) \
	{ \
		struct oem_fg_provider *p; \
		int idx; \
		bool ret = fallback; \
		idx = srcu_read_lock(&_slot##_srcu); \
		p = srcu_dereference(_slot, &_slot##_srcu); \
		if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->_member) \
			ret = p->ops->_member(p->ctx); \
		else if (p && p->kind == OEM_PROVIDER_LEGACY && \
			 p->legacy->_member) \
			ret = p->legacy->_member(); \
		srcu_read_unlock(&_slot##_srcu, idx); \
		return ret; \
	} \
	EXPORT_SYMBOL(_name)

#define OEM_FG_SET_INT_HELPER(_name, _slot, _member) \
	int _name(int value) \
	{ \
		struct oem_fg_provider *p; \
		int idx, ret = -ENODEV; \
		idx = srcu_read_lock(&_slot##_srcu); \
		p = srcu_dereference(_slot, &_slot##_srcu); \
		if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->_member) \
			ret = p->ops->_member(p->ctx, value); \
		else if (p && p->kind == OEM_PROVIDER_LEGACY && \
			 p->legacy->_member) \
			ret = p->legacy->_member(value); \
		srcu_read_unlock(&_slot##_srcu, idx); \
		return ret; \
	} \
	EXPORT_SYMBOL(_name)

#define OEM_FG_SET_BOOL_HELPER(_name, _slot, _member) \
	int _name(bool value) \
	{ \
		struct oem_fg_provider *p; \
		int idx, ret = -ENODEV; \
		idx = srcu_read_lock(&_slot##_srcu); \
		p = srcu_dereference(_slot, &_slot##_srcu); \
		if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->_member) \
			ret = p->ops->_member(p->ctx, value); \
		else if (p && p->kind == OEM_PROVIDER_LEGACY && \
			 p->legacy->_member) \
			ret = p->legacy->_member(value); \
		srcu_read_unlock(&_slot##_srcu, idx); \
		return ret; \
	} \
	EXPORT_SYMBOL(_name)

OEM_FG_INT_HELPER(oem_external_fg_get_battery_mvolts, external_fg,
		get_battery_mvolts);
OEM_FG_INT_HELPER(oem_external_fg_get_battery_temperature, external_fg,
		get_battery_temperature);
OEM_FG_BOOL_FALLBACK_HELPER(oem_external_fg_is_battery_present, external_fg,
		is_battery_present);
OEM_FG_BOOL_FALLBACK_HELPER(oem_external_fg_is_battery_temp_within_range,
		external_fg, is_battery_temp_within_range);
OEM_FG_BOOL_FALLBACK_HELPER(oem_external_fg_is_battery_id_valid, external_fg,
		is_battery_id_valid);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_remaining_capacity, external_fg,
		get_batt_remaining_capacity);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_design_capacity, external_fg,
		get_batt_design_capacity);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_full_chg_capacity, external_fg,
		get_batt_full_chg_capacity);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_health, external_fg,
		get_batt_health);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_bq_soc, external_fg,
		get_batt_bq_soc);
OEM_FG_INT_HELPER(oem_external_fg_get_battery_soc, external_fg,
		get_battery_soc);
OEM_FG_INT_HELPER(oem_external_fg_get_average_current, external_fg,
		get_average_current);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_cc, external_fg, get_batt_cc);
OEM_FG_INT_HELPER(oem_external_fg_get_batt_fcc, external_fg, get_batt_fcc);
OEM_FG_SET_INT_HELPER(oem_external_fg_set_allow_reading, external_fg,
		set_allow_reading);
OEM_FG_SET_INT_HELPER(oem_external_fg_set_lcd_off_status, external_fg,
		set_lcd_off_status);
OEM_FG_SET_BOOL_HELPER(oem_external_fg_fast_chg_started_status, external_fg,
		fast_chg_started_status);
OEM_FG_INT_HELPER(oem_external_fg_get_device_type, external_fg,
		get_device_type);

bool oem_external_fg_registered(void)
{
	return rcu_access_pointer(external_fg) != NULL;
}
EXPORT_SYMBOL(oem_external_fg_registered);

OEM_FG_BOOL_HELPER(oem_bq_fast_chg_started, bq_data, fast_chg_started);
OEM_FG_BOOL_HELPER(oem_bq_fast_switch_to_normal, bq_data,
		fast_switch_to_normal);
OEM_FG_SET_BOOL_HELPER(oem_bq_set_fast_chg_allow, bq_data,
		set_fast_chg_allow);
OEM_FG_BOOL_HELPER(oem_bq_get_fast_chg_allow, bq_data, get_fast_chg_allow);
OEM_FG_INT_HELPER(oem_bq_get_adapter_update, bq_data, get_adapter_update);
OEM_FG_BOOL_HELPER(oem_bq_get_fast_chg_ing, bq_data, get_fast_chg_ing);
OEM_FG_BOOL_HELPER(oem_bq_get_fast_low_temp_full, bq_data,
		get_fast_low_temp_full);
OEM_FG_BOOL_HELPER(oem_bq_get_fastchg_firmware_already_updated, bq_data,
		get_fastchg_firmware_already_updated);
OEM_FG_BOOL_HELPER(oem_bq_is_usb_switch_on, bq_data, is_usb_switch_on);
OEM_FG_INT_HELPER(oem_bq_get_battery_mvolts, bq_data, get_battery_mvolts);
OEM_FG_INT_HELPER(oem_bq_get_battery_temperature, bq_data,
		get_battery_temperature);
OEM_FG_INT_HELPER(oem_bq_get_batt_remaining_capacity, bq_data,
		get_batt_remaining_capacity);
OEM_FG_INT_HELPER(oem_bq_get_battery_soc, bq_data, get_battery_soc);
OEM_FG_INT_HELPER(oem_bq_get_average_current, bq_data, get_average_current);
OEM_FG_INT_HELPER(oem_bq_get_device_type, bq_data, get_device_type);

int oem_bq_set_switch_to_noraml_false(void)
{
	struct oem_fg_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&bq_data_srcu);
	p = srcu_dereference(bq_data, &bq_data_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT &&
			p->ops->set_switch_to_noraml_false)
		ret = p->ops->set_switch_to_noraml_false(p->ctx);
	else if (p && p->kind == OEM_PROVIDER_LEGACY &&
			 p->legacy->set_switch_to_noraml_false)
		ret = p->legacy->set_switch_to_noraml_false();
	srcu_read_unlock(&bq_data_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_bq_set_switch_to_noraml_false);

OEM_FG_SET_BOOL_HELPER(oem_fastchg_set_fast_chg_allow, fastchg,
		set_fast_chg_allow);
OEM_FG_BOOL_HELPER(oem_fastchg_get_fast_chg_allow, fastchg,
		get_fast_chg_allow);
OEM_FG_BOOL_HELPER(oem_fastchg_started, fastchg, fast_chg_started);
OEM_FG_BOOL_HELPER(oem_fastchg_switch_to_normal, fastchg,
		fast_switch_to_normal);
OEM_FG_BOOL_HELPER(oem_fastchg_get_fast_chg_ing, fastchg, get_fast_chg_ing);
OEM_FG_BOOL_HELPER(oem_fastchg_get_fast_low_temp_full, fastchg,
		get_fast_low_temp_full);
OEM_FG_BOOL_HELPER(oem_fastchg_get_fastchg_firmware_already_updated,
		fastchg, get_fastchg_firmware_already_updated);
OEM_FG_INT_HELPER(oem_fastchg_get_adapter_update, fastchg,
		get_adapter_update);

int oem_fastchg_set_switch_to_noraml_false(void)
{
	struct oem_fg_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&fastchg_srcu);
	p = srcu_dereference(fastchg, &fastchg_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT &&
			p->ops->set_switch_to_noraml_false)
		ret = p->ops->set_switch_to_noraml_false(p->ctx);
	else if (p && p->kind == OEM_PROVIDER_LEGACY &&
			 p->legacy->set_switch_to_noraml_false)
		ret = p->legacy->set_switch_to_noraml_false();
	srcu_read_unlock(&fastchg_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_fastchg_set_switch_to_noraml_false);

int oem_fastchg_set_low_temp_full_false(void)
{
	struct oem_fg_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&fastchg_srcu);
	p = srcu_dereference(fastchg, &fastchg_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT &&
			p->ops->set_low_temp_full_false)
		ret = p->ops->set_low_temp_full_false(p->ctx);
	else if (p && p->kind == OEM_PROVIDER_LEGACY &&
			 p->legacy->set_low_temp_full_false)
		ret = p->legacy->set_low_temp_full_false();
	srcu_read_unlock(&fastchg_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_fastchg_set_low_temp_full_false);

int oem_fastchg_set_usb_switch(bool enable)
{
	struct oem_fg_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&fastchg_srcu);
	p = srcu_dereference(fastchg, &fastchg_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->set_usb_switch)
		ret = p->ops->set_usb_switch(p->ctx, enable);
	srcu_read_unlock(&fastchg_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_fastchg_set_usb_switch);

int oem_fastchg_reset_mcu(void)
{
	struct oem_fg_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&fastchg_srcu);
	p = srcu_dereference(fastchg, &fastchg_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->reset_mcu)
		ret = p->ops->reset_mcu(p->ctx);
	srcu_read_unlock(&fastchg_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_fastchg_reset_mcu);

int notify_dash_unplug_register_ctx(const struct oem_dash_notify_ops *ops,
		void *ctx)
{
	struct oem_dash_provider *provider;

	if (!ops || !ctx)
		return -EINVAL;
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	provider->kind = OEM_PROVIDER_CONTEXT;
	provider->owner = ctx;
	provider->ctx = ctx;
	provider->ops = ops;
	return OEM_REGISTER(dash_notify, provider);
}
EXPORT_SYMBOL(notify_dash_unplug_register_ctx);

int notify_dash_unplug_unregister_ctx(void *ctx)
{
	return ctx ? OEM_UNREGISTER(dash_notify, ctx) : -EINVAL;
}
EXPORT_SYMBOL(notify_dash_unplug_unregister_ctx);

void notify_dash_unplug_register(struct notify_dash_event *ops)
{
	struct oem_dash_provider *provider;

	if (!ops)
		return;
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return;
	provider->kind = OEM_PROVIDER_LEGACY;
	provider->owner = ops;
	provider->legacy = ops;
	OEM_REGISTER(dash_notify, provider);
}
EXPORT_SYMBOL(notify_dash_unplug_register);

void notify_dash_unplug_unregister(struct notify_dash_event *ops)
{
	if (ops)
		OEM_UNREGISTER(dash_notify, ops);
}
EXPORT_SYMBOL(notify_dash_unplug_unregister);

#define OEM_DASH_INVOKE(_member, _legacy_args, _ctx_args) ({ \
	struct oem_dash_provider *__p; \
	int __idx, __ret = -ENODEV; \
	__idx = srcu_read_lock(&dash_notify_srcu); \
	__p = srcu_dereference(dash_notify, &dash_notify_srcu); \
	if (__p && __p->kind == OEM_PROVIDER_CONTEXT && __p->ops->_member) \
		__ret = __p->ops->_member _ctx_args; \
	else if (__p && __p->kind == OEM_PROVIDER_LEGACY && \
		 __p->legacy->_member) \
		__ret = __p->legacy->_member _legacy_args; \
	srcu_read_unlock(&dash_notify_srcu, __idx); \
	__ret; \
})

int oem_dash_notify_event(void)
{
	return OEM_DASH_INVOKE(notify_event, (), (__p->ctx));
}
EXPORT_SYMBOL(oem_dash_notify_event);

int oem_dash_op_contrl(int status, bool check_power_ok)
{
	return OEM_DASH_INVOKE(op_contrl, (status, check_power_ok),
			(__p->ctx, status, check_power_ok));
}
EXPORT_SYMBOL(oem_dash_op_contrl);

int oem_dash_notify_charger_present(int present)
{
	return OEM_DASH_INVOKE(notify_dash_charger_present, (present),
			(__p->ctx, present));
}
EXPORT_SYMBOL(oem_dash_notify_charger_present);

int oem_dash_get_temp_region(int fallback)
{
	struct oem_dash_provider *p;
	int idx, ret = fallback;

	idx = srcu_read_lock(&dash_notify_srcu);
	p = srcu_dereference(dash_notify, &dash_notify_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->get_temp_region)
		ret = p->ops->get_temp_region(p->ctx);
	srcu_read_unlock(&dash_notify_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_dash_get_temp_region);

bool oem_dash_get_charge_done(bool fallback)
{
	struct oem_dash_provider *p;
	int idx;
	bool ret = fallback;

	idx = srcu_read_lock(&dash_notify_srcu);
	p = srcu_dereference(dash_notify, &dash_notify_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT && p->ops->get_charge_done)
		ret = p->ops->get_charge_done(p->ctx);
	srcu_read_unlock(&dash_notify_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_dash_get_charge_done);

int oem_dash_get_charging_status(int fallback)
{
	struct oem_dash_provider *p;
	int idx, ret = fallback;

	idx = srcu_read_lock(&dash_notify_srcu);
	p = srcu_dereference(dash_notify, &dash_notify_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT &&
		p->ops->get_charging_status)
		ret = p->ops->get_charging_status(p->ctx);
	srcu_read_unlock(&dash_notify_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_dash_get_charging_status);

int register_notify_usb_enumeration_status_ctx(
		const struct oem_usb_enumeration_ops *ops, void *ctx)
{
	struct oem_usb_provider *provider;

	if (!ops || !ctx)
		return -EINVAL;
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	provider->kind = OEM_PROVIDER_CONTEXT;
	provider->owner = ctx;
	provider->ctx = ctx;
	provider->ops = ops;
	return OEM_REGISTER(usb_enum, provider);
}
EXPORT_SYMBOL(register_notify_usb_enumeration_status_ctx);

int unregister_notify_usb_enumeration_status_ctx(void *ctx)
{
	return ctx ? OEM_UNREGISTER(usb_enum, ctx) : -EINVAL;
}
EXPORT_SYMBOL(unregister_notify_usb_enumeration_status_ctx);

void regsister_notify_usb_enumeration_status(
		struct notify_usb_enumeration_status *ops)
{
	struct oem_usb_provider *provider;

	if (!ops)
		return;
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return;
	provider->kind = OEM_PROVIDER_LEGACY;
	provider->owner = ops;
	provider->legacy = ops;
	OEM_REGISTER(usb_enum, provider);
}
EXPORT_SYMBOL(regsister_notify_usb_enumeration_status);

void unregsister_notify_usb_enumeration_status(
		struct notify_usb_enumeration_status *ops)
{
	if (ops)
		OEM_UNREGISTER(usb_enum, ops);
}
EXPORT_SYMBOL(unregsister_notify_usb_enumeration_status);

int oem_usb_enumeration_notify(int status)
{
	struct oem_usb_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&usb_enum_srcu);
	p = srcu_dereference(usb_enum, &usb_enum_srcu);
	if (p && p->kind == OEM_PROVIDER_CONTEXT &&
			p->ops->notify_usb_enumeration)
		ret = p->ops->notify_usb_enumeration(p->ctx, status);
	else if (p && p->kind == OEM_PROVIDER_LEGACY &&
			 p->legacy->notify_usb_enumeration)
		ret = p->legacy->notify_usb_enumeration(status);
	srcu_read_unlock(&usb_enum_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_usb_enumeration_notify);

int oem_pon_soc_register(const struct oem_pon_soc_ops *ops, void *ctx)
{
	struct oem_pon_provider *provider;

	if (!ops || !ctx || !ops->read || !ops->masked_write)
		return -EINVAL;
	provider = kzalloc(sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	provider->owner = ctx;
	provider->ctx = ctx;
	provider->ops = ops;
	return OEM_REGISTER(pon_soc, provider);
}
EXPORT_SYMBOL(oem_pon_soc_register);

int oem_pon_soc_unregister(void *ctx)
{
	return ctx ? OEM_UNREGISTER(pon_soc, ctx) : -EINVAL;
}
EXPORT_SYMBOL(oem_pon_soc_unregister);

int oem_pon_soc_read(u16 addr, u8 *val)
{
	struct oem_pon_provider *p;
	int idx, ret = -ENODEV;

	if (!val)
		return -EINVAL;
	idx = srcu_read_lock(&pon_soc_srcu);
	p = srcu_dereference(pon_soc, &pon_soc_srcu);
	if (p)
		ret = p->ops->read(p->ctx, addr, val);
	srcu_read_unlock(&pon_soc_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_pon_soc_read);

int oem_pon_soc_masked_write(u16 addr, u8 mask, u8 val)
{
	struct oem_pon_provider *p;
	int idx, ret = -ENODEV;

	idx = srcu_read_lock(&pon_soc_srcu);
	p = srcu_dereference(pon_soc, &pon_soc_srcu);
	if (p)
		ret = p->ops->masked_write(p->ctx, addr, mask, val);
	srcu_read_unlock(&pon_soc_srcu, idx);
	return ret;
}
EXPORT_SYMBOL(oem_pon_soc_masked_write);

int load_soc(void)
{
	u8 stored_soc;
	int rc, soc;

	rc = oem_pon_soc_read(OEM_SOC_DATA_REG, &stored_soc);
	if (rc || !(stored_soc & OEM_SOC_VALID_BIT))
		return -EINVAL;
	soc = stored_soc >> 1;
	return soc <= 100 ? soc : -EINVAL;
}
EXPORT_SYMBOL(load_soc);

void clean_backup_soc_ex(void)
{
	/* Clear validity first, then retain the established invalid payload. */
	oem_pon_soc_masked_write(OEM_SOC_DATA_REG, OEM_SOC_VALID_BIT, 0);
	oem_pon_soc_masked_write(OEM_SOC_DATA_REG, OEM_SOC_DATA_MASK,
			OEM_SOC_INVALID);
}
EXPORT_SYMBOL(clean_backup_soc_ex);

void backup_soc_ex(int soc)
{
	u8 stored;

	if (soc < 0 || soc > 100) {
		clean_backup_soc_ex();
		return;
	}
	stored = (u8)(soc * 2);
	oem_pon_soc_masked_write(OEM_SOC_DATA_REG, OEM_SOC_DATA_MASK,
			stored & OEM_SOC_DATA_MASK);
	oem_pon_soc_masked_write(OEM_SOC_DATA_REG, OEM_SOC_VALID_BIT,
			OEM_SOC_VALID_BIT);
}
EXPORT_SYMBOL(backup_soc_ex);

MODULE_DESCRIPTION("OnePlus charging callback registry");
MODULE_LICENSE("GPL v2");
