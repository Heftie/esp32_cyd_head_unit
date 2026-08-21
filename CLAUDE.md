# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP-IDF (native, not Arduino) firmware for the "Cheap Yellow Display" board
(ESP32-2432S028R). It drives the onboard TFT + resistive touch via
`esp_lcd`/`lvgl`/`esp_lvgl_port`, and acts as the display/UI end of a
UART-based telemetry link to a companion MCU. See `README.md` for the board
overview and `ESP32-CYD-Pinout.md` for the full pin map — the latter is the
authoritative reference for what each GPIO does and which ones are free;
consult it before adding anything that touches a new pin, and update it if
you change `main/hardware.h`'s pin assignments.

## Build / flash / monitor

This is a standard ESP-IDF project — use `idf.py` from an ESP-IDF v6.x
environment (` . '/home/heftie/.espressif/tools/activate_idf_v6.0.2.sh'` or the VS Code IDF extension; this repo
targets `esp32` per `sdkconfig.defaults`).

```sh
idf.py build
idf.py -p <PORT> flash monitor
idf.py menuconfig        # project/component Kconfig options
idf.py fullclean         # if switching targets or after deep CMake changes
```

There are no host-side unit tests in this repo — verification is by
building and flashing to hardware.

### Board variant

Two physical display drivers exist for this board. Default build targets the
newer ST7789 boards. For the older ILI9341-only boards (micro-USB only, no
USB-C), uncomment the `CYD_ILI9341` define in the top-level `CMakeLists.txt`
before building. This flag flows into `components/lcd` (driver selection)
and `main/hardware.h` (`LCD_MIRROR_X`/`LCD_MIRROR_Y`).

## Architecture

`main/main.c` is the app entry point: it brings up the LCD/touch panel,
then `data_hub` (the one hard prerequisite `ui_init()` has —
`data_hub_list_channels()` takes a mutex that's NULL before
`data_hub_init()` runs), then `ui_init()` itself — deliberately before
`uart_link`, `rgb_led`, `sd_storage`/`logger`, and `web_server`, so the
screen is up and showing something before any of those, several of
which are slow or can fail outright (UART's `*IDN?` handshake waits up
to 500ms, `web_server`'s WiFi connect can take up to 20s before falling
back to its setup AP). Every screen reads those subsystems only through
its own refresh timer, via plain static-default reads
(`uart_link_mcu_present()`/`sd_storage_is_mounted()`/
`logger_is_running()` all default false, `web_server_get_wall_clock()`
returns false until synced) that are safe to see before that
subsystem's own `_init()` has run — so a screen just shows "not there
yet" (or, on `tiles`, a "SD card not mounted" banner) until each one
catches up, rather than the whole UI waiting behind whichever is
slowest. `main.c` then finishes bringing up the rest and settles into a
loop that cycles the status LED and periodically dumps `data_hub` to
the log. It owns hardware bring-up only — no screen, LVGL widget, or
navigation logic lives in `main.c` itself.

