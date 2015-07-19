/* Copyright (c) 2011-2013, The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/irq.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/debugfs.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/mfd/pmic8058.h>
#include <linux/msm_charm.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/mach-types.h>
#include <asm/uaccess.h>
#include <mach/mdm2.h>
#include <mach/restart.h>
#include <mach/subsystem_notif.h>
#include <mach/subsystem_restart.h>
#include <mach/rpm.h>
#include <mach/gpiomux.h>
#include <linux/notifier.h>
#include "msm_watchdog.h"
#include "mdm_private.h"
#include "sysmon.h"

#ifndef __KERNEL_SYSCALLS__
#define __KERNEL_SYSCALLS__
#endif

#define MDM_MODEM_TIMEOUT	6000
#define MDM_MODEM_DELTA	100
#define MDM_BOOT_TIMEOUT	60000L
#define MDM_RDUMP_TIMEOUT	120000L
#define MDM2AP_STATUS_TIMEOUT_MS 60000L

/* Allow a maximum device id of this many digits */
#define MAX_DEVICE_DIGITS  10
#define EXTERNAL_MODEM "external_modem"
#define SUBSYS_NAME_LENGTH \
	(sizeof(EXTERNAL_MODEM) + MAX_DEVICE_DIGITS)

#define DEVICE_BASE_NAME "mdm"
#define DEVICE_NAME_LENGTH \
	(sizeof(DEVICE_BASE_NAME) + MAX_DEVICE_DIGITS)

#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
#define STEP_PBL_FAIL		1
#define STEP_9008_FAIL		2
#define STEP_UNMEASURABLE_FAIL	3
#define STEP_9048_FAIL		4
#define STEP_CRASH		5
#define STEP_SHUTDOWN		6
#define STEP_ERROR		7
#define STEP_MAX		8

static int modem_boot_check = 0;
static int failure_step[100];
static int failure_step_count = 0;
static int root_failure_step = STEP_MAX;
static int now_early_booting = 1;
static int mdm2ap_status_error_timer = 0;
static int mdm2ap_errfatal_error_timer = 0;
module_param(modem_boot_check, int, S_IRUGO | S_IWUSR);
module_param(root_failure_step, int, S_IRUGO | S_IWUSR);

void update_failure_case(void);
int get_error_step(int *phase, int phase_size);
int translate_failure(void);
#endif

#define RD_BUF_SIZE			100
#define SFR_MAX_RETRIES		10
#define SFR_RETRY_INTERVAL	1000

enum gpio_update_config {
	GPIO_UPDATE_BOOTING_CONFIG = 1,
	GPIO_UPDATE_RUNNING_CONFIG,
};

static LIST_HEAD(mdm_driver_list);
static DEFINE_MUTEX(mdm_driver_list_lock);
static DEFINE_MUTEX(mdm_driver_list_add_lock);

struct mdm_device {
	struct list_head		link;
	struct mdm_modem_drv	mdm_data;

	int mdm2ap_status_valid_old_config;
	struct gpiomux_setting mdm2ap_status_old_config;
	int first_boot;
	struct workqueue_struct *mdm_queue;
	struct workqueue_struct *mdm_sfr_queue;
	unsigned int dump_timeout_ms;

	char subsys_name[SUBSYS_NAME_LENGTH];
	struct subsys_desc mdm_subsys;
	struct subsys_device *mdm_subsys_dev;

	char device_name[DEVICE_NAME_LENGTH];
	struct miscdevice misc_device;

	struct completion mdm_needs_reload;
	struct completion mdm_boot;
	struct completion mdm_ram_dumps;
	int mdm_errfatal_irq;
	int mdm_status_irq;
	int mdm_pblrdy_irq;

	struct delayed_work mdm2ap_status_check_work;
	struct work_struct mdm_status_work;
	struct work_struct sfr_reason_work;

	struct notifier_block mdm_panic_blk;
	struct notifier_block ssr_notifier_blk;

	int ssr_started_internally;
};

static struct list_head	mdm_devices;
static DEFINE_SPINLOCK(mdm_devices_lock);
static int disable_boot_timeout = 0;

static int ssr_count;
static DEFINE_SPINLOCK(ssr_lock);

static unsigned int mdm_debug_mask;
int vddmin_gpios_sent;
static struct mdm_ops *mdm_ops;
static struct mutex restart_lock;

static void mdm_device_list_add(struct mdm_device *mdev)
{
	unsigned long flags;

	spin_lock_irqsave(&mdm_devices_lock, flags);
	list_add_tail(&mdev->link, &mdm_devices);
	spin_unlock_irqrestore(&mdm_devices_lock, flags);
}

static void mdm_device_list_remove(struct mdm_device *mdev)
{
	unsigned long flags;
	struct mdm_device *lmdev, *tmp;

	spin_lock_irqsave(&mdm_devices_lock, flags);
	list_for_each_entry_safe(lmdev, tmp, &mdm_devices, link) {
		if (mdev && mdev == lmdev) {
			pr_debug("%s: removing device id %d\n",
			  __func__, mdev->mdm_data.device_id);
			list_del(&mdev->link);
			break;
		}
	}
	spin_unlock_irqrestore(&mdm_devices_lock, flags);
}

static int param_set_disable_boot_timeout(const char *val,
		const struct kernel_param *kp)
{
	int rcode;
	pr_info("%s called\n",__func__);
	rcode = param_set_bool(val, kp);
	if (rcode)
		pr_err("%s: Failed to set boot_timout_disabled flag\n",
				__func__);
	pr_info("%s: disable_boot_timeout is now %d\n",
			__func__, disable_boot_timeout);
	return rcode;
}

static struct kernel_param_ops disable_boot_timeout_ops = {
	.set = param_set_disable_boot_timeout,
	.get = param_get_bool,
};
module_param_cb(disable_boot_timeout, &disable_boot_timeout_ops,
		&disable_boot_timeout, 0644);
MODULE_PARM_DESC(disable_boot_timeout, "Disable panic on mdm bootup timeout");

// kyle00.choi, 20140411, RF Device Check [START]		
#ifdef CONFIG_MACH_APQ8064_ALTEV
static void log_modem_sfr(void);
#endif
// kyle00.choi, 20140411, RF Device Check [END]


/* If the platform's cascading_ssr flag is set, the subsystem
 * restart module will restart the other modems so stop
 * monitoring them as well.
 * This function can be called from interrupt context.
 */
static void mdm_start_ssr(struct mdm_device *mdev)
{
	unsigned long flags;
	int start_ssr = 1;

	spin_lock_irqsave(&ssr_lock, flags);
	if (mdev->mdm_data.pdata->cascading_ssr &&
			ssr_count > 0) {
		start_ssr = 0;
	} else {
		ssr_count++;
		mdev->ssr_started_internally = 1;
	}
	spin_unlock_irqrestore(&ssr_lock, flags);

	if (start_ssr) {
		atomic_set(&mdev->mdm_data.mdm_ready, 0);
		pr_info("%s: Resetting mdm id %d due to mdm error\n",
				__func__, mdev->mdm_data.device_id);
// kyle00.choi, 20140411, RF Device Check [START]		
#ifdef CONFIG_MACH_APQ8064_ALTEV
		log_modem_sfr();
#endif
// kyle00.choi, 20140411, RF Device Check [END]

		subsystem_restart_dev(mdev->mdm_subsys_dev);
	} else {
		pr_info("%s: Another modem is already in SSR\n",
				__func__);
	}
}

