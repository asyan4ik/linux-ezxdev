/*
 * bus.c - bus driver management
 * 
 * Copyright (c) 2002 Patrick Mochel
 *		 2002 Open Source Development Lab
 * 
 * 
 */

#undef DEBUG

#include <linux/device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/string.h>
#include "base.h"

static DECLARE_MUTEX(bus_sem);

#define to_dev(node) container_of(node,struct device,bus_list)
#define to_drv(node) container_of(node,struct device_driver,bus_list)

#define to_bus_attr(_attr) container_of(_attr,struct bus_attribute,attr)
#define to_bus(obj) container_of(obj,struct bus_type,subsys.kobj)

/*
 * sysfs bindings for drivers
 */

#define to_drv_attr(_attr) container_of(_attr,struct driver_attribute,attr)
#define to_driver(obj) container_of(obj, struct device_driver, kobj)


static ssize_t
drv_attr_show(struct kobject * kobj, struct attribute * attr,
	      char * buf, size_t count, loff_t off)
{
	struct driver_attribute * drv_attr = to_drv_attr(attr);
	struct device_driver * drv = to_driver(kobj);
	ssize_t ret = 0;

	if (drv_attr->show)
		ret = drv_attr->show(drv,buf,count,off);
	return ret;
}

static ssize_t
drv_attr_store(struct kobject * kobj, struct attribute * attr,
	       const char * buf, size_t count, loff_t off)
{
	struct driver_attribute * drv_attr = to_drv_attr(attr);
	struct device_driver * drv = to_driver(kobj);
	ssize_t ret = 0;

	if (drv_attr->store)
		ret = drv_attr->store(drv,buf,count,off);
	return ret;
}

static struct sysfs_ops driver_sysfs_ops = {
	.show	= drv_attr_show,
	.store	= drv_attr_store,
};


static void driver_release(struct kobject * kobj)
{
	struct device_driver * drv = to_driver(kobj);
	up(&drv->unload_sem);
}


/*
 * sysfs bindings for buses
 */


static ssize_t
bus_attr_show(struct kobject * kobj, struct attribute * attr,
	      char * buf, size_t count, loff_t off)
{
	struct bus_attribute * bus_attr = to_bus_attr(attr);
	struct bus_type * bus = to_bus(kobj);
	ssize_t ret = 0;

	if (bus_attr->show)
		ret = bus_attr->show(bus,buf,count,off);
	return ret;
}

static ssize_t
bus_attr_store(struct kobject * kobj, struct attribute * attr,
	       const char * buf, size_t count, loff_t off)
{
	struct bus_attribute * bus_attr = to_bus_attr(attr);
	struct bus_type * bus = to_bus(kobj);
	ssize_t ret = 0;

	if (bus_attr->store)
		ret = bus_attr->store(bus,buf,count,off);
	return ret;
}

static struct sysfs_ops bus_sysfs_ops = {
	.show	= bus_attr_show,
	.store	= bus_attr_store,
};

int bus_create_file(struct bus_type * bus, struct bus_attribute * attr)
{
	int error;
	if (get_bus(bus)) {
		error = sysfs_create_file(&bus->subsys.kobj,&attr->attr);
		put_bus(bus);
	} else
		error = -EINVAL;
	return error;
}

void bus_remove_file(struct bus_type * bus, struct bus_attribute * attr)
{
	if (get_bus(bus)) {
		sysfs_remove_file(&bus->subsys.kobj,&attr->attr);
		put_bus(bus);
	}
}

struct subsystem bus_subsys = {
	.kobj	= { .name = "bus" },
	.sysfs_ops	= &bus_sysfs_ops,
};


/**
 *	attach - add device to driver.
 *	@dev:	device.
 *	
 *	By this point, we know for sure what the driver is, so we 
 *	do the rest (which ain't that much).
 */
static void attach(struct device * dev)
{
	pr_debug("bound device '%s' to driver '%s'\n",
		 dev->bus_id,dev->driver->name);
	list_add_tail(&dev->driver_list,&dev->driver->devices);
	sysfs_create_link(&dev->driver->kobj,&dev->kobj,dev->kobj.name);
}


/**
 *	bus_match - check compatibility between device & driver.
 *	@dev:	device.
 *	@drv:	driver.
 *
 *	First, we call the bus's match function, which should compare
 *	the device IDs the driver supports with the device IDs of the 
 *	device. Note we don't do this ourselves because we don't know 
 *	the format of the ID structures, nor what is to be considered
 *	a match and what is not.
 *	
 *	If we find a match, we call @drv->probe(@dev) if it exists, and 
 *	call attach() above.
 */
