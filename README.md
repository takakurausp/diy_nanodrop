# diy_nanodrop

DIY DNA quantifier (UV absorbance, 260nm/280nm).

This branch targets an **ESP32 board with an integrated 2.8" ST7789 touch LCD**
and the **AS7331** spectral UV sensor, with a touch UI, guided calibration,
WiFi (WPS) and CSV download.

- Branch `main`: LGT8F328P + GUVA-S12SD (analog)
- Branch `as7331`: LGT8F328P + AS7331 (I2C)
- Branch `esp32` (this branch): **ESP32 + 2.8" ST7789 touch LCD + AS7331**

## Target board

**ideaspark ESP32 2.8" IPS LCD touch board (240x320, ST7789)**
<https://www.amazon.co.jp/dp/B0HHSHFZ8Q>

ESP32-WROOM-32 (3.3V, 16MB), 2.8" 240x320 IPS LCD (ST7789, SPI),
XPT2046 resistive touch, microSD, CH340G USB. Free GPIOs:
5, 12, 16, 17, 21, 22, 25, 26, 33.

## UI

Touch only. On boot, three large buttons:

- **Calibration**
  - Checks EEPROM for calibration data.
  - If present: shows "Calibration data found" plus the K/B coefficients,
    with `Re-calibrate` / `Back`.
  - If absent: asks "Configure now?". Yes starts a guided 3-step procedure:
    BLANK -> DNA (50 ug/mL) -> Protein (1% w/v). On-screen text tells you
    which standard to apply at each step. The resulting K/B coefficients are
    saved to EEPROM.
- **Measuring**
  - Each sample is preceded by a BLANK measurement.
  - Flow: apply BLANK -> measure -> apply SAMPLE -> measure -> result.
  - Result shows A260, A280, Purity and Concentration (ug/mL). Records are
    kept in memory. After a result the screen stays until you continue and
    warns you to apply BLANK before the next sample.
  - A `Download data` button is on the screen edge.
- **Settings**
  - Brightness (backlight PWM, stored in EEPROM).
  - WiFi settings (see below).
  - Touch calibration (2-point, stored in EEPROM).
  - Initialize WiFi settings (erase stored SSID/password).

## WiFi

- If SSID/password are stored in EEPROM, the device connects as a station.
- Otherwise it starts an **AP**: SSID `mynanodrop`, password `12345678`,
  IP **192.168.5.1**.
- A WebServer runs at all times:
  - `/` -> status page
  - `/data.csv` -> all stored measurements as CSV
    (index, A260, A280, Purity, Conc_ug_ml)
- `Scan & WPS` lists nearby SSIDs; selecting one starts **WPS (push button)**.
  Press the WPS button on your router when prompted. On success the
  credentials are stored in EEPROM and reused on the next boot.

> Note: ESP32 WPS (push-button/PBC) connects to the router that has WPS
> enabled; the SSID you pick in the list is informational.

To download data: connect your phone/PC to the same WiFi (or to the
`mynanodrop` AP when in AP mode) and open `http://<ip>/data.csv`
(AP mode: `http://192.168.5.1/data.csv`).

## Sensor options

The sensor is selected at build time with `SENSOR_AS7331` (see the top of
`nanodrop.ino` or pass a `build_flags` from `platformio.ini`):

| Mode | Macro | Notes |
|------|-------|-------|
| AS7331 (I2C, 3ch) | `-DSENSOR_AS7331=1` | Original 265nm->UVC, 280nm->UVB |
| GUVA-S12SD + ADS1115 (default) | `-DSENSOR_AS7331=0 -DGUVA_USE_ADS1115=1` | External 16-bit I2C ADC, recommended |
| GUVA-S12SD + ESP32 ADC | `-DSENSOR_AS7331=0 -DGUVA_USE_ADS1115=0` | AOUT -> GPIO33, simple but low accuracy |

### GUVA-S12SD fallback

If the AS7331 is hard to source, the analog **GUVA-S12SD** module can be used
instead. It is a single broadband channel, so both the 265nm and 280nm
measurements read the same detector (absorbance is still a ratio `I/I0`, so
this works, but channel separation/cross-talk is worse than the AS7331).

**Use an external ADC (ADS1115) rather than the ESP32 internal ADC.** Reasons:

- The ESP32 ADC is noisy and non-linear (especially near the rails); factory
  calibration is poor.
- The GUVA-S12SD module output is roughly 0-1V, so on the 0-3.3V ESP32 ADC
  range only ~1/3 of the codes are used, losing resolution.
- The **ADS1115** is a 16-bit delta-sigma ADC with a PGA. At `+/-2.048V` full
  scale, a 0-1V signal uses about half the range at ~62.5uV/LSB, and it is
  stable. It is I2C, so it shares the existing bus.

Wiring (ADS1115):
```
GUVA-S12SD AOUT -> ADS1115 AIN0
ADS1115 SDA/SCL  -> GPIO21 / GPIO22
ADS1115 ADDR     -> GND (address 0x48)
ADS1115 VDD/GND  -> 3.3V / GND
```
The 265/280 LEDs are unchanged (GPIO16 / GPIO17).

An **ADS1015** (12-bit, cheaper) also works in principle but its conversion
result is left-justified in the 16-bit register; the current driver assumes
ADS1115. Ask if you want ADS1015 support.

## Wiring (AS7331)

```
LED_265 PWM  -> GPIO16
LED_280 PWM  -> GPIO17
AS7331 SDA   -> GPIO21
AS7331 SCL   -> GPIO22
```

LCD and touch are already wired on the board (LCD CS=15/DC=2/RST=4/BL=32,
touch CS=14/IRQ=27, VSPI SCK=18/MISO=19/MOSI=23). Power the AS7331 from 3.3V;
no level shifter is needed.

## Build / upload (PlatformIO)

```bash
pio run                 # build
pio run -t upload       # upload
pio device monitor      # serial monitor (115200)
```

or via the helper script:

```bash
./build.sh          # build
./build.sh upload   # upload
./build.sh monitor  # serial monitor
```

`build.sh` copies `nanodrop.ino` to `main.cpp` before building, so keep edits
in `nanodrop.ino`. Libraries are pulled by `platformio.ini`
(Adafruit GFX, Adafruit ST7735/ST7789, XPT2046_Touchscreen). The AS7331 driver
is in-sketch.

## Sensor settings

Defaults: gain 2x, conversion time 64ms, conversion clock 1.024MHz, CMD
(one-shot) mode. Raw counts are converted to uW/cm2 (datasheet eq. 3) before
`A = -log10(I/I0)`. Change `AS7331_GAIN_RAW` / `AS7331_TIME_RAW` /
`AS7331_CCLK_RAW` to adjust.

See `nanodrop_BOM.md` for the BOM and calibration standard preparation.
