#include "ui.h"
#include "util.h"

#include <ctype.h>
#include <curses.h>
#include <locale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXLIST 1024
#define MIN_ROWS 12
#define MIN_COLS 64
#define KEY_ESC 27
#define CTRL(c) ((c) & 0x1f)

enum { M_BOARD, M_PLAN, M_DETAIL };

#define COLW 7 /* one train column of the schedule table */

/* A schedule reload running on a worker thread so the UI keeps redrawing. */
typedef struct {
    UiReloadFn fn;
    void *ctx;
    Gtfs *ng;
    char note[512];
    int rc;
    atomic_int done;
} ReloadJob;

typedef struct {
    Gtfs **gp;
    UiReloadFn reload;
    void *ctx;
    int mode, prev_mode;
    int date;
    int *vis, nvis; /* stations that have service, north to south */
    /* departures board */
    int sel, st_top, focus, dirf, dep_sel, dep_top, ndeps;
    Departure *deps;
    int list_view; /* 0 = station x train table, 1 = departures list */
    int *tcols, ntcols, tc_sel, tc_left, tpage, tdirty; /* table columns = trips */
    /* trip planner */
    int from, to, plan_sel, plan_top, njr;
    int plan_live, pdate, ptime; /* planner "leave after"; live follows the clock */
    Journey *jrs;
    /* train detail */
    int dtrip, d_top, hl_a, hl_b;
    int page; /* rows per page of the list last drawn */
    char msg[512];
    time_t msg_until;
    int help;
    ReloadJob *job;
    pthread_t job_th;
    int spin;
} App;

static int at_title, at_sel, at_head, at_good, at_nb, at_sb, at_dim, at_warn, at_bar;

static void init_attrs(void) {
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
        init_pair(2, COLOR_BLACK, COLOR_CYAN);
        init_pair(3, COLOR_YELLOW, -1);
        init_pair(4, COLOR_GREEN, -1);
        init_pair(5, COLOR_CYAN, -1);
        init_pair(6, COLOR_MAGENTA, -1);
        init_pair(7, COLOR_BLACK, COLOR_WHITE);
        init_pair(8, COLOR_RED, -1);
        at_title = COLOR_PAIR(1) | A_BOLD;
        at_sel = COLOR_PAIR(2);
        at_head = COLOR_PAIR(3) | A_BOLD;
        at_good = COLOR_PAIR(4) | A_BOLD;
        at_nb = COLOR_PAIR(5) | A_BOLD;
        at_sb = COLOR_PAIR(6) | A_BOLD;
        at_dim = A_DIM;
        at_warn = COLOR_PAIR(8) | A_BOLD;
        at_bar = COLOR_PAIR(7);
    } else {
        at_title = A_REVERSE | A_BOLD;
        at_sel = A_REVERSE;
        at_head = A_BOLD | A_UNDERLINE;
        at_good = A_BOLD;
        at_nb = at_sb = 0;
        at_dim = A_DIM;
        at_warn = A_BOLD;
        at_bar = A_REVERSE;
    }
}

/* ---- drawing helpers --------------------------------------------------- */

/* Print formatted text clipped/padded to exactly w cells. */
static void txt(int y, int x, int w, int attr, const char *fmt, ...) {
    if (w <= 0 || y < 0 || y >= LINES) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    attrset(attr);
    mvaddnstr(y, x, buf, w);
    for (int i = (int)strlen(buf); i < w; i++) mvaddch(y, x + i, ' ');
    attrset(A_NORMAL);
}

static void box_at(int y, int x, int h, int w, const char *title, int attr) {
    attrset(attr);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    if (title && w > 6) mvprintw(y, x + 2, " %.*s ", w - 6, title);
    attrset(A_NORMAL);
}

static void clear_rect(int y, int x, int h, int w) {
    for (int r = 0; r < h; r++) txt(y + r, x, w, A_NORMAL, "");
}

static void keep_visible(int *sel, int *top, int n, int rows) {
    if (n <= 0 || rows <= 0) {
        *sel = *top = 0;
        return;
    }
    if (*sel < 0) *sel = 0;
    if (*sel >= n) *sel = n - 1;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + rows) *top = *sel - rows + 1;
    if (*top > n - rows) *top = n > rows ? n - rows : 0;
    if (*top < 0) *top = 0;
}

static int row_attr(int selected, int focused) {
    if (!selected) return A_NORMAL;
    return focused ? at_sel : A_BOLD;
}

/* ---- formatting -------------------------------------------------------- */

static void fmt_hm(int secs, char *buf, size_t n) {
    snprintf(buf, n, "%02d:%02d%s", (secs / 3600) % 24, (secs / 60) % 60, secs >= 86400 ? "+1" : "");
}

static void fmt_dur(int mins, char *buf, size_t n) {
    if (mins < 60) snprintf(buf, n, "%dm", mins);
    else snprintf(buf, n, "%dh%02d", mins / 60, mins % 60);
}

static void fmt_in(int mins, char *buf, size_t n) {
    if (mins <= 0) snprintf(buf, n, "now");
    else fmt_dur(mins, buf, n);
}

static const char *dir_name(int d) { return d == 0 ? "NB" : d == 1 ? "SB" : "--"; }
static int dir_attr(int d) { return d == 0 ? at_nb : d == 1 ? at_sb : A_NORMAL; }

