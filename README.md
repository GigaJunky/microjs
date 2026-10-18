# micro-js — JavaScript for microcontroller display boards

A JavaScript interpreter written from scratch for microcontroller boards with a screen. It started
on the **ESP32-2432S028R** ("Cheap Yellow Display", CYD): a 2.8" 320×240 ILI9341 touchscreen, RGB
LED, speaker output, light sensor and SD slot on one ESP32 board. It also runs on the ESP32-S3
version of the CYD, the M5Stack Cardputer ADV, mast4rbug's ESP32-CoCo VGA board and the Waveshare
RP2350-PiZero (HDMI output, USB keyboards and gamepads), and other boards can be added (see
"Supported boards").

You can type JavaScript into a serial REPL and see it on the screen right away. You can also
save scripts to flash or SD and have one start at boot.

```js
lcd.fillScreen(lcd.NAVY);
lcd.setTextColor(lcd.YELLOW);
lcd.drawString("Hello, CYD!", 90, 100, 4);
touch.on("down", ({ x, y }) => lcd.fillCircle(x, y, 5, lcd.RED));
setInterval(() => led(0, Math.random() * 40, 0), 200);
```

## Features

- **Interpreter core** (portable C++17, about 5,000 lines): a tree-walking evaluator with a
  precise mark-and-sweep garbage collector. The same code builds for the ESP32 and for a desktop
  machine.
- **Modern JavaScript subset:**
  - `let`/`const`/`var`, arrow functions, closures, classes (`extends`, `super`, static and
    instance fields, getters/setters)
  - destructuring, spread/rest, default parameters, template literals
  - optional chaining `?.`, nullish `??`, `**`, `try`/`catch`/`finally`, `switch`,
    `for…of`/`for…in`
- **Standard library:**
  - `Object`, `Array` (map/filter/reduce/sort/splice…), `String`, `Number`, `Math`
  - `JSON.parse`/`stringify`, `Map`, `Set`, `Error` types
  - `setTimeout`/`setInterval`, `console.log` with Node-style object printing
- **Hardware:**
  - display drawing (TFT_eSPI underneath) and off-screen sprites for flicker-free animation
  - JPEG, PNG and BMP pictures, scaled to fit
  - touch events (with a calibration app), keyboards, USB gamepads
  - RGB LED, backlight dimming, speaker tones, light sensor, battery level, GPIO
  - MP3 music and Motion-JPEG video with sound (I2S audio, or a plain speaker pin used as an
    8-bit DAC)
  - LittleFS flash and SD card files: copy, move, folders, free space
  - WiFi, HTTP(S) and NTP time
- **Apps:** a launcher with volume and battery, a file manager, music and video players, a
  picture viewer, a JavaScript editor, games, a weather/stocks/news dashboard, and a firmware
  installer that switches between micro-js and other firmware from the SD card.
- **Serial REPL:** multi-line input, ↑ history, Ctrl-C to interrupt runaway scripts, and
  dot-commands to manage files.
- **Desktop simulator:** runs scripts on your PC with a fake screen (saves PNG screenshots) and
  scripted touch input.

## Supported boards