All screen/LVGL-UI code lives in `components/ui`, which `main.c` reaches
only through `ui_init()` (`include/ui.h`). Screens switch through a small
name-based registry (`screen_nav.c`, private to the component) rather than
one screen calling another's constructor directly: `screen_register(name,
create_fn)` in `ui_init()`, then `screen_push(name)` to enter a screen
(remembering where you came from) and `screen_pop()` to return to it,
falling back to `SCREEN_HOME` ("tiles") if the back stack is empty — this
is how e.g. `sd_info_screen`'s Back button can just call `screen_pop()`
and correctly land back on `settings` without knowing or caring that
that's where it was entered from. Each screen is its own file
(`tiles_screen.c`/`settings_screen.c`/`time_settings_screen.c`/
`wifi_settings_screen.c`/`measurement_screen.c`/`set_time_screen.c`/
`sd_info_screen.c`/`log_manager_screen.c`/`graph_screen.c` (which holds
both the **graph** and **graph_config** screens)/`timezone_screen.c`,
talking to the others only by name through `screen_nav.h` — the
exceptions are `measurement_screen_set_channel()` and
`graph_screen_set_channel()`, small setters `tiles_screen.c` calls before
`screen_push("measurement")`/`screen_push("graph")` so the single
measurement/graph screen instance knows which channel to show, since
`screen_push()` itself only takes a name.

- **tiles** (default/home) — a DD/MM/YYYY HH:MM:SS clock in the
  configured local timezone (from `web_server_get_wall_clock()` converted
  via `localtime_r()`, with a `%Z` zone-abbreviation suffix, e.g. "CEST";
  "Time not set" until NTP or a manual set lands), a Start log/Stop log
  toggle, a "SD card not mounted" banner
  (only visible when `sd_storage_is_mounted()` is false — hidden objects
  take no space in a flex column, so it doesn't reserve a blank row while
  the card's fine), over a data-driven dashboard, one tile per `data_hub`
  channel, created on first sight and refreshed on an LVGL timer; nothing
  about the channel set is hardcoded. Starting from here always names a
  fresh file after the current wall clock
  (`YYYYMMDD_HHMMSS_log.csv`, via `log_naming_default_filename()` +
  `logger_start(name)`) rather than
  resuming whatever was last active — this is the "just start logging"
  quick action, so each press begins its own dated session; falls back
  to `logger_start(NULL)` (keep whatever file is already set) only if
  wall clock isn't available yet. Tapping a tile opens **measurement**
  for that channel; long-pressing it opens **graph** for that channel
  instead; its Settings button pushes **settings**.
- **settings** — a flat menu, four buttons and nothing else: **time_settings**,
  **wifi_settings**, **log_manager**, and **sd_info**. Each of those used to be
  reached through a different, less consistent path (Settings' own status
  text and buttons for time/WiFi, a "Manage logs" button buried inside
  **sd_info**) — flattening them into one menu here means every settings
  destination is now exactly one tap from **settings**, and each
  sub-screen owns only the state and refresh timer its own job actually
  needs instead of sharing one screen's worth of both.
- **time_settings** — current time source (`web_server_get_status()`'s
  `time_source`, polled on its own LVGL timer, shown as "Source: NTP" /
  "manual" / "not set"), and buttons into **set_time** and **timezone**,
  plus a "Sync NTP" button (`web_server_sync_ntp_now()`) that forces an
  immediate resync attempt — the way back to `WEB_SERVER_TIME_NTP` after a
  manual set, once the network can reach an NTP server again. Sync NTP is
  greyed out (non-clickable) outside STA mode, same as before — it's a
  no-op with no network path to an NTP server anyway. Reachable only from
  **settings**.
- **wifi_settings** — WiFi status (mode/SSID/IP, from
  `web_server_get_status()`, polled on its own LVGL timer) and a "Forget
  network" action (`web_server_forget_wifi()`, which erases the stored
  credentials and reboots) gated behind a tap-to-arm/tap-to-confirm
  sequence since it's a one-way trip off the current network — also
  greyed out outside STA mode, since there's nothing to forget while
  running the setup AP. Reachable only from **settings**.
- **measurement** — multimeter-style detail view for one channel: current
  value at large scale, plus running MIN/MAX/AVG since a Start press and a
  Reset. The MIN/MAX/AVG accumulator lives entirely in this screen, not in
  `data_hub` — `data_hub`'s own ring buffer is only
  `DATA_HUB_HISTORY_LEN` (32) samples deep, meant to bridge to `logger`'s
  next flush, not hold a real run's worth of history, and it keeps
  overwriting itself regardless of this screen's Start/Reset state.
- **set_time** — manual UTC wall-clock entry via five `lv_roller`s
  (year/month/day/hour/minute) and a Set button, feeding
  `web_server_set_wall_clock()`. Exists because this board has no
  battery-backed RTC crystal: `web_server`'s wall clock normally comes
  only from SNTP and resets to unset every boot, and a network with no
  path to an NTP server otherwise leaves `logger` unable to write
  anything at all — see its entry below — with no way to recover.
  Converts the roller selections to a UTC epoch
  via a small `days_from_civil()` helper (Howard Hinnant's constant-time
  civil-calendar algorithm) rather than libc's `timegm()`/`mktime()`, to
  not depend on a timezone-aware libc build for what this screen's input
  treats as UTC regardless of the configured display timezone — entering
  a manual time here is always "the UTC time is X," converted the same
  way no matter what **timezone** is set to. Reachable only from
  **time_settings**.
- **timezone** — a single `lv_roller` over `web_server_timezones[]`
  (a curated, fixed list of POSIX TZ rule strings — not free text, since
  this board's only text input is the on-screen keyboard used elsewhere
  for filenames) and an Apply button that calls `web_server_set_timezone()`.
  Changes only how times are *displayed* — the tiles clock's `%Z`-suffixed
  local time and any future web-side conversion — never what's stored:
  `logger`'s CSV timestamps, `web_server_get_wall_clock()`'s return value,
  and **set_time**'s manual entry all stay UTC regardless, so log files
  stay unambiguous across a timezone change. The
  selection persists across reboots via NVS (namespace `tz_cfg`), applied
  by `web_server_init()` before Wi-Fi even comes up — unlike the wall
  clock itself, the TZ rule doesn't depend on a network, so there's no
  reason the on-device clock should show UTC while waiting for one.
  Defaults to index 0, Europe/Berlin (`CET-1CEST,M3.5.0,M10.5.0/3`),
  since this firmware's primary deployment is in Germany. Reachable only
  from **time_settings**.
- **sd_info** — `sd_storage_is_mounted()` / `sd_storage_get_info()` read
  out as mounted/not, card type, capacity, used/free space, polled on its
  own LVGL timer; reachable only from **settings**. No longer the path to
  **log_manager** — that moved to a direct button on **settings** itself,
  since managing logs isn't really about card info and didn't need to sit
  behind it.
- **log_manager** — on-device log management, now with a near-identical
  counterpart on the web dashboard's own Logs card (see the `web_server`
  component entry below) rather than being the only place any of this is
  possible: a
  name field (`lv_textarea` + on-screen `lv_keyboard`, shown/hidden on
  textarea focus/defocus so it doesn't have to permanently share the
  screen with the file list), a "Default" button that fills that field
  with `log_naming_default_filename()`'s `YYYYMMDD_HHMMSS_log.csv` (the
  same pattern **tiles**' Start button has always used, factored out into
  `components/ui/log_naming.c` so neither screen — nor the web
  dashboard's own Default-name button — hand-rolls its own copy of the
  same `snprintf`; a silent no-op if wall clock isn't available yet,
  same as **time_settings**' Sync NTP with no network to reach), and
  "Use" button that calls
  `logger_start(name)` to switch which file `logger` is appending to; a
  scrollable list of files on the card (`sd_storage_list_dir()`, refreshed
  every 2s and sorted newest-first by `mtime` — a fixed order, unlike the
  web dashboard's sortable Logs card, since this list is short enough on
  a 240x320 screen that newest-first is just always the useful one),
  each with a size and either a Delete button
  (tap-to-arm/confirm) or an "active" tag if it's the file `logger` is
  currently writing — the active file is never offered for deletion; and
  a "Clear SD card" button (tap-to-arm/confirm) that calls `logger_stop()`
  before `sd_storage_format()`, specifically so `logger_is_running()`
  correctly reflects reality afterward instead of silently recreating a
  headerless file on the next scheduled flush. Reachable only from
  **settings**.
- **graph** / **graph_config** — a live line chart (`lv_chart`,
  `LV_CHART_UPDATE_MODE_SHIFT`) for one channel, opened by long-pressing
  its tile. Sampling period (tick) and time span are both user-selectable
  in **graph_config** (reached via a "Cfg" button, three `lv_roller`s:
  channel/tick/span) and persist across re-entries as file-scope statics;
  changing them and hitting Apply rebuilds **graph** from scratch, which
  resets the accumulated chart data by design. Point count is capped at
  `GRAPH_MAX_POINTS` (240, roughly the panel's pixel width) since this
  board has no PSRAM — `graph_compute_effective()` is the single place
  that clamp math lives (`points = span/tick`, clamped, then
  `effective_span = points * tick`), reused by both the chart view's
  range label and the config screen's live readout, so the displayed span
  never silently disagrees with what's actually being sampled. The sample
  timer is the one screen-timer in this codebase torn down and recreated
  on every `graph_screen_create()` call rather than created once and left
  running, since its period depends on the config that can change between
  visits.

All eleven screens reuse the same `lv_scr_act()` object — `lv_obj_clean()`
only removes children, not styles/layout applied directly to the screen,
so each screen-creation function must reset anything it doesn't want
leaking in from whichever screen ran before it. Every container in a
screen's layout gets an explicit width/height (or `flex_grow`) — an
earlier draft of the measurement screen left one container unsized and let
a long label wrap onto extra lines, which silently pushed content off the
bottom of the display; screens also explicitly clear
`LV_OBJ_FLAG_SCROLLABLE` where a layout bug should be visible clipping
rather than an invisible scroll. Add a new screen by writing its create
function in a new file and calling `screen_register()` for it in
`ui_init()` — no other call site needs to change unless something should
navigate to it.

All hardware/pin configuration is centralized in `main/hardware.h` as
`#define`s consumed by the `*_config_t` structs passed into each
component's `_init()`. Components themselves are hardware-abstracted and
take no compile-time pin dependency — they only know about the config
struct fields. When adding a peripheral, add its pins to `hardware.h`
rather than hardcoding GPIOs in the component.

