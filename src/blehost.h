// blehost.h - the board as a Bluetooth LE HID host: pair a BLE keyboard, mouse or gamepad and use it like
// the USB ones. Included at the end of ble.h (only where JS_BLE is set).
//   ble.host.begin() -> true        start: bonded devices are reconnected whenever they're in range
//   ble.host.connect(mac[, type])   pair / connect one device (from ble.scan: hid is true for HID devices)
//   ble.host.status()               { state: "idle"|"connecting"|"pairing"|"connected"|"failed", passkey, error, devices }
//                                   passkey: a keyboard asks for it to be typed on the keyboard itself
//   ble.host.devices()              [{ mac, name, kind: "keyboard"|"mouse"|"gamepad" ... }]
//   ble.host.bonded()  /  ble.host.forget(mac | "*")  /  ble.host.cursor(on)  /  ble.host.stop()
// Keys arrive as keyboard.on("key"); a gamepad is joystick 0 or 1; a mouse moves an on-screen arrow
// and its left button is a touch (touch.on "down"/"move"/"up"), so touch apps work with it.
// Each device's HID report descriptor is parsed once, so any standard keyboard, mouse or gamepad decodes.
#pragma once
#include "usb_hid.h"

// ---------------------------------------------------------------- report descriptor -> fields
enum { E_MOD = 1, E_KEYS, E_BTN, E_AXIS, E_HAT, E_WHEEL, E_TRIG };
struct HidElem { uint8_t kind, id; uint16_t bit; uint8_t size, count; uint16_t usage; int32_t lmin, lmax; bool rel; };
static const int HID_MAX_ELEMS = 48;
struct HidMap {
  HidElem e[HID_MAX_ELEMS];
  int n;
  uint8_t apps;  // bit 0 keyboard, 1 mouse, 2 gamepad
  uint8_t appOf[HID_MAX_ELEMS];
};

static int32_t hidItem(const uint8_t* d, int size, bool sign) {
  uint32_t v = 0;
  for (int i = 0; i < size; i++) v |= (uint32_t)d[i] << (8 * i);
  if (sign && size && size < 4 && (v & (1u << (size * 8 - 1)))) v |= ~0u << (size * 8);
  return (int32_t)v;
}

static void hidParse(const uint8_t* d, size_t len, HidMap& m) {
  m.n = 0;
  m.apps = 0;
  uint16_t page = 0, usages[12], umin = 0;
  int nu = 0, depth = 0, app = 0;
  bool range = false;
  int32_t lmin = 0, lmax = 0;
  uint8_t rsize = 0, rcount = 0, rid = 0;
  struct { uint8_t id; uint16_t bit; } offs[8];
  int noffs = 0;
  auto off = [&](uint8_t id) -> uint16_t& {
    for (int i = 0; i < noffs; i++) if (offs[i].id == id) return offs[i].bit;
    if (noffs < 8) { offs[noffs].id = id; offs[noffs].bit = 0; return offs[noffs++].bit; }
    return offs[7].bit;
  };
  auto add = [&](uint8_t kind, uint16_t bit, uint8_t size, uint8_t count, uint16_t usage, bool rel) {
    if (m.n >= HID_MAX_ELEMS) return;
    m.e[m.n] = {kind, rid, bit, size, count, usage, lmin, lmax, rel};
    m.appOf[m.n++] = (uint8_t)app;
    m.apps |= 1 << (app - 1);
  };
  for (size_t i = 0; i < len;) {
    uint8_t b = d[i];
    if (b == 0xFE) { i += (i + 1 < len ? d[i + 1] : 0) + 3; continue; }  // long item: skip
    int size = b & 3;
    if (size == 3) size = 4;
    int type = (b >> 2) & 3, tag = b >> 4;
    if (i + 1 + size > len) break;
    const uint8_t* p = d + i + 1;
    i += 1 + size;
    if (type == 1) {  // global
      switch (tag) {
        case 0: page = (uint16_t)hidItem(p, size, false); break;
        case 1: lmin = hidItem(p, size, true); break;
        case 2: lmax = hidItem(p, size, lmin < 0); break;
        case 7: rsize = (uint8_t)hidItem(p, size, false); break;
        case 8: rid = (uint8_t)hidItem(p, size, false); break;
        case 9: rcount = (uint8_t)hidItem(p, size, false); break;
      }
    } else if (type == 2) {  // local
      if (tag == 0 && nu < 12) usages[nu++] = (uint16_t)hidItem(p, size, false);
      else if (tag == 1) { umin = (uint16_t)hidItem(p, size, false); range = true; }
    } else if (type == 0) {  // main
      if (tag == 8) {  // Input
        int flags = hidItem(p, size, false);
        uint16_t start = off(rid);
        bool variable = flags & 2, rel = flags & 4;
        if (!(flags & 1) && app) {
          for (int k = 0; k < rcount; k++) {
            uint16_t u = k < nu ? usages[k] : range ? umin + k : nu ? usages[nu - 1] : 0;
            uint16_t bit = start + k * rsize;
            if (page == 7 && variable && u >= 0xE0 && u <= 0xE7) add(E_MOD, bit, 1, 1, u - 0xE0, false);
            else if (page == 9 && variable) add(E_BTN, bit, rsize, 1, u, false);
            else if (page == 2 && variable && (u == 0xC4 || u == 0xC5)) add(E_TRIG, bit, rsize, 1, u, false);  // accelerator / brake: Xbox triggers
            else if (page == 1 && variable) {
              if (u == 0x39) add(E_HAT, bit, rsize, 1, u, false);
              else if (u == 0x38) add(E_WHEEL, bit, rsize, 1, u, rel);
              else if (u >= 0x30 && u <= 0x35) add(E_AXIS, bit, rsize, 1, u, rel);
            }
          }
          if (page == 7 && !variable) add(E_KEYS, start, rsize, rcount, 0, false);
        }
        off(rid) += (uint16_t)(rsize * rcount);
      } else if (tag == 10) {  // Collection
        if (depth == 0) {
          int u = nu ? usages[0] : 0;
          app = page == 1 ? (u == 6 || u == 7 ? 1 : u == 2 ? 2 : u == 4 || u == 5 ? 3 : 0) : 0;
        }
        depth++;
      } else if (tag == 12) {  // End Collection
        if (depth > 0) depth--;
        if (!depth) app = 0;
      }
      nu = 0;
      range = false;
    }
  }
}

