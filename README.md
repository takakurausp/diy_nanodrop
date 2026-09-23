# diy_nanodrop

DIY DNA quantifier (UV absorbance, 260nm/280nm).

This branch targets an **ESP32 board with an integrated 2.8" ST7789 touch LCD**
and a DIY UV sensor module (**SG01S-C18** SiC photodiode + transimpedance
amplifier + **ADS1115** ADC), with a touch UI, guided calibration, WiFi (WPS)
and CSV download. The **AS7331** spectral UV sensor is also supported as a
build option.

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
  - LED tune (see below).
  - WiFi settings (see below).
  - Touch calibration (2-point, stored in EEPROM).
  - Initialize WiFi settings (erase stored SSID/password).

### LED tune

To help adjust the LED output, **Settings -> LED tune** continuously alternates
the 265nm/280nm LEDs and displays the sensor output (raw counts) and the
voltage-converted value for each. The active LED row is highlighted.

Raw readings are smoothed with an exponential moving average (EMA) to reject
noise:

```
X = a * Xnow + (1 - a) * X
```

`a` (`TUNE_ALPHA`, default 0.20) and the per-LED dwell (`TUNE_STEP_MS`, default
200ms) are defined in `nanodrop.ino`. The voltage column uses the ADS1115 full
scale of +/-4.096V (1 LSB = 125uV).

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
| SG01S-C18 + TIA + ADS1115 (default) | `-DSENSOR_AS7331=0 -DPD_USE_ADS1115=1` | SiC UV photodiode + transimpedance amp, 16-bit I2C ADC, recommended |
| SG01S-C18 + TIA + ESP32 ADC | `-DSENSOR_AS7331=0 -DPD_USE_ADS1115=0` | TIA output -> GPIO33, simple but low accuracy |
| AS7331 (I2C, 3ch) | `-DSENSOR_AS7331=1` | Original 265nm->UVC, 280nm->UVB |

### SG01S-C18 + TIA module

The primary detector is a **sglux SG01S-C18** SiC UV photodiode. Its
photocurrent is converted to a voltage by a **transimpedance amplifier (TIA)**
and digitized by an **ADS1115** 16-bit I2C ADC.

Absorbance is a ratio `I/I0` measured on the same detector, so the TIA gain and
the absolute responsivity cancel out: **no absolute calibration is needed**.
Keep the TIA output positive and within the ADS1115 full scale (PGA = +/-4.096V
in this firmware, `ADS1115_CFG_START 0xC383`).

It is a single broadband channel, so the 265nm and 280nm measurements read the
same detector (absorbance still works as a ratio, but channel
separation/cross-talk is worse than the AS7331). A **GUVA-S12SD** or a similar
analog UV photodiode can be used on the same path.

Wiring (SG01S-C18 + TIA + ADS1115):
```
SG01S-C18 -> TIA -> ADS1115 AIN0
ADS1115 SDA/SCL  -> GPIO21 / GPIO22
ADS1115 ADDR     -> GND (address 0x48)
ADS1115 VDD/GND  -> 3.3V / GND
```
The 265/280 LEDs are unchanged (GPIO16 / GPIO17).

Use an external ADC (ADS1115) rather than the ESP32 internal ADC. Reasons:

- The ESP32 ADC is noisy and non-linear (especially near the rails); factory
  calibration is poor.
- In the internal-ADC mode the TIA output must stay within the 0-3.3V ESP32
  ADC range (use a 3.3V-powered TIA). The ADS1115 at `+/-4.096V` full scale
  accepts a wider output swing and is stable.

An **ADS1015** (12-bit, cheaper) also works in principle but its conversion
result is left-justified in the 16-bit register; the current driver assumes
ADS1115. Ask if you want ADS1015 support.

## Wiring (AS7331 option)

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
