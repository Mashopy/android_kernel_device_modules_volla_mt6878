// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/delay.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#endif
// prize add by lvyuanchuan for lcd hardware info 20220331 start
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
#include "../../../misc/mediatek/hardware_info/hardware_info.h"
extern struct hardware_info current_lcm_info;
#endif
// prize add by lvyuanchuan for lcd hardware info 20220331 end
// drv-Modify the problem of incorrect third-party size detection-pzp-start
#define PHYSICAL_WIDTH 69552
#define PHYSICAL_HEIGHT 154560
// drv-Modify the problem of incorrect third-party size detection-pzp-end
//add by huangxinglve, 20250826, add for esd brightness start
bool oled_esd_recovery = false;
EXPORT_SYMBOL(oled_esd_recovery);
//add by huangxinglve, 20250826, add for esd brightness end

// static char init_head_tb[] = {0xFE, 0x00};	//modify by shenwenbin for tuning backlight 20231108
static char bl_tb[] = { 0x51, 0x0D, 0xBB };
static char hbm_tb[] = { 0x51, 0x0F, 0xFE }; // drv modify hbm function
struct lcm {
    struct device* dev;
    struct drm_panel panel;
    struct backlight_device* backlight;
    struct gpio_desc* reset_gpio;
    struct gpio_desc* bias_pos;
    struct gpio_desc* bias_neg;
    struct gpio_desc* vldo18_gpio;
    bool prepared;
    bool enabled;

    /* drv modify hbm function start */
    bool hbm_en;
    bool hbm_wait;
    bool hbm_stat;
    bool doze_en; // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516
    /* drv modify hbm function end */

    int error;
};

static unsigned int last_level; // drv modify hbm function
static unsigned int rawlevel;
struct lcm* g_ctx;
/* drv modify ts suspend enter for fod reporting start */
typedef void (*pfunc)(void);
static pfunc ts_suspend_callback = NULL;
/* drv modify ts suspend enter for fod reporting end */

unsigned int hc5622_cmd_fhd_buf_thresh[14] = {
	896, 1792, 2688, 3584, 4480,
	5376, 6272, 6720, 7168, 7616,
	7744, 7872, 8000, 8064};
unsigned int hc5622_cmd_fhd_range_min_qp[15] = {0, 4, 5, 5, 7, 7, 7, 7, 7, 7,9, 9, 9, 13, 16};
unsigned int hc5622_cmd_fhd_range_max_qp[15] = {8, 8, 9, 10, 11, 11, 11, 12,13, 14, 14, 15, 15, 16, 17};
int hc5622_cmd_fhd_range_bpg_ofs[15] = {2, 0, 0, -2, -4, -6, -8, -8, -8, -10,-10, -12, -12, -12, -12};

#define lcm_dcs_write_seq(ctx, seq...)         \
    ({                                         \
        const u8 d[] = { seq };                \
        BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,   \
            "DCS sequence too big for stack"); \
        lcm_dcs_write(ctx, d, ARRAY_SIZE(d));  \
    })

#define lcm_dcs_write_seq_static(ctx, seq...) \
    ({                                        \
        static const u8 d[] = { seq };        \
        lcm_dcs_write(ctx, d, ARRAY_SIZE(d)); \
    })

static inline struct lcm* panel_to_lcm(struct drm_panel* panel)
{
    return container_of(panel, struct lcm, panel);
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct lcm* ctx, u8 cmd, void* data, size_t len)
{
    struct mipi_dsi_device* dsi = to_mipi_dsi_device(ctx->dev);
    ssize_t ret;

    if (ctx->error < 0)
        return 0;

    ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
    if (ret < 0) {
        dev_info(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret,
            cmd);
        ctx->error = ret;
    }

    return ret;
}

static void lcm_panel_get_data(struct lcm* ctx)
{
    u8 buffer[3] = { 0 };
    static int ret;

    pr_info("%s+\n", __func__);

    if (ret == 0) {
        ret = lcm_dcs_read(ctx, 0x0A, buffer, 1);
        pr_info("%s  0x%08x\n", __func__, buffer[0] | (buffer[1] << 8));
        dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
            ret, buffer[0] | (buffer[1] << 8));
    }
}
#endif