static uint32_t hidBits(const uint8_t* d, size_t len, uint16_t bit, uint8_t size) {
  uint32_t v = 0;
  for (int i = 0; i < size && i < 32; i++) {
    size_t at = (size_t)(bit + i);
    if ((at >> 3) < len && ((d[at >> 3] >> (at & 7)) & 1)) v |= 1u << i;
  }
  return v;
}
static int32_t hidValue(const uint8_t* d, size_t len, const HidElem& e) {
  uint32_t v = hidBits(d, len, e.bit, e.size);
  if (e.lmin < 0 && e.size < 32 && (v & (1u << (e.size - 1)))) v |= ~0u << e.size;
  return (int32_t)v;
}

// ---------------------------------------------------------------- connected devices
static const int HOST_MAX = 3;
struct HostDev {
  bool used;
  volatile bool gone;
  NimBLEClient* c;
  std::string mac, name;
  HidMap map;
  uint8_t prevKb[8];
  bool caps;
  int slot;  // joystick number, -1 = none
  uint32_t rx;     // reports received from this device (ble.host.devices()[i].rx)
  bool keysPad;    // a keyboard+mouse "controller" (Steam Controller in lizard mode): its keys also drive a joystick number
  uint8_t bootKb;  // the keyboard's boot-protocol characteristic stands in for a missing report map
  std::string rawMap, gatt;  // the report descriptor bytes, and a list of every service / characteristic (ble.host.dump())
  struct { uint16_t handle; uint8_t id; } subs[9];
  struct { uint16_t handle; uint8_t id; } feats[4];  // HID feature reports (id, characteristic)
  int nfeats, subOk;  // feature reports found; input characteristics whose notifications were enabled
  uint16_t valveIn, valveOut;  // the Steam Controller's private service: notify / write characteristics (0 = none)
  int nsubs;
};
static HostDev hostDevs[HOST_MAX];

// the mouse pointer (screen coordinates), shared with the main loop
static portMUX_TYPE ptrMux = portMUX_INITIALIZER_UNLOCKED;
static int ptrX = 0, ptrY = 0, ptrW = 320, ptrH = 240;
static uint8_t ptrButtons = 0;
static volatile bool ptrActive = false, ptrMoved = false;
static bool ptrCursorOn = true;

static void ptrUpdate(int dx, int dy, int buttons) {
  portENTER_CRITICAL(&ptrMux);
  ptrX = std::max(0, std::min(ptrW - 1, ptrX + dx));
  ptrY = std::max(0, std::min(ptrH - 1, ptrY + dy));
  ptrButtons = (uint8_t)buttons;
  ptrActive = true;
  ptrMoved = true;
  portEXIT_CRITICAL(&ptrMux);
}
static bool blePointerDown(int& x, int& y, int& z) {
  if (!ptrActive) return false;
  portENTER_CRITICAL(&ptrMux);
  bool down = ptrButtons & 1;
  x = ptrX; y = ptrY; z = 1000;
  portEXIT_CRITICAL(&ptrMux);
  return down;
}