/* Increment the reference count to handle the case where
 * subsystem restart is initiated by the SSR service.
 */
static void mdm_ssr_started(struct mdm_device *mdev)
{
	unsigned long flags;

	spin_lock_irqsave(&ssr_lock, flags);
	ssr_count++;
	atomic_set(&mdev->mdm_data.mdm_ready, 0);
	spin_unlock_irqrestore(&ssr_lock, flags);
}

/* mdm_ssr_completed assumes that mdm_ssr_started has previously
 * been called.
 */
static void mdm_ssr_completed(struct mdm_device *mdev)
{
	unsigned long flags;

	spin_lock_irqsave(&ssr_lock, flags);
	ssr_count--;
	if (mdev->ssr_started_internally) {
		mdev->ssr_started_internally = 0;
		ssr_count--;
	}

	if (ssr_count < 0) {
		pr_err("%s: ssr_count = %d\n",
			    __func__, ssr_count);
		panic("%s: ssr_count = %d < 0\n",
			  __func__, ssr_count);
	}
	spin_unlock_irqrestore(&ssr_lock, flags);
}

static struct mdm_driver_notif_info *mdm_notif_find_subsys(const char *name)
{
	struct mdm_driver_notif_info *notif;
	mutex_lock(&mdm_driver_list_lock);
	list_for_each_entry(notif, &mdm_driver_list, list)
		if (!strncmp(notif->name, name, ARRAY_SIZE(notif->name))) {
			mutex_unlock(&mdm_driver_list_lock);
			return notif;
		}
	mutex_unlock(&mdm_driver_list_lock);
	return NULL;
}

static void *mdm_notif_add_subsys(const char *name)
{
	struct mdm_driver_notif_info *notif = NULL;
	if (!name)
		goto done;
	mutex_lock(&mdm_driver_list_add_lock);
	notif = mdm_notif_find_subsys(name);
	if (notif) {
		mutex_unlock(&mdm_driver_list_add_lock);
		goto done;
	}
	notif = kmalloc(sizeof(struct mdm_driver_notif_info), GFP_KERNEL);
	if (!notif) {
		mutex_unlock(&mdm_driver_list_add_lock);
		return ERR_PTR(-EINVAL);
	}
	strlcpy(notif->name, name, ARRAY_SIZE(notif->name));
	srcu_init_notifier_head(&notif->mdm_driver_notif_rcvr_list);
	INIT_LIST_HEAD(&notif->list);
	list_add_tail(&notif->list, &mdm_driver_list);
	mutex_unlock(&mdm_driver_list_add_lock);
done:
	return notif;
}

struct mdm_driver_notif_info *mdm_driver_register_notifier(
			const char *name, struct notifier_block *nb)
{
	int ret;
	struct mdm_driver_notif_info *notif;

	notif = mdm_notif_find_subsys(name);
	if (!notif) {
		notif = (struct mdm_driver_notif_info *)
				mdm_notif_add_subsys(name);
		if (!notif)
			return NULL;
		pr_debug("%s : Created notifier node for %s\n", __func__, name);
	}
	ret = srcu_notifier_chain_register(
		&notif->mdm_driver_notif_rcvr_list, nb);
	if (ret < 0)
		return NULL;
	return notif;
}

static int mdm_driver_queue_notification(char *name,
					enum subsys_notif_type notif_type)
{
	int ret = 0;
	struct mdm_driver_notif_info *notif;
	if (!name)
		return -EINVAL;
	notif = mdm_notif_find_subsys(name);
	if (!notif) {
		pr_err("%s: Couldn't find notif for dev %s\n", __func__, name);
		return -EINVAL;
	}
	ret = srcu_notifier_call_chain(
			&notif->mdm_driver_notif_rcvr_list, notif_type,
			(void *)notif);
	return ret;
}
static irqreturn_t mdm_vddmin_change(int irq, void *dev_id)
{
	struct mdm_device *mdev = (struct mdm_device *)dev_id;
	struct mdm_vddmin_resource *vddmin_res;
	int value;

	if (!mdev)
		goto handled;

	vddmin_res = mdev->mdm_data.pdata->vddmin_resource;
	if (!vddmin_res)
		goto handled;

	value = gpio_get_value(
	   vddmin_res->mdm2ap_vddmin_gpio);
	if (value == 0)
		pr_info("External Modem id %d entered Vddmin\n",
				mdev->mdm_data.device_id);
	else
		pr_info("External Modem id %d exited Vddmin\n",
				mdev->mdm_data.device_id);
handled:
	return IRQ_HANDLED;
}

/* The vddmin_res resource may not be supported by some platforms. */
static void mdm_setup_vddmin_gpios(void)
{
	unsigned long flags;
	struct msm_rpm_iv_pair req;
	struct mdm_device *mdev;
	struct mdm_vddmin_resource *vddmin_res;
	int irq, ret;

	spin_lock_irqsave(&mdm_devices_lock, flags);
	list_for_each_entry(mdev, &mdm_devices, link) {
		vddmin_res = mdev->mdm_data.pdata->vddmin_resource;
		if (!vddmin_res)
			continue;

		pr_info("Enabling vddmin logging on modem id %d\n",
				mdev->mdm_data.device_id);
		req.id = vddmin_res->rpm_id;
		req.value =
			((uint32_t)vddmin_res->ap2mdm_vddmin_gpio & 0x0000FFFF)
						<< 16;
		req.value |= ((uint32_t)vddmin_res->modes & 0x000000FF) << 8;
		req.value |= (uint32_t)vddmin_res->drive_strength & 0x000000FF;

		msm_rpm_set(MSM_RPM_CTX_SET_0, &req, 1);

		/* Start monitoring low power gpio from mdm */
		irq = MSM_GPIO_TO_INT(vddmin_res->mdm2ap_vddmin_gpio);
		if (irq < 0)
			pr_err("%s: could not get LPM POWER IRQ resource mdm id %d.\n",
				   __func__, mdev->mdm_data.device_id);
		else {
			ret = request_threaded_irq(irq, NULL, mdm_vddmin_change,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				"mdm lpm", mdev);

			if (ret < 0)
				pr_err("%s: MDM LPM IRQ#%d request failed with error=%d",
					   __func__, irq, ret);
		}
	}
	spin_unlock_irqrestore(&mdm_devices_lock, flags);
	return;
}

#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
#define MAX_SSR_REASON_LEN 81U
char ssr_reason[MAX_SSR_REASON_LEN];
#endif

static void mdm_restart_reason_fn(struct work_struct *work)
{
	int ret, ntries = 0;
	char sfr_buf[RD_BUF_SIZE];
	struct mdm_platform_data *pdata;
	struct mdm_device *mdev = container_of(work,
			struct mdm_device, sfr_reason_work);

	pdata = mdev->mdm_data.pdata;
	do {
		if (pdata->sysmon_subsys_id_valid)
		{
			msleep(SFR_RETRY_INTERVAL);
			ret = sysmon_get_reason(pdata->sysmon_subsys_id,
					sfr_buf, sizeof(sfr_buf));
			if (ret) {
				/*
				 * The sysmon device may not have been probed as
				 * yet after the restart.
				 */
				pr_err("%s: Error retrieving restart reason,"
						"ret = %d %d/%d tries\n",
						__func__, ret,
						ntries + 1,
						SFR_MAX_RETRIES);
			} else {
				pr_err("mdm restart reason: %s\n", sfr_buf);
#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
                                sprintf(ssr_reason, "%s\n", sfr_buf);
#endif
				break;
			}
		}
	} while (++ntries < SFR_MAX_RETRIES);
}

