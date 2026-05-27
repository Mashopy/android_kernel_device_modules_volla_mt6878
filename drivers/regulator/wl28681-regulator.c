// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Willsemi Co. Ltd.
 *
 * Author: Ray Deng <ray.deng@corp.ovt.com>
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define WL28681_DEVICE_D1		0x00
#define WL28681_DEVICE_D2		0x01
#define WL28681_ILIM			0x02
#define WL28681_LDO_EN			0x03

#define WL28681_LDO1_VOUT		0x04
#define WL28681_LDO2_VOUT		0x05
#define WL28681_LDO3_VOUT		0x06
#define WL28681_LDO4_VOUT		0x07
#define WL28681_LDO5_VOUT		0x08
#define WL28681_LDO6_VOUT		0x09
#define WL28681_LDO7_VOUT		0x0a

#define WL28681_LDO1_LDO2_SEQ		0x0b
#define WL28681_LDO3_LDO4_SEQ		0x0c
#define WL28681_LDO5_LDO6_SEQ		0x0d
#define WL28681_LDO7_SEQ			0x0e
#define WL28681_SEQ_STATUS			0x0f

#define WL28681_DISCHARGE_RESISTORS		0x10
#define WL28681_RESET					0x11
#define WL28681_REPROGRAMMABLE_I2C_ADDR	0x12

#define WL28681_LDO1234_COMP	0x13
#define WL28681_LDO567_COMP		0x14

#define WL28681_UVP_INT			0x15
#define WL28681_OCP_INT			0x16
#define WL28681_TSD_UVLO_INT	0x17
#define WL28681_UVP_INT_STATUS			0x18
#define WL28681_OCP_INT_STATUS			0x19
#define WL28681_TSD_UVLO_INT_STATUS		0x1a
#define WL28681_SUSD_STATUS				0x1b
#define WL28681_UVP_INT_MASK			0x1c
#define WL28681_OCP_INT_MASK			0x1d
#define WL28681_TSD_UVLO_INT_MASK		0x1e

#define RESERVED_2                      0x1e

#define WL28681_VSEL_MASK         0xff
#define WL28681_ENABLE            1
#define WL28681_DISABLE           0
static int __maybe_unused wl28681_suspend(struct device *dev);
static int __maybe_unused wl28681_resume(struct device *dev);
int wl28681_flag;
enum wl28681_regulators {
	WL28681_REGULATOR_LDO1 = 0,
	WL28681_REGULATOR_LDO2,
	WL28681_REGULATOR_LDO3,
	WL28681_REGULATOR_LDO4,
	WL28681_REGULATOR_LDO5,
	WL28681_REGULATOR_LDO6,
	WL28681_REGULATOR_LDO7,
	WL28681_MAX_REGULATORS,
};

enum LDO_NUM {
       WL28681_LDO1 = 0,
       WL28681_LDO2,
       WL28681_LDO3,
       WL28681_LDO4,
       WL28681_LDO5,
       WL28681_LDO6,
       WL28681_LDO7,
};
static const unsigned int wl28681_curr_table[] = {0, 1};
/* ldo1~7  0-current,1-current*/
/*			740000, 950000
			50000, 700000
			740000, 950000
			500000, 700000
			500000, 700000
			1460000, 2000000
*/

struct wl28681 {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_dev *rdev;
	struct gpio_desc *reset_gpio;
       struct gpio_desc *dcdc_gpio;
	int min_dropout_uv;
	int ldo_vout[7];
	int ldo_en;
};

struct wl28681 *wl28681_ldo;

static const struct regulator_ops wl28681_reg_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.get_voltage_sel	= regulator_get_voltage_sel_regmap,
	.set_voltage_sel	= regulator_set_voltage_sel_regmap,
	.enable			= regulator_enable_regmap,
	.disable		= regulator_disable_regmap,
	.is_enabled		= regulator_is_enabled_regmap,
};

