// SPDX-License-Identifier: GPL-2.0-only
/*
 * Root-only RW server (/proc/dnsk/rw) for the M6 evaluation described in
 * docs/evidence/rw-evaluation.md.
 *
 * Two primitives, both single-page and <= 256 bytes per operation:
 *   - kernel virtual addresses: read via copy_from_kernel_nofault(), write via
 *     dnsk_patch_text() (stop_machine + fixmap + copy_to_kernel_nofault), so
 *     read-only text/data is covered;
 *   - physical RAM addresses: same operations through the linear map (__va),
 *     validated with pfn_valid(); MMIO is intentionally excluded because
 *     reads can have side effects.
 *
 * Every write journals the original bytes (8 slots) and can be reverted
 * through the same atomic path; a failed write tries to restore them.  Writes
 * require CAP_SYS_ADMIN in the initial user namespace.  Apply time and
 * counters are reported by "info".
 *
 * Protocol: write one command line, then read the file for the result.
 *   read <hex-va> <len>        write <hex-va> <hex>
 *   readp <hex-pa> <len>       writep <hex-pa> <hex>
 *   revert <slot>              resolve <symbol>       pa <symbol>
 *   info
 *
 * This is a primitive, not a functional acceptance: callers must still prove
 * the rewritten target behaves natively, has no lost references, and can be
 * rolled back; kernel addresses must not be written to public logs.
 */
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pfn.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include "dnsk_internal.h"

#define DNSK_RW_MAX 256
#define DNSK_RW_SLOTS 8
#define DNSK_RW_OUT 1400

struct dnsk_rw_slot {
	bool used;
	bool phys;
	u64 addr;
	size_t len;
	u8 orig[DNSK_RW_MAX];
};

static struct proc_dir_entry *dnsk_rw_entry;
static DEFINE_MUTEX(dnsk_rw_lock);
static struct dnsk_rw_slot dnsk_rw_slots[DNSK_RW_SLOTS];
static unsigned int dnsk_rw_next;
static char dnsk_rw_out[DNSK_RW_OUT];
static size_t dnsk_rw_out_len;
static int (*dnsk_rw_read_fn)(void *dst, const void *src, size_t size);
static u64 dnsk_rw_reads, dnsk_rw_writes, dnsk_rw_reverts;

static int dnsk_rw_check(u64 addr, size_t len, bool phys)
{
	if (!len || len > DNSK_RW_MAX)
		return -EINVAL;
	if ((addr & ~PAGE_MASK) + len > PAGE_SIZE)
		return -EINVAL;
	if (phys) {
		if (addr + len < addr)
			return -EINVAL;
		if (!pfn_valid(PHYS_PFN(addr)) ||
		    !pfn_valid(PHYS_PFN(addr + len - 1)))
			return -EINVAL;
	} else {
		if (!addr || (s64)addr > 0)
			return -EINVAL;
	}
	return 0;
}

static void *dnsk_rw_ptr(u64 addr, bool phys)
{
	return (void *)(phys ? (u64)__va((phys_addr_t)addr) : addr);
}

static int dnsk_rw_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int dnsk_rw_hex2bin(const char *s, u8 *out, size_t *len)
{
	size_t n = 0;

	while (*s) {
		int hi = dnsk_rw_hexval(s[0]);
		int lo = dnsk_rw_hexval(s[1]);

		if (n >= DNSK_RW_MAX)
			return -E2BIG;
		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[n++] = (hi << 4) | lo;
		s += 2;
		if (*s == ' ')
			s++;
	}
	*len = n;
	return n ? 0 : -EINVAL;
}

static int dnsk_rw_parse_hex(const char **pp, u64 *v)
{
	const char *s = skip_spaces(*pp);
	char *end;
	unsigned long long x;

	if (!*s)
		return -EINVAL;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	x = simple_strtoull(s, &end, 16);
	if (end == s)
		return -EINVAL;
	*v = x;
	*pp = end;
	return 0;
}