static void mdm2ap_status_check(struct work_struct *work)
{
	struct mdm_device *mdev =
		container_of(work, struct mdm_device,
					 mdm2ap_status_check_work.work);
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;
	/*
	 * If the mdm modem did not pull the MDM2AP_STATUS gpio
	 * high then call subsystem_restart.
	 */
	if (!mdm_drv->disable_status_check) {
		if (gpio_get_value(mdm_drv->mdm2ap_status_gpio) == 0) {
			pr_err("%s: MDM2AP_STATUS did not go high on mdm id %d\n",
				   __func__, mdev->mdm_data.device_id);
			if (!disable_boot_timeout) {
#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
			mdm2ap_status_error_timer = 3;
#endif
				mdm_start_ssr(mdev);
			}
		}
	}
}

static void mdm_update_gpio_configs(struct mdm_device *mdev,
				enum gpio_update_config gpio_config)
{
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;

	/* Some gpio configuration may need updating after modem bootup.*/
	switch (gpio_config) {
	case GPIO_UPDATE_RUNNING_CONFIG:
		if (mdm_drv->pdata->mdm2ap_status_gpio_run_cfg) {
			if (msm_gpiomux_write(mdm_drv->mdm2ap_status_gpio,
				GPIOMUX_ACTIVE,
				mdm_drv->pdata->mdm2ap_status_gpio_run_cfg,
				&mdev->mdm2ap_status_old_config))
				pr_err("%s: failed updating running gpio config mdm id %d\n",
					   __func__, mdev->mdm_data.device_id);
			else
				mdev->mdm2ap_status_valid_old_config = 1;
		}
		break;
	case GPIO_UPDATE_BOOTING_CONFIG:
		if (mdev->mdm2ap_status_valid_old_config) {
			msm_gpiomux_write(mdm_drv->mdm2ap_status_gpio,
					GPIOMUX_ACTIVE,
					&mdev->mdm2ap_status_old_config,
					NULL);
			mdev->mdm2ap_status_valid_old_config = 0;
		}
		break;
	default:
		pr_err("%s: called with no config\n", __func__);
		break;
	}
}

static int reset_mdm(struct mdm_modem_drv *mdm_drv)
{
	int value;

	pr_info("%s++\n", __func__);

	if (!mutex_trylock(&restart_lock))
		return -EINVAL;

	value = gpio_get_value(mdm_drv->mdm2ap_status_gpio);
	if (value == 0 || atomic_read(&mdm_drv->mdm_ready) == 0) {
		pr_err("%s: mdm_status = %d, mdm_ready = %d, return\n",
			__func__, value, atomic_read(&mdm_drv->mdm_ready));
		mutex_unlock(&restart_lock);
		return -EINVAL;
	}

	atomic_set(&mdm_drv->mdm_ready, 0);
	mutex_unlock(&restart_lock);

	subsystem_restart(EXTERNAL_MODEM);

	pr_info("%s--\n", __func__);
	return 0;
}

static long mdm_modem_ioctl(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	int status, ret = 0;
	struct mdm_device *mdev = filp->private_data;
	struct mdm_modem_drv *mdm_drv;
	struct mdm_device *l_mdev;

	if (_IOC_TYPE(cmd) != CHARM_CODE) {
		pr_err("%s: invalid ioctl code to mdm id %d\n",
			   __func__, mdev->mdm_data.device_id);
		return -EINVAL;
	}

	mdm_drv = &mdev->mdm_data;
	pr_debug("%s: Entering ioctl cmd = %d, mdm id = %d\n",
			 __func__, _IOC_NR(cmd), mdev->mdm_data.device_id);
	switch (cmd) {
	case WAKE_CHARM:
		pr_info("%s: Powering on mdm id %d\n",
				__func__, mdev->mdm_data.device_id);
#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
		if((mdm2ap_status_error_timer == 3 || mdm2ap_status_error_timer <= 0) &&
			(mdm2ap_errfatal_error_timer == 3 || mdm2ap_errfatal_error_timer <= 0)) {
			if(mdm_drv->pdata->early_power_on &&
				now_early_booting != 1) {
				update_failure_case();
				modem_boot_check = 0x1;
				printk("modem_boot_check : %d\n", modem_boot_check);
				printk("%s : mdm2ap_status_error_timer : %d, mdm2ap_errfatal_error_timer : %d",
						 __func__, mdm2ap_status_error_timer, mdm2ap_errfatal_error_timer);
				mdm2ap_status_error_timer--;
				mdm2ap_errfatal_error_timer --;
			} else {
				now_early_booting = 0;
			}
		}
#endif
		mdm_ops->power_on_mdm_cb(mdm_drv);
		break;
	case RESET_CHARM:
		pr_info("RESET_CHARM");
		ret = reset_mdm(mdm_drv);
		break;
	case CHECK_FOR_BOOT:
		if (gpio_get_value(mdm_drv->mdm2ap_status_gpio) == 0)
			put_user(1, (unsigned long __user *) arg);
		else
			put_user(0, (unsigned long __user *) arg);
		break;
	case NORMAL_BOOT_DONE:
		pr_debug("%s: check if mdm id %d is booted up\n",
				 __func__, mdev->mdm_data.device_id);
		get_user(status, (unsigned long __user *) arg);
		if (status) {
			pr_debug("%s: normal boot of mdm id %d failed\n",
					 __func__, mdev->mdm_data.device_id);
			mdm_drv->mdm_boot_status = -EIO;
		} else {
			pr_info("%s: normal boot of mdm id %d done\n",
					__func__, mdev->mdm_data.device_id);
#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
			modem_boot_check = 0x7;
			printk("modem_boot_check : %d", modem_boot_check);
#endif
			mdm_drv->mdm_boot_status = 0;
		}
		atomic_set(&mdm_drv->mdm_ready, 1);

		if (mdm_ops->normal_boot_done_cb != NULL)
			mdm_ops->normal_boot_done_cb(mdm_drv);

		if (!mdev->first_boot)
			complete(&mdev->mdm_boot);
		else
			mdev->first_boot = 0;

		/* If successful, start a timer to check that the mdm2ap_status
		 * gpio goes high.
		 */
		if (!status && gpio_get_value(mdm_drv->mdm2ap_status_gpio) == 0)
			schedule_delayed_work(&mdev->mdm2ap_status_check_work,
				msecs_to_jiffies(MDM2AP_STATUS_TIMEOUT_MS));
		break;
	case RAM_DUMP_DONE:
		pr_debug("%s: mdm done collecting RAM dumps\n", __func__);
		get_user(status, (unsigned long __user *) arg);
		if (status)
			mdm_drv->mdm_ram_dump_status = -EIO;
		else {
			pr_info("%s: ramdump collection completed\n", __func__);
			mdm_drv->mdm_ram_dump_status = 0;
		}
		complete(&mdev->mdm_ram_dumps);
		break;
	case WAIT_FOR_RESTART:
		pr_debug("%s: wait for mdm to need images reloaded\n",
				__func__);
		ret = wait_for_completion_interruptible(
				&mdev->mdm_needs_reload);
		if (!ret)
			put_user(mdm_drv->boot_type,
					 (unsigned long __user *) arg);
		init_completion(&mdev->mdm_needs_reload);
		break;
	case GET_DLOAD_STATUS:
		pr_debug("getting status of mdm2ap_errfatal_gpio\n");
		if (gpio_get_value(mdm_drv->mdm2ap_errfatal_gpio) == 1 &&
			!atomic_read(&mdm_drv->mdm_ready))
			put_user(1, (unsigned long __user *) arg);
		else
			put_user(0, (unsigned long __user *) arg);
		break;
	case IMAGE_UPGRADE:
		pr_debug("%s Image upgrade ioctl recieved\n", __func__);
		if (mdm_drv->pdata->image_upgrade_supported &&
				mdm_ops->image_upgrade_cb) {
			list_for_each_entry(l_mdev, &mdm_devices, link) {
				if (l_mdev != mdev) {
					pr_debug("%s:setting mdm_rdy to false",
							__func__);
					atomic_set(&l_mdev->mdm_data.mdm_ready,
							0);
				}
			}
			get_user(status, (unsigned long __user *) arg);
			mdm_ops->image_upgrade_cb(mdm_drv, status);
		} else
			pr_debug("%s Image upgrade not supported\n", __func__);
		break;
	case SHUTDOWN_CHARM:
		if (!mdm_drv->pdata->send_shdn ||
				!mdm_drv->pdata->sysmon_subsys_id_valid) {
			pr_debug("%s shutdown not supported for this mdm\n",
					__func__);
			break;
		}
		atomic_set(&mdm_drv->mdm_ready, 0);
		if (mdm_debug_mask & MDM_DEBUG_MASK_SHDN_LOG)
			pr_info("Sending shutdown request to mdm\n");
		ret = sysmon_send_shutdown(mdm_drv->pdata->sysmon_subsys_id);
		if (ret)
			pr_err("%s:Graceful shutdown of mdm failed, ret = %d\n",
			   __func__, ret);
		put_user(ret, (unsigned long __user *) arg);
		break;
#ifdef CONFIG_LGE_PM_SHUTDOWN_MDM_IN_CHARGERLOGO
   case FORCE_SHUTDOWN_CHARM:
        pr_debug("%s FORCE_SHUTDOWN_CHARM\n", __func__);
        gpio_direction_output(mdm_drv->ap2mdm_soft_reset_gpio,1);
        mdelay(250);
        gpio_direction_output(mdm_drv->ap2mdm_soft_reset_gpio,0);
        break;
#endif
	default:
		pr_err("%s: invalid ioctl cmd = %d\n", __func__, _IOC_NR(cmd));
		ret = -EINVAL;
		break;
	}
	return ret;
}

