#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/workqueue.h>
/*
#include <linux/battery/sb_def.h>
#include <linux/battery/sb_notify.h>
#include <linux/battery/common/sb_event.h>
*/
#include "mt5706_core.h"
#include "mt5706_firmware.h"
#include "mt5706_firmware_array.h"

#define fw_log(str, ...) pr_emerg("[MT5706-FW]:%s: "str, __func__, ##__VA_ARGS__)

#define FW_MODULE_NAME "mt5706-fw"

enum sb_wrl_fw_result {
        SB_WRL_FW_RESULT_INIT = -1,
        SB_WRL_FW_RESULT_FAIL,
        SB_WRL_FW_RESULT_PASS,
        SB_WRL_FW_RESULT_RUNNING
};

#define MTP_WRITE_ADDR 0x0000
#define SRAM_BOOT_ADDR 0x0000
#define BOOTADDR_INFO_CHIPID 0x1008
#define CHIP_ID 0x5706
#define MTP_SIZE ((32 * 1024) - 256) // MPT Size
#define SRAM_SIZE (6 * 1024) // SRAM Size
#define SDRAM_PAGE_SIZE 128  // max sram write size allowed by bootloader
#define MTP_BLOCK_SIZE 256   // MTP block size

enum boot_status
{
    PGM_STATUS_READY = 0,
    PGM_STATUS_BUFVALID = 0x01,
    PGM_STATUS_PROGOK = 0x02,
    PGM_STATUS_ERRCS = 0x04,
    PGM_STATUS_READMTP = 0x08,
    PGM_STATUS_WMTP = 0x10,
    PGM_STATUS_ERRPROG = 0x20,
    PGM_STATUS_VERIFY = 0x40,
    PGM_STATUS_VERIFYERR = 0x80,
    PGM_STATUS_VERIFYOK = 0x100,
    PGM_STATUS_ERRLENGTH = 0x400,
    PGM_STATUS_ERASE_MAIN_PROTECT = 0x8000,
};

enum boot_cmd_addr {
    PGM_OFFSET = 0x00001000,
    PGM_STATUS_ADDR_L = 0x0000 + PGM_OFFSET,
    PGM_ADDR_ADDR = 0x0002 + PGM_OFFSET,
    PGM_LENGTH_ADDR = 0x0004 + PGM_OFFSET,
    PGM_CHECKSUM_ADDR = 0x0006 + PGM_OFFSET,
    PGM_DATA_ADDR = 0x0008 + PGM_OFFSET,
};

int sram_write(
        struct regmap* rm, 
        unsigned int start_addr, const unsigned char* data, unsigned int len)
{
    unsigned int addr_offset = 0;
    unsigned int size = len;
    while (size > 0) {
        if (size >= SDRAM_PAGE_SIZE) {
            mt5706_reg_write(
                    rm, start_addr + addr_offset, 
                    data + addr_offset, SDRAM_PAGE_SIZE);
            size -= SDRAM_PAGE_SIZE;
            addr_offset += SDRAM_PAGE_SIZE;
        } else {
            mt5706_reg_write(
                    rm, start_addr + addr_offset, 
                    data + addr_offset, size);
            size = 0;
        }
    }
    return 0;
}

int sram_read(
        struct regmap* rm, 
        unsigned int start_addr, unsigned char* data, unsigned int len)
{
    unsigned int addr_offset = 0;
    unsigned int size = len;
    while (size > 0) {
        if (size >= SDRAM_PAGE_SIZE) {
            mt5706_reg_read(
                    rm, start_addr + addr_offset, 
                    data + addr_offset, SDRAM_PAGE_SIZE);
            size -= SDRAM_PAGE_SIZE;
            addr_offset += SDRAM_PAGE_SIZE;
        } else {
            mt5706_reg_read(
                    rm, start_addr + addr_offset, 
                    data + addr_offset, size);
            size = 0;
        }
    }
    return 0;
}

static int watchdog_disable(struct regmap* rm)
{
	uint8_t val0 = 1;
    uint8_t val1 = 1;
		
    for (int i = 0; i < 24; i++) {
        mt5706_reg_u8_write(rm, 0x5808, 0x95);
        mt5706_reg_u8_write(rm, 0x5800, 0x3);

        mt5706_reg_u8_read(rm, 0x5808, &val0);
        usleep_range(1000, 2000);
        mt5706_reg_u8_read(rm, 0x5808, &val1);
        if (val0 == 0x00 && val1 == 0x00) {
            return 0;
        }

        usleep_range(1000, 2000);
    }

    return -1;
}

