// vga_screen.h - the screen for VGA boards: a full-screen 8-bit TFT_eSPI sprite that the ESP32-S3's
// LCD_CAM peripheral scans out as VGA, using bitluni's ESP32-S3-VGA library for the video timing.
//
// TFT_eSPI's 8-bit sprites store RGB332 (RRRGGGBB). The board's 8-bit video bus carries the top
// two bits of each colour: bit 0-1 blue (GPIO 8/9), bit 3-4 green (6/7), bit 6-7 red (4/5), with
// bits 2 and 5 unconnected - exactly RGB332 minus the low bit of red and green. So the sprite's pixel
// buffer can be the framebuffer itself: every TFT_eSPI drawing function, font and text datum works
// unchanged, and scripts render the same as on the CYD (in 64 colours).
//
// hw.cpp uses this class as its `tft` when built with
//   -DBOARD_SCREEN_HEADER='"vga_screen.h"' -DBOARD_SCREEN_CLASS=VgaScreen
#pragma once
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>
#include "VGA.h"

// 8-bit video bus: r0..r4, g0..g5, b0..b4, hsync, vsync. In 8-bit mode the library uses r2-r4,
// g3-g5 and b3-b4; -1 leaves a line unconnected (so GPIO 0 stays free for the BOOT button).
#ifndef VGA_PINS
#define VGA_PINS -1, -1, 8, 9, -1,  -1, -1, -1, 6, 7, -1,  -1, -1, -1, 4, 5,  2, 1
#endif

class VgaScreen : public TFT_eSprite {
 public:
  VgaScreen() : TFT_eSprite(this) {}

  // Standard 640x480@60 timing (25.175 MHz dot clock, 800 x 525 total) at half the horizontal
  // resolution, so every pixel is two VGA pixels wide; each line is sent twice (vClones = 2).
  // Mode(hFront, hSync, hBack, hRes, vFront, vSync, vBack, vRes, pixelClock, hPol, vPol, vClones)
  static Mode mode() { return Mode(8, 48, 24, 320, 10, 2, 33, 240, 12587500, 0, 0, 2); }

  void init() {
    Mode m = mode();
    W = m.hRes;
    H = m.vRes;
    setColorDepth(8);
    setAttribute(PSRAM_ENABLE, true);  // the temporary buffer createSprite makes; replaced below
    if (!createSprite(W, H)) { Serial.println("VGA: no memory for the screen sprite"); return; }
    // TFT_eSPI's screen-wide calls (fillScreen, text wrapping) use the base class's size, which
    // still holds the dummy panel's; make it the sprite's, or the right of the screen never clears.
    _width = _init_width = W;
    _height = _init_height = H;

    // The framebuffer: internal RAM if possible (the DMA reads it directly, no cache to flush),
    // otherwise PSRAM (aligned for DMA, written back from the cache in service()).
    size_t size = (size_t)W * H + 1;
    uint8_t* fb = (uint8_t*)heap_caps_aligned_calloc(64, 1, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    inPsram = !fb;
    if (!fb) fb = (uint8_t*)heap_caps_aligned_calloc(64, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) { Serial.println("VGA: no memory for the framebuffer"); return; }
    free(_img8_1);
    _img8 = _img8_1 = _img8_2 = fb;
    _img = (uint16_t*)fb;  // sprite-to-sprite copies (pushToSprite) write through _img, even at 8 bits

    vga.bufferCount = 1;
    const PinConfig pins(VGA_PINS);
    if (!vga.init(pins, m, 8)) { Serial.println("VGA: init failed"); return; }
    // Point every scan line at the sprite's rows (the library allocated its own line buffers; they
    // stay unused). Each line is listed vClones times.
    dma_descriptor_t* d = vga.dmaBuffer->getDescriptor();
    int lines = vga.dmaBuffer->lines, clones = vga.dmaBuffer->clones;
    for (int y = 0; y < lines; y++)
      for (int c = 0; c < clones; c++) d[y * clones + c].buffer = fb + (size_t)y * W;
    vga.start();
    ok = true;
  }

  // Called often from the main loop: with the framebuffer in PSRAM, push the CPU's cached writes
  // out to memory so the video DMA sees them (~30 times a second).
  void service() {
    if (!ok || !inPsram) return;
    uint32_t now = millis();
    if (now - lastFlush < 33) return;
    lastFlush = now;
    Cache_WriteBack_Addr((uint32_t)_img8, (uint32_t)W * H);
  }

  // A VGA monitor has a fixed landscape picture and nothing to invert.
  void setRotation(uint8_t) {}
  void invertDisplay(bool) {}

 private:
  VGA vga;
  int W = 320, H = 240;
  bool ok = false, inPsram = false;
  uint32_t lastFlush = 0;
};
