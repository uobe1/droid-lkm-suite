// SPDX-License-Identifier: GPL-2.0-only
/* getpid kprobe overhead benchmark for the LKM vs KPM comparison. */
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>

static unsigned long long hits;
module_param(hits, ullong, 0444);
MODULE_PARM_DESC(hits, "getpid kprobe hits");

static int dnsk_bench_pre(struct kprobe *p, struct pt_regs *regs)
{
	hits++;
	return 0;
}

static struct kprobe dnsk_bench_kp = {
	.symbol_name = "__arm64_sys_getpid",
	.pre_handler = dnsk_bench_pre,
};

static int __init dnsk_bench_init(void)
{
	int ret = register_kprobe(&dnsk_bench_kp);

	if (!ret)
		pr_info("dnsk_bench: getpid kprobe registered\n");
	return ret;
}

static void __exit dnsk_bench_exit(void)
{
	unregister_kprobe(&dnsk_bench_kp);
	pr_info("dnsk_bench: hits=%llu\n", hits);
}

module_init(dnsk_bench_init);
module_exit(dnsk_bench_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("getpid kprobe overhead benchmark");
