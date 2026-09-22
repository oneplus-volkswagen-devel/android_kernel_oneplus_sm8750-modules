// SPDX-License-Identifier: GPL-2.0
/*
 * ST54SPI GPIO driver
 * Copyright (C) 2021 ST Microelectronics S.A.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *  * This program is free software; you can redistribute it and/or modify
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 **********************************************************************************/

#include <linux/version.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/of_gpio.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/rwsem.h>

/* Flag ESE_CONF_GPIO_OPEN_RELEASE is to configure nRESET GPIO in
   open function and freeing GPIO in release function */
//#define ESE_CONF_GPIO_OPEN_RELEASE

/* Flag ESE_CONF_GPIO_PROBE_REMOVE is to configure
   nRESET GPIO in probe function */
#define ESE_CONF_GPIO_PROBE_REMOVE

#define ST54SPI_GPIO__MAGIC  0xEB
#define ST54SPI_GET_GPIO	_IOW(ST54SPI_GPIO__MAGIC, 0x01, unsigned int)
#define ST54SPI_SET_GPIO	_IOW(ST54SPI_GPIO__MAGIC, 0x02, unsigned int)

struct st54spi_gpio_device {
	dev_t st54spi_gpio_dev_t;
	struct cdev c_dev;
	struct class *class;
	struct device device;
	struct spi_device *spi_dev;
	struct rw_semaphore ops_rwsem;
	bool removed;
	/* GPIO for st54spi Reset pin (output) */
	int gpiod_reset;
};

static void st54spi_gpio_dev_release_device(struct device *dev)
{
	struct st54spi_gpio_device *st54spi_gpio_dev =
		container_of(dev, struct st54spi_gpio_device, device);

	kfree(st54spi_gpio_dev);
}

/** @brief   IOCTL function  to be used to set or get data from upper layer.
 *
 *  @param   pfile  fil node for opened device.
 *  @cmd     IOCTL type from upper layer.
 *  @arg     IOCTL arg from upper layer.
 *
 *  @return 0 on success, error code for failures.
 */
long st54spi_gpio_dev_ioctl(struct file *pfile, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct st54spi_gpio_device *st54spi_gpio_dev = pfile->private_data;

	if (!st54spi_gpio_dev) {
		pr_err("%s ENODEV! st54spi_gpio_dev is NULL\n", __func__);
		return -ENODEV;
	}

	down_read(&st54spi_gpio_dev->ops_rwsem);
	if (st54spi_gpio_dev->removed) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case ST54SPI_GET_GPIO:
		ret = gpio_get_value(st54spi_gpio_dev->gpiod_reset);
		break;
	case ST54SPI_SET_GPIO:
		if ((arg == 0) || (arg == 1)) {
			gpio_set_value(st54spi_gpio_dev->gpiod_reset, arg);
			if (arg == 0) {
				usleep_range(3000, 3001);
				}
		} else {
			pr_err("%s bad arg %lu\n", __func__, arg);
			ret = -ENOIOCTLCMD;
		}
		break;
	default:
		pr_err("%s Unsupported ioctl cmd 0x%x, arg %lu\n",
						__func__, cmd, arg);
		ret = -ENOIOCTLCMD;
	}
out:
	up_read(&st54spi_gpio_dev->ops_rwsem);
	return ret;
}

/* This function will be called when we open the Device file*/

static int st54spi_gpio_dev_open(struct inode *inode, struct file *pfile)
{
#ifdef ESE_CONF_GPIO_OPEN_RELEASE
	int rc;
#endif
	struct st54spi_gpio_device *st54spi_gpio_dev = container_of(inode->i_cdev,
					struct st54spi_gpio_device, c_dev);

	pr_info("%s : Device File Opened\n", __func__);
	if (!st54spi_gpio_dev) {
		pr_err("%s ENODEV NULL\n", __func__);
		return -ENODEV;
	}
	down_read(&st54spi_gpio_dev->ops_rwsem);
	if (st54spi_gpio_dev->removed) {
		up_read(&st54spi_gpio_dev->ops_rwsem);
		return -ENODEV;
	}
#ifdef ESE_CONF_GPIO_OPEN_RELEASE
	rc = gpio_request(st54spi_gpio_dev->gpiod_reset, "gpio-power_nreset");
	if (rc < 0) {
		pr_err("%s: request gpio failed: %d\n", __func__, rc);
		goto err_unlock;
	}

	rc = gpio_direction_output(st54spi_gpio_dev->gpiod_reset, 1);
	if (rc < 0) {
		pr_err("%s: gpio cannot set the output %d\n", __func__, rc);
		gpio_free(st54spi_gpio_dev->gpiod_reset);
		goto err_unlock;
	}
#endif

	pfile->private_data = st54spi_gpio_dev;
	up_read(&st54spi_gpio_dev->ops_rwsem);
	return 0;

#ifdef ESE_CONF_GPIO_OPEN_RELEASE
err_unlock:
	up_read(&st54spi_gpio_dev->ops_rwsem);
	return -EFAULT;
#endif
}

