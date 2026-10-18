// usb_hid.h - USB keyboard and joystick reports -> micro-js key and joystick events, for the boards
// whose USB host driver hands over raw HID reports (src/boards/coco-vga, src/boards/rp2350-pizero).
//
// Keyboards send 8-byte boot-protocol reports (modifiers, reserved, up to six pressed key codes);
// comparing each report with the previous one gives key presses and releases, which go to hw.cpp's
// keyboard queue (hwKeyEvent) and from there to keyboard.on("key"/"keyup") handlers in JavaScript.
#pragma once
#include <Arduino.h>
#include <string.h>
#include "hw.h"

namespace usbhid {

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
  int8_t index;        // which joystick (0, 1) this is, -1 = none
};

inline void learn(JoyLayout& L, const uint8_t* d, uint8_t len) {
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

inline void joystickReport(JoyLayout& L, const uint8_t* d, uint8_t len) {
  if (L.index < 0) return;
  if (len > 8) len = 8;  // the layout covers the first 8 bytes (sticks, hat and buttons come first)
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

// Is this HID interface a joystick? Its report descriptor's first Usage says: Generic Desktop page
// with Joystick (4), Game Pad (5) or Multi-axis Controller (8). A keyboard's extra interfaces (media
// keys on the Consumer page, system control) aren't, and mustn't turn up as a joystick.
inline bool isJoystickDescriptor(const uint8_t* d, uint16_t len) {
  int page = -1;
  for (uint16_t i = 0; i < len;) {
    uint8_t item = d[i];
    if (item == 0xFE) {  // long item: skip it
      if (i + 1 >= len) break;
      i += 3 + d[i + 1];
      continue;
    }
    uint8_t size = item & 3;
    if (size == 3) size = 4;
    if (i + 1 + size > len) break;
    uint32_t v = 0;
    for (int b = 0; b < size; b++) v |= (uint32_t)d[i + 1 + b] << (8 * b);
    uint8_t tag = item & 0xFC;
    if (tag == 0x04) page = (int)v;  // Usage Page
    else if (tag == 0x08) {          // the first Usage (a 4-byte one carries its page)
      if (size == 4) { page = (int)(v >> 16); v &= 0xFFFF; }
      return page == 1 && (v == 4 || v == 5 || v == 8);
    }
    i += 1 + size;
  }
  return false;
}

// give a newly found joystick (layouts[slot]) the lowest free number
inline void joystickAttach(JoyLayout* layouts, int count, int slot) {
  bool used[HW_JOYSTICKS] = {};
  for (int p = 0; p < count; p++)
    if (p != slot && layouts[p].index >= 0) used[layouts[p].index] = true;
  layouts[slot].index = -1;
  for (int n = 0; n < HW_JOYSTICKS; n++)
    if (!used[n]) { layouts[slot].index = n; break; }
  layouts[slot].learned = false;
}

inline void joystickDetach(JoyLayout& L) {
  if (L.index >= 0) {
    HwJoystick off{};
    hwJoystickUpdate(L.index, off);
  }
  L.index = -1;
}

// ---- keyboards
// HID usage -> key name, unshifted and shifted (browser KeyboardEvent.key style)
inline const char* keyName(uint8_t code, bool shift, bool capsLock, char* buf) {
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

inline bool reportHas(const uint8_t* r, uint8_t code) {
  for (int i = 2; i < 8; i++)
    if (r[i] == code) return true;
  return false;
}

// A boot-protocol keyboard report: send the presses and releases since the previous one (prev).
inline void keyboardReport(uint8_t* prev, const uint8_t* data, bool& capsLock) {
  if (data[2] == 1) return;  // "phantom" report: too many keys pressed at once
  uint8_t m = data[0];  // left/right ctrl 0x01/0x10, shift 0x02/0x20, alt 0x04/0x40, gui 0x08/0x80
  uint8_t mods = ((m & 0x22) ? 1 : 0) | ((m & 0x11) ? 2 : 0) | ((m & 0x44) ? 4 : 0) | ((m & 0x88) ? 8 : 0);
  bool shift = mods & 1;
  char buf[8];
  for (int i = 2; i < 8; i++) {  // released
    if (prev[i] > 3 && !reportHas(data, prev[i])) {
      const char* k = keyName(prev[i], shift, capsLock, buf);
      if (k) hwKeyEvent(k, false, mods);
    }
  }
  for (int i = 2; i < 8; i++) {  // newly pressed
    if (data[i] > 3 && !reportHas(prev, data[i])) {
      if (data[i] == 0x39) capsLock = !capsLock;
      const char* k = keyName(data[i], shift, capsLock, buf);
      if (k) hwKeyEvent(k, true, mods);
    }
  }
  memcpy(prev, data, 8);
}

}  // namespace usbhid
