#ifndef __MT5706_CORE_H
#define __MT5706_CORE_H __FILE__

#include <linux/i2c.h>
#include <linux/types.h>

//#define BIT(offset) (1 << (offset))

#define MT5706_CHIP_ID (0x5706)
#define MT5706_HW_CHIP_ID_REG (0x5A50)
#define MT5706_SW_CHIP_ID_REG (0x0000)
#define MT5706_INT_INTEN_REG (0x001C)
#define MT5706_INT_FLAG_REG (0x0020)
#define MT5706_INT_CLEAR_REG (0x0024)
#define MT5706_SYS_STATE_REG (0x0018)
#define MT5706_CMD_REG (0x002C)

#define MT5706_SBY_INT_READY BIT(0)
#define MT5706_TX_VIN_OVP_REG (0x014A)

#define MT5706_CMD0_ENTER_TX BIT(1)
#define MT5706_CMD1_CLEAR_INT BIT(0)

#define MT5706_WORK_MODE_SBY 0x01
#define MT5706_WORK_MODE_RX 0x02
#define MT5706_WORK_MODE_TX 0x03

#define MT5706_RX_CTRL_REG (0x0028)
#define MT5706_RX_PT_FREQ_REG (0x0052)
#define MT5706_RX_IOUT_REG (0x0054)
#define MT5706_RX_VOUT_REG (0x0056)
#define MT5706_RX_VRECT_REG (0x0058)
#define MT5706_RX_TEMP_REG (0x005A)
#define MT5706_RX_SS_CODE_REG (0x0072)
#define MT5706_RX_VOUT_TARGET_REG (0x0076)
#define MT5706_RX_MLDO_OFF_REASON_REG (0x00A8)
#define MT5706_RX_SET_VOUT_REG (0x00BE)
#define MT5706_RX_PPP_FSK_REG (0x00E1)
#define MT5706_RX_NEG_POWER_REG (0x016E)

#define MT5706_RX_NEG_POWER_TYPE_BPP 0x01
#define MT5706_RX_NEG_POWER_TYPE_1P2_EPP 0x02
#define MT5706_RX_NEG_POWER_TYPE_1P3_EPP 0x03

#define MT5706_RX_CMD1_SET_VOUT BIT(2)

//drv add wanwen,add firmware version info 20250807 start
#define MT5706_FIRMWARE_HIGH 0x0008
#define MT5706_FIRMWARE_LOW 0x000C
//drv add wanwen,add firmware version info 20250807 end

typedef enum {
    MT5706_RX_INT_READY = BIT(1),

    MT5706_RX_INT_SS_READY = BIT(4),
    MT5706_RX_INT_LDO_ON = BIT(5),
    MT5706_RX_INT_LDO_OFF = BIT(6),

    MT5706_RX_INT_PPP_ASK_SEND = BIT(7),
    MT5706_RX_INT_PPP_FSK_RCV = BIT(8),
    MT5706_RX_INT_PPP_FSK_TO = BIT(9),

    MT5706_RX_INT_AC_MISSING = BIT(10),

    MT5706_RX_INT_FC_OK = BIT(11),
    MT5706_RX_INT_FC_FAILED = BIT(12),

    MT5706_RX_INT_SV_OK = BIT(13),
    MT5706_RX_INT_SV_FAILED = BIT(14),

    MT5706_RX_INT_LDO_OPP0 = BIT(15),
    MT5706_RX_INT_LDO_OPP1 = BIT(16),
    MT5706_RX_INT_LDO_OVP0 = BIT(17),
    MT5706_RX_INT_LDO_OVP1 = BIT(18),
    MT5706_RX_INT_LDO_OCP = BIT(19),
    MT5706_RX_INT_LDO_SCP = BIT(20),
    MT5706_RX_INT_LDO_OTP0 = BIT(21),
    MT5706_RX_INT_LDO_OTP1 = BIT(22),
    MT5706_RX_INT_VOUT_OVP = BIT(23),
    MT5706_RX_INT_BACK_FLOW = BIT(24),

    MT5706_RX_INT_WPC_NEG_OK = BIT(25),

    MT5706_RX_INT_TX_FSK_RCV = BIT(26),
    MT5706_RX_INT_READY_FOR_LOAD = BIT(30),
} MT5706_RX_INT_NAME;