static void lcm_dcs_write(struct lcm* ctx, const void* data, size_t len)
{
    struct mipi_dsi_device* dsi = to_mipi_dsi_device(ctx->dev);
    ssize_t ret;
    char* addr;

    if (ctx->error < 0)
        return;

    addr = (char*)data;
    if ((int)*addr < 0xB0)
        ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
    else
        ret = mipi_dsi_generic_write(dsi, data, len);
    if (ret < 0) {
        dev_info(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
        ctx->error = ret;
    }
}
//add by huangxinglve, 20250826, add for esd brightness start
static void lcm_pannel_reconfig_blk(struct lcm *ctx)
{
	char bl_tb[] = {0x51,0x07,0xFF};
	unsigned int reg_level = rawlevel;
	printk("[%s][%d]esd recovery backlight bl_level:%d rawlevel:%d \n",__func__,__LINE__,reg_level, rawlevel);

	bl_tb[1] = (char)((reg_level >> 8) & 0xFF);
	bl_tb[2] = (char)(reg_level & 0xFF);
	lcm_dcs_write(ctx, bl_tb, ARRAY_SIZE(bl_tb));
}
//add by huangxinglve, 20250826, add for esd brightness end
static void lcm_panel_init(struct lcm* ctx)
{

    lcm_dcs_write_seq_static(ctx, 0x9C, 0xA5, 0xA5);
    lcm_dcs_write_seq_static(ctx, 0xFD, 0x5A, 0x5A);
    lcm_dcs_write_seq_static(ctx, 0x48, 0x00); // 切频 60hz-command
    lcm_dcs_write_seq_static(ctx, 0x53, 0xE0);
    lcm_dcs_write_seq_static(ctx, 0x35, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x11);
    msleep(150);
    lcm_dcs_write_seq_static(ctx, 0x9F, 0x0F);
    lcm_dcs_write_seq_static(ctx, 0xCE, 0x22);
    lcm_dcs_write_seq_static(ctx, 0x9F, 0x01);
    lcm_dcs_write_seq_static(ctx, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90);
    //lcm_dcs_write_seq_static(ctx, 0x51, 0x0D,0xBB);//mod by huangxinglve, 20250620, fix phone call resume with two brightness
    lcm_dcs_write_seq_static(ctx, 0x9F, 0x01);
    lcm_dcs_write_seq_static(ctx, 0xC7, 0x11, 0x00, 0x00, 0xAB, 0x30, 0x80, 0x09, 0x60, 0x04, 0x38, 0x00, 0x14, 0x02, 0x1C, 0x02, 0x1C, 0x02, 0x00, 0x02, 0x25, 0x00, 0x20, 0x01, 0xD5, 0x00, 0x07, 0x00, 0x0D, 0x05, 0x7A, 0x05, 0x16);
    lcm_dcs_write_seq_static(ctx, 0xC8, 0x18, 0x00, 0x10, 0xF0, 0x07, 0x10, 0x20, 0x00, 0x06, 0x0F, 0x0F, 0x33, 0x0E, 0x1C, 0x2A, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x02, 0x02, 0x22, 0x00, 0x2A, 0x40);
    lcm_dcs_write_seq_static(ctx, 0xC9, 0x2A, 0xBE, 0x3A, 0xFC, 0x3A, 0xFA, 0x3A, 0xF8, 0x3B, 0x38, 0x3B, 0x78, 0x3B, 0xB6, 0x4B, 0xB6, 0x4B, 0xF4, 0x4B, 0xF4, 0x6C, 0x34, 0x84, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    lcm_dcs_write_seq_static(ctx, 0x29);
    msleep(50);
//add by huangxinglve, 20250826, mod for resume high brightnees start
	if (oled_esd_recovery == true) {
		lcm_pannel_reconfig_blk(ctx);
		oled_esd_recovery = false;
	}
//add by huangxinglve, 20250826, mod for resume high brightnees end

    pr_info("%s-\n", __func__);
}

static int lcm_disable(struct drm_panel* panel)
{
    struct lcm* ctx = panel_to_lcm(panel);

    if (!ctx->enabled)
        return 0;

    if (ctx->backlight) {
        ctx->backlight->props.power = FB_BLANK_POWERDOWN;
        backlight_update_status(ctx->backlight);
    }

    ctx->enabled = false;

    return 0;
}

static int lcm_unprepare(struct drm_panel* panel)
{
    struct lcm* ctx = panel_to_lcm(panel);

    if (!ctx->prepared)
        return 0;
    lcm_dcs_write_seq_static(ctx, 0x28);
    msleep(120);
    lcm_dcs_write_seq_static(ctx, 0x10);
    msleep(50);

    // enter deep standby mode
    // lcm_dcs_write_seq_static(ctx, 0x4f, 0x01);
    // msleep(120);

    // lcd reset L
    ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
    gpiod_set_value(ctx->reset_gpio, 0);
    msleep(10);
    devm_gpiod_put(ctx->dev, ctx->reset_gpio);

    // LCM_VCI_EN -- AMOLED_VCI
    ctx->vldo18_gpio = devm_gpiod_get(ctx->dev, "vldo18", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->vldo18_gpio)) {
        dev_err(ctx->dev, "%s: cannot get vldo18 %ld\n",
            __func__, PTR_ERR(ctx->vldo18_gpio));
        return PTR_ERR(ctx->vldo18_gpio);
    }
    gpiod_set_value(ctx->vldo18_gpio, 0);
    devm_gpiod_put(ctx->dev, ctx->vldo18_gpio);
    msleep(5);

    // LCM_DVDD_EN -- AMOLED_DVDD
    ctx->bias_neg = devm_gpiod_get_index(ctx->dev, "bias", 1, GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bias_neg)) {
        dev_err(ctx->dev, "%s: cannot get bias_neg %ld\n",
            __func__, PTR_ERR(ctx->bias_neg));
        return PTR_ERR(ctx->bias_neg);
    }
    gpiod_set_value(ctx->bias_neg, 0);
    devm_gpiod_put(ctx->dev, ctx->bias_neg);
    msleep(10);

    // LCM_VDDI_EN -- AMOLED_VDDI
    ctx->bias_pos = devm_gpiod_get_index(ctx->dev, "bias", 0, GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bias_pos)) {
        dev_err(ctx->dev, "%s: cannot get bias_pos %ld\n",
            __func__, PTR_ERR(ctx->bias_pos));
        return PTR_ERR(ctx->bias_pos);
    }
    gpiod_set_value(ctx->bias_pos, 0);
    devm_gpiod_put(ctx->dev, ctx->bias_pos);
    msleep(5);
    ctx->hbm_en = false;
    ctx->doze_en = false; // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516
    /* drv modify hbm function end */
    ctx->error = 0;
    ctx->prepared = false;
    pr_info("%s-\n", __func__);
    return 0;
}

static int lcm_prepare(struct drm_panel* panel)
{
    struct lcm* ctx = panel_to_lcm(panel);
    int ret;

    pr_info("%s+\n", __func__);
    if (ctx->prepared)
        return 0;

    // LCM_VDDI_EN -- AMOLED_VDDI
    ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
    gpiod_set_value(ctx->reset_gpio, 0);
	msleep(5);

    ctx->bias_pos = devm_gpiod_get_index(ctx->dev, "bias", 0, GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bias_pos)) {
        dev_err(ctx->dev, "%s: cannot get bias_pos %ld\n",
            __func__, PTR_ERR(ctx->bias_pos));
        return PTR_ERR(ctx->bias_pos);
    }
    gpiod_set_value(ctx->bias_pos, 1);
    devm_gpiod_put(ctx->dev, ctx->bias_pos);
    msleep(5);

    // LCM_DVDD_EN -- AMOLED_DVDD
    ctx->bias_neg = devm_gpiod_get_index(ctx->dev, "bias", 1, GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bias_neg)) {
        dev_err(ctx->dev, "%s: cannot get bias_neg %ld\n",
            __func__, PTR_ERR(ctx->bias_neg));
        return PTR_ERR(ctx->bias_neg);
    }
    gpiod_set_value(ctx->bias_neg, 1);
    devm_gpiod_put(ctx->dev, ctx->bias_neg);
    msleep(10);

    // LCM_VCI_EN -- AMOLED_VCI
    ctx->vldo18_gpio = devm_gpiod_get(ctx->dev, "vldo18", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->vldo18_gpio)) {
        dev_err(ctx->dev, "%s: cannot get vldo18 %ld\n",
            __func__, PTR_ERR(ctx->vldo18_gpio));
        return PTR_ERR(ctx->vldo18_gpio);
    }
    gpiod_set_value(ctx->vldo18_gpio, 1);
    devm_gpiod_put(ctx->dev, ctx->vldo18_gpio);
    msleep(5);
    // lcd reset H -> L -> L

    msleep(10);
    gpiod_set_value(ctx->reset_gpio, 0);
    msleep(10);
    gpiod_set_value(ctx->reset_gpio, 1);
    msleep(50);
    devm_gpiod_put(ctx->dev, ctx->reset_gpio);
    // end
    lcm_panel_init(ctx);

    ret = ctx->error;
    if (ret < 0)
        lcm_unprepare(panel);

    ctx->prepared = true;
