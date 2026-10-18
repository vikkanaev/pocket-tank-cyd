# Waveshare ESP32-S3-Touch-LCD-1.69: bench log (phases 1 and 2)

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

## Phase 2 (battery)

Bench 2026-10-17..18, firmware 85ad0e6, then ec6996c and ff22d5f.

### Power circuit (schematic V2.1)

Source: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69/ESP32-S3-Touch-LCD-1.69_V2.1.pdf

- Key2 (PWR) goes to GND. Through D2 it pulls SYS_OUT (GPIO40), which has a
  10k pull-up: active LOW, so `PWR_KEY_ACTIVE_LEVEL 0` is right. Through D3 it
  pulls the gate of the P-MOS Q5 (B+ to VBAT): a press switches the board on.
- SYS_EN GPIO41 goes through 1k to the NPN T2 (10k base pulldown) that holds
  the same gate: this is the latch.
- Q4 + D4 select VBUS or VBAT as the input of the ME6217 3.3 V LDO.
- BAT_ADC (GPIO1) = B+ through 200k/100k. B+ is taken before the PWR switch.

### USB bench

- Boot log is clean: the latch comes first, then "PWR key on GPIO40 active 0,
  up at boot". 0 touch I2C errors. No buzzer noise.
- The PWR polarity is confirmed: GPIO40 idles high and goes low on a press.

### RST is not PWR

An early finding "every PWR press power-cycles the board" was the RST button
(CHIP_PU). Reset reason POWERON. RST releases SYS_EN, so on battery RST is
power-off, and only PWR or USB brings the board back. Not a firmware bug.

### VBAT is about 4.16 V with no cell

With no cell the meter reads ~4158 mV (~100%). BAT_ADC measures B+ before the
PWR switch, i.e. the ETA6098 output, so on the cable "no battery" looks like
"full". The meter cannot tell the two apart. Open item.

### Battery bench (a 1S cell connected)

- Hold PWR: on; stays on after release (the latch in app_main is fast enough,
  no bootloader hook needed).
- Long press: off; PWR: on again.
- Short press: sleep; wake works. A short PWR wake does not re-sleep at once.
- The battery meter shows the charge.
- Not checked yet: charging from a wall charger, an overnight run (RTC
  catch-up), the low-voltage cutoff, the dark-mode current, wake by touch and
  by BOOT individually.

### Found on the bench and fixed

- ec6996c: the stats card stays 30 s on WST169 (10 s on the other boards), at
  Viktor's request. Card close reasons are logged.
- ff22d5f: the CST816T sometimes returns an all-ones report (raw 0xFFF, i.e.
  4095,61680 after the driver's mirror/swap), from one every few seconds up to
  bursts. The driver took it for a finger and map_touch pinned it to the
  corner 279,239 as a 0 ms tap, which closed cards and pages. Off-panel points
  (x >= PANEL_H or y >= PANEL_W) are now dropped under WST169. Verified: the
  card lasted the full 30 s; the drops are logged.
- Deferred: the drop log line is per report; rate-limit it.
