// SPDX-License-Identifier: GPL-2.0-only
/*
 * trigger_probe: measure whether a devtmpfs node exists at bus-notify time.
 *
 * Load-time one-shot.  It arms a platform_bus notifier, adds a fake platform
 * device whose devt is MKDEV(240, 0) (devtmpfs calls it /dev/dnsktrig), then
 * removes the device again.  Everything else is observed inside the two bus
 * callbacks, which is exactly the window the devtmpfs module under test moves:
 *
 *   trigger=bus    the module creates the node before BUS_NOTIFY_ADD_DEVICE and
 *                  removes it only after BUS_NOTIFY_DEL_DEVICE, so the callbacks
 *                  see it present  -> DNSK_TRIGGER_BUS_*_NODE_PRESENT
 *   trigger=entry  devtmpfs itself creates/deletes around the same notify, so
 *                  the callbacks see it absent
 *                  -> DNSK_TRIGGER_ENTRY_*_NO_NODE
 *
 * Both marker sets go to the console via pr_info, which lands in the QEMU
 * serial log the workflow greps.
 */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>

#define PROBE_NODE	"/dev/dnsktrig"
#define PROBE_NAME	"dnsktrig"
#define PROBE_MAJOR	240

static struct platform_device *probe_pdev;
static struct notifier_block probe_nb;

/*
 * vfs_stat() is not exported to modules, and vfs_getattr() is behind
 * ANDROID_GKI_VFS_EXPORT_ONLY, so existence is checked with kern_path() +
 * path_put(), both plain EXPORT_SYMBOL.  Present/absent is all this probe needs.
 */
static bool node_present(void)
{
	struct path path;

	if (kern_path(PROBE_NODE, LOOKUP_FOLLOW, &path) != 0)
		return false;
	path_put(&path);
	return true;
}

static int probe_notify(struct notifier_block *nb, unsigned long action,
			void *data)
{
	struct device *dev = data;

	if (!dev || dev->devt != MKDEV(PROBE_MAJOR, 0))
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		pr_info("dnsk_trigger: add %s present=%d\n",
			dev_name(dev), node_present());
		if (node_present())
			pr_info("DNSK_TRIGGER_BUS_ADD_NODE_PRESENT\n");
		else
			pr_info("DNSK_TRIGGER_ENTRY_ADD_NO_NODE\n");
		break;
	case BUS_NOTIFY_DEL_DEVICE:
		pr_info("dnsk_trigger: del %s present=%d\n",
			dev_name(dev), node_present());
		if (node_present())
			pr_info("DNSK_TRIGGER_BUS_DEL_NODE_PRESENT\n");
		else
			pr_info("DNSK_TRIGGER_ENTRY_DEL_NO_NODE\n");
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int __init trigger_probe_init(void)
{
	int ret;

	probe_pdev = platform_device_alloc(PROBE_NAME, -1);
	if (!probe_pdev)
		return -ENOMEM;
	probe_pdev->dev.devt = MKDEV(PROBE_MAJOR, 0);

	probe_nb.notifier_call = probe_notify;
	ret = bus_register_notifier(&platform_bus_type, &probe_nb);
	if (ret) {
		platform_device_put(probe_pdev);
		probe_pdev = NULL;
		return ret;
	}

	ret = platform_device_add(probe_pdev);
	if (ret) {
		bus_unregister_notifier(&platform_bus_type, &probe_nb);
		platform_device_put(probe_pdev);
		probe_pdev = NULL;
		return ret;
	}

	/* Second window: DEL must still see the node under trigger=bus. */
	platform_device_del(probe_pdev);
	bus_unregister_notifier(&platform_bus_type, &probe_nb);
	pr_info("DNSK_TRIGGER_PROBE_DONE\n");
	return 0;
}

static void __exit trigger_probe_exit(void)
{
	if (probe_pdev) {
		platform_device_put(probe_pdev);
		probe_pdev = NULL;
	}
}

module_init(trigger_probe_init);
module_exit(trigger_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("devtmpfs bus-notify timing probe");
