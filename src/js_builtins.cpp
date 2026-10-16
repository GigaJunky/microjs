// js_builtins.cpp - standard library: Object, Array, String, Number, Math, JSON, Map, Set, timers...
#include "js.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <algorithm>
#include <ctime>

namespace js {

#define NATIVE JS_NATIVE
#define ARG JS_ARG

static int digitVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

static Array* thisArray(Interp& I, Value self, const char* m) {
  if (!self.isObj() || self.o()->kind != K::Array)
    I.throwError("TypeError", std::string("Array.prototype.") + m + " called on a non-array");
  return (Array*)self.o();
}

static std::string thisStr(Interp& I, Value self) {
  if (self.isStr()) return self.s()->s;
  if (self.isNullish()) I.throwError("TypeError", "String method called on null or undefined");
  return I.toStr(self);
}

static double thisNum(Interp& I, Value self) {
  if (!self.isNum()) I.throwError("TypeError", "Number method called on a non-number");
  return self.n;
}

static double argNum(Interp& I, Value* args, int argc, int i, double def) {
  return i < argc && !args[i].isUndef() ? I.toNum(args[i]) : def;
}

// clamp a relative index (negative counts from the end) into [0, len]
static size_t relIndex(double d, size_t len) {
  if (std::isnan(d)) return 0;
  d = std::trunc(d);
  if (d < 0) return d + (double)len < 0 ? 0 : (size_t)(d + (double)len);
  return d > (double)len ? len : (size_t)d;
}

static Value needFn(Interp& I, Value f, const char* what) {
  if (!I.isCallable(f)) I.throwError("TypeError", I.toStr(f) + " is not a function (in " + what + ")");
  return f;
}

static bool sameValueZero(Interp& I, Value a, Value b) {
  if (a.isNum() && b.isNum()) return a.n == b.n || (std::isnan(a.n) && std::isnan(b.n));
  return I.strictEq(a, b);
}

static bool isError(Interp& I, Value v) {
  if (!v.isObj()) return false;
  for (Object* o = v.o(); o; o = o->proto)
    if (o == I.ErrorProto) return true;
  return false;
}

// =================================================================== inspect
static std::string quoteStr(const std::string& s, char q) {
  std::string r(1, q);
  for (unsigned char c : s) {
    switch (c) {
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\\': r += "\\\\"; break;
      default:
        if (c == (unsigned char)q) { r += '\\'; r += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\x%02x", c); r += b; }
        else r += (char)c;
    }
  }
  r += q;
  return r;
}

static bool isIdent(const std::string& k) {
  if (k.empty() || isdigit((unsigned char)k[0])) return false;
  for (unsigned char c : k)
    if (!isalnum(c) && c != '_' && c != '$') return false;
  return true;
}

static std::string inspectRec(Interp& I, Value v, int depth, std::vector<Object*>& seen);

static std::string joinParts(const std::vector<std::string>& parts, const std::string& open, const std::string& close,
                             int depth) {
  if (parts.empty()) return open + close;
  size_t total = open.size() + close.size();
  bool multi = false;
  for (auto& p : parts) {
    total += p.size() + 2;
    if (p.find('\n') != std::string::npos) multi = true;
  }
  if (total > 72) multi = true;
  std::string r = open;
  if (multi) {
    std::string ind((depth + 1) * 2, ' ');
    r += "\n";
    for (size_t i = 0; i < parts.size(); i++) {
      r += ind + parts[i];
      if (i + 1 < parts.size()) r += ",";
      r += "\n";
    }
    r += std::string(depth * 2, ' ') + close;
  } else {
    r += " ";
    for (size_t i = 0; i < parts.size(); i++) {
      if (i) r += ", ";
      r += parts[i];
    }
    r += " " + close;
  }
  return r;
}

static std::string inspectRec(Interp& I, Value v, int depth, std::vector<Object*>& seen) {
  switch (v.t) {
    case T::Undef: return "undefined";
    case T::Null: return "null";
    case T::Bool: return v.b ? "true" : "false";
    case T::Num: return numToStr(v.n);
    case T::Str: return quoteStr(v.s()->s, '\'');
    case T::Obj: break;
  }
  Object* o = v.o();
  if (o->kind == K::Function) {
    Function* f = (Function*)o;
    if (f->isClass) return std::string("[class ") + (f->name ? f->name : "(anonymous)") + "]";
    return f->name ? std::string("[Function: ") + f->name + "]" : "[Function (anonymous)]";
  }
  if (o->kind == K::Native) return std::string("[Function: ") + ((Native*)o)->name + "]";
  if (isError(I, v)) return I.errorString(v);
  if (std::find(seen.begin(), seen.end(), o) != seen.end()) return "[Circular]";
  if (depth > 2) return o->kind == K::Array ? "[Array]" : "[Object]";
  seen.push_back(o);
  std::vector<std::string> parts;
  std::string open = "{", close = "}";
  if (o->kind == K::Array) {
    auto& it = ((Array*)o)->items;
    size_t n = std::min<size_t>(it.size(), 100);
    for (size_t i = 0; i < n; i++) parts.push_back(inspectRec(I, it[i], depth + 1, seen));
    if (it.size() > n) parts.push_back("... " + std::to_string(it.size() - n) + " more items");
    open = "[";
    close = "]";
  } else if (o->kind == K::Map || o->kind == K::Set) {
    auto& en = ((MapObj*)o)->entries;
    for (auto& e : en) {
      if (o->kind == K::Map)
        parts.push_back(inspectRec(I, e.first, depth + 1, seen) + " => " + inspectRec(I, e.second, depth + 1, seen));
      else parts.push_back(inspectRec(I, e.first, depth + 1, seen));
    }
    open = (o->kind == K::Map ? "Map(" : "Set(") + std::to_string(en.size()) + ") {";
  } else if (o->proto && o->proto != I.ObjectProto) {
    Prop* c = o->proto->own("constructor");
    if (c && c->v.isObj() && c->v.o()->kind == K::Function && ((Function*)c->v.o())->name)
      open = std::string(((Function*)c->v.o())->name) + " {";
  }
  for (auto& p : o->props) {
    std::string k = isIdent(p.k) ? p.k : quoteStr(p.k, '\'');
    if (p.accessor) {
      auto& gs = ((Array*)p.v.o())->items;
      parts.push_back(k + ": " + (gs[1].isUndef() ? "[Getter]" : gs[0].isUndef() ? "[Setter]" : "[Getter/Setter]"));
    }
    else parts.push_back(k + ": " + inspectRec(I, p.v, depth + 1, seen));
  }
  seen.pop_back();
  return joinParts(parts, open, close, depth);
}

std::string Interp::inspect(Value v, int depth) {
  std::vector<Object*> seen;
  return inspectRec(*this, v, depth, seen);
}

std::string Interp::errorString(Value v) {
  if (v.isStr()) return v.s()->s;
  if (isError(*this, v)) {
    try {
      std::string name = toStr(get(v, "name"));
      std::string msg = toStr(get(v, "message"));
      return msg.empty() ? name : name + ": " + msg;
    } catch (JsThrow&) {
      return "Error";
    }
  }
  if (v.isObj()) return inspect(v);
  return toStr(v);
}

// =================================================================== console & globals
NATIVE(console_log) {
  (void)self;
  std::string out;
  for (int i = 0; i < argc; i++) {
    if (i) out += ' ';
    out += args[i].isStr() ? args[i].s()->s : I.inspect(args[i]);
  }
  out += '\n';
  I.print(out);
  return Value();
}

NATIVE(g_parseInt) {
  (void)self;
  std::string s = I.toStr(ARG(0));
  int radix = argc > 1 && !args[1].isUndef() ? I.toInt32(args[1]) : 0;
  size_t i = 0;
  while (i < s.size() && isspace((unsigned char)s[i])) i++;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
  if ((radix == 0 || radix == 16) && i + 1 < s.size() && s[i] == '0' && (s[i + 1] | 32) == 'x') {
    i += 2;
    radix = 16;
  }
  if (radix == 0) radix = 10;
  if (radix < 2 || radix > 36) return Value::num(NAN);
  double v = 0;
  size_t start = i;
  for (; i < s.size(); i++) {
    int d = digitVal(s[i]);
    if (d >= radix) break;
    v = v * radix + d;
  }
  if (i == start) return Value::num(NAN);
  return Value::num(neg ? -v : v);
}

NATIVE(g_parseFloat) {
  (void)self;
  std::string s = I.toStr(ARG(0));
  size_t i = 0;
  while (i < s.size() && isspace((unsigned char)s[i])) i++;
  size_t st = i;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
  if (s.compare(i, 8, "Infinity") == 0) return Value::num(s[st] == '-' ? -INFINITY : INFINITY);
  bool digits = false;
  while (i < s.size() && isdigit((unsigned char)s[i])) { i++; digits = true; }
  if (i < s.size() && s[i] == '.') {
    i++;
    while (i < s.size() && isdigit((unsigned char)s[i])) { i++; digits = true; }
  }
  if (!digits) return Value::num(NAN);
  if (i < s.size() && (s[i] | 32) == 'e') {
    size_t j = i + 1;
    if (j < s.size() && (s[j] == '+' || s[j] == '-')) j++;
    if (j < s.size() && isdigit((unsigned char)s[j])) {
      while (j < s.size() && isdigit((unsigned char)s[j])) j++;
      i = j;
    }
  }
  return Value::num(strtod(s.substr(st, i - st).c_str(), nullptr));
}

NATIVE(g_isNaN) { (void)self; return Value::boolean(std::isnan(I.toNum(ARG(0)))); }
NATIVE(g_isFinite) { (void)self; return Value::boolean(std::isfinite(I.toNum(ARG(0)))); }
NATIVE(g_millis) { (void)self; (void)args; (void)argc; return Value::num(I.millis ? I.millis() : 0); }
NATIVE(g_gc) { (void)self; (void)args; (void)argc; I.gcPending = true; return Value(); }

NATIVE(g_load) {
  (void)self;
  std::string path = I.toStr(ARG(0));
  std::string src;
  if (!I.readFile || !I.readFile(path, src)) I.throwError("Error", "cannot read file '" + path + "'");
  return I.run(src, path.c_str());
}

static Value addTimer(Interp& I, Value* args, int argc, bool repeat) {
  needFn(I, ARG(0), repeat ? "setInterval" : "setTimeout");
  double ms = argNum(I, args, argc, 1, 0);
  if (!(ms >= 0)) ms = 0;
  Timer t;
  t.id = I.nextTimerId++;
  t.interval = (uint32_t)ms;
  t.when = (I.millis ? I.millis() : 0) + t.interval;
  t.repeat = repeat;
  t.fn = args[0];
  for (int i = 2; i < argc; i++) t.args.push_back(args[i]);
  I.timers.push_back(t);
  return Value::num(t.id);
}
NATIVE(g_setTimeout) { (void)self; return addTimer(I, args, argc, false); }
NATIVE(g_setInterval) { (void)self; return addTimer(I, args, argc, true); }
NATIVE(g_clearTimer) {
  (void)self;
  int id = I.toInt32(ARG(0));
  for (size_t i = 0; i < I.timers.size(); i++)
    if (I.timers[i].id == id) { I.timers.erase(I.timers.begin() + i); break; }
  return Value();
}

// =================================================================== Object
NATIVE(object_ctor) {
  (void)self;
  if (argc && ARG(0).isObj()) return args[0];
  return Value::obj(I.newObject());
}
NATIVE(object_keys) {
  (void)self;
  Array* a = I.newArray();
  for (auto& k : I.ownKeys(ARG(0))) a->items.push_back(I.str(k));
  return Value::obj(a);
}
NATIVE(object_values) {
  (void)self;
  Value o = ARG(0);
  Array* a = I.newArray();
  for (auto& k : I.ownKeys(o)) a->items.push_back(I.getProp(o, k.data(), k.size()));
  return Value::obj(a);
}
NATIVE(object_entries) {
  (void)self;
  Value o = ARG(0);
  Array* a = I.newArray();
  for (auto& k : I.ownKeys(o)) {
    Array* p = I.newArray();
    p->items.push_back(I.str(k));
    p->items.push_back(I.getProp(o, k.data(), k.size()));
    a->items.push_back(Value::obj(p));
  }
  return Value::obj(a);
}
NATIVE(object_fromEntries) {
  (void)self;
  Object* o = I.newObject();
  for (auto& e : I.toList(ARG(0))) {
    std::string k = I.toKey(I.getIndex(e, Value::num(0)));
    I.setOwn(o, k.c_str(), k.size(), I.getIndex(e, Value::num(1)));
  }
  return Value::obj(o);
}
NATIVE(object_assign) {
  (void)self;
  Value t = ARG(0);
  if (!t.isObj()) I.throwError("TypeError", "Object.assign target must be an object");
  for (int i = 1; i < argc; i++) {
    if (args[i].isNullish()) continue;
    for (auto& k : I.ownKeys(args[i])) I.putProp(t, k.data(), k.size(), I.getProp(args[i], k.data(), k.size()));
  }
  return t;
}
NATIVE(object_create) {
  (void)self;
  Value p = ARG(0);
  if (!p.isObj() && !p.isNull()) I.throwError("TypeError", "Object prototype may only be an Object or null");
  return Value::obj(I.newObject(p.isObj() ? p.o() : nullptr));
}
NATIVE(object_getProto) {
  (void)self;
  Value v = ARG(0);
  Object* p = nullptr;
  if (v.isObj()) p = v.o()->proto;
  else if (v.isStr()) p = I.StringProto;
  else if (v.isNum()) p = I.NumberProto;
  else if (v.isBool()) p = I.BooleanProto;
  return p ? Value::obj(p) : Value::null();
}
NATIVE(object_setProto) {
  (void)self;
  if (ARG(0).isObj()) args[0].o()->proto = ARG(1).isObj() ? args[1].o() : nullptr;
  return ARG(0);
}
NATIVE(object_freeze) { (void)self; (void)I; return ARG(0); }
NATIVE(object_defineProperty) {
  (void)self;
  Value o = ARG(0), d = ARG(2);
  if (!o.isObj() || !d.isObj()) I.throwError("TypeError", "Object.defineProperty called on non-object");
  std::string k = I.toKey(ARG(1));
  Value g = I.get(d, "get"), s = I.get(d, "set");
  if (!g.isUndef() || !s.isUndef()) {
    if (!g.isUndef()) I.defineAccessor(o.o(), k, g, false);
    if (!s.isUndef()) I.defineAccessor(o.o(), k, s, true);
  } else I.putProp(o, k.data(), k.size(), I.get(d, "value"));
  return o;
}
NATIVE(objproto_hasOwn) {
  std::string k = I.toKey(ARG(0));
  if (!self.isObj()) return Value::boolean(self.isStr() && I.hasProp(I.toStrV(self), k));
  Object* o = self.o();
  if (o->kind == K::Array) {
    char* end;
    unsigned long i = strtoul(k.c_str(), &end, 10);
    if (!*end && !k.empty()) return Value::boolean(i < ((Array*)o)->items.size());
    if (k == "length") return Value::boolean(true);
  }
  return Value::boolean(o->own(k.data(), k.size()) != nullptr);
}
NATIVE(object_hasOwn) {
  (void)self;
  return objproto_hasOwn(I, ARG(0), args + (argc > 0), argc > 0 ? argc - 1 : 0);
}
NATIVE(objproto_toString) {
  (void)I; (void)args; (void)argc;
  if (self.isObj() && self.o()->kind == K::Array) return I.str("[object Array]");
  return I.str(self.isNull() ? "[object Null]" : self.isUndef() ? "[object Undefined]" : "[object Object]");
}
NATIVE(objproto_valueOf) { (void)I; (void)args; (void)argc; return self; }
NATIVE(objproto_isPrototypeOf) {
  (void)argc;
  if (!self.isObj() || !ARG(0).isObj()) return Value::boolean(false);
  for (Object* o = args[0].o()->proto; o; o = o->proto)
    if (o == self.o()) return Value::boolean(true);
  (void)I;
  return Value::boolean(false);
}

// =================================================================== Function
NATIVE(fn_call) { return I.call(self, ARG(0), args + (argc > 0), argc > 0 ? argc - 1 : 0); }
NATIVE(fn_apply) {
  std::vector<Value> a;
  if (argc > 1 && !args[1].isNullish()) a = I.toList(args[1]);
  return I.call(self, ARG(0), a.data(), (int)a.size());
}
NATIVE(bound_call) {
  (void)self;
  Array* d = (Array*)I.curNative->data.o();
  std::vector<Value> a(d->items.begin() + 2, d->items.end());
  a.insert(a.end(), args, args + argc);
  Value fn = d->items[0], th = d->items[1];
  return I.call(fn, th, a.data(), (int)a.size());
}
NATIVE(fn_bind) {
  if (!I.isCallable(self)) I.throwError("TypeError", "Bind must be called on a function");
  Array* d = I.newArray();
  d->items.push_back(self);
  d->items.push_back(ARG(0));
  for (int i = 1; i < argc; i++) d->items.push_back(args[i]);
  return Value::obj(I.newNative(bound_call, "bound", Value::obj(d)));
}
NATIVE(fn_toString) {
  (void)args; (void)argc;
  std::string name = I.toStr(I.get(self, "name"));
  return I.str("function " + name + "() { [code] }");
}

// =================================================================== Array
NATIVE(array_ctor) {
  (void)self;
  Array* a = I.newArray();
  if (argc == 1 && args[0].isNum()) {
    double n = args[0].n;
    if (n < 0 || n != std::floor(n) || n > 10000000) I.throwError("RangeError", "Invalid array length");
    a->items.resize((size_t)n);
  } else a->items.assign(args, args + argc);
  return Value::obj(a);
}
NATIVE(array_isArray) { (void)self; (void)I; return Value::boolean(ARG(0).isObj() && args[0].o()->kind == K::Array); }
NATIVE(array_of) { (void)self; Array* a = I.newArray(); a->items.assign(args, args + argc); return Value::obj(a); }
NATIVE(array_from) {
  (void)self;
  Value src = ARG(0), fn = ARG(1);
  Array* a = I.newArray();
  if (src.isObj() && src.o()->kind == K::Plain) {
    double n = I.toNum(I.get(src, "length"));
    size_t len = std::isnan(n) || n < 0 ? 0 : (size_t)std::min(n, 10000000.0);
    for (size_t i = 0; i < len; i++) a->items.push_back(I.getIndex(src, Value::num((double)i)));
  } else if (!src.isNullish()) {
    auto list = I.toList(src);
    a->items.assign(list.begin(), list.end());
  }
  if (!fn.isUndef()) {
    needFn(I, fn, "Array.from");
    size_t m = I.temps.size();
    for (size_t i = 0; i < a->items.size(); i++) {
      Value cb[2] = {a->items[i], Value::num((double)i)};
      a->items[i] = I.call(fn, Value(), cb, 2);
      I.temps.resize(m);
    }
  }
  return Value::obj(a);
}
NATIVE(array_push) {
  Array* a = thisArray(I, self, "push");
  for (int i = 0; i < argc; i++) a->items.push_back(args[i]);
  return Value::num((double)a->items.size());
}
NATIVE(array_pop) {
  (void)args; (void)argc;
  Array* a = thisArray(I, self, "pop");
  if (a->items.empty()) return Value();
  Value v = a->items.back();
  a->items.pop_back();
  return v;
}
NATIVE(array_shift) {
  (void)args; (void)argc;
  Array* a = thisArray(I, self, "shift");
  if (a->items.empty()) return Value();
  Value v = a->items.front();
  a->items.erase(a->items.begin());
  return v;
}
NATIVE(array_unshift) {
  Array* a = thisArray(I, self, "unshift");
  a->items.insert(a->items.begin(), args, args + argc);
  return Value::num((double)a->items.size());
}
NATIVE(array_slice) {
  Array* a = thisArray(I, self, "slice");
  size_t len = a->items.size();
  size_t b = relIndex(argNum(I, args, argc, 0, 0), len);
  size_t e = relIndex(argNum(I, args, argc, 1, (double)len), len);
  Array* r = I.newArray();
  if (b < e) r->items.assign(a->items.begin() + b, a->items.begin() + e);
  return Value::obj(r);
}
NATIVE(array_splice) {
  Array* a = thisArray(I, self, "splice");
  size_t len = a->items.size();
  size_t start = relIndex(argNum(I, args, argc, 0, 0), len);
  size_t del = argc < 2 ? len - start : relIndex(std::max(0.0, argNum(I, args, argc, 1, 0)), len - start);
  Array* r = I.newArray();
  r->items.assign(a->items.begin() + start, a->items.begin() + start + del);
  a->items.erase(a->items.begin() + start, a->items.begin() + start + del);
  if (argc > 2) a->items.insert(a->items.begin() + start, args + 2, args + argc);
  return Value::obj(r);
}
NATIVE(array_concat) {
  Array* a = thisArray(I, self, "concat");
  Array* r = I.newArray();
  r->items = a->items;
  for (int i = 0; i < argc; i++) {
    if (args[i].isObj() && args[i].o()->kind == K::Array) {
      auto& src = ((Array*)args[i].o())->items;
      r->items.insert(r->items.end(), src.begin(), src.end());
    } else r->items.push_back(args[i]);
  }
  return Value::obj(r);
}
NATIVE(array_join) {
  Array* a = thisArray(I, self, "join");
  std::string sep = argc > 0 && !args[0].isUndef() ? I.toStr(args[0]) : ",";
  std::string r;
  for (size_t i = 0; i < a->items.size(); i++) {
    if (i) r += sep;
    Value v = a->items[i];
    if (!v.isNullish()) r += v.isStr() ? v.s()->s : I.toStr(v);
  }
  return I.str(r);
}
NATIVE(array_toString) { (void)args; (void)argc; return array_join(I, self, nullptr, 0); }
NATIVE(array_reverse) {
  (void)args; (void)argc;
  Array* a = thisArray(I, self, "reverse");
  std::reverse(a->items.begin(), a->items.end());
  return self;
}
NATIVE(array_indexOf) {
  Array* a = thisArray(I, self, "indexOf");
  size_t from = relIndex(argNum(I, args, argc, 1, 0), a->items.size());
  for (size_t i = from; i < a->items.size(); i++)
    if (I.strictEq(a->items[i], ARG(0))) return Value::num((double)i);
  return Value::num(-1);
}
NATIVE(array_lastIndexOf) {  // lastIndexOf(value, [from]); a negative from counts from the end
  Array* a = thisArray(I, self, "lastIndexOf");
  double n = (double)a->items.size(), start = n - 1;
  if (argc > 1) {
    double f = I.toNum(args[1]);
    start = std::isnan(f) ? 0 : f < 0 ? n + f : std::min(f, n - 1);
  }
  for (double i = std::floor(start); i >= 0; i--)
    if (I.strictEq(a->items[(size_t)i], ARG(0))) return Value::num(i);
  return Value::num(-1);
}
NATIVE(array_includes) {
  Array* a = thisArray(I, self, "includes");
  for (auto& v : a->items)
    if (sameValueZero(I, v, ARG(0))) return Value::boolean(true);
  return Value::boolean(false);
}

// iterate calling fn(item, index, array); `mode` decides what to do with results
enum { IT_EACH, IT_MAP, IT_FILTER, IT_FIND, IT_FINDIDX, IT_SOME, IT_EVERY, IT_FINDLAST, IT_FINDLASTIDX };
static Value iterate(Interp& I, Value self, Value* args, int argc, int mode, const char* name) {
  Array* a = thisArray(I, self, name);
  Value fn = needFn(I, ARG(0), name), th = ARG(1);
  Array* r = (mode == IT_MAP || mode == IT_FILTER) ? I.newArray() : nullptr;
  size_t m = I.temps.size();
  bool rev = mode == IT_FINDLAST || mode == IT_FINDLASTIDX;
  size_t n = a->items.size();
  for (size_t k = 0; k < n && k < a->items.size(); k++) {
    size_t i = rev ? n - 1 - k : k;
    if (i >= a->items.size()) continue;
    Value item = a->items[i];
    Value cb[3] = {item, Value::num((double)i), self};
    Value res = I.call(fn, th, cb, 3);
    switch (mode) {
      case IT_MAP: r->items.push_back(res); break;
      case IT_FILTER: if (I.truthy(res)) r->items.push_back(item); break;
      case IT_FIND: case IT_FINDLAST: if (I.truthy(res)) return item; break;
      case IT_FINDIDX: case IT_FINDLASTIDX: if (I.truthy(res)) return Value::num((double)i); break;
      case IT_SOME: if (I.truthy(res)) return Value::boolean(true); break;
      case IT_EVERY: if (!I.truthy(res)) return Value::boolean(false); break;
    }
    I.temps.resize(m);
  }
  switch (mode) {
    case IT_MAP: case IT_FILTER: return Value::obj(r);
    case IT_FINDIDX: case IT_FINDLASTIDX: return Value::num(-1);
    case IT_SOME: return Value::boolean(false);
    case IT_EVERY: return Value::boolean(true);
    default: return Value();
  }
}
NATIVE(array_forEach) { return iterate(I, self, args, argc, IT_EACH, "forEach"); }
NATIVE(array_map) { return iterate(I, self, args, argc, IT_MAP, "map"); }
NATIVE(array_filter) { return iterate(I, self, args, argc, IT_FILTER, "filter"); }
NATIVE(array_find) { return iterate(I, self, args, argc, IT_FIND, "find"); }
NATIVE(array_findIndex) { return iterate(I, self, args, argc, IT_FINDIDX, "findIndex"); }
NATIVE(array_findLast) { return iterate(I, self, args, argc, IT_FINDLAST, "findLast"); }
NATIVE(array_findLastIndex) { return iterate(I, self, args, argc, IT_FINDLASTIDX, "findLastIndex"); }
NATIVE(array_some) { return iterate(I, self, args, argc, IT_SOME, "some"); }
NATIVE(array_every) { return iterate(I, self, args, argc, IT_EVERY, "every"); }

static Value reduce(Interp& I, Value self, Value* args, int argc, bool right) {
  Array* a = thisArray(I, self, right ? "reduceRight" : "reduce");
  Value fn = needFn(I, ARG(0), "reduce");
  size_t n = a->items.size();
  size_t k = 0;
  Value acc;
  if (argc > 1) acc = args[1];
  else {
    if (n == 0) I.throwError("TypeError", "Reduce of empty array with no initial value");
    acc = a->items[right ? n - 1 : 0];
    k = 1;
  }
  size_t m = I.temps.size();
  for (; k < n && k < a->items.size(); k++) {
    size_t i = right ? n - 1 - k : k;
    Value cb[4] = {acc, a->items[i], Value::num((double)i), self};
    acc = I.call(fn, Value(), cb, 4);
    I.temps.resize(m);
    I.rootTemp(acc);
  }
  return acc;
}
NATIVE(array_reduce) { return reduce(I, self, args, argc, false); }
NATIVE(array_reduceRight) { return reduce(I, self, args, argc, true); }

NATIVE(array_sort) {
  Array* a = thisArray(I, self, "sort");
  Value cmp = ARG(0);
  if (!cmp.isUndef()) needFn(I, cmp, "sort");
  std::vector<Value> v(a->items.begin(), a->items.end());
  size_t n = v.size();
  std::vector<Value> tmp(n);
  size_t m = I.temps.size();
  auto greater = [&](Value x, Value y) -> bool {  // should x come after y?
    if (x.isUndef()) return !y.isUndef();
    if (y.isUndef()) return false;
    if (!cmp.isUndef()) {
      Value cb[2] = {x, y};
      double r = I.toNum(I.call(cmp, Value(), cb, 2));
      I.temps.resize(m);
      return r > 0;
    }
    if (x.isStr() && y.isStr()) return x.s()->s > y.s()->s;
    return I.toStr(x) > I.toStr(y);
  };
  for (size_t w = 1; w < n; w *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * w) {
      size_t mid = std::min(lo + w, n), hi = std::min(lo + 2 * w, n);
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) tmp[k++] = greater(v[i], v[j]) ? v[j++] : v[i++];
      while (i < mid) tmp[k++] = v[i++];
      while (j < hi) tmp[k++] = v[j++];
    }
    v.swap(tmp);
  }
  a->items.assign(v.begin(), v.end());
  return self;
}
NATIVE(array_fill) {
  Array* a = thisArray(I, self, "fill");
  size_t len = a->items.size();
  size_t b = relIndex(argNum(I, args, argc, 1, 0), len), e = relIndex(argNum(I, args, argc, 2, (double)len), len);
  for (size_t i = b; i < e; i++) a->items[i] = ARG(0);
  return self;
}
static void flatInto(Interp& I, PVec<Value>& out, const PVec<Value>& src, int depth) {
  for (auto& v : src) {
    if (depth > 0 && v.isObj() && v.o()->kind == K::Array) flatInto(I, out, ((Array*)v.o())->items, depth - 1);
    else out.push_back(v);
  }
}
NATIVE(array_flat) {
  Array* a = thisArray(I, self, "flat");
  Array* r = I.newArray();
  flatInto(I, r->items, a->items, (int)argNum(I, args, argc, 0, 1));
  return Value::obj(r);
}
NATIVE(array_flatMap) {
  Value mapped = iterate(I, self, args, argc, IT_MAP, "flatMap");
  Array* r = I.newArray();
  flatInto(I, r->items, ((Array*)mapped.o())->items, 1);
  return Value::obj(r);
}
NATIVE(array_at) {
  Array* a = thisArray(I, self, "at");
  double i = std::trunc(argNum(I, args, argc, 0, 0));
  if (i < 0) i += (double)a->items.size();
  return i >= 0 && i < (double)a->items.size() ? a->items[(size_t)i] : Value();
}
NATIVE(array_keys) {
  (void)args; (void)argc;
  Array* a = thisArray(I, self, "keys");
  Array* r = I.newArray();
  for (size_t i = 0; i < a->items.size(); i++) r->items.push_back(Value::num((double)i));
  return Value::obj(r);
}
NATIVE(array_values) {
  (void)args; (void)argc;
  Array* a = thisArray(I, self, "values");
  Array* r = I.newArray();
  r->items = a->items;
  return Value::obj(r);
}
NATIVE(array_entries) {
  (void)args; (void)argc;
  Array* a = thisArray(I, self, "entries");
  Array* r = I.newArray();
  for (size_t i = 0; i < a->items.size(); i++) {
    Array* p = I.newArray();
    p->items = {Value::num((double)i), a->items[i]};
    r->items.push_back(Value::obj(p));
  }
  return Value::obj(r);
}