#ifdef PANEL_SUPPORT_READBACK
    lcm_panel_get_data(ctx);
#endif

    pr_info("%s-\n", __func__);
    return ret;
}

static int lcm_enable(struct drm_panel* panel)
{
    struct lcm* ctx = panel_to_lcm(panel);

    if (ctx->enabled)
        return 0;

    if (ctx->backlight) {
        ctx->backlight->props.power = FB_BLANK_UNBLANK;
        backlight_update_status(ctx->backlight);
    }

    ctx->enabled = true;

    return 0;
}

#define HFP (140)
#define HSA (4)
#define HBP (24)
#define HACT (1080)
#define VFP (2448)
#define VSA (4)
#define VBP (12)
#define VACT (2400)
#define VFP_90 (820)
#define VFP_120 (16)
static const struct drm_display_mode switch_mode_120hz = {
    .clock = ((HACT + HFP + HSA + HBP) * (VACT + VFP_120 + VSA + VBP) * (120) / 1000),
    .hdisplay = HACT,
    .hsync_start = HACT + HFP,
    .hsync_end = HACT + HFP + HSA,
    .htotal = HACT + HFP + HSA + HBP,
    .vdisplay = VACT,
    .vsync_start = VACT + VFP_120,
    .vsync_end = VACT + VFP_120 + VSA,
    .vtotal = VACT + VFP_120 + VSA + VBP,
};

static const struct drm_display_mode switch_mode_90hz = {
    .clock = ((HACT + HFP + HSA + HBP) * (VACT + VFP_90 + VSA + VBP) * (90) / 1000),
    .hdisplay = HACT,
    .hsync_start = HACT + HFP,
    .hsync_end = HACT + HFP + HSA,
    .htotal = HACT + HFP + HSA + HBP,
    .vdisplay = VACT,
    .vsync_start = VACT + VFP_90,
    .vsync_end = VACT + VFP_90 + VSA,
    .vtotal = VACT + VFP_90 + VSA + VBP,
};

static const struct drm_display_mode switch_mode_60hz = {
    .clock = ((HACT + HFP + HSA + HBP) * (VACT + VFP + VSA + VBP) * (60) / 1000),
    .hdisplay = HACT,
    .hsync_start = HACT + HFP,
    .hsync_end = HACT + HFP + HSA,
    .htotal = HACT + HFP + HSA + HBP,
    .vdisplay = VACT,
    .vsync_start = VACT + VFP,
    .vsync_end = VACT + VFP + VSA,
    .vtotal = VACT + VFP + VSA + VBP,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static struct mtk_panel_params ext_params_120hz = {
	.data_rate = 467*2,
	.pll_clk = 467,
	//drv-Modify the problem of incorrect third-party size detection-pzp-start
	.physical_width_um = PHYSICAL_WIDTH,
    .physical_height_um = PHYSICAL_HEIGHT,
    // drv-Modify the problem of incorrect third-party size detection-pzp-end
    // drv modify open esd-pengzhipeng20231219-start
    .cust_esd_check = 1,
    .esd_check_enable = 1,
    // drv modify open esd-pengzhipeng20231219-end
    .lcm_esd_check_table[0] = {
        .cmd = 0x0a,
        .count = 1,
        .para_list[0] = 0x9c,
    },

    .lp_perline_en = 1,
    .hbm_en_time = 0,
    .hbm_dis_time = 0,
    //+Mod by-drv huangxinglve,20250623,mod for HBM shinning start
    .target_time = 12233,//13233 14233 11233  15233:15 awful  12733
    .hbm_deadline = 12233,
    .vsync_time = 12233,
    //-Mod by-drv huangxinglve,20250623,mod for HBM shinning end
    .dsc_params = {
        .enable = 1,
        .ver = 17,
        .slice_mode = 1,
        .rgb_swap = 0,
        .dsc_cfg = 2088,
        .rct_on = 1,
        .bit_per_channel = 10,
        .dsc_line_buf_depth = 11,
        .bp_enable = 1,
        .bit_per_pixel = 128,
        .pic_height = 2400,
        .pic_width = 1080,
        .slice_height = 20,
        .slice_width = 540,
        .chunk_size = 540,
        .xmit_delay = 512,
        .dec_delay = 549,
        .scale_value = 32,
        .increment_interval = 469,
        .decrement_interval = 7,
        .line_bpg_offset = 13,
        .nfl_bpg_offset = 1402,
        .slice_bpg_offset = 1302,
        .initial_offset = 6144,
        .final_offset = 4336,
        .flatness_minqp = 7,
        .flatness_maxqp = 16,
        .rc_model_size = 8192,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit0 = 15,
        .rc_quant_incr_limit1 = 15,
        .rc_tgt_offset_hi = 3,
        .rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = hc5622_cmd_fhd_buf_thresh,
			.range_min_qp = hc5622_cmd_fhd_range_min_qp,
			.range_max_qp = hc5622_cmd_fhd_range_max_qp,
			.range_bpg_ofs = hc5622_cmd_fhd_range_bpg_ofs,
		},
/*        .rc_buf_thresh[0] = 14,
        .rc_buf_thresh[1] = 28,
        .rc_buf_thresh[2] = 42,
        .rc_buf_thresh[3] = 56,
        .rc_buf_thresh[4] = 70,
        .rc_buf_thresh[5] = 84,
        .rc_buf_thresh[6] = 98,
        .rc_buf_thresh[7] = 105,
        .rc_buf_thresh[8] = 112,
        .rc_buf_thresh[9] = 119,
        .rc_buf_thresh[10] = 121,
        .rc_buf_thresh[11] = 123,
        .rc_buf_thresh[12] = 125,
        .rc_buf_thresh[13] = 126,
        .rc_range_parameters[0].range_min_qp = 0,
        .rc_range_parameters[0].range_max_qp = 8,
        .rc_range_parameters[0].range_bpg_offset = 2,
        .rc_range_parameters[1].range_min_qp = 4,
        .rc_range_parameters[1].range_max_qp = 8,
        .rc_range_parameters[1].range_bpg_offset = 0,
        .rc_range_parameters[2].range_min_qp = 5,
        .rc_range_parameters[2].range_max_qp = 9,
        .rc_range_parameters[2].range_bpg_offset = 0,
        .rc_range_parameters[3].range_min_qp = 5,
        .rc_range_parameters[3].range_max_qp = 10,
        .rc_range_parameters[3].range_bpg_offset = -2,
        .rc_range_parameters[4].range_min_qp = 7,
        .rc_range_parameters[4].range_max_qp = 11,
        .rc_range_parameters[4].range_bpg_offset = -4,
        .rc_range_parameters[5].range_min_qp = 7,
        .rc_range_parameters[5].range_max_qp = 11,
        .rc_range_parameters[5].range_bpg_offset = -6,
        .rc_range_parameters[6].range_min_qp = 7,
        .rc_range_parameters[6].range_max_qp = 11,
        .rc_range_parameters[6].range_bpg_offset = -8,
        .rc_range_parameters[7].range_min_qp = 7,
        .rc_range_parameters[7].range_max_qp = 12,
        .rc_range_parameters[7].range_bpg_offset = -8,
        .rc_range_parameters[8].range_min_qp = 7,
        .rc_range_parameters[8].range_max_qp = 13,
        .rc_range_parameters[8].range_bpg_offset = -8,
        .rc_range_parameters[9].range_min_qp = 7,
        .rc_range_parameters[9].range_max_qp = 14,
        .rc_range_parameters[9].range_bpg_offset = -10,
        .rc_range_parameters[10].range_min_qp = 9,
        .rc_range_parameters[10].range_max_qp = 14,
        .rc_range_parameters[10].range_bpg_offset = -10,
        .rc_range_parameters[11].range_min_qp = 9,
        .rc_range_parameters[11].range_max_qp = 15,
        .rc_range_parameters[11].range_bpg_offset = -12,
        .rc_range_parameters[12].range_min_qp = 9,
        .rc_range_parameters[12].range_max_qp = 15,
        .rc_range_parameters[12].range_bpg_offset = -12,
        .rc_range_parameters[13].range_min_qp = 13,
        .rc_range_parameters[13].range_max_qp = 16,
        .rc_range_parameters[13].range_bpg_offset = -12,
        .rc_range_parameters[14].range_min_qp = 16,
        .rc_range_parameters[14].range_max_qp = 17,
        .rc_range_parameters[14].range_bpg_offset = -12,*/
    },
};

