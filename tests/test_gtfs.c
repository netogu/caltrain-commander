#include "csv.h"
#include "gtfs.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_dates(void) {
    CHECK(date_dow(20261014) == 2); /* Wednesday */
    CHECK(date_dow(20261010) == 5); /* Saturday */
    CHECK(date_add(20261231, 1) == 20270101);
    CHECK(date_add(20260301, -1) == 20260228);
}

static void test_csv(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/stops.txt", dir);
    CsvReader *r = csv_open(path);
    CHECK(r != NULL);
    if (!r) return;
    int name = csv_col(r, "stop_name");
    CHECK(name == 1);
    CHECK(csv_col(r, "nope") == -1);
    CHECK(csv_next(r));
    CHECK(strcmp(csv_get(r, name), "Alpha, North Station") == 0); /* quoted comma */
    CHECK(strcmp(csv_get(r, 99), "") == 0);
    csv_close(r);

    snprintf(path, sizeof path, "%s/routes.txt", dir); /* CRLF line endings */
    r = csv_open(path);
    CHECK(r != NULL);
    if (r) {
        CHECK(csv_next(r));
        CHECK(strcmp(csv_get(r, csv_col(r, "route_short_name")), "Local") == 0);
        CHECK(strcmp(csv_get(r, csv_col(r, "route_long_name")), "") == 0);
        CHECK(!csv_next(r));
        csv_close(r);
    }
}

static void test_model(const Gtfs *g) {
    CHECK(g->nstations == 3);
    CHECK(strcmp(g->stations[0].name, "Alpha, North") == 0); /* sorted north to south */
    CHECK(strcmp(g->stations[1].name, "Beta") == 0);         /* suffix stripped */
    CHECK(strcmp(g->stations[2].name, "Gamma") == 0);
    CHECK(g->ntrips == 4);
    CHECK(g->ntimes == 11);
    CHECK(g->trips[0].count == 3);
    CHECK(g->min_date == 20260101 && g->max_date == 20261231);
}

static int service_of(const Gtfs *g, const char *id) {
    for (int i = 0; i < g->nservices; i++)
        if (strcmp(g->services[i].id, id) == 0) return i;
    return -1;
}

static void test_service(const Gtfs *g) {
    int wd = service_of(g, "WD"), sp = service_of(g, "SPECIAL");
    CHECK(wd >= 0 && sp >= 0);
    CHECK(gtfs_service_active(g, wd, 20261014));  /* Wednesday */
    CHECK(!gtfs_service_active(g, wd, 20261017)); /* Saturday */
    CHECK(!gtfs_service_active(g, wd, 20261012)); /* removed by calendar_dates */
    CHECK(!gtfs_service_active(g, wd, 20270105)); /* outside range */
    CHECK(gtfs_service_active(g, sp, 20261010));  /* added, no calendar row */
    CHECK(!gtfs_service_active(g, sp, 20261011));
}

static void test_departures(const Gtfs *g) {
    Departure d[16];
    /* Alpha on a Wednesday: T1 and T3 depart; T2 terminates here. */
    int n = gtfs_departures(g, 0, 20261014, 0, DIR_ANY, d, 16);
    CHECK(n == 2);
    CHECK(d[0].time == 8 * 3600 && strcmp(g->trips[d[0].trip].number, "101") == 0);
    CHECK(d[1].time == 23 * 3600 + 50 * 60);

    /* from_time and direction filters. */
    CHECK(gtfs_departures(g, 0, 20261014, 9 * 3600, DIR_ANY, d, 16) == 1);
    CHECK(gtfs_departures(g, 0, 20261014, 0, 0, d, 16) == 0);

    /* Beta on Thursday: Wednesday's late T3 carries over past midnight. */
    n = gtfs_departures(g, 1, 20261015, 0, DIR_ANY, d, 16);
    CHECK(n == 4);
    CHECK(d[0].shift == 86400 && d[0].time == 20 * 60);
    CHECK(d[1].time == 8 * 3600 + 30 * 60 && d[2].time == 8 * 3600 + 40 * 60);
    CHECK(d[3].shift == 0 && d[3].time == 24 * 3600 + 20 * 60);

    /* Removed-service day: nothing runs. */
    CHECK(gtfs_departures(g, 0, 20261012, 0, DIR_ANY, d, 16) == 0);
    /* Added-only service on a Saturday. */
    CHECK(gtfs_departures(g, 0, 20261010, 0, DIR_ANY, d, 16) == 1);
}

static void test_journeys(const Gtfs *g) {
    Journey j[16];
    int n = gtfs_journeys(g, 0, 2, 20261014, 0, j, 16);
    CHECK(n == 2);
    CHECK(j[0].dep.time == 8 * 3600 && j[0].arr_time == 9 * 3600);
    CHECK(j[1].arr_time == 24 * 3600 + 50 * 60);
    CHECK(gtfs_journeys(g, 2, 0, 20261014, 0, j, 16) == 1); /* northbound T2 */
    CHECK(gtfs_journeys(g, 0, 0, 20261014, 0, j, 16) == 0);
    /* Beta -> Alpha: only the northbound train goes that way. */
    CHECK(gtfs_journeys(g, 1, 0, 20261014, 0, j, 16) == 1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
        return 2;
    }
    test_dates();
    test_csv(argv[1]);

    char err[256];
    Gtfs *g = gtfs_load(argv[1], err, sizeof err);
    if (!g) {
        fprintf(stderr, "load failed: %s\n", err);
        return 1;
    }
    test_model(g);
    test_service(g);
    test_departures(g);
    test_journeys(g);
    gtfs_free(g);

    CHECK(gtfs_load("/nonexistent", err, sizeof err) == NULL);
    CHECK(err[0] != 0);

    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    else puts("all tests passed");
    return failures != 0;
}
