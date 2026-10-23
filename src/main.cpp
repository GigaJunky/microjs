// main.cpp - micro-js firmware: serial REPL + event loop running the JavaScript interpreter
#include "version.h"
#include <Arduino.h>
#include <new>
#include "port.h"
#include "board.h"
#include "js.h"
#include "hw.h"

static const uint32_t JS_STACK = BOARD_JS_STACK_KB * 1024;  // JavaScript task stack (recursion depth)
static const uint32_t STACK_RESERVE = 12 * 1024;  // headroom for native code (TLS, drawing)
static const int PIN_BOOT = BOARD_BOOT_BUTTON;  // press: back to the menu; hold at reset: skip main.js

static js::Interp* I = nullptr;
static std::string pending;   // serial bytes received while a script was running
static std::string line, code, lastLine;
static bool saving = false;
static std::string saveName, saveBuf;
static uint32_t lastYield = 0;
static bool bootDown = true;    // BOOT button state (debounced); true so a press held since reset is ignored
static uint32_t bootChanged = 0;
static bool backToMenu = false;
static std::string launchPath;  // set by launch(): run this script in a fresh interpreter
static std::string launchArg;   // ...with this string as the global ARG (e.g. the file to edit)
static bool consoleShown = false;

// true once per press of the BOOT button
static bool bootPressed() {
  if (PIN_BOOT < 0) return false;  // no BOOT button the firmware can read (RP2350: it's the flash's)
  bool down = digitalRead(PIN_BOOT) == LOW;
  uint32_t now = millis();
  if (down == bootDown || now - bootChanged < 50) return false;
  bootChanged = now;
  bootDown = down;
  return down;
}

// BOOT, or Escape / F12 / Ctrl+C on a USB keyboard
static bool menuPressed() {
  bool boot = bootPressed();
  return hwMenuKey() || boot;
}

// Out-of-memory handling. ESP-IDF has no emergency pool for C++ exceptions, so an empty heap
// would abort the firmware. A reserved block is released when `new` fails; the collector then
// runs at the next statement and, if memory is still short, a catchable JS error is thrown.
static const size_t PARACHUTE = 4 * 1024;
static void* parachute = nullptr;
static void onNewFailure() {
  if (!parachute) throw std::bad_alloc();
  free(parachute);
  parachute = nullptr;
  if (I) I->gcPending = true;
}

// REPL output goes to the serial port and, on boards with a keyboard, to the on-screen console
// Quiet mode (Ctrl-E on, Ctrl-F or Ctrl-C off, or 3 s without input): tools/upload.js and the web IDE
// switch it on for an upload. The REPL then neither echoes the lines it receives nor shows a prompt, and
// stays off the on-screen console (drawing every typed character there made uploads slow); output a
// script prints still goes to the serial port.
static bool quiet = false;
static uint32_t quietAt = 0;
static void term(const char* s, size_t n) {
  replSerial.write((const uint8_t*)s, n);
  if (!quiet) hwConsoleWrite(s, n);
}
// Lines starting with STX (0x02) come from tools/bridge.js (network and clock through the computer,
// MICROJS_BRIDGE): they go to hwBridgeLine(), never to the REPL. true = the byte was one of theirs.
static bool bridgeByte(int c) {
#if MICROJS_BRIDGE
  static bool in = false;
  static std::string msg;
  if (!in) {
    if (c != 2) return false;
    in = true;
    msg.clear();
    return true;
  }
  if (c == '\n' || c == '\r') {
    in = false;
    hwBridgeLine(msg);
  } else if (msg.size() < 600 * 1024) {
    msg += (char)c;
  }
  return true;
#else
  (void)c;
  return false;
#endif
}

static void out(const char* s) { term(s, strlen(s)); }
static void out(const std::string& s) { term(s.data(), s.size()); }