// =================================================================== String
NATIVE(string_ctor) {
  (void)self;
  return argc ? I.toStrV(args[0]) : I.str("");
}
NATIVE(string_fromCharCode) {
  (void)self;
  std::string s;
  for (int i = 0; i < argc; i++) {
    uint32_t c = (uint32_t)I.toInt32(args[i]);
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 63)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
  }
  return I.str(s);
}
NATIVE(str_charAt) {
  std::string s = thisStr(I, self);
  double i = argNum(I, args, argc, 0, 0);
  if (i < 0 || i >= (double)s.size()) return I.str("");
  return I.charStr((unsigned char)s[(size_t)i]);
}
NATIVE(str_charCodeAt) {
  std::string s = thisStr(I, self);
  double i = argNum(I, args, argc, 0, 0);
  if (i < 0 || i >= (double)s.size()) return Value::num(NAN);
  return Value::num((unsigned char)s[(size_t)i]);
}
NATIVE(str_codePointAt) {
  std::string s = thisStr(I, self);
  double di = argNum(I, args, argc, 0, 0);
  if (di < 0 || di >= (double)s.size()) return Value();
  size_t i = (size_t)di;
  unsigned char c = s[i];
  if (c < 0x80) return Value::num(c);
  int len = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  uint32_t v = c & (0xFF >> (len + 1));
  for (int k = 1; k < len && i + k < s.size(); k++) v = (v << 6) | (s[i + k] & 63);
  return Value::num(v);
}
NATIVE(str_indexOf) {
  std::string s = thisStr(I, self), sub = I.toStr(ARG(0));
  size_t from = relIndex(std::max(0.0, argNum(I, args, argc, 1, 0)), s.size());
  size_t p = s.find(sub, from);
  return Value::num(p == std::string::npos ? -1 : (double)p);
}
NATIVE(str_lastIndexOf) {  // lastIndexOf(sub, [from]): the last match starting at or before from
  std::string s = thisStr(I, self), sub = I.toStr(ARG(0));
  size_t from = std::string::npos;
  if (argc > 1) {
    double f = I.toNum(args[1]);
    if (!std::isnan(f)) from = f < 0 ? 0 : (size_t)std::min(f, (double)s.size());
  }
  size_t p = s.rfind(sub, from);
  return Value::num(p == std::string::npos ? -1 : (double)p);
}
NATIVE(str_includes) {
  std::string s = thisStr(I, self), sub = I.toStr(ARG(0));
  size_t from = relIndex(std::max(0.0, argNum(I, args, argc, 1, 0)), s.size());
  return Value::boolean(s.find(sub, from) != std::string::npos);
}
NATIVE(str_startsWith) {
  std::string s = thisStr(I, self), sub = I.toStr(ARG(0));
  size_t pos = relIndex(std::max(0.0, argNum(I, args, argc, 1, 0)), s.size());
  return Value::boolean(s.compare(pos, sub.size(), sub) == 0);
}
NATIVE(str_endsWith) {
  std::string s = thisStr(I, self), sub = I.toStr(ARG(0));
  size_t end = relIndex(std::max(0.0, argNum(I, args, argc, 1, (double)s.size())), s.size());
  return Value::boolean(sub.size() <= end && s.compare(end - sub.size(), sub.size(), sub) == 0);
}
NATIVE(str_slice) {
  std::string s = thisStr(I, self);
  size_t b = relIndex(argNum(I, args, argc, 0, 0), s.size());
  size_t e = relIndex(argNum(I, args, argc, 1, (double)s.size()), s.size());
  return I.str(b < e ? s.substr(b, e - b) : "");
}
NATIVE(str_substring) {
  std::string s = thisStr(I, self);
  double fb = argNum(I, args, argc, 0, 0), fe = argNum(I, args, argc, 1, (double)s.size());
  size_t b = relIndex(std::max(0.0, std::isnan(fb) ? 0 : fb), s.size());
  size_t e = relIndex(std::max(0.0, std::isnan(fe) ? 0 : fe), s.size());
  if (b > e) std::swap(b, e);
  return I.str(s.substr(b, e - b));
}
NATIVE(str_substr) {
  std::string s = thisStr(I, self);
  size_t b = relIndex(argNum(I, args, argc, 0, 0), s.size());
  double len = argNum(I, args, argc, 1, (double)s.size());
  if (!(len > 0)) return I.str("");
  return I.str(s.substr(b, (size_t)std::min(len, (double)s.size())));
}
NATIVE(str_toUpperCase) {
  (void)args; (void)argc;
  std::string s = thisStr(I, self);
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  return I.str(s);
}
NATIVE(str_toLowerCase) {
  (void)args; (void)argc;
  std::string s = thisStr(I, self);
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return I.str(s);
}
static Value trimImpl(Interp& I, Value self, bool start, bool end) {
  std::string s = thisStr(I, self);
  size_t b = 0, e = s.size();
  if (start) while (b < e && isspace((unsigned char)s[b])) b++;
  if (end) while (e > b && isspace((unsigned char)s[e - 1])) e--;
  return I.str(s.substr(b, e - b));
}
NATIVE(str_trim) { (void)args; (void)argc; return trimImpl(I, self, true, true); }
NATIVE(str_trimStart) { (void)args; (void)argc; return trimImpl(I, self, true, false); }
NATIVE(str_trimEnd) { (void)args; (void)argc; return trimImpl(I, self, false, true); }
NATIVE(str_split) {
  std::string s = thisStr(I, self);
  Array* r = I.newArray();
  double lim = argNum(I, args, argc, 1, 4294967295.0);
  size_t limit = lim < 0 ? 0 : (size_t)lim;
  if (ARG(0).isUndef()) { r->items.push_back(I.str(s)); return Value::obj(r); }
  std::string sep = I.toStr(args[0]);
  if (sep.empty()) {
    for (size_t i = 0; i < s.size() && r->items.size() < limit; i++) r->items.push_back(I.charStr((unsigned char)s[i]));
    return Value::obj(r);
  }
  size_t pos = 0;
  while (r->items.size() < limit) {
    size_t p = s.find(sep, pos);
    if (p == std::string::npos) { r->items.push_back(I.str(s.substr(pos))); break; }
    r->items.push_back(I.str(s.substr(pos, p - pos)));
    pos = p + sep.size();
  }
  return Value::obj(r);
}
static Value replaceImpl(Interp& I, Value self, Value* args, int argc, bool all) {
  std::string s = thisStr(I, self);
  std::string pat = I.toStr(ARG(0));
  Value rep = ARG(1);
  bool fn = I.isCallable(rep);
  std::string repStr = fn ? "" : I.toStr(rep);
  std::string out;
  size_t pos = 0;
  for (;;) {
    size_t p = s.find(pat, pos);
    if (p == std::string::npos) break;
    out += s.substr(pos, p - pos);
    if (fn) {
      Value cb[3] = {I.str(pat), Value::num((double)p), self};
      out += I.toStr(I.call(rep, Value(), cb, 3));
    } else {
      for (size_t i = 0; i < repStr.size(); i++) {
        if (repStr[i] == '$' && i + 1 < repStr.size() && repStr[i + 1] == '&') { out += pat; i++; }
        else if (repStr[i] == '$' && i + 1 < repStr.size() && repStr[i + 1] == '$') { out += '$'; i++; }
        else out += repStr[i];
      }
    }
    pos = p + pat.size();
    if (pat.empty()) {
      if (pos < s.size()) out += s[pos];
      pos++;
      if (pos > s.size()) break;
    }
    if (!all) break;
  }
  if (pos < s.size()) out += s.substr(pos);
  return I.str(out);
}
NATIVE(str_replace) { return replaceImpl(I, self, args, argc, false); }
NATIVE(str_replaceAll) { return replaceImpl(I, self, args, argc, true); }
NATIVE(str_repeat) {
  std::string s = thisStr(I, self);
  double n = argNum(I, args, argc, 0, 0);
  if (n < 0 || std::isinf(n) || n * s.size() > 1e7) I.throwError("RangeError", "Invalid count value");
  std::string r;
  for (int i = 0; i < (int)n; i++) r += s;
  return I.str(r);
}
static Value padImpl(Interp& I, Value self, Value* args, int argc, bool start) {
  std::string s = thisStr(I, self);
  size_t len = (size_t)std::max(0.0, std::min(argNum(I, args, argc, 0, 0), 1e7));
  std::string fill = argc > 1 && !args[1].isUndef() ? I.toStr(args[1]) : " ";
  if (len <= s.size() || fill.empty()) return I.str(s);
  std::string pad;
  while (pad.size() < len - s.size()) pad += fill;
  pad.resize(len - s.size());
  return I.str(start ? pad + s : s + pad);
}
NATIVE(str_padStart) { return padImpl(I, self, args, argc, true); }
NATIVE(str_padEnd) { return padImpl(I, self, args, argc, false); }
NATIVE(str_concat) {
  std::string s = thisStr(I, self);
  for (int i = 0; i < argc; i++) s += I.toStr(args[i]);
  return I.str(s);
}
NATIVE(str_at) {
  std::string s = thisStr(I, self);
  double i = std::trunc(argNum(I, args, argc, 0, 0));
  if (i < 0) i += (double)s.size();
  return i >= 0 && i < (double)s.size() ? I.charStr((unsigned char)s[(size_t)i]) : Value();
}
NATIVE(str_toString) { (void)args; (void)argc; return I.toStrV(self); }
NATIVE(str_localeCompare) {
  std::string s = thisStr(I, self), o = I.toStr(ARG(0));
  int c = s.compare(o);
  return Value::num(c < 0 ? -1 : c > 0 ? 1 : 0);
}

