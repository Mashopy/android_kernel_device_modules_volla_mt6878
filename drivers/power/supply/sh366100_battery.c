/*
 * Fuelgauge battery driver
 *
 * Copyright (C) 2021 SinoWealth
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#define pr_fmt(fmt) "[sh366100] %s(%d): " fmt, __func__, __LINE__

#include <asm/unaligned.h>
#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/param.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include "sh366100_battery.h"
#include "charger_class.h"
#include <asm/div64.h>
//#include <stdbool.h>
#if !(IS_PACK_ONLY)
//#include <linux/pmic-voter.h>
#endif
/* prize add by fangduozhu 20250717, show fuelgauge firmware version start */
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
#include "hardware_info.h"
#endif
/* prize add by fangduozhu 20250717, show fuelgauge firmware version end */
#undef FG_DEBUG
#define FG_DEBUG 1

// drv add tankaikun, update fg state, 20250530 start
#define queue_delayed_work_time  5000
#define queue_start_work_time    50
// drv add tankaikun, update fg state, 20250530 end

// drv add tankaikun, battery info, 20250702 start
#define OFFSET_VENDOR_NAME 0
#define LENTH_VENDER_NAME 3
#define OFFSET_DEVICE_MODEL 2
#define LENTH_DEVICE_MODEL 6
#define OFFSET_MANU_DATA 7
#define LENTH_MANU_DATA 4
#define OFFSET_SERIAL_NUM 10
#define LENTH_SERIAL_NUM 6
#define OFFSET_ACTIVE_DATA 15
#define LENTH_ACTIVE_DATA 4
// drv add tankaikun, battery info, 20250702 end

// #undef pr_debug
// #undef pr_info
// #if FG_DEBUG
// #define pr_debug pr_err
// #define pr_info pr_err
//#endif
int major;
enum sh_fg_reg_idx {
	SH_FG_REG_DEVICE_ID = 0,
	SH_FG_REG_CNTL,
	SH_FG_REG_STATUS,
	SH_FG_REG_SOC,
	SH_FG_REG_VOLTAGE,
	SH_FG_REG_CURRENT,
	SH_FG_REG_TEMPERATURE_IN,
	SH_FG_REG_TEMPERATURE_EX,
	SH_FG_REG_BAT_RMC,
	SH_FG_REG_BAT_FCC,
	SH_FG_REG_RESET,
	SH_FG_REG_SOC_CYCLE,
	SH_FG_REG_DESIGN_CAPCITY, /* 20211116, Ethan */
	SH_FG_REG_INTSTATUS,
	SH_FG_REG_BAT_HEALTH, // drv add by liuruiqian, EU new charging regulations, 20250603
	NUM_REGS,
};

static u32 sh366100_regs[NUM_REGS] = {
    CMDMASK_ALTMAC_R | 0x0001, /* DEVICE_ID */
    CMDMASK_ALTMAC_R | 0x00CE, /* CNTL */
    0x0A,		       /* STATUS */
    0x2C,		       /* SOC */
    0x08,		       /* VOLTAGE */
    0x0C,		       /* CURRENT */
    0x28,		       /* TEMPERATURE_IN */
    0x06,		       /* TEMPERATURE_EX */
    0x10,		       /* BAT_RMC */
    0x12,		       /* BAT_FCC */
    CMDMASK_ALTMAC_W | 0x41,   /* RESET */
    0x2A,		       /* SOC_CYCLE */
    0x3C,		       /* SH_FG_REG_DESIGN_CAPCITY. 20211116, Ethan */
	CMDMASK_SINGLE_R | 0x6E, /* SH_FG_REG_INTSTATUS */
	0X2E,				/* SH_FG_REG_BAT_HEALTH */
};

enum sh_fg_device {
	SH366100,
};

enum sh_fg_temperature_type {
	TEMPERATURE_IN = 0,
	TEMPERATURE_EX,
};

const unsigned char* device2str[] = {
    "sh366100",
};

enum battery_table_type {
	BATTERY_TABLE0 = 0,
	BATTERY_TABLE1,
	BATTERY_TABLE2,
	BATTERY_TABLE_MAX,
};

/* 20211112, Ethan. Charge Block */
enum sh_fg_charge_temper_range {
	TEMPER_RANGE_T1T2 = 0, /* value also used as block_chargingvoltage index */
	TEMPER_RANGE_T2T3 = 1,
	TEMPER_RANGE_T3T4 = 2,
	TEMPER_RANGE_T4T5 = 3,
	TEMPER_RANGE_T5T6 = 4,
	TEMPER_RANGE_BELOW_T1 = 5,
	TEMPER_RANGE_ABOVE_T6 = 6,
};

/* 20211112, Ethan. Charge Block */
enum sh_fg_charge_degrade_flag {
	DEGRADE_PHASE_0 = 0,
	DEGRADE_PHASE_1,
	DEGRADE_PHASE_2,
	DEGRADE_PHASE_3,
	DEGRADE_PHASE_4,
};

struct sh_fg_chip;

//drv add tankaikun, start
struct sh_dm_info {
	char vendor_name[LENTH_VENDER_NAME+1];
	char device_model[LENTH_DEVICE_MODEL+1];
	char mfr_date[LENTH_MANU_DATA+1];
	char serial_number[LENTH_SERIAL_NUM+1];
	char act_date[LENTH_ACTIVE_DATA+1];

	char manufacturer_date[7];
	char activation_date[7];
};
struct sh_dm_info g_battery_dm_info;
//drv add tankaikun, end

//drv add wanwen function declaration 250712 start
static u16 fg_set_act_mfr_date(struct sh_fg_chip *sh,u32 date, enum battery_info_property prop);
//drv add wanwen function declaration 250712 end

struct sh_fg_chip {
	struct device* dev;
	struct i2c_client* client;
	struct mutex i2c_rw_lock; /* I2C Read/Write Lock */
	struct mutex data_lock;	  /* Data Lock */
	struct mutex cali_lock;	  /* Cali Lock. 20220106, Ethan */
	u16 chip;
	u32 regs[NUM_REGS];
	s32 batt_id;
	s32 gpio_int;

	struct notifier_block nb;

	/* Status Tracking */
	bool batt_present;
	bool batt_fc;	/* Battery Full Condition */
	bool batt_tc;	/* Battery Full Condition */
	bool batt_ot;	/* Battery Over Temperature */
	bool batt_ut;	/* Battery Under Temperature */
	bool batt_soc1; /* SOC Low */
	bool batt_socp; /* SOC Poor */
	bool batt_dsg;	/* Discharge Condition*/
	s32 batt_soc;
	s32 batt_ocv;
	s32 batt_fcc;	    /* Full charge capacity */
	s32 batt_rmc;	    /* Remaining capacity */
	s32 batt_designcap; /* 20211116, Ethan */
	s32 batt_volt;
	s32 aver_batt_volt;
	s32 batt_temp;
	s32 batt_curr;
	s32 is_charging;    /* Charging informaion from charger IC */
	s32 batt_soc_cycle; /* Battery SOC cycle */
	s32 batt_dod0; /* 20220422, Ethan */
	s32 soh; // drv add by liuruiqian, EU new charging regulations, 20250603

	s32 health;
	s32 recharge_vol;
	bool usb_present;
	bool batt_sw_fc;
	bool fast_mode;

	s32 is_enable_autocali; /* 20220118, Ethan */

	/* previous battery voltage current*/
	s32 p_batt_voltage;
	s32 p_batt_current;

	/* DT */
	bool en_temp_ex;
	bool en_temp_in;
	bool en_batt_det;
	s32 fg_irq_set;

	struct workqueue_struct *shfg_workqueue; //drv add tankaikun
	struct delayed_work monitor_work;
	u64 last_update;
	u64 log_lastUpdate; /* 20211025, Ethan */
	struct votable* fcc_votable;
	struct votable* fv_votable;
	struct votable* chg_dis_votable;

	enum sh_fg_charge_temper_range temper_range; /* 20211112, Ethan. Charge Block */
	enum sh_fg_charge_degrade_flag degrade_flag; /* 20211112, Ethan. Charge Block */
	s32 terminate_voltage;			     /* 20211112, Ethan. Termniate Voltage */
	struct dentry* debug_root;
	struct power_supply* fg_psy;
#if !(IS_PACK_ONLY)
	struct power_supply* usb_psy;
	struct power_supply* batt_psy;
	struct power_supply* bbc_psy;
#endif
	struct power_supply_desc fg_psy_d;
};
struct sh_fg_chip* sm;					  

// drv add by liuruiqian, EU new charging regulations, 20250603 start
struct YearMap {
	char c;
	char value[3];
};

struct MonthMap {
	char c;
	char value[3];
};

struct DayMap {
	char c;
	char value[3];
};

const struct YearMap Ymappings[] = {
	{'F', "25"},
	{'G', "26"},
	{'H', "27"},
	{'I', "28"},
	{'J', "29"},
	{'K', "30"},
	{'L', "31"},
};

const struct MonthMap Mmappings[] = {
	{'1', "01"},
	{'2', "02"},
	{'3', "03"},
	{'4', "04"},
	{'5', "05"},
	{'6', "06"},
	{'7', "07"},
	{'8', "08"},
	{'9', "09"},
	{'A', "10"},
	{'B', "11"},
	{'C', "12"},
};

const struct DayMap Dmappings[] = {
	{'1', "01"},
	{'2', "02"},
	{'3', "03"},
	{'4', "04"},
	{'5', "05"},
	{'6', "06"},
	{'7', "07"},
	{'8', "08"},
	{'9', "09"},
	{'A', "10"},
	{'B', "11"},
	{'C', "12"},
	{'D', "13"},
	{'E', "14"},
	{'F', "15"},
	{'G', "16"},
	{'H', "17"},
	{'J', "18"},
	{'K', "19"},
	{'L', "20"},
	{'M', "21"},
	{'N', "22"},
	{'P', "23"},
	{'R', "24"},
	{'S', "25"},
	{'T', "26"},
	{'V', "27"},
	{'W', "28"},
	{'X', "29"},
	{'Y', "30"},
	{'Z', "31"},
};

const int YMAP_SIZE = sizeof(Ymappings) / sizeof(Ymappings[0]);
const int MMAP_SIZE = sizeof(Mmappings) / sizeof(Mmappings[0]);
const int DMAP_SIZE = sizeof(Dmappings) / sizeof(Dmappings[0]);

int convertToACall(u8 *array, u8 start, u8 length, char *data) {
	int i;
	char subArray[LEN_USERBUFFER]={0, };

	if (array == NULL || start < 0 || length <= 0 || data == NULL) {
		return -1;
	}

	for (i = 0; i < length; i++) {
		subArray[i] = array[start + i];
	}

	data[length] = '\0';
	for (i = 0; i < length; i++) {
		sprintf(data, "%c ", subArray[i]);
		data++;
	}

	return 0;
}

int convertDate(u8 *array, char *data) {
	int i;
	int len;

	//Year
	if (array[0]>=Ymappings[0].c && array[0]<=Ymappings[YMAP_SIZE-1].c) {
		for (i = 0; i < YMAP_SIZE; i++) {
			if (Ymappings[i].c == array[0]) {
				snprintf(data, sizeof(data), "%s", Ymappings[i].value);
				break;
			}
		}
	} else {
		snprintf(data, sizeof(data), "%s", "0");
		return -1;
	}

	//Month
	len = strlen(data);
	if (array[1]>=Mmappings[0].c && array[1]<=Mmappings[MMAP_SIZE-1].c) {
		for (i = 0; i < MMAP_SIZE; i++) {
			if (Mmappings[i].c == array[1]) {
				len += snprintf(data + len, sizeof(data) - len, "%s", Mmappings[i].value);
				break;
			}
		}
	} else {
		snprintf(data, sizeof(data), "%s", "0");
		return -1;
	}

	//Day
	if (array[2]>=Dmappings[0].c && array[2]<=Dmappings[DMAP_SIZE-1].c) {
		for (i = 0; i < DMAP_SIZE; i++) {
			if (Dmappings[i].c == array[2]) {
				len += snprintf(data + len, sizeof(data) - len, "%s", Dmappings[i].value);
				break;
			}
		}
	} else {
		snprintf(data, sizeof(data), "%s", "0");
		return -1;
	}

	pr_err("convert date: %s\n",data);
	return 0;
}

// drv add by liuruiqian, EU new charging regulations, 20250603 end

static bool fg_init(struct i2c_client* client);

static int __fg_read_byte(struct i2c_client* client, u8 reg, u8* val)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(client, reg); /* little endian */
	if (ret < 0) {
		pr_err("i2c read byte fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}
	*val = (u8)ret;

	return 0;
}

static int __fg_read_word(struct i2c_client* client, u8 reg, u16* val)
{
	s32 ret;

	ret = i2c_smbus_read_word_data(client, reg); /* little endian */
	if (ret < 0) {
		pr_err("i2c read word fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}
	*val = (u16)ret;

	return 0;
}

static __maybe_unused int __fg_write_byte(struct i2c_client* client, u8 reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(client, reg, val); /* little endian */
	if (ret < 0) {
		pr_err("i2c write byte fail: can't write 0x%02X to reg 0x%02X\n", val, reg);
		return ret;
	}

	return 0;
}

static int __fg_write_word(struct i2c_client* client, u8 reg, u16 val)
{
	s32 ret;

	pr_err(" reg=0x%x,val=0x%04x\n",reg,val); 											 
	ret = i2c_smbus_write_word_data(client, reg, val); /* little endian */
	if (ret < 0) {
		pr_err("i2c write word fail: can't write 0x%02X to reg 0x%02X\n", val, reg);
		return ret;
	}

	return 0;
}

static int fg_read_sbs_word(struct sh_fg_chip* sm, u32 reg, u16* val)
{
	int ret = -1;
	u8 readB=0;
	pr_info("fg_read_sbs_word start, reg=%08X", reg);

	mutex_lock(&sm->i2c_rw_lock);
	if ((reg & CMDMASK_READ_MASK) == CMDMASK_ALTMAC_R) { /* 20211116, Ethan */
		ret = __fg_write_word(sm->client, CMD_ALTMAC, (u16)reg);
		if (ret < 0)
			goto fg_read_sbs_word_end;

		HOST_DELAY(CMD_SBS_DELAY); /* 20211029, Ethan */
		ret = __fg_read_word(sm->client, CMD_ALTBLOCK, val);
	} else if ((reg & CMDMASK_READ_MASK) == CMDMASK_SINGLE_R) {
		ret = __fg_read_byte(sm->client, (u8)reg, &readB);
		*val = (u16)readB;
	} else {
		ret = __fg_read_word(sm->client, (u8)reg, val);
	}
fg_read_sbs_word_end:
	mutex_unlock(&sm->i2c_rw_lock);

	return ret;
}

#if 1
static int fg_write_sbs_word(struct sh_fg_chip* sm, u32 reg, u16 val)
{
	int ret;
	u8 readB=0;
	mutex_lock(&sm->i2c_rw_lock);
	if ((reg & CMDMASK_WRITE_MASK) == CMDMASK_SINGLE_W) {
		ret = __fg_write_word(sm->client, (u8)reg, (u8)val);
		val = (u16)readB;
	} else {
		ret = __fg_write_word(sm->client, (u8)reg, val);
	}

	mutex_unlock(&sm->i2c_rw_lock);

	return ret;
}
#endif

/* return -1: error; else return string valid length */
static s32 print_buffer(char* str, s32 strlen, u8* buf, s32 buflen)
{
#define PRINT_BUFFER_FORMAT_LEN 3
	s32 i, j;

	if ((strlen <= 0) || (buflen <= 0))
		return -1;

	memset(str, 0, strlen * sizeof(char));

	j = min(buflen, strlen / PRINT_BUFFER_FORMAT_LEN);
	for (i = 0; i < j; i++) {
		sprintf(&str[i * PRINT_BUFFER_FORMAT_LEN], "%02X ", buf[i]);
	}

	return i * PRINT_BUFFER_FORMAT_LEN;
}

static s32 __fg_read_buffer(struct i2c_client* client, u8 reg, u8 length, u8* val)
{
	static struct i2c_msg msg[2];

	if (!client->adapter)
		return -ENODEV;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = &reg;
	msg[0].len = sizeof(u8);
	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = val;
	msg[1].len = length;

	return (s32)i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
}

static int fg_read_block(struct sh_fg_chip* sm, u32 reg, u8 length, u8* val)
{
	int ret = -1;
	int i;
	u8 sum;
	u16 checksum;

	mutex_lock(&sm->i2c_rw_lock);
	if ((reg & CMDMASK_READ_MASK) == CMDMASK_ALTMAC_R) { /* 20211116, Ethan */
		ret = __fg_write_word(sm->client, CMD_ALTMAC, (u16)reg);
		if (ret < 0)
			goto fg_read_block_end;
		msleep(CMD_SBS_DELAY);

		if (length > 32)
			length = 32;

		ret = __fg_read_buffer(sm->client, CMD_ALTBLOCK, length, val);
		if (ret < 0)
			goto fg_read_block_end;
		msleep(CMD_SBS_DELAY);

		/* check buffer */
		ret = __fg_read_word(sm->client, CMD_ALTCHK, &checksum);
		if (ret < 0)
			goto fg_read_block_end;

		i = (checksum >> 8) - 4;
		if (i <= 0)
			goto fg_read_block_end;

		sum = (u8)(reg & 0xFF) + (u8)((reg >> 8) & 0xFF);
		while (i--)
			sum += val[i];
		sum = ~sum;
		if (sum != (u8)checksum)
			ret = -1;
		else
			ret = 0;
	} else {
		ret = __fg_read_buffer(sm->client, reg, length, val);
	}

fg_read_block_end:
	mutex_unlock(&sm->i2c_rw_lock);

	return ret;
}

#if 1
static s32 __fg_write_buffer(struct i2c_client* client, u8 reg, u8 length, u8* val)
{
	static struct i2c_msg msg[1];
	static u8 write_buf[WRITE_BUF_MAX_LEN];
	s32 ret;

	if (!client->adapter)
		return -ENODEV;

	if ((length <= 0) || (length + 1 >= WRITE_BUF_MAX_LEN)) {
		pr_err("i2c write buffer fail: length invalid!");
		return -1;
	}

	memset(write_buf, 0, WRITE_BUF_MAX_LEN * sizeof(u8));
	write_buf[0] = reg;
	memcpy(&write_buf[1], val, length);

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].buf = write_buf;
	msg[0].len = sizeof(u8) * (length + 1);

	ret = i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0) {
		pr_err("i2c write buffer fail: can't write reg 0x%02X\n", reg);
		return (s32)ret;
	}

	return 0;
}

/* 20211116, Ethan */
static int fg_write_block(struct sh_fg_chip* sm, u32 reg, u8 length, u8* val)
{
	int ret;
	int i;
	u8 sum;
	u16 checksum;

	if (length > 32)
		length = 32;

	mutex_lock(&sm->i2c_rw_lock);
	if ((reg & CMDMASK_ALTMAC_W) == CMDMASK_ALTMAC_W) {
		ret = __fg_write_word(sm->client, CMD_ALTMAC, (u16)reg);
		if (ret < 0)
			goto fg_write_block_end;
		msleep(CMD_SBS_DELAY);

		ret = __fg_write_buffer(sm->client, CMD_ALTBLOCK, length, val);
		if (ret < 0)
			goto fg_write_block_end;
		msleep(CMD_SBS_DELAY);

		sum = (u8)reg + (u8)(reg >> 8);
		for (i = 0; i < length; i++)
			sum += val[i];
		sum = ~sum; /* 20220104, Ethan */
		checksum = length + 4;
		checksum = (checksum << 8) | sum;

		ret = __fg_write_word(sm->client, CMD_ALTCHK, checksum);
		if (ret < 0)
			goto fg_write_block_end;
	} else {
		ret = __fg_write_buffer(sm->client, (u8)reg, length, val);
	}
fg_write_block_end:
	mutex_unlock(&sm->i2c_rw_lock);

	return ret;
}
#endif

#if 1 /* 20211026, Ethan. FileDecode Struct */
struct sh_decoder;

struct sh_decoder {
	u8 addr;
	u8 reg;
	u8 length;
	u8 buf_first_val;
};

static s32 fg_decode_iic_read(struct sh_fg_chip* sm, struct sh_decoder* decoder, u8* pBuf)
{
	static struct i2c_msg msg[2];
	u8 addr = IIC_ADDR_OF_2_KERNEL(decoder->addr);
	s32 ret;
	pr_err("decoder->addr = 0x%02x\n",decoder->addr);
	if (!sm->client->adapter)
		return -ENODEV;

	mutex_lock(&sm->i2c_rw_lock);

	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].buf = &(decoder->reg);
	msg[0].len = sizeof(u8);
	msg[1].addr = addr;
	msg[1].flags = I2C_M_RD;
	msg[1].buf = pBuf;
	msg[1].len = decoder->length;
	ret = (s32)i2c_transfer(sm->client->adapter, msg, ARRAY_SIZE(msg));

	mutex_unlock(&sm->i2c_rw_lock);
	if(ret<0){
		pr_err("read error:decoder->addr = 0x%02x\n",decoder->addr);
	}
	return ret;
}