static void mdm_status_fn(struct work_struct *work)
{
	enum subsys_notif_type powerup_notify = SUBSYS_AFTER_POWERUP;
	struct mdm_device *mdev =
		container_of(work, struct mdm_device, mdm_status_work);
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;
	int value = gpio_get_value(mdm_drv->mdm2ap_status_gpio);

	pr_debug("%s: status:%d\n", __func__, value);
	if (atomic_read(&mdm_drv->mdm_ready) && mdm_ops->status_cb) {
		mdm_ops->status_cb(mdm_drv, value);
		pr_debug("%s: sending powerup notification for %s\n",
			__func__, mdm_drv->pdata->subsys_name);
		mdm_driver_queue_notification(mdm_drv->pdata->subsys_name,
						 powerup_notify);
	}
	/* Update gpio configuration to "running" config. */
	mdm_update_gpio_configs(mdev, GPIO_UPDATE_RUNNING_CONFIG);
}

static void mdm_disable_irqs(struct mdm_device *mdev)
{
	if (!mdev)
		return;
	disable_irq_nosync(mdev->mdm_errfatal_irq);
	disable_irq_nosync(mdev->mdm_status_irq);
	disable_irq_nosync(mdev->mdm_pblrdy_irq);
}

static irqreturn_t mdm_errfatal(int irq, void *dev_id)
{
	struct mdm_modem_drv *mdm_drv;
	struct mdm_device *mdev = (struct mdm_device *)dev_id;
	if (!mdev)
		return IRQ_HANDLED;

	pr_debug("%s: mdm id %d sent errfatal interrupt\n",
			 __func__, mdev->mdm_data.device_id);
	mdm_drv = &mdev->mdm_data;
	if (atomic_read(&mdm_drv->mdm_ready) &&
		(gpio_get_value(mdm_drv->mdm2ap_status_gpio) == 1)) {
		pr_info("%s: Received err fatal from mdm id %d\n",
				__func__, mdev->mdm_data.device_id);
#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
                mdm2ap_errfatal_error_timer = 3;
#endif
		mdm_start_ssr(mdev);
	}
	return IRQ_HANDLED;
}

/* set the mdm_device as the file's private data */
static int mdm_modem_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct mdm_device *mdev = container_of(misc,
			struct mdm_device, misc_device);

	file->private_data = mdev;
	return 0;
}

