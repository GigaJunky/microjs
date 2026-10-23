# micro-js 1.1.124

A file manager that fits the classic CYD, music on the classic CYD, firmware install on the classic CYD,
and fixes for the memory the classic's SD card was holding.

## New
- **mp3 plays on the classic CYD.** The decoder's 16 KB of scratch space used to sit on its task stack,
  which the classic's heap has no block big enough for; it is now packed into the few free blocks it has.
  (Video still doesn't start there: the JPEG decoder needs one 18 KB block, the largest free is 17 KB.)
- **Firmware install on the classic CYD.** Its flash gets two 1.5 MB app slots (`partitions/cyd_ota.csv`), so
  the file manager's "Install firmware..." works there too. The apps and settings stay where they were.
  Flash the new `-cyd-` image once from a computer to get the layout.
- **`fs.dir(path, skip, limit)`** reads a page of a big folder; the file manager shows 40 entries at a time
  with a "More..." row.

## Changed
- **The file manager is small enough for the classic CYD** (22 KB down to 10 KB). Its menu (`fmenu.js`), the
  picture and text viewers (`fview.js`), the mp3 / video player (`fplay.js`), copy / move / delete / rename /
  new folder (`fops.js`) and the firmware install screens (`finstall.js`) are separate apps in `lib/` that it
  launches, each starting with a clean heap; where you were is kept in `/files.state`. Replaces
  `lib/videoplay.js` and `lib/fwinstall.js`. Music no longer keeps playing while you browse.
- **espnow, neopix and other apps ran out of memory on the classic CYD** once its SD card was in: a
  mounted card holds about 30 KB of heap. A launch on a small board (or one using WiFi) that never names
  `/sd` in its code or argument now lets the card go, and any other launch mounts it again. As a result such
  an app reads `micro-js.json` from the flash, not the card.
- Reading an app file keeps a 64 byte margin instead of 512; `sys.installFirmware` says why an update
  couldn't start.

## Files
- `micro-js-1.1.124-<board>-firmware.bin` (`.uf2` for the RP2350-PiZero): the firmware only; it keeps the
  apps and settings on the board.
- `micro-js-1.1.124-apps.zip`: every app, for every board.
- `MICROJS_1-1-124.FLH`: for the ESP32-CoCo emulator's own Flash menu.

## Boards
cyd (limited: no PSRAM; music yes, video no), cyd2usb, cyd-s3, cardputer-adv, coco-vga, rp2350-pizero. The
AMOLED watch profile (`amoled-206`) has an image but has never run on a working screen. The changes above to
mp3 and the SD card apply to boards without PSRAM; They have only been tried on the classic CYD. On the classic
memory is still very tight once the radio is up: scripts there can't be much bigger than 8 KB.
