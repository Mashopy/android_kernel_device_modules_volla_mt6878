#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/err.h>

/* 全局pinctrl变量 */
static struct pinctrl *hac_pinctrl;
static struct pinctrl_state *hac_pins_high;
static struct pinctrl_state *hac_pins_low;
static int hac_current_state = 0; /* 记录当前状态 */

/* 导出HAC控制函数，供其他模块调用 */
int set_hac_status(bool enable)
{
    int ret = -EINVAL;
    
    if (!hac_pinctrl || IS_ERR(hac_pinctrl)) {
        pr_err("%s: HAC pinctrl not initialized\n", __func__);
        return -EINVAL;
    }
    
    struct pinctrl_state *pins_state = enable ? hac_pins_high : hac_pins_low;
    
    if (!pins_state || IS_ERR(pins_state)) {
        pr_err("%s: Invalid HAC pinctrl state for %s\n", 
               __func__, enable ? "high" : "low");
        return -EINVAL;
    }
    
    ret = pinctrl_select_state(hac_pinctrl, pins_state);
    if (ret) {
        pr_err("%s: Failed to set HAC pinctrl state %s (%d)\n", 
               __func__, enable ? "high" : "low", ret);
    } else {
        hac_current_state = enable ? 1 : 0;
        pr_info("%s: HAC set to %d successfully\n", __func__, enable ? 1 : 0);
    }
    
    return ret;
}
EXPORT_SYMBOL_GPL(set_hac_status);

/* 导出HAC状态获取函数 */
int get_hac_status(void)
{
    return hac_current_state;
}
EXPORT_SYMBOL_GPL(get_hac_status);

static int hac_gpio_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    dev_info(dev, "HAC GPIO driver probing\n");
    
    /* 获取pinctrl句柄 */
    hac_pinctrl = devm_pinctrl_get(dev);
    if (IS_ERR(hac_pinctrl)) {
        ret = PTR_ERR(hac_pinctrl);
        dev_err(dev, "Failed to get HAC pinctrl (%d)\n", ret);
        return ret;
    }
    
    /* 获取高低电平状态 */
    hac_pins_high = pinctrl_lookup_state(hac_pinctrl, "hac_high");
    if (IS_ERR(hac_pins_high)) {
        ret = PTR_ERR(hac_pins_high);
        dev_err(dev, "Failed to get HAC high pinctrl state (%d)\n", ret);
        hac_pins_high = NULL;
        return ret;
    }
    
    hac_pins_low = pinctrl_lookup_state(hac_pinctrl, "hac_low");
    if (IS_ERR(hac_pins_low)) {
        ret = PTR_ERR(hac_pins_low);
        dev_err(dev, "Failed to get HAC low pinctrl state (%d)\n", ret);
        hac_pins_low = NULL;
        return ret;
    }
    
    /* 设置初始状态为低电平 */
    ret = pinctrl_select_state(hac_pinctrl, hac_pins_low);
    if (ret) {
        dev_err(dev, "Failed to set initial HAC state (%d)\n", ret);
    } else {
        hac_current_state = 0;
        dev_info(dev, "HAC pinctrl initialized successfully\n");
    }
    
    dev_info(dev, "HAC GPIO driver initialized successfully\n");
    return 0;
}

static int hac_gpio_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "HAC GPIO driver removing\n");
    /* 设置为低电平状态 */
    if (hac_pinctrl && hac_pins_low && !IS_ERR(hac_pins_low))
        pinctrl_select_state(hac_pinctrl, hac_pins_low);
    
    /* devm管理的资源会自动释放 */
    return 0;
}

static const struct of_device_id hacgpio_match_table[] = {
    { .compatible = "prize,hac-gpio", },
    { },
};
MODULE_DEVICE_TABLE(of, hacgpio_match_table);

static struct platform_driver hac_gpio_driver = {
    .driver = {
        .name  = "hac_gpio",
        .owner = THIS_MODULE,
        .of_match_table = hacgpio_match_table,
    },
    .probe = hac_gpio_probe,
    .remove = hac_gpio_remove,
};

module_platform_driver(hac_gpio_driver);

MODULE_AUTHOR("Pri");
MODULE_DESCRIPTION("HAC (Hearing Aid Compatibility) GPIO driver with pinctrl support");
MODULE_LICENSE("GPL v2");
