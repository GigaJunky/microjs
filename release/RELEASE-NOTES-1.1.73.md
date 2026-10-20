# micro-js 1.1.73

A big step up from 1.0.1: a new board, a way to give boards without WiFi the network, and a lot of
memory and sound fixes for the small boards.

## New
- **Waveshare RP2350-PiZero** (HDMI): the whole system on a Raspberry Pi RP2350 - video through the
  mini HDMI port, USB keyboards and gamepads (hubs work), SD card, sound. Flash a `.uf2` (see
  `README.txt` in the release).
- **Network and clock through the computer** for boards with no WiFi: run `npm run bridge` (or open
  the web IDE) and `fetch`, `weather` and `clock` work unchanged.
- **MP3 and video players:** a "Looking for songs / videos..." screen while scanning; the result is
  saved as a playlist (`/sd/playlist.m3u`, `/sd/videos.m3u`) so the next start is instant, and `r`
  rescans. Opened from the file manager they play that file without searching.
- Video **sound on the Cardputer** (and the SD card at 20 MHz there).

## Fixes
- Classic CYD: the menu no longer fails with "out of memory", apps load again after the menu has run,
  and the list editor moved into its own app (`lib/listedit.js`).
- A `micro-js.json` shared on an SD card no longer drives the classic CYD's `speakerEnable` pin on
  the other boards (it silenced the S3 CYD's codec).
- `paint` uses a quarter of the memory; TFT_eSPI's uninitialised font pointer no longer hangs text.
- The dungeon and shooting games are left out of the Cardputer's menu: they need more memory than it has.

## Boards
cyd (limited: no PSRAM), cyd2usb, cyd-s3, cardputer-adv, coco-vga, rp2350-pizero.
The AMOLED watch profile (`amoled-206`) is in the source but untested and has no image here.

## Files
`-full` images replace everything on the board (apps and saves too); `-app` images only replace the
firmware. `MICROJS_1-1-73.FLH` is for the ESP32-CoCo emulator's own Flash menu.