static s32 fg_decode_iic_write(struct sh_fg_chip* sm, struct sh_decoder* decoder)
{
	static struct i2c_msg msg[1];
	static u8 write_buf[WRITE_BUF_MAX_LEN];
	u8 addr = IIC_ADDR_OF_2_KERNEL(decoder->addr);
	u8 length = decoder->length;
	s32 ret;

	if (!sm->client->adapter)
		return -ENODEV;

	if ((length <= 0) || (length + 1 >= WRITE_BUF_MAX_LEN)) {
		pr_err("i2c write buffer fail: length invalid!");
		return -1;
	}

	mutex_lock(&sm->i2c_rw_lock);
	memset(write_buf, 0, WRITE_BUF_MAX_LEN * sizeof(u8));
	write_buf[0] = decoder->reg;
	memcpy(&write_buf[1], &(decoder->buf_first_val), length);

	msg[0].addr = addr;
	msg[0].flags = 0;
	msg[0].buf = write_buf;
	msg[0].len = sizeof(u8) * (length + 1);

	ret = i2c_transfer(sm->client->adapter, msg, ARRAY_SIZE(msg));
	if (ret < 0) {
		pr_err("i2c write buffer fail: can't write reg 0x%02X\n", decoder->reg);
	}

	mutex_unlock(&sm->i2c_rw_lock);
	return (ret < 0) ? ret : 0;
}
#endif

static int fg_read_status(struct sh_fg_chip* sm)
{
	int ret;
	u16 flags1, cntl, intstatus;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_CNTL], &cntl);
	if (ret < 0)
		return ret;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_STATUS], &flags1);
	if (ret < 0)
		return ret;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_INTSTATUS], &intstatus);
	if (ret < 0)
		return ret;

	pr_err("cntl=0x%04X, bat_flags=0x%04X, intstatus=0x%2X", cntl, flags1, intstatus);
	mutex_lock(&sm->data_lock);
	sm->batt_present = 1;
	sm->batt_ot = !!(intstatus & FG_INTSTATUS_HIGH_TEMPER);
	sm->batt_ut = !!(intstatus & FG_INTSTATUS_LOW_TEMPER);
	sm->batt_tc = !!(flags1 & FG_STATUS_TERM_SOC);
	sm->batt_fc = !!(flags1 & FG_STATUS_FULL_SOC);
	sm->batt_soc1 = !!(flags1 & FG_STATUS_LOW_SOC2);
	sm->batt_socp = !!(flags1 & FG_STATUS_LOW_SOC1);
	sm->batt_dsg = !!(flags1 & FG_STATUS_DISCHG);
	mutex_unlock(&sm->data_lock);

	return 0;
}

#if (FG_REMOVE_IRQ == 0)
static int fg_status_changed(struct sh_fg_chip* sm)
{
	cancel_delayed_work(&sm->monitor_work);
	schedule_delayed_work(&sm->monitor_work, 0);
	power_supply_changed(sm->fg_psy);

	return IRQ_HANDLED;
}

static irqreturn_t fg_irq_thread(int irq, void* dev_id)
{
	struct sh_fg_chip* sm = dev_id;

	fg_status_changed(sm);
	pr_info("fg_read_int");

	return 0;
}
#endif