static struct mtk_panel_params ext_params_90hz = {
	.data_rate = 467*2,
	.pll_clk = 467,
	//drv-Modify the problem of incorrect third-party size detection-pzp-start
	.physical_width_um = PHYSICAL_WIDTH,
    .physical_height_um = PHYSICAL_HEIGHT,
    // drv-Modify the problem of incorrect third-party size detection-pzp-end
    // drv modify open esd-pengzhipeng20231219-start
    .cust_esd_check = 1,
    .esd_check_enable = 1,
    // drv modify open esd-pengzhipeng20231219-end
    .lcm_esd_check_table[0] = {
        .cmd = 0x0a,
        .count = 1,
        .para_list[0] = 0x9c,
    },

    .lp_perline_en = 1,
    .hbm_en_time = 0,
    .hbm_dis_time = 0,
    //+Mod by-drv huangxinglve,20250623,mod for HBM shinning start
    .target_time = 12233,
    .hbm_deadline = 12233,
    .vsync_time = 12233,
    //-Mod by-drv huangxinglve,20250623,mod for HBM shinning end
    .dsc_params = {
        .enable = 1,
        .ver = 17,
        .slice_mode = 1,
        .rgb_swap = 0,
        .dsc_cfg = 2088,
        .rct_on = 1,
        .bit_per_channel = 10,
        .dsc_line_buf_depth = 11,
        .bp_enable = 1,
        .bit_per_pixel = 128,
        .pic_height = 2400,
        .pic_width = 1080,
        .slice_height = 20,
        .slice_width = 540,
        .chunk_size = 540,
        .xmit_delay = 512,
        .dec_delay = 549,
        .scale_value = 32,
        .increment_interval = 469,
        .decrement_interval = 7,
        .line_bpg_offset = 13,
        .nfl_bpg_offset = 1402,
        .slice_bpg_offset = 1302,
        .initial_offset = 6144,
        .final_offset = 4336,
        .flatness_minqp = 7,
        .flatness_maxqp = 16,
        .rc_model_size = 8192,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit0 = 15,
        .rc_quant_incr_limit1 = 15,
        .rc_tgt_offset_hi = 3,
        .rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = hc5622_cmd_fhd_buf_thresh,
			.range_min_qp = hc5622_cmd_fhd_range_min_qp,
			.range_max_qp = hc5622_cmd_fhd_range_max_qp,
			.range_bpg_ofs = hc5622_cmd_fhd_range_bpg_ofs,
		},
/*        .rc_buf_thresh[0] = 14,
        .rc_buf_thresh[1] = 28,
        .rc_buf_thresh[2] = 42,
        .rc_buf_thresh[3] = 56,
        .rc_buf_thresh[4] = 70,
        .rc_buf_thresh[5] = 84,
        .rc_buf_thresh[6] = 98,
        .rc_buf_thresh[7] = 105,
        .rc_buf_thresh[8] = 112,
        .rc_buf_thresh[9] = 119,
        .rc_buf_thresh[10] = 121,
        .rc_buf_thresh[11] = 123,
        .rc_buf_thresh[12] = 125,
        .rc_buf_thresh[13] = 126,
        .rc_range_parameters[0].range_min_qp = 0,
        .rc_range_parameters[0].range_max_qp = 8,
        .rc_range_parameters[0].range_bpg_offset = 2,
        .rc_range_parameters[1].range_min_qp = 4,
        .rc_range_parameters[1].range_max_qp = 8,
        .rc_range_parameters[1].range_bpg_offset = 0,
        .rc_range_parameters[2].range_min_qp = 5,
        .rc_range_parameters[2].range_max_qp = 9,
        .rc_range_parameters[2].range_bpg_offset = 0,
        .rc_range_parameters[3].range_min_qp = 5,
        .rc_range_parameters[3].range_max_qp = 10,
        .rc_range_parameters[3].range_bpg_offset = -2,
        .rc_range_parameters[4].range_min_qp = 7,
        .rc_range_parameters[4].range_max_qp = 11,
        .rc_range_parameters[4].range_bpg_offset = -4,
        .rc_range_parameters[5].range_min_qp = 7,
        .rc_range_parameters[5].range_max_qp = 11,
        .rc_range_parameters[5].range_bpg_offset = -6,
        .rc_range_parameters[6].range_min_qp = 7,
        .rc_range_parameters[6].range_max_qp = 11,
        .rc_range_parameters[6].range_bpg_offset = -8,
        .rc_range_parameters[7].range_min_qp = 7,
        .rc_range_parameters[7].range_max_qp = 12,
        .rc_range_parameters[7].range_bpg_offset = -8,
        .rc_range_parameters[8].range_min_qp = 7,
        .rc_range_parameters[8].range_max_qp = 13,
        .rc_range_parameters[8].range_bpg_offset = -8,
        .rc_range_parameters[9].range_min_qp = 7,
        .rc_range_parameters[9].range_max_qp = 14,
        .rc_range_parameters[9].range_bpg_offset = -10,
        .rc_range_parameters[10].range_min_qp = 9,
        .rc_range_parameters[10].range_max_qp = 14,
        .rc_range_parameters[10].range_bpg_offset = -10,
        .rc_range_parameters[11].range_min_qp = 9,
        .rc_range_parameters[11].range_max_qp = 15,
        .rc_range_parameters[11].range_bpg_offset = -12,
        .rc_range_parameters[12].range_min_qp = 9,
        .rc_range_parameters[12].range_max_qp = 15,
        .rc_range_parameters[12].range_bpg_offset = -12,
        .rc_range_parameters[13].range_min_qp = 13,
        .rc_range_parameters[13].range_max_qp = 16,
        .rc_range_parameters[13].range_bpg_offset = -12,
        .rc_range_parameters[14].range_min_qp = 16,
        .rc_range_parameters[14].range_max_qp = 17,
        .rc_range_parameters[14].range_bpg_offset = -12,*/
    },
};

