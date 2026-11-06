// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Device-side preflight.  This is intentionally conservative: it checks the
 * facts dnsk_core depends on and refuses to report success when a required
 * symbol or environment fact is missing.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/*
 * Android Bionic requires a 64-byte-aligned PT_TLS for arm64.  A static
 * clang binary may otherwise be rejected by the loader ("TLS segment is
 * underaligned") even when the program itself has no TLS.  This aligned
 * thread-local object forces the linker's PT_TLS alignment to 64.
 */
#if defined(__ANDROID__) && defined(__aarch64__)
__attribute__((visibility("default"), aligned(64)))
_Thread_local int dnsk_android_tls_alignment_fix = 0;
#endif

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct btf_member {
	char name[32];
	uint32_t bit_off;
};

struct btf_type_info {
	uint32_t size;
	uint32_t n;
	/* task_struct has 234 members; checks beyond the cap fail closed. */
	struct btf_member members[256];
};

struct btf_field {
	const char *name;
	uint32_t off;
};

struct btf_nf_layout {
	int hook_ops;
	int hook_state;
	int conn;
	int pernet_ops;
	int nfnetlink;
	int nfnl_callback;
	int nat_hook;
	int nat_range2;
	int rhashtable;
	int ok;
};

struct btf_layout {
	struct btf_type_info match;
	struct btf_type_info target;
	int found_match;
	int found_target;
	struct btf_nf_layout nf;
	int locks_ok;
	int xt_params_ok;
	int skb_ok;
	int net_ok;
	int fs_ok;
	int ns_ok;
	int cgpids_ok;
	int tmpfsx_ok;
	int seq_private;
};

