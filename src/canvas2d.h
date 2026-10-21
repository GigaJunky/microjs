// canvas2d.h - the browser's Canvas 2D API in C++, drawn through a small Gfx adapter. hw.cpp (a TFT_eSPI
// screen or sprite) and host/sim.cpp (the simulator's pixels) both include it, so the simulator runs the
// code the boards run. install() creates the global `canvas`; getContext("2d") returns the context.
// Supported: fillRect strokeRect clearRect, beginPath closePath moveTo lineTo arc ellipse rect roundRect
// quadraticCurveTo bezierCurveTo, fill stroke, fillText strokeText measureText, drawImage(path, x, y),
// save restore translate scale rotate transform setTransform resetTransform, and the properties fillStyle
// strokeStyle lineWidth font textAlign textBaseline. canvas.buffer(true) draws off-screen until
// canvas.flush(). Not supported: gradients, clip, shadows, alpha blending, getImageData, rotated text.
// A rectangle or circle goes straight to the Gfx, a convex shape is a triangle fan, a small concave one is
// cut into triangles, anything else is filled a row at a time; thick lines are filled quads.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "js.h"

namespace cvs {
using namespace js;

struct Gfx {  // the surface drawn on: colours are the device's own 16-bit values
  virtual ~Gfx() {}
  virtual int width() = 0;
  virtual int height() = 0;
  virtual uint16_t color(int r, int g, int b) = 0;
  virtual void fillRect(int x, int y, int w, int h, uint16_t c) = 0;
  virtual void drawRect(int x, int y, int w, int h, uint16_t c) = 0;
  virtual void fillCircle(int x, int y, int r, uint16_t c) = 0;
  virtual void drawCircle(int x, int y, int r, uint16_t c) = 0;
  virtual void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) = 0;
  virtual void hline(int x, int y, int w, uint16_t c) = 0;
  virtual void line(int x0, int y0, int x1, int y1, uint16_t c) = 0;
  virtual void setText(uint16_t color, int datum) = 0;  // text colour (no background) and anchor 0-8: TL TC TR ML MC MR BL BC BR
  virtual int drawText(const char* s, int x, int y, int font) = 0;
  virtual int textWidth(const char* s, int font) = 0;
  virtual bool buffer(bool on) = 0;  // draw off-screen (full colour if memory allows); false: no room
  virtual void flush() = 0;          // show the off-screen image
};

static const double kPi = 3.14159265358979323846, kTau = 2 * kPi;
struct Mat { double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };
struct Sub {  // one subpath, in screen coordinates
  std::vector<float> p;
  bool closed = false;
  char kind = 0;  // 'r': a rectangle k = x y w h; 'c': a full circle k = cx cy R a0 sx sy, outline not built yet
  float k[6] = {0, 0, 0, 0, 0, 0};
};
struct Saved { Mat m; std::string fs, ss, font, ta, tb; double lw; };

static Gfx* G = nullptr;
static Mat M;
static std::vector<Sub> path;
static int cur = -1;  // the subpath being extended, or -1
static std::vector<Saved> stack;
static Object *ctxObj = nullptr, *canvasObj = nullptr;

static inline double tx(double x, double y) { return M.a * x + M.c * y + M.e; }
static inline double ty(double x, double y) { return M.b * x + M.d * y + M.f; }
static inline bool simple() { return M.b == 0 && M.c == 0; }
static inline double sc() { return std::sqrt(std::fabs(M.a * M.d - M.b * M.c)); }
static inline int rd(double v) { return (int)std::lround(v); }

