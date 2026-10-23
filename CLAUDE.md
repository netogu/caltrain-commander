# Caltrain Commander

Interactive terminal app for Caltrain's schedule: C11 + ncurses, built with CMake. Loads the public GTFS feed (downloaded and cached, or a local zip/dir) and offers a departures board, a direct-trip planner, and a per-train stop list. See `README.md` for user-facing usage and keys.

## Build and test
```
cmake -S . -B build -G Ninja && cmake --build build   # must stay warning-free (-Wall -Wextra)
ctest --test-dir build                                # runs test_gtfs against tests/fixture
./build/caltrain --summary                            # load data, print counts, no UI
```
For memory checks, compile with `-fsanitize=address,undefined` (`cc -std=c11 -g -fsanitize=address,undefined -Isrc src/*.c -lncurses`) and run both `test_gtfs tests/fixture` and the UI.

## Layout
- `src/csv.c` RFC-4180 reader; columns are looked up by header name, never by position.
- `src/gtfs.c` model, loader, service-day rules, `gtfs_departures` / `gtfs_journeys`. No UI dependencies (the `gtfs` static lib), so it is testable alone.
- `src/config.c` saved Next-tab origin/destination (station ids) in `$XDG_CONFIG_HOME/caltrain-commander/config`.
- `src/fetch.c` cache + `curl`/`unzip` via `fork/execvp` (argv arrays, never a shell string).
- `src/ui.c` all ncurses code. `src/main.c` args and wiring. `src/util.h` alloc helpers and the `PUSH` growable-array macro.
- `tests/test_gtfs.c` + `tests/fixture/` a tiny hand-written feed.

## Decisions (don't relitigate)
- C + ncurses chosen over notcurses/termbox2/libtickit/S-Lang after a trade study: no extra dependency, most portable. CMake is the build system.
- Download/extract shells out to `curl` and `unzip` rather than linking libcurl/libzip, to keep the build to ncurses only.
- POSIX only (fork, `localtime_r`); Windows is out of scope.

## GTFS facts about the Caltrain feed
- Feed: `https://data.trilliumtransit.com/gtfs/caltrain-ca-us/caltrain-ca-us.zip` (~170 KB, no API key).
- Stops come as parent stations (`location_type=1`) with NB/SB platform children; the model folds platforms into stations. Shuttle "stations" (San Jose Shuttle, Tamien Caltrain Shuttle) and Stanford have no trains; the UI hides stations with zero stop times.
- `direction_id`: 0 = northbound, 1 = southbound. Route short names are "Local Weekday", "Local Weekend", "Limited", "Express", "South County"; the UI strips the Weekday/Weekend suffix.
- Times can exceed 24:00:00 (shown with a `+1` suffix). `gtfs_departures` also scans the previous service day for trips whose stop time is >= 86400.
- `calendar_dates` can add a service that has no `calendar` row, so services are created on demand (`service_ref`).
- Station names have " Caltrain Station" / " Station" / " Caltrain" stripped; stations sort north to south by latitude.

## Gotchas
- ncurses defines macros that collide with common names (`move`, `overlay`, `box`, ...). Use distinct names for static helpers in `ui.c` (`step`, `popup`, `box_at`).
- BSD `sed` on macOS lacks `\b`; use `perl -pi -e` for word-boundary renames.
- The stop-id map owns its (duplicated) keys; the other maps in `gtfs.c` borrow keys from the `Gtfs` arrays. `service_ref` can realloc `g->services`, so re-fetch pointers after calling it.
- Times display in Pacific: `main.c` sets `TZ=America/Los_Angeles` if unset.
- The cache lives in `$XDG_CACHE_HOME/caltrain-commander` (default `~/.cache/caltrain-commander`); delete it to force a clean download test.

- `gtfs_journeys` truncates to `max` departures from the origin *before* filtering to the destination, so pass a large `max` and take the head yourself (see Next tab in `refresh_lists`).
- The GTFS feed is static: there is no delay data. The Next tab shows `Delay: n/a`; real delays need GTFS-realtime (511.org, API key).

## Verifying UI changes
Drive the real app in tmux and read the screen: `tmux new-session -d -s ct -x 110 -y 30 "LANG=en_US.UTF-8 TERM=xterm-256color ./build/caltrain"`, then `tmux send-keys` and `tmux capture-pane -p`. Use `tmux -u` or the line-drawing characters appear as `lqqk` letters in captures. Minimum terminal size is 64x12.