static int bus_match(struct device * dev, struct device_driver * drv)
{
	int error = -ENODEV;
	if (dev->bus->match(dev,drv)) {
		dev->driver = drv;
		if (drv->probe) {
			if (!(error = drv->probe(dev)))
				attach(dev);
			else 
				dev->driver = NULL;
		} else {
			attach(dev);
			error = 0;
		}
	}
	return error;
}


/**
 *	device_attach - try to attach device to a driver.
 *	@dev:	device.
 *
 *	Walk the list of drivers that the bus has and call bus_match() 
 *	for each pair. If a compatible pair is found, break out and return.
 */
static int device_attach(struct device * dev)
{
	struct bus_type * bus = dev->bus;
	struct list_head * entry;
	int error = 0;

	if (dev->driver) {
		attach(dev);
		return 0;
	}

	if (!bus->match)
		return 0;

	list_for_each(entry,&bus->drivers) {
		struct device_driver * drv = 
			container_of(entry,struct device_driver,bus_list);
		if (!(error = bus_match(dev,drv)))
			break;
	}
	return error;
}


/**
 *	driver_attach - try to bind driver to devices.
 *	@drv:	driver.
 *
 *	Walk the list of devices that the bus has on it and try to match
 *	the driver with each one.
 *	If bus_match() returns 0 and the @dev->driver is set, we've found
 *	a compatible pair, so we call devclass_add_device() to add the 
 *	device to the class. 
 */
static int driver_attach(struct device_driver * drv)
{
	struct bus_type * bus = drv->bus;
	struct list_head * entry;
	int error = 0;

	if (!bus->match)
		return 0;

	list_for_each(entry,&bus->devices) {
		struct device * dev = container_of(entry,struct device,bus_list);
		if (!dev->driver) {
			if (!bus_match(dev,drv) && dev->driver)
				devclass_add_device(dev);
		}
	}
	return error;
}


/**
 *	detach - do dirty work of detaching device from its driver.
 *	@dev:	device.
 *	@drv:	its driver.
 *
 *	Note that calls to this function are serialized by taking the 
 *	bus's rwsem in both bus_remove_device() and bus_remove_driver().
 */

static void detach(struct device * dev, struct device_driver * drv)
{
	if (drv) {
		sysfs_remove_link(&drv->kobj,dev->kobj.name);
		list_del_init(&dev->driver_list);
		devclass_remove_device(dev);
		if (drv->remove)
			drv->remove(dev);
		dev->driver = NULL;
	}
}


/**
 *	device_detach - detach device from its driver.
 *	@dev:	device.
 */

static void device_detach(struct device * dev)
{
	detach(dev,dev->driver);
}


/**
 *	driver_detach - detach driver from all devices it controls.
 *	@drv:	driver.
 */

static void driver_detach(struct device_driver * drv)
{
	struct list_head * entry, * next;
	list_for_each_safe(entry,next,&drv->devices) {
		struct device * dev = container_of(entry,struct device,driver_list);
		detach(dev,drv);
	}
	
}

/**
 *	bus_add_device - add device to bus
 *	@dev:	device being added
 *
 *	- Add the device to its bus's list of devices.
 *	- Try to attach to driver.
 *	- Create link to device's physical location.
 */
int bus_add_device(struct device * dev)
{
	struct bus_type * bus = get_bus(dev->bus);
	if (bus) {
		down_write(&dev->bus->subsys.rwsem);
		pr_debug("bus %s: add device %s\n",bus->name,dev->bus_id);
		list_add_tail(&dev->bus_list,&dev->bus->devices);
		device_attach(dev);
		up_write(&dev->bus->subsys.rwsem);
		sysfs_create_link(&bus->devsubsys.kobj,&dev->kobj,dev->bus_id);
#if 1 /* linux-pm */
		assert_constraints(bus, dev->constraints);
#endif /* linux-pm */
	}
	return 0;
}

/**
 *	bus_remove_device - remove device from bus
 *	@dev:	device to be removed
 *
 *	- Remove symlink from bus's directory.
 *	- Delete device from bus's list.
 *	- Detach from its driver.
 *	- Drop reference taken in bus_add_device().
 */
void bus_remove_device(struct device * dev)
{
	if (dev->bus) {
		sysfs_remove_link(&dev->bus->devsubsys.kobj,dev->bus_id);
		down_write(&dev->bus->subsys.rwsem);
		pr_debug("bus %s: remove device %s\n",dev->bus->name,dev->bus_id);
		device_detach(dev);
		list_del_init(&dev->bus_list);
		up_write(&dev->bus->subsys.rwsem);
		put_bus(dev->bus);
	}
}


