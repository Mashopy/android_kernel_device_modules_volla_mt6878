#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>

#include "mt5706_core.h"
#include "mt5706_charger.h"
#include "mt5706_firmware.h"
#include "mt5706_core.h"

#include "charger_class.h"

#define WAKE_LOCK_TIME_OUT    10000

// drv add tankaikun, enable otg power when mt5706 update fw, 20250409
#define UPDATE_MT5706_FW_ENABLE 0

static const struct regmap_config rm_cfg = {
    .reg_bits = 16,
    .val_bits = 8,
};

// drv add tankaikun, enable otg power when mt5706 update fw, 20250409 start
static int mtk_usb_extcon_set_vbus(bool is_on) {
	static struct charger_device *primary_charger;
//drv add wanwen,pd-vbus compatible wireless charging status 20250715 start
	struct power_supply *psy;
	struct mt5706_charger_data *charger;
//drv add wanwen,pd-vbus compatible wireless charging status 20250715 end

	primary_charger = get_charger_by_name("primary_chg");
	if (!primary_charger) {
		pr_info("%s: get primary charger device failed\n", __func__);
		return -ENODEV;
	}
//drv add wanwen,pd-vbus compatible wireless charging status 20250715 start
	psy = power_supply_get_by_name("wpc");
	if (!psy) {
		pr_info("%s: get wls device failed\n", __func__);
		return -ENODEV;
	}

	charger = (struct mt5706_charger_data *)power_supply_get_drvdata(psy);
	if (charger) {
		if (is_on) {
			charger->pd_vbus = 1;
			pr_info("%s: charger->pd_vbus is %d\n", __func__, charger->pd_vbus);
		} else {
			charger->pd_vbus = 0;
			pr_info("%s: charger->pd_vbus is %d\n", __func__, charger->pd_vbus);
		}
	}
//drv add wanwen,pd-vbus compatible wireless charging status 20250715 end
	if (is_on) {
		charger_dev_enable_otg(primary_charger, true);
	} else {
		charger_dev_enable_otg(primary_charger, false);
	}
	return 0;
}
// drv add tankaikun, enable otg power when mt5706 update fw, 20250409 end

/*
int of_get_named_gpio_flags(const struct device_node *np, const char *propname,
                     int index, enum of_gpio_flags *flags);
*/

static enum power_supply_property wpc_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_ONLINE,
    POWER_SUPPLY_PROP_TYPE,
    POWER_SUPPLY_PROP_CURRENT_MAX,
    POWER_SUPPLY_PROP_VOLTAGE_MAX,
};

static void mt5706_set_gpio_state(
        struct mt5706_gpio *gpio, unsigned int state)
{
    struct irq_desc *desc;

    if (gpio->state == state)
        return;

    desc = irq_to_desc(gpio->irq);

    if ((desc->wake_depth == 0) && (state & MAXIC_IRQ_ENWAKE))
        enable_irq_wake(gpio->irq);

    if ((desc->wake_depth > 0) && (state & MAXIC_IRQ_DISWAKE))
        disable_irq_wake(gpio->irq);

    if ((desc->depth > 0) && (state & MAXIC_IRQ_EN))
        enable_irq(gpio->irq);

    if ((desc->depth == 0) && (state & MAXIC_IRQ_DIS))
        disable_irq(gpio->irq);

    if ((desc->depth == 0) && (state & MAXIC_IRQ_DISNOSYNC))
        disable_irq_nosync(gpio->irq);

    pr_emerg("%s: %s[%d, %d, 0x%lx] set 0x%x --> 0x%x\n",
        __func__,
        gpio->name, desc->depth, desc->wake_depth,
        irq_desc_get_chip(desc)->flags,
        gpio->state, state);

    gpio->state = state;
}

static void mt5706_set_gpio_irq(
        struct mt5706_charger_data *charger, int gpio_type, unsigned int state)
{
    mutex_lock(&charger->gpio_lock);

    mt5706_set_gpio_state(&charger->pdata->gpios[gpio_type], state);

    mutex_unlock(&charger->gpio_lock);
}

static void mt5706_chip_reset(struct mt5706_charger_data *charger)
{
    uint8_t reg_buf[16];

    reg_buf[0] = 0x04;
    mt5706_reg_u8_write(charger->rm, 0x5810, reg_buf[0]);
    reg_buf[0] = 0x59;
    mt5706_reg_u8_write(charger->rm, 0x5808, reg_buf[0]);
    reg_buf[0] = 0x59;
    mt5706_reg_u8_write(charger->rm, 0x580C, reg_buf[0]);
}

static bool mt5706_power_ready_check(struct mt5706_charger_data *charger)
{
    //int ret = -1;

    uint16_t hw_chip_id = 0;
//add by wanwen,Loop through and read the chip ID 20250807 start
    for (int i = 0; i < 3; i++) {
        mt5706_reg_u16_read(charger->rm, MT5706_HW_CHIP_ID_REG, &hw_chip_id);
        if (MT5706_CHIP_ID == hw_chip_id) {
            return true;
        }
        msleep(30);
    }
//add by wanwen,Loop through and read the chip ID 20250807 end
    pr_emerg("%s: hw chip id %X\n", __func__ , hw_chip_id);
    return false;
}

static u8 mt5706_work_mode_get(struct mt5706_charger_data *charger)
{
    int ret = -1;
    uint8_t sys_state;
    uint8_t work_mode = 0;

    ret = mt5706_reg_u8_read(
            charger->rm, MT5706_SYS_STATE_REG, &sys_state);
    if (ret != 0) {
        pr_emerg("%s: i2c read failed\n", __func__);
        return work_mode;
    }

    work_mode = sys_state & 0x07;
    return work_mode;
}

static bool mt5706_firmware_ready_check(struct mt5706_charger_data *charger)
{
    uint16_t hw_chip_id = 0;
    mt5706_reg_u16_read(charger->rm, MT5706_SW_CHIP_ID_REG, &hw_chip_id);

    if (MT5706_CHIP_ID == hw_chip_id) {
        return true;
    }

    return false;
}

static bool mt5706_rx_ac_state_get(struct mt5706_charger_data *charger)
{
    uint16_t sys_state;
	bool ac_state = false;
	
    mt5706_reg_u16_read(charger->rm, MT5706_SYS_STATE_REG, &sys_state);

    ac_state = (sys_state & 0x100) ? false : true;

    return ac_state;
}
/*
static void mt5706_rx_usb_in_event(struct power_supply *psy)
{
    struct mt5706_charger_data *charger = power_supply_get_drvdata(psy);

    mutex_lock(&charger->gpio_lock);
    charger->usb_in = true;
    gpio_set_value(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 1);
    mutex_unlock(&charger->gpio_lock);
}

static void mt5706_rx_usb_remove_event(struct power_supply *psy)
{
    struct mt5706_charger_data *charger = power_supply_get_drvdata(psy);

    mutex_lock(&charger->gpio_lock);
    charger->usb_in = false;
    gpio_set_value(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 0);
    mutex_unlock(&charger->gpio_lock);
}
*/
static void mt5706_fw_result_cb(struct i2c_client *i2c, int state)
{
    struct mt5706_charger_data *charger = i2c_get_clientdata(i2c);

    pr_emerg("%s: finished fw (state = %d)\n", __func__, state);
    /*固件检查更新或更新完成，打开中断*/
    mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, MAXIC_IRQ_EN);

    mt5706_chip_reset(charger);

    // drv add tankaikun, enable otg power when mt5706 update fw, 20250409 start
    #if UPDATE_MT5706_FW_ENABLE
    mtk_usb_extcon_set_vbus(0);
    #endif /* UPDATE_MT5706_FW_ENABLE */
    // drv add tankaikun, enable otg power when mt5706 update fw, 20250409 end
}

static int mt5706_chg_get_property(struct power_supply *psy,
        enum power_supply_property psp,
        union power_supply_propval *val)
{
    struct mt5706_charger_data *charger = power_supply_get_drvdata(psy);
    int ret = 0;

    switch ((int)psp) {
    case POWER_SUPPLY_PROP_STATUS:
        /*check firmware version*/
        val->intval = 0;
	break;
    case POWER_SUPPLY_PROP_ONLINE:
	val->intval = charger->rx_ready;
        break;
    case POWER_SUPPLY_PROP_TYPE:
        val->intval = psy->desc->type;
	break;
    case POWER_SUPPLY_PROP_CURRENT_MAX:
        if (charger->vout <= 6500) {
            val->intval = 1000000;
        } else {
            if (charger->rx_neg_type == WLS_CHARGER_TYPE_EPP_15W) {
                val->intval = 1500000;
            } else if (charger->rx_neg_type == WLS_CHARGER_TYPE_EPP_10W) {
                val->intval = 1100000;
            } else {
                val->intval = 500000;
            }
        }
        break;
    case POWER_SUPPLY_PROP_VOLTAGE_MAX:
        val->intval = charger->vout;
	break;
    default:
        return -EINVAL;
    }

    return ret;
}

static int mt5706_chg_set_property(struct power_supply *psy,
        enum power_supply_property psp,
        const union power_supply_propval *val)
{
    //struct mt5706_charger_data *charger = power_supply_get_drvdata(psy);

    switch ((int)psp) {
    default:
        return -EINVAL;
    }

    return 0;
}

