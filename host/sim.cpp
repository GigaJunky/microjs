// sim.cpp - desktop stand-in for the CYD hardware API (lcd, touch, led, fs, ...).
// Drawing goes into an in-memory framebuffer that can be saved as a PNG, so example
// scripts can be run and checked without a board. Text uses the classic 5x7 font
// scaled up as an approximation of the TFT_eSPI fonts.
#include "sim.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include "../src/version.h"
#include <thread>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

#ifdef SIM_FONT
#define PROGMEM
#include SIM_FONT
#define HAVE_FONT 1
#endif

using namespace js;

namespace {

struct Canvas {
  int w, h;
  std::vector<uint16_t> px;
  int curX = 0, curY = 0, size = 1, font = 1, datum = 0, padding = 0;
  uint16_t fg = 0xFFFF, bg = 0;
  bool bgfill = false;
  Canvas(int W, int H) : w(W), h(H), px(W * H, 0) {}
  void pixel(int x, int y, uint16_t c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = c;
  }
  void fillRect(int x, int y, int rw, int rh, uint16_t c) {
    for (int j = y; j < y + rh; j++)
      for (int i = x; i < x + rw; i++) pixel(i, j, c);
  }
  void line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
      pixel(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  void ellipse(int cx, int cy, int rx, int ry, uint16_t c, bool fill) {
    if (rx <= 0 || ry <= 0) { pixel(cx, cy, c); return; }
    for (int y = -ry; y <= ry; y++)
      for (int x = -rx; x <= rx; x++) {
        double d = (double)x * x / ((double)rx * rx) + (double)y * y / ((double)ry * ry);
        double inner = rx > 1 && ry > 1 ? (double)(x * x) / ((rx - 1.0) * (rx - 1.0)) + (double)(y * y) / ((ry - 1.0) * (ry - 1.0)) : 0;
        if (d <= 1.0 && (fill || inner > 1.0)) pixel(cx + x, cy + y, c);
      }
  }
  void triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int minX = std::min({x0, x1, x2}), maxX = std::max({x0, x1, x2});
    int minY = std::min({y0, y1, y2}), maxY = std::max({y0, y1, y2});
    auto edge = [](int ax, int ay, int bx, int by, int px, int py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
    for (int y = minY; y <= maxY; y++)
      for (int x = minX; x <= maxX; x++) {
        int a = edge(x0, y0, x1, y1, x, y), b = edge(x1, y1, x2, y2, x, y), d = edge(x2, y2, x0, y0, x, y);
        if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) pixel(x, y, c);
      }
  }
  void roundRect(int x, int y, int rw, int rh, int r, uint16_t c, bool fill) {
    r = std::min(r, std::min(rw, rh) / 2);
    for (int j = 0; j < rh; j++)
      for (int i = 0; i < rw; i++) {
        int dx = i < r ? r - i : i >= rw - r ? i - (rw - r - 1) : 0;
        int dy = j < r ? r - j : j >= rh - r ? j - (rh - r - 1) : 0;
        double d = std::sqrt((double)dx * dx + (double)dy * dy);
        bool inside = d <= r + 0.5;
        bool edge = i == 0 || j == 0 || i == rw - 1 || j == rh - 1 || (dx && dy && d > r - 0.5);
        if (inside && (fill || edge)) pixel(x + i, y + j, c);
      }
  }
  void wideLine(double ax, double ay, double bx, double by, double wd, uint16_t c) {
    double len = std::hypot(bx - ax, by - ay);
    int steps = std::max(1, (int)len);
    for (int i = 0; i <= steps; i++) {
      double t = (double)i / steps;
      ellipse((int)std::lround(ax + (bx - ax) * t), (int)std::lround(ay + (by - ay) * t), (int)(wd / 2), (int)(wd / 2), c, true);
    }
  }
  void arc(int cx, int cy, int r, int ir, int a0, int a1, uint16_t c) {
    for (int y = -r; y <= r; y++)
      for (int x = -r; x <= r; x++) {
        double d = std::hypot(x, y);
        if (d > r || d < ir) continue;
        double a = std::atan2(-x, y) * 180 / M_PI;
        if (a < 0) a += 360;
        bool in = a0 <= a1 ? (a >= a0 && a <= a1) : (a >= a0 || a <= a1);
        if (in) pixel(cx + x, cy + y, c);
      }
  }
  void scale(int f, int& sx, int& sy) const {
    switch (f) {
      case 2: sx = 1; sy = 2; break;
      case 4: sx = 2; sy = 3; break;
      case 6: case 7: sx = 5; sy = 6; break;
      case 8: sx = 8; sy = 9; break;
      default: sx = 1; sy = 1;
    }
    sx *= size;
    sy *= size;
  }
  int textWidth(const std::string& s, int f) const { int sx, sy; scale(f, sx, sy); return (int)s.size() * 6 * sx; }
  int fontHeight(int f) const { int sx, sy; scale(f, sx, sy); return 8 * sy; }
  void glyph(int x, int y, unsigned char ch, int sx, int sy, uint16_t c) {
#if HAVE_FONT
    for (int col = 0; col < 5; col++) {
      uint8_t bits = ::font[ch * 5 + col];
      for (int row = 0; row < 8; row++)
        if (bits & (1 << row)) fillRect(x + col * sx, y + row * sy, sx, sy, c);
    }
#else
    if (ch != ' ') fillRect(x, y + sy, 5 * sx, 6 * sy, c);
#endif
  }
  int text(const std::string& s, int x, int y, int f) {
    int sx, sy;
    scale(f, sx, sy);
    int tw = textWidth(s, f), th = 8 * sy;
    int bw = std::max(tw, padding);
    int ox = x, oy = y;
    if (datum % 3 == 1) ox -= tw / 2;
    else if (datum % 3 == 2) ox -= tw;
    if (datum / 3 == 1) oy -= th / 2;
    else if (datum / 3 == 2) oy -= th;
    if (bgfill) {
      int bx = datum % 3 == 1 ? x - bw / 2 : datum % 3 == 2 ? x - bw : x;
      fillRect(bx, oy, bw, th, bg);
    }
    for (size_t i = 0; i < s.size(); i++) glyph(ox + (int)i * 6 * sx, oy, (unsigned char)s[i], sx, sy, fg);
    return tw;
  }
  void print(const std::string& s) {
    int sx, sy;
    scale(font, sx, sy);
    for (char ch : s) {
      if (ch == '\n') { curX = 0; curY += 8 * sy; continue; }
      if (bgfill) fillRect(curX, curY, 6 * sx, 8 * sy, bg);
      glyph(curX, curY, (unsigned char)ch, sx, sy, fg);
      curX += 6 * sx;
    }
  }
};

Canvas* screen = nullptr;
int rotation = 1;
std::string fsRoot = ".";
Object* touchHandlers = nullptr;
Object* spriteProto = nullptr;
struct TouchEvent { uint32_t at; const char* type; int x, y; };
bool cardputer = false;  // --board cardputer: 240x135 screen, keyboard, no touch
int panelLong() { return cardputer ? 240 : 320; }
int panelShort() { return cardputer ? 135 : 240; }
Object* keyHandlers = nullptr;
struct KeyEvent { uint32_t at; bool down; std::string key; };
std::vector<KeyEvent> keyQueue;
std::vector<std::string> keysHeld;
std::vector<TouchEvent> touchQueue;
int ledR = 0, ledG = 0, ledB = 0;

void spriteFree(void* p) { delete (Canvas*)p; }
Canvas* cv(Interp& I, Value self) {
  if (self.isObj() && self.o()->finalize == spriteFree) {
    if (!self.o()->ext) I.throwError("Error", "sprite has been freed");
    return (Canvas*)self.o()->ext;
  }
  return screen;
}

#define IA(i) I.toInt32(JS_ARG(i))
#define FA(i) I.toNum(JS_ARG(i))
uint16_t CA_(Interp& I, Value v) {
  if (v.isStr() && v.s()->s.size() > 1 && v.s()->s[0] == '#') {
    const std::string& s = v.s()->s;
    uint32_t x = strtoul(s.c_str() + 1, nullptr, 16);
    int r, g, b;
    if (s.size() == 4) { r = ((x >> 8) & 15) * 17; g = ((x >> 4) & 15) * 17; b = (x & 15) * 17; }
    else { r = (x >> 16) & 255; g = (x >> 8) & 255; b = x & 255; }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  return (uint16_t)(I.toInt32(v) & 0xFFFF);
}
#define CA(i) CA_(I, JS_ARG(i))
#define NOP(name) JS_NATIVE(name) { (void)I; (void)self; (void)args; (void)argc; return Value(); }

JS_NATIVE(d_fillScreen) { Canvas* c = cv(I, self); c->fillRect(0, 0, c->w, c->h, CA(0)); return Value(); }
JS_NATIVE(d_drawPixel) { cv(I, self)->pixel(IA(0), IA(1), CA(2)); return Value(); }
JS_NATIVE(d_readPixel) {
  Canvas* c = cv(I, self);
  int x = IA(0), y = IA(1);
  return Value::num(x >= 0 && y >= 0 && x < c->w && y < c->h ? c->px[y * c->w + x] : 0);
}
JS_NATIVE(d_drawLine) { cv(I, self)->line(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_drawWideLine) { cv(I, self)->wideLine(FA(0), FA(1), FA(2), FA(3), FA(4), CA(5)); return Value(); }
JS_NATIVE(d_hline) { cv(I, self)->fillRect(IA(0), IA(1), IA(2), 1, CA(3)); return Value(); }
JS_NATIVE(d_vline) { cv(I, self)->fillRect(IA(0), IA(1), 1, IA(2), CA(3)); return Value(); }
JS_NATIVE(d_drawRect) {
  Canvas* c = cv(I, self);
  int x = IA(0), y = IA(1), w = IA(2), h = IA(3);
  uint16_t col = CA(4);
  c->fillRect(x, y, w, 1, col); c->fillRect(x, y + h - 1, w, 1, col);
  c->fillRect(x, y, 1, h, col); c->fillRect(x + w - 1, y, 1, h, col);
  return Value();
}
JS_NATIVE(d_fillRect) { cv(I, self)->fillRect(IA(0), IA(1), IA(2), IA(3), CA(4)); return Value(); }
JS_NATIVE(d_drawRoundRect) { cv(I, self)->roundRect(IA(0), IA(1), IA(2), IA(3), IA(4), CA(5), false); return Value(); }
JS_NATIVE(d_fillRoundRect) { cv(I, self)->roundRect(IA(0), IA(1), IA(2), IA(3), IA(4), CA(5), true); return Value(); }
JS_NATIVE(d_drawCircle) { cv(I, self)->ellipse(IA(0), IA(1), IA(2), IA(2), CA(3), false); return Value(); }
JS_NATIVE(d_fillCircle) { cv(I, self)->ellipse(IA(0), IA(1), IA(2), IA(2), CA(3), true); return Value(); }
JS_NATIVE(d_drawEllipse) { cv(I, self)->ellipse(IA(0), IA(1), IA(2), IA(3), CA(4), false); return Value(); }
JS_NATIVE(d_fillEllipse) { cv(I, self)->ellipse(IA(0), IA(1), IA(2), IA(3), CA(4), true); return Value(); }
JS_NATIVE(d_drawTriangle) {
  Canvas* c = cv(I, self);
  uint16_t col = CA(6);
  c->line(IA(0), IA(1), IA(2), IA(3), col); c->line(IA(2), IA(3), IA(4), IA(5), col); c->line(IA(4), IA(5), IA(0), IA(1), col);
  return Value();
}
JS_NATIVE(d_fillTriangle) { cv(I, self)->triangle(IA(0), IA(1), IA(2), IA(3), IA(4), IA(5), CA(6)); return Value(); }
JS_NATIVE(d_drawArc) { cv(I, self)->arc(IA(0), IA(1), IA(2), IA(3), IA(4), IA(5), CA(6)); return Value(); }
JS_NATIVE(d_drawString) {
  Canvas* c = cv(I, self);
  return Value::num(c->text(I.toStr(JS_ARG(0)), IA(1), IA(2), argc > 3 ? IA(3) : c->font));
}
JS_NATIVE(d_print) {
  Canvas* c = cv(I, self);
  for (int i = 0; i < argc; i++) c->print((i ? " " : "") + (args[i].isStr() ? args[i].s()->s : I.inspect(args[i])));
  return Value();
}
JS_NATIVE(d_println) { d_print(I, self, args, argc); cv(I, self)->print("\n"); return Value(); }
JS_NATIVE(d_setTextColor) {
  Canvas* c = cv(I, self);
  c->fg = CA(0);
  c->bgfill = argc > 1;
  if (argc > 1) c->bg = CA(1);
  return Value();
}
JS_NATIVE(d_setTextSize) { cv(I, self)->size = std::max(1, std::min(7, IA(0))); return Value(); }
JS_NATIVE(d_setTextFont) { cv(I, self)->font = IA(0); return Value(); }
JS_NATIVE(d_setTextDatum) { cv(I, self)->datum = IA(0); return Value(); }
JS_NATIVE(d_setTextPadding) { cv(I, self)->padding = IA(0); return Value(); }
JS_NATIVE(d_setCursor) { Canvas* c = cv(I, self); c->curX = IA(0); c->curY = IA(1); return Value(); }
JS_NATIVE(d_textWidth) { Canvas* c = cv(I, self); return Value::num(c->textWidth(I.toStr(JS_ARG(0)), argc > 1 ? IA(1) : c->font)); }
JS_NATIVE(d_fontHeight) { Canvas* c = cv(I, self); return Value::num(c->fontHeight(argc ? IA(0) : c->font)); }
JS_NATIVE(d_width) { (void)args; (void)argc; return Value::num(cv(I, self)->w); }
JS_NATIVE(d_height) { (void)args; (void)argc; return Value::num(cv(I, self)->h); }
JS_NATIVE(d_color) {
  (void)self;
  int r = IA(0) & 255, g = IA(1) & 255, b = IA(2) & 255;
  return Value::num(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
NOP(d_setTextWrap)
JS_NATIVE(lcd_setRotation) {
  (void)self;
  rotation = IA(0) & 3;
  bool landscape = rotation & 1;
  Canvas* n = new Canvas(landscape ? panelLong() : panelShort(), landscape ? panelShort() : panelLong());
  delete screen;
  screen = n;
  return Value();
}
JS_NATIVE(lcd_getRotation) { (void)I; (void)self; (void)args; (void)argc; return Value::num(rotation); }
NOP(lcd_invert)
NOP(g_backlight)
std::string fsPath(const std::string& p);
// lcd.drawImage / imageSize: the boards decode JPEG, PNG and BMP; here stb_image stands in, with
// the same fitting rules (shrink by a whole factor - powers of two for JPEG - and centre).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "stb_image.h"
static std::string simExt(const std::string& p) {
  size_t d = p.rfind('.');
  std::string e = d == std::string::npos ? "" : p.substr(d + 1);
  for (auto& c : e) c = tolower(c);
  return e;
}
JS_NATIVE(lcd_imageSize) {
  (void)self;
  int w, h, n;
  if (!stbi_info(fsPath(I.toStr(JS_ARG(0))).c_str(), &w, &h, &n)) return Value::null();
  Object* o = I.newObject();
  I.setOwn(o, "width", Value::num(w));
  I.setOwn(o, "height", Value::num(h));
  return Value::obj(o);
}
JS_NATIVE(lcd_drawImage) {  // same arguments and fitting as the boards (src/hw.cpp)
  (void)self;
  std::string path = I.toStr(JS_ARG(0));
  int w, h, n;
  unsigned char* img = stbi_load(fsPath(path).c_str(), &w, &h, &n, 3);
  if (!img) I.throwError("Error", "drawImage: not a JPEG, PNG or BMP picture: " + path);
  int x, y, dw, dh, bx = 0, by = 0, bw = screen->w, bh = screen->h;
  if (argc >= 5) bx = IA(1), by = IA(2), bw = IA(3), bh = IA(4);
  if (argc == 3 || argc == 4) {
    double zoom = argc == 4 ? I.toNum(args[3]) : 1.0;
    x = IA(1), y = IA(2), dw = std::max(1, (int)(w * zoom)), dh = std::max(1, (int)(h * zoom));
  } else {
    double zoom = std::min((double)bw / w, (double)bh / h);
    dw = std::max(1, (int)(w * zoom + 0.5)), dh = std::max(1, (int)(h * zoom + 0.5));
    x = bx + (bw - dw) / 2, y = by + (bh - dh) / 2;
  }
  double f = (double)dw / w;
  for (int j = 0; j < dh; j++)
    for (int i = 0; i < dw; i++) {
      int sx = std::min(w - 1, (int)((i + 0.5) / f)), sy = std::min(h - 1, (int)((j + 0.5) / f));
      const unsigned char* p = img + ((size_t)sy * w + sx) * 3;
      screen->pixel(x + i, y + j, (uint16_t)((p[0] & 0xF8) << 8 | (p[1] & 0xFC) << 3 | p[2] >> 3));
    }
  stbi_image_free(img);
  Object* o = I.newObject();
  I.setOwn(o, "width", Value::num(w));
  I.setOwn(o, "height", Value::num(h));
  I.setOwn(o, "zoom", Value::num(f));
  I.setOwn(o, "x", Value::num(x));
  I.setOwn(o, "y", Value::num(y));
  I.setOwn(o, "w", Value::num(dw));
  I.setOwn(o, "h", Value::num(dh));
  return Value::obj(o);
}
JS_NATIVE(lcd_createSprite) {
  (void)self;
  int w = IA(0), h = IA(1);
  if (w <= 0 || h <= 0 || w > 320 || h > 320) I.throwError("RangeError", "createSprite: bad size");
  Object* o = I.newObject(spriteProto);
  o->ext = new Canvas(w, h);
  o->finalize = spriteFree;
  return Value::obj(o);
}
JS_NATIVE(sprite_push) {
  Canvas* s = cv(I, self);
  int x = IA(0), y = IA(1);
  bool transp = argc > 2;
  uint16_t t = transp ? CA(2) : 0;
  for (int j = 0; j < s->h; j++)
    for (int i = 0; i < s->w; i++) {
      uint16_t c = s->px[j * s->w + i];
      if (!transp || c != t) screen->pixel(x + i, y + j, c);
    }
  return Value();
}
JS_NATIVE(sprite_free) {
  (void)I; (void)args; (void)argc;
  if (self.isObj() && self.o()->finalize == spriteFree && self.o()->ext) {
    delete (Canvas*)self.o()->ext;
    self.o()->ext = nullptr;
  }
  return Value();
}

JS_NATIVE(touch_on) {
  (void)self;
  std::string ev = I.toStr(JS_ARG(0));
  if (ev != "down" && ev != "move" && ev != "up") I.throwError("TypeError", "touch.on: event must be 'down', 'move' or 'up'");
  if (!I.isCallable(JS_ARG(1))) I.throwError("TypeError", "touch.on: handler must be a function");
  I.setOwn(touchHandlers, ev.c_str(), args[1]);
  return Value();
}
JS_NATIVE(touch_off) {
  (void)self;
  if (argc == 0) touchHandlers->props.clear();
  else I.delProp(Value::obj(touchHandlers), I.toStr(args[0]));
  return Value();
}
JS_NATIVE(keyboard_on) {
  (void)self;
  std::string ev = I.toStr(JS_ARG(0));
  if (ev != "key" && ev != "keyup") I.throwError("TypeError", "keyboard.on: event must be 'key' or 'keyup'");
  if (!I.isCallable(JS_ARG(1))) I.throwError("TypeError", "keyboard.on: handler must be a function");
  I.setOwn(keyHandlers, ev.c_str(), args[1]);
  return Value();
}
JS_NATIVE(keyboard_off) {
  (void)self;
  if (argc == 0) keyHandlers->props.clear();
  else I.delProp(Value::obj(keyHandlers), I.toStr(args[0]));
  return Value();
}
JS_NATIVE(keyboard_held) {
  (void)self; (void)args; (void)argc;
  Array* a = I.newArray();
  for (auto& k : keysHeld) a->items.push_back(I.str(k));
  return Value::obj(a);
}
static bool simTouching = false;  // the simulated finger, between "down" and "up"
static int simTouchX = 0, simTouchY = 0;
JS_NATIVE(touch_read) {
  (void)self; (void)args; (void)argc;
  if (!simTouching) return Value::null();
  Object* o = I.newObject();
  I.setOwn(o, "x", Value::num(simTouchX));
  I.setOwn(o, "y", Value::num(simTouchY));
  I.setOwn(o, "z", Value::num(1000));
  return Value::obj(o);
}
// touch.calibrate: kept (not applied - simulated touches are already screen positions)
static std::vector<double> simCal;
static int simCalRot = -1;
JS_NATIVE(touch_calibrate) {
  (void)self;
  if (argc >= 1 && args[0].isNull()) simCalRot = -1;
  else if (argc >= 1 && args[0].isObj() && args[0].o()->kind == K::Array) {
    simCal.clear();
    for (auto& v : ((Array*)args[0].o())->items) simCal.push_back(I.toNum(v));
    simCalRot = argc >= 2 ? IA(1) & 3 : 1;
  }
  if (simCalRot < 0) return Value::null();
  Object* o = I.newObject();
  Array* a = I.newArray();
  for (double v : simCal) a->items.push_back(Value::num(v));
  I.setOwn(o, "matrix", Value::obj(a));
  I.setOwn(o, "rotation", Value::num(simCalRot));
  return Value::obj(o);
}

JS_NATIVE(g_led) {
  (void)self;
  ledR = IA(0); ledG = IA(1); ledB = IA(2);
  return Value();
}
NOP(g_tone)
NOP(g_noTone)
NOP(g_pinMode)
NOP(g_digitalWrite)
JS_NATIVE(g_neopixel) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(true); }
JS_NATIVE(g_zero) { (void)I; (void)self; (void)args; (void)argc; return Value::num(0); }
JS_NATIVE(g_light) { (void)I; (void)self; (void)args; (void)argc; return Value::num(1234); }
JS_NATIVE(g_delay) {
  (void)self;
  std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, IA(0))));
  return Value();
}
JS_NATIVE(g_micros) {
  (void)I; (void)self; (void)args; (void)argc;
  return Value::num((double)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}
JS_NATIVE(g_localTime) {
  (void)self; (void)args; (void)argc;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  Object* o = I.newObject();
  I.setOwn(o, "year", Value::num(t.tm_year + 1900));
  I.setOwn(o, "month", Value::num(t.tm_mon + 1));
  I.setOwn(o, "day", Value::num(t.tm_mday));
  I.setOwn(o, "hour", Value::num(t.tm_hour));
  I.setOwn(o, "minute", Value::num(t.tm_min));
  I.setOwn(o, "second", Value::num(t.tm_sec));
  I.setOwn(o, "weekday", Value::num(t.tm_wday));
  I.setOwn(o, "valid", Value::boolean(true));
  return Value::obj(o);
}
JS_NATIVE(sys_heap) {
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "free", Value::num(180000));
  I.setOwn(o, "js", Value::num(I.heapBytes()));
  return Value::obj(o);
}
JS_NATIVE(sys_cpuMhz) { (void)I; (void)self; (void)args; (void)argc; return Value::num(240); }

std::string sdRoot;  // where "/sd/..." lives (--sd DIR); default: an sd/ folder inside the fs root
std::string fsPath(const std::string& p) {
  if (p == "/sd" || p.rfind("/sd/", 0) == 0) return (sdRoot.empty() ? fsRoot + "/sd" : sdRoot) + p.substr(3);
  return fsRoot + (p.empty() || p[0] != '/' ? "/" : "") + p;
}
// sys.firmwareInfo / installFirmware: the same .FLH check as the boards (see src/hw.cpp)
JS_NATIVE(sys_firmwareInfo) {
  (void)self;
  std::string data, err, notes;
  size_t imageAt = 0;
  if (!simReadFile(I.toStr(JS_ARG(0)), data)) err = "can't open the file";
  else if (!data.empty() && ((uint8_t)data[0] == 0xE9 || (data.size() > 0x1000 && (uint8_t)data[0] == 0xFF && (uint8_t)data[0x1000] == 0xE9))) {  // a .bin: app image, or merged with the app at 0x10000
    bool merged = data.size() > 0x10000 && (uint8_t)data[0x8000] == 0xAA && (uint8_t)data[0x8001] == 0x50;
    imageAt = merged ? 0x10000 : 0;
    if (merged) notes = "Full flash image: installing just its firmware (the part at 0x10000).";
    if ((uint8_t)data[imageAt] != 0xE9) err = "no firmware image inside";
  } else {
    size_t dash = data.find('-'), tilde = data.find('~');
    uint32_t want = 0, sum = 0;
    bool digits = dash != std::string::npos && dash > 0 && dash < 12;
    for (size_t i = 0; digits && i < dash; i++) digits = isdigit((unsigned char)data[i]), want = want * 10 + (data[i] - '0');
    if (!digits) err = "not a firmware file";
    else if (tilde == std::string::npos) err = "not a firmware file (no '~')";
    else {
      for (size_t i = dash; i < data.size(); i++) sum += (uint8_t)data[i];
      imageAt = tilde + 1;
      notes = data.substr(dash + 1, std::min<size_t>(700, tilde - dash - 1));
      const std::string tag = "COCO ESP32 CUSTOM FIRMWARE/";
      if (notes.rfind(tag, 0) == 0) notes.erase(0, tag.size());
      if (sum != want) err = "checksum mismatch - the file is damaged";
      else if ((uint8_t)data[imageAt] != 0xE9) err = "no firmware image inside";
    }
  }
  Object* o = I.newObject();
  I.setOwn(o, "ok", Value::boolean(err.empty()));
  I.setOwn(o, "error", I.str(err));
  I.setOwn(o, "notes", I.str(notes));
  I.setOwn(o, "size", Value::num(err.empty() ? (double)(data.size() - imageAt) : 0));
  return Value::obj(o);
}
// audio: the simulator plays silently - "playing" for as long as the file would last at 128 kbps
static std::string simSong;
static double simSongStart = 0, simSongLen = 0, simPausedAt = -1;
static int simVolume = 100;
static double simNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
}
static double simSongAt() { return simPausedAt >= 0 ? simPausedAt : simNow() - simSongStart; }
JS_NATIVE(audio_play) {
  (void)self;
  std::string data;
  if (!simReadFile(I.toStr(JS_ARG(0)), data)) return I.str("can't open the file");
  simSong = I.toStr(JS_ARG(0));
  simSongLen = data.size() * 8 / 128000.0;
  simSongStart = simNow();
  simPausedAt = -1;
  return I.str("");
}
JS_NATIVE(audio_stop) { (void)I; (void)self; (void)args; (void)argc; simSong.clear(); return Value(); }
JS_NATIVE(audio_pause) {
  (void)self;
  bool p = argc ? I.truthy(args[0]) : simPausedAt < 0;
  if (p && simPausedAt < 0) simPausedAt = simNow() - simSongStart;
  else if (!p && simPausedAt >= 0) { simSongStart = simNow() - simPausedAt; simPausedAt = -1; }
  return Value::boolean(p);
}
JS_NATIVE(audio_volume) {
  (void)self;
  if (argc) simVolume = std::max(0, std::min(100, (int)I.toNum(args[0])));
  return Value::num(simVolume);
}
static bool simMuted = false;
JS_NATIVE(audio_mute) { (void)self; if (argc) simMuted = I.truthy(args[0]); return Value::boolean(simMuted); }
JS_NATIVE(audio_status) {
  (void)self; (void)args; (void)argc;
  if (!simSong.empty() && simSongAt() >= simSongLen) simSong.clear();
  Object* o = I.newObject();
  I.setOwn(o, "playing", Value::boolean(!simSong.empty()));
  I.setOwn(o, "paused", Value::boolean(simPausedAt >= 0));
  I.setOwn(o, "path", I.str(simSong));
  I.setOwn(o, "seconds", Value::num(simSong.empty() ? 0 : simSongAt()));
  I.setOwn(o, "duration", Value::num(simSongLen));
  I.setOwn(o, "rate", Value::num(44100));
  I.setOwn(o, "kbps", Value::num(128));
  I.setOwn(o, "channels", Value::num(2));
  I.setOwn(o, "error", I.str(""));
  return Value::obj(o);
}
// video: the simulator can't decode video; the API answers so apps run
JS_NATIVE(video_play) { (void)self; (void)args; (void)argc; return I.str("the simulator can't play video"); }
JS_NATIVE(video_stop) { (void)I; (void)self; (void)args; (void)argc; return Value(); }
JS_NATIVE(video_pause) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(false); }
JS_NATIVE(video_dma) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(false); }
JS_NATIVE(video_status) {
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "playing", Value::boolean(false));
  I.setOwn(o, "error", I.str(""));
  return Value::obj(o);
}
// sys.battery: a pretend battery at 3.87 V, so battery displays can be tried here
JS_NATIVE(sys_battery) {
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "volts", Value::num(3.87));
  I.setOwn(o, "percent", Value::num(60));
  I.setOwn(o, "charging", Value::boolean(true));
  I.setOwn(o, "usb", Value::boolean(true));
  return Value::obj(o);
}
// web: no server in the simulator; the API answers so apps run (requests never arrive)
JS_NATIVE(web_start) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(true); }
JS_NATIVE(web_nop) { (void)I; (void)self; (void)args; (void)argc; return Value(); }
JS_NATIVE(web_url) { (void)self; (void)args; (void)argc; return I.str("http://192.168.1.50/"); }
JS_NATIVE(wifi_ap_sim) {  // the board's own network: pretend it started
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "ssid", I.str("micro-js-SIM0"));
  I.setOwn(o, "ip", I.str("192.168.4.1"));
  I.setOwn(o, "password", I.str(""));
  return Value::obj(o);
}
JS_NATIVE(sys_installFirmware) { (void)self; (void)args; (void)argc; return I.str("the simulator can't install firmware"); }

