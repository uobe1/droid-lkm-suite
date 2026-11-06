// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test-only helper for the device-image harness.  beryl's ReSukiSU build
 * forces enforce=1 after late-load and the initramfs init stays in the
 * kernel domain, which then cannot exec rootfs binaries to patch policy.
 * Arm a timer before late-load and clear selinux_state.enforcing directly
 * after KSU commits its policy.  Never shipped in a release package.
 */

#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/workqueue.h>

/* beryl 6.12.30 BTF: struct selinux_state, enforcing at offset 0. */
#define DNSK_SELINUX_ENFORCING_OFF 0

static void *dnsk_selinux_state;
static struct delayed_work dnsk_clear_work;
static int dnsk_rounds = 8;

static void dnsk_clear_enforcing(struct work_struct *work)
{
	if (dnsk_selinux_state)
		WRITE_ONCE(*(u8 *)dnsk_selinux_state, 0);
	if (--dnsk_rounds > 0)
		schedule_delayed_work(&dnsk_clear_work, 2 * HZ);
}

static int __init dnsk_selpermissive_init(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	unsigned long (*lookup)(const char *name);
	unsigned long addr;

	if (register_kprobe(&kp))
		return -ENOENT;
	lookup = (void *)kp.addr;
	unregister_kprobe(&kp);

	addr = lookup("selinux_state");
	if (!addr)
		return -ENOENT;
	dnsk_selinux_state = (void *)(addr + DNSK_SELINUX_ENFORCING_OFF);

	INIT_DELAYED_WORK(&dnsk_clear_work, dnsk_clear_enforcing);
	schedule_delayed_work(&dnsk_clear_work, 8 * HZ);
	pr_info("dnsk_test_selpermissive: armed\n");
	return 0;
}

static void __exit dnsk_selpermissive_exit(void)
{
	cancel_delayed_work_sync(&dnsk_clear_work);
}

module_init(dnsk_selpermissive_init);
module_exit(dnsk_selpermissive_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("droid-lkm-suite test-only SELinux enforce clear");