static uint32_t btf_rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t btf_rd16(const unsigned char *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static const char *btf_str(const unsigned char *strs, size_t str_len,
			   uint32_t off)
{
	const char *s;

	if (off >= str_len)
		return NULL;
	s = (const char *)strs + off;
	if (!memchr(s, 0, str_len - off))
		return NULL;
	return s;
}

static int btf_find_struct_info(const unsigned char *d, size_t len,
				const char *want, struct btf_type_info *out)
{
	uint32_t hdr_len, type_off, type_len, str_off, str_len;
	const unsigned char *types;
	const unsigned char *strs;
	size_t pos = 0;

	if (len < 24 || btf_rd16(d) != 0xeb9f || d[2] != 1)
		return -1;
	hdr_len = btf_rd32(d + 4);
	if (hdr_len < 24)
		return -1;
	type_off = btf_rd32(d + 8);
	type_len = btf_rd32(d + 12);
	str_off = btf_rd32(d + 16);
	str_len = btf_rd32(d + 20);
	if (hdr_len > len || type_off > len - hdr_len ||
	    type_len > len - hdr_len - type_off ||
	    str_off > len - hdr_len || str_len > len - hdr_len - str_off)
		return -1;
	types = d + hdr_len + type_off;
	strs = d + hdr_len + str_off;

	while (pos + 12 <= type_len) {
		uint32_t name_off = btf_rd32(types + pos);
		uint32_t info = btf_rd32(types + pos + 4);
		uint32_t size_type = btf_rd32(types + pos + 8);
		unsigned int kind = (info >> 24) & 0x1f;
		unsigned int vlen = info & 0xffff;
		int kflag = (int)(info >> 31);
		const char *name = btf_str(strs, str_len, name_off);

		pos += 12;
		if (name && kind == 4 && !strcmp(name, want)) {
			unsigned int i;

			if ((size_t)vlen * 12 > type_len - pos)
				return -1;
			out->size = size_type;
			out->n = 0;
			for (i = 0; i < vlen; i++) {
				uint32_t mo = btf_rd32(types + pos);
				uint32_t mb = btf_rd32(types + pos + 8);
				const char *mn;

				pos += 12;
				if (i >= ARRAY_SIZE(out->members))
					continue;
				mn = btf_str(strs, str_len, mo);
				out->members[out->n].bit_off =
					(uint32_t)(kflag ? (mb & 0xffffff) : mb);
				snprintf(out->members[out->n].name,
					 sizeof(out->members[out->n].name),
					 "%s", mn ? mn : "");
				out->n++;
			}
			return 0;
		}
		switch (kind) {
		case 1: /* INT */
		case 14: /* VAR */
		case 17: /* DECL_TAG */
			pos += 4;
			break;
		case 3: /* ARRAY */
			pos += 12;
			break;
		case 4: /* STRUCT */
		case 5: /* UNION */
		case 15: /* DATASEC */
			pos += (size_t)vlen * 12;
			break;
		case 6: /* ENUM */
		case 13: /* FUNC_PROTO */
			pos += (size_t)vlen * 8;
			break;
		case 19: /* ENUM64 */
			pos += (size_t)vlen * 12;
			break;
		default:
			break;
		}
		if (pos > type_len)
			return -1;
	}
	return -1;
}

static int btf_member_off(const struct btf_type_info *t, const char *name,
			  uint32_t *off)
{
	uint32_t i;

	for (i = 0; i < t->n; i++) {
		if (!strcmp(t->members[i].name, name)) {
			*off = t->members[i].bit_off / 8;
			return 0;
		}
	}
	return -1;
}

static int member_is(const struct btf_type_info *t, const char *name,
		     uint32_t expected)
{
	uint32_t off;

	return btf_member_off(t, name, &off) == 0 && off == expected;
}

static int xt_layout_matches(const struct btf_type_info *t, uint32_t size,
			     int target)
{
	int compat = size == 128;
	uint32_t me = compat ? 88 : 72;
	uint32_t table = compat ? 96 : 80;
	uint32_t size_off = compat ? 104 : 88;
	uint32_t usersize = compat ? 108 : 92;
	uint32_t hooks = compat ? 116 : 96;
	uint32_t proto = compat ? 120 : 100;
	uint32_t family = compat ? 122 : 102;

	if (t->size != size)
		return 0;
	if (!member_is(t, "list", 0) || !member_is(t, "name", 16) ||
	    !member_is(t, "revision", 45) ||
	    !member_is(t, "checkentry", 56) ||
	    !member_is(t, "destroy", 64) || !member_is(t, "me", me) ||
	    !member_is(t, "table", table) || !member_is(t, "usersize", usersize) ||
	    !member_is(t, "hooks", hooks) || !member_is(t, "proto", proto) ||
	    !member_is(t, "family", family))
		return 0;
	if (!compat)
		return target ? member_is(t, "target", 48) &&
				member_is(t, "targetsize", size_off) :
				member_is(t, "match", 48) &&
				member_is(t, "matchsize", size_off);
	if (!member_is(t, "compat_from_user", 72) ||
	    !member_is(t, "compat_to_user", 80) ||
	    !member_is(t, "compatsize", 112))
		return 0;
	return target ? member_is(t, "target", 48) &&
			member_is(t, "targetsize", size_off) :
			member_is(t, "match", 48) &&
			member_is(t, "matchsize", size_off);
}

static int btf_fields_match(const unsigned char *d, size_t len,
			    const char *name, uint32_t size,
			    const struct btf_field *fields, size_t n)
{
	struct btf_type_info t;

	if (btf_find_struct_info(d, len, name, &t) != 0 || t.size != size)
		return 0;
	for (size_t i = 0; i < n; i++)
		if (!member_is(&t, fields[i].name, fields[i].off))
			return 0;
	return 1;
}

static void btf_check_netfilter(const unsigned char *d, size_t len,
				struct btf_nf_layout *nf)
{
	static const struct btf_field hook_ops[] = {
		{ "hook", 0 }, { "dev", 8 }, { "priv", 16 }, { "pf", 24 },
		{ "hook_ops_type", 25 }, { "hooknum", 28 }, { "priority", 32 },
	};
	static const struct btf_field conn[] = {
		{ "status", 128 }, { "nat_bysource", 144 }, { "master", 160 },
		{ "mark", 168 }, { "secmark", 172 }, { "ext", 176 },
		{ "proto", 184 },
	};
	static const struct btf_field pernet_ops[] = {
		{ "init", 16 }, { "pre_exit", 24 }, { "exit", 32 },
		{ "exit_batch", 40 }, { "exit_batch_rtnl", 48 }, { "id", 56 },
		{ "size", 64 },
	};
	static const struct btf_field nfnetlink_subsys[] = {
		{ "owner", 24 }, { "commit", 32 }, { "abort", 40 },
		{ "valid_genid", 48 },
	};
	static const struct btf_field nfnl_callback[] = {
		{ "call", 0 }, { "policy", 8 }, { "type", 16 },
		{ "attr_count", 20 },
	};
	static const struct btf_field hook_state[] = {
		{ "hook", 0 }, { "pf", 1 }, { "in", 8 }, { "out", 16 },
		{ "sk", 24 }, { "net", 32 }, { "okfn", 40 },
	};
	static const struct btf_field nat_hook[] = {
		{ "parse_nat_setup", 0 }, { "decode_session", 8 },
		{ "remove_nat_bysrc", 16 },
	};
	static const struct btf_field nat_range2[] = {
		{ "flags", 0 }, { "min_addr", 4 }, { "max_addr", 20 },
		{ "min_proto", 36 }, { "max_proto", 38 }, { "base_proto", 40 },
	};

	memset(nf, 0, sizeof(*nf));
	nf->hook_ops = btf_fields_match(d, len, "nf_hook_ops", 40, hook_ops,
					ARRAY_SIZE(hook_ops));
	nf->hook_state = btf_fields_match(d, len, "nf_hook_state", 48,
					  hook_state, ARRAY_SIZE(hook_state));
	nf->conn = btf_fields_match(d, len, "nf_conn", 272, conn,
				    ARRAY_SIZE(conn));
	nf->pernet_ops = btf_fields_match(d, len, "pernet_operations", 72,
					  pernet_ops, ARRAY_SIZE(pernet_ops));
	nf->nfnetlink = btf_fields_match(d, len, "nfnetlink_subsystem", 64,
					 nfnetlink_subsys,
					 ARRAY_SIZE(nfnetlink_subsys));
	nf->nfnl_callback = btf_fields_match(d, len, "nfnl_callback", 32,
					     nfnl_callback,
					     ARRAY_SIZE(nfnl_callback));
	nf->nat_hook = btf_fields_match(d, len, "nf_nat_hook", 32, nat_hook,
					ARRAY_SIZE(nat_hook));
	nf->nat_range2 = btf_fields_match(d, len, "nf_nat_range2", 44,
					  nat_range2, ARRAY_SIZE(nat_range2));
	nf->rhashtable = btf_fields_match(d, len, "rhashtable", 160, NULL, 0);
	nf->ok = nf->hook_ops && nf->hook_state && nf->conn &&
		 nf->pernet_ops && nf->nfnetlink && nf->nfnl_callback &&
		 nf->nat_hook && nf->nat_range2 && nf->rhashtable;
}

static void btf_check_ns(const unsigned char *d, size_t len,
			 struct btf_layout *l)
{
	static const struct btf_field task[] = {
		{ "pid", 1800 }, { "tgid", 1804 }, { "real_parent", 1816 },
		{ "parent", 1824 }, { "group_leader", 1864 },
		{ "thread_pid", 1904 }, { "nsproxy", 2384 },
		{ "signal", 2392 }, { "sighand", 2400 },
		{ "real_cred", 2296 }, { "cred", 2304 },
		{ "cgroups", 2872 }, { "cg_list", 2880 },
		{ "security", 3288 }, { "comm", 2320 }, { "fs", 2360 },
		{ "files", 2368 }, { "mm", 1672 }, { "active_mm", 1680 },
	};
	static const struct btf_field nsproxy[] = {
		{ "uts_ns", 8 }, { "ipc_ns", 16 }, { "mnt_ns", 24 },
		{ "pid_ns_for_children", 32 }, { "net_ns", 40 },
		{ "time_ns", 48 }, { "time_ns_for_children", 56 },
		{ "cgroup_ns", 64 },
	};
	static const struct btf_field pidns[] = {
		{ "idr", 0 }, { "child_reaper", 48 }, { "level", 64 },
		{ "parent", 72 }, { "user_ns", 80 }, { "ucounts", 88 },
		{ "ns", 104 }, { "memfd_noexec_scope", 128 },
	};
	static const struct btf_field ipcns[] = {
		{ "ids", 0 }, { "sem_ctls", 792 }, { "mq_mnt", 960 },
		{ "mq_sysctls", 1096 }, { "ipc_sysctls", 1208 },
		{ "user_ns", 1216 }, { "ucounts", 1224 }, { "ns", 1240 },
	};
	static const struct btf_field userns[] = {
		{ "uid_map", 0 }, { "gid_map", 64 }, { "parent", 192 },
		{ "level", 200 }, { "ns", 216 }, { "ucounts", 488 },
	};
	static const struct btf_field cred[] = {
		{ "uid", 8 }, { "euid", 24 }, { "security", 128 },
		{ "user_ns", 144 }, { "ucounts", 152 },
	};
	static const struct btf_field pid[] = {
		{ "count", 0 }, { "level", 4 }, { "tasks", 32 },
	};
	static const struct btf_field upid[] = {
		{ "nr", 0 }, { "ns", 8 },
	};
	static const struct btf_field ns_common[] = {
		{ "stashed", 0 }, { "ops", 8 }, { "inum", 16 },
	};
	static const struct btf_field signal_struct[] = {
		{ "sigcnt", 0 }, { "nr_threads", 8 }, { "thread_head", 16 },
		{ "shared_pending", 64 },
	};
	static const struct btf_field sighand_struct[] = {
		{ "siglock", 0 }, { "action", 32 },
	};
	static const struct btf_field cgroup[] = {
		{ "self", 0 }, { "flags", 216 }, { "level", 224 },
		{ "subsys", 616 }, { "nr_dying_subsys", 680 },
		{ "e_csets", 736 }, { "psi", 1176 }, { "bpf", 1184 },
		{ "freezer", 1904 }, { "ancestors", 1936 },
	};
	static const struct btf_field css_set[] = {
		{ "subsys", 0 }, { "refcount", 64 }, { "dfl_cgrp", 80 },
		{ "cgrp_links", 336 },
	};

	l->ns_ok =
		btf_fields_match(d, len, "task_struct", 5184, task,
				 ARRAY_SIZE(task)) &&
		btf_fields_match(d, len, "nsproxy", 72, nsproxy,
				 ARRAY_SIZE(nsproxy)) &&
		btf_fields_match(d, len, "pid_namespace", 136, pidns,
				 ARRAY_SIZE(pidns)) &&
		btf_fields_match(d, len, "ipc_namespace", 1264, ipcns,
				 ARRAY_SIZE(ipcns)) &&
		btf_fields_match(d, len, "user_namespace", 632, userns,
				 ARRAY_SIZE(userns)) &&
		btf_fields_match(d, len, "cred", 184, cred,
				 ARRAY_SIZE(cred)) &&
		btf_fields_match(d, len, "pid", 112, pid, ARRAY_SIZE(pid)) &&
		btf_fields_match(d, len, "upid", 16, upid, ARRAY_SIZE(upid)) &&
		btf_fields_match(d, len, "ns_common", 24, ns_common,
				 ARRAY_SIZE(ns_common)) &&
		btf_fields_match(d, len, "signal_struct", 1120, signal_struct,
				 ARRAY_SIZE(signal_struct)) &&
		btf_fields_match(d, len, "sighand_struct", 2080,
				 sighand_struct, ARRAY_SIZE(sighand_struct)) &&
		btf_fields_match(d, len, "cgroup", 1984, cgroup,
				 ARRAY_SIZE(cgroup)) &&
		btf_fields_match(d, len, "css_set", 456, css_set,
				 ARRAY_SIZE(css_set));
}

static int btf_cgroup_subsys_ok(const unsigned char *d, size_t len)
{
	struct btf_type_info t;
	uint32_t off;

	if (btf_find_struct_info(d, len, "cgroup_subsys", &t) != 0 ||
	    t.size != 256)
		return 0;
	/* Device has no css_killed; DDK's 264-byte layout does. */
	if (btf_member_off(&t, "css_killed", &off) == 0)
		return 0;
	return member_is(&t, "can_attach", 72) &&
	       member_is(&t, "attach", 88) &&
	       member_is(&t, "can_fork", 104) &&
	       member_is(&t, "cancel_fork", 112) &&
	       member_is(&t, "fork", 120) &&
	       member_is(&t, "exit", 128) &&
	       member_is(&t, "release", 136) &&
	       member_is(&t, "bind", 144) &&
	       member_is(&t, "id", 156) &&
	       member_is(&t, "name", 160);
}

static int btf_kernfs_syscall_ops_ok(const unsigned char *d, size_t len)
{
	struct btf_type_info t;

	if (btf_find_struct_info(d, len, "kernfs_syscall_ops", &t) != 0)
		return 0;
	/* Android KABI appends reserved fields; the callbacks stay at 0..32. */
	if (t.size != 40 && t.size != 72)
		return 0;
	return member_is(&t, "show_options", 0) && member_is(&t, "mkdir", 8) &&
	       member_is(&t, "rmdir", 16) && member_is(&t, "rename", 24) &&
	       member_is(&t, "show_path", 32);
}

static void btf_check_cgpids(const unsigned char *d, size_t len,
			     struct btf_layout *l)
{
	static const struct btf_field cgroup[] = {
		{ "self", 0 }, { "flags", 216 }, { "level", 224 },
		{ "subsys", 616 }, { "nr_dying_subsys", 680 },
		{ "e_csets", 736 }, { "psi", 1176 }, { "bpf", 1184 },
		{ "freezer", 1904 }, { "ancestors", 1936 },
	};
	static const struct btf_field css_set[] = {
		{ "subsys", 0 }, { "refcount", 64 }, { "dfl_cgrp", 80 },
		{ "nr_tasks", 88 }, { "mg_tasks", 112 }, { "cgrp_links", 336 },
		{ "mg_node", 384 }, { "mg_src_cgrp", 400 },
		{ "mg_dst_cgrp", 408 }, { "mg_dst_cset", 416 },
	};
	static const struct btf_field cftype[] = {
		{ "name", 0 }, { "private", 64 }, { "max_write_len", 72 },
		{ "flags", 80 }, { "file_offset", 84 }, { "ss", 88 },
		{ "node", 96 }, { "kf_ops", 112 }, { "seq_show", 152 },
		{ "write", 200 }, { "poll", 208 },
	};
	static const struct btf_field css_state[] = {
		{ "cgroup", 0 }, { "ss", 8 }, { "refcnt", 16 },
		{ "sibling", 32 }, { "children", 48 }, { "id", 80 },
		{ "flags", 84 }, { "parent", 192 },
	};
	static const struct btf_field cgroup_file[] = {
		{ "kn", 0 }, { "notified_at", 8 }, { "notify_timer", 16 },
	};
	static const struct btf_field kernfs_node[] = {
		{ "parent", 8 }, { "name", 16 }, { "flags", 60 },
		{ "priv", 104 },
	};
	static const struct btf_field cgroup_root[] = {
		{ "subsys_mask", 8 }, { "cgrp", 64 }, { "nr_cgrps", 2056 },
	};
	static const struct btf_field cgroup_taskset[] = {
		{ "src_csets", 0 }, { "dst_csets", 16 }, { "nr_tasks", 32 },
	};
	static const struct btf_field cgroup_mgctx[] = {
		{ "preloaded_src_csets", 0 }, { "preloaded_dst_csets", 16 },
		{ "tset", 32 }, { "ss_mask", 96 },
	};
	static const struct btf_field task[] = {
		{ "cgroups", 2872 },
	};
	struct btf_type_info sf;
	uint32_t seq_off;

	l->seq_private = -1;
	if (btf_find_struct_info(d, len, "seq_file", &sf) == 0 &&
	    sf.size == 136 && btf_member_off(&sf, "private", &seq_off) == 0)
		l->seq_private = (int)seq_off;

	l->cgpids_ok = btf_cgroup_subsys_ok(d, len) &&
		       l->seq_private >= 0 &&
		       btf_fields_match(d, len, "cgroup", 1984, cgroup,
					ARRAY_SIZE(cgroup)) &&
		       btf_fields_match(d, len, "css_set", 456, css_set,
					ARRAY_SIZE(css_set)) &&
		       btf_fields_match(d, len, "cftype", 216, cftype,
					ARRAY_SIZE(cftype)) &&
		       btf_fields_match(d, len, "cgroup_subsys_state", 216,
					css_state, ARRAY_SIZE(css_state)) &&
		       btf_fields_match(d, len, "cgroup_file", 56, cgroup_file,
					ARRAY_SIZE(cgroup_file)) &&
		       btf_fields_match(d, len, "kernfs_node", 144, kernfs_node,
					ARRAY_SIZE(kernfs_node)) &&
		       btf_fields_match(d, len, "cgroup_root", 6272,
					cgroup_root, ARRAY_SIZE(cgroup_root)) &&
		       btf_fields_match(d, len, "cgroup_taskset", 64,
					cgroup_taskset,
					ARRAY_SIZE(cgroup_taskset)) &&
		       btf_fields_match(d, len, "cgroup_mgctx", 104,
					cgroup_mgctx,
					ARRAY_SIZE(cgroup_mgctx)) &&
		       btf_kernfs_syscall_ops_ok(d, len) &&
		       btf_fields_match(d, len, "task_struct", 5184, task,
					ARRAY_SIZE(task));
}

static void btf_check_tmpfsx(const unsigned char *d, size_t len,
			     struct btf_layout *l)
{
	static const struct btf_field shmem_info[] = {
		{ "lock", 0 }, { "seals", 4 }, { "xattrs", 80 },
		{ "vfs_inode", 112 },
	};
	static const struct btf_field xattr_handler[] = {
		{ "prefix", 8 }, { "list", 24 }, { "get", 32 }, { "set", 40 },
	};
	static const struct btf_field inode_ops[] = {
		{ "get_inode_acl", 24 }, { "listxattr", 120 },
		{ "get_acl", 160 }, { "set_acl", 168 },
	};
	static const struct btf_field super_block[] = {
		{ "s_magic", 96 }, { "s_xattr", 192 },
	};
	static const struct btf_field simple_xattrs[] = {
		{ "rb_root", 0 }, { "lock", 8 },
	};
	static const struct btf_field fc_ops[] = {
		{ "parse_param", 16 }, { "parse_monolithic", 24 },
	};

	l->tmpfsx_ok =
		btf_fields_match(d, len, "shmem_inode_info", 824, shmem_info,
				 ARRAY_SIZE(shmem_info)) &&
		btf_fields_match(d, len, "fs_context_operations", 48, fc_ops,
				 ARRAY_SIZE(fc_ops)) &&
		btf_fields_match(d, len, "xattr_handler", 48, xattr_handler,
				 ARRAY_SIZE(xattr_handler)) &&
		btf_fields_match(d, len, "inode_operations", 256, inode_ops,
				 ARRAY_SIZE(inode_ops)) &&
		btf_fields_match(d, len, "super_block", 1536, super_block,
				 ARRAY_SIZE(super_block)) &&
		btf_fields_match(d, len, "simple_xattrs", 16, simple_xattrs,
				 ARRAY_SIZE(simple_xattrs));
}

static void btf_check_common(const unsigned char *d, size_t len, int check_xt,
			     int check_fs, int check_ns, int check_cgpids,
			     struct btf_layout *l)
{
	static const struct btf_field mutex[] = {
		{ "owner", 0 }, { "wait_lock", 8 }, { "wait_list", 16 },
		{ "android_oem_data1", 32 },
	};
	static const struct btf_field rwsem[] = {
		{ "count", 0 }, { "owner", 8 }, { "wait_list", 24 },
		{ "android_vendor_data1", 40 }, { "android_oem_data1", 48 },
	};
	static const struct btf_field raw_spinlock[] = {
		{ "raw_lock", 0 },
	};
	static const struct btf_field wait_queue_head[] = {
		{ "lock", 0 }, { "head", 8 },
	};
	static const struct btf_field timer_list[] = {
		{ "entry", 0 }, { "expires", 16 }, { "function", 24 },
		{ "flags", 32 },
	};
	static const struct btf_field work_struct[] = {
		{ "data", 0 }, { "entry", 8 }, { "func", 24 },
	};
	static const struct btf_field xt_action_param[] = {
		{ "state", 16 }, { "thoff", 24 }, { "fragoff", 28 },
		{ "hotdrop", 30 },
	};
	static const struct btf_field xt_mtchk_param[] = {
		{ "net", 0 }, { "table", 8 }, { "entryinfo", 16 },
		{ "match", 24 }, { "matchinfo", 32 }, { "hook_mask", 40 },
		{ "family", 44 }, { "nft_compat", 45 },
	};
	static const struct btf_field xt_tgchk_param[] = {
		{ "net", 0 }, { "table", 8 }, { "entryinfo", 16 },
		{ "target", 24 }, { "targinfo", 32 }, { "hook_mask", 40 },
		{ "family", 44 }, { "nft_compat", 45 },
	};
	static const struct btf_field sk_buff[] = {
		{ "sk", 24 }, { "_nfct", 104 }, { "len", 112 },
		{ "data_len", 116 }, { "active_extensions", 127 },
		{ "tail", 208 }, { "end", 212 }, { "head", 216 },
		{ "data", 224 }, { "truesize", 232 }, { "users", 236 },
		{ "extensions", 240 },
	};
	static const struct btf_field skb_shared_info[] = {
		{ "flags", 0 }, { "nr_frags", 2 }, { "frag_list", 8 },
		{ "gso_type", 24 }, { "dataref", 32 },
		{ "destructor_arg", 40 }, { "frags", 88 },
	};
	static const struct btf_field net[] = {
		{ "user_ns", 80 }, { "proc_net", 160 }, { "sysctls", 176 },
		{ "ipv4", 768 }, { "ipv6", 1536 }, { "nf", 2384 },
		{ "ct", 2608 }, { "wext_nlevents", 2824 }, { "gen", 2848 },
		{ "xfrm", 2944 },
	};
	static const struct btf_field device[] = {
		{ "kobj", 0 }, { "devt", 760 },
	};
	static const struct btf_field kobject[] = {
		{ "entry", 8 },
	};
	static const struct btf_field kset[] = {
		{ "list", 0 }, { "list_lock", 16 },
	};
	static const struct btf_field super_block[] = {
		{ "s_root", 104 }, { "s_umount", 112 }, { "s_active", 180 },
	};
	static const struct btf_field inode[] = {
		{ "i_mode", 0 }, { "i_rdev", 76 }, { "i_private", 696 },
	};
	static const struct btf_field kstat[] = {
		{ "mode", 4 }, { "rdev", 44 },
	};
	static const struct btf_field vfsmount[] = {
		{ "mnt_root", 0 }, { "mnt_sb", 8 },
	};
	static const struct btf_field path[] = {
		{ "mnt", 0 }, { "dentry", 8 },
	};
	static const struct btf_field file_system_type[] = {
		{ "name", 0 }, { "fs_flags", 8 }, { "mount", 32 },
		{ "kill_sb", 40 }, { "owner", 48 },
	};

	l->locks_ok = btf_fields_match(d, len, "mutex", 48, mutex,
				       ARRAY_SIZE(mutex)) &&
		      btf_fields_match(d, len, "rw_semaphore", 64, rwsem,
				       ARRAY_SIZE(rwsem)) &&
		      btf_fields_match(d, len, "spinlock", 4, NULL, 0) &&
		      btf_fields_match(d, len, "raw_spinlock", 4, raw_spinlock,
				       ARRAY_SIZE(raw_spinlock)) &&
		      btf_fields_match(d, len, "wait_queue_head", 24,
				       wait_queue_head,
				       ARRAY_SIZE(wait_queue_head)) &&
		      btf_fields_match(d, len, "timer_list", 40, timer_list,
				       ARRAY_SIZE(timer_list)) &&
		      btf_fields_match(d, len, "work_struct", 32, work_struct,
				       ARRAY_SIZE(work_struct));
	if (check_fs)
		l->fs_ok =
			btf_fields_match(d, len, "device", 912, device,
					 ARRAY_SIZE(device)) &&
			btf_fields_match(d, len, "kobject", 96, kobject,
					 ARRAY_SIZE(kobject)) &&
			btf_fields_match(d, len, "kset", 160, kset,
					 ARRAY_SIZE(kset)) &&
			btf_fields_match(d, len, "super_block", 1536,
					 super_block, ARRAY_SIZE(super_block)) &&
			btf_fields_match(d, len, "inode", 704, inode,
					 ARRAY_SIZE(inode)) &&
			btf_fields_match(d, len, "kstat", 184, kstat,
					 ARRAY_SIZE(kstat)) &&
			btf_fields_match(d, len, "vfsmount", 64, vfsmount,
					 ARRAY_SIZE(vfsmount)) &&
			btf_fields_match(d, len, "path", 16, path,
					 ARRAY_SIZE(path)) &&
			btf_fields_match(d, len, "file_system_type", 72,
					 file_system_type,
					 ARRAY_SIZE(file_system_type));
	if (check_ns)
		btf_check_ns(d, len, l);
	if (check_cgpids)
		btf_check_cgpids(d, len, l);
	if (!check_xt)
		return;
	l->xt_params_ok =
		btf_fields_match(d, len, "xt_action_param", 32, xt_action_param,
				 ARRAY_SIZE(xt_action_param)) &&
		btf_fields_match(d, len, "xt_mtchk_param", 48, xt_mtchk_param,
				 ARRAY_SIZE(xt_mtchk_param)) &&
		btf_fields_match(d, len, "xt_tgchk_param", 48, xt_tgchk_param,
				 ARRAY_SIZE(xt_tgchk_param));
	l->skb_ok = btf_fields_match(d, len, "sk_buff", 248, sk_buff,
				     ARRAY_SIZE(sk_buff)) &&
		    btf_fields_match(d, len, "skb_shared_info", 360,
				     skb_shared_info,
				     ARRAY_SIZE(skb_shared_info));
	l->net_ok = btf_fields_match(d, len, "net", 4160, net,
				     ARRAY_SIZE(net));
}

static int btf_read_layout(const char *path, struct btf_layout *layout,
			   int check_xt, int check_nf, int check_fs,
			   int check_ns, int check_cgpids)
{
	FILE *fp;
	unsigned char *data = NULL;
	size_t cap = 0, used = 0;
	size_t n;
	int read_err;

	memset(layout, 0, sizeof(*layout));
	fp = fopen(path, "rb");
	if (!fp)
		return -1;
	while (1) {
		if (used == cap) {
			size_t next = cap ? cap * 2 : 1024 * 1024;
			unsigned char *grown;

			if (cap == 64 * 1024 * 1024) {
				int c = fgetc(fp);

				if (c == EOF)
					break;
				free(data);
				fclose(fp);
				return -1;
			}
			if (next > 64 * 1024 * 1024) {
				free(data);
				fclose(fp);
				return -1;
			}
			grown = realloc(data, next);
			if (!grown) {
				free(data);
				fclose(fp);
				return -1;
			}
			data = grown;
			cap = next;
		}
		n = fread(data + used, 1, cap - used, fp);
		used += n;
		if (n == 0)
			break;
	}
	read_err = ferror(fp);
	fclose(fp);
	if (read_err || used == 0) {
		free(data);
		return -1;
	}
	layout->found_match =
		btf_find_struct_info(data, used, "xt_match", &layout->match) == 0;
	layout->found_target =
		btf_find_struct_info(data, used, "xt_target", &layout->target) == 0;
	btf_check_common(data, used, check_xt, check_fs, check_ns,
			 check_cgpids, layout);
	btf_check_tmpfsx(data, used, layout);
	if (check_nf)
		btf_check_netfilter(data, used, &layout->nf);
	free(data);
	return 0;
}

enum sym_id {
	SYM_SYS_CALL_TABLE,
	SYM_INIT_MM,
	SYM_PANIC_NOTIFIER_LIST,
	SYM_STOP_MACHINE,
	SYM_REGISTER_KPROBE,
	SYM_KALLSYMS_LOOKUP_NAME,
	SYM_KALLSYMS_ON_EACH_SYMBOL,
	SYM_DCACHE_CLEAN_INVAL_POC,
	SYM_FLUSH_DCACHE_AREA,
	SYM_COUNT,
};

struct sym_req {
	const char *name;
	int found;
};

static struct sym_req required[] = {
	[SYM_SYS_CALL_TABLE] = { "sys_call_table", 0 },
	[SYM_INIT_MM] = { "init_mm", 0 },
	[SYM_PANIC_NOTIFIER_LIST] = { "panic_notifier_list", 0 },
	[SYM_STOP_MACHINE] = { "stop_machine", 0 },
	[SYM_REGISTER_KPROBE] = { "register_kprobe", 0 },
	[SYM_KALLSYMS_LOOKUP_NAME] = { "kallsyms_lookup_name", 0 },
	[SYM_KALLSYMS_ON_EACH_SYMBOL] = { "kallsyms_on_each_symbol", 0 },
	[SYM_DCACHE_CLEAN_INVAL_POC] = { "dcache_clean_inval_poc", 0 },
	[SYM_FLUSH_DCACHE_AREA] = { "__flush_dcache_area", 0 },
};

_Static_assert(ARRAY_SIZE(required) == SYM_COUNT, "required symbol table out of sync");

/* Resolved by ksud at load time; needed only by dnsk_netfilter. */
static struct sym_req nf_required[] = {
	{ "nf_nat_masquerade_inet_register_notifiers", 0 },
	{ "nf_nat_register_fn", 0 },
	{ "xt_register_matches", 0 },
	{ "xt_unregister_matches", 0 },
	{ "xt_register_target", 0 },
	{ "xt_unregister_target", 0 },
	{ "xt_check_proc_name", 0 },
	{ "nfnetlink_subsys_register", 0 },
	{ "rhashtable_init_noprof", 0 },
	{ "register_pernet_subsys", 0 },
	{ "nf_send_unreach", 0 },
	{ "nf_send_unreach6", 0 },
	{ "nf_send_reset", 0 },
	{ "nf_send_reset6", 0 },
};

/* Internal cgroup symbols needed by dnsk_cgpids; resolved through kallsyms. */
static struct sym_req cgpids_required[] = {
	{ "cgroup_mutex", 0 },
	{ "cgrp_dfl_root", 0 },
	{ "cgroup_init_cftypes", 0 },
	{ "cgroup_addrm_files", 0 },
	{ "cgroup_mkdir", 0 },
	{ "cgroup_rmdir", 0 },
	{ "cgroup_destroy_locked", 0 },
	{ "cgroup_migrate_execute", 0 },
	{ "cgroup_subtree_control_write", 0 },
	{ "cgroup_controllers_show", 0 },
	{ "cgroup_subtree_control_show", 0 },
	{ "memory_cgrp_subsys", 0 },
	{ "have_canfork_callback", 0 },
	{ "have_release_callback", 0 },
	{ "cgroup_taskset_first", 0 },
	{ "cgroup_taskset_next", 0 },
	{ "of_css", 0 },
	{ "cgroup_kf_syscall_ops", 0 },
	{ "cgroup_base_files", 0 },
	{ "kernfs_activate", 0 },
};

/* tmpfs xattr internals needed by dnsk_tmpfsx; resolved through kallsyms. */
static struct sym_req tmpfsx_required[] = {
	{ "shmem_inode_operations", 0 },
	{ "shmem_dir_inode_operations", 0 },
	{ "shmem_fill_super", 0 },
	{ "shmem_evict_inode", 0 },
	{ "simple_xattr_set", 0 },
	{ "simple_xattr_get", 0 },
	{ "simple_xattr_list", 0 },
	{ "simple_xattrs_free", 0 },
	{ "iterate_supers", 0 },
	{ "shmem_fs_context_ops", 0 },
	{ "shmem_init_fs_context", 0 },
};

static int find_symbol_in(struct sym_req *tab, size_t n, const char *name)
{
	for (size_t i = 0; i < n; i++)
		if (!strcmp(tab[i].name, name))
			return (int)i;
	return -1;
}

static int scan_kallsyms(void)
{
	FILE *fp;
	char line[512];

	fp = fopen("/proc/kallsyms", "r");
	if (!fp)
		return -1;
	while (fgets(line, sizeof(line), fp)) {
		char type;
		char name[256];
		int idx;

		if (sscanf(line, "%*s %c %255s", &type, name) != 2)
			continue;
		idx = find_symbol_in(required, ARRAY_SIZE(required), name);
		if (idx >= 0)
			required[idx].found = 1;
		idx = find_symbol_in(nf_required, ARRAY_SIZE(nf_required), name);
		if (idx >= 0)
			nf_required[idx].found = 1;
		idx = find_symbol_in(cgpids_required, ARRAY_SIZE(cgpids_required),
				     name);
		if (idx >= 0)
			cgpids_required[idx].found = 1;
		idx = find_symbol_in(tmpfsx_required, ARRAY_SIZE(tmpfsx_required),
				     name);
		if (idx >= 0)
			tmpfsx_required[idx].found = 1;
	}
	fclose(fp);
	return 0;
}

static int all_found(const struct sym_req *tab, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!tab[i].found)
			return 0;
	return 1;
}

