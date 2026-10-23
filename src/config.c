#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int config_dir(char *out, size_t n) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s", xdg);
    else if (home && *home) snprintf(out, n, "%s/.config", home);
    else return -1;
    return 0;
}

static int config_path(char *out, size_t n) {
    char dir[PATH_MAX - 64];
    if (config_dir(dir, sizeof dir) != 0) return -1;
    snprintf(out, n, "%s/caltrain-commander/config", dir);
    return 0;
}

void config_load(Config *c) {
    char path[PATH_MAX], line[256];
    memset(c, 0, sizeof *c);
    if (config_path(path, sizeof path) != 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "origin=", 7)) snprintf(c->origin, sizeof c->origin, "%s", line + 7);
        else if (!strncmp(line, "dest=", 5)) snprintf(c->dest, sizeof c->dest, "%s", line + 5);
    }
    fclose(f);
}

int config_save(const Config *c) {
    char dir[PATH_MAX - 64], sub[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX + 8];
    if (config_dir(dir, sizeof dir) != 0 || config_path(path, sizeof path) != 0) return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(sub, sizeof sub, "%s/caltrain-commander", dir);
    if (mkdir(sub, 0755) != 0 && errno != EEXIST) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "origin=%s\ndest=%s\n", c->origin, c->dest);
    if (fclose(f) != 0) {
        remove(tmp);
        return -1;
    }
    return rename(tmp, path);
}
