// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "droidspaces_devtmpfs: " fmt
#include <linux/module.h>
#include <linux/of.h>
#include <linux/utsname.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <generated/utsrelease.h>
#include "core.h"
#include "hint.h"
#include "port.h"
#include "dnsk_fs_layout.h"

extern struct kset *devices_kset;
static DEFINE_MUTEX(nodes_lock);
static LIST_HEAD(changing_devices);
struct changing_device {
	struct list_head link;
	struct device *dev;
};
static bool ready;
static unsigned int errors;
static bool activate;
static bool gki;
module_param(ready, bool, 0400);
module_param(errors, uint, 0400);
module_param(activate, bool, 0400);
module_param(gki, bool, 0400);
MODULE_PARM_DESC(ready, "Existing devices populated and device lifecycle hooks active");
MODULE_PARM_DESC(errors, "Device node update errors, excluding existing or removed nodes");
MODULE_PARM_DESC(activate, "Install device lifecycle hooks after preflight");
MODULE_PARM_DESC(gki, "Allow the compiled android16-6.12 GKI family");

static bool ds_gki_supported(void)
{
	const char *release = utsname()->release;

	/* The DDK test kernel reports its own release, not the phone's KMI. */
	if (of_machine_is_compatible("linux,dummy-virt") &&
	    !strcmp(release, UTS_RELEASE))
		return true;
	return gki && !strncmp(release, "6.12.", 5) &&
	       strstr(release, "-android16-");
}

static void update_node(struct device *dev, bool create)
{
	int ret;
	struct changing_device *changing;
	if (create)
		list_for_each_entry(changing, &changing_devices, link)
			if (changing->dev == dev)
				return;
	if (!MAJOR(dev->devt))
		return;
	ret = create ? ds_devtmpfs_create_node(dev) : ds_devtmpfs_delete_node(dev);
	if (ret && ret != -EEXIST && ret != -ENOENT) {
		errors++;
		pr_err("%s %s: %d\n", create ? "create" : "remove", dev_name(dev), ret);
	}
}

int ds_device_add(struct device *dev)
{
	struct changing_device changing = { .dev = dev };
	int ret;
	/* Snapshot enumeration must not publish a partially registered device. */
	mutex_lock(&nodes_lock);
	list_add(&changing.link, &changing_devices);
	mutex_unlock(&nodes_lock);
	ret = ds_device_add_original(dev);
	mutex_lock(&nodes_lock);
	list_del(&changing.link);
	if (!ret && device_is_registered(dev))
		update_node(dev, true);
	mutex_unlock(&nodes_lock);
	return ret;
}

void ds_device_del(struct device *dev)
{
	struct changing_device changing = { .dev = dev };
	mutex_lock(&nodes_lock);
	list_add(&changing.link, &changing_devices);
	update_node(dev, false);
	mutex_unlock(&nodes_lock);
	/* Driver teardown can recursively unregister children; do not hold our lock. */
	ds_device_del_original(dev);
	mutex_lock(&nodes_lock);
	list_del(&changing.link);
	mutex_unlock(&nodes_lock);
}

struct existing_device {
	struct list_head link;
	struct device *dev;
};

static int populate_existing(void)
{
	LIST_HEAD(snapshot);
	struct existing_device *item, *next;
	struct kobject *obj;
	int ret = 0;

	spin_lock(&devices_kset->list_lock);
	list_for_each_entry(obj, &devices_kset->list, entry) {
		struct device *dev = container_of(obj, struct device, kobj);
		if (!MAJOR(dev->devt))
			continue;
		item = kmalloc(sizeof(*item), GFP_ATOMIC);
		if (!item) { ret = -ENOMEM; break; }
		item->dev = get_device(dev);
		list_add_tail(&item->link, &snapshot);
	}
	spin_unlock(&devices_kset->list_lock);
	list_for_each_entry_safe(item, next, &snapshot, link) {
		mutex_lock(&nodes_lock);
		if (device_is_registered(item->dev))
			update_node(item->dev, true);
		mutex_unlock(&nodes_lock);
		put_device(item->dev);
		list_del(&item->link);
		kfree(item);
	}
	return ret;
}

static int __init ds_init(void)
{
	struct kallrecon_hint hint = { .timeout_ms = 15000 };
	struct file_system_type *existing;
	int ret;

	if (!ds_gki_supported())
		return -EOPNOTSUPP;
	if (!activate) {
		pr_info("inactive: activate=1 and gki=1 are required\n");
		return 0;
	}
	existing = get_fs_type("devtmpfs");
	if (existing) { put_filesystem(existing); return -EEXIST; }
	ret = kallrecon_supply(&hint);
	if (ret) return ret;
	find_kallsyms_base();
	if (kallrecon_fail_reason() != KALLRECON_OK || !klnum_val)
		return -ENODATA;
	ret = ds_hooks_prepare();
	if (ret) return ret;
	ret = ds_devtmpfs_start();
	if (ret) return ret;
	__module_get(THIS_MODULE);
	ret = ds_hooks_install();
	if (ret) {
		module_put(THIS_MODULE);
		ds_devtmpfs_stop();
		return ret;
	}
	/* Hooks are permanent; initialization failures after this point stay visible. */
	ret = populate_existing();
	if (ret) { errors++; pr_err("initial device enumeration failed: %d\n", ret); }
	ready = !ret && !errors;
	pr_info("device lifecycle active, ready=%u errors=%u; pinned until reboot\n", ready, errors);
	return 0;
}
module_init(ds_init);

static void __exit ds_exit(void)
{
	/* Active mode is pinned by __module_get(); inactive mode has no state. */
}

module_exit(ds_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shikochin");
MODULE_DESCRIPTION("Late-loaded devtmpfs for android16-6.12 GKI");