static const char *trip_type(const Gtfs *g, const Trip *t) {
    return t->route >= 0 ? g->routes[t->route].name : "";
}

/* "Local Weekday" and "Local Weekend" are both just "Local" on a dated list. */
static int type_len(const char *name) {
    int n = (int)strlen(name);
    static const char *suffixes[] = {" Weekday", " Weekend"};
    for (int i = 0; i < 2; i++) {
        int sl = (int)strlen(suffixes[i]);
        if (n > sl && strcmp(name + n - sl, suffixes[i]) == 0) return n - sl;
    }
    return n;
}

/* Case-insensitive subsequence match: "sj" finds "San Jose Diridon". */
static int fuzzy_match(const char *hay, const char *needle) {
    for (; *hay && *needle; hay++)
        if (tolower((unsigned char)*hay) == tolower((unsigned char)*needle)) needle++;
    return *needle == 0;
}

static void set_msg(App *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof a->msg, fmt, ap);
    va_end(ap);
    a->msg_until = time(NULL) + 8;
}

/* ---- state ------------------------------------------------------------- */

static void build_vis(App *a) {
    const Gtfs *g = *a->gp;
    free(a->vis);
    a->vis = xrealloc(NULL, (size_t)(g->nstations + 1) * sizeof *a->vis);
    a->nvis = 0;
    for (int i = 0; i < g->nstations; i++)
        if (g->stations[i].ntimes > 0) a->vis[a->nvis++] = i;
}

static int now_floor(const App *a) { return a->date == date_today() ? time_now() / 60 * 60 : 0; }

static int from_time_live(void) { return time_now() / 60 * 60; }

static const Gtfs *cmp_g;
static int cmp_trip_start(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    int ta = cmp_g->times[cmp_g->trips[a].first].dep, tb = cmp_g->times[cmp_g->trips[b].first].dep;
    return ta != tb ? ta - tb : a - b;
}

/* Columns of the schedule table: every train running on the board's date. */
static void build_cols(App *a) {
    const Gtfs *g = *a->gp;
    a->tcols = xrealloc(a->tcols, (size_t)(g->ntrips + 1) * sizeof *a->tcols);
    a->ntcols = 0;
    for (int i = 0; i < g->ntrips; i++) {
        const Trip *t = &g->trips[i];
        if (t->count < 1 || (a->dirf != DIR_ANY && t->direction != a->dirf)) continue;
        if (gtfs_service_active(g, t->service, a->date)) a->tcols[a->ntcols++] = i;
    }
    cmp_g = g;
    qsort(a->tcols, (size_t)a->ntcols, sizeof *a->tcols, cmp_trip_start);
    a->tc_sel = a->tc_left = 0;
    if (a->date == date_today()) { /* start at the first train still running */
        int now = time_now();
        for (int i = 0; i < a->ntcols; i++) {
            const Trip *t = &g->trips[a->tcols[i]];
            if (g->times[t->first + t->count - 1].arr >= now) {
                a->tc_sel = i;
                break;
            }
        }
    }
    a->tdirty = 0;
}

static void refresh_lists(App *a) {
    const Gtfs *g = *a->gp;
    int from_time = now_floor(a);
    a->ndeps = 0;
    if (a->nvis > 0 && a->sel >= 0 && a->sel < a->nvis)
        a->ndeps = gtfs_departures(g, a->vis[a->sel], a->date, from_time, a->dirf, a->deps, MAXLIST);
    if (a->plan_live) {
        a->pdate = date_today();
        a->ptime = from_time_live();
    }
    a->njr = gtfs_journeys(g, a->from, a->to, a->pdate, a->ptime, a->jrs, MAXLIST);
    if (a->tdirty) build_cols(a);
}

static void open_detail(App *a, int trip, int hl_a, int hl_b) {
    const Gtfs *g = *a->gp;
    a->dtrip = trip;
    a->hl_a = hl_a;
    a->hl_b = hl_b;
    a->d_top = 0;
    const Trip *t = &g->trips[trip];
    for (int i = 0; i < t->count; i++) /* scroll the origin into view */
        if (g->times[t->first + i].station == hl_a) {
            a->d_top = i > 2 ? i - 2 : 0;
            break;
        }
    if (a->mode != M_DETAIL) a->prev_mode = a->mode;
    a->mode = M_DETAIL;
}

/* ---- views ------------------------------------------------------------- */

static void draw_title(const App *a, int cols) {
    static const char *tabs[] = {" 1 Departures ", " 2 Trip Planner "};
    txt(0, 0, cols, at_title, " Caltrain Commander");
    int x = 21, active = a->mode == M_DETAIL ? a->prev_mode : a->mode;
    for (int i = 0; i < 2; i++) {
        int w = (int)strlen(tabs[i]);
        txt(0, x, w, i == active ? at_sel : at_title, "%s", tabs[i]);
        x += w + 1;
    }
    char clk[32];
    time_t t = time(NULL);
    strftime(clk, sizeof clk, "%H:%M:%S ", localtime(&t));
    txt(0, cols - (int)strlen(clk), (int)strlen(clk), at_title, "%s", clk);
}