### Components (`components/`)

- **lcd** — brings up the SPI panel (ILI9341 or ST7789, selected by
  `CYD_ILI9341`) and registers it with `esp_lvgl_port` as an LVGL display.
  Also owns backlight brightness (LEDC-driven) and rotation.
- **touch** — brings up the XPT2046 touch controller *bit-banged over plain
  GPIO* (`xpt2046_bitbang_io.c`), not a hardware SPI peripheral. It supplies
  a fake `esp_lcd_panel_io_t` whose `rx_param` speaks the XPT2046's
  register protocol by toggling GPIOs directly; the vendored
  `esp_lcd_touch_xpt2046` driver runs completely unmodified on top of it,
  since that driver only ever calls through `rx_param`. This exists so
  SPI3 (VSPI) — touch's pins on stock CYD wiring — is entirely free for
  `sd_storage` to own exclusively at full hardware clock, with no bus
  arbitration between the two ever needed. Don't "optimize" touch back onto
  real SPI without re-reading this — it would reintroduce exactly the
  bus-sharing problem this design sidesteps.
- **data_hub** — an in-memory pub/sub-style ring buffer keyed by channel
  name (e.g. `"VOLT:DC"`), storing recent samples with timestamps.
  `data_hub_publish()` is called only by `uart_link`; UI/web code reads via
  `data_hub_get_latest()` / `data_hub_get_history()` /
  `data_hub_list_channels()`. Not persisted on its own — resets on reboot;
  `logger` is what makes any of it durable. `DATA_HUB_HISTORY_LEN` is
  deliberately small (32, not a "real" history depth) — it only needs to
  bridge the gap to `logger`'s next flush, and it's the single largest
  static RAM allocation in the firmware, so don't casually grow it back up
  without checking the link still fits (see the DRAM budget note below).
