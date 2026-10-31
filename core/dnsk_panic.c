// SPDX-License-Identifier: GPL-2.0-only
/*
 * Panic breadcrumb.  ramoops/pstore captures printk; this notifier only adds
 * a structured marker.  It never writes block devices or filesystems.
 */

#include <linux/notifier.h>
#include <linux/string.h>
#include "dnsk_internal.h"

static char dnsk_last_op[DNSK_PANIC_OP_LEN];
static struct notifier_block dnsk_panic_nb;
static struct atomic_notifier_head *dnsk_panic_head;
static bool dnsk_panic_active;

bool dnsk_panic_ready(void)
{
	return dnsk_panic_active;
}

void dnsk_panic_mark(const char *op)
{
	if (!op)
		return;
	strscpy(dnsk_last_op, op, sizeof(dnsk_last_op));
}

static int dnsk_panic_notify(struct notifier_block *nb, unsigned long event,
			     void *data)
{
	pr_emerg("DNSK_PANIC event=%lu op=%s features=%u hooks=%u\n",
		 event, dnsk_last_op[0] ? dnsk_last_op : "none",
		 dnsk_feature_count(), dnsk_hook_count());
	return NOTIFY_DONE;
}

int dnsk_panic_init(void)
{
	void *head = dnsk_resolve_symbol("panic_notifier_list");

	dnsk_panic_head = head;
	if (!dnsk_panic_head) {
		dnsk_warn("panic_notifier_list not found; panic markers disabled\n");
		return -ENOENT;
	}

	dnsk_panic_nb.notifier_call = dnsk_panic_notify;
	if (atomic_notifier_chain_register(dnsk_panic_head, &dnsk_panic_nb)) {
		dnsk_warn("failed to register panic notifier\n");
		dnsk_panic_head = NULL;
		return -EINVAL;
	}
	dnsk_panic_active = true;
	return 0;
}

void dnsk_panic_exit(void)
{
	if (dnsk_panic_active && dnsk_panic_head)
		atomic_notifier_chain_unregister(dnsk_panic_head, &dnsk_panic_nb);
	dnsk_panic_active = false;
	dnsk_panic_head = NULL;
}