static void draw_status(const App *a, int rows, int cols) {
    static const char *hints[] = {
        " h/l train  j/k station  Enter stops  / find  n/s/a dir  v list  [ ] day  t today  r reload  ? help  q quit",
        " o origin  d dest  x swap  T date/time  [ ] day  , . 30min  t now  Enter stops  r reload  ? help  q quit",
        " j/k scroll  PgUp/PgDn page  Esc back  q quit",
    };
    if (a->job) {
        static const char spin[] = "|/-\\";
        txt(rows - 1, 0, cols, at_bar | A_BOLD, " Reloading schedule... %c (keys still work; old data shown until done)",
            spin[a->spin & 3]);
    } else if (a->msg[0] && time(NULL) < a->msg_until) txt(rows - 1, 0, cols, at_bar | A_BOLD, " %s", a->msg);
    else txt(rows - 1, 0, cols, at_bar, "%s", hints[a->mode]);
}

static void date_label(const App *a, char *buf, size_t n) {
    char d[64];
    date_format(a->date, "%a %b %d %Y", d, sizeof d);
    snprintf(buf, n, "%s%s", d, a->date == date_today() ? " (today)" : "");
}

static void draw_dep_list(App *a, int y0, int h, int cols) {
    const Gtfs *g = *a->gp;
    int lw = cols / 3;
    if (lw < 24) lw = 24;
    if (lw > 34) lw = 34;
    int rw = cols - lw, ih = h - 2;

    /* Station list. */
    box_at(y0, 0, h, lw, "Stations", a->focus == 0 ? at_head : A_NORMAL);
    keep_visible(&a->sel, &a->st_top, a->nvis, ih);
    for (int r = 0; r < ih; r++) {
        int i = a->st_top + r;
        if (i < a->nvis) {
            int at = row_attr(i == a->sel, a->focus == 0);
            txt(y0 + 1 + r, 1, lw - 2, at, "%c %s", i == a->sel ? '>' : ' ', g->stations[a->vis[i]].name);
        } else txt(y0 + 1 + r, 1, lw - 2, A_NORMAL, "");
    }

    /* Departures. */
    const char *name = a->nvis ? g->stations[a->vis[a->sel]].name : "";
    char title[128], label[96], t1[16], t2[16];
    snprintf(title, sizeof title, "Departures - %s", name);
    box_at(y0, lw, h, rw, title, a->focus == 1 ? at_head : A_NORMAL);
    int x = lw + 1, iw = rw - 2, today = a->date == date_today();
    date_label(a, label, sizeof label);
    txt(y0 + 1, x, iw, at_dim, " %s   Direction: %s", label,
        a->dirf == 0 ? "northbound" : a->dirf == 1 ? "southbound" : "all");

    int wide = iw >= 58;
    int to_w = iw - 26 - (wide ? 13 : 0);
    if (to_w < 4) to_w = 4;
    txt(y0 + 2, x, iw, at_head, " %-8s %-5s %-2s %-*s%-*s %6s", "Time", "Train", "", wide ? 13 : 0,
        wide ? "Type" : "", to_w, "To", today ? "In" : "");

    int lines = ih - 2;
    a->page = lines;
    keep_visible(&a->dep_sel, &a->dep_top, a->ndeps, lines);
    for (int r = 0; r < lines; r++) {
        int i = a->dep_top + r, y = y0 + 3 + r;
        if (i >= a->ndeps) {
            txt(y, x, iw, A_NORMAL, "");
            continue;
        }
        const Departure *d = &a->deps[i];
        const Trip *t = &g->trips[d->trip];
        int sel = i == a->dep_sel, at = row_attr(sel, a->focus == 1);
        fmt_hm(d->time, t1, sizeof t1);
        txt(y, x, iw, at, "");
        int cx = x + 1;
        txt(y, cx, 8, at, "%s", t1);
        cx += 9;
        txt(y, cx, 5, at, "%s", t->number);
        cx += 6;
        txt(y, cx, 2, sel && a->focus == 1 ? at : dir_attr(t->direction), "%s", dir_name(t->direction));
        cx += 3;
        if (wide) {
            txt(y, cx, 12, at, "%.*s", type_len(trip_type(g, t)), trip_type(g, t));
            cx += 13;
        }
        txt(y, cx, to_w, at, "%s", t->headsign);
        cx += to_w;
        if (today) {
            int mins = (d->time - now_floor(a)) / 60;
            fmt_in(mins, t2, sizeof t2);
            txt(y, cx, 7, sel && a->focus == 1 ? at : (mins <= 10 ? at_good : at), "%7s", t2);
        }
    }
    if (a->ndeps == 0)
        txt(y0 + 3, x, iw, at_dim, " %s", today ? "No more trains today." : "No trains on this date.");
}

static const char *short_type(const Gtfs *g, const Trip *t) {
    static char buf[8];
    const char *n = trip_type(g, t);
    int len = type_len(n);
    if (len == 7 && !strncmp(n, "Limited", 7)) return "Ltd";
    if (len == 7 && !strncmp(n, "Express", 7)) return "Exp";
    if (len == 12 && !strncmp(n, "South County", 12)) return "SoCo";
    snprintf(buf, sizeof buf, "%.*s", len < 6 ? len : 6, n);
    return buf;
}

