// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

/*
 *
 * Filename:
 * ---------
 *    mtk_basic_charger.c
 *
 * Project:
 * --------
 *   Android_Software
 *
 * Description:
 * ------------
 *   This Module defines functions of Battery charging
 *
 * Author:
 * -------
 * Wy Chuang
 *
 */
#include <linux/init.h>		/* For init/exit macros */
#include <linux/module.h>	/* For MODULE_ marcros  */
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>
#include <linux/time.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/scatterlist.h>
#include <linux/suspend.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/reboot.h>

#include "mtk_charger.h"

static int _uA_to_mA(int uA)
{
	if (uA == -1)
		return -1;
	else
		return uA / 1000;
}

// drv add tankaikun, add ffc charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_FFC_CHARGE)
static int cs_get_ffc_fv(struct mtk_charger *info, int temp_c)
{
	int ffc_max_fv;
	int i = 0;
	int temp = temp_c;
	int num_zones;
	struct cs_chrg_ffc_zone *zone;

	num_zones = info->num_ffc_zones;
	zone = info->ffc_zones;
	while (i < num_zones && temp > zone[i++].temp_c);
	zone = i > 0? &zone[i - 1] : NULL;

	info->chrg_iterm = zone->ffc_chg_iterm;
	ffc_max_fv = zone->ffc_max_mv*1000;
	pr_info("[cs_chrg] FFC temp zone %d, fv %d mV, chg iterm %d mA\n",
		  i, ffc_max_fv, info->chrg_iterm);

	return ffc_max_fv;
}
#endif /* CONFIG_CHARGER_FFC_CHARGE */
// drv add tankaikun, add ffc charging, 20231130 end

static void select_cv(struct mtk_charger *info)
{
	u32 constant_voltage;
// drv add tankaikun, add step charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_FFC_CHARGE)
	int ffc_max_fv;
	int ta_type;
#endif /* CONFIG_CHARGER_STEP_CHARGE */

	if (info->enable_sw_jeita)
		if (info->sw_jeita.cv != 0) {
			info->setting.cv = info->sw_jeita.cv;
			return;
		}

	constant_voltage = info->data.battery_cv;
	info->setting.cv = constant_voltage;
#if IS_ENABLED(CONFIG_CHARGER_STEP_CHARGE)
	info->setting.step_cv = info->chrg_step.chrg_step_cv_volt;
	info->setting.cv = info->chrg_step.chrg_step_cv_volt;
#if IS_ENABLED(CONFIG_CHARGER_FFC_CHARGE)
	ta_type = adapter_dev_get_property(info->adapter_dev[PD], CAP_TYPE);
	if (ta_type == MTK_PD_APDO &&
			info->data.battery_cv == info->chrg_step.chrg_step_cv_volt) {
		ffc_max_fv = cs_get_ffc_fv(info, info->battery_temp);
		info->setting.cv = ffc_max_fv;
		info->setting.step_cv = ffc_max_fv;
	}
#endif /* CONFIG_CHARGER_FFC_CHARGE */
#endif /* CONFIG_CHARGER_STEP_CHARGE */
// drv add tankaikun, add step charging, 20231130 end
}

static bool is_typec_adapter(struct mtk_charger *info)
{
	int rp;

	if (info->select_adapter_idx != PD || !info->select_adapter)
		return false;
	rp = adapter_dev_get_property(info->select_adapter, TYPEC_RP_LEVEL);
	if (info->ta_status[info->select_adapter_idx] != TA_HARD_RESET &&
			rp != 500 &&
			info->chr_type != POWER_SUPPLY_TYPE_USB &&
			info->chr_type != POWER_SUPPLY_TYPE_USB_CDP)
		return true;

	return false;
}

// drv add tankaikun, add step charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_STEP_CHARGE)
#define MIN_TEMP_C -20
#define MAX_TEMP_C 60
#define HYSTEREISIS_DEGC 2

bool cs_find_temp_zone(struct mtk_charger *info, int temp_c, bool ignore_hysteresis_degc)
{
	int prev_zone, num_zones;
	struct cs_chrg_temp_zone *zones;
	int hotter_t = 0, hotter_fcc = 0;
	int colder_t = 0, colder_fcc = 0;
	int i;
	int max_temp;

	if (!info) {
		pr_err("[cs_chrg] called before info valid!\n");
		return false;
	}

	zones = info->temp_zones;
	num_zones = info->num_temp_zones;
	prev_zone = info->pres_temp_zone;

	pr_err("[cs_chrg] num_zones:%d prev_zone:%d, temp_c:%d\n",num_zones, prev_zone, temp_c);
	max_temp = zones[num_zones - 1].temp_c;

	if (prev_zone == ZONE_NONE) {
		for (i = num_zones - 1; i >= 0; i--) {
			pr_err("[cs_chrg] temp_c:%d zones[%d].temp_c:%d \n", temp_c, i, zones[i].temp_c);
			if (temp_c >= zones[i].temp_c) {
				if (i == num_zones - 1)
					info->pres_temp_zone = ZONE_HOT;
				else
					info->pres_temp_zone = i + 1;
				return true;
			}
		}
		info->pres_temp_zone = ZONE_COLD;
		return true;
	}
	pr_err("[cs_chrg] pres_temp_zone:%d\n",info->pres_temp_zone);

	if (prev_zone == ZONE_COLD) {
		if (temp_c >= MIN_TEMP_C + HYSTEREISIS_DEGC)
			info->pres_temp_zone = ZONE_FIRST;
	} else if (prev_zone == ZONE_HOT) {
		if (temp_c <=  max_temp - HYSTEREISIS_DEGC)
			info->pres_temp_zone = num_zones - 1;
	} else {
		if (prev_zone == ZONE_FIRST) {
			hotter_t = zones[prev_zone].temp_c;
			colder_t = MIN_TEMP_C;
			hotter_fcc = zones[prev_zone + 1].chrg_step_power->chrg_step_curr;
			colder_fcc = 0;
		} else if (prev_zone == num_zones - 1) {
			hotter_t = zones[prev_zone].temp_c;
			colder_t = zones[prev_zone - 1].temp_c;
			hotter_fcc = 0;
			colder_fcc = zones[prev_zone - 1].chrg_step_power->chrg_step_curr;
		} else {
			hotter_t = zones[prev_zone].temp_c;
			colder_t = zones[prev_zone - 1].temp_c;
			hotter_fcc = zones[prev_zone + 1].chrg_step_power->chrg_step_curr;
			colder_fcc = zones[prev_zone - 1].chrg_step_power->chrg_step_curr;
		}

		if (!ignore_hysteresis_degc) {
			if (zones[prev_zone].chrg_step_power->chrg_step_curr < hotter_fcc)
				hotter_t += HYSTEREISIS_DEGC;
			if (zones[prev_zone].chrg_step_power->chrg_step_curr < colder_fcc)
				colder_t -= HYSTEREISIS_DEGC;
		}

		if (temp_c <= MIN_TEMP_C)
			info->pres_temp_zone = ZONE_COLD;
		else if (temp_c >= max_temp)
			info->pres_temp_zone = ZONE_HOT;
		else if (temp_c >= hotter_t)
			info->pres_temp_zone++;
		else if (temp_c < colder_t)
			info->pres_temp_zone--;
	}

	pr_err("[cs_chrg] batt temp_c %d, prev zone %d, pres zone %d, "
						"hotter_fcc %duA, colder_fcc %duA, "
						"hotter_t %dC, colder_t %dC\n",
						temp_c,prev_zone, info->pres_temp_zone,
						hotter_fcc, colder_fcc, hotter_t, colder_t);

	if (prev_zone != info->pres_temp_zone) {
		pr_err("[cs_chrg] Entered Temp Zone %d!\n",
			   info->pres_temp_zone);
		return true;
	}
	return false;
}

