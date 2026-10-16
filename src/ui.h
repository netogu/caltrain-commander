#ifndef UI_H
#define UI_H

#include "gtfs.h"

/* Called on `r`: load a fresh schedule into *out. Returns 0 on success;
 * `msg` receives a status note either way. */
typedef int (*UiReloadFn)(void *ctx, Gtfs **out, char *msg, size_t n);

/* Runs the interactive UI until the user quits. *g may be replaced by a
 * reload (the old one is freed); the caller frees the final *g. */
int ui_run(Gtfs **g, const char *note, UiReloadFn reload, void *ctx);

#endif