// ---- colours: "#rgb" "#rrggbb" "rgb(r,g,b)" "rgba(r,g,b,a)" and names; -1 = draw nothing
static int32_t colorOf(const std::string& s) {
  static std::string last;
  static int32_t lastV = 0;
  static bool have = false;
  if (have && s == last) return lastV;
  static const struct { const char* n; uint32_t v; } NAMES[] = {
    {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"lime", 0x00ff00}, {"blue", 0x0000ff},
    {"yellow", 0xffff00}, {"cyan", 0x00ffff}, {"aqua", 0x00ffff}, {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff}, {"gray", 0x808080},
    {"grey", 0x808080}, {"silver", 0xc0c0c0}, {"orange", 0xffa500}, {"purple", 0x800080}, {"pink", 0xffc0cb}, {"brown", 0xa52a2a},
    {"navy", 0x000080}, {"teal", 0x008080}, {"maroon", 0x800000}, {"olive", 0x808000}, {"gold", 0xffd700}, {"skyblue", 0x87ceeb},
    {"violet", 0xee82ee}};
  int r = 255, g = 255, b = 255;
  bool none = s == "transparent";
  for (auto& n : NAMES)
    if (s == n.n) { r = n.v >> 16; g = (n.v >> 8) & 255; b = n.v & 255; }
  if (!s.empty() && s[0] == '#') {
    std::string h = s.substr(1);
    if (h.size() == 3) h = std::string{h[0], h[0], h[1], h[1], h[2], h[2]};
    if (h.size() >= 6) {
      unsigned v = (unsigned)strtoul(h.substr(0, 6).c_str(), nullptr, 16);
      r = v >> 16; g = (v >> 8) & 255; b = v & 255;
    }
  } else if (s.compare(0, 3, "rgb") == 0) {
    size_t o = s.find('(');
    double v[4] = {255, 255, 255, 1};
    if (o != std::string::npos) {
      const char* p = s.c_str() + o + 1;
      for (int i = 0; i < 4 && *p && *p != ')'; i++) { char* e; v[i] = strtod(p, &e); p = *e == ',' ? e + 1 : e; }
    }
    r = (int)v[0]; g = (int)v[1]; b = (int)v[2];
    if (v[3] == 0) none = true;
  }
  last = s;
  have = true;
  return lastV = none ? -1 : (int32_t)G->color(std::max(0, std::min(255, r)), std::max(0, std::min(255, g)), std::max(0, std::min(255, b)));
}
static std::string prop(Interp& I, const char* k) { return I.toStr(I.get(Value::obj(ctxObj), k)); }
static int32_t styleColor(Interp& I, const char* k) { return colorOf(prop(I, k)); }
static double lineWidth(Interp& I) { double w = I.toNum(I.get(Value::obj(ctxObj), "lineWidth")); return w > 0 ? w : 1; }

