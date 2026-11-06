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
.global ds_userns_dup_mmap_original
.type ds_userns_dup_mmap_original, %function
ds_userns_dup_mmap_original:
.rept 8
brk #0
.endr
.size ds_userns_dup_mmap_original, .-ds_userns_dup_mmap_original
