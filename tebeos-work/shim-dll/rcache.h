/* rcache.h -- read cache for the vendor's element-list scans (see rcache.c). */

#ifndef SHIM_RCACHE_H
#define SHIM_RCACHE_H

#include "page.h"

/* Something may have changed the page: forget everything cached. */
void rc_bump(void);

/* Calls made between enter/leave are pure reads and keep the cache (page_call bumps it for
 * every other call). Nests. */
void rc_pure_enter(void);
void rc_pure_leave(void);
int rc_pure(void);

/* An element's id / tagName / className as last fetched with its list (1), or 0 if unknown.
 * *out is malloc'd, or NULL for a null value. */
int rc_elem_prop(Page *pg, long h, const char *prop, char **out);

/* On a miss: fetch the labels of h and the elements stashed after it (one request). */
int rc_elem_fetch(Page *pg, long h);

/* A list's length as fetched with its elements: 1 if known. */
int rc_col_length(Page *pg, long col, long *len);

/* The handle of list element `idx` (-1: none), fetching it and the elements after it in one
 * request on a miss. 1 if answered, 0 if the request failed (fall back to a plain fetch). */
int rc_col_item(Page *pg, long col, long idx, long *h);

#endif