#define WL28681_DESC(_id, _match, _supply, _min, _max, _step, _vreg,	\
	_vmask, _ereg, _emask, _enval, _disval, _curr_table, _creg, _cmask)		\
	{\
		.id		= (_id),\
		.name		= (_match),\
		.of_match	= of_match_ptr(_match),\
		.supply_name	= (_supply),\
		.min_uV		= (_min) * 1000,\
		.uV_step	= (_step) * 1000,\
		.n_voltages	= (((_max) - (_min)) / (_step) + 1),\
		.regulators_node = of_match_ptr("regulators"),\
		.type		= REGULATOR_VOLTAGE,\
		.vsel_reg	= (_vreg),\
		.vsel_mask	= (_vmask),\
		.csel_reg	= (_creg),\
		.csel_mask	= (_cmask),\
		.enable_reg	= (_ereg),\
		.enable_mask	= (_emask),\
		.enable_val     = (_enval),\
		.disable_val     = (_disval),\
		.ops		= &wl28681_reg_ops,\
		.curr_table = _curr_table,\
		.owner		= THIS_MODULE,\
	}

static const struct regulator_desc wl28681_reg[] = {
       WL28681_DESC(WL28681_REGULATOR_LDO1, "wl28681-ldo1", "LDO1", 496, 2536, 8,
		     WL28681_LDO1_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(0),
			 BIT(0), 0,	wl28681_curr_table, WL28681_ILIM, BIT(0)),
       WL28681_DESC(WL28681_REGULATOR_LDO2, "wl28681-ldo2", "LDO2", 496, 2536, 8,
		     WL28681_LDO2_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(1),
			 BIT(1), 0, wl28681_curr_table, WL28681_ILIM, BIT(1)),
       WL28681_DESC(WL28681_REGULATOR_LDO3, "wl28681-ldo3", "LDO3", 1504, 3544, 8,
		     WL28681_LDO3_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(2),
			 BIT(2), 0, wl28681_curr_table, WL28681_ILIM, BIT(2)),
       WL28681_DESC(WL28681_REGULATOR_LDO4, "wl28681-ldo4", "LDO4", 1504, 3544, 8,
		     WL28681_LDO4_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(3),
			 BIT(3), 0, wl28681_curr_table, WL28681_ILIM, BIT(3)),
       WL28681_DESC(WL28681_REGULATOR_LDO5, "wl28681-ldo5", "LDO5", 1504, 3544, 8,
		     WL28681_LDO5_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(4),
			 BIT(4), 0, wl28681_curr_table, WL28681_ILIM, BIT(4)),
       WL28681_DESC(WL28681_REGULATOR_LDO6, "wl28681-ldo6", "LDO6", 1504, 3544, 8,
		     WL28681_LDO6_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(5),
			 BIT(5), 0, wl28681_curr_table, WL28681_ILIM, BIT(5)),
       WL28681_DESC(WL28681_REGULATOR_LDO7, "wl28681-ldo7", "LDO7", 496, 2536, 8,
		     WL28681_LDO7_VOUT, WL28681_VSEL_MASK, WL28681_LDO_EN, BIT(6),
			 BIT(6), 0, wl28681_curr_table, WL28681_ILIM, BIT(6)),
};



static const struct regmap_range wl28681_writeable_ranges[] = {
	regmap_reg_range(WL28681_DISCHARGE_RESISTORS, WL28681_SEQ_STATUS),
	regmap_reg_range(WL28681_ILIM, WL28681_LDO7_SEQ),
	regmap_reg_range(WL28681_DISCHARGE_RESISTORS, WL28681_TSD_UVLO_INT),
	regmap_reg_range(WL28681_UVP_INT_MASK, WL28681_TSD_UVLO_INT_MASK),
};

static const struct regmap_range wl28681_readable_ranges[] = {
	regmap_reg_range(WL28681_DEVICE_D1, WL28681_DEVICE_D2),
};

