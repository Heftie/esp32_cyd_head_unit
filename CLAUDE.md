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
initializes every data-producing component (`data_hub`, `uart_link`,
`rgb_led`, `sd_storage`, `logger`, `web_server`), then hands off to
`ui_init()` and settles into a loop that cycles the status LED and
periodically dumps `data_hub` to the log. It owns hardware bring-up only —
no screen, LVGL widget, or navigation logic lives in `main.c` itself.

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
(`tiles_screen.c`/`settings_screen.c`/`measurement_screen.c`/
`set_time_screen.c`/`sd_info_screen.c`), talking to the others only by
name through `screen_nav.h` — the one exception is
`measurement_screen_set_channel()`, a small setter `tiles_screen.c` calls
before `screen_push("measurement")` so the single measurement-screen
instance knows which channel to show, since `screen_push()` itself only
takes a name.

- **tiles** (default/home) — a DD/MM/YYYY HH:MM:SS UTC clock (from
  `web_server_get_wall_clock()`; "Time not set" until NTP or a manual set
  lands) over a data-driven dashboard, one tile per `data_hub` channel,
  created on first sight and refreshed on an LVGL timer; nothing about the
  channel set is hardcoded. Tapping a tile opens **measurement** for that
  channel; its Settings button pushes **settings**.
- **settings** — WiFi status (mode/SSID/IP/time source, from
  `web_server_get_status()`, polled on its own LVGL timer); a "Sync NTP"
  button (`web_server_sync_ntp_now()`) that forces an immediate resync
  attempt — the way back to `WEB_SERVER_TIME_NTP` after a manual set, once
  the network can reach an NTP server again; a "Forget network" action
  (`web_server_forget_wifi()`, which erases the stored credentials and
  reboots) gated behind a tap-to-arm/tap-to-confirm sequence since it's a
  one-way trip off the current network; and buttons into **set_time** and
  **sd_info**. Sync/Forget are both greyed out (non-clickable) outside STA
  mode — neither means anything while running the setup AP.
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
  path to an NTP server otherwise leaves `/api/history` permanently 503
  with no way to recover. Converts the roller selections to a UTC epoch
  via a small `days_from_civil()` helper (Howard Hinnant's constant-time
  civil-calendar algorithm) rather than libc's `timegm()`/`mktime()`, to
  not depend on a timezone-aware libc build for something this firmware
  treats as UTC-only everywhere else anyway.
- **sd_info** — `sd_storage_is_mounted()` / `sd_storage_get_info()` read
  out as mounted/not, card type, capacity, used/free space, polled on its
  own LVGL timer; reachable only from **settings**.

All five screens reuse the same `lv_scr_act()` object — `lv_obj_clean()`
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
  Currently just a boot-time color-cycle smoke test in `main.c` — not yet
  wired to any real system state.
- **sd_storage** — FAT-over-SDSPI wrapper around the microSD slot (path
  read/write/erase/format, mount/unmount, capacity/free-space info). Owns
  SPI3 (VSPI) exclusively and permanently once mounted — see the `touch`
  entry above for why that bus is free for it. `bus_already_initialized`
  still exists in the config struct for a caller that's already brought up
  the target SPI host itself, but nothing in this repo uses that path today.
- **logger** — polls `data_hub` on a timer (default 5s) and appends any
  samples not yet written to a single continuous CSV on the SD card
  (`log.csv` — no per-day rotation) via `sd_storage`. Runs from its own
  task rather than writing from inside `data_hub_publish()`, specifically
  so a slow SD write (single-digit ms, but still) never blocks
  `uart_link`'s RX task and risks dropping UART bytes.
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
  Once connected, serves an embedded dashboard at `/`, live channel values
  as JSON at `/api/data`, per-day CSV rows at
  `/api/history?date=YYYY-MM-DD`, and the raw log at `/download`. Since
  `logger`'s timestamps are `esp_timer_get_time()` (boot-relative, not
  wall-clock), `web_server` computes a `boot_epoch_offset_us` once SNTP
  syncs and adds it to any stored timestamp to get a real date —
  `/api/history` returns a JSON error (503) if SNTP hasn't synced yet
  rather than guessing. This board has no battery-backed RTC crystal, so
  `boot_epoch_offset_us` is the *only* wall clock the firmware has, and it
  resets to unset on every boot; `web_server_set_wall_clock()` lets a
  caller (`components/ui`'s set_time screen) set it manually, the same
  way `sntp_sync_cb()` would, for when there's no network path to an NTP
  server at all. `web_server_get_status()` hands the current WiFi
  mode/SSID/IP/time-source (`WEB_SERVER_TIME_UNSET`/`_NTP`/`_MANUAL`)
  state to non-HTTP callers (`components/ui`'s settings screen) without
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
- **ui** — every screen and the navigation framework between them; see
  the Architecture section above for the full breakdown. Depends on
  `data_hub` (tiles, measurement), `web_server` (tiles' clock, settings,
  set_time), and `sd_storage` (sd_info — read-only status/info calls
  only, never mount/write/erase) to read state, but never on
  `lcd`/`touch`/`uart_link`/`rgb_led`/`logger` — hardware bring-up stays
  in `main.c`, and screens only ever reach data through hub-style
  components that exist precisely to decouple UI from hardware specifics.
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