/* This function will be called when we close the Device file*/

static int st54spi_gpio_dev_release(struct inode *inode, struct file *pfile)
{
#ifdef ESE_CONF_GPIO_OPEN_RELEASE
	struct st54spi_gpio_device *st54spi_gpio_dev = pfile->private_data;

	down_read(&st54spi_gpio_dev->ops_rwsem);
	if (!st54spi_gpio_dev->removed &&
	    gpio_is_valid(st54spi_gpio_dev->gpiod_reset))
		gpio_free(st54spi_gpio_dev->gpiod_reset);
	up_read(&st54spi_gpio_dev->ops_rwsem);
#endif
	pr_info("%s: Device File Closed\n", __func__);
	pfile->private_data = NULL;
	return 0;
}


static const struct file_operations st54spi_gpio_dev_fops = {
	.owner = THIS_MODULE,
	.llseek = no_llseek,
	.open = st54spi_gpio_dev_open,
	.release = st54spi_gpio_dev_release,
	.unlocked_ioctl = st54spi_gpio_dev_ioctl,
};

/* This function will be called to probe the character device*/
static int st54spi_gpio_probe(struct platform_device *pdev)
{
	int rc;
	struct st54spi_gpio_device *st54spi_gpio_dev;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	if (np == NULL) {
		pr_err("%s : struct np is null\n", __func__);
		return -ENOMEM;
	}

	pr_info("%s : enter", __func__);
	st54spi_gpio_dev = kzalloc(sizeof(*st54spi_gpio_dev), GFP_KERNEL);
	if (!st54spi_gpio_dev)
		return -ENOMEM;
	init_rwsem(&st54spi_gpio_dev->ops_rwsem);

	/* Setup gpio-power_nreset */
	st54spi_gpio_dev->gpiod_reset = of_get_named_gpio(np, "gpio-power_nreset", 0);
	if (!gpio_is_valid(st54spi_gpio_dev->gpiod_reset)) {
		pr_err("%s : Unable to request gpio-power_nreset\n", __func__);
		rc = -EFAULT;
		goto fail_gpio_get;
	}

#ifdef ESE_CONF_GPIO_PROBE_REMOVE
	rc = gpio_request(st54spi_gpio_dev->gpiod_reset, "gpio-power_nreset");
	if (rc < 0) {
		pr_err("%s: request gpio failed: %d\n", __func__, rc);
		rc = -EFAULT;
		goto fail_gpio_get;
	}

	rc = gpio_direction_output(st54spi_gpio_dev->gpiod_reset, 1);
	if (rc < 0) {
		pr_err("%s: gpio cannot set the output %d\n", __func__, rc);
		rc = -EFAULT;
		goto fail_gpio_request;
	}
#endif

	/* Create device node after all hardware initialization succeeds. */
	rc = alloc_chrdev_region(&st54spi_gpio_dev->st54spi_gpio_dev_t, 0, 1,
				 "st54spi_gpio");
	if (rc < 0) {
		pr_err("%s: alloc_chrdev_region() failed\n", __func__);
		goto fail_gpio_request;
	}

#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
	st54spi_gpio_dev->class = class_create("st54spi_gpio");
#else
	st54spi_gpio_dev->class = class_create(THIS_MODULE, "st54spi_gpio");
#endif

	if (IS_ERR(st54spi_gpio_dev->class)) {
		rc = PTR_ERR(st54spi_gpio_dev->class);
		pr_err("%s: Error creating st54spi_gpio_dev->class: %d\n",
		       __func__, rc);
		goto fail_class_create;
	}

	device_initialize(&st54spi_gpio_dev->device);
	st54spi_gpio_dev->device.devt = st54spi_gpio_dev->st54spi_gpio_dev_t;
	st54spi_gpio_dev->device.class = st54spi_gpio_dev->class;
	st54spi_gpio_dev->device.parent = dev;
	st54spi_gpio_dev->device.release = st54spi_gpio_dev_release_device;
	dev_set_drvdata(&st54spi_gpio_dev->device, st54spi_gpio_dev);
	rc = dev_set_name(&st54spi_gpio_dev->device, "st54spi_gpio");
	if (rc)
		goto fail_device_add;

	cdev_init(&st54spi_gpio_dev->c_dev, &st54spi_gpio_dev_fops);
	st54spi_gpio_dev->c_dev.owner = THIS_MODULE;
	rc = cdev_device_add(&st54spi_gpio_dev->c_dev,
			     &st54spi_gpio_dev->device);
	if (rc) {
		pr_err("%s: cdev_device_add failed: %d\n", __func__, rc);
		goto fail_device_add;
	}

	platform_set_drvdata(pdev, st54spi_gpio_dev);

	return 0;
fail_device_add:
	down_write(&st54spi_gpio_dev->ops_rwsem);
	st54spi_gpio_dev->removed = true;
#ifdef ESE_CONF_GPIO_PROBE_REMOVE
	gpio_free(st54spi_gpio_dev->gpiod_reset);
#endif
	up_write(&st54spi_gpio_dev->ops_rwsem);
	class_destroy(st54spi_gpio_dev->class);
	unregister_chrdev_region(st54spi_gpio_dev->st54spi_gpio_dev_t, 1);
	put_device(&st54spi_gpio_dev->device);
	return rc;

fail_class_create:
	unregister_chrdev_region(st54spi_gpio_dev->st54spi_gpio_dev_t, 1);
fail_gpio_request:
#ifdef ESE_CONF_GPIO_PROBE_REMOVE
	gpio_free(st54spi_gpio_dev->gpiod_reset);
#endif
fail_gpio_get:
	kfree(st54spi_gpio_dev);

	return rc;
}

