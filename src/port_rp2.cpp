// port_rp2.cpp - cooperative tasks for the RP2350 build (see port.h): each task has its own stack,
// and vTaskDelay switches to the next task that is ready to run. Everything runs on core 0.
#include "port.h"
#if MICROJS_RP2
#include <vector>
#include <cstdint>

namespace {
struct Task {
  uint32_t* sp = nullptr;       // saved stack pointer while switched out
  uint8_t* stack = nullptr;     // malloc'd stack (nullptr for the main task, which has its own)
  void (*fn)(void*) = nullptr;
  void* arg = nullptr;
  uint32_t wake = 0;            // millis() when it may run again
  bool dead = false;
};
std::vector<Task*> tasks;       // tasks[0] is the main (JavaScript) task
size_t cur = 0;
Task* graveyard = nullptr;      // a finished task, freed from another task's stack
}  // namespace

// Save the callee-saved registers on this stack, store sp in *save, switch to next and restore.
__attribute__((naked, noinline)) static void ctxSwitch(uint32_t** save, uint32_t* next) {
  asm volatile(
      "push {r4-r11, lr}\n"
      "vpush {s16-s31}\n"
      "str sp, [r0]\n"
      "mov sp, r1\n"
      "vpop {s16-s31}\n"
      "pop {r4-r11, pc}\n");
}

static void ensureMain() {
  if (tasks.empty()) tasks.push_back(new Task());
}

static void reap() {
  if (!graveyard) return;
  free(graveyard->stack);
  delete graveyard;
  graveyard = nullptr;
}

// Pick the next task that's ready (round robin, the current one last) and switch to it. With none
// ready, keep the hardware serviced until one is.
static void schedule() {
  for (;;) {
    uint32_t now = millis();
    size_t n = tasks.size(), next = SIZE_MAX;
    for (size_t k = 1; k <= n; k++) {  // the next task that's ready, the current one last
      size_t i = (cur + k) % n;
      Task* t = tasks[i];
      if (!t->dead && (int32_t)(now - t->wake) >= 0) { next = i; break; }
    }
    // Nothing ready: wait on the main task's stack. The waiting work (hwYield: the USB host stack,
    // its HID callbacks, and the interrupts that land meanwhile) needs far more room than a small
    // task stack has - waiting on the 2 KB speaker task's stack overflowed it and hung the board.
    if (next == SIZE_MAX && cur != 0) next = 0;
    if (next == SIZE_MAX) { hwYield(); continue; }  // the main task waits here
    if (next == cur) { reap(); return; }
    Task* from = tasks[cur];
    cur = next;
    ctxSwitch(&from->sp, tasks[next]->sp);
    reap();  // back here: maybe the task we came from just finished
    // We may have been switched to only so the waiting happens on this (main) stack: carry on
    // only once this task is really due.
    if ((int32_t)(millis() - tasks[cur]->wake) >= 0) return;
  }
}

static void taskEntry() {
  Task* t = tasks[cur];
  t->fn(t->arg);
  vTaskDelete(nullptr);  // a task function that returns ends the task
}

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t stackBytes, void* arg, int,
                                   TaskHandle_t* handle, int) {
  ensureMain();
  Task* t = new (std::nothrow) Task();
  if (!t) return pdFAIL;
  stackBytes = (stackBytes + 7) & ~7u;
  t->stack = (uint8_t*)malloc(stackBytes);
  if (!t->stack) { delete t; return pdFAIL; }
  t->fn = fn;
  t->arg = arg;
  t->wake = millis();
  // the frame ctxSwitch pops: s16-s31, r4-r11, then pc = taskEntry (100 bytes, so sp ends 8-aligned)
  uint32_t* top = (uint32_t*)(t->stack + stackBytes);
  uint32_t* sp = top - 25;
  for (int i = 0; i < 24; i++) sp[i] = 0;
  sp[24] = (uint32_t)&taskEntry;
  t->sp = sp;
  tasks.push_back(t);
  if (handle) *handle = t;
  return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
  ensureMain();
  if (task && task != tasks[cur]) return;  // only a task ending itself is supported
  if (cur == 0) return;                     // the main task can't end
  Task* t = tasks[cur];
  t->dead = true;
  tasks.erase(tasks.begin() + cur);
  cur = 0;
  reap();
  graveyard = t;                            // its stack is freed once we're off it
  Task* m = tasks[0];
  uint32_t* dummy;
  m->wake = 0;
  ctxSwitch(&dummy, m->sp);                 // never returns
  for (;;) {}
}

void vTaskDelay(uint32_t ms) {
  ensureMain();
  tasks[cur]->wake = millis() + ms;
  schedule();
}

// ---- the REPL's serial port (see port.h)
#include "Adafruit_TinyUSB.h"
extern mutex_t __usb_mutex;
extern "C" void TinyUSB_Device_Task(void);

ReplSerial replSerial;
namespace {
// The USB interrupt skips its work while this is held, and nothing triggers it again until the next
// USB event - so do the skipped work on the way out (tud_task takes the lock itself).
struct UsbLock {
  UsbLock() { mutex_enter_blocking(&__usb_mutex); }
  ~UsbLock() {
    mutex_exit(&__usb_mutex);
    TinyUSB_Device_Task();
  }
};
}  // namespace

int ReplSerial::available() { UsbLock l; return tud_cdc_available(); }
int ReplSerial::read() { UsbLock l; return tud_cdc_available() ? tud_cdc_read_char() : -1; }
int ReplSerial::peek() { UsbLock l; uint8_t c; return tud_cdc_peek(&c) ? c : -1; }
void ReplSerial::flush() { UsbLock l; tud_cdc_write_flush(); }
int ReplSerial::availableForWrite() { UsbLock l; return tud_cdc_write_available(); }

size_t ReplSerial::write(const uint8_t* buf, size_t n) {
  size_t done = 0;
  uint32_t stuck = millis();
  while (done < n) {
    uint32_t k;
    {
      UsbLock l;
      if (!tud_cdc_connected()) break;  // nobody listening: drop it, as Serial does
      k = tud_cdc_write(buf + done, n - done);
      tud_cdc_write_flush();
    }
    done += k;
    if (done < n) {
      if (k) stuck = millis();
      else if (millis() - stuck > 500) break;  // the computer stopped reading
    }
  }
  return done;
}
#endif
