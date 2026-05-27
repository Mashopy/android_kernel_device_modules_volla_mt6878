

#ifndef _HARDWARE_INFO_H_
#define _HARDWARE_INFO_H_

/* prize modify for audio hardware info 202400305 start */
/* Hardcoded audio param version here,
 * since kernel-5.x can't access filesystem in kernel space
 chip:MT6878(CPU)+MT6369(PMIC)+TFA9873(PA)\nproduct:X10\nversion:V04-20231101\ */
#ifndef AUDIO_PARA_CHIP
#define AUDIO_PARA_CHIP    "MT6878(CPU)+MT6369(PMIC)+TFA9873(PA)"
#endif
#ifndef AUDIO_PARA_PRODUCT
#define AUDIO_PARA_PRODUCT "X10"
#endif
#ifndef AUDIO_PARA_VERSION
#define AUDIO_PARA_VERSION "V01-20240321"
#endif
/* prize modify for audio hardware info 202400305 end */

struct hardware_info{
	unsigned char chip[32];
	unsigned char vendor[32];
	unsigned char id[32];
	unsigned char more[64];
#if IS_ENABLED(CONFIG_PRI_HARDWARE_INFO_BAT)
	unsigned char batt_versions[32];
	unsigned char Q_MAX_POS_50[32];
	unsigned char Q_MAX_POS_25[32];
	unsigned char Q_MAX_POS_10[32];
	unsigned char Q_MAX_POS_0[32];
#endif
};

extern struct hardware_info current_lcm_info;
/* pri LAX10-192 added by xuejian 20240409 begin*/
extern struct hardware_info current_sub_lcm_info;
/* pri LAX10-192 added by xuejian 20240409 end*/
extern struct hardware_info current_camera_info[5];
extern struct hardware_info current_tp_info;
/* pri LAX10-192 added by xuejian 20240410 begin*/
extern struct hardware_info current_sub_tp_info;
/* pri LAX10-192 added by xuejian 20240410 end*/
extern struct hardware_info current_fingerprint_info;
extern struct hardware_info current_coulo_info;
extern struct hardware_info current_alsps_info;
extern struct hardware_info current_gsensor_info;
extern struct hardware_info current_msensor_info;
extern struct hardware_info current_gyroscope_info;
extern struct hardware_info current_barosensor_info;
extern struct hardware_info current_sarsensor_info;
extern struct hardware_info current_flash_lpddr_info;
#if IS_ENABLED(CONFIG_PRI_HARDWARE_INFO_BAT)
extern struct hardware_info current_battery_info;
#endif
#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO_UFS_HEALTH)
extern struct hardware_info current_flash_health_info;
#endif
extern struct hardware_info current_line_motor_info;
#endif /* _HARDWARE_INFO_H_ */
