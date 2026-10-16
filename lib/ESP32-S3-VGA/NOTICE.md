# ESP32-S3-VGA

VGA output for the ESP32-S3 through its LCD_CAM peripheral, used by the `coco-vga` profile
(`src/boards/coco-vga/vga_screen.h`).

- **Original:** [bitluni/ESP32-S3-VGA](https://github.com/bitluni/ESP32-S3-VGA) by bitluni. That
  repository does not state a licence.
- **This copy** comes from mast4rbug's
  [ESP32_CoCo2_Emulator](https://github.com/mast4rbug/ESP32_CoCo2_Emulator)
  (`lib/ESP32-S3-VGA-main`, published under CC BY-NC 4.0), which extended it for the ESP32-CoCo
  board (pixel clock on GPIO 48, `Reinit`, and more).
- **Changes in micro-js:** the horizontal blanking (`lcd_hb_front`) is set to sync + back porch,
  so the picture starts after the back porch and the front porch follows it; before, the picture
  was shifted right with a black bar on the left.

It is included so the `coco-vga` profile builds as-is. All credit and rights belong to its
authors; if you are one of them and would like it handled differently, please open an issue.
