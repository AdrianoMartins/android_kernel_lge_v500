/*
 * android vibrator driver
 *
 * Copyright (C) 2009-2012 LGE, Inc.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
#if defined(CONFIG_ANDROID_SW_IRRC)
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/err.h>
#include <linux/android_irrc.h>
#include <linux/spinlock.h>
#include "../staging/android/timed_output.h"
#include <linux/types.h>
#include <linux/err.h>
#include <mach/msm_iomap.h>
#include <linux/io.h>
#include <mach/gpiomux.h>
#include <mach/board_lge.h>
#include <linux/i2c.h>
#include <mach/msm_xo.h>
#include <linux/slab.h>

#include <linux/ioctl.h>
#include <asm/ioctls.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [START]
#if defined (CONFIG_MACH_APQ8064_AWIFI)
#include <linux/regulator/consumer.h>
#endif
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [END]

#define GPIO_IRRC_PWM               34
#define D_INV 0             /* QCT support invert bit for msm8960 */
#define GP_CLK_ID 0			/* gp clk 0 */
#define REG_WRITEL(value, reg)	writel(value, (MSM_CLK_CTL_BASE+reg))
#define GPn_NS_REG(n)                           (0x2D24+32*(n))
#define GPn_MD_REG(n)                           (0x2D00+32*(n))
static struct irrc_ts_data *ts;
static int gpio_set=0;

//static struct irrc_ts_data *ts;
struct timed_irrc_data {
	struct platform_device dev;
	spinlock_t lock;
	atomic_t data_code; /* vibrator duration */		//vib_timems -> data code change
	struct android_irrc_platform_data *irrc_data;
};

struct irrc_compr_params {
	int frequency;
	int duty;
	int length;
};

struct irrc_ts_data {
    struct workqueue_struct *workqueue;
    struct delayed_work input_PWM_off_work;
	struct delayed_work input_GPIO_off_work;
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [START]
#if defined (CONFIG_MACH_APQ8064_AWIFI)
	struct delayed_work irrc_power_off_work;
#endif
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [END]
};

//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [START]
#if defined (CONFIG_MACH_APQ8064_AWIFI)
static struct regulator *vreg_l8 = NULL;
static bool snddev_reg_8921_l8_status = false;

static int irrc_power_set(bool enable)
{
	int rc = -EINVAL;
	if (NULL == vreg_l8) {
		vreg_l8 = regulator_get(NULL, "sw_irrc");   //2.6 ~ 3V
		INFO_MSG("enable=%d\n", enable);

		if (IS_ERR(vreg_l8)) {
			pr_err("%s: regulator get of sw_irrc failed (%ld)\n"
					, __func__, PTR_ERR(vreg_l8));
			printk("%s ERROR\n", __FUNCTION__);
			rc = PTR_ERR(vreg_l8);
			return rc;
		}
	}
	rc = regulator_set_voltage(vreg_l8, 2800000, 2800000);

	if (enable == snddev_reg_8921_l8_status)
		return 0;

	if (enable) {
		rc = regulator_set_voltage(vreg_l8, 2800000, 2800000);
		if (rc < 0)
			pr_err("LGE:  VIB %s: regulator_set_voltage(l8) failed (%d)\n",
			__func__, rc);

		rc = regulator_enable(vreg_l8);

		if (rc < 0)
			pr_err("LGE: VIB %s: regulator_enable(l8) failed (%d)\n", __func__, rc);
		snddev_reg_8921_l8_status = true;

		printk(KERN_INFO "ooooo %s_ regulator(l8) volatage = %d ooooo\n",
			__FUNCTION__, regulator_get_voltage(vreg_l8));

	} else {
		rc = regulator_disable(vreg_l8);
		if (rc < 0)
			pr_err("%s: regulator_disable(l8) failed (%d)\n", __func__, rc);
		snddev_reg_8921_l8_status = false;

                printk(KERN_INFO "ooooo %s_ regulator(l8) volatage = %d ooooo\n",
			__FUNCTION__, regulator_get_voltage(vreg_l8));
	}

	return 0;
}
#endif
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [END]

static struct msm_xo_voter *irrc_clock;


static int irrc_clock_init(void)
{
	int rc;
	/*Vote for XO clock*/
	irrc_clock = msm_xo_get(MSM_XO_TCXO_D0, "pmic_xoadc");

	if (IS_ERR(irrc_clock)) {
		rc = PTR_ERR(irrc_clock);
		printk(KERN_ERR "%s: Couldn't get TCXO_D0 vote for irrc(%d)\n", __func__, rc);
	}
	return rc;
}