bool cs_find_chrg_step(struct mtk_charger *info, int temp_zone, int vbatt_volt)
{
	int batt_volt, ibat, i, ret;
	bool find_step = false;
	struct cs_chrg_temp_zone zone;
	struct cs_chrg_step_power *chrg_steps;
	struct cs_chrg_step_info chrg_step_inline;
	struct cs_chrg_step_info prev_step;
	struct power_supply *bat_psy = NULL;
	union power_supply_propval prop = {0};

	if (!info) {
		pr_err("[cs_chrg] called before info valid!\n");
		return false;
	}

	if (info->pres_temp_zone == ZONE_HOT ||
		info->pres_temp_zone == ZONE_COLD ||
		info->pres_temp_zone < ZONE_FIRST) {
		pr_err("[cs_chrg] pres temp zone is HOT or COLD, "
							"can't find chrg step\n");
		info->chrg_step.pres_chrg_step = 0;
		info->chrg_step.chrg_step_cc_curr = 0;
		info->chrg_step.chrg_step_cv_tapper_curr = 0;
		return false;
	}

	zone = info->temp_zones[info->pres_temp_zone];
	chrg_steps = zone.chrg_step_power;
	prev_step = info->chrg_step;

	batt_volt = vbatt_volt*1000;
	chrg_step_inline.temp_c = zone.temp_c;

	bat_psy = power_supply_get_by_name("battery");
	if (bat_psy == NULL || IS_ERR(bat_psy)) {
		chr_err("%s Couldn't get bat_psy\n", __func__);
		return -EINVAL;
	}

	ret = power_supply_get_property(bat_psy, POWER_SUPPLY_PROP_CURRENT_NOW, &prop);
	if (ret) {
		pr_err("[cs_chrg] power_supply_get_property POWER_SUPPLY_PROP_CURRENT_NOW failed ret = %d \n", ret);
		ibat = 0;
	} else {
		ibat = prop.intval;
	}

	pr_err("[cs_chrg] batt_volt %d, chrg step %d, step nums %d ibat %d \n",
						batt_volt, prev_step.pres_chrg_step,
						info->chrg_step_nums, ibat);

	/*In the first search cycle, find out the vbatt is less than step volt*/
	for (i = 0; i < info->chrg_step_nums; i++) {
		pr_err("[cs_chrg] first cycle,i %d, step volt %d, batt_volt:%d cv_tapper_curr:%d ibat:%d\n",
					i, chrg_steps[i].chrg_step_volt, batt_volt, prev_step.chrg_step_cv_tapper_curr, ibat);
		if (chrg_steps[i].chrg_step_volt > 0 && batt_volt < (chrg_steps[i].chrg_step_volt - 100000)
				&& (prev_step.chrg_step_cv_tapper_curr + 100000) > ibat) {
			if ((i + 1) < info->chrg_step_nums
				&& chrg_steps[i + 1].chrg_step_volt > 0) {
				chrg_step_inline.chrg_step_cv_tapper_curr =
					chrg_steps[i + 1].chrg_step_curr;
			} else
				chrg_step_inline.chrg_step_cv_tapper_curr =
					chrg_steps[i].chrg_step_curr;

			chrg_step_inline.chrg_step_cc_curr =
				chrg_steps[i].chrg_step_curr;
			chrg_step_inline.chrg_step_cv_volt =
				chrg_steps[i].chrg_step_volt;
			chrg_step_inline.pres_chrg_step = i;
			find_step = true;
			pr_err("[cs_chrg] find chrg step\n");
			break;
		}
	}

	if (find_step) {
		pr_err("[cs_chrg] chrg step %d, "
					"step cc curr %d, step cv volt %d, "
					"step cv tapper curr %d\n",
					chrg_step_inline.pres_chrg_step,
					chrg_step_inline.chrg_step_cc_curr,
					chrg_step_inline.chrg_step_cv_volt,
					chrg_step_inline.chrg_step_cv_tapper_curr);
		info->chrg_step = chrg_step_inline;
	} else {
		if (prev_step.pres_chrg_step <= 0) {
			for (i = 0; i < info->chrg_step_nums; i++) {
				if (chrg_steps[i].chrg_step_volt > 0
					&& batt_volt > chrg_steps[i].chrg_step_volt) {
					if ( (i + 1) < info->chrg_step_nums
						&& chrg_steps[i + 1].chrg_step_volt > 0) {
						chrg_step_inline.chrg_step_cv_tapper_curr =
							chrg_steps[i + 1].chrg_step_curr;
					} else
						chrg_step_inline.chrg_step_cv_tapper_curr =
							chrg_steps[i].chrg_step_curr;
					chrg_step_inline.chrg_step_cc_curr =
						chrg_steps[i].chrg_step_curr;
					chrg_step_inline.chrg_step_cv_volt =
						chrg_steps[i].chrg_step_volt;
					chrg_step_inline.pres_chrg_step = i;
					find_step = true;
					pr_err("[cs_chrg] find second cycle, i %d, step volt %d, batt_volt %d\n",
							i, chrg_steps[i].chrg_step_volt, batt_volt);
				}
			}
		}

		if (find_step) {
			pr_err("[cs_chrg] chrg step %d, "
					"step cc curr %d, step cv volt %d, "
					"step cv tapper curr %d\n",
					chrg_step_inline.pres_chrg_step,
					chrg_step_inline.chrg_step_cc_curr,
					chrg_step_inline.chrg_step_cv_volt,
					chrg_step_inline.chrg_step_cv_tapper_curr);
			info->chrg_step = chrg_step_inline;
		}
	}

	if (!find_step && info->temp_zone_change) {
		if (chrg_steps[1].chrg_step_volt > 0) {
			chrg_step_inline.chrg_step_cv_tapper_curr =
				chrg_steps[1].chrg_step_curr;
		} else
			chrg_step_inline.chrg_step_cv_tapper_curr =
				chrg_steps[0].chrg_step_curr;
		chrg_step_inline.chrg_step_cc_curr =
			chrg_steps[0].chrg_step_curr;
		chrg_step_inline.chrg_step_cv_volt =
			chrg_steps[0].chrg_step_volt;
		chrg_step_inline.pres_chrg_step = i;
		info->chrg_step = chrg_step_inline;
		find_step = true;
	}

	if (find_step) {
		if (info->chrg_step.chrg_step_cc_curr ==
			info->chrg_step.chrg_step_cv_tapper_curr)
			info->chrg_step.last_step = true;
		else
			info->chrg_step.last_step = false;

		pr_err("[cs_chrg] Temp zone %d, "
				"select chrg step %d, step cc curr %d,"
				"step cv volt %d, step cv tapper curr %d, "
				"is the last chrg step %d\n",
				info->pres_temp_zone,
				info->chrg_step.pres_chrg_step,
				info->chrg_step.chrg_step_cc_curr,
				info->chrg_step.chrg_step_cv_volt,
				info->chrg_step.chrg_step_cv_tapper_curr,
				info->chrg_step.last_step);

		if (prev_step.pres_chrg_step != info->chrg_step.pres_chrg_step) {
			pr_err("[cs_chrg] Find the next chrg step\n");
			return true;
		}
	}
	return false;
}
#endif /* CONFIG_CHARGER_STEP_CHARGE */