// A key typed on the board's keyboard while no app handles keys: REPL input, as if from the serial
// port. Escape clears the line, the arrows recall it like on the serial terminal.
void hwConsoleKey(const char* key, uint8_t mods) {
  bool ctrl = mods & 2;
  if (ctrl && key[0] && !key[1]) {  // Ctrl+letter -> control code (Ctrl+C 3, Ctrl+D 4, ...)
    char c = (char)(toupper((unsigned char)key[0]) & 0x1F);
    pending += c;
  } else if (!key[1]) pending += key[0];
  else if (!strcmp(key, "Enter")) pending += '\r';
  else if (!strcmp(key, "Backspace")) pending += (char)127;
  else if (!strcmp(key, "Tab")) pending += "  ";
  else if (!strcmp(key, "Escape")) pending += (char)0x15;  // kill line
  else if (!strcmp(key, "ArrowUp")) pending += "\x1b[A";
}

static void prompt() {
  if (!saving && !quiet) out(code.empty() ? "> " : "... ");
}

static void newInterp() {
  delete I;
  js::poolTrim();  // the old interpreter's spilled pool chunks back to the heap: a clean start
  I = new js::Interp();
  I->write = [](const char* s, size_t n) { term(s, n); };
  I->millis = []() { return (uint32_t)millis(); };
  I->readFile = [](const std::string& p, std::string& o) { return hwReadFile(p, o); };
  I->stackLimit = JS_STACK - STACK_RESERVE;
  I->gcMin = BOARD_GC_MIN_KB * 1024;
  I->gcMax = BOARD_GC_MAX_KB * 1024;
  I->freeMemory = []() -> size_t { return ESP.getFreeHeap(); };
  I->lowMemory = BOARD_LOW_MEMORY_KB * 1024;
  I->poll = []() {
    if (hwStopKey()) {  // Ctrl+C on the board's keyboard: stop the script, back to the console
      pending.clear();
      I->clearTimers();
      hwStop(*I);
      throw js::JsInterrupt();
    }
    if (menuPressed()) {  // BOOT button (or a menu key): abort the running script, back to the menu
      backToMenu = true;
      I->clearTimers();
      hwStop(*I);
      throw js::JsInterrupt();
    }
    while (replSerial.available()) {
      int c = replSerial.read();
      if (bridgeByte(c)) continue;
      if (c == 3) {  // Ctrl-C aborts the running script and stops timers & handlers
        pending.clear();
        I->clearTimers();
        hwStop(*I);
        throw js::JsInterrupt();
      }
      if (pending.size() < 8192) pending += (char)c;
    }
    hwService();
    uint32_t now = millis();
    if (now - lastYield > (MICROJS_RP2 ? 10 : 100)) {  // let the idle task run (watchdog); RP2350: music and video
      lastYield = now;
      vTaskDelay(1);
    }
    if (ESP.getFreeHeap() < BOARD_LOW_MEMORY_KB * 1024) I->gcPending = true;
  };
  I->onCollect = []() {
    if (!parachute) parachute = malloc(PARACHUTE);
    if (!parachute && ESP.getFreeHeap() < 24 * 1024) I->throwError("Error", "out of memory");
  };
  hwInstall(*I);
  // launch(path): stop everything and run the script in a fresh interpreter. Unlike load(),
  // the caller's memory is freed first, so the script starts with a clean, unfragmented heap.
  I->setGlobal("launch", js::Value::obj(I->newNative(
      [](js::Interp& in, js::Value, js::Value* args, int argc) -> js::Value {
        launchPath = in.toStr(JS_ARG(0));
        launchArg = argc > 1 ? in.toStr(args[1]) : "";
        in.clearTimers();
        hwStop(in);
        return js::Value();
      }, "launch")));
}

static void help() {
  out("micro-js " MICROJS_VERSION " (built " __DATE__ ") commands:\n"
      "  .help            this text\n"
      "  .ls [dir]        list files (\"/sd\" for the SD card)\n"
      "  .cat FILE        print a file\n"
      "  .run FILE        run a script\n"
      "  .edit FILE       edit a file on the screen (boards with a keyboard)\n"
      "  .save FILE       paste a file, finish with a line containing only .end (or Ctrl-D)\n"
      "  .rm FILE         delete a file\n"
      "  .mem             memory usage\n"
      "  .stop            stop timers and touch handlers\n"
      "  .reset           fresh interpreter (clears all variables)\n"
      "  .reboot          restart the board\n"
      "Ctrl-C interrupts a running script; the BOOT button also stops it and reopens the menu.\n"
      "Scripts named main.js (flash or /sd) run at boot; hold BOOT while resetting to skip them.\n"
      "APIs: lcd, touch, led(), backlight(), tone(), light(), fs, wifi, http, sys, pinMode()...\n");
}