// ble.host.decode(n): 0 ignore what a device sends, 1 everything (default), 2 gamepads without the arrow-key emulation,
// 3 decode but send nothing on (for tracking down faults)
static volatile int hostDecodeMode = 1;
static volatile uint32_t hostRx = 0, hostPads = 0;  // reports received / gamepad states sent on (ble.host.status().rx / .pads)
static void hostDecode(HostDev& d, uint8_t id, const uint8_t* data, size_t len) {
  if (hostDecodeMode == 0) return;
  const HidMap& m = d.map;
  if (d.bootKb && !m.n) {  // boot-protocol keyboard (8 bytes)
    if (len >= 8) usbhid::keyboardReport(d.prevKb, data, d.caps);
    return;
  }
  uint8_t kb[8] = {0};
  bool isKb = false, isPad = false, isMouse = false;
  HwJoystick js{};
  js.connected = true;
  js.hat = -1;
  int axisSlot[4] = {-1, -1, -1, -1}, mdx = 0, mdy = 0, mouseBtn = 0, nkeys = 0;
  (void)axisSlot;
  for (int i = 0; i < m.n; i++) {
    const HidElem& e = m.e[i];
    if (e.id != id) continue;
    int app = m.appOf[i];
    if (app == 1) {
      isKb = true;
      if (e.kind == E_MOD) { if (hidBits(data, len, e.bit, 1)) kb[0] |= 1 << e.usage; }
      else if (e.kind == E_KEYS) {
        for (int k = 0; k < e.count; k++) {
          uint32_t code = hidBits(data, len, e.bit + k * e.size, e.size);
          if (code > 3 && nkeys < 6) kb[2 + nkeys++] = (uint8_t)code;
        }
      }
    } else if (app == 2) {
      isMouse = true;
      int32_t v = hidValue(data, len, e);
      if (e.kind == E_BTN && v) { if (e.usage >= 1 && e.usage <= 3) mouseBtn |= 1 << (e.usage - 1); }
      else if (e.kind == E_AXIS && e.rel) { if (e.usage == 0x30) mdx = v; else if (e.usage == 0x31) mdy = v; }
    } else if (app == 3) {
      isPad = true;
      int32_t v = hidValue(data, len, e);
      if (e.kind == E_BTN) { if (v && e.usage >= 1 && e.usage <= 32) js.buttons |= 1u << (e.usage - 1); }
      else if (e.kind == E_HAT) { int idx = v - e.lmin; js.hat = (idx >= 0 && idx <= 7) ? idx : -1; }
      else if (e.kind == E_TRIG) { int32_t span = e.lmax - e.lmin; if (span > 0) js.trig[e.usage == 0xC5 ? 0 : 1] = (uint8_t)(((int64_t)(v - e.lmin) * 255) / span); }
      else if (e.kind == E_AXIS) {
        int a = e.usage == 0x30 ? 0 : e.usage == 0x31 ? 1 : (e.usage == 0x32 || e.usage == 0x33) ? 2 : (e.usage == 0x35 || e.usage == 0x34) ? 3 : -1;
        int32_t span = e.lmax - e.lmin;
        if (a >= 0 && span > 0) js.axis[a] = (int8_t)(((int64_t)(v - e.lmin) * 254) / span - 127);
      }
    }
  }
  if (hostDecodeMode == 3) return;
  if (isKb) usbhid::keyboardReport(d.prevKb, kb, d.caps);
  if (isKb && d.keysPad && d.slot >= 0) {  // the controller's arrow keys are a stick + hat, Enter / Space / Tab / F9 buttons 1-4
    HwJoystick j{};
    j.connected = true;
    j.hat = -1;
    bool up = false, dn = false, lf = false, rt = false;
    for (int i = 2; i < 8; i++) {
      uint8_t k = kb[i];
      if (k == 0x52) up = true; else if (k == 0x51) dn = true; else if (k == 0x50) lf = true; else if (k == 0x4F) rt = true;
      else if (k == 0x28) j.buttons |= 1; else if (k == 0x2C) j.buttons |= 2; else if (k == 0x2B) j.buttons |= 4; else if (k == 0x42) j.buttons |= 8;
    }
    j.axis[0] = rt ? 127 : lf ? -127 : 0;
    j.axis[1] = dn ? 127 : up ? -127 : 0;
    static const int8_t hats[3][3] = {{7, 0, 1}, {6, -1, 2}, {5, 4, 3}};  // [row: up, none, down][col: left, none, right]
    int r = up ? 0 : dn ? 2 : 1, c = lf ? 0 : rt ? 2 : 1;
    j.hat = hats[r][c];
    hwJoystickUpdateQuiet(d.slot, j);
  }
  if (isMouse) ptrUpdate(mdx, mdy, mouseBtn);
  if (isPad && d.slot >= 0) { hostPads++; if (hostDecodeMode == 2) hwJoystickUpdateQuiet(d.slot, js); else hwJoystickUpdate(d.slot, js); }
}

// reports no collection of the report map covers (a controller's vendor-defined state), for ble.host.raw()
struct RawRep { uint8_t id, len; uint8_t d[24]; };
static const int RAW_Q = 12;
static RawRep rawQ[RAW_Q];
static volatile int rawHead = 0, rawTail = 0;
static volatile bool rawOn = false;
static void rawPush(uint8_t id, const uint8_t* data, size_t len) {
  int next = (rawHead + 1) % RAW_Q;
  if (next == rawTail) rawTail = (rawTail + 1) % RAW_Q;  // full: drop the oldest
  rawQ[rawHead].id = id;
  rawQ[rawHead].len = (uint8_t)std::min<size_t>(len, 24);
  memcpy(rawQ[rawHead].d, data, rawQ[rawHead].len);
  __sync_synchronize();
  rawHead = next;
}

static void hostNotify(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool) {
  hostRx++;
  NimBLEClient* c = chr->getRemoteService()->getClient();
  for (HostDev& d : hostDevs) {
    if (!d.used || d.c != c) continue;
    d.rx++;
    uint8_t id = 0;
    for (int i = 0; i < d.nsubs; i++) if (d.subs[i].handle == chr->getHandle()) id = d.subs[i].id;
    if (rawOn) {
      bool known = false;
      for (int i = 0; i < d.map.n; i++) known |= d.map.e[i].id == id && id != 0xFE;
      if (!known) rawPush(id, data, len);
    }
    hostDecode(d, id, data, len);
    return;
  }
}

