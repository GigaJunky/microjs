// usb_input.cpp - USB keyboards and joysticks on the board's software USB host ports
// (ESP32-USB-Soft-Host).
//
// The host runs on its own task and timer; it hands over each HID report, which src/usb_hid.h turns
// into key and joystick events. Only low-speed devices work on a software host, and hubs are not
// supported: plug the keyboard straight into one of the board's ports.
#include <Arduino.h>
#include "ESP32-USB-Soft-Host.h"
#include "../../usb_hid.h"

// D+/D- pins of the board's USB ports (-1 = unused)
#ifndef USB_PINS
#define USB_PINS 18, 17,  12, 11,  16, 15,  -1, -1
#endif

static const int PORTS = 4;
static bool isKeyboard[PORTS];       // interface protocol 1 = keyboard (2 = mouse)
static bool isJoystick[PORTS];       // HID with no boot protocol: a joystick / gamepad
static uint8_t prev[PORTS][8];
static bool capsLock = false;
static usbhid::JoyLayout layout[PORTS];

static void onIface(uint8_t ref, int, int, void* intf, size_t) {
  sIntfDesc* d = (sIntfDesc*)intf;
  if (ref >= PORTS || d->iClass != 0x03) return;
  if (d->iProto == 1) isKeyboard[ref] = true;  // any keyboard interface makes it a keyboard
  else if (d->iProto == 0 && !isKeyboard[ref] && !isJoystick[ref]) {
    isJoystick[ref] = true;
    usbhid::joystickAttach(layout, PORTS, ref);
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
  if (isJoystick[usbNum]) usbhid::joystickDetach(layout[usbNum]);
  isKeyboard[usbNum] = isJoystick[usbNum] = false;
  layout[usbNum].index = -1;
}

static void onReport(uint8_t usbNum, uint8_t, uint8_t* data, uint8_t len) {
  if (usbNum >= PORTS) return;
  if (isJoystick[usbNum] && !isKeyboard[usbNum]) return usbhid::joystickReport(layout[usbNum], data, len);
  if (len != 8 || !isKeyboard[usbNum]) return;
  usbhid::keyboardReport(prev[usbNum], data, capsLock);
}

// Called by hw.cpp's keyboardBegin() (BOARD_KEYBOARD == KEYBOARD_EXTERNAL).
void hwExternalKeyboardBegin() {
  static const usb_pins_config_t pins = {USB_PINS};
  for (auto& L : layout) L.index = -1;
  USH.setOnIfaceDescCb(onIface);
  USH.init(pins, onDetect, onReport, nullptr, onDisconnect);
}
