#include "gtfs.h"
#include "csv.h"
#include "util.h"

#include <stdint.h>
#include <time.h>

/* ---- string -> index hash map (keys are borrowed, not copied) ---------- */

typedef struct {
    const char **keys;
    int *vals;
    size_t cap, n;
} Map;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int map_get(const Map *m, const char *k) {
    if (!m->cap) return -1;
    for (size_t i = hash_str(k) & (m->cap - 1); m->keys[i]; i = (i + 1) & (m->cap - 1))
        if (strcmp(m->keys[i], k) == 0) return m->vals[i];
    return -1;
}

static void map_put(Map *m, const char *k, int v);

static void map_grow(Map *m) {
    Map old = *m;
    m->cap = old.cap ? old.cap * 2 : 64;
    m->keys = xrealloc(NULL, m->cap * sizeof *m->keys);
    m->vals = xrealloc(NULL, m->cap * sizeof *m->vals);
    memset(m->keys, 0, m->cap * sizeof *m->keys);
    m->n = 0;
    for (size_t i = 0; i < old.cap; i++)
        if (old.keys[i]) map_put(m, old.keys[i], old.vals[i]);
    free(old.keys);
    free(old.vals);
}

static void map_put(Map *m, const char *k, int v) {
    if ((m->n + 1) * 2 > m->cap) map_grow(m);
    size_t i = hash_str(k) & (m->cap - 1);
    while (m->keys[i] && strcmp(m->keys[i], k) != 0) i = (i + 1) & (m->cap - 1);
    if (!m->keys[i]) {
        m->keys[i] = k;
        m->n++;
    }
    m->vals[i] = v;
}

static void map_free(Map *m) {
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof *m);
}

/* ---- dates and times --------------------------------------------------- */

static void date_to_tm(int date, struct tm *tm) {
    memset(tm, 0, sizeof *tm);
    tm->tm_year = date / 10000 - 1900;
    tm->tm_mon = date / 100 % 100 - 1;
    tm->tm_mday = date % 100;
    tm->tm_hour = 12; /* noon keeps DST shifts out of the way */
    tm->tm_isdst = -1;
}

static int tm_to_date(const struct tm *tm) {
    return (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
}

int date_today(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm_to_date(&tm);
}

int time_now(void) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

int date_add(int date, int days) {
    struct tm tm;
    date_to_tm(date, &tm);
    tm.tm_mday += days;
    mktime(&tm);
    return tm_to_date(&tm);
}

int date_dow(int date) {
    struct tm tm;
    date_to_tm(date, &tm);
    mktime(&tm);
    return (tm.tm_wday + 6) % 7;
}

void date_format(int date, const char *fmt, char *buf, size_t n) {
    struct tm tm;
    date_to_tm(date, &tm);
    mktime(&tm);
    strftime(buf, n, fmt, &tm);
}

static int parse_time(const char *s) {
    int h, m, sec = 0;
    if (sscanf(s, " %d:%d:%d", &h, &m, &sec) >= 2) return h * 3600 + m * 60 + sec;
    return -1;
}

/* ---- loading ----------------------------------------------------------- */

typedef struct {
    Gtfs *g;
    int ncap_st, ncap_rt, ncap_tr, ncap_tm, ncap_sv, ncap_ex;
    Map stops;    /* stop_id -> station */
    Map stations; /* station id -> station */
    Map routes, services, trips;
    char *err;
    size_t errn;
} Loader;

static CsvReader *open_table(Loader *L, const char *dir, const char *name, int required) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.txt", dir, name);
    CsvReader *r = csv_open(path);
    if (!r && required) snprintf(L->err, L->errn, "Missing or empty %s.txt in %s", name, dir);
    return r;
}

static int need(Loader *L, const CsvReader *r, const char *table, const char *col) {
    int c = csv_col(r, col);
    if (c < 0) snprintf(L->err, L->errn, "%s.txt has no '%s' column", table, col);
    return c;
}