JS_NATIVE(fs_read) {
  (void)self;
  std::string out;
  if (!simReadFile(I.toStr(JS_ARG(0)), out)) return Value::null();
  return I.str(out);
}
JS_NATIVE(fs_write) {
  (void)self;
  std::ofstream f(fsPath(I.toStr(JS_ARG(0))), std::ios::binary);
  f << I.toStr(JS_ARG(1));
  return Value::boolean((bool)f);
}
JS_NATIVE(fs_exists) { (void)self; return Value::boolean(std::filesystem::exists(fsPath(I.toStr(JS_ARG(0))))); }
JS_NATIVE(fs_remove) {
  (void)self;
  std::error_code ec;
  return Value::boolean(std::filesystem::remove(fsPath(I.toStr(JS_ARG(0))), ec));
}
JS_NATIVE(fs_append) {
  (void)self;
  std::ofstream f(fsPath(I.toStr(JS_ARG(0))), std::ios::binary | std::ios::app);
  f << I.toStr(JS_ARG(1));
  return Value::boolean((bool)f);
}
JS_NATIVE(fs_list) {
  (void)self;
  Array* a = I.newArray();
  std::vector<std::string> names;
  std::error_code ec;  // a missing directory (e.g. no SD card) lists as empty, like on the board
  for (auto& e : std::filesystem::directory_iterator(fsPath(argc ? I.toStr(args[0]) : "/"), ec))
    names.push_back(e.path().filename().string() + (e.is_directory() ? "/" : ""));
  std::sort(names.begin(), names.end());
  for (auto& n : names) a->items.push_back(I.str(n));
  return Value::obj(a);
}
namespace sfs = std::filesystem;
static Value simEntry(Interp& I, const sfs::path& p) {
  std::error_code ec;
  bool dir = sfs::is_directory(p, ec);
  Object* o = I.newObject();
  I.setOwn(o, "name", I.str(p.filename().string()));
  I.setOwn(o, "size", Value::num(dir ? 0 : (double)sfs::file_size(p, ec)));
  I.setOwn(o, "dir", Value::boolean(dir));
  return Value::obj(o);
}
static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
JS_NATIVE(fs_writeBase64) {
  (void)self;
  std::string in = I.toStr(JS_ARG(1)), out;
  int val = 0, bits = -8;
  for (char c : in) {
    const char* p = strchr(B64, c);
    if (c == '=' || !p || !c) continue;
    val = (val << 6) + (int)(p - B64);
    bits += 6;
    if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
  }
  std::ofstream f(fsPath(I.toStr(JS_ARG(0))), std::ios::binary | (argc > 2 && I.truthy(args[2]) ? std::ios::app : std::ios::trunc));
  f << out;
  return Value::boolean((bool)f);
}
JS_NATIVE(fs_readBase64) {
  (void)self;
  std::string data;
  if (!simReadFile(I.toStr(JS_ARG(0)), data)) return Value::null();
  size_t off = argc > 1 ? (size_t)I.toNum(args[1]) : 0, len = argc > 2 ? (size_t)I.toNum(args[2]) : 480;
  if (off >= data.size()) return I.str("");
  std::string chunk = data.substr(off, std::min<size_t>(len, 3000)), out;
  int val = 0, bits = -6;
  for (unsigned char c : chunk) {
    val = (val << 8) + c;
    bits += 8;
    while (bits >= 0) { out.push_back(B64[(val >> bits) & 0x3F]); bits -= 6; }
  }
  if (bits > -6) out.push_back(B64[((val << 8) >> (bits + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return I.str(out);
}
JS_NATIVE(fs_contains) {
  (void)self;
  std::string data;
  return Value::boolean(simReadFile(I.toStr(JS_ARG(0)), data) && data.find(I.toStr(JS_ARG(1))) != std::string::npos);
}
JS_NATIVE(fs_dir) {
  (void)self;
  Array* a = I.newArray();
  std::vector<sfs::path> ps;
  std::error_code ec;
  for (auto& e : sfs::directory_iterator(fsPath(argc ? I.toStr(args[0]) : "/"), ec)) ps.push_back(e.path());
  std::sort(ps.begin(), ps.end());
  for (auto& p : ps) a->items.push_back(simEntry(I, p));
  return Value::obj(a);
}
JS_NATIVE(fs_stat) {
  (void)self;
  sfs::path p = fsPath(I.toStr(JS_ARG(0)));
  std::error_code ec;
  return sfs::exists(p, ec) ? simEntry(I, p) : Value::null();
}
JS_NATIVE(fs_mkdir) { (void)self; std::error_code ec; return Value::boolean(sfs::create_directory(fsPath(I.toStr(JS_ARG(0))), ec)); }
JS_NATIVE(fs_rmdir) { (void)self; std::error_code ec; sfs::path p = fsPath(I.toStr(JS_ARG(0))); return Value::boolean(sfs::is_directory(p, ec) && sfs::remove(p, ec)); }
JS_NATIVE(fs_rename) {
  (void)self;
  std::string a = I.toStr(JS_ARG(0)), b = I.toStr(JS_ARG(1));
  if ((a.rfind("/sd", 0) == 0) != (b.rfind("/sd", 0) == 0)) return Value::boolean(false);  // like the boards
  std::error_code ec;
  sfs::rename(fsPath(a), fsPath(b), ec);
  return Value::boolean(!ec);
}
JS_NATIVE(fs_copy) {
  (void)self;
  std::error_code ec;
  sfs::copy_file(fsPath(I.toStr(JS_ARG(0))), fsPath(I.toStr(JS_ARG(1))), sfs::copy_options::overwrite_existing, ec);
  return Value::boolean(!ec);
}
JS_NATIVE(fs_space) {
  (void)self;
  std::error_code ec;
  sfs::space_info si = sfs::space(fsPath(argc ? I.toStr(args[0]) : "/"), ec);
  Object* o = I.newObject();
  I.setOwn(o, "total", Value::num(ec ? 0 : (double)si.capacity));
  I.setOwn(o, "used", Value::num(ec ? 0 : (double)(si.capacity - si.available)));
  return Value::obj(o);
}
// The simulator's "WiFi" is the computer's network: http.get runs curl. With
// SIM_HTTP_DIR=dir, requests are answered from files named after the host instead
// (e.g. dir/api.open-meteo.com), for offline tests.
static const char* simHttpDir() { return getenv("SIM_HTTP_DIR"); }
JS_NATIVE(sim_joystick_read) {
  (void)self; (void)args; (void)argc;
  Object* o = I.newObject();
  I.setOwn(o, "connected", Value::boolean(false));
  for (const char* k : {"x", "y", "x2", "y2", "buttons"}) I.setOwn(o, k, Value::num(0));
  I.setOwn(o, "hat", Value::num(-1));
  I.setOwn(o, "raw", Value::obj(I.newArray()));
  return Value::obj(o);
}
JS_NATIVE(sim_joystick_count) { (void)I; (void)self; (void)args; (void)argc; return Value::num(0); }
JS_NATIVE(wifi_true) { (void)I; (void)self; (void)args; (void)argc; return Value::boolean(true); }
static Value httpResult(Interp& I, int status, const std::string& body) {
  Object* o = I.newObject();
  I.setOwn(o, "status", Value::num(status));
  I.setOwn(o, "body", I.str(body));
  return Value::obj(o);
}
JS_NATIVE(sim_http_get) {
  (void)self;
  std::string url = I.toStr(JS_ARG(0));
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) I.throwError("Error", "http: bad url " + url);
  if (simHttpDir()) {
    std::string host = url.substr(url.find("//") + 2);
    host = host.substr(0, host.find('/'));
    std::ifstream f(std::string(simHttpDir()) + "/" + host);
    if (!f) I.throwError("Error", "http: no canned response for " + host);
    std::stringstream ss;
    ss << f.rdbuf();
    return httpResult(I, 200, ss.str());
  }
  if (url.find('\'') != std::string::npos) I.throwError("Error", "http: unsupported character in url");
  std::string cmd = "curl -s -L -m 15 -w '\\n%{http_code}' -- '" + url + "'";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) I.throwError("Error", "http: cannot run curl");
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) out.append(buf, n);
  pclose(pipe);
  size_t nl = out.rfind('\n');
  int status = nl == std::string::npos ? 0 : atoi(out.c_str() + nl + 1);
  if (status <= 0) I.throwError("Error", "http: connection refused (simulator: curl failed)");
  return httpResult(I, status, out.substr(0, nl));
}
JS_NATIVE(sim_date_now) {
  (void)I; (void)self; (void)args; (void)argc;
  return Value::num((double)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
}
JS_NATIVE(wifi_status) { (void)self; (void)args; (void)argc; return I.str("connected"); }
JS_NATIVE(wifi_ip) { (void)self; (void)args; (void)argc; return I.str("192.168.0.42"); }
JS_NATIVE(wifi_rssi) { (void)self; (void)args; (void)argc; return Value::num(-47); }
JS_NATIVE(wifi_scan) {  // a few fake networks so WiFi tools can be tried in the simulator
  (void)self; (void)args; (void)argc;
  static const struct { const char* ssid; int rssi; bool open; } nets[] = {
    {"HomeNetwork", -46, false}, {"xfinitywifi", -61, true}, {"NETGEAR58", -72, false},
    {"ATT5g9ks", -77, false}, {"Chromecast1234", -83, false}, {"", -88, false},
  };
  Array* a = I.newArray();
  for (auto& n : nets) {
    Object* o = I.newObject();
    I.setOwn(o, "ssid", I.str(n.ssid));
    I.setOwn(o, "rssi", Value::num(n.rssi));
    I.setOwn(o, "open", Value::boolean(n.open));
    a->items.push_back(Value::obj(o));
  }
  return Value::obj(a);
}
JS_NATIVE(http_fail) { (void)self; (void)args; (void)argc; I.throwError("Error", "http: WiFi is not connected (simulator)"); }

std::string simLaunchPath;  // set by launch() in the browser simulator: restart with this script
bool simLaunchRestarts = false;
JS_NATIVE(g_launch) {
  (void)self;
  std::string path = I.toStr(JS_ARG(0));
  if (!simLaunchRestarts) {  // command-line simulator: just run it here
    std::string src;
    if (!simReadFile(path, src)) I.throwError("Error", "cannot read file '" + path + "'");
    return I.run(src, path.c_str());
  }
  simLaunchPath = path;  // like the board: a fresh interpreter once this handler returns
  I.clearTimers();
  return Value();
}

JS_NATIVE(g_readConfig) {
  (void)self; (void)args; (void)argc;
  for (const char* path : {"/sd/micro-js.json", "/micro-js.json", "/sd/esp32-js.json", "/esp32-js.json", "/sd/cyd-js.json", "/cyd-js.json"}) {
    std::string src;
    if (!simReadFile(path, src)) continue;
    Value json = I.getGlobal("JSON"), text = I.str(src);
    try {
      return I.call(I.get(json, "parse"), json, &text, 1);
    } catch (JsThrow& e) {
      I.throwError("SyntaxError", std::string(path) + ": " + I.toStr(I.get(e.v, "message")));
    }
  }
  return Value::obj(I.newObject());
}

void fire(Interp& I, const char* ev, int x, int y) {
  Prop* p = touchHandlers->own(ev);
  if (!p || !I.isCallable(p->v)) return;
  size_t mark = I.temps.size();
  Value fn = p->v;
  I.rootTemp(fn);
  Object* o = I.newObject();
  I.setOwn(o, "x", Value::num(x));
  I.setOwn(o, "y", Value::num(y));
  I.setOwn(o, "z", Value::num(1000));
  I.callSafe(fn, Value(), {Value::obj(o)});
  I.temps.resize(mark);
}

}  // namespace

bool simReadFile(const std::string& path, std::string& out) {
  std::ifstream f(fsPath(path), std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

void simSetSdDir(const std::string& dir) { sdRoot = dir; }

void simInstall(Interp& I, const std::string& root) {
  fsRoot = root;
  screen = new Canvas(panelLong(), panelShort());
  Object* G = I.newObject();
  struct { const char* n; NativeFn f; } dm[] = {
    {"fillScreen", d_fillScreen}, {"drawPixel", d_drawPixel}, {"readPixel", d_readPixel}, {"drawLine", d_drawLine},
    {"drawWideLine", d_drawWideLine}, {"drawFastHLine", d_hline}, {"drawFastVLine", d_vline}, {"drawRect", d_drawRect},
    {"fillRect", d_fillRect}, {"drawRoundRect", d_drawRoundRect}, {"fillRoundRect", d_fillRoundRect},
    {"fillSmoothRoundRect", d_fillRoundRect}, {"drawCircle", d_drawCircle}, {"fillCircle", d_fillCircle},
    {"fillSmoothCircle", d_fillCircle}, {"drawEllipse", d_drawEllipse}, {"fillEllipse", d_fillEllipse},
    {"drawTriangle", d_drawTriangle}, {"fillTriangle", d_fillTriangle}, {"drawArc", d_drawArc},
    {"drawString", d_drawString}, {"print", d_print}, {"println", d_println}, {"setTextColor", d_setTextColor},
    {"setTextSize", d_setTextSize}, {"setTextFont", d_setTextFont}, {"setTextDatum", d_setTextDatum},
    {"setTextPadding", d_setTextPadding}, {"setTextWrap", d_setTextWrap}, {"setCursor", d_setCursor},
    {"textWidth", d_textWidth}, {"fontHeight", d_fontHeight}, {"width", d_width}, {"height", d_height},
    {"color", d_color}};
  for (auto& m : dm) I.def(G, m.n, m.f);
  struct { const char* n; uint32_t c; } colors[] = {
    {"BLACK", 0x0000}, {"WHITE", 0xFFFF}, {"RED", 0xF800}, {"GREEN", 0x07E0}, {"BLUE", 0x001F},
    {"YELLOW", 0xFFE0}, {"CYAN", 0x07FF}, {"MAGENTA", 0xF81F}, {"ORANGE", 0xFDA0}, {"PINK", 0xFE19},
    {"PURPLE", 0x780F}, {"NAVY", 0x000F}, {"MAROON", 0x7800}, {"OLIVE", 0x7BE0}, {"DARKGREEN", 0x03E0},
    {"DARKCYAN", 0x03EF}, {"LIGHTGREY", 0xD69A}, {"DARKGREY", 0x7BEF}, {"GREENYELLOW", 0xB7E0},
    {"BROWN", 0x9A60}, {"GOLD", 0xFEA0}, {"SILVER", 0xC618}, {"SKYBLUE", 0x867D}, {"VIOLET", 0x915C}};
  for (auto& c : colors) I.setOwn(G, c.n, Value::num(c.c));
  const char* datums[] = {"TL", "TC", "TR", "ML", "MC", "MR", "BL", "BC", "BR"};
  for (int i = 0; i < 9; i++) I.setOwn(G, datums[i], Value::num(i));

  Object* lcd = I.newObject(G);
  I.def(lcd, "setRotation", lcd_setRotation);
  I.def(lcd, "getRotation", lcd_getRotation);
  I.def(lcd, "invert", lcd_invert);
  I.def(lcd, "backlight", g_backlight);
  I.def(lcd, "createSprite", lcd_createSprite);
  I.def(lcd, "drawImage", lcd_drawImage);
  I.def(lcd, "imageSize", lcd_imageSize);
  I.setGlobal("lcd", Value::obj(lcd));
  spriteProto = I.newObject(G);
  I.def(spriteProto, "push", sprite_push);
  I.def(spriteProto, "free", sprite_free);
  I.roots.push_back(Value::obj(spriteProto));

  keyHandlers = I.newObject();
  I.roots.push_back(Value::obj(keyHandlers));
  Object* keyboard = I.newObject();
  I.def(keyboard, "on", keyboard_on);
  I.def(keyboard, "off", keyboard_off);
  I.def(keyboard, "held", keyboard_held);
  I.def(keyboard, "capture", wifi_true);  // board-only (Escape / Ctrl+C go to the app); nothing to do here
  {  // joysticks come from the board's USB ports; the simulator has none connected
    Object* joystick = I.newObject();
    I.def(joystick, "read", sim_joystick_read);
    I.def(joystick, "count", sim_joystick_count);
    I.def(joystick, "on", wifi_true);
    I.def(joystick, "off", wifi_true);
    I.def(joystick, "keys", wifi_true);
    I.setGlobal("joystick", Value::obj(joystick));
  }
  I.setGlobal("keyboard", Value::obj(keyboard));
  touchHandlers = I.newObject();
  I.roots.push_back(Value::obj(touchHandlers));
  Object* touch = I.newObject();
  I.def(touch, "read", touch_read);
  I.def(touch, "raw", touch_read);
  I.def(touch, "calibrate", touch_calibrate);
  I.def(touch, "on", touch_on);
  I.def(touch, "off", touch_off);
  I.setGlobal("touch", Value::obj(touch));

  struct { const char* n; NativeFn f; } globals[] = {
    {"led", g_led}, {"backlight", g_backlight}, {"tone", g_tone}, {"noTone", g_noTone}, {"light", g_light},
    {"pinMode", g_pinMode}, {"digitalWrite", g_digitalWrite}, {"digitalRead", g_zero}, {"analogRead", g_zero}, {"neopixel", g_neopixel},
    {"delay", g_delay}, {"micros", g_micros}, {"localTime", g_localTime}, {"readConfig", g_readConfig}};
  for (auto& g : globals) I.setGlobal(g.n, Value::obj(I.newNative(g.f, g.n)));
  I.setGlobal("launch", Value::obj(I.newNative(g_launch, "launch")));
  const char* pins[][2] = {{"INPUT", "1"}, {"OUTPUT", "3"}, {"INPUT_PULLUP", "5"}, {"INPUT_PULLDOWN", "9"}, {"HIGH", "1"}, {"LOW", "0"}};
  for (auto& p : pins) I.setGlobal(p[0], Value::num(atoi(p[1])));

  Object* board = I.newObject();  // same shape as on the boards (see src/hw.cpp)
  I.setOwn(board, "name", I.str(cardputer ? "Simulator (Cardputer)" : "Simulator"));
  I.setOwn(board, "touch", I.str(cardputer ? "none" : "capacitive"));
  I.setOwn(board, "hasTouch", Value::boolean(!cardputer));
  I.setOwn(board, "hasKeyboard", Value::boolean(true));
  I.setOwn(board, "hasLed", Value::boolean(true));
  I.setOwn(board, "hasSpeaker", Value::boolean(false));
  I.setOwn(board, "hasMusic", Value::boolean(true));
  I.setOwn(board, "hasBattery", Value::boolean(true));
  I.setOwn(board, "id", I.str("00:00:00:00:00:51"));
  I.setOwn(board, "lowMemory", Value::boolean(getenv("SIM_LOW_MEMORY") != nullptr));  // try the CYD's launcher
  I.setOwn(board, "hasLight", Value::boolean(!cardputer));
  I.setOwn(board, "hasSd", Value::boolean(true));
  I.setOwn(board, "psram", Value::num(8 * 1024 * 1024));  // the desktop has memory to spare (and TLS)
  I.setGlobal("board", Value::obj(board));

  Object* sys = I.newObject();
  I.def(sys, "heap", sys_heap);
  I.def(sys, "cpuMhz", sys_cpuMhz);
  I.def(sys, "battery", sys_battery);
  I.setOwn(sys, "version", I.str(MICROJS_VERSION));
  I.def(sys, "firmwareInfo", sys_firmwareInfo);
  I.def(sys, "installFirmware", sys_installFirmware);
  I.setGlobal("sys", Value::obj(sys));
  Object* audio = I.newObject();
  I.def(audio, "play", audio_play);
  I.def(audio, "stop", audio_stop);
  I.def(audio, "pause", audio_pause);
  I.def(audio, "volume", audio_volume);
  I.def(audio, "status", audio_status);
  I.def(audio, "mute", audio_mute);
  I.setGlobal("audio", Value::obj(audio));
  Object* video = I.newObject();
  I.def(video, "play", video_play);
  I.def(video, "stop", video_stop);
  I.def(video, "pause", video_pause);
  I.def(video, "status", video_status);
  I.def(video, "dma", video_dma);
  I.setGlobal("video", Value::obj(video));
  Object* web = I.newObject();
  I.def(web, "start", web_start);
  I.def(web, "on", web_nop);
  I.def(web, "off", web_nop);
  I.def(web, "stop", web_nop);
  I.def(web, "url", web_url);
  I.setGlobal("web", Value::obj(web));
  Object* fsObj = I.newObject();
  I.def(fsObj, "read", fs_read);
  I.def(fsObj, "write", fs_write);
  I.def(fsObj, "append", fs_append);
  I.def(fsObj, "remove", fs_remove);
  I.def(fsObj, "exists", fs_exists);
  I.def(fsObj, "list", fs_list);
  I.def(fsObj, "dir", fs_dir);
  I.def(fsObj, "contains", fs_contains);
  I.def(fsObj, "writeBase64", fs_writeBase64);
  I.def(fsObj, "readBase64", fs_readBase64);
  I.def(fsObj, "stat", fs_stat);
  I.def(fsObj, "mkdir", fs_mkdir);
  I.def(fsObj, "rmdir", fs_rmdir);
  I.def(fsObj, "rename", fs_rename);
  I.def(fsObj, "copy", fs_copy);
  I.def(fsObj, "space", fs_space);
  I.setGlobal("fs", Value::obj(fsObj));
  Object* wifi = I.newObject();
  I.def(wifi, "connect", wifi_true);
  I.def(wifi, "ntp", wifi_true);
  I.def(wifi, "status", wifi_status);
  I.def(wifi, "ip", wifi_ip);
  I.def(wifi, "rssi", wifi_rssi);
  I.def(wifi, "scan", wifi_scan);
  I.def(wifi, "ap", wifi_ap_sim);
  I.setGlobal("wifi", Value::obj(wifi));
  Object* http = I.newObject();
  I.def(http, "get", sim_http_get);
  I.def(http, "post", http_fail);
  I.setGlobal("http", Value::obj(http));
  I.def(I.getGlobal("Date").o(), "now", sim_date_now);
  I.temps.clear();
}

void simQueueTap(uint32_t at, int x, int y) {
  touchQueue.push_back({at, "down", x, y});
  touchQueue.push_back({at + 60, "up", x, y});
}

void simQueueDrag(uint32_t at, int x0, int y0, int x1, int y1) {
  touchQueue.push_back({at, "down", x0, y0});
  for (int i = 1; i <= 10; i++)
    touchQueue.push_back({at + i * 15, "move", x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10});
  touchQueue.push_back({at + 180, "up", x1, y1});
}

void simQueueKey(uint32_t at, const std::string& key, uint32_t holdMs) {
  keyQueue.push_back({at, true, key});
  keyQueue.push_back({at + holdMs, false, key});
}

bool simPending() { return !touchQueue.empty() || !keyQueue.empty(); }

void simPoll(Interp& I, uint32_t now) {
  while (!keyQueue.empty()) {
    auto it = std::min_element(keyQueue.begin(), keyQueue.end(),
                               [](const KeyEvent& a, const KeyEvent& b) { return a.at < b.at; });
    if (it->at > now) break;
    KeyEvent ev = *it;
    keyQueue.erase(it);
    auto held = std::find(keysHeld.begin(), keysHeld.end(), ev.key);
    if (ev.down && held == keysHeld.end()) keysHeld.push_back(ev.key);
    if (!ev.down && held != keysHeld.end()) keysHeld.erase(held);
    Prop* p = keyHandlers->own(ev.down ? "key" : "keyup");
    if (!p || !I.isCallable(p->v)) continue;
    size_t mark = I.temps.size();
    Value fn = p->v;
    I.rootTemp(fn);
    Object* o = I.newObject();
    I.setOwn(o, "key", I.str(ev.key));
    for (const char* m : {"shift", "fn", "ctrl", "opt", "alt"}) I.setOwn(o, m, Value::boolean(false));
    I.callSafe(fn, Value(), {Value::obj(o)});
    I.temps.resize(mark);
  }
  while (!touchQueue.empty()) {
    auto it = std::min_element(touchQueue.begin(), touchQueue.end(),
                               [](const TouchEvent& a, const TouchEvent& b) { return a.at < b.at; });
    if (it->at > now) break;
    TouchEvent ev = *it;
    touchQueue.erase(it);
    simTouching = ev.type != std::string("up");  // for touch.read / touch.raw
    simTouchX = ev.x, simTouchY = ev.y;
    fire(I, ev.type, ev.x, ev.y);
  }
}

// minimal PNG writer (stored deflate blocks, no compression library needed)
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0 - (c & 1)));
  }
  return ~c;
}

