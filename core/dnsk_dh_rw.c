/* SPDX-License-Identifier: GPL-2.0-only */
/* By-path embedding of droid-hook into dnsk_core.ko, no copied source.
 *
 * One wrapper per droid-hook source keeps each source's file-local statics
 * TU-local.  The Kbuild defines DNSK_DH_RW_SRC as the absolute/relative path
 * to the droid-hook checkout selected by DNSK_HOOK_DIR. */
#include DNSK_DH_RW_SRC