static void mt5706_check_fw_work(struct work_struct *work)
{
	int ret = 0;
	bool power_ready_state = false;
    struct mt5706_charger_data *charger =
        container_of(work, struct mt5706_charger_data, check_fw_work.work);

    __pm_stay_awake(charger->check_fw_ws);

    mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, MAXIC_IRQ_DISNOSYNC);

    /*TODO:check battary level
     * if (battary_leve < 50%) {
     *     goto error_free_fw_ws;
     * }
     */

    /*TODO:DC power on wireless charger chip*/
    /*TODO:wait for DC power ready
     * msleep(600);
     * */

    power_ready_state = mt5706_power_ready_check(charger);
    if (false == power_ready_state) {
        pr_emerg("%s DC power on failed\n", __func__);
        mtk_usb_extcon_set_vbus(0); //add by wanwen,Lower the VBUS 20250807 start.
        goto error_free_fw_ws;
    }

    msleep(50);
    /*TODO:check vrect voltage，must over 4v*/

    ret = mt5706_fw_update(charger->fw);
    if (0 == ret) {
        pr_emerg("%s firmware update start\n", __func__);
    } else {
        pr_emerg("%s firmware update failed %d\n", __func__, ret);
    }

error_free_fw_ws:
    __pm_relax(charger->check_fw_ws);
}

static int mt5706_set_cmd_reg(
        struct mt5706_charger_data *charger, 
        u8 cmd_code, 
        u8 cmd_index)
{
    int ret = -1;
    int time_out_cnt = 0;
    uint8_t tmp = 0;

    uint16_t cmd_addr = MT5706_CMD_REG + cmd_index;
    ret = mt5706_reg_u8_write(charger->rm, cmd_addr, cmd_code);
    if (0 != ret) {
        return ret;
    }

    ret = -1;
    do {
        mt5706_reg_u8_read(charger->rm, cmd_addr, &tmp);
        if (0 == (tmp & cmd_code)) {
            ret = 0;
            break;
        }
        msleep(20);
        time_out_cnt++;
    } while (time_out_cnt <= 6);

    return ret;
}

static int mt5706_clear_interrupt_flag(
        struct mt5706_charger_data *charger, uint32_t int_flag)
{
    int ret = 0;
    ret = mt5706_reg_u32_write(
            charger->rm, MT5706_INT_CLEAR_REG, int_flag);
    if (0 != ret) {
        pr_emerg("%s write clear reg failed\n", __func__);
        return ret;
    }

    ret = mt5706_set_cmd_reg(charger, MT5706_CMD1_CLEAR_INT, 1);
    if (0 != ret) {
        pr_emerg("%s run clear reg cmd failed\n", __func__);
        return ret;
    }

    return 0;
}

static irqreturn_t mt5706_irq_sby_thread(int irq, void *irq_data)
{
    int ret;
    struct mt5706_charger_data *charger = irq_data;
    uint32_t int_flag = 0;

    ret = mt5706_reg_u32_read(
            charger->rm, MT5706_INT_FLAG_REG, &int_flag);
    if (0 != ret) {
        pr_emerg("%s: i2c read failed\n", __func__);
        int_flag = 0;
    }

    pr_emerg("%s: start tx irq_thread 0x%X\n", __func__, int_flag);

    ret = mt5706_clear_interrupt_flag(charger, int_flag & MT5706_TX_INT_ERROR_STAT);
    if (0 != ret) {
        pr_emerg("%s: clear rx int flag failed\n", __func__);
    }

    if (int_flag & MT5706_TX_INT_ERROR_STAT) {
        pr_emerg("%s: ERR_STAT\n", __func__);
	//drv add tankaikun, disable revere after connect charger, start
	mt5706_tx_stop_tx(charger);
	charger->revere_mode=0;
	charger->recv_rx_status = 0;
	pr_emerg("%s: Tx Stop Work\n", __func__);
	//drv add tankaikun, disable revere after connect charger, end
    }

    return IRQ_HANDLED;
}

