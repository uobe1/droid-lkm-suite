/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DROID_HOOK_RUNTIME_DH_CORE_BACKEND_H
#define _DROID_HOOK_RUNTIME_DH_CORE_BACKEND_H

/*
 * Runtime adapter: maps the core RW ABI 5 single entry (rw + patch_pointer +
 * resolve) onto struct dh_backend used by the Droid-Hook library.
 *
 * This header is owned by runtime/.  It is the only place that mentions core
 * types outside dh_core_types.h; include it from an embedding LKM, never from
 * the library layer.
 */

#include "dh.h"
#include "dh_core_types.h"

/**
 * struct dh_core_state - per-backend state owned by the caller
 *
 * No global state: dh_core_backend_init() binds @be to @st, and every backend
 * callback recovers @st from @be.  The caller must keep @st and @api alive
 * for as long as @be is in use, and must not reuse @st for a second backend.
 */
struct dh_core_state {
	struct dnsk_core_api *api;
	struct dh_backend *be;
	u16 page_size;
	u8 bound;
};

/**
 * dh_core_backend_init() - bind a dh_backend to a core ABI 5 API table
 * @be: backend to fill, zeroed by the caller before the call
 * @st: caller-owned state, must outlive @be
 * @api: core API table, validated for magic/ABI/mandatory primitives
 *
 * Validates @api (non-NULL, DNSK_CORE_MAGIC, DNSK_CORE_ABI, non-NULL rw),
 * then fills every mandatory dh_backend primitive.  On success @be->abi is
 * DH_ABI and @be->name is the stable string "dnsk_core_rw5".
 *
 * Return: %DH_OK on success, %DH_E_INVAL on a bad or incomplete @api.
 */
int dh_core_backend_init(struct dh_backend *be, struct dh_core_state *st,
			 struct dnsk_core_api *api);

/**
 * dh_core_backend_fini() - unbind a backend previously passed to init
 * @be: backend to clear
 *
 * Clears the function pointers and the bound flag.  Does not release @api or
 * @st; those belong to the caller.
 *
 * Return: %DH_OK on success, %DH_E_INVAL when @be was never bound.
 */
int dh_core_backend_fini(struct dh_backend *be);

/**
 * dh_core_backend_page_size() - page size used for transfer splitting
 * @st: state previously bound by dh_core_backend_init()
 *
 * The default is DNSK_RW_MAX (256) so a single transfer never exceeds the
 * core's per-request cap.  A caller may raise it up to one page only when the
 * target is known to be page-aligned; splitting still caps each chunk at 256.
 *
 * Return: the configured page size.
 */
size_t dh_core_backend_page_size(struct dh_core_state *st);

/**
 * dh_core_backend_set_page_size() - override the split page size
 * @st: bound state
 * @page_size: new page size, must be 0 (meaning default) or >= DNSK_RW_MAX
 *
 * Return: %DH_OK, or %DH_E_INVAL when @page_size is invalid or @st unbound.
 */
int dh_core_backend_set_page_size(struct dh_core_state *st, size_t page_size);

#endif /* _DROID_HOOK_RUNTIME_DH_CORE_BACKEND_H */