static const struct regmap_range wl28681_volatile_ranges[] = {
	regmap_reg_range(WL28681_DISCHARGE_RESISTORS, WL28681_SEQ_STATUS),
	regmap_reg_range(WL28681_ILIM, WL28681_LDO7_SEQ),
	regmap_reg_range(WL28681_DISCHARGE_RESISTORS, WL28681_TSD_UVLO_INT),
	regmap_reg_range(WL28681_UVP_INT_MASK, WL28681_TSD_UVLO_INT_MASK),
};

static const struct regmap_access_table wl28681_writeable_table = {
	.yes_ranges   = wl28681_writeable_ranges,
	.n_yes_ranges = ARRAY_SIZE(wl28681_writeable_ranges),
};

static const struct regmap_access_table wl28681_readable_table = {
	.yes_ranges   = wl28681_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(wl28681_readable_ranges),
};

static const struct regmap_access_table wl28681_volatile_table = {
	.yes_ranges   = wl28681_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(wl28681_volatile_ranges),
};

static const struct regmap_config wl28681_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = RESERVED_2,
	.wr_table = &wl28681_writeable_table,
	.rd_table = &wl28681_readable_table,
	.cache_type = REGCACHE_RBTREE,
	.volatile_table = &wl28681_volatile_table,
};

/*static void wl28681_dcdc(struct wl28681 *wl28681)
{
       gpiod_set_value_cansleep(wl28681->reset_gpio, 0);
	   usleep_range(100000, 110000);
       gpiod_set_value_cansleep(wl28681->dcdc_gpio, 0);
}

static void wl28681_reset(struct wl28681 *wl28681)
{
       gpiod_set_value_cansleep(wl28681->reset_gpio, 1);
       usleep_range(100000, 110000);
       gpiod_set_value_cansleep(wl28681->dcdc_gpio, 1);
}*/