/* Rows are stations (north to south), columns are trains. */
static void draw_table(App *a, int y0, int h, int cols) {
    const Gtfs *g = *a->gp;
    char label[96], cell[16];
    box_at(y0, 0, h, cols, "Schedule", A_NORMAL);
    int iw = cols - 2, nw = iw / 4 < 22 ? 22 : iw / 4;
    if (nw > 28) nw = 28;
    int vis = (iw - nw) / COLW;
    if (vis < 1) vis = 1;
    a->tpage = vis;

    if (a->tc_sel >= a->ntcols) a->tc_sel = a->ntcols - 1;
    if (a->tc_sel < 0) a->tc_sel = 0;
    if (a->tc_sel < a->tc_left) a->tc_left = a->tc_sel;
    if (a->tc_sel >= a->tc_left + vis) a->tc_left = a->tc_sel - vis + 1;
    if (a->tc_left > a->ntcols - vis) a->tc_left = a->ntcols > vis ? a->ntcols - vis : 0;
    if (a->tc_left < 0) a->tc_left = 0;

    date_label(a, label, sizeof label);
    int shown_hi = a->tc_left + vis < a->ntcols ? a->tc_left + vis : a->ntcols;
    txt(y0 + 1, 1, iw, at_dim, " %s   %s   trains %d-%d of %d", label,
        a->dirf == 0 ? "northbound" : a->dirf == 1 ? "southbound" : "all directions",
        a->ntcols ? a->tc_left + 1 : 0, shown_hi, a->ntcols);
    txt(y0 + 2, 1, nw, at_head, " Train");
    txt(y0 + 3, 1, nw, at_head, " Station");

    int lines = h - 5;
    a->page = lines;
    keep_visible(&a->sel, &a->st_top, a->nvis, lines);

    /* time of each visible train at each station, -1 where it does not stop */
    int ns = g->nstations;
    int *tm = xrealloc(NULL, (size_t)vis * (size_t)(ns ? ns : 1) * sizeof *tm);
    for (int k = 0; k < vis * ns; k++) tm[k] = -1;
    for (int c = 0; c < vis; c++) {
        int i = a->tc_left + c, x = 1 + nw + c * COLW;
        if (i >= a->ntcols) {
            txt(y0 + 2, x, COLW, A_NORMAL, "");
            txt(y0 + 3, x, COLW, A_NORMAL, "");
            continue;
        }
        const Trip *t = &g->trips[a->tcols[i]];
        int sel = i == a->tc_sel;
        txt(y0 + 2, x, COLW, sel ? at_sel : dir_attr(t->direction), " %-6s", t->number);
        txt(y0 + 3, x, COLW, sel ? at_sel : at_dim, " %-6s", short_type(g, t));
        for (int k = 0; k < t->count; k++) {
            const StopTime *st = &g->times[t->first + k];
            tm[c * ns + st->station] = k == t->count - 1 ? st->arr : st->dep;
        }
    }

    for (int r = 0; r < lines; r++) {
        int i = a->st_top + r, y = y0 + 4 + r;
        if (i >= a->nvis) {
            txt(y, 1, iw, A_NORMAL, "");
            continue;
        }
        int st = a->vis[i], rsel = i == a->sel;
        txt(y, 1, nw, rsel ? A_BOLD : A_NORMAL, "%c%s", rsel ? '>' : ' ', g->stations[st].name);
        for (int c = 0; c < vis; c++) {
            int x = 1 + nw + c * COLW, tcol = a->tc_left + c, v = tm[c * ns + st];
            int csel = tcol == a->tc_sel;
            if (tcol >= a->ntcols) {
                txt(y, x, COLW, A_NORMAL, "");
                continue;
            }
            int at = rsel && csel ? at_sel : csel || rsel ? A_BOLD : A_NORMAL;
            if (v < 0) {
                txt(y, x, COLW, rsel && csel ? at_sel : at_dim, "   -   ");
                continue;
            }
            fmt_hm(v, cell, sizeof cell);
            if (v >= 86400) cell[5] = '+', cell[6] = 0;
            else cell[5] = ' ', cell[6] = 0;
            txt(y, x, COLW, at, " %s", cell);
        }
    }
    free(tm);
    if (a->nvis == 0 || a->ntcols == 0)
        txt(y0 + 4, 1, iw, at_dim, " %s", "No trains on this date.");
}

static void draw_board(App *a, int y0, int h, int cols) {
    if (a->list_view) draw_dep_list(a, y0, h, cols);
    else draw_table(a, y0, h, cols);
}

static void plan_label(const App *a, char *buf, size_t n) {
    char d[64];
    date_format(a->pdate, "%a %b %d %Y", d, sizeof d);
    snprintf(buf, n, "%s %02d:%02d%s", d, (a->ptime / 3600) % 24, (a->ptime / 60) % 60,
             a->plan_live ? " (now)" : "");
}

