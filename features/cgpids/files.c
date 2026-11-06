// SPDX-License-Identifier: GPL-2.0-only
/*
 * Real pids.max/current/peak/events files backed by dnsk_pids_state, plus
 * the cgroup.controllers/subtree_control view and cgroup mkdir/rmdir
 * lifecycle hooks.  No display-only state: every file reads/writes the
 * accounting used by fork enforcement.
 */

#include <linux/build_bug.h>
#include <linux/cgroup.h>
#include <linux/cgroup-defs.h>
#include <linux/kernfs.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "cgpids.h"

#define DNSK_SEQ_PRIVATE(seq) \
	(*(void **)((char *)(seq) + DNSK_SEQ_FILE_PRIVATE))

static_assert(offsetof(struct cgroup, self) == 0);
static_assert(offsetof(struct cgroup, kn) == 264);
static_assert(offsetof(struct cgroup_subsys_state, cgroup) == 0);
static_assert(offsetof(struct cgroup_subsys_state, parent) == 192);
static_assert(offsetof(struct css_set, dfl_cgrp) == 80);
static_assert(offsetof(struct css_set, mg_tasks) == 112);
static_assert(offsetof(struct css_set, mg_node) == 384);
static_assert(offsetof(struct css_set, mg_src_cgrp) == 400);
static_assert(offsetof(struct css_set, mg_dst_cgrp) == 408);
static_assert(offsetof(struct css_set, mg_dst_cset) == 416);
/* task_struct offset varies with the kernel config; the module is built
 * against the same headers as the target kernel. */
static_assert(offsetof(struct seq_file, buf) == 0);
static_assert(offsetof(struct seq_file, count) == 24);
static_assert(offsetof(struct seq_file, private) == DNSK_SEQ_FILE_PRIVATE);
static_assert(offsetof(struct kernfs_node, priv) == 104);
static_assert(offsetof(struct cftype, name) == 0);
static_assert(offsetof(struct cftype, seq_show) == 152);
static_assert(offsetof(struct cftype, write) == 200);
static_assert(sizeof(struct cftype) == 216);
static_assert(offsetof(struct kernfs_syscall_ops, mkdir) == 8);
static_assert(offsetof(struct kernfs_syscall_ops, rmdir) == 16);
static_assert(offsetof(struct cgroup_root, cgrp) == 64);

struct cgroup_subsys_state *(*dnsk_of_css)(struct kernfs_open_file *of);

static int (*orig_controllers_show)(struct seq_file *seq, void *v);
static int (*orig_subtree_show)(struct seq_file *seq, void *v);
static ssize_t (*orig_subtree_write)(struct kernfs_open_file *of, char *buf,
				     size_t nbytes, loff_t off);
static int (*orig_mkdir)(struct kernfs_node *parent, const char *name,
			 umode_t mode);
static int (*orig_rmdir)(struct kernfs_node *kn);

/*
 * Some cgroup internals are static and never address-taken in the kernel, so
 * the kernel compiler did not emit a KCFI preamble for them.  Call them from
 * __nocfi helpers: the signatures are verified against BTF/source, and the
 * helpers themselves are direct-called, so their missing preamble is fine.
 */
static __nocfi noinline int dnsk_call_init_cftypes(void)
{
	return dnsk_cgroup_init_cftypes(NULL, dnsk_pids_files);
}

static __nocfi noinline int dnsk_call_addrm(struct cgroup *cgrp, bool add)
{
	return dnsk_cgroup_addrm_files(&cgrp->self, cgrp, dnsk_pids_files,
				       add);
}

static __nocfi noinline struct cgroup_subsys_state *
dnsk_call_of_css(struct kernfs_open_file *of)
{
	return dnsk_of_css(of);
}

static __nocfi noinline int dnsk_call_mkdir(struct kernfs_node *parent,
					    const char *name, umode_t mode)
{
	return orig_mkdir(parent, name, mode);
}

static __nocfi noinline int dnsk_call_rmdir(struct kernfs_node *kn)
{
	return orig_rmdir(kn);
}

static __nocfi noinline int dnsk_call_controllers_show(struct seq_file *seq,
						       void *v)
{
	return orig_controllers_show(seq, v);
}

static __nocfi noinline int dnsk_call_subtree_show(struct seq_file *seq,
						   void *v)
{
	return orig_subtree_show(seq, v);
}

static __nocfi noinline ssize_t
dnsk_call_subtree_write(struct kernfs_open_file *of, char *buf, size_t nbytes,
			loff_t off)
{
	return orig_subtree_write(of, buf, nbytes, off);
}

