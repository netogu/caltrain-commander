#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

/* Saved choices for the "Next trains" tab, as GTFS station ids ("" = unset). */
typedef struct {
    char origin[64], dest[64];
} Config;

/* Read $XDG_CONFIG_HOME/caltrain-commander/config (default ~/.config/...).
 * Missing or unreadable file yields empty ids. */
void config_load(Config *c);

/* Write the file, creating the directory. Returns 0 on success. */
int config_save(const Config *c);

#endif