static void draw_plan(App *a, int y0, int h, int cols) {
    const Gtfs *g = *a->gp;
    char label[96], t1[16], t2[16], t3[16];
    box_at(y0, 0, 4, cols, "Trip planner", A_NORMAL);
    const char *from = a->from >= 0 ? g->stations[a->from].name : "(press o to choose)";
    const char *to = a->to >= 0 ? g->stations[a->to].name : "(press d to choose)";
    int half = (cols - 2) / 2;
    txt(y0 + 1, 1, half, A_NORMAL, " From: %s", from);
    txt(y0 + 1, 1 + half, cols - 2 - half, A_NORMAL, " To:   %s", to);
    plan_label(a, label, sizeof label);
    txt(y0 + 2, 1, cols - 2, at_dim, " Leave after: %s   (T sets date/time, x swaps)", label);

    int by = y0 + 4, bh = h - 4, iw = cols - 2, today = a->plan_live;
    box_at(by, 0, bh, cols, "Direct trains", A_NORMAL);
    int wide = iw >= 70;
    int to_w = iw - 42 - (wide ? 13 : 0);
    txt(by + 1, 1, iw, at_head, " %-8s %-8s %-6s %-5s %-2s %-*s%-*s %6s", "Depart", "Arrive", "Dur", "Train", "",
        wide ? 13 : 0, wide ? "Type" : "", to_w > 0 ? to_w : 0, "To", today ? "In" : "");
    int lines = bh - 3;
    a->page = lines;
    keep_visible(&a->plan_sel, &a->plan_top, a->njr, lines);
    for (int r = 0; r < lines; r++) {
        int i = a->plan_top + r, y = by + 2 + r;
        if (i >= a->njr) {
            txt(y, 1, iw, A_NORMAL, "");
            continue;
        }
        const Journey *j = &a->jrs[i];
        const Trip *t = &g->trips[j->dep.trip];
        int sel = i == a->plan_sel, at = row_attr(sel, 1);
        txt(y, 1, iw, at, "");
        int cx = 2;
        fmt_hm(j->dep.time, t1, sizeof t1);
        fmt_hm(j->arr_time, t2, sizeof t2);
        fmt_dur((j->arr_time - j->dep.time) / 60, t3, sizeof t3);
        txt(y, cx, 8, at, "%s", t1);
        cx += 9;
        txt(y, cx, 8, at, "%s", t2);
        cx += 9;
        txt(y, cx, 6, at, "%s", t3);
        cx += 7;
        txt(y, cx, 5, at, "%s", t->number);
        cx += 6;
        txt(y, cx, 2, sel ? at : dir_attr(t->direction), "%s", dir_name(t->direction));
        cx += 3;
        if (wide) {
            txt(y, cx, 12, at, "%.*s", type_len(trip_type(g, t)), trip_type(g, t));
            cx += 13;
        }
        if (to_w > 0) txt(y, cx, to_w, at, "%s", t->headsign);
        cx += to_w > 0 ? to_w : 0;
        if (today) {
            int mins = (j->dep.time - a->ptime) / 60;
            fmt_in(mins, t1, sizeof t1);
            txt(y, cx, 7, sel ? at : (mins <= 10 ? at_good : at), "%7s", t1);
        }
    }
    if (a->from < 0 || a->to < 0) txt(by + 2, 1, iw, at_dim, " Choose an origin (o) and a destination (d).");
    else if (a->from == a->to) txt(by + 2, 1, iw, at_dim, " Origin and destination are the same station.");
    else if (a->njr == 0) txt(by + 2, 1, iw, at_dim, " No direct trains %s.", today ? "for the rest of today" : "on this date");
}

static void draw_detail(App *a, int y0, int h, int cols) {
    const Gtfs *g = *a->gp;
    const Trip *t = &g->trips[a->dtrip];
    char title[64], ts[16], tt[16];
    snprintf(title, sizeof title, "Train %s", t->number);
    box_at(y0, 0, h, cols, title, A_NORMAL);
    int iw = cols - 2;
    txt(y0 + 1, 1, iw, at_head, " %s - %s to %s", trip_type(g, t),
        t->direction == 0 ? "Northbound" : t->direction == 1 ? "Southbound" : "Train", t->headsign);
    int nw = iw - 21 < 34 ? iw - 21 : 34;
    if (nw < 8) nw = 8;
    txt(y0 + 2, 1, iw, at_dim, " %-*s %8s %8s", nw, "Stop", "Arrive", "Depart");
    int lines = h - 4;
    a->page = lines;
    if (a->d_top > t->count - lines) a->d_top = t->count - lines; /* d_top doubles as the scroll cursor */
    if (a->d_top < 0) a->d_top = 0;
    for (int r = 0; r < lines; r++) {
        int i = a->d_top + r, y = y0 + 3 + r;
        if (i >= t->count) {
            txt(y, 1, iw, A_NORMAL, "");
            continue;
        }
        const StopTime *st = &g->times[t->first + i];
        int hl = st->station == a->hl_a || st->station == a->hl_b;
        fmt_hm(st->arr, ts, sizeof ts);
        fmt_hm(st->dep, tt, sizeof tt);
        txt(y, 1, iw, hl ? at_good : A_NORMAL, "%c%-*s %8s %8s", hl ? '*' : ' ', nw,
            g->stations[st->station].name, ts, tt);
    }
}

static void popup(int h, int w, const char *title, int *py, int *px) {
    if (h > LINES - 2) h = LINES - 2;
    if (w > COLS - 2) w = COLS - 2;
    int y = (LINES - h) / 2, x = (COLS - w) / 2;
    clear_rect(y, x, h, w);
    box_at(y, x, h, w, title, at_head);
    *py = y;
    *px = x;
}