// ---------------------------------------------------------------- the connection task
enum HostState { H_IDLE, H_CONNECTING, H_PAIRING, H_CONNECTED, H_FAILED };
static volatile int hostState = H_IDLE;
static volatile uint32_t hostPasskey = 0;
static std::string hostError, hostReqMac;
static uint8_t hostReqType = 0;
static volatile bool hostWant = false, hostRun = false, hostBusy = false;
static TaskHandle_t hostTask = nullptr;

class HostCb : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) {}
  void onDisconnect(NimBLEClient* c) {
    for (HostDev& d : hostDevs) if (d.used && d.c == c) d.gone = true;
  }
};
static HostCb hostCb;

static const char* kindName(uint8_t apps, int bit) { return bit == 0 ? "keyboard" : bit == 1 ? "mouse" : "gamepad"; }

static void hostDrop(HostDev& d) {
  if (d.slot >= 0) { HwJoystick off{}; hwJoystickUpdate(d.slot, off); }
  if (d.map.apps & 2) { portENTER_CRITICAL(&ptrMux); ptrButtons = 0; portEXIT_CRITICAL(&ptrMux); }
  if (d.c) { if (d.c->isConnected()) d.c->disconnect(); NimBLEDevice::deleteClient(d.c); }
  d.used = false;
  d.gone = false;
  d.c = nullptr;
}

// The devices paired from here (the bonds hold the keys; this list says which ones are HID devices to
// reconnect - the radio's bond list also holds computers that paired with the keyboard emulation).
struct HostSaved { std::string mac, name; uint8_t type; };
static std::vector<HostSaved> hostSavedLoad() {
  std::vector<HostSaved> v;
  std::string t = nvsGet("mjsble", "host");
  size_t at = 0;
  while (at < t.size()) {  // "AA:BB:CC:DD:EE:FF/0/name;..."
    size_t semi = t.find(';', at);
    std::string e = t.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
    size_t s1 = e.find('/'), s2 = s1 == std::string::npos ? s1 : e.find('/', s1 + 1);
    if (s1 != std::string::npos) v.push_back({e.substr(0, s1), s2 == std::string::npos ? "" : e.substr(s2 + 1), (uint8_t)atoi(e.c_str() + s1 + 1)});
    if (semi == std::string::npos) break;
    at = semi + 1;
  }
  return v;
}
static void hostSavedStore(const std::vector<HostSaved>& v) {
  std::string t;
  for (auto& e : v) t += e.mac + "/" + std::to_string(e.type) + "/" + e.name + ";";
  nvsPut("mjsble", "host", t);
}
// A new pairing replaces an older entry with the same address or the same name (a Steam Controller takes a new
// address each time it enters pairing mode, and the old one is dead); the list keeps the last four.
static void hostSavedAdd(const std::string& mac, uint8_t type, std::string name) {
  for (char& c : name) if (c == ';' || c == '/') c = ' ';
  auto v = hostSavedLoad();
  v.erase(std::remove_if(v.begin(), v.end(), [&](const HostSaved& e) { return e.mac == mac || (!name.empty() && e.name == name); }), v.end());
  v.push_back({mac, name, type});
  while (v.size() > 4) v.erase(v.begin());
  hostSavedStore(v);
}