static int parse_kmi(const char *release, char *out, size_t out_len)
{
	const char *a = strstr(release, "android");
	char *end;
	int android;
	int major, minor;

	if (!a)
		return -1;
	android = (int)strtol(a + strlen("android"), &end, 10);
	if (end == a + strlen("android"))
		return -1;
	if (sscanf(release, "%d.%d", &major, &minor) != 2)
		return -1;
	if (snprintf(out, out_len, "android%d-%d.%d", android, major, minor) >=
	    (int)out_len)
		return -1;
	return 0;
}

static int supported_kmi(const char *path, const char *kmi)
{
	FILE *fp;
	char line[128];
	int found = 0;

	if (!path || !path[0])
		return 0;
	fp = fopen(path, "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strcmp(line, kmi)) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	return found;
}

static char *path_ksud(void)
{
	static char cand[PATH_MAX];
	const char *p = getenv("PATH");

	if (!p)
		return NULL;
	while (*p) {
		size_t len = strcspn(p, ":");

		if (len && len < sizeof(cand) - sizeof("/ksud")) {
			snprintf(cand, sizeof(cand), "%.*s/ksud", (int)len, p);
			if (access(cand, X_OK) == 0)
				return cand;
		}
		p += len;
		if (*p == ':')
			p++;
	}
	return NULL;
}

static char *find_ksud(void)
{
	static char path[PATH_MAX];
	const char *env = getenv("DNSK_KSUD");
	const char *candidates[] = {
		"/data/adb/ksud",
		"/data/adb/ksu/bin/ksud",
	};

	if (env && env[0] && access(env, X_OK) == 0) {
		snprintf(path, sizeof(path), "%s", env);
		return path;
	}
	for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
		if (access(candidates[i], X_OK) == 0) {
			snprintf(path, sizeof(path), "%s", candidates[i]);
			return path;
		}
	}
	return path_ksud();
}