static char *clean_name(const char *n) {
    static const char *suffixes[] = {" Caltrain Station", " Station", " Caltrain"};
    char *s = xstrdup(n);
    size_t len = strlen(s);
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        size_t sl = strlen(suffixes[i]);
        if (len > sl && strcmp(s + len - sl, suffixes[i]) == 0) {
            s[len - sl] = 0;
            break;
        }
    }
    return s;
}

static int cmp_station(const void *a, const void *b) {
    const Station *x = a, *y = b;
    if (x->lat != y->lat) return x->lat < y->lat ? 1 : -1; /* north to south */
    return strcmp(x->name, y->name);
}

typedef struct {
    char *id, *name, *parent;
    double lat;
    int type;
} RawStop;

static int load_stops(Loader *L, const char *dir) {
    Gtfs *g = L->g;
    CsvReader *r = open_table(L, dir, "stops", 1);
    if (!r) return -1;
    int c_id = need(L, r, "stops", "stop_id"), c_name = need(L, r, "stops", "stop_name");
    int c_lat = csv_col(r, "stop_lat"), c_type = csv_col(r, "location_type");
    int c_par = csv_col(r, "parent_station");
    if (c_id < 0 || c_name < 0) {
        csv_close(r);
        return -1;
    }
    RawStop *raw = NULL;
    int n = 0, cap = 0;
    while (csv_next(r)) {
        PUSH(raw, n, cap);
        raw[n].id = xstrdup(csv_get(r, c_id));
        raw[n].name = clean_name(csv_get(r, c_name));
        raw[n].parent = xstrdup(csv_get(r, c_par));
        raw[n].lat = atof(csv_get(r, c_lat));
        raw[n].type = atoi(csv_get(r, c_type));
        n++;
    }
    csv_close(r);

    /* Pass 1: stations are parent stops, or boardable stops with no parent. */
    for (int i = 0; i < n; i++) {
        int is_station = raw[i].type == 1 || (raw[i].type == 0 && !raw[i].parent[0]);
        if (!is_station) continue;
        PUSH(g->stations, g->nstations, L->ncap_st);
        g->stations[g->nstations++] = (Station){xstrdup(raw[i].id), xstrdup(raw[i].name), raw[i].lat, 0};
    }
    qsort(g->stations, (size_t)g->nstations, sizeof *g->stations, cmp_station);
    for (int i = 0; i < g->nstations; i++) map_put(&L->stations, g->stations[i].id, i);

    /* Pass 2: map every boardable stop_id to its station. The map keeps its
     * own copy of each id; gtfs_load frees them. */
    for (int i = 0; i < n; i++) {
        if (raw[i].type != 0) continue;
        int s = map_get(&L->stations, raw[i].parent[0] ? raw[i].parent : raw[i].id);
        if (s >= 0) map_put(&L->stops, xstrdup(raw[i].id), s);
    }
    for (int i = 0; i < n; i++) {
        free(raw[i].id);
        free(raw[i].name);
        free(raw[i].parent);
    }
    free(raw);
    return 0;
}

static int load_routes(Loader *L, const char *dir) {
    Gtfs *g = L->g;
    CsvReader *r = open_table(L, dir, "routes", 1);
    if (!r) return -1;
    int c_id = need(L, r, "routes", "route_id");
    int c_short = csv_col(r, "route_short_name"), c_long = csv_col(r, "route_long_name");
    if (c_id < 0) {
        csv_close(r);
        return -1;
    }
    while (csv_next(r)) {
        const char *name = csv_get(r, c_short);
        if (!*name) name = csv_get(r, c_long);
        PUSH(g->routes, g->nroutes, L->ncap_rt);
        g->routes[g->nroutes] = (Route){xstrdup(csv_get(r, c_id)), xstrdup(name)};
        map_put(&L->routes, g->routes[g->nroutes].id, g->nroutes);
        g->nroutes++;
    }
    csv_close(r);
    return 0;
}