static void draw_help(void) {
    static const char *lines[] = {
        "1 / 2 / Tab     Departures board / Trip planner",
        "j k  arrows     Move       PgUp PgDn  Page     g G  First/last",
        "h l             Board: previous / next train (list view: switch pane)",
        "v               Board: table / departures list view",
        "Enter           Show every stop of the selected train",
        "Esc             Close a train / dialog",
        "/               Board: find a station by name",
        "n s a           Board: northbound / southbound / all",
        "o d x           Planner: origin / destination / swap",
        "T , .  [ ] t    Planner: set date/time, -/+30 min, -/+ day, now",
        "[ ] t           Board: previous day / next day / today",
        "r               Re-download the schedule",
        "q               Quit",
        "",
        "Times are Pacific. \"+1\" means after midnight.",
    };
    int n = (int)(sizeof lines / sizeof *lines), y, x;
    popup(n + 2, 66, "Help (any key closes)", &y, &x);
    for (int i = 0; i < n; i++) txt(y + 1 + i, x + 2, 62, A_NORMAL, "%s", lines[i]);
}

static void render(App *a) {
    erase();
    if (LINES < MIN_ROWS || COLS < MIN_COLS) {
        txt(0, 0, COLS, A_NORMAL, "Terminal too small: %dx%d (need %dx%d)", COLS, LINES, MIN_COLS, MIN_ROWS);
        return;
    }
    refresh_lists(a);
    draw_title(a, COLS);
    int h = LINES - 2;
    if (a->mode == M_BOARD) draw_board(a, 1, h, COLS);
    else if (a->mode == M_PLAN) draw_plan(a, 1, h, COLS);
    else draw_detail(a, 1, h, COLS);
    draw_status(a, LINES, COLS);
    if (a->help) draw_help();
}

/* ---- station picker ---------------------------------------------------- */

/* Modal station chooser with type-to-filter. Returns a station index, or -1. */
static int pick_station(App *a, const char *title, int current) {
    const Gtfs *g = *a->gp;
    char q[64] = "";
    int ql = 0, sel = 0, top = 0, result = -1, first = 1;
    int *list = xrealloc(NULL, (size_t)(a->nvis + 1) * sizeof *list);

    for (;;) {
        int n = 0;
        for (int i = 0; i < a->nvis; i++)
            if (fuzzy_match(g->stations[a->vis[i]].name, q)) list[n++] = a->vis[i];
        if (first) {
            for (int i = 0; i < n; i++)
                if (list[i] == current) sel = i;
            first = 0;
        }

        render(a);
        int y, x, rows = n < 12 ? n : 12;
        if (rows < 3) rows = 3;
        popup(rows + 4, 40, title, &y, &x);
        txt(y + 1, x + 2, 36, A_NORMAL, "Find: %s_", q);
        keep_visible(&sel, &top, n, rows);
        for (int r = 0; r < rows; r++) {
            int i = top + r;
            if (i < n) txt(y + 3 + r, x + 2, 36, row_attr(i == sel, 1), " %s", g->stations[list[i]].name);
        }
        if (n == 0) txt(y + 3, x + 2, 36, at_dim, " No matching station");
        refresh();

        int ch = getch();
        if (ch == ERR || ch == KEY_RESIZE) continue;
        if (ch == KEY_ESC) break;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (n > 0) result = list[sel];
            break;
        }
        if (ch == KEY_UP || ch == CTRL('p')) sel--;
        else if (ch == KEY_DOWN || ch == CTRL('n')) sel++;
        else if (ch == KEY_PPAGE) sel -= rows;
        else if (ch == KEY_NPAGE) sel += rows;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (ql > 0) q[--ql] = 0;
            sel = top = 0;
        } else if (ch == CTRL('u')) {
            q[ql = 0] = 0;
            sel = top = 0;
        } else if (ch >= 32 && ch < 127 && ql < (int)sizeof q - 1) {
            q[ql++] = (char)ch;
            q[ql] = 0;
            sel = top = 0;
        }
    }
    free(list);
    return result;
}

/* ---- input ------------------------------------------------------------- */

static void reset_lists(App *a) {
    a->dep_sel = a->dep_top = a->plan_sel = a->plan_top = 0;
    a->tdirty = 1;
}

/* Move the planner's "leave after" time; the first change freezes the clock. */
static void plan_shift(App *a, int days, int minutes) {
    if (a->plan_live) {
        a->plan_live = 0;
        a->pdate = date_today();
        a->ptime = from_time_live();
    }
    a->pdate = date_add(a->pdate, days);
    a->ptime += minutes * 60;
    while (a->ptime < 0) a->ptime += 86400, a->pdate = date_add(a->pdate, -1);
    while (a->ptime >= 86400) a->ptime -= 86400, a->pdate = date_add(a->pdate, 1);
    a->plan_sel = a->plan_top = 0;
}

/* Accepts "HH:MM", "YYYY-MM-DD" or both; missing parts keep their value. */
static int parse_datetime(const char *q, int *date, int *time) {
    int y, mo, d, h, mi, n = 0;
    if (sscanf(q, "%d-%d-%d %d:%d%n", &y, &mo, &d, &h, &mi, &n) == 5 && !q[n]) {
    } else if (sscanf(q, "%d-%d-%d%n", &y, &mo, &d, &n) == 3 && !q[n]) {
        h = *time / 3600, mi = *time / 60 % 60;
    } else if (sscanf(q, "%d:%d%n", &h, &mi, &n) == 2 && !q[n]) {
        y = *date / 10000, mo = *date / 100 % 100, d = *date % 100;
    } else return 0;
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
        return 0;
    int nd = date_add(y * 10000 + mo * 100 + d, 0);
    if (nd != y * 10000 + mo * 100 + d) return 0; /* e.g. Feb 30 */
    *date = nd;
    *time = h * 3600 + mi * 60;
    return 1;
}

