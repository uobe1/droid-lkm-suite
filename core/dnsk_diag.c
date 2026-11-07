// SPDX-License-Identifier: GPL-2.0-only
/* /proc/dnsk/status: read-only truth source for dnskctl and WebUI. */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>
#include "dnsk_internal.h"

static struct proc_dir_entry *dnsk_proc_dir;
static struct proc_dir_entry *dnsk_proc_status;

static int dnsk_status_show(struct seq_file *m, void *unused)
{
	seq_printf(m, "dnsk_core abi=%u\n", DNSK_CORE_ABI);
	seq_printf(m, "kmi=%s\n", utsname()->release);
	seq_printf(m, "patch=%s\n", dnsk_patch_ready() ? "ready" : "disabled");
	seq_printf(m, "panic=%s\n", dnsk_panic_ready() ? "ready" : "disabled");
	seq_printf(m, "hooks=%u\n", dnsk_hook_count());
	seq_printf(m, "features=%u\n", dnsk_feature_count());
	dnsk_features_show(m);
	return 0;
}

struct proc_dir_entry *dnsk_proc_dir_entry(void)
{
	return dnsk_proc_dir;
}

int dnsk_diag_init(void)
{
	dnsk_proc_dir = proc_mkdir("dnsk", NULL);
	if (!dnsk_proc_dir)
		return -ENOMEM;

	dnsk_proc_status = proc_create_single_data("status", 0400, dnsk_proc_dir,
						   dnsk_status_show, NULL);
	if (!dnsk_proc_status) {
		proc_remove(dnsk_proc_dir);
		dnsk_proc_dir = NULL;
		return -ENOMEM;
	}
	return 0;
}

void dnsk_diag_exit(void)
{
	if (dnsk_proc_status) {
		proc_remove(dnsk_proc_status);
		dnsk_proc_status = NULL;
	}
	if (dnsk_proc_dir) {
		proc_remove(dnsk_proc_dir);
		dnsk_proc_dir = NULL;
	}
}