static int service_ref(Loader *L, const char *id) {
    Gtfs *g = L->g;
    int s = map_get(&L->services, id);
    if (s >= 0) return s;
    PUSH(g->services, g->nservices, L->ncap_sv);
    memset(&g->services[g->nservices], 0, sizeof *g->services);
    g->services[g->nservices].id = xstrdup(id);
    map_put(&L->services, g->services[g->nservices].id, g->nservices);
    return g->nservices++;
}

static int load_calendar(Loader *L, const char *dir) {
    Gtfs *g = L->g;
    CsvReader *r = open_table(L, dir, "calendar", 0);
    if (!r) return 0; /* calendar.txt is optional if calendar_dates.txt exists */
    static const char *days[] = {"monday", "tuesday", "wednesday", "thursday",
                                 "friday", "saturday", "sunday"};
    int c_id = need(L, r, "calendar", "service_id");
    int c_start = csv_col(r, "start_date"), c_end = csv_col(r, "end_date");
    int c_day[7];
    for (int i = 0; i < 7; i++) c_day[i] = csv_col(r, days[i]);
    if (c_id < 0) {
        csv_close(r);
        return -1;
    }
    while (csv_next(r)) {
        int si = service_ref(L, csv_get(r, c_id)); /* may reallocate g->services */
        Service *s = &g->services[si];
        for (int i = 0; i < 7; i++) s->dow[i] = atoi(csv_get(r, c_day[i])) == 1;
        s->start = atoi(csv_get(r, c_start));
        s->end = atoi(csv_get(r, c_end));
        if (s->start && (!g->min_date || s->start < g->min_date)) g->min_date = s->start;
        if (s->end > g->max_date) g->max_date = s->end;
    }
    csv_close(r);
    return 0;
}

static int cmp_exception(const void *a, const void *b) {
    const Exception *x = a, *y = b;
    if (x->service != y->service) return x->service < y->service ? -1 : 1;
    return (x->date > y->date) - (x->date < y->date);
}

static int load_calendar_dates(Loader *L, const char *dir) {
    Gtfs *g = L->g;
    CsvReader *r = open_table(L, dir, "calendar_dates", 0);
    if (!r) return 0;
    int c_id = need(L, r, "calendar_dates", "service_id");
    int c_date = need(L, r, "calendar_dates", "date");
    int c_type = need(L, r, "calendar_dates", "exception_type");
    if (c_id < 0 || c_date < 0 || c_type < 0) {
        csv_close(r);
        return -1;
    }
    while (csv_next(r)) {
        PUSH(g->exceptions, g->nexceptions, L->ncap_ex);
        g->exceptions[g->nexceptions++] =
            (Exception){service_ref(L, csv_get(r, c_id)), atoi(csv_get(r, c_date)), atoi(csv_get(r, c_type))};
    }
    csv_close(r);
    qsort(g->exceptions, (size_t)g->nexceptions, sizeof *g->exceptions, cmp_exception);
    return 0;
}

static int load_trips(Loader *L, const char *dir) {
    Gtfs *g = L->g;
    CsvReader *r = open_table(L, dir, "trips", 1);
    if (!r) return -1;
    int c_id = need(L, r, "trips", "trip_id"), c_svc = need(L, r, "trips", "service_id");
    int c_route = need(L, r, "trips", "route_id");
    int c_num = csv_col(r, "trip_short_name"), c_head = csv_col(r, "trip_headsign");
    int c_dir = csv_col(r, "direction_id");
    if (c_id < 0 || c_svc < 0 || c_route < 0) {
        csv_close(r);
        return -1;
    }
    while (csv_next(r)) {
        PUSH(g->trips, g->ntrips, L->ncap_tr);
        Trip *t = &g->trips[g->ntrips];
        t->id = xstrdup(csv_get(r, c_id));
        t->number = xstrdup(*csv_get(r, c_num) ? csv_get(r, c_num) : t->id);
        t->headsign = xstrdup(csv_get(r, c_head));
        t->service = service_ref(L, csv_get(r, c_svc));
        t->route = map_get(&L->routes, csv_get(r, c_route));
        t->direction = atoi(csv_get(r, c_dir));
        t->first = t->count = 0;
        map_put(&L->trips, t->id, g->ntrips);
        g->ntrips++;
    }
    csv_close(r);
    return 0;
}