static int dnsk_rw_parse_uint(const char **pp, unsigned long *v)
{
	const char *s = skip_spaces(*pp);
	char *end;

	if (!*s)
		return -EINVAL;
	*v = simple_strtoul(s, &end, 0);
	if (end == s)
		return -EINVAL;
	*pp = end;
	return 0;
}

static int dnsk_rw_do_read(u64 addr, size_t len, bool phys)
{
	u8 buf[DNSK_RW_MAX];
	size_t i;
	int ret;

	ret = dnsk_rw_check(addr, len, phys);
	if (ret)
		return ret;
	if (!dnsk_rw_read_fn)
		return -EOPNOTSUPP;
	ret = dnsk_rw_read_fn(buf, dnsk_rw_ptr(addr, phys), len);
	if (ret)
		return ret;
	dnsk_rw_reads++;
	dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT, "%s %llx ",
				    phys ? "readp" : "read", addr);
	for (i = 0; i < len; i++)
		dnsk_rw_out_len += scnprintf(dnsk_rw_out + dnsk_rw_out_len,
					     DNSK_RW_OUT - dnsk_rw_out_len,
					     "%02x", buf[i]);
	dnsk_rw_out_len += scnprintf(dnsk_rw_out + dnsk_rw_out_len,
				     DNSK_RW_OUT - dnsk_rw_out_len, "\n");
	return 0;
}

static int dnsk_rw_do_write(u64 addr, const u8 *data, size_t len, bool phys)
{
	u8 orig[DNSK_RW_MAX];
	struct dnsk_rw_slot *slot = NULL;
	unsigned int i;
	u64 t0;
	int ret;

	ret = dnsk_rw_check(addr, len, phys);
	if (ret)
		return ret;
	if (!dnsk_patch_ready() || !dnsk_rw_read_fn)
		return -EOPNOTSUPP;
	ret = dnsk_rw_read_fn(orig, dnsk_rw_ptr(addr, phys), len);
	if (ret)
		return ret;

	for (i = 0; i < DNSK_RW_SLOTS; i++) {
		if (!dnsk_rw_slots[i].used) {
			slot = &dnsk_rw_slots[i];
			break;
		}
	}
	if (!slot) {
		slot = &dnsk_rw_slots[dnsk_rw_next++ % DNSK_RW_SLOTS];
		dnsk_warn("rw: journal slot %u overwritten\n",
			  (unsigned int)(slot - dnsk_rw_slots));
	}

	t0 = ktime_get_ns();
	ret = dnsk_patch_text(dnsk_rw_ptr(addr, phys), data, len, false);
	if (ret) {
		if (dnsk_patch_text(dnsk_rw_ptr(addr, phys), orig, len, false))
			dnsk_err("rw: restore after failed write failed\n");
		return ret;
	}

	memcpy(slot->orig, orig, len);
	slot->addr = addr;
	slot->len = len;
	slot->phys = phys;
	slot->used = true;
	dnsk_rw_writes++;
	dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
				    "%s %llx slot=%u len=%zu ns=%llu\n",
				    phys ? "writep" : "write", addr,
				    (unsigned int)(slot - dnsk_rw_slots), len,
				    ktime_get_ns() - t0);
	return 0;
}

static int dnsk_rw_do_revert(unsigned long slot)
{
	struct dnsk_rw_slot *s;
	int ret;

	if (slot >= DNSK_RW_SLOTS)
		return -EINVAL;
	s = &dnsk_rw_slots[slot];
	if (!s->used)
		return -ENOENT;
	ret = dnsk_patch_text(dnsk_rw_ptr(s->addr, s->phys), s->orig, s->len,
			      false);
	if (ret)
		return ret;
	s->used = false;
	dnsk_rw_reverts++;
	dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
				    "revert %lu ok\n", slot);
	return 0;
}