// ---- the path
static void start(double x, double y) {
  path.emplace_back();
  cur = (int)path.size() - 1;
  path[cur].p = {(float)x, (float)y};
}
static int segs(double r, double sweep) { return std::max(6, std::min(64, (int)std::ceil(std::fabs(sweep) * r / 5))); }
static void mat(Sub& s) {  // a full circle is kept as a centre and radius until something needs its outline
  if (s.kind == 'c' && s.p.size() < 4) {
    double R = s.k[2];
    int n = segs(R, kTau);
    s.p.clear();
    for (int i = 0; i <= n; i++) {
      double a = s.k[3] + kTau * i / n;
      s.p.push_back((float)(s.k[0] + s.k[4] * R * std::cos(a)));
      s.p.push_back((float)(s.k[1] + s.k[5] * R * std::sin(a)));
    }
  }
}
static void add(double x, double y) {
  if (cur < 0) { start(x, y); return; }
  Sub& s = path[cur];
  mat(s);
  s.p.push_back((float)x);
  s.p.push_back((float)y);
  s.kind = 0;
}
static void ell(double x, double y, double rx, double ry, double rot, double a0, double a1, bool ccw) {
  double sweep = a1 - a0;
  if (ccw ? sweep <= -kTau : sweep >= kTau) sweep = ccw ? -kTau : kTau;
  else {
    sweep = std::fmod(sweep, kTau);
    if (!ccw && sweep < 0) sweep += kTau;
    else if (ccw && sweep > 0) sweep -= kTau;
  }
  if (cur < 0 && std::fabs(sweep) >= kTau - 1e-6 && rx == ry && simple() && std::fabs(M.a) == std::fabs(M.d)) {
    start(tx(x, y), ty(x, y));  // no outline built: fill and stroke draw a circle
    Sub& s = path[cur];
    s.kind = 'c';
    s.k[0] = s.p[0]; s.k[1] = s.p[1]; s.k[2] = (float)(rx * std::fabs(M.a)); s.k[3] = (float)a0; s.k[4] = M.a < 0 ? -1 : 1; s.k[5] = M.d < 0 ? -1 : 1;
    return;
  }
  int n = segs(std::max(rx, ry) * sc(), sweep);
  double cr = std::cos(rot), sr = std::sin(rot);
  for (int i = 0; i <= n; i++) {
    double a = a0 + sweep * i / n, ex = rx * std::cos(a), ey = ry * std::sin(a);
    double lx = x + ex * cr - ey * sr, ly = y + ex * sr + ey * cr;
    add(tx(lx, ly), ty(lx, ly));
  }
}
static void curve(const double* q, int n) {  // a bezier from the current point; q: control point(s) then the end, screen space
  double x0 = q[0], y0 = q[1];
  if (cur >= 0) { mat(path[cur]); Sub& s = path[cur]; x0 = s.p[s.p.size() - 2]; y0 = s.p[s.p.size() - 1]; }
  for (int i = 1; i <= 10; i++) {
    double t = i / 10.0, u = 1 - t;
    if (n == 2) add(u * u * x0 + 2 * u * t * q[0] + t * t * q[2], u * u * y0 + 2 * u * t * q[1] + t * t * q[3]);
    else add(u * u * u * x0 + 3 * u * u * t * q[0] + 3 * u * t * t * q[2] + t * t * t * q[4], u * u * u * y0 + 3 * u * u * t * q[1] + 3 * u * t * t * q[3] + t * t * t * q[5]);
  }
}
static void rectPath(double x, double y, double w, double h) {
  start(tx(x, y), ty(x, y)); add(tx(x + w, y), ty(x + w, y)); add(tx(x + w, y + h), ty(x + w, y + h)); add(tx(x, y + h), ty(x, y + h));
  Sub& s = path[cur];
  s.closed = true;
  if (simple()) {
    s.kind = 'r';
    s.k[0] = (float)std::min(tx(x, y), tx(x + w, y + h)); s.k[1] = (float)std::min(ty(x, y), ty(x + w, y + h));
    s.k[2] = (float)std::fabs(w * M.a); s.k[3] = (float)std::fabs(h * M.d);
  }
  cur = -1;
}