static int irrc_pwm_set(int enable,int PWM_CLK, int duty)
{
	int M_VAL=1;
	int N_VAL=1;
	int D_VAL=1;


	//pwm_clk khz = 19.2 Mhz  * M_VAL / Predive *N_VAL
	//N_VAL = floor(19200*M_VAL/PWM_CLK*4);

	N_VAL = (9600+PWM_CLK)/(PWM_CLK*2);
	D_VAL = (N_VAL*duty+50)/100;
	if (D_VAL==0)
		D_VAL=1;


	printk(KERN_INFO "enable = %d ,M_VAL = %d, N_VAL = %d , D_VAL = %d\n",enable,M_VAL,N_VAL,D_VAL);
	msm_xo_mode_vote(irrc_clock, MSM_XO_MODE_ON);
	//PWM frequency setting
	REG_WRITEL(
	(((M_VAL & 0xffU) << 16U) + /* M_VAL[23:16] */
	((~(D_VAL << 1)) & 0xffU)),  /* D_VAL[7:0] */
	GPn_MD_REG(GP_CLK_ID));



	/* TODO: set clk for amp */
	if (enable) {
			////GP_CLK ON

			////GP_CLK source 32khz  select , predive Div -1 select
		REG_WRITEL(
			((((~(N_VAL-M_VAL)) & 0xffU) << 16U) + /* N_VAL[23:16] */
			(0U << 11U) +  /* CLK_ROOT_ENA[11]	: Disable(0) */
			(0U << 10U) +  /* CLK_INV[10]		: Disable(0) */
			(0U << 9U) +	 /* CLK_BRANCH_ENA[9] : Disable(0) */
			(0U << 8U) +   /* NMCNTR_EN[8]		: Disable(0) */
			(1U << 7U) +   /* MNCNTR_RST[7] 	: Not Active(0) */
			(2U << 5U) +   /* MNCNTR_MODE[6:5]	: Dual-edge mode(2) */
			(3U << 3U) +   /* PRE_DIV_SEL[4:3]	: Div-4(3) */
			(5U << 0U)),   /* SRC_SEL[2:0]		: CXO (5)  */
			GPn_NS_REG(GP_CLK_ID));

		REG_WRITEL(
			((((~(N_VAL-M_VAL)) & 0xffU) << 16U) + /* N_VAL[23:16] */
			(1U << 11U) +  /* CLK_ROOT_ENA[11]  : Enable(1) */
			(0U << 10U) +  /* CLK_INV[10]       : Disable(0) */
			(1U << 9U) +   /* CLK_BRANCH_ENA[9] : Enable(1) */
			(1U << 8U) +   /* NMCNTR_EN[8]      : Enable(1) */
			(0U << 7U) +   /* MNCNTR_RST[7]     : Not Active(0) */
			(2U << 5U) +   /* MNCNTR_MODE[6:5]  : Dual-edge mode(2) */
			(3U << 3U) +   /* PRE_DIV_SEL[4:3]  : Div-4(3) */
			(5U << 0U)),   /* SRC_SEL[2:0]      : CXO (5)  */
			GPn_NS_REG(GP_CLK_ID));

	} else {
		////GP_CLK OFF
		REG_WRITEL(
			((((~(N_VAL-M_VAL)) & 0xffU) << 16U) + /* N_VAL[23:16] */
			(0U << 11U) +  /* CLK_ROOT_ENA[11]  : Disable(0) */
			(0U << 10U) +  /* CLK_INV[10]	    : Disable(0) */
			(0U << 9U) +	 /* CLK_BRANCH_ENA[9] : Disable(0) */
			(0U << 8U) +   /* NMCNTR_EN[8]      : Disable(0) */
			(1U << 7U) +   /* MNCNTR_RST[7]     : Not Active(0) */
			(2U << 5U) +   /* MNCNTR_MODE[6:5]  : Dual-edge mode(2) */
			(3U << 3U) +   /* PRE_DIV_SEL[4:3]  : Div-4 (3) */
			(5U << 0U)),   /* SRC_SEL[2:0]      : CXO (5)  */
			GPn_NS_REG(GP_CLK_ID));
	}

	return 0;
}