// drv add tankaikun, thermal charging limit, start
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
bool cs_find_thmeral_zone(struct mtk_charger *info, int temp_c)
{
	int prev_zone, num_zones;
	struct cs_chrg_thermal_zone *zones;
	int target_fcc = 0;
	int hotter_t, colder_t;
	int i;
	int max_temp;

	if (!info) {
		pr_err("[cs_chrg] called before info valid!\n");
		return false;
	}

	zones = info->therm_zones;
	num_zones = info->num_thermal_zones;
	prev_zone = info->pres_therm_zone;

	if (zones==NULL || num_zones==0)
		return false;

	pr_err("[cs_chrg] num_zones:%d prev_zone:%d, temp_c:%d\n",num_zones, prev_zone, temp_c);
	max_temp = zones[num_zones - 1].temp_c;

	if (prev_zone == THERM_ZONE_FIRST) {
		for (i = num_zones - 1; i >= 0; i--) {
			pr_err("[cs_chrg] temp_c:%d zones[%d].temp_c:%d \n", temp_c, i, zones[i].temp_c);
			if (temp_c >= zones[i].temp_c) {
				if (i == num_zones - 1)
					info->pres_therm_zone = num_zones - 1;
				else
					info->pres_therm_zone = i;
				break;
			}
		}
	}

	if (prev_zone == THERM_ZONE_FIRST) {
		hotter_t = zones[prev_zone+1].temp_c;
		colder_t = zones[prev_zone].temp_hyst_c;
		target_fcc = -1;
		info->chrg_therm_limit_curr = -1;
	} else if (prev_zone == (num_zones - 1)) {
		hotter_t = zones[num_zones - 1].temp_c;
		colder_t = zones[num_zones - 1].temp_hyst_c;
		if (temp_c < colder_t)
			info->pres_therm_zone--;
	} else {
		hotter_t = zones[prev_zone+1].temp_c;
		colder_t = zones[prev_zone].temp_hyst_c;

		if (temp_c <= zones[THERM_ZONE_FIRST].temp_c)
			info->pres_therm_zone = 0;
		else if (temp_c >= max_temp)
			info->pres_therm_zone = (num_zones - 1);
		else if (temp_c >= hotter_t)
			info->pres_therm_zone++;
		else if (temp_c < colder_t)
			info->pres_therm_zone--;
	}

	if (info->pres_therm_zone != THERM_ZONE_FIRST && info->pres_therm_zone < info->num_thermal_zones) {
		target_fcc = zones[info->pres_therm_zone].chrg_limit_curr;
		info->chrg_therm_limit_curr = target_fcc*1000;
	}

	pr_err("[cs_chrg] batt temp_c %d, prev therm zone %d, pres therm zone %d, "
						"target_fcc %duA, "
						"hotter_t %dC, colder_t %dC\n",
						temp_c, prev_zone, info->pres_therm_zone,
						target_fcc, hotter_t, colder_t);

	if (prev_zone != info->pres_therm_zone) {
		pr_err("[cs_chrg] Entered Temp Zone %d!\n",
			   info->pres_therm_zone);
		return true;
	}

	return false;
}

bool cs_find_wireless_thmeral_zone(struct mtk_charger *info, int temp_c)
{
	int prev_zone, num_zones;
	struct cs_chrg_thermal_zone *zones;
	int target_fcc = 0;
	int hotter_t, colder_t;
	int i;
	int max_temp;

	if (!info) {
		pr_err("[cs_chrg] called before info valid!\n");
		return false;
	}

	zones = info->wireless_therm_zones;
	num_zones = info->num_wireless_thermal_zones;
	prev_zone = info->pres_wireless_therm_zone;

	if (zones==NULL || num_zones==0)
		return false;

	pr_err("[cs_chrg] wireless num_zones:%d prev_zone:%d, temp_c:%d\n",num_zones, prev_zone, temp_c);
	max_temp = zones[num_zones - 1].temp_c;

	if (prev_zone == THERM_ZONE_FIRST) {
		for (i = num_zones - 1; i >= 0; i--) {
			pr_err("[cs_chrg] wireless temp_c:%d zones[%d].temp_c:%d \n", temp_c, i, zones[i].temp_c);
			if (temp_c >= zones[i].temp_c) {
				if (i == num_zones - 1)
					info->pres_wireless_therm_zone = num_zones - 1;
				else
					info->pres_wireless_therm_zone = i;
				break;
			}
		}
	}

	if (prev_zone == THERM_ZONE_FIRST) {
		hotter_t = zones[prev_zone+1].temp_c;
		colder_t = zones[prev_zone].temp_hyst_c;
		target_fcc = -1;
		info->chrg_wireless_therm_limit_curr = -1;
	} else if (prev_zone == (num_zones - 1)) {
		hotter_t = zones[num_zones - 1].temp_c;
		colder_t = zones[num_zones - 1].temp_hyst_c;
		if (temp_c < colder_t)
			info->pres_wireless_therm_zone--;
	} else {
		hotter_t = zones[prev_zone+1].temp_c;
		colder_t = zones[prev_zone].temp_hyst_c;

		if (temp_c <= zones[THERM_ZONE_FIRST].temp_c)
			info->pres_wireless_therm_zone = 0;
		else if (temp_c >= max_temp)
			info->pres_wireless_therm_zone = (num_zones - 1);
		else if (temp_c >= hotter_t)
			info->pres_wireless_therm_zone++;
		else if (temp_c < colder_t)
			info->pres_wireless_therm_zone--;
	}

	if (info->pres_wireless_therm_zone != THERM_ZONE_FIRST && info->pres_wireless_therm_zone < info->num_wireless_thermal_zones) {
		target_fcc = zones[info->pres_wireless_therm_zone].chrg_limit_curr;
		info->chrg_wireless_therm_limit_curr = target_fcc*1000;
	}

	pr_err("[cs_chrg] wireless batt temp_c %d, prev therm zone %d, pres therm zone %d, "
						"target_fcc %duA, "
						"hotter_t %dC, colder_t %dC\n",
						temp_c, prev_zone, info->pres_wireless_therm_zone,
						info->chrg_wireless_therm_limit_curr, hotter_t, colder_t);

	if (prev_zone != info->pres_wireless_therm_zone) {
		pr_err("[cs_chrg] wireless Entered Temp Zone %d!\n",
			   info->pres_wireless_therm_zone);
		return true;
	}

	return false;
}