// =================================================================== Number & Boolean
NATIVE(number_ctor) { (void)self; return Value::num(argc ? I.toNum(args[0]) : 0); }
NATIVE(boolean_ctor) { (void)self; return Value::boolean(argc ? I.truthy(args[0]) : false); }
NATIVE(number_isInteger) { (void)self; (void)I; return Value::boolean(ARG(0).isNum() && std::isfinite(args[0].n) && args[0].n == std::floor(args[0].n)); }
NATIVE(number_isSafeInteger) { (void)self; (void)I; return Value::boolean(ARG(0).isNum() && args[0].n == std::floor(args[0].n) && std::fabs(args[0].n) <= 9007199254740991.0); }
NATIVE(number_isFinite) { (void)self; (void)I; return Value::boolean(ARG(0).isNum() && std::isfinite(args[0].n)); }
NATIVE(number_isNaN) { (void)self; (void)I; return Value::boolean(ARG(0).isNum() && std::isnan(args[0].n)); }
NATIVE(num_toFixed) {
  double x = thisNum(I, self);
  int d = (int)argNum(I, args, argc, 0, 0);
  if (d < 0 || d > 100) I.throwError("RangeError", "toFixed() digits argument must be between 0 and 100");
  if (!std::isfinite(x) || std::fabs(x) >= 1e21) return I.str(numToStr(x));
  char buf[160];
  snprintf(buf, sizeof buf, "%.*f", d, x);
  if (!strncmp(buf, "-0", 2) && strspn(buf + 1, "0.") == strlen(buf + 1)) return I.str(buf + 1);  // -0.00 -> 0.00
  return I.str(buf);
}
NATIVE(num_toPrecision) {
  double x = thisNum(I, self);
  if (ARG(0).isUndef() || !std::isfinite(x)) return I.str(numToStr(x));
  int p = (int)I.toNum(args[0]);
  if (p < 1 || p > 100) I.throwError("RangeError", "toPrecision() argument must be between 1 and 100");
  char buf[160];
  snprintf(buf, sizeof buf, "%.*e", p - 1, x);
  int e = atoi(strchr(buf, 'e') + 1);
  if (e < -6 || e >= p) {
    std::string s(buf, strchr(buf, 'e'));
    return I.str(s + "e" + (e >= 0 ? "+" : "-") + std::to_string(std::abs(e)));
  }
  snprintf(buf, sizeof buf, "%.*f", p - 1 - e, x);
  return I.str(buf);
}
NATIVE(num_toString) {
  double x = thisNum(I, self);
  int radix = ARG(0).isUndef() ? 10 : I.toInt32(args[0]);
  if (radix < 2 || radix > 36) I.throwError("RangeError", "toString() radix must be between 2 and 36");
  if (radix == 10 || !std::isfinite(x)) return I.str(numToStr(x));
  const char* dig = "0123456789abcdefghijklmnopqrstuvwxyz";
  bool neg = x < 0;
  x = std::fabs(x);
  double ip = std::floor(x), fp = x - ip;
  std::string s;
  if (ip == 0) s = "0";
  while (ip >= 1) {
    s += dig[(int)std::fmod(ip, radix)];
    ip = std::floor(ip / radix);
  }
  std::reverse(s.begin(), s.end());
  if (fp > 0) {
    s += '.';
    for (int k = 0; k < 20 && fp > 0; k++) {
      fp *= radix;
      int d = (int)fp;
      s += dig[d];
      fp -= d;
    }
  }
  return I.str(neg ? "-" + s : s);
}
NATIVE(num_valueOf) { (void)args; (void)argc; return Value::num(thisNum(I, self)); }
NATIVE(bool_toString) { (void)args; (void)argc; return I.str(I.truthy(self) ? "true" : "false"); }

