/* Copyright (c) 2012, LGE Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/regulator/consumer.h>
#include "devices.h"

#include "board-8064.h"
#ifdef CONFIG_SND_SOC_TPA2028D
#include <sound/tpa2028d.h>
#endif

#include "board-gvar.h"
#ifdef CONFIG_SWITCH_FSA8008
#include <linux/platform_data/hds_fsa8008.h>
#include <mach/board_lge.h>
#endif
#ifdef CONFIG_SWITCH_MAX1462X 
#include <linux/platform_data/hds_max1462x.h>
#include <mach/board_lge.h>
#include <linux/regulator/consumer.h> //[AUDIO_BSP], 20120730, sehwan.lee@lge.com PMIC L29 Control(because headset noise)
#endif
#include "../../../../sound/soc/codecs/wcd9310.h"


#define TPA2028D_ADDRESS (0xB0>>1)
#define MSM_AMP_EN (PM8921_GPIO_PM_TO_SYS(19))
#define AGC_COMPRESIION_RATE        0
#define AGC_OUTPUT_LIMITER_DISABLE  1
#define AGC_FIXED_GAIN              15


#define GPIO_EAR_SENSE_N            38
#define GPIO_EAR_MIC_EN             PM8921_GPIO_PM_TO_SYS(31)
#define GPIO_EARPOL_DETECT          PM8921_GPIO_PM_TO_SYS(32)
#define GPIO_MIC_BIAS_CONTROL       PM8921_GPIO_PM_TO_SYS(32)
#define GPIO_EAR_KEY_INT            23

#define I2C_SURF 1
#define I2C_FFA  (1 << 1)
#define I2C_RUMI (1 << 2)
#define I2C_SIM  (1 << 3)
#define I2C_LIQUID (1 << 4)

struct i2c_registry {
	u8                     machs;
	int                    bus;
	struct i2c_board_info *info;
	int                    len;
};

#ifdef CONFIG_SND_SOC_TPA2028D
int amp_enable(int on_state)
{
	int err = 0;
	static int init_status = 0;
	struct pm_gpio param = {
		.direction      = PM_GPIO_DIR_OUT,
		.output_buffer  = PM_GPIO_OUT_BUF_CMOS,
		.output_value   = 1,
		.pull           = PM_GPIO_PULL_NO,
		.vin_sel	= PM_GPIO_VIN_S4,
		.out_strength   = PM_GPIO_STRENGTH_MED,
		.function       = PM_GPIO_FUNC_NORMAL,
	};

	if (init_status == 0) {
		err = gpio_request(MSM_AMP_EN, "AMP_EN");
		if (err)
			pr_err("%s: Error requesting GPIO %d\n",
					__func__, MSM_AMP_EN);

		err = pm8xxx_gpio_config(MSM_AMP_EN, &param);
		if (err)
			pr_err("%s: Failed to configure gpio %d\n",
					__func__, MSM_AMP_EN);
		else
			init_status++;
	}

	switch (on_state) {
	case 0:
		err = gpio_direction_output(MSM_AMP_EN, 0);
		printk(KERN_INFO "%s: AMP_EN is set to 0\n", __func__);
		break;
	case 1:
		err = gpio_direction_output(MSM_AMP_EN, 1);
		printk(KERN_INFO "%s: AMP_EN is set to 1\n", __func__);
		break;
	case 2:
		printk(KERN_INFO "%s: amp enable bypass(%d)\n", __func__, on_state);
		err = 0;
		break;

	default:
		pr_err("amp enable fail\n");
		err = 1;
		break;
	}
	return err;
}

static struct audio_amp_platform_data amp_platform_data =  {
	.enable = amp_enable,
	.agc_compression_rate = AGC_COMPRESIION_RATE,
	.agc_output_limiter_disable = AGC_OUTPUT_LIMITER_DISABLE,
	.agc_fixed_gain = AGC_FIXED_GAIN,
};
#endif

static struct i2c_board_info msm_i2c_audiosubsystem_info[] = {
#ifdef CONFIG_SND_SOC_TPA2028D
	{
		I2C_BOARD_INFO("tpa2028d_amp", TPA2028D_ADDRESS),
		.platform_data = &amp_platform_data,
	}
#endif
};


static struct i2c_registry msm_i2c_audiosubsystem __initdata = {
	/* Add the I2C driver for Audio Amp */
	I2C_SURF | I2C_FFA | I2C_RUMI | I2C_SIM | I2C_LIQUID,
	APQ_8064_GSBI1_QUP_I2C_BUS_ID,
	msm_i2c_audiosubsystem_info,
	ARRAY_SIZE(msm_i2c_audiosubsystem_info),
};


static void __init lge_add_i2c_tpa2028d_devices(void)
{
	/* Run the array and install devices as appropriate */
	i2c_register_board_info(msm_i2c_audiosubsystem.bus,
				msm_i2c_audiosubsystem.info,
				msm_i2c_audiosubsystem.len);
}

#ifdef CONFIG_SWITCH_FSA8008
static struct fsa8008_platform_data lge_hs_pdata = {
	.switch_name = "h2w",
	.keypad_name = "hs_detect",

	.key_code = KEY_MEDIA,

	.gpio_detect = GPIO_EAR_SENSE_N,
	.gpio_mic_en = GPIO_EAR_MIC_EN,
	.gpio_jpole  = GPIO_EARPOL_DETECT,
	.gpio_key    = GPIO_EAR_KEY_INT,
       .set_headset_mic_bias = NULL,

	.latency_for_detection = 75,
};

static struct platform_device lge_hsd_device = {
	.name = "fsa8008",
	.id   = -1,
	.dev = {
		.platform_data = &lge_hs_pdata,
	},
};