static bool dnsk_rw_find_slot(bool phys, u64 addr, unsigned int *slot)
{
	unsigned int i;

	for (i = 0; i < DNSK_RW_SLOTS; i++) {
		if (dnsk_rw_slots[i].used && dnsk_rw_slots[i].phys == phys &&
		    dnsk_rw_slots[i].addr == addr) {
			*slot = i;
			return true;
		}
	}
	return false;
}

static int dnsk_rw_selftest(void)
{
	u8 *buf, *back;
	unsigned long pa = 0;
	u8 a[16], b[16];
	unsigned int slot;
	u64 t0, t1;
	int i, ret;

	buf = kmalloc(64, GFP_KERNEL);
	back = kmalloc(64, GFP_KERNEL);
	if (!buf || !back) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < 16; i++) {
		a[i] = 0xa0 + i;
		b[i] = 0x50 + i;
	}
	memcpy(buf, a, 16);
	ret = dnsk_virt_to_phys(buf, &pa);
	if (ret)
		goto out;

	t0 = ktime_get_ns();
	/* physical write -> virtual read -> revert */
	ret = dnsk_rw_do_write((u64)pa, b, 16, true);
	if (ret)
		goto out;
	if (!dnsk_rw_find_slot(true, (u64)pa, &slot)) {
		ret = -ENOENT;
		goto out;
	}
	ret = dnsk_rw_read_fn(back, buf, 16);
	if (ret || memcmp(back, b, 16)) {
		ret = ret ? ret : -EIO;
		goto out;
	}
	ret = dnsk_rw_do_revert(slot);
	if (ret)
		goto out;
	ret = dnsk_rw_read_fn(back, __va((phys_addr_t)pa), 16);
	if (ret || memcmp(back, a, 16)) {
		ret = ret ? ret : -EIO;
		goto out;
	}

	/* virtual write -> physical read -> revert */
	ret = dnsk_rw_do_write((u64)(unsigned long)buf, b, 16, false);
	if (ret)
		goto out;
	if (!dnsk_rw_find_slot(false, (u64)(unsigned long)buf, &slot)) {
		ret = -ENOENT;
		goto out;
	}
	ret = dnsk_rw_read_fn(back, __va((phys_addr_t)pa), 16);
	if (ret || memcmp(back, b, 16)) {
		ret = ret ? ret : -EIO;
		goto out;
	}
	ret = dnsk_rw_do_revert(slot);
	if (ret)
		goto out;
	ret = dnsk_rw_read_fn(back, buf, 16);
	if (ret || memcmp(back, a, 16)) {
		ret = ret ? ret : -EIO;
		goto out;
	}
	t1 = ktime_get_ns();
	dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
				    "selftest pass pa=0x%lx ns=%llu\n", pa,
				    t1 - t0);
out:
	kfree(buf);
	kfree(back);
	return ret;
}

static ssize_t dnsk_rw_read_file(struct file *file, char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&dnsk_rw_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, dnsk_rw_out,
				      dnsk_rw_out_len);
	mutex_unlock(&dnsk_rw_lock);
	return ret;
}

