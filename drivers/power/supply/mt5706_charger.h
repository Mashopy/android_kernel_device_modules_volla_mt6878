#ifndef __MT5706_CHARGER_H
#define __MT5706_CHARGER_H __FILE__

#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <linux/types.h>

#define MAXIC_IRQ_DISWAKE (1 << 4)
#define MAXIC_IRQ_ENWAKE (1 << 3)
#define MAXIC_IRQ_DISNOSYNC    (1 << 2)
#define MAXIC_IRQ_DIS (1 << 1)
#define MAXIC_IRQ_EN (1)

struct mt5706_gpio {
    const char *name;
    int gpio;
    int irq;
    unsigned int flags;
    unsigned int state;
};

enum {
    MT5706_GPIO_INT = 0,
    MT5706_GPIO_CHIP_EN,
    MT5706_GPIO_TX_EN,
    MT5706_GPIO_PG_INT,
    MT5706_GPIO_OTG_EN,

    MT5706_GPIO_MAX
};

struct mt5706_charger_platform_data {
    char *wireless_charger_name;
    char *charger_name;
    char *fuelgauge_name;
    char *wireless_name;
    char *battery_name;

    struct mt5706_gpio gpios[MT5706_GPIO_MAX];
};

#define mt5706_charger_platform_data_t \
    struct mt5706_charger_platform_data

struct mt5706_charger_data {
    struct i2c_client *client;
    struct regmap *rm;
    struct device *dev;
    mt5706_charger_platform_data_t *pdata;

    struct mutex charger_lock;

    struct sb_wrl_fw *fw;

    struct mutex gpio_lock;
    int det_gpio_type;

    struct power_supply *psy_chg;
    struct workqueue_struct *wqueue;

    struct wakeup_source *wpc_ws;

    struct wakeup_source *check_fw_ws;
    struct delayed_work check_fw_work;

    uint16_t addr;
    uint16_t size;
    uint8_t buffer[64];

    bool usb_in;

    /*strx*/
    uint8_t work_mode;

    /*rx*/
    bool rx_ready;
    uint16_t rx_neg_power; /*mW*/
	uint8_t rx_neg_type;

	//drv add tankaikun, start
	/*tx*/
    bool tx_stoped;
	bool revere_mode;
	bool pd_vbus;//drv add wanwen,pd-vbus compatible wireless charging status 20250715
	uint8_t recv_rx_status;

	/*mtk class*/
	struct charger_device *chg_dev;
	bool otg_enable;

	/*step charge*/
	int wireless_max_power;
	int input_current;
	// drv add tankaikun, end
	bool online;
	bool ping_ready;
    int vout;
};

ssize_t sec_wpc_show_attrs(struct device *dev,
                struct device_attribute *attr, char *buf);

ssize_t sec_wpc_store_attrs(struct device *dev,
                struct device_attribute *attr,
                const char *buf, size_t count);

#define SEC_WPC_ATTR(_name)                        \
{                                    \
    .attr = {.name = #_name, .mode = 0664},    \
    .show = sec_wpc_show_attrs,                    \
    .store = sec_wpc_store_attrs,                    \
}

static int mt5706_tx_stop_tx(struct mt5706_charger_data *charger);
//static void mt5706_rx_usb_in_event(struct power_supply *psy);
//static void mt5706_rx_usb_remove_event(struct power_supply *psy);

#endif