static struct mtk_panel_params ext_params_60hz = {
	.data_rate = 467*2,
	.pll_clk = 467,
	//drv-Modify the problem of incorrect third-party size detection-pzp-start
	.physical_width_um = PHYSICAL_WIDTH,
    .physical_height_um = PHYSICAL_HEIGHT,
    // drv-Modify the problem of incorrect third-party size detection-pzp-end
    // drv modify open esd-pengzhipeng20231219-start
    .cust_esd_check = 1,
    .esd_check_enable = 1,
    // drv modify open esd-pengzhipeng20231219-end
    .lcm_esd_check_table[0] = {
        .cmd = 0x0a,
        .count = 1,
        .para_list[0] = 0x9c,
    },

    .lp_perline_en = 1,
    .hbm_en_time = 0,
    .hbm_dis_time = 0,
    //+Mod by-drv huangxinglve,20250623,mod for HBM shinning start
    .target_time = 12233,
    .hbm_deadline = 12233,
    .vsync_time = 12233,
    //-Mod by-drv huangxinglve,20250623,mod for HBM shinning end
    .dsc_params = {
        .enable = 1,
        .ver = 17,
        .slice_mode = 1,
        .rgb_swap = 0,
        .dsc_cfg = 2088,
        .rct_on = 1,
        .bit_per_channel = 10,
        .dsc_line_buf_depth = 11,
        .bp_enable = 1,
        .bit_per_pixel = 128,
        .pic_height = 2400,
        .pic_width = 1080,
        .slice_height = 20,
        .slice_width = 540,
        .chunk_size = 540,
        .xmit_delay = 512,
        .dec_delay = 549,
        .scale_value = 32,
        .increment_interval = 469,
        .decrement_interval = 7,
        .line_bpg_offset = 13,
        .nfl_bpg_offset = 1402,
        .slice_bpg_offset = 1302,
        .initial_offset = 6144,
        .final_offset = 4336,
        .flatness_minqp = 7,
        .flatness_maxqp = 16,
        .rc_model_size = 8192,
        .rc_edge_factor = 6,
        .rc_quant_incr_limit0 = 15,
        .rc_quant_incr_limit1 = 15,
        .rc_tgt_offset_hi = 3,
        .rc_tgt_offset_lo = 3,
		.ext_pps_cfg = {
			.enable = 1,
			.rc_buf_thresh = hc5622_cmd_fhd_buf_thresh,
			.range_min_qp = hc5622_cmd_fhd_range_min_qp,
			.range_max_qp = hc5622_cmd_fhd_range_max_qp,
			.range_bpg_ofs = hc5622_cmd_fhd_range_bpg_ofs,
		},

/*        .rc_buf_thresh[0] = 14,
        .rc_buf_thresh[1] = 28,
        .rc_buf_thresh[2] = 42,
        .rc_buf_thresh[3] = 56,
        .rc_buf_thresh[4] = 70,
        .rc_buf_thresh[5] = 84,
        .rc_buf_thresh[6] = 98,
        .rc_buf_thresh[7] = 105,
        .rc_buf_thresh[8] = 112,
        .rc_buf_thresh[9] = 119,
        .rc_buf_thresh[10] = 121,
        .rc_buf_thresh[11] = 123,
        .rc_buf_thresh[12] = 125,
        .rc_buf_thresh[13] = 126,
        .rc_range_parameters[0].range_min_qp = 0,
        .rc_range_parameters[0].range_max_qp = 8,
        .rc_range_parameters[0].range_bpg_offset = 2,
        .rc_range_parameters[1].range_min_qp = 4,
        .rc_range_parameters[1].range_max_qp = 8,
        .rc_range_parameters[1].range_bpg_offset = 0,
        .rc_range_parameters[2].range_min_qp = 5,
        .rc_range_parameters[2].range_max_qp = 9,
        .rc_range_parameters[2].range_bpg_offset = 0,
        .rc_range_parameters[3].range_min_qp = 5,
        .rc_range_parameters[3].range_max_qp = 10,
        .rc_range_parameters[3].range_bpg_offset = -2,
        .rc_range_parameters[4].range_min_qp = 7,
        .rc_range_parameters[4].range_max_qp = 11,
        .rc_range_parameters[4].range_bpg_offset = -4,
        .rc_range_parameters[5].range_min_qp = 7,
        .rc_range_parameters[5].range_max_qp = 11,
        .rc_range_parameters[5].range_bpg_offset = -6,
        .rc_range_parameters[6].range_min_qp = 7,
        .rc_range_parameters[6].range_max_qp = 11,
        .rc_range_parameters[6].range_bpg_offset = -8,
        .rc_range_parameters[7].range_min_qp = 7,
        .rc_range_parameters[7].range_max_qp = 12,
        .rc_range_parameters[7].range_bpg_offset = -8,
        .rc_range_parameters[8].range_min_qp = 7,
        .rc_range_parameters[8].range_max_qp = 13,
        .rc_range_parameters[8].range_bpg_offset = -8,
        .rc_range_parameters[9].range_min_qp = 7,
        .rc_range_parameters[9].range_max_qp = 14,
        .rc_range_parameters[9].range_bpg_offset = -10,
        .rc_range_parameters[10].range_min_qp = 9,
        .rc_range_parameters[10].range_max_qp = 14,
        .rc_range_parameters[10].range_bpg_offset = -10,
        .rc_range_parameters[11].range_min_qp = 9,
        .rc_range_parameters[11].range_max_qp = 15,
        .rc_range_parameters[11].range_bpg_offset = -12,
        .rc_range_parameters[12].range_min_qp = 9,
        .rc_range_parameters[12].range_max_qp = 15,
        .rc_range_parameters[12].range_bpg_offset = -12,
        .rc_range_parameters[13].range_min_qp = 13,
        .rc_range_parameters[13].range_max_qp = 16,
        .rc_range_parameters[13].range_bpg_offset = -12,
        .rc_range_parameters[14].range_min_qp = 16,
        .rc_range_parameters[14].range_max_qp = 17,
        .rc_range_parameters[14].range_bpg_offset = -12,*/
    },
};