static struct cgroup *dnsk_of_cgrp(struct kernfs_open_file *of)
{
	struct cgroup_subsys_state *css = dnsk_call_of_css(of);

	return css ? css->cgroup : NULL;
}

static struct cgroup *dnsk_seq_cgrp(struct seq_file *seq)
{
	struct kernfs_open_file *of = DNSK_SEQ_PRIVATE(seq);

	return of ? dnsk_of_cgrp(of) : NULL;
}

static void dnsk_seq_append_pids(struct seq_file *seq)
{
	if (seq->count && seq->buf[seq->count - 1] == '\n')
		seq->count--;
	if (seq->count)
		seq_putc(seq, ' ');
	seq_puts(seq, "pids\n");
}

static int dnsk_controllers_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp;
	struct dnsk_pids_state *st;
	bool show = false;
	int ret;

	ret = dnsk_call_controllers_show(seq, v);
	if (ret)
		return ret;
	cgrp = dnsk_seq_cgrp(seq);
	if (!cgrp)
		return 0;

	rcu_read_lock();
	st = dnsk_pids_lookup(cgrp);
	/* Root always advertises pids; children only once we manage them. */
	show = !cgrp->self.parent || st;
	rcu_read_unlock();
	if (show)
		dnsk_seq_append_pids(seq);
	return 0;
}

static int dnsk_subtree_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp;
	struct dnsk_pids_state *st;
	bool show = false;
	int ret;

	ret = dnsk_call_subtree_show(seq, v);
	if (ret)
		return ret;
	cgrp = dnsk_seq_cgrp(seq);
	if (!cgrp)
		return 0;

	rcu_read_lock();
	st = dnsk_pids_lookup(cgrp);
	if (st && st->enabled)
		show = true;
	rcu_read_unlock();
	if (show)
		dnsk_seq_append_pids(seq);
	return 0;
}

static void dnsk_filter_pids(char *buf, bool *enable, bool *disable)
{
	char *p = buf, *tok, *out = buf;

	*enable = false;
	*disable = false;
	while ((tok = strsep(&p, " "))) {
		size_t len;

		if (!tok[0])
			continue;
		if (!strcmp(tok, "+pids")) {
			*enable = true;
			continue;
		}
		if (!strcmp(tok, "-pids")) {
			*disable = true;
			continue;
		}
		len = strlen(tok);
		if (out != buf)
			*out++ = ' ';
		if (out != tok)
			memmove(out, tok, len);
		out += len;
	}
	*out = '\0';
}

static bool dnsk_child_enabled(struct cgroup *cgrp)
{
	struct cgroup_subsys_state *css;
	bool enabled = false;

	mutex_lock(dnsk_cgroup_mutex);
	list_for_each_entry(css, &cgrp->self.children, sibling) {
		struct dnsk_pids_state *st;

		rcu_read_lock();
		st = dnsk_pids_lookup(css->cgroup);
		if (st && st->enabled)
			enabled = true;
		rcu_read_unlock();
		if (enabled)
			break;
	}
	mutex_unlock(dnsk_cgroup_mutex);
	return enabled;
}

static ssize_t dnsk_subtree_write(struct kernfs_open_file *of, char *buf,
				  size_t nbytes, loff_t off)
{
	struct cgroup *cgrp = dnsk_of_cgrp(of);
	struct dnsk_pids_state *st;
	bool enable, disable;
	ssize_t ret;

	if (!cgrp)
		return -ENODEV;
	buf = strstrip(buf);
	dnsk_filter_pids(buf, &enable, &disable);
	if (!enable && !disable)
		return dnsk_call_subtree_write(of, buf, nbytes, off);

	if (disable && dnsk_child_enabled(cgrp))
		return -EBUSY;

	st = dnsk_pids_ensure(cgrp);
	if (!st)
		return -ENOMEM;

	ret = dnsk_call_subtree_write(of, buf, nbytes, off);
	if (ret >= 0) {
		if (enable)
			st->enabled = true;
		if (disable)
			st->enabled = false;
	}
	return ret;
}

static ssize_t dnsk_max_write(struct kernfs_open_file *of, char *buf,
			      size_t nbytes, loff_t off)
{
	struct cgroup *cgrp = dnsk_of_cgrp(of);
	struct dnsk_pids_state *st;
	long long limit;
	int ret;

	if (!cgrp)
		return -ENODEV;
	st = dnsk_pids_ensure(cgrp);
	if (!st)
		return -ENOMEM;

	buf = strstrip(buf);
	if (!strcmp(buf, "max")) {
		limit = DNSK_PIDS_MAX;
	} else {
		ret = kstrtoll(buf, 0, &limit);
		if (ret)
			return ret;
		if (limit < 0 || limit >= DNSK_PIDS_MAX)
			return -EINVAL;
	}
	atomic64_set(&st->limit, limit);
	return nbytes;
}