#if 0
static s32 fg_read_gaugeinfo_block(struct sh_fg_chip* sm)
{
	static u8 buf[GAUGEINFO_LEN];
	static char str[GAUGESTR_LEN];
	int i, j = 0;
	int ret;
	u16 temp;

	/* 20211029, Ethan. Tick Start*/
	u64 jiffies_now = get_jiffies_64(); /* 20211203, Ethan */
	s64 tick = (s64)(jiffies_now - sm->log_lastUpdate);
	if (tick < 0)  //overflow
	{
		tick = (s64)(U64_MAXVALUE - (u64)sm->log_lastUpdate);
		tick += jiffies_now + 1;
	}
	do_div(tick,HZ);//tick /= HZ;
	if (tick < GAUGE_LOG_MIN_TIMESPAN)
		return 0;

	if (!mutex_trylock(&sm->cali_lock)) { /* 20220106, Ethan */
		pr_err("SH366100_GaugeLog: could not get mutex!");
		return -1;
	}

	sm->log_lastUpdate = jiffies_now;
	/* 20211029, Ethan. Tick End */

	/* Cali Info */
	ret = fg_read_block(sm, CMD_CALIINFO, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CALIINFO, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now); //20211111, Ethan. In case print twice
	i += sprintf(&str[i], "Elasp=%lld, ", tick);
	i += sprintf(&str[i], "Voltage=%d, ", (s16)BUF2U16_LT(&buf[12]));
	i += sprintf(&str[i], "Current=%d, ", (s16)BUF2U32_LT(&buf[8]));
	i += sprintf(&str[i], "TS1Temp=%d, ", (s16)(BUF2U16_LT(&buf[22]) - TEMPER_OFFSET));
	i += sprintf(&str[i], "IntTemper=%d, ", (s16)(BUF2U16_LT(&buf[18]) - TEMPER_OFFSET));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: CMD_CALIINFO is %s", str);

	/* 20211116, Ethan. Charge Info */
	ret = fg_read_block(sm, CMD_CHARGESTATUS, LEN_CHARGESTATUS, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CHARGESTATUS, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}
	ret = BUF2U16_BG(&buf[1]);
	ret |= ((u32)buf[0]) << 16;

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "ChgStatus=0x%06X, ", (u32)(ret & 0xFFFFFF));
	i += sprintf(&str[i], "DegradeFlag=0x%08X, ", BUF2U32_BG(&buf[3]));

	/* 20211116, Ethan. Term Volt */
	ret = fg_read_block(sm, CMD_TERMINATEVOLT, LEN_TERMINATEVOLT, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read TERMINATEVOLT, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	i += sprintf(&str[i], "TermVolt=%d, ", BUF2U16_LT(&buf[0]));
	i += sprintf(&str[i], "TermVoltTime=%d, ", buf[2]);

	/* 20211123, Ethan */
	ret = fg_read_sbs_word(sm, CMD_CONTROLSTATUS, &temp);
		if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_CONTROLSTATUS, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}
	i += sprintf(&str[i], "ControlStatus=0x%04X, ", temp);

	ret = fg_read_sbs_word(sm, CMD_RUNFLAG, &temp);
		if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_RUNFLAG, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}
	i += sprintf(&str[i], "Flags=0x%04X, ", temp);

	ret = fg_read_sbs_word(sm, CMD_OEMFLAG, &temp); /* 20211126, Ethan */
		if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_OEMFLAG, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}
	i += sprintf(&str[i], "OEMFLAG=0x%04X, ", temp);


	j = max(i, j);
	pr_err("SH366100_GaugeLog: CHARGESTATUS is %s", str);

	/* Lifetime Info. 20211126, Ethan */
	ret = fg_read_block(sm, CMD_LIFETIMEADC, LEN_LIFETIMEADC, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read LIFETIMEADC, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "LT_MaxVolt=%d, ", (s16)BUF2U16_BG(&buf[0]));
	i += sprintf(&str[i], "LT_MinVolt=%d, ", (s16)BUF2U16_BG(&buf[2]));
	i += sprintf(&str[i], "LT_MaxChgCUR=%d, ", (s16)BUF2U16_BG(&buf[4]));
	i += sprintf(&str[i], "LT_MaxDsgCUR=%d, ", (s16)BUF2U16_BG(&buf[6]));
	i += sprintf(&str[i], "LT_MaxTemper=%d, ", (s8)buf[8]);
	i += sprintf(&str[i], "LT_MinTemper=%d, ", (s8)buf[9]);
	i += sprintf(&str[i], "LT_MaxIntTemper=%d, ", (s8)buf[10]);
	i += sprintf(&str[i], "LT_MinIntTemper=%d, ", (s8)buf[11]);
	j = max(i, j);
	pr_err("SH366100_GaugeLog: LIFETIMEADC is %s", str);

	/* Gauge Info */
	ret = fg_read_block(sm, CMD_GAUGEINFO, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read GAUGEINFO, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "RunState=0x%08X, ", BUF2U32_LT(&buf[0]));
	i += sprintf(&str[i], "GaugeState=0x%08X, ", BUF2U32_LT(&buf[4]));
	i += sprintf(&str[i], "GaugeS2=0x%04X, ", BUF2U16_LT(&buf[8]));
	i += sprintf(&str[i], "WorkState=0x%04X, ", BUF2U16_LT(&buf[10]));
	i += sprintf(&str[i], "TimeInc=%d, ", buf[12]);
	i += sprintf(&str[i], "MainTick=%d, ", buf[13]);
	i += sprintf(&str[i], "SysTick=%d, ", buf[14]);
	i += sprintf(&str[i], "ClockH=%d, ", BUF2U16_LT(&buf[15]));
	i += sprintf(&str[i], "RamCheckT=%d, ", buf[17]);
	i += sprintf(&str[i], "AutoCaliT=%d, ", buf[18]);
	i += sprintf(&str[i], "LTHour=%d, ", buf[19]);
	i += sprintf(&str[i], "LTTimer=%d, ", buf[20]);
	i += sprintf(&str[i], "FlashT=%d, ", buf[21]);
	i += sprintf(&str[i], "LTFlag=0x%02X, ", buf[22]);
	i += sprintf(&str[i], "RSTS=0x%02X, ", buf[23]);
	i += sprintf(&str[i], "TimeInc_HighFeq=%d, ", buf[24]);
	i += sprintf(&str[i], "MainTick_HighFeq=%d, ", buf[25]);
	i += sprintf(&str[i], "SysTick_HighFeq=%d, ", buf[26]);
	j = max(i, j);
	pr_err("SH366100_GaugeLog: CMD_GAUGEINFO is %s", str);

	/* Gauge Block 1 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK1, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK1, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "Smooth_Timer=%d, ", BUF2U32_BG(&buf[0]));
	i += sprintf(&str[i], "FilRE0=%d, ", (s8)buf[14]);
	i += sprintf(&str[i], "FilRE1=%d, ", (s16)BUF2U32_BG(&buf[16]));
	i += sprintf(&str[i], "FilFCE0=%d, ", (s8)buf[15]);
	i += sprintf(&str[i], "FilFCE1=%d, ", (s16)BUF2U32_BG(&buf[20]));
	i += sprintf(&str[i], "FracRE=%d, ", (s16)BUF2U32_BG(&buf[18]));
	i += sprintf(&str[i], "FracFCE=%d, ", (s16)BUF2U32_BG(&buf[22]));
	i += sprintf(&str[i], "FilRC0=%d, ", (s8)buf[4]); 
	i += sprintf(&str[i], "FilRC1=%d, ", (s16)BUF2U32_BG(&buf[6]));
	i += sprintf(&str[i], "FilFCC0=%d, ", (s8)buf[5]);
	i += sprintf(&str[i], "FilFCC1=%d, ", (s16)BUF2U32_BG(&buf[10]));
	i += sprintf(&str[i], "FracRC=%d, ", (s16)BUF2U32_BG(&buf[8]));
	i += sprintf(&str[i], "FracFCC=%d, ", (s16)BUF2U32_BG(&buf[12]));
	i += sprintf(&str[i], "RTCnt=%d, ", buf[24]);
	i += sprintf(&str[i], "DsgMaxT=%d, ", buf[25]);
	i += sprintf(&str[i], "DsgMaxI=%d, ", (s16)BUF2U32_BG(&buf[26]));
	i += sprintf(&str[i], "DsgMaxP=%d, ", (s16)BUF2U32_BG(&buf[28]));
	i += sprintf(&str[i], "OverChgTimer=%d, ", BUF2U32_BG(&buf[30]));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK1 is %s", str);

	/* Gauge Block 2 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK2, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK2, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "QF1=0x%02X, ", buf[0]);
	i += sprintf(&str[i], "QF2=0x%02X, ", buf[1]);
	i += sprintf(&str[i], "PackQmax=%d, ", (s16)BUF2U16_BG(&buf[2]));
	i += sprintf(&str[i], "CycleCount=%d, ", BUF2U16_BG(&buf[14]));
	i += sprintf(&str[i], "VatEOC=%d, ", (s16)BUF2U16_BG(&buf[4]));
	i += sprintf(&str[i], "IatEOC=%d, ", (s16)BUF2U16_BG(&buf[6]));
	i += sprintf(&str[i], "ChgVEOC=%d, ", (s16)BUF2U16_BG(&buf[8]));
	i += sprintf(&str[i], "AltVatEOC=%d, ", (s16)BUF2U16_BG(&buf[22]));
	i += sprintf(&str[i], "AltIatEOC=%d, ", (s16)BUF2U16_BG(&buf[24]));
	i += sprintf(&str[i], "AltChgVEOC=%d, ", (s16)BUF2U16_BG(&buf[26]));
	i += sprintf(&str[i], "AVILR=%d, ", (s16)BUF2U16_BG(&buf[10]));
	i += sprintf(&str[i], "AVPLR=%d, ", (s16)BUF2U16_BG(&buf[12]));
	i += sprintf(&str[i], "QmaxCycle=%d, ", buf[16]);
	i += sprintf(&str[i], "QmaxCount=%d, ", buf[17]);
	i += sprintf(&str[i], "ModelCycle=%d, ", buf[18]);
	i += sprintf(&str[i], "ModelCount=%d, ", buf[19]);
	i += sprintf(&str[i], "VCTCycle=%d, ", buf[20]);
	i += sprintf(&str[i], "VCTCount=%d, ", buf[21]);
	i += sprintf(&str[i], "RelaxCycle=%d, ", buf[28]);
	i += sprintf(&str[i], "RatioCycle=%d, ", buf[29]);
	i += sprintf(&str[i], "RatioGrid=0x%02X, ", buf[30]);
	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK2 is %s", str);

	/* Gauge Block 3 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK3, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK3, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "SFR_RC=%d, ", (s16)BUF2U16_BG(&buf[0]));
	i += sprintf(&str[i], "SFR_DCC=%d, ", (s16)BUF2U16_BG(&buf[2]));
	i += sprintf(&str[i], "SFR_ICC=%d, ", (s16)BUF2U16_BG(&buf[4]));
	i += sprintf(&str[i], "RCOffset=%d, ", (s16)BUF2U16_BG(&buf[6]));
	i += sprintf(&str[i], "C0DOD1=%d, ", (s16)BUF2U16_BG(&buf[8]));
	i += sprintf(&str[i], "PasCol=%d, ", (s16)BUF2U16_BG(&buf[10]));
	i += sprintf(&str[i], "PasEgy=%d, ", (s16)BUF2U16_BG(&buf[12]));
	i += sprintf(&str[i], "Qstart=%d, ", (s16)BUF2U16_BG(&buf[14]));
	i += sprintf(&str[i], "Estart=%d, ", (s16)BUF2U16_BG(&buf[16]));
	i += sprintf(&str[i], "FastTim=%d, ", buf[18]);
	i += sprintf(&str[i], "FILFLG=0x%02X, ", buf[19]);
	i += sprintf(&str[i], "StateTime=%d, ", BUF2U32_BG(&buf[20]));
	i += sprintf(&str[i], "StateHour=%d, ", BUF2U16_BG(&buf[24]));
	i += sprintf(&str[i], "StateSec=%d, ", BUF2U16_BG(&buf[26]));
	i += sprintf(&str[i], "OCVTim=%d, ", BUF2U16_BG(&buf[28]));
	i += sprintf(&str[i], "RaCalT1=%d, ", BUF2U16_BG(&buf[30]));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK3 is %s", str);

	/* Gauge Block 4 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK4, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK4, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "GUInx=0x%02X, ", buf[0]);
	i += sprintf(&str[i], "GULoad=0x%02X, ", buf[1]);
	i += sprintf(&str[i], "GUStatus=0x%08X, ", BUF2U32_BG(&buf[2]));
	i += sprintf(&str[i], "EodLoad=%d, ", (s16)BUF2U16_BG(&buf[6]));
	i += sprintf(&str[i], "CRatio=%d, ", (s16)BUF2U16_BG(&buf[8]));
	i += sprintf(&str[i], "C0DOD0=%d, ", (s16)BUF2U16_BG(&buf[10]));
	i += sprintf(&str[i], "C0EOC=%d, ", (s16)BUF2U16_BG(&buf[12]));
	i += sprintf(&str[i], "C0EOD=%d, ", (s16)BUF2U16_BG(&buf[14]));
	i += sprintf(&str[i], "C0ACV=%d, ", (s16)BUF2U16_BG(&buf[16]));
	i += sprintf(&str[i], "ThemT=%d, ", (s16)BUF2U16_BG(&buf[18]));
	i += sprintf(&str[i], "Told=%d, ", (s16)BUF2U16_BG(&buf[20]));
	i += sprintf(&str[i], "Tout=%d, ", (s16)BUF2U16_BG(&buf[22]));
	i += sprintf(&str[i], "RCRaw=%d, ", (s16)BUF2U16_BG(&buf[24]));
	i += sprintf(&str[i], "FCCRaw=%d, ", (s16)BUF2U16_BG(&buf[26]));
	i += sprintf(&str[i], "RERaw=%d, ", (s16)BUF2U16_BG(&buf[28]));
	i += sprintf(&str[i], "FCERaw=%d, ", (s16)BUF2U16_BG(&buf[30]));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK4 is %s", str);

	/* 20220422, Ethan */
	sm->batt_dod0 = (s32)(BUF2U16_BG(&buf[10]) ^ 0x644F) << 3;
	sm->batt_dod0 = ((sm->batt_dod0 >> 16) & 0x7) | (sm->batt_dod0 & 0xFFF8);

	/* Gauge Block 5 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK5, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK5, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "IdealFCC=%d, ", (s16)BUF2U16_BG(&buf[0]));
	i += sprintf(&str[i], "IdealFCE=%d, ", (s16)BUF2U16_BG(&buf[2]));
	i += sprintf(&str[i], "FilRC=%d, ", (s16)BUF2U16_BG(&buf[4]));
	i += sprintf(&str[i], "FilFCC=%d, ", (s16)BUF2U16_BG(&buf[6]));
	i += sprintf(&str[i], "FSOC=%d, ", buf[8]);
	i += sprintf(&str[i], "TrueRC=%d, ", (s16)BUF2U16_BG(&buf[9]));
	i += sprintf(&str[i], "TrueFCC=%d, ", (s16)BUF2U16_BG(&buf[11]));
	i += sprintf(&str[i], "RSOC=%d, ", buf[13]);
	i += sprintf(&str[i], "FilRE=%d, ", (s16)BUF2U16_BG(&buf[14]));
	i += sprintf(&str[i], "FilFCE=%d, ", (s16)BUF2U16_BG(&buf[16]));
	i += sprintf(&str[i], "FSOCW=%d, ", buf[18]);
	i += sprintf(&str[i], "TrueRE=%d, ", (s16)BUF2U16_BG(&buf[19]));
	i += sprintf(&str[i], "TrueFCE=%d, ", (s16)BUF2U16_BG(&buf[21]));
	i += sprintf(&str[i], "RSOCW=%d, ", buf[23]);
	i += sprintf(&str[i], "EquRE=%d, ", (s16)BUF2U16_BG(&buf[24]));
	i += sprintf(&str[i], "EquFCE=%d, ", (s16)BUF2U16_BG(&buf[26]));
	i += sprintf(&str[i], "EquRC=%d, ", (s16)BUF2U16_BG(&buf[28]));
	i += sprintf(&str[i], "EquFCC=%d, ", (s16)BUF2U16_BG(&buf[30]));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK5 is %s", str);

	/* Gauge Block 6 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK6, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK6, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "GaugeS3=0x%04X, ", BUF2U16_BG(&buf[0]));
	i += sprintf(&str[i], "CCON1=0x%02X, ", buf[2]);
	i += sprintf(&str[i], "CCON2=0x%02X, ", buf[3]);
	i += sprintf(&str[i], "ModelS=0x%02X, ", buf[4]);
	i += sprintf(&str[i], "FGUpdate=0x%02X, ", buf[5]);
	i += sprintf(&str[i], "FMGrid=0x%02X, ", buf[6]);
	i += sprintf(&str[i], "ToggleCnt=%d, ", buf[7]);
	i += sprintf(&str[i], "ORUpdate=0x%02X, ", buf[8]);
	i += sprintf(&str[i], "UpState=0x%02X, ", buf[9]);
	i += sprintf(&str[i], "ChgVol=%d, ", (s16)BUF2U16_BG(&buf[10]));
	i += sprintf(&str[i], "TapCur=%d, ", (s16)BUF2U16_BG(&buf[12]));
	i += sprintf(&str[i], "ChgCur=%d, ", (s16)BUF2U16_BG(&buf[14]));
	i += sprintf(&str[i], "ChgRes=%d, ", (s16)BUF2U16_BG(&buf[16]));
	i += sprintf(&str[i], "PrevI=%d, ", (s16)BUF2U16_BG(&buf[18]));
	i += sprintf(&str[i], "DeltaC=%d, ", (s16)BUF2U16_BG(&buf[20]));
	i += sprintf(&str[i], "SOCJmpCnt=%d, ", buf[22]);
	i += sprintf(&str[i], "SOWJmpCnt=%d, ", buf[23]);
	i += sprintf(&str[i], "OcvVcell=%d, ", (s16)BUF2U16_BG(&buf[24]));
	i += sprintf(&str[i], "FGMeas=%d, ", (s16)BUF2U16_BG(&buf[26]));
	i += sprintf(&str[i], "FGPrid=%d, ", (s16)BUF2U16_BG(&buf[28]));
	i += sprintf(&str[i], "FastTime=%d, ", (s16)BUF2U16_BG(&buf[30]));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK6 is %s", str);

	/* 20230505, Ethan. Gauge Block Qmax1 */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK_Qmax1, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK_Qmax1, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "QmaxInfoFlag=0x%02X, ", buf[13]);
	i += sprintf(&str[i], "QmaxRLX_DOD=%d, ", (s16)BUF2U16_BG(&buf[14 + 0]));
	i += sprintf(&str[i], "QmaxRLX_Volt=%d, ", (s16)BUF2U16_BG(&buf[14 + 2]));
	i += sprintf(&str[i], "QmaxRLX_PassedC=%d, ", (s16)BUF2U16_BG(&buf[14 + 4]));
	i += sprintf(&str[i], "QmaxRLX_InvalidPassedC=%d, ", (s16)BUF2U16_BG(&buf[14 + 6]));
	i += sprintf(&str[i], "QmaxRLX_Cycle=%d, ", BUF2U16_BG(&buf[14 + 8]));
	i += sprintf(&str[i], "QmaxRLX_Flag=0x%02X, ", buf[14 + 10]);

	j = max(i, j);
	pr_err("SH366100_GaugeLog: GAUGEBLOCK_Qmax1 is %s\n", str);

	/* Gauge Fusion Model */
	ret = fg_read_block(sm, CMD_GAUGEBLOCK_FG, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_GAUGEBLOCK_FG, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "FusionModel=");
	for (ret = 0; ret < 15; ret++)
		i += sprintf(&str[i], "0x%04X ", BUF2U16_BG(&buf[ret * 2]));
	j = max(i, j); //20211116, Ethan
	pr_err("SH366100_GaugeLog: FusionModel is %s", str); //20211116, Ethan

	//20211111, Ethan
	/* CADC Info */
	ret = fg_read_block(sm, CMD_CADCINFO, GAUGEINFO_LEN, buf);
	if (ret < 0) {
		pr_err("SH366100_GaugeLog: could not read CMD_CADCINFO, ret = %d\n", ret);
		goto fg_read_gaugeinfo_block_end; /* 20220105, Ethan */
	}

	memset(str, 0, GAUGESTR_LEN);
	i = 0;
	i += sprintf(&str[i], "Tick=%d, ", (u32)jiffies_now);  //20211116, Ethan. In case print twice
	i += sprintf(&str[i], "TEOFFSET=%d, ", (s16)BUF2U16_LT(&buf[0]));
	i += sprintf(&str[i], "UserOFFSET=%d, ", (s16)BUF2U16_LT(&buf[2]));
	i += sprintf(&str[i], "BoardOffset=%d, ", (s16)BUF2U16_LT(&buf[4]));
	i += sprintf(&str[i], "CADC25DEG=%d, ", (s16)BUF2U16_LT(&buf[6]));
	i += sprintf(&str[i], "CADCKR=%d, ", (s16)BUF2U16_LT(&buf[8]));
	i += sprintf(&str[i], "CurrentRatio=%d, ", (s16)BUF2U16_LT(&buf[10]));
	i += sprintf(&str[i], "TELiner=%d, ", (s16)BUF2U16_LT(&buf[12]));
	i += sprintf(&str[i], "UserLiner=%d, ", (s32)BUF2U32_LT(&buf[14]));
	i += sprintf(&str[i], "CADCOffset=%d, ", (s16)BUF2U16_LT(&buf[18]));
	i += sprintf(&str[i], "CADC=%d, ", (s32)BUF2U32_LT(&buf[20]));
	i += sprintf(&str[i], "Current=%d, ", (s16)BUF2U16_LT(&buf[24]));
	i += sprintf(&str[i], "BoardOffsetFlag=0x%04X, ", BUF2U16_LT(&buf[26]));
	i += sprintf(&str[i], "CurLiner=%d, ", (s32)BUF2U32_LT(&buf[28]));
	j = max(i, j);
	pr_err("SH366100_GaugeLog: CADCINFO is %s", str);

	//20211116, Ethan
	// j = max(i, j);
	// pr_err("SH366100_GaugeLog: FusionModel is %s", str);
	pr_err("SH366100_GaugeLog: max len=%d", j);

	ret = 0;

fg_read_gaugeinfo_block_end: /* 20220105, Ethan */
	mutex_unlock(&sm->cali_lock); /* 20220105, Ethan */
	return ret;
}
#endif