static int panel_ata_check(struct drm_panel* panel)
{
    /* Customer test by own ATA tool */
    return 1;
}
static atomic_t current_backlight;//add by huangxinglve, 20250527, add for als calibration
static int lcm_setbacklight_cmdq(void* dsi, dcs_write_gce cb, void* handle,
    unsigned int level)
{

    rawlevel = level;
    /* drv modify by pengzhipeng add for als HBM start */
    if (rawlevel > 3800 && rawlevel <= 4095)//mod by huangxinglve, 20250826, mod for custom request 550nit:3624
        rawlevel = 4094;
    /* drv modify by pengzhipeng add for als HBM end */

    bl_tb[1] = (rawlevel >> 8) & 0xf;
    bl_tb[2] = (rawlevel) & 0xff;
    if (!cb)
        return -1;

    atomic_set(&current_backlight, rawlevel);//add by huangxinglve, 20250527, add for als calibration
    if (g_ctx->hbm_stat == false || rawlevel == 0)
        cb(dsi, handle, bl_tb, ARRAY_SIZE(bl_tb));

    if (rawlevel != 0) {
        last_level = rawlevel;
    }
    pr_err("%s level=%d, rawlevel=%d, bl_tb[1]=0x%x, bl_tb[2]=0x%x\n",
        __func__, level, rawlevel, bl_tb[1], bl_tb[2]);

    return 0;
}
/* pri add by huangxinglve 20250521,for als cali begin */
unsigned short led_level_disp_get(char *name)
{
	int trans_level = 0;
	trans_level = atomic_read(&current_backlight);
	//pr_err("[panel][%s]: name: %s, level : %d",__func__, name, trans_level);
	return trans_level;
}
EXPORT_SYMBOL(led_level_disp_get);
/* pri add by huangxinglve 20250521,for als cali end */

/* drv modify hbm function start */
static int panel_hbm_set_cmdq(struct drm_panel* panel, void* dsi,
    dcs_write_gce cb, void* handle, bool en)
{
    struct lcm* ctx = panel_to_lcm(panel);

    if (!cb)
        return -1;

    if (ctx->hbm_en == en)
        goto done;

    if (en) {
        pr_info("[panel] %s : enter HBM, last_level = %d\n", __func__, last_level);
        ctx->hbm_stat = true;
        // cb(dsi, handle, init_head_tb, ARRAY_SIZE(init_head_tb));	//modify by shenwenbin for tuning backlight 20231108
        cb(dsi, handle, hbm_tb, ARRAY_SIZE(hbm_tb));
        // usleep_range(100, 101);
    } else {
        pr_info("[panel] %s : exit HBM\n", __func__);
        ctx->hbm_stat = false;
        lcm_setbacklight_cmdq(dsi, cb, handle, last_level);
    }

    ctx->hbm_en = en;
    ctx->hbm_wait = true;

done:
    return 0;
}

static void panel_hbm_get_state(struct drm_panel* panel, bool* state)
{

    struct lcm* ctx = panel_to_lcm(panel);
    //	pr_info("[panel] %s : hbm_en = %d\n",__func__,ctx->hbm_en);

    *state = ctx->hbm_en;
}

static void panel_hbm_get_wait_state(struct drm_panel* panel, bool* wait)
{
    struct lcm* ctx = panel_to_lcm(panel);

    *wait = ctx->hbm_wait;
}

static bool panel_hbm_set_wait_state(struct drm_panel* panel, bool wait)
{
    struct lcm* ctx = panel_to_lcm(panel);
    bool old = ctx->hbm_wait;

    ctx->hbm_wait = wait;
    return old;
}

/* drv modify hbm function end */

struct drm_display_mode* get_mode_by_id_hfp(struct drm_connector* connector,
    unsigned int mode)
{
    struct drm_display_mode* m;
    unsigned int i = 0;

    list_for_each_entry(m, &connector->modes, head)
    {
        if (i == mode)
            return m;
        i++;
    }
    return NULL;
}

static int panel_ext_reset(struct drm_panel* panel, int on)
{
    struct lcm* ctx = panel_to_lcm(panel);

    ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
    gpiod_set_value(ctx->reset_gpio, on);
    devm_gpiod_put(ctx->dev, ctx->reset_gpio);

    return 0;
}

struct drm_display_mode* get_mode_by_id(struct drm_connector* connector,
    unsigned int mode)
{
    struct drm_display_mode* m;
    unsigned int i = 0;

    list_for_each_entry(m, &connector->modes, head)
    {
        if (i == mode)
            return m;
        i++;
    }
    return NULL;
}

static int mtk_panel_ext_param_set(struct drm_panel* panel,
    struct drm_connector* connector, unsigned int mode)
{
    struct mtk_panel_ext* ext = find_panel_ext(panel);
    int ret = 0;
    struct drm_display_mode* m = get_mode_by_id(connector, mode);
    if (!m) {
        pr_err("%s:%d invalid display_mode\n", __func__, __LINE__);
        return ret;
    }
    if (drm_mode_vrefresh(m) == 120)
        ext->params = &ext_params_120hz;
    else if (drm_mode_vrefresh(m) == 90)
        ext->params = &ext_params_90hz;
    else if (drm_mode_vrefresh(m) == 60)
        ext->params = &ext_params_60hz;
    else
        ret = 1;

    return ret;
}