static int ssr_notifier_cb(struct notifier_block *this,
				unsigned long code,
				void *data)
{
	struct mdm_device *mdev =
		container_of(this, struct mdm_device, ssr_notifier_blk);

	switch (code) {
	case SUBSYS_AFTER_POWERUP:
		mdm_ssr_completed(mdev);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int mdm_panic_prep(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	int i;
	struct mdm_modem_drv *mdm_drv;
	struct mdm_device *mdev =
		container_of(this, struct mdm_device, mdm_panic_blk);

	mdm_drv = &mdev->mdm_data;

	pr_debug("%s: setting AP2MDM_ERRFATAL high for a non graceful reset\n",
			 __func__);
	mdm_disable_irqs(mdev);
	gpio_set_value(mdm_drv->ap2mdm_errfatal_gpio, 1);

	for (i = MDM_MODEM_TIMEOUT; i > 0; i -= MDM_MODEM_DELTA) {
		pet_watchdog();
		mdelay(MDM_MODEM_DELTA);
		if (gpio_get_value(mdm_drv->mdm2ap_status_gpio) == 0)
			break;
	}
	if (i <= 0) {
		pr_err("%s: MDM2AP_STATUS never went low\n", __func__);
		/* Reset the modem so that it will go into download mode. */
		if (mdm_drv && mdm_ops->atomic_reset_mdm_cb)
			mdm_ops->atomic_reset_mdm_cb(mdm_drv);
	}
	return NOTIFY_DONE;
}

static irqreturn_t mdm_status_change(int irq, void *dev_id)
{
	struct mdm_modem_drv *mdm_drv;
	struct mdm_device *mdev = (struct mdm_device *)dev_id;
	int value;
	if (!mdev)
		return IRQ_HANDLED;

	mdm_drv = &mdev->mdm_data;
	value = gpio_get_value(mdm_drv->mdm2ap_status_gpio);

	if ((mdm_debug_mask & MDM_DEBUG_MASK_SHDN_LOG) && (value == 0))
		pr_info("%s: mdm2ap_status went low\n", __func__);

	pr_debug("%s: mdm id %d sent status change interrupt\n",
			 __func__, mdev->mdm_data.device_id);
	if (value == 0 && atomic_read(&mdm_drv->mdm_ready)) {
		pr_info("%s: unexpected reset external modem id %d\n",
				__func__, mdev->mdm_data.device_id);
		mdm_drv->mdm_unexpected_reset_occurred = 1;
		mdm_start_ssr(mdev);
	} else if (value == 1) {
		cancel_delayed_work(&mdev->mdm2ap_status_check_work);
		pr_info("%s: status = 1: mdm id %d is now ready\n",
				__func__, mdev->mdm_data.device_id);
		queue_work(mdev->mdm_queue, &mdev->mdm_status_work);
	}
	return IRQ_HANDLED;
}

static irqreturn_t mdm_pblrdy_change(int irq, void *dev_id)
{
	struct mdm_modem_drv *mdm_drv;
	struct mdm_device *mdev = (struct mdm_device *)dev_id;
	int pblrdy;

	if (!mdev)
		return IRQ_HANDLED;

	mdm_drv = &mdev->mdm_data;

	pblrdy = gpio_get_value(mdm_drv->mdm2ap_pblrdy);

	pr_info("%s: mdm id %d: pbl ready:%d\n",
			__func__, mdev->mdm_data.device_id,
			pblrdy);

#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
	if(pblrdy == 1) {
		modem_boot_check = 0x3;
		printk("modem_boot_check : %d", modem_boot_check);
	}
#endif
	return IRQ_HANDLED;
}

static int mdm_subsys_shutdown(const struct subsys_desc *crashed_subsys)
{
	struct mdm_device *mdev =
	 container_of(crashed_subsys, struct mdm_device, mdm_subsys);
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;

	pr_debug("%s: ssr on modem id %d\n", __func__,
			 mdev->mdm_data.device_id);

	mdm_ssr_started(mdev);
	cancel_delayed_work(&mdev->mdm2ap_status_check_work);

	if (!mdm_drv->pdata->no_a2m_errfatal_on_ssr)
		gpio_direction_output(mdm_drv->ap2mdm_errfatal_gpio, 1);

	if (mdm_drv->pdata->ramdump_delay_ms > 0) {
		/* Wait for the external modem to complete
		 * its preparation for ramdumps.
		 */
		msleep(mdm_drv->pdata->ramdump_delay_ms);
	}
	if (!mdm_drv->mdm_unexpected_reset_occurred) {
#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
		if((mdm2ap_status_error_timer == 3 || mdm2ap_status_error_timer <= 0) &&
			(mdm2ap_errfatal_error_timer == 3 || mdm2ap_errfatal_error_timer <= 0)) {	
			update_failure_case();
		}
		modem_boot_check = 0x1;
		printk("%s : mdm2ap_status_error_timer : %d, mdm2ap_errfatal_error_timer : %d\n", 
			__func__, mdm2ap_status_error_timer, mdm2ap_errfatal_error_timer);
		mdm2ap_status_error_timer--;
		mdm2ap_errfatal_error_timer--;
#endif
		mdm_ops->reset_mdm_cb(mdm_drv);
		/* Update gpio configuration to "booting" config. */
		mdm_update_gpio_configs(mdev, GPIO_UPDATE_BOOTING_CONFIG);
	} else {
		mdm_drv->mdm_unexpected_reset_occurred = 0;
	}
	return 0;
}

#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
void update_failure_case(void)
{
	failure_step[failure_step_count % 100] = translate_failure();
	failure_step_count++;
	if(failure_step_count == 1) {
		root_failure_step = failure_step[0];
	} else {
		root_failure_step = get_error_step(failure_step, failure_step_count);
	}
	printk("root_failure_step : %d\n", root_failure_step);
}

int translate_failure(void)
{
 	mm_segment_t fs;	
	int modem_enumeration_step = 0;
	char *modem_enumeration_path = "/sys/module/usbcore/parameters/modem_enumeration_check";
	char buf[100];
	struct file *f;
	int ret = STEP_ERROR;
	
	f = filp_open(modem_enumeration_path, O_RDONLY, 0);

	if (f == NULL) {
        	printk(KERN_ALERT "filp_open error!!.\n");
    	} else {
		// Get current segment descriptor
		fs = get_fs();
		// Set segment descriptor associated to kernel space
		set_fs(get_ds());
		// Read the file
		f->f_op->read(f, buf, 100, &f->f_pos);
		// Restore segment descriptor
		set_fs(fs);
		// See what we read from file
		printk(KERN_INFO "buf:%s\n",buf);
	}

	modem_enumeration_step = buf[0] - '0';

	printk(KERN_ALERT "modem_enumeration_step : %d, modem_boot_check : %d\n", modem_enumeration_step, modem_boot_check);

	filp_close(f, NULL);

	if(modem_enumeration_step == 0x0) {
		if(modem_boot_check == 0x1) ret = STEP_PBL_FAIL;
		if(modem_boot_check == 0x3) ret = STEP_9008_FAIL;
	} else if (modem_enumeration_step == 0x1) {
		if(modem_boot_check == 0x3) ret = STEP_UNMEASURABLE_FAIL;
		if(modem_boot_check == 0x7) ret = STEP_9048_FAIL;
	} else if (modem_enumeration_step == 0x3) {
		if(modem_boot_check == 0x7) ret = STEP_CRASH;
	}

	if (mdm2ap_status_error_timer == 3) ret = STEP_9048_FAIL;      // hardcoding : sometimes mismatched pattern discovered.
	if (mdm2ap_errfatal_error_timer == 3) ret = STEP_CRASH;	// hardcoding : sometimes mismatched pattern discovered.

	return ret;
}

int get_error_step(int *phase, int phase_size) 
{
	int i=0;
	int final_result;
	
	final_result = phase[0];

	for(i=1; i<phase_size; i++)
	{
		if(phase[i] < final_result)
		{
			final_result = phase[i];
		}
	}

	return final_result;
}
#endif

static int mdm_subsys_powerup(const struct subsys_desc *crashed_subsys)
{
	struct mdm_device *mdev =
		container_of(crashed_subsys, struct mdm_device,
					 mdm_subsys);
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;

	pr_debug("%s: ssr on modem id %d\n",
			 __func__, mdev->mdm_data.device_id);

	gpio_direction_output(mdm_drv->ap2mdm_errfatal_gpio, 0);
	gpio_direction_output(mdm_drv->ap2mdm_status_gpio, 1);

	if (mdm_drv->pdata->ps_hold_delay_ms > 0)
		msleep(mdm_drv->pdata->ps_hold_delay_ms);

#if defined(CONFIG_MACH_APQ8064_OMEGAR_KR) || defined(CONFIG_MACH_APQ8064_ALTEV)
	if((mdm2ap_status_error_timer == 3 || mdm2ap_status_error_timer <= 0) &&
		(mdm2ap_errfatal_error_timer == 3 || mdm2ap_errfatal_error_timer <= 0)) {
		update_failure_case();
	}
	modem_boot_check = 0x1;
	printk("%s : mdm2ap_status_error_timer : %d, mdm2ap_errfatal_error_timer : %d", 
		__func__, mdm2ap_status_error_timer, mdm2ap_errfatal_error_timer);
	mdm2ap_status_error_timer--;
	mdm2ap_errfatal_error_timer--;
#endif

	mdm_ops->power_on_mdm_cb(mdm_drv);
	mdm_drv->boot_type = CHARM_NORMAL_BOOT;
	complete(&mdev->mdm_needs_reload);
	if (!wait_for_completion_timeout(&mdev->mdm_boot,
			msecs_to_jiffies(MDM_BOOT_TIMEOUT))) {
		mdm_drv->mdm_boot_status = -ETIMEDOUT;
		pr_info("%s: mdm modem restart timed out.\n", __func__);
	} else {
		pr_info("%s: id %d: mdm modem has been restarted\n",
				__func__, mdm_drv->device_id);

		/* Log the reason for the restart */
		if (mdm_drv->pdata->sfr_query)
			queue_work(mdev->mdm_sfr_queue, &mdev->sfr_reason_work);
	}
	init_completion(&mdev->mdm_boot);
	return mdm_drv->mdm_boot_status;
}

static int mdm_subsys_ramdumps(int want_dumps,
				const struct subsys_desc *crashed_subsys)
{
	struct mdm_device *mdev =
		container_of(crashed_subsys, struct mdm_device,
					 mdm_subsys);
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;

	pr_debug("%s: ssr on modem id %d\n", __func__,
			 mdev->mdm_data.device_id);

	mdm_drv->mdm_ram_dump_status = 0;
	cancel_delayed_work(&mdev->mdm2ap_status_check_work);
	if (want_dumps) {
		mdm_drv->boot_type = CHARM_RAM_DUMPS;
		complete(&mdev->mdm_needs_reload);
		if (!wait_for_completion_timeout(&mdev->mdm_ram_dumps,
				msecs_to_jiffies(mdev->dump_timeout_ms))) {
			mdm_drv->mdm_ram_dump_status = -ETIMEDOUT;
			pr_err("%s: mdm modem ramdumps timed out.\n",
					__func__);
		} else
			pr_info("%s: mdm modem ramdumps completed.\n",
					__func__);
		init_completion(&mdev->mdm_ram_dumps);
		if (!mdm_drv->pdata->no_powerdown_after_ramdumps) {
			mdm_ops->power_down_mdm_cb(mdm_drv);
			/* Update gpio configuration to "booting" config. */
			mdm_update_gpio_configs(mdev,
						GPIO_UPDATE_BOOTING_CONFIG);
		}
	}
	return mdm_drv->mdm_ram_dump_status;
}

/* Once the gpios are sent to RPM and debugging
 * starts, there is no way to stop it without
 * rebooting the device.
 */
static int mdm_debug_mask_set(void *data, u64 val)
{
	if (!vddmin_gpios_sent &&
		(val & MDM_DEBUG_MASK_VDDMIN_SETUP)) {
		mdm_setup_vddmin_gpios();
		vddmin_gpios_sent = 1;
	}

	mdm_debug_mask = val;
	if (mdm_ops->debug_state_changed_cb)
		mdm_ops->debug_state_changed_cb(mdm_debug_mask);
	return 0;
}

static int mdm_debug_mask_get(void *data, u64 *val)
{
	*val = mdm_debug_mask;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(mdm_debug_mask_fops,
			mdm_debug_mask_get,
			mdm_debug_mask_set, "%llu\n");

static int mdm_debugfs_init(void)
{
	struct dentry *dent;

	dent = debugfs_create_dir("mdm_dbg", 0);
	if (IS_ERR(dent))
		return PTR_ERR(dent);

	debugfs_create_file("debug_mask", 0644, dent, NULL,
			&mdm_debug_mask_fops);
	return 0;
}

static const struct file_operations mdm_modem_fops = {
	.owner		= THIS_MODULE,
	.open		= mdm_modem_open,
	.unlocked_ioctl	= mdm_modem_ioctl,
};

static void mdm_modem_initialize_data(struct platform_device *pdev,
						struct mdm_device *mdev)
{
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;
	struct resource *pres;

	mdm_drv->pdata    = pdev->dev.platform_data;
	if (pdev->id < 0)
		mdm_drv->device_id   = 0;
	else
		mdm_drv->device_id   = pdev->id;

	memset((void *)&mdev->mdm_subsys, 0,
		   sizeof(struct subsys_desc));
	if (mdm_drv->pdata->subsys_name) {
		strlcpy(mdev->subsys_name, mdm_drv->pdata->subsys_name,
				sizeof(mdev->subsys_name));
	} else {
		if (mdev->mdm_data.device_id <= 0)
			snprintf(mdev->subsys_name, sizeof(mdev->subsys_name),
				"%s",  EXTERNAL_MODEM);
		else
			snprintf(mdev->subsys_name, sizeof(mdev->subsys_name),
				"%s.%d",  EXTERNAL_MODEM,
				mdev->mdm_data.device_id);
	}
	mdev->mdm_subsys.shutdown = mdm_subsys_shutdown;
	mdev->mdm_subsys.ramdump = mdm_subsys_ramdumps;
	mdev->mdm_subsys.powerup = mdm_subsys_powerup;
	mdev->mdm_subsys.name = mdev->subsys_name;

	memset((void *)&mdev->misc_device, 0,
		   sizeof(struct miscdevice));
	if (mdev->mdm_data.device_id <= 0)
		snprintf(mdev->device_name, sizeof(mdev->device_name),
			 "%s",  DEVICE_BASE_NAME);
	else
		snprintf(mdev->device_name, sizeof(mdev->device_name),
			 "%s%d",  DEVICE_BASE_NAME, mdev->mdm_data.device_id);
	mdev->misc_device.minor	= MISC_DYNAMIC_MINOR;
	mdev->misc_device.name	= mdev->device_name;
	mdev->misc_device.fops	= &mdm_modem_fops;

	memset((void *)&mdev->mdm_panic_blk, 0,
		   sizeof(struct notifier_block));
	mdev->mdm_panic_blk.notifier_call  = mdm_panic_prep;
	atomic_notifier_chain_register(&panic_notifier_list,
				   &mdev->mdm_panic_blk);

	/* MDM2AP_ERRFATAL */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"MDM2AP_ERRFATAL");
	mdm_drv->mdm2ap_errfatal_gpio = pres ? pres->start : -1;

	/* AP2MDM_ERRFATAL */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"AP2MDM_ERRFATAL");
	mdm_drv->ap2mdm_errfatal_gpio = pres ? pres->start : -1;

	/* MDM2AP_STATUS */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"MDM2AP_STATUS");
	mdm_drv->mdm2ap_status_gpio = pres ? pres->start : -1;

	/* AP2MDM_STATUS */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"AP2MDM_STATUS");
	mdm_drv->ap2mdm_status_gpio = pres ? pres->start : -1;

	/* MDM2AP_WAKEUP */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"MDM2AP_WAKEUP");
	mdm_drv->mdm2ap_wakeup_gpio = pres ? pres->start : -1;

	/* AP2MDM_WAKEUP */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"AP2MDM_WAKEUP");
	mdm_drv->ap2mdm_wakeup_gpio = pres ? pres->start : -1;

	/* AP2MDM_SOFT_RESET */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"AP2MDM_SOFT_RESET");
	mdm_drv->ap2mdm_soft_reset_gpio = pres ? pres->start : -1;

	/* AP2MDM_KPDPWR_N */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"AP2MDM_KPDPWR_N");
	mdm_drv->ap2mdm_kpdpwr_n_gpio = pres ? pres->start : -1;

	/* AP2MDM_PMIC_PWR_EN */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"AP2MDM_PMIC_PWR_EN");
	mdm_drv->ap2mdm_pmic_pwr_en_gpio = pres ? pres->start : -1;

	/* MDM2AP_PBLRDY */
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"MDM2AP_PBLRDY");
	mdm_drv->mdm2ap_pblrdy = pres ? pres->start : -1;

	/*USB_SW*/
	pres = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"USB_SW");
	mdm_drv->usb_switch_gpio = pres ? pres->start : -1;

	mdm_drv->boot_type                  = CHARM_NORMAL_BOOT;

	mdev->dump_timeout_ms = mdm_drv->pdata->ramdump_timeout_ms > 0 ?
		mdm_drv->pdata->ramdump_timeout_ms : MDM_RDUMP_TIMEOUT;

	init_completion(&mdev->mdm_needs_reload);
	init_completion(&mdev->mdm_boot);
	init_completion(&mdev->mdm_ram_dumps);

	mdev->first_boot = 1;
	mutex_init(&mdm_drv->peripheral_status_lock);
}

