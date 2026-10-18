// dvi_screen.cpp - core 1's job on the RP2350-PiZero: DVI video out of the mini HDMI port with
// libdvi (see dvi_screen.h). Core 1 does nothing else, so the signal never misses its timing.
#include <Arduino.h>
#include <hardware/vreg.h>
#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <pico/sync.h>
#include <pico/multicore.h>
#include <hardware/structs/busctrl.h>
extern "C" {
#include "dvi.h"
#include "dvi_serialiser.h"
#include "tmds_encode.h"
}

// The board wires the HDMI connector straight to GPIO 32-39 (through resistors): data lanes 0-2 on
// 36/37, 34/35 and 32/33, the clock on 38/39.
static dvi_serialiser_cfg pinsCfg() {
  dvi_serialiser_cfg c{};
  c.pio = pio0;
  c.sm_tmds[0] = 0; c.sm_tmds[1] = 1; c.sm_tmds[2] = 2;
  c.pins_tmds[0] = 36; c.pins_tmds[1] = 34; c.pins_tmds[2] = 32;
  c.pins_clk = 38;
  c.invert_diffpairs = false;
  return c;
}

static dvi_inst dvi0;
static const uint8_t* fb = nullptr;
static int fbW = 0, fbH = 0, fbDepth = 0;
static uint32_t line[320 / 2];  // one 16-bit line in the encoder's byte order

static void encodeLoop();  // core 1's loop, below
bool dviStart(void* framebuffer, int depth, int width, int height) {
  if (!framebuffer || width > 320) return false;
  fb = (const uint8_t*)framebuffer;
  fbW = width;
  fbH = height;
  fbDepth = depth;
  // 640x480 at 60 Hz needs a 252 MHz system clock (the TMDS bit rate); the build sets that, and a
  // little more core voltage keeps it reliable
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  delay(10);
  if (clock_get_hz(clk_sys) != dvi_timing_640x480p_60hz.bit_clk_khz * 1000u)
    set_sys_clock_khz(dvi_timing_640x480p_60hz.bit_clk_khz, true);
  dvi0.timing = &dvi_timing_640x480p_60hz;
  dvi0.ser_cfg = pinsCfg();
  dvi0.scanline_callback = nullptr;
  // the video DMA goes first on the bus, so core 0 drawing into the framebuffer can't make a
  // scanline late (as in the PicoDVI examples)
  busctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
  pio_set_gpio_base(pio0, 16);  // PIO 0 reaches GPIO 16-47
  dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
  // Core 1 is started here rather than through arduino-pico's setup1(): with setup1() the core
  // pauses core 1 around every flash write (LittleFS), and a pause around a short page write hung
  // the board every time. Core 1 never needs pausing - everything it runs from here on (the encode
  // loop, libdvi's DMA interrupt, queues and TMDS encoders) is in RAM, so flash writes can't upset it.
  // (This lambda itself is in flash: it only starts things, then stays in encodeLoop.)
  static void (*const core1)() = [] {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    encodeLoop();
  };
  multicore_launch_core1(core1);
  return true;
}

// Encode the framebuffer one line at a time, forever, just ahead of the picture being sent. TFT_eSPI
// keeps 16-bit pixels byte-swapped (ready for an SPI panel), so those are swapped back first.
// (__no_inline_: inlined into the launch lambda above, it ran from flash - and core 1 stalled
// whenever core 0 erased flash for a file write, which froze the board until the watchdog reset it)
static void __no_inline_not_in_flash_func(encodeLoop)() {
  const uint words = dvi0.timing->h_active_pixels / DVI_SYMBOLS_PER_WORD;
  for (;;) {
    for (int y = 0; y < fbH; y++) {
      uint32_t* tmds;
      queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmds);
      if (fbDepth == 16) {
        const uint32_t* src = (const uint32_t*)(fb + (size_t)y * fbW * 2);
        for (int i = 0; i < fbW / 2; i++) {
          uint32_t v = src[i];
          line[i] = (v & 0x00FF00FF) << 8 | (v >> 8 & 0x00FF00FF);
        }
        tmds_encode_data_channel_16bpp(line, tmds + 0 * words, fbW, DVI_16BPP_BLUE_MSB, DVI_16BPP_BLUE_LSB);
        tmds_encode_data_channel_16bpp(line, tmds + 1 * words, fbW, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_16bpp(line, tmds + 2 * words, fbW, DVI_16BPP_RED_MSB, DVI_16BPP_RED_LSB);
      } else {
        const uint32_t* src = (const uint32_t*)(fb + (size_t)y * fbW);
        tmds_encode_data_channel_8bpp(src, tmds + 0 * words, fbW, DVI_8BPP_BLUE_MSB, DVI_8BPP_BLUE_LSB);
        tmds_encode_data_channel_8bpp(src, tmds + 1 * words, fbW, DVI_8BPP_GREEN_MSB, DVI_8BPP_GREEN_LSB);
        tmds_encode_data_channel_8bpp(src, tmds + 2 * words, fbW, DVI_8BPP_RED_MSB, DVI_8BPP_RED_LSB);
      }
      queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmds);
    }
  }
}