static void mode_switch_to_120(struct drm_panel* panel,
    enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
    if (stage == BEFORE_DSI_POWERDOWN) {
        struct lcm* ctx = panel_to_lcm(panel);

        lcm_dcs_write_seq_static(ctx, 0x48, 0x20); // drv-The producer of Ying screen recorded two different frame rates, causing some screens to cut 120 screen.-pzp
        // msleep(40);
        pr_info("%s:%d  120 display_mode end\n", __func__, __LINE__);
    }
}

static void mode_switch_to_90(struct drm_panel* panel,
    enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
    if (stage == BEFORE_DSI_POWERDOWN) {
        struct lcm* ctx = panel_to_lcm(panel);

        lcm_dcs_write_seq_static(ctx, 0x48, 0x10);
        // msleep(40);
        pr_info("%s:%d  60 display_mode end\n", __func__, __LINE__);
    }
}

static void mode_switch_to_60(struct drm_panel* panel,
    enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
    if (stage == BEFORE_DSI_POWERDOWN) {
        struct lcm* ctx = panel_to_lcm(panel);

        lcm_dcs_write_seq_static(ctx, 0x48, 0x00);
        // msleep(40);
        pr_info("%s:%d  60 display_mode end\n", __func__, __LINE__);
    }
}

static int mode_switch(struct drm_panel* panel,
    struct drm_connector* connector, unsigned int cur_mode,
    unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
    int ret = 0;
    struct drm_display_mode* m = get_mode_by_id(connector, dst_mode);

    if (cur_mode == dst_mode)
        return ret;
    if (drm_mode_vrefresh(m) == 60) { /*switch to 60 */
        mode_switch_to_60(panel, stage);
    } else if (drm_mode_vrefresh(m) == 90) { /*switch to 120 */
        mode_switch_to_90(panel, stage);
    } else if (drm_mode_vrefresh(m) == 120) { /*switch to 120 */
        mode_switch_to_120(panel, stage);
    } else
        ret = 1;

    return ret;
}

static int panel_doze_enable(struct drm_panel* panel,
    void* dsi_drv, dcs_write_gce cb, void* handle)
{
    struct lcm* ctx = panel_to_lcm(panel);
    pr_err("%s", __func__);
    // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-start
    if (!ctx->doze_en)
        ctx->doze_en = true;
    // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-end
    lcm_dcs_write_seq_static(ctx, 0x90, 0x01);
    lcm_dcs_write_seq_static(ctx, 0x51, 0x01, 0x55);
    msleep(100);
	/* drv modify ts suspend enter for fod reporting start */
	if(!IS_ERR_OR_NULL(ts_suspend_callback))
		ts_suspend_callback();
	/* drv modify ts suspend enter for fod reporting end */
    return 0;
}
// drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-start
bool drv_get_hc5622_doze_state(void)
{
    if (IS_ERR_OR_NULL(g_ctx)) {
        return false;
    }
    return g_ctx->doze_en;
}
EXPORT_SYMBOL_GPL(drv_get_hc5622_doze_state);
// drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-end
static int panel_doze_disable(struct drm_panel* panel,
    void* dsi_drv, dcs_write_gce cb, void* handle)
{
    struct lcm* ctx = panel_to_lcm(panel);
    pr_err("%s", __func__);
    // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-start
    if (ctx->doze_en)
        ctx->doze_en = false;
    // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516-end
    lcm_dcs_write_seq_static(ctx, 0x90, 0x00);
    msleep(40);
    return 0;
}
/* drv modify K80NF-263, K80NF-273 start */
bool panel_visionox_hc5622_fod_is_enabled(void)
{
    //	pr_info("panel %s g_ctx->doze_en=%d,g_ctx->prepared=%d\n", __func__,g_ctx->doze_en,g_ctx->prepared);
    if (IS_ERR_OR_NULL(g_ctx)) {
        return false;
    }

    if (g_ctx->doze_en)
        return g_ctx->doze_en;
    else
        return !g_ctx->prepared;
}
EXPORT_SYMBOL(panel_visionox_hc5622_fod_is_enabled);
/* drv modify K80NF-263, K80NF-273 end */
/* drv modify ts suspend enter for fod reporting start */
void panel_visionox_hc5622_fod_set_callback(void* func)
{
	if (!IS_ERR_OR_NULL(g_ctx)) {
	    pr_err("%s", __func__);
		if(IS_ERR_OR_NULL(ts_suspend_callback)) {
			pr_err("%s set ", __func__);
			ts_suspend_callback = func;
		}
	}
}
EXPORT_SYMBOL(panel_visionox_hc5622_fod_set_callback);
/* drv modify ts suspend enter for fod reporting end */
static int panel_set_aod_light_mode(void* dsi_drv, dcs_write_gce cb,
    void* handle, unsigned int mode)
{
    pr_err("%s, mode%d", __func__, mode);

    if (mode == 1) {
        /* AOD1 Switch */
        lcm_dcs_write_seq_static(g_ctx, 0x90, 0x01);
        lcm_dcs_write_seq_static(g_ctx, 0x51, 0x0F, 0xFE);
        // msleep(40);
    } else if (mode == 2) {
        /* AOD2 Switch */
        lcm_dcs_write_seq_static(g_ctx, 0x90, 0x01);
        lcm_dcs_write_seq_static(g_ctx, 0x51, 0x04, 0x00);
        // msleep(40);
    } else if (mode == 3) {
        /* AOD3 Switch */
        lcm_dcs_write_seq_static(g_ctx, 0x90, 0x01);
        lcm_dcs_write_seq_static(g_ctx, 0x51, 0x01, 0x55);
        // msleep(40);
    } else {
        pr_err("%s, invalid mode", __func__);
    }
    return 0;
}

static struct mtk_panel_funcs ext_funcs = {
    .reset = panel_ext_reset,
    .set_backlight_cmdq = lcm_setbacklight_cmdq,
    .ata_check = panel_ata_check,
    .ext_param_set = mtk_panel_ext_param_set,
    .mode_switch = mode_switch,
    .doze_enable = panel_doze_enable,
    .doze_disable = panel_doze_disable,
    .set_aod_light_mode = panel_set_aod_light_mode,
    /* drv modify hbm function start */
    .hbm_set_cmdq = panel_hbm_set_cmdq,
    .hbm_get_state = panel_hbm_get_state,
    .hbm_get_wait_state = panel_hbm_get_wait_state,
    .hbm_set_wait_state = panel_hbm_set_wait_state,
    /* drv modify hbm function end */
};
#endif

static int lcm_get_modes(struct drm_panel* panel,
    struct drm_connector* connector)
{
    struct drm_display_mode* mode;
    struct drm_display_mode* mode_1;
    struct drm_display_mode* mode_2;

