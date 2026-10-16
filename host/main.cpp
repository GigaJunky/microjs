// Desktop build of the interpreter, for testing scripts without hardware.
//
//   ./jsh                              REPL (core language only)
//   ./jsh script.js                    run a script, then its timers until none are left
//   ./jsh --sim [options] script.js    run with a simulated CYD (lcd, touch, led, fs, ...)
//       --fs DIR          directory that plays the role of the flash filesystem (default: script's dir)
//       --sd DIR          directory that plays the SD card, i.e. "/sd/..." (default: FS_DIR/sd)
//       --board NAME      cyd (default) or cardputer (240x135, keyboard instead of touch)
//       --time MS         stop after MS milliseconds (needed for scripts with endless intervals)
//       --png FILE        save the simulated screen as a PNG at the end
//       --tap X,Y@MS      simulate a tap at time MS
//       --drag X,Y,X2,Y2@MS  simulate a finger drag starting at time MS
//       --key NAME@MS[+HOLD] press a key at time MS (for HOLD ms, default 50); NAME "space" = " "
//   --gc-stress     run the garbage collector before every statement (finds rooting bugs)
//   ./jsh --serve [PORT] script.js   interactive simulator in the browser (default port 8080):
//       live screen, mouse = touch, console, JavaScript prompt, BOOT button
#include "../src/js.h"
#include "sim.h"
#include "web.h"
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

static bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

int main(int argc, char** argv) {
  js::Interp I;
  auto t0 = std::chrono::steady_clock::now();
  auto now = [t0]() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  };
  I.write = [](const char* s, size_t n) { fwrite(s, 1, n, stdout); fflush(stdout); };
  I.millis = now;
  I.readFile = readFile;
  I.stackLimit = 512 * 1024;

  bool sim = false;
  int servePort = 0;
  long limit = -1;
  std::string png, fsDir, script;
  struct Gesture { bool drag; int x0, y0, x1, y1; uint32_t at; };
  std::vector<Gesture> gestures;
  struct KeyPress { std::string key; uint32_t at, hold; };
  std::vector<KeyPress> keys;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
      return argv[++i];
    };
    if (a == "--sim") sim = true;
    else if (a == "--serve") {
      servePort = 8080;
      if (i + 1 < argc && atoi(argv[i + 1]) > 0) servePort = atoi(argv[++i]);
    }
    else if (a == "--gc-stress") I.gcStress = true, I.gcThreshold = 0;
    else if (a == "--time") limit = atol(val().c_str());
    else if (a == "--png") png = val();
    else if (a == "--fs") fsDir = val();
    else if (a == "--sd") simSetSdDir(val());
    else if (a == "--board") {
      std::string b = val();
      if (b == "cardputer") simUseCardputer();
      else if (b != "cyd") { fprintf(stderr, "unknown board %s (cyd, cardputer)\n", b.c_str()); return 2; }
    }
    else if (a == "--key") {
      std::string v = val();
      size_t atPos = v.rfind('@');
      KeyPress k{v.substr(0, atPos), 0, 50};
      if (atPos == std::string::npos || sscanf(v.c_str() + atPos + 1, "%u+%u", &k.at, &k.hold) < 1) {
        fprintf(stderr, "bad key %s\n", v.c_str());
        return 2;
      }
      if (k.key == "space") k.key = " ";
      keys.push_back(k);
    }
    else if (a == "--tap" || a == "--drag") {
      Gesture g{a == "--drag", 0, 0, 0, 0, 0};
      std::string v = val();
      int n = g.drag ? sscanf(v.c_str(), "%d,%d,%d,%d@%u", &g.x0, &g.y0, &g.x1, &g.y1, &g.at)
                     : sscanf(v.c_str(), "%d,%d@%u", &g.x0, &g.y0, &g.at);
      if (n != (g.drag ? 5 : 3)) { fprintf(stderr, "bad gesture %s\n", v.c_str()); return 2; }
      gestures.push_back(g);
    } else script = a;
  }

  if (servePort) {
    if (script.empty()) { fprintf(stderr, "usage: jsh --serve [PORT] script.js\n"); return 2; }
    if (fsDir.empty()) {
      size_t slash = script.rfind('/');
      fsDir = slash == std::string::npos ? "." : script.substr(0, slash);
    }
    WebSim web;
    std::unique_ptr<js::Interp> ip;
    std::string current = script;  // what BOOT runs: the script given on the command line
    auto start = [&](const std::string& path, bool fromSimFs) {
      ip.reset();
      ip.reset(new js::Interp());
      ip->write = [&web](const char* s, size_t n) {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
        web.log.append(s, n);
        if (web.log.size() > 400000) web.log.erase(0, 100000);
      };
      ip->millis = now;
      ip->stackLimit = 512 * 1024;
      simInstall(*ip, fsDir);
      ip->readFile = [](const std::string& p, std::string& o) { return simReadFile(p, o) || readFile(p, o); };
      simUseRestartingLaunch();
      std::string src;
      bool ok = fromSimFs ? simReadFile(path, src) : readFile(path, src);
      if (!ok) web.log += "cannot read " + path + "\n";
      else ip->runSafe(src, path.c_str());
    };
    web.boot = [&]() { start(current, false); };
    web.step = [&]() {
      simPoll(*ip, now());
      ip->runTimers();
      std::string next = simTakeLaunch();
      if (!next.empty()) {
        web.log += "\nrunning " + next + "\n";
        start(next, true);
      }
    };
    web.runCode = [&](const std::string& code) {
      web.log += "> " + code + "\n";
      js::Value r;
      if (ip->runSafe(code, "<prompt>", &r) && !r.isUndef()) web.log += ip->inspect(r) + "\n";
    };
    serveSim(web, servePort);
  }

  if (sim) {
    if (fsDir.empty()) {
      size_t slash = script.rfind('/');
      fsDir = slash == std::string::npos ? "." : script.substr(0, slash);
    }
    simInstall(I, fsDir);
    I.readFile = [](const std::string& p, std::string& o) { return simReadFile(p, o) || readFile(p, o); };
    for (auto& g : gestures) {
      if (g.drag) simQueueDrag(g.at, g.x0, g.y0, g.x1, g.y1);
      else simQueueTap(g.at, g.x0, g.y0);
    }
    for (auto& k : keys) simQueueKey(k.at, k.key, k.hold);
  }

  if (!script.empty()) {
    std::string src;
    if (!readFile(script, src)) { fprintf(stderr, "cannot read %s\n", script.c_str()); return 2; }
    bool ok = I.runSafe(src, script.c_str());
    while ((I.hasTimers() || (sim && simPending())) && (limit < 0 || now() < (uint32_t)limit)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (sim) simPoll(I, now());
      I.runTimers();
    }
    if (sim && !png.empty()) {
      simSavePng(png);
      printf("[sim] screen saved to %s, %s\n", png.c_str(), simLedState().c_str());
    }
    return ok ? 0 : 1;
  }

  std::string buf, line;
  printf("> ");
  fflush(stdout);
  while (std::getline(std::cin, line)) {
    buf += line + "\n";
    if (js::Interp::isIncomplete(buf)) { printf("... "); fflush(stdout); continue; }
    js::Value r;
    if (I.runSafe(buf, "<repl>", &r) && !r.isUndef()) I.print(I.inspect(r) + "\n");
    buf.clear();
    while (I.hasTimers()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      I.runTimers();
    }
    printf("> ");
    fflush(stdout);
  }
  return 0;
}