static bool hostConnect(const std::string& mac, uint8_t type, const std::string& nameHint, std::string& err, bool user) {
  for (HostDev& d : hostDevs) if (d.used && d.mac == mac && !d.gone) return true;
  if (esp_get_free_internal_heap_size() < BLE_CONNECT_MIN_KB * 1024) { err = "not enough free memory to connect (" + std::to_string(esp_get_free_internal_heap_size() / 1024) + " KB)"; return false; }
  HostDev* slot = nullptr;
  for (HostDev& d : hostDevs) if (!d.used) { slot = &d; break; }
  if (!slot) { err = "three devices are connected already"; return false; }
  NimBLEClient* c = NimBLEDevice::createClient();
  c->setClientCallbacks(&hostCb, false);
  c->setConnectTimeout(user ? 20 : 8);  // seconds: a user's pick gets longer (the device may only advertise now and then)
  if (user) hostState = H_CONNECTING;
  if (!c->connect(NimBLEAddress(mac, type), false)) { NimBLEDevice::deleteClient(c); err = "can't reach the device (is it in pairing mode?)"; return false; }
  if (user) hostState = H_PAIRING;
  uint32_t pk = 100000 + (esp_random() % 900000);
  NimBLEDevice::setSecurityPasskey(pk);
  hostPasskey = pk;
  bool secured = c->secureConnection();
  hostPasskey = 0;
  if (!secured) { c->disconnect(); NimBLEDevice::deleteClient(c); err = "pairing failed"; return false; }
  NimBLERemoteService* svc = c->getService(NimBLEUUID((uint16_t)0x1812));
  if (!svc) { c->disconnect(); NimBLEDevice::deleteClient(c); err = "not a HID device"; return false; }
  slot->used = false;  // (not memset: the record holds std::strings)
  slot->gone = false;
  slot->map.n = 0;
  slot->map.apps = 0;
  memset(slot->prevKb, 0, sizeof slot->prevKb);
  slot->caps = false;
  slot->bootKb = 0;
  slot->keysPad = false;
  slot->rx = 0;
  slot->nsubs = 0;
  slot->nfeats = 0;
  slot->subOk = 0;
  slot->valveIn = slot->valveOut = 0;
  slot->rawMap.clear();
  slot->gatt.clear();
  slot->c = c;
  slot->mac = mac;
  slot->name = nameHint;
  slot->slot = -1;
  {  // everything the device offers (for ble.host.dump). First: this rediscovers the services, which drops any notification handler set before it: service and characteristic UUIDs with what each can do
    char t[96];
    auto services = c->getServices(true);
    if (services) for (NimBLERemoteService* sv : *services) {
      slot->gatt += "S " + sv->getUUID().toString() + "\n";
      auto cs = sv->getCharacteristics(true);
      if (cs) for (NimBLERemoteCharacteristic* ch : *cs) {
        snprintf(t, sizeof t, "  C %s %s%s%s%s%s%s\n", ch->getUUID().toString().c_str(), ch->canRead() ? "R" : "", ch->canWrite() ? "W" : "", ch->canWriteNoResponse() ? "w" : "", ch->canNotify() ? "N" : "", ch->canIndicate() ? "I" : "", "");
        slot->gatt += t;
      }
    }
  }
  svc = c->getService(NimBLEUUID((uint16_t)0x1812));  // (the listing above rebuilt the service objects)
  if (!svc) { c->disconnect(); NimBLEDevice::deleteClient(c); err = "not a HID device"; return false; }
  auto chars = svc->getCharacteristics(true);
  for (NimBLERemoteCharacteristic* ch : *chars) {
    if (ch->getUUID() == NimBLEUUID((uint16_t)0x2A4B) && ch->canRead()) {
      NimBLEAttValue v = ch->readValue();
      hidParse(v.data(), v.size(), slot->map);
      slot->rawMap.assign((const char*)v.data(), v.size());
    }
  }
  bool any = false;
  for (NimBLERemoteCharacteristic* ch : *chars) {
    if (ch->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && ch->canNotify() && slot->nsubs < 8) {
      uint8_t id = 0, kind = 1;
      NimBLERemoteDescriptor* ref = ch->getDescriptor(NimBLEUUID((uint16_t)0x2908));
      if (ref) { NimBLEAttValue r = ref->readValue(); if (r.size() >= 2) { id = r.data()[0]; kind = r.data()[1]; } }
      if (kind != 1) continue;  // input reports only
      slot->subs[slot->nsubs].handle = ch->getHandle();
      slot->subs[slot->nsubs++].id = id;
      if (ch->subscribe(true, hostNotify, true)) { any = true; slot->subOk++; }
    }
  }
  for (NimBLERemoteCharacteristic* ch : *chars) {  // feature reports: commands to the device
    if (ch->getUUID() == NimBLEUUID((uint16_t)0x2A4D) && !ch->canNotify() && ch->canWrite() && slot->nfeats < 4) {
      NimBLERemoteDescriptor* ref = ch->getDescriptor(NimBLEUUID((uint16_t)0x2908));
      if (ref) {
        NimBLEAttValue r = ref->readValue();
        if (r.size() >= 2 && r.data()[1] == 3) { slot->feats[slot->nfeats].handle = ch->getHandle(); slot->feats[slot->nfeats++].id = r.data()[0]; }
      }
    }
  }
  if (NimBLERemoteService* vs = c->getService(NimBLEUUID("100f6c32-1735-4313-b402-38567131e5f3"))) {  // Valve's own service
    if (NimBLERemoteCharacteristic* vi = vs->getCharacteristic(NimBLEUUID("100f6c33-1735-4313-b402-38567131e5f3"))) {
      slot->valveIn = vi->getHandle();
      if (vi->canNotify() && slot->nsubs < 9) {
        slot->subs[slot->nsubs].handle = vi->getHandle();
        slot->subs[slot->nsubs++].id = 0xFE;
        vi->subscribe(true, hostNotify, true);
      }
    }
    if (NimBLERemoteCharacteristic* vo = vs->getCharacteristic(NimBLEUUID("100f6c34-1735-4313-b402-38567131e5f3"))) slot->valveOut = vo->getHandle();
  }
  if (!any && !slot->map.n) {  // no report map: a boot-protocol keyboard
    NimBLERemoteCharacteristic* bk = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A22));
    if (bk && bk->canNotify() && bk->subscribe(true, hostNotify, true)) { slot->bootKb = 1; any = true; slot->map.apps = 1; }
  }
  if (!any) { c->disconnect(); NimBLEDevice::deleteClient(c); err = "no input reports"; return false; }
  if (slot->name.empty()) {
    NimBLERemoteService* gap = c->getService(NimBLEUUID((uint16_t)0x1800));
    NimBLERemoteCharacteristic* nm = gap ? gap->getCharacteristic(NimBLEUUID((uint16_t)0x2A00)) : nullptr;
    if (nm && nm->canRead()) slot->name = nm->readValue().c_str();
  }
  auto has = [&](const char* w) { return slot->name.find(w) != std::string::npos; };
  bool lizard = (slot->map.apps & 3) == 3 && !(slot->map.apps & 4) && (has("Steam") || has("Controller") || has("Gamepad"));
  if ((slot->map.apps & 4) || lizard) {  // a gamepad, or a controller sending keys: the lowest free joystick number
    bool used[HW_JOYSTICKS] = {};
    for (HostDev& d : hostDevs) if (d.used && d.slot >= 0) used[d.slot] = true;
    for (int n = 0; n < HW_JOYSTICKS; n++) if (!used[n]) { slot->slot = n; break; }
    slot->keysPad = lizard && slot->slot >= 0;
  }
  slot->used = true;
  hostSavedAdd(mac, type, slot->name);
  return true;
}