#endif /* CONFIG_CHARGER_THERMAL_LIMIT */
// drv add tankaikun, thermal charging limit, 20231130 end

static bool support_fast_charging(struct mtk_charger *info)
{
	struct chg_alg_device *alg;
	int i = 0, state = 0;
	bool ret = false;

	for (i = 0; i < MAX_ALG_NO; i++) {
		alg = info->alg[i];
		if (alg == NULL)
			continue;

		if (info->enable_fast_charging_indicator &&
		    ((alg->alg_id & info->fast_charging_indicator) == 0))
			continue;

		chg_alg_set_current_limit(alg, &info->setting);
		state = chg_alg_is_algo_ready(alg);
		chr_debug("%s %s ret:%s, prtocol_state:%d\n",
			__func__, dev_name(&alg->dev),
			chg_alg_state_to_str(state), info->protocol_state);

		if (state == ALG_READY || state == ALG_RUNNING) {
			ret = true;
			break;
		}
	}
	return ret;
}

static bool select_charging_current_limit(struct mtk_charger *info,
	struct chg_limit_setting *setting)
{
	struct charger_data *pdata, *pdata2, *pdata_dvchg, *pdata_dvchg2;
	bool is_basic = false;
	u32 ichg1_min = 0, aicr1_min = 0;
	int ret;
// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
#if IS_ENABLED(CONFIG_WIRELESS_MT5706)
	union charger_propval wls_online = {0};
	union charger_propval wls_type = {0};
	union charger_propval wls_curr = {0};
#endif
// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end
// drv add tankaikun, add step charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_STEP_CHARGE)
	int vbat = 0;
	int temp_zone = info->pres_temp_zone;

	vbat = get_battery_voltage(info);
	info->temp_zone_change = cs_find_temp_zone(info, info->battery_temp, true);
	info->chrg_step_change = cs_find_chrg_step(info, temp_zone, vbat);
#endif /* CONFIG_CHARGER_STEP_CHARGE */
// drv add tankaikun, add step charging, 20231130 end

// drv add tankaikun, thermal charging limit, start
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
	cs_find_thmeral_zone(info, info->battery_temp);
	cs_find_wireless_thmeral_zone(info, info->battery_temp);
#endif /*CONFIG_CHARGER_THERMAL_LIMIT*/
// drv add tankaikun, thermal charging limit, end

	select_cv(info);

	pdata = &info->chg_data[CHG1_SETTING];
	pdata2 = &info->chg_data[CHG2_SETTING];
	pdata_dvchg = &info->chg_data[DVCHG1_SETTING];
	pdata_dvchg2 = &info->chg_data[DVCHG2_SETTING];
	if (info->usb_unlimited) {
		pdata->input_current_limit =
					info->data.ac_charger_input_current;
		pdata->charging_current_limit =
					info->data.ac_charger_current;
		is_basic = true;
		goto done;
	}

	if (info->water_detected) {
		pdata->input_current_limit = info->data.usb_charger_current;
		pdata->charging_current_limit = info->data.usb_charger_current;
		is_basic = true;
		goto done;
	}

	if (((info->bootmode == 1) ||
	    (info->bootmode == 5)) && info->enable_meta_current_limit != 0) {
		pdata->input_current_limit = 200000; // 200mA
		is_basic = true;
		goto done;
	}

	if (info->atm_enabled == true
		&& (info->chr_type == POWER_SUPPLY_TYPE_USB ||
		info->chr_type == POWER_SUPPLY_TYPE_USB_CDP)
		) {
		pdata->input_current_limit = 100000; /* 100mA */
		is_basic = true;
		goto done;
	}

	if (info->chr_type == POWER_SUPPLY_TYPE_USB &&
	    info->usb_type == POWER_SUPPLY_USB_TYPE_SDP) {
		pdata->input_current_limit =
				info->data.usb_charger_current;
		/* it can be larger */
		pdata->charging_current_limit =
				info->data.usb_charger_current;
		is_basic = true;
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_CDP) {
		pdata->input_current_limit =
			info->data.charging_host_charger_current;
		pdata->charging_current_limit =
			info->data.charging_host_charger_current;
		is_basic = true;

	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB_DCP) {
		pdata->input_current_limit =
			info->data.ac_charger_input_current;
		pdata->charging_current_limit =
			info->data.ac_charger_current;
		if (info->config == DUAL_CHARGERS_IN_SERIES) {
			pdata2->input_current_limit =
				pdata->input_current_limit;
			pdata2->charging_current_limit = 2000000;
		}
	} else if (info->chr_type == POWER_SUPPLY_TYPE_USB &&
	    info->usb_type == POWER_SUPPLY_USB_TYPE_DCP) {
		/* NONSTANDARD_CHARGER */
		pdata->input_current_limit =
			info->data.usb_charger_current;
		pdata->charging_current_limit =
			info->data.usb_charger_current;
		is_basic = true;
	} else {
		/*chr_type && usb_type cannot match above, set 500mA*/
		pdata->input_current_limit =
				info->data.usb_charger_current;
		pdata->charging_current_limit =
				info->data.usb_charger_current;
		is_basic = true;
	}

	if (support_fast_charging(info))
		is_basic = false;
	else {
		is_basic = true;
		/* AICL */
		if (!info->disable_aicl)
			charger_dev_run_aicl(info->chg1_dev,
				&pdata->input_current_limit_by_aicl);
		if (info->enable_dynamic_mivr) {
			if (pdata->input_current_limit_by_aicl >
				info->data.max_dmivr_charger_current)
				pdata->input_current_limit_by_aicl =
					info->data.max_dmivr_charger_current;
		}
		if (is_typec_adapter(info)) {
			if (adapter_dev_get_property(info->adapter_dev[PD]
			, TYPEC_RP_LEVEL)
				== 3000) {
				pdata->input_current_limit = 3000000;
				pdata->charging_current_limit = 3000000;
			} else if (adapter_dev_get_property(info->adapter_dev[PD],
				TYPEC_RP_LEVEL) == 1500) {
				pdata->input_current_limit = 1500000;
				pdata->charging_current_limit = 2000000;
			} else {
				chr_err("type-C: inquire rp error\n");
				pdata->input_current_limit = 500000;
				pdata->charging_current_limit = 500000;
			}

			chr_err("type-C:%d current:%d\n",
				info->ta_status[PD],
				adapter_dev_get_property(info->adapter_dev[PD],
					TYPEC_RP_LEVEL));
		}
	}

