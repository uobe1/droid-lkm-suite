/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Host-side compile probe for the Droid-Hook library contract.
 *
 * dh.h promises to compile in both __KERNEL__ and host contexts.  This file
 * includes it with no kernel headers available and instantiates every public
 * struct, so a host `cc -c` failure is a real contract violation rather than
 * a documentation claim.  Offsets are printed for the Python selftest to
 * assert; they are not hard-coded ABIs yet, only stability checks.
 */

#include "dh.h"

#include <stddef.h>
#include <stdio.h>

static struct dh_ctx ctx;
static struct dh_backend be;
static struct dh_txn txn;
static struct dh_undo_ent undo;
static struct dh_rw_req req;
static struct dh_hook hook;
static struct dh_shadow_item *shadow;

int main(void)
{
	printf("DH_ABI=%d DH_KEY_LEN=%d DH_UNDO_SLOTS=%d DH_MAX_BATCH=%d\n",
	       DH_ABI, DH_KEY_LEN, DH_UNDO_SLOTS, DH_MAX_BATCH);
	printf("sizeof(ctx)=%zu sizeof(be)=%zu sizeof(txn)=%zu\n",
	       sizeof(ctx), sizeof(be), sizeof(txn));
	printf("sizeof(undo)=%zu sizeof(req)=%zu sizeof(hook)=%zu\n",
	       sizeof(undo), sizeof(req), sizeof(hook));
	printf("offsetof(dh_txn,n)=%zu offsetof(dh_rw_req,ret)=%zu\n",
	       offsetof(struct dh_txn, n), offsetof(struct dh_rw_req, ret));
	printf("shadow_data_offset=%zu\n", offsetof(struct dh_shadow_item, data));
	printf("route_alias=%d route_dma=%d ret_verify=%d\n",
	       DH_ROUTE_ALIAS, DH_ROUTE_DMA, DH_E_VERIFY);
	(void)shadow;
	return 0;
}
