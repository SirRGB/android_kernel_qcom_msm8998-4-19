#ifndef	__OEM_EXTERNAL_FG_H__
#define __OEM_EXTERNAL_FG_H__
#include <linux/regmap.h>
#include <linux/input/qpnp-power-on.h>

/* Linux 4.19 removed the arm64 FIQ mask aliases used by the imported,
 * byte-immutable DASH adapter.  Preserve their 4.14 DAIF semantics here.
 */
#if defined(CONFIG_ARCH_ONEPLUS_MSM8998) && defined(CONFIG_ARM64)
#ifndef local_fiq_enable
#define local_fiq_enable() asm volatile("msr daifclr, #1" : : : "memory")
#define local_fiq_disable() asm volatile("msr daifset, #1" : : : "memory")
#endif
#endif
enum {
	ADAPTER_FW_UPDATE_NONE,
	ADAPTER_FW_NEED_UPDATE,
	ADAPTER_FW_UPDATE_SUCCESS,
	ADAPTER_FW_UPDATE_FAIL,
};
struct op_adapter_chip {
	int		timer_delay;
	bool	tx_byte_over;
	bool	rx_byte_over;
	bool	rx_timeout;
	unsigned long uart_tx_gpio;
	unsigned long uart_rx_gpio;
	char	*adapter_firmware_data;
	unsigned int	adapter_fw_data_count;
	unsigned int	tx_invalid_val;
	bool	adapter_update_ing;
	struct op_adapter_operations	*vops;
};
struct op_adapter_operations {
	bool (*adapter_update)(struct op_adapter_chip *chip,
			unsigned long tx_pin, unsigned long rx_pin);
};

struct external_battery_gauge {
	int (*get_battery_mvolts)(void);
	int (*get_battery_temperature)(void);
	bool (*is_battery_present)(void);
	bool (*is_battery_temp_within_range)(void);
	bool (*is_battery_id_valid)(void);
	bool (*is_usb_switch_on)(void);
	int (*get_battery_status)(void);
	int (*get_batt_design_capacity)(void);
	int (*get_batt_remaining_capacity)(void);
	int (*get_batt_full_chg_capacity)(void);
	int (*get_batt_health)(void);
	int (*get_batt_bq_soc)(void);
	int (*monitor_for_recharging)(void);
	int (*get_battery_soc)(void);
	int (*get_average_current)(void);
	int (*get_batt_cc)(void);
	/* yangfangbiao@oneplus.cn, 2015/02/13  Add fcc interface */
	int (*get_batt_fcc)(void);
	/* yangfangbiao@oneplus.cn, */
	/*2015/01/06  Modify for  sync with KK charge standard  */
	bool (*fast_chg_started)(void);
	bool (*fast_switch_to_normal)(void);
	int (*set_switch_to_noraml_false)(void);
	int (*set_fast_chg_allow)(bool enable);
	bool (*get_fast_chg_allow)(void);
	int (*fast_normal_to_warm)(void);
	int (*set_normal_to_warm_false)(void);
	int (*get_adapter_update)(void);
	bool (*get_fast_chg_ing)(void);
	bool (*get_fast_low_temp_full)(void);
	int (*set_low_temp_full_false)(void);
	int (*set_allow_reading)(int enable);
	int (*set_lcd_off_status)(int status);
	int (*fast_chg_started_status)(bool status);
	bool (*get_fastchg_firmware_already_updated)(void);
	int (*get_device_type)(void);
	/* david.liu@bsp, 20161025 Add BQ27411 dash charging */
};

struct notify_dash_event {
	int (*notify_event)(void);
	int (*op_contrl)(int status, bool check_power_ok);
	int (*notify_dash_charger_present)(int true);
};

struct notify_usb_enumeration_status {
	int (*notify_usb_enumeration)(int status);
};

/*
 * Context-aware interfaces used by in-tree providers.  The legacy records
 * above remain part of the downstream ABI and are adapted by the registry.
 */
