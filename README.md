# diy_nanodrop

DIY DNA quantifier (UV absorbance, 260nm/280nm).

This is the **AS7331 fork**. The original `main` branch uses the analog
GUVA-S12SD sensor; this branch replaces it with the digital I2C
**AS7331** spectral UV sensor (SparkFun SEN-23517 / Qwiic 1x1).

- Branch `main`: GUVA-S12SD (analog, A0)
- Branch `as7331`: AS7331 (I2C, UVA/UVB/UVC)

## What changed

| | `main` (GUVA-S12SD) | `as7331` (this branch) |
|---|---|---|
| Sensor | GUVA-S12SD analog module | AS7331 digital I2C |
| Interface | ADC A0 | I2C 0x74 (shared with OLED) |
| Channels | 1 (broadband) | UVA/UVB/UVC |
| 265nm LED | single detector | **UVC** channel (200-280nm) |
| 280nm LED | single detector | **UVB** channel (280-320nm) |
| Driver | `analogRead` | in-sketch driver, no external lib |

The measurement / calibration state machine, EEPROM calibration storage,
OLED UI and pin assignments for LEDs, button and arm switch are unchanged.

## Wiring (LGT8F328P Nano, 5V)

```
LED_265 PWM  -> D3
LED_280 PWM  -> D5
AS7331 SDA   -> D18   (via level shifter)
AS7331 SCL   -> D19   (via level shifter)
OLED  SDA    -> D18   (same bus)
OLED  SCL    -> D19
CALIB BUTTON -> D7
ARM SWITCH   -> D8
```

> **Important:** the AS7331 operates at **2.7-3.6V (3.3V)**. Power it from
> 3.3V and put a bidirectional I2C level shifter (PCA9306, SparkFun
> BOB-12009, etc.) between the 5V MCU and the sensor. Do not apply 5V to the
> sensor's SDA/SCL.

The AS7331 and the SSD1306 OLED share the I2C bus (0x74 and 0x3C).

## Build

Same as before (direct `avr-g++`, LGT8F328P core):

```bash
USE_OLED=1 ./build.sh     # OLED + Serial
USE_OLED=0 ./build.sh     # Serial only
```

`build.sh` copies `nanodrop.ino` to `main.cpp` before compiling, so keep
edits in `nanodrop.ino`.

## Sensor settings

Defaults match the SparkFun library: gain 2x, conversion time 64ms,
conversion clock 1.024MHz, CMD (one-shot) mode. Each measurement turns an
LED on, waits 300ms, triggers one conversion, waits 64ms, then reads the
result. The raw counts are converted to uW/cm2 (datasheet equation 3) before
`A = -log10(I/I0)`.

To change gain/time, edit `AS7331_GAIN_RAW` / `AS7331_TIME_RAW` /
`AS7331_CCLK_RAW` in the sketch. The conversion factor updates automatically.

See `nanodrop_BOM.md` for the full BOM and calibration standard preparation.