- **uart_link** — talks a small SCPI-style text protocol to a companion MCU
  over UART2: newline-terminated commands/replies. Handles a `*IDN?`
  handshake on init, answers some queries locally (see
  `s_local_commands` in `uart_link.c`), parses push lines shaped like
  `MEAS:VOLT:DC 12.84,V` (name, value, unit) into `data_hub_publish()`
  calls, and exposes `uart_link_query()` for synchronous request/reply
  (single query in flight at a time, mutex-serialized).
- **rgb_led** — drives the onboard 3-channel LED via LEDC PWM,
  0–255 per channel; channels set to `GPIO_NUM_NC` in config are skipped.
  `main.c` runs each channel individually once at boot as a wiring
  self-test, then hands the LED a permanent job: green once
  `uart_link_mcu_present()` and `sd_storage_is_mounted()` are both true,
  yellow if only one is, red if neither is. WiFi status isn't folded in —
  it's already visible on **wifi_settings**, and a 3-color LED runs out of
  clean states fast past two independent yes/no signals.
- **sd_storage** — FAT-over-SDSPI wrapper around the microSD slot (path
  read/write/erase/format, rename, delete, directory listing
  (`sd_storage_list_dir()`, flat — skips subdirectories, since the card is
  used flat — name, size, and `mtime` per entry), mount/unmount,
  capacity/free-space info). `mtime` comes from `stat()`'s `st_mtime`,
  which FATFS populates via upstream ESP-IDF's `get_fattime()`
  (`components/fatfs/diskio/diskio.c`, not something this repo owns) —
  that reads libc's `time()`, so it's only meaningful once something has
  actually called `settimeofday()` this boot (SNTP does so internally;
  `web_server_set_wall_clock()` now does too, specifically so a
  manual-time-only session — no NTP at all — still gets real file
  timestamps instead of everything reading back near the Unix epoch).
  FAT timestamps also carry no timezone of their own — they're whatever
  `localtime_r()` under the currently-active TZ said at write time — so a
  file written before a **timezone** change can read back `mtime` a fixed
  offset off from reality after one; a known FAT limitation, not
  something this component works around. Owns SPI3 (VSPI)
  exclusively and permanently once mounted — see the `touch` entry above
  for why that bus is free for it. `bus_already_initialized` still exists
  in the config struct for a caller that's already brought up the target
  SPI host itself, but nothing in this repo uses that path today.