// ---- filling
static bool convex(const std::vector<float>& p) {
  size_t n = p.size() / 2;
  int sign = 0;
  for (size_t i = 0; i < n; i++) {
    size_t j = (i + 1) % n, k = (i + 2) % n;
    double z = (p[2 * j] - p[2 * i]) * (p[2 * k + 1] - p[2 * j + 1]) - (p[2 * j + 1] - p[2 * i + 1]) * (p[2 * k] - p[2 * j]);
    if (std::fabs(z) < 1e-9) continue;
    if (sign && (z > 0) != (sign > 0)) return false;
    sign = z > 0 ? 1 : -1;
  }
  return n >= 3;
}
static bool ears(const std::vector<float>& p, uint16_t c) {  // a simple polygon as triangles; false if it isn't simple
  std::vector<float> q;
  for (size_t i = 0; i < p.size(); i += 2)
    if (q.empty() || p[i] != q[q.size() - 2] || p[i + 1] != q[q.size() - 1]) { q.push_back(p[i]); q.push_back(p[i + 1]); }
  int n = (int)q.size() / 2;
  if (n < 3) return true;
  std::vector<int> idx;
  double area = 0;
  for (int i = 0; i < n; i++) { idx.push_back(i); int j = (i + 1) % n; area += (double)q[2 * i] * q[2 * j + 1] - (double)q[2 * j] * q[2 * i + 1]; }
  double dir = area > 0 ? 1 : -1;
  auto cr = [&](int a, int b, int d) { return ((q[2 * b] - q[2 * a]) * (q[2 * d + 1] - q[2 * b + 1]) - (q[2 * b + 1] - q[2 * a + 1]) * (q[2 * d] - q[2 * b])) * dir; };
  auto tri = [&](int a, int b, int d) { G->fillTriangle(rd(q[2 * a]), rd(q[2 * a + 1]), rd(q[2 * b]), rd(q[2 * b + 1]), rd(q[2 * d]), rd(q[2 * d + 1]), c); };
  while (idx.size() > 3) {
    bool cut = false;
    for (size_t a = 0; a < idx.size() && !cut; a++) {
      int i0 = idx[(a + idx.size() - 1) % idx.size()], i1 = idx[a], i2 = idx[(a + 1) % idx.size()];
      if (cr(i0, i1, i2) <= 0) continue;
      bool freeEar = true;
      for (int v : idx)
        if (v != i0 && v != i1 && v != i2 && cr(i0, i1, v) >= 0 && cr(i1, i2, v) >= 0 && cr(i2, i0, v) >= 0) { freeEar = false; break; }
      if (freeEar) { tri(i0, i1, i2); idx.erase(idx.begin() + a); cut = true; }
    }
    if (!cut) return false;
  }
  tri(idx[0], idx[1], idx[2]);
  return true;
}
static void fillPath(int32_t c) {
  if (c < 0) return;
  uint16_t col = (uint16_t)c;
  if (path.size() == 1 && path[0].kind) {
    const float* k = path[0].k;
    if (path[0].kind == 'r') G->fillRect(rd(k[0]), rd(k[1]), rd(k[2]), rd(k[3]), col);
    else G->fillCircle(rd(k[0]), rd(k[1]), rd(k[2]), col);
    return;
  }
  for (Sub& s : path) mat(s);
  if (path.size() == 1) {
    const std::vector<float>& p = path[0].p;
    size_t n = p.size() / 2;
    if (convex(p)) {
      for (size_t i = 1; i + 1 < n; i++) G->fillTriangle(rd(p[0]), rd(p[1]), rd(p[2 * i]), rd(p[2 * i + 1]), rd(p[2 * i + 2]), rd(p[2 * i + 3]), col);
      return;
    }
    if (n <= 48 && ears(p, col)) return;
  }
  struct Edge { float top, bot, xt, sl; };
  std::vector<Edge> E;
  float y0 = 1e9f, y1 = -1e9f;
  for (Sub& s : path) {
    size_t n = s.p.size() / 2;
    if (n < 3) continue;
    for (size_t i = 0; i < n; i++) {
      size_t j = (i + 1) % n;
      float ax = s.p[2 * i], ay = s.p[2 * i + 1], bx = s.p[2 * j], by = s.p[2 * j + 1];
      if (ay == by) continue;
      bool up = ay < by;
      E.push_back({up ? ay : by, up ? by : ay, up ? ax : bx, (bx - ax) / (by - ay)});
      y0 = std::min(y0, std::min(ay, by));
      y1 = std::max(y1, std::max(ay, by));
    }
  }
  int W = G->width(), H = G->height();
  std::vector<float> xs;
  for (int y = std::max(0, (int)std::floor(y0)); y <= std::min(H - 1, (int)std::ceil(y1)); y++) {
    float yy = y + 0.5f;
    xs.clear();
    for (const Edge& e : E)
      if (yy >= e.top && yy < e.bot) xs.push_back(e.xt + (yy - e.top) * e.sl);
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2) {
      int a = std::max(0, rd(xs[i])), b = std::min(W, rd(xs[i + 1]));
      if (b > a) G->hline(a, y, b - a, col);
    }
  }
}
static void seg(double ax, double ay, double bx, double by, double w, uint16_t c) {  // 1 px: a line; thicker: a filled quad
  if (w <= 1.5) { G->line(rd(ax), rd(ay), rd(bx), rd(by), c); return; }
  double dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
  if (len == 0) len = 1;
  double ox = -dy / len * w / 2, oy = dx / len * w / 2;
  G->fillTriangle(rd(ax + ox), rd(ay + oy), rd(ax - ox), rd(ay - oy), rd(bx + ox), rd(by + oy), c);
  G->fillTriangle(rd(ax - ox), rd(ay - oy), rd(bx - ox), rd(by - oy), rd(bx + ox), rd(by + oy), c);
}
static void strokePath(int32_t c, double lw) {
  if (c < 0) return;
  uint16_t col = (uint16_t)c;
  double w = lw * sc();
  if (path.size() == 1 && path[0].kind) {
    const float* k = path[0].k;
    if (path[0].kind == 'r' && w <= 1.5) { G->drawRect(rd(k[0]), rd(k[1]), rd(k[2]), rd(k[3]), col); return; }
    if (path[0].kind == 'c' && w <= 8) {  // a ring: circles side by side
      int cnt = std::max(1, rd(w));
      for (int i = 0; i < cnt; i++) G->drawCircle(rd(k[0]), rd(k[1]), rd(k[2] - (cnt - 1) / 2.0 + i), col);
      return;
    }
  }
  for (Sub& s : path) {
    mat(s);
    size_t n = s.p.size() / 2;
    for (size_t i = 0; i < (s.closed ? n : (n ? n - 1 : 0)); i++) {
      size_t j = (i + 1) % n;
      seg(s.p[2 * i], s.p[2 * i + 1], s.p[2 * j], s.p[2 * j + 1], w, col);
      if (w >= 3 && n <= 16) G->fillCircle(rd(s.p[2 * j]), rd(s.p[2 * j + 1]), rd(w / 2), col);  // round the corners
    }
  }
}
template <class F> static void onRect(double x, double y, double w, double h, F fn) {  // a rectangle through the path code
  std::vector<Sub> keep;
  keep.swap(path);
  int keepCur = cur;
  cur = -1;
  rectPath(x, y, w, h);
  fn();
  path.swap(keep);
  cur = keepCur;
}

