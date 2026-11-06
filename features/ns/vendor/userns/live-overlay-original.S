/* SPDX-License-Identifier: GPL-2.0-only */
/* Checked, position-independent native prologue copied by the hook engine. */
.text

.balign 16
.global ds_userns_ovl_fill_super_original
.type ds_userns_ovl_fill_super_original, %function
ds_userns_ovl_fill_super_original:
.rept 8
brk #0
.endr
.size ds_userns_ovl_fill_super_original, .-ds_userns_ovl_fill_super_original