static s32 fg_read_soc(struct sh_fg_chip* sm)
{
	int ret;
	u16 data = 0;
	//static u8 buf[GAUGEINFO_LEN];
	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_SOC], &data);
	if (ret < 0) {
		pr_err("could not read SOC, ret = %d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_soc = data;
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_read_temperature(struct sh_fg_chip* sm, enum sh_fg_temperature_type temperature_type)
{
	s32 ret;
	u16 data = 0;

	if (temperature_type == TEMPERATURE_IN) {
		ret = sm->regs[SH_FG_REG_TEMPERATURE_IN];
	} else if (temperature_type == TEMPERATURE_EX) {
		ret = sm->regs[SH_FG_REG_TEMPERATURE_EX];
	} else {
		return -EINVAL;
	}

	ret = fg_read_sbs_word(sm, ret, &data);
	if (ret < 0) {
		pr_err("could not read temperature, ret = %d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_temp = (s32)((s16)(data - 2731));
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_read_volt(struct sh_fg_chip* sm)
{
	s32 ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_VOLTAGE], &data);
	if (ret < 0) {
		pr_err("could not read voltage, ret = %d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_volt = (s32)data * MA_TO_UA;
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_get_cycle(struct sh_fg_chip* sm)
{
	int ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_SOC_CYCLE], &data);
	if (ret < 0) {
		pr_err("read cycle reg fail ret = %d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_soc_cycle = data;
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_get_soh(struct sh_fg_chip* sm)
{
	int ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_BAT_HEALTH], &data);
	if (ret < 0) {
		pr_err("read cycle reg fail ret = %d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->soh = data;
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_read_current(struct sh_fg_chip* sm)
{
	int ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_CURRENT], &data);
	if (ret < 0) {
		pr_err("could not read current, ret = %d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_curr = (s32)((s16)data * MA_TO_UA);
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_read_fcc(struct sh_fg_chip* sm)
{
	int ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_BAT_FCC], &data);
	if (ret < 0) {
		pr_err("could not read FCC, ret=%d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_fcc = (s32)((s16)data * MA_TO_UA);
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_read_rmc(struct sh_fg_chip* sm)
{
	int ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_BAT_RMC], &data);
	if (ret < 0) {
		pr_err("could not read RMC, ret=%d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_rmc = (s32)((s16)data * MA_TO_UA);
	mutex_unlock(&sm->data_lock);
	return 0;
}

static s32 fg_read_designcap(struct sh_fg_chip* sm) /* 20211108, Ethan */
{
	int ret;
	u16 data = 0;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_DESIGN_CAPCITY], &data);
	if (ret < 0) {
		pr_err("could not read DesignCap, ret=%d\n", ret);
		return ret;
	}

	mutex_lock(&sm->data_lock);
	sm->batt_designcap = (s32)((s16)data * MA_TO_UA); /* 20211112, Ethan */
	mutex_unlock(&sm->data_lock);
	return 0;
}

#if !(IS_PACK_ONLY)
static s32 get_battery_status(struct sh_fg_chip* sm)
{
	union power_supply_propval ret = {
	    0,
	};
	s32 rc;

	if (sm->batt_psy == NULL)
		sm->batt_psy = power_supply_get_by_name("battery");
	if (sm->batt_psy) {
		/* if battery has been registered, use the status property */
		rc = power_supply_get_property(sm->batt_psy, POWER_SUPPLY_PROP_STATUS, &ret);
		if (rc) {
			pr_err("Battery does not export status: %d\n", rc);
			return POWER_SUPPLY_STATUS_UNKNOWN;
		}
		return ret.intval;
	}

	/* Default to false if the battery power supply is not registered. */
	pr_err("battery power supply is not registered\n");
	return POWER_SUPPLY_STATUS_UNKNOWN;
}

static bool is_battery_charging(struct sh_fg_chip* sm)
{
	return get_battery_status(sm) == POWER_SUPPLY_STATUS_CHARGING;
}

static void fg_vbatocv_check(struct sh_fg_chip* sm)
{
	sm->p_batt_voltage = sm->batt_volt;
	sm->p_batt_current = sm->batt_curr;
}

static s32 fg_cal_carc(struct sh_fg_chip* sm)
{
	fg_vbatocv_check(sm);
	sm->is_charging = is_battery_charging(sm);

	return 1;
}
#endif

static s32 fg_get_batt_status(struct sh_fg_chip* sm)
{
	if (!sm->batt_present)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	else if (sm->batt_dsg)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	else if (sm->batt_curr > 0)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static s32 fg_get_batt_capacity_level(struct sh_fg_chip* sm)
{
	if (!sm->batt_present)
		return POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
	else if (sm->batt_fc)
		return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
	else if (sm->batt_tc) /* [tc] always set when [fc] set */
		return POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
	else if (sm->batt_soc < 1) /* [soc1] always set when [socp] set */
		return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
	else if (sm->batt_soc1)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;
	else
		return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
}

static s32 fg_get_batt_health(struct sh_fg_chip* sm)
{
	if (!sm->batt_present)
		return POWER_SUPPLY_HEALTH_UNKNOWN;
	else if (sm->batt_ot)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (sm->batt_ut)
		return POWER_SUPPLY_HEALTH_COLD;
	else
		return POWER_SUPPLY_HEALTH_GOOD;
}

static enum power_supply_property fg_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_PRESENT,
    POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_CURRENT_NOW,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_CAPACITY_LEVEL,
    POWER_SUPPLY_PROP_TEMP,
    POWER_SUPPLY_PROP_CHARGE_FULL,
    POWER_SUPPLY_PROP_HEALTH,
    POWER_SUPPLY_PROP_TECHNOLOGY,
    POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
    POWER_SUPPLY_PROP_CHARGE_NOW,
    POWER_SUPPLY_PROP_CYCLE_COUNT,
/* add by wanwen add battery info class 20250711 start */
    POWER_SUPPLY_PROP_MODEL_NAME,
    POWER_SUPPLY_PROP_SERIAL_NUMBER,
/* add by wanwen add battery info class 20250711 end */
};

static void fg_monitor_workfunc(struct work_struct* work);

static s32 fg_get_property(struct power_supply* psy, enum power_supply_property psp, union power_supply_propval* val)
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	//s32 ret;

	/*
		if (time_is_before_jiffies((unsigned long)sm->last_update + 2 * HZ)) {
			cancel_delayed_work_sync(&sm->monitor_work);
			fg_monitor_workfunc(&sm->monitor_work.work);
		}
	*/

	int health_factor = 0;
	int charge_full_design_uah = 5300000;
	int charge_full_estimated_uah = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = fg_get_batt_status(sm);
		/* pr_info("fg POWER_SUPPLY_PROP_STATUS:%d\n", val->intval); */
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
#if IS_ADC_HIGHFREQ
		fg_read_volt(sm);
#endif
		val->intval = sm->batt_volt;
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;//sm->batt_present; /* updated in POWER_SUPPLY_PROP_STATUS */
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
#if IS_ADC_HIGHFREQ
		fg_read_current(sm);
#endif
		val->intval = sm->batt_curr;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
#if IS_ADC_HIGHFREQ
		fg_read_soc(sm);
#endif
		val->intval = sm->batt_soc;
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = fg_get_batt_capacity_level(sm); /* updated in POWER_SUPPLY_PROP_STATUS */
		break;

	case POWER_SUPPLY_PROP_TEMP:
#if IS_ADC_HIGHFREQ
		if (sm->en_temp_in)
			fg_read_temperature(sm, TEMPERATURE_IN);
		else if (sm->en_temp_ex)
			fg_read_temperature(sm, TEMPERATURE_EX);
#endif
		val->intval = sm->batt_temp;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
#if IS_ADC_HIGHFREQ
		fg_read_fcc(sm);
#endif
		// Health factor degration is 0.0375% per cycle for Li-Poly batteries, fallback
		// to 0 for heavily degraded batteries due to long-term use or harsh conditions.
		health_factor = 1000000 - (sm->batt_soc_cycle * 375);
		if (health_factor < 0) {
			health_factor = 0;
		}

		// Cast to long long to prevent overflow during multiplication, then apply the health factor
		// and divide by 1,000,000 to get the estimated full charge capacity in microampere-hours (uAh).
		charge_full_estimated_uah = (int)( ((long long)charge_full_design_uah * health_factor) / 1000000 );
		val->intval = charge_full_estimated_uah;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
#if IS_ADC_HIGHFREQ
		fg_read_designcap(sm);
#endif
		val->intval = 5300000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_NOW:
#if IS_ADC_HIGHFREQ
		fg_read_rmc(sm);
#endif
		val->intval = sm->batt_rmc;
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNT:
#if IS_ADC_HIGHFREQ
		fg_get_cycle(sm);
#endif
		val->intval = sm->batt_soc_cycle;
		break;

	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = fg_get_batt_health(sm);
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;
/* add by wanwen add battery info class 20250711 start */
	case POWER_SUPPLY_PROP_MODEL_NAME:
		pr_err("POWER_SUPPLY_PROP_MODEL_NAME is %s\n", g_battery_dm_info.device_model);
		val->strval = g_battery_dm_info.device_model;
		break;

	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		pr_err("POWER_SUPPLY_PROP_SERIAL_NUMBER is %s\n", g_battery_dm_info.serial_number);
		val->strval = g_battery_dm_info.serial_number;
		break;

/* add by wanwen add battery info class 20250711 end */
	default:
		return -EINVAL;
	}

	return 0;
}

static s32 fg_set_property(struct power_supply* psy, enum power_supply_property prop, const union power_supply_propval* val)
{
#if 0 /* 20211029, Ethan. */
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	switch (prop) {
	case POWER_SUPPLY_PROP_TEMP:
		sm->fake_temp = val->intval;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		sm->fake_soc = val->intval;
		power_supply_changed(sm->fg_psy);
		break;

	default:
		return -EINVAL;
	}
#endif
	return 0;
}

/* add by wanwen add battery info class 20250711 start */
int battery_info_get_property(struct power_supply* psy, enum battery_info_property psp, union power_supply_propval* val)
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	switch (psp) {
	case BAT_INFO_PROP_SOH:
		val->intval = sm->soh;
		break;

	case BAT_INFO_PROP_MANUFACTURER_DATE:
		pr_err("BAT_INFO_PROP_MANUFACTURER_DATE is %s\n", g_battery_dm_info.manufacturer_date);
		val->strval = g_battery_dm_info.manufacturer_date;
		break;

	case BAT_INFO_PROP_ACTIVATION_DATE:
		pr_err("BAT_INFO_PROP_ACTIVATION_DATE is %s\n", g_battery_dm_info.activation_date);
		val->strval = g_battery_dm_info.activation_date;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(battery_info_get_property);

int battery_info_set_property(struct power_supply* psy, enum battery_info_property prop, union power_supply_propval* val)
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	switch (prop) {
	case BAT_INFO_PROP_MANUFACTURER_DATE:
	case BAT_INFO_PROP_ACTIVATION_DATE:
		pr_err("set %d prop is %d\n", BAT_INFO_PROP_MANUFACTURER_DATE, val->intval);
		fg_set_act_mfr_date(sm, val->intval, prop);
		break;

	default:
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(battery_info_set_property);
/* add by wanwen add battery info class 20250711 end */

static s32 fg_prop_is_writeable(struct power_supply* psy, enum power_supply_property prop)
{
	return 0;
}

static void fg_external_power_changed(struct power_supply* psy)
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);

	cancel_delayed_work(&sm->monitor_work);
	schedule_delayed_work(&sm->monitor_work, 0);
}

static s32 fg_psy_register(struct sh_fg_chip* sm)
{
	struct power_supply_config fg_psy_cfg = {};

#if IS_PACK_ONLY
	sm->fg_psy_d.name = "ext-bat";
	sm->fg_psy_d.type = POWER_SUPPLY_TYPE_UNKNOWN;
#else
	sm->fg_psy_d.name = "ext-bat";
	sm->fg_psy_d.type = POWER_SUPPLY_TYPE_UPS;
#endif
	sm->fg_psy_d.properties = fg_props;
	sm->fg_psy_d.num_properties = ARRAY_SIZE(fg_props);
	sm->fg_psy_d.get_property = fg_get_property;
	sm->fg_psy_d.set_property = fg_set_property;
	sm->fg_psy_d.external_power_changed = fg_external_power_changed;
	sm->fg_psy_d.property_is_writeable = fg_prop_is_writeable;

	fg_psy_cfg.drv_data = sm;
	fg_psy_cfg.num_supplicants = 0;

	sm->fg_psy = devm_power_supply_register(sm->dev, &sm->fg_psy_d, &fg_psy_cfg);
	if (IS_ERR(sm->fg_psy)) {
		pr_err("Failed to register fg_psy");
		return PTR_ERR(sm->fg_psy);
	}

	return 0;
}

static void fg_psy_unregister(struct sh_fg_chip* sm)
{
	power_supply_unregister(sm->fg_psy);
}

static ssize_t fg_attr_show_rm(struct device* dev, struct device_attribute* attr, char* buf)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	int len;

	fg_read_rmc(sm);
	len = snprintf(buf, MAX_BUF_LEN, "%d\n", sm->batt_rmc);

	return len;
}

static ssize_t fg_attr_show_fcc(struct device* dev, struct device_attribute* attr, char* buf)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	int len;

	fg_read_fcc(sm);
	len = snprintf(buf, MAX_BUF_LEN, "%d\n", sm->batt_fcc);

	return len;
}

static ssize_t fg_attr_show_batt_volt(struct device* dev, struct device_attribute* attr, char* buf)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	int len;

    fg_read_volt(sm);
	len = snprintf(buf, MAX_BUF_LEN, "%d\n", sm->batt_volt);

	return len;
}

static DEVICE_ATTR(rm, S_IRUGO, fg_attr_show_rm, NULL);
static DEVICE_ATTR(fcc, S_IRUGO, fg_attr_show_fcc, NULL);
static DEVICE_ATTR(batt_volt, S_IRUGO, fg_attr_show_batt_volt, NULL);

static struct attribute* fg_attributes[] = {
    &dev_attr_rm.attr,
    &dev_attr_fcc.attr,
    &dev_attr_batt_volt.attr,
    NULL,
};

static const struct attribute_group fg_attr_group = {
    .attrs = fg_attributes,
};

#if ENABLE_CHGBLOCK
/* 20211112, Ethan. Charge Status */
static s32 fg_read_chargestatus(struct sh_fg_chip* sm)
{
	int ret;
	u8 buf[LEN_CHARGESTATUS];
	enum sh_fg_charge_temper_range temper_range;
	enum sh_fg_charge_degrade_flag degrade_flag;

	ret = fg_read_block(sm, CMD_CHARGESTATUS, LEN_CHARGESTATUS, &buf);
	if (ret < 0) {
		pr_err("could not read Charge Status, ret=%d\n", ret);
		return ret;
	}

	switch (buf[0]) {
	case 0x01:
		temper_range = TEMPER_RANGE_BELOW_T1;
		break;
	case 0x02:
		temper_range = TEMPER_RANGE_T1T2;
		break;
	case 0x04:
		temper_range = TEMPER_RANGE_T2T3;
		break;
	case 0x08:
		temper_range = TEMPER_RANGE_T3T4;
		break;
	case 0x10:
		temper_range = TEMPER_RANGE_T4T5;
		break;
	case 0x20:
		temper_range = TEMPER_RANGE_T5T6;
		break;
	case 0x40:
		temper_range = TEMPER_RANGE_ABOVE_T6;
		break;
	default:
		pr_err("Charge Status Temper Range Error, value=0x%02X\n", buf[0]);
		return -1;
	}

	switch (buf[1] & MASK_CHARGESTATUS_DEGRADE) {
	case 0x00:
		degrade_flag = DEGRADE_PHASE_0;
		break;
	case 0x01:
		degrade_flag = DEGRADE_PHASE_1;
		break;
	case 0x02:
		degrade_flag = DEGRADE_PHASE_2;
		break;
	case 0x03:
		degrade_flag = DEGRADE_PHASE_3;
		break;
	case 0x04:
		degrade_flag = DEGRADE_PHASE_4;
		break;
	default:
		pr_err("Charge Status Degrade flag Error, value=0x%02X\n", (buf[1] & MASK_CHARGESTATUS_DEGRADE));
		return -1;
	}

	mutex_lock(&sm->data_lock);
	sm->temper_range = temper_range;
	sm->degrade_flag = degrade_flag;
	mutex_unlock(&sm->data_lock);
	return 0;
}

/* 20211112, Ethan. Charge Status */
static enum sh_fg_charge_temper_range fg_get_charge_temper_range(struct device* dev)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	fg_read_chargestatus(sm);
	return sm->temper_range;
}

/* 20211112, Ethan. Charge Status */
static enum sh_fg_charge_degrade_flag fg_get_charge_degrade_flag(struct device* dev)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	fg_read_chargestatus(sm);
	return sm->degrade_flag;
}

/* 20211112, Ethan. Charge Status */
static s32 fg_set_charging_voltage(struct device* dev, enum sh_fg_charge_temper_range temper_range, s32 charging_voltage)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	s32 ret;
	u8 buf[LEN_CHARGEVOLTAGES];

	/* check input */
	switch (temper_range) {
	case TEMPER_RANGE_T1T2:
	case TEMPER_RANGE_T2T3:
	case TEMPER_RANGE_T3T4:
	case TEMPER_RANGE_T4T5:
	case TEMPER_RANGE_T5T6:
		break;
	default:
		pr_err("fg_write_charging_voltage error! temper_range input error");
		return -1;
	}

	if (charging_voltage <= 0) {
		pr_err("fg_write_charging_voltage error! charging_voltage input error");
		return -1;
	}

	/* read block */
	ret = fg_read_block(sm, CMD_CHARGEVOLTAGES, LEN_CHARGEVOLTAGES, buf);
	if (ret < 0) {
		pr_err("fg_write_charging_voltage error! Could not read charge-voltge block!, ret=%d", ret);
		return ret;
	}

	/* modify block. little endian */
	ret = ((s32)temper_range << 1);
	buf[ret] = (u8)(charging_voltage & 0xFF);
	buf[ret + 1] = (u8)((charging_voltage >> 8) & 0xFF);

	/* write block */
	ret = fg_write_block(sm, CMD_CHARGEVOLTAGES, LEN_CHARGEVOLTAGES, buf);
	if (ret < 0) {
		pr_err("fg_write_charging_voltage error! Could not write charge-voltge block!, ret=%d", ret);
		return ret;
	}

	return 0;
}
#endif

/* 20211112, Ethan. Termniate Voltage */
static s32 fg_read_terminate_voltage(struct sh_fg_chip* sm)
{
	int ret;
	u8 buf[LEN_TERMINATEVOLT];

	ret = fg_read_block(sm, CMD_TERMINATEVOLT, LEN_TERMINATEVOLT, buf);
	if (ret < 0) {
		pr_err("could not read Terminate Voltage, ret=%d\n", ret);
		return ret;
	}

	ret = (s32)(buf[0] | ((u16)buf[1] << 8));
	mutex_lock(&sm->data_lock);
	sm->terminate_voltage = ret * MA_TO_UA; /* 20211208, Ethan */
	mutex_unlock(&sm->data_lock);
	return 0;
}

/* 20211112, Ethan. Termniate Voltage */
static __maybe_unused s32 fg_get_terminate_voltage(struct device* dev)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	fg_read_terminate_voltage(sm);
	return sm->terminate_voltage;
}

/* 20211112, Ethan. Termniate Voltage */
static __maybe_unused s32 fg_set_terminate_voltage(struct device* dev, s32 terminate_voltage)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	s32 ret;
	u8 buf[LEN_TERMINATEVOLT];

	/* check input */
	if (terminate_voltage <= 0) {
		pr_err("fg_set_terminate_voltage error! terminate_voltage input error");
		return -1;
	}

	/* read block */
	ret = fg_read_block(sm, CMD_TERMINATEVOLT, LEN_TERMINATEVOLT, buf);
	if (ret < 0) {
		pr_err("fg_set_terminate_voltage error! Could not read terminate_voltage block!, ret=%d", ret);
		return ret;
	}

	/* modify block. little endian */
	buf[0] = (u8)(terminate_voltage & 0xFF);
	buf[1] = (u8)((terminate_voltage >> 8) & 0xFF);

	/* write block */
	ret = fg_write_block(sm, CMD_TERMINATEVOLT, LEN_TERMINATEVOLT, buf);
	if (ret < 0) {
		pr_err("fg_set_terminate_voltage error! Could not write terminate_voltageblock!, ret=%d", ret);
		return ret;
	}

	return 0;
}

/* 20211113, Ethan */
static __maybe_unused s32 fg_get_user_info(struct device* dev, s32 length, u8* pbuf)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	s32 ret;

	if (length > LEN_USERBUFFER)
		length = LEN_USERBUFFER;

	ret = fg_read_block(sm, CMD_USERBUFFER, length, pbuf);
	if (ret < 0) {
		pr_err("fg_get_user_info error! ret=%d", ret);
		return ret;
	}

	return 0;
}