static int __init lge_hsd_fsa8008_init(void)
{
    hw_rev_type bd_rev = -1;
    hw_rev_type lge_bd_rev = HW_REV_EVB1;
	printk(KERN_INFO "lge_hsd_fsa8008_init\n");

    lge_bd_rev = lge_get_board_revno();

    if (lge_bd_rev == HW_REV_F) {
        lge_hs_pdata.set_headset_mic_bias = NULL; //2012-08-24 mikyoung.chang@lge.com mic bias is controlled by mic_en
        printk(KERN_INFO "lge_bd_rev : %d, >= bd_rev : %d, so set set_headset_mic_bias = NULL!!!\n", lge_bd_rev, bd_rev);
    }
    else {
        lge_hs_pdata.set_headset_mic_bias = tabla_codec_micbias2_ctl;
        printk(KERN_INFO "lge_bd_rev : %d, < bd_rev : %d, so set_mic_bias = tabla_codec_micbias2_ctl!!!\n", lge_bd_rev, bd_rev);
    }
	return platform_device_register(&lge_hsd_device);
}

static void __exit lge_hsd_fsa8008_exit(void)
{
	printk(KERN_INFO "lge_hsd_fsa8008_exit\n");
	platform_device_unregister(&lge_hsd_device);
}
#endif

#ifdef CONFIG_SWITCH_MAX1462X
static struct max1462x_platform_data lge_hs_pdata = {
	.switch_name = "h2w",
	.keypad_name = "hs_detect",

	.key_code = 0,	                                   //KEY_MEDIA, KEY_VOLUMEUP or KEY_VOLUMEDOWN

	.gpio_mode	= GPIO_EAR_MIC_EN,		              // .gpio_mic_en = GPIO_EAR_MIC_EN,
	.gpio_det	= GPIO_EAR_SENSE_N,		              // .gpio_detect = GPIO_EAR_SENSE_N,
	.gpio_swd	= GPIO_EAR_KEY_INT,		       // .gpio_key    = GPIO_EAR_KEY_INT,										
	.external_ldo_mic_bias	= GPIO_MIC_BIAS_CONTROL,	// .gpio_jpole  = GPIO_EARPOL_DETECT
	.latency_for_detection = 75,
	.latency_for_key = 50,                                 //latency for key detect (ms)
	.adc_mpp_num = PM8XXX_AMUX_MPP_1,	/* PMIC adc mpp number to read adc level on MIC */
	.adc_channel = ADC_MPP_1_AMUX6, 	/* PMIC adc channel to read adc level on MIC */


};

static struct platform_device lge_hsd_device = {
	.name = "max1462x",
	.id   = -1,
	.dev = {
		.platform_data = &lge_hs_pdata,
	},
};

/* LGE_CHANGED_START 2012.10.25, gyuhwa.park@lge.com
 * PMIC L10 Control
*/
static bool max1462x_mic_bias = false;
#define REGLULATOR_VOLTAGE_L10_MIN	2500000
#define REGLULATOR_VOLTAGE_L10_MAX	3100000


void set_headset_mic_bias_l10(int on)
{
	int rc;
	int voltage_l10;
	static struct regulator *vreg_l10;

	if (!max1462x_mic_bias) {

		vreg_l10 = regulator_get(NULL, "8921_l10");   //HEADSET_MIC_BIAS, VREG_L10 , 2.8V
		if (IS_ERR(vreg_l10)) {
			pr_err("regulator get of vreg_l10 failed (%ld)\n", PTR_ERR(vreg_l10));
			return;
		}

		voltage_l10 = regulator_get_voltage(vreg_l10);
		if (voltage_l10 < REGLULATOR_VOLTAGE_L10_MIN || voltage_l10 > REGLULATOR_VOLTAGE_L10_MAX) {
			pr_err("failed out of range voltage_l10 %d\n", voltage_l10);
			return;
		}

		rc = regulator_set_voltage(vreg_l10, voltage_l10, voltage_l10);
		if (rc) {
			pr_err("failed regulator_set_voltage rc=%d\n", rc);
			return;
		}

		pr_info("voltage_l10 = [%d]\n", voltage_l10);
		max1462x_mic_bias = true;
	}

	if (on) {
		pr_info("MIC_BIAS Enabled");
		regulator_enable(vreg_l10);
	} else {
		pr_info("MIC_BIAS Disabled");
		regulator_disable(vreg_l10);
	}

}
/* LGE_CHANGED_END 2012.10.25, gyuhwa.park@lge.com */

static int __init lge_hsd_max1462x_init(void)
{
    hw_rev_type bd_rev = -1;
    hw_rev_type lge_bd_rev = HW_REV_EVB1;
    pr_info("lge_hsd_max1462x_init\n");

    lge_bd_rev = lge_get_board_revno();

	if (lge_bd_rev != HW_REV_D) {
		lge_hs_pdata.set_headset_mic_bias = set_headset_mic_bias_l10;
		pr_info("lge_bd_rev : %d, >= bd_rev : %d\n", lge_bd_rev, bd_rev);
	} else {
		lge_hs_pdata.set_headset_mic_bias = NULL;
		pr_info("lge_bd_rev : %d, < bd_rev : %d\n", lge_bd_rev, bd_rev);
	}

	return platform_device_register(&lge_hsd_device);
}

static void __exit lge_hsd_max1462x_exit(void)
{
	pr_info("lge_hsd_max1462x_exit\n");
	platform_device_unregister(&lge_hsd_device);
}

#endif
void __init lge_add_sound_devices(void)
{
	lge_add_i2c_tpa2028d_devices();
#ifdef CONFIG_SWITCH_FSA8008
	lge_hsd_fsa8008_init();
#endif
#ifdef CONFIG_SWITCH_MAX1462X
	lge_hsd_max1462x_init();
#endif
}