static int dnsk_max_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = dnsk_seq_cgrp(seq);
	struct dnsk_pids_state *st;
	s64 limit;

	if (!cgrp)
		return 0;
	rcu_read_lock();
	st = dnsk_pids_lookup(cgrp);
	limit = st ? atomic64_read(&st->limit) : DNSK_PIDS_MAX;
	rcu_read_unlock();
	if (limit >= DNSK_PIDS_MAX)
		seq_puts(seq, "max\n");
	else
		seq_printf(seq, "%lld\n", limit);
	return 0;
}

static s64 dnsk_current_read(struct cgroup_subsys_state *css,
			     struct cftype *cft)
{
	struct dnsk_pids_state *st;
	s64 val;

	rcu_read_lock();
	st = dnsk_pids_lookup(css->cgroup);
	val = st ? atomic64_read(&st->counter) : 0;
	rcu_read_unlock();
	return val;
}

static s64 dnsk_peak_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct dnsk_pids_state *st;
	s64 val;

	rcu_read_lock();
	st = dnsk_pids_lookup(css->cgroup);
	val = st ? atomic64_read(&st->watermark) : 0;
	rcu_read_unlock();
	return val;
}

static int dnsk_events_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = dnsk_seq_cgrp(seq);
	struct dnsk_pids_state *st;
	s64 val = 0;

	if (!cgrp)
		return 0;
	rcu_read_lock();
	st = dnsk_pids_lookup(cgrp);
	if (st)
		val = atomic64_read(&st->events[DNSK_PIDS_EVENT_MAX]);
	rcu_read_unlock();
	seq_printf(seq, "max %lld\n", val);
	return 0;
}

static int dnsk_events_local_show(struct seq_file *seq, void *v)
{
	struct cgroup *cgrp = dnsk_seq_cgrp(seq);
	struct dnsk_pids_state *st;
	s64 val = 0;

	if (!cgrp)
		return 0;
	rcu_read_lock();
	st = dnsk_pids_lookup(cgrp);
	if (st)
		val = atomic64_read(&st->events_local[DNSK_PIDS_EVENT_MAX]);
	rcu_read_unlock();
	seq_printf(seq, "max %lld\n", val);
	return 0;
}

struct cftype dnsk_pids_files[] = {
	{
		.name = "pids.max",
		.write = dnsk_max_write,
		.seq_show = dnsk_max_show,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "pids.current",
		.read_s64 = dnsk_current_read,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "pids.peak",
		.read_s64 = dnsk_peak_read,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "pids.events",
		.seq_show = dnsk_events_show,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{
		.name = "pids.events.local",
		.seq_show = dnsk_events_local_show,
		.flags = CFTYPE_NOT_ON_ROOT,
	},
	{ }
};

static struct cgroup *dnsk_find_child(struct cgroup *parent, const char *name)
{
	struct cgroup_subsys_state *css;

