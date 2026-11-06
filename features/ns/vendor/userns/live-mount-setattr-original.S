/* SPDX-License-Identifier: GPL-2.0-only */
/* Checked, position-independent native prologue copied by the hook engine. */
.text

.balign 16
.global ds_userns_do_mount_setattr_original
.type ds_userns_do_mount_setattr_original, %function
ds_userns_do_mount_setattr_original:
.rept 8
brk #0
.endr
.size ds_userns_do_mount_setattr_original, .-ds_userns_do_mount_setattr_original
