// dvi_screen.h - the screen for the RP2350-PiZero: a full-screen TFT_eSPI sprite that core 1 sends
// out of the board's mini HDMI port as DVI, 640x480 at 60 Hz (each of the 320x240 pixels doubled).
//
// The RP2350 has no video hardware on these pins, so Luke Wren's libdvi (lib/libdvi) makes the
// TMDS signal with PIO 0 (GPIO 32-39) and DMA, with core 1 encoding each line just in time. The
// sprite's pixel buffer is the framebuffer: every TFT_eSPI drawing function, font and text datum
// works unchanged, and scripts render the same as on the CYD. The framebuffer is 8-bit colour
// (RRRGGGBB, 256 colours, 75 KB): 16-bit (150 KB) left apps short of memory, and encoding it made
// core 1 miss lines (libdvi shows a missed line in red). -DDVI_DEPTH=16 brings 16-bit back.
//
// hw.cpp uses this class as its `tft` when built with
//   -DBOARD_SCREEN_HEADER='"boards/rp2350-pizero/dvi_screen.h"' -DBOARD_SCREEN_CLASS=DviScreen
#pragma once
#include <TFT_eSPI.h>

// core 1's side, in dvi_screen.cpp
bool dviStart(void* framebuffer, int depth, int width, int height);

class DviScreen : public TFT_eSprite {
 public:
  DviScreen() : TFT_eSprite(this) {}

  void init() {
    const int W = 320, H = 240;
#ifndef DVI_DEPTH
#define DVI_DEPTH 8
#endif
    int depth = DVI_DEPTH;
    setColorDepth(depth);
    if (!createSprite(W, H) && depth == 16) {  // not enough memory for 16 bits a pixel: 8
      depth = 8;
      setColorDepth(8);
      createSprite(W, H);
    }
    if (!created()) { Serial.println("DVI: no memory for the screen"); return; }
    // TFT_eSPI's screen-wide calls (fillScreen, text wrapping) use the base class's size, which
    // still holds the dummy panel's; make it the sprite's.
    _width = _init_width = W;
    _height = _init_height = H;
    fillSprite(TFT_BLACK);
    ok = dviStart(getPointer(), depth, W, H);
    if (!ok) Serial.println("DVI: couldn't start the video output");
  }

  void service() {}

  // A monitor has a fixed landscape picture and nothing to invert.
  void setRotation(uint8_t) {}
  void invertDisplay(bool) {}

 private:
  bool ok = false;
};
