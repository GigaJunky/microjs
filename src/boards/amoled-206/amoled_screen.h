// amoled_screen.h - the screen for the Waveshare ESP32-S3-Touch-AMOLED-2.06 watch: a 410x502 AMOLED
// panel with a CO5300 controller on a QSPI bus (four data lines, 40 MHz).
//
// The picture is a full-screen 16-bit TFT_eSPI sprite in PSRAM, so every TFT_eSPI drawing function,
// font and text datum works unchanged. A task on core 0 copies it to the panel continuously (~25
// times a second): a band of rows at a time into DMA buffers in internal RAM, sent while the next
// band is copied. TFT_eSPI keeps 16-bit pixels big-endian, which is what the panel wants.
//
// The CO5300 speaks the SH8601 QSPI protocol: a command is opcode 0x02 + a 24-bit address holding
// the command (all on one line), then its parameters; pixels are opcode 0x32 with address 0x2C00
// (RAMWR, then 0x3C00 to continue), the pixel data on all four lines. The panel's 410 columns start
// at controller column 22 (from Waveshare's board support package).
//
// hw.cpp uses this class as its `tft` when built with
//   -DBOARD_SCREEN_HEADER='"boards/amoled-206/amoled_screen.h"' -DBOARD_SCREEN_CLASS=AmoledScreen
#pragma once
#include <TFT_eSPI.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

#ifndef AMOLED_CS
#define AMOLED_CS 12
#define AMOLED_SCLK 11
#define AMOLED_D0 4
#define AMOLED_D1 5
#define AMOLED_D2 6
#define AMOLED_D3 7
#define AMOLED_RST 8
#endif
#ifndef AMOLED_COL_OFFSET
#define AMOLED_COL_OFFSET 22
#endif

class AmoledScreen : public TFT_eSprite {
 public:
  static const int W = 410, H = 502;
  static const int BAND = 16;  // rows per DMA transfer (13 KB)

  AmoledScreen() : TFT_eSprite(this) {}

  void init() {
    setColorDepth(16);
    setAttribute(PSRAM_ENABLE, true);
    if (!createSprite(W, H)) { Serial.println("AMOLED: no memory for the screen"); return; }
    // TFT_eSPI's screen-wide calls (fillScreen, text wrapping) use the base class's size, which
    // still holds the dummy panel's; make it the sprite's.
    _width = _init_width = W;
    _height = _init_height = H;
    fillSprite(TFT_BLACK);
    if (!busBegin()) { Serial.println("AMOLED: couldn't start the QSPI bus"); return; }
    panelBegin();
    ok = xTaskCreatePinnedToCore(pushTask, "amoled", 4096, this, 2, nullptr, 0) == pdPASS;
  }

  void service() {}


  // The panel has one fixed orientation, and nothing to invert.
  void setRotation(uint8_t) {}
  void invertDisplay(bool) {}

  // 0..255 (an AMOLED has no backlight: this is the panel's own brightness)
  void setBrightness(uint8_t level) {
    brightness = level;
    pendingBrightness = true;  // sent by the push task, between frames
  }

 private:
  spi_device_handle_t dev = nullptr;
  uint8_t* dma[2] = {nullptr, nullptr};
  bool ok = false;
  volatile uint8_t brightness = 200;
  volatile bool pendingBrightness = false;