	if (info->enable_sw_jeita) {
		if (IS_ENABLED(CONFIG_USBIF_COMPLIANCE)
			&& info->chr_type == POWER_SUPPLY_TYPE_USB)
			chr_debug("USBIF & STAND_HOST skip current check\n");
		else {
			if (info->sw_jeita.sm == TEMP_T0_TO_T1) {
				pdata->input_current_limit = 500000;
				pdata->charging_current_limit = 350000;
			}
		}
	}

	sc_select_charging_current(info, pdata);

// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
#if IS_ENABLED(CONFIG_WIRELESS_MT5706)
	if (info->wlchg1_dev) {
		charger_dev_get_property(info->wlchg1_dev, CHARGER_PROP_WLS_CHG_ONLINE, &wls_online);
		if (wls_online.intval) {
			charger_dev_get_property(info->wlchg1_dev, CHARGER_PROP_WLS_CHG_TYPE, &wls_type);
			charger_dev_get_property(info->wlchg1_dev, CHARGER_PROP_WLS_MAX_CURR_LIMIT, &wls_curr);
			//if (wls_type.intval == WLS_CHARGER_TYPE_BPP)
			//	pdata->input_current_limit = 1000000; // 5V 1A
			//else if (wls_type.intval == WLS_CHARGER_TYPE_EPP_10W)
			//	pdata->input_current_limit = 1100000; // 9V 1.1A
			//else if (wls_type.intval == WLS_CHARGER_TYPE_EPP_15W)
			//	pdata->input_current_limit = 1500000; // 9v 1.67A
			//else
			//	pdata->input_current_limit = 500000; // 5V 0.5A
			pdata->input_current_limit = wls_curr.intval;
			pdata->charging_current_limit = 4000000;

			if(info->chrg_wireless_therm_limit_curr != -1 &&
				info->chrg_wireless_therm_limit_curr < pdata->input_current_limit) {
				pdata->input_current_limit = info->chrg_wireless_therm_limit_curr;
			}
		}
		chr_err("%s, wls chg: online:%d type:%d icl:%d wtl:%d\n" , __func__, wls_online.intval,
			wls_type.intval, pdata->input_current_limit, info->chrg_wireless_therm_limit_curr);
	} else {
		chr_err("%s, *** Error : can't find wireless charger ***\n" , __func__);
	}
#endif /* CONFIG_WIRELESS_MT5706 */
// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end

// drv add wanwen, add bypass charger function, 20250610 start
	if (info->cmd_bypass_charging) {
		pdata->charging_current_limit = 0;
		info->setting.charging_current_limit1 = 0;
	}
// drv add wanwen, add bypass charger function, 20250610 end

// drv add tankaikun, add step charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_STEP_CHARGE)
	if (info->chrg_step.chrg_step_cc_curr <= pdata->charging_current_limit) {
		pdata->charging_current_limit = info->chrg_step.chrg_step_cc_curr;
		info->setting.charging_current_limit1 = info->chrg_step.chrg_step_cc_curr;
	} else {
		info->setting.charging_current_limit1 = pdata->charging_current_limit;
	}
	info->setting.charging_current_cv_tapper = info->chrg_step.chrg_step_cc_curr;
#endif /*CONFIG_CHARGER_STEP_CHARGE*/
// drv add tankaikun, add step charging, 20231130 end

// drv add tankaikun, add odm charger class, 20241015 start
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
	if (info->thermal_input_current_limit != -1) {
		pdata->thermal_input_current_limit =
				info->thermal_input_current_limit;
	} else {
		pdata->thermal_input_current_limit = -1;
	}
	if (info->thermal_charging_current_limit != -1) {
		pdata->thermal_charging_current_limit =
				info->thermal_charging_current_limit;
	} else {
		pdata->thermal_charging_current_limit = -1;
	}

	chr_err("%s [cs_chrg] thrm_icl:%d thrm_cur:%d \n", __func__,
			pdata->thermal_input_current_limit,pdata->thermal_charging_current_limit);
#endif /* CONFIG_CHARGER_THERMAL_LIMIT */
// drv add tankaikun, add odm charger class, 20241015 end

// drv add tankaikun, add for screen on charging 20230108 start
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
	info->setting.input_current_limit_dvchg1 = -1;
	if (g_charge_is_screen_on) {
		if (pdata->input_current_limit > 1000000) {
			pdata->input_current_limit = 1000000;
		}

		// pe50: input limit 1A
		if ((info->setting.input_current_limit_dvchg1 > 1000000)
				|| (-1 == info->setting.input_current_limit_dvchg1))
			info->setting.input_current_limit_dvchg1 = 1000000;

		// pe pd: input limit 1A
		if ((info->setting.input_current_limit1 > 1000000)
				|| (-1 == info->setting.input_current_limit1))
			info->setting.input_current_limit1 = 1000000;

		// pe50: charging limit to be themal cur
		if (pdata->thermal_charging_current_limit != -1
				&& pdata->thermal_charging_current_limit < info->setting.charging_current_cv_tapper)
			info->setting.charging_current_cv_tapper = pdata->thermal_charging_current_limit;

		chr_err("screen_on icl:%d icl_dv:%d icl_sw:%d thl_cur:%d \n",
				pdata->input_current_limit,info->setting.input_current_limit_dvchg1,
				info->setting.input_current_limit1, info->setting.charging_current_cv_tapper);
	}
	else {
		if (info->chrg_therm_limit_curr != -1 && info->chrg_therm_limit_curr != 0) {
			if (info->setting.input_current_limit_dvchg1 > info->chrg_therm_limit_curr
					|| (-1 == info->setting.input_current_limit_dvchg1))
				info->setting.input_current_limit_dvchg1 = info->chrg_therm_limit_curr;

			if (pdata->input_current_limit > info->chrg_therm_limit_curr){
				pdata->input_current_limit = info->chrg_therm_limit_curr;
				info->setting.input_current_limit1 = info->chrg_therm_limit_curr;
			}
			chr_err("screen_off thermal limit occur \n");
		} else {
			info->setting.input_current_limit1 = -1;
			info->setting.input_current_limit_dvchg1 = -1;
		}
		goto skip_thermal_limit;
	}
	// second charge cur limit control by alg
	pdata2->thermal_charging_current_limit = -1;
	pdata2->thermal_input_current_limit = -1;
#endif /* CONFIG_CHARGER_THERMAL_LIMIT */
#endif /* CONFIG_DRM_MEDIATEK */
// drv add tankaikun, add for screen on charging 20230108 end

// drv add linaiyu, add bypass charger function, 20250630 start
	if (1 == info->cmd_charge_power_limit)
		info->setting.charging_current_limit1 = (int)pdata->input_current_limit / 2;
	else if (2 == info->cmd_charge_power_limit)
		info->setting.charging_current_limit1 = (int)pdata->input_current_limit / 3;
// drv add linaiyu, add bypass charger function, 20250630 end

