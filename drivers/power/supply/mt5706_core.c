#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/regmap.h>

#include "mt5706_core.h"

static DEFINE_MUTEX(io_lock);

int mt5706_reg_write(
        struct regmap *rm, uint16_t reg, const uint8_t *buf, uint16_t len)
{
    int ret = 0;

    mutex_lock(&io_lock);
    ret = regmap_bulk_write(rm, reg, buf, len);
    mutex_unlock(&io_lock);

    if (0 != ret) {
        pr_emerg("%s:write 0x%x failed\n", __func__, reg);
    }
    return ret;
}

int mt5706_reg_read(
        struct regmap *rm, uint16_t reg, uint8_t *buf, uint16_t len)
{
    int ret = 0;

    mutex_lock(&io_lock);
    ret = regmap_bulk_read(rm, reg, buf, len);
    mutex_unlock(&io_lock);

    if (0 != ret) {
        pr_emerg("%s:read 0x%x failed\n", __func__, reg);
    }
    return ret;
}

int mt5706_reg_u8_write(struct regmap *rm, uint16_t reg, uint8_t value)
{
    uint8_t data[1] = { value };
    return mt5706_reg_write(rm, reg, data, 1);
}

int mt5706_reg_u16_write(struct regmap *rm, uint16_t reg, uint16_t value)
{
    uint8_t data[2] = { 
        (uint8_t)(value & 0xFF), 
        (uint8_t)((value >> 8) & 0xFF)
    };

    return mt5706_reg_write(rm, reg, data, 2);
}

int mt5706_reg_u32_write(struct regmap *rm, uint16_t reg, uint32_t value)
{
    uint8_t data[4] = {
        (uint8_t)(value & 0xFF), 
        (uint8_t)((value >> 8) & 0xFF), 
        (uint8_t)((value >> 16) & 0xFF), 
        (uint8_t)((value >> 24) & 0xFF)
    };
    return mt5706_reg_write(rm, reg, data, 4);
}

int mt5706_reg_u8_read(struct regmap *rm, uint16_t reg, uint8_t *value)
{
    uint8_t buf[16];
    int ret = 0;

    ret = mt5706_reg_read(rm, reg, buf, 1);
    if (0 == ret) {
        *value = buf[0];
    } else {
        *value = 0xFF;
    }

    return ret;
}

int mt5706_reg_u16_read(struct regmap *rm, uint16_t reg, uint16_t *value)
{
    uint8_t buf[16];
    int ret = 0;

    ret = mt5706_reg_read(rm, reg, buf, 2);
    if (0 == ret) {
        *value = (buf[1] << 8) | buf[0];
    } else {
        *value = 0xFFFF;
    }

    return ret;
}

int mt5706_reg_u32_read(struct regmap *rm, uint16_t reg, uint32_t *value)
{
    uint8_t buf[16];
    int ret = 0;

    ret = mt5706_reg_read(rm, reg, buf, 4);
    if (0 == ret) {
        *value = (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];
    } else {
        *value = 0xFFFFFFFF;
    }

    return ret;
}