/* Modal text entry for the planner's date and time. */
static void prompt_datetime(App *a) {
    char q[32] = "", err[64] = "";
    int ql = 0;
    for (;;) {
        render(a);
        int y, x;
        popup(7, 52, "Leave after", &y, &x);
        txt(y + 1, x + 2, 48, A_NORMAL, "Date/time: %s_", q);
        txt(y + 2, x + 2, 48, at_dim, "e.g. 2026-10-15 08:30, 2026-10-15, or 17:45");
        txt(y + 3, x + 2, 48, at_dim, "Enter applies, Esc cancels, empty = now");
        txt(y + 4, x + 2, 48, at_warn, "%s", err);
        refresh();
        int ch = getch();
        if (ch == ERR || ch == KEY_RESIZE) continue;
        if (ch == KEY_ESC) return;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (ql == 0) {
                a->plan_live = 1;
                a->plan_sel = a->plan_top = 0;
                return;
            }
            int d = a->pdate, t = a->ptime;
            if (parse_datetime(q, &d, &t)) {
                a->plan_live = 0;
                a->pdate = d;
                a->ptime = t;
                a->plan_sel = a->plan_top = 0;
                return;
            }
            snprintf(err, sizeof err, "Not a valid date/time");
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (ql > 0) q[--ql] = 0;
        } else if (ch == CTRL('u')) {
            q[ql = 0] = 0;
        } else if (((ch >= '0' && ch <= '9') || ch == '-' || ch == ':' || ch == ' ') && ql < (int)sizeof q - 1) {
            q[ql++] = (char)ch;
            q[ql] = 0;
            err[0] = 0;
        }
    }
}

static void *reload_worker(void *arg) {
    ReloadJob *j = arg;
    j->rc = j->fn(j->ctx, &j->ng, j->note, sizeof j->note);
    atomic_store(&j->done, 1);
    return NULL;
}

static void start_reload(App *a) {
    if (a->job) {
        set_msg(a, "Already reloading...");
        return;
    }
    if (!a->reload) {
        set_msg(a, "Reload failed: not supported");
        return;
    }
    ReloadJob *j = xrealloc(NULL, sizeof *j);
    memset(j, 0, sizeof *j);
    j->fn = a->reload;
    j->ctx = a->ctx;
    if (pthread_create(&a->job_th, NULL, reload_worker, j) != 0) {
        free(j);
        set_msg(a, "Reload failed: cannot start thread");
        return;
    }
    a->job = j;
}

/* Called every loop turn: swaps in the new schedule once the worker is done. */
static void poll_reload(App *a) {
    ReloadJob *j = a->job;
    if (!j) return;
    a->spin++;
    if (!atomic_load(&j->done)) return;
    pthread_join(a->job_th, NULL);
    a->job = NULL;
    if (j->rc == 0) {
        gtfs_free(*a->gp);
        *a->gp = j->ng;
        build_vis(a);
        a->sel = a->st_top = 0;
        a->from = a->to = -1;
        if (a->mode == M_DETAIL) a->mode = a->prev_mode;
        reset_lists(a);
    }
    if (j->rc == 0) set_msg(a, "%s", j->note);
    else set_msg(a, "Reload failed: %s", j->note);
    free(j);
}

static void step(int *sel, int n, int delta) {
    *sel += delta;
    if (*sel < 0) *sel = 0;
    if (*sel > n - 1) *sel = n > 0 ? n - 1 : 0;
}

/* Applies a movement key to whichever list is active. Returns 1 if handled. */
static int list_key(App *a, int ch) {
    int *sel, n, page = a->page > 1 ? a->page - 1 : 1;
    int board = a->mode == M_BOARD;
    if (a->mode == M_DETAIL) {
        sel = &a->d_top;
        n = (*a->gp)->trips[a->dtrip].count;
    } else if (board && a->focus == 0) {
        sel = &a->sel;
        n = a->nvis;
    } else if (board) {
        sel = &a->dep_sel;
        n = a->ndeps;
    } else {
        sel = &a->plan_sel;
        n = a->njr;
    }
    int before = *sel;
    switch (ch) {
    case KEY_UP: case 'k': step(sel, n, -1); break;
    case KEY_DOWN: case 'j': step(sel, n, 1); break;
    case KEY_PPAGE: step(sel, n, -page); break;
    case KEY_NPAGE: step(sel, n, page); break;
    case KEY_HOME: case 'g': *sel = 0; break;
    case KEY_END: case 'G': *sel = n > 0 ? n - 1 : 0; break;
    default: return 0;
    }
    if (board && a->focus == 0 && *sel != before) a->dep_sel = a->dep_top = 0;
    return 1;
}

/* Movement in the schedule table: h/l pick a train, j/k a station. */
static int table_key(App *a, int ch) {
    int page = a->tpage > 1 ? a->tpage - 1 : 1;
    switch (ch) {
    case KEY_LEFT: case 'h': step(&a->tc_sel, a->ntcols, -1); return 1;
    case KEY_RIGHT: case 'l': step(&a->tc_sel, a->ntcols, 1); return 1;
    case ',': step(&a->tc_sel, a->ntcols, -page); return 1;
    case '.': step(&a->tc_sel, a->ntcols, page); return 1;
    case '\n': case '\r': case KEY_ENTER:
        if (a->ntcols > 0 && a->nvis > 0) open_detail(a, a->tcols[a->tc_sel], a->vis[a->sel], -1);
        return 1;
    case 'v': a->list_view = 1; return 1;
    default: return 0;
    }
}

