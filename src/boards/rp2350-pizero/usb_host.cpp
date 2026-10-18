// usb_host.cpp - USB keyboards and joysticks on the RP2350-PiZero's PIO-USB port (the USB-C port
// marked PIO-USB; the other one is power and the REPL).
//
// Pico-PIO-USB makes a full-speed (12 Mbit/s) USB host out of PIO 1 and GPIO 28/29, and TinyUSB runs
// the USB protocol on top, hubs included: a keyboard and a gamepad together need a simple USB hub.
// Each HID report goes to src/usb_hid.h, which turns it into key and joystick events. Keyboards are
// asked for the simple "boot" report format, which every USB keyboard supports.
// The board can't switch the port's power, so a device plugged straight in after power-on may not
// be seen until the board is restarted; through a hub, plugging in later works.
#include <Arduino.h>
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"
#include "../../usb_hid.h"

#ifndef USB_HOST_DP
#define USB_HOST_DP 28  // D+ (D- is the next pin)
#endif

static Adafruit_USBH_Host usbHost;
static bool started = false;

// one slot per HID interface (a keyboard often has two or three, a gamepad one or two)
static const int SLOTS = 8;
struct Slot {
  uint8_t addr, inst;  // addr 0 = free
  bool keyboard;
  uint8_t prev[8];
};
static Slot slots[SLOTS];
static usbhid::JoyLayout layout[SLOTS];
static bool capsLock = false;

static int slotOf(uint8_t addr, uint8_t inst) {
  for (int i = 0; i < SLOTS; i++)
    if (slots[i].addr == addr && slots[i].inst == inst) return i;
  return -1;
}

extern "C" void tuh_hid_mount_cb(uint8_t addr, uint8_t inst, const uint8_t* desc, uint16_t descLen) {
  int i = slotOf(0, 0);  // a free slot
  if (i < 0) return;
  uint8_t proto = tuh_hid_interface_protocol(addr, inst);
  if (proto == HID_ITF_PROTOCOL_MOUSE) return;  // no use for a mouse yet
  if (proto != HID_ITF_PROTOCOL_KEYBOARD && !usbhid::isJoystickDescriptor(desc, descLen)) {
    return;  // e.g. a keyboard's media keys
  }
  Slot& s = slots[i];
  s.addr = addr;
  s.inst = inst;
  s.keyboard = proto == HID_ITF_PROTOCOL_KEYBOARD;
  memset(s.prev, 0, sizeof s.prev);
  if (!s.keyboard) usbhid::joystickAttach(layout, SLOTS, i);
  tuh_hid_receive_report(addr, inst);
}

extern "C" void tuh_hid_umount_cb(uint8_t addr, uint8_t inst) {
  int i = slotOf(addr, inst);
  if (i < 0) return;
  if (!slots[i].keyboard) usbhid::joystickDetach(layout[i]);
  slots[i] = Slot{};
  layout[i].index = -1;
}

extern "C" void tuh_hid_report_received_cb(uint8_t addr, uint8_t inst, const uint8_t* report, uint16_t len) {
  int i = slotOf(addr, inst);
  if (i >= 0) {
    Slot& s = slots[i];
    if (s.keyboard) {
      if (len == 8) usbhid::keyboardReport(s.prev, report, capsLock);
    } else if (len) {
      usbhid::joystickReport(layout[i], report, (uint8_t)std::min<uint16_t>(len, 255));
    }
  }
  tuh_hid_receive_report(addr, inst);  // and the next one
}

// Called by hw.cpp's keyboardBegin() (BOARD_KEYBOARD == KEYBOARD_EXTERNAL).
void hwExternalKeyboardBegin() {
  for (auto& L : layout) L.index = -1;
  pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  cfg.pin_dp = USB_HOST_DP;
  // PIO 0 makes the picture; transmit on PIO 2 and receive on PIO 1 (the default split, moved up
  // one), so neither block runs out of instruction memory.
  cfg.pio_tx_num = 2;
  cfg.pio_rx_num = 1;
  // The default is DMA channel 0, which the video took first (libdvi claims the lowest free ones);
  // claiming a taken channel panics and hangs the board at boot. Use a free one (PIO-USB claims it).
  int ch = dma_claim_unused_channel(true);
  dma_channel_unclaim(ch);
  cfg.tx_ch = ch;
  tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);
  usbHost.configure_pio_usb(1, &cfg);
  started = usbHost.begin(1);
}

// Called often (hw.cpp's hwYield / hwService): runs the USB host's pending work.
void hwExternalKeyboardPoll() {
  if (started) usbHost.task(0);
}
