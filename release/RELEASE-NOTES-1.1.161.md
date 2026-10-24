# micro-js 1.1.161

Bluetooth LE on the CYD-S3: scan, pair BLE keyboards, mice and gamepads, and act as one.

## New
- **`ble`** (CYD-S3 only; check `board.hasBle`). `ble.scan()` lists nearby Bluetooth Low Energy devices.
  `ble.host` pairs a BLE keyboard, mouse or gamepad: keys arrive as `keyboard.on("key")`, a gamepad is
  `joystick.read(0)` with sticks, hat, buttons and analogue triggers, and a mouse moves an arrow on the screen
  whose left button is a touch. The device's HID report map is parsed, so standard devices need no table.
  `ble.hid` turns the board into a Bluetooth keyboard + mouse + gamepad for a phone or computer. See the README.
- **The host starts at boot** when something is paired from the board (`"bleHost": true / false` in
  `micro-js.json` overrides), reconnects by scanning when you press the device's button, and stays connected from
  app to app.
- **`joystick.read()` has `lt` and `rt`**, the analogue triggers (0 to 1).
- **Bruce has a Bluetooth sub menu:** scan, an on-screen keyboard, mouse, gamepad, an input test, raw reports and
  pairing.
- **`board.resetReason`, `board.resetCause`, `board.bootLog`** (ESP32 boards): why the board last reset, and the
  last eight resets; for tracking down power and crash problems.

## Changed
- **Bruce is easier on a touch screen:** a 34 px title bar with a big Back button and the page arrows, 40 px rows,
  swiping to scroll lists with a scroll bar on the right, WiFi and Bluetooth scans that show a count and page
  through every result, and big buttons along the bottom.

## Notes
- Bluetooth is built **only for the CYD-S3**. On the CoCo VGA board it took 47 KB of about 67 KB free memory and video
  and mp3 crashed, the Cardputer has no PSRAM, the classic CYD has no room, and the RP2350 has no radio.
- The ESP32-S3 does Bluetooth **LE only**. Classic-Bluetooth devices (Joy-Cons, the Switch Pro Controller, most
  Switch-style pads, older keyboards and mice) can't pair. The Xbox One S controller (model 1708) needs the firmware
  update that adds Bluetooth LE; the Series X|S controllers have it.
- Tested: scanning and the Bruce menus, and an Xbox One S controller as a gamepad. Not tested: the phone / computer
  side of `ble.hid`, BLE keyboards and mice paired to the board. The original Steam Controller pairs but only its
  default keyboard + mouse mode is seen (its gamepad mode needs Valve's own commands, not implemented).
- If the CYD-S3 resets by itself when a controller connects, try another USB port or a wall charger: the reset code
  in `board.bootLog` (`1` = power-on) points at a supply dip, which some PC USB ports and hubs cause.

## Files
- `micro-js-1.1.161-<board>-firmware.bin` (`.uf2` for the RP2350-PiZero): the firmware only; it keeps the
  apps and settings on the board.
- `micro-js-1.1.161-apps.zip`: every app, for every board (Bruce's Bluetooth tools are in `lib/bruce-ble.js`).
- `MICROJS_1-1-161.FLH`: for the ESP32-CoCo emulator's own Flash menu.

## Boards
cyd (limited: no PSRAM; music yes, video no), cyd2usb, cyd-s3 (the only board with Bluetooth), cardputer-adv,
coco-vga, rp2350-pizero. The AMOLED watch profile (`amoled-206`) has an image but has never run on a working screen.
