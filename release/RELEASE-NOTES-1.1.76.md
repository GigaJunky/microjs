# micro-js 1.1.76

NeoPixel strips and matrices, and a simpler release: one firmware image per board and one apps
bundle for all of them.

## New
- **NeoPixel app (`neopix.js`)** drives a WS2812 strip or matrix from a GPIO. Pick the pin, the
  number of LEDs (1-512), the grid (columns; 1 is a plain strip), the wiring (zigzag or all rows the
  same way), the pattern, colour, speed and brightness; the screen shows a live preview. Patterns:
  Solid, Rainbow, Chase, Scanner, Breathe, Twinkle, Fire, Plasma, Ripple, Rain, Wipe, Strobe.
  Settings are kept in `/neopix.json`. ESP32 boards only.
- **`neopixel(pin, colors, [order])`** for your own scripts: `colors` is an array of `0xRRGGBB`
  numbers, `order` is `"GRB"` (default) or `"RGB"`. Returns `true` when sent.
- **Install the apps from a computer:** `node tools/upload.js /dev/ttyACM0 micro-js-1.1.76-apps.zip`
  (or a folder such as `data/`), with sub-folders. A `micro-js.json` already on the board is kept.
  The web IDE unpacks a dropped `.zip` the same way.

## Wiring notes
- Classic CYD: the strip goes on **GPIO 27** (the CN1 connector). GPIO 22 is the red LED on some
  boards.
- CYD-S3: **GPIO 2** works on the header (tested). GPIO 6 doesn't: the audio codec drives it and the
  strip sticks at white. The board's own RGB LED is a WS2812 on GPIO 42.
- Power a long strip from its own 5 V supply with a common ground; the boards only give 3.3 V.

## Files
- `micro-js-1.1.76-<board>-firmware.bin` (or `.uf2` for the RP2350-PiZero): the firmware only. It
  keeps the apps and settings on the board, so it installs and updates.
- `micro-js-1.1.76-apps.zip`: every app, for every board.
- The `-full` and `-app` images are gone. First install: flash the firmware, then install the apps
  zip. `MICROJS_1-1-76.FLH` is for the ESP32-CoCo emulator's own Flash menu.

## Boards
cyd (limited: no PSRAM), cyd2usb, cyd-s3, cardputer-adv, coco-vga, rp2350-pizero. NeoPixel output was
tested on the classic CYD and the CYD-S3; the other boards are untested for it. The AMOLED watch
profile (`amoled-206`) has an image again but has never run on a working screen.
