// board.h - everything that differs between supported boards.
//
// Each PlatformIO environment (see platformio.ini) describes one board with -D flags; anything
// it leaves out gets the defaults below, which are the ESP32-2432S028R "Cheap Yellow Display".
// To add a board: copy an environment, set its display flags (TFT_eSPI) and the BOARD_* flags
// for its pins and parts, build it, and check display, touch, LED, SD and sound on the device.
#pragma once

// MICROJS_BOARD_NAME wins over a BOARD_NAME the core defines itself (arduino-pico sets it to the
// board's id, e.g. "waveshare_rp2350_pizero").
#ifdef MICROJS_BOARD_NAME
#undef BOARD_NAME
#define BOARD_NAME MICROJS_BOARD_NAME
#endif
#ifndef BOARD_NAME
#define BOARD_NAME "CYD (ESP32-2432S028R)"
#endif

// ---- display (the driver itself is configured with TFT_eSPI's flags)
#ifndef BOARD_ROTATION
#define BOARD_ROTATION 1           // 0..3; 1 = landscape on the CYD
#endif
#ifndef BOARD_BACKLIGHT
#define BOARD_BACKLIGHT 21         // backlight pin (PWM dimmable), -1 = none / always on
#endif

// ---- touch
#define TOUCH_NONE 0
#define TOUCH_XPT2046 1            // resistive, bit-banged SPI (CYD)
#define TOUCH_FT6336 2             // capacitive, I2C (FT6336U / FT6236 / FT5x06 family)
#ifndef BOARD_TOUCH
#define BOARD_TOUCH TOUCH_XPT2046
#endif
#if BOARD_TOUCH == TOUCH_XPT2046
#ifndef BOARD_XPT_CLK
#define BOARD_XPT_CLK 25
#define BOARD_XPT_MOSI 32
#define BOARD_XPT_MISO 39
#define BOARD_XPT_CS 33
#endif
#ifndef BOARD_XPT_CAL                // raw x min, x max, y min, y max (touch.calibrate())
#define BOARD_XPT_CAL 200, 3700, 240, 3800
#endif
#endif
#if BOARD_TOUCH == TOUCH_FT6336
#ifndef BOARD_TOUCH_SDA
#error "set BOARD_TOUCH_SDA / BOARD_TOUCH_SCL for the FT6336 touch controller"
#endif
#ifndef BOARD_TOUCH_ADDR
#define BOARD_TOUCH_ADDR 0x38
#endif
#ifndef BOARD_TOUCH_RST
#define BOARD_TOUCH_RST -1
#endif
// The controller reports coordinates in the panel's native (rotation 0) orientation; these fix
// up panels mounted swapped or mirrored before the screen rotation is applied.
#ifndef BOARD_TOUCH_SWAP_XY
#define BOARD_TOUCH_SWAP_XY 0
#endif
#ifndef BOARD_TOUCH_FLIP_X
#define BOARD_TOUCH_FLIP_X 0
#endif
#ifndef BOARD_TOUCH_FLIP_Y
#define BOARD_TOUCH_FLIP_Y 0
#endif
#endif

// ---- keyboard
#define KEYBOARD_NONE 0
#define KEYBOARD_CARDPUTER_TCA8418 1  // M5Stack Cardputer ADV: 4x14 keys on a TCA8418 (I2C)
#define KEYBOARD_EXTERNAL 2           // keys come from another driver via hwKeyEvent() (e.g. USB HID)
#ifndef BOARD_KEYBOARD
#define BOARD_KEYBOARD KEYBOARD_NONE
#endif
#if BOARD_KEYBOARD == KEYBOARD_CARDPUTER_TCA8418
#ifndef BOARD_KEYBOARD_SDA
#define BOARD_KEYBOARD_SDA 8
#define BOARD_KEYBOARD_SCL 9
#endif
#ifndef BOARD_KEYBOARD_ADDR
#define BOARD_KEYBOARD_ADDR 0x34
#endif
#endif

// ---- RGB LED: three PWM pins (BOARD_LED_R/G/B) or one addressable WS2812 (BOARD_LED_WS2812)
#ifndef BOARD_LED_WS2812
#define BOARD_LED_WS2812 -1
#endif
#if BOARD_LED_WS2812 < 0 && !defined(BOARD_LED_R)
#define BOARD_LED_R 4
#define BOARD_LED_G 16
#define BOARD_LED_B 17
#endif
#ifndef BOARD_LED_R
#define BOARD_LED_R -1
#define BOARD_LED_G -1
#define BOARD_LED_B -1
#endif
#ifndef BOARD_LED_ACTIVE_LOW
#define BOARD_LED_ACTIVE_LOW 1
#endif
// Or a two-colour LED between two pins (anti-parallel): A high = red, B high = green (the
// ESP32-CoCo front LED: -DBOARD_LED_BICOLOR_A=45 -DBOARD_LED_BICOLOR_B=46). led() shows red or
// green, whichever is larger.
// Colour shown at boot and whenever an app starts (e.g. 255,0,0 for a red "power on" light).
#ifndef BOARD_LED_IDLE
#define BOARD_LED_IDLE 0, 0, 0
#endif

