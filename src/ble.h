// ble.h - Bluetooth LE for hw.cpp (included only where JS_BLE is set: the S3 boards; the classic CYD has no
// room for it). NimBLE keeps the cost low, and everything is released when an app ends (bleStop).
//   ble.scan([seconds]) -> [{ name, mac, rssi, connectable }]   a blocking scan, strongest first
//   ble.hid.begin([name]) -> true   advertise as a keyboard + mouse + gamepad; pair from the phone/PC
//   ble.hid.connected()             a computer or phone is connected
//   ble.hid.type(text)              type text (US layout)      ble.hid.tap("ctrl+c") / press(k) / release(k) / releaseAll()
//   ble.hid.move(dx, dy[, wheel])   move the mouse             ble.hid.buttons(mask) 1 left, 2 right, 4 middle / click([mask])
//   ble.hid.pad({ x, y, rx, ry, buttons })  gamepad axes -127..127, buttons a 16-bit mask
//   ble.hid.stop() / ble.stop()     leave Bluetooth and free its memory
//   ble.host.*                      the other way round: pair a BLE keyboard, mouse or gamepad (src/blehost.h)
#pragma once
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

static bool bleOn = false;
static volatile bool bleScanBusy = false;  // the scan object is shared: ble.scan() and the host's reconnect scan take turns
static NimBLEServer* bleServer = nullptr;
static NimBLEHIDDevice* bleHid = nullptr;
static NimBLECharacteristic *bleKb = nullptr, *bleMouse = nullptr, *blePad = nullptr;
static volatile bool bleLinked = false;
static uint8_t kbReport[8] = {0};
static uint8_t mouseButtons = 0;
static uint8_t padReport[6] = {0};

// Report map: id 1 keyboard (8 bytes + LED output), id 2 mouse (buttons, x, y, wheel), id 3 gamepad
// (16 buttons, X, Y, Rx, Ry as signed bytes).
static const uint8_t HID_MAP[] = {
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,                          // keyboard
  0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
  0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
  0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
  0xC0,
  0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00,   // mouse
  0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
  0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
  0xC0, 0xC0,
  0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x03,                          // gamepad
  0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
  0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
  0xC0};

class BleServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) { bleLinked = true; }
  void onDisconnect(NimBLEServer*) { bleLinked = false; NimBLEDevice::startAdvertising(); }
};

#ifndef BLE_CONNECT_MIN_KB
#define BLE_CONNECT_MIN_KB 24  // free internal memory needed to connect to a device (service discovery, report map)
#endif
#ifndef BLE_HOST_STACK
#define BLE_HOST_STACK 8192  // bytes: the stack of the task that connects and pairs
#endif
#ifndef BLE_MIN_FREE_KB
#define BLE_MIN_FREE_KB 52  // internal memory that must be free to start Bluetooth (the CoCo, with its buffers in PSRAM, needs less)
#endif
// Bluetooth needs ~45 KB of internal memory to start (about 50 KB free); on a board whose screen takes a lot (the CoCo VGA)
// starting it with less would abort the whole board, so it declines instead.
static bool bleInit(const std::string& name) {
  if (bleOn) { NimBLEDevice::init(name); return true; }
  if (esp_get_free_internal_heap_size() < BLE_MIN_FREE_KB * 1024) return false;
  NimBLEDevice::init(name);
  bleOn = true;
  return true;
}
static void bleNeedMemory(Interp& I) { I.throwError("Error", "Bluetooth needs about " + std::to_string(BLE_MIN_FREE_KB) + " KB of free memory (" + std::to_string(esp_get_free_internal_heap_size() / 1024) + " KB free): restart the board and start Bluetooth first"); }

#if EXT_INPUT
static void hostStop();
#endif
static void bleStop() {
#if EXT_INPUT
  hostStop();
#endif
  if (!bleOn) return;
  NimBLEDevice::deinit(true);
  bleOn = false;
  bleServer = nullptr;
  bleHid = nullptr;
  bleKb = bleMouse = blePad = nullptr;
  bleLinked = false;
  memset(kbReport, 0, sizeof kbReport);
  memset(padReport, 0, sizeof padReport);
  mouseButtons = 0;
}

