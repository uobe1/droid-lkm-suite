/* SPDX-License-Identifier: GPL-2.0-only */
/* Only a checked, position-independent native prologue is copied here. */
.text

.balign 16
.global ds_userns_mm_alloc_original
.type ds_userns_mm_alloc_original, %function
ds_userns_mm_alloc_original:
.rept 8
brk #0
.endr
.size ds_userns_mm_alloc_original, .-ds_userns_mm_alloc_original

.balign 16
.global ds_userns_wake_up_new_task_original
.type ds_userns_wake_up_new_task_original, %function
ds_userns_wake_up_new_task_original:
.rept 8
brk #0
.endr
.size ds_userns_wake_up_new_task_original, .-ds_userns_wake_up_new_task_original