static void irrc_pwmoff_work_func(struct work_struct *work)
{
	irrc_pwm_set(0,38,30);
	msm_xo_mode_vote(irrc_clock, MSM_XO_MODE_OFF);
}


static void irrc_gpio_off_work_func(struct work_struct *work)
{
	gpio_set_value(GPIO_IRRC_PWM, 0);

}

static void irrc_power_off_work_func(struct work_struct *work)
{
	irrc_power_set(0);
}

static int irrc_pwmon(int PWM_CLK, int duty)
{

	if((PWM_CLK==0)||(duty==100)){
		printk(KERN_INFO "no frequency signal!\n");
		cancel_delayed_work_sync(&ts->input_GPIO_off_work);
		gpio_tlmm_config(GPIO_CFG(GPIO_IRRC_PWM, 0, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_4MA), GPIO_CFG_ENABLE);
		gpio_set_value(GPIO_IRRC_PWM, 1);
		gpio_set=1;
		}
	else if((PWM_CLK<23)||(PWM_CLK>1200)||(duty>60)||(duty<20) ){
		printk(KERN_INFO "Out of range ! \n");
		}
	else{
		cancel_delayed_work_sync(&ts->input_PWM_off_work);
		gpio_tlmm_config(GPIO_CFG(GPIO_IRRC_PWM, 2, GPIO_CFG_OUTPUT, GPIO_CFG_NO_PULL, GPIO_CFG_4MA), GPIO_CFG_ENABLE);
		irrc_pwm_set(1,PWM_CLK,duty);
		gpio_set=0;
			}
	return 0;

}


static long IRRC_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	int PWM_CLK;
	int duty;

	struct irrc_compr_params test;

	rc = copy_from_user(&test, (void __user *)arg, sizeof(test));
    printk(KERN_INFO "IRRC_ioctl freqeuncy = %d, duty = %d\n",test.frequency,test.duty);

	PWM_CLK=(test.frequency)/1000;
	duty=test.duty;

	switch (cmd) {
	case IRRC_START:
		printk(KERN_INFO "IRRC_START\n");
		irrc_pwmon(PWM_CLK,duty);
		//LGE_CHANGE_S 20130813 keunhui.park [IrRC] fix for long key press
		if (delayed_work_pending(&ts->input_PWM_off_work)) {
			cancel_delayed_work(&ts->input_PWM_off_work);
		}
		if (delayed_work_pending(&ts->irrc_power_off_work)) {
			cancel_delayed_work(&ts->irrc_power_off_work);
		}
		//LGE_CHANGE_E 20130813 keunhui.park [IrRC] fix for long key press
		break;
	case IRRC_STOP:
		printk(KERN_INFO "IRRC_STOP\n");
		if(gpio_set==1){
			printk(KERN_INFO "gpio_signal_off!\n");
			cancel_delayed_work_sync(&ts->input_GPIO_off_work);
			queue_delayed_work(ts->workqueue,&ts->input_GPIO_off_work, msecs_to_jiffies(1500));
			}
		else{
			printk(KERN_INFO "pwm_signal_off!\n");
			cancel_delayed_work_sync(&ts->input_PWM_off_work);
			queue_delayed_work(ts->workqueue,&ts->input_PWM_off_work, msecs_to_jiffies(1500));
				}
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [START]
#if defined (CONFIG_MACH_APQ8064_AWIFI)
		if (lge_get_board_revno() >= HW_REV_A) {
			printk(KERN_INFO "IRRC_power_off!\n");
			cancel_delayed_work_sync(&ts->irrc_power_off_work);
			queue_delayed_work(ts->workqueue,&ts->irrc_power_off_work, msecs_to_jiffies(1500));
		}
#endif
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [END]
		break;
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [START]
	case IRRC_POWER:
#if defined (CONFIG_MACH_APQ8064_AWIFI)
		if (lge_get_board_revno() >= HW_REV_A) {
			printk(KERN_INFO "IRRC_POWER\n");
			irrc_power_set(1);
			rc = 1;
		}
#else
		rc = -EPERM;
#endif
		break;
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [END]
	default:
		rc = -EINVAL;
	}

	return rc;
}



static int IRRC_open(struct inode *inode, struct file *file)
{
	return 0;
}
static int IRRC_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t IRRC_write(struct file *file, const char __user *buf,
	size_t count, loff_t *pos)
{
	return 0;
}


