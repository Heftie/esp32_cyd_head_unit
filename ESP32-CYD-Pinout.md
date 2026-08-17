# ESP32 Cheap Yellow Display (CYD) — ESP32-2432S028R Pinout Reference

Board: **ESP32-2432S028R**, MCU module: **ESP32-WROOM-32**
Sources: [witnessmenow/ESP32-Cheap-Yellow-Display schematic (MCU board)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/OriginalDocumentation/5-Schematic/ESP32-2432S028-MCU.jpg), Random Nerd Tutorials CYD Pinout guide, Kafkar Projects CYD pinout guide.

> ⚠️ Note: There are several hardware revisions of this board floating around AliExpress (v1/v2/v3, "GUITION" branded, ST7789 vs ILI9341 driver, different silk-screening for CN1/P3). Pin assignments below match the most common revision documented by the CYD community. Always double-check your specific board's silkscreen/schematic before wiring external peripherals.

---

## 1. Onboard Peripherals — Pin Definitions

### 1.1 TFT Display (ILI9341/ST7789, SPI — HSPI bus)

| Function | Signal | GPIO |
|---|---|---|
| MISO | TFT_MISO | GPIO 12 |
| MOSI | TFT_MOSI | GPIO 13 |
| SCLK | TFT_SCLK | GPIO 14 |
| CS | TFT_CS | GPIO 15 |
| DC (Data/Command) | TFT_DC | GPIO 2 |
| RST (Reset) | TFT_RST | -1 on stock wiring (tied to EN) — **this project drives GPIO 4 as `LCD_RESET` instead** (see note below) |
| Backlight (BL) | TFT_BL | GPIO 21 |

> **This project's deviation:** [`hardware.h`](main/hardware.h) defines `LCD_RESET` as `GPIO_NUM_4`, which `main.c` passes into `lcd_config_t.reset_gpio` for [`app_lcd_init()`](components/lcd/lcd.c). On stock CYD wiring GPIO 4 is the RGB LED's red channel, not a display reset line — the panel's actual reset happens via the EN pin at power-on and the ILI9341/ST7789 software-reset command sent during `esp_lcd_panel_init()`. Toggling GPIO 4 has no effect on the display; it just blips the red LED at boot/reset. Since this project also drives the RGB LED on the same pin (`RGB_LED_RED` in `hardware.h`), the two definitions share GPIO 4 — harmlessly, because the `LCD_RESET` role is a no-op. Just expect the red LED to blip on every display init.

### 1.2 Resistive Touchscreen (XPT2046 — bit-banged GPIO in this project, not VSPI)

| Function | Signal | GPIO |
|---|---|---|
| IRQ | XPT2046_IRQ (T_IRQ) | GPIO 36 |
| MOSI | XPT2046_MOSI (T_DIN) | GPIO 32 |
| MISO | XPT2046_MISO (T_OUT) | GPIO 39 |
| CLK | XPT2046_CLK (T_CLK) | GPIO 25 |
| CS | XPT2046_CS (T_CS) | GPIO 33 |

> **This project's deviation:** these are electrically VSPI (SPI3) pins on
> stock CYD wiring, and early on this project drove them with the real SPI3
> peripheral. It now bit-bangs the XPT2046 protocol over plain GPIO instead
> (`components/touch/xpt2046_bitbang_io.c`) — a fake `esp_lcd_panel_io_t`
> whose `rx_param` toggles these pins directly, so the vendored XPT2046
> driver runs unmodified on top of it. This frees VSPI entirely for the
> microSD slot (§1.4) to own exclusively at full hardware SPI clock, with
> no bus arbitration between touch and SD ever needed.

### 1.3 RGB LED (onboard, back of board — active LOW)

| Color | GPIO |
|---|---|
| Red | GPIO 4 |
| Green | GPIO 16 |
| Blue | GPIO 17 |

> LOW = ON, HIGH = OFF (inverted logic). This project drives all three channels via `rgb_led_init()`/`rgb_led_set_rgb()` (see `main/hardware.h`, `components/rgb_led`). GPIO 4 (red channel) is also labeled `LCD_RESET` (see §1.1 note), but that's a no-op for the display, so red is genuinely usable alongside green/blue.

### 1.4 MicroSD Card Slot (SPI — owns VSPI exclusively in this project)

| Function | GPIO |
|---|---|
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| SCK | GPIO 18 |
| CS | GPIO 5 |

