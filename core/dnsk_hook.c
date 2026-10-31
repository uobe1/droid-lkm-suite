// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hook registry.  Every syscall/data/kprobe hook is owned by core so that
 * feature modules cannot leave permanent hooks behind on unload.
 */

#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/string.h>
#include "dnsk_internal.h"

static struct dnsk_hook dnsk_hooks[DNSK_MAX_HOOKS];
static DEFINE_MUTEX(dnsk_hooks_lock);

static struct dnsk_hook *dnsk_find_locked(const char *key)
{
	int i;

	for (i = 0; i < DNSK_MAX_HOOKS; i++) {
		if (dnsk_hooks[i].active && !strcmp(dnsk_hooks[i].key, key))
			return &dnsk_hooks[i];
	}
	return NULL;
}

static void dnsk_release_owner(struct dnsk_hook *hook)
{
	if (hook->owner) {
		module_put(hook->owner);
		hook->owner = NULL;
	}
}

static int dnsk_kprobe_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct dnsk_hook *hook = container_of(kp, struct dnsk_hook, u.kprobe.kp);

	if (!hook->active)
		return 0;
	if (hook->u.kprobe.pre_handler)
		return hook->u.kprobe.pre_handler(kp, regs);
	return 0;
}

int dnsk_hooks_init(void)
{
	memset(dnsk_hooks, 0, sizeof(dnsk_hooks));
	return 0;
}

static int dnsk_remove_locked(struct dnsk_hook *hook)
{
	int ret = 0;

	switch (hook->type) {
	case DNSK_HOOK_KPROBE:
		unregister_kprobe(&hook->u.kprobe.kp);
		break;
	case DNSK_HOOK_SYSCALL:
		ret = dnsk_patch_pointer(hook->u.syscall.slot,
					 (void *)hook->u.syscall.original);
		break;
	case DNSK_HOOK_DATA:
		ret = dnsk_patch_pointer(hook->u.data.target,
					 (void *)hook->u.data.original);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret) {
		dnsk_err("failed to restore hook %s: %d\n", hook->key, ret);
		return ret;
	}

	hook->active = false;
	memset(hook->key, 0, sizeof(hook->key));
	dnsk_release_owner(hook);
	return 0;
}

int dnsk_hook_add_internal(const struct dnsk_hook_req *req)
{
	struct dnsk_hook *hook;
	int i;
	int ret;

	if (!req || !req->key || !req->key[0] || strlen(req->key) >= DNSK_HOOK_KEY_LEN)
		return -EINVAL;
	if (!req->owner)
		return -EINVAL;

	mutex_lock(&dnsk_hooks_lock);
	if (dnsk_find_locked(req->key)) {
		mutex_unlock(&dnsk_hooks_lock);
		return -EBUSY;
	}

	hook = NULL;
	for (i = 0; i < DNSK_MAX_HOOKS; i++) {
		if (!dnsk_hooks[i].active) {
			hook = &dnsk_hooks[i];
			break;
		}
	}
	if (!hook) {
		mutex_unlock(&dnsk_hooks_lock);
		return -ENOSPC;
	}

	if (!try_module_get(req->owner)) {
		mutex_unlock(&dnsk_hooks_lock);
		return -ENOENT;
	}

	memset(hook, 0, sizeof(*hook));
	strscpy(hook->key, req->key, sizeof(hook->key));
	hook->type = req->type;
	hook->owner = req->owner;

	switch (req->type) {
	case DNSK_HOOK_KPROBE:
		if (!req->symbol || !req->replacement) {
			ret = -EINVAL;
			break;
		}
		hook->u.kprobe.symbol = req->symbol;
		hook->u.kprobe.pre_handler =
			(int (*)(struct kprobe *, struct pt_regs *))req->replacement;
		hook->u.kprobe.kp.symbol_name = req->symbol;
		hook->u.kprobe.kp.pre_handler = dnsk_kprobe_pre;
		ret = register_kprobe(&hook->u.kprobe.kp);
		if (!ret) {
			hook->u.kprobe.addr =
				(unsigned long)hook->u.kprobe.kp.addr;
			if (req->original_out)
				*req->original_out = (void *)hook->u.kprobe.addr;
		}
		break;
	case DNSK_HOOK_SYSCALL: {
		unsigned long *table = dnsk_resolve_symbol("sys_call_table");
		unsigned long original;

		if (!table || req->syscall_nr >= 512 || !req->replacement) {
			ret = -ENOENT;
			break;
		}
		hook->u.syscall.slot = table + req->syscall_nr;
		hook->u.syscall.nr = req->syscall_nr;
		original = READ_ONCE(*hook->u.syscall.slot);
		if (!original || !dnsk_patch_ready()) {
			ret = -EOPNOTSUPP;
			break;
		}
		hook->u.syscall.original = original;
		ret = dnsk_patch_pointer(hook->u.syscall.slot, req->replacement);
		if (!ret && req->original_out)
			*req->original_out = (void *)original;
		break;
	}
	case DNSK_HOOK_DATA: {
		unsigned long original;

		if (!req->target || !req->replacement) {
			ret = -EINVAL;
			break;
		}
		hook->u.data.target = req->target;
		original = READ_ONCE(*(unsigned long *)req->target);
		if (!original || !dnsk_patch_ready()) {
			ret = -EOPNOTSUPP;
			break;
		}
		hook->u.data.original = original;
		ret = dnsk_patch_pointer(req->target, req->replacement);
		if (!ret && req->original_out)
			*req->original_out = (void *)original;
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	if (ret) {
		memset(hook->key, 0, sizeof(hook->key));
		dnsk_release_owner(hook);
		mutex_unlock(&dnsk_hooks_lock);
		return ret;
	}

	hook->active = true;
	mutex_unlock(&dnsk_hooks_lock);
	return 0;
}

int dnsk_hook_del_internal(const char *key)
{
	struct dnsk_hook *hook;
	int ret;

	if (!key)
		return -EINVAL;

	mutex_lock(&dnsk_hooks_lock);
	hook = dnsk_find_locked(key);
	if (!hook) {
		mutex_unlock(&dnsk_hooks_lock);
		return -ENOENT;
	}
	ret = dnsk_remove_locked(hook);
	mutex_unlock(&dnsk_hooks_lock);

	if (!ret)
		synchronize_rcu();
	return ret;
}

unsigned int dnsk_hook_count(void)
{
	unsigned int n = 0;
	int i;

	mutex_lock(&dnsk_hooks_lock);
	for (i = 0; i < DNSK_MAX_HOOKS; i++)
		if (dnsk_hooks[i].active)
			n++;
	mutex_unlock(&dnsk_hooks_lock);
	return n;
}

void dnsk_hooks_exit(void)
{
	int i;

	mutex_lock(&dnsk_hooks_lock);
	for (i = 0; i < DNSK_MAX_HOOKS; i++) {
		if (dnsk_hooks[i].active)
			dnsk_remove_locked(&dnsk_hooks[i]);
	}
	mutex_unlock(&dnsk_hooks_lock);
}
