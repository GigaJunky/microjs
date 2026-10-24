// hw.h - Cheap Yellow Display hardware bindings for the JavaScript interpreter
#pragma once
#include <string>
#include <vector>
#include "js.h"

void hwBegin();                     // one-time hardware init (display, touch, LED, filesystems)
void hwInstall(js::Interp& I);      // register lcd, touch, led, fs, ... globals
void hwPoll(js::Interp& I);
bool hwLowMemory();                    // an original ESP32 without PSRAM (the CYD): no room for the big apps
void hwShowNotice(const std::string& title, const std::string& msg);  // a calm full-screen message         // dispatch touch events etc. (call from the event loop)
void hwService();                   // cheap housekeeping (tone timeout); safe to call anywhere
void hwStop(js::Interp& I);         // Ctrl-C: drop handlers, silence speaker
void hwReset();                     // display, LED and backlight back to their boot defaults
void hwShowError(const std::string& title, const std::string& msg);  // full-screen error message
void hwSdRelease();                  // let go of the SD card (its memory) / mount it again
void hwSdEnsure();
void hwWifiPrepare(bool use);       // start (or stop) the WiFi driver before a script is loaded
void hwNetReserve();                // set aside memory for HTTP requests (call after parsing)
void hwNetRelease();                // free that reserve (before loading the next script)
void hwSplash();                    // boot screen
void hwBridgeLine(const std::string& line);  // a message from tools/bridge.js (MICROJS_BRIDGE)
void hwKeyEvent(const char* key, bool down, uint8_t mods);  // external keyboard drivers (KEYBOARD_EXTERNAL)
void hwExternalKeyboardBegin();     // implemented by an external keyboard driver, if any
bool hwMenuKey();                   // true once after a "back to the menu" key (F12, Escape)
bool hwStopKey();                   // true once after Ctrl+C on a keyboard: stop the app, go to the console
bool hwHasHandlers();               // an app has key or touch handlers registered (it is "running")
// On-screen console (boards with a keyboard): REPL output is drawn while it's showing; keys that no
// app handles arrive at hwConsoleKey(), which main.cpp turns into REPL input.
void hwConsoleWrite(const char* s, size_t n);
void hwConsoleShow(bool on);
void hwConsoleKey(const char* key, uint8_t mods);

// Joysticks / gamepads, fed by an input driver (e.g. USB HID) from its own task. Axes -127..127
// (x, y, then a second stick), hat -1 = centred or 0..7 clockwise from up, buttons as a bit mask.
struct HwJoystick {
  bool connected;
  int8_t axis[4];
  int8_t hat;
  uint32_t buttons;
  uint8_t raw[8];
  uint8_t rawLen;
  uint8_t trig[2];  // analogue triggers 0..255 (left, right), 0 if the pad has none
};
static const int HW_JOYSTICKS = 2;
void hwJoystickUpdate(int n, const HwJoystick& state);
void hwJoystickUpdateQuiet(int n, const HwJoystick& state);  // the same without the arrow-key emulation

// files: "/sd/..." lives on the SD card, everything else on the internal LittleFS flash
struct FileEntry {
  std::string name;
  size_t size;
  bool dir;
};
bool hwReadFile(const std::string& path, std::string& out);
bool hwFileContains(const std::string& path, const char* needle);
bool hwWriteFile(const std::string& path, const std::string& data, bool append = false);
bool hwRemove(const std::string& path);
std::vector<FileEntry> hwList(const std::string& dir, size_t skip = 0, size_t limit = 0);  // limit 0: all of them
