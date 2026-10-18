// port.h - the few chip services the firmware needs, so the same code builds for the ESP32 family
// and the Raspberry Pi RP2350 (arduino-pico core).
//
// On ESP32 this is just ESP-IDF's own headers. On the RP2350 it supplies stand-ins with the same
// names (heap_caps_*, ESP.*, vTaskDelay) for the parts that work the same way there, so hw.cpp
// and main.cpp keep one code path; the parts that differ (tasks, sound, firmware updates) are
// under #if MICROJS_RP2 where they're used.
#pragma once
#include <Arduino.h>

#if defined(ARDUINO_ARCH_RP2040)
#define MICROJS_RP2 1
#include <malloc.h>
#include <pico/unique_id.h>

#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA 0
inline void* heap_caps_malloc(size_t n, uint32_t) { return malloc(n); }
// The largest block malloc can hand out: newlib can't say, so find it by trying (about 20 tries).
inline size_t heap_caps_get_largest_free_block(uint32_t) {
  size_t lo = 0, hi = rp2040.getFreeHeap();
  while (hi - lo > 256) {
    size_t mid = lo + (hi - lo) / 2;
    void* p = malloc(mid);
    if (p) { free(p); lo = mid; } else hi = mid;
  }
  return lo;
}
inline bool psramFound() { return false; }
inline uint32_t getCpuFrequencyMhz() { return rp2040.f_cpu() / 1000000; }

// Tasks: no RTOS here (core 1 is busy making the picture), so port_rp2.cpp runs the firmware's few
// background jobs (music, video) as cooperative tasks on core 0 with the same calls as FreeRTOS.
// A task runs until it waits (vTaskDelay); the JavaScript interpreter waits often too. The core
// number is ignored, and only a task deleting itself (vTaskDelete(nullptr)) is supported.
typedef void* TaskHandle_t;
typedef int BaseType_t;
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, uint32_t stackBytes, void* arg,
                                   int priority, TaskHandle_t* handle, int core);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(uint32_t ms);  // let the other tasks run for (at least) ms milliseconds
void hwYield();                // hw.cpp: work done whenever a task waits (USB host, sound)
// Every task runs on the same core and switches only in vTaskDelay: a critical section needs nothing.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))

struct PortChip {
  uint32_t getFreeHeap() { return rp2040.getFreeHeap(); }
  uint32_t minFree = 0xFFFFFFFF;
  uint32_t getMinFreeHeap() {
    uint32_t f = getFreeHeap();
    if (f < minFree) minFree = f;
    return minFree;
  }
  uint32_t getPsramSize() { return 0; }
  void restart() { rp2040.reboot(); }
  uint64_t getEfuseMac() {  // the flash chip's unique ID stands in for a MAC address
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | id.id[i];
    return v;
  }
};
inline PortChip ESP;

// The REPL's USB serial port (port_rp2.cpp). The USB stack here (Adafruit TinyUSB, which the PIO-USB
// host needs) runs its device work in an interrupt and its Serial calls take no lock, so that
// interrupt could land inside a Serial.write, wait for a lock the write held, and freeze the board
// for good (the watchdog reset it - most often during file transfers). This one takes the USB
// stack's lock around each call and never waits while holding it.
class ReplSerial : public Stream {
 public:
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  int availableForWrite() override;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t n) override;
  using Print::write;
};
extern ReplSerial replSerial;
#else
#define MICROJS_RP2 0
#include <esp_heap_caps.h>
#define replSerial Serial
#endif