	if (pdata->thermal_charging_current_limit != -1) {
		if (pdata->thermal_charging_current_limit <=
			pdata->charging_current_limit) {
			pdata->charging_current_limit =
					pdata->thermal_charging_current_limit;
			info->setting.charging_current_limit1 =
					pdata->thermal_charging_current_limit;
		}
		pdata->thermal_throttle_record = true;
	} else
		info->setting.charging_current_limit1 = info->sc.sc_ibat;

	if (pdata->thermal_input_current_limit != -1) {
		if (pdata->thermal_input_current_limit <=
			pdata->input_current_limit) {
			pdata->input_current_limit =
					pdata->thermal_input_current_limit;
			info->setting.input_current_limit1 =
					pdata->input_current_limit;
		}
		pdata->thermal_throttle_record = true;
	} else
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
		if (!g_charge_is_screen_on || 0 == info->setting.input_current_limit1) {
			info->setting.input_current_limit1 = -1;
		}
#else
		info->setting.input_current_limit1 = -1;
#endif /* CONFIG_CHARGER_THERMAL_LIMIT */

	if (pdata2->thermal_charging_current_limit != -1) {
		if (pdata2->thermal_charging_current_limit <=
			pdata2->charging_current_limit) {
			pdata2->charging_current_limit =
					pdata2->thermal_charging_current_limit;
			info->setting.charging_current_limit2 =
					pdata2->charging_current_limit;
		}
	} else
		info->setting.charging_current_limit2 = info->sc.sc_ibat;

	if (pdata2->thermal_input_current_limit != -1) {
		if (pdata2->thermal_input_current_limit <=
			pdata2->input_current_limit) {
			pdata2->input_current_limit =
					pdata2->thermal_input_current_limit;
			info->setting.input_current_limit2 =
					pdata2->input_current_limit;
		}
	} else
		info->setting.input_current_limit2 = -1;

// drv add tankaikun, add for screen on charging 20230108 start
#if IS_ENABLED(CONFIG_CHARGER_THERMAL_LIMIT)
skip_thermal_limit:
#endif /* CONFIG_CHARGER_THERMAL_LIMIT */
// drv add tankaikun, add for screen on charging 20230108 end

	if (is_basic == true && pdata->input_current_limit_by_aicl != -1
		&& !info->charger_unlimited
		&& !info->disable_aicl) {
		if (pdata->input_current_limit_by_aicl <
		    pdata->input_current_limit)
			pdata->input_current_limit =
					pdata->input_current_limit_by_aicl;
	}
// drv add wanwen, The fast charging temperature rise strategy did not take effect. 20250711 start
//	info->setting.input_current_limit_dvchg1 =
//		pdata_dvchg->thermal_input_current_limit;
// drv add wanwen, The fast charging temperature rise strategy did not take effect. 20250711 end

done:

	ret = charger_dev_get_min_charging_current(info->chg1_dev, &ichg1_min);
	if (ret != -EOPNOTSUPP && pdata->charging_current_limit < ichg1_min) {
		pdata->charging_current_limit = 0;
		/* For TC_018, pleasae don't modify the format */
		chr_err("min_charging_current is too low %d %d\n",
			pdata->charging_current_limit, ichg1_min);
		is_basic = true;
	}

	ret = charger_dev_get_min_input_current(info->chg1_dev, &aicr1_min);
	if (ret != -EOPNOTSUPP && pdata->input_current_limit < aicr1_min) {
		pdata->input_current_limit = 0;
		/* For TC_018, pleasae don't modify the format */
		chr_err("min_input_current is too low %d %d\n",
			pdata->input_current_limit, aicr1_min);
		is_basic = true;
	}
	/* For TC_018, pleasae don't modify the format */
	chr_err("m:%d chg1:%d,%d,%d,%d chg2:%d,%d,%d,%d dvchg1:%d sc:%d %d %d type:%d:%d usb_unlimited:%d usbif:%d usbsm:%d aicl:%d atm:%d bm:%d b:%d d:%d\n",
		info->config,
		_uA_to_mA(pdata->thermal_input_current_limit),
		_uA_to_mA(pdata->thermal_charging_current_limit),
		_uA_to_mA(pdata->input_current_limit),
		_uA_to_mA(pdata->charging_current_limit),
		_uA_to_mA(pdata2->thermal_input_current_limit),
		_uA_to_mA(pdata2->thermal_charging_current_limit),
		_uA_to_mA(pdata2->input_current_limit),
		_uA_to_mA(pdata2->charging_current_limit),
		_uA_to_mA(pdata_dvchg->thermal_input_current_limit),
		info->sc.pre_ibat,
		info->sc.sc_ibat,
		info->sc.solution,
		info->chr_type, info->ta_status[info->select_adapter_idx],
		info->usb_unlimited,
		IS_ENABLED(CONFIG_USBIF_COMPLIANCE), info->usb_state,
		pdata->input_current_limit_by_aicl, info->atm_enabled,
		info->bootmode, is_basic, info->is_chg_done);

// drv add tankaikun, add step charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_STEP_CHARGE)
	chr_err("ieoc:%d cv:%d,%d \n",
		info->chrg_iterm,
		info->setting.cv,
		info->chrg_step.chrg_step_cv_volt);
#endif /* CONFIG_CHARGER_STEP_CHARGE */
// drv add tankaikun, add step charging, 20231130 end

	return is_basic;
}

