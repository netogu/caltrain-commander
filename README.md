# Caltrain Commander

An interactive terminal app for Caltrain's schedule, written in C11 with ncurses.
It loads the agency's public GTFS feed (downloaded and cached, or read from a
local file) and lets you browse departures, plan direct trips, and inspect every
stop of a train.

## Build

Requires a C compiler, CMake ≥ 3.16, and ncurses. Downloading needs `curl` and
`unzip` on your `PATH`.

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build      # unit tests on a small fixture feed
    ./build/caltrain

## Usage

    caltrain [-f PATH] [-u URL] [--refresh] [--summary]

| Option | Meaning |
|---|---|
| `-f PATH` | Read a GTFS `.zip` or an extracted directory instead of downloading |
| `-u URL` | Download from another feed URL |
| `--refresh` | Re-download even if the cache is under 7 days old |
| `--summary` | Print what was loaded and exit (no UI) |

The download is cached in `$XDG_CACHE_HOME/caltrain-commander` (default
`~/.cache/caltrain-commander`). If a refresh fails, the cached copy is used and
the status bar says so. Times are shown in Pacific time.

## Keys

| Key | Action |
|---|---|
| `1` / `2` / `Tab` | Departures board / Trip planner |
| `j` `k` / arrows, `PgUp` `PgDn`, `g` `G` | Move, page, first / last |
| `h` `l` | Board table: previous / next train (`,` `.` page). List view: switch pane |
| `v` | Board: toggle schedule table (stations × trains) / departures list |
| `Enter` | Show every stop of the selected train; `Esc` goes back |
| `/` | Board: find a station (type to filter, `sj` finds San Jose Diridon) |
| `n` `s` `a` | Board: northbound / southbound / all |
| `o` `d` `x` | Planner: pick origin / destination / swap |
| `[` `]` `t` | Previous day / next day / today (board and planner each keep their own) |
| `T` `,` `.` | Planner: type a date/time, or shift it by −/+30 minutes (`t` returns to now) |
| `r` | Re-download the schedule |
| `?` / `q` | Help / quit |

A time like `00:05+1` means the train runs after midnight on that service day.

## Layout

- `src/csv.c` — RFC-4180 reader (quoted fields, CRLF, BOM, lookup by header name)
- `src/gtfs.c` — schedule model, service-day rules (`calendar` + `calendar_dates`), departure and A→B queries
- `src/fetch.c` — cache handling; runs `curl` / `unzip` without a shell
- `src/ui.c` — ncurses views
- `tests/` — fixture feed and checks for quoting, service exceptions, and after-midnight trains