/* Returns 1 to quit. */
static int handle_key(App *a, int ch) {
    if (ch == ERR || ch == KEY_RESIZE) return 0;
    if (a->help) {
        a->help = 0;
        return 0;
    }
    const Gtfs *g = *a->gp;
    switch (ch) {
    case 'q': case 'Q': return 1;
    case '?': a->help = 1; return 0;
    case '1': a->mode = M_BOARD; return 0;
    case '2': a->mode = M_PLAN; return 0;
    case '\t':
        a->mode = a->mode == M_PLAN ? M_BOARD : M_PLAN;
        return 0;
    case '[': case ']': case 't':
        if (a->mode == M_PLAN) {
            if (ch == 't') a->plan_live = 1, a->plan_sel = a->plan_top = 0;
            else plan_shift(a, ch == ']' ? 1 : -1, 0);
        } else if (a->mode == M_BOARD) {
            a->date = ch == 't' ? date_today() : date_add(a->date, ch == ']' ? 1 : -1);
            reset_lists(a);
        }
        return 0;
    case 'r': start_reload(a); return 0;
    default: break;
    }
    if (a->mode == M_BOARD && !a->list_view && table_key(a, ch)) return 0;
    if (list_key(a, ch)) return 0;

    int enter = ch == '\n' || ch == '\r' || ch == KEY_ENTER;
    if (a->mode == M_DETAIL) {
        if (ch == KEY_ESC || ch == KEY_BACKSPACE || ch == 127 || ch == 'h' || ch == KEY_LEFT || enter)
            a->mode = a->prev_mode;
    } else if (a->mode == M_BOARD) {
        if (ch == 'v') a->list_view = 0;
        else if (ch == 'h' || ch == KEY_LEFT) a->focus = 0;
        else if (ch == 'l' || ch == KEY_RIGHT) a->focus = 1;
        else if (ch == 'n') a->dirf = 0, a->dep_sel = a->dep_top = 0, a->tdirty = 1;
        else if (ch == 's') a->dirf = 1, a->dep_sel = a->dep_top = 0, a->tdirty = 1;
        else if (ch == 'a') a->dirf = DIR_ANY, a->dep_sel = a->dep_top = 0, a->tdirty = 1;
        else if (ch == '/') {
            int s = pick_station(a, "Go to station", a->nvis ? a->vis[a->sel] : -1);
            for (int i = 0; s >= 0 && i < a->nvis; i++)
                if (a->vis[i] == s) {
                    a->sel = i;
                    a->focus = 1;
                    a->dep_sel = a->dep_top = 0;
                }
        } else if (enter) {
            if (a->focus == 0) a->focus = 1;
            else if (a->ndeps > 0) {
                const Departure *d = &a->deps[a->dep_sel];
                open_detail(a, d->trip, a->vis[a->sel], -1);
            }
        }
    } else if (a->mode == M_PLAN) {
        if (ch == 'o' || ch == 'd') {
            int cur = ch == 'o' ? a->from : a->to;
            int s = pick_station(a, ch == 'o' ? "Origin" : "Destination", cur);
            if (s >= 0) *(ch == 'o' ? &a->from : &a->to) = s;
            a->plan_sel = a->plan_top = 0;
        } else if (ch == 'T') {
            prompt_datetime(a);
        } else if (ch == ',' || ch == '.') {
            plan_shift(a, 0, ch == '.' ? 30 : -30);
        } else if (ch == 'x') {
            int tmp = a->from;
            a->from = a->to;
            a->to = tmp;
            a->plan_sel = a->plan_top = 0;
        } else if (enter && a->njr > 0) {
            const Journey *j = &a->jrs[a->plan_sel];
            open_detail(a, j->dep.trip, a->from, a->to);
        }
    }
    (void)g;
    return 0;
}

int ui_run(Gtfs **g, const char *note, UiReloadFn reload, void *ctx) {
    setlocale(LC_ALL, "");
    if (!initscr()) return 1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
#ifdef NCURSES_VERSION
    set_escdelay(25);
#endif
    timeout(1000); /* wake once a second so the clock and "In" column tick */
    init_attrs();

    App a;
    memset(&a, 0, sizeof a);
    a.gp = g;
    a.reload = reload;
    a.ctx = ctx;
    a.mode = M_BOARD;
    a.date = date_today();
    a.dirf = DIR_ANY;
    a.from = a.to = -1;
    a.plan_live = 1;
    a.tdirty = 1;
    a.deps = xrealloc(NULL, MAXLIST * sizeof *a.deps);
    a.jrs = xrealloc(NULL, MAXLIST * sizeof *a.jrs);
    build_vis(&a);
    if (note && *note) set_msg(&a, "%s", note);

    for (;;) {
        poll_reload(&a);
        render(&a);
        refresh();
        timeout(a.job ? 100 : 1000);
        if (handle_key(&a, getch())) break;
    }

    endwin();
    free(a.deps);
    free(a.jrs);
    free(a.vis);
    free(a.tcols);
    return 0; /* a reload still running is abandoned; exiting ends it */
}