struct oem_battery_gauge_ops {
	int (*get_battery_mvolts)(void *ctx);
	int (*get_battery_temperature)(void *ctx);
	bool (*is_battery_present)(void *ctx);
	bool (*is_battery_temp_within_range)(void *ctx);
	bool (*is_battery_id_valid)(void *ctx);
	bool (*is_usb_switch_on)(void *ctx);
	int (*get_battery_status)(void *ctx);
	int (*get_batt_design_capacity)(void *ctx);
	int (*get_batt_remaining_capacity)(void *ctx);
	int (*get_batt_full_chg_capacity)(void *ctx);
	int (*get_batt_health)(void *ctx);
	int (*get_batt_bq_soc)(void *ctx);
	int (*monitor_for_recharging)(void *ctx);
	int (*get_battery_soc)(void *ctx);
	int (*get_average_current)(void *ctx);
	int (*get_batt_cc)(void *ctx);
	int (*get_batt_fcc)(void *ctx);
	bool (*fast_chg_started)(void *ctx);
	bool (*fast_switch_to_normal)(void *ctx);
	int (*set_switch_to_noraml_false)(void *ctx);
	int (*set_fast_chg_allow)(void *ctx, bool enable);
	bool (*get_fast_chg_allow)(void *ctx);
	int (*fast_normal_to_warm)(void *ctx);
	int (*set_normal_to_warm_false)(void *ctx);
	int (*get_adapter_update)(void *ctx);
	bool (*get_fast_chg_ing)(void *ctx);
	bool (*get_fast_low_temp_full)(void *ctx);
	int (*set_low_temp_full_false)(void *ctx);
	int (*set_allow_reading)(void *ctx, int enable);
	int (*set_lcd_off_status)(void *ctx, int status);
	int (*fast_chg_started_status)(void *ctx, bool status);
	bool (*get_fastchg_firmware_already_updated)(void *ctx);
	int (*get_device_type)(void *ctx);
	int (*set_usb_switch)(void *ctx, bool enable);
	int (*reset_mcu)(void *ctx);
};

struct oem_dash_notify_ops {
	int (*notify_event)(void *ctx);
	int (*op_contrl)(void *ctx, int status, bool check_power_ok);
	int (*notify_dash_charger_present)(void *ctx, int present);
	int (*get_temp_region)(void *ctx);
	bool (*get_charge_done)(void *ctx);
	int (*get_charging_status)(void *ctx);
};

struct oem_usb_enumeration_ops {
	int (*notify_usb_enumeration)(void *ctx, int status);
};

struct oem_pon_soc_ops {
	int (*read)(void *ctx, u16 addr, u8 *val);
	int (*masked_write)(void *ctx, u16 addr, u8 mask, u8 val);
};

enum temp_region_type {
	BATT_TEMP_COLD = 0,
	BATT_TEMP_LITTLE_COLD,
	BATT_TEMP_COOL,
	BATT_TEMP_LITTLE_COOL,
	BATT_TEMP_PRE_NORMAL,
	BATT_TEMP_NORMAL,
	BATT_TEMP_WARM,
	BATT_TEMP_HOT,
	BATT_TEMP_INVALID,
};

enum batt_status_type {
	BATT_STATUS_GOOD,
	BATT_STATUS_BAD_TEMP, /* cold or hot */
	BATT_STATUS_BAD,
	BATT_STATUS_REMOVED, /* on v2.2 only */
	BATT_STATUS_INVALID_v1 = BATT_STATUS_REMOVED,
	BATT_STATUS_INVALID
};
//void op_pm8998_regmap_register(struct qpnp_pon *pon);

void regsister_notify_usb_enumeration_status(
	struct notify_usb_enumeration_status *event);
void notify_dash_unplug_register(struct notify_dash_event *event);
void notify_dash_unplug_unregister(struct notify_dash_event *event);
void fastcharge_information_unregister(struct external_battery_gauge *fast_chg);
void fastcharge_information_register(struct external_battery_gauge *fast_chg);
void external_battery_gauge_register(struct external_battery_gauge *batt_gauge);
void external_battery_gauge_unregister(
	struct external_battery_gauge *batt_gauge);
void bq27541_information_register(struct external_battery_gauge *fast_chg);
void bq27541_information_unregister(struct external_battery_gauge *fast_chg);

int external_battery_gauge_register_ctx(const struct oem_battery_gauge_ops *ops,
		void *ctx);
int external_battery_gauge_unregister_ctx(void *ctx);
int bq27541_information_register_ctx(const struct oem_battery_gauge_ops *ops,
		void *ctx);
int bq27541_information_unregister_ctx(void *ctx);
int fastcharge_information_register_ctx(const struct oem_battery_gauge_ops *ops,
		void *ctx);
int fastcharge_information_unregister_ctx(void *ctx);
int notify_dash_unplug_register_ctx(const struct oem_dash_notify_ops *ops,
		void *ctx);
