#ifndef GTFS_H
#define GTFS_H

#include <stddef.h>

#define DIR_ANY (-1) /* GTFS direction_id: 0 = northbound, 1 = southbound */

/* A station is a parent stop; platform stops are folded into it. */
typedef struct {
    char *id;
    char *name; /* "Station" suffixes stripped */
    double lat;
    int ntimes; /* number of stop_times at this station (0 = unserved) */
} Station;

typedef struct {
    char *id;
    char *name; /* route_short_name, e.g. "Limited" */
} Route;

typedef struct {
    int trip;
    int station;
    int arr, dep; /* seconds since service-day midnight; may exceed 86400 */
    int seq;
} StopTime;

typedef struct {
    char *id;
    char *number;   /* train number shown to riders */
    char *headsign;
    int service;
    int route;
    int direction;
    int first, count; /* slice of Gtfs.times, ordered by stop_sequence */
} Trip;

typedef struct {
    char *id;
    unsigned char dow[7]; /* Monday..Sunday */
    int start, end;       /* yyyymmdd, inclusive */
} Service;

typedef struct {
    int service, date;
    int type; /* 1 = added, 2 = removed */
} Exception;

typedef struct Gtfs {
    Station *stations; int nstations;
    Route *routes;     int nroutes;
    Trip *trips;       int ntrips;
    StopTime *times;   int ntimes;
    Service *services; int nservices;
    Exception *exceptions; int nexceptions;
    int *st_off, *st_idx; /* per-station index into times (CSR layout) */
    int min_date, max_date; /* calendar.txt coverage, 0 if unknown */
} Gtfs;

/* One upcoming departure of a trip from a station. */
typedef struct {
    int trip;
    int st;    /* index into Gtfs.times of the departing stop */
    int time;  /* departure, seconds after midnight of the queried date */
    int shift; /* 86400 if the trip belongs to the previous service day */
} Departure;

typedef struct {
    Departure dep;
    int arr_st;   /* index into Gtfs.times of the arrival stop */
    int arr_time; /* seconds after midnight of the queried date */
} Journey;

Gtfs *gtfs_load(const char *dir, char *err, size_t errn);
void gtfs_free(Gtfs *g);

int gtfs_service_active(const Gtfs *g, int service, int date);

/* Departures at `station` on `date` at or after `from_time`, soonest first.
 * Terminating trips are excluded. Returns the number written (<= max). */
int gtfs_departures(const Gtfs *g, int station, int date, int from_time,
                    int dir, Departure *out, int max);

/* Direct trains from `from` to `to`. */
int gtfs_journeys(const Gtfs *g, int from, int to, int date, int from_time,
                  Journey *out, int max);

/* Dates are yyyymmdd integers; weekdays are 0 = Monday .. 6 = Sunday. */
int date_today(void);
int time_now(void); /* seconds since local midnight */
int date_add(int date, int days);
int date_dow(int date);
void date_format(int date, const char *fmt, char *buf, size_t n);

#endif