/**
 *	bus_add_driver - Add a driver to the bus.
 *	@drv:	driver.
 *
 */
int bus_add_driver(struct device_driver * drv)
{
	struct bus_type * bus = get_bus(drv->bus);
	if (bus) {
		down_write(&bus->subsys.rwsem);
		pr_debug("bus %s: add driver %s\n",bus->name,drv->name);

		strncpy(drv->kobj.name,drv->name,KOBJ_NAME_LEN);
		drv->kobj.subsys = &bus->drvsubsys;
		kobject_register(&drv->kobj);

		devclass_add_driver(drv);
		list_add_tail(&drv->bus_list,&bus->drivers);
		driver_attach(drv);
		up_write(&bus->subsys.rwsem);
	}
	return 0;
}


/**
 *	bus_remove_driver - delete driver from bus's knowledge.
 *	@drv:	driver.
 *
 *	Detach the driver from the devices it controls, and remove
 *	it from it's bus's list of drivers. Finally, we drop the reference
 *	to the bus we took in bus_add_driver().
 */

void bus_remove_driver(struct device_driver * drv)
{
	if (drv->bus) {
		down_write(&drv->bus->subsys.rwsem);
		pr_debug("bus %s: remove driver %s\n",drv->bus->name,drv->name);
		driver_detach(drv);
		list_del_init(&drv->bus_list);
		devclass_remove_driver(drv);
		kobject_unregister(&drv->kobj);
		up_write(&drv->bus->subsys.rwsem);
		put_bus(drv->bus);
#if 1 /* linux-pm */
		bus_reeval_constraints(drv->bus);
#endif /* linux-pm */
	}
}

struct bus_type * get_bus(struct bus_type * bus)
{
	return bus ? container_of(subsys_get(&bus->subsys),struct bus_type,subsys) : NULL;
}

void put_bus(struct bus_type * bus)
{
	subsys_put(&bus->subsys);
}

/**
 *	bus_register - register a bus with the system.
 *	@bus:	bus.
 *
 *	We take bus_sem here to protect against the bus being 
 *	unregistered during the registration process.
 *	Once we have that, we registered the bus with the kobject
 *	infrastructure, then register the children subsystems it has:
 *	the devices and drivers that belong to the bus. 
 */
int bus_register(struct bus_type * bus)
{
	INIT_LIST_HEAD(&bus->devices);
	INIT_LIST_HEAD(&bus->drivers);

	down(&bus_sem);
	strncpy(bus->subsys.kobj.name,bus->name,KOBJ_NAME_LEN);
	bus->subsys.parent = &bus_subsys;
	subsystem_register(&bus->subsys);

	snprintf(bus->devsubsys.kobj.name,KOBJ_NAME_LEN,"devices");
	bus->devsubsys.parent = &bus->subsys;
	subsystem_register(&bus->devsubsys);

	snprintf(bus->drvsubsys.kobj.name,KOBJ_NAME_LEN,"drivers");
	bus->drvsubsys.parent = &bus->subsys;
	bus->drvsubsys.sysfs_ops = &driver_sysfs_ops;
	bus->drvsubsys.release = driver_release;
	subsystem_register(&bus->drvsubsys);

	pr_debug("bus type '%s' registered\n",bus->name);
	up(&bus_sem);
	return 0;
}


/**
 *	bus_unregister - remove a bus from the system 
 *	@bus:	bus.
 *
 *	Take bus_sem, in case the bus we're registering hasn't 
 *	finished registering. Once we have it, unregister the child
 *	subsystems and the bus itself.
 *	Finally, we call put_bus() to release the refcount
 */
void bus_unregister(struct bus_type * bus)
{
	down(&bus_sem);
	pr_debug("bus %s: unregistering\n",bus->name);
	subsystem_unregister(&bus->drvsubsys);
	subsystem_unregister(&bus->devsubsys);
	subsystem_unregister(&bus->subsys);
	up(&bus_sem);
}

#if 1 /* linux-pm */
int __init bus_subsys_init(void)
#else
static int __init bus_subsys_init(void)
#endif
{
	return subsystem_register(&bus_subsys);
}

#if 0 /* linux-pm */
core_initcall(bus_subsys_init);
#endif /* linux-pm */

#if 1 /* linux-pm */
void notify_dpm_constraints(struct bus_type * bus)
{
	/* linux-pm-TODO */
}

