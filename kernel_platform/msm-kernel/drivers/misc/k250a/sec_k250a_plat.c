/*
 * Copyright (C) 2020 Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/ioctl.h>
#include <linux/gpio.h>
#include <linux/version.h>
#include <linux/clk.h>
#include <linux/platform_device.h>

#include "sec_k250a.h"

#define ERR(msg...)		pr_err("[star-k250a] : " msg)
#define INFO(msg...)	pr_info("[star-k250a] : " msg)

struct k250a_dev {
	struct regulator *vdd;
	struct mutex platform_lock;
	bool platform_opened;
	struct miscdevice misc;

#ifdef CONFIG_SEC_SNVM_I2C_CLOCK_CONTROL
	struct clk *i2c_main_clk;
	struct clk *i2c_dma_clk;
	bool i2c_main_clk_enabled;
	bool i2c_dma_clk_enabled;
#endif
};

#ifdef CONFIG_SEC_SNVM_I2C_CLOCK_CONTROL
static int k250a_parse_i2c_clock(struct k250a_dev *k250a, struct device_node *np)
{
	struct device_node *i2c_np;

	i2c_np = of_parse_phandle(np, "i2c_node", 0);
	if (!i2c_np) {
		INFO("i2c_node not found\n");
		return -ENODEV;
	}

	k250a->i2c_main_clk = of_clk_get_by_name(i2c_np, "main");
	if (IS_ERR(k250a->i2c_main_clk)) {
		INFO("failed to get i2c main clock\n");
		return PTR_ERR(k250a->i2c_main_clk);
	}
	INFO("i2c main clock found");

	k250a->i2c_dma_clk = of_clk_get_by_name(i2c_np, "dma");
	if (IS_ERR(k250a->i2c_dma_clk)) {
		INFO("failed to get i2c dma clock\n");
		return PTR_ERR(k250a->i2c_dma_clk);
	}
	INFO("i2c dma clock found");

	return 0;
}

static void k250a_i2c_clock_enable(struct k250a_dev *k250a)
{
	int ret = 0;

	INFO("%s\n", __func__);
	if (!k250a->i2c_main_clk_enabled && !IS_ERR_OR_NULL(k250a->i2c_main_clk)) {
		ret = clk_prepare_enable(k250a->i2c_main_clk);
		if (ret)
			ERR("failed to enable i2c main clock\n");
		else
			k250a->i2c_main_clk_enabled = true;
	}

	if (!k250a->i2c_dma_clk_enabled && !IS_ERR_OR_NULL(k250a->i2c_dma_clk)) {
		ret = clk_prepare_enable(k250a->i2c_dma_clk);
		if (ret)
			ERR("failed to enable i2c dma clock\n");
		else
			k250a->i2c_dma_clk_enabled = true;
	}
}

static void k250a_i2c_clock_disable(struct k250a_dev *k250a)
{
	INFO("%s\n", __func__);
	if (k250a->i2c_main_clk_enabled && !IS_ERR_OR_NULL(k250a->i2c_main_clk)) {
		clk_disable_unprepare(k250a->i2c_main_clk);
		k250a->i2c_main_clk_enabled = false;
	}

	if (k250a->i2c_dma_clk_enabled && !IS_ERR_OR_NULL(k250a->i2c_dma_clk)) {
		clk_disable_unprepare(k250a->i2c_dma_clk);
		k250a->i2c_dma_clk_enabled = false;
	}
}
#endif

static int k250a_poweron(struct k250a_dev *k250a)
{
	int ret = 0;

	INFO("%s\n", __func__);

	if (IS_ERR_OR_NULL(k250a->vdd)) {
		ERR("%s VDD is not ready\n", __func__);
		return -ENODEV;
	}

	ret = regulator_is_enabled(k250a->vdd);
	if (ret < 0) {
		INFO("%s err on regulator_is_enabled %d\n", __func__, ret);
		return ret;
	}
	if (!ret) {
		ret = regulator_enable(k250a->vdd);
		if (ret) {
			ERR("%s - enable failed, ret=%d\n", __func__, ret);
			return ret;
		}
	} else {
		INFO("%s power is already on\n", __func__);
	}

	msleep(POWER_ON_DELAY_MS);

	return 0;
}

static int k250a_poweroff(struct k250a_dev *k250a)
{
	int ret = 0;

	INFO("%s\n", __func__);

	if (IS_ERR_OR_NULL(k250a->vdd)) {
		ERR("%s VDD is not ready\n", __func__);
		return -ENODEV;
	}

	ret = regulator_is_enabled(k250a->vdd);
	if (ret < 0) {
		INFO("%s err on regulator_is_enabled %d\n", __func__, ret);
		return ret;
	}
	if (ret > 0) {
		ret = regulator_disable(k250a->vdd);
		if (ret) {
			ERR("%s - disable failed, ret=%d\n", __func__, ret);
			return ret;
		}
	} else {
		INFO("%s power is already off\n", __func__);
	}

	usleep_range(POWER_OF_DELAY_US, POWER_OF_DELAY_US + 5000);

	return 0;
}

static int k250a_dev_open(struct inode *inode, struct file *filp)
{
	struct k250a_dev *k250a = container_of(filp->private_data, struct k250a_dev, misc);
	int ret = 0;

	mutex_lock(&(k250a->platform_lock));
	INFO("%s: %d\n", __func__, k250a->platform_opened);

	if (k250a->platform_opened) {
		ERR("%s already opened\n", __func__);
		ret = -EBUSY;
		goto open_end;
	}

	k250a->platform_opened = true;
	filp->private_data = k250a;

	ret = k250a_poweron(k250a);
	if (ret)
		goto open_end;

#ifdef CONFIG_SEC_SNVM_I2C_CLOCK_CONTROL
	k250a_i2c_clock_enable(k250a);
#endif

open_end:
	mutex_unlock(&(k250a->platform_lock));

	return ret;
}

static int k250a_dev_release(struct inode *inode, struct file *filp)
{
	struct k250a_dev *k250a = filp->private_data;
	int ret = 0;

	mutex_lock(&(k250a->platform_lock));
	INFO("%s: %d\n", __func__, k250a->platform_opened);

	if (!k250a->platform_opened) {
		ERR("%s already closed\n", __func__);
		goto release_end;
	}

#ifdef CONFIG_SEC_SNVM_I2C_CLOCK_CONTROL
	k250a_i2c_clock_disable(k250a);
#endif
	ret = k250a_poweroff(k250a);
	k250a->platform_opened = false;

release_end:
	mutex_unlock(&(k250a->platform_lock));

	return ret;
}

static const struct file_operations k250a_dev_fops = {
	.owner = THIS_MODULE,
	.open = k250a_dev_open,
	.release = k250a_dev_release,
};

static int k250a_parse_dt_for_platform_device(struct k250a_dev *k250a, struct device *dev)
{
	int ret = 0;

	k250a->vdd = devm_regulator_get_optional(dev, "1p8_pvdd");
	if (IS_ERR_OR_NULL(k250a->vdd)) {
		if (PTR_ERR(k250a->vdd) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		ERR("%s - 1p8_pvdd can not be used\n", __func__);
	} else {
		INFO("%s: regulator_get success\n", __func__);
	}

#ifdef CONFIG_SEC_SNVM_I2C_CLOCK_CONTROL
	ret = k250a_parse_i2c_clock(k250a, dev->of_node);
#endif

	return ret;
}

static int k250a_platform_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct k250a_dev *k250a;

	k250a = devm_kzalloc(&pdev->dev, sizeof(struct k250a_dev), GFP_KERNEL);
	if (!k250a) {
		ERR("%s failed to allocate memory\n", __func__);
		return -ENOMEM;
	}

	ret = k250a_parse_dt_for_platform_device(k250a, &pdev->dev);
	if (ret == -EPROBE_DEFER) {
		ERR("%s EPROBE_DEFER\n", __func__);
		return ret;
	} else if (ret) {
		ERR("%s parse dt failed\n", __func__);
		return ret;
	}

	k250a->platform_opened = false;
	mutex_init(&(k250a->platform_lock));

	k250a->misc.minor = MISC_DYNAMIC_MINOR;
	k250a->misc.name = "k250a";
	k250a->misc.fops = &k250a_dev_fops;

	ret = misc_register(&k250a->misc);
	if (ret)
		ERR("misc_register failed! %d\n", ret);

	dev_set_drvdata(&pdev->dev, k250a);

	INFO("%s: finished...\n", __func__);

	return ret;
}

static int k250a_platform_remove(struct platform_device *pdev)
{
	struct k250a_dev *k250a = dev_get_drvdata(&pdev->dev);

	INFO("Entry : %s\n", __func__);
	misc_deregister(&k250a->misc);
	mutex_destroy(&(k250a->platform_lock));

	return 0;
}

static const struct of_device_id k250a_secure_match_table[] = {
	{ .compatible = "sec_k250a_platform_misc",},
	{},
};

static struct platform_driver k250a_platform_driver = {
	.driver = {
		.name = "k250a_platform_misc",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = k250a_secure_match_table,
#endif
	},
	.probe = k250a_platform_probe,
	.remove = k250a_platform_remove,
};

static int __init k250a_platform_init(void)
{
	int ret = 0;

	INFO("Entry : %s\n", __func__);
	ret = platform_driver_register(&k250a_platform_driver);
	if (ret)
		ERR("platform_driver_register fail : %s\n", __func__);

	return ret;
}
module_init(k250a_platform_init);

static void __exit k250a_exit(void)
{
	INFO("Entry : %s\n", __func__);
	platform_driver_unregister(&k250a_platform_driver);
}

module_exit(k250a_exit);

MODULE_AUTHOR("Sec");
MODULE_DESCRIPTION("K250A driver");
MODULE_LICENSE("GPL");