// ---- other parts (-1 = not present)
// Speaker on an I2S DAC / amplifier instead (tone() plays a square wave): bit clock, word select
// and data pins. BOARD_SPEAKER_ES8311 = the I2C address of an ES8311 codec in front of it, set up on
// the keyboard's I2C bus (Cardputer ADV: 0x18); leave it out for a plain I2S amplifier.
// Battery voltage on an ADC pin, through a divider (volts at the pin x BOARD_BATTERY_DIVIDER):
// sys.battery() and the launcher's battery gauge. -1: no battery sense.
#ifndef BOARD_BATTERY_ADC
#define BOARD_BATTERY_ADC -1
#endif
#ifndef BOARD_BATTERY_DIVIDER
#define BOARD_BATTERY_DIVIDER 2.0
#endif
#ifndef BOARD_SPEAKER_I2S_BCK
#define BOARD_SPEAKER_I2S_BCK -1
#define BOARD_SPEAKER_I2S_WS -1
#define BOARD_SPEAKER_I2S_DOUT -1
#endif
#ifndef BOARD_SPEAKER_I2S_LEVEL
#define BOARD_SPEAKER_I2S_LEVEL 5000  // square wave level, out of 32767
#endif
#ifndef BOARD_SPEAKER_ES8311_VOL
#define BOARD_SPEAKER_ES8311_VOL 0xBF  // ES8311 DAC volume: 0xBF = 0 dB, 0xFF = +32 dB
#endif
#ifndef BOARD_SPEAKER_I2S_MCLK
#define BOARD_SPEAKER_I2S_MCLK -1  // master clock pin, if the codec has one wired
#endif
#ifndef BOARD_SPEAKER_ES8311
#define BOARD_SPEAKER_ES8311 0
#endif
// a pin that switches the speaker amplifier on, and the level that means "on" (-1 = none)
#ifndef BOARD_SPEAKER_AMP_EN
#define BOARD_SPEAKER_AMP_EN -1
#define BOARD_SPEAKER_AMP_ON LOW
#endif
#ifndef BOARD_SPEAKER_PWM
#define BOARD_SPEAKER_PWM 26       // speaker/amplifier driven by a PWM pin (tone())
#endif
#ifndef BOARD_LIGHT_SENSOR
#define BOARD_LIGHT_SENSOR 34      // analog light sensor (light())
#endif
#ifndef BOARD_BOOT_BUTTON
#define BOARD_BOOT_BUTTON 0        // press: back to the menu; hold at reset: skip main.js
#endif

// ---- SD card. Either SD_MMC in 1-bit mode (define BOARD_SD_MMC_CLK, _CMD and _D0), or SPI
// (BOARD_SD_CS < 0 = no SD slot).
#ifndef BOARD_SD_CS
#define BOARD_SD_SCK 18
#define BOARD_SD_MISO 19
#define BOARD_SD_MOSI 23
#define BOARD_SD_CS 5
#endif
#ifndef BOARD_SD_HZ
#define BOARD_SD_HZ 4000000  // SPI clock of an SD card on its own pins (the Arduino default); faster where the wiring allows
#endif
#ifndef BOARD_SD_SPI_HOST          // SPI peripheral for the card (not the one the display uses)
#define BOARD_SD_SPI_HOST VSPI
#endif
// Other devices on the SD's SPI bus (e.g. a LoRa/sub-GHz module on an expansion header) must
// have their chip-select parked HIGH, or they drive the shared MISO line and the card won't
// answer. Set BOARD_SD_PARK_CS to such a pin (-1 = none); it's held high for the whole session.
#ifndef BOARD_SD_PARK_CS
#define BOARD_SD_PARK_CS -1
#endif

// ---- screen. A board without a TFT_eSPI panel (e.g. VGA output) can supply its own
// TFT_eSPI-compatible screen class: -DBOARD_SCREEN_HEADER='"vga_screen.h"' -DBOARD_SCREEN_CLASS=VgaScreen
// The class must provide service() (called often from the main loop) and, as a TFT_eSprite, gets
// sprites pushed onto it with pushToSprite().

// ---- network through the computer over USB (tools/bridge.js), for boards without WiFi
#ifndef MICROJS_BRIDGE
#define MICROJS_BRIDGE (!JS_WIFI)
#endif

// ---- memory. Boards with PSRAM get big regions there; without it, everything is tuned to
// fit next to the WiFi driver in ~300 KB of internal RAM (see README "Memory").
#ifdef BOARD_HAS_PSRAM
#define BOARD_PSRAM 1
#else
#define BOARD_PSRAM 0
#endif
#ifndef BOARD_JS_STACK_KB
#define BOARD_JS_STACK_KB (BOARD_PSRAM ? 64 : 40)    // interpreter stack (internal RAM)
#endif
#ifndef BOARD_POOL_KB
#define BOARD_POOL_KB (BOARD_PSRAM ? 1024 : 48)      // small objects: properties, lists, ...
#endif
#ifndef BOARD_ARENA_KB
#define BOARD_ARENA_KB (BOARD_PSRAM ? 512 : 40)      // parsed programs
#endif
#ifndef BOARD_GC_MIN_KB
#define BOARD_GC_MIN_KB (BOARD_PSRAM ? 256 : 16)     // collect after this much allocation...
#endif
#ifndef BOARD_GC_MAX_KB
#define BOARD_GC_MAX_KB (BOARD_PSRAM ? 1024 : 24)    // ...and never let more garbage build up
#endif
#ifndef BOARD_LOW_MEMORY_KB
#define BOARD_LOW_MEMORY_KB (BOARD_PSRAM ? 32 : 48)  // collect early below this much free heap
#endif
