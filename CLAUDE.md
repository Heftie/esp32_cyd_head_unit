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
environment (`. $IDF_PATH/export.sh` or the VS Code IDF extension; this repo
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

`main/main.c` is the app entry point and currently doubles as a scratch pad
for UI experiments (multiple demo UIs — `multimeter_create_ui`,
`touch_test_create_ui`, the RGB/counter demo — are defined side by side and
swapped by commenting/uncommenting calls in `app_main`). When working here,
follow the existing pattern rather than assuming one UI is "the" app.

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
- **touch** — brings up the XPT2046 touch controller on its own SPI bus
  (separate from the LCD's) and rescales raw touch coordinates onto the
  display resolution before handing back an `esp_lcd_touch_handle_t` for
  `esp_lvgl_port` to consume.
- **data_hub** — an in-memory pub/sub-style ring buffer keyed by channel
  name (e.g. `"VOLT:DC"`), storing recent samples with timestamps.
  `data_hub_publish()` is called only by `uart_link`; UI code reads via
  `data_hub_get_latest()` / `data_hub_get_history()` /
  `data_hub_list_channels()`. Not persisted — resets on reboot.
- **uart_link** — talks a small SCPI-style text protocol to a companion MCU
  over UART2: newline-terminated commands/replies. Handles a `*IDN?`
  handshake on init, answers some queries locally (see
  `s_local_commands` in `uart_link.c`), parses push lines shaped like
  `MEAS:VOLT:DC 12.84,V` (name, value, unit) into `data_hub_publish()`
  calls, and exposes `uart_link_query()` for synchronous request/reply
  (single query in flight at a time, mutex-serialized).
- **rgb_led** — drives the onboard 3-channel LED via LEDC PWM,
  0–255 per channel; channels set to `GPIO_NUM_NC` in config are skipped.
- **sd_storage** — FAT-over-SDSPI wrapper around the microSD slot (path
  read/write/erase/format, mount/unmount, capacity/free-space info). Can
  share an already-initialized SPI bus (e.g. with `touch`, since both sit
  on the same physical SPI lines on this board) via
  `bus_already_initialized`, or bring up its own bus.

Component dependencies are declared per-component in each
`components/*/CMakeLists.txt` (`REQUIRES`/`PRIV_REQUIRES`) — check there
before assuming what a component can call into. Managed third-party
components (lvgl, esp_lvgl_port, esp_lcd_ili9341, esp_lcd_touch_xpt2046)
are pulled via `idf_component.yml` manifests into `managed_components/`
(vendored/generated — don't hand-edit).

### GPIO sharing note

`hardware.h` deliberately reuses GPIO 4 for both `LCD_RESET` and
`RGB_LED_RED` — this is safe and intentional (GPIO 4 isn't wired to an
actual display reset line on stock CYD boards; the comment in `hardware.h`
and §1.1 of `ESP32-CYD-Pinout.md` explain why). Don't "fix" this by
reassigning pins without reading that context first. Similarly, `touch` and
`sd_storage` are expected to share one physical SPI bus — see
`sd_storage_config_t.bus_already_initialized`.