static int cmp_stop_time(const void *a, const void *b) {
    const StopTime *x = a, *y = b;
    if (x->trip != y->trip) return x->trip < y->trip ? -1 : 1;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

static int load_stop_times(Loader *L, const char *dir) {
    Gtfs *g = L->g;
    CsvReader *r = open_table(L, dir, "stop_times", 1);
    if (!r) return -1;
    int c_trip = need(L, r, "stop_times", "trip_id"), c_stop = need(L, r, "stop_times", "stop_id");
    int c_seq = need(L, r, "stop_times", "stop_sequence");
    int c_arr = csv_col(r, "arrival_time"), c_dep = csv_col(r, "departure_time");
    if (c_trip < 0 || c_stop < 0 || c_seq < 0) {
        csv_close(r);
        return -1;
    }
    while (csv_next(r)) {
        int trip = map_get(&L->trips, csv_get(r, c_trip));
        int station = map_get(&L->stops, csv_get(r, c_stop));
        if (trip < 0 || station < 0) continue;
        int arr = parse_time(csv_get(r, c_arr)), dep = parse_time(csv_get(r, c_dep));
        if (arr < 0) arr = dep;
        if (dep < 0) dep = arr;
        if (arr < 0) continue;
        PUSH(g->times, g->ntimes, L->ncap_tm);
        g->times[g->ntimes++] = (StopTime){trip, station, arr, dep, atoi(csv_get(r, c_seq))};
    }
    csv_close(r);

    qsort(g->times, (size_t)g->ntimes, sizeof *g->times, cmp_stop_time);
    for (int i = 0; i < g->ntimes; i++) {
        Trip *t = &g->trips[g->times[i].trip];
        if (t->count == 0) t->first = i;
        t->count++;
    }

    /* Per-station index (counting sort into CSR layout). */
    g->st_off = xrealloc(NULL, (size_t)(g->nstations + 1) * sizeof(int));
    memset(g->st_off, 0, (size_t)(g->nstations + 1) * sizeof(int));
    for (int i = 0; i < g->ntimes; i++) g->st_off[g->times[i].station + 1]++;
    for (int s = 0; s < g->nstations; s++) {
        g->stations[s].ntimes = g->st_off[s + 1];
        g->st_off[s + 1] += g->st_off[s];
    }
    g->st_idx = xrealloc(NULL, (size_t)g->ntimes * sizeof(int));
    int *cur = xrealloc(NULL, (size_t)g->nstations * sizeof(int));
    memcpy(cur, g->st_off, (size_t)g->nstations * sizeof(int));
    for (int i = 0; i < g->ntimes; i++) g->st_idx[cur[g->times[i].station]++] = i;
    free(cur);
    return 0;
}

void gtfs_free(Gtfs *g) {
    if (!g) return;
    for (int i = 0; i < g->nstations; i++) {
        free(g->stations[i].id);
        free(g->stations[i].name);
    }
    for (int i = 0; i < g->nroutes; i++) {
        free(g->routes[i].id);
        free(g->routes[i].name);
    }
    for (int i = 0; i < g->ntrips; i++) {
        free(g->trips[i].id);
        free(g->trips[i].number);
        free(g->trips[i].headsign);
    }
    for (int i = 0; i < g->nservices; i++) free(g->services[i].id);
    free(g->stations);
    free(g->routes);
    free(g->trips);
    free(g->times);
    free(g->services);
    free(g->exceptions);
    free(g->st_off);
    free(g->st_idx);
    free(g);
}

Gtfs *gtfs_load(const char *dir, char *err, size_t errn) {
    Loader L;
    memset(&L, 0, sizeof L);
    L.g = xrealloc(NULL, sizeof *L.g);
    memset(L.g, 0, sizeof *L.g);
    L.err = err;
    L.errn = errn;
    err[0] = 0;

    int ok = load_stops(&L, dir) == 0 && load_routes(&L, dir) == 0 &&
             load_calendar(&L, dir) == 0 && load_calendar_dates(&L, dir) == 0 &&
             load_trips(&L, dir) == 0 && load_stop_times(&L, dir) == 0;
    if (ok && L.g->nservices == 0) {
        snprintf(err, errn, "No calendar.txt or calendar_dates.txt in %s", dir);
        ok = 0;
    }
    if (ok && L.g->ntimes == 0) {
        snprintf(err, errn, "No usable stop_times in %s", dir);
        ok = 0;
    }
    /* The stops map owns its keys (the other maps borrow from the Gtfs). */
    for (size_t i = 0; i < L.stops.cap; i++) free((void *)L.stops.keys[i]);
    map_free(&L.stops);
    map_free(&L.stations);
    map_free(&L.routes);
    map_free(&L.services);
    map_free(&L.trips);
    if (!ok) {
        gtfs_free(L.g);
        return NULL;
    }
    return L.g;
}

/* ---- queries ----------------------------------------------------------- */

int gtfs_service_active(const Gtfs *g, int service, int date) {
    if (service < 0 || service >= g->nservices) return 0;
    Exception key = {service, date, 0};
    const Exception *e = bsearch(&key, g->exceptions, (size_t)g->nexceptions,
                                 sizeof *g->exceptions, cmp_exception);
    if (e) return e->type == 1;
    const Service *s = &g->services[service];
    return s->dow[date_dow(date)] && date >= s->start && date <= s->end;
}

static const Gtfs *sort_ctx;

static int cmp_departure(const void *a, const void *b) {
    const Departure *x = a, *y = b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    return atoi(sort_ctx->trips[x->trip].number) - atoi(sort_ctx->trips[y->trip].number);
}

int gtfs_departures(const Gtfs *g, int station, int date, int from_time,
                    int dir, Departure *out, int max) {
    if (station < 0 || station >= g->nstations) return 0;
    int n = 0, prev = date_add(date, -1);
    for (int i = g->st_off[station]; i < g->st_off[station + 1]; i++) {
        int ti = g->st_idx[i];
        const StopTime *st = &g->times[ti];
        const Trip *tr = &g->trips[st->trip];
        if (dir != DIR_ANY && tr->direction != dir) continue;
        if (ti == tr->first + tr->count - 1) continue; /* terminus */
        for (int shift = 0; shift <= 86400; shift += 86400) {
            if (shift && st->dep < 86400) continue; /* only after-midnight stops carry over */
            int t = st->dep - shift;
            if (t < from_time) continue;
            if (!gtfs_service_active(g, tr->service, shift ? prev : date)) continue;
            if (n < max) out[n++] = (Departure){st->trip, ti, t, shift};
        }
    }
    sort_ctx = g;
    qsort(out, (size_t)n, sizeof *out, cmp_departure);
    return n;
}

int gtfs_journeys(const Gtfs *g, int from, int to, int date, int from_time,
                  Journey *out, int max) {
    if (from < 0 || to < 0 || from == to) return 0;
    Departure *deps = xrealloc(NULL, (size_t)max * sizeof *deps);
    int nd = gtfs_departures(g, from, date, from_time, DIR_ANY, deps, max), n = 0;
    for (int i = 0; i < nd; i++) {
        const Trip *tr = &g->trips[deps[i].trip];
        int end = tr->first + tr->count;
        for (int s = deps[i].st + 1; s < end; s++) {
            if (g->times[s].station != to) continue;
            out[n++] = (Journey){deps[i], s, g->times[s].arr - deps[i].shift};
            break;
        }
    }
    free(deps);
    return n;
}