/* 20211113, Ethan */
static __maybe_unused s32 fg_set_user_info(struct device* dev, s32 length, u8* pbuf)
{
	struct i2c_client* client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	s32 ret;

	if (length > LEN_USERBUFFER)
		length = LEN_USERBUFFER;

	ret = fg_write_block(sm, CMD_USERBUFFER, length, pbuf);
	if (ret < 0) {
		pr_err("fg_get_user_info error! ret=%d", ret);
		return ret;
	}

	return 0;
}

static void fg_refresh_status(struct sh_fg_chip* sm)
{
	//s32 ret;
	bool last_batt_inserted = sm->batt_present;
	bool last_batt_fc = sm->batt_fc;
	bool last_batt_ot = sm->batt_ot;
	bool last_batt_ut = sm->batt_ut;
	static s32 last_soc, last_temp;
	pr_err("start **********************************************\n");
	fg_read_status(sm);
	pr_err("batt_present=%d, sm->batt_socp=%d\n", sm->batt_present, sm->batt_socp);

	if (!last_batt_inserted && sm->batt_present) { /* battery inserted */
		pr_err("Battery inserted\n");
	} else if (last_batt_inserted && !sm->batt_present) { /* battery removed */
		pr_err("Battery removed\n");
		sm->batt_soc = -ENODATA;
		sm->batt_fcc = -ENODATA;
		sm->batt_volt = -ENODATA;
		sm->batt_curr = -ENODATA;
		sm->batt_temp = -ENODATA;
	}

	if ((last_batt_inserted != sm->batt_present) || (last_batt_fc != sm->batt_fc) || (last_batt_ot != sm->batt_ot) || (last_batt_ut != sm->batt_ut))
		power_supply_changed(sm->fg_psy);

	if (sm->batt_present) {
//		fg_read_gaugeinfo_block(sm);

		fg_read_current(sm);
		fg_read_soc(sm);
		fg_read_volt(sm);
		fg_get_cycle(sm);
		fg_get_soh(sm);
		fg_read_rmc(sm);
		fg_read_fcc(sm);
		fg_read_designcap(sm);
		if (sm->en_temp_in)
			fg_read_temperature(sm, TEMPERATURE_IN);
		else if (sm->en_temp_ex)
			fg_read_temperature(sm, TEMPERATURE_EX);

#if !(IS_PACK_ONLY)
		fg_cal_carc(sm);
#endif

		pr_err("RSOC:%d, Volt:%d, Current:%d, Temperature:%d\n", sm->batt_soc, sm->batt_volt, sm->batt_curr, sm->batt_temp);
		pr_err("RM:%d,FC:%d,FAST:%d\n", sm->batt_rmc, sm->batt_fcc, sm->fast_mode);

		if ((last_soc != sm->batt_soc) || (last_temp != sm->batt_temp)) {
			if (sm->fg_psy)
				power_supply_changed(sm->fg_psy);
		}

		last_soc = sm->batt_soc;
		last_temp = sm->batt_temp;
	}

	sm->last_update = jiffies;
	pr_err("end ************************************************\n\n");
}

#define SH366100_FFC_TERM_WAM_TEMP 350
#define SH366100_COLD_TEMP_TERM 0
#define BAT_FULL_CHECK_TIME 1

static __maybe_unused s32 fg_check_full_status(struct sh_fg_chip* sm)
{
	return 0;
}

static __maybe_unused s32 fg_check_recharge_status(struct sh_fg_chip* sm)
{
	return 0;
}

// drv add tankaikun, update fg state, 20250530 start
static void fg_monitor_workfunc(struct work_struct *work)
{
	struct delayed_work *delay_work;
	struct sh_fg_chip* sm;

	delay_work = container_of(work, struct delayed_work, work);
	sm = container_of(delay_work, struct sh_fg_chip, monitor_work);

	fg_refresh_status(sm);

	queue_delayed_work(sm->shfg_workqueue, &sm->monitor_work, msecs_to_jiffies(queue_delayed_work_time));
}
// drv add tankaikun, update fg state, 20250530 end

static s32 fg_get_device_id(struct i2c_client* client)
{
	struct sh_fg_chip* sm = i2c_get_clientdata(client);
	s32 ret;
	u16 data;

	ret = fg_read_sbs_word(sm, sm->regs[SH_FG_REG_DEVICE_ID], &data);
	if (ret < 0) {
		pr_err("Failed to read DEVICE_ID, ret = %d\n", ret);
		return ret;
	}

	pr_info("device_id = 0x%04X\n", data);
	return ret;
}

static bool fg_init(struct i2c_client* client)
{
	s32 ret;

	/*sh366100 i2c read check*/
	ret = fg_get_device_id(client);
	if (ret < 0) {
		pr_err("%s: fail to do i2c read(%d)\n", __func__, ret);
		return false;
	}

	return true;
}

static s32 fg_common_parse_dt(struct sh_fg_chip* sm)
{
	struct device* dev = &sm->client->dev;
	struct device_node* np = dev->of_node;

	BUG_ON(dev == 0);
	BUG_ON(np == 0);

	sm->gpio_int = of_get_named_gpio(np, "qcom,irq-gpio", 0);
	pr_info("gpio_int=%d\n", sm->gpio_int);

	if (!gpio_is_valid(sm->gpio_int)) {
		pr_info("gpio_int is not valid\n");
		sm->gpio_int = -EINVAL;
	}

	/* EN TEMP EX/IN */
	if (of_property_read_bool(np, "sm,en_temp_ex"))
		sm->en_temp_ex = true;
	else
		sm->en_temp_ex = 0;
	pr_info("Temperature EX enabled = %d\n", sm->en_temp_ex);

	if (of_property_read_bool(np, "sm,en_temp_in"))
		sm->en_temp_in = true;
	else
		sm->en_temp_in = 0;
	pr_info("Temperature IN enabled = %d\n", sm->en_temp_in);

	/* EN BATT DET  */
	if (of_property_read_bool(np, "sm,en_batt_det"))
		sm->en_batt_det = true;
	else
		sm->en_batt_det = 0;
	pr_info("Batt Det enabled = %d\n", sm->en_batt_det);

	return 0;
}

static s32 get_battery_id(struct sh_fg_chip* sm)
{
	return 0;
}

static s32 fg_gauge_unseal(struct sh_fg_chip* sm) /* 20211122, Ethan. Gauge Enable */
{
	s32 ret;

	ret = fg_write_sbs_word(sm, CMD_ALTMAC, (u16)CMD_UNSEALKEY);
	if (ret < 0)
		goto fg_gauge_unseal_End;
	HOST_DELAY(CMD_SBS_DELAY);

	ret = fg_write_sbs_word(sm, CMD_ALTMAC, (u16)(CMD_UNSEALKEY >> 16));
	if (ret < 0)
		goto fg_gauge_unseal_End;
	HOST_DELAY(CMD_SBS_DELAY);

	ret = 0;
fg_gauge_unseal_End:
	return ret;
}

static s32 fg_gauge_seal(struct sh_fg_chip* sm) /* 20211122, Ethan. Gauge Enable */
{
	return fg_write_sbs_word(sm, CMD_ALTMAC, CMD_SEAL);
}

#define CMD_BATINSERT_WORD 0x66 /* 20220427, Ethan */
static s32 fg_get_user_word(struct sh_fg_chip* sm, u16* userWord) /* 20220427, Ethan */
{
	return fg_read_sbs_word(sm, CMD_BATINSERT_WORD, userWord);
}

static s32 fg_set_user_word(struct sh_fg_chip* sm, u16 userWord) /* 20220427, Ethan */
{
	//userWord = ((userWord&0x00ff)<<8)|((userWord&0xff00)>>8);
	return fg_write_sbs_word(sm, CMD_BATINSERT_WORD, userWord);
}
static s32 fg_gauge_runstate_check(struct sh_fg_chip* sm) /* 20211126, Ethan */
{
	s32 ret;
	u16 oemflag;
	s32 retry_cnt;
	u32 socFlag = 0, qenFlag = 0, ltFlag = 0;
	u32 cali_checked = 0;
	u8 bufComm[32];

	/* 20211208, Ethan. In case por with poor connection */
	for (retry_cnt = 0; retry_cnt < 5; retry_cnt++) {
		ret = fg_read_soc(sm);
		if (ret < 0)
			goto fg_gauge_runstate_check_End;
		HOST_DELAY(CMD_SBS_DELAY);

		ret = fg_read_volt(sm);
		if (ret < 0)
			goto fg_gauge_runstate_check_End;
		HOST_DELAY(CMD_SBS_DELAY);

		ret = fg_read_current(sm);
		if (ret < 0)
			goto fg_gauge_runstate_check_End;
		HOST_DELAY(CMD_SBS_DELAY);

		//20220106, Ethan
		ret = fg_read_temperature(sm,TEMPERATURE_EX);
		if (ret < 0)
			goto fg_gauge_runstate_check_End;

		ret = fg_read_terminate_voltage(sm);
		if (ret < 0)
			goto fg_gauge_runstate_check_End;
		HOST_DELAY(CMD_SBS_DELAY);

		ret = fg_read_sbs_word(sm, CMD_OEMFLAG, &oemflag);
		if (ret < 0)
			goto fg_gauge_runstate_check_End;

		//20220106, Ethan
		socFlag = !!(sm->batt_temp < TEMPER_MIN_RESET);
		if (sm->batt_curr <= 0) {
			socFlag |= !!((sm->batt_volt > VOLT_MIN_RESET) && (sm->batt_soc < SOC_MIN_RESET));
			socFlag |= !!((sm->batt_volt > (3850 * MA_TO_UA)) && (sm->batt_soc < 3));
			socFlag |= !!((sm->batt_soc == 0) && ((sm->batt_volt - sm->terminate_voltage) > DELTA_VOLT));
		}
		if (sm->batt_volt - sm->terminate_voltage > (50 * MA_TO_UA)) { /* 20220422, Ethan */
			socFlag |= !!(sm->batt_dod0 >= 16200);
		}

		if (socFlag) { 
			ret = fg_gauge_unseal(sm);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;

			ret = fg_write_sbs_word(sm, CMD_ALTMAC, (u16)CMD_RESET);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;
			HOST_DELAY(DELAY_RESET);
		}

		 /* por with poor connection */
		qenFlag = !!((oemflag & CMD_MASK_OEM_GAUGEEN) != CMD_MASK_OEM_GAUGEEN); /* Gauge Un-enable */
		if (qenFlag) {  //Gauge Disable. Re-enable gauge
			ret = fg_gauge_unseal(sm);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;

			ret = fg_write_sbs_word(sm, CMD_ALTMAC, (u16)CMD_ENABLE_GAUGE);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;
			HOST_DELAY(DELAY_ENABLE_GAUGE);
		}

		ltFlag = !!((oemflag & CMD_MASK_OEM_LIFETIMEEN) != CMD_MASK_OEM_LIFETIMEEN);
		if (ltFlag) { //Lifetime Disable. Re-enable lifetime
			ret = fg_gauge_unseal(sm);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;

			ret = fg_write_sbs_word(sm, CMD_ALTMAC, (u16)CMD_ENABLE_LIFETIME);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;
			HOST_DELAY(DELAY_ENABLE_GAUGE);
		}

		if (!cali_checked) { //20220118, Ethan
			ret = fg_gauge_unseal(sm);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;
			
			ret = fg_read_block(sm, CMD_OEMINFO, 32, bufComm);
			if (ret < 0)
				goto fg_gauge_runstate_check_End;
			ret = BUF2U16_LT(&bufComm[INDEX_OEMINFO_INTFLAG]);
			sm->is_enable_autocali = !!(ret & MASK_OEMINFO_AUTOCALI);
			cali_checked = 1;
		}
		ret = 0;
		break;

fg_gauge_runstate_check_End:
		HOST_DELAY(CMD_SBS_DELAY << 1);
	}

	pr_err("fg_gauge_runstate_check: soc=%d, volt=%d, termVolt=%d, OEMFlag=0x%04X, QEN_FLAG=%u, SOC_FLAG=%u, LifeTime_Flag=%u, AutoCali=%u", 
		sm->batt_soc, sm->batt_volt, sm->terminate_voltage, oemflag, qenFlag, socFlag, ltFlag, sm->is_enable_autocali);

	fg_gauge_seal(sm);
	return ret;
}

static __maybe_unused s32 Host_Force_Reset(struct power_supply* psy) /* 20220524, Ethan */
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	s32 ret = fg_gauge_unseal(sm);
	if (ret < 0)
		goto Host_Force_Reset_End;

	ret = fg_write_sbs_word(sm, CMD_ALTMAC, (u16)CMD_RESET);
	if (ret < 0)
		goto Host_Force_Reset_End;
	HOST_DELAY(DELAY_RESET);

Host_Force_Reset_End:
	pr_err("SH366100 Host_Force_Reset: ret=%d", ret);
	return ret;
}

/* prize add by fangduozhu 20250717, only update on normal mode start */
static int Get_Bootmode(void)
{
	struct tag_bootmode {
		u32 size;
		u32 tag;
		u32 bootmode;
		u32 boottype;
	} *tag = NULL;
	struct device_node *of_chosen = NULL;
	int ret = -1;

	of_chosen = of_find_node_by_path("/chosen");
	if (of_chosen == NULL) {
		return -EINVAL;
	}
	tag = (struct tag_bootmode *)of_get_property(of_chosen,
		"atag,boot", NULL);
	if (tag) {
		ret = tag->bootmode;
	}

	return ret;
}
/* prize add by fangduozhu 20250717, only update on normal mode end */

static s32 Check_Chip_Version(struct sh_fg_chip* sm)
{
	struct device* dev = &sm->client->dev;
	struct device_node* np = dev->of_node;
	u32 version_main, version_date, version_afi, version_ts;
	u16 sector_flag = 0; //20220422, Ethan
	s32 ret = CHECK_VERSION_ERR;
	u16 temp;
	s32 date;
	/* 20211025, Ethan. IAP Fail Check */
	struct sh_decoder decoder;
	u8 iap_read[IAP_READ_LEN];

	/* prize add by fangduozhu 20250717, only update on normal mode start */
	if (Get_Bootmode() != 0) {
		pr_err("Check_Chip_Version: not normal mode, bypass\n");
		return CHECK_VERSION_ERR;
	}
	/* prize add by fangduozhu 20250717, only update on normal mode end */
	/* battery_params node*/
	np = of_find_node_by_name(of_node_get(np), "battery_params");
	if (np == NULL) {
		pr_err("Check_Chip_Version: Cannot find child node \"battery_params\"\n");
		return CHECK_VERSION_ERR;
	}

	of_property_read_u32(np, "version_main", &version_main);
	of_property_read_u32(np, "version_date", &version_date);
	of_property_read_u32(np, "version_afi2", &version_afi);
	of_property_read_u32(np, "version_ts", &version_ts);
	of_property_read_u8(np, "iap_twiadr", &decoder.addr); /* 20211025, Ethan */

	pr_err("Check_Chip_Version: main=0x%04X, date=0x%08X, afi=0x%04X, ts=0x%04X", version_main, version_date, version_afi, version_ts);

	/* 20211025, Ethan. IAP Fail Check. iap addr may differ from normal addr */
	//从这里开始属于isp
	decoder.addr = sm->client->addr;
	//pr_err("decoder.addr = 0x%02x\n",decoder.addr);
	decoder.reg = (u8)CMD_IAPSTATE_CHECK;
	decoder.length = IAP_READ_LEN;
	if ((fg_decode_iic_read(sm, &decoder, iap_read) >= 0) && (iap_read[0] != 0) && (iap_read[1] != 0)) {
		pr_err("Check_Chip_Version: ic is in iap mode, force update all\n");
		ret = CHECK_VERSION_WHOLE_CHIP;
		goto Check_Chip_Version_End;
	}
	HOST_DELAY(CMD_SBS_DELAY); /* 20211029, Ethan */

	if (fg_gauge_unseal(sm) < 0) { /* 20211122, Ethan. Gauge Enable */
		ret = CHECK_VERSION_ERR;
		goto Check_Chip_Version_End;
	}

	/* check fw version. FW update must update afi(for iap check flag) */
	if (fg_read_sbs_word(sm, CMD_FWVERSION_MAIN, &temp) < 0) {
		ret = CHECK_VERSION_ERR;
		goto Check_Chip_Version_End;
	}
	pr_err(" Chip_Version: ic main=0x%04X\n ", temp);

	if (temp < version_main) {
		ret = CHECK_VERSION_WHOLE_CHIP;
		goto Check_Chip_Version_End;
	} else if (temp > version_main)
		ret = CHECK_VERSION_OK;
	else { /* version equal, check date */
		if (fg_read_sbs_word(sm, CMD_FWDATE1, &temp) < 0) {
			ret = CHECK_VERSION_ERR;
			goto Check_Chip_Version_End;
		}
		msleep(CMD_SBS_DELAY);
		date = (u32)temp << 16;

		if (fg_read_sbs_word(sm, CMD_FWDATE2, &temp) < 0) {
			ret = CHECK_VERSION_ERR;
			goto Check_Chip_Version_End;
		}
		date |= (temp & FW_DATE_MASK);
		pr_err(" Chip_Version: ic date=0x%08X \n", date);
		if (date < version_date) {
			ret = CHECK_VERSION_WHOLE_CHIP;
			goto Check_Chip_Version_End;
		} else
			ret = CHECK_VERSION_OK;
	}

	/* check afi */
	if (fg_read_sbs_word(sm, CMD_AFI_STATIC_SUM, &temp) < 0) {
		ret = CHECK_VERSION_ERR;
		goto Check_Chip_Version_End;
	}
	//20220422, Ethan
	if (fg_read_sbs_word(sm, CMD_SECTOR_FLAG, &sector_flag) < 0) {
		ret = CHECK_VERSION_ERR;
		goto Check_Chip_Version_End;
	}

	pr_err(" Chip_Version: ic afi=0x%04X, sector_flag=0x%04X\n ", temp, sector_flag); //20220422, Ethan
	if ((temp != version_afi) || ((sector_flag & MASK_SECTOR_FLAG) != 0)) //20220422, Ethan
		ret |= CHECK_VERSION_AFI;

	/* check TS */
	if (fg_read_sbs_word(sm, CMD_TS_VER, &temp) < 0) {
		ret = CHECK_VERSION_ERR;
		goto Check_Chip_Version_End;
	}
	// pr_err(" Chip_Version: ic ts=0x%04X \n", temp);
	// if (temp != version_ts)
	// 	ret |= CHECK_VERSION_TS;

Check_Chip_Version_End:
	fg_gauge_seal(sm); /* 20211122, Ethan. Gauge Enable */
	return ret;
}

