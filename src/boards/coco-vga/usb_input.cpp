// usb_input.cpp - USB keyboards and joysticks on the board's software USB host ports
// (ESP32-USB-Soft-Host).
//
// The host runs on its own task and timer; it hands over each HID report. Keyboards send 8-byte
// boot-protocol reports (modifiers, reserved, up to six pressed key codes); comparing each report
// with the previous one gives key presses and releases, which go to hw.cpp's keyboard queue
// (hwKeyEvent) and from there to keyboard.on("key"/"keyup") handlers in JavaScript.
// Only low-speed devices work on a software host, and hubs are not supported: plug the keyboard
// straight into one of the board's ports.
#include <Arduino.h>
#include "ESP32-USB-Soft-Host.h"
#include "../../hw.h"

// D+/D- pins of the board's USB ports (-1 = unused)
#ifndef USB_PINS
#define USB_PINS 18, 17,  12, 11,  16, 15,  -1, -1
#endif

static const int PORTS = 4;
static bool isKeyboard[PORTS];       // interface protocol 1 = keyboard (2 = mouse)
static bool isJoystick[PORTS];       // HID with no boot protocol: a joystick / gamepad
static uint8_t prev[PORTS][8];
static bool capsLock = false;

// ---- joysticks: every gamepad lays out its report differently, so learn the layout from the
// first report (taken as "nothing pressed"): bytes resting near the middle are stick axes, a low
// nibble resting at 15 or 8 is a hat switch (D-pad), and every other bit is a button - pressed when
// it differs from its resting state, so active-low buttons work too. A small non-zero byte before the
// first axis is a report ID, not buttons (counting it would shift every button number by 8).
struct JoyLayout {
  bool learned;
  uint8_t idle[8], len;
  int8_t axisByte[4];  // -1 = none
  int8_t hatByte;
  bool skip[8];        // report ID bytes: not buttons
  int8_t index;        // which joystick (0, 1) this port is, -1 = none
};
static JoyLayout layout[PORTS];

static void learn(JoyLayout& L, const uint8_t* d, uint8_t len) {
  L.learned = true;
  L.len = len;
  memcpy(L.idle, d, len);
  int axes = 0;
  L.hatByte = -1;
  for (int i = 0; i < 4; i++) L.axisByte[i] = -1;
  for (int i = 0; i < len; i++) {
    if (d[i] >= 0x70 && d[i] <= 0x8F && axes < 4) L.axisByte[axes++] = i;
    else if (L.hatByte < 0 && ((d[i] & 0x0F) == 0x0F || (d[i] & 0x0F) == 0x08)) L.hatByte = i;
  }
  int firstAxis = L.axisByte[0] < 0 ? 0 : L.axisByte[0];
  for (int i = 0; i < 8; i++) L.skip[i] = i < len && i < firstAxis && d[i] >= 1 && d[i] <= 15;
}

static void joystickReport(uint8_t port, const uint8_t* d, uint8_t len) {
  JoyLayout& L = layout[port];
  if (L.index < 0) return;
  if (!L.learned || L.len != len) learn(L, d, len);
  HwJoystick s{};
  s.connected = true;
  s.rawLen = len < sizeof s.raw ? len : sizeof s.raw;
  memcpy(s.raw, d, s.rawLen);
  for (int a = 0; a < 4; a++) {
    int b = L.axisByte[a];
    int v = b < 0 ? 0 : (int)d[b] - 128;
    s.axis[a] = (int8_t)(v < -127 ? -127 : v > 127 ? 127 : v);
  }
  s.hat = -1;
  if (L.hatByte >= 0) {
    int h = d[L.hatByte] & 0x0F;
    if (h < 8) s.hat = h;
  }
  int button = 0;
  for (int i = 0; i < len && button < 32; i++) {
    bool axis = false;
    for (int a = 0; a < 4; a++) axis |= L.axisByte[a] == i;
    if (axis || L.skip[i]) continue;
    for (int bit = 0; bit < 8 && button < 32; bit++) {
      if (i == L.hatByte && bit < 4) continue;  // hat nibble
      if (((d[i] ^ L.idle[i]) >> bit) & 1) s.buttons |= 1u << button;
      button++;
    }
  }
  hwJoystickUpdate(L.index, s);
}

static void joystickAttach(uint8_t port) {  // give a newly found joystick the lowest free number
  bool used[HW_JOYSTICKS] = {};
  for (int p = 0; p < PORTS; p++)
    if (p != port && layout[p].index >= 0) used[layout[p].index] = true;
  layout[port].index = -1;
  for (int n = 0; n < HW_JOYSTICKS; n++)
    if (!used[n]) { layout[port].index = n; break; }
  layout[port].learned = false;
}