static void mdm_deconfigure_ipc(struct mdm_device *mdev)
{
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;

	gpio_free(mdm_drv->ap2mdm_status_gpio);
	gpio_free(mdm_drv->ap2mdm_errfatal_gpio);
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_kpdpwr_n_gpio))
		gpio_free(mdm_drv->ap2mdm_kpdpwr_n_gpio);
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_pmic_pwr_en_gpio))
		gpio_free(mdm_drv->ap2mdm_pmic_pwr_en_gpio);
	gpio_free(mdm_drv->mdm2ap_status_gpio);
	gpio_free(mdm_drv->mdm2ap_errfatal_gpio);
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_soft_reset_gpio))
		gpio_free(mdm_drv->ap2mdm_soft_reset_gpio);

	if (GPIO_IS_VALID(mdm_drv->ap2mdm_wakeup_gpio))
		gpio_free(mdm_drv->ap2mdm_wakeup_gpio);

	if (mdev->mdm_queue) {
		destroy_workqueue(mdev->mdm_queue);
		mdev->mdm_queue = NULL;
	}
	if (mdev->mdm_sfr_queue) {
		destroy_workqueue(mdev->mdm_sfr_queue);
		mdev->mdm_sfr_queue = NULL;
	}
}

static int mdm_configure_ipc(struct mdm_device *mdev)
{
	struct mdm_modem_drv *mdm_drv = &mdev->mdm_data;
	int ret = -1, irq;

	gpio_request(mdm_drv->ap2mdm_status_gpio, "AP2MDM_STATUS");
	gpio_request(mdm_drv->ap2mdm_errfatal_gpio, "AP2MDM_ERRFATAL");
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_kpdpwr_n_gpio))
		gpio_request(mdm_drv->ap2mdm_kpdpwr_n_gpio, "AP2MDM_KPDPWR_N");
	gpio_request(mdm_drv->mdm2ap_status_gpio, "MDM2AP_STATUS");
	gpio_request(mdm_drv->mdm2ap_errfatal_gpio, "MDM2AP_ERRFATAL");
	if (GPIO_IS_VALID(mdm_drv->mdm2ap_pblrdy))
		gpio_request(mdm_drv->mdm2ap_pblrdy, "MDM2AP_PBLRDY");

	if (GPIO_IS_VALID(mdm_drv->ap2mdm_pmic_pwr_en_gpio))
		gpio_request(mdm_drv->ap2mdm_pmic_pwr_en_gpio,
					 "AP2MDM_PMIC_PWR_EN");
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_soft_reset_gpio))
		gpio_request(mdm_drv->ap2mdm_soft_reset_gpio,
					 "AP2MDM_SOFT_RESET");

	if (GPIO_IS_VALID(mdm_drv->ap2mdm_wakeup_gpio))
		gpio_request(mdm_drv->ap2mdm_wakeup_gpio, "AP2MDM_WAKEUP");

	if (GPIO_IS_VALID(mdm_drv->usb_switch_gpio)) {
		if (gpio_request(mdm_drv->usb_switch_gpio, "USB_SW")) {
			pr_err("%s Failed to get usb switch gpio\n", __func__);
			mdm_drv->usb_switch_gpio = -1;
		}
	}

	gpio_direction_output(mdm_drv->ap2mdm_status_gpio, 0);
	gpio_direction_output(mdm_drv->ap2mdm_errfatal_gpio, 0);

	if (GPIO_IS_VALID(mdm_drv->ap2mdm_wakeup_gpio))
		gpio_direction_output(mdm_drv->ap2mdm_wakeup_gpio, 0);

	gpio_direction_input(mdm_drv->mdm2ap_status_gpio);
	gpio_direction_input(mdm_drv->mdm2ap_errfatal_gpio);

	mdev->mdm_queue = alloc_workqueue("mdm_queue", 0, 0);
	if (!mdev->mdm_queue) {
		pr_err("%s: could not create mdm_queue for mdm id %d\n",
			   __func__, mdev->mdm_data.device_id);
		ret = -ENOMEM;
		goto fatal_err;
	}

	mdev->mdm_sfr_queue = alloc_workqueue("mdm_sfr_queue", 0, 0);
	if (!mdev->mdm_sfr_queue) {
		pr_err("%s: could not create mdm_sfr_queue for mdm id %d\n",
			   __func__, mdev->mdm_data.device_id);
		ret = -ENOMEM;
		goto fatal_err;
	}

	/* Register subsystem handlers */
	mdev->mdm_subsys_dev = subsys_register(&mdev->mdm_subsys);
	if (IS_ERR(mdev->mdm_subsys_dev)) {
		ret = PTR_ERR(mdev->mdm_subsys_dev);
		goto fatal_err;
	}
	memset((void *)&mdev->ssr_notifier_blk, 0,
			sizeof(struct notifier_block));
	mdev->ssr_notifier_blk.notifier_call  = ssr_notifier_cb;
	subsys_notif_register_notifier(mdev->subsys_name,
			&mdev->ssr_notifier_blk);

	/* ERR_FATAL irq. */
	irq = MSM_GPIO_TO_INT(mdm_drv->mdm2ap_errfatal_gpio);
	if (irq < 0) {
		pr_err("%s: bad MDM2AP_ERRFATAL IRQ resource, err = %d\n",
			   __func__, irq);
		goto errfatal_err;
	}
	ret = request_irq(irq, mdm_errfatal,
			IRQF_TRIGGER_RISING , "mdm errfatal", mdev);

	if (ret < 0) {
		pr_err("%s: MDM2AP_ERRFATAL IRQ#%d request failed, err=%d\n",
					__func__, irq, ret);
		goto errfatal_err;
	}
	mdev->mdm_errfatal_irq = irq;