static void wl28681_reset(struct wl28681 *wl28681)
{
	gpiod_set_value_cansleep(wl28681->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(wl28681->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(wl28681->reset_gpio, 1);
	usleep_range(10000, 11000);
}

static int wl28681_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct regulator_config config = {};
	struct regulator_dev *rdev;
	const struct regulator_desc *regulators;
	struct wl28681 *wl28681;
	int ret, i;
	int read_ldo_value;
	printk("WL28681 %s start" , __FUNCTION__);
	wl28681 = devm_kzalloc(dev, sizeof(struct wl28681), GFP_KERNEL);

	if (!wl28681)
		return -ENOMEM;

       /*wl28681->dcdc_gpio = devm_gpiod_get(dev, "dcdc", GPIOD_OUT_HIGH);
       if (IS_ERR(wl28681->dcdc_gpio)) {
              ret = PTR_ERR(wl28681->dcdc_gpio);
              dev_err(dev, "failed to request dcdc GPIO: %d\n", ret);
              return ret;
       }*/

       wl28681->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
       if (IS_ERR(wl28681->reset_gpio)) {
              ret = PTR_ERR(wl28681->reset_gpio);
              printk("failed to request reset GPIO: %d\n", ret);
              //return ret;
       }

       wl28681_reset(wl28681);

       i2c_set_clientdata(client, wl28681);
       wl28681->dev = dev;
       wl28681->regmap = devm_regmap_init_i2c(client, &wl28681_regmap_config);
       if (IS_ERR(wl28681->regmap)) {
              ret = PTR_ERR(wl28681->regmap);
              dev_err(dev, "Failed to allocate register map: %d\n", ret);
              return ret;
       }

       config.dev = &client->dev;
       config.regmap = wl28681->regmap;
       regulators = wl28681_reg;

       /* Instantiate the regulators */
       for (i = 0; i < WL28681_MAX_REGULATORS; i++) {
              rdev = devm_regulator_register(&client->dev,
                                          &regulators[i], &config);
              if (IS_ERR(rdev)) {
                     dev_err(&client->dev,
                            "failed to register %d regulator\n", i);
                     //return PTR_ERR(rdev);
              }
       }

       wl28681_ldo = wl28681;

       regmap_read(wl28681->regmap, 0x00,&read_ldo_value);
       dev_err(&client->dev,"wl28681 Product :0x%x",read_ldo_value);
       regmap_read(wl28681->regmap, 0x01,&read_ldo_value);
       dev_err(&client->dev,"wl28681 Revision :0x%x",read_ldo_value);

       regmap_read(wl28681->regmap, WL28681_LDO1_VOUT,&read_ldo_value);
       dev_err(&client->dev,"wl28681 WL28681_LDO1_VOUT :0x%x",read_ldo_value);
       regmap_read(wl28681->regmap, WL28681_LDO3_VOUT,&read_ldo_value);
       dev_err(&client->dev,"wl28681 WL28681_LDO3_VOUT :0x%x",read_ldo_value);
       regmap_read(wl28681->regmap, WL28681_LDO4_VOUT,&read_ldo_value);
       dev_err(&client->dev,"wl28681 read WL28681_LDO4_VOUT :0x%x",read_ldo_value);
       regmap_read(wl28681->regmap, WL28681_LDO6_VOUT,&read_ldo_value);
       dev_err(&client->dev,"wl28681 read WL28681_LDO6_VOUT :0x%x",read_ldo_value);
       regmap_read(wl28681->regmap, WL28681_LDO_EN,&read_ldo_value);
       dev_err(&client->dev,"wl28681 read LDO_EN :0x%x",read_ldo_value);

       printk("WL28681 %s end",__func__);
       return 0;
}

static void wl28681_regulator_shutdown(struct i2c_client *client)
{
       struct wl28681 *wl28681 = i2c_get_clientdata(client);
	   return;
	   dev_err(&client->dev,"wl28681 wl28681_regulator_shutdown system_state:%d SYSTEM_POWER_OFF:%d",system_state,SYSTEM_POWER_OFF);
       if (system_state == SYSTEM_POWER_OFF) {
			  dev_err(&client->dev,"wl28681 wl28681_regulator_shutdown");
              regmap_write(wl28681->regmap, WL28681_LDO_EN, 0x80);
	   }
}

static void wl28681_set_reset(struct wl28681 *wl28681,bool enable)
{
	if (enable)
	    gpiod_set_value_cansleep(wl28681->reset_gpio, 1);
	else
	    gpiod_set_value_cansleep(wl28681->reset_gpio, 0);

	usleep_range(10000, 11000);
}

int  wl28681_set_ldo(int sensoridx,int enable)
{
	printk("WL28681 %s sensoridx:%d enable:%d",__func__,sensoridx,enable);
	if (enable == 1)
	   wl28681_set_reset(wl28681_ldo, 1);// reset pin high

    switch (sensoridx)
    {
		case 0:
			regmap_write(wl28681_ldo->regmap, WL28681_LDO6_VOUT, 0x36);//IOVDD 1.8V
			regmap_write(wl28681_ldo->regmap, WL28681_LDO7_VOUT, 0xB3);//AFVDD 2.8V
			break;
		case 1:
			regmap_write(wl28681_ldo->regmap, WL28681_LDO6_VOUT, 0x36);//IOVDD 1.8V
			regmap_write(wl28681_ldo->regmap, WL28681_LDO1_VOUT, 0x95);//DVDD 1.2V
			regmap_write(wl28681_ldo->regmap, WL28681_LDO3_VOUT, 0xB3);//AVDD 2.8V
			break;
		case 2:
			regmap_write(wl28681_ldo->regmap, WL28681_LDO6_VOUT, 0x36);//IOVDD 1.8V
			regmap_write(wl28681_ldo->regmap, WL28681_LDO2_VOUT, 0x95);//DVDD 1.2V
			regmap_write(wl28681_ldo->regmap, WL28681_LDO4_VOUT, 0xB3);//AFVDD 2.8V
			break;
		case 3:
			regmap_write(wl28681_ldo->regmap, WL28681_LDO6_VOUT, 0x36);//IOVDD 1.8V
			regmap_write(wl28681_ldo->regmap, WL28681_LDO5_VOUT, 0xB3);//AFVDD 2.8V
			break;
		default:
			regmap_write(wl28681_ldo->regmap, WL28681_LDO6_VOUT, 0x36);
        break;
	}

	if (enable) {
		regmap_write(wl28681_ldo->regmap, WL28681_LDO_EN, 0xFF);
	} else {
		regmap_write(wl28681_ldo->regmap, WL28681_LDO_EN, 0x80);
	}

	if (enable == 0)
		wl28681_set_reset(wl28681_ldo, 0);// reset pin low

    return 0;
}
EXPORT_SYMBOL_GPL(wl28681_set_ldo);

// prize add by chenwenhui for otp start
#define GC08A3_OTP_FOR_MTK       1

#if GC08A3_OTP_FOR_MTK
struct otp_awb_info_struct {
	unsigned char  awb_flag;
	unsigned char  unit_r_h;
	unsigned char  unit_r_l;
	unsigned char  unit_gr_h;
	unsigned char  unit_gr_l;
	unsigned char  unit_gb_h;
	unsigned char  unit_gb_l;
	unsigned char  unit_b_h;
	unsigned char  unit_b_l;
	unsigned char  golden_r_h;
	unsigned char  golden_r_l;
	unsigned char  golden_gr_h;
	unsigned char  golden_gr_l;
	unsigned char  golden_gb_h;
	unsigned char  golden_gb_l;
	unsigned char  golden_b_h;
	unsigned char  golden_b_l;
	unsigned char  checksum_of_awb;
};

struct imgsensor_otp_info_struct {
	unsigned char  info_flag;
	unsigned char  supply_id;
	unsigned char  module_id;
	unsigned char  lends_id;
	unsigned char  vcm_ld;
	unsigned char  driver_id;
	unsigned char  module_version;
	unsigned char  software_version;
	unsigned char  year;
	unsigned char  month;
	unsigned char  day;
	unsigned char  reserved0;
	unsigned char  reserved1;
	unsigned char  checksum_of_info;
	struct otp_awb_info_struct awb;
	unsigned char  lsc_flag;
	unsigned char  lsc[1868];
	unsigned char  checksum_of_lsc;
};

struct imgsensor_otp_info_struct gc08a3_otp_info = {0};
EXPORT_SYMBOL_GPL(gc08a3_otp_info);
#endif
// prize add by chenwenhui for otp end


#if 0
static int __maybe_unused wl28681_suspend(struct device *dev)
{
	// gpiod_set_value_cansleep(wl28681_ldo->reset_gpio, 0);
	// usleep_range(10000, 11000);
	printk("WL28681 %s\n",__func__);

	return 0;
}

static int __maybe_unused wl28681_resume(struct device *dev)
{
	// gpiod_set_value_cansleep(wl28681_ldo->reset_gpio, 1);
	// usleep_range(10000, 11000);
	printk("WL28681 %s\n",__func__);

	return 0;
}
static SIMPLE_DEV_PM_OPS(wl28681_pm_ops, wl28681_suspend, wl28681_resume);
#endif

static const struct i2c_device_id wl28681_i2c_id[] = {
       { "wl28681", 0 },
       { }
};

MODULE_DEVICE_TABLE(i2c, wl28681_i2c_id);

static const struct of_device_id wl28681_of_match[] = {
       { .compatible = "willsemi,wl28681" },
       {}
};

MODULE_DEVICE_TABLE(of, wl28681_of_match);

static struct i2c_driver wl28681_i2c_driver = {
       .driver = {
       .name = "wl28681",
       .of_match_table = of_match_ptr(wl28681_of_match),
       //.pm = &wl28681_pm_ops,
       },
       .id_table = wl28681_i2c_id,
       .probe    = wl28681_i2c_probe,
       .shutdown = wl28681_regulator_shutdown,
};

module_i2c_driver(wl28681_i2c_driver);

MODULE_DESCRIPTION("WL28681 regulator driver");
MODULE_AUTHOR("willsemi");
MODULE_LICENSE("GPL");
