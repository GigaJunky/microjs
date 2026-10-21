micro-js firmware images for web flashers
=======================================

Each file name carries the micro-js version (1.1.76). Each .bin is a merged image: write it at
offset 0x0 (address 0). A -firmware image is the firmware only: it keeps the apps and settings
already on the board, so use it to install or update. The apps come separately, one bundle for
every board:

  micro-js-1.1.76-apps.zip
      every app and the files they load, with a blank micro-js.json. To install or update the apps:
      open the web IDE (node tools/serve.js, or tools/web-ide.html), connect, and drop the zip on
      the file list (or Upload...): it unpacks into the current folder, sub-folders included. Or
      unzip it and copy the files to the board's flash or the SD card. From a command line (npm
      install once, then): node tools/upload.js /dev/ttyACM0 micro-js-1.1.76-apps.zip  (add
      --to /sd for the SD card). Either way a micro-js.json already on the board is kept.

  micro-js-1.1.76-cyd-firmware.bin
      standard CYD (ESP32-2432S028R, one USB port)
  micro-js-1.1.76-cyd2usb-firmware.bin
      two-USB-port CYD variant (ST7789 panel)
  micro-js-1.1.76-cyd-s3-firmware.bin
      ESP32-S3 CYD 2.8" (ES3C28P: 8 MB PSRAM, capacitive touch, ES8311 audio, battery)
  micro-js-1.1.76-cardputer-adv-firmware.bin
      M5Stack Cardputer ADV (ESP32-S3, 240x135, keyboard)
  micro-js-1.1.76-coco-vga-firmware.bin
      mast4rbug's ESP32-CoCo board (ESP32-S3, VGA output, USB keyboard and joystick ports).
      The full image replaces the CoCo emulator - back up the board's flash first if you want it.
  micro-js-1.1.76-rp2350-pizero-firmware.uf2
      Waveshare RP2350-PiZero (HDMI out, USB keyboard and joystick through the PIO-USB port).
      Not a .bin: hold the board's BOOT button while plugging it into a computer, and drag the
      .uf2 onto the drive that appears (RP2350). The apps are in the apps zip (see above). It has no WiFi: run tools/bridge.js (or the web IDE) on the computer for the
      clock and the network.
  MICROJS_1-1-73.FLH
      ESP32-CoCo board, firmware only, for the CoCo emulator's own Flash menu: copy it to the SD
      card, open the emulator's menu, press F and pick it. Back the other way: in the file manager (files.js) pick
      micro-js and pick the emulator's .FLH file.

Not included: the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch (written but never run on a working screen).

Flashing the .bin images from the browser (Chrome or Edge on a desktop computer):

  1. Open https://espressif.github.io/esptool-js/
  2. Baudrate 115200 or 460800, click "Connect", pick the board's serial port
     (often "USB Serial" / CH340).
  3. Flash Address: 0x0, choose the .bin file, click "Program".
     For a clean install, click "Erase Flash" first, then flash the firmware and install the apps zip.
  4. When it finishes, press the board's RST button.

Then connect at 115200 baud with any serial terminal (or https://espressif.github.io/esptool-js/
-> "Console") and type .help.

If the upload fails, hold the BOOT button while clicking Connect, or try a lower baud rate.
Built from this project with tools/build_release.sh (one firmware image per board profile, one apps zip,
plus MICROJS_<version>.FLH for the ESP32-CoCo board; the scripts image always contains a blank
micro-js.json).
