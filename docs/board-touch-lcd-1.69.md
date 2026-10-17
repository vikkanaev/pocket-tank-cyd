# Waveshare ESP32-S3-Touch-LCD-1.69: bench log (phase 1)

Branch `wst169`, 2026-10-17. Build and flash: `tools/build_wst169.sh`
(`CONFIG_POCKET_TANK_WST169`; CYD.md, "Touch-LCD-1.69").

## Board identity

- ESP32-S3 rev v0.2 (ESP32-S3R8), 8 MB octal PSRAM, 16 MB flash.
- The NEW board revision: the model name is printed on the board (Waveshare
  wiki's rule), so the power pins are SYS_EN GPIO41, SYS_OUT/PWR key GPIO40,
  buzzer GPIO42, RTC_INT GPIO39.
- Factory image backed up before the first flash: `~/factory_wst169_16MB.bin`.

## I2C devices (shared bus SDA 11, SCL 10)

| Address | Chip | Notes |
|---|---|---|
| 0x15 | CST816T touch, IC id 181 | dozes shortly after the reset; auto-sleep disabled with register 0xFE = 1 (reads back 0x01) |
| 0x51 | PCF85063 RTC | boot log: "rtc: system clock set from RTC" |
| 0x6B | QMI8658 IMU | at rest g = [910 -19 -16325]: orientation axis -Y, out-of-glass Z |

## Display

ST7789V2, 240 x 280 scanned landscape 280 x 240. Values verified on the glass:

- gap 20,0: correct, no band.
- colours: INVERT true, BGR false: correct (blue water, not inverted).
- orientation as is (MX | MV): USB on top is upright.
- 40 MHz SPI kept, 15.9 fps before the touch fix, 25.7 fps after. 80 MHz not tried.
- 180-degree flip (the SCREEN row): check PENDING.

## Touch

First boot: after the driver's reset the CST816T stopped answering within half
a second; ~1100 "I2C read failed" pairs in 25 s with nobody at the glass.
Fix (commit 0ae2f37): 0xFE = 1 (auto-sleep off) at init, and a silent
`i2c_master_probe` before each poll read.

| | before | after |
|---|---|---|
| I2C errors per 25 s | ~1100 | 0 |
| touch poll | 13.0 ms | 0.7 ms |
| frame rate | 15.9 fps | 25.7 fps |

Per Viktor: feed, hold, wipe and the page buttons all work; text is not mirrored.
Touch mapping 1/1/0 (swap, mirror x, mirror y as in touch_port_ft3168.c).

## LLM

12.4 tok/s, about 3.5 s per decision.

## Flashing

```
tools/build_wst169.sh build                    # build only
tools/build_wst169.sh /dev/cu.usbmodemNNN flash --model   # app + model (model once per board)
```

The ESP-IDF Python env: `export.sh` names the env after the first python3 on
PATH, but on this Mac only `~/.espressif/python_env/idf5.5_py3.9_env` exists.
`build_wst169.sh` exports `IDF_PYTHON_ENV_PATH` to it when unset (before it
calls `tools/ota_key.sh`, which inherits it). For a hand-run `idf.py`:

```
export IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.5_py3.9_env
. ~/esp/v5.5.2/esp-idf/export.sh
```

The signing key `firmware/keys/ota_signing_key.pem` is local and gitignored.