int main(int argc, char **argv)
{
	struct utsname uts;
	char kmi[64] = { 0 };
	char derived[64] = { 0 };
	const char *supported_path = NULL;
	const char *feature = "core";
	const char *ksud;
	unsigned long page_size;
	const char *btf_path = "/sys/kernel/btf/vmlinux";
	long xt_expected = -1;
	long seq_expected = -1;
	struct btf_layout layout;
	int check_nf;
	int check_xt;
	int check_fs;
	int check_ns;
	int check_cgpids;
	int check_tmpfsx;
	int btf;
	int layout_ok;
	int boundary_ok;
	int nf_ok;
	int nf_symbols;
	int cgpids_symbols;
	int tmpfsx_symbols;
	int seq_ok;
	int supported;
	int dcache;
	int resolver;
	int ok;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--kmi") && i + 1 < argc)
			snprintf(kmi, sizeof(kmi), "%s", argv[++i]);
		else if (!strcmp(argv[i], "--supported") && i + 1 < argc)
			supported_path = argv[++i];
		else if (!strcmp(argv[i], "--btf") && i + 1 < argc)
			btf_path = argv[++i];
		else if (!strcmp(argv[i], "--feature") && i + 1 < argc)
			feature = argv[++i];
		else if (!strcmp(argv[i], "--xt-layout") && i + 1 < argc)
			xt_expected = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--seq-private") && i + 1 < argc)
			seq_expected = strtol(argv[++i], NULL, 10);
	}
	check_nf = !strcmp(feature, "netfilter");
	check_xt = check_nf || !strcmp(feature, "ipset");
	check_fs = !strcmp(feature, "devtmpfs");
	check_ns = !strcmp(feature, "ns");
	check_cgpids = !strcmp(feature, "cgpids");
	check_tmpfsx = !strcmp(feature, "tmpfsx");

	if (uname(&uts) != 0) {
		perror("uname");
		return 1;
	}
	if (!kmi[0] && parse_kmi(uts.release, derived, sizeof(derived)) == 0)
		snprintf(kmi, sizeof(kmi), "%s", derived);

	page_size = (unsigned long)sysconf(_SC_PAGESIZE);
	btf = access(btf_path, R_OK) == 0;
	memset(&layout, 0, sizeof(layout));
	if (btf)
		btf_read_layout(btf_path, &layout, check_xt, check_nf, check_fs,
				check_ns, check_cgpids);
	if (xt_expected < 0)
		layout_ok = 1;
	else if (xt_expected == 104 || xt_expected == 128)
		layout_ok = layout.found_match && layout.found_target &&
			    xt_layout_matches(&layout.match,
					      (uint32_t)xt_expected, 0) &&
			    xt_layout_matches(&layout.target,
					      (uint32_t)xt_expected, 1);
	else
		layout_ok = 0;
	ksud = find_ksud();
	supported = supported_kmi(supported_path, kmi);
	scan_kallsyms();

	dcache = required[SYM_DCACHE_CLEAN_INVAL_POC].found ||
		 required[SYM_FLUSH_DCACHE_AREA].found;
	resolver = required[SYM_KALLSYMS_LOOKUP_NAME].found ||
		   required[SYM_KALLSYMS_ON_EACH_SYMBOL].found;
	boundary_ok = layout.locks_ok &&
		      (!check_xt || (layout.xt_params_ok && layout.skb_ok &&
				     layout.net_ok)) &&
		      (!check_fs || layout.fs_ok) &&
		      (!check_ns || layout.ns_ok) &&
		      (!check_cgpids || layout.cgpids_ok) &&
		      (!check_tmpfsx || layout.tmpfsx_ok);
	nf_ok = !check_nf || layout.nf.ok;
	nf_symbols = all_found(nf_required, ARRAY_SIZE(nf_required));
	cgpids_symbols = all_found(cgpids_required,
				   ARRAY_SIZE(cgpids_required));
	tmpfsx_symbols = all_found(tmpfsx_required,
				   ARRAY_SIZE(tmpfsx_required));
	seq_ok = !check_cgpids ||
		 (seq_expected >= 0 && layout.seq_private == seq_expected);
	ok = page_size == 4096 && btf && layout_ok && boundary_ok && ksud &&
	     supported && kmi[0] && required[SYM_SYS_CALL_TABLE].found &&
	     required[SYM_INIT_MM].found &&
	     required[SYM_PANIC_NOTIFIER_LIST].found &&
	     required[SYM_STOP_MACHINE].found &&
	     required[SYM_REGISTER_KPROBE].found && dcache && resolver &&
	     nf_ok && (!check_nf || nf_symbols) && seq_ok &&
	     (!check_cgpids || cgpids_symbols) &&
	     (!check_tmpfsx || tmpfsx_symbols);

	printf("{\"ok\":%s,\"feature\":\"%s\",\"release\":\"%s\","
	       "\"kmi\":\"%s\",\"supported\":%s,\"page_size\":%lu,"
	       "\"btf\":%s,\"ksud\":\"%s\",\"btf_layout\":{"
	       "\"xt_match_size\":%u,\"xt_target_size\":%u,"
	       "\"seq_file_private\":%d,"
	       "\"xt_ok\":%s,\"boundary\":{\"locks\":%s,"
	       "\"checked_xt\":%s,\"xt_params\":%s,\"skb\":%s,\"net\":%s,"
	       "\"checked_fs\":%s,\"fs\":%s,"
	       "\"checked_ns\":%s,\"ns\":%s,"
	       "\"checked_cgpids\":%s,\"cgpids\":%s,"
	       "\"checked_tmpfsx\":%s,\"tmpfsx\":%s,\"ok\":%s},\"nf\":{\"checked\":%s,"
	       "\"hook_ops\":%s,\"hook_state\":%s,\"conn\":%s,"
	       "\"pernet_ops\":%s,\"nfnetlink\":%s,\"nfnl_callback\":%s,"
	       "\"nat_hook\":%s,\"nat_range2\":%s,"
	       "\"rhashtable\":%s,\"ok\":%s}},\"symbols\":{",
	       ok ? "true" : "false", feature, uts.release, kmi,
	       supported ? "true" : "false", page_size,
	       btf ? "true" : "false", ksud ? ksud : "",
	       layout.match.size, layout.target.size, layout.seq_private,
	       layout_ok ? "true" : "false",
	       layout.locks_ok ? "true" : "false",
	       check_xt ? "true" : "false",
	       layout.xt_params_ok ? "true" : "false",
	       layout.skb_ok ? "true" : "false",
	       layout.net_ok ? "true" : "false",
	       check_fs ? "true" : "false",
	       layout.fs_ok ? "true" : "false",
	       check_ns ? "true" : "false",
	       layout.ns_ok ? "true" : "false",
	       check_cgpids ? "true" : "false",
	       layout.cgpids_ok ? "true" : "false",
	       check_tmpfsx ? "true" : "false",
	       layout.tmpfsx_ok ? "true" : "false",
	       boundary_ok ? "true" : "false",
	       check_nf ? "true" : "false",
	       layout.nf.hook_ops ? "true" : "false",
	       layout.nf.hook_state ? "true" : "false",
	       layout.nf.conn ? "true" : "false",
	       layout.nf.pernet_ops ? "true" : "false",
	       layout.nf.nfnetlink ? "true" : "false",
	       layout.nf.nfnl_callback ? "true" : "false",
	       layout.nf.nat_hook ? "true" : "false",
	       layout.nf.nat_range2 ? "true" : "false",
	       layout.nf.rhashtable ? "true" : "false",
	       layout.nf.ok ? "true" : "false");
	for (size_t i = 0; i < ARRAY_SIZE(required); i++) {
		printf("%s\"%s\":%s", i ? "," : "", required[i].name,
		       required[i].found ? "true" : "false");
	}
	for (size_t i = 0; i < ARRAY_SIZE(nf_required); i++) {
		printf(",\"%s\":%s", nf_required[i].name,
		       nf_required[i].found ? "true" : "false");
	}
	if (check_cgpids) {
		for (size_t i = 0; i < ARRAY_SIZE(cgpids_required); i++) {
			printf(",\"%s\":%s", cgpids_required[i].name,
			       cgpids_required[i].found ? "true" : "false");
		}
	}
	if (check_tmpfsx) {
		for (size_t i = 0; i < ARRAY_SIZE(tmpfsx_required); i++) {
			printf(",\"%s\":%s", tmpfsx_required[i].name,
			       tmpfsx_required[i].found ? "true" : "false");
		}
	}
	printf("},\"checks\":{\"page_size_4k\":%s,\"btf\":%s,"
	       "\"kmi_supported\":%s,\"ksud\":%s,\"dcache\":%s,"
	       "\"kallsyms_resolver\":%s,\"xt_layout\":%s,"
	       "\"boundary\":%s,\"nf_abi\":%s,\"nf_symbols\":%s,"
	       "\"cgpids_symbols\":%s,\"tmpfsx_symbols\":%s,"
	       "\"seq_private\":%s}}\n",
	       page_size == 4096 ? "true" : "false", btf ? "true" : "false",
	       supported ? "true" : "false", ksud ? "true" : "false",
	       dcache ? "true" : "false", resolver ? "true" : "false",
	       layout_ok ? "true" : "false", boundary_ok ? "true" : "false",
	       nf_ok ? "true" : "false",
	       (!check_nf || nf_symbols) ? "true" : "false",
	       (!check_cgpids || cgpids_symbols) ? "true" : "false",
	       (!check_tmpfsx || tmpfsx_symbols) ? "true" : "false",
	       seq_ok ? "true" : "false");
	return ok ? 0 : 1;
}