// ---- text
static int fontNo(Interp& I) {
  std::string f = prop(I, "font");
  int px = 10, num = 0;
  size_t i = 0;
  while (i < f.size()) {
    size_t j = f.find(' ', i);
    if (j == std::string::npos) j = f.size();
    std::string t = f.substr(i, j - i);
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "px") == 0) px = atoi(t.c_str());
    else if (t.size() == 1 && t[0] >= '0' && t[0] <= '9') num = t[0] - '0';
    i = j + 1;
  }
  return num ? num : px <= 9 ? 1 : px <= 18 ? 2 : 4;
}
static void text(Interp& I, const std::string& s, double x, double y, int32_t c) {
  if (c < 0) return;
  std::string a = prop(I, "textAlign"), b = prop(I, "textBaseline");
  int row = (b == "top" || b == "hanging") ? 0 : b == "middle" ? 1 : 2, col = a == "center" ? 1 : (a == "right" || a == "end") ? 2 : 0;
  G->setText((uint16_t)c, row * 3 + col);
  G->drawText(s.c_str(), rd(tx(x, y)), rd(ty(x, y)), fontNo(I));
}

// ---- the context's methods
#define NUM(i) I.toNum(JS_ARG(i))
static Value cx_fillRect(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double x = NUM(0), y = NUM(1), w = NUM(2), h = NUM(3);
  int32_t c = styleColor(I, "fillStyle");
  if (simple()) {
    if (c >= 0) G->fillRect(rd(std::min(tx(x, y), tx(x + w, y + h))), rd(std::min(ty(x, y), ty(x + w, y + h))), rd(std::fabs(w * M.a)), rd(std::fabs(h * M.d)), (uint16_t)c);
  } else onRect(x, y, w, h, [&] { fillPath(c); });
  return Value();
}
static Value cx_strokeRect(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  int32_t c = styleColor(I, "strokeStyle");
  double lw = lineWidth(I);
  onRect(NUM(0), NUM(1), NUM(2), NUM(3), [&] { strokePath(c, lw); });
  return Value();
}
static Value cx_clearRect(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double x = NUM(0), y = NUM(1), w = NUM(2), h = NUM(3);
  int32_t c = colorOf(I.toStr(I.get(Value::obj(canvasObj), "background")));
  if (c < 0) return Value();
  if (simple()) G->fillRect(rd(std::min(tx(x, y), tx(x + w, y + h))), rd(std::min(ty(x, y), ty(x + w, y + h))), rd(std::fabs(w * M.a)), rd(std::fabs(h * M.d)), (uint16_t)c);
  else onRect(x, y, w, h, [&] { fillPath(c); });
  return Value();
}
static Value cx_beginPath(Interp& I, Value self, Value* args, int argc) { (void)I; (void)self; (void)args; (void)argc; path.clear(); cur = -1; return Value(); }
static Value cx_closePath(Interp& I, Value self, Value* args, int argc) {
  (void)I; (void)self; (void)args; (void)argc;
  if (cur >= 0) { mat(path[cur]); path[cur].closed = true; path[cur].kind = 0; cur = -1; }
  return Value();
}
static Value cx_moveTo(Interp& I, Value self, Value* args, int argc) { (void)self; double x = NUM(0), y = NUM(1); start(tx(x, y), ty(x, y)); return Value(); }
static Value cx_lineTo(Interp& I, Value self, Value* args, int argc) { (void)self; double x = NUM(0), y = NUM(1); add(tx(x, y), ty(x, y)); return Value(); }
static Value cx_arc(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  ell(NUM(0), NUM(1), NUM(2), NUM(2), 0, NUM(3), NUM(4), I.truthy(JS_ARG(5)));
  return Value();
}
static Value cx_ellipse(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  ell(NUM(0), NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), NUM(6), I.truthy(JS_ARG(7)));
  return Value();
}
static Value cx_rect(Interp& I, Value self, Value* args, int argc) { (void)self; rectPath(NUM(0), NUM(1), NUM(2), NUM(3)); return Value(); }
static Value cx_roundRect(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double x = NUM(0), y = NUM(1), w = NUM(2), h = NUM(3), r = argc > 4 ? NUM(4) : 0;
  r = std::max(0.0, std::min(r, std::min(std::fabs(w), std::fabs(h)) / 2));
  start(tx(x + r, y), ty(x + r, y));
  add(tx(x + w - r, y), ty(x + w - r, y));
  ell(x + w - r, y + r, r, r, 0, -kPi / 2, 0, false);
  add(tx(x + w, y + h - r), ty(x + w, y + h - r));
  ell(x + w - r, y + h - r, r, r, 0, 0, kPi / 2, false);
  add(tx(x + r, y + h), ty(x + r, y + h));
  ell(x + r, y + h - r, r, r, 0, kPi / 2, kPi, false);
  add(tx(x, y + r), ty(x, y + r));
  ell(x + r, y + r, r, r, 0, kPi, kPi * 1.5, false);
  if (cur >= 0) { path[cur].closed = true; path[cur].kind = 0; cur = -1; }
  return Value();
}
static Value cx_quadraticCurveTo(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double cxp = NUM(0), cyp = NUM(1), x = NUM(2), y = NUM(3);
  double q[4] = {tx(cxp, cyp), ty(cxp, cyp), tx(x, y), ty(x, y)};
  curve(q, 2);
  return Value();
}
static Value cx_bezierCurveTo(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double a = NUM(0), b = NUM(1), c = NUM(2), d = NUM(3), x = NUM(4), y = NUM(5);
  double q[6] = {tx(a, b), ty(a, b), tx(c, d), ty(c, d), tx(x, y), ty(x, y)};
  curve(q, 3);
  return Value();
}
static Value cx_fill(Interp& I, Value self, Value* args, int argc) { (void)self; (void)args; (void)argc; fillPath(styleColor(I, "fillStyle")); return Value(); }
static Value cx_stroke(Interp& I, Value self, Value* args, int argc) { (void)self; (void)args; (void)argc; strokePath(styleColor(I, "strokeStyle"), lineWidth(I)); return Value(); }
static Value cx_fillText(Interp& I, Value self, Value* args, int argc) { (void)self; text(I, I.toStr(JS_ARG(0)), NUM(1), NUM(2), styleColor(I, "fillStyle")); return Value(); }
static Value cx_strokeText(Interp& I, Value self, Value* args, int argc) { (void)self; text(I, I.toStr(JS_ARG(0)), NUM(1), NUM(2), styleColor(I, "strokeStyle")); return Value(); }
static Value cx_measureText(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  Object* o = I.newObject();
  I.setOwn(o, "width", Value::num(G->textWidth(I.toStr(JS_ARG(0)).c_str(), fontNo(I))));
  return Value::obj(o);
}
static Value cx_drawImage(Interp& I, Value self, Value* args, int argc) {  // drawImage(path, x, y): lcd.drawImage straight to the screen
  (void)self;
  if (!JS_ARG(0).isStr()) return Value();
  Value lcd = I.getGlobal("lcd");
  Value fn = I.get(lcd, "drawImage");
  if (!I.isCallable(fn)) return Value();
  Value a[3] = {args[0], Value::num(rd(tx(NUM(1), NUM(2)))), Value::num(rd(ty(NUM(1), NUM(2))))};
  I.call(fn, lcd, a, 3);
  return Value();
}
static Value cx_save(Interp& I, Value self, Value* args, int argc) {
  (void)self; (void)args; (void)argc;
  stack.push_back({M, prop(I, "fillStyle"), prop(I, "strokeStyle"), prop(I, "font"), prop(I, "textAlign"), prop(I, "textBaseline"), lineWidth(I)});
  return Value();
}
static Value cx_restore(Interp& I, Value self, Value* args, int argc) {
  (void)self; (void)args; (void)argc;
  if (stack.empty()) return Value();
  Saved s = stack.back();
  stack.pop_back();
  M = s.m;
  I.setOwn(ctxObj, "fillStyle", I.str(s.fs));
  I.setOwn(ctxObj, "strokeStyle", I.str(s.ss));
  I.setOwn(ctxObj, "font", I.str(s.font));
  I.setOwn(ctxObj, "textAlign", I.str(s.ta));
  I.setOwn(ctxObj, "textBaseline", I.str(s.tb));
  I.setOwn(ctxObj, "lineWidth", Value::num(s.lw));
  return Value();
}
static Value cx_translate(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double x = NUM(0), y = NUM(1);
  M.e += M.a * x + M.c * y;
  M.f += M.b * x + M.d * y;
  return Value();
}
static Value cx_scale(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double x = NUM(0), y = NUM(1);
  M.a *= x; M.b *= x; M.c *= y; M.d *= y;
  return Value();
}
static Value cx_rotate(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double a = NUM(0), c = std::cos(a), s = std::sin(a);
  Mat n = M;
  M.a = n.a * c + n.c * s; M.b = n.b * c + n.d * s; M.c = n.c * c - n.a * s; M.d = n.d * c - n.b * s;
  return Value();
}
static Value cx_transform(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  double a = NUM(0), b = NUM(1), c = NUM(2), d = NUM(3), e = NUM(4), f = NUM(5);
  Mat n = M;
  M.a = n.a * a + n.c * b; M.b = n.b * a + n.d * b; M.c = n.a * c + n.c * d; M.d = n.b * c + n.d * d;
  M.e = n.a * e + n.c * f + n.e; M.f = n.b * e + n.d * f + n.f;
  return Value();
}
static Value cx_setTransform(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  M.a = NUM(0); M.b = NUM(1); M.c = NUM(2); M.d = NUM(3); M.e = NUM(4); M.f = NUM(5);
  return Value();
}
static Value cx_resetTransform(Interp& I, Value self, Value* args, int argc) { (void)I; (void)self; (void)args; (void)argc; M = Mat(); return Value(); }