// =================================================================== Math
#define MATH1(name, expr) NATIVE(math_##name) { (void)self; double x = I.toNum(ARG(0)); (void)x; return Value::num(expr); }
MATH1(abs, std::fabs(x))
MATH1(floor, std::floor(x))
MATH1(ceil, std::ceil(x))
MATH1(round, std::floor(x + 0.5))
MATH1(trunc, std::trunc(x))
MATH1(sign, std::isnan(x) ? NAN : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(sqrt, std::sqrt(x))
MATH1(cbrt, std::cbrt(x))
MATH1(exp, std::exp(x))
MATH1(log, std::log(x))
MATH1(log2, std::log2(x))
MATH1(log10, std::log10(x))
MATH1(sin, std::sin(x))
MATH1(cos, std::cos(x))
MATH1(tan, std::tan(x))
MATH1(asin, std::asin(x))
MATH1(acos, std::acos(x))
MATH1(atan, std::atan(x))
MATH1(sinh, std::sinh(x))
MATH1(cosh, std::cosh(x))
MATH1(tanh, std::tanh(x))
MATH1(fround, (double)(float)x)
NATIVE(math_atan2) { (void)self; return Value::num(std::atan2(I.toNum(ARG(0)), I.toNum(ARG(1)))); }
NATIVE(math_pow) { (void)self; return Value::num(std::pow(I.toNum(ARG(0)), I.toNum(ARG(1)))); }
NATIVE(math_random) { (void)self; (void)args; (void)argc; return Value::num(I.random()); }
NATIVE(math_min) {
  (void)self;
  double r = INFINITY;
  for (int i = 0; i < argc; i++) {
    double x = I.toNum(args[i]);
    if (std::isnan(x)) return Value::num(NAN);
    if (x < r) r = x;
  }
  return Value::num(r);
}
NATIVE(math_max) {
  (void)self;
  double r = -INFINITY;
  for (int i = 0; i < argc; i++) {
    double x = I.toNum(args[i]);
    if (std::isnan(x)) return Value::num(NAN);
    if (x > r) r = x;
  }
  return Value::num(r);
}
NATIVE(math_hypot) {
  (void)self;
  double s = 0;
  for (int i = 0; i < argc; i++) { double x = I.toNum(args[i]); s += x * x; }
  return Value::num(std::sqrt(s));
}

// =================================================================== JSON
static void jsonQuote(const std::string& s, std::string& out) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += (char)c;
    }
  }
  out += '"';
}

