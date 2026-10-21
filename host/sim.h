// sim.h - desktop simulator for the CYD hardware API
#pragma once
#include <string>
#include "../src/js.h"

void simInstall(js::Interp& I, const std::string& fsRoot);
void simSetSdDir(const std::string& dir);  // folder that plays the SD card ("/sd/...")
void simPoll(js::Interp& I, uint32_t nowMs);
void simQueueTap(uint32_t atMs, int x, int y);
void simQueueDrag(uint32_t atMs, int x0, int y0, int x1, int y1);
bool simPending();
bool simReadFile(const std::string& path, std::string& out);
bool simSavePng(const std::string& path);
std::string simLedState();
const uint16_t* simScreen(int& w, int& h);          // current framebuffer (RGB565)
void simLed(int& r, int& g, int& b);
void simTouchNow(const char* type, int x, int y);  // "down" / "move" / "up", delivered at the next poll
void simUseRestartingLaunch();
void simKeyNow(bool down, const std::string& key);
void simQueueKey(uint32_t at, const std::string& key, uint32_t holdMs);  // --key  // keyboard event from the browser
void simUseCardputer();  // 240x135 screen, keyboard, no touch (call before simInstall)  // launch() asks for a fresh interpreter (like the board)
std::string simTakeLaunch();    // the script launch() asked for, if any
std::string simTakeLaunchArg();  // ...and the ARG it asked for (call after simTakeLaunch)