static void hostLoop(void*) {
  uint32_t lastAuto = 0;
  while (hostRun) {
    for (HostDev& d : hostDevs) if (d.used && d.gone) hostDrop(d);
    if (hostWant) {
      hostBusy = true;
      std::string err;
      bool ok = hostConnect(hostReqMac, hostReqType, "", err, true);
      hostError = ok ? "" : err;
      hostState = ok ? H_CONNECTED : H_FAILED;
      hostWant = false;
      hostBusy = false;
    } else if (millis() - lastAuto > 600) {  // bonded devices come back by themselves (the scan below is the wait)
      lastAuto = millis();
      // Reconnect: a bonded device that wakes up advertises with a private address that changes, so connecting to the
      // saved address finds nothing. Scan instead, and connect to a device that is a saved one by address or name, or a
      // bonded HID device.
      auto saved = hostSavedLoad();
      bool missing = false;
      for (auto& e : saved) {
        bool have = false;
        for (HostDev& d : hostDevs) if (d.used && !d.gone && (d.mac == e.mac || (!e.name.empty() && d.name == e.name))) have = true;
        missing |= !have;
      }
      if (missing && !bleScanBusy) {
        bleScanBusy = true;
        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setActiveScan(true);
        scan->setInterval(100);
        scan->setWindow(99);
        scan->setDuplicateFilter(true);
        NimBLEScanResults found = scan->start(3, false);  // a device that wakes advertises for only a short while: listen for most of the time
        struct Cand { std::string mac, name; uint8_t type; };
        std::vector<Cand> cands;
        for (int i = 0; i < found.getCount(); i++) {
          NimBLEAdvertisedDevice d = found.getDevice(i);
          std::string mac = d.getAddress().toString(), nm = d.haveName() ? d.getName() : std::string();
          bool match = false;
          for (auto& e : saved) match |= e.mac == mac || (!e.name.empty() && nm == e.name);
          if (!match && d.isAdvertisingService(NimBLEUUID((uint16_t)0x1812)) && NimBLEDevice::isBonded(d.getAddress())) match = true;
          if (match) cands.push_back({mac, nm, d.getAddressType()});
        }
        scan->clearResults();
        bleScanBusy = false;
        for (Cand& c : cands) {
          if (!hostRun || hostWant) break;
          bool have = false;
          for (HostDev& d : hostDevs) if (d.used && !d.gone && d.mac == c.mac) have = true;
          if (have) continue;
          std::string err;
          hostBusy = true;
          if (hostConnect(c.mac, c.type, c.name, err, false)) hostState = H_CONNECTED;
          hostBusy = false;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  for (HostDev& d : hostDevs) if (d.used) hostDrop(d);
  hostState = H_IDLE;
  hostTask = nullptr;
  vTaskDelete(nullptr);
}

static void hostStop() {
  if (!hostTask && !hostRun) return;
  hostRun = false;
  for (int i = 0; i < 100 && hostTask; i++) vTaskDelay(pdMS_TO_TICKS(50));
  ptrActive = false;
  ptrCursorOn = true;
}

// ---------------------------------------------------------------- the on-screen arrow
static uint16_t curSave[12 * 19];
static int curX = -1, curY = -1;
static const uint16_t* curBits() {
  static const char* rows[19] = {
    "X...........", "XX..........", "XWX.........", "XWWX........", "XWWWX.......", "XWWWWX......", "XWWWWWX.....", "XWWWWWWX....",
    "XWWWWWWWX...", "XWWWWWWWWX..", "XWWWWWXXXXX.", "XWWXWWX.....", "XWX.XWWX....", "XX..XWWX....", "X....XWWX...", ".....XWWX...",
    "......XWWX..", "......XXX...", "............"};
  static uint16_t px[12 * 19];
  static bool made = false;
  if (!made) {
    for (int y = 0; y < 19; y++) for (int x = 0; x < 12; x++) px[y * 12 + x] = rows[y][x] == 'X' ? 0x0000 : rows[y][x] == 'W' ? 0xFFFF : 0x0001;  // 1 = transparent
    made = true;
  }
  return px;
}
static void cursorHide() {
  if (curX < 0) return;
  tft.pushRect(curX, curY, 12, 19, curSave);
  curX = curY = -1;
}
static void cursorShow(int x, int y) {
  int w = std::min(12, ptrW - x), h = std::min(19, ptrH - y);
  if (w <= 0 || h <= 0) return;
  uint16_t line[12];
  for (int r = 0; r < 19; r++) {  // read row by row into the save area, only the visible part
    if (r < h) tft.readRect(x, y + r, w, 1, line); else memset(line, 0, sizeof line);
    memcpy(curSave + r * 12, line, sizeof line);
  }
  const uint16_t* bits = curBits();
  for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) { uint16_t v = bits[r * 12 + c]; if (v != 1) tft.drawPixel(x + c, y + r, v); }
  curX = x;
  curY = y;
}
static void bleHostService() {
  if (!ptrActive) return;
  static uint32_t last = 0;
  if (!ptrCursorOn || !ptrMoved || millis() - last < 20) return;
  last = millis();
  int x, y;
  portENTER_CRITICAL(&ptrMux);
  x = ptrX; y = ptrY; ptrMoved = false;
  portEXIT_CRITICAL(&ptrMux);
  if (curX >= 0 && tft.readPixel(curX, curY) != 0x0000) curX = -1;  // the app drew over the arrow: don't paste old pixels back
  cursorHide();
  cursorShow(x, y);
}

// ---------------------------------------------------------------- JavaScript
static bool hostStart() {
  if (bleServer) return false;
  if (hostRun) return true;
  if (!bleInit("")) return false;
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM: gamepads and mice can only do "Just Works" (a keyboard that wants a passkey still asks)
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  ptrW = tft.width();
  ptrH = tft.height();
  ptrX = ptrW / 2;
  ptrY = ptrH / 2;
  hostRun = true;
  xTaskCreate(hostLoop, "blehost", BLE_HOST_STACK, nullptr, 2, &hostTask);
  return hostTask != nullptr;
}
// an app ended: the pointer arrow's saved pixels are the old app's, so forget them
static void bleHostReset() { curX = curY = -1; ptrMoved = true; }
JS_NATIVE(host_begin) {
  (void)self; (void)args; (void)argc;
  if (bleServer) I.throwError("Error", "ble.host: stop the keyboard/mouse emulation first (ble.stop())");
  bool ok = hostStart();
  if (!ok && !hostRun && esp_get_free_internal_heap_size() < BLE_MIN_FREE_KB * 1024) bleNeedMemory(I);
  return Value::boolean(ok);
}
JS_NATIVE(host_connect) {
  (void)self;
  if (!hostRun) I.throwError("Error", "ble.host.connect: call ble.host.begin() first");
  if (hostWant || hostBusy) return Value::boolean(false);
  hostReqMac = I.toStr(JS_ARG(0));
  hostReqType = argc > 1 && !args[1].isUndef() ? (uint8_t)IA(1) : 0;
  hostError.clear();
  hostState = H_CONNECTING;
  hostWant = true;
  return Value::boolean(true);
}
static Value hostDeviceList(Interp& I) {
  Array* a = I.newArray();
  for (HostDev& d : hostDevs) {
    if (!d.used || d.gone) continue;
    Object* o = I.newObject();
    I.setOwn(o, "mac", I.str(d.mac));
    I.setOwn(o, "name", I.str(d.name));
    std::string kinds;
    for (int b = 0; b < 3; b++) if (d.map.apps & (1 << b)) kinds += std::string(kinds.empty() ? "" : "+") + kindName(d.map.apps, b);
    I.setOwn(o, "kind", I.str(kinds));
    if (d.slot >= 0) I.setOwn(o, "joystick", Value::num(d.slot));
    I.setOwn(o, "rx", Value::num(d.rx));
    a->items.push_back(Value::obj(o));
  }
  return Value::obj(a);
}
JS_NATIVE(host_status) {
  (void)self; (void)args; (void)argc;
  static const char* names[] = {"idle", "connecting", "pairing", "connected", "failed"};
  Object* o = I.newObject();
  int st = hostState;
  bool any = false;
  for (HostDev& d : hostDevs) any |= d.used && !d.gone;
  if (st == H_IDLE && any) st = H_CONNECTED;
  I.setOwn(o, "state", I.str(names[st]));
  I.setOwn(o, "passkey", Value::num((double)hostPasskey));
  I.setOwn(o, "error", I.str(hostError));
  I.setOwn(o, "devices", hostDeviceList(I));
  I.setOwn(o, "rx", Value::num(hostRx));
  I.setOwn(o, "pads", Value::num(hostPads));
  return Value::obj(o);
}
JS_NATIVE(host_dump) {  // [{ mac, name, map: hex of the report descriptor, gatt: "S service\n  C characteristic RWwNI" }]
  (void)self; (void)args; (void)argc;
  Array* a = I.newArray();
  for (HostDev& d : hostDevs) {
    if (!d.used || d.gone) continue;
    Object* o = I.newObject();
    std::string hex;
    char b[3];
    for (unsigned char ch : d.rawMap) { snprintf(b, sizeof b, "%02x", ch); hex += b; }
    I.setOwn(o, "mac", I.str(d.mac));
    I.setOwn(o, "name", I.str(d.name));
    I.setOwn(o, "map", I.str(hex));
    I.setOwn(o, "gatt", I.str(d.gatt));
    I.setOwn(o, "subs", Value::num(d.nsubs));
    I.setOwn(o, "subOk", Value::num(d.subOk));
    a->items.push_back(Value::obj(o));
  }
  return Value::obj(a);
}
static HostDev* hostPick(Interp& I, Value v) {  // a connected device by address (or the first one)
  std::string m = v.isStr() ? v.s()->s : "";
  for (HostDev& d : hostDevs) if (d.used && !d.gone && (m.empty() || d.mac == m)) return &d;
  I.throwError("Error", "ble.host: no such device connected");
  return nullptr;
}
static std::vector<uint8_t> hostBytes(Interp& I, Value v) {
  std::vector<uint8_t> b;
  for (Value& x : I.toList(v)) b.push_back((uint8_t)I.toInt32(x));
  return b;
}
static std::string hexOf(const uint8_t* d, size_t n) { std::string h; char b[3]; for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02x", d[i]); h += b; } return h; }
JS_NATIVE(host_raw) {  // raw(true|false) turns the log on / off; raw() returns [{ id, hex }] since the last call
  (void)self;
  if (argc > 0 && !args[0].isUndef()) { rawOn = I.truthy(args[0]); rawHead = rawTail = 0; return Value(); }
  Array* a = I.newArray();
  while (rawTail != rawHead) {
    RawRep r = rawQ[rawTail];
    rawTail = (rawTail + 1) % RAW_Q;
    Object* o = I.newObject();
    I.setOwn(o, "id", Value::num(r.id));
    I.setOwn(o, "hex", I.str(hexOf(r.d, r.len)));
    a->items.push_back(Value::obj(o));
  }
  return Value::obj(a);
}
JS_NATIVE(host_feature) {  // feature(mac | "", id, [bytes]) writes a HID feature report; feature(mac, id) reads it -> hex
  (void)self;
  HostDev* d = hostPick(I, JS_ARG(0));
  int id = IA(1);
  for (int i = 0; i < d->nfeats; i++) {
    if (d->feats[i].id != id) continue;
    NimBLERemoteCharacteristic* ch = d->c->getCharacteristic(d->feats[i].handle);
    if (!ch) break;
    if (argc > 2) { std::vector<uint8_t> b = hostBytes(I, args[2]); return Value::boolean(ch->writeValue(b.data(), b.size(), true)); }
    NimBLEAttValue v = ch->readValue();
    return I.str(hexOf(v.data(), v.size()));
  }
  I.throwError("Error", "ble.host.feature: that device has no feature report with that id");
  return Value();
}
JS_NATIVE(host_valve) {  // valve(mac | "", [bytes]) writes the Steam Controller's private characteristic; valve(mac) reads the other -> hex
  (void)self;
  HostDev* d = hostPick(I, JS_ARG(0));
  if (!d->valveOut) I.throwError("Error", "ble.host.valve: not a Steam Controller");
  if (argc > 1) {
    NimBLERemoteCharacteristic* ch = d->c->getCharacteristic(d->valveOut);
    std::vector<uint8_t> b = hostBytes(I, args[1]);
    return Value::boolean(ch && ch->writeValue(b.data(), b.size(), true));
  }
  NimBLERemoteCharacteristic* ch = d->c->getCharacteristic(d->valveOut);
  if (!ch || !ch->canRead()) return I.str("");
  NimBLEAttValue v = ch->readValue();
  return I.str(hexOf(v.data(), v.size()));
}
JS_NATIVE(host_decode) { (void)self; hostDecodeMode = argc == 0 ? 1 : (args[0].isBool() ? (I.truthy(args[0]) ? 1 : 0) : IA(0)); return Value(); }
JS_NATIVE(host_report) {  // report(mac | "", id) reads an input report's characteristic now -> hex (when notifications don't come)
  (void)self;
  HostDev* d = hostPick(I, JS_ARG(0));
  int id = IA(1);
  for (int i = 0; i < d->nsubs; i++) {
    if (d->subs[i].id != id) continue;
    NimBLERemoteCharacteristic* ch = d->c->getCharacteristic(d->subs[i].handle);
    if (!ch || !ch->canRead()) break;
    NimBLEAttValue v = ch->readValue();
    return I.str(hexOf(v.data(), v.size()));
  }
  I.throwError("Error", "ble.host.report: no readable input report with that id");
  return Value();
}
JS_NATIVE(host_devices) { (void)self; (void)args; (void)argc; return hostDeviceList(I); }
JS_NATIVE(host_bonded) {
  (void)self; (void)args; (void)argc;
  Array* a = I.newArray();
  for (auto& e : hostSavedLoad()) a->items.push_back(I.str(e.mac));
  return Value::obj(a);
}
JS_NATIVE(host_forget) {
  (void)self;
  std::string mac = I.toStr(JS_ARG(0));
  if (!bleInit("")) bleNeedMemory(I);
  auto v = hostSavedLoad();
  if (mac == "*") { v.clear(); NimBLEDevice::deleteAllBonds(); hostSavedStore(v); return Value::boolean(true); }
  v.erase(std::remove_if(v.begin(), v.end(), [&](const HostSaved& e) { return e.mac == mac; }), v.end());
  hostSavedStore(v);
  return Value::boolean(NimBLEDevice::deleteBond(NimBLEAddress(mac)));
}
JS_NATIVE(host_cursor) {
  (void)self;
  ptrCursorOn = argc == 0 || I.truthy(args[0]);
  if (!ptrCursorOn) cursorHide();
  return Value();
}
JS_NATIVE(host_stop) { (void)I; (void)self; (void)args; (void)argc; hostStop(); return Value(); }