int run_boot_loader(struct regmap* rm, const uint8_t *boot, uint16_t boot_size)
{
    int ret = 0;
	uint16_t chip_id = 0;
	uint8_t *sram_read_buf = NULL;
    if ((NULL == boot) || (0 == boot_size)) {
        fw_log("%s boot loader file error\n", __func__);
        return -1;
    }

    if (0 != watchdog_disable(rm)) {
        fw_log("%s watchdog disable fail\n", __func__);
        return -2;
    }

    if (0 != mt5706_reg_u8_write(rm, 0x5800, 0x07)) {
        fw_log("%s start firmware failed 0x5800 0x07\n", __func__);
        return -10;
    }

    if (0 != mt5706_reg_u8_write(rm, 0x5244, 0x57)) {
        fw_log("%s start firmware failed 0x5244\n", __func__);
        return -3;
    }
    if (0 != mt5706_reg_u8_write(rm, 0x5200, 0x20)) {
        fw_log("%s start firmware failed 0x5200\n", __func__);
        return -4;
    }
    if (0 != mt5706_reg_u8_write(rm, 0x5209, 1)) {
        fw_log("%s start firmware failed 0x5209\n", __func__);
        return -5;
    }
    msleep(50);
    sram_read_buf = kzalloc(boot_size, GFP_NOWAIT);
    if (NULL == sram_read_buf) {
        return -ENOMEM;
    }

    sram_write(rm, SRAM_BOOT_ADDR, boot, boot_size);
    msleep(50);
    mt5706_reg_u8_write(rm, 0x5200, 0x90);
    usleep_range(10000, 20000);

    sram_read(rm, SRAM_BOOT_ADDR, sram_read_buf, boot_size);
    for (int i = 0; i < boot_size; i++) {
        if (boot[i] != sram_read_buf[i]) {
            fw_log("%s write bootloader failed\n", __func__);
            ret = -6;
            goto free_read_buf;
        }
    }

    
    mt5706_reg_u16_read(rm, BOOTADDR_INFO_CHIPID, &chip_id);
    if (chip_id != CHIP_ID) {
        fw_log("%s chip_id = %x,run bootloader fail\n", __func__, chip_id);
        ret = -7;
        goto free_read_buf;
    } else {
        fw_log("%s run bootloader success\n", __func__);
    }

free_read_buf:
    kfree(sram_read_buf);
    return ret;
}

int boot_loader_erase_mtp_app(struct regmap* rm)
{
   

    unsigned int time = 0;
    uint16_t status = 0;
	
	mt5706_reg_u16_write(rm, PGM_STATUS_ADDR_L, PGM_STATUS_ERASE_MAIN_PROTECT);

    while (1) {
        mt5706_reg_u16_read(rm, PGM_STATUS_ADDR_L, &status);
        if (PGM_STATUS_PROGOK == status) {
            fw_log("%s erase mtp app ok\n", __func__);
            return 0;
        }

        time += 1;
        if (time > 50) {
            fw_log("%s erase mtp app timeout\n", __func__);
            return -1;
        }
        usleep_range(10000, 11000);
    }

    return -2;
}

