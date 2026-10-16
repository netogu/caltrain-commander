#ifndef CSV_H
#define CSV_H

#include <stdio.h>

/* Minimal RFC-4180 reader for GTFS tables. The first record is the header;
 * columns are then looked up by name so file column order does not matter. */
typedef struct CsvReader {
    FILE *fp;
    char *buf;
    size_t len, cap;
    size_t *offs;
    int offcap;
    char **fields;
    int nfields;
    char **hdr;
    int nhdr;
} CsvReader;

CsvReader *csv_open(const char *path);          /* NULL if unreadable/empty */
int csv_next(CsvReader *r);                     /* 1 = record read, 0 = EOF */
int csv_col(const CsvReader *r, const char *name); /* -1 if no such column */
const char *csv_get(const CsvReader *r, int col);  /* "" if absent */
void csv_close(CsvReader *r);

#endif