/* This function will be called to remove the character device*/

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0))
static int st54spi_gpio_remove(struct platform_device *pdev)
#else
static void st54spi_gpio_remove(struct platform_device *pdev)
#endif
{
	struct st54spi_gpio_device *st54spi_gpio_dev;
	struct device *dev = &pdev->dev;

	pr_info("%s : enter", __func__);
	st54spi_gpio_dev = platform_get_drvdata(pdev);
	cdev_device_del(&st54spi_gpio_dev->c_dev,
			&st54spi_gpio_dev->device);
	unregister_chrdev_region(st54spi_gpio_dev->st54spi_gpio_dev_t, 1);
	class_destroy(st54spi_gpio_dev->class);
	down_write(&st54spi_gpio_dev->ops_rwsem);
	st54spi_gpio_dev->removed = true;
#ifdef ESE_CONF_GPIO_PROBE_REMOVE
	if (gpio_is_valid(st54spi_gpio_dev->gpiod_reset))
		gpio_free(st54spi_gpio_dev->gpiod_reset);
#endif
	up_write(&st54spi_gpio_dev->ops_rwsem);
	dev_set_drvdata(dev, NULL);
	put_device(&st54spi_gpio_dev->device);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0))
	return 0;
#endif

}

static const struct of_device_id st54spi_gpio_of_match[] = {
	{ .compatible = "st,st54spi_gpio", },
	{},
};
MODULE_DEVICE_TABLE(of, st54spi_gpio_of_match);

static struct platform_driver st54spi_gpio_driver = {
	.driver = {
		   .name = "st54spi_gpio",
		   .owner = THIS_MODULE,
		   .of_match_table = st54spi_gpio_of_match,
		   },
	.probe = st54spi_gpio_probe,
	.remove = st54spi_gpio_remove,
};

/* module load/unload record keeping */
static int __init st54spi_gpio_dev_init(void)
{
	pr_info("%s : Loading st54spi gpio_driver 1.0\n", __func__);
	return platform_driver_register(&st54spi_gpio_driver);
}

module_init(st54spi_gpio_dev_init);

static void __exit st54spi_gpio_dev_exit(void)
{
	pr_info("%s : Unloading st54spi gpio_driver 1.0\n", __func__);
	platform_driver_unregister(&st54spi_gpio_driver);
}

module_exit(st54spi_gpio_dev_exit);

MODULE_AUTHOR("STMicroelectronics");
MODULE_DESCRIPTION("ST54SPI GPIO driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("st54spi_gpio");
