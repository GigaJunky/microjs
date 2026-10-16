# SoftUSB (ESP32-USB-Soft-Host)

A software USB 1.1 low-speed host for the ESP32 family, used by the `coco-vga` profile for
keyboards and gamepads on the ESP32-CoCo board's USB ports.

- **Original:** [esp32_usb_soft_host](https://github.com/sdima1357/esp32_usb_soft_host) by
  Dmitry Samsonov (@sdima1357), wrapped for Arduino as
  [tobozo/ESP32-USB-Soft-Host](https://github.com/tobozo/ESP32-USB-Soft-Host) (credits also to
  Grigory Prigodin @zbx-sadman and @ivo1981).
- **Licence:** AGPL-3.0 (see `LICENSE`, from tobozo's repository).
- **This copy** comes from mast4rbug's
  [ESP32_CoCo2_Emulator](https://github.com/mast4rbug/ESP32_CoCo2_Emulator) (`lib/SoftUSB`), which
  adapted it to run the emulated 6809 CPU from the USB timer.
- **Changes in micro-js:** the emulator's CPU hook (`DoCPU`) removed, so the timer only services
  USB (1 ms); no blink pin when the pin is GPIO 0; the timer interrupt allocated without the IRAM
  flag (most of the host code runs from flash). Debug printing is behind `DEBUG_PRINT`.