int file_decode_process(struct sh_fg_chip* sm, char* profile_name)
{
	struct device* dev = &sm->client->dev;
	struct device_node* np = dev->of_node;
	u8* pBuf = NULL;
	u8* pBuf_Read = NULL;
	char strDebug[FILEDECODE_STRLEN];
	int buflen;
	int wait_ms;
	int i, j;
	int line_length;
	//int i_bak = 0;
	int result = -1;
	int retry;
	//int retry2 = 0;

	pr_err("file_decode_process: start");

	/* battery_params node*/
	np = of_find_node_by_name(of_node_get(np), "battery_params");
	if (np == NULL) {
		pr_err("file_decode_process: Cannot find child node \"battery_params\"");
		return -EINVAL;
	}

	buflen = of_property_count_u8_elems(np, profile_name);
	pr_err("file_decode_process: ele_len=%d, key=%s", buflen, profile_name);

	pBuf = (u8*)devm_kzalloc(dev, buflen, 0);
	pBuf_Read = (u8*)devm_kzalloc(dev, BUF_MAX_LENGTH, 0);

	if ((pBuf == NULL) || (pBuf_Read == NULL)) {
		result = ERRORTYPE_ALLOC;
		pr_err("file_decode_process: kzalloc error");
		goto main_process_error;
	}

	result = of_property_read_u8_array(np, profile_name, pBuf, buflen);
	if (result) {
		pr_err("file_decode_process: read dts fail %s\n", profile_name);
		goto main_process_error;
	}
	print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, pBuf, 32);
	pr_err("file_decode_process: first data=%s", strDebug);

	i = 0;
	j = 0;
	while (i < buflen) {
		/* delay: b0: operate, b1: 2, b2-b3: time, big-endian */
		/* other: b0: operate, b1: TWIADR, b2: reg, b3: data_length, b4...end: item */
		if (pBuf[i + INDEX_TYPE] == OPERATE_WAIT) {
			wait_ms = ((int)pBuf[i + INDEX_WAIT_HIGH] * 256) + pBuf[i + INDEX_WAIT_LOW];

			if (pBuf[i + INDEX_WAIT_LENGTH] == 2) {
				HOST_DELAY(wait_ms+10); /* 20211029, Ethan */
				i += LINELEN_WAIT;
			} else {
				print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, &pBuf[i + INDEX_TYPE], 32);
				pr_err("file_decode_process wait error! index=%d, str=%s", i, strDebug);
				result = ERRORTYPE_LINE;
				goto main_process_error;
			}
		} else if (pBuf[i + INDEX_TYPE] == OPERATE_READ) {
			line_length = pBuf[i + INDEX_LENGTH];
			if (line_length <= 0) {
				result = ERRORTYPE_LINE;
				goto main_process_error;
			}

			/* 20211026, Ethan. IAP addr may differ from default addr */
			/*if (fg_read_block(sm, pBuf[i + INDEX_REG], line_length, pBuf_Read) < 0) { */
			if (fg_decode_iic_read(sm, (struct sh_decoder*)&pBuf[i + INDEX_ADDR], pBuf_Read) < 0) {
				print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, &pBuf[i + INDEX_TYPE], 32);
				pr_err("file_decode_process read error! index=%d, str=%s", i, strDebug);
				result = ERRORTYPE_COMM;
				goto main_process_error;
			}

			i += LINELEN_READ;
		} else if (pBuf[i + INDEX_TYPE] == OPERATE_COMPARE) {
			line_length = pBuf[i + INDEX_LENGTH];
			if (line_length <= 0) {
				result = ERRORTYPE_LINE;
				goto main_process_error;
			}

			for (retry = 0; retry < COMPARE_RETRY_CNT; retry++) {
				/* 20211026, Ethan. IAP addr may differ from default addr */
				/*if (fg_read_block(sm, pBuf[i + INDEX_REG], line_length, pBuf_Read) < 0) { */
				if (fg_decode_iic_read(sm, (struct sh_decoder*)&pBuf[i + INDEX_ADDR], pBuf_Read) < 0) {
					print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, &pBuf[i + INDEX_TYPE], 32);
					pr_err("file_decode_process compare_read error! index=%d, str=%s", i, strDebug);
					result = ERRORTYPE_COMM;
					goto file_decode_process_compare_loop_end;
				}

				print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, pBuf_Read, line_length);
				pr_debug("file_decode_process loop compare: IC read=%s", strDebug);

				result = 0;
				for (j = 0; j < line_length; j++) {
					if (pBuf[INDEX_DATA + i + j] != pBuf_Read[j]) {
						result = ERRORTYPE_COMPARE;
						break;
					}
				}

				if (result == 0)
					break;

				/* compare fail */
				print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, &pBuf[i + INDEX_TYPE], 32);
				pr_err("file_decode_process compare error! index=%d, retry=%d, host=%s", i, retry, strDebug);
				print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, pBuf_Read, 32);
				pr_err("ic=%s", strDebug);

			file_decode_process_compare_loop_end:
				HOST_DELAY(COMPARE_RETRY_WAIT); /* 20211029, Ethan */
			}

			if (retry >= COMPARE_RETRY_CNT) {
				result = ERRORTYPE_COMPARE; /* 20211125, Ethan */
				goto main_process_error;
			}

			i += LINELEN_COMPARE + line_length;
		} else if (pBuf[i + INDEX_TYPE] == OPERATE_WRITE) {
			line_length = pBuf[i + INDEX_LENGTH];
			if (line_length <= 0) {
				result = ERRORTYPE_LINE;
				goto main_process_error;
			}

			/* 20211026, Ethan. IAP addr may differ from default addr */
			/* if (fg_write_block(sm, pBuf[i + INDEX_REG], line_length, &pBuf[i + INDEX_DATA]) != 0) { */
			if (fg_decode_iic_write(sm, (struct sh_decoder*)&pBuf[i + INDEX_ADDR]) != 0) {
				print_buffer(strDebug, sizeof(char) * FILEDECODE_STRLEN, &pBuf[i + INDEX_TYPE], 32);
				pr_err("file_decode_process write error! index=%d, str=%s", i, strDebug);
				result = ERRORTYPE_COMM;
				goto main_process_error;
			}

			i += LINELEN_WRITE + line_length;
		} else {
			result = ERRORTYPE_LINE;
			goto main_process_error;
		}
	}
	result = ERRORTYPE_NONE;

main_process_error:
	pr_err("file_decode_process end: result=%d", result);
	return result;
}


static __maybe_unused s32 fg_gauge_check_autocali(struct sh_fg_chip* sm) /* 20220117, Ethan */
{
	s32 ret;
	u8 bufComm[32];
	u16 temp16;

	ret = fg_gauge_unseal(sm);
	if (ret < 0) {
		pr_err("fg_gauge_check_autocali Comm error! cannot unseal!");
		ret = CALI_ERR_COMM;
		goto fg_gauge_check_autocali_end;
	}
	HOST_DELAY(CMD_SBS_DELAY);

	ret = fg_read_block(sm, CMD_CALIED_FLAG, 32, bufComm);
	if (ret < 0) {
		pr_err("fg_gauge_check_autocali Comm error! cannot read cali flag!"); 
		ret = CALI_ERR_COMM;
		goto fg_gauge_check_autocali_end;
	}
	HOST_DELAY(CMD_SBS_DELAY);
	temp16 = BUF2U16_BG(&bufComm[INDEX_CALIED_FLAG]);
	if (temp16 == FLAG_CALIED_FLAG) {
		pr_err("fg_gauge_check_autocali End. FG is calied! cali-flag=0x%04X", temp16);
		ret = 1;
		goto fg_gauge_check_autocali_end;
	} else {
		bufComm[INDEX_CALIED_FLAG] = 0;
		bufComm[INDEX_CALIED_FLAG + 1] = 0;
		fg_write_block(sm, CMD_CALIED_FLAG, LEN_CALIED_FLAG, bufComm);

		pr_err("fg_gauge_check_autocali End. FG is un-calied! cali-flag=0x%04X", temp16);
		ret = 0;
		goto fg_gauge_check_autocali_end;
	}

fg_gauge_check_autocali_end:
	HOST_DELAY(CMD_SBS_DELAY);
	fg_gauge_seal(sm);

	pr_err("fg_gauge_check_autocali: ret=%d, AutoCali=%u", ret, sm->is_enable_autocali);

	return ret;
}

static __maybe_unused s32 fg_gauge_check_autocali_entry(struct power_supply* psy) /* 20220117, Ethan */
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	int i;
	int ret=0;

	for (i = 0; i < 2; i++) {
		ret = -1;
		if (mutex_trylock(&sm->cali_lock)) {
			ret = fg_gauge_check_autocali(sm);
			mutex_unlock(&sm->cali_lock);
		} else {
			pr_err("fg_gauge_check_autocali: could not get mutex!");
		}

		if (ret >= 0)
			break;
		HOST_DELAY(200);
	}
	return ret;
}

static s32 fg_gauge_enable_autocali(struct sh_fg_chip* sm) /* 20220117, Ethan */
{
	s32 ret;
	u8 bufComm[32];
	u16 temp16;

	if (!sm->is_enable_autocali) {
		pr_err("fg_gauge_eable_autocali error! FW donot support auto-cali!");
		ret = CALI_ERR_UNSUPPORT;
		goto fg_gauge_eable_autocali_end;
	}
	ret = fg_gauge_unseal(sm);
	if (ret < 0) {
		pr_err("fg_gauge_eable_autocali Comm error! cannot unseal!");
		ret = CALI_ERR_COMM;
		goto fg_gauge_eable_autocali_end;
	}
	HOST_DELAY(CMD_SBS_DELAY);

	ret = fg_read_block(sm, CMD_CALIED_FLAG, 32, bufComm);
	if (ret < 0) {
		pr_err("fg_gauge_eable_autocali Comm error! cannot read cali flag!"); 
		ret = CALI_ERR_COMM;
		goto fg_gauge_eable_autocali_end;
	}
	HOST_DELAY(CMD_SBS_DELAY);
	temp16 = BUF2U16_BG(&bufComm[INDEX_CALIED_FLAG]);
	if (temp16 == FLAG_CALIED_FLAG) {
		pr_err("fg_gauge_eable_autocali OK! FG has already calied! cali-flag=0x%04X", temp16);
		ret = CALI_ERR_NONE;
		goto fg_gauge_eable_autocali_end;
	}

	bufComm[INDEX_CALIED_FLAG] = (FLAG_CALIED_ENABLE >> 8) & 0xFF;
	bufComm[INDEX_CALIED_FLAG + 1] = FLAG_CALIED_ENABLE & 0xFF;

	ret = fg_write_block(sm, CMD_CALIED_FLAG, LEN_CALIED_FLAG, bufComm);
	if (ret < 0) {
		pr_err("fg_gauge_calibrate_board Comm error! cannot write CMD_CALIED_FLAG!");
		ret = CALI_ERR_COMM;
		goto fg_gauge_eable_autocali_end;
	}
	ret = CALI_ERR_NONE;

fg_gauge_eable_autocali_end:
	HOST_DELAY(CMD_SBS_DELAY);
	fg_gauge_seal(sm);

	pr_err("fg_gauge_calibrate_board: ret=%d", ret);

	return ret;
}

static __maybe_unused s32 fg_gauge_eable_autocali_entry(struct power_supply* psy) /* 20220117, Ethan */
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	int i;
	int ret=0;

	for (i = 0; i < 2; i++) {
		ret = -1;
		if (mutex_trylock(&sm->cali_lock)) {
			ret = fg_gauge_enable_autocali(sm);
			mutex_unlock(&sm->cali_lock);
		} else {
			pr_err("fg_gauge_eable_autocali: could not get mutex!");
		}

		if (ret >= 0)
			break;
		HOST_DELAY(200);
	}
	return ret;
}

/* 20220121, Ethan. Enter Cali-mode */
static s32 fg_enter_calibration_mode(struct sh_fg_chip* sm)
{
#define CMD_ENTER_CALI (CMDMASK_ALTMAC_W | 0xE014)
#define LEN_ENTER_CALI 6
#define CMD_ENTER_OEM (CMDMASK_ALTMAC_W | 0xE001)
#define LEN_ENTER_OEM 8
#define PACKCON_MASK 
	u8 bufcmd[LEN_ENTER_CALI] = { 0x45, 0x54, 0x43, 0x41, 0x4C, 0x49 };
	u8 bufoem[LEN_ENTER_OEM] = { 0x65, 0x4E, 0x54, 0x45, 0x52, 0x4F, 0x45, 0x4D };
	s32 retry;
	u16 temp = 0;
	s32 ret;

//20220226, Ethan
	//u8 buf[32];

	for (retry = 0; retry < 5; retry++) {
		ret = fg_read_sbs_word(sm, CMD_OEMFLAG, &temp);
		HOST_DELAY(CMD_SBS_DELAY);
		if (ret >= 0) {
			if ((temp & CMD_MASK_OEM_CALI) == CMD_MASK_OEM_CALI) {
				ret = 0;
				goto fg_check_calibration_mode_end;
			}

			ret = fg_gauge_unseal(sm);
			if (ret < 0)
				continue;
			msleep(CMD_SBS_DELAY);

			ret = fg_write_block(sm, CMD_ENTER_OEM, LEN_ENTER_OEM, bufoem);
			if (ret < 0)
				continue;
			msleep(CMD_SBS_DELAY);
			
			ret = fg_write_block(sm, CMD_ENTER_CALI, LEN_ENTER_CALI, bufcmd);
			if (ret >= 0)
				break;
		}
		HOST_DELAY(200);
	}

fg_check_calibration_mode_end:

	ret = fg_read_sbs_word(sm, CMD_OEMFLAG, &temp);
	if (ret >= 0)
		ret = ((temp & CMD_MASK_OEM_CALI) == CMD_MASK_OEM_CALI) ? 0 : -1;

	pr_err("fg_enter_calibration_mode: ret = %d, oemFlag=0x%04X\n", ret, temp);
	return ret;
}

/* 20220121, Ethan. Exit Cali-mode */
static s32 fg_exit_calibration_mode(struct sh_fg_chip* sm)
{
#define CMD_EXIT_CALI (CMDMASK_ALTMAC_W | 0xE015)
#define LEN_EXIT_CALI 6
	u8 bufcmd[LEN_EXIT_CALI] = { 0x65, 0x78, 0x63, 0x61, 0x6C, 0x69 };
	s32 retry;
	s32 ret;
#define CMD_EXIT_OEM (CMDMASK_ALTMAC_W | 0xE018)
#define LEN_EXIT_OEM 7
	u8 bufoem[LEN_EXIT_OEM] = { 0x45, 0x78, 0x69, 0x74, 0x6F, 0x65, 0x6D };
	u16 temp;

	for (retry = 0; retry < 2; retry++) {
		ret = fg_write_block(sm, CMD_EXIT_CALI, LEN_EXIT_CALI, bufcmd);
		msleep(CMD_SBS_DELAY);

		ret |= fg_write_block(sm, CMD_EXIT_OEM, LEN_EXIT_OEM, bufoem);
		if (ret >= 0)
			break;

		HOST_DELAY(200);
	}

//fg_check_calibration_mode_end:
	fg_gauge_seal(sm);
	msleep(CMD_SBS_DELAY);

	fg_read_sbs_word(sm, CMD_OEMFLAG, &temp);
	ret = !!((temp & (CMD_MASK_OEM_CALI | CMD_MASK_OEM_OEM | CMD_MASK_OEM_SEAL)) == CMD_MASK_OEM_SEAL);
	pr_err("fg_exit_calibration_mode: ret = %d, oemFlag=0x%04X\n", ret, temp);
	return ret;
}

