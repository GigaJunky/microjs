micro-js firmware images for web flashers
=======================================

Each file name carries the micro-js version (1.1.73). Each .bin is a merged image: write it at
offset 0x0 (address 0). A -full image has the firmware and the apps and replaces everything on the
board (saved scripts and high scores too); an -app image is the firmware only and keeps the apps
already on the board (use it to update).

  micro-js-1.1.73-cyd-full.bin
  micro-js-1.1.73-cyd-app.bin
      standard CYD (ESP32-2432S028R, one USB port)
  micro-js-1.1.73-cyd2usb-full.bin
  micro-js-1.1.73-cyd2usb-app.bin
      two-USB-port CYD variant (ST7789 panel)
  micro-js-1.1.73-cyd-s3-full.bin
  micro-js-1.1.73-cyd-s3-app.bin
      ESP32-S3 CYD 2.8" (ES3C28P: 8 MB PSRAM, capacitive touch, ES8311 audio, battery)
  micro-js-1.1.73-cardputer-adv-full.bin
  micro-js-1.1.73-cardputer-adv-app.bin
      M5Stack Cardputer ADV (ESP32-S3, 240x135, keyboard)
  micro-js-1.1.73-coco-vga-full.bin
  micro-js-1.1.73-coco-vga-app.bin
      mast4rbug's ESP32-CoCo board (ESP32-S3, VGA output, USB keyboard and joystick ports).
      The full image replaces the CoCo emulator - back up the board's flash first if you want it.
  micro-js-1.1.73-rp2350-pizero-full.uf2
  micro-js-1.1.73-rp2350-pizero-app.uf2
      Waveshare RP2350-PiZero (HDMI out, USB keyboard and joystick through the PIO-USB port).
      Not a .bin: hold the board's BOOT button while plugging it into a computer, and drag the
      .uf2 onto the drive that appears (RP2350). -full has the firmware and the apps, -app only
      the firmware. It has no WiFi: run tools/bridge.js (or the web IDE) on the computer for the
      clock and the network.
  MICROJS_1-1-73.FLH
      ESP32-CoCo board, firmware only, for the CoCo emulator's own Flash menu: copy it to the SD
      card, open the emulator's menu, press F and pick it. Back the other way: run flash.js in
      micro-js and pick the emulator's .FLH file.

Not included: the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch (written but never run on a working screen).

Flashing the .bin images from the browser (Chrome or Edge on a desktop computer):

  1. Open https://espressif.github.io/esptool-js/
  2. Baudrate 115200 or 460800, click "Connect", pick the board's serial port
     (often "USB Serial" / CH340).
  3. Flash Address: 0x0, choose the .bin file, click "Program".
     For a clean install, click "Erase Flash" first and use a -full image.
  4. When it finishes, press the board's RST button.

Then connect at 115200 baud with any serial terminal (or https://espressif.github.io/esptool-js/
-> "Console") and type .help.

If the upload fails, hold the BOOT button while clicking Connect, or try a lower baud rate.
Built from this project with tools/build_release.sh (one pair of images per board profile,
plus MICROJS_<version>.FLH for the ESP32-CoCo board; the scripts image always contains a blank
micro-js.json).