errfatal_err:

	 /* status irq */
	irq = MSM_GPIO_TO_INT(mdm_drv->mdm2ap_status_gpio);
	if (irq < 0) {
		pr_err("%s: bad MDM2AP_STATUS IRQ resource, err = %d\n",
				__func__, irq);
		goto status_err;
	}

	ret = request_threaded_irq(irq, NULL, mdm_status_change,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_SHARED,
		"mdm status", mdev);

	if (ret < 0) {
		pr_err("%s: MDM2AP_STATUS IRQ#%d request failed, err=%d",
			 __func__, irq, ret);
		goto status_err;
	}
	mdev->mdm_status_irq = irq;

status_err:
	if (GPIO_IS_VALID(mdm_drv->mdm2ap_pblrdy)) {
		irq = MSM_GPIO_TO_INT(mdm_drv->mdm2ap_pblrdy);
		if (irq < 0) {
			pr_err("%s: could not get MDM2AP_PBLRDY IRQ resource\n",
				 __func__);
			goto pblrdy_err;
		}

		ret = request_threaded_irq(irq, NULL, mdm_pblrdy_change,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_SHARED,
				"mdm pbl ready", mdev);

		if (ret < 0) {
			pr_err("%s: MDM2AP_PBL IRQ#%d request failed error=%d\n",
				__func__, irq, ret);
			goto pblrdy_err;
		}
		mdev->mdm_pblrdy_irq = irq;
	}