static bool jsonStr(Interp& I, Value v, std::string& out, const std::string& gap, const std::string& ind,
                    std::vector<Object*>& stack) {
  if (v.isObj()) {
    Value tj = I.get(v, "toJSON");
    if (I.isCallable(tj)) v = I.call(tj, v, nullptr, 0);
  }
  switch (v.t) {
    case T::Undef: return false;
    case T::Null: out += "null"; return true;
    case T::Bool: out += v.b ? "true" : "false"; return true;
    case T::Num: out += std::isfinite(v.n) ? numToStr(v.n) : "null"; return true;
    case T::Str: jsonQuote(v.s()->s, out); return true;
    case T::Obj: break;
  }
  if (I.isCallable(v)) return false;
  Object* o = v.o();
  if (std::find(stack.begin(), stack.end(), o) != stack.end())
    I.throwError("TypeError", "Converting circular structure to JSON");
  stack.push_back(o);
  std::string inner = ind + gap;
  std::string sep = gap.empty() ? "," : ",\n" + inner;
  std::string open = gap.empty() ? "" : "\n" + inner;
  std::string close = gap.empty() ? "" : "\n" + ind;
  if (o->kind == K::Array) {
    auto& it = ((Array*)o)->items;
    out += '[';
    for (size_t i = 0; i < it.size(); i++) {
      out += i ? sep : open;
      if (!jsonStr(I, it[i], out, gap, inner, stack)) out += "null";
    }
    if (!it.empty()) out += close;
    out += ']';
  } else {
    out += '{';
    bool first = true;
    if (o->kind == K::Plain || o->kind == K::Function)
      for (size_t i = 0; i < o->props.size(); i++) {
        std::string k = o->props[i].k;
        Value pv = I.getProp(v, k.data(), k.size());
        std::string part;
        if (!jsonStr(I, pv, part, gap, inner, stack)) continue;
        out += first ? open : sep;
        first = false;
        jsonQuote(k, out);
        out += gap.empty() ? ":" : ": ";
        out += part;
      }
    if (!first) out += close;
    out += '}';
  }
  stack.pop_back();
  return true;
}

NATIVE(json_stringify) {
  (void)self;
  std::string gap;
  Value sp = ARG(2);
  if (sp.isNum()) gap = std::string((size_t)std::max(0.0, std::min(10.0, sp.n)), ' ');
  else if (sp.isStr()) gap = sp.s()->s.substr(0, 10);
  std::string out;
  std::vector<Object*> stack;
  if (!jsonStr(I, ARG(0), out, gap, "", stack)) return Value();
  return I.str(out);
}

struct JsonParser {
  Interp& I;
  const std::string& s;
  size_t i = 0;
  JsonParser(Interp& in, const std::string& src) : I(in), s(src) {}
  [[noreturn]] void fail() {
    if (i >= s.size()) I.throwError("SyntaxError", "Unexpected end of JSON input");
    I.throwError("SyntaxError", std::string("Unexpected token '") + s[i] + "' in JSON at position " + std::to_string(i));
  }
  void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if (s.compare(i, n, w) == 0) { i += n; return true; }
    return false;
  }
  std::string str() {
    i++;
    std::string r;
    for (;;) {
      if (i >= s.size()) fail();
      char c = s[i++];
      if (c == '"') break;
      if ((unsigned char)c < 0x20) { i--; fail(); }
      if (c != '\\') { r += c; continue; }
      if (i >= s.size()) fail();
      c = s[i++];
      switch (c) {
        case 'n': r += '\n'; break;
        case 't': r += '\t'; break;
        case 'r': r += '\r'; break;
        case 'b': r += '\b'; break;
        case 'f': r += '\f'; break;
        case 'u': {
          if (i + 4 > s.size()) fail();
          uint32_t v = 0;
          for (int k = 0; k < 4; k++) {
            int d = digitVal(s[i++]);
            if (d > 15) { i--; fail(); }
            v = v * 16 + d;
          }
          if (v >= 0xD800 && v < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            uint32_t lo = strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
            if (lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
          }
          if (v < 0x80) r += (char)v;
          else if (v < 0x800) { r += (char)(0xC0 | (v >> 6)); r += (char)(0x80 | (v & 63)); }
          else if (v < 0x10000) { r += (char)(0xE0 | (v >> 12)); r += (char)(0x80 | ((v >> 6) & 63)); r += (char)(0x80 | (v & 63)); }
          else { r += (char)(0xF0 | (v >> 18)); r += (char)(0x80 | ((v >> 12) & 63)); r += (char)(0x80 | ((v >> 6) & 63)); r += (char)(0x80 | (v & 63)); }
          break;
        }
        default: r += c;
      }
    }
    return r;
  }
  Value value(int depth) {
    if (depth > 64) I.throwError("SyntaxError", "JSON nested too deeply");
    ws();
    if (i >= s.size()) fail();
    char c = s[i];
    if (c == '{') {
      i++;
      Object* o = I.newObject();
      ws();
      if (i < s.size() && s[i] == '}') { i++; return Value::obj(o); }
      for (;;) {
        ws();
        if (i >= s.size() || s[i] != '"') fail();
        std::string k = str();
        ws();
        if (i >= s.size() || s[i] != ':') fail();
        i++;
        Value v = value(depth + 1);
        I.setOwn(o, k.c_str(), k.size(), v);
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == '}') { i++; return Value::obj(o); }
        fail();
      }
    }
    if (c == '[') {
      i++;
      Array* a = I.newArray();
      ws();
      if (i < s.size() && s[i] == ']') { i++; return Value::obj(a); }
      for (;;) {
        a->items.push_back(value(depth + 1));
        ws();
        if (i < s.size() && s[i] == ',') { i++; continue; }
        if (i < s.size() && s[i] == ']') { i++; return Value::obj(a); }
        fail();
      }
    }
    if (c == '"') return I.str(str());
    if (lit("true")) return Value::boolean(true);
    if (lit("false")) return Value::boolean(false);
    if (lit("null")) return Value::null();
    if (c == '-' || isdigit((unsigned char)c)) {
      size_t st = i;
      if (s[i] == '-') i++;
      if (i >= s.size() || !isdigit((unsigned char)s[i])) fail();
      while (i < s.size() && isdigit((unsigned char)s[i])) i++;
      if (i < s.size() && s[i] == '.') {
        i++;
        if (i >= s.size() || !isdigit((unsigned char)s[i])) fail();
        while (i < s.size() && isdigit((unsigned char)s[i])) i++;
      }
      if (i < s.size() && (s[i] | 32) == 'e') {
        i++;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) i++;
        if (i >= s.size() || !isdigit((unsigned char)s[i])) fail();
        while (i < s.size() && isdigit((unsigned char)s[i])) i++;
      }
      return Value::num(strtod(s.substr(st, i - st).c_str(), nullptr));
    }
    fail();
  }
};

NATIVE(json_parse) {
  (void)self;
  std::string src = I.toStr(ARG(0));
  JsonParser p(I, src);
  Value v = p.value(0);
  p.ws();
  if (p.i < src.size()) p.fail();
  return v;
}

