/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_DEVTMPFS_TRIGGER_PLAN_H
#define DS_DEVTMPFS_TRIGGER_PLAN_H

/*
 * Pure ordering decision shared by ds_bus_notify() and the host test.
 *
 * The kernel values BUS_NOTIFY_ADD_DEVICE / BUS_NOTIFY_DEL_DEVICE are mapped
 * to booleans at the call site, so this header stays kernel-free and can be
 * compiled by tests/host without kernel headers.
 */
/* DNSK_TRIGGER_PURE_BEGIN */
#define DS_TRIGGER_NONE		0u
#define DS_TRIGGER_CREATE_BEFORE	1u
#define DS_TRIGGER_DELETE_AFTER	2u

static inline unsigned int ds_trigger_plan(int is_add, int is_del)
{
	unsigned int plan = DS_TRIGGER_NONE;

	if (is_add)
		plan |= DS_TRIGGER_CREATE_BEFORE;
	if (is_del)
		plan |= DS_TRIGGER_DELETE_AFTER;
	return plan;
}
/* DNSK_TRIGGER_PURE_END */

#endif /* DS_DEVTMPFS_TRIGGER_PLAN_H */