- **logger** — polls `data_hub` on a timer (default 5s) and appends any
  samples not yet written to a CSV on the SD card via `sd_storage`, to
  whichever file is current (`logger_get_current_path()`; `log.csv` by
  default), as rows shaped `timestamp_epoch_us,channel,value,unit`.
  That timestamp is real UTC microseconds since 1970, not the
  `esp_timer_get_time()`-based boot-relative count `data_hub`'s samples
  actually carry — `logger_config_t.convert_boot_time_fn` (wired by
  `main.c` to `web_server_convert_boot_time_us()`, a plain function
  pointer rather than a `logger`→`web_server` component dependency,
  since `web_server` already depends on `logger` for
  `logger_get_current_path()`) converts each sample at write time using
  whatever NTP/manual offset `web_server` currently has. Until that
  offset exists — no SNTP sync yet and no manual set — flushes are a
  no-op: nothing is written, and nothing beyond `data_hub`'s own normal
  `DATA_HUB_HISTORY_LEN`-deep eviction is lost, so logging just silently
  starts working the moment a wall clock lands rather than ever writing
  a meaningless "seconds since boot" row. (An older revision wrote that
  boot-relative count directly and left the UTC conversion to read time,
  in `web_server`'s now-removed `/api/history` handler — abandoned
  because a raw boot-relative number in the file is useless without also
  knowing when that boot happened, which the file itself never recorded.)
  `logger_start(name)` switches the active file: renaming
  (`sd_storage_rename()`) if it differs from the current one, writing a
  fresh header if the target doesn't already exist, and resetting every
  channel's "already logged" bookmark; `logger_stop()`/`logger_is_running()`
  gate the polling task so `components/ui`'s Start/Stop log toggle (tiles
  screen) and the log-manager screen's rename/switch can control it
  without reaching into `data_hub` or SD state themselves. `logger_init()`
  leaves logging idle (`s_running` stays false) until one of those
  callers explicitly starts it — nothing is written just because the
  card mounted. Once the current file crosses `max_file_bytes` (default
  5 MB), it rotates the same way: current file becomes `<name>.1`
  (overwriting any older backup), fresh file starts from just the header
  row — single generation, not a full logrotate scheme, since this is a
  hobby-scale device; rotation's bookmark reset can duplicate up to one
  flush's worth of rows across the seam, an accepted trade for staying
  simple. Runs from its own task rather than writing from inside
  `data_hub_publish()`, specifically so a slow SD write (single-digit
  ms, but still) never blocks `uart_link`'s RX task and risks dropping
  UART bytes.
