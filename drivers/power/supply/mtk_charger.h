/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_CHARGER_H
#define __MTK_CHARGER_H

#include <linux/alarmtimer.h>
#include "charger_class.h"
#include "adapter_class.h"
#include "mtk_charger_algorithm_class.h"
#include <linux/power_supply.h>
#include "mtk_smartcharging.h"

// drv add by tankaikun, add for screen on charging 20230108 start
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
#include "../../gpu/drm/mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../../gpu/drm/mediatek/mediatek_v2/mtk_disp_notify.h"
#endif
// drv add by tankaikun, add for screen on charging 20230108 end

#define CHARGING_INTERVAL 10
#define CHARGING_FULL_INTERVAL 20

#define CHRLOG_ERROR_LEVEL	1
#define CHRLOG_INFO_LEVEL	2
#define CHRLOG_DEBUG_LEVEL	3

#define SC_TAG "smartcharging"

extern int chr_get_debug_level(void);

#define chr_err(fmt, args...)					\
do {								\
	if (chr_get_debug_level() >= CHRLOG_ERROR_LEVEL) {	\
		pr_notice(fmt, ##args);				\
	}							\
} while (0)

#define chr_info(fmt, args...)					\
do {								\
	if (chr_get_debug_level() >= CHRLOG_INFO_LEVEL) {	\
		pr_notice_ratelimited(fmt, ##args);		\
	}							\
} while (0)

#define chr_debug(fmt, args...)					\
do {								\
	if (chr_get_debug_level() >= CHRLOG_DEBUG_LEVEL) {	\
		pr_notice(fmt, ##args);				\
	}							\
} while (0)

struct mtk_charger;
struct charger_data;
#define BATTERY_CV 4350000
#define V_CHARGER_MAX 6500000 /* 6.5 V */
#define V_CHARGER_MIN 4600000 /* 4.6 V */
#define VBUS_OVP_VOLTAGE 15000000 /* 15V */

#define USB_CHARGER_CURRENT_SUSPEND		0 /* def CONFIG_USB_IF */
#define USB_CHARGER_CURRENT_UNCONFIGURED	70000 /* 70mA */
#define USB_CHARGER_CURRENT_CONFIGURED		500000 /* 500mA */
#define USB_CHARGER_CURRENT			500000 /* 500mA */
#define AC_CHARGER_CURRENT			2050000
#define AC_CHARGER_INPUT_CURRENT		3200000
#define NON_STD_AC_CHARGER_CURRENT		500000
#define CHARGING_HOST_CHARGER_CURRENT		650000

/* dynamic mivr */
#define V_CHARGER_MIN_1 4400000 /* 4.4 V */
#define V_CHARGER_MIN_2 4200000 /* 4.2 V */
#define MAX_DMIVR_CHARGER_CURRENT 1800000 /* 1.8 A */

/* battery warning */
#define BATTERY_NOTIFY_CASE_0001_VCHARGER
#define BATTERY_NOTIFY_CASE_0002_VBATTEMP

/* charging abnormal status */
#define CHG_VBUS_OV_STATUS	(1 << 0)
#define CHG_BAT_OT_STATUS	(1 << 1)
#define CHG_OC_STATUS		(1 << 2)
#define CHG_BAT_OV_STATUS	(1 << 3)
#define CHG_ST_TMO_STATUS	(1 << 4)
#define CHG_BAT_LT_STATUS	(1 << 5)
#define CHG_TYPEC_WD_STATUS	(1 << 6)
#define CHG_DPDM_OV_STATUS	(1 << 7)

/* Battery Temperature Protection */
#define MIN_CHARGE_TEMP  0
#define MIN_CHARGE_TEMP_PLUS_X_DEGREE	6
#define MAX_CHARGE_TEMP  50
#define MAX_CHARGE_TEMP_MINUS_X_DEGREE	47

#define MAX_ALG_NO 10

#define RESET_BOOT_VOLT_TIME 50

// drv add tankaikun, add battery temp debug, 20231212 start
#define MTK_DEBUG_TEMP_EN_CMD		0xb5b
#define MTK_DEBUG_TEMP_DIS_CMD		0x5b5
// drv add tankaikun, battery temp debug, 20231212 end

// drv add by tankaikun, add for screen on charging 20230108 start
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
extern bool g_charge_is_screen_on;
#endif
// drv add by tankaikun, add for screen on charging 20230108 end

/* prize add by fangduozhu 20250707, low battery temp notify start */
#define BAT_LOW_TEMP_PROTECT_ENABLE
/* prize add by fangduozhu 20250707, low battery temp notify end */

enum bat_temp_state_enum {
	BAT_TEMP_LOW = 0,
	BAT_TEMP_NORMAL,
	BAT_TEMP_HIGH
};

enum TA_STATE {
	TA_INIT_FAIL,
	TA_CHECKING,
	TA_NOT_SUPPORT,
	TA_NOT_READY,
	TA_READY,
	TA_PD_PPS_READY,
};

enum adapter_protocol_state {
	FIRST_HANDSHAKE,
	RUN_ON_PD,
	RUN_ON_UFCS,
};

enum TA_CAP_STATE {
	APDO_TA,
	WO_APDO_TA,
	STD_TA,
	ONLY_APDO_TA,
};
enum chg_dev_notifier_events {
	EVENT_FULL,
	EVENT_RECHARGE,
	EVENT_DISCHARGE,
};

struct battery_thermal_protection_data {
	int sm;
	bool enable_min_charge_temp;
	int min_charge_temp;
	int min_charge_temp_plus_x_degree;
	int max_charge_temp;
	int max_charge_temp_minus_x_degree;
};

/* sw jeita */
#define JEITA_TEMP_ABOVE_T4_CV	4240000
#define JEITA_TEMP_T3_TO_T4_CV	4240000
#define JEITA_TEMP_T2_TO_T3_CV	4340000
#define JEITA_TEMP_T1_TO_T2_CV	4240000
#define JEITA_TEMP_T0_TO_T1_CV	4040000
#define JEITA_TEMP_BELOW_T0_CV	4040000
#define TEMP_T4_THRES  50
#define TEMP_T4_THRES_MINUS_X_DEGREE 47
#define TEMP_T3_THRES  45
#define TEMP_T3_THRES_MINUS_X_DEGREE 39
#define TEMP_T2_THRES  10
#define TEMP_T2_THRES_PLUS_X_DEGREE 16
#define TEMP_T1_THRES  0
#define TEMP_T1_THRES_PLUS_X_DEGREE 6
#define TEMP_T0_THRES  0
#define TEMP_T0_THRES_PLUS_X_DEGREE  0
#define TEMP_NEG_10_THRES 0

/*
 * Software JEITA
 * T0: -10 degree Celsius
 * T1: 0 degree Celsius
 * T2: 10 degree Celsius
 * T3: 45 degree Celsius
 * T4: 50 degree Celsius
 */

enum sw_jeita_state_enum {
	TEMP_BELOW_T0 = 0,
	TEMP_T0_TO_T1,
	TEMP_T1_TO_T2,
	TEMP_T2_TO_T3,
	TEMP_T3_TO_T4,
	TEMP_ABOVE_T4
};

struct info_notifier_block {
	struct notifier_block nb;
	struct mtk_charger *info;
};

struct sw_jeita_data {
	int sm;
	int pre_sm;
	int cv;
	bool charging;
	bool error_recovery_flag;
};

struct mtk_charger_algorithm {

	int (*do_algorithm)(struct mtk_charger *info);
	int (*enable_charging)(struct mtk_charger *info, bool en);
	int (*do_event)(struct notifier_block *nb, unsigned long ev, void *v);
	int (*do_dvchg1_event)(struct notifier_block *nb, unsigned long ev,
			       void *v);
	int (*do_dvchg2_event)(struct notifier_block *nb, unsigned long ev,
			       void *v);
	int (*do_hvdvchg1_event)(struct notifier_block *nb, unsigned long ev,
			       void *v);
	int (*do_hvdvchg2_event)(struct notifier_block *nb, unsigned long ev,
			       void *v);
	int (*change_current_setting)(struct mtk_charger *info);
	void *algo_data;
};

struct charger_custom_data {
	int battery_cv;	/* uv */
	int max_charger_voltage;
	int max_charger_voltage_setting;
	int min_charger_voltage;
	int vbus_sw_ovp_voltage;

	int usb_charger_current;
	int ac_charger_current;
	int ac_charger_input_current;
	int charging_host_charger_current;

	/* sw jeita */
	int jeita_temp_above_t4_cv;
	int jeita_temp_t3_to_t4_cv;
	int jeita_temp_t2_to_t3_cv;
	int jeita_temp_t1_to_t2_cv;
	int jeita_temp_t0_to_t1_cv;
	int jeita_temp_below_t0_cv;
	int temp_t4_thres;
	int temp_t4_thres_minus_x_degree;
	int temp_t3_thres;
	int temp_t3_thres_minus_x_degree;
	int temp_t2_thres;
	int temp_t2_thres_plus_x_degree;
	int temp_t1_thres;
	int temp_t1_thres_plus_x_degree;
	int temp_t0_thres;
	int temp_t0_thres_plus_x_degree;
	int temp_neg_10_thres;

	/* battery temperature protection */
	int mtk_temperature_recharge_support;
	int max_charge_temp;
	int max_charge_temp_minus_x_degree;
	int min_charge_temp;
	int min_charge_temp_plus_x_degree;

	/* dynamic mivr */
	int min_charger_voltage_1;
	int min_charger_voltage_2;
	int max_dmivr_charger_current;

};

struct charger_data {
	int input_current_limit;
	int charging_current_limit;

	int force_charging_current;
	int thermal_input_current_limit;
	int thermal_charging_current_limit;
	bool thermal_throttle_record;
	int disable_charging_count;
	int input_current_limit_by_aicl;
	int junction_temp_min;
	int junction_temp_max;
};

// drv add tankaikun, add facoryt charger class, 20231204 start
enum mtk_charge_type {
	MTK_CHARGER_TYPE_UNKNOWN,
	MTK_CHARGER_TYPE_SDP,
	MTK_CHARGER_TYPE_CDP,
	MTK_CHARGER_TYPE_DCP,
	MTK_CHARGER_TYPE_WL_BPP,
	MTK_CHARGER_TYPE_WL_EPP,
};

enum mtk_fast_charge_type {
	MTK_FAST_CHARGER_TYPE_UNKNOWN = 0,
	MTK_FAST_CHARGER_TYPE_PEP,
	MTK_FAST_CHARGER_TYPE_PE20,
	MTK_FAST_CHARGER_TYPE_PDC,
	MTK_FAST_CHARGER_TYPE_PE40,
	MTK_FAST_CHARGER_TYPE_PE50,
	MTK_FAST_CHARGER_TYPE_HVBP,
	MTK_FAST_CHARGER_TYPE_PE5P,
	MTK_FAST_CHARGER_TYPE_WIRELESS_FAST,
	MTK_FAST_CHARGER_TYPE_MAX,
};

struct mtk_fast_chg_type_map {
	int fast_chg_type;
	int fast_chrg_id;
};

static const char * const mtk_chg_type_name_list[] = {
	[MTK_CHARGER_TYPE_UNKNOWN] = "Unknown",
	[MTK_CHARGER_TYPE_SDP] = "USB_SDP",
	[MTK_CHARGER_TYPE_CDP] = "USB_CDP",
	[MTK_CHARGER_TYPE_DCP] = "USB_DCP",
	[MTK_CHARGER_TYPE_WL_BPP] = "WIRELESS_BPP",
	[MTK_CHARGER_TYPE_WL_EPP] = "WIRELESS_EPP",
};

static const char * const mtk_fast_chg_algo_list[] = {
	[MTK_FAST_CHARGER_TYPE_UNKNOWN] = "Unknown",
	[MTK_FAST_CHARGER_TYPE_PEP] = "PE+",
	[MTK_FAST_CHARGER_TYPE_PE20] = "PE20",
	[MTK_FAST_CHARGER_TYPE_PDC] = "PDC",
	[MTK_FAST_CHARGER_TYPE_PE40] = "PE40",
	[MTK_FAST_CHARGER_TYPE_PE50] = "PE50",
	[MTK_FAST_CHARGER_TYPE_HVBP] = "HVBP",
	[MTK_FAST_CHARGER_TYPE_PE5P] = "PE5P",
	[MTK_FAST_CHARGER_TYPE_WIRELESS_FAST] = "WIRELESS_FAST_CHARGER",
	[MTK_FAST_CHARGER_TYPE_MAX] = "ERROR",
};
// drv add tankaikun, add facoryt charger class, 20231204 start

enum chg_data_idx_enum {
	CHG1_SETTING,
	CHG2_SETTING,
	DVCHG1_SETTING,
	DVCHG2_SETTING,
	HVDVCHG1_SETTING,
	HVDVCHG2_SETTING,
	CHGS_SETTING_MAX,
};

// drv add tankaikun, add step charging, 20231130 start
#define MAX_NUM_STEPS 10

enum cs_chrg_steps {
	STEP_FIRST = 0,
	STEP_LAST = MAX_NUM_STEPS + STEP_FIRST - 1,
	STEP_NONE = 0xFF,
};

enum cs_chrg_temp_zones {
	ZONE_FIRST = 0,
	/* states 0-9 are reserved for zones */
	ZONE_LAST = MAX_NUM_STEPS + ZONE_FIRST - 1,
	ZONE_HOT,
	ZONE_COLD,
	ZONE_NONE = 0xFF,
};

struct cs_chrg_step_power {
	u32 chrg_step_volt;
	u32 chrg_step_curr;
};

struct cs_chrg_temp_zone {
	int	temp_c;	/*temperature*/
	struct	cs_chrg_step_power *chrg_step_power;
};

struct cs_chrg_step_info {
	enum cs_chrg_steps	pres_chrg_step;
	int	temp_c;
	int	chrg_step_cc_curr;
	int	chrg_step_cv_volt;
	int	chrg_step_cv_tapper_curr;
	bool	last_step;
};

struct cs_chrg_ffc_zone {
	int	temp_c;	/*temperature*/
	int ffc_max_mv;
	int ffc_chg_iterm;
};

struct cs_chrg_thermal_zone {
	int	temp_c;	/*temperature*/
	int temp_hyst_c;
	int chrg_limit_curr;
};

enum cs_chrg_thermal_zones {
	THERM_ZONE_FIRST = 0,
	/* states 0-9 are reserved for zones */
	THERM_ZONE_LAST = MAX_NUM_STEPS + THERM_ZONE_FIRST - 1,
	THERM_ZONE_NONE = 0xFF,
};

// drv add tankaikun, add step charging, 20231130 end

struct mtk_charger {
	struct platform_device *pdev;
	struct charger_device *chg1_dev;
	struct notifier_block chg1_nb;
	struct charger_device *chg2_dev;
	struct charger_device *dvchg1_dev;
	struct notifier_block dvchg1_nb;
	struct charger_device *dvchg2_dev;
	struct notifier_block dvchg2_nb;
	struct charger_device *hvdvchg1_dev;
	struct notifier_block hvdvchg1_nb;
	struct charger_device *hvdvchg2_dev;
	struct notifier_block hvdvchg2_nb;
	struct charger_device *bkbstchg_dev;
	struct notifier_block bkbstchg_nb;
	// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
	struct charger_device *wlchg1_dev;
	struct notifier_block wlchg1_nb;
	// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end

	struct charger_data chg_data[CHGS_SETTING_MAX];
	struct chg_limit_setting setting;
	enum charger_configuration config;

	struct power_supply_desc psy_desc1;
	struct power_supply_config psy_cfg1;
	struct power_supply *psy1;

	struct power_supply_desc psy_desc2;
	struct power_supply_config psy_cfg2;
	struct power_supply *psy2;

	struct power_supply_desc psy_dvchg_desc1;
	struct power_supply_config psy_dvchg_cfg1;
	struct power_supply *psy_dvchg1;

	struct power_supply_desc psy_dvchg_desc2;
	struct power_supply_config psy_dvchg_cfg2;
	struct power_supply *psy_dvchg2;

	struct power_supply_desc psy_hvdvchg_desc1;
	struct power_supply_config psy_hvdvchg_cfg1;
	struct power_supply *psy_hvdvchg1;

	struct power_supply_desc psy_hvdvchg_desc2;
	struct power_supply_config psy_hvdvchg_cfg2;
	struct power_supply *psy_hvdvchg2;

	struct power_supply  *chg_psy;
	struct power_supply  *bc12_psy;
	struct power_supply  *bat_psy;
	struct adapter_device *select_adapter;
	struct adapter_device *pd_adapter;
	struct adapter_device *adapter_dev[MAX_TA_IDX];
	struct notifier_block *nb_addr;
	struct info_notifier_block ta_nb[MAX_TA_IDX];
	struct adapter_device *ufcs_adapter;
	struct mutex pd_lock;
	struct mutex ufcs_lock;
	struct mutex ta_lock;

	u32 bootmode;
	u32 boottype;

	int ta_status[MAX_TA_IDX];
	int select_adapter_idx;
	int chr_type;
	int usb_type;
	int usb_state;
	int adapter_priority;

	struct mutex cable_out_lock;
	int cable_out_cnt;

	/* system lock */
	spinlock_t slock;
	struct wakeup_source *charger_wakelock;
	struct mutex charger_lock;

	/* thread related */
	wait_queue_head_t  wait_que;
	bool charger_thread_timeout;
	unsigned int polling_interval;
	bool charger_thread_polling;

	/* alarm timer */
	struct alarm charger_timer;
	struct timespec64 endtime;
	bool is_suspend;
	struct notifier_block pm_notifier;

	/* notify charger user */
	struct srcu_notifier_head evt_nh;

	/* common info */
	int log_level;
	bool usb_unlimited;
	bool charger_unlimited;
	bool disable_charger;
	bool disable_aicl;
	int battery_temp;
	bool can_charging;
	bool cmd_discharging;
	// drv add by wanwen, add for charge power limit, 20250610 start
	bool cmd_bypass_charging;
	// drv add by wanwen, add for charge power limit, 20250610 end
	// drv add by linaiyu, add for bypass charge power limit, 20250630 start
	int cmd_charge_power_limit;
	// drv add by linaiyu, add for bypass charge power limit, 20250630 end
	bool safety_timeout;
	int safety_timer_cmd;
	bool vbusov_stat;
	bool dpdmov_stat;
	bool lst_dpdmov_stat;
	bool is_chg_done;
	/* ATM */
	bool atm_enabled;

	const char *algorithm_name;
	struct mtk_charger_algorithm algo;

	/* dtsi custom data */
	struct charger_custom_data data;

	/* battery warning */
	unsigned int notify_code;
	unsigned int notify_test_mode;

	/* sw safety timer */
	bool enable_sw_safety_timer;
	bool sw_safety_timer_setting;
	struct timespec64 charging_begin_time;

	/* vbat monitor, 6pin bat */
	bool batpro_done;
	bool enable_vbat_mon;
	bool enable_vbat_mon_bak;
	int old_cv;
	bool stop_6pin_re_en;
	int vbat0_flag;

	/* sw jeita */
	bool enable_sw_jeita;
	struct sw_jeita_data sw_jeita;

	/* battery thermal protection */
	struct battery_thermal_protection_data thermal;

	struct chg_alg_device *alg[MAX_ALG_NO];
	int lst_rnd_alg_idx;
	bool alg_new_arbitration;
	bool alg_unchangeable;
	struct notifier_block chg_alg_nb;
	bool enable_hv_charging;

	/* water detection */
	bool water_detected;
	bool record_water_detected;

	int cc_hi;

	bool enable_dynamic_mivr;

	/* fast charging algo support indicator */
	bool enable_fast_charging_indicator;
	unsigned int fast_charging_indicator;

	/* diasable meta current limit for testing */
	unsigned int enable_meta_current_limit;

	struct smartcharging sc;

	/*daemon related*/
	struct sock *daemo_nl_sk;
	u_int g_scd_pid;
	struct scd_cmd_param_t_1 sc_data;

	/*charger IC charging status*/
	bool is_charging;

	// drv add tankaikun, add step charging, 20231130 start
	/* battery jeita control */
	int num_temp_zones;
	struct cs_chrg_temp_zone *temp_zones;
	enum cs_chrg_temp_zones	pres_temp_zone;
	int	chrg_step_nums;
	struct cs_chrg_step_info chrg_step;
	bool temp_zone_change;
	bool chrg_step_change;

	/* ffc charging control */
	int num_ffc_zones;
	struct cs_chrg_ffc_zone *ffc_zones;
	int chrg_iterm;
	// drv add tankaikun, add step charging, 20231130 end
	// drv add tanakikun, add facoryt charger class, 20231204

	//  drv add tankaikun, add battery temp debug, 20231220 start
	bool debug_temp_en;
	int debug_temp;
	// drv add tankaikun, add battery temp debug, 20231220 end
// drv add by tankaikun, add for screen on charging 20230108 start
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
	struct notifier_block disp_notifier;
#endif
// drv add by tankaikun, add for screen on charging 20230108 end
	// drv add by tankaikun, add for fast power show, 20240218 start
	unsigned int max_pwr;
	// drv add by tankaikun, add for fast power show, 20240218 end

// drv add tankaikun, thermal current limit, 20241015 start
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
	/* battery ntc: thermal charing limit */
	int num_thermal_zones;
	int pres_therm_zone;
	int chrg_therm_limit_curr;
	struct cs_chrg_thermal_zone *therm_zones;

	int num_wireless_thermal_zones;
	int pres_wireless_therm_zone;
	int chrg_wireless_therm_limit_curr;
	struct cs_chrg_thermal_zone *wireless_therm_zones;

	/* userspace control */
	int thermal_input_current_limit;
	int thermal_charging_current_limit;
#endif /* CONFIG_ODM_CHARGER_CLASS_SUPPORT */
// drv add tankaikun, thermal current limit, 20241015 end
	ktime_t uevent_time_check;

	bool force_disable_pp[CHG2_SETTING + 1];
	bool enable_pp[CHG2_SETTING + 1];
	struct mutex pp_lock[CHG2_SETTING + 1];
	int cmd_pp;

	/* enable boot volt*/
	bool enable_boot_volt;
	bool reset_boot_volt_times;
	/* adapter switch control */
	int protocol_state;
	int ta_capability;
	int wait_times;

    // Add for user fast charge control
    bool user_fast_charge_enabled;
};

static inline int mtk_chg_alg_notify_call(struct mtk_charger *info,
					  enum chg_alg_notifier_events evt,
					  int value)
{
	int i;
	struct chg_alg_notify notify = {
		.evt = evt,
		.value = value,
	};

	for (i = 0; i < MAX_ALG_NO; i++) {
		if (info->alg[i])
			chg_alg_notifier_call(info->alg[i], &notify);
	}
	return 0;
}

/* functions which framework needs*/
extern int mtk_basic_charger_init(struct mtk_charger *info);
extern int mtk_pulse_charger_init(struct mtk_charger *info);
extern int get_uisoc(struct mtk_charger *info);
extern int get_battery_voltage(struct mtk_charger *info);
extern int get_battery_temperature(struct mtk_charger *info);
extern int get_battery_current(struct mtk_charger *info);
extern int get_vbus(struct mtk_charger *info);
extern int get_ibat(struct mtk_charger *info);
extern int get_ibus(struct mtk_charger *info);
extern bool is_battery_exist(struct mtk_charger *info);
extern int get_charger_type(struct mtk_charger *info);
extern int get_usb_type(struct mtk_charger *info);
extern int disable_hw_ovp(struct mtk_charger *info, int en);
extern bool is_charger_exist(struct mtk_charger *info);
extern int get_charger_temperature(struct mtk_charger *info,
	struct charger_device *chg);
extern int get_charger_charging_current(struct mtk_charger *info,
	struct charger_device *chg);
extern int get_charger_input_current(struct mtk_charger *info,
	struct charger_device *chg);
extern int get_charger_zcv(struct mtk_charger *info,
	struct charger_device *chg);
extern void _wake_up_charger(struct mtk_charger *info);
extern int mtk_adapter_switch_control(struct mtk_charger *info);
extern int mtk_selected_adapter_ready(struct mtk_charger *info);
extern int mtk_adapter_protocol_init(struct mtk_charger *info);
extern void mtk_check_ta_status(struct mtk_charger *info);

/* functions for other */
extern int mtk_chg_enable_vbus_ovp(bool enable);
/* pri LAX10-339 add by lvyuanchuan 20240325 */
extern int mtk_chg_set_vbus_ovp(bool enable, int alg_id, int ovp);
enum attach_type {
	ATTACH_TYPE_NONE,
	ATTACH_TYPE_PWR_RDY,
	ATTACH_TYPE_TYPEC,
	ATTACH_TYPE_PD,
	ATTACH_TYPE_PD_SDP,
	ATTACH_TYPE_PD_DCP,
	ATTACH_TYPE_PD_NONSTD,
	ATTACH_TYPE_MAX,
};

#define ONLINE(idx, attach)		((idx & 0xf) << 4 | (attach & 0xf))
#define ONLINE_GET_IDX(online)		((online >> 4) & 0xf)
#define ONLINE_GET_ATTACH(online)	(online & 0xf)

#endif /* __MTK_CHARGER_H */