bool simSavePng(const std::string& path) {
  int w = screen->w, h = screen->h;
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      uint16_t c = screen->px[y * w + x];
      raw.push_back(((c >> 11) & 31) * 255 / 31);
      raw.push_back(((c >> 5) & 63) * 255 / 63);
      raw.push_back((c & 31) * 255 / 31);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t i = 0; i < raw.size(); i += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size() ? 1 : 0);
    z.push_back(n & 255); z.push_back(n >> 8);
    z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
  }
  uint32_t adler = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 255);
  std::ofstream f(path, std::ios::binary);
  auto be32 = [&](uint32_t v) { uint8_t q[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; f.write((char*)q, 4); };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32((uint32_t)data.size());
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    f.write((char*)td.data(), td.size());
    be32(crc32(td.data(), td.size()));
  };
  f.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                               uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  return (bool)f;
}

const uint16_t* simScreen(int& w, int& h) {
  w = screen->w;
  h = screen->h;
  return screen->px.data();
}

void simLed(int& r, int& g, int& b) { r = ledR; g = ledG; b = ledB; }

void simTouchNow(const char* type, int x, int y) {
  if (!cardputer) touchQueue.push_back({0, type, x, y});
}

void simKeyNow(bool down, const std::string& key) { keyQueue.push_back({0, down, key}); }

void simUseCardputer() { cardputer = true; }

void simUseRestartingLaunch() { simLaunchRestarts = true; }
std::string simTakeLaunch() {
  std::string p;
  p.swap(simLaunchPath);
  if (!p.empty()) { touchQueue.clear(); keyQueue.clear(); }
  return p;
}

std::string simLedState() {
  return "led(" + std::to_string(ledR) + "," + std::to_string(ledG) + "," + std::to_string(ledB) + ")";
}