int notify_dash_unplug_unregister_ctx(void *ctx);
int register_notify_usb_enumeration_status_ctx(
		const struct oem_usb_enumeration_ops *ops, void *ctx);
int unregister_notify_usb_enumeration_status_ctx(void *ctx);
void unregsister_notify_usb_enumeration_status(
		struct notify_usb_enumeration_status *event);
int oem_pon_soc_register(const struct oem_pon_soc_ops *ops, void *ctx);
int oem_pon_soc_unregister(void *ctx);

int oem_external_fg_get_battery_mvolts(int fallback);
int oem_external_fg_get_battery_temperature(int fallback);
int oem_external_fg_is_battery_present(bool fallback);
int oem_external_fg_is_battery_temp_within_range(bool fallback);
int oem_external_fg_is_battery_id_valid(bool fallback);
int oem_external_fg_get_batt_remaining_capacity(int fallback);
int oem_external_fg_get_batt_design_capacity(int fallback);
int oem_external_fg_get_batt_full_chg_capacity(int fallback);
int oem_external_fg_get_batt_health(int fallback);
int oem_external_fg_get_batt_bq_soc(int fallback);
int oem_external_fg_get_battery_soc(int fallback);
int oem_external_fg_get_average_current(int fallback);
int oem_external_fg_get_batt_cc(int fallback);
int oem_external_fg_get_batt_fcc(int fallback);
int oem_external_fg_set_allow_reading(int enable);
int oem_external_fg_set_lcd_off_status(int status);
int oem_external_fg_fast_chg_started_status(bool status);
int oem_external_fg_get_device_type(int fallback);
bool oem_external_fg_registered(void);

bool oem_bq_fast_chg_started(void);
bool oem_bq_fast_switch_to_normal(void);
int oem_bq_set_switch_to_noraml_false(void);
int oem_bq_set_fast_chg_allow(bool enable);
bool oem_bq_get_fast_chg_allow(void);
int oem_bq_get_adapter_update(int fallback);
bool oem_bq_get_fast_chg_ing(void);
bool oem_bq_get_fast_low_temp_full(void);
bool oem_bq_get_fastchg_firmware_already_updated(void);
bool oem_bq_is_usb_switch_on(void);
int oem_bq_get_battery_mvolts(int fallback);
int oem_bq_get_battery_temperature(int fallback);
int oem_bq_get_batt_remaining_capacity(int fallback);
int oem_bq_get_battery_soc(int fallback);
int oem_bq_get_average_current(int fallback);
int oem_bq_get_device_type(int fallback);

int oem_fastchg_set_fast_chg_allow(bool enable);
bool oem_fastchg_get_fast_chg_allow(void);
bool oem_fastchg_started(void);
bool oem_fastchg_switch_to_normal(void);
int oem_fastchg_set_switch_to_noraml_false(void);
bool oem_fastchg_get_fast_chg_ing(void);
bool oem_fastchg_get_fast_low_temp_full(void);
bool oem_fastchg_get_fastchg_firmware_already_updated(void);
int oem_fastchg_set_low_temp_full_false(void);
int oem_fastchg_get_adapter_update(int fallback);
int oem_fastchg_set_usb_switch(bool enable);
int oem_fastchg_reset_mcu(void);

int oem_dash_notify_event(void);
int oem_dash_op_contrl(int status, bool check_power_ok);
int oem_dash_notify_charger_present(int present);
int oem_dash_get_temp_region(int fallback);
bool oem_dash_get_charge_done(bool fallback);
int oem_dash_get_charging_status(int fallback);
int oem_usb_enumeration_notify(int status);
int oem_pon_soc_read(u16 addr, u8 *val);
int oem_pon_soc_masked_write(u16 addr, u8 mask, u8 val);
bool get_extern_fg_regist_done(void);
bool get_extern_bq_present(void);
int get_prop_pre_shutdown_soc(void);
extern int get_charging_status(void);
extern int fuelgauge_battery_temp_region_get(void);
extern bool get_oem_charge_done_status(void);
extern int load_soc(void);
extern void backup_soc_ex(int soc);
extern void clean_backup_soc_ex(void);
/*add for dash adapter update*/
extern bool dash_adapter_update_is_tx_gpio(unsigned long gpio_num);
extern bool dash_adapter_update_is_rx_gpio(unsigned long gpio_num);
#endif