static int do_algorithm(struct mtk_charger *info)
{
	struct chg_alg_device *alg;
	struct charger_data *pdata;
	struct chg_alg_notify notify;
	bool is_basic = true;
	bool chg_done = false;
	int i;
	int ret, ret2, ret3;
	int val = 0;
	int lst_rnd_alg_idx = info->lst_rnd_alg_idx;

	pdata = &info->chg_data[CHG1_SETTING];
	charger_dev_is_charging_done(info->chg1_dev, &chg_done);
	is_basic = select_charging_current_limit(info, &info->setting);

	if (info->is_chg_done != chg_done) {
		if (chg_done) {
			charger_dev_do_event(info->chg1_dev, EVENT_FULL, 0);
			info->polling_interval = CHARGING_FULL_INTERVAL;
			chr_err("%s battery full\n", __func__);
		} else {
			charger_dev_do_event(info->chg1_dev, EVENT_RECHARGE, 0);
			info->polling_interval = CHARGING_INTERVAL;
			chr_err("%s battery recharge\n", __func__);
		}
	}

	chr_err("%s is_basic:%d\n", __func__, is_basic);
	if (is_basic != true) {
		is_basic = true;
		for (i = 0; i < MAX_ALG_NO; i++) {
			alg = info->alg[i];
			if (alg == NULL)
				continue;

			if (info->enable_fast_charging_indicator &&
			    ((alg->alg_id & info->fast_charging_indicator) == 0))
				continue;

			if (!info->enable_hv_charging ||
			    pdata->charging_current_limit == 0 ||
			    pdata->input_current_limit == 0) {
				chg_alg_get_prop(alg, ALG_MAX_VBUS, &val);
				if (val > 5000)
					chg_alg_stop_algo(alg);
				chr_err("%s: alg:%s alg_vbus:%d\n", __func__,
					dev_name(&alg->dev), val);
				continue;
			}

			if (info->alg_new_arbitration && info->alg_unchangeable &&
				(lst_rnd_alg_idx > -1)) {
				if (lst_rnd_alg_idx != i)
					continue;
			}

			if (chg_done != info->is_chg_done) {
				if (chg_done) {
					notify.evt = EVT_FULL;
					notify.value = 0;
				} else {
					notify.evt = EVT_RECHARGE;
					notify.value = 0;
				}
				chg_alg_notifier_call(alg, &notify);
				chr_err("%s notify:%d\n", __func__, notify.evt);
			}

			chg_alg_set_current_limit(alg, &info->setting);
			ret = chg_alg_is_algo_ready(alg);

			chr_err("%s %s ret:%s\n", __func__,
				dev_name(&alg->dev),
				chg_alg_state_to_str(ret));

			if (ret == ALG_INIT_FAIL || ret == ALG_TA_NOT_SUPPORT) {
				/* try next algorithm */
				continue;
			} else if (ret == ALG_WAIVER) {
				if (info->alg_new_arbitration)
					continue; /* try next algorithm */
				else {
					is_basic = true;
					break;
				}
			} else if (ret == ALG_TA_CHECKING || ret == ALG_DONE ||
						ret == ALG_NOT_READY) {
				/* wait checking , use basic first */
				is_basic = true;
				if (info->alg_new_arbitration && !info->alg_unchangeable &&
					(lst_rnd_alg_idx > -1)) {
					if (lst_rnd_alg_idx != i && lst_rnd_alg_idx < MAX_ALG_NO)
						chg_alg_stop_algo(info->alg[lst_rnd_alg_idx]);
				}
				break;
			} else if (ret == ALG_READY || ret == ALG_RUNNING) {
				// Add for user fast charge control
				if (!info->user_fast_charge_enabled) {
					// If user requested fast charge to be disabled,
					// stop the active fast charging algorithm, break
					// out and fall back to basic. If algorithm is in
					// ALG_READY state, leave it there. Fast charge
					// will re-activate as soon as user enables it.
					if (ret == ALG_RUNNING)
						chg_alg_stop_algo(alg);
					break;
				}
				is_basic = false;
				if (info->alg_new_arbitration && !info->alg_unchangeable &&
					(lst_rnd_alg_idx > -1)) {
					if (lst_rnd_alg_idx != i && lst_rnd_alg_idx < MAX_ALG_NO)
						chg_alg_stop_algo(info->alg[lst_rnd_alg_idx]);
				}
				chg_alg_start_algo(alg);
				info->lst_rnd_alg_idx = i;
				break;
			} else {
				chr_err("algorithm ret is error");
				is_basic = true;
			}
		}
	} else {
		if (info->enable_hv_charging != true ||
		    pdata->charging_current_limit == 0 ||
		    pdata->input_current_limit == 0) {
			for (i = 0; i < MAX_ALG_NO; i++) {
				alg = info->alg[i];
				if (alg == NULL)
					continue;

				chg_alg_get_prop(alg, ALG_MAX_VBUS, &val);
				if (val > 5000 && chg_alg_is_algo_running(alg))
					chg_alg_stop_algo(alg);

				chr_err("%s: Stop hv charging. en_hv:%d alg:%s alg_vbus:%d\n",
					__func__, info->enable_hv_charging,
					dev_name(&alg->dev), val);
			}
		}
	}
	info->is_chg_done = chg_done;

	if (is_basic == true) {
		charger_dev_set_input_current(info->chg1_dev,
			pdata->input_current_limit);
		charger_dev_set_charging_current(info->chg1_dev,
			pdata->charging_current_limit);
// drv add tankaikun, add ffc charging, 20231130 start
#if IS_ENABLED(CONFIG_CHARGER_FFC_CHARGE)
		charger_dev_set_eoc_current(info->chg1_dev,
			(info->chrg_iterm*1000));
#endif /* CONFIG_CHARGER_STEP_CHARGE&&CONFIG_CHARGER_FFC_CHARGE */
// drv add tankaikun, add ffc charging, 20231130 end
		info->lst_rnd_alg_idx = -1;

		chr_debug("%s:old_cv=%d,cv=%d, vbat_mon_en=%d\n",
			__func__,
			info->old_cv,
			info->setting.cv,
			info->setting.vbat_mon_en);
		if (info->old_cv == 0 || (info->old_cv != info->setting.cv)
		    || info->setting.vbat_mon_en == 0) {
			charger_dev_enable_6pin_battery_charging(
				info->chg1_dev, false);
			charger_dev_set_constant_voltage(info->chg1_dev,
				info->setting.cv);
			if (info->setting.vbat_mon_en && info->stop_6pin_re_en != 1)
				charger_dev_enable_6pin_battery_charging(
					info->chg1_dev, true);
			info->old_cv = info->setting.cv;
		} else {
			if (info->setting.vbat_mon_en && info->stop_6pin_re_en != 1) {
				info->stop_6pin_re_en = 1;
				charger_dev_enable_6pin_battery_charging(
					info->chg1_dev, true);
			}
		}
	}
// drv add tankaikun, add step charging, 20231227 start
#if IS_ENABLED(CONFIG_CHARGER_STEP_CHARGE)
	else if(info->chrg_step_change) {
		if (info->old_cv == 0 || (info->old_cv != info->setting.cv)) {
			charger_dev_set_constant_voltage(info->chg1_dev,
				info->setting.cv);
			info->old_cv = info->setting.cv;
		}
	}
#endif /* CONFIG_CHARGER_STEP_CHARGE */
// drv add tankaikun, add step charging, 20231227 end

	if (pdata->input_current_limit == 0 ||
	    pdata->charging_current_limit == 0)
		charger_dev_enable(info->chg1_dev, false);
	else {
		alg = get_chg_alg_by_name("pe5p");
		ret = chg_alg_is_algo_ready(alg);
		alg = get_chg_alg_by_name("pe5");
		ret2 = chg_alg_is_algo_ready(alg);
		alg = get_chg_alg_by_name("hvbp");
		ret3 = chg_alg_is_algo_ready(alg);
		if (!(ret == ALG_READY || ret == ALG_RUNNING) &&
			!(ret2 == ALG_READY || ret2 == ALG_RUNNING) &&
			!(ret3 == ALG_READY || ret3 == ALG_RUNNING))
			charger_dev_enable(info->chg1_dev, true);
	}

	if (info->chg1_dev != NULL) {
		charger_dev_dump_registers(info->chg1_dev);
		charger_dev_kick_wdt(info->chg1_dev);
	}

	if (info->chg2_dev != NULL) {
		charger_dev_dump_registers(info->chg2_dev);
		charger_dev_kick_wdt(info->chg2_dev);
	}

	if (info->bkbstchg_dev != NULL)
		charger_dev_dump_registers(info->bkbstchg_dev);

	return 0;
}