> These are physically different pins from the touchscreen's (§1.2) — the
> two were never on the same electrical bus, despite some community docs
> describing SD as "sharing VSPI with touch." This project's `sd_storage`
> component (see `main/hardware.h`'s `SD_SPI_*`/`SD_CS` defines) mounts the
> card on VSPI (SPI3_HOST) at boot and keeps it mounted permanently — that
> peripheral has nothing else to share it with once touch moved to
> bit-banged GPIO (§1.2).

### 1.5 LDR (Light-Dependent Resistor / ambient light sensor)

| Function | GPIO |
|---|---|
| LDR analog output | GPIO 34 (input-only pin) |

### 1.6 Speaker (2P 1.25mm JST connector, passive/amplified output)

| Function | GPIO |
|---|---|
| Audio out | GPIO 26 |

### 1.7 Buttons

| Button | GPIO |
|---|---|
| BOOT (flash mode) | GPIO 0 |
| RST / RESET | Hardware EN line (not a GPIO) |

### 1.8 TX/RX Serial Connector (labeled P1, wired to onboard CH340 USB-serial)

| Function | GPIO |
|---|---|
| TX | GPIO 1 |
| RX | GPIO 3 |

> These are shared with the CH340 USB-to-serial converter used for flashing/Serial Monitor. Using them for an external peripheral (e.g. GPS) can conflict with USB serial; results vary by board revision. Also note: on many boards the RX line has a known hardware quirk (resistor placement) that can prevent it from reliably receiving external serial data.

### 1.9 Extended GPIO Connectors (P3 and CN1)

**P3 connector:** GND, GPIO 35, GPIO 22, GPIO 21
**CN1 connector:** GND, GPIO 22, GPIO 27, 3V3

| Pin | Also available on | Notes |
|---|---|---|
| GPIO 35 | P3 only | **Input-only** pin (no output/PWM capability) |
| GPIO 22 | P3 and CN1 | Free GPIO, good for I2C SCL |
| GPIO 21 | P3 only | Shared with TFT backlight — stays HIGH while backlight is on |
| GPIO 27 | CN1 only | Free GPIO, good for I2C SDA (note: on some board revisions this pad reportedly isn't connected — verify with a multimeter) |
| 3V3 | CN1 | Power output |
| GND | P3, CN1 | Ground |

**➡️ Effectively free/available pins for external peripherals: GPIO 35 (input only), GPIO 22, GPIO 27.**
(GPIO 21 is usable only if you don't need the backlight controllable, since it's tied high by the backlight circuit.)

---

## 2. Full GPIO Map — What's Attached to Each Pin

| GPIO | Attached Peripheral / Function | Direction | Free for external use? |
|---|---|---|---|
| GPIO 0 | BOOT button | Input (internal pull-up) | ⚠️ Affects boot mode — avoid |
| GPIO 1 | TX — CH340 USB-serial (P1 connector) | Output | ⚠️ Shared w/ USB serial |
| GPIO 2 | TFT_DC (Data/Command) | Output | ❌ Used by display |
| GPIO 3 | RX — CH340 USB-serial (P1 connector) | Input | ⚠️ Shared w/ USB serial, RX quirk |
| GPIO 4 | RGB LED — Red channel (stock); also labeled `LCD_RESET` in this project (no-op for display, see §1.1) | Output | ❌ Used by LED |
| GPIO 5 | MicroSD CS | Output | ❌ Used by SD card |
| GPIO 12 | TFT_MISO | Input | ❌ Used by display |
| GPIO 13 | TFT_MOSI | Output | ❌ Used by display |
| GPIO 14 | TFT_SCLK | Output | ❌ Used by display |
| GPIO 15 | TFT_CS | Output | ❌ Used by display |
| GPIO 16 | RGB LED — Green channel | Output | ❌ Used by LED (unless LED removed) |
| GPIO 17 | RGB LED — Blue channel | Output | ❌ Used by LED (unless LED removed) |
| GPIO 18 | MicroSD SCK | Output | ❌ Used by SD card |
| GPIO 19 | MicroSD MISO | Input | ❌ Used by SD card |
| GPIO 21 | TFT Backlight | Output | ⚠️ On P3 connector, tied high while backlight on |
| GPIO 22 | Free — on P3 & CN1 connectors | I/O | ✅ Available (good I2C SCL) |
| GPIO 23 | MicroSD MOSI | Output | ❌ Used by SD card |
| GPIO 25 | Touch CLK (XPT2046) | Output | ❌ Used by touchscreen |
| GPIO 26 | Speaker output | Output (DAC-capable) | ❌ Used by speaker |
| GPIO 27 | Free — on CN1 connector | I/O | ✅ Available (good I2C SDA) |
| GPIO 32 | Touch MOSI (XPT2046) | Output | ❌ Used by touchscreen |
| GPIO 33 | Touch CS (XPT2046) | Output | ❌ Used by touchscreen |
| GPIO 34 | LDR (light sensor) | Input only (ADC) | ❌ Used by LDR |
| GPIO 35 | Free — on P3 connector | **Input only** (ADC) | ✅ Available (input only) |
| GPIO 36 | Touch IRQ (XPT2046) | Input only | ❌ Used by touchscreen |
| GPIO 39 | Touch MISO (XPT2046) | Input only | ❌ Used by touchscreen |

**Not broken out / not used on this board:** GPIO 6–11 (connected to onboard SPI flash — never use), GPIO 20, GPIO 24, GPIO 28–31, GPIO 37, GPIO 38.

---

## 3. Practical Notes for Using Free Pins

- **True general-purpose pins available for your own sensors/actuators: GPIO 22, GPIO 27, GPIO 35 (input-only).** That's the well-known limitation of this board — most GPIOs are already claimed by the display, touchscreen, SD card, LED, and speaker.
- **I2C:** Since default I2C pins (GPIO 21/22) can't both be used (21 is the backlight), define custom I2C pins in software, e.g.:
  ```cpp
  #define I2C_SDA 27
  #define I2C_SCL 22
  Wire.begin(I2C_SDA, I2C_SCL);
  ```
  This combo has been confirmed working with sensors like BME280, HTU21D, AHT10, Si7021, and MPU-6050.
- **Freeing up more pins (mods, not stock):**
  - Removing the RGB LED frees GPIO 4, 16, 17.
  - Cutting/rewiring the backlight FET trace can free GPIO 21 (advanced mod).
  - The SD card pins (5, 18, 19, 23) can be reclaimed if you don't need the SD slot (requires soldering directly to the SD card IC pads — difficult, small pitch).
- **GPIO 1/3 (P1 connector):** Usable for external serial/UART devices (e.g. GPS modules) in some cases, but they're wired directly to the CH340 USB-serial chip, so behavior can conflict with the USB Serial Monitor, and RX has a known routing issue on many boards that prevents reliable external reception without a hardware fix.
- **GPIO 35 and 34 are input-only** — cannot drive outputs or provide PWM.
- **GPIO 6–11** connect to the board's integrated SPI flash memory — never use these, even though they may be electrically exposed on the raw ESP32-WROOM-32 module footprint.

---

## 4. This Project's Actual Pin Config

This project is **ESP-IDF native** (`esp_lcd` + `esp_lvgl_port`), not Arduino/TFT_eSPI, so it doesn't use a `User_Setup.h`. The real source of truth is [`main/hardware.h`](main/hardware.h):

```c
// Display (esp_lcd, SPI2_HOST)
#define LCD_SPI_CLK   GPIO_NUM_14
#define LCD_SPI_MOSI  GPIO_NUM_13
#define LCD_SPI_MISO  GPIO_NUM_12
#define LCD_DC        GPIO_NUM_2
#define LCD_CS        GPIO_NUM_15
#define LCD_RESET     GPIO_NUM_4   // see §1.1 — inert on stock wiring, don't rely on it
#define LCD_BACKLIGHT GPIO_NUM_21  // PWM via LEDC

// Touch (esp_lcd_touch_xpt2046, bit-banged GPIO — see §1.2, not SPI3_HOST)
#define TOUCH_SPI_CLK  GPIO_NUM_25
#define TOUCH_SPI_MOSI GPIO_NUM_32
#define TOUCH_SPI_MISO GPIO_NUM_39
#define TOUCH_CS       GPIO_NUM_33
#define TOUCH_IRQ      GPIO_NUM_NC  // disabled on purpose, see main/hardware.h comment

// MicroSD (sd_storage, SPI3_HOST — exclusive, see §1.4)
#define SD_SPI_CLK     GPIO_NUM_18
#define SD_SPI_MOSI    GPIO_NUM_23
#define SD_SPI_MISO    GPIO_NUM_19
#define SD_CS          GPIO_NUM_5
```

No speaker code exists in this project — GPIO 26 is untouched by the
codebase, free per the board's stock wiring, but not wired up in software
here.

---

*Board pinout compiled from the [ESP32-Cheap-Yellow-Display GitHub schematic](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display) and community documentation (Random Nerd Tutorials, Kafkar Projects); cross-checked line-by-line against this repo's [`hardware.h`](main/hardware.h), [`lcd.c`](components/lcd/lcd.c), [`touch.c`](components/touch/touch.c)/[`xpt2046_bitbang_io.c`](components/touch/xpt2046_bitbang_io.c), and [`sd_storage.c`](components/sd_storage/sd_storage.c) on 2026-08-17. Verify against your specific board revision before wiring external peripherals.*