pblrdy_err:
	/*
	 * If AP2MDM_PMIC_PWR_EN gpio is used, pull it high. It remains
	 * high until the whole phone is shut down.
	 */
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_pmic_pwr_en_gpio))
		gpio_direction_output(mdm_drv->ap2mdm_pmic_pwr_en_gpio, 1);

	return 0;

fatal_err:
	mdm_deconfigure_ipc(mdev);
	return ret;
}

static int __devinit mdm_modem_probe(struct platform_device *pdev)
{
	struct mdm_device *mdev = NULL;
	int ret = -1;

	mdev = kzalloc(sizeof(struct mdm_device), GFP_KERNEL);
	if (!mdev) {
		pr_err("%s: kzalloc fail.\n", __func__);
		ret = -ENOMEM;
		goto init_err;
	}

	platform_set_drvdata(pdev, mdev);
	mdm_modem_initialize_data(pdev, mdev);

	if (mdm_ops->debug_state_changed_cb)
		mdm_ops->debug_state_changed_cb(mdm_debug_mask);

	if (mdm_configure_ipc(mdev)) {
		pr_err("%s: mdm_configure_ipc failed, id = %d\n",
			   __func__, mdev->mdm_data.device_id);
		goto init_err;
	}

	pr_debug("%s: Registering mdm id %d\n", __func__,
			mdev->mdm_data.device_id);
	ret = misc_register(&mdev->misc_device);
	if (ret) {
		pr_err("%s: failed registering mdm id %d, ret = %d\n",
			   __func__, mdev->mdm_data.device_id, ret);
		mdm_deconfigure_ipc(mdev);
		goto init_err;
	} else {
		pr_err("%s: registered mdm id %d\n",
			   __func__, mdev->mdm_data.device_id);

		mdm_device_list_add(mdev);
		INIT_DELAYED_WORK(&mdev->mdm2ap_status_check_work,
					mdm2ap_status_check);
		INIT_WORK(&mdev->mdm_status_work, mdm_status_fn);
		INIT_WORK(&mdev->sfr_reason_work, mdm_restart_reason_fn);

		/* Perform early powerup of the external modem in order to
		 * allow tabla devices to be found.
		 */
		if (mdev->mdm_data.pdata->early_power_on)
			mdm_ops->power_on_mdm_cb(&mdev->mdm_data);
	}

	mutex_init(&restart_lock);

	return ret;

init_err:
	kfree(mdev);
	return ret;
}

static int __devexit mdm_modem_remove(struct platform_device *pdev)
{
	int ret;
	struct mdm_device *mdev = platform_get_drvdata(pdev);

	pr_debug("%s: removing device id %d\n",
			__func__, mdev->mdm_data.device_id);
	mdm_deconfigure_ipc(mdev);
	ret = misc_deregister(&mdev->misc_device);
	mdm_device_list_remove(mdev);
	kfree(mdev);
	mutex_destroy(&restart_lock);

	return ret;
}

static void mdm_modem_shutdown(struct platform_device *pdev)
{
	struct mdm_modem_drv *mdm_drv;
	struct mdm_device *mdev = platform_get_drvdata(pdev);

	pr_debug("%s: shutting down device id %d\n",
		 __func__, mdev->mdm_data.device_id);

	mdm_disable_irqs(mdev);
	mdm_drv = &mdev->mdm_data;
	mdm_ops->power_down_mdm_cb(mdm_drv);
	if (GPIO_IS_VALID(mdm_drv->ap2mdm_pmic_pwr_en_gpio))
		gpio_direction_output(mdm_drv->ap2mdm_pmic_pwr_en_gpio, 0);
}

// kyle00.choi, 20140411, RF Device Check [START]	
#ifdef CONFIG_MACH_APQ8064_ALTEV
char ssr_noti[MAX_SSR_REASON_LEN];
static void log_modem_sfr(void)
{
	u32 size;
	char *smem_reason, reason[MAX_SSR_REASON_LEN];

	smem_reason = smem_get_entry(SMEM_SSR_REASON_MSS0, &size);
	if (!smem_reason || !size) {
		pr_err("GSS subsystem failure reason: (unknown, smem_get_entry failed).\n");
		return;
	}
	if (!smem_reason[0]) {
		pr_err("GSS subsystem failure reason: (unknown, init string found).\n");
		return;
	}

	size = min(size, MAX_SSR_REASON_LEN-1);
	memcpy(reason, smem_reason, size);

	memcpy(ssr_noti, smem_reason, size);
	ssr_noti[size] = '\0';

	reason[size] = '\0';
	pr_err("GSS subsystem failure reason: %s.\n", reason);

	smem_reason[0] = '\0';
	wmb();
}
#endif
// kyle00.choi, 20140411, RF Device Check [END]

static struct of_device_id mdm_match_table[] = {
	{.compatible = "qcom,mdm2_modem,mdm2_modem.1"},
	{},
};

static struct platform_driver mdm_modem_driver = {
	.probe    = mdm_modem_probe,
	.remove   = __devexit_p(mdm_modem_remove),
	.shutdown = mdm_modem_shutdown,
	.driver         = {
		.name = "mdm2_modem",
		.owner = THIS_MODULE,
		.of_match_table = mdm_match_table,
	},
};

static int __init mdm_modem_init(void)
{
	int ret;

	ret = mdm_get_ops(&mdm_ops);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&mdm_devices);
	mdm_debugfs_init();
	return platform_driver_register(&mdm_modem_driver);
}

static void __exit mdm_modem_exit(void)
{
	platform_driver_unregister(&mdm_modem_driver);
}

module_init(mdm_modem_init);
module_exit(mdm_modem_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("mdm modem driver");
MODULE_VERSION("2.0");
MODULE_ALIAS("mdm_modem");
