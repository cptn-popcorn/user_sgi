/*
 * experimental driver to allow tracking software generated interrupts (SGIs)
 * from user space code (known as IPI in the kernel),
 * similiar to the generic UIO driver for hardware interrupts
 *
 * Copyright (C) 2016 Ellisys, SA
 * 
 * current limitation: only one instance is supported - the parameterless handler callback
 * declared in smp.h does not pass the source SGI number along and would require
 * a cumbersome workaround, having distinct handler functions for each possible SGI
 *
 * countrary to the UIO driver, our pollable device attribute 'count' is not in the /dev file-system
 * but directly available in /sys, exploiting the sysfs notification mechanism and
 * thus realizing the same functionality with very few lines of code
 *
 * this platform driver can be instantiated in the device tree like this:
 *
 * user_sgi@1 {
 *         ipi_number = <8>;
 *         compatible="ellisys,user-sgi-1.0";
 * };
 *
 * and polled from user space code like this:
 *
 * int fd;
 * int pollret;
 * struct pollfd fds { .fd = fd, .events = POLLPRI };
 * fd = open(@"/sys/devices/soc0/user_sgi@1/count", O_RDONLY);
 * while(1) {
 *   lseek(fd, 0, SEEK_SET);
 *   int buf;
 *   read(fd, &buf, sizeof(buf));
 *   pollret = poll(&fds, 1, -1);
 *   ...
 * }
 *
 * you can expect fds.revents to contain POLLPRI and POLLERR each time the SGI is triggered
 *
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/smp.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <asm/barrier.h>
#include <asm/page.h>

#define DRIVER_NAME "user_sgi"
#define IPI_NUMBER_NAME "ipi_number"

static struct device *ipi_dev = NULL;
static u32 ipi_count = 0;

struct user_sgi_data
{
	u32 ipi_number;
};

static ssize_t count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d", smp_load_acquire(&ipi_count));
}

static DEVICE_ATTR_RO(count);

static void handle_ipi(void)
{
	struct device *dev;
	
	smp_store_release(&ipi_count, ipi_count + 1);
	dev = smp_load_acquire(&ipi_dev);
	
	if(dev == NULL)
	{
		return;
	}
	
	sysfs_notify(&dev->kobj, NULL, dev_attr_count.attr.name);
}

int allocate_device_data(struct platform_device *pdev)
{
	struct user_sgi_data *data;
	
	data = devm_kzalloc(&pdev->dev, sizeof(struct user_sgi_data), GFP_KERNEL);
	
	if(data == NULL)
	{
		return -ENOMEM;
	}
	
	platform_set_drvdata(pdev, data);
	
	return 0;
}

int get_ipi_number(struct device *dev, struct user_sgi_data* data)
{
	return of_property_read_u32(dev->of_node, "ipi_number", &data->ipi_number);
}

int release_after_ipi(struct platform_device *pdev, int result)
{
	smp_store_release(&ipi_dev, NULL);
	clear_ipi_handler(((struct user_sgi_data*)platform_get_drvdata(pdev))->ipi_number);
	return result;
}

int release_after_attribute_count(struct platform_device *pdev, int result)
{
	device_remove_file(&pdev->dev, &dev_attr_count);
	return release_after_ipi(pdev, result);
};

static int user_sgi_probe(struct platform_device *pdev)
{
	int allocate_result;
	int get_ipi_number_result;
	int create_device_result;
	struct user_sgi_data* data;

	printk(KERN_NOTICE "probing user sgi\n");
	
	allocate_result = allocate_device_data(pdev) ;
	
	if(allocate_result != 0)
	{
		return allocate_result;
	}
	
	data = platform_get_drvdata(pdev);
	
	get_ipi_number_result = get_ipi_number(&pdev->dev, data);
	
	if(get_ipi_number_result < 0)
	{
		return get_ipi_number_result;
	}

	smp_store_release(&ipi_dev, &pdev->dev);
	
	if(set_ipi_handler(data->ipi_number, handle_ipi, "user sgi") != 0)
	{
		return -EBUSY;
	}
	
	create_device_result = device_create_file(&pdev->dev, &dev_attr_count);
	
	if(create_device_result != 0)
	{
		return release_after_ipi(pdev, create_device_result);
	}
	
	printk(KERN_NOTICE "user sgi activated for IPI number %d\n", data->ipi_number);
	
	return 0;
}

static int user_sgi_remove(struct platform_device *pdev)
{
	printk(KERN_NOTICE "removing user sgi\n");
	return release_after_attribute_count(pdev, 0);
}

static struct of_device_id user_sgi_match[] =
{
	{ .compatible = "ellisys,user-sgi-1.0", },
	{},
};

static struct platform_driver user_sgi_platform_driver =
{
	.probe = user_sgi_probe,
	.remove = user_sgi_remove,

	.driver =
	{
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = user_sgi_match,
	},
};

module_platform_driver(user_sgi_platform_driver);

MODULE_AUTHOR("Hagen Hentschel <hagen.hentschel@ellisys.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("User-mode software-generated interrupt driver");
