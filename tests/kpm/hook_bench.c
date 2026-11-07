// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal KPM for the M6 KPM-vs-LKM overhead evaluation: install an empty
 * getpid hook, count hits, and remove the hook in exit.  It must only be
 * loaded through the APatch manager SuperCall (apd has no KPM command).
 *
 * Statically verified against KernelPatch 0.13.9 (tag 0.13.9, commit
 * b51197a):
 *   - kernel/include/kpmodule.h: KPM_NAME/VERSION/LICENSE/AUTHOR/DESCRIPTION,
 *     mod_initcall_t/mod_ctl0call_t/mod_exitcall_t;
 *   - kernel/include/hook.h: hook_fargs0_t, hook_chain0_callback;
 *   - kernel/patch/include/syscall.h: fp_hook_syscalln/fp_unhook_syscalln;
 *   - kernel/patch/include/kputils.h: compat_copy_to_user;
 *   - pr_info/pr_err live in kernel/linux/include/linux/printk.h and
 *     strncmp/memcpy in kernel/linux/include/linux/string.h (there is no
 *     kernel/include/linux/ tree in this tag);
 *   - kernel/linux/include/uapi/asm-generic/unistd.h: __NR_getpid 172.
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

/*
 * 0.13.9 hook.h: narg 0 callbacks receive hook_fargs0_t; the callback typedef
 * is hook_chain0_callback.  syscall.h promotes narg to 1 on kernels with
 * syscall wrappers and then passes hook_fargs1_t, whose prefix
 * (chain/skip_origin/local/ret) is layout-identical, so reading only
 * skip_origin stays valid either way.
 */
static void before_getpid(hook_fargs0_t *args, void *udata)
{
	(void)args;
	(void)udata;
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
	hook_err_t ret;

	(void)args;
	(void)reserved;
	pr_info("dnsk-hook-bench: init event=%s\n", event ? event : "?");
	/* hook_err_t fp_hook_syscalln(int nr, int narg, void *before,
	 *                             void *after, void *udata); after may be 0. */
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

	if (args && !strncmp(args, "reset", 5)) {
		__atomic_store_n(&hits, 0, __ATOMIC_RELAXED);
		n = 5;
		memcpy(buf, "reset", 5);
	} else {
		unsigned long long v = __atomic_load_n(&hits, __ATOMIC_RELAXED);

		memcpy(buf, "hits=", 5);
		n = append_dec(buf, 5, v);
	}
	/*
	 * 0.13.9 kputils.h: int __must_check compat_copy_to_user(
	 *     void __user *to, const void *from, int n);
	 * The helper does not take the destination length, so honour outlen
	 * here instead of trusting the caller's buffer.  __must_check is empty
	 * in ktypes.h, so the unused result is warning-free.
	 */
	if (out_msg && outlen >= n)
		compat_copy_to_user(out_msg, buf, n);
	return 0;
}

static long bench_exit(void *reserved)
{
	/* void fp_unhook_syscalln(int nr, void *before, void *after); */
	(void)reserved;
	fp_unhook_syscalln(__NR_getpid, before_getpid, 0);
	pr_info("dnsk-hook-bench: getpid unhooked\n");
	return 0;
}

KPM_INIT(bench_init);
KPM_CTL0(bench_ctl0);
KPM_EXIT(bench_exit);

/* Fail the build if the 0.13.9 lifecycle/callback typedefs drift. */
_Static_assert(__same_type(&bench_init, mod_initcall_t), "KPM_INIT signature drift");
_Static_assert(__same_type(&bench_ctl0, mod_ctl0call_t), "KPM_CTL0 signature drift");
_Static_assert(__same_type(&bench_exit, mod_exitcall_t), "KPM_EXIT signature drift");
_Static_assert(__same_type(&before_getpid, hook_chain0_callback), "hook_fargs0_t callback drift");