int boot_loader_write_mtp_app(
        struct regmap* rm, 
        const uint8_t *mtp_app, uint16_t mtp_app_size)
{
    unsigned int data_size = mtp_app_size;
    unsigned int addr_offset = 0;
    unsigned int write_size = 0;
    unsigned int _addr = 0;
    unsigned int _check_sum = 0;
    unsigned int _length = 0;
    unsigned int time = 0;
    uint16_t status = 0;
	uint32_t process_bar = 0;
    unsigned int write_retrycnt = 8;
	
    while (data_size > 0) {
        if (data_size > MTP_BLOCK_SIZE) {
            _length = MTP_BLOCK_SIZE;
            write_size = MTP_BLOCK_SIZE;
        } else {
            _length = data_size;
            write_size = data_size;
        }
        _addr = addr_offset;
        _check_sum = _addr;
        for (int i = 0; i < _length; i++) {
            _check_sum += mtp_app[addr_offset + i];
        }
        _check_sum += _length;

        if (0 != mt5706_reg_u16_write(
                    rm, PGM_STATUS_ADDR_L, PGM_STATUS_READY)) {
            fw_log("%s clear cmd failed\n", __func__);
            return -1;
        }
        if (0 != mt5706_reg_u16_write(rm, PGM_ADDR_ADDR, _addr)) {
            fw_log("%s write addr failed\n", __func__);
            return -2;
        }
        if (0 != mt5706_reg_u16_write(rm, PGM_LENGTH_ADDR, _length)) {
            fw_log("%s write length failed\n", __func__);
            return -3;
        }
        if (0 != mt5706_reg_u16_write(rm, PGM_CHECKSUM_ADDR, _check_sum)) {
            fw_log("%s write cs failed\n", __func__);
            return -4;
        }
        if (0 != sram_write(
                    rm, PGM_DATA_ADDR, mtp_app + addr_offset, write_size)) {
            fw_log("%s write data failed\n", __func__);
            return -5;
        }
        if (0 != mt5706_reg_u16_write(
                    rm, PGM_STATUS_ADDR_L, PGM_STATUS_BUFVALID)) {
            fw_log("%s write data failed\n", __func__);
            return -6;
        }


        while(1) {
            mt5706_reg_u16_read(rm, PGM_STATUS_ADDR_L, &status);
            if (status == PGM_STATUS_PROGOK) {
                break;
            }
            time += 1;
            if (time > 50) {
				fw_log("read PGM_STATUS_ADDR_L not ok \n");
                break;
            }
            usleep_range(10000, 11000);
			//msleep(10);
        }

        if (status == PGM_STATUS_PROGOK) {
            data_size -= write_size;
            addr_offset += write_size;
            process_bar = addr_offset * 100 / mtp_app_size;
			time = 0;
            fw_log("[##########]%d\n", process_bar);
        } else if (status == PGM_STATUS_BUFVALID) {
            if (write_retrycnt > 0) {
                write_retrycnt--;
				time = 0;
                fw_log("%s write mtp timeout\n", __func__);
                continue;
            } else {
                fw_log("%s write mtp timeout\n", __func__);
                return -7;
            }
        } else if (status == PGM_STATUS_ERRCS) {
            if (write_retrycnt > 0) {
                write_retrycnt--;
				time = 0;
                fw_log("%s write mtp check sum error\n", __func__);
                continue;
            } else {
                fw_log("%s write mtp check sum error\n", __func__);
                return -8;
            }
        } else if (status == PGM_STATUS_ERRPROG) {
            if (write_retrycnt > 0) {
                write_retrycnt--;
				time = 0;
                fw_log("%s addr and length over limit error\n", __func__);
                continue;
            } else {
                fw_log("%s addr and length over limit error\n", __func__);
                return -9;
            }
        } else if (status == PGM_STATUS_ERRLENGTH) {
            if (write_retrycnt > 0) {
                write_retrycnt--;
				time = 0;
                fw_log("%s over 512 bytes in length\n", __func__);
                continue;
            } else {
                fw_log("%s over 512 bytes in length\n", __func__);
                return -10;
            }
        } else {
            if (write_retrycnt > 0) {
                write_retrycnt--;
				time = 0;
                fw_log("%s unknown error status:0x%02X\n", __func__, status);
                continue;
            } else {
                fw_log("%s unknown error status:0x%02X\n", __func__, status);
                return -11;
            }
        }
    }

    return 0;
}

int boot_loader_verify_mtp_app(
        struct regmap* rm, 
        uint16_t app_size, uint16_t app_crc)
{
	unsigned int time = 0;
    uint16_t status = 0;
	
    if (0 != mt5706_reg_u16_write(rm, PGM_ADDR_ADDR, 0x0000)) {
        fw_log("%s write addr failed\n", __func__);
        return -1;
    }
    if (0 != mt5706_reg_u16_write(rm, PGM_LENGTH_ADDR, app_size)) {
        fw_log("%s write length failed\n", __func__);
        return -2;
    }
    if (0 != mt5706_reg_u16_write(rm, PGM_CHECKSUM_ADDR, app_crc)) {
        fw_log("%s write cs failed\n", __func__);
        return -3;
    }
    if (0 != mt5706_reg_u16_write(rm, PGM_STATUS_ADDR_L, PGM_STATUS_VERIFY)) {
        fw_log("%s write cmd failed\n", __func__);
        return -4;
    }

    while(1) {
        usleep_range(10000, 11000);
        mt5706_reg_u16_read(rm, PGM_STATUS_ADDR_L, &status);
        if (PGM_STATUS_VERIFYOK == status) {
            return 0;
        }
        if (PGM_STATUS_VERIFYERR == status) {
            fw_log("bootloader_verify_mtp verify error\n");
            return -5;
        }
        time += 1;
        if (time > 10) {
            fw_log("bootloader_verify_mtp timeout\n");
            return -6;
        }
    }

    return -3;
}