static irqreturn_t mt5706_irq_tx_thread(int irq, void *irq_data)
{
    int ret;
	u16 id_manufactrue = 0;
	uint8_t pkh = 0;
	uint8_t ss = 0;
	uint32_t pt_end_code = 0;
    struct mt5706_charger_data *charger = irq_data;
    uint32_t int_flag = 0;
	u8 id_version = 0;
	u32 id_basic_id = 0;
	u8 rx_ept_code = 0;
	static bool ocp_restart;
	if (charger->otg_enable == 1) {
		mt5706_tx_stop_tx(charger);
		charger->revere_mode = 0;
		charger->recv_rx_status = 0;
		return IRQ_HANDLED;
	}
    ret = mt5706_reg_u32_read(
            charger->rm, MT5706_INT_FLAG_REG, &int_flag);
    if (0 != ret) {
        pr_emerg("%s: i2c read failed\n", __func__);
        int_flag = 0;
    }

    pr_emerg("%s: start tx irq_thread 0x%X\n", __func__, int_flag);

    ret = mt5706_clear_interrupt_flag(charger, int_flag);
    if (0 != ret) {
        pr_emerg("%s: clear rx int flag failed\n", __func__);
    }

    if (int_flag & MT5706_TX_INT_READY) {
        pr_emerg("%s: enter tx mode ok\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_START_WORK) {
        charger->ping_ready = true;
        pr_emerg("%s: start tx work ok\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_STOP_WORK) {
        pr_emerg("%s: stop tx work ok\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_ASK_RECV) {
       
        ret = mt5706_reg_u8_read(
                charger->rm, MT5706_TX_ASK_PPP_REG, &pkh);
        pr_emerg("%s: Received Rx PPP PKT:%x\n", __func__, pkh);
    }
    if (int_flag & MT5706_TX_INT_SS) {
        ret = mt5706_reg_u8_read(
                charger->rm, MT5706_TX_RX_SS_REG, &ss);
		charger->recv_rx_status = 2;
        pr_emerg("%s: Received Rx SS PKT:%x\n", __func__, ss);
    }
    if (int_flag & MT5706_TX_INT_ID) {
        ret = mt5706_reg_u8_read(
                charger->rm, MT5706_TX_RX_ID_VERSION_REG, &id_version);
       
        ret = mt5706_reg_u16_read(
                charger->rm, MT5706_TX_RX_ID_MANUFACTURE_REG, &id_manufactrue);
        
        ret = mt5706_reg_u32_read(
                charger->rm, MT5706_TX_RX_ID_BASIC_ID_REG, &id_basic_id);
        pr_emerg("%s: Received Rx ID PKT:%x,%x,%x\n", \
                __func__, id_version, id_manufactrue, id_basic_id);
    }
    if (int_flag & MT5706_TX_INT_EXT_ID) {
        pr_emerg("%s: Received Rx Ext ID PKT\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_CONFIG) {
        pr_emerg("%s: Received Rx Config PKT\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_PT) {
        pr_emerg("%s: Power Transfer Start\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_RM_POWER) {
        
        ret = mt5706_reg_u32_read(
                charger->rm, MT5706_TX_PT_END_CODE_REG, &pt_end_code);
        pr_emerg("%s: Stop Power Transfer,Code:%x\n", __func__, pt_end_code);
        if ((TX_PT_END_RX_REQUEST == pt_end_code) 
                || (TX_PT_END_AP_CMD == pt_end_code)
                || (TX_PT_END_UVP == pt_end_code)
                || (TX_PT_END_FOD == pt_end_code)
                || (TX_PT_END_BRG_OCP == pt_end_code)
                || (TX_PT_END_PING_CLASH == pt_end_code)
                || (TX_PT_END_OTP0 == pt_end_code)
                || (TX_PT_END_VIN_OVP == pt_end_code)
                || (TX_PT_END_IIN_OCP == pt_end_code)) {
            /*触发了异常保护，Tx停止Ping，关掉OTG，等待用户干预*/
			mt5706_tx_stop_tx(charger);
			charger->revere_mode=0;
			charger->recv_rx_status = 0;
            pr_emerg("%s: Tx Stop Work\n", __func__);
        }
	charger->recv_rx_status = 0;// drv add wanwen, set tx run flag, 20250729 start
    }
    if (int_flag & MT5706_TX_INT_PKT_TIMEOUT) {
        pr_emerg("%s: Rx PKT Timeout\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_RX_EPT) {
        
        ret = mt5706_reg_u8_read(
                charger->rm, MT5706_TX_RX_EPT_CODE_REG, &rx_ept_code);
        pr_emerg("%s: Rx Send EPT,Code:%x\n", __func__, rx_ept_code);
    }
    if (int_flag & MT5706_TX_INT_PING_START) {
        pr_emerg("%s: Ping Start\n", __func__);
		ocp_restart = false;
    }
    if (int_flag & MT5706_TX_INT_PING_END) {
        pr_emerg("%s: Ping End\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_VIN_OVP) {
        pr_emerg("%s: Vin OVP\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_VIN_UVP) {
        pr_emerg("%s: Vin UVP\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_IIN_OCP) {
        pr_emerg("%s: Iin OCP\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_OTP0) {
        pr_emerg("%s: OTP\n", __func__);
    }
    if (int_flag & MT5706_TX_INT_PING_CLASH) {
        pr_emerg("%s: Ping Clash\n", __func__);
		//drv add tankaikun, disable revere after connect charger, start
		mt5706_tx_stop_tx(charger);
		charger->revere_mode=0;
		charger->recv_rx_status = 0;
		pr_emerg("%s: Tx Stop Work\n", __func__);
		//drv add tankaikun, disable revere after connect charger, end
    }
    if (int_flag & MT5706_TX_INT_BRG_OCP) {
        pr_emerg("%s: Bridge OCP\n", __func__);
		if (ocp_restart) {
			ocp_restart = true;
			if (0 != mt5706_set_cmd_reg(charger, MT5706_CMD0_ENTER_TX, 0)) {
				pr_emerg("%s:run enter tx cmd failed\n", __func__);
				/*TODO:DC power off*/
				mtk_usb_extcon_set_vbus(0);
				charger->revere_mode=0;
				charger->recv_rx_status = 0;
			}
			msleep(300); /*wait for tx ready*/
			if (0 != mt5706_set_cmd_reg(charger, MT5706_TX_CMD1_START_WORKING, 1)) {
				pr_emerg("%s:run start tx cmd failed\n", __func__);
				/*TODO:DC power off*/
				mtk_usb_extcon_set_vbus(0);
				charger->revere_mode=0;
				charger->recv_rx_status = 0;
			}
		} else {
			pr_emerg("%s:cannot restart tx again\n", __func__);
			mt5706_tx_stop_tx(charger);
			charger->revere_mode=0;
			charger->recv_rx_status = 0;
		}
    }
    if (int_flag & MT5706_TX_INT_FOD) {
        pr_emerg("%s: FOD\n", __func__);
    }

    return IRQ_HANDLED;
}
/*
static void mt5706_rx_alive_monitor_work(struct work_struct *work)
{
    //when Rx remove from Tx,Rx(without AUX power) can not report interrupt
}
*/

static int mt5706_rx_set_vout_target(
        struct mt5706_charger_data *charger, uint16_t vout_target)
{
    int ret = 0;

    if (vout_target > 12500) {
        return -1;
    }

    ret = mt5706_reg_u16_write(
            charger->rm, MT5706_RX_SET_VOUT_REG, vout_target);
    if (0 != ret) {
        pr_emerg("%s write clear reg failed\n", __func__);
        return ret;
    }

    ret = mt5706_set_cmd_reg(charger, MT5706_RX_CMD1_SET_VOUT, 1);
    if (0 != ret) {
        pr_emerg("%s run clear reg cmd failed\n", __func__);
        return ret;
    }

    charger->vout = vout_target;
    return 0;
}

static void mt5706_rx_restart(struct mt5706_charger_data *charger)
{
    /*TODO:diconnect AUX power*/

    mutex_lock(&charger->gpio_lock);
    gpio_set_value(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 1);
    mutex_unlock(&charger->gpio_lock);

    msleep(10 * 1000); /*wait for Tx stop power transfer*/

    mutex_lock(&charger->gpio_lock);
    if (false == charger->otg_enable) {
        gpio_set_value(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 0);
    }
    mutex_unlock(&charger->gpio_lock);
}

uint8_t mt5706_rx_get_neg_power_type(struct mt5706_charger_data *charger)
{
    uint32_t sys_state = 0;
	int ret = mt5706_reg_u32_read(
            charger->rm, MT5706_SYS_STATE_REG, &sys_state);
    if (0 != ret) {
        sys_state = 0;
        pr_emerg("%s: i2c read failed\n", __func__);
    }
    return (sys_state >> 19) & 0x07;
}

//drv add tankaikun, step charging, 20250611 start
void En_Dis_add_current(struct mt5706_charger_data *charger, bool enable){
	int ret = 0;
	uint32_t temp=0x20,temp_code=0;

	ret = mt5706_reg_u32_read(charger->rm, MT5706_RX_CTRL_REG, &temp_code);
	if (0 != ret) {
		pr_emerg("%s: i2c read failed\n", __func__);
		temp_code = 0;
	}
	if (enable) {
		temp_code |= temp;
	} else {
		temp_code &= ~temp;
	}
	pr_err("tempcode=0x%x\n", temp_code);
	mt5706_reg_u32_write(charger->rm, MT5706_RX_CTRL_REG, temp_code);
}

void Set_staystate_current(void)
{
	int ret = 0;
	struct power_supply *chg_psy = NULL;
	union  power_supply_propval online = {0};

	chg_psy = power_supply_get_by_name("mtk-master-charger");
	if (IS_ERR_OR_NULL(chg_psy))
		pr_emerg("%s: devm power fail to get chg_psy\n", __func__);
	else {
		pr_emerg("%s: update mtk master charge \n", __func__);
		online.intval = 1;
		ret = power_supply_set_property(chg_psy,
                POWER_SUPPLY_PROP_ONLINE, &online);
		if (ret < 0)
			pr_err("set chg online fail\n");
	}

	return;
}

void mt5706_dump_info(struct mt5706_charger_data *charger) {
	int ret = 0;
	uint16_t vout = 0;
	uint16_t vrect = 0;
	uint16_t iout = 0;

	ret = mt5706_reg_u16_read(
			charger->rm, MT5706_RX_VOUT_REG, &vout);
	if (0 != ret) {
		vout = 0;
		pr_emerg("%s: i2c read failed\n", __func__);
	}

	ret = mt5706_reg_u16_read(
			charger->rm, MT5706_RX_IOUT_REG, &iout);
	if (0 != ret) {
		iout = 0;
		pr_emerg("%s: i2c read failed\n", __func__);
	}

	ret = mt5706_reg_u16_read(
			charger->rm, MT5706_RX_VRECT_REG, &vrect);
	if (0 != ret) {
		vrect = 0;
		pr_emerg("%s: i2c read failed\n", __func__);
	}
	pr_emerg("%s: rx_neg_power vout:%dmV vrect:%dmV iout:%dmA\n", __func__, vout, vrect, iout);

	return;
}

static void mt5706_add_current(struct mt5706_charger_data *charger) {
	long voltage = 9000;
	long tcurrent =0;
	long maxpower =0;
	long powertemp =0;
	long maxchargecurrent = 1000000;

	mt5706_dump_info(charger);

	if(charger->rx_neg_type == WLS_CHARGER_TYPE_BPP){
		charger->wireless_max_power = 5;
		voltage = 5000;
		maxchargecurrent = 1000000;
		pr_info("[%s]: BPP Load power is recommended to be less than %dW\n",
			__func__,charger->wireless_max_power);
	} else if(charger->rx_neg_type == WLS_CHARGER_TYPE_EPP_10W){
		charger->wireless_max_power = 10;
		voltage = 9000;
		maxchargecurrent = 1100000;
		pr_info("[%s]: EPP_10W Load power is recommended to be less than %dW\n",
			__func__,charger->wireless_max_power);
	} else if(charger->rx_neg_type == WLS_CHARGER_TYPE_EPP_15W){
		charger->wireless_max_power = 15;
		voltage = 9000;
		maxchargecurrent = 1500000;
		pr_info("[%s]: EPP_15W Load power is recommended to be less than %dW\n",
			__func__,charger->wireless_max_power);
	} else {
		charger->wireless_max_power = 5;
		voltage = 5000;
		maxchargecurrent = 1000000;
		pr_info("[%s]: Unknow type Load power is recommended to be less than %dW\n",
			__func__,charger->wireless_max_power);
	}

	maxpower = charger->wireless_max_power * 1000 * 88 / 100;
	tcurrent = charger->input_current / 1000;
	if(tcurrent == 0)
		tcurrent = 100;
	powertemp = voltage * tcurrent / 1000;
	pr_info("[%s]: max_power=%dW max_curr=%ldmA vol=%ldmV tcurrent=%ldmA \n",__func__,
		charger->wireless_max_power, (maxchargecurrent/1000), voltage, tcurrent);
	pr_info("[%s]: powertemp = %ld , maxpower = %ld\n",__func__,powertemp, maxpower);

	if(powertemp <= maxpower) {
		powertemp = powertemp + 1000;
		if(powertemp >= (charger->wireless_max_power*1000)) {
			powertemp = (charger->wireless_max_power*1000);
		}
		pr_info("[%s] add power  = %ld mW\n", __func__, powertemp);
		charger->input_current = powertemp * 1000 / voltage * 1000;
		if(charger->rx_neg_type == WLS_CHARGER_TYPE_BPP) {
			charger->input_current = 1000000;
		}
		if(charger->input_current > maxchargecurrent) {
			charger->input_current = maxchargecurrent;
		}
		pr_info("[%s] Set input_current = %d mA \n", __func__, charger->input_current/1000);
		Set_staystate_current();
	}else{
		pr_info("[%s] return!\n", __func__);
		En_Dis_add_current(charger, false);
		return;
	}
}
//drv add tankaikun, step charging, 20250611 end

static irqreturn_t mt5706_irq_rx_thread(int irq, void *irq_data)
{
    int ret;
    struct mt5706_charger_data *charger = irq_data;
    struct power_supply *chg_psy = NULL;
    bool power_ready_state = false;//add by wanwen,check if rx is ok

    if (false == charger->tx_stoped) {
		mt5706_tx_stop_tx(charger);
    }

    uint32_t int_flag = 0;
    ret = mt5706_reg_u32_read(
            charger->rm, MT5706_INT_FLAG_REG, &int_flag);
    if (0 != ret) {
        pr_emerg("%s: i2c read failed\n", __func__);
        int_flag = 0;
    }

    pr_emerg("%s: start rx irq_thread 0x%X\n", __func__, int_flag);

    chg_psy = power_supply_get_by_name("primary_chg");

    ret = mt5706_clear_interrupt_flag(charger, int_flag);
    if (0 != ret) {
        pr_emerg("%s: clear rx int flag failed\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_WPC_NEG_OK) {
        uint16_t max_power = 0;
        uint8_t neg_type = 0;
        neg_type = mt5706_rx_get_neg_power_type(charger);
        if (MT5706_RX_NEG_POWER_TYPE_BPP == neg_type) {
            max_power = 5000;
			charger->rx_neg_type = WLS_CHARGER_TYPE_BPP; // drv add tankaikun, apply mt5706 to mtk charger class, 20250409
            pr_emerg("%s: Power Type BPP\n", __func__);
        } else if ((MT5706_RX_NEG_POWER_TYPE_1P2_EPP == neg_type) 
                      || (MT5706_RX_NEG_POWER_TYPE_1P3_EPP == neg_type)) {
            ret = mt5706_reg_u16_read(
                    charger->rm, MT5706_RX_NEG_POWER_REG, &max_power);
            if (0 != ret) {
                max_power = 0;
                pr_emerg("%s: i2c read failed\n", __func__);
            }
            max_power = max_power * 500;
			// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
			if (max_power > 12000) {
				charger->rx_neg_type = WLS_CHARGER_TYPE_EPP_15W;
			} else {
				charger->rx_neg_type = WLS_CHARGER_TYPE_EPP_10W;
			}
			// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end
            pr_emerg("%s: Power Type EPP %dmW\n", __func__, max_power);
        } else {
			charger->rx_neg_type = WLS_CHARGER_TYPE_UNKNOWN;// drv add tankaikun, apply mt5706 to mtk charger class, 20250409
            pr_emerg("%s: Unknown Power Type 0x%X\n", __func__, neg_type);
        }
        charger->rx_neg_power = max_power;
    }

    if ((int_flag & MT5706_RX_INT_READY) && (charger->otg_enable == 0)) {
        /*all Rx cust register valide*/
//add by wanwen,check if rx is ok 20250815 start
        power_ready_state = mt5706_power_ready_check(charger);
        if (true == power_ready_state) {
            charger->work_mode = MT5706_WORK_MODE_RX;
            charger->rx_ready = true;
        } else {
            pr_err("%s: mt5706_power_ready_check failed\n", __func__);
	}
//add by wanwen,check if rx is ok 20250815 end
    }

    if (int_flag & MT5706_RX_INT_SS_READY) {
        uint16_t ss_code = 0;
        ret = mt5706_reg_u16_read(
                charger->rm, MT5706_RX_SS_CODE_REG, &ss_code);
        if (0 != ret) {
            ss_code = 0;
            pr_emerg("%s: i2c read failed\n", __func__);
        }
        pr_emerg("%s: SS Ready %d\n", __func__, ss_code);
    }

    if (int_flag & MT5706_RX_INT_LDO_ON) {
        uint16_t vout = 0;
        uint16_t iout = 0;
        ret = mt5706_reg_u16_read(
                charger->rm, MT5706_RX_VOUT_REG, &vout);
        if (0 != ret) {
            vout = 0;
            pr_emerg("%s: i2c read failed\n", __func__);
        }
        pr_emerg("%s: MLDO On %dmV\n", __func__, vout);

        // drv add tankaikun, step charging, 20250611 start
        En_Dis_add_current(charger, true);
        charger->input_current = 500000;
        // drv add tankaikun, step charging, 20250611  end

        if (charger->rx_neg_power > 12000) { /*Tx Max power over 12w*/
            /*TODO:set PMIC current limist
             * not over rx_neg_power / 12000
             */
            ret = mt5706_rx_set_vout_target(charger, 9000);
            if (0 != ret) {
                pr_emerg("%s: set vout %dmV start failed\n", __func__, 9000);
            }

            ret = mt5706_reg_u16_read(
					charger->rm, MT5706_RX_VOUT_REG, &vout);
            if (0 != ret) {
				vout = 0;
				pr_emerg("%s: i2c read failed\n", __func__);
            }
            pr_emerg("%s: rx_neg_power_12W On %dmV\n", __func__, vout);

            ret = mt5706_reg_u16_read(
					charger->rm, MT5706_RX_IOUT_REG, &iout);
            if (0 != ret) {
				iout = 0;
				pr_emerg("%s: i2c read failed\n", __func__);
            }
            pr_emerg("%s: rx_neg_power_12W On %dmA\n", __func__, iout);
        } else if (charger->rx_neg_power > 9000) { /*Tx Max power over 9w*/
            /*TODO:set PMIC current limist
             * not over rx_neg_power / 9000
             */
            ret = mt5706_rx_set_vout_target(charger, 9000);

            if (0 != ret) {
                pr_emerg("%s: set vout %dmV start failed\n", __func__, 9000);
            }

            //drv add tankaikun start
            ret = mt5706_reg_u16_read(
					charger->rm, MT5706_RX_VOUT_REG, &vout);
            if (0 != ret) {
				vout = 0;
				pr_emerg("%s: i2c read failed\n", __func__);
            }
            pr_emerg("%s: rx_neg_power_9W On %dmV\n", __func__, vout);

            ret = mt5706_reg_u16_read(
					charger->rm, MT5706_RX_IOUT_REG, &iout);
            if (0 != ret) {
				iout = 0;
				pr_emerg("%s: i2c read failed\n", __func__);
            }
			pr_emerg("%s: rx_neg_power_9W On %dmA\n", __func__, iout);
			// drv add tankaikun end
        }
		// drv add tankaikun, update mtk charge thread, start
		if (IS_ERR_OR_NULL(chg_psy))
			pr_emerg("%s: devm power fail to get chg_psy\n", __func__);
		else {
			pr_emerg("%s: update mtk charging thread \n", __func__);
			power_supply_changed(chg_psy);
		}
		// drv add tankaikun, update mtk charge thread, end
    }

    if (int_flag & MT5706_RX_INT_LDO_OFF) {
        uint32_t off_code = 0;
        mt5706_reg_u32_read(
                charger->rm, MT5706_RX_MLDO_OFF_REASON_REG, &off_code);
        /*TODO:restart Rx after several seconds,but with lower payload*/
        pr_emerg("%s: MLDO Off 0x%X\n", __func__, off_code);

        mt5706_rx_restart(charger);
    }

    if (int_flag & MT5706_RX_INT_PPP_ASK_SEND) {
        pr_emerg("%s: PPP ASK Send Out\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_PPP_FSK_RCV) {
        uint8_t fsk_data_buf[12];
        ret = mt5706_reg_read(
                charger->rm, MT5706_RX_PPP_FSK_REG, fsk_data_buf, 12);
        if (0 != ret) {
            pr_emerg("%s: i2c read failed\n", __func__);
        }
        pr_emerg("%s: PPP FSK Received 0x%x 0x%x\n", 
                __func__, fsk_data_buf[0], fsk_data_buf[1]);
    }

    if (int_flag & MT5706_RX_INT_PPP_FSK_TO) {
        /*TODO:retry PPP or give up*/
        pr_emerg("%s: PPP FSK Timeout\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_AC_MISSING) {
        /*Rx remove from Tx or AC not stable*/
        /*if no aux power,Rx cant not send out this interrupt*/
        /*TODO:diconnect AUX power*/
    }

    if (int_flag & MT5706_RX_INT_SV_OK) {
        uint16_t vout = 0;
        ret = mt5706_reg_u16_read(
                charger->rm, MT5706_RX_VOUT_REG, &vout);
        if (0 != ret) {
            vout = 0;
            pr_emerg("%s: i2c read failed\n", __func__);
        }
        pr_emerg("%s: Set Vout OK %dmV\n", __func__, vout);
    }

    if (int_flag & MT5706_RX_INT_SV_FAILED) {
        uint16_t vout = 0;
        ret = mt5706_reg_u16_read(
                charger->rm, MT5706_RX_VOUT_REG, &vout);
        if (0 != ret) {
            vout = 0;
            pr_emerg("%s: i2c read failed\n", __func__);
        }
        /*TODO:retry or set vout to previous value*/
        pr_emerg("%s: Set Vout Failed %dmV\n", __func__, vout);
    }

    if (int_flag & MT5706_RX_INT_LDO_OPP0) {
        pr_emerg("%s: !!! OPP0\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_LDO_OPP1) {
        pr_emerg("%s: !!! OPP1\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_LDO_OVP0) {
        pr_emerg("%s: !!! OVP0\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_LDO_OVP1) {
        pr_emerg("%s: !!! OVP1\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_LDO_OCP) {
        /*TODO:set Rx payload lower*/
        pr_emerg("%s: !!! OCP\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_LDO_SCP) {
        pr_emerg("%s: !!! SCP\n", __func__);
        if (charger->revere_mode) {
            mt5706_tx_stop_tx(charger);
            charger->revere_mode=0;
            charger->recv_rx_status = 0;
            pr_emerg("%s: mt5706_tx_stop_tx for SCP !!!\n", __func__);
        }
    }

    if (int_flag & MT5706_RX_INT_LDO_OTP0) {
        /*TODO:set Rx payload lower*/
        pr_emerg("%s: !!! OTP0\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_LDO_OTP1) {
        pr_emerg("%s: !!! OTP1\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_VOUT_OVP) {
        pr_emerg("%s: !!! Vout OVP\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_BACK_FLOW) {
        /*TODO:diconnect aux power*/
        pr_emerg("%s: !!! Back Flow\n", __func__);
    }

    if (int_flag & MT5706_RX_INT_TX_FSK_RCV) {
        pr_emerg("%s: FSK From Tx\n", __func__);
    }

	// drv add tankaikun, step charging, 20250611 start
	if (int_flag & MT5706_RX_INT_READY_FOR_LOAD) {
		pr_emerg("%s: curr load From Tx\n", __func__);
		mt5706_add_current(charger);
	}
	// drv add tankaikun, step charging, 20250611 end

    pr_emerg("%s: finish\n", __func__);
    power_supply_changed(charger->psy_chg);
    return IRQ_HANDLED;
}

static irqreturn_t mt5706_irq_thread(int irq, void *irq_data)
{	
	uint8_t work_mode = 0;
    struct mt5706_charger_data *charger = irq_data;
    __pm_stay_awake(charger->wpc_ws);
    mutex_lock(&charger->charger_lock);

    if (true == charger->ping_ready) {
        uint32_t int_flag = 0;
        uint8_t ret = 0;
        ret = mt5706_reg_u32_read(charger->rm, MT5706_INT_FLAG_REG, &int_flag);
        if (0 != ret) {
            charger->ping_ready = false;
            mt5706_tx_stop_tx(charger);
            charger->revere_mode=0;
            charger->recv_rx_status = 0;
            pr_emerg("%s: i2c read failed\n", __func__);
        } else {
            if (int_flag & MT5706_SBY_INT_READY) {
                charger->ping_ready = false;
                mt5706_tx_stop_tx(charger);
                charger->revere_mode=0;
                charger->recv_rx_status = 0;
            }
        }
    }

    work_mode = mt5706_work_mode_get(charger);
    if (MT5706_WORK_MODE_RX == work_mode) {
        mt5706_irq_rx_thread(irq, irq_data);
    } else if (MT5706_WORK_MODE_TX == work_mode) {
        mt5706_irq_tx_thread(irq, irq_data);
    } else if (MT5706_WORK_MODE_SBY == work_mode) {
        /*standby mode interrupt disabled by firmware*/
        mt5706_irq_sby_thread(irq, irq_data);
    } else {
        pr_emerg("%s: mode error %x\n", __func__, work_mode);
        if (charger->revere_mode) {
            mt5706_tx_stop_tx(charger);
            charger->revere_mode=0;
            charger->recv_rx_status = 0;
            pr_emerg("%s: mt5706_tx_stop_tx for SCP !!!\n", __func__);
        }
    }

    mutex_unlock(&charger->charger_lock);
    __pm_relax(charger->wpc_ws);

    return IRQ_HANDLED;
}

enum {
    WPC_ADDR = 0,
    WPC_SIZE,
    WPC_DATA,
    WPC_EN_TX,
    WPC_STOP_TX,
    WPC_MODE_GET,
};

static struct device_attribute mt5706_attributes[] = {
    SEC_WPC_ATTR(addr),
    SEC_WPC_ATTR(size),
    SEC_WPC_ATTR(data),
    SEC_WPC_ATTR(en_tx),
    SEC_WPC_ATTR(stop_tx),
    SEC_WPC_ATTR(mode_get),
};

ssize_t sec_wpc_show_attrs(struct device *dev,
                  struct device_attribute *attr, char *buf)
{
    struct power_supply *psy = dev_get_drvdata(dev);
    struct mt5706_charger_data *charger = power_supply_get_drvdata(psy);
    const ptrdiff_t offset = attr - mt5706_attributes;
    int i = 0;

    switch (offset) {
    case WPC_ADDR:
        i += scnprintf(buf + i, PAGE_SIZE - i, "0x%x\n", charger->addr);
        break;
    case WPC_SIZE:
        i += scnprintf(buf + i, PAGE_SIZE - i, "0x%x\n", charger->size);
        break;
    case WPC_DATA:
        if (charger->size == 0) {
            charger->size = 1;
        }

        if (charger->size + charger->addr <= 0xFFFF) {
            u8 data;
            int j;

            for (j = 0; j < charger->size; j++) {
                if (0 != mt5706_reg_u8_read(
                            charger->rm, charger->addr + j, &data)) {
                    pr_emerg("%s: read fail\n", __func__);
                    i += scnprintf(buf + i, PAGE_SIZE - i,
                        "addr: 0x%x read fail\n", charger->addr + j);
                    continue;
                }
                i += scnprintf(buf + i, PAGE_SIZE - i,
                    "addr: 0x%x, data: 0x%x\n", charger->addr + j, data);
            }
        }
        break;
    case WPC_MODE_GET:
        if (MT5706_WORK_MODE_TX == charger->work_mode) {
            i += scnprintf(buf + i, PAGE_SIZE - i, "%s\n", "work mode tx\n");
        } else if (MT5706_WORK_MODE_SBY == charger->work_mode) {
            i += scnprintf(buf + i, PAGE_SIZE - i, "%s\n", "work mode sby\n");
        } else if (MT5706_WORK_MODE_RX == charger->work_mode) {
            i += scnprintf(buf + i, PAGE_SIZE - i, "%s\n", "work mode rx\n");
        }
		break;
    default:
        break;
    }

    return i;
}

static int mt5706_tx_enter_tx(struct mt5706_charger_data *charger)
{
    bool power_ready_state = false;
    power_ready_state = mt5706_power_ready_check(charger);
    if (true == power_ready_state) {
        u8 work_mode = mt5706_work_mode_get(charger);
        if (MT5706_WORK_MODE_RX == work_mode) {
            pr_emerg("%s:Rx is working\n", __func__);
            return -1;
        } else {
            mtk_usb_extcon_set_vbus(0);
            /*延时确保芯片下电*/
			msleep(200);
        }
    }

    gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_TX_EN].gpio, 1);
    /*开OTG前，禁用Rx模式。防止无线充输出和OTG输入冲突*/
    gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 1);

    mtk_usb_extcon_set_vbus(1);
    msleep(200);

    power_ready_state = false;
    power_ready_state = mt5706_power_ready_check(charger);
    if (false == power_ready_state) {
        pr_emerg("%s:DC power on failed\n", __func__);
        goto en_tx_failed;
    }

    if (0 != mt5706_set_cmd_reg(charger, MT5706_CMD0_ENTER_TX, 0)) {
        pr_emerg("%s:run enter tx cmd failed\n", __func__);
        goto en_tx_failed;
    }
    msleep(260); /*wait for tx ready*/

	if (0 != mt5706_reg_u16_write(charger->rm, MT5706_TX_VIN_OVP_REG, 7000)) {
        pr_emerg("%s write tx vin ovp reg failed\n", __func__);
        goto en_tx_failed;
    }
    msleep(40);

    charger->ping_ready = false;
    if (0 != mt5706_set_cmd_reg(charger, MT5706_TX_CMD1_START_WORKING, 1)) {
        pr_emerg("%s:run start tx cmd failed\n", __func__);
        goto en_tx_failed;
    }

    charger->work_mode = MT5706_WORK_MODE_TX;
    charger->tx_stoped = false;

    return 0;

en_tx_failed:
    mtk_usb_extcon_set_vbus(0);
    /*允许Rx模式*/
    if (false == charger->otg_enable) {
        gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 0);
    }
    return -1;
}

static int mt5706_tx_stop_tx(struct mt5706_charger_data *charger)
{
	charger->ping_ready = false;
    mtk_usb_extcon_set_vbus(0);
    gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_TX_EN].gpio, 0);
    charger->tx_stoped = true;

    msleep(100); //等待下电完成
    charger->work_mode = mt5706_work_mode_get(charger); //更新work_mode

    /*允许Rx模式*/
    if (false == charger->otg_enable) {
        gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 0);
    }

    return 0;
}

ssize_t sec_wpc_store_attrs(
                    struct device *dev,
                    struct device_attribute *attr,
                    const char *buf, size_t count)
{
    struct power_supply *psy = dev_get_drvdata(dev);
    struct mt5706_charger_data *charger =
        power_supply_get_drvdata(psy);
    const ptrdiff_t offset = attr - mt5706_attributes;
    int x, ret = -EINVAL;

    switch (offset) {
    case WPC_ADDR:
        if (sscanf(buf, "0x%x\n", &x) == 1) {
            charger->addr = x;
        }
        ret = count;
        break;
    case WPC_SIZE:
        if (sscanf(buf, "%d\n", &x) == 1) {
            charger->size = x;
        }
        ret = count;
        break;
    case WPC_DATA:
        if (sscanf(buf, "0x%x", &x) == 1) {
            u8 data = x;
            if (0 != mt5706_reg_u8_write(charger->rm, charger->addr, data)) {
                pr_emerg("%s: addr: 0x%x write fail\n", \
                        __func__, charger->addr);
            }
        }
        ret = count;
        break;
    case WPC_EN_TX:
        if (0 != mt5706_tx_enter_tx(charger)) {
            pr_emerg("%s: enter tx failed\n", __func__);
        }
        ret = count;
        break;
    case WPC_STOP_TX:
        if (0 != mt5706_tx_stop_tx(charger)) {
            pr_emerg("%s: stop tx failed\n", __func__);
        }
        ret = count;
        break;
    default:
        break;
    }
    return ret;
}

static int sec_wpc_create_attrs(struct device *dev)
{
    unsigned long i;
    int rc;

    for (i = 0; i < ARRAY_SIZE(mt5706_attributes); i++) {
        rc = device_create_file(dev, &mt5706_attributes[i]);
        if (rc)
            goto create_attrs_failed;
    }
    goto create_attrs_succeed;

create_attrs_failed:
    while (i--)
        device_remove_file(dev, &mt5706_attributes[i]);
create_attrs_succeed:
    return rc;
}

static int mt5706_parse_gpio(struct device_node *np,
    const char *name, mt5706_charger_platform_data_t *pdata, int gpio_type)
{
    struct mt5706_gpio *gpio;
    unsigned int gpio_flags = 0;
    int ret = 0;
	int pin_num = 0;

    ret = of_get_named_gpio(np, name, 0);
    if (ret < 0) {
        pr_emerg("%s: failed to get %s, ret = %d\n", __func__, name, ret);
        return ret;
    }

    pin_num = ret;
    ret = gpio_request(pin_num, name);
    if ((MT5706_GPIO_CHIP_EN == gpio_type) 
            || (MT5706_GPIO_TX_EN == gpio_type)
			|| (MT5706_GPIO_OTG_EN == gpio_type)) {
        if (ret) {
            pr_emerg("%s: can't get chip_en %d\n", __func__, ret);
            return ret;
        }
        gpio_direction_output(pin_num, 0);
    } else if (MT5706_GPIO_INT == gpio_type || MT5706_GPIO_PG_INT == gpio_type) {
        if (ret) {
            pr_emerg("%s: can't get wpc_en %d\n", __func__, ret);
            return ret;
        }
        gpio_direction_input(pin_num);
    }

    gpio = &pdata->gpios[gpio_type];
    gpio->name = name;
    gpio->gpio = pin_num;
    gpio->flags = gpio_flags;
	if (MT5706_GPIO_INT == gpio_type || MT5706_GPIO_PG_INT == gpio_type)
		gpio->irq = gpio_to_irq(pin_num);

    pr_emerg("%s: name = %s, gpio = %d, flags = %d, irq = %d\n",
        __func__, gpio->name, gpio->gpio, gpio->flags, gpio->irq);
    return 0;
}

static int mt5706_parse_dt(struct device *dev, mt5706_charger_platform_data_t *pdata)
{
    struct device_node *np = dev->of_node;
    //enum of_gpio_flags irq_gpio_flags;
    int ret;
    /*changes can be added later, when needed*/

    ret = of_property_read_string(np,
        "mt5706,charger_name", (char const **)&pdata->charger_name);
    if (ret) {
        pr_emerg("%s: Charger name is Empty\n", __func__);
        pdata->charger_name = "sec-charger";
    }

    ret = of_property_read_string(np,
        "mt5706,fuelgauge_name", (char const **)&pdata->fuelgauge_name);
    if (ret) {
        pr_emerg("%s: Fuelgauge name is Empty\n", __func__);
        pdata->fuelgauge_name = "sec-fuelgauge";
    }

    ret = of_property_read_string(np,
        "mt5706,battery_name", (char const **)&pdata->battery_name);
    if (ret) {
        pr_emerg("%s: battery_name is Empty\n", __func__);
        pdata->battery_name = "battery";
    }

    ret = of_property_read_string(np,
        "mt5706,wireless_name", (char const **)&pdata->wireless_name);
    if (ret) {
        pr_emerg("%s: wireless_name is Empty\n", __func__);
        pdata->wireless_name = "wireless";
    }

    ret = of_property_read_string(np, "mt5706,wireless_charger_name",
        (char const **)&pdata->wireless_charger_name);
    if (ret) {
        pr_emerg("%s: wireless_charger_name is Empty\n", __func__);
        pdata->wireless_charger_name = "wpc";
    }

    ret = mt5706_parse_gpio(np, "irq_gpio", pdata, MT5706_GPIO_INT);
    ret = mt5706_parse_gpio(np, "chipen_gpio", pdata, MT5706_GPIO_CHIP_EN);
    ret = mt5706_parse_gpio(np, "tx_en_gpio", pdata, MT5706_GPIO_TX_EN);
	ret = mt5706_parse_gpio(np, "otg_en_gpio", pdata, MT5706_GPIO_OTG_EN);
    ret = mt5706_parse_gpio(np, "pg_irq_gpio", pdata, MT5706_GPIO_PG_INT);

    return 0;
}

static const struct power_supply_desc wpc_power_supply_desc = {
    .name = "wpc",
    .type = POWER_SUPPLY_TYPE_WIRELESS,
    .properties = wpc_props,
    .num_properties = ARRAY_SIZE(wpc_props),
    .get_property = mt5706_chg_get_property,
    .set_property = mt5706_chg_set_property,
};

static int mt5706_request_irq(struct mt5706_charger_data *charger,
    int gpio_type, irq_handler_t thread_fn, unsigned int irqflags)
{
    struct mt5706_gpio *gpio = &charger->pdata->gpios[gpio_type];
    int ret = 0;

    if (gpio->irq <= 0)
        return 0;
    /*申请中断时，中断为关闭状态
     *在固件更新结束或不更新固件，才会开启中断
     *bypass功能不再需要
     */
    ret = request_threaded_irq(
            gpio->irq, NULL, 
            thread_fn, irqflags, gpio->name, charger);
    if (ret) {
        pr_emerg("%s: failed to request irq (%s, ret = %d)\n",
            __func__, gpio->name, ret);
        return ret;
    }

    return 0;
}

static void mt5706_startup(struct mt5706_charger_data *charger)
{
    bool power_ready_state = false;
    bool rx_is_working = false;
    bool firmware_ready_state = false;
    u8 work_mode = 0;

    charger->tx_stoped = true;
    charger->work_mode = 0;

    charger->rx_ready = false;
    charger->rx_neg_power = false;

    power_ready_state = mt5706_power_ready_check(charger);
    if (false == power_ready_state) {
        /*没有供电/或者无线充芯片无固件*/
	goto check_fw_update;
    }

    firmware_ready_state = mt5706_firmware_ready_check(charger);
    if (true != firmware_ready_state) {
        /*无线充芯片无固件*/
        pr_emerg("%s,firmware not ready\n", __func__);
        goto check_fw_update;
    }

    work_mode = mt5706_work_mode_get(charger);
    charger->work_mode = work_mode;
    if (MT5706_WORK_MODE_RX == work_mode) {
        bool ac_state = mt5706_rx_ac_state_get(charger);
        if (false == ac_state) {
            /*vin or aux power on*/
            /*TODO:remove vin or aux power*/
        } else {
            /*手机开机，无线充就在Rx模式，暂时不检查固件更新*/
            rx_is_working = true;
            charger->rx_ready = true;
            /*Rx is working*/
        }
    } else if (MT5706_WORK_MODE_TX == work_mode) {
        /*vin or aux power on*/
        /*TODO:remove vin or aux power*/
    } else if (MT5706_WORK_MODE_SBY == work_mode) {
        //if (no vin and aux power) {
            /*此时OTG没有开启，应该是放在Tx上，会很快进入Rx*/
            rx_is_working = true;
            charger->rx_ready = true;
        //} else {
            /*if not intend to check firmware crc or update firmware
             *TODO:disconnect vin or aux power
             */
        //}
    }

check_fw_update:
    if (false == rx_is_working) {
// drv mod tankaikun, disable update fw, 20250409 start
#if UPDATE_MT5706_FW_ENABLE
	// drv add tankaikun, enable otg power when mt5706 update fw, 20250409 start
	mtk_usb_extcon_set_vbus(1);
	// drv add tankaikun, enable otg power when mt5706 update fw, 20250409 end
        queue_delayed_work(charger->wqueue, &charger->check_fw_work, msecs_to_jiffies(1));
#else
	mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, MAXIC_IRQ_EN);
#endif
// drv mod tankaikun, disable update fw, 20250409 end
    } else {
        /*不进行固件升级，把中断打开*/
	mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, MAXIC_IRQ_EN);
// drv add wanwen, reset wls state, 20250818 start
	mt5706_chip_reset(charger);
// drv add wanwen, reset wls state, 20250818 end
    }
}

// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
static int mt5706_is_enabled(struct charger_device *chg_dev, bool * enabled) {
    int ret=0;
    struct mt5706_charger_data *charger = dev_get_drvdata(&chg_dev->dev);

	*enabled = charger->rx_ready;
    dev_info(charger->dev, "charger is %s\n",
            *enabled ? "charging" : "not charging");

    return ret;
}

static int mt5706_plug_out(struct charger_device *chg_dev) {
    int ret = 0;
    struct mt5706_charger_data *charger = dev_get_drvdata(&chg_dev->dev);

    dev_info(charger->dev, "%s\n", __func__);
	charger->rx_ready = false;
	charger->rx_neg_type = WLS_CHARGER_TYPE_UNKNOWN;

    return ret;
}

static int mt5706_get_property(struct charger_device *chg_dev,
			    enum charger_property prop,
			    union charger_propval *val) {
    int ret = 0;
    struct mt5706_charger_data *charger = dev_get_drvdata(&chg_dev->dev);

    switch ((int)prop) {
	case CHARGER_PROP_WLS_CHG_ONLINE:
		val->intval = charger->rx_ready;
		break;
    case CHARGER_PROP_WLS_CHG_PWR:
		val->intval = charger->rx_neg_power;
        break;
    case CHARGER_PROP_WLS_CHG_TYPE:
		val->intval = charger->rx_neg_type;
        break;
    case CHARGER_PROP_WLS_MODE:
		if (charger->rx_ready)
			val->intval = WLS_WORK_MODE_RX;
		else if(charger->revere_mode)
			val->intval = WLS_WORK_MODE_TX;
		else
			val->intval = WLS_WORK_MODE_NONE;
        break;
//drv add wanwen,pd-vbus compatible wireless charging status 20250715 start
	case CHARGER_PROP_WLS_PD_VBUS_MODE:
		if ((charger->pd_vbus || charger->rx_ready) && (charger->revere_mode==0))
			val->intval = WLS_WORK_PD_VBUS;
		else
			val->intval = WLS_WORK_MODE_NONE;
		pr_err("pd_vbus,charger->revere_mode is %d,%d,%d\n",charger->pd_vbus, charger->rx_ready, charger->revere_mode);
	break;
//drv add wanwen,pd-vbus compatible wireless charging status 20250715 end
	case CHARGER_PROP_WLS_MAX_CURR_LIMIT:
		val->intval = charger->input_current;
		break;
    default:
        return -EINVAL;
    }
	return ret;
}

static int mt5706_set_property(struct charger_device *chg_dev,
				enum charger_property prop,
				union charger_propval *val)
{
	int ret = 0;
	struct mt5706_charger_data *charger = dev_get_drvdata(&chg_dev->dev);

	pr_err("mt5706_set_property is %d,%d\n", (int)prop,val->intval);
	switch ((int)prop) {
	case CHARGER_PROP_WLS_TX_ENABLE:
		if(val->intval==1) {
			pr_err("WLS_TX_ENABLE: enable tx\n");
			ret = mt5706_tx_enter_tx(charger);
			if (!ret)
				charger->revere_mode=1;
			charger->rx_ready = false;
		} else {
			pr_err("WLS_TX_ENABLE: disable_tx\n");
			ret = mt5706_tx_stop_tx(charger);
			charger->revere_mode=0;
			charger->recv_rx_status = 0;
		}
		break;
	case CHARGER_PROP_WLS_RX_ENABLE:
//drv add wanwen, simulation earphone plug_out 20250716 start
		if(val->intval==1) {
			pr_err("WLS_RX_ENABLE: enable rx\n");
			charger->otg_enable = 0;//drv add wanwen, simulation earphone plug_out 20250716
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 0);
		} else if (val->intval==2) {
			pr_err("audio plug_in,WLS_RX_ENABLE: disable_rx\n");
			charger->otg_enable = 1;
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 1);
//drv add wanwen, simulation earphone plug_in 20250716
		} else {
			pr_err("WLS_RX_ENABLE: disable_rx\n");
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, 1);
			charger->rx_ready = false;
			charger->rx_neg_type = WLS_CHARGER_TYPE_UNKNOWN;
			power_supply_changed(charger->psy_chg);
		}
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int mt5706_set_otg(struct charger_device *chg_dev, bool enable) {
    struct mt5706_charger_data *charger = dev_get_drvdata(&chg_dev->dev);

	if (charger->revere_mode==1) {
		pr_err("revere_mode is on & stop tx mode \n");
		mt5706_tx_stop_tx(charger);
		charger->revere_mode=0;
		charger->recv_rx_status = 0;
	}

	if (enable) {
		if (charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio < 0)
			pr_err("chip en gpio is not good \n");
		else
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, !!enable);
		msleep(20);
		if (charger->pdata->gpios[MT5706_GPIO_OTG_EN].gpio < 0)
			pr_err("otg en gpio is not good \n");
		else
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_OTG_EN].gpio, !!enable);
	} else {
		if (charger->pdata->gpios[MT5706_GPIO_OTG_EN].gpio < 0)
			pr_err("otg en gpio is not good \n");
		else
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_OTG_EN].gpio, !!enable);
		msleep(5);
		if (charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio < 0)
			pr_err("chip en gpio is not good \n");
		else
			gpio_direction_output(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio, !!enable);
	}

	charger->otg_enable = !!enable;
	//pr_err("chip_en:%d otg_en:%d \n",gpio_get_value(charger->pdata->gpios[MT5706_GPIO_CHIP_EN].gpio),
	//	gpio_get_value(charger->pdata->gpios[MT5706_GPIO_OTG_EN].gpio));

    return 0;
}


static struct charger_ops mt5706_chg_ops = {
    /* Normal charging */
    .plug_in = NULL,
    .plug_out = mt5706_plug_out,
    .enable = NULL,
    .is_enabled = mt5706_is_enabled,
    .get_input_current = NULL,
    .set_input_current = NULL,
    .kick_wdt = NULL,
    .dump_registers = NULL,

    /* OTG */
    .enable_otg = mt5706_set_otg,

    /* ADC */
    .get_adc = NULL,
    .get_vbus_adc = NULL,

    .event = NULL,

	/* other */
	.get_property = mt5706_get_property,
	.set_property = mt5706_set_property,
};

static const struct charger_properties mt5706_chg_props = {
    .alias_name = "mt5706_chg",
};
#if defined (CONFIG_PRIZE_REVERE_CHARGING_MODE)
static ssize_t gettx_flag_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	struct mt5706_charger_data *charger = dev_get_drvdata(dev);
    return sprintf(buf, "%d", charger->recv_rx_status);
}
static DEVICE_ATTR(gettxflag, 0644, gettx_flag_show, NULL);

static ssize_t enable_tx_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	struct mt5706_charger_data *charger = dev_get_drvdata(dev);
    return sprintf(buf, "%d", charger->revere_mode);
}

static ssize_t enable_tx_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count)
{
    int error;
    unsigned int temp;
    uint16_t ping_code;
    struct mt5706_charger_data *charger = dev_get_drvdata(dev);

    error = kstrtouint(buf, 10, &temp);
    printk("enable_tx_store temp=%d\n",temp);

    if (error) {
		return error;
	}

    if(temp==1) {
		printk(KERN_INFO"mt5706 enable tx start\n");
		if (charger->otg_enable){
			printk(KERN_INFO"mt5706 otg is already enable,cannot enable revere chg\n");
			return count;
		}
		error = mt5706_tx_enter_tx(charger);
		if (!error)
			charger->revere_mode=1;
		mt5706_reg_u16_write(charger->rm, MT5706_TX_RX_PING_DUTY, 256);

		mt5706_reg_u16_read(charger->rm, MT5706_TX_RX_PING_DUTY, &ping_code);
		pr_emerg("%s: TX_RX_PING_DUTY:%x\n", __func__, ping_code);

		charger->rx_ready = false;
    } else {
		printk(KERN_INFO"mt5706 disable_tx start\n");
		mt5706_tx_stop_tx(charger);
		charger->revere_mode=0;
		charger->recv_rx_status = 0;
	}
    return count;
}
static DEVICE_ATTR(enabletx, 0664, enable_tx_show, enable_tx_store);
#endif /* CONFIG_PRIZE_REVERE_CHARGING_MODE */

//drv add wanwen,add firmware version info 20250807 start
static ssize_t mt5706_fw_version_show(struct device *dev, struct device_attribute *attr,char *buf)
{
    struct mt5706_charger_data *charger = dev_get_drvdata(dev);
    uint16_t high_version,low_version;

    mt5706_reg_u16_read(charger->rm, MT5706_FIRMWARE_HIGH, &high_version);
    mt5706_reg_u16_read(charger->rm, MT5706_FIRMWARE_LOW, &low_version);

    pr_err("fw version is 0x%x, 0x%x\n", high_version, low_version);
    return sprintf(buf, "0x%x%x\n", high_version,low_version);
}
static DEVICE_ATTR(fw_version, 0644, mt5706_fw_version_show, NULL);
//drv add wanwen,add firmware version info 20250807 end

static ssize_t wireless_connect_show(struct device *dev, struct device_attribute *attr,char *buf)
{
	struct mt5706_charger_data *charger = dev_get_drvdata(dev);
	if(charger->rx_ready)
	{
		return sprintf(buf, "%d", 1);
	}else{
	    return sprintf(buf, "%d", 0);
	}
}
static DEVICE_ATTR(wireless_connect, 0444, wireless_connect_show, NULL);
// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end

static irqreturn_t mt5706_pg_irq_handler(int irq, void *data) {
    struct mt5706_charger_data *charger = (struct mt5706_charger_data *)data;
    struct mt5706_gpio *gpio = NULL;
    int ret = 0;
    gpio = &charger->pdata->gpios[MT5706_GPIO_PG_INT];
    pr_err("pg status:%d \n", gpio_get_value(gpio->gpio));

    if (gpio_get_value(gpio->gpio)) {
//        chg_psy = power_supply_get_by_name("primary_chg");
			pr_emerg("%s: update mtk charge ic \n", __func__);
			charger->rx_ready = false;
			power_supply_changed(charger->psy_chg);
			if (ret < 0)
				pr_err("set chg online fail\n");
    }

    return IRQ_HANDLED;
}

static char *mt5706_psy_supplied_to[] = {
    "battery",
    "mtk-master-charger",
};

static int mt5706_charger_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct mt5706_charger_data *charger;
    mt5706_charger_platform_data_t *pdata = client->dev.platform_data;
    struct power_supply_config wpc_cfg = {
    	.of_node = client->dev.of_node,
        .supplied_to = mt5706_psy_supplied_to,
        .num_supplicants = ARRAY_SIZE(mt5706_psy_supplied_to),
    };

    int ret = 0;
	struct regmap *rm = NULL;
    struct mt5706_gpio *gpio = NULL;
    pr_emerg("%s: mt5706 Charger Driver Loading\n", __func__);

    pdata = devm_kzalloc(
            &client->dev, 
            sizeof(mt5706_charger_platform_data_t), 
            GFP_KERNEL);
    if (!pdata) {
        return -ENOMEM;
    }
    ret = mt5706_parse_dt(&client->dev, pdata);
    if (ret < 0) {
        return ret;
    }

    charger = devm_kzalloc(&client->dev, sizeof(*charger), GFP_KERNEL);
    if (charger == NULL) {
        ret = -ENOMEM;
        goto err_wpc_nomem;
    }
	dev_err(charger->dev, "mt5706 probe 1\n");
    charger->dev = &client->dev;
    charger->client = client;
    charger->pdata = pdata;
	dev_err(charger->dev, "mt5706 probe 2\n");
    ret = i2c_check_functionality(
            client->adapter, I2C_FUNC_SMBUS_BYTE_DATA | 
            I2C_FUNC_SMBUS_WORD_DATA | I2C_FUNC_SMBUS_I2C_BLOCK);
    if (!ret) {
        ret = i2c_get_functionality(client->adapter);
        dev_err(charger->dev, "I2C functionality is not supported.\n");
        ret = -ENOSYS;
        goto err_i2cfunc_not_support;
    }

    i2c_set_clientdata(client, charger);

    mutex_init(&charger->charger_lock);
    mutex_init(&charger->gpio_lock);
	dev_err(charger->dev, "mt5706 probe 3\n");
    rm = devm_regmap_init_i2c(client, &rm_cfg);
    if (IS_ERR(rm)) {
        ret = PTR_ERR(rm);
        goto err_regmap;
    }
    charger->rm = rm;
    charger->fw = sb_wrl_fw_register(client, rm, mt5706_fw_result_cb);
	if(!(charger->fw)){
		dev_err(&client->dev, "%s: failed to sb_wrl_fw_register\n", __func__);
	}

    charger->wqueue = create_singlethread_workqueue("mt5706_workqueue");
    if (!charger->wqueue) {
        dev_err(&client->dev, "%s: failed to create wqueue\n", __func__);
        ret = -ENOMEM;
        goto err_wqueue;
    }

    charger->check_fw_ws = 
        wakeup_source_register(charger->dev, "mt5706-check_fw");
    INIT_DELAYED_WORK(&charger->check_fw_work, mt5706_check_fw_work);
	dev_err(charger->dev, "mt5706 probe 4\n");
    /*申请但是不打开中断，以防Probe还没执行完进入中断*/
    ret = mt5706_request_irq(charger,
        MT5706_GPIO_INT, mt5706_irq_thread,
        IRQF_TRIGGER_FALLING | IRQF_ONESHOT | IRQF_NO_AUTOEN);
    if (ret) {
        goto err_supply_unreg;
    }
    /*是否需要禁用唤醒？*/
    mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, MAXIC_IRQ_DISWAKE);

    // drv add tankaikun start
    gpio = &charger->pdata->gpios[MT5706_GPIO_PG_INT];
    ret = devm_request_threaded_irq(charger->dev, gpio->irq, NULL,
                mt5706_pg_irq_handler,
                IRQF_TRIGGER_RISING | IRQF_ONESHOT,
                "pg_irq", charger);
    if (ret < 0) {
        dev_err(charger->dev, "request pg_irq failed:%d\n", ret);
        goto err_supply_unreg;
    } else {
        dev_err(charger->dev, "request pg_irq pass:%d  gpio->irq =%d\n", ret, gpio->irq);
    }
    enable_irq_wake(gpio->irq);
    // drv add tankaikun end

	dev_err(charger->dev, "mt5706 probe 5\n");
    wpc_cfg.drv_data = charger;

    charger->psy_chg = power_supply_register(&client->dev,
        &wpc_power_supply_desc, &wpc_cfg);
    if (IS_ERR(charger->psy_chg)) {
        goto err_supply_unreg;
    }

    ret = sec_wpc_create_attrs(&charger->psy_chg->dev);
    if (ret) {
        dev_err(&client->dev,
            "%s: Failed to Register psy_chg\n", __func__);
        goto err_pdata_free;
    }

    mt5706_startup(charger);

    dev_info(&client->dev,
        "%s: mt5706 Charger Driver Loaded\n", __func__);

    device_init_wakeup(charger->dev, 1);
	dev_err(charger->dev, "mt5706 probe 6\n");

	// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
    charger->vout = 5000;
    /* Register charger device */
    charger->chg_dev = charger_device_register("wireless_chg",
                                        charger->dev, charger, &mt5706_chg_ops,
                                        &mt5706_chg_props);
    if (IS_ERR_OR_NULL(charger->chg_dev)) {
        ret = PTR_ERR(charger->chg_dev);
        dev_notice(charger->dev, "%s register chg dev fail(%d)\n", __func__, ret);
        goto err_register_chg_dev;
    }
	// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end

	// drv add tankaikun, apply mt5706 factory test, 20250409 start

	ret = sysfs_create_link(kernel_kobj,&client->dev.kobj,"wirelessrx");
	if (ret){
		pr_err(KERN_ERR"mt5706 sysfs_create_link fail\n");
	}

	#if defined (CONFIG_PRIZE_REVERE_CHARGING_MODE)
	ret = device_create_file(&client->dev, &dev_attr_gettxflag);
	if (ret){
		pr_err(KERN_ERR"mt5706 failed device_create_file(dev_attr_gettxflag)\n");
	}

	ret = device_create_file(&client->dev, &dev_attr_enabletx);
	if (ret){
		pr_err(KERN_ERR"mt5706 failed device_create_file(dev_attr_enabletx)\n");
	}
	#endif /* CONFIG_PRIZE_REVERE_CHARGING_MODE */

//drv add wanwen,add firmware version info 20250807 start
	ret = device_create_file(&client->dev, &dev_attr_fw_version);
	if (ret){
		pr_err(KERN_ERR"mt5706 failed device_create_file(dev_attr_fw_version)\n");
	}
//drv add wanwen,add firmware version info 20250807 end
	ret = device_create_file(&client->dev, &dev_attr_wireless_connect);
	if (ret){
		pr_err(KERN_ERR"mt5706 failed device_create_file(dev_attr_wireless_connect)\n");
	}
	// drv add tankaikun, apply mt5706 factory tesy, 20250409 end

    return 0;

// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 start
err_register_chg_dev:
// drv add tankaikun, apply mt5706 to mtk charger class, 20250409 end
err_pdata_free:
    power_supply_unregister(charger->psy_chg);
err_supply_unreg:
    wakeup_source_unregister(charger->check_fw_ws);
err_regmap:
err_wqueue:
    mutex_destroy(&charger->gpio_lock);
    mutex_destroy(&charger->charger_lock);
err_i2cfunc_not_support:
    kfree(charger);
err_wpc_nomem:
    devm_kfree(&client->dev, pdata);
    return ret;
}

#if IS_ENABLED(CONFIG_PM)
static int mt5706_charger_suspend(struct device *dev)
{
    struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
    struct mt5706_charger_data *charger = i2c_get_clientdata(i2c);
    unsigned int wake_irq_flag = MAXIC_IRQ_DISNOSYNC;

    pr_emerg("%s\n", __func__);

    if (device_may_wakeup(charger->dev))
        wake_irq_flag |= MAXIC_IRQ_ENWAKE;

    mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, wake_irq_flag);
    return 0;
}

static int mt5706_charger_resume(struct device *dev)
{
    struct i2c_client *i2c = container_of(dev, struct i2c_client, dev);
    struct mt5706_charger_data *charger = i2c_get_clientdata(i2c);
    unsigned int wake_irq_flag = MAXIC_IRQ_EN;

    //pr_emerg("%s: charge_mode = %d, charger->pdata->otp_firmware_ver = %x\n",
//        __func__, charger->charge_mode, charger->pdata->otp_firmware_ver);

    if (device_may_wakeup(charger->dev))
        wake_irq_flag |= MAXIC_IRQ_DISWAKE;

    mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, wake_irq_flag);
    return 0;
}
#else
#define mt5706_charger_suspend NULL
#define mt5706_charger_resume NULL
#endif