int bus_scale(struct bus_type * bus, struct bus_op_point * op)
{
	int error = 0;
	struct list_head * entry;

	if (bus->bus_op) {
		*bus->bus_op = *op;
		list_for_each(entry,&bus->drivers) {
			struct device_driver * drv = 
				container_of(entry,struct device_driver,
					     bus_list);
			error |= driver_scale(drv, (void *) DPM_SCALE_NOTIFY);
		}

		if (! error) {

			/* linux-pm-TODO: Call bus driver to do actual
			   change first. */

			list_for_each(entry,&bus->drivers) {
				struct device_driver * drv = 
					container_of(entry,
						     struct device_driver,
						     bus_list);
				error |= driver_scale(drv, (void *) DPM_SCALE);
			}
		}
	}

	return error;
}

static int assert_a_constraint(struct bus_type *bus, 
			       struct constraints *constraints)
{
	int i, j;
	int bus_constrained = 0;

	if (! constraints || ! bus->constraints)
		return 0;

	get_bus(bus);
	
	for (i = 0; i < constraints->count; i++) {
		for (j = 0; j < bus->constraints->count; j++) {
			if (bus->constraints->param[j].id == 
			    constraints->param[i].id) {

				if (constraints->param[i].min > 
				    bus->constraints->param[j].min) {
					bus->constraints->param[j].min =
						constraints->param[i].min;
					bus_constrained = 1;
				}

				if (constraints->param[i].max < 
				    bus->constraints->param[j].max) {
					bus->constraints->param[j].max =
						constraints->param[i].max;
					bus_constrained = 1;
				}

				break;
			}
		}

		if (j >= bus->constraints->count) {
			bus->constraints->param[bus->constraints->count].id =
				constraints->param[i].id;
			bus->constraints->param[bus->constraints->count].min =
				constraints->param[i].min;
			bus->constraints->param[bus->constraints->count].max =
				constraints->param[i].max;
			bus->constraints->count++;
			bus_constrained = 1;
		}
	}

	put_bus(bus);
	return bus_constrained;
}


void assert_constraints(struct bus_type *bus, 
			struct constraints *constraints)
{
	if (! constraints || ! bus->constraints)
		return;

	constraints->asserted = 1;

	if (assert_a_constraint(bus, constraints))
		notify_dpm_constraints(bus);

	return;
}


void deassert_constraints(struct bus_type *bus, 
			  struct constraints *constraints)
{
	if (! constraints)
		return;

	constraints->asserted = 0;
	bus_reeval_constraints(bus);
	return;
}


void update_constraints(struct bus_type *bus, 
			struct constraints *constraints)
{
	if (! constraints)
		return;

	constraints->asserted = 1;
	bus_reeval_constraints(bus);
	return;
}


static int do_assert_a_constraint(struct device * dev, void * data)
{
	if (dev->constraints && dev->constraints->asserted &&
	    dev->driver && dev->driver->bus)
		(void) assert_a_constraint(dev->driver->bus, dev->constraints);

        return 0;
}

void bus_reeval_constraints(struct bus_type * bus)
{
	struct constraints old_constraints;
	int i, j;
	struct list_head * entry;

	if (! bus->constraints)
		return;

	old_constraints = *bus->constraints;

	bus->constraints->count = 0;

	list_for_each(entry,&bus->devices) {
		struct device * dev = 
			container_of(entry, struct device, bus_list);
		do_assert_a_constraint(dev, NULL);
	}

	/*
	 * Compare old and new constraints and notify DPM upper layers if
	 * changed.
	 */

	if (old_constraints.count != bus->constraints->count) 
		goto update;

	for (i = 0; i < old_constraints.count; i++) {
		for (j = 0; j < bus->constraints->count; j++) {
			if (bus->constraints->param[j].id == 
			    old_constraints.param[i].id)
				
				if ((old_constraints.param[i].min !=
				     bus->constraints->param[j].min) ||
				    (old_constraints.param[i].max != 
				     bus->constraints->param[j].max))
					goto update;
		}
		
		if (j >= bus->constraints->count)
			goto update;
	}		

	return;

 update:
	notify_dpm_constraints(bus);
	return;
}

EXPORT_SYMBOL(assert_constraints);
EXPORT_SYMBOL(deassert_constraints);
EXPORT_SYMBOL(update_constraints);
#endif /* linux-pm */

EXPORT_SYMBOL(bus_add_device);
EXPORT_SYMBOL(bus_remove_device);
EXPORT_SYMBOL(bus_register);
EXPORT_SYMBOL(bus_unregister);
EXPORT_SYMBOL(get_bus);
EXPORT_SYMBOL(put_bus);

EXPORT_SYMBOL(bus_create_file);
EXPORT_SYMBOL(bus_remove_file);