- **web_server** — brings up WiFi, SNTP, and mDNS from a task pinned to
  core 1 (the WiFi driver's own task defaults to core 0), then starts
  `esp_http_server`. WiFi credentials are never hardcoded or built into
  the firmware: `wifi_provision.c` loads them from NVS at boot and tries
  STA with a bounded wait (`WIFI_CONNECT_TIMEOUT_MS`); no stored
  credentials, or no connection within that window, falls back to an open
  SoftAP (`CYD-Setup-XXXX`) plus a hand-rolled captive-portal DNS responder
  (answers every query with the AP's own IP) and a setup page — submitting
  it writes to NVS via `wifi_provision_save()` and reboots, so the normal
  boot path picks the new credentials up. `esp_netif_create_default_wifi_ap()`
  must only ever be called once per boot (it asserts on a duplicate netif
  key) — it belongs solely to `wifi_provision_start_ap()`, not to
  `web_server.c`'s own bring-up, even speculatively.
  Once connected, serves an embedded dashboard at `/` — sent with
  `Cache-Control: no-store`, since this one HTML response is the entire
  app (CSS/JS inlined, no separate asset files) and a browser has no
  Last-Modified/ETag to revalidate against otherwise; without it, a tab
  or browser cache could keep serving an old version of the dashboard
  indefinitely across a firmware update with no way to tell just by
  looking at it — live channel values
  as JSON at `/api/data`, a directory listing of every log file on the
  card plus free/total space as JSON at `/api/logs`
  (`sd_storage_list_dir()` + `sd_storage_get_info()`), and any one log
  file at `/download?file=<name>` — `is_safe_filename()` rejects `/` and
  `..` in `file` before it ever reaches `sd_storage`, since it comes
  straight off the query string. Logging can also be driven entirely from
  the web now, matching what **log_manager** does on-device:
  `GET /api/log/status` (`{running, current_file}`), `POST
  /api/log/start?name=` (optional; omitted/empty just resumes the current
  file, same as **log_manager**'s Use button with an empty textarea),
  `POST /api/log/stop`, and `POST /api/log/delete?file=` — the last one
  enforcing the exact same rule `log_manager_screen.c` does: refuses a
  file that's both `logger_get_current_path()` and currently
  `logger_is_running()`, since erasing an actively-open log file is a
  footgun with no upside over just stopping first. `name`/`file` both go
  through `is_safe_filename()` the same as `/download` does.
  `start_httpd()`'s `routes[]` table is at 9 entries now — past
  `HTTPD_DEFAULT_CONFIG()`'s default `max_uri_handlers` of 8, which
  doesn't fail loudly: `httpd_register_uri_handler()` just returns an
  error for whichever route runs out of slots (`/download`, last in the
  array, until this was caught) while every route before it keeps
  working, so the server looks fine unless something specifically checks
  that call's return value or greps the boot log for "no slots left".
  `config.max_uri_handlers` is set to 16 explicitly now, with headroom —
  bump it again before it's ever tight, not after another route goes
  quietly missing. There's no
  web equivalent of **log_manager**'s "Clear SD card" — that one's kept
  on-device only, as the one action here destructive enough to be worth
  requiring physical access for. A "Default name" button next to the
  name field mirrors **log_manager**'s own — it fills the field with
  `YYYYMMDD_HHMMSS_log.csv` built from `GET /api/time`
  (`{synced, epoch}`, `web_server_get_wall_clock()` over HTTP) rather
  than this browser's own `Date()`, so the generated name reflects the
  device's wall clock rather than whatever the visiting browser's clock
  happens to read — the two are usually close but not guaranteed to be,
  especially right after a manual set_time entry with no NTP involved at
  all. `synced: false` (no NTP sync, no manual set yet) shows an alert
  instead of generating a name that wouldn't mean anything.
  The dashboard's "Logs" card fetches `/api/logs` and `/api/log/status`
  together on load, on a Refresh button, and after any
  start/stop/delete action (not polled every second like `/api/data`,
  since a directory listing changes rarely and each call is an SD read),
  and renders each file as a row — File/Size/Modified, a `/download?file=`
  link, and either an "active" tag (the file `logger` is currently
  writing) or a Delete button — this is now near feature parity with
  **log_manager**, just reachable from a browser instead of the
  touchscreen. The three data columns' headers are clickable
  (`logSortKey`/`logSortDir` in `index.html`'s script, an ascending/
  descending toggle with a `▲`/`▼` indicator) and re-sort the
  already-fetched `logFiles` array in place — sorting never triggers
  another `/api/logs` request, so flipping between, say, newest-first and
  by-name doesn't cost another SD directory read each time. `log_manager`
  itself has no equivalent sort — its file list is short enough on a
  240×320 screen that it hasn't needed one.
  The dashboard's "Live Graph" card
  is the web analog of **graph**/**graph_config** — a per-channel line
  chart drawn on a `<canvas>` with plain JS (no charting library) — but
  sourced entirely client-side: it's fed off the same `/api/data` poll
  the values above it already runs, appending each poll's samples to a
  per-channel in-memory array (capped at `GRAPH_MAX_POINTS`, 3600 points
  — an hour at the 1s poll rate, the widest span the picker below
  offers) that resets to empty on every page load. A span picker (1/5/15/
  30 min, 1h) filters that array down to just the newest N seconds at
  draw time rather than bounding what's collected — so widening the span
  doesn't need to wait and re-accumulate, since the buffer was already
  holding up to an hour regardless of which span happens to be selected.
  Nothing is sent to or stored on the device for any of this —
  deliberately, since the ESP32 has no spare static RAM for another
  history buffer (see the DRAM budget note), and the point is to graph
  exactly what this browser tab has observed during its own session, not
  to reconstruct history from the card (that's what `/download`ing a log
  file is for — there's no JSON date-range query for that anymore; an
  earlier `/api/history?date=` endpoint that reconstructed one day's rows
  from the current log file was removed as more machinery than the
  actual use case needed once raw CSV access already covered it).
  Channels populate the graph picker
  dynamically from whatever `/api/data` has reported so far, same
  "nothing hardcoded" approach as `data_hub`'s channel table.
  Since `data_hub`'s own timestamps are
  `esp_timer_get_time()` (boot-relative, not wall-clock), `web_server`
  computes a `boot_epoch_offset_us` once SNTP
  syncs and adds it to any stored timestamp to get a real date for
  `/api/data`'s `ts_epoch` field. This board has no battery-backed RTC
  crystal, so
  `boot_epoch_offset_us` is the *only* wall clock the firmware has, and it
  resets to unset on every boot; `web_server_set_wall_clock()` lets a
  caller (`components/ui`'s set_time screen) set it manually, the same
  way `sntp_sync_cb()` would, for when there's no network path to an NTP
  server at all — and, like an SNTP sync, also calls `settimeofday()`,
  so libc's own `time()` (which this app's own offset math never
  touches) agrees too; see the `sd_storage` entry above for why that
  matters even though nothing here reads `time()` directly itself.
  `web_server_convert_boot_time_us()` exposes the same
  offset math as a plain function so `logger` can convert its own
  boot-relative sample timestamps into real UTC before writing them to a
  CSV — see the `logger` entry above for why that lives there now
  instead of being deferred to whatever later reads the file.
  `web_server_get_status()` hands the current WiFi
  mode/SSID/IP/time-source (`WEB_SERVER_TIME_UNSET`/`_NTP`/`_MANUAL`)
  state to non-HTTP callers (`components/ui`'s time_settings and
  wifi_settings screens) without
  them reaching into WiFi driver state directly; those fields are written
  once per bring-up outcome from the WiFi task/event handlers and read
  back without a lock, same convention as `boot_epoch_offset_us` above —
  a manual set simply overwrites them the same way, and a later SNTP sync
  (network recovers) overwrites a manual one right back, since NTP is
  always the preferred source when it's available —
  `web_server_sync_ntp_now()` (`esp_netif_sntp_start()`, which restarts
  the client if already running) forces that recovery attempt immediately
  rather than waiting for the client's own poll interval.
  `web_server_get_wall_clock()` reads the current time back out (boot-
  relative `esp_timer_get_time()` plus `boot_epoch_offset_us`) for a UI
  clock display; false if neither source has landed yet.
  `web_server_forget_wifi()` erases the stored credentials via
  `wifi_provision_clear()` and reboots —
  the only other way off a bad network is finding the captive portal
  again, which requires already being off it.
  `web_server_set_timezone()`/`web_server_get_timezone_index()` and the
  `web_server_timezones[]` table are a separate, independent axis from
  all of the above: they change how an already-known UTC time is
  *displayed* (via `setenv("TZ", ...)` + `tzset()`, so any `localtime_r()`
  call anywhere in the firmware honors it), never what
  `web_server_get_wall_clock()` itself returns or what `logger` writes to
  a CSV. `web_server_init()` loads the persisted choice from NVS
  (namespace `tz_cfg`, defaulting to index 0 — Europe/Berlin,
  `CET-1CEST,M3.5.0,M10.5.0/3` — since this firmware's primary deployment
  is in Germany) and applies it before spawning `wifi_bringup_task`, so
  the on-device clock reads correctly in local time even with no network
  at all — unlike the wall clock itself, the TZ rule doesn't need one.
- **ui** — every screen and the navigation framework between them; see
  the Architecture section above for the full breakdown. Depends on
  `data_hub` (tiles, measurement, graph), `web_server` (tiles' clock,
  time_settings, wifi_settings, set_time, timezone), `logger` (tiles'
  Start/Stop toggle, log_manager),
  and `sd_storage` (sd_info's read-only status/info calls; log_manager is
  the one screen that goes further and calls `sd_storage_list_dir()` /
  `sd_storage_erase()` / `sd_storage_format()` directly, since managing
  files on the card is its entire job) to read/change state, but never on
  `lcd`/`touch`/`uart_link`/`rgb_led` — hardware bring-up stays in
  `main.c`, and screens only ever reach data through hub-style components
  that exist precisely to decouple UI from hardware specifics.
  `screen_nav.h` and each screen's own header live at the component root,
  not `include/` — they're internal to `components/ui` (only
  `include/ui.h`'s `ui_init()` is public), matching how `wifi_provision.h`
  sits alongside `web_server.c` rather than in that component's `include/`.

Component dependencies are declared per-component in each
`components/*/CMakeLists.txt` (`REQUIRES`/`PRIV_REQUIRES`) — check there
before assuming what a component can call into. Managed third-party
components (lvgl, esp_lvgl_port, esp_lcd_ili9341, esp_lcd_touch_xpt2046,
mdns, cjson) are pulled via `idf_component.yml` manifests into
`managed_components/` (vendored/generated — don't hand-edit).

### GPIO sharing note

`hardware.h` deliberately reuses GPIO 4 for both `LCD_RESET` and
`RGB_LED_RED` — this is safe and intentional (GPIO 4 isn't wired to an
actual display reset line on stock CYD boards; the comment in `hardware.h`
and §1.1 of `ESP32-CYD-Pinout.md` explain why). Don't "fix" this by
reassigning pins without reading that context first.

### DRAM budget

This board has no PSRAM, so static (`.bss`/`.data`) RAM is tight once WiFi
is in the build — WiFi/lwip/mbedTLS's own static buffers plus LVGL's
built-in memory pool (`CONFIG_LV_MEM_SIZE`, 64 KB) already consume most of
it. `data_hub`'s channel table used to overflow the link on its own before
`DATA_HUB_HISTORY_LEN` got trimmed down (see that component's entry above).
If a future change needs more static RAM (bigger buffers, more channels),
check `idf.py build`'s linker output actually still fits before assuming
it's just a Kconfig/flash-size question — flash headroom (this partition
table has plenty) and DRAM headroom are separate budgets.