// =================================================================== Map & Set
static MapObj* thisMap(Interp& I, Value self, K kind) {
  if (!self.isObj() || self.o()->kind != kind)
    I.throwError("TypeError", kind == K::Map ? "method called on incompatible receiver (expected Map)"
                                             : "method called on incompatible receiver (expected Set)");
  return (MapObj*)self.o();
}
static int mapFind(Interp& I, MapObj* m, Value k) {
  for (size_t i = 0; i < m->entries.size(); i++)
    if (sameValueZero(I, m->entries[i].first, k)) return (int)i;
  return -1;
}
static Value makeMap(Interp& I, Value* args, int argc, bool isSet) {
  MapObj* m = I.gcNew<MapObj>();
  m->kind = isSet ? K::Set : K::Map;
  m->proto = isSet ? I.SetProto : I.MapProto;
  if (argc && !args[0].isNullish()) {
    for (auto& e : I.toList(args[0])) {
      if (isSet) {
        if (mapFind(I, m, e) < 0) m->entries.push_back({e, Value()});
      } else {
        Value k = I.getIndex(e, Value::num(0)), v = I.getIndex(e, Value::num(1));
        int i = mapFind(I, m, k);
        if (i >= 0) m->entries[i].second = v;
        else m->entries.push_back({k, v});
      }
    }
  }
  return Value::obj(m);
}
NATIVE(map_ctor) { (void)self; return makeMap(I, args, argc, false); }
NATIVE(set_ctor) { (void)self; return makeMap(I, args, argc, true); }
NATIVE(map_get) {
  MapObj* m = thisMap(I, self, K::Map);
  int i = mapFind(I, m, ARG(0));
  return i >= 0 ? m->entries[i].second : Value();
}
NATIVE(map_set) {
  MapObj* m = thisMap(I, self, K::Map);
  int i = mapFind(I, m, ARG(0));
  if (i >= 0) m->entries[i].second = ARG(1);
  else m->entries.push_back({ARG(0), ARG(1)});
  return self;
}
NATIVE(set_add) {
  MapObj* m = thisMap(I, self, K::Set);
  if (mapFind(I, m, ARG(0)) < 0) m->entries.push_back({ARG(0), Value()});
  return self;
}
static MapObj* anyMap(Interp& I, Value self) {
  if (self.isObj() && (self.o()->kind == K::Map || self.o()->kind == K::Set)) return (MapObj*)self.o();
  I.throwError("TypeError", "method called on incompatible receiver");
}
NATIVE(map_has) { return Value::boolean(mapFind(I, anyMap(I, self), ARG(0)) >= 0); }
NATIVE(map_delete) {
  MapObj* m = anyMap(I, self);
  int i = mapFind(I, m, ARG(0));
  if (i < 0) return Value::boolean(false);
  m->entries.erase(m->entries.begin() + i);
  return Value::boolean(true);
}
NATIVE(map_clear) { (void)args; (void)argc; anyMap(I, self)->entries.clear(); return Value(); }
NATIVE(map_forEach) {
  MapObj* m = anyMap(I, self);
  Value fn = needFn(I, ARG(0), "forEach");
  bool isSet = m->kind == K::Set;
  size_t mark = I.temps.size();
  for (size_t i = 0; i < m->entries.size(); i++) {
    auto e = m->entries[i];
    Value cb[3] = {isSet ? e.first : e.second, e.first, self};
    I.call(fn, ARG(1), cb, 3);
    I.temps.resize(mark);
  }
  return Value();
}
static Value mapList(Interp& I, Value self, int which) {  // 0 keys, 1 values, 2 entries
  MapObj* m = anyMap(I, self);
  bool isSet = m->kind == K::Set;
  Array* r = I.newArray();
  for (auto& e : m->entries) {
    if (which == 0) r->items.push_back(e.first);
    else if (which == 1) r->items.push_back(isSet ? e.first : e.second);
    else {
      Array* p = I.newArray();
      p->items = {e.first, isSet ? e.first : e.second};
      r->items.push_back(Value::obj(p));
    }
  }
  return Value::obj(r);
}
NATIVE(map_keys) { (void)args; (void)argc; return mapList(I, self, 0); }
NATIVE(map_values) { (void)args; (void)argc; return mapList(I, self, 1); }
NATIVE(map_entries) { (void)args; (void)argc; return mapList(I, self, 2); }

// =================================================================== Error
NATIVE(error_ctor) {
  Object* proto = I.curNative->data.o();
  bool ctor = I.constructing;
  Object* o = (ctor && self.isObj()) ? self.o() : I.newObject(proto);
  if (argc > 0 && !args[0].isUndef()) I.setOwn(o, "message", I.toStrV(args[0]));
  return Value::obj(o);
}
NATIVE(error_toString) {
  (void)args; (void)argc;
  std::string name = I.toStr(I.get(self, "name")), msg = I.toStr(I.get(self, "message"));
  return I.str(msg.empty() ? name : name + ": " + msg);
}

NATIVE(date_now) { (void)self; (void)args; (void)argc; return Value::num(I.millis ? I.millis() : 0); }

