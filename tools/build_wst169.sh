#!/bin/bash
# build_wst169.sh - build, and optionally flash, the Waveshare ESP32-S3-Touch-LCD-1.69
# (this fork, 2026-10-17; modelled on tools/build_cyd.sh, CYD.md "Touch-LCD-1.69").
#
#   tools/build_wst169.sh                         # build only (= build)
#   tools/build_wst169.sh build
#   tools/build_wst169.sh [port] flash            # build, then flash the app (+ bootloader, partition table)
#   tools/build_wst169.sh [port] flash --model    # ... and model/out/model_q4.bin at 0x290000 (once per board)
#   tools/build_wst169.sh [port] monitor          # build, flash, then the serial console (Ctrl-] quits)
#
# [port] defaults to the one /dev/cu.usbmodem* there is (macOS; the board's
# native USB). With none or several plugged in, name it. Written for the
# macOS bash 3.2 as well as Linux: no /dev/serial/by-id, no bash 4 features.
#
# Its own build directory and sdkconfig, so the AMOLED build (build/) and the
# Touch-LCD-2's (build_wst/) are untouched: sdkconfig.defaults, then
# sdkconfig.defaults.wst169 on top.
set -u
cd "$(dirname "$0")/../firmware"
BUILD=build_wst169
DEFAULTS=sdkconfig.defaults.wst169
PORT=""
ACTION=build
MODEL=0
for arg in "$@"; do
  case "$arg" in
    build|flash|monitor) ACTION=$arg ;;
    --model) MODEL=1 ;;
    /dev/*) PORT=$arg ;;
    *) echo "build_wst169: unknown argument $arg"; exit 1 ;;
  esac
done
# (this fork, 2026-10-17) export.sh names the Python env after the first
# python3 on PATH (3.13 on Viktor's Mac), but the one installed is py3.9's:
# point it there when nothing else says which, and that env exists.
PYENV_DIR=$HOME/.espressif/python_env/idf5.5_py3.9_env
if [ -z "${IDF_PYTHON_ENV_PATH:-}" ] && [ -d "$PYENV_DIR" ]; then export IDF_PYTHON_ENV_PATH=$PYENV_DIR; fi
IDF=${IDF_PATH:-$HOME/esp/v5.5.2/esp-idf}
. "$IDF/export.sh" > /dev/null 2>&1 || { echo "build_wst169: ESP-IDF export failed ($IDF)"; exit 1; }
# Every image is signed (sdkconfig.defaults, docs/OTA.md): without the key the
# build stops at the signing step. OTA is off on the 320x240 family, but the
# key is still the tank's one key - make it once if this disk has none.
if [ ! -f keys/ota_signing_key.pem ]; then
  bash ../tools/ota_key.sh || { echo "build_wst169: no signing key and ota_key.sh failed"; exit 1; }
fi
# The defaults only fill in what an sdkconfig lacks (build_cyd.sh): start the
# sdkconfig afresh whenever a defaults file is newer than it.
if [ -f "$BUILD/sdkconfig" ] && { [ sdkconfig.defaults -nt "$BUILD/sdkconfig" ] || [ "$DEFAULTS" -nt "$BUILD/sdkconfig" ]; }; then
  echo "build_wst169: a defaults file changed - regenerating $BUILD/sdkconfig"
  rm -f "$BUILD/sdkconfig"
fi
# reconfigure first, every time: the image's build id is the commit's, read
# while CMake configures (build_cyd.sh says why). The filter is build_cyd.sh's.
idf.py -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;$DEFAULTS" reconfigure build \
  2>&1 | grep -E "(^| )error: |^FAILED|ninja: build stopped|CMake Error|display_port_spi[.]c:[0-9]+:[0-9]+: warning: |binary size|build complete|Project build complete"
[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "build_wst169: BUILD FAILED - nothing flashed"; exit 1; }
[ "$ACTION" = build ] && exit 0
if [ -z "$PORT" ]; then
  n=0
  for p in /dev/cu.usbmodem*; do [ -e "$p" ] && { PORT=$p; n=$((n + 1)); }; done
  [ "$n" -eq 1 ] || { echo "build_wst169: $n /dev/cu.usbmodem* ports - name the port"; exit 1; }
fi
[ -e "$PORT" ] || { echo "build_wst169: no $PORT"; exit 1; }
idf.py -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -p "$PORT" -b 921600 flash 2>&1 | grep -E "verified|Hard resetting|rror" | tail -3
[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "build_wst169: FLASH FAILED"; exit 1; }
if [ "$MODEL" -eq 1 ]; then
  python -m esptool --chip esp32s3 -p "$PORT" -b 921600 write_flash 0x290000 ../model/out/model_q4.bin 2>&1 | grep -E "Wrote|verified|rror"
  [ "${PIPESTATUS[0]}" -eq 0 ] || { echo "build_wst169: MODEL WRITE FAILED"; exit 1; }
fi
[ "$ACTION" = monitor ] || exit 0
exec idf.py -B "$BUILD" -D SDKCONFIG="$BUILD/sdkconfig" -p "$PORT" monitor