static std::string bleName(const std::string& s) { return s.empty() ? "micro-js" : s.substr(0, 28); }

JS_NATIVE(ble_scan) {
  (void)self;
  if (bleServer) I.throwError("Error", "ble.scan: stop the keyboard first (ble.stop())");
  double secs = argc > 0 && !args[0].isUndef() ? I.toNum(args[0]) : 3;
  if (secs < 1) secs = 1;
  if (secs > 30) secs = 30;
  for (int i = 0; i < 40 && bleScanBusy; i++) delay(100);  // the host's reconnect scan may be running
  if (!bleInit("")) bleNeedMemory(I);
  bleScanBusy = true;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setDuplicateFilter(true);
  NimBLEScanResults found = scan->start((uint32_t)secs, false);
  Array* a = I.newArray();
  for (int i = 0; i < found.getCount(); i++) {
    NimBLEAdvertisedDevice d = found.getDevice(i);
    Object* o = I.newObject();
    I.setOwn(o, "name", I.str(d.haveName() ? d.getName() : std::string()));
    I.setOwn(o, "mac", I.str(d.getAddress().toString()));
    I.setOwn(o, "rssi", Value::num(d.getRSSI()));
    I.setOwn(o, "connectable", Value::boolean(d.isConnectable()));
    I.setOwn(o, "type", Value::num(d.getAddressType()));  // for ble.host.connect(mac, type)
    I.setOwn(o, "hid", Value::boolean(d.isAdvertisingService(NimBLEUUID((uint16_t)0x1812)) || (d.haveAppearance() && (d.getAppearance() & 0xFFC0) == 0x03C0)));
    a->items.push_back(Value::obj(o));
  }
  scan->clearResults();
  bleScanBusy = false;
  std::sort(a->items.begin(), a->items.end(), [&](const Value& x, const Value& y) {
    return I.toNum(I.get(y, "rssi")) < I.toNum(I.get(x, "rssi"));
  });
  return Value::obj(a);
}

JS_NATIVE(blehid_begin) {
  (void)self;
  if (bleServer) return Value::boolean(true);
  std::string name = bleName(argc > 0 && !args[0].isUndef() ? I.toStr(args[0]) : std::string());
  if (!bleInit(name)) bleNeedMemory(I);
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no passkey ("just works"), secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCb());
  bleHid = new NimBLEHIDDevice(bleServer);
  bleKb = bleHid->inputReport(1);
  bleHid->outputReport(1);
  bleMouse = bleHid->inputReport(2);
  blePad = bleHid->inputReport(3);
  bleHid->manufacturer()->setValue("micro-js");
  bleHid->pnp(0x02, 0xE502, 0xA111, 0x0210);
  bleHid->hidInfo(0x00, 0x01);
  bleHid->reportMap((uint8_t*)HID_MAP, sizeof HID_MAP);
  bleHid->startServices();
  bleHid->setBatteryLevel(100);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C4);  // gamepad: hosts then accept all three report kinds
  adv->addServiceUUID(bleHid->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->start();
  return Value::boolean(true);
}
JS_NATIVE(blehid_connected) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(bleLinked); }
JS_NATIVE(blehid_peer) {  // the address of the computer / phone that is connected ("" = none; phones change theirs, so it isn't a name)
  (void)self; (void)args; (void)argc;
  if (!bleServer || !bleLinked || bleServer->getConnectedCount() == 0) return I.str("");
  return I.str(bleServer->getPeerInfo(0).getAddress().toString());
}
JS_NATIVE(blehid_stop) { (void)I; (void)self; (void)args; (void)argc; bleStop(); return Value(); }

