#include "csv.h"
#include "util.h"

static void put(CsvReader *r, int c) {
    if (r->len + 1 >= r->cap) {
        r->cap = r->cap ? r->cap * 2 : 1024;
        r->buf = xrealloc(r->buf, r->cap);
    }
    r->buf[r->len++] = (char)c;
}

static void end_field(CsvReader *r, size_t start) {
    put(r, 0);
    if (r->nfields == r->offcap) r->offs = xgrow(r->offs, &r->offcap, sizeof *r->offs);
    r->offs[r->nfields++] = start;
}

/* Reads one record into r->fields. Blank lines are skipped. */
static int read_record(CsvReader *r) {
    for (;;) {
        r->len = 0;
        r->nfields = 0;
        size_t start = 0;
        int in_q = 0, any = 0, c;
        while ((c = getc(r->fp)) != EOF) {
            any = 1;
            if (in_q) {
                if (c == '"') {
                    int n = getc(r->fp);
                    if (n == '"') put(r, '"');
                    else {
                        in_q = 0;
                        if (n != EOF) ungetc(n, r->fp);
                    }
                } else put(r, c);
            } else if (c == '"') in_q = 1;
            else if (c == ',') {
                end_field(r, start);
                start = r->len;
            } else if (c == '\n') break;
            else if (c != '\r') put(r, c);
        }
        if (!any) return 0;
        end_field(r, start);
        if (r->nfields == 1 && r->buf[0] == 0) {
            if (c == EOF) return 0;
            continue; /* blank line */
        }
        if (r->nfields > 0) {
            r->fields = xrealloc(r->fields, (size_t)r->nfields * sizeof *r->fields);
            for (int i = 0; i < r->nfields; i++) r->fields[i] = r->buf + r->offs[i];
        }
        return 1;
    }
}

CsvReader *csv_open(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    CsvReader *r = xrealloc(NULL, sizeof *r);
    memset(r, 0, sizeof *r);
    r->fp = fp;
    /* Skip a UTF-8 byte-order mark. */
    int b0 = getc(fp);
    if (b0 == 0xEF) {
        int b1 = getc(fp), b2 = getc(fp);
        if (!(b1 == 0xBB && b2 == 0xBF)) rewind(fp);
    } else if (b0 != EOF) ungetc(b0, fp);

    if (!read_record(r)) {
        csv_close(r);
        return NULL;
    }
    r->nhdr = r->nfields;
    r->hdr = xrealloc(NULL, (size_t)r->nhdr * sizeof *r->hdr);
    for (int i = 0; i < r->nhdr; i++) r->hdr[i] = xstrdup(r->fields[i]);
    return r;
}

int csv_next(CsvReader *r) { return read_record(r); }

int csv_col(const CsvReader *r, const char *name) {
    for (int i = 0; i < r->nhdr; i++)
        if (strcmp(r->hdr[i], name) == 0) return i;
    return -1;
}

const char *csv_get(const CsvReader *r, int col) {
    if (col < 0 || col >= r->nfields) return "";
    return r->fields[col];
}

void csv_close(CsvReader *r) {
    if (!r) return;
    for (int i = 0; i < r->nhdr; i++) free(r->hdr[i]);
    free(r->hdr);
    free(r->buf);
    free(r->offs);
    free(r->fields);
    if (r->fp) fclose(r->fp);
    free(r);
}