// =================================================================== Date
// A Date keeps its time value (ms since 1970-01-01 UTC, or NaN) outside the property list, in the
// object's ext slot, so it doesn't show up in Object.keys or JSON. Local time follows the C
// library's time zone (TZ), which the board sets from utcOffsetHours in its config file.
static void dateFree(void* p) { free(p); }
static double& dateValue(Interp& I, Value v) {
  if (!v.isObj() || v.o()->finalize != dateFree || !v.o()->ext) I.throwError("TypeError", "this is not a Date");
  return *(double*)v.o()->ext;
}
static int64_t daysFromCivil(int64_t y, int m, int d) {  // m 1..12 -> days since 1970-01-01
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static void civilFromDays(int64_t z, int64_t& y, int& m, int& d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d = (int)(doy - (153 * mp + 2) / 5 + 1);
  m = (int)(mp < 10 ? mp + 3 : mp - 9);
  y = yoe + era * 400 + (m <= 2);
}
// ms from (possibly out-of-range) components, all treated as UTC; month 0-based
static double makeTime(double y, double mon, double day, double h, double mi, double sec, double ms) {
  double v[] = {y, mon, day, h, mi, sec, ms};
  for (double x : v) if (!std::isfinite(x)) return NAN;
  double ym = std::trunc(y) + std::floor(std::trunc(mon) / 12), mn = std::fmod(std::trunc(mon), 12);
  if (mn < 0) mn += 12;
  if (std::fabs(ym) > 400000) return NAN;
  double days = (double)daysFromCivil((int64_t)ym, (int)mn + 1, 1) + std::trunc(day) - 1;
  return days * 86400000.0 + std::trunc(h) * 3600000.0 + std::trunc(mi) * 60000.0 + std::trunc(sec) * 1000.0 + std::trunc(ms);
}
static double timeClip(double t) { return (!std::isfinite(t) || std::fabs(t) > 8.64e15) ? NAN : std::trunc(t) + 0.0; }
static double tzOffset(double utc) {  // local time minus UTC at that instant, in ms
  if (!std::isfinite(utc)) return 0;
  time_t t = (time_t)std::floor(utc / 1000);
  struct tm lt;
  if (!localtime_r(&t, &lt)) return 0;
  return makeTime(lt.tm_year + 1900.0, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, 0) - (double)t * 1000;
}
static double localToUtc(double local) { return local - tzOffset(local - tzOffset(local)); }
struct DateFields { double f[7]; int weekday; };  // year, month (0-11), day, hours, minutes, seconds, ms
static DateFields dateFields(double t) {
  DateFields r;
  double days = std::floor(t / 86400000.0), rem = t - days * 86400000.0;
  int64_t y; int m, d;
  civilFromDays((int64_t)days, y, m, d);
  r.f[0] = (double)y; r.f[1] = m - 1; r.f[2] = d;
  r.f[3] = std::floor(rem / 3600000.0); r.f[4] = std::floor(std::fmod(rem, 3600000.0) / 60000.0);
  r.f[5] = std::floor(std::fmod(rem, 60000.0) / 1000.0); r.f[6] = std::fmod(rem, 1000.0);
  r.weekday = (int)(((int64_t)days % 7 + 11) % 7);  // 1970-01-01 was a Thursday
  return r;
}
// ISO 8601 subset: YYYY[-MM[-DD]][(T| )HH:MM[:SS[.mmm]]][Z|+HH:MM|-HH:MM]. Date-only forms are UTC,
// date-times without an offset are local time (as in JavaScript).
static double parseDate(const std::string& str) {
  const char* p = str.c_str();
  while (*p == ' ') p++;
  auto num = [&](int n, double& out) {
    double v = 0;
    for (int i = 0; i < n; i++, p++) { if (!isdigit((unsigned char)*p)) return false; v = v * 10 + (*p - '0'); }
    out = v; return true;
  };
  double y = 0, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0, sign = 1;
  if (*p == '+' || *p == '-') { sign = *p == '-' ? -1 : 1; p++; if (!num(6, y)) return NAN; }
  else if (!num(4, y)) return NAN;
  y *= sign;
  bool hasTime = false, hasZone = false; double zone = 0;
  if (*p == '-') { p++; if (!num(2, mo)) return NAN; if (*p == '-') { p++; if (!num(2, d)) return NAN; } }
  if (*p == 'T' || *p == 't' || *p == ' ') {
    p++; hasTime = true;
    if (!num(2, h) || *p++ != ':' || !num(2, mi)) return NAN;
    if (*p == ':') { p++; if (!num(2, sec)) return NAN;
      if (*p == '.' || *p == ',') { p++; double frac = 0, scale = 100; while (isdigit((unsigned char)*p)) { frac += (*p++ - '0') * scale; scale /= 10; } ms = std::floor(frac); } }
  }
  if (*p == 'Z' || *p == 'z') { p++; hasZone = true; }
  else if (*p == '+' || *p == '-') {
    double zs = *p++ == '-' ? -1 : 1, zh, zm = 0;
    if (!num(2, zh)) return NAN;
    if (*p == ':') p++;
    if (isdigit((unsigned char)*p) && !num(2, zm)) return NAN;
    hasZone = true; zone = zs * (zh * 60 + zm) * 60000.0;
  }
  while (*p == ' ') p++;
  if (*p || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || sec > 59) return NAN;
  double t = makeTime(y, mo - 1, d, h, mi, sec, ms);
  if (hasZone) t -= zone;
  else if (hasTime) t = localToUtc(t);
  return timeClip(t);
}
static Value nowValue(Interp& I) {  // Date.now(), which the board/simulator may replace
  Value f = I.get(I.getGlobal("Date"), "now");
  return I.isCallable(f) ? I.call(f, Value(), nullptr, 0) : Value::num(I.millis ? I.millis() : 0);
}
static const char* const DAY_NAMES = "SunMonTueWedThuFriSat";
static const char* const MONTH_NAMES = "JanFebMarAprMayJunJulAugSepOctNovDec";
static std::string dateString(double t, int which) {  // 0 toString, 1 date, 2 time, 3 UTC, 4 ISO, 5-7 locale
  if (std::isnan(t)) return "Invalid Date";
  double off = (which == 3 || which == 4) ? 0 : tzOffset(t);
  DateFields f = dateFields(t + off);
  char buf[64], zone[16];
  int oz = (int)std::lround(off / 60000.0);
  snprintf(zone, sizeof zone, "GMT%c%02d%02d", oz < 0 ? '-' : '+', std::abs(oz) / 60, std::abs(oz) % 60);
  std::string day(DAY_NAMES + f.weekday * 3, 3), mon(MONTH_NAMES + (int)f.f[1] * 3, 3);
  int Y = (int)f.f[0], M = (int)f.f[1] + 1, D = (int)f.f[2], h = (int)f.f[3], mi = (int)f.f[4], sec = (int)f.f[5], ms = (int)f.f[6];
  int h12 = h % 12 == 0 ? 12 : h % 12;
  switch (which) {
    case 0: snprintf(buf, sizeof buf, "%s %s %02d %04d %02d:%02d:%02d %s", day.c_str(), mon.c_str(), D, Y, h, mi, sec, zone); break;
    case 1: snprintf(buf, sizeof buf, "%s %s %02d %04d", day.c_str(), mon.c_str(), D, Y); break;
    case 2: snprintf(buf, sizeof buf, "%02d:%02d:%02d %s", h, mi, sec, zone); break;
    case 3: snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", day.c_str(), D, mon.c_str(), Y, h, mi, sec); break;
    case 4:
      if (Y >= 0 && Y <= 9999) snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", Y, M, D, h, mi, sec, ms);
      else snprintf(buf, sizeof buf, "%c%06d-%02d-%02dT%02d:%02d:%02d.%03dZ", Y < 0 ? '-' : '+', std::abs(Y), M, D, h, mi, sec, ms);
      break;
    case 5: snprintf(buf, sizeof buf, "%d/%d/%d, %d:%02d:%02d %s", M, D, Y, h12, mi, sec, h < 12 ? "AM" : "PM"); break;
    case 6: snprintf(buf, sizeof buf, "%d/%d/%d", M, D, Y); break;
    default: snprintf(buf, sizeof buf, "%d:%02d:%02d %s", h12, mi, sec, h < 12 ? "AM" : "PM"); break;
  }
  return buf;
}
NATIVE(date_ctor) {
  double t;
  if (!I.constructing) return I.str(dateString(I.toNum(nowValue(I)), 0));  // Date() is a string
  if (argc == 0) t = I.toNum(nowValue(I));
  else if (argc == 1) {
    Value v = args[0];
    if (v.isObj() && v.o()->finalize == dateFree && v.o()->ext) t = *(double*)v.o()->ext;
    else {
      Value pv = I.toPrimitive(v, true);
      t = pv.isStr() ? parseDate(pv.s()->s) : I.toNum(pv);
    }
  } else {
    double c[7] = {0, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < argc && i < 7; i++) c[i] = I.toNum(args[i]);
    if (std::isfinite(c[0]) && std::trunc(c[0]) >= 0 && std::trunc(c[0]) <= 99) c[0] = 1900 + std::trunc(c[0]);
    t = localToUtc(makeTime(c[0], c[1], c[2], c[3], c[4], c[5], c[6]));
  }
  Object* o = self.isObj() ? self.o() : I.newObject(I.DateProto);
  double* slot = (double*)malloc(sizeof(double));
  if (!slot) I.throwError("Error", "out of memory");
  *slot = timeClip(t);
  o->ext = slot;
  o->finalize = dateFree;
  return Value::obj(o);
}
NATIVE(date_parse) { (void)self; return Value::num(parseDate(I.toStr(JS_ARG(0)))); }
NATIVE(date_UTC) {
  (void)self;
  double c[7] = {NAN, 0, 1, 0, 0, 0, 0};
  for (int i = 0; i < argc && i < 7; i++) c[i] = I.toNum(args[i]);
  if (std::isfinite(c[0]) && std::trunc(c[0]) >= 0 && std::trunc(c[0]) <= 99) c[0] = 1900 + std::trunc(c[0]);
  return Value::num(timeClip(makeTime(c[0], c[1], c[2], c[3], c[4], c[5], c[6])));
}
static Value dateGet(Interp& I, Value self, int field, bool utc) {
  double t = dateValue(I, self);
  if (std::isnan(t)) return Value::num(NAN);
  DateFields f = dateFields(utc ? t : t + tzOffset(t));
  return Value::num(field == 7 ? f.weekday : f.f[field]);
}
// setFullYear(y, m, d) from field 0, setMonth(m, d) from 1, ..., setMilliseconds(ms) from 6
static Value dateSet(Interp& I, Value self, Value* args, int argc, int first, int count, bool utc) {
  double& t = dateValue(I, self);
  double base = std::isnan(t) ? (first == 0 ? 0 : NAN) : t;
  if (std::isnan(base)) return Value::num(NAN);
  double off = utc ? 0 : tzOffset(base);
  DateFields f = dateFields(base + off);
  if (argc == 0) { t = NAN; return Value::num(NAN); }
  for (int i = 0; i < count && i < argc; i++) f.f[first + i] = I.toNum(args[i]);
  double v = makeTime(f.f[0], f.f[1], f.f[2], f.f[3], f.f[4], f.f[5], f.f[6]);
  t = timeClip(utc ? v : localToUtc(v));
  return Value::num(t);
}
#define DATE_GET(fn, field, utc) NATIVE(fn) { (void)args; (void)argc; return dateGet(I, self, field, utc); }
DATE_GET(date_getFullYear, 0, false) DATE_GET(date_getMonth, 1, false) DATE_GET(date_getDate, 2, false)
DATE_GET(date_getHours, 3, false) DATE_GET(date_getMinutes, 4, false) DATE_GET(date_getSeconds, 5, false)
DATE_GET(date_getMilliseconds, 6, false) DATE_GET(date_getDay, 7, false)
DATE_GET(date_getUTCFullYear, 0, true) DATE_GET(date_getUTCMonth, 1, true) DATE_GET(date_getUTCDate, 2, true)
DATE_GET(date_getUTCHours, 3, true) DATE_GET(date_getUTCMinutes, 4, true) DATE_GET(date_getUTCSeconds, 5, true)
DATE_GET(date_getUTCMilliseconds, 6, true) DATE_GET(date_getUTCDay, 7, true)
#define DATE_SET(fn, first, count, utc) NATIVE(fn) { return dateSet(I, self, args, argc, first, count, utc); }
DATE_SET(date_setFullYear, 0, 3, false) DATE_SET(date_setMonth, 1, 2, false) DATE_SET(date_setDate, 2, 1, false)
DATE_SET(date_setHours, 3, 4, false) DATE_SET(date_setMinutes, 4, 3, false) DATE_SET(date_setSeconds, 5, 2, false)
DATE_SET(date_setMilliseconds, 6, 1, false)
DATE_SET(date_setUTCFullYear, 0, 3, true) DATE_SET(date_setUTCMonth, 1, 2, true) DATE_SET(date_setUTCDate, 2, 1, true)
DATE_SET(date_setUTCHours, 3, 4, true) DATE_SET(date_setUTCMinutes, 4, 3, true) DATE_SET(date_setUTCSeconds, 5, 2, true)
DATE_SET(date_setUTCMilliseconds, 6, 1, true)
NATIVE(date_getTime) { (void)args; (void)argc; return Value::num(dateValue(I, self)); }
NATIVE(date_setTime) { double& t = dateValue(I, self); t = timeClip(I.toNum(JS_ARG(0))); return Value::num(t); }
NATIVE(date_getTimezoneOffset) {
  (void)args; (void)argc;
  double t = dateValue(I, self);
  return Value::num(std::isnan(t) ? NAN : -std::round(tzOffset(t) / 60000.0) + 0.0);
}
#define DATE_STR(fn, which) NATIVE(fn) { (void)args; (void)argc; return I.str(dateString(dateValue(I, self), which)); }
DATE_STR(date_toString, 0) DATE_STR(date_toDateString, 1) DATE_STR(date_toTimeString, 2) DATE_STR(date_toUTCString, 3)
DATE_STR(date_toLocaleString, 5) DATE_STR(date_toLocaleDateString, 6) DATE_STR(date_toLocaleTimeString, 7)
NATIVE(date_toISOString) {
  (void)args; (void)argc;
  double t = dateValue(I, self);
  if (std::isnan(t)) I.throwError("RangeError", "Invalid time value");
  return I.str(dateString(t, 4));
}
NATIVE(date_toJSON) {
  (void)args; (void)argc;
  double t = dateValue(I, self);
  return std::isnan(t) ? Value::null() : I.str(dateString(t, 4));
}

// =================================================================== setup
// Built-in methods: function objects are created on first use (see NativeEntry).
static const NativeEntry LAZY_ArrayProto[] = {{"push", array_push}, {"pop", array_pop}, {"shift", array_shift}, {"unshift", array_unshift}, {"slice", array_slice}, {"splice", array_splice}, {"concat", array_concat}, {"join", array_join}, {"toString", array_toString}, {"reverse", array_reverse}, {"indexOf", array_indexOf}, {"lastIndexOf", array_lastIndexOf}, {"includes", array_includes}, {"forEach", array_forEach}, {"map", array_map}, {"filter", array_filter}, {"find", array_find}, {"findIndex", array_findIndex}, {"findLast", array_findLast}, {"findLastIndex", array_findLastIndex}, {"some", array_some}, {"every", array_every}, {"reduce", array_reduce}, {"reduceRight", array_reduceRight}, {"sort", array_sort}, {"fill", array_fill}, {"flat", array_flat}, {"flatMap", array_flatMap}, {"at", array_at}, {"keys", array_keys}, {"values", array_values}, {"entries", array_entries}, {nullptr, nullptr}};
static const NativeEntry LAZY_StringProto[] = {{"charAt", str_charAt}, {"charCodeAt", str_charCodeAt}, {"codePointAt", str_codePointAt}, {"indexOf", str_indexOf}, {"lastIndexOf", str_lastIndexOf}, {"includes", str_includes}, {"startsWith", str_startsWith}, {"endsWith", str_endsWith}, {"slice", str_slice}, {"substring", str_substring}, {"substr", str_substr}, {"toUpperCase", str_toUpperCase}, {"toLowerCase", str_toLowerCase}, {"trim", str_trim}, {"trimStart", str_trimStart}, {"trimEnd", str_trimEnd}, {"split", str_split}, {"replace", str_replace}, {"replaceAll", str_replaceAll}, {"repeat", str_repeat}, {"padStart", str_padStart}, {"padEnd", str_padEnd}, {"concat", str_concat}, {"at", str_at}, {"toString", str_toString}, {"valueOf", str_toString}, {"localeCompare", str_localeCompare}, {nullptr, nullptr}};
static const NativeEntry LAZY_M[] = {{"abs", math_abs}, {"floor", math_floor}, {"ceil", math_ceil}, {"round", math_round}, {"trunc", math_trunc}, {"sign", math_sign}, {"sqrt", math_sqrt}, {"cbrt", math_cbrt}, {"exp", math_exp}, {"log", math_log}, {"log2", math_log2}, {"log10", math_log10}, {"sin", math_sin}, {"cos", math_cos}, {"tan", math_tan}, {"asin", math_asin}, {"acos", math_acos}, {"atan", math_atan}, {"sinh", math_sinh}, {"cosh", math_cosh}, {"tanh", math_tanh}, {"fround", math_fround}, {"atan2", math_atan2}, {"pow", math_pow}, {"random", math_random}, {"min", math_min}, {"max", math_max}, {"hypot", math_hypot}, {nullptr, nullptr}};
static const NativeEntry LAZY_O[] = {{"keys", object_keys}, {"getOwnPropertyNames", object_keys}, {"values", object_values}, {"entries", object_entries}, {"fromEntries", object_fromEntries}, {"assign", object_assign}, {"create", object_create}, {"getPrototypeOf", object_getProto}, {"setPrototypeOf", object_setProto}, {"freeze", object_freeze}, {"seal", object_freeze}, {"defineProperty", object_defineProperty}, {"hasOwn", object_hasOwn}, {nullptr, nullptr}};
static const NativeEntry LAZY_ObjectProto[] = {{"hasOwnProperty", objproto_hasOwn}, {"toString", objproto_toString}, {"valueOf", objproto_valueOf}, {"isPrototypeOf", objproto_isPrototypeOf}, {nullptr, nullptr}};
static const NativeEntry LAZY_A[] = {{"isArray", array_isArray}, {"from", array_from}, {"of", array_of}, {nullptr, nullptr}};
static const NativeEntry LAZY_S[] = {{"fromCharCode", string_fromCharCode}, {"fromCodePoint", string_fromCharCode}, {nullptr, nullptr}};
static const NativeEntry LAZY_N[] = {{"isInteger", number_isInteger}, {"isSafeInteger", number_isSafeInteger}, {"isFinite", number_isFinite}, {"isNaN", number_isNaN}, {"parseFloat", g_parseFloat}, {"parseInt", g_parseInt}, {nullptr, nullptr}};
static const NativeEntry LAZY_NumberProto[] = {{"toFixed", num_toFixed}, {"toPrecision", num_toPrecision}, {"toString", num_toString}, {"valueOf", num_valueOf}, {nullptr, nullptr}};
static const NativeEntry LAZY_J[] = {{"stringify", json_stringify}, {"parse", json_parse}, {nullptr, nullptr}};
static const NativeEntry LAZY_MapProto[] = {{"get", map_get}, {"set", map_set}, {"has", map_has}, {"delete", map_delete}, {"clear", map_clear}, {"forEach", map_forEach}, {"keys", map_keys}, {"values", map_values}, {"entries", map_entries}, {nullptr, nullptr}};
static const NativeEntry LAZY_SetProto[] = {{"add", set_add}, {"has", map_has}, {"delete", map_delete}, {"clear", map_clear}, {"forEach", map_forEach}, {"keys", map_keys}, {"values", map_values}, {"entries", map_entries}, {nullptr, nullptr}};
static const NativeEntry LAZY_DateProto[] = {{"getTime", date_getTime}, {"valueOf", date_getTime}, {"setTime", date_setTime}, {"getFullYear", date_getFullYear}, {"getMonth", date_getMonth}, {"getDate", date_getDate}, {"getDay", date_getDay}, {"getHours", date_getHours}, {"getMinutes", date_getMinutes}, {"getSeconds", date_getSeconds}, {"getMilliseconds", date_getMilliseconds}, {"getUTCFullYear", date_getUTCFullYear}, {"getUTCMonth", date_getUTCMonth}, {"getUTCDate", date_getUTCDate}, {"getUTCDay", date_getUTCDay}, {"getUTCHours", date_getUTCHours}, {"getUTCMinutes", date_getUTCMinutes}, {"getUTCSeconds", date_getUTCSeconds}, {"getUTCMilliseconds", date_getUTCMilliseconds}, {"setFullYear", date_setFullYear}, {"setMonth", date_setMonth}, {"setDate", date_setDate}, {"setHours", date_setHours}, {"setMinutes", date_setMinutes}, {"setSeconds", date_setSeconds}, {"setMilliseconds", date_setMilliseconds}, {"setUTCFullYear", date_setUTCFullYear}, {"setUTCMonth", date_setUTCMonth}, {"setUTCDate", date_setUTCDate}, {"setUTCHours", date_setUTCHours}, {"setUTCMinutes", date_setUTCMinutes}, {"setUTCSeconds", date_setUTCSeconds}, {"setUTCMilliseconds", date_setUTCMilliseconds}, {"getTimezoneOffset", date_getTimezoneOffset}, {"toString", date_toString}, {"toDateString", date_toDateString}, {"toTimeString", date_toTimeString}, {"toUTCString", date_toUTCString}, {"toGMTString", date_toUTCString}, {"toISOString", date_toISOString}, {"toJSON", date_toJSON}, {"toLocaleString", date_toLocaleString}, {"toLocaleDateString", date_toLocaleDateString}, {"toLocaleTimeString", date_toLocaleTimeString}, {nullptr, nullptr}};
static const NativeEntry LAZY_C[] = {{"log", console_log}, {"info", console_log}, {"warn", console_log}, {"error", console_log}, {"debug", console_log}, {nullptr, nullptr}};
static const NativeEntry LAZY_GLOBALS[] = {{"print", console_log}, {"parseInt", g_parseInt}, {"parseFloat", g_parseFloat}, {"isNaN", g_isNaN}, {"isFinite", g_isFinite}, {"setTimeout", g_setTimeout}, {"setInterval", g_setInterval}, {"clearTimeout", g_clearTimer}, {"clearInterval", g_clearTimer}, {"millis", g_millis}, {"load", g_load}, {"gc", g_gc}, {nullptr, nullptr}};

void Interp::setupBuiltins() {
  static const char* tn[] = {"undefined", "object", "boolean", "number", "string", "function"};
  for (int i = 0; i < 6; i++) typeStr[i] = allocStr(tn[i], strlen(tn[i]));

  ObjectProto = newObject(nullptr);
  FunctionProto = newObject(ObjectProto);
  ArrayProto = newObject(ObjectProto);
  StringProto = newObject(ObjectProto);
  NumberProto = newObject(ObjectProto);
  BooleanProto = newObject(ObjectProto);
  ErrorProto = newObject(ObjectProto);
  MapProto = newObject(ObjectProto);
  SetProto = newObject(ObjectProto);

  auto ctor = [&](const char* name, NativeFn fn, Object* proto) {
    Native* c = newNative(fn, name);
    if (proto) {
      setOwn(c, "prototype", Value::obj(proto));
      setOwn(proto, "constructor", Value::obj(c));
    }
    setGlobal(name, Value::obj(c));
    return c;
  };

  // Object
  Native* O = ctor("Object", object_ctor, ObjectProto);
  O->lazy = LAZY_O;
  ObjectProto->lazy = LAZY_ObjectProto;

  // Function
  setOwn(FunctionProto, "constructor", Value::obj(O));  // no Function constructor
  def(FunctionProto, "call", fn_call);
  def(FunctionProto, "apply", fn_apply);
  def(FunctionProto, "bind", fn_bind);
  def(FunctionProto, "toString", fn_toString);

  // Array
  Native* A = ctor("Array", array_ctor, ArrayProto);
  A->lazy = LAZY_A;
  ArrayProto->lazy = LAZY_ArrayProto;

  // String
  Native* S = ctor("String", string_ctor, StringProto);
  S->lazy = LAZY_S;
  StringProto->lazy = LAZY_StringProto;

  // Number & Boolean
  Native* N = ctor("Number", number_ctor, NumberProto);
  N->lazy = LAZY_N;
  setOwn(N, "MAX_SAFE_INTEGER", Value::num(9007199254740991.0));
  setOwn(N, "MIN_SAFE_INTEGER", Value::num(-9007199254740991.0));
  setOwn(N, "EPSILON", Value::num(2.220446049250313e-16));
  setOwn(N, "MAX_VALUE", Value::num(1.7976931348623157e308));
  setOwn(N, "MIN_VALUE", Value::num(5e-324));
  setOwn(N, "POSITIVE_INFINITY", Value::num(INFINITY));
  setOwn(N, "NEGATIVE_INFINITY", Value::num(-INFINITY));
  setOwn(N, "NaN", Value::num(NAN));
  NumberProto->lazy = LAZY_NumberProto;
  ctor("Boolean", boolean_ctor, BooleanProto);
  def(BooleanProto, "toString", bool_toString);

  // Math
  Object* M = newObject();
  setGlobal("Math", Value::obj(M));
  M->lazy = LAZY_M;
  setOwn(M, "PI", Value::num(3.141592653589793));
  setOwn(M, "E", Value::num(2.718281828459045));
  setOwn(M, "LN2", Value::num(0.6931471805599453));
  setOwn(M, "LN10", Value::num(2.302585092994046));
  setOwn(M, "LOG2E", Value::num(1.4426950408889634));
  setOwn(M, "LOG10E", Value::num(0.4342944819032518));
  setOwn(M, "SQRT2", Value::num(1.4142135623730951));
  setOwn(M, "SQRT1_2", Value::num(0.7071067811865476));

  // JSON
  Object* J = newObject();
  setGlobal("JSON", Value::obj(J));
  J->lazy = LAZY_J;

  // Map & Set
  ctor("Map", map_ctor, MapProto);
  ctor("Set", set_ctor, SetProto);
  MapProto->lazy = LAZY_MapProto;
  SetProto->lazy = LAZY_SetProto;

  // Errors
  auto errType = [&](const char* name, Object* proto) {
    Native* c = newNative(error_ctor, name, Value::obj(proto));
    setOwn(c, "prototype", Value::obj(proto));
    setOwn(proto, "constructor", Value::obj(c));
    setOwn(proto, "name", str(name));
    setGlobal(name, Value::obj(c));
  };
  errType("Error", ErrorProto);
  setOwn(ErrorProto, "message", str(""));
  def(ErrorProto, "toString", error_toString);
  TypeErrorProto = newObject(ErrorProto);
  RangeErrorProto = newObject(ErrorProto);
  ReferenceErrorProto = newObject(ErrorProto);
  SyntaxErrorProto = newObject(ErrorProto);
  errType("TypeError", TypeErrorProto);
  errType("RangeError", RangeErrorProto);
  errType("ReferenceError", ReferenceErrorProto);
  errType("SyntaxError", SyntaxErrorProto);
  oomError->proto = ErrorProto;
  setOwn(oomError, "message", str("out of memory"));

  // console & globals
  Object* C = newObject();
  setGlobal("console", Value::obj(C));
  C->lazy = LAZY_C;
  lazyGlobals(LAZY_GLOBALS);
  DateProto = newObject(ObjectProto);
  DateProto->lazy = LAZY_DateProto;
  Native* D = ctor("Date", date_ctor, DateProto);
  def(D, "now", date_now);
  def(D, "parse", date_parse);
  def(D, "UTC", date_UTC);
  declare(global, intern("NaN"), Value::num(NAN), 2);
  declare(global, intern("Infinity"), Value::num(INFINITY), 2);
  declare(global, intern("undefined"), Value(), 2);
}

}  // namespace js