int update_firmware(struct regmap* rm, 
        const uint8_t *boot, uint16_t boot_size, 
        const uint8_t *mtp_app, uint16_t mtp_app_size, 
        uint32_t mtp_app_version, uint16_t mtp_app_crc)
{
    if (0 != run_boot_loader(rm, boot, boot_size)) {
        fw_log("run_bootloader fail\n");
        return SB_WRL_FW_RESULT_FAIL;
    }

    if (0 != boot_loader_verify_mtp_app(rm, mtp_app_size, mtp_app_crc)) {
        if (boot_loader_erase_mtp_app(rm) != 0) {
            return SB_WRL_FW_RESULT_FAIL;
        }
        if (0 != boot_loader_write_mtp_app(rm, mtp_app, mtp_app_size)) {
            fw_log("bootloader_write_mtp fail\n");
            return SB_WRL_FW_RESULT_FAIL;
        } else {
            fw_log("bootloader_write_mtp succeed\n");
        }

        if (0 != boot_loader_verify_mtp_app(rm, mtp_app_size, mtp_app_crc)) {
            fw_log("bootloader_verify_mtp fail\n");
            return SB_WRL_FW_RESULT_FAIL;
        } else {
            fw_log("%s firmware update to V:%x,Tag:%x%x\n", \
                    __func__, mtp_app_version, \
                    *((uint32_t *)(&(mtp_app[0x1c]))), \
                    *((uint32_t *)(&(mtp_app[0x20]))));
            return SB_WRL_FW_RESULT_PASS;
        }
    } else {
        fw_log("%s firmware update to V:%x,Tag:%x%x\n", \
                __func__, mtp_app_version, \
                *((uint32_t *)(&(mtp_app[0x1c]))), \
                *((uint32_t *)(&(mtp_app[0x20]))));
        return SB_WRL_FW_RESULT_PASS;
    }

    return SB_WRL_FW_RESULT_FAIL;
}

static void cb_work(struct work_struct *work)
{
    
    struct sb_wrl_fw *fw = container_of(work, struct sb_wrl_fw, work.work);
    int state = SB_WRL_FW_RESULT_FAIL;
	
	fw_log("start\n");

    __pm_stay_awake(fw->ws);

    mutex_lock(&fw->mlock);
    fw->state = SB_WRL_FW_RESULT_RUNNING;
    mutex_unlock(&fw->mlock);

    state = update_firmware(
            fw->rm, 
            mt5706_boot_loader, 
            sizeof(mt5706_boot_loader), 
            mt5706_mtp_app, 
            sizeof(mt5706_mtp_app), 
            MT5706_MTP_APP_VERSION,
            MT5706_MTP_APP_CRC);

    fw_log("%s firmware update\n",
        (state == SB_WRL_FW_RESULT_PASS) ? "success" : "fail");

    if (fw->func) {
        fw->func(fw->parent, state);
    }

    mutex_lock(&fw->mlock);
    fw->state = state;
    mutex_unlock(&fw->mlock);

    __pm_relax(fw->ws);
}

struct sb_wrl_fw *sb_wrl_fw_register(
        struct i2c_client *i2c, struct regmap *rm, cb_fw_result func)
{
    struct sb_wrl_fw *fw = NULL;

    if (IS_ERR_OR_NULL(i2c)) {
        return NULL;
    }

    if (IS_ERR_OR_NULL(rm)) {
        return NULL;
    }

    fw = devm_kzalloc(&i2c->dev, sizeof(struct sb_wrl_fw), GFP_KERNEL);
    if (!fw) {
        return NULL;
    }

    fw->parent = i2c;
    fw->func = func;
    fw->rm = rm;
    fw->ws = wakeup_source_register(&i2c->dev, FW_MODULE_NAME);
    INIT_DELAYED_WORK(&fw->work, cb_work);
    mutex_init(&fw->mlock);

    fw->state = SB_WRL_FW_RESULT_INIT;

    return fw;
}

int mt5706_fw_update(struct sb_wrl_fw *fw)
{
    unsigned int work_state;

    if (IS_ERR_OR_NULL(fw)) {
        pr_emerg("%s:fw error\n", __func__);
        return -EINVAL;
    }

    work_state = work_busy(&fw->work.work);
    fw_log("work_state = 0x%x, fw_state = %d\n", work_state, fw->state);
    if (work_state & (WORK_BUSY_PENDING | WORK_BUSY_RUNNING)) {
        pr_emerg("%s:work busy\n", __func__);
        return -EBUSY;
    }

    mutex_lock(&fw->mlock);
    fw->state = SB_WRL_FW_RESULT_INIT;
    schedule_delayed_work(&fw->work, 0);
    mutex_unlock(&fw->mlock);

    return 0;
}