static int enable_charging(struct mtk_charger *info,
						bool en)
{
	int i;
	struct chg_alg_device *alg;


	chr_err("%s %d\n", __func__, en);

	if (en == false) {
		for (i = 0; i < MAX_ALG_NO; i++) {
			alg = info->alg[i];
			if (alg == NULL)
				continue;
			chg_alg_stop_algo(alg);
		}
		charger_dev_enable(info->chg1_dev, false);
		charger_dev_do_event(info->chg1_dev, EVENT_DISCHARGE, 0);
	} else {
		charger_dev_enable(info->chg1_dev, true);
		charger_dev_do_event(info->chg1_dev, EVENT_RECHARGE, 0);
	}

	return 0;
}

static int charger_dev_event(struct notifier_block *nb, unsigned long event,
				void *v)
{
	struct chg_alg_device *alg;
	struct chg_alg_notify notify;
	struct mtk_charger *info =
			container_of(nb, struct mtk_charger, chg1_nb);
	struct chgdev_notify *data = v;
	int i;

	chr_err("%s %lu\n", __func__, event);

	switch (event) {
	case CHARGER_DEV_NOTIFY_EOC:
		info->stop_6pin_re_en = 1;
		notify.evt = EVT_FULL;
		notify.value = 0;
		for (i = 0; i < 10; i++) {
			alg = info->alg[i];
			chg_alg_notifier_call(alg, &notify);
		}

		break;
	case CHARGER_DEV_NOTIFY_RECHG:
		pr_info("%s: recharge\n", __func__);
		break;
	case CHARGER_DEV_NOTIFY_SAFETY_TIMEOUT:
		info->safety_timeout = true;
		pr_info("%s: safety timer timeout\n", __func__);
		break;
	case CHARGER_DEV_NOTIFY_VBUS_OVP:
		info->vbusov_stat = data->vbusov_stat;
		pr_info("%s: vbus ovp = %d\n", __func__, info->vbusov_stat);
		break;
	case CHARGER_DEV_NOTIFY_BATPRO_DONE:
		info->batpro_done = true;
		info->setting.vbat_mon_en = 0;
		notify.evt = EVT_BATPRO_DONE;
		notify.value = 0;
		for (i = 0; i < 10; i++) {
			alg = info->alg[i];
			chg_alg_notifier_call(alg, &notify);
		}
		pr_info("%s: batpro_done = %d\n", __func__, info->batpro_done);
		break;
	case CHARGER_DEV_NOTIFY_DPDM_OVP:
		info->dpdmov_stat = data->dpdmov_stat;
		pr_info("%s: DPDM ovp = %d\n", __func__, info->dpdmov_stat);
		break;
	default:
		return NOTIFY_DONE;
	}

	if (info->chg1_dev->is_polling_mode == false)
		_wake_up_charger(info);

	return NOTIFY_DONE;
}

static int to_alg_notify_evt(unsigned long evt)
{
	switch (evt) {
	case CHARGER_DEV_NOTIFY_VBUS_OVP:
		return EVT_VBUSOVP;
	case CHARGER_DEV_NOTIFY_IBUSOCP:
		return EVT_IBUSOCP;
	case CHARGER_DEV_NOTIFY_IBUSUCP_FALL:
		return EVT_IBUSUCP_FALL;
	case CHARGER_DEV_NOTIFY_BAT_OVP:
		return EVT_VBATOVP;
	case CHARGER_DEV_NOTIFY_IBATOCP:
		return EVT_IBATOCP;
	case CHARGER_DEV_NOTIFY_VBATOVP_ALARM:
		return EVT_VBATOVP_ALARM;
	case CHARGER_DEV_NOTIFY_VBUSOVP_ALARM:
		return EVT_VBUSOVP_ALARM;
	case CHARGER_DEV_NOTIFY_VOUTOVP:
		return EVT_VOUTOVP;
	case CHARGER_DEV_NOTIFY_VDROVP:
		return EVT_VDROVP;
	default:
		return -EINVAL;
	}
}

static int dvchg1_dev_event(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct mtk_charger *info =
		container_of(nb, struct mtk_charger, dvchg1_nb);
	int alg_evt = to_alg_notify_evt(event);

	chr_info("%s %ld", __func__, event);
	if (alg_evt < 0)
		return NOTIFY_DONE;
	mtk_chg_alg_notify_call(info, alg_evt, 0);
	return NOTIFY_OK;
}

static int dvchg2_dev_event(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct mtk_charger *info =
		container_of(nb, struct mtk_charger, dvchg2_nb);
	int alg_evt = to_alg_notify_evt(event);

	chr_info("%s %ld", __func__, event);
	if (alg_evt < 0)
		return NOTIFY_DONE;
	mtk_chg_alg_notify_call(info, alg_evt, 0);
	return NOTIFY_OK;
}

static int hvdvchg1_dev_event(struct notifier_block *nb, unsigned long event,
			      void *data)
{
	struct mtk_charger *info =
		container_of(nb, struct mtk_charger, hvdvchg1_nb);
	int alg_evt = to_alg_notify_evt(event);

	chr_info("%s %ld", __func__, event);
	if (alg_evt < 0)
		return NOTIFY_DONE;
	mtk_chg_alg_notify_call(info, alg_evt, 0);
	return NOTIFY_OK;
}

static int hvdvchg2_dev_event(struct notifier_block *nb, unsigned long event,
			      void *data)
{
	struct mtk_charger *info =
		container_of(nb, struct mtk_charger, hvdvchg2_nb);
	int alg_evt = to_alg_notify_evt(event);

	chr_info("%s %ld", __func__, event);
	if (alg_evt < 0)
		return NOTIFY_DONE;
	mtk_chg_alg_notify_call(info, alg_evt, 0);
	return NOTIFY_OK;
}

int mtk_basic_charger_init(struct mtk_charger *info)
{

	info->algo.do_algorithm = do_algorithm;
	info->algo.enable_charging = enable_charging;
	info->algo.do_event = charger_dev_event;
	info->algo.do_dvchg1_event = dvchg1_dev_event;
	info->algo.do_dvchg2_event = dvchg2_dev_event;
	info->algo.do_hvdvchg1_event = hvdvchg1_dev_event;
	info->algo.do_hvdvchg2_event = hvdvchg2_dev_event;
	info->lst_rnd_alg_idx = -1;
	//info->change_current_setting = mtk_basic_charging_current;
	return 0;
}