	list_for_each_entry(css, &parent->self.children, sibling) {
		struct cgroup *child = css->cgroup;

		if (child->kn && child->kn->name &&
		    !strcmp(child->kn->name, name))
			return child;
	}
	return NULL;
}

static bool dnsk_on_dfl(struct cgroup *cgrp)
{
	struct cgroup_subsys_state *css = &cgrp->self;

	while (css->parent)
		css = css->parent;
	return css->cgroup == (struct cgroup *)((char *)dnsk_cgrp_dfl_root + 64);
}

static int dnsk_mkdir(struct kernfs_node *parent_kn, const char *name,
		      umode_t mode)
{
	struct cgroup *parent, *child;
	struct dnsk_pids_state *st;
	int ret, add_ret;

	pr_info("dnsk_cgpids: mkdir name=%s\n", name);
	ret = dnsk_call_mkdir(parent_kn, name, mode);
	if (ret)
		return ret;
	parent = parent_kn->priv;
	if (!parent)
		return 0;

	mutex_lock(dnsk_cgroup_mutex);
	child = dnsk_find_child(parent, name);
	pr_info("dnsk_cgpids: mkdir parent=%px child=%px dfl=%d\n",
		parent, child, child ? dnsk_on_dfl(child) : -1);
	if (child)
		pr_info("dnsk_cgpids: child_kn=%s flags=%u kf=%px ss=%px sp=%px\n",
			child->kn->name, dnsk_pids_files[0].flags,
			dnsk_pids_files[0].kf_ops, dnsk_pids_files[0].ss,
			child->self.parent);
	if (!child || !dnsk_on_dfl(child)) {
		mutex_unlock(dnsk_cgroup_mutex);
		return 0;
	}
	st = dnsk_pids_ensure(child);
	if (!st) {
		mutex_unlock(dnsk_cgroup_mutex);
		return -ENOMEM;
	}
	add_ret = 0;
	if (!st->files_added) {
		add_ret = dnsk_call_addrm(child, true);
		pr_info("dnsk_cgpids: mkdir addrm=%d files=%d\n",
			add_ret, st->files_added);
		if (!add_ret)
			st->files_added = true;
	}
	mutex_unlock(dnsk_cgroup_mutex);

	if (add_ret) {
		mutex_lock(dnsk_cgroup_mutex);
		dnsk_call_addrm(child, false);
		mutex_unlock(dnsk_cgroup_mutex);
		dnsk_pids_destroy(child);
	}
	return add_ret;
}

static int dnsk_rmdir(struct kernfs_node *kn)
{
	struct cgroup *cgrp = kn->priv;
	int ret;

	ret = dnsk_call_rmdir(kn);
	if (!ret && cgrp)
		dnsk_pids_destroy(cgrp);
	return ret;
}

static void dnsk_restore_base_files(void)
{
	struct cftype *cfts = dnsk_cgroup_base_files;

	for (; cfts->name[0]; cfts++) {
		if (!strcmp(cfts->name, "cgroup.controllers") &&
		    cfts->seq_show == dnsk_controllers_show)
			WRITE_ONCE(cfts->seq_show, orig_controllers_show);
		else if (!strcmp(cfts->name, "cgroup.subtree_control")) {
			if (cfts->seq_show == dnsk_subtree_show)
				WRITE_ONCE(cfts->seq_show, orig_subtree_show);
			if (cfts->write == dnsk_subtree_write)
				WRITE_ONCE(cfts->write, orig_subtree_write);
		}
	}
}

int dnsk_cgpids_files_init(void)
{
	struct cftype *cfts = dnsk_cgroup_base_files;
	bool found_ctrl = false, found_sub = false;
	int ret;

	mutex_lock(dnsk_cgroup_mutex);
	ret = dnsk_call_init_cftypes();
	if (ret)
		goto out;

	for (; cfts->name[0]; cfts++) {
		if (!strcmp(cfts->name, "cgroup.controllers")) {
			orig_controllers_show = cfts->seq_show;
			WRITE_ONCE(cfts->seq_show, dnsk_controllers_show);
			found_ctrl = true;
		} else if (!strcmp(cfts->name, "cgroup.subtree_control")) {
			orig_subtree_show = cfts->seq_show;
			orig_subtree_write = cfts->write;
			WRITE_ONCE(cfts->seq_show, dnsk_subtree_show);
			WRITE_ONCE(cfts->write, dnsk_subtree_write);
			found_sub = true;
		}
	}
	if (!found_ctrl || !found_sub) {
		dnsk_restore_base_files();
		ret = -ENOENT;
	}
out:
	mutex_unlock(dnsk_cgroup_mutex);
	return ret;
}

void dnsk_cgpids_files_exit(void)
{
	struct cftype *cft;

	mutex_lock(dnsk_cgroup_mutex);
	dnsk_restore_base_files();
	for (cft = dnsk_pids_files; cft->name[0]; cft++) {
		cft->kf_ops = NULL;
		cft->ss = NULL;
		cft->flags &= ~__CFTYPE_ADDED;
	}
	mutex_unlock(dnsk_cgroup_mutex);
}

void dnsk_cgpids_install_sysops(void)
{
	struct kernfs_syscall_ops *ops = dnsk_cgroup_kf_syscall_ops;

	orig_mkdir = ops->mkdir;
	orig_rmdir = ops->rmdir;
	pr_info("dnsk_cgpids: sysops mkdir=%ps rmdir=%ps\n",
		orig_mkdir, orig_rmdir);
	WRITE_ONCE(ops->mkdir, dnsk_mkdir);
	WRITE_ONCE(ops->rmdir, dnsk_rmdir);
}

void dnsk_cgpids_restore_sysops(void)
{
	struct kernfs_syscall_ops *ops = dnsk_cgroup_kf_syscall_ops;

	if (ops->mkdir == dnsk_mkdir)
		WRITE_ONCE(ops->mkdir, orig_mkdir);
	if (ops->rmdir == dnsk_rmdir)
		WRITE_ONCE(ops->rmdir, orig_rmdir);
}
