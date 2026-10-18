#!/usr/bin/env bash
# Build web-flasher images for every board profile into release/:
#   micro-js-<version>-<board>-full.bin   firmware + bootloader + example scripts (flash at 0x0)
#   micro-js-<version>-<board>-app.bin    firmware + bootloader only; keeps the scripts on the board
# RP2350 boards get .uf2 files instead (-full.uf2 / -app.uf2): drag one onto the drive the board
# shows when plugged in with BOOT held.
# <version> comes from src/version.h (MICROJS_VERSION).
# The scripts image is built from a copy of data/ with a blank micro-js.json (and without the high
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

STAGE=$(mktemp -d)   # the data/ folder that becomes the filesystem image (.js + a blank config)
BINS=$(mktemp -d)    # app binaries stashed per env (buildfs and run clean each other's output)
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
for env in $ENVS; do
  echo "== $env"
  # `buildfs` and `run` each clean the other's artifacts out of .pio/build/<env>, so copy the
  # app binaries aside after the app build, then the filesystem image after buildfs.
  dir=$BINS/$env; mkdir -p "$dir"
  "$PIO" run -e "$env" -s
  envcfg=$(sed -n "/^\[env:$env\]/,/^\[/p" platformio.ini)
  if grep -q 'platform-raspberrypi' <<<"$envcfg"; then  # RP2350: UF2 files
    cp .pio/build/$env/firmware.{bin,elf} "$dir"/
    PLATFORMIO_DATA_DIR="$STAGE" "$PIO" run -e "$env" -t buildfs -s
    cp .pio/build/$env/littlefs.bin "$dir"/
    # the filesystem's flash address, from the firmware's own linker symbols
    NM=$(ls "$CORE"/packages/toolchain-rp2040-earlephilhower/bin/arm-none-eabi-nm)
    fsoff=0x$("$NM" "$dir/firmware.elf" | awk '$3 == "_FS_start" {print $1}')
    node "$ROOT/tools/make-uf2.js" "$ROOT/release/micro-js-$VERSION-$env-app.uf2" "$dir/firmware.bin@0x10000000" >/dev/null
    node "$ROOT/tools/make-uf2.js" "$ROOT/release/micro-js-$VERSION-$env-full.uf2" "$dir/firmware.bin@0x10000000" \
      "$dir/littlefs.bin@$fsoff" >/dev/null
    echo "   release/micro-js-$VERSION-$env-app.uf2, release/micro-js-$VERSION-$env-full.uf2 (scripts at $fsoff)"
    continue
  fi
  cp .pio/build/$env/{bootloader,partitions,firmware}.bin "$dir"/
  PLATFORMIO_DATA_DIR="$STAGE" "$PIO" run -e "$env" -t buildfs -s
  cp .pio/build/$env/littlefs.bin "$dir"/
  # grep under `set -e` must not abort the script when it finds nothing, so test explicitly
  chip=esp32; boot=0x1000
  if grep -q 'esp32-s3' <<<"$envcfg"; then chip=esp32s3; boot=0x0; fi
  size=4MB
  if grep -q 'flash_size = 8MB' <<<"$envcfg"; then size=8MB; fi
  if grep -q 'flash_size = 16MB' <<<"$envcfg"; then size=16MB; fi
  # LittleFS offset = the "spiffs" partition's offset. Decode it from the build's own
  # partition table (handles profiles that inherit their partitions via `extends`).
  fsoff=$("$PY" "$CORE/packages/framework-arduinoespressif32/tools/gen_esp32part.py" "$dir/partitions.bin" 2>/dev/null |
          awk -F, '$1 ~ /spiffs/ {gsub(/ /,"",$4); print $4}')
  args=(--chip "$chip" merge_bin --flash_mode dio --flash_freq 40m --flash_size "$size"
        "$boot" "$dir/bootloader.bin" 0x8000 "$dir/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$dir/firmware.bin")
  "$PY" "$ESPTOOL" "${args[@]}" -o "$ROOT/release/micro-js-$VERSION-$env-app.bin" >/dev/null
  "$PY" "$ESPTOOL" "${args[@]}" "$fsoff" "$dir/littlefs.bin" -o "$ROOT/release/micro-js-$VERSION-$env-full.bin" >/dev/null
  echo "   release/micro-js-$VERSION-$env-app.bin, release/micro-js-$VERSION-$env-full.bin (scripts at $fsoff)"
  if [[ $env == coco* ]]; then  # the ESP32-CoCo emulator's flash menu format (see tools/make-flh.js)
    node "$ROOT/tools/make-flh.js" "$dir/firmware.bin" "$ROOT/release/MICROJS_${VERSION//./-}.FLH" \
      "micro-js $(sed -n 's/.*MICROJS_VERSION "\(.*\)".*/\1/p' "$ROOT/src/version.h") for the ESP32-CoCo board: JavaScript interpreter, REPL and app launcher on VGA,
with USB keyboard and joystick. F12 or Escape: menu. Ctrl+C: console.
To go back to the CoCo emulator, run flash.js and pick its .FLH file." | sed 's/^/   /'
  fi
done
# leave .pio's filesystem image matching data/ again, for `pio run -t uploadfs`
for env in $ENVS; do "$PIO" run -e "$env" -t buildfs -s >/dev/null; done
