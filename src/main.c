#include "fetch.h"
#include "gtfs.h"
#include "ui.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *path, *url;
    int refresh;
} Options;

static void usage(FILE *out) {
    fputs("usage: caltrain [options]\n"
          "  -f PATH      read GTFS from a .zip file or an extracted directory\n"
          "  -u URL       download the feed from URL instead of Caltrain's default\n"
          "  --refresh    re-download even if the cache is fresh\n"
          "  --summary    print what was loaded and exit (no UI)\n"
          "  -h, --help   show this help\n",
          out);
}

static int load(const Options *o, Gtfs **out, char *msg, size_t n) {
    char dir[PATH_MAX], err[256];
    if (fetch_prepare(o->path, o->url, o->refresh, dir, sizeof dir, msg, n) != 0) return -1;
    Gtfs *g = gtfs_load(dir, err, sizeof err);
    if (!g) {
        snprintf(msg, n, "%s", err);
        return -1;
    }
    *out = g;
    return 0;
}

static int reload_cb(void *ctx, Gtfs **out, char *msg, size_t n) {
    Options o = *(Options *)ctx;
    if (!o.path) o.refresh = 1; /* `r` always asks the server for a fresh copy */
    return load(&o, out, msg, n);
}

int main(int argc, char **argv) {
    Options o = {NULL, NULL, 0};
    int summary = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) o.path = argv[++i];
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) o.url = argv[++i];
        else if (!strcmp(argv[i], "--refresh")) o.refresh = 1;
        else if (!strcmp(argv[i], "--summary")) summary = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    /* Schedules are in agency-local time; so should "now" be. */
    setenv("TZ", "America/Los_Angeles", 0);
    tzset();

    char msg[512];
    Gtfs *g = NULL;
    fputs("Loading Caltrain schedule...\n", stderr);
    if (load(&o, &g, msg, sizeof msg) != 0) {
        fprintf(stderr, "caltrain: %s\n", msg);
        return 1;
    }

    if (summary) {
        printf("%s\n%d stations, %d trips, %d stop times, %d services\n", msg,
               g->nstations, g->ntrips, g->ntimes, g->nservices);
        for (int i = 0; i < g->nstations; i++)
            printf("  %-28s %4d stop times\n", g->stations[i].name, g->stations[i].ntimes);
        gtfs_free(g);
        return 0;
    }

    int rc = ui_run(&g, msg, reload_cb, &o);
    gtfs_free(g);
    return rc;
}
