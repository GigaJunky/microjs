# micro-js 1.1.88

Boards that talk to each other (ESP-NOW), browser-style drawing (canvas), and one file manager for
music, video, pictures and firmware.

## New
- **ESP-NOW** (ESP32 boards): `espnow.begin()`, `espnow.send(mac, data)`, `espnow.on("message", fn)`,
  `espnow.mac()`: messages straight from one board to another, no router. `espnow.js` is a radio
  version of `remote.js`: a shared LED colour mixer (move a slider on one board, every board's LED
  follows), Beep, and chat (type on a keyboard board, or the Say button on a touch board).
  Tested on the classic CYD, CYD-S3, CoCo VGA and Cardputer. Messages aren't encrypted.
- **`canvas`**: the browser's Canvas 2D API, built into the firmware (`canvas.getContext("2d")`:
  rectangles, paths, arcs, curves, fills, strokes, text, transforms, `save` / `restore`). `lcd` is
  unchanged and can be used beside it. `canvas.buffer(true)` draws off-screen so frames don't flicker
  (S3 CYD, Cardputer; not the classic CYD). `load("/lib/canvas.js")` adds `requestAnimationFrame`.
  `canvasdemo.js` runs at about 20 frames a second on the Cardputer and S3 CYD, about 10 on the classic
  CYD; `tools/canvas-demo.html` runs the same file unchanged in a browser.
- **Faster uploads**: while a file is uploading the board stops echoing every line and drawing it on its
  screen (it made uploads to the Cardputer and CoCo about 2.5 times slower). Reflash for it to apply.
- **Install the apps from a computer**: `node tools/upload.js /dev/ttyACM0 micro-js-1.1.88-apps.zip`
  (or a folder); the web IDE unpacks a dropped `.zip`. A `micro-js.json` already on the board is kept.

## Changed
- **The file manager does media and firmware**, and `mp3.js`, `video.js` (which scanned the card and
  kept playlists) and `flash.js` are gone. Browse the flash and the SD card by folder (nothing is scanned, no playlist
  files); a Type button (`f`) shows only music, video, pictures, apps or firmware files. `.avi` videos
  play full screen (a tap or Space pauses, a long press or Enter stops); a `.FLH` / `.bin` file's menu
  has "Install firmware...".
- **A flash button in the menu** (S3 boards): opens the file manager on the SD card showing only
  firmware files.
- **`.EFW`** (the newer ESP32-CoCo firmware): recognised and its release notes shown, but not
  installable: its image is encrypted. Use the `.FLH` of the same release.
- `neopix.js` stops with a message on the CoCo VGA board (its pins carry the picture).
- The simulator: `launch()` passes its argument, and the flash reports its own size so a missing SD
  card can be told from a present one.

## Files
- `micro-js-1.1.88-<board>-firmware.bin` (`.uf2` for the RP2350-PiZero): the firmware only; it keeps the
  apps and settings on the board.
- `micro-js-1.1.88-apps.zip`: every app, for every board.
- `MICROJS_1-1-88.FLH`: for the ESP32-CoCo emulator's own Flash menu.

## Boards
cyd (limited: no PSRAM), cyd2usb, cyd-s3, cardputer-adv, coco-vga, rp2350-pizero. The AMOLED watch
profile (`amoled-206`) has an image but has never run on a working screen. On the classic CYD memory is
very tight once the radio is up: scripts there can't be much bigger than 8 KB.