//20220625, Ethan
#define CMD_FORCE_UPDATE_E2ROM (CMDMASK_ALTMAC_W | 0xE012)
#define LEN_FORCE_UPDATE_E2ROM 4
#define CMD_E2ROM_BOARDOFFSET (CMDMASK_ALTMAC_W | 0x4044)
#define CMD_E2ROM_CURRENTRATIO (CMDMASK_ALTMAC_W | 0x4040)

static s32 fgcali_force_e2rom_update(struct sh_fg_chip* sm) //20220625, Ethan
{
	u8 buf_ForceE2romUpdate[LEN_FORCE_UPDATE_E2ROM] = { 0x45, 0x32, 0x55, 0x50 };
	s32 ret;
	s32 i;
	u16 test_comm;

	for (i = 0; i < 5; i++) { //e2rom may updating
		ret = fg_read_sbs_word(sm, 0x00, &test_comm);
		if (ret >= 0)
			break;
		msleep(250);
	}
	if (ret < 0)
		return ret;
	ret = fg_write_block(sm, CMD_FORCE_UPDATE_E2ROM, LEN_FORCE_UPDATE_E2ROM, buf_ForceE2romUpdate);
	msleep(1500);
	return ret;
}

static s32 fgcali_set_e2rom_offset(struct sh_fg_chip* sm, s32 offset_input) //20220625, Ethan
{
	s32 ret;
	u8 buf[2];

	buf[0] = (offset_input >> 8) & 0xFF;
	buf[1] = offset_input & 0xFF;

	ret = fg_write_block(sm, CMD_E2ROM_BOARDOFFSET, 2, buf);
	msleep(CMD_SBS_DELAY);
	return ret;
}

static __maybe_unused s32 fgcali_set_e2rom_ratio(struct sh_fg_chip* sm, s32 ratio_input) //20220625, Ethan
{
	s32 ret;
	u8 buf[2];

	buf[0] = (ratio_input >> 8) & 0xFF;
	buf[1] = ratio_input & 0xFF;

	ret = fg_write_block(sm, CMD_E2ROM_CURRENTRATIO, 2, buf);
	msleep(CMD_SBS_DELAY);
	return ret;
}


static s32 fg_gauge_calibrate_board(struct sh_fg_chip* sm) //20220625, Ethan
{
#define CALI_MAX_CURRENT (25)
#define CALI_MAX_CURRENT_DELTA (15)
#define DEFAULT_SENSOR_RATIO (1000)
#define CALI_MAX_CNT   5
#define CALI_DELAY_MS  250
#define CMD_CADCINFO (CMDMASK_ALTMAC_R | 0xCF)
#define CALIINFO_INDEX_CHOSEOFFSET 10
#define CALIINFO_INDEX_CURRENT 24
#define CALIINFO_INDEX_CADC 20
#define CALIINFO_INDEX_FLASHOFFSET 2

	s32 ret;
	u8 buf[32];
	s32 i;
	s32 cali_cur;
	s32 cali_cadc;
	s32 offset;
	s32 cur_max, cur_min,cadc_max,cadc_min;
	s32 te_offset;

	ret = fg_enter_calibration_mode(sm);
	if (ret < 0) {
		pr_err("fg_calibrate_boardoffset error! cannot enter cali mode!");
		goto fg_calibrate_boardoffset_end;
	}

	offset = 0;
	cur_max = 0x80000000;
	cur_min = 0x7FFFFFFF;
	cadc_max = 0x80000000;
	cadc_min = 0x7FFFFFFF;
	for (i = 0; i < CALI_MAX_CNT; i++) {
		msleep(CALI_DELAY_MS);
		ret = fg_read_block(sm, CMD_CALIINFO, 32, buf);
		if (ret < 0) {
			pr_err("fg_calibrate_boardoffset %d comm error!\n", i);
			goto fg_calibrate_boardoffset_end;
		}

		cali_cur = (s32)((s16)BUF2U16_LT(&buf[CALIINFO_INDEX_CURRENT]));
		cur_max = max(cur_max, cali_cur);
		cur_min = min(cur_min, cali_cur);
		
		cali_cadc = (s32)BUF2U32_LT(&buf[CALIINFO_INDEX_CADC]);
		offset += cali_cadc;
								   
		cadc_max = max(cadc_max, cali_cadc);
		cadc_min = min(cadc_min, cali_cadc);
		te_offset = 0;
        if (buf[27] & 0x40) //TE_Offset
            te_offset = (s32)((s16)BUF2U16_LT(&buf[0]));

		pr_err("fg_calibrate_boardoffset raw-data %d: cadc=%d, cur=%d\n", i, cali_cadc, cali_cur);
	}

	offset = offset - cadc_max - cadc_min;
	offset /= (CALI_MAX_CNT - 2);
	ret = 0;
	if ((abs(cur_max) >= CALI_MAX_CURRENT) || (abs(cur_min) >= CALI_MAX_CURRENT) || (abs(cur_max - cur_min) >= CALI_MAX_CURRENT_DELTA))
		ret = -1;

	if (ret < 0) {
		pr_err("fg_calibrate_boardoffset error! Raw-data exceed! max cur=%d, min cur=%d, delta cur=%d, avg_offset=%d, te_offset=%d\n", cur_max, cur_min, cur_max - cur_min, offset, te_offset);
		goto fg_calibrate_boardoffset_end;
	}
	else
		pr_err("fg_calibrate_boardoffset raw-data summary: max cur=%d, min cur=%d, delta cur=%d, avg_offset=%d, te_offset=%d\n", cur_max, cur_min, cur_max - cur_min, offset, te_offset);
    offset -= te_offset;
	ret = fgcali_set_e2rom_offset(sm, offset);
	if (ret < 0) {
		pr_err("fg_calibrate_boardoffset error! cannot write offset!\n");
		goto fg_calibrate_boardoffset_end;
	}

	ret = fgcali_force_e2rom_update(sm);
	if (ret < 0) {
		pr_err("fg_calibrate_boardoffset error! cannot update e2rom!");
		goto fg_calibrate_boardoffset_end;
	}

fg_calibrate_boardoffset_end:
	msleep(CALI_DELAY_MS);
	ret |= fg_read_block(sm, CMD_CALIINFO, 32, buf);
	offset = (s32)((s16)BUF2U16_LT(&buf[CALIINFO_INDEX_CHOSEOFFSET]));
	cali_cur = (s32)((s16)BUF2U16_LT(&buf[CALIINFO_INDEX_CURRENT]));
	cali_cadc = (s32)BUF2U32_LT(&buf[CALIINFO_INDEX_CADC]);
	cur_max = (s32)((s16)BUF2U16_LT(&buf[CALIINFO_INDEX_FLASHOFFSET]));

	ret |= fg_exit_calibration_mode(sm);
	
	pr_err("fg_calibrate_boardoffset end: ret=%d, read ic chose_offset=%d, flash_offset=%d, current=%d, cadc=%d\n", ret, offset, cur_max, cali_cur, cali_cadc);
	return ret;
}

static __maybe_unused s32 fg_gauge_calibrate_board_entry(struct power_supply* psy) /* 20220106, Ethan */
{
	struct sh_fg_chip* sm = power_supply_get_drvdata(psy);
	int i;
	int ret=0;

	if (sm->is_enable_autocali) {
		pr_err("fg_gauge_calibrate_board: autocali supported! no need to run this func!");
		return -1;
	}

	for (i = 0; i < 5; i++) {
		ret = -1;
		if (mutex_trylock(&sm->cali_lock)) {
			ret = fg_gauge_calibrate_board(sm);
			mutex_unlock(&sm->cali_lock);
		} else {
			pr_err("fg_gauge_calibrate_board: could not get mutex!\n");
		}

		if (ret >= 0)
			break;
		HOST_DELAY(200);
	}
	return ret;
}