    mode = drm_mode_duplicate(connector->dev, &switch_mode_120hz);
    if (!mode) {
        dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
            switch_mode_120hz.hdisplay, switch_mode_120hz.vdisplay,
            drm_mode_vrefresh(&switch_mode_120hz));
        return -ENOMEM;
    }

    drm_mode_set_name(mode);
    mode->type = DRM_MODE_TYPE_DRIVER;
    drm_mode_probed_add(connector, mode);

    mode_2 = drm_mode_duplicate(connector->dev, &switch_mode_90hz);
    if (!mode_2) {
        dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
            switch_mode_120hz.hdisplay, switch_mode_120hz.vdisplay,
            drm_mode_vrefresh(&switch_mode_120hz));
        return -ENOMEM;
    }

    drm_mode_set_name(mode_2);
    mode_2->type = DRM_MODE_TYPE_DRIVER;
    drm_mode_probed_add(connector, mode_2);

    mode_1 = drm_mode_duplicate(connector->dev, &switch_mode_60hz);
    if (!mode_1) {
        dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
            switch_mode_60hz.hdisplay, switch_mode_60hz.vdisplay,
            drm_mode_vrefresh(&switch_mode_60hz));
        return -ENOMEM;
    }

    drm_mode_set_name(mode_1);
    mode_1->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    drm_mode_probed_add(connector, mode_1);

    connector->display_info.width_mm = 64;
    connector->display_info.height_mm = 129;

    return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
    .disable = lcm_disable,
    .unprepare = lcm_unprepare,
    .prepare = lcm_prepare,
    .enable = lcm_enable,
    .get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device* dsi)
{
    struct device* dev = &dsi->dev;
    struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
    struct lcm* ctx;
    struct device_node* backlight;
    int ret;

    pr_info("%s+\n", __func__);

    dsi_node = of_get_parent(dev->of_node);
    if (dsi_node) {
        endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
        if (endpoint) {
            remote_node = of_graph_get_remote_port_parent(endpoint);
            if (!remote_node) {
                pr_info("No panel connected,skip probe lcm\n");
                return -ENODEV;
            }
            pr_info("device node name:%s\n", remote_node->name);
        }
    }
    if (remote_node != dev->of_node) {
        pr_info("%s+ skip probe due to not current lcm\n", __func__);
        return -ENODEV;
    }

    ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    mipi_dsi_set_drvdata(dsi, ctx);

    ctx->dev = dev;
    dsi->lanes = 4;
    dsi->format = MIPI_DSI_FMT_RGB888;
    dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET// MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

    backlight = of_parse_phandle(dev->of_node, "backlight", 0);
    if (backlight) {
        ctx->backlight = of_find_backlight_by_node(backlight);
        of_node_put(backlight);

        if (!ctx->backlight)
            return -EPROBE_DEFER;
    }

    ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->reset_gpio)) {
        dev_info(dev, "cannot get reset-gpios %ld\n",
            PTR_ERR(ctx->reset_gpio));
        return PTR_ERR(ctx->reset_gpio);
    }
    devm_gpiod_put(dev, ctx->reset_gpio);

    ctx->vldo18_gpio = devm_gpiod_get(dev, "vldo18", GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->vldo18_gpio)) {
        dev_info(dev, "cannot get vldo18-gpios %ld\n",
            PTR_ERR(ctx->vldo18_gpio));
        return PTR_ERR(ctx->vldo18_gpio);
    }
    devm_gpiod_put(dev, ctx->vldo18_gpio);

    ctx->bias_pos = devm_gpiod_get_index(dev, "bias", 0, GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bias_pos)) {
        dev_info(dev, "cannot get bias-gpios 0 %ld\n",
            PTR_ERR(ctx->bias_pos));
        return PTR_ERR(ctx->bias_pos);
    }
    devm_gpiod_put(dev, ctx->bias_pos);

    ctx->bias_neg = devm_gpiod_get_index(dev, "bias", 1, GPIOD_OUT_HIGH);
    if (IS_ERR(ctx->bias_neg)) {
        dev_info(dev, "cannot get bias-gpios 1 %ld\n",
            PTR_ERR(ctx->bias_neg));
        return PTR_ERR(ctx->bias_neg);
    }
    devm_gpiod_put(dev, ctx->bias_neg);

    ctx->prepared = true;
    ctx->enabled = true;
    drm_panel_init(&ctx->panel, dev, &lcm_drm_funcs, DRM_MODE_CONNECTOR_DSI);

    drm_panel_add(&ctx->panel);

    ret = mipi_dsi_attach(dsi);
    if (ret < 0)
        drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
    mtk_panel_tch_handle_reg(&ctx->panel);
    ret = mtk_panel_ext_create(dev, &ext_params_60hz, &ext_funcs, &ctx->panel);
    if (ret < 0)
        return ret;

#endif

    g_ctx = ctx;
    g_ctx->doze_en = false; // drv-Fixed the issue of entering aod and TP having touch-pengzhipeng-20230516
    pr_info("%s- lcm,hc5622,cmd,60hz\n", __func__);
    /* drv modify hbm function start */
    ctx->hbm_en = false;
    ctx->hbm_stat = false;
    /* drv modify hbm function end */
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
    strcpy(current_lcm_info.chip, "HC5622");
    strcpy(current_lcm_info.vendor, "Visionox");
    sprintf(current_lcm_info.id, "0x%02x", 0x01);
    strcpy(current_lcm_info.more, "1080*2400");
#endif
    return ret;
}

static void lcm_remove(struct mipi_dsi_device* dsi)
{
    struct lcm* ctx = mipi_dsi_get_drvdata(dsi);
#if defined(CONFIG_MTK_PANEL_EXT)
    struct mtk_panel_ctx* ext_ctx = find_panel_ctx(&ctx->panel);
#endif

    mipi_dsi_detach(dsi);
    drm_panel_remove(&ctx->panel);
#if defined(CONFIG_MTK_PANEL_EXT)
    mtk_panel_detach(ext_ctx);
    mtk_panel_remove(ext_ctx);
#endif

    //return 0;
}

static const struct of_device_id lcm_of_match[] = {
    {
        .compatible = "visionox,hc5622,cmd",
    },
    {}
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
    .probe = lcm_probe,
    .remove = lcm_remove,
    .driver = {
        .name = "panel-visionox-hc5622-dphy-cmd-120hz",
        .owner = THIS_MODULE,
        .of_match_table = lcm_of_match,
    },
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("MEDIATEK");
MODULE_DESCRIPTION("hc5622 AMOLED CMD LCD Panel Driver");
MODULE_LICENSE("GPL v2");
