// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal KPM for the M6 KPM-vs-LKM overhead evaluation: install an empty
 * getpid hook, count hits, and remove the hook in exit.  It must only be
 * loaded through the APatch manager SuperCall (apd has no KPM command).
 */
#include <compiler.h>
#include <kpmodule.h>
#include <hook.h>
#include <syscall.h>
#include <kputils.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <uapi/asm-generic/unistd.h>

KPM_NAME("dnsk-hook-bench");
KPM_VERSION("0.1.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("uobe1");
KPM_DESCRIPTION("Empty getpid hook for the KPM vs LKM overhead evaluation");

static volatile unsigned long long hits;

static void before_getpid(hook_fargs0_t *args, void *udata)
{
	__atomic_add_fetch(&hits, 1, __ATOMIC_RELAXED);
}

static int append_dec(char *buf, int pos, unsigned long long v)
{
	char tmp[24];
	int n = 0, i;

	do {
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	} while (v);
	for (i = n - 1; i >= 0; i--)
		buf[pos++] = tmp[i];
	return pos;
}

static long bench_init(const char *args, const char *event, void *reserved)
{
	int ret;

	pr_info("dnsk-hook-bench: init event=%s\n", event ? event : "?");
	ret = fp_hook_syscalln(__NR_getpid, 0, before_getpid, 0, 0);
	if (ret) {
		pr_err("dnsk-hook-bench: fp_hook_syscalln failed %d\n", ret);
		return ret;
	}
	pr_info("dnsk-hook-bench: getpid hooked\n");
	return 0;
}

static long bench_ctl0(const char *args, char *__user out_msg, int outlen)
{
	char buf[48];
	int n;

	(void)outlen;
	if (args && !strncmp(args, "reset", 5)) {
		__atomic_store_n(&hits, 0, __ATOMIC_RELAXED);
		n = 5;
		memcpy(buf, "reset", 5);
	} else {
		unsigned long long v = __atomic_load_n(&hits, __ATOMIC_RELAXED);

		memcpy(buf, "hits=", 5);
		n = append_dec(buf, 5, v);
	}
	compat_copy_to_user(out_msg, buf, n);
	return 0;
}

static long bench_exit(void *reserved)
{
	fp_unhook_syscalln(__NR_getpid, before_getpid, 0);
	pr_info("dnsk-hook-bench: getpid unhooked\n");
	return 0;
}

KPM_INIT(bench_init);
KPM_CTL0(bench_ctl0);
KPM_EXIT(bench_exit);
