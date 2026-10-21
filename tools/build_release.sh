#!/usr/bin/env bash
# Build web-flasher images for every board profile into release/:
#   micro-js-<version>-<board>-firmware.bin  firmware + bootloader (flash at 0x0); keeps the scripts on the board
#   micro-js-<version>-apps.zip           the scripts for every board (unzip, or drop it on the web IDE)
# RP2350 boards get .uf2 files instead (-firmware.uf2): drag one onto the drive the board
# shows when plugged in with BOOT held.
# <version> comes from src/version.h (MICROJS_VERSION).
# The apps zip is built from a copy of data/ with a blank micro-js.json (and without the high
# scores and settings a board saves there), so your own WiFi password and API keys never end up in
# a release.
#
#   tools/build_release.sh              all boards
#   tools/build_release.sh cyd          just one
# The coco-vga profile also gets release/MICROJS.FLH, for the ESP32-CoCo emulator's Flash menu.
# (PROJECT=dir builds another PlatformIO project that shares data/; images still go to release/.)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
cd "${PROJECT:-.}"

PIO=${PIO:-$(command -v pio || echo ~/.local/bin/pio)}
CORE=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}
PY=$(head -1 "$(command -v "$PIO")" | sed 's/^#!//')
ESPTOOL="$CORE/packages/tool-esptoolpy/esptool.py"
BOOT_APP0="$CORE/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
ENVS=${*:-$(grep -oP '^\[env:\K[^\]]+' platformio.ini | grep -v -- '-template$' | tr '\n' ' ')}

STAGE=$(mktemp -d)   # the data/ folder that becomes the apps zip (.js + a blank config)
BINS=$(mktemp -d)    # the binaries of each env
trap 'rm -rf "$STAGE" "$BINS"' EXIT
# the apps and everything they load (pages, lib/), but none of a board's own state or settings
cp -r "$ROOT"/data/. "$STAGE"/
rm -f "$STAGE"/{micro-js,esp32-js,cyd-js,sound,touchcal,launcher}.json "$STAGE"/*.hi "$STAGE"/*.part
cat > "$STAGE/micro-js.json" <<'EOF'
{
  "wifi": { "ssid": "", "pass": "" },
  "utcOffsetHours": 0,
  "location": { "name": "Berlin", "lat": 52.52, "lon": 13.41 },
  "weatherRefreshMinutes": 10,
  "stocks": { "apiKey": "", "symbols": ["AAPL", "MSFT"], "refreshMinutes": 5 }
}
EOF

mkdir -p "$ROOT/release"
VERSION=$(sed -n 's/.*MICROJS_VERSION "\(.*\)".*/\1/p' "$ROOT/src/version.h")
[ -n "$VERSION" ] || { echo "can't read the version from src/version.h" >&2; exit 1; }
# the apps for every board: one zip (scripts are the same everywhere); the web IDE unpacks it
rm -f "$ROOT/release/micro-js-$VERSION-apps.zip"
(cd "$STAGE" && zip -qr -X "$ROOT/release/micro-js-$VERSION-apps.zip" .)
echo "== release/micro-js-$VERSION-apps.zip"
for env in $ENVS; do
  echo "== $env"
  dir=$BINS/$env; mkdir -p "$dir"
  "$PIO" run -e "$env" -s
  envcfg=$(sed -n "/^\[env:$env\]/,/^\[/p" platformio.ini)
  if grep -q 'platform-raspberrypi' <<<"$envcfg"; then  # RP2350: UF2 files
    cp .pio/build/$env/firmware.bin "$dir"/
    node "$ROOT/tools/make-uf2.js" "$ROOT/release/micro-js-$VERSION-$env-firmware.uf2" "$dir/firmware.bin@0x10000000" >/dev/null
    echo "   release/micro-js-$VERSION-$env-firmware.uf2"
    continue
  fi
  cp .pio/build/$env/{bootloader,partitions,firmware}.bin "$dir"/
  # grep under `set -e` must not abort the script when it finds nothing, so test explicitly
  chip=esp32; boot=0x1000
  if grep -q 'esp32-s3' <<<"$envcfg"; then chip=esp32s3; boot=0x0; fi
  size=4MB
  if grep -q 'flash_size = 8MB' <<<"$envcfg"; then size=8MB; fi
  if grep -q 'flash_size = 16MB' <<<"$envcfg"; then size=16MB; fi
  args=(--chip "$chip" merge_bin --flash_mode dio --flash_freq 40m --flash_size "$size"
        "$boot" "$dir/bootloader.bin" 0x8000 "$dir/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$dir/firmware.bin")
  "$PY" "$ESPTOOL" "${args[@]}" -o "$ROOT/release/micro-js-$VERSION-$env-firmware.bin" >/dev/null
  echo "   release/micro-js-$VERSION-$env-firmware.bin"
  if [[ $env == coco* ]]; then  # the ESP32-CoCo emulator's flash menu format (see tools/make-flh.js)
    node "$ROOT/tools/make-flh.js" "$dir/firmware.bin" "$ROOT/release/MICROJS_${VERSION//./-}.FLH" \
      "micro-js $(sed -n 's/.*MICROJS_VERSION "\(.*\)".*/\1/p' "$ROOT/src/version.h") for the ESP32-CoCo board: JavaScript interpreter, REPL and app launcher on VGA,
with USB keyboard and joystick. F12 or Escape: menu. Ctrl+C: console.
To go back to the CoCo emulator, open the file manager (files.js), pick its .FLH file on the SD card and choose Install firmware." | sed 's/^/   /'
  fi
done