static s32 fg_battery_parse_dt(struct sh_fg_chip* sm)
{
	struct device* dev = &sm->client->dev;
	struct device_node* np = dev->of_node;
	s32 battery_id = -1;

	BUG_ON(dev == 0);
	BUG_ON(np == 0);

	/* battery_params node*/
	np = of_find_node_by_name(of_node_get(np), "battery_params");
	if (np == NULL) {
		pr_info("Cannot find child node \"battery_params\"\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "battery,id", &battery_id) < 0)
		pr_err("not battery,id property\n");
	if (battery_id == -1)
		battery_id = get_battery_id(sm);
	pr_info("battery id = %d\n", battery_id);

	return 0;
}

bool hal_fg_init(struct i2c_client* client)
{
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	pr_info("sh366100 hal_fg_init...\n");
	mutex_lock(&sm->data_lock);
	if (client->dev.of_node) {
		/* Load common data from DTS*/
		fg_common_parse_dt(sm);
		/* Load battery data from DTS*/
		fg_battery_parse_dt(sm);
	}

	sm->log_lastUpdate = jiffies; /* 20211025, Ethan */

	if (!fg_init(client))
		return false;

	mutex_unlock(&sm->data_lock);
	pr_info("hal fg init OK\n");
	return true;
}

static s32 sh366100_get_psy(struct sh_fg_chip* sm)
{
#if !(IS_PACK_ONLY)
	if (!sm->usb_psy || !sm->batt_psy)
		return -EINVAL;

	sm->usb_psy = power_supply_get_by_name("usb");
	if (!sm->usb_psy) {
		pr_err("USB supply not found, defer probe\n");
		return -EINVAL;
	}

	sm->batt_psy = power_supply_get_by_name("battery");
	if (!sm->batt_psy) {
		pr_err("bms supply not found, defer probe\n");
		return -EINVAL;
	}
#endif
	return 0;
}

static s32 sh366100_notifier_call(struct notifier_block* nb, unsigned long ev, void* v)
{
#if !(IS_PACK_ONLY)
	struct power_supply* psy = v;
	struct sh_fg_chip* sm = container_of(nb, struct sh_fg_chip, nb);
	union power_supply_propval pval = {
	    0,
	};
	s32 rc;

	if (ev != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	rc = sh366100_get_psy(sm);
	if (rc < 0) {
		return NOTIFY_OK;
	}

	if (strcmp(psy->desc->name, "usb") != 0)
		return NOTIFY_OK;

	if (sm->usb_psy) {
		rc = power_supply_get_property(sm->usb_psy, POWER_SUPPLY_PROP_PRESENT, &pval);

		if (rc < 0) {
			pr_err("failed get usb present\n");
			return -EINVAL;
		}
		if (pval.intval) {
			sm->usb_present = true;
			pm_stay_awake(sm->dev);
		} else {
			sm->batt_sw_fc = false;
			sm->usb_present = false;
			pm_relax(sm->dev);
		}
	}
#endif
	return NOTIFY_OK;
}
static struct class *sh366100_class;

static ssize_t sh366100_read (struct file *file, char __user *buf, size_t size, loff_t *offset)
{
	u16 kernel_buf;
	int ret;
	pr_err("sh366100_read\n");
	ret = fg_get_user_word(sm,&kernel_buf);
	ret = copy_to_user(buf,&kernel_buf,2);
	return ret;

}

static ssize_t sh366100_write (struct file *file, const char __user *buf, size_t size, loff_t *offset)
{
	u16 kernel_buf;
	int ret;
	
	ret = copy_from_user(&kernel_buf,buf,2);
	pr_err("sh366100_write %d\n",kernel_buf);
	ret = fg_set_user_word(sm,kernel_buf);
	return ret;
}

static int sh366100_open (struct inode *node, struct file *file)
{
	printk("/dev/sh366100 open");
	return 0;
}

static struct file_operations sh366100_ops = {
	.owner = THIS_MODULE,
	.open  = sh366100_open,
	.read  = sh366100_read,
	.write = sh366100_write,
};


static __maybe_unused s32 fg_Read_Date_Block(struct sh_fg_chip* sm)
{
	int ret = 0;
	u8 r_buf[32];
	ret = fg_read_block(sm, CMD_USERBUFFER, 32, r_buf);
	if (ret < 0) {
		pr_err("fg_Read_Date_Block error! ret=%d", ret);
		return ret;
	}
	return ret;
}

// drv add tankaikun, battery info class, 20250605 start
static void get_battery_info_date(struct sh_fg_chip* sm)
{
	int len = 0;
	int ret = 0;
	u8 pbuf[LEN_USERBUFFER];
	struct sh_dm_info *dm_info = &g_battery_dm_info;

	fg_get_user_info(sm->dev, LEN_USERBUFFER, pbuf);

	len = LENTH_VENDER_NAME -1;
	ret = convertToACall(pbuf, OFFSET_VENDOR_NAME, len, dm_info->vendor_name);
	if (!ret) {
	    pr_err("Vendor num: %s\n", dm_info->vendor_name);
	} else {
	    pr_err("Error: Unable to extract and convert to vendor name\n");
	}

	len = LENTH_DEVICE_MODEL -1;
	ret = convertToACall(pbuf, OFFSET_DEVICE_MODEL, len, dm_info->device_model);
	if (!ret) {
	    pr_err("Device model: %s\n", dm_info->device_model);
	} else {
	    pr_err("Error: Unable to extract and convert to device model\n");
	}

	len = LENTH_MANU_DATA -1;
	ret = convertToACall(pbuf, OFFSET_MANU_DATA, len, dm_info->mfr_date);
	if (!ret) {
	    pr_err("Manufacturer date: %s\n", dm_info->mfr_date);
	} else {
	    pr_err("Error: Unable to extract and convert to manufacturer date\n");
	}

	len = LENTH_SERIAL_NUM -1;
	ret = convertToACall(pbuf, OFFSET_SERIAL_NUM, len, dm_info->serial_number);
	if (!ret) {
	    pr_err("Serial number: %s\n", dm_info->serial_number);
	} else {
	    pr_err("Error: Unable to extract and convert to serial number\n");
	}

	len = LENTH_ACTIVE_DATA -1;
	ret = convertToACall(pbuf, OFFSET_ACTIVE_DATA, len, dm_info->act_date);
	if (!ret) {
	    pr_err("Activation date: %s\n", dm_info->act_date);
	} else {
	    pr_err("Error: Unable to extract and convert to activation date\n");
	}

	convertDate(dm_info->mfr_date, dm_info->manufacturer_date);
	convertDate(dm_info->act_date, dm_info->activation_date);

	return;
}

static u16 fg_set_act_mfr_date(struct sh_fg_chip *sh, u32 date, enum battery_info_property prop)
{
	u8 i = 0;
	u8 pbuf[LEN_USERBUFFER];
	u8 temp = 0;
	u8 data_temp = 0;
	int len = 0;
	int ret = 0;
	struct sh_dm_info *dm_info = &g_battery_dm_info;

	ret = fg_get_user_info(sh->dev, LEN_USERBUFFER, pbuf);
	if (ret < 0) {
		pr_err("Error: cannot get user_info\n");
		return ret;
	}

	pr_err("fg_set_act_mfr_date:0x%x ", date);
	//Year
	data_temp = (u8)(date >> 16);
	for (i = 0; i < YMAP_SIZE; i++) {
		ret = kstrtou8(Ymappings[i].value, 16, &temp);
		pr_err("yearmap:%x,year:%x ", temp, data_temp);
		if (temp == data_temp) {
			if (prop == BAT_INFO_PROP_ACTIVATION_DATE) {
				pbuf[OFFSET_ACTIVE_DATA] = Ymappings[i].c;
			} else if (prop == BAT_INFO_PROP_MANUFACTURER_DATE) {
				pbuf[OFFSET_MANU_DATA] = Ymappings[i].c;
			}
			break;
		}
	}
	if (i >= YMAP_SIZE) {
		pr_err("cannot match year num \n");
		return -1;
	}

	//Month
	data_temp = (u8)(date >> 8);
	for (i = 0; i < MMAP_SIZE; i++) {
		ret = kstrtou8(Mmappings[i].value, 16, &temp);
		pr_err("monthmap:%x,month:%x ", temp, data_temp);
		if (temp == data_temp) {
			if (prop == BAT_INFO_PROP_ACTIVATION_DATE) {
				pbuf[OFFSET_ACTIVE_DATA+1] = Mmappings[i].c;
			} else if (prop == BAT_INFO_PROP_MANUFACTURER_DATE) {
				pbuf[OFFSET_MANU_DATA+1] = Mmappings[i].c;
			}
			break;
		}
	}
	if (i >= MMAP_SIZE) {
		pr_err("cannot match month num \n");
		return -1;
	}

	//Day
	data_temp = (u8)date;
	for (i = 0; i < DMAP_SIZE; i++) {
		ret = kstrtou8(Dmappings[i].value, 16, &temp);
		pr_err("daymap:%x,day:%x ", temp, data_temp);
		if (temp == data_temp) {
			if (prop == BAT_INFO_PROP_ACTIVATION_DATE) {
				pbuf[OFFSET_ACTIVE_DATA+2] = Dmappings[i].c;
			} else if (prop == BAT_INFO_PROP_MANUFACTURER_DATE) {
				pbuf[OFFSET_MANU_DATA+2] = Dmappings[i].c;
			}
			break;
		}
	}
	if (i >= DMAP_SIZE) {
		pr_err("cannot match day num \n");
		return -1;
	}

	ret = fg_set_user_info(sh->dev, LEN_USERBUFFER, pbuf);
	if (ret < 0) {
		pr_err("Error: cannot set user_info\n");
		return ret;
	}
	msleep(10);
	ret = fg_get_user_info(sh->dev, LEN_USERBUFFER, pbuf);
	if (ret < 0) {
		pr_err("Error: cannot get user_info\n");
		return ret;
	}

	if (prop == BAT_INFO_PROP_ACTIVATION_DATE) {
		len = LENTH_ACTIVE_DATA -1;
		ret = convertToACall(pbuf, OFFSET_ACTIVE_DATA, len, dm_info->act_date);
		if (!ret) {
			pr_err("Activation date: %s\n", dm_info->act_date);
		} else {
			pr_err("Error: Unable to extract and convert to activation date\n");
		}
		convertDate(dm_info->act_date, dm_info->activation_date);
	} else if (prop == BAT_INFO_PROP_MANUFACTURER_DATE) {
		len = LENTH_MANU_DATA -1;
		ret = convertToACall(pbuf, OFFSET_MANU_DATA, len, dm_info->mfr_date);
		if (!ret) {
			pr_err("Activation date: %s\n", dm_info->mfr_date);
		} else {
			pr_err("Error: Unable to extract and convert to manufacturer date\n");
		}
		convertDate(dm_info->mfr_date, dm_info->manufacturer_date);
	}
	//fg_gauge_seal(sh);

	pr_err("[%s],ok!\n",__func__);

	return 0;
}

/*
static ssize_t device_name_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	return sprintf(buf, "%s\n", g_battery_dm_info.device_model);
}

static ssize_t manufacturer_date_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	return sprintf(buf, "%s\n", g_battery_dm_info.manufacturer_date);
}

static ssize_t serial_number_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	return sprintf(buf, "%s\n", g_battery_dm_info.serial_number);
}

static ssize_t battery_capacity_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	struct sh_fg_chip *sm = NULL;
	struct power_supply *bat_psy = NULL;

	bat_psy = power_supply_get_by_name("ext-bat");
	if(bat_psy == NULL){
		pr_err("[battery_info] get chrg_psy err\n");
		goto failed;
	}

	sm = (struct sh_fg_chip *)power_supply_get_drvdata(bat_psy);
	if(!sm){
		pr_err("[battery_info] get sm err\n");
		goto failed;
	}
	return sprintf(buf, "%d\n", sm->batt_soc);
failed:
	return sprintf(buf, "%d\n", 100);
}

static ssize_t max_voltage_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	return sprintf(buf, "%d\n", 4450);
}

static ssize_t cycle_count_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	struct sh_fg_chip *sm = NULL;
	struct power_supply *bat_psy = NULL;

	bat_psy = power_supply_get_by_name("ext-bat");
	if(bat_psy == NULL){
		pr_err("[battery_info] get bat_psy err\n");
		goto failed;
	}

	sm = (struct sh_fg_chip *)power_supply_get_drvdata(bat_psy);
	if(!sm){
		pr_err("[battery_info] get sm err\n");
		goto failed;
	}
	return sprintf(buf, "%d\n", sm->batt_soc_cycle);

failed:
	return sprintf(buf, "%d\n", 1);
}

static ssize_t soh_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	struct sh_fg_chip *sm = NULL;
	struct power_supply *bat_psy = NULL;

	bat_psy = power_supply_get_by_name("ext-bat");
	if(bat_psy == NULL){
		pr_err("[battery_info] get bat_psy err\n");
		goto failed;
	}

	sm = (struct sh_fg_chip *)power_supply_get_drvdata(bat_psy);
	if(!sm){
		pr_err("[battery_info] get sm err\n");
		goto failed;
	}
	return sprintf(buf, "%d\n", sm->soh);

failed:
	return sprintf(buf, "%d\n", 100);
}

static ssize_t activation_date_show(struct class *class, struct class_attribute *attr,	char *buf)
{
	return sprintf(buf, "%s\n", g_battery_dm_info.activation_date);
}

static ssize_t activation_date_store(struct class *class, struct class_attribute *attr,
						const char *buf, size_t count)
{
	int i = 0, ret = 0;
	u32 tmp = 0;
	struct sh_fg_chip *sh = NULL;
	struct power_supply *chrg_psy = NULL;

	chrg_psy = power_supply_get_by_name("ext-bat");
	if(chrg_psy == NULL) {
		pr_err("get bat_psy err\n");
		return count;
	}
	sh = (struct sh_fg_chip *)power_supply_get_drvdata(chrg_psy);
	if(!sh) {
		pr_err("get sh_fg_chip err\n");
		return count;
	}

	if (buf != NULL && count != 0) {
		pr_err("[activation][activation_date_store] buf is %s and size is %zu\n", buf, count);
		for(i = 0; i < count;i++){
			pr_err("[activation][activation_date_store] buf[%d]=0x%x \n", i,buf[i]);
		}
		ret = kstrtouint(buf, 16, &tmp);
		fg_set_activation_date(sh,tmp);
		pr_err("[activation][activation_date_store] ret=%d date=0x%x \n",ret, tmp);
	}
	return count;
}

static struct class * battery_info_class;
static struct class_attribute battery_info_class_attrs[] = {
	__ATTR(device_name, S_IRUGO, device_name_show, NULL),
	__ATTR(manufacturer_date, S_IRUGO, manufacturer_date_show, NULL),
	__ATTR(serial_number, S_IRUGO, serial_number_show, NULL),
	__ATTR(battery_capacity, S_IRUGO,battery_capacity_show, NULL),
	__ATTR(max_voltage, S_IRUGO, max_voltage_show, NULL),
	__ATTR(cycle_count, S_IRUGO, cycle_count_show, NULL),
	__ATTR(soh, S_IRUGO, soh_show, NULL),
	__ATTR(activation_date, S_IRUGO | S_IWUSR, activation_date_show, activation_date_store),
	__ATTR_NULL,
};

static int battery_info_sysfs_create(void)
{
	int i = 0,ret = 0;
	battery_info_class = class_create(THIS_MODULE, "battery_info");
	if (IS_ERR(battery_info_class))
		return PTR_ERR(battery_info_class);
	for (i = 0; battery_info_class_attrs[i].attr.name; i++) {
		ret = class_create_file(battery_info_class,&battery_info_class_attrs[i]);
		if (ret < 0)
		{
			pr_err("battery_info sysfs create error !!\n");
			return ret;
		}
	}
	return ret;
}
*/
// drv add tankaikun, battery info class, 20250605 end

static int sh_fg_probe(struct i2c_client* client, const struct i2c_device_id *id)
{
	int ret;
	int version_ret;
	int retry;
	//struct sh_fg_chip* sm;
	int* regs;
	/* prize add by fangduozhu 20250717, show fuelgauge firmware version start */
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
	u16 afi_version = 0;
#endif
	/* prize add by fangduozhu 20250717, show fuelgauge firmware version end */
	pr_err("2021.09.10 wsy %s: start\n", __func__);

	pr_info("enter\n");
	major = register_chrdev(0, "sh366100", &sh366100_ops);
	sh366100_class = class_create(THIS_MODULE, "sh366100_class");
	device_create(sh366100_class, NULL, MKDEV(major, 0), NULL, "sh366100"); /* /dev/sh366100 */											   
	sm = devm_kzalloc(&client->dev, sizeof(*sm), GFP_KERNEL);

	if (!sm)
		return -ENOMEM;

	sm->dev = &client->dev;
	sm->client = client;
	//sm->chip = id->driver_data;

	sm->batt_soc = -ENODATA;
	sm->batt_fcc = -ENODATA;
	sm->batt_volt = -ENODATA;
	sm->batt_temp = -ENODATA;
	sm->batt_curr = -ENODATA;
	/* 20211029, Ethan. */
	/* sm->fake_soc = -EINVAL; */
	/* sm->fake_temp = -EINVAL; */

	if (sm->chip == SH366100) {
		regs = sh366100_regs;
	} else {
		pr_err("unexpected fuel gauge: %d\n", sm->chip);
		regs = sh366100_regs;
	}

	memcpy(sm->regs, regs, NUM_REGS * sizeof(u32));
	
	i2c_set_clientdata(client, sm);

	mutex_init(&sm->i2c_rw_lock);
	mutex_init(&sm->data_lock);
	mutex_init(&sm->cali_lock); /* 20220105, Ethan */

	// fg_read_gaugeinfo_block(sm); /* 20211115. LJQ Debug *101读取gauge信息，可移除/
	pr_err("Probe:start!!!");
	/* 20211013, Ethan. Firmware Update */
	version_ret = Check_Chip_Version(sm);
	// version_ret = 2;
	// version_ret = 0;
	if (version_ret == CHECK_VERSION_ERR) {
		pr_err("Probe: Check version error!");
	} else if (version_ret == CHECK_VERSION_OK) {
		pr_err("Probe: Check version ok!");
	} else {
		pr_err("Probe: Check version update: %X", version_ret);

		ret = ERRORTYPE_NONE; //20220218, Ethan
		if (version_ret & CHECK_VERSION_FW) {
			pr_err("Probe: Firmware Update start");
			for (retry = 0; retry < FILE_DECODE_RETRY; retry++) {
				ret = file_decode_process(sm, "sinofs_image_data");
				if (ret == ERRORTYPE_NONE)
					break;
				HOST_DELAY(FILE_DECODE_DELAY); /* 20211029, Ethan */
			}
			pr_err("Probe: Firmware Update end, ret=%d", ret);
		}
		if (ret != ERRORTYPE_NONE) //20220218, Ethan
			goto sh_fg_probe_fwupdate_end;

		// if (version_ret & CHECK_VERSION_TS) {
		// 	pr_err("Probe: TS Update start");
		// 	for (retry = 0; retry < FILE_DECODE_RETRY; retry++) {
		// 		ret = file_decode_process(sm, "sinofs_ts_data");
		// 		if (ret == ERRORTYPE_NONE)
		// 			break;
		// 		HOST_DELAY(FILE_DECODE_DELAY); /* 20211029, Ethan */
		// 	}
		// 	pr_err("Probe: TS Update end, ret=%d", ret);
		// }
		// if (ret != ERRORTYPE_NONE) //20220218, Ethan
		// 	goto sh_fg_probe_fwupdate_end;

		if (version_ret & CHECK_VERSION_AFI) {
			pr_err("Probe: AFI Update start");
			for (retry = 0; retry < FILE_DECODE_RETRY; retry++) {
				ret = file_decode_process(sm, "sinofs_afi_data");
				if (ret == ERRORTYPE_NONE)
					break;
				HOST_DELAY(FILE_DECODE_DELAY); /* 20211029, Ethan */
			}
			pr_err("Probe: AFI Update end, ret=%d", ret);
		}
		if (ret != ERRORTYPE_NONE) //20220218, Ethan
			goto sh_fg_probe_fwupdate_end;
	}
sh_fg_probe_fwupdate_end: //20220218, Ethan

	if (fg_gauge_runstate_check(sm) < 0) { /* 20211122, Ethan. Gauge Enable */
		pr_err("Failed to Enable Gauge\n");
		goto err_0;
	}

	if (!hal_fg_init(client)) {
		pr_err("Failed to Initialize Fuelgauge\n");
		goto err_0;
	}

	// drv add tankaikun, update fg state, 20250530 start
	sm->shfg_workqueue = create_singlethread_workqueue("shfg_gauge");
	INIT_DELAYED_WORK(&sm->monitor_work, fg_monitor_workfunc);
	queue_delayed_work(sm->shfg_workqueue, &sm->monitor_work , msecs_to_jiffies(queue_start_work_time));
	// drv add tankaikun, update fg state, 20250530 end

	fg_psy_register(sm);

#if FG_REMOVE_IRQ == 0
	if (sm->gpio_int != -EINVAL)
		pr_err("unuse\n");
	else {
		pr_err("Failed to registe gpio interrupt\n");
		goto err_0;
	}

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq, NULL,
						fg_irq_thread,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						"sh fuel gauge irq", sm);
		if (ret < 0) {
			pr_err("request irq for irq=%d failed, ret = %d\n", client->irq, ret);
		}
	}
#endif

	sm->nb.notifier_call = &sh366100_notifier_call;
	ret = power_supply_reg_notifier(&sm->nb);
	if (ret < 0) {
		pr_err("Couldn't register psy notifier rc = %d\n", ret);
		return ret;
	}

	ret = sysfs_create_group(&sm->dev->kobj, &fg_attr_group);
	if (ret)
		pr_err("Failed to register sysfs:%d\n", ret);

	// drv add tankaikun, battery info class, 20250605 start
//	battery_info_sysfs_create();
	get_battery_info_date(sm);
	// drv add tankaikun, battery info class, 20250605 end

	/* prize add by fangduozhu 20250717, show fuelgauge firmware version start */
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
	fg_read_sbs_word(sm, CMD_AFI_STATIC_SUM, &afi_version);
	snprintf (current_battery_info.chip, 32, "sh366100");
	snprintf (current_battery_info.vendor, 32, "Sinowealth");
	snprintf (current_battery_info.more, 64, "afi:0x%x", afi_version);
#endif
	/* prize add by fangduozhu 20250717, show fuelgauge firmware version end */
	// schedule_delayed_work(&sm->monitor_work, 10 * HZ);
	pr_info("sh fuel gauge probe successfully, %s\n", device2str[sm->chip]);
	pr_err("2021.09.10 wsy %s: end\n", __func__);

	return 0;

err_0:
//drv add wanwen,clear pm operations, 20250627 start
#ifdef CONFIG_PM
	if (client->dev.driver) {
		client->dev.driver->pm = NULL;
	}
#endif
//drv add wanwen,clear pm operations, 20250627 end
	return ret;
}

static void sh_fg_remove(struct i2c_client* client)
{
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&sm->monitor_work);

	fg_psy_unregister(sm);

	mutex_destroy(&sm->cali_lock); /* 20220105, Ethan */
	mutex_destroy(&sm->data_lock);
	mutex_destroy(&sm->i2c_rw_lock);

	debugfs_remove_recursive(sm->debug_root);

	sysfs_remove_group(&sm->dev->kobj, &fg_attr_group);
	printk("%s %s %d\n", __FILE__, __FUNCTION__, __LINE__);
	device_destroy(sh366100_class, MKDEV(major, 0));
	class_destroy(sh366100_class);
	
	/* unregister_chrdev */
	unregister_chrdev(major, "sh366100");					  
	return;
}

static void sh_fg_shutdown(struct i2c_client* client)
{
	pr_info("sm fuel gauge driver shutdown!\n");
}

// drv add tankaikun, update fg state, 20250530 start
#ifdef CONFIG_PM
static int sh_bat_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	cancel_delayed_work(&sm->monitor_work);
	return 0;
}

static int sh_bat_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sh_fg_chip* sm = i2c_get_clientdata(client);

	queue_delayed_work(sm->shfg_workqueue, &sm->monitor_work, msecs_to_jiffies(20));
	return 0;
}

static const struct dev_pm_ops sh_bat_pm_ops = {
	.suspend  = sh_bat_suspend,
	.resume   = sh_bat_resume,
};
#endif
// drv add tankaikun, update fg state, 20250530 end

static const struct of_device_id sh_fg_match_table[] = {
    {
	.compatible = "sh,sh366100",
    },
    {},
};
MODULE_DEVICE_TABLE(of, sh_fg_match_table);

static const struct i2c_device_id sh_fg_id[] = {
    {"sh366100", SH366100},
    {},
};
MODULE_DEVICE_TABLE(i2c, sh_fg_id);

static struct i2c_driver sh_fg_driver = {
    .driver = {
	.name = "sh366100",
	.owner = THIS_MODULE,
	.of_match_table = sh_fg_match_table,
// drv add tankaikun, update fg state, 20250530 start
#ifdef CONFIG_PM
	.pm = &sh_bat_pm_ops,
#endif
// drv add tankaikun, update fg state, 20250530 end
    },
    .id_table = sh_fg_id,
    .probe = sh_fg_probe,
    .remove = sh_fg_remove,
    .shutdown = sh_fg_shutdown,
};

//module_i2c_driver(sh_fg_driver);
//module_i2c_driver(sh_fg_driver);
static int __init sh_fg_driver_init(void)
{
    return i2c_add_driver(&sh_fg_driver);
}

static void __exit sh_fg_driver_exit(void)
{
    i2c_del_driver(&sh_fg_driver);
} 
module_init(sh_fg_driver_init);
module_exit(sh_fg_driver_exit);
MODULE_DESCRIPTION("SH SH366100 Gauge Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sinowealth");
