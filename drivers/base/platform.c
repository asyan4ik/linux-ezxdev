/*
 * platform.c - platform 'psuedo' bus for legacy devices
 *
 * Please see Documentation/driver-model/platform.txt for more
 * information.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>

static struct device legacy_bus = {
	.name		= "legacy bus",
	.bus_id		= "legacy",
};

/**
 *	platform_device_register - add a platform-level device
 *	@dev:	platform device we're adding
 *
 */
int platform_device_register(struct platform_device * pdev)
{
	if (!pdev)
		return -EINVAL;

	if (!pdev->dev.parent)
		pdev->dev.parent = &legacy_bus;

	pdev->dev.bus = &platform_bus_type;
	
	snprintf(pdev->dev.bus_id,BUS_ID_SIZE,"%s%u",pdev->name,pdev->id);

	pr_debug("Registering platform device '%s'. Parent at %s\n",
		 pdev->dev.bus_id,pdev->dev.parent->bus_id);
	return device_register(&pdev->dev);
}

void platform_device_unregister(struct platform_device * pdev)
{
	if (pdev)
		device_unregister(&pdev->dev);
}
	
static int platform_match(struct device * dev, struct device_driver * drv)
{
	return 0;
}

struct bus_type platform_bus_type = {
	.name		= "platform",
	.match		= platform_match,
};

#if 1 /* linux-pm */
int __init platform_bus_init(void)
#else
static int __init platform_bus_init(void)
#endif
{
	device_register(&legacy_bus);
	return bus_register(&platform_bus_type);
}

#if 0 /* linux-pm */
postcore_initcall(platform_bus_init);
#endif

EXPORT_SYMBOL(platform_device_register);
EXPORT_SYMBOL(platform_device_unregister);