// HID usage -> key name, unshifted and shifted (browser KeyboardEvent.key style)
static const char* keyName(uint8_t code, bool shift, char* buf) {
  static const char* const digits = "1234567890", * const shifted = "!@#$%^&*()";
  static const char* const named[] = {"Enter", "Escape", "Backspace", "Tab", " "};  // 0x28-0x2C
  static const char* const punct = "-=[]\\#;'`,./", * const punctShift = "_+{}|~:\"~<>?";
  static const char* const nav[] = {"PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp",
                                    "Delete", "End", "PageDown", "ArrowRight", "ArrowLeft", "ArrowDown", "ArrowUp"};
  buf[1] = 0;
  if (code >= 0x04 && code <= 0x1D) {  // letters
    bool upper = shift != capsLock;
    buf[0] = (upper ? 'A' : 'a') + (code - 0x04);
    return buf;
  }
  if (code >= 0x1E && code <= 0x27) { buf[0] = (shift ? shifted : digits)[code - 0x1E]; return buf; }
  if (code >= 0x28 && code <= 0x2C) return named[code - 0x28];
  if (code >= 0x2D && code <= 0x38) { buf[0] = (shift ? punctShift : punct)[code - 0x2D]; return buf; }
  if (code == 0x39) return "CapsLock";
  if (code >= 0x3A && code <= 0x45) { snprintf(buf, 8, "F%d", code - 0x3A + 1); return buf; }
  if (code >= 0x46 && code <= 0x52) return nav[code - 0x46];
  if (code >= 0x54 && code <= 0x63) {  // keypad
    static const char* const kp[] = {"/", "*", "-", "+", "Enter", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "."};
    return kp[code - 0x54];
  }
  return nullptr;
}

static bool contains(const uint8_t* r, uint8_t code) {
  for (int i = 2; i < 8; i++)
    if (r[i] == code) return true;
  return false;
}

static void onIface(uint8_t ref, int, int, void* intf, size_t) {
  sIntfDesc* d = (sIntfDesc*)intf;
  if (ref >= PORTS || d->iClass != 0x03) return;
  if (d->iProto == 1) isKeyboard[ref] = true;  // any keyboard interface makes it a keyboard
  else if (d->iProto == 0 && !isKeyboard[ref] && !isJoystick[ref]) {
    isJoystick[ref] = true;
    joystickAttach(ref);
  }
}

static void onDetect(uint8_t usbNum, void*) {
  if (usbNum < PORTS) {
    memset(prev[usbNum], 0, 8);
    isKeyboard[usbNum] = false;  // set again by onIface for a keyboard...
    isJoystick[usbNum] = false;  // ...or a joystick
  }
}

static void onDisconnect(uint8_t usbNum) {
  if (usbNum >= PORTS) return;
  if (isJoystick[usbNum] && layout[usbNum].index >= 0) {
    HwJoystick off{};
    hwJoystickUpdate(layout[usbNum].index, off);
  }
  isKeyboard[usbNum] = isJoystick[usbNum] = false;
  layout[usbNum].index = -1;
}

static void onReport(uint8_t usbNum, uint8_t, uint8_t* data, uint8_t len) {
  if (usbNum >= PORTS) return;
  if (isJoystick[usbNum] && !isKeyboard[usbNum]) return joystickReport(usbNum, data, len);
  if (len != 8 || !isKeyboard[usbNum]) return;
  if (data[2] == 1) return;  // "phantom" report: too many keys pressed at once
  uint8_t m = data[0];  // left/right ctrl 0x01/0x10, shift 0x02/0x20, alt 0x04/0x40, gui 0x08/0x80
  uint8_t mods = ((m & 0x22) ? 1 : 0) | ((m & 0x11) ? 2 : 0) | ((m & 0x44) ? 4 : 0) | ((m & 0x88) ? 8 : 0);
  bool shift = mods & 1;
  char buf[8];
  uint8_t* old = prev[usbNum];
  for (int i = 2; i < 8; i++) {  // released
    if (old[i] > 3 && !contains(data, old[i])) {
      const char* k = keyName(old[i], shift, buf);
      if (k) hwKeyEvent(k, false, mods);
    }
  }
  for (int i = 2; i < 8; i++) {  // newly pressed
    if (data[i] > 3 && !contains(old, data[i])) {
      if (data[i] == 0x39) capsLock = !capsLock;
      const char* k = keyName(data[i], shift, buf);
      if (k) hwKeyEvent(k, true, mods);
    }
  }
  memcpy(old, data, 8);
}

// Called by hw.cpp's keyboardBegin() (BOARD_KEYBOARD == KEYBOARD_EXTERNAL).
void hwExternalKeyboardBegin() {
  static const usb_pins_config_t pins = {USB_PINS};
  for (auto& L : layout) L.index = -1;
  USH.setOnIfaceDescCb(onIface);
  USH.init(pins, onDetect, onReport, nullptr, onDisconnect);
}