| Profile (`-e ...`) | Board | Status |
|---|---|---|
| `cyd` | ESP32-2432S028R "Cheap Yellow Display" (one USB port) | tested; limited: no PSRAM, so the big and WiFi apps may run out of memory |
| `cyd2usb` | CYD variant with two USB ports (ST7789 panel) | builds; same board otherwise |
| `cyd-s3` | ESP32-S3 "CYD" 2.8" (LCDWiki ES3C28P): 8 MB PSRAM, 16 MB flash, ILI9341V IPS, FT6336G capacitive touch, ES8311 audio, SD (SDIO), WS2812, battery | tested: display, touch, sound, MP3, video, pictures, SD, battery |
| `cardputer-adv` | M5Stack Cardputer ADV: ESP32-S3, 240×135 ST7789, 56-key keyboard, ES8311 audio, battery, no touch, no PSRAM | tested: display, keyboard, sound, LED, SD |
| `coco-vga` | [ESP32-CoCo](https://github.com/mast4rbug/ESP32-CoCo) board: ESP32-S3, VGA output, USB keyboards and gamepads, SD, PWM sound | tested (see "The ESP32-CoCo VGA board") |
| `rp2350-pizero` | Waveshare RP2350-PiZero: RP2350B (520 KB RAM, 16 MB flash), mini HDMI out, USB keyboards and gamepads (hubs work), microSD, PWM sound on a header pin; **no WiFi or Bluetooth** | tested: HDMI, USB keyboard and gamepad, SD, flash, network and clock through the computer (see "The RP2350-PiZero") |
| `esp32s3-28-template` | other ESP32-S3 2.8" boards with 8 MB PSRAM, ILI9341 + FT6336U capacitive touch (Hosyond, Freenove, MELIFE style) | template: pins are placeholders until a board is in hand |

Each profile is a `[env:...]` section in `platformio.ini`. It holds the display settings
(TFT_eSPI flags) and `BOARD_*` flags for the pins and parts; `src/board.h` lists every flag,
with the CYD's values as defaults. Boards with PSRAM automatically get large memory regions
there and a deeper interpreter stack.

To add a board: copy the closest profile, fill in its pins from the board's documentation,
build it, then check display, touch (direction and rotation), LED, SD and sound on the device.
Supported parts: ILI9341 / ST7789 / ST7796 and other TFT_eSPI displays on SPI, or your own screen
class (`BOARD_SCREEN_HEADER`, as the VGA board does); XPT2046 resistive or FT6336U capacitive
touch; the Cardputer's TCA8418 keyboard, or keys from another driver (`hwKeyEvent`); RGB LED on
three PWM pins, one WS2812 or a two-colour LED; a PWM speaker, or I2S with an optional ES8311
codec and amplifier pin; analog light sensor; battery voltage on an ADC pin; SD card on SPI or
SD_MMC (1-bit). Code only one board needs goes in `src/boards/<board>/` and `lib/`, built only by
that profile (see `[env]` in `platformio.ini`).

Scripts can see what the board has through `board`:
`board.name`, `board.touch` (`"resistive"`, `"capacitive"` or `"none"`), `board.hasTouch`, `board.hasKeyboard`,
`board.hasLed`, `board.hasSpeaker`, `board.hasMusic` (MP3 and video sound),
`board.hasLight`, `board.hasSd`, `board.hasBattery`, `board.hasWifi`, `board.psram` (bytes),
`board.chip` (`"esp32"`, `"esp32s3"` or `"rp2350"`). The launcher leaves out apps marked
`@needs-wifi` on boards without WiFi and `@needs-esp32` on other chips.

## Build and flash

You need [PlatformIO](https://platformio.org/) (VS Code extension or `pip install platformio`).
`-e cyd` below picks the board profile; it's the default, so you can leave it out for the CYD.

```bash
pio run -t upload
```

Then flash the example scripts and the touch launcher (`data/` → on-board flash):

```bash
pio run -t uploadfs
```

Open the REPL:

```bash
pio device monitor
```

For another board add its profile to each command, e.g. `pio run -e cyd-s3 -t upload`. If your
CYD has **two USB ports** (the "CYD2USB" variant with an ST7789 panel), use `-e cyd2usb`. If the
colours look inverted, try `lcd.invert(true)`.

**Version numbers.** The version (`src/version.h`, shown at boot, in the launcher and as
`sys.version`) goes up by itself: before each build, `tools/auto-version.js` checks whether the code
that goes onto a board (`src/`, `lib/`, `data/`, `platformio.ini`) changed since the version was set,
and if so bumps the last number (1.0.0 → 1.0.1). Building several boards bumps once. For a feature
release set it yourself (1.1.0, or 2.0.0 when scripts would break); later changes count up from
there. `tools/version-state.json` remembers the last fingerprint - keep it with the code.

**Without PlatformIO:** ready-made images for every board are on the releases page (built with
`tools/build_release.sh`). Flash one at address 0 with
[esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge: `micro-js-<version>-<board>-full.bin` has
the firmware and the apps, `-app.bin` only the firmware (keeps the apps on the board). The
RP2350-PiZero takes `.uf2` files instead: hold its BOOT button while plugging it into a computer,
and drag `micro-js-<version>-rp2350-pizero-full.uf2` (or `-app.uf2`) onto the drive that appears.

## Using it

At boot the board shows a splash screen and runs `/sd/main.js` if that exists, otherwise
`/main.js` from flash. The bundled `main.js` is a launcher listing every script on the flash and
the SD card, with sound (mute and volume) and battery controls along the top. (A `main.js` on the
SD card overrides it - delete or rename an old one when you update.) **Press the BOOT button** at
any time to stop the running script and return to the menu with a fresh interpreter. **Hold BOOT
while resetting** to skip `main.js` altogether. On keyboard boards **Escape** (or F12) returns to
the menu and **Ctrl+C** stops the app and shows the console; on boards with a screen the console
also appears there when nothing is running, and `.edit FILE` opens a JavaScript editor.

REPL commands:

| Command | What it does |
|---|---|
| `.help` | list commands |
| `.ls [dir]` | list files (`.ls /sd` for the SD card) |
| `.cat FILE` | print a file |
| `.run FILE` | run a script |
| `.save FILE` | paste a file's contents; finish with a line containing only `.end` (or Ctrl-D) |
| `.rm FILE` | delete a file |
| `.mem` | heap usage |
| `.stop` | stop all timers and touch handlers |
| `.reset` | fresh interpreter (forget all variables) |
| `.reboot` | restart the board |

**Ctrl-C** interrupts a running script, even `while (true) {}`, and stops its timers and
handlers. JavaScript `try/catch` cannot swallow it.

### The web IDE (edit, upload and flash from the browser)

Open **`tools/web-ide.html`** in Chrome or Edge on a desktop computer - straight from the file, or
(if Chrome won't allow Web Serial for a file) from the little web server in this project:
`node tools/serve.js` (or `npm run ide`), then http://localhost:8000/tools/web-ide.html. Plug the
board in by USB and press **Connect**. Nothing to install:

- **Files:** browse the flash and the SD card; open, run, download, rename and delete files; new
  files and folders.
- **Editor:** JavaScript with syntax colours; **Ctrl+S** saves, **Ctrl+Enter** saves and runs.
- **Upload:** any file - scripts, pictures, MP3s, videos - with the button or by dropping files on
  the list (about 5 KB/s on the S3 boards; slower through the classic CYD's USB-serial chip).
- **Console:** everything the board prints, and a line to type JavaScript; Stop app and Launcher
  buttons.
- **Flash firmware:** writes a `.bin` (e.g. a release image at 0x0) with Espressif's esptool-js.

It talks to the board through the ordinary REPL, using `fs.writeBase64` / `fs.readBase64` for
files, so the board needs firmware 1.0.0 or later.

### Uploading scripts from your computer

```bash
npm install                                                   # once: gets the serialport package
node tools/upload.js /dev/ttyUSB0 myapp.js --run              # upload + run, streams output
node tools/upload.js /dev/ttyUSB0 myapp.js --as main.js       # make it the boot script
node tools/upload.js /dev/ttyACM0 song.mp3 photo.jpg --to /sd/media   # any file, any folder
```

(The port is `COM5` or similar on Windows.) Files go to the board's flash unless `--to` names a
folder - `/sd/...` is the SD card, and missing folders are made. Any file works, text or binary. The
web IDE (`tools/web-ide.html`) does the same from a browser.

## Hardware API

Colours are RGB565 numbers. Use the constants (`lcd.RED`, `lcd.NAVY`…), `lcd.color(r, g, b)`,
or `"#ff8800"` / `"#f80"` strings. The default orientation is landscape, 320×240.

### `lcd` — the screen

| Call | Notes |
|---|---|
| `fillScreen(c)` | |
| `drawPixel(x, y, c)` | |
| `readPixel(x, y)` | |
| `drawLine(x0, y0, x1, y1, c)` | |
| `drawWideLine(x0, y0, x1, y1, width, c, [bg])` | anti-aliased |
| `drawFastHLine(x, y, w, c)` | |
| `drawFastVLine(x, y, h, c)` | |
| `drawRect` / `fillRect(x, y, w, h, c)` | |
| `drawRoundRect` / `fillRoundRect` / `fillSmoothRoundRect(x, y, w, h, r, c)` | |
| `drawCircle` / `fillCircle` / `fillSmoothCircle(x, y, r, c, [bg])` | |
| `drawEllipse` / `fillEllipse(x, y, rx, ry, c)` | |
| `drawTriangle` / `fillTriangle(x0, y0, x1, y1, x2, y2, c)` | |
| `drawArc(x, y, r, innerR, startDeg, endDeg, c, [bg])` | 0° = 6 o'clock, clockwise |
| `drawString(text, x, y, [font])` | returns the width in pixels; fonts 1, 2, 4, 6, 7 (7-segment), 8 |
| `setTextColor(fg, [bg])` | a `bg` fills behind the text, so it overwrites cleanly |
| `setTextSize(n)` | |
| `setTextFont(n)` | |
| `setTextDatum(d)` | anchor point: `lcd.TL TC TR ML MC MR BL BC BR` |
| `setTextPadding(px)` | blank width so shorter text erases longer old text |
| `setCursor(x, y)`, `print(...)`, `println(...)` | |
| `textWidth(s, [font])`, `fontHeight([font])` | |
| `width()`, `height()` | |
| `color(r, g, b)` | |
| `setRotation(0..3)` | touch coordinates follow the rotation |
| `getRotation()` | |
| `invert(bool)` | |
| `backlight(0..255)` | |
| `createSprite(w, h, [depth])` | see below |

**Sprites** are off-screen images with the same drawing methods. Draw into one, then
`sprite.push(x, y, [transparentColor])`; call `sprite.free()` when you're done. Memory is
w×h×depth/8 bytes and must be one contiguous block. Once a script is running, the largest
free block is typically only about 20 KB, so keep sprites around 10–15 KB. For a bigger
area, draw it in horizontal bands through one small sprite, as `data/bounce.js` does.

### `touch`

- `touch.on("down" | "move" | "up", ({x, y, z}) => …)` registers an event handler (one per
  event); `touch.off([event])` removes it.
- `touch.read()` polls once and returns `{x, y, z}` or `null`.
- `touch.raw()` gives the controller's own coordinates.
- `touch.calibrate([a, b, c, d, e, f], rotation)` sets the mapping from raw to screen
  (x = a·rawX + b·rawY + c, y = d·rawX + e·rawY + f, at that rotation); `touch.calibrate(null)`
  goes back to the built-in one. You don't need to work these out: run **`calibrate.js`**, touch
  five targets, check, and Save - it's stored in `/touchcal.json` and loaded at every start.
  (The resistive CYD's old form, `touch.calibrate(xMin, xMax, yMin, yMax)`, still works.)

### `joystick` (USB gamepads: VGA board, RP2350-PiZero)

- `joystick.read(n = 0)` returns `{connected, x, y, x2, y2, hat, buttons, raw}`:
  - `x`/`y` and `x2`/`y2` are the two sticks, from −1 to 1.
  - `hat` is the D-pad: −1 when centred, otherwise 0–7 clockwise from up.
  - `buttons` is a bit mask, with button 1 as bit 0.
- `joystick.count()` gives the number of pads connected (up to two).
- `joystick.on("press" | "release", ({joystick, button}) => …)` fires on button changes;
  `joystick.off([event])` removes the handler.
- `joystick.keys(false)` stops the D-pad and buttons from also acting as arrow keys, Space and
  Enter. Use it when a game reads the pad itself. This is reset for each app.
- Each pad's layout is learned when you plug it in, from its first (resting) report: bytes
  resting in the middle are sticks, and a nibble resting at 15 or 8 is the D-pad. Everything
  else is a button. Don't hold anything while plugging a pad in.

### `keyboard` (Cardputer, USB keyboards)

- `keyboard.on("key" | "keyup", ({key, shift, fn, ctrl, opt, alt}) => …)` registers a handler;
  `keyboard.off([event])` removes it. `key` is the character (`"a"`, `"A"` with Shift, `" "`),
  or `"Enter"`, `"Backspace"`, `"Tab"`. Fn + `;` `,` `.` `/` sends `"ArrowUp"`, `"ArrowLeft"`,
  `"ArrowDown"`, `"ArrowRight"`, and Fn + `` ` `` sends `"Escape"`. A held key repeats.
- `keyboard.held()` lists the keys down right now. Games poll it every frame for smooth movement.
- The examples treat `;` `,` `.` `/` (the arrows printed on the keys) as arrows even without Fn.
- USB keyboards (VGA board) send the browser's key names: `"a"`, `"Enter"`, `"ArrowUp"`, `"F5"`...
- Escape and Ctrl+C belong to the firmware (back to the menu, stop the app) unless an app calls
  `keyboard.capture(true)` to receive them itself, as the editor and file manager do.
- On boards without a keyboard, `keyboard` exists but never fires. Use `board.hasKeyboard` and
  `board.hasTouch` to pick the controls.

### Board I/O

| Call | Notes |
|---|---|
| `led(r, g, b)` | RGB LED, 0–255 per channel (it's bright; 20–40 is plenty) |
| `backlight(0..255)` | |
| `tone(freqHz, [ms])`, `noTone()` | speaker connector; non-blocking |
| `light()` | light sensor, raw 0–4095 |
| `pinMode(pin, INPUT \| OUTPUT \| INPUT_PULLUP \| INPUT_PULLDOWN)` | for the free GPIOs on the expansion connectors (e.g. 22, 27, 35) |
| `digitalWrite(pin, v)`, `digitalRead(pin)`, `analogRead(pin)` | |
| `millis()`, `micros()` | |
| `delay(ms)` | blocking, but Ctrl-C still works; prefer timers |
| `sys.heap()` | `{free, minFree, largest, js}` |
| `sys.cpuMhz()` | |
| `sys.restart()` | |
| `sys.battery()` | `{volts, percent, charging, usb}`, or `null` without a battery sense pin |
| `sys.firmwareInfo(path)`, `sys.installFirmware(path)` | check / install a `.FLH` or `.bin` firmware file (see `flash.js`) |
| `sys.i2cScan()`, `sys.i2cRead(addr, reg)`, `sys.i2cWrite(addr, reg, value)` | the board's I2C bus (touch, keyboard, codec) |

### Sound, music and video

- `audio.volume(0..100)` and `audio.mute(true|false)` set the level of everything (tones, music,
  video); the launcher's controls save them in `/sound.json`. On a PWM beeper only mute applies.
- `audio.play(path)` plays an MP3 (on its own core, so the app keeps running). Through an I2S
  DAC or codec it's full quality; on boards with only a speaker pin (the CYD, the CoCo VGA board)
  the pin becomes an 8-bit DAC - PWM at 156.25 kHz, its duty set 39062.5 times a second, locked to
  the same clock so there's no whistle - mono, about AM-radio quality.
  `audio.pause([on])`, `audio.stop()`, `audio.status()` →
  `{playing, paused, path, seconds, duration, rate, kbps, channels, error}`.
- `video.play(path)` plays an AVI with Motion-JPEG video and MP3 sound, centred and shrunk to fit;
  `video.pause([on])` (returns once the screen is free to draw on), `video.stop()`,
  `video.status()` → `{playing, paused, frame, frames, shown, dropped, fps, width, height,
  seconds, duration, sound, error, ended}`. Don't draw while a video plays. The CYD-S3 plays
  320×180 at 30 fps and 320×240 at about 24 fps; frames that would be late are skipped so the
  sound never stutters.
- **Finding the songs and videos:** `mp3.js` and `video.js` look through the SD card (the top level
  and two folders down), which takes seconds on a card with many files (about 5 s on a media card, a
  minute on a card holding a whole system image). A "Looking for songs / videos..." screen shows the
  folder being listed and the count so far. The result is saved as a playlist on the card -
  `/sd/playlist.m3u` for music, `/sd/videos.m3u` for video, ordinary M3U files - and the next start
  reads that instead (a fraction of a second). The first line records how full the card is
  (`fs.space`, which answers at once), so the list is rebuilt by itself when files were added or
  removed; on the RP2350, where that figure takes too long to read, the saved list is used until you
  rescan. **`r`** (or tapping the title on a touch screen) rescans the card whenever you want. Started
  with one file (from the file manager), the players don't search at all: they play it, and **`l`**
  (or the same tap) brings up the whole list. The shared code is `lib/medialist.js`.
- Make videos with **`tools/convert-videos.sh`** on a computer (needs ffmpeg). It converts
  `.mp4`, `.mkv`, `.mov`, `.webm`... files or whole folders, picking the size and frame rate for
  the board, and can copy them straight to the SD card:
  `tools/convert-videos.sh -c /media/$USER/SDCARD/videos ~/Videos` (`--help` for the options).
  MP4/MKV can't be played directly: their H.264/H.265 video is far beyond an ESP32.

### Pictures

- `lcd.drawImage(path)` draws a JPEG, PNG or BMP fitted to the screen and centred (shrunk or
  enlarged); `lcd.drawImage(path, x, y)` at actual size, `(path, x, y, zoom)` scaled,
  `(path, x, y, w, h)` fitted into a box. Returns `{width, height, zoom, x, y, w, h}`.
- `lcd.imageSize(path)` → `{width, height}`. Progressive JPEGs aren't supported.

### Files

Paths starting with `/sd/` are on the SD card; everything else is on the internal LittleFS
flash (about 900 KB).

- `fs.read(path)` → string or `null`
- `fs.write(path, text)`, `fs.append(path, text)` → bool
- `fs.list([dir])` → names (directories end in `/`); `fs.dir([dir])` → `[{name, size, dir}]`
- `fs.exists(path)`, `fs.remove(path)`, `fs.stat(path)` → `{name, size, dir}` or `null`
- `fs.mkdir(path)`, `fs.rmdir(path)` (empty folders), `fs.rename(from, to)` (same storage)
- `fs.copy(from, to)` (any size, also between flash and SD), `fs.space("/" or "/sd")` →
  `{total, used}`. On the RP2350 the SD card's `used` is `-1`: working it out reads the card's whole
  allocation table (25 s on a 32 GB card, and it reset the board before), so `fs.space("/sd", true)`
  does it on request, with the watchdog off meanwhile
- `fs.contains(path, text)` searches a file without loading it whole
- `fs.writeBase64(path, b64, [append])`, `fs.readBase64(path, offset, length)` - binary-safe
  transfer for tools (the web IDE uses them)
- `load(path)` runs another script in the same global scope.
- `launch(path)` stops everything and starts the script in a fresh interpreter. The launcher
  uses this, so each app starts with all memory free.

### Settings file

`readConfig()` returns the parsed contents of `micro-js.json`, so WiFi passwords and similar
settings live in one place instead of inside each script. It looks on the SD card first
(`/sd/micro-js.json`), then on the internal flash (`/micro-js.json`), and returns `{}` if neither
exists. Files with the project's older names (`esp32-js.json`, `cyd-js.json`) are read when there's
no `micro-js.json`. Keep your real settings on the SD card: you can edit them with a card reader, and
`pio run -t uploadfs` won't overwrite them.

```json
{
  "wifi": { "ssid": "MyNetwork", "pass": "secret" },
  "utcOffsetHours": -5,
  "location": { "name": "Berlin", "lat": 52.52, "lon": 13.41 },
  "weatherRefreshMinutes": 10,
  "stocks": {
    "apiKey": "your Twelve Data key",
    "symbols": ["AAPL", "MSFT", "NVDA", "TSLA"],
    "refreshMinutes": 5
  }
}
```

Get a free stock API key at [twelvedata.com](https://twelvedata.com/pricing). The free plan
allows 8 symbols per minute and 800 per day.

**Boards wired differently.** Some CYDs have the red LED on GPIO 22 instead of 4, and their speaker
amplifier is switched on by GPIO 4 held low. For those, add
`"ledPins": [22, 16, 17]` (red, green, blue) and `"speakerEnable": [4, 0]` (pin, level) - read at
every boot. (Don't add `speakerEnable` on a standard CYD: there GPIO 4 is the red LED.) Both apply
only to the classic CYDs, so a `micro-js.json` shared on an SD card is safe in the other boards.

`utcOffsetHours` is your time zone as hours from UTC (e.g. `-5`, `1`, `5.5`); change it by one
when daylight saving starts or ends. `fetch.js` and `clock.js` read their WiFi, clock offset,
location and stock symbols from this file. Your own scripts
can add any keys they like:

```js
const cfg = readConfig();
if (cfg.wifi) wifi.connect(cfg.wifi.ssid, cfg.wifi.pass);
```

### Network (WiFi build)

| Call | Notes |
|---|---|
| `wifi.connect(ssid, pass, [timeoutMs])` | returns bool |
| `wifi.status()` | |
| `wifi.ip()` | |
| `wifi.rssi()` | |
| `wifi.scan()` | |
| `wifi.disconnect()` | |
| `wifi.ap([name], [password])` | starts the board's own network instead, returns `{ssid, ip, password}` (`192.168.4.1`). Without a name it's `micro-js-` plus the end of the MAC address; without a password it's open (8+ characters otherwise). A built-in DNS answers every name with the board, so phones show its page as a sign-in page (serve it for `"*"`). `wifi.status()` is then `"ap"` |
| `wifi.ntp([tz], [server])` | syncs the clock. `tz` is a POSIX TZ string, e.g. `"CET-1CEST,M3.5.0,M10.5.0/3"` |
| `localTime()` | `{year, month, day, hour, minute, second, weekday, valid}` |
| `Date.now()` | epoch milliseconds once synced, otherwise milliseconds since boot |
| `http.get(url, [headers])` | returns `{status, body}` (blocking). Prefer `http://`: an HTTPS handshake needs ~40 KB of contiguous RAM, which usually isn't left once WiFi is running (and certificates aren't checked) |
| `http.post(url, body, [headers])` | object bodies are sent as JSON |

**Web server.** An app can serve pages to phones and computers on the same network
(`data/remote.js` is a complete example). Requests are handled in the app's event loop, like
touches, so handlers can draw and beep.

| Call | Notes |
|---|---|
| `web.start([port])` | after `wifi.connect`; port 80 by default |
| `web.on(path, (req) => reply)` | `req` is `{method, path, query, body, ip}`. Reply with a string (HTML if it starts with `<`), an object (sent as JSON), `{status, type, body}`, or `{file: "/page.html"}` to send a file straight from flash or SD without loading it into memory. `"*"` catches every other path |
| `web.off([path])`, `web.stop()` | leaving the app stops the server too |
| `web.url()` | e.g. `"http://192.168.0.118/"` |

There's no password: anyone on your network can open the pages.

To save about 600 KB of flash, set `-DJS_WIFI=0` in `platformio.ini`.

## Examples (`data/`)

| Script | Shows |
|---|---|
| `main.js` | touch launcher for all the other scripts |
| `hello.js` | text, shapes, LED colour cycling, light sensor |
| `paint.js` | finger painting with a colour palette |
| `bounce.js` | sprite double-buffering, classes, physics, FPS counter |
| `clock.js` | analog and digital clock (NTP time when WiFi is set in `micro-js.json`) |
| `controls.js` | the LED colour mixer (red / green / blue / brightness / cycle-speed sliders, auto colour cycling, a swatch matching the LED), backlight and beep |
| `remote.js` | a web remote: open the address it shows on a phone or computer for the LED mixer, a beep button and a chat with the board (the page is `remote.html`). If it can't join your WiFi it makes its own network (`micro-js-xxxx`, or `"ap": {"ssid", "pass"}` in `micro-js.json`) and phones that join get the page by themselves |
| `demo.js` | a demoscene show - plasma, starfield, a shaded cube, copper bars and a scroller - to an original chiptune (tap / Space: next effect, `m`: music) |
| `life.js` | Conway's Game of Life, redrawing only cells that change |
| `fetch.js` | fetches weather + stocks (+ news) and writes them to small cache files, then launches `weather.js`. The one app that uses WiFi. See "The weather dashboard" below |
| `weather.js` | the dashboard display: now / hourly / 5 days / stocks / news (°C and °F). Reads the cache files `fetch.js` wrote - no WiFi - so it stays light. Tap left/right (or `,` `/`) to change page; pages rotate every 30 s |
| `breakout.js` | drag to move the paddle, tap to serve |
| `tetris.js` | on-screen rotate / drop / left / right buttons |
| `asteroids.js` | the ship turns towards your finger and fires; touch far away to thrust |
| `snake.js` | tap on the side you want to turn to |
| `flappy.js` | tap to flap |
| `bruce.js` | a Bruce-style tool menu: WiFi analyzer, system monitor, the LED colour mixer, sensor readouts (touch or keyboard) |
| `files.js` | file manager for the flash and SD card: open, run, view text and pictures (swipe, rotate), play music and videos, copy / move / rename / delete, new folders |
| `mp3.js` | music player for the `.mp3` files on the SD card; started from the file manager ("Open in MP3 player") it just plays that file |
| `video.js` | video player for `.avi` files (see "Sound, music and video"); started from the file manager it just plays that file |
| `flash.js` | installs `.FLH` and `.bin` firmware files from the SD card (see "Switching firmware") |
| `calibrate.js` | touch calibration |
| `edit.js` | a JavaScript editor for keyboard boards (`.edit FILE` in the console) |
| `joystick.js` | live view of USB gamepads |
| `dungeon.js` | a first-person dungeon crawl after *Dungeons of Daggorath* (TRS-80 CoCo, 1982), with a "1982 mode" to compare |
| `shoot.js` | a shooting gallery after the CoCo's *Shooting Gallery* ROM pak (1980) |

The games keep their high scores in files like `/tetris.hi` on the flash.

### The weather dashboard (`fetch.js` + `weather.js`)

The dashboard is split in two so the display can stay in memory on a board without PSRAM:

- **`fetch.js`** is the only part that touches the network. It connects WiFi and pulls each feed
  (weather from open-meteo, stocks from Twelve Data, headlines from Hacker News), writing each to
  its own small file on the SD card or flash (`wxmain.json`, `wxstock.json`, `wxnews.json`, and a
  `wxmeta.json` timestamp). It fetches one feed per run, re-launching itself for the next so each
  runs in a fresh interpreter with an unfragmented heap - the only way the CYD has enough
  contiguous memory for a feed. Then it launches `weather.js`.
- **`weather.js`** just renders those files. It never starts WiFi, so it runs with almost all of
  the heap free. When the data is older than `weatherRefreshMinutes` it re-launches `fetch.js`,
  which refreshes the files and launches it back.

Launch `weather.js` (or `fetch.js`) from the menu - the first run has no data, so it hands off to
`fetch.js` to populate the cache.

**News** uses an HTTPS feed by default, and the TLS handshake needs ~40 KB of contiguous memory -
more than a no-PSRAM board (the CYD) has free with WiFi up - so news is skipped there and the news
page simply doesn't appear. It works on PSRAM boards (the ESP32-S3 profiles). To use news on a
small board, point it at a plain-`http` JSON feed: `"news": { "url": "http://..." }` in
`micro-js.json` (items need a `title`, `story_title` or `headline`). `"news": false` disables it.

**Keyboard controls (Cardputer).** The arrows are `;` up, `.` down, `,` left and `/` right.
BOOT (G0) always returns to the menu. Every script also lays itself out for the 240×135 screen.
`controls.js` needs touch, so the menu hides it on boards without a touch screen.

| Script | Keys |
|---|---|
| menu | `;` `.` move, `,` `/` page, Enter run, 1–9 pick |
| tetris | `;` rotate, `,` `/` move, `.` soft drop, Space hard drop, Enter restart |
| asteroids | `,` `/` turn, `;` thrust, Space fire (hold), Enter restart |
| breakout | `,` `/` move the paddle (hold), Space launch / restart |
| snake | arrows steer, Enter restart |
| flappy | Space flap, Enter restart |
| paint | arrows move the cursor, hold Space to paint, 1–8 colour, `c` clear |
| life | Space reseed, `p` pause |
| bounce | Space adds a ball |
| weather | `,` `/` (or Space) change page; `r` or Enter refresh now |

**Gamepads (USB joysticks, on the VGA board).** Without any changes, the D-pad or stick acts as
the arrow keys, and buttons 1 and 2 act as Space and Enter. So the menu (button 1 starts the
highlighted app) and every game work with a pad. Some games read it directly:

| Game | Gamepad |
|---|---|
| tetris | D-pad / stick moves (hold to auto-repeat), down soft-drops, up or button 1 rotates, button 2 hard-drops |
| asteroids | stick turns (the further you push, the faster), button 1 fires, button 2 or up thrusts |
| breakout | stick moves the paddle in proportion, D-pad at full speed, any button launches |
| paint | stick / D-pad moves (push fully to go faster), hold button 1 to paint, button 2 next colour, button 3 clears |
| life | button 3 pauses |
| joystick | live view of both pads: sticks, D-pad, buttons and raw report bytes |

## Language notes and limits

**Supported:**
- functions, arrows, closures, classes, getters/setters, `#private`-style fields (stored as
  normal properties)
- destructuring (declarations, parameters, assignment, `for…of`), spread/rest, default
  parameters
- template literals, optional chaining, `??=`/`||=`/`&&=`, `break`/`continue`
  (without labels), `switch`, exceptions
- automatic semicolon insertion, `arguments`, `Function.prototype.call/apply/bind`

**Not supported:**
- regular expressions (string methods take plain strings)
- `async`/`await`, Promises, generators, Symbols, the iterator protocol,
  labels, `with`, `eval`
- tagged templates, Proxy/Reflect, typed arrays, BigInt
- `Date` is supported (local time from `utcOffsetHours`, UTC, parsing ISO strings, `toISOString`,
  getters and setters), but not locale formatting beyond a simple `toLocale*String`

**Deliberate differences:**
- Strings are UTF-8 byte strings: `"é".length` is 2, indexing is by byte. `for…of` and spread
  do iterate by code point.
- `map.keys()`, `arr.entries()` and similar return arrays rather than iterators. They work the
  same in `for…of` and spread.
- Sorting is stable merge sort. `var` declarations are not hoisted, but function
  declarations are.

**Big apps on small boards:** an app with a `@needs-more-memory` comment (the video and music
players, the demo, the web remote, the file manager, the dungeon and shooting games, fetch, and the
clock - WiFi alone takes about 40 KB) is big for an original ESP32 without PSRAM: the classic CYD's
launcher hides them at first ("edit list" or "+ N hidden" shows them). They may run out of memory
there.

**Apps that can't run on a small board at all:** the dungeon and shooting games carry a `@needs-ram`
comment: they want about 135 KB of memory and fail with "out of memory" on less, so the menu leaves
them out of the list (`board.ramKB` is the memory - free heap plus PSRAM - measured at boot: about 124
on the Cardputer, 147 on the RP2350, thousands on the PSRAM boards).

**The classic CYD is a limited board.** With no PSRAM it has about 100 KB for scripts, and the
games and small apps run well; the big apps above, and anything using WiFi, are at the edge of its
memory and may stop with "out of memory". Boards with PSRAM (the S3 CYD, the CoCo VGA board, the
Cardputer) run everything.

**Memory and speed:**
- The interpreter task has a 40 KB stack, which allows about 35 levels of JS recursion.
  Deeper recursion throws a catchable `RangeError` instead of crashing the board.
- Objects use small linear property lists. That's fast for typical objects; avoid using one
  object as a dictionary with thousands of keys (use `Map`).
- Rough costs on the board: a function call is about 16 µs and `fib(20)` about 0.4 s. Simple
  arithmetic loops run at about 18 µs per iteration, because the ESP32 has no hardware for
  64-bit floating point, which JavaScript numbers use.
- A parsed script takes about 4–5 times its source size in RAM, so keep scripts under about
  10 KB. Scripts that call `wifi.connect` get WiFi started before they are parsed (the WiFi
  driver needs about 50 KB), so they have roughly 40 KB less to work with.
- Built-in functions are created the first time a script uses them, so the whole library
  costs only a few KB until it's needed.
- Garbage collection runs automatically, and also when free heap drops below 48 KB. If
  memory still runs out, you get a catchable `Error: out of memory` instead of a crash.
  `gc()` requests a collection. `sys.heap()` and `.mem` show usage.

## Simulator in your browser

Try scripts on your computer, without a board:

```bash
make -C host
host/jsh --serve data/main.js
```

Then open http://localhost:8080. You get the live 320×240 screen (the mouse is your finger), a
console showing `print`/`console.log` output, a prompt for typing JavaScript (↑ for history), a
BOOT button that returns to the script you started with, and the RGB LED. `launch()` starts each
app in a fresh interpreter, as on the board.

- **Network:** `http.get` uses your computer's network (via `curl`), and `wifi.connect` always
  succeeds, so `fetch.js` pulls live weather, stocks and news. The simulator reports PSRAM, so it
  fetches news too (the headless build does not chain `launch()`, so run `fetch.js` and then
  `weather.js` to see the dashboard).
- **SD card:** `--sd DIR` makes a folder play the SD card (`/sd/...`). Put a `micro-js.json` with
  your WiFi, location and stock API key in it. `sim-sd/` is set up for this and kept out of git
  and out of the board's file-system image.
- **Port:** `--serve 9000` uses another port.
- **Cardputer:** `--board cardputer` gives a 240×135 screen with no touch. Your computer's
  keyboard is the Cardputer's: click the screen first, then type. For a non-interactive run,
  `host/jsh --sim --board cardputer --key space@500 --key ,@800+300 --png out.png data/tetris.js`
  presses keys at given times (`+300` holds the key for 300 ms).

The simulator only listens on this computer (localhost). Fonts are approximations of the
board's; colours, layout, touch and timing logic are the same.

## Testing on your computer

`host/` builds the same interpreter for Linux or macOS, plus a simulator of the board's API:

```bash
make -C host
host/jsh                       # desktop REPL
host/jsh host/tests.js         # 309-check language test suite
host/jsh --sim --time 2000 --png shot.png --tap 100,15@200 --drag 40,80,280,200@400 data/paint.js
host/jsh --gc-stress host/tests.js   # collect after every allocation (finds GC bugs)
```

The simulator draws into a 320×240 framebuffer and saves it as a PNG. Fonts are
approximated with the 5×7 font, so text sizes differ a little from the real panel. Taps and
drags are injected at the times you give. Network calls fail as if WiFi were down.

## Project layout

```
platformio.ini        one [env:...] profile per board
src/js.h, js.cpp      interpreter: lexer, parser (AST in arenas), evaluator, GC
src/js_builtins.cpp   standard library
src/hw.cpp            board bindings: lcd, sprites, pictures, touch, keyboard, gamepads, sound,
                      MP3, video, LED, fs, wifi...
src/board.h           every BOARD_* option, with the CYD's values as defaults
src/main.cpp          firmware: JS task, event loop, serial REPL, on-screen console
src/minimp3.h         MP3 decoder (lieff/minimp3, CC0)
src/boards/coco-vga/  the ESP32-CoCo board's VGA screen and USB input (only that profile builds it)
lib/                  ESP32-S3-VGA and SoftUSB, used only by the coco-vga profile
data/                 the apps -> flashed with `pio run -t uploadfs`
data/lib/mixer.js     the LED colour mixer, loaded by controls.js, bruce.js and remote.js
host/                 desktop build, simulator, test suite (stb_image.h: picture loading)
tools/upload.js       copy files to the board over serial (npm install first)
tools/web-ide.html    browser IDE over USB: files, editor, upload, console, firmware flashing
tools/serve.js        serves the IDE at http://localhost:8000 (node tools/serve.js, or npm run ide)
tools/build_release.sh       web-flasher images for every board, into release/
tools/convert-videos.sh      videos -> .avi for video.js
tools/make-flh.js            wrap a firmware.bin as an .FLH file
tools/auto-version.js        bumps src/version.h when the code changed (run before each build)
package.json                 the Node tools' one dependency (serialport)
```

**Design notes:**
- The touch controller is bit-banged, so the SD card can have the VSPI bus to itself.
- JavaScript runs in its own FreeRTOS task with a big stack. The stack depth is checked on
  every call and statement.
- Memory: small interpreter allocations (objects, property and variable lists) come from a
  pool of fixed-size blocks, carved from a 48 KB region reserved at boot. The ESP32's own
  allocator gets slow and fragmented under thousands of short-lived blocks; the pool made
  function calls about 10x faster on the board. Parsed programs use variable-sized syntax
  tree nodes, and each app started from the menu gets a fresh interpreter.
- Every heap object allocated during a statement stays rooted until that statement finishes.
  So the collector only runs at statement boundaries, and native code never sees a value freed
  underneath it.

## The ESP32-CoCo VGA board

Profile `coco-vga` runs micro-js on mast4rbug's [ESP32-CoCo](https://github.com/mast4rbug/ESP32-CoCo)
hardware, built for a TRS-80 Color Computer emulator: an ESP32-S3 with VGA output and USB ports.

- **Screen:** the board drives an 8-bit video bus from the S3's LCD_CAM peripheral, two bits per
  colour through resistors (blue on GPIO 8/9, green 6/7, red 4/5, HSYNC 2, VSYNC 1, pixel clock
  48). That byte layout is RGB332, TFT_eSPI's 8-bit sprite format, so the screen is a full-screen
  sprite whose buffer the video DMA reads directly (`src/boards/coco-vga/vga_screen.h`): every
  font and drawing call works, in 64 colours. The picture is 320×240 sent with 640×480@60
  timing.
- **Keyboards and gamepads** plug straight into the board's USB-A ports (software USB host on
  18/17, 12/11, 16/15; low-speed devices only, **no hubs**). F12 or Escape: menu; Ctrl+C: console.
  Gamepads are described under `joystick`.
- **Also:** SD card (SD_MMC 1-bit: CLK 39, CMD 38, D0 40), PWM sound on GPIO 47, the red/green
  front LED (45/46), BOOT button (GPIO 0).
- **Sound:** tones, and MP3 and video sound through the PWM pin as an 8-bit DAC (see "Sound,
  music and video").
- **Not yet:** USB mice, Bluetooth, screen rotation.
- After editing `vga_screen.h`, delete `.pio/build/coco-vga/src/hw.cpp.o`: `hw.cpp` includes it
  through a macro, so the build doesn't notice.

## The RP2350-PiZero

Profile `rp2350-pizero` runs micro-js on Waveshare's [RP2350-PiZero](https://www.waveshare.com/wiki/RP2350-PiZero),
a Raspberry Pi Zero-sized board with the RP2350B chip (two 252 MHz Cortex-M33 cores here, 520 KB
RAM, 16 MB flash, no PSRAM). It has **no WiFi or Bluetooth**; it gets its clock and the web
through the computer it's plugged into instead (below). There's no `web` server object, so
`remote` doesn't show in its launcher.

- **Screen:** the mini HDMI port, as DVI: 640×480 at 60 Hz, showing a 320×240 picture with every
  pixel doubled. The RP2350 makes the signal itself - core 1 does nothing else - with Luke Wren's
  [libdvi](https://github.com/Wren6991/PicoDVI) on PIO 0 (GPIO 32-39). The screen is a full-screen
  8-bit (256-colour) sprite (`src/boards/rp2350-pizero/dvi_screen.h`), so every font and drawing
  call works as on the CYD; 16-bit (`-DDVI_DEPTH=16`) costs 75 KB more RAM and made core 1 miss
  lines (a missed line shows as a red stripe). Most monitors and TVs with HDMI take it (it's DVI: no sound over the cable yet).
- **Keyboards and gamepads** go in the USB-C port marked **PIO-USB** (GPIO 28/29); use a USB-C to
  USB-A adapter for ordinary plugs, and a simple USB hub for a keyboard and a gamepad together.
  Full-speed and low-speed devices work. The other USB-C port is power and the REPL. F12 or
  Escape: menu; Ctrl+C: console. The board can't switch that port's power, so plug devices in
  before powering up (or plug them into a hub).
- **Sound:** PWM on **GPIO 18 (header pin 12)**, as a Raspberry Pi does: tones, MP3 and video
  sound, 44.1 kHz from a DMA channel, so nothing else the board does makes it stutter. The pin
  can't drive a speaker by itself. For headphones or an amplifier's input, use a 1 kΩ resistor
  from pin 12, then a 10 µF capacitor in series to the plug's tip, and a 10 nF capacitor from the
  resistor's far end to ground (pin 14) to take off the PWM hiss; the plug's sleeve goes to ground.
  A small amplifier board (PAM8302, PAM8403, or a powered speaker) is loudest.
- **Network and clock through the computer:** run `node tools/bridge.js` (or `npm run bridge`)
  on the computer the board is plugged into. It sets the board's clock and time zone, and fetches
  `http.get` / `http.post` for it - HTTPS included, as the computer does the TLS - so the weather
  dashboard (news included), stocks and the clock work as on a WiFi board: `wifi.connect()` waits
  for the bridge, `wifi.ntp()` asks it for the time, `board.network` is `"usb"`. It's also a REPL
  terminal (Ctrl-] quits), and its log hides API keys in URLs. It uses the REPL's serial port, so
  close other serial monitors first; without it, network calls fail with a message saying so.
- **microSD** on SPI: SCK 30, MOSI 31, MISO 40, CS 43. Use a name-brand SDHC card (4-32 GB, FAT32).
- **Flashing:** `pio run -e rp2350-pizero -t upload` (if it can't find the board, hold BOOT
  while plugging it in; on Linux picotool also needs the Raspberry Pi udev rule:
  `SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"` in `/etc/udev/rules.d/99-picotool.rules`), then `pio run -e rp2350-pizero -t uploadfs` for the apps; or drag a
  release `.uf2` onto the board's drive. `flash.js` doesn't apply (it installs ESP32 firmware).
- **Not yet:** sound over HDMI, the BOOT button as a menu key (it's wired to the flash chip),
  installing firmware from a file, WiFi through an add-on board, a web server over the USB bridge.
- If it ever hangs while starting up, a watchdog restarts it after 8 seconds and skips the step that
  hung; `board.bootProblem` then says which.
- The background jobs (music, video) run as cooperative tasks on core 0 (`src/port_rp2.cpp`):
  they get turns whenever the interpreter waits or checks for Ctrl+C.

## Switching firmware (`.FLH` files)

`flash.js` installs firmware from the SD card into the board's spare firmware slot, keeping the
apps on the flash. It takes plain `firmware.bin` files, full flash images (only their firmware
part is used), and `.FLH` files - the ESP32-CoCo emulator's format (a checksum, release notes,
then the firmware). So on the CoCo board you can switch between micro-js and the emulators from
either side: micro-js → `flash.js` → the emulator's `.FLH`; emulator → its Flash menu (F) →
`MICROJS_<version>.FLH`, which `tools/build_release.sh` makes (or `node tools/make-flh.js firmware.bin OUT.FLH
"notes"`). Every file is checked (checksum, image, chip) before anything is written. Boards with
no spare slot (the classic CYD's partition table) can't install this way.

## Credits and licences

- TFT_eSPI, TJpg_Decoder (Bodmer), PNGdec and JPEGDEC (Larry Bank) come from the PlatformIO
  registry under their own licences.
- `src/minimp3.h`: [lieff/minimp3](https://github.com/lieff/minimp3), CC0.
- `host/stb_image.h`: [nothings/stb](https://github.com/nothings/stb), public domain / MIT.
- `lib/SoftUSB`: ESP32-USB-Soft-Host by Dmitry Samsonov and tobozo, **AGPL-3.0**. Firmware built
  with the `coco-vga` profile includes it, so that firmware is distributed under the AGPL-3.0
  (its source is this repository).
- `lib/ESP32-S3-VGA`: bitluni's ESP32-S3-VGA, as extended by mast4rbug's ESP32-CoCo emulator.
  bitluni's repository doesn't state a licence. Each library's `NOTICE.md` lists its origin and
  the changes made here.
- `lib/libdvi`: from Luke Wren's [PicoDVI](https://github.com/Wren6991/PicoDVI), BSD-3-Clause
  (`lib/libdvi/LICENSE`); `dvi_serialiser.pio.h` is the generated header from Adafruit's PicoDVI
  fork (BSD).
- `lib/PicoPIOUSB`: sekigon-gonnoc's [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
  0.7.2, MIT (`lib/PicoPIOUSB/LICENSE`).
- The RP2350-PiZero's pin wiring was checked against Waveshare's schematic and Paul Cunningham's
  [XRoar port for the board](https://github.com/ugufru/xroar-waveshare-rp2350-pizero) (no code from
  it is used here).
- Everything else here is **MIT** licensed (`LICENSE`).

## Troubleshooting

- **The web IDE says "the device has been lost" or won't connect (Linux):** restart Chrome fully
  (`chrome://restart`) - a Chrome that updated itself while running often can't open serial ports -
  and stop ModemManager, which probes new USB serial devices (`sudo systemctl stop ModemManager`, or
  `disable` it if you don't use a mobile-data modem). Close other tabs or programs using the port.
- **Garbled colours:** you probably have the CYD2USB variant; build with `-e cyd2usb`, or call
  `lcd.invert(true)`.
- **Taps land in the wrong place:** run `calibrate.js` from the launcher.
- **The launcher looks old after an update:** an old `main.js` on the SD card runs instead of the
  one on the flash; delete or rename `/sd/main.js`.
- **Nothing on the serial monitor:** check the baud rate is 115200; some CYDs need the CH340
  USB driver.
- **`out of memory`:** free sprites you no longer need, use 8-bit sprites, and keep big strings
  short-lived.
- **SD card not detected (`board.hasSd` false):** another SPI device sharing the card's bus may
  be holding the data line. On the Cardputer ADV a LoRa / sub-GHz module on the EXT header
  shares the SD bus, so its chip-select (G5) is parked high via `BOARD_SD_PARK_CS`; set that
  flag if you add another board with a shared-bus peripheral. Also note the ESP32 SD library
  reads only FAT16/FAT32 partitions — a card formatted exFAT (common for 64 GB+ cards) won't
  mount, so use a FAT32 partition. Cards up to at least 128 GB work when FAT32-formatted.