static void mt5706_charger_shutdown(struct i2c_client *client)
{
    struct mt5706_charger_data *charger = i2c_get_clientdata(client);

    pr_emerg("%s\n", __func__);

    //if (!mt5706_is_on_pad(charger) ||
    //    !gpio_is_valid(charger->pdata->wpc_en))
    //    return;

    /* disable irqs */
    mt5706_set_gpio_irq(charger, MT5706_GPIO_INT, MAXIC_IRQ_DISNOSYNC);

    //msleep(PHM_DETACH_DELAY);
}

static const struct i2c_device_id mt5706_charger_id[] = {
    {"mt5706-charger", 0},
    {}
};

MODULE_DEVICE_TABLE(i2c, mt5706_charger_id);

static const struct of_device_id mt5706_i2c_match_table[] = {
    { .compatible = "maxictech,mt5706"},
    {},
};

static const struct dev_pm_ops mt5706_charger_pm = {
    .suspend = mt5706_charger_suspend,
    .resume = mt5706_charger_resume,
};

static struct i2c_driver mt5706_charger_driver = {
    .driver = {
        .name    = "mt5706-charger",
        .owner    = THIS_MODULE,
#if IS_ENABLED(CONFIG_PM)
        .pm    = &mt5706_charger_pm,
#endif /* CONFIG_PM */
        .of_match_table = mt5706_i2c_match_table,
    },
    .shutdown    = mt5706_charger_shutdown,
    .probe    = mt5706_charger_probe,
    //.remove    = mt5706_charger_remove,
    .id_table    = mt5706_charger_id,
};

static int __init mt5706_charger_init(void)
{
    pr_emerg("%s\n", __func__);
    return i2c_add_driver(&mt5706_charger_driver);
}

static void __exit mt5706_charger_exit(void)
{
    pr_emerg("%s\n", __func__);
    i2c_del_driver(&mt5706_charger_driver);
}

module_init(mt5706_charger_init);
module_exit(mt5706_charger_exit);

MODULE_DESCRIPTION("MT5706 Charger Driver");
MODULE_AUTHOR("Maxic");
MODULE_LICENSE("GPL");