#define MT5706_TX_PT_END_CODE_REG (0x0080)
#define MT5706_TX_ASK_PPP_REG (0x00A8)
#define MT5706_TX_RX_SS_REG (0x0070)
#define MT5706_TX_RX_PING_DUTY (0x0218)
#define MT5706_TX_RX_ID_VERSION_REG (0x02F5)
#define MT5706_TX_RX_ID_MANUFACTURE_REG (0x02F6)
#define MT5706_TX_RX_ID_BASIC_ID_REG (0x02F8)
#define MT5706_TX_RX_EPT_CODE_REG (0x02F4)

#define MT5706_TX_CMD1_START_WORKING BIT(1)
#define MT5706_TX_CMD1_STOP_WORKING BIT(2)

typedef enum {
    MT5706_TX_INT_READY = BIT(2),
    MT5706_TX_INT_ERROR_STAT = BIT(3), // drv add tankaikun, tx error
    MT5706_TX_INT_START_WORK = BIT(4),
    MT5706_TX_INT_STOP_WORK = BIT(5),

    MT5706_TX_INT_ASK_RECV = BIT(6),
    MT5706_TX_INT_RSV7 = BIT(7),

    MT5706_TX_INT_SS = BIT(8),
    MT5706_TX_INT_ID = BIT(9),
    MT5706_TX_INT_EXT_ID = BIT(10),
    MT5706_TX_INT_CONFIG = BIT(11),
    MT5706_TX_INT_NEG = BIT(12),
    MT5706_TX_INT_CAL = BIT(13),
    MT5706_TX_INT_PT = BIT(14),
    MT5706_TX_INT_RM_POWER = BIT(15),

    MT5706_TX_INT_RSV16 = BIT(16),
    MT5706_TX_INT_PKT_TIMEOUT = BIT(17),
    MT5706_TX_INT_RX_EPT = BIT(18),

    MT5706_TX_INT_PING_START = BIT(19),
    MT5706_TX_INT_PING_END = BIT(20),
    MT5706_TX_INT_RSV21 = BIT(21),
    MT5706_TX_INT_RSV22 = BIT(22),
    MT5706_TX_INT_RSV23 = BIT(23),

    MT5706_TX_INT_VIN_OVP = BIT(24),
    MT5706_TX_INT_VIN_UVP = BIT(25),
    MT5706_TX_INT_IIN_OCP = BIT(26),
    MT5706_TX_INT_OTP0 = BIT(27),
    MT5706_TX_INT_PING_CLASH = BIT(28),
    MT5706_TX_INT_BRG_OCP = BIT(29),
    MT5706_TX_INT_FOD = BIT(30),
} MT5706_TX_INT_NAME;

typedef enum {
    TX_PT_END_INVALID_SS_PKT = 1,
    TX_PT_END_INVALID_ID_PKT = 2,
    TX_PT_END_INVALID_EXT_ID_PKT = 3,
    TX_PT_END_INVALID_PCH_PARAM = 4,
    TX_PT_END_INVALID_CFG_CNT = 5,
    TX_PT_END_INVALID_CFG_PKT = 6,
    TX_PT_END_INVALID_PT_PKT = 7,
    TX_PT_END_RX_REQUEST = 8,
    TX_PT_END_TIMEOUT = 9,
    TX_PT_END_AP_CMD = 10,
    TX_PT_END_RSV11 = 11,
    TX_PT_END_UVP = 12,
    TX_PT_END_FOD = 13,
    TX_PT_END_BRG_OCP = 14,
    TX_PT_END_PING_CLASH = 15,
    TX_PT_END_OTP0 = 16,
    TX_PT_END_VIN_OVP = 17,
    TX_PT_END_RSV18 = 18,
    TX_PT_END_IIN_OCP = 19,
} MT5706_TX_PT_END_CODE;

/* i2c interface */
int mt5706_reg_write(
        struct regmap *rm, uint16_t reg, const uint8_t *buf, uint16_t len);
int mt5706_reg_read(
        struct regmap *rm, uint16_t reg, uint8_t *buf, uint16_t len);

int mt5706_reg_u8_write(struct regmap *rm, uint16_t reg, uint8_t value);
int mt5706_reg_u16_write(
        struct regmap *rm, uint16_t reg, uint16_t value);
int mt5706_reg_u32_write(
        struct regmap *rm, uint16_t reg, uint32_t value);

int mt5706_reg_u8_read(struct regmap *rm, uint16_t reg, uint8_t *value);
int mt5706_reg_u16_read(
        struct regmap *rm, uint16_t reg, uint16_t *value);
int mt5706_reg_u32_read(
        struct regmap *rm, uint16_t reg, uint32_t *value);

#endif
