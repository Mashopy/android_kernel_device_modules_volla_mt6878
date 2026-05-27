#ifndef __MT5706_FIRMWARE_H
#define __MT5706_FIRMWARE_H __FILE__

#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/types.h>

typedef void (*cb_fw_result)(struct i2c_client *i2c, int result);

struct sb_wrl_fw {
    struct i2c_client *parent;
    struct regmap *rm;
    struct wakeup_source *ws;
    struct delayed_work work;
    struct mutex mlock;
    cb_fw_result func;

    int state;
};

int mt5706_fw_update(struct sb_wrl_fw *fw);
struct sb_wrl_fw *sb_wrl_fw_register(
        struct i2c_client *i2c, struct regmap *rm, cb_fw_result func);

#endif