int IRRCpcm_fsync(struct file *file, loff_t a, loff_t b, int datasync)
{
	return 0;
}



static const struct file_operations IRRC_pcm_fops = {
	.owner		= THIS_MODULE,
	.open		= IRRC_open,
	.release	= IRRC_release,
	.write		= IRRC_write,
	.unlocked_ioctl	= IRRC_ioctl,
	.fsync = IRRCpcm_fsync,
};


struct miscdevice IRRC_pcm_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "msm_IRRC_pcm_dec",
	.fops	= &IRRC_pcm_fops,
};


struct timed_irrc_data android_irrc_data = {
	.dev.name = "irrc",
//	.dev.enable = irrc_enable,
	.irrc_data = NULL,

};


static int android_irrc_probe(struct platform_device *pdev)
{
		int nRet;
		struct timed_irrc_data *irrc;
		printk(KERN_INFO "[IRRC][%s] \n", __func__);

		platform_set_drvdata(pdev, &android_irrc_data);
		irrc = (struct timed_irrc_data *)platform_get_drvdata(pdev);
		irrc->irrc_data = (struct android_irrc_platform_data *)pdev->dev.platform_data;

		if (irrc->irrc_data->irrc_init() < 0) {
			ERR_MSG("IRRC GPIO set failed\n");
			return -ENODEV;
		}

		nRet = misc_register(&IRRC_pcm_misc);
		if (nRet)
		{
			ERR_MSG("IRRC: misc_register failed.\n");
			return -ENODEV;
		}

		ts = kzalloc(sizeof(struct irrc_ts_data), GFP_KERNEL);
		if (ts == NULL) {
			ERR_MSG("Can not allocate memory\n");
			goto err_1;
		}

		ts->workqueue = create_workqueue("irrc_ts_workqueue");
		if (!ts->workqueue) {
			ERR_MSG("Unable to create workqueue\n");
			goto err_2;
		}

		INIT_DELAYED_WORK(&ts->input_PWM_off_work, irrc_pwmoff_work_func);
		INIT_DELAYED_WORK(&ts->input_GPIO_off_work, irrc_gpio_off_work_func);
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [START]
#if defined (CONFIG_MACH_APQ8064_AWIFI)
		INIT_DELAYED_WORK(&ts->irrc_power_off_work, irrc_power_off_work_func);
#endif
//2013-06-11 Ilda_jung(ilda.jung@lge.com) [AWIFI] set power for SW_IRRC on AWIFI [END]

		INFO_MSG("Android IRRC Initialization was done\n");
		return 0;

		err_2:
			kfree(ts);
		err_1:
			misc_deregister(&IRRC_pcm_misc);
			return -ENODEV;

}

static int android_irrc_remove(struct platform_device *pdev)
{
	misc_deregister(&IRRC_pcm_misc);
	kfree(ts);
	return 0;
}

#ifdef CONFIG_PM
static int android_irrc_suspend(struct platform_device *pdev,
		pm_message_t state)
{
	printk("[IRRC][%s] \n", __func__);
	return 0;
}

static int android_irrc_resume(struct platform_device *pdev)
{
	printk("[IRRC][%s] \n", __func__);
	return 0;
}
#endif

static void android_irrc_shutdown(struct platform_device *pdev)
{
}



static struct platform_driver android_irrc_driver = {
	.probe = android_irrc_probe,
	.remove = android_irrc_remove,
	.shutdown = android_irrc_shutdown,
#ifdef CONFIG_PM
	.suspend = android_irrc_suspend,
	.resume = android_irrc_resume,
#else
	.suspend = NULL,
	.resume = NULL,
#endif
	.driver = {
		.name = "android-irrc",
	},
};

static int __init android_irrc_init(void)
{

	printk(KERN_INFO "[IRRC][%s] \n", __func__);
	irrc_clock_init();
	return platform_driver_register(&android_irrc_driver);
}

static void __exit android_irrc_exit(void)
{

	INFO_MSG(KERN_INFO "Android IRRC Driver Exit\n");
	platform_driver_unregister(&android_irrc_driver);

}

late_initcall_sync(android_irrc_init); /* to let init lately */
module_exit(android_irrc_exit);

MODULE_AUTHOR("LG Electronics Inc.");
MODULE_DESCRIPTION("Android IRRC Driver");
MODULE_LICENSE("GPL");
#endif
