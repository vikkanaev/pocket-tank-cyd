#!/bin/bash
# ota_key.sh - the tank's OTA signing key (docs/OTA.md, "Safety").
#
# Images are verified on update against the public key digest carried by the
# RUNNING image's own signature block, so every build that goes on a tank by
# cable must be signed with the same key that signs releases, or that tank
# can never take an update. One key, kept out of git:
#
#   firmware/keys/ota_signing_key.pem   (RSA-3072, secure boot v2 format)
#
#   tools/ota_key.sh            # create it if missing (never overwrites)
#   tools/ota_key.sh --pub      # print the public key digest, for a report
#
# Back the key up somewhere that is not this disk. CI gets it as the
# repository secret OTA_SIGNING_KEY (the PEM's text), written to the same
# path before the build. Losing the key = a cable release for everyone.
set -euo pipefail
cd "$(dirname "$0")/.."
KEY=firmware/keys/ota_signing_key.pem
# (this fork, 2026-10-17) $IDF_PATH first, then ~/esp/esp-idf as before, then
# ~/esp/v5.5.2/esp-idf: the Mac the Touch-LCD-1.69 is built on has only the
# last, and tools/build_wst169.sh calls this script when the key is missing.
if [ -n "${IDF_PATH:-}" ]; then IDF=$IDF_PATH
elif [ -f ~/esp/esp-idf/export.sh ]; then IDF=~/esp/esp-idf
else IDF=~/esp/v5.5.2/esp-idf; fi
# export.sh is not written for set -eu (v5.5.2 stops in it, silently): source
# it with both off.
set +eu
. "$IDF/export.sh" > /dev/null 2>&1 || { echo "ota_key: ESP-IDF export failed ($IDF)"; exit 1; }
set -eu
if [[ "${1:-}" == "--pub" ]]; then
  [ -f "$KEY" ] || { echo "ota_key: no $KEY"; exit 1; }
  T=$(mktemp); espsecure.py digest_sbv2_public_key --keyfile "$KEY" --output "$T" > /dev/null; xxd -p "$T" | tr -d "\n"; echo; rm -f "$T"
  exit 0
fi
if [ -f "$KEY" ]; then echo "ota_key: $KEY exists - leaving it alone"; exit 0; fi
mkdir -p firmware/keys
espsecure.py generate_signing_key --version 2 --scheme rsa3072 "$KEY"
chmod 600 "$KEY"
echo "ota_key: wrote $KEY - BACK IT UP off this disk"
