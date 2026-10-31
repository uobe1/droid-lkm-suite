// SPDX-License-Identifier: GPL-2.0-only
/*
 * Feature registry.  A feature is visible only while its module holds a
 * registration; unloading the owner removes it and releases the module ref.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include "dnsk_internal.h"

static struct dnsk_feature dnsk_features[DNSK_MAX_FEATURES];
static DEFINE_MUTEX(dnsk_features_lock);

const char *dnsk_status_name(enum dnsk_status status)
{
	switch (status) {
	case DNSK_STATUS_NOT_SHIPPED:
		return "not-shipped";
	case DNSK_STATUS_FEASIBILITY:
		return "feasibility";
	case DNSK_STATUS_DEVELOPMENT:
		return "development";
	case DNSK_STATUS_NATIVE:
		return "native";
	case DNSK_STATUS_REAL:
		return "real";
	case DNSK_STATUS_DIFFERENTIAL:
		return "differential";
	case DNSK_STATUS_VIRTUAL:
		return "virtual";
	case DNSK_STATUS_NA:
		return "n/a";
	case DNSK_STATUS_UNSUPPORTED:
		return "unsupported";
	default:
		return "unknown";
	}
}

int dnsk_features_init(void)
{
	memset(dnsk_features, 0, sizeof(dnsk_features));
	return 0;
}

static struct dnsk_feature *dnsk_feature_find_locked(const char *name)
{
	int i;

	for (i = 0; i < DNSK_MAX_FEATURES; i++) {
		if (dnsk_features[i].active &&
		    !strcmp(dnsk_features[i].name, name))
			return &dnsk_features[i];
	}
	return NULL;
}

int dnsk_feature_add_internal(const struct dnsk_feature_desc *desc)
{
	struct dnsk_feature *feature = NULL;
	int i;

	if (!desc || !desc->name || !desc->name[0] || !desc->owner)
		return -EINVAL;
	if (strlen(desc->name) >= DNSK_FEATURE_NAME_LEN)
		return -ENAMETOOLONG;

	mutex_lock(&dnsk_features_lock);
	if (dnsk_feature_find_locked(desc->name)) {
		mutex_unlock(&dnsk_features_lock);
		return -EEXIST;
	}

	for (i = 0; i < DNSK_MAX_FEATURES; i++) {
		if (!dnsk_features[i].active) {
			feature = &dnsk_features[i];
			break;
		}
	}
	if (!feature) {
		mutex_unlock(&dnsk_features_lock);
		return -ENOSPC;
	}
	if (!try_module_get(desc->owner)) {
		mutex_unlock(&dnsk_features_lock);
		return -ENOENT;
	}

	memset(feature, 0, sizeof(*feature));
	strscpy(feature->name, desc->name, sizeof(feature->name));
	if (desc->version)
		strscpy(feature->version, desc->version, sizeof(feature->version));
	if (desc->caps)
		strscpy(feature->caps, desc->caps, sizeof(feature->caps));
	feature->status = desc->status;
	feature->owner = desc->owner;
	feature->active = true;
	mutex_unlock(&dnsk_features_lock);
	return 0;
}

int dnsk_feature_del_internal(const char *name)
{
	struct dnsk_feature *feature;

	if (!name)
		return -EINVAL;

	mutex_lock(&dnsk_features_lock);
	feature = dnsk_feature_find_locked(name);
	if (!feature) {
		mutex_unlock(&dnsk_features_lock);
		return -ENOENT;
	}
	feature->active = false;
	if (feature->owner) {
		module_put(feature->owner);
		feature->owner = NULL;
	}
	memset(feature, 0, sizeof(*feature));
	mutex_unlock(&dnsk_features_lock);
	return 0;
}

unsigned int dnsk_feature_count(void)
{
	unsigned int n = 0;
	int i;

	mutex_lock(&dnsk_features_lock);
	for (i = 0; i < DNSK_MAX_FEATURES; i++)
		if (dnsk_features[i].active)
			n++;
	mutex_unlock(&dnsk_features_lock);
	return n;
}

void dnsk_features_show(struct seq_file *m)
{
	int i;

	mutex_lock(&dnsk_features_lock);
	for (i = 0; i < DNSK_MAX_FEATURES; i++) {
		struct dnsk_feature *f = &dnsk_features[i];

		if (!f->active)
			continue;
		seq_printf(m, "feature %s %s version=%s caps=%s\n",
			   f->name, dnsk_status_name(f->status),
			   f->version[0] ? f->version : "-",
			   f->caps[0] ? f->caps : "-");
	}
	mutex_unlock(&dnsk_features_lock);
}

void dnsk_features_exit(void)
{
	int i;

	mutex_lock(&dnsk_features_lock);
	for (i = 0; i < DNSK_MAX_FEATURES; i++) {
		if (!dnsk_features[i].active)
			continue;
		dnsk_features[i].active = false;
		if (dnsk_features[i].owner) {
			module_put(dnsk_features[i].owner);
			dnsk_features[i].owner = NULL;
		}
	}
	mutex_unlock(&dnsk_features_lock);
}
