#include "fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Runs argv without a shell, output discarded. Returns the exit status. */
static int run(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(tmp, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static int cache_root(char *out, size_t n) {
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/caltrain-commander", xdg);
    else if (home && *home) snprintf(out, n, "%s/.cache/caltrain-commander", home);
    else snprintf(out, n, "/tmp/caltrain-commander");
    return mkdir_p(out);
}

static int extract(const char *zip, const char *dir) {
    if (mkdir_p(dir) != 0) return -1;
    char *argv[] = {"unzip", "-o", "-q", (char *)zip, "-d", (char *)dir, NULL};
    return run(argv);
}

static int download(const char *url, const char *zip) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.part", zip);
    char *argv[] = {"curl", "-fsSL", "--connect-timeout", "10", "--max-time", "120",
                    "-o", tmp, "--url", (char *)url, NULL};
    int rc = run(argv);
    if (rc == 0 && rename(tmp, zip) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}

static long age_days(const char *path) {
    struct stat sb;
    if (stat(path, &sb) != 0) return -1;
    time_t now = time(NULL);
    return now > sb.st_mtime ? (long)((now - sb.st_mtime) / 86400) : 0;
}

static int has_feed(const char *dir) {
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/stop_times.txt", dir);
    return access(p, R_OK) == 0;
}

int fetch_prepare(const char *path, const char *url, int refresh,
                  char *dir, size_t dirn, char *msg, size_t msgn) {
    char root[PATH_MAX], zip[PATH_MAX];
    struct stat sb;

    if (cache_root(root, sizeof root) != 0) {
        snprintf(msg, msgn, "Cannot create cache directory %s: %s", root, strerror(errno));
        return -1;
    }

    if (path) {
        if (stat(path, &sb) != 0) {
            snprintf(msg, msgn, "Cannot open %s: %s", path, strerror(errno));
            return -1;
        }
        if (S_ISDIR(sb.st_mode)) {
            snprintf(dir, dirn, "%s", path);
            snprintf(msg, msgn, "Schedule from %s", path);
            return 0;
        }
        char abs[PATH_MAX];
        if (!realpath(path, abs)) snprintf(abs, sizeof abs, "%s", path);
        snprintf(dir, dirn, "%s/local", root);
        if (extract(abs, dir) != 0) {
            snprintf(msg, msgn, "Cannot extract %s (is unzip installed?)", path);
            return -1;
        }
        snprintf(msg, msgn, "Schedule from %s", path);
        return 0;
    }

    snprintf(zip, sizeof zip, "%s/gtfs.zip", root);
    snprintf(dir, dirn, "%s/feed", root);
    long age = age_days(zip);
    if (!refresh && age >= 0 && age < CACHE_MAX_AGE_DAYS && has_feed(dir)) {
        snprintf(msg, msgn, "Using cached schedule (%ld day%s old)", age, age == 1 ? "" : "s");
        return 0;
    }

    int rc = download(url ? url : CALTRAIN_GTFS_URL, zip);
    if (rc == 0 && extract(zip, dir) == 0) {
        snprintf(msg, msgn, "Downloaded fresh schedule");
        return 0;
    }
    if (age >= 0 && has_feed(dir)) {
        snprintf(msg, msgn, "Download failed (%s %d); using cached schedule from %ld days ago",
                 rc == 0 ? "unzip" : "curl", rc, age);
        return 0;
    }
    snprintf(msg, msgn, "Could not download the schedule (%s exit %d). "
                        "Check your connection or pass -f <gtfs.zip|dir>.",
             rc == 0 ? "unzip" : "curl", rc);
    return -1;
}