static const NativeEntry LAZY_ctx[] = {
  {"fillRect", cx_fillRect}, {"strokeRect", cx_strokeRect}, {"clearRect", cx_clearRect}, {"beginPath", cx_beginPath}, {"closePath", cx_closePath},
  {"moveTo", cx_moveTo}, {"lineTo", cx_lineTo}, {"arc", cx_arc}, {"ellipse", cx_ellipse}, {"rect", cx_rect}, {"roundRect", cx_roundRect},
  {"quadraticCurveTo", cx_quadraticCurveTo}, {"bezierCurveTo", cx_bezierCurveTo}, {"fill", cx_fill}, {"stroke", cx_stroke},
  {"fillText", cx_fillText}, {"strokeText", cx_strokeText}, {"measureText", cx_measureText}, {"drawImage", cx_drawImage}, {"save", cx_save},
  {"restore", cx_restore}, {"translate", cx_translate}, {"scale", cx_scale}, {"rotate", cx_rotate}, {"transform", cx_transform},
  {"setTransform", cx_setTransform}, {"resetTransform", cx_resetTransform}, {nullptr, nullptr}};

// ---- the canvas object
static Value cv_getContext(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  if (I.toStr(JS_ARG(0)) != "2d") return Value::null();
  if (!ctxObj) {
    ctxObj = I.newObject();
    I.roots.push_back(Value::obj(ctxObj));
    ctxObj->lazy = LAZY_ctx;
    I.setOwn(ctxObj, "canvas", Value::obj(canvasObj));
    I.setOwn(ctxObj, "fillStyle", I.str("#000000"));
    I.setOwn(ctxObj, "strokeStyle", I.str("#000000"));
    I.setOwn(ctxObj, "lineWidth", Value::num(1));
    I.setOwn(ctxObj, "font", I.str("10px sans-serif"));
    I.setOwn(ctxObj, "textAlign", I.str("start"));
    I.setOwn(ctxObj, "textBaseline", I.str("alphabetic"));
    I.setOwn(ctxObj, "globalAlpha", Value::num(1));
  }
  return Value::obj(ctxObj);
}
static Value cv_buffer(Interp& I, Value self, Value* args, int argc) {
  (void)self;
  return Value::boolean(G->buffer(argc == 0 || I.truthy(args[0])));
}
static Value cv_flush(Interp& I, Value self, Value* args, int argc) { (void)I; (void)self; (void)args; (void)argc; G->flush(); return Value(); }
static const NativeEntry LAZY_canvas[] = {{"getContext", cv_getContext}, {"buffer", cv_buffer}, {"flush", cv_flush}, {nullptr, nullptr}};

// call once per interpreter: the global `canvas` (a new run starts from a clean state and the screen)
static void install(Interp& I, Gfx* g) {
  G = g;
  G->buffer(false);
  M = Mat();
  path.clear();
  cur = -1;
  stack.clear();
  ctxObj = nullptr;
  canvasObj = I.newObject();
  canvasObj->lazy = LAZY_canvas;
  I.setOwn(canvasObj, "width", Value::num(G->width()));
  I.setOwn(canvasObj, "height", Value::num(G->height()));
  I.setOwn(canvasObj, "background", I.str("#000000"));
  I.setOwn(canvasObj, "native", Value::boolean(true));
  I.setGlobal("canvas", Value::obj(canvasObj));
}
#undef NUM
}  // namespace cvs
