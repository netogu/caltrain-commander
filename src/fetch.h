#ifndef FETCH_H
#define FETCH_H

#include <stddef.h>

#define CALTRAIN_GTFS_URL "https://data.trilliumtransit.com/gtfs/caltrain-ca-us/caltrain-ca-us.zip"
#define CACHE_MAX_AGE_DAYS 7

/* Resolves a directory of extracted GTFS tables into `dir`.
 *   path   : local GTFS .zip or directory, or NULL to use the download cache
 *   url    : feed to download when the cache is missing/stale (NULL = default)
 *   refresh: ignore cache freshness and download again
 * `msg` receives a one-line note on success (source / cache age / fallback
 * warning) or the reason for failure. Returns 0 on success. */
int fetch_prepare(const char *path, const char *url, int refresh,
                  char *dir, size_t dirn, char *msg, size_t msgn);

#endif