static std::string fullPath(const std::string& f) { return f.empty() || f[0] == '/' ? f : "/" + f; }

static void command(const std::string& cmdline) {
  size_t sp = cmdline.find(' ');
  std::string cmd = cmdline.substr(0, sp);
  std::string arg = sp == std::string::npos ? "" : cmdline.substr(sp + 1);
  while (!arg.empty() && arg.back() == ' ') arg.pop_back();
  if (cmd == ".help") help();
  else if (cmd == ".ls") {
    std::string dir = arg.empty() ? "/" : fullPath(arg);
    auto list = hwList(dir);
    if (list.empty()) out("(empty)\n");
    for (auto& e : list) {
      char buf[96];
      snprintf(buf, sizeof buf, "%8u  %s%s\n", (unsigned)e.size, e.name.c_str(), e.dir ? "/" : "");
      out(buf);
    }
  } else if (cmd == ".cat") {
    std::string s;
    if (hwReadFile(fullPath(arg), s)) { out(s); if (s.empty() || s.back() != '\n') out("\n"); }
    else out("cannot read " + arg + "\n");
  } else if (cmd == ".run") {
    std::string s;
    if (hwReadFile(fullPath(arg), s)) I->runSafe(s, arg.c_str());
    else out("cannot read " + arg + "\n");
  } else if (cmd == ".edit") {
    if (arg.empty()) { out("usage: .edit FILE\n"); return; }
    std::string ed;
    for (const char* p : {"/edit.js", "/sd/edit.js"})
      if (hwFileContains(p, "")) { ed = p; break; }
    if (ed.empty()) { out("the editor (edit.js) is not on this board\n"); return; }
    launchPath = ed;
    launchArg = fullPath(arg);
  } else if (cmd == ".save") {
    if (arg.empty()) { out("usage: .save FILE\n"); return; }
    saving = true;
    saveName = fullPath(arg);
    saveBuf.clear();
    out("paste now; end with a line containing only .end\n");
  } else if (cmd == ".rm") {
    out(hwRemove(fullPath(arg)) ? "removed\n" : "cannot remove\n");
  } else if (cmd == ".mem") {
    char buf[128];
    snprintf(buf, sizeof buf, "heap free %u (min %u), js heap %u bytes\n", (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getMinFreeHeap(), (unsigned)I->heapBytes());
    out(buf);
  } else if (cmd == ".stop") {
    I->clearTimers();
    hwStop(*I);
    out("stopped\n");
  } else if (cmd == ".reset") {
    hwStop(*I);
    newInterp();
    out("interpreter reset\n");
  } else if (cmd == ".reboot") {
    ESP.restart();
  } else out("unknown command, try .help\n");
}

static void finishSave() {
  saving = false;
  bool ok = hwWriteFile(saveName, saveBuf);
  out(ok ? "saved " + saveName + " (" + std::to_string(saveBuf.size()) + " bytes)\n" : "cannot write " + saveName + "\n");
  saveBuf.clear();
}

static void handleLine(const std::string& ln) {
  if (saving) {
    if (ln == ".end") finishSave();
    else saveBuf += ln + "\n";
    return;
  }
  if (code.empty() && !ln.empty() && ln[0] == '.') {
    command(ln);
    return;
  }
  code += ln + "\n";
  if (js::Interp::isIncomplete(code)) return;
  std::string src;
  src.swap(code);
  js::Value r;
  if (I->runSafe(src, "<repl>", &r) && !r.isUndef()) out(I->inspect(r) + "\n");
}

static int readByte() {
  if (!pending.empty()) {
    int c = (uint8_t)pending[0];
    pending.erase(0, 1);
    return c;
  }
  while (replSerial.available()) {
    int c = replSerial.read();
    if (!bridgeByte(c)) return c;
  }
  return -1;
}

static void pollRepl() {
  static bool lastCR = false;
  static int esc = 0;
  int c;
  if (quiet && millis() - quietAt > 3000) { quiet = false; prompt(); }  // the uploader went away
  while ((c = readByte()) >= 0) {
    if (quiet) quietAt = millis();
    if (c == 5) { quiet = true; quietAt = millis(); line.clear(); continue; }  // Ctrl-E: quiet mode on
    if (c == 6) { quiet = false; prompt(); continue; }  // Ctrl-F: off
    if (esc) {  // ANSI escape sequence: ESC [ X
      if (esc == 1) { esc = c == '[' ? 2 : 0; continue; }
      esc = 0;
      if (c == 'A' && !saving) {  // up arrow: recall previous line
        out("\r\x1b[K");
        prompt();
        line = lastLine;
        out(line);
      }
      continue;
    }
    if (c == 27) { esc = 1; continue; }
    if (c == 3) {
      quiet = false;
      out("^C\n");
      line.clear();
      code.clear();
      saving = false;
      I->clearTimers();
      hwStop(*I);
      prompt();
      continue;
    }
    if (c == 0x15) {  // Ctrl-U (Escape on the board's keyboard): clear the line being typed
      line.clear();
      out("\r\x1b[K");
      prompt();
      continue;
    }
    if (c == 4 && saving) {
      if (!line.empty()) saveBuf += line + "\n";
      line.clear();
      finishSave();
      prompt();
      continue;
    }
    if (c == '\n' && lastCR) { lastCR = false; continue; }
    lastCR = c == '\r';
    if (c == '\r' || c == '\n') {
      if (!saving && !quiet) out("\r\n");
      std::string ln;
      ln.swap(line);
      if (!ln.empty() && !saving) lastLine = ln;
      handleLine(ln);
      prompt();
      continue;
    }
    if (c == 8 || c == 127) {
      if (!line.empty()) {
        line.pop_back();
        if (!saving) out("\b \b");
      }
      continue;
    }
    line += (char)c;
    if (!saving && !quiet) { char ch = (char)c; term(&ch, 1); }
  }
}

static void hideConsole() {  // an app is about to take the screen
  consoleShown = false;
  hwConsoleShow(false);
}

static void runLaunched() {
  std::string path = launchPath, src;
  launchPath.clear();
  hideConsole();
  hwStop(*I);
  newInterp();
  hwReset();
  out("\r\nrunning " + path + "\r\n");
  // Loading the script is one contiguous allocation the size of the file. Under memory pressure
  // (e.g. the previous app left WiFi up) that can fail, and on this board the C++ runtime cannot
  // even raise the out-of-memory exception to catch - it aborts. So free memory BEFORE the read:
  // always the HTTP reserve, and the whole WiFi driver unless the next app needs it (peek first,
  // without loading the file whole). The launched app re-reserves / reconnects as needed.
  hwSdEnsure();  // (a script on the card is read below, and hwFileContains reads it too)
  bool usesWifi = hwFileContains(path, "wifi.connect") || hwFileContains(path, "espnow.");  // (espnow needs the radio too)
  // the card's memory goes to the app when it needs it (the radio, or a board this small) and never names the
  // card: neither its code nor its ARG (the viewer, player and editor get their file's path that way)
  if ((usesWifi || hwLowMemory()) && !hwFileContains(path, "/sd") && launchArg.find("/sd") == std::string::npos) hwSdRelease();
  hwWifiPrepare(usesWifi);  // (false) also frees the net reserve + radio; (true) ensures WiFi
  hwNetRelease();           // drop the HTTP reserve either way so the read has contiguous room
  if (I) I->gcPending = false;
  if (!launchArg.empty()) I->setGlobal("ARG", I->str(launchArg));
  launchArg.clear();
  if (!hwReadFile(path, src)) {
    hwShowError(path, "cannot read file (low memory?)");
    out(path + ": cannot read the file (low memory?)\r\n");
  }
  else {
    I->dropSourceAfterParse = true;
    if (usesWifi) I->afterParse = hwNetReserve;  // reclaims the hole the freed source leaves
    if (!I->runSafe(src, path.c_str())) hwShowError(path + " failed:", I->lastError);
  }
  prompt();
}

static void runMain() {
  std::string src;
  const char* path = hwReadFile("/sd/main.js", src) ? "/sd/main.js" : hwReadFile("/main.js", src) ? "/main.js" : nullptr;
  if (!path) return;
  // as launched apps do: free the source once parsed, for the menu to run in (on a board without
  // PSRAM, the CYD, it needs that room)
  I->dropSourceAfterParse = true;
  I->runSafe(src, path);
}

#if MICROJS_RP2
// Call fn(nullptr) with the stack pointer at top (8-byte aligned); fn never returns here.
__attribute__((naked, noinline)) static void runOnStack(void (*fn)(void*), void* top) {
  asm volatile(
      "mov sp, r1\n"
      "mov r1, r0\n"
      "movs r0, #0\n"
      "blx r1\n"
      "1: b 1b\n");
}
#endif

static void jsTask(void*) {
#if MICROJS_RP2
  hwBegin();  // here, on the big stack: formatting the filesystem alone overflows the core's 4 KB
#endif
  newInterp();
  hwSplash();
  out("\r\nmicro-js " MICROJS_VERSION " - JavaScript for microcontroller display boards (" BOARD_NAME "). Type .help\r\n");
  bootDown = PIN_BOOT >= 0 && digitalRead(PIN_BOOT) == LOW;
  if (!bootDown) runMain();
  else out("BOOT held: skipping main.js\r\n");
  prompt();
  for (;;) {
    if (menuPressed()) backToMenu = true;
    if (backToMenu) {  // fresh interpreter (frees the old script's memory), then the launcher
      backToMenu = false;
      out("\r\nBOOT: back to menu\r\n");
      line.clear();
      code.clear();
      saving = false;
      hwStop(*I);
      newInterp();
      hideConsole();
      hwReset();
      runMain();
      prompt();
    }
    if (hwStopKey()) {  // Ctrl+C on the board's keyboard while an app runs between events
      out("^C\n");
      I->clearTimers();
      hwStop(*I);
      prompt();
    }
    pollRepl();
    hwPoll(*I);
    I->runTimers();
    if (!launchPath.empty()) runLaunched();
    // Nothing running (no timers, no key/touch handlers): the console takes the screen.
    bool idle = !I->hasTimers() && !hwHasHandlers() && launchPath.empty();
    if (idle != consoleShown) {
      consoleShown = idle;
      hwConsoleShow(idle);
    }
    if (I->gcPending) {
      try {
        I->collect();
      } catch (js::JsThrow& e) {
        I->reportError(e);
        I->clearTimers();
        hwStop(*I);
      }
    }
    vTaskDelay(1);
  }
}

void setup() {
#if !MICROJS_RP2  // (the RP2350's USB serial buffers on its own)
  Serial.setRxBufferSize(1024);
#endif
  Serial.begin(115200);
  if (PIN_BOOT >= 0) pinMode(PIN_BOOT, INPUT_PULLUP);
  // the interpreter's memory regions: in PSRAM when the board has it, else internal RAM
  uint32_t caps = BOARD_PSRAM && psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
  js::poolReserve(BOARD_POOL_KB * 1024, heap_caps_malloc(BOARD_POOL_KB * 1024, caps));
  js::arenaReserve(BOARD_ARENA_KB * 1024, heap_caps_malloc(BOARD_ARENA_KB * 1024, caps));
  // ...and anything beyond them in PSRAM too, so scripts never wear down the internal RAM that
  // WiFi, the display and task stacks need (falls back to internal RAM if PSRAM is full)
  if (caps == MALLOC_CAP_SPIRAM)
    js::poolSystemAlloc = [](size_t n) -> void* {
      void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      return p ? p : malloc(n);
    };
  parachute = malloc(PARACHUTE);
  std::set_new_handler(onNewFailure);
#if !MICROJS_RP2
  hwBegin();
#endif
#if MICROJS_RP2
  // No RTOS: the interpreter runs right here on core 0 (core 1 makes the picture), but the core's
  // own stack is only 4 KB, so switch to a big one of our own first.
  static uint8_t* stack = (uint8_t*)malloc(JS_STACK);
  runOnStack(jsTask, stack + (JS_STACK & ~7u));
#else
  xTaskCreatePinnedToCore(jsTask, "js", JS_STACK, nullptr, 1, nullptr, 1);
#endif
}

void loop() {
#if !MICROJS_RP2
  vTaskDelete(nullptr);
#endif
}