static void bleSendKb() { if (bleKb && bleLinked) { bleKb->setValue(kbReport, sizeof kbReport); bleKb->notify(); } }
static void bleSendMouse(int8_t dx, int8_t dy, int8_t wheel) {
  if (!bleMouse || !bleLinked) return;
  uint8_t r[4] = {mouseButtons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
  bleMouse->setValue(r, sizeof r);
  bleMouse->notify();
}
static void bleNeedLink(Interp& I, const char* what) {
  if (!bleServer) I.throwError("Error", std::string(what) + ": call ble.hid.begin() first");
}

// ASCII -> usage id (+ shift), US layout
static bool asciiKey(char c, uint8_t& code, bool& shift) {
  shift = false;
  if (c >= 'a' && c <= 'z') { code = 0x04 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { code = 0x04 + (c - 'A'); shift = true; return true; }
  if (c >= '1' && c <= '9') { code = 0x1E + (c - '1'); return true; }
  if (c == '0') { code = 0x27; return true; }
  static const char* plain = "\n\x1b\b\t -=[]\\#;'`,./";
  static const uint8_t plainC[] = {0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
  static const char* shifted = "!@#$%^&*()_+{}|:\"~<>?";
  static const uint8_t shiftedC[] = {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
  for (int i = 0; plain[i]; i++) if (plain[i] == c) { code = plainC[i]; return true; }
  for (int i = 0; shifted[i]; i++) if (shifted[i] == c) { code = shiftedC[i]; shift = true; return true; }
  return false;
}
// a key name -> modifier bit or usage id
static bool namedKey(const std::string& n, uint8_t& mod, uint8_t& code) {
  mod = 0; code = 0;
  struct N { const char* name; uint8_t mod, code; };
  static const N T[] = {
    {"ctrl", 1, 0}, {"shift", 2, 0}, {"alt", 4, 0}, {"gui", 8, 0}, {"win", 8, 0}, {"cmd", 8, 0},
    {"enter", 0, 0x28}, {"esc", 0, 0x29}, {"backspace", 0, 0x2A}, {"tab", 0, 0x2B}, {"space", 0, 0x2C},
    {"delete", 0, 0x4C}, {"home", 0, 0x4A}, {"end", 0, 0x4D}, {"pageup", 0, 0x4B}, {"pagedown", 0, 0x4E},
    {"right", 0, 0x4F}, {"left", 0, 0x50}, {"down", 0, 0x51}, {"up", 0, 0x52}, {"capslock", 0, 0x39}};
  for (const N& e : T) if (n == e.name) { mod = e.mod; code = e.code; return true; }
  if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1])) {
    int f = atoi(n.c_str() + 1);
    if (f >= 1 && f <= 12) { code = 0x3A + (f - 1); return true; }
  }
  if (n.size() == 1) {
    bool sh; uint8_t c;
    if (asciiKey(n[0], c, sh)) { code = c; mod = sh ? 2 : 0; return true; }
  }
  return false;
}
static void kbPress(uint8_t mod, uint8_t code) {
  kbReport[0] |= mod;
  if (code) {
    for (int i = 2; i < 8; i++) if (kbReport[i] == code) { bleSendKb(); return; }
    for (int i = 2; i < 8; i++) if (!kbReport[i]) { kbReport[i] = code; break; }
  }
  bleSendKb();
}
static void kbRelease(uint8_t mod, uint8_t code) {
  kbReport[0] &= ~mod;
  if (code) for (int i = 2; i < 8; i++) if (kbReport[i] == code) kbReport[i] = 0;
  bleSendKb();
}

JS_NATIVE(blehid_type) {
  (void)self;
  bleNeedLink(I, "ble.hid.type");
  std::string s = I.toStr(JS_ARG(0));
  for (char c : s) {
    if (!bleLinked) return Value::boolean(false);
    uint8_t code; bool shift;
    if (!asciiKey(c, code, shift)) continue;
    kbPress(shift ? 2 : 0, code);
    delay(6);
    kbRelease(shift ? 2 : 0, code);
    delay(6);
    if (I.poll) I.poll();
  }
  return Value::boolean(true);
}
JS_NATIVE(blehid_press) {
  (void)self;
  bleNeedLink(I, "ble.hid.press");
  uint8_t mod, code;
  if (!namedKey(I.toStr(JS_ARG(0)), mod, code)) I.throwError("Error", "ble.hid.press: unknown key");
  kbPress(mod, code);
  return Value();
}
JS_NATIVE(blehid_release) {
  (void)self;
  bleNeedLink(I, "ble.hid.release");
  uint8_t mod, code;
  if (!namedKey(I.toStr(JS_ARG(0)), mod, code)) I.throwError("Error", "ble.hid.release: unknown key");
  kbRelease(mod, code);
  return Value();
}
JS_NATIVE(blehid_releaseAll) {
  (void)self; (void)args; (void)argc;
  bleNeedLink(I, "ble.hid.releaseAll");
  memset(kbReport, 0, sizeof kbReport);
  bleSendKb();
  return Value();
}
JS_NATIVE(blehid_tap) {  // "ctrl+c", "alt+f4", "enter"
  (void)self;
  bleNeedLink(I, "ble.hid.tap");
  std::string combo = I.toStr(JS_ARG(0));
  std::vector<std::pair<uint8_t, uint8_t>> keys;
  size_t start = 0;
  while (start <= combo.size()) {
    size_t plus = combo.find('+', start);
    std::string part = combo.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
    uint8_t mod, code;
    if (part.empty() && plus != std::string::npos) { part = "+"; plus = combo.find('+', plus + 1); }  // a literal +
    if (!namedKey(part, mod, code)) I.throwError("Error", "ble.hid.tap: unknown key \"" + part + "\"");
    keys.push_back({mod, code});
    if (plus == std::string::npos) break;
    start = plus + 1;
  }
  for (auto& k : keys) kbPress(k.first, k.second);
  delay(12);
  for (auto& k : keys) kbRelease(k.first, k.second);
  return Value();
}

static int8_t clamp8(int v) { return (int8_t)(v < -127 ? -127 : v > 127 ? 127 : v); }
JS_NATIVE(blehid_move) {
  (void)self;
  bleNeedLink(I, "ble.hid.move");
  int dx = IA(0), dy = IA(1), wh = IA(2);
  while (dx || dy || wh) {  // a report holds -127..127
    int8_t sx = clamp8(dx), sy = clamp8(dy), sw = clamp8(wh);
    bleSendMouse(sx, sy, sw);
    dx -= sx; dy -= sy; wh -= sw;
    if (dx || dy || wh) delay(4);
  }
  return Value();
}
JS_NATIVE(blehid_buttons) {
  (void)self;
  bleNeedLink(I, "ble.hid.buttons");
  mouseButtons = (uint8_t)(IA(0) & 7);
  bleSendMouse(0, 0, 0);
  return Value();
}
JS_NATIVE(blehid_click) {
  (void)self;
  bleNeedLink(I, "ble.hid.click");
  uint8_t m = argc > 0 && !args[0].isUndef() ? (uint8_t)(IA(0) & 7) : 1;
  mouseButtons = m;
  bleSendMouse(0, 0, 0);
  delay(30);
  mouseButtons = 0;
  bleSendMouse(0, 0, 0);
  return Value();
}
JS_NATIVE(blehid_pad) {
  (void)self;
  bleNeedLink(I, "ble.hid.pad");
  Value o = JS_ARG(0);
  auto num = [&](const char* k) { Value v = I.get(o, k); return v.isUndef() ? 0 : I.toInt32(v); };
  int b = num("buttons");
  padReport[0] = (uint8_t)b;
  padReport[1] = (uint8_t)(b >> 8);
  padReport[2] = (uint8_t)clamp8(num("x"));
  padReport[3] = (uint8_t)clamp8(num("y"));
  padReport[4] = (uint8_t)clamp8(num("rx"));
  padReport[5] = (uint8_t)clamp8(num("ry"));
  if (blePad && bleLinked) { blePad->setValue(padReport, sizeof padReport); blePad->notify(); }
  return Value();
}

#if EXT_INPUT  // boards whose keyboard/joystick input can come from another driver (not the Cardputer, which has its own)
#include "blehost.h"
#endif

// an app ended: a running host (paired keyboard / mouse / gamepad) stays up for the next app; anything else is released
static void bleAppEnd() {
#if EXT_INPUT
  if (hostRun && !bleServer) { bleHostReset(); return; }
#endif
  bleStop();
}
