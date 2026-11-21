/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef DS_DEVTMPFS_PORT_H
#define DS_DEVTMPFS_PORT_H
#include <linux/device.h>
#include <linux/types.h>
int ds_devtmpfs_start(void);
void ds_devtmpfs_stop(void);
int ds_devtmpfs_create_node(struct device *dev);
int ds_devtmpfs_delete_node(struct device *dev);
int ds_device_add(struct device *dev);
void ds_device_del(struct device *dev);
int ds_hooks_prepare(bool bus_trigger);
int ds_hooks_install(void);
int ds_hooks_uninstall(void);
extern bool ds_hooks_active;
void ds_devtmpfs_update_node(struct device *dev, bool create);
int ds_device_add_original(struct device *dev);
void ds_device_del_original(struct device *dev);
void ds_bus_notify(struct device *dev, unsigned int value);
void ds_bus_notify_original(struct device *dev, unsigned int value);
#endif