  bool busBegin() {
    spi_bus_config_t bus = {};
    bus.data0_io_num = AMOLED_D0;
    bus.data1_io_num = AMOLED_D1;
    bus.data2_io_num = AMOLED_D2;
    bus.data3_io_num = AMOLED_D3;
    bus.sclk_io_num = AMOLED_SCLK;
    bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
    bus.max_transfer_sz = W * BAND * 2 + 16;
    bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    spi_device_interface_config_t d = {};
    d.command_bits = 8;
    d.address_bits = 24;
    d.mode = 0;
    d.clock_speed_hz = 40 * 1000 * 1000;  // the GPIO matrix (these aren't the SPI pins) tops out here
    d.spics_io_num = AMOLED_CS;
    d.flags = SPI_DEVICE_HALFDUPLEX;
    d.queue_size = 2;
    if (spi_bus_add_device(SPI2_HOST, &d, &dev) != ESP_OK) return false;
    for (auto& b : dma) {
      b = (uint8_t*)heap_caps_malloc(W * BAND * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      if (!b) return false;
    }
    return true;
  }

  // a command and its parameters, all on one data line
  void cmd(uint8_t c, const uint8_t* data = nullptr, size_t len = 0) {
    spi_transaction_t t = {};
    t.cmd = 0x02;
    t.addr = (uint32_t)c << 8;
    t.length = len * 8;
    t.tx_buffer = len ? data : nullptr;
    spi_device_polling_transmit(dev, &t);
  }
  void cmd1(uint8_t c, uint8_t v) { cmd(c, &v, 1); }

  void panelBegin() {
    pinMode(AMOLED_RST, OUTPUT);
    digitalWrite(AMOLED_RST, HIGH);
    delay(5);
    digitalWrite(AMOLED_RST, LOW);
    delay(20);
    digitalWrite(AMOLED_RST, HIGH);
    delay(120);
    // Waveshare's init sequence (board support package), plus 16-bit pixels
    cmd1(0xFE, 0x00);  // command page 0 (as Arduino_GFX's CO5300 driver does)
    cmd(0x11);  // sleep out
    delay(120);
    cmd1(0xC4, 0x80);
    const uint8_t te[] = {0x01, 0xD1};
    cmd(0x44, te, 2);    // tearing-effect scanline
    cmd1(0x35, 0x00);    // tearing-effect signal on
    cmd1(0x3A, 0x55);    // 16 bits a pixel (RGB565)
    cmd1(0x53, 0x20);    // brightness control on
    cmd1(0x63, 0xFF);    // HBM brightness
    cmd1(0x51, 0x00);    // dark until the first frame is in
    const uint16_t x0 = AMOLED_COL_OFFSET, x1 = AMOLED_COL_OFFSET + W - 1;
    const uint8_t caset[] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1};
    const uint8_t raset[] = {0, 0, (uint8_t)((H - 1) >> 8), (uint8_t)(H - 1)};
    cmd(0x2A, caset, 4);
    cmd(0x2B, raset, 4);
    cmd(0x29);  // display on
    delay(10);
    pendingBrightness = true;
  }

  static void pushTask(void* arg) {
    AmoledScreen* s = (AmoledScreen*)arg;
    for (;;) {
      s->pushFrame();
      if (s->pendingBrightness) {
        s->pendingBrightness = false;
        s->cmd1(0x51, s->brightness);
      }
      vTaskDelay(1);
    }
  }

  // The whole sprite to the panel: band by band, copying the next while the last one is sent.
  void pushFrame() {
    const uint8_t* src = (const uint8_t*)_img;
    spi_transaction_t t[2] = {};
    int inFlight = 0;
    for (int y = 0, n = 0; y < H; y += BAND, n++) {
      int rows = std::min(BAND, H - y);
      size_t bytes = (size_t)W * rows * 2;
      uint8_t* buf = dma[n & 1];
      if (inFlight == 2) {  // wait for the transfer that used this buffer
        spi_transaction_t* done;
        spi_device_get_trans_result(dev, &done, portMAX_DELAY);
        inFlight--;
      }
      memcpy(buf, src + (size_t)y * W * 2, bytes);
      spi_transaction_t& tr = t[n & 1];
      tr = {};
      tr.flags = SPI_TRANS_MODE_QIO;   // command and address on one line, pixels on four
      tr.cmd = 0x32;
      tr.addr = y == 0 ? 0x2C00 : 0x3C00;  // RAMWR, then "continue"
      tr.length = bytes * 8;
      tr.tx_buffer = buf;
      spi_device_queue_trans(dev, &tr, portMAX_DELAY);
      inFlight++;
    }
    while (inFlight--) {
      spi_transaction_t* done;
      spi_device_get_trans_result(dev, &done, portMAX_DELAY);
    }
  }
};
