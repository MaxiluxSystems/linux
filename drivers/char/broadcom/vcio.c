/*
 *  Copyright (C) 2010 Broadcom
 *  Copyright (C) 2015 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MBOX_CHAN_PROPERTY 8

#define VCIO_IOC_MAGIC 100
#define IOCTL_MBOX_PROPERTY _IOWR(VCIO_IOC_MAGIC, 0, char *)
#ifdef CONFIG_COMPAT
#define IOCTL_MBOX_PROPERTY32 _IOWR(VCIO_IOC_MAGIC, 0, compat_uptr_t)
#endif

static struct {
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct rpi_firmware *fw;
} vcio;

static int vcio_user_property_list(void *user)
{
	u32 *buf, size;
	int ret;

	/* The first 32-bit is the size of the buffer */
	if (copy_from_user(&size, user, sizeof(size)))
		return -EFAULT;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (copy_from_user(buf, user, size)) {
		kfree(buf);
		return -EFAULT;
	}

	/* Strip off protocol encapsulation */
	ret = rpi_firmware_property_list(vcio.fw, &buf[2], size - 12);
	if (ret) {
		kfree(buf);
		return ret;
	}

	buf[1] = RPI_FIRMWARE_STATUS_SUCCESS;
	if (copy_to_user(user, buf, size))
		ret = -EFAULT;

	kfree(buf);

	return ret;
}

static int vcio_device_open(struct inode *inode, struct file *file)
{
	try_module_get(THIS_MODULE);

	return 0;
}

static int vcio_device_release(struct inode *inode, struct file *file)
{
	module_put(THIS_MODULE);

	return 0;
}

static long vcio_device_ioctl(struct file *file, unsigned int ioctl_num,
			      unsigned long ioctl_param)
{
	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY:
		return vcio_user_property_list((void *)ioctl_param);
	default:
		pr_err("unknown ioctl: %x\n", ioctl_num);
		return -EINVAL;
	}
}

#ifdef CONFIG_COMPAT
static long vcio_device_compat_ioctl(struct file *file, unsigned int ioctl_num,
				     unsigned long ioctl_param)
{
	switch (ioctl_num) {
	case IOCTL_MBOX_PROPERTY32:
		return vcio_user_property_list(compat_ptr(ioctl_param));
	default:
		pr_err("unknown ioctl: %x\n", ioctl_num);
		return -EINVAL;
	}
}
#endif

const struct file_operations vcio_fops = {
	.unlocked_ioctl = vcio_device_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = vcio_device_compat_ioctl,
#endif
	.open = vcio_device_open,
	.release = vcio_device_release,
};

static int __init vcio_init(void)
{
	struct device_node *np;
	static struct device *dev;
	int ret;

	np = of_find_compatible_node(NULL, NULL,
				     "raspberrypi,bcm2835-firmware");
	if (!of_device_is_available(np))
		return -ENODEV;

	vcio.fw = rpi_firmware_get(np);
	if (!vcio.fw)
		return -ENODEV;

	ret = alloc_chrdev_region(&vcio.devt, 0, 1, "vcio");
	if (ret) {
		pr_err("failed to allocate device number\n");
		return ret;
	}

	cdev_init(&vcio.cdev, &vcio_fops);
	vcio.cdev.owner = THIS_MODULE;
	ret = cdev_add(&vcio.cdev, vcio.devt, 1);
	if (ret) {
		pr_err("failed to register device\n");
		goto err_unregister_chardev;
	}

	/*
	 * Create sysfs entries
	 * 'bcm2708_vcio' is used for backwards compatibility so we don't break
	 * userspace. Raspian has a udev rule that changes the permissions.
	 */
	vcio.class = class_create(THIS_MODULE, "bcm2708_vcio");
	if (IS_ERR(vcio.class)) {
		ret = PTR_ERR(vcio.class);
		pr_err("failed to create class\n");
		goto err_cdev_del;
	}

	dev = device_create(vcio.class, NULL, vcio.devt, NULL, "vcio");
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		pr_err("failed to create device\n");
		goto err_class_destroy;
	}

	return 0;

err_class_destroy:
	class_destroy(vcio.class);
err_cdev_del:
	cdev_del(&vcio.cdev);
err_unregister_chardev:
	unregister_chrdev_region(vcio.devt, 1);

	return ret;
}
module_init(vcio_init);

static void __exit vcio_exit(void)
{
	device_destroy(vcio.class, vcio.devt);
	class_destroy(vcio.class);
	cdev_del(&vcio.cdev);
	unregister_chrdev_region(vcio.devt, 1);
}
module_exit(vcio_exit);

MODULE_AUTHOR("Gray Girling");
MODULE_AUTHOR("Noralf Trønnes");
MODULE_DESCRIPTION("Mailbox userspace access");
MODULE_LICENSE("GPL");