static ssize_t dnsk_rw_write_file(struct file *file, const char __user *ubuf,
				  size_t count, loff_t *ppos)
{
	char kbuf[160];
	char *p = kbuf, *cmd, *rest;
	const char *arg;
	unsigned long slot;
	size_t len;
	u64 addr;
	u8 data[DNSK_RW_MAX];
	int ret = -EINVAL;

	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (!count || count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	mutex_lock(&dnsk_rw_lock);
	cmd = strsep(&p, " ");
	rest = p;
	arg = rest ? rest : "";
	if (!cmd)
		goto out;
	if (!strcmp(cmd, "read") || !strcmp(cmd, "readp")) {
		bool phys = cmd[4] == 'p';
		unsigned long l;

		ret = dnsk_rw_parse_hex(&arg, &addr);
		if (!ret)
			ret = dnsk_rw_parse_uint(&arg, &l);
		if (!ret)
			ret = dnsk_rw_do_read(addr, l, phys);
	} else if (!strcmp(cmd, "write") || !strcmp(cmd, "writep")) {
		bool phys = cmd[5] == 'p';

		ret = dnsk_rw_parse_hex(&arg, &addr);
		if (!ret)
			ret = dnsk_rw_hex2bin(skip_spaces(arg), data, &len);
		if (!ret)
			ret = dnsk_rw_do_write(addr, data, len, phys);
	} else if (!strcmp(cmd, "revert")) {
		ret = dnsk_rw_parse_uint(&arg, &slot);
		if (!ret)
			ret = dnsk_rw_do_revert(slot);
	} else if (!strcmp(cmd, "resolve") || !strcmp(cmd, "pa")) {
		const char *name = skip_spaces(arg);
		void *va = NULL;

		if (*name)
			va = dnsk_resolve_symbol(name);
		if (!va) {
			dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
						    "%s %s null\n", cmd, name);
			ret = 0;
		} else if (cmd[0] == 'p') {
			unsigned long pa = 0;

			if (dnsk_virt_to_phys(va, &pa)) {
				dnsk_rw_out_len = scnprintf(dnsk_rw_out,
							    DNSK_RW_OUT,
							    "pa %s null\n", name);
			} else {
				dnsk_rw_out_len = scnprintf(dnsk_rw_out,
							    DNSK_RW_OUT,
							    "pa %s 0x%lx\n", name,
							    pa);
			}
			ret = 0;
		} else {
			dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
						    "resolve %s %px\n", name, va);
			ret = 0;
		}
	} else if (!strcmp(cmd, "selftest")) {
		ret = dnsk_rw_selftest();
		if (ret)
			dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
						    "selftest fail %d\n", ret);
	} else if (!strcmp(cmd, "info")) {
		ret = 0;
		dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
					    "rw=%s reads=%llu writes=%llu reverts=%llu cap=%u slots=%u pa=ram-only\n",
					    dnsk_patch_ready() ? "ready" : "disabled",
					    dnsk_rw_reads, dnsk_rw_writes,
					    dnsk_rw_reverts, DNSK_RW_MAX,
					    DNSK_RW_SLOTS);
	}
	if (ret)
		dnsk_rw_out_len = scnprintf(dnsk_rw_out, DNSK_RW_OUT,
					    "error %d\n", ret);
out:
	mutex_unlock(&dnsk_rw_lock);
	return ret ? ret : count;
}

static const struct proc_ops dnsk_rw_proc_ops = {
	.proc_read = dnsk_rw_read_file,
	.proc_write = dnsk_rw_write_file,
};

int dnsk_rw_init(struct proc_dir_entry *proc_dir)
{
	dnsk_rw_read_fn = dnsk_resolve_symbol("copy_from_kernel_nofault");
	if (!dnsk_rw_read_fn)
		dnsk_warn("rw: copy_from_kernel_nofault missing; server disabled\n");
	dnsk_rw_entry = proc_create("rw", 0400, proc_dir, &dnsk_rw_proc_ops);
	if (!dnsk_rw_entry) {
		dnsk_err("rw: cannot create /proc/dnsk/rw\n");
		return -ENOMEM;
	}
	dnsk_info("rw: /proc/dnsk/rw ready (patch=%s, read=%s, pa=ram-only)\n",
		  dnsk_patch_ready() ? "yes" : "no",
		  dnsk_rw_read_fn ? "yes" : "no");
	return 0;
}

void dnsk_rw_exit(void)
{
	unsigned int i, used = 0;

	for (i = 0; i < DNSK_RW_SLOTS; i++)
		if (dnsk_rw_slots[i].used)
			used++;
	if (used)
		dnsk_warn("rw: exiting with %u unreverted write(s)\n", used);
	if (dnsk_rw_entry) {
		proc_remove(dnsk_rw_entry);
		dnsk_rw_entry = NULL;
	}
}
