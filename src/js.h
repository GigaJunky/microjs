// js.h - a small JavaScript interpreter for microcontrollers (ESP32 "Cheap Yellow Display")
//
// Tree-walking interpreter with a precise mark/sweep garbage collector.
// Portable C++17: the same core builds on the ESP32 and on a desktop for testing.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <deque>

namespace js {

struct Interp;
struct GcObj;
struct Str;
struct Object;
struct Array;
struct Function;
struct Native;
struct Env;
struct Node;
struct Arena;

enum class T : uint8_t { Undef, Null, Bool, Num, Str, Obj };

struct Value {
  T t;
  union { double n; bool b; GcObj* g; };
  Value() : t(T::Undef), n(0) {}
  static Value num(double d) { Value v; v.t = T::Num; v.n = d; return v; }
  static Value boolean(bool x) { Value v; v.t = T::Bool; v.n = 0; v.b = x; return v; }
  static Value null() { Value v; v.t = T::Null; return v; }
  static inline Value str(Str* s);
  static inline Value obj(Object* o);
  bool isUndef() const { return t == T::Undef; }
  bool isNull() const { return t == T::Null; }
  bool isNullish() const { return t == T::Undef || t == T::Null; }
  bool isBool() const { return t == T::Bool; }
  bool isNum() const { return t == T::Num; }
  bool isStr() const { return t == T::Str; }
  bool isObj() const { return t == T::Obj; }
  bool isGc() const { return t == T::Str || t == T::Obj; }
  inline Str* s() const;
  inline Object* o() const;
};

// ---------------------------------------------------------------- memory pool
// Small interpreter allocations (heap objects, property/variable/element lists) come from
// size-segregated free lists carved out of 1-2 KB chunks. On the ESP32 this avoids the system
// allocator, which gets slow and fragmented under the many short-lived blocks a script makes.
void* poolAlloc(size_t n);
void poolFree(void* p, size_t n);
void poolReserve(size_t bytes, void* mem = nullptr);  // once at startup: a contiguous region for the pool
size_t poolRegionLeft();         // unused bytes left in that region
size_t poolTrim();               // give wholly free chunks taken from the system heap back to it
// Where the interpreter's memory comes from once its reserved regions are used up (and for blocks
// over 256 bytes). nullptr = malloc. On a board with PSRAM, point it at PSRAM: the system malloc
// keeps blocks under 4 KB in internal RAM, and pool chunks are never given back, so overflow
// would slowly eat the internal RAM that WiFi needs.
extern void* (*poolSystemAlloc)(size_t);
void arenaReserve(size_t bytes, void* mem = nullptr);  // once at startup: region for parsed programs

template <class T>
struct PoolAlloc {
  using value_type = T;
  PoolAlloc() = default;
  template <class U> PoolAlloc(const PoolAlloc<U>&) {}
  T* allocate(size_t n) { return static_cast<T*>(poolAlloc(n * sizeof(T))); }
  void deallocate(T* p, size_t n) { poolFree(p, n * sizeof(T)); }
  template <class U> bool operator==(const PoolAlloc<U>&) const { return true; }
  template <class U> bool operator!=(const PoolAlloc<U>&) const { return false; }
};
template <class T> using PVec = std::vector<T, PoolAlloc<T>>;

// ---------------------------------------------------------------- heap objects
struct GcObj {
  static void* operator new(size_t n) { return poolAlloc(n); }
  static void operator delete(void* p, size_t n) { poolFree(p, n); }
  GcObj* gcNext = nullptr;
  bool marked = false;
  virtual ~GcObj() {}
  virtual void trace(Interp&) {}
  virtual size_t gcSize() const { return 32; }
};

struct Str final : GcObj {
  std::string s;
  size_t gcSize() const override { return sizeof(Str) + s.capacity(); }
};

enum class K : uint8_t { Plain, Array, Function, Native, Map, Set };

typedef Value (*NativeFn)(Interp& I, Value self, Value* args, int argc);

// A static table of built-in functions. Objects can point at one: each function object is then
// created only when a script first looks it up, which saves most of the built-in library's RAM.
struct NativeEntry {
  const char* name;
  NativeFn fn;
};

const char* intern(const char* s, size_t n);
const char* intern(const char* s);
const char* findAtom(const char* s, size_t n);  // nullptr if never interned

// 24 bytes on the ESP32. Keys are interned strings, compared by pointer.
struct Prop {
  const char* k;
  bool accessor;  // v is then an Array [getter, setter]
  Value v;
};

struct Object : GcObj {
  K kind = K::Plain;
  Object* proto = nullptr;
  PVec<Prop> props;
  const NativeEntry* lazy = nullptr; // built-in methods created on first use (nullptr-terminated)
  void* ext = nullptr;               // host data (e.g. a sprite)
  void (*finalize)(void*) = nullptr; // called when the object is collected
  ~Object() override { if (finalize && ext) finalize(ext); }
  Prop* ownA(const char* atom) {  // atom: an interned key
    for (auto& p : props)
      if (p.k == atom) return &p;
    return nullptr;
  }
  Prop* own(const char* k, size_t n) {
    const char* a = findAtom(k, n);
    return a ? ownA(a) : nullptr;
  }
  Prop* own(const char* k) { return own(k, strlen(k)); }
  void trace(Interp&) override;
  size_t gcSize() const override { return sizeof(Object) + props.capacity() * sizeof(Prop); }
};

struct Array final : Object {
  PVec<Value> items;
  Array() { kind = K::Array; }
  void trace(Interp&) override;
  size_t gcSize() const override { return Object::gcSize() + items.capacity() * sizeof(Value); }
};

struct Native final : Object {
  NativeFn fn = nullptr;
  const char* name = "";
  Value data;            // per-function data (bound args, prototype, ...)
  Native() { kind = K::Native; }
  void trace(Interp&) override;
};

struct Function final : Object {
  Node* node = nullptr;  // N_FUNC node (constructor for classes, may be null)
  Node* cls = nullptr;   // N_CLASS node for classes
  Env* env = nullptr;
  std::shared_ptr<Arena> arena;  // keeps the AST alive
  Object* home = nullptr;        // for super.x lookups
  Value superCls;
  const char* name = nullptr;
  bool isArrow = false, isClass = false, isDerived = false;
  Function() { kind = K::Function; }
  void trace(Interp&) override;
  size_t gcSize() const override { return sizeof(Function) + props.capacity() * sizeof(Prop); }
};

struct MapObj final : Object {
  PVec<std::pair<Value, Value>> entries;
  void trace(Interp&) override;
  size_t gcSize() const override { return sizeof(MapObj) + entries.capacity() * 2 * sizeof(Value); }
};

struct Binding {
  const char* name;  // interned
  bool isConst;
  Value v;
};

struct Env final : GcObj {
  Env* parent = nullptr;
  PVec<Binding> vars;
  Function* func = nullptr;
  Value thisV;
  bool funcScope = false, hasThis = false;
  void trace(Interp&) override;
  size_t gcSize() const override { return sizeof(Env) + vars.capacity() * sizeof(Binding); }
};

inline Value Value::str(Str* s) { Value v; v.t = T::Str; v.g = s; return v; }
inline Value Value::obj(Object* o) { Value v; v.t = T::Obj; v.g = o; return v; }
inline Str* Value::s() const { return static_cast<Str*>(g); }
inline Object* Value::o() const { return static_cast<Object*>(g); }

// Thrown (as a C++ exception) for JavaScript exceptions.
struct JsThrow { Value v; int line; };
// Thrown by the poll hook to abort execution (Ctrl-C). Not catchable from JS.
struct JsInterrupt {};

struct Timer {
  int id;
  uint32_t when, interval;
  bool repeat;
  Value fn;
  std::vector<Value> args;
};

enum Completion : uint8_t { C_NORMAL, C_BREAK, C_CONTINUE, C_RETURN };

std::string numToStr(double d);

#define JS_NATIVE(name) static js::Value name(js::Interp& I, js::Value self, js::Value* args, int argc)
#define JS_ARG(i) ((i) < argc ? args[i] : js::Value())

struct Interp {
  Interp();
  ~Interp();
  Interp(const Interp&) = delete;
  Interp& operator=(const Interp&) = delete;

  // ------------------------------------------------------------ host hooks
  std::function<void(const char* s, size_t n)> write;   // console output
  std::function<uint32_t()> millis;                     // monotonic milliseconds
  std::function<void()> poll;                           // called periodically; may throw JsInterrupt
  std::function<bool(const std::string& path, std::string& out)> readFile;  // used by load()
  std::function<void()> onCollect;                     // after each GC; may throw JsThrow
  size_t stackLimit = 256 * 1024;  // bytes of C stack the interpreter may use
  size_t gcMin = 64 * 1024;        // collect after this many bytes were allocated
  size_t gcMax = SIZE_MAX;         // ...but never let more than this much garbage build up
  bool gcPending = false;          // set to force a collection at the next statement
  bool dropSourceAfterParse = false;  // next run(): free the (non-const) source once parsed
  std::function<void()> afterParse;    // next run(): called once parsing is done
  bool gcStress = false;           // testing: collect after every allocation
  size_t (*freeMemory)() = nullptr;  // optional probe of the real free heap...
  size_t lowMemory = 0;              // ...collect when it drops below this many bytes

  // ------------------------------------------------------------ running code
  Value run(const std::string& src, const char* name = "<input>");  // throws JsThrow / JsInterrupt
  bool runSafe(const std::string& src, const char* name = "<input>", Value* result = nullptr);
  bool callSafe(Value fn, Value self, const std::vector<Value>& args, Value* result = nullptr);
  void runTimers();
  bool hasTimers() const { return !timers.empty(); }
  int32_t msUntilNextTimer();
  void clearTimers() { timers.clear(); }
  void reportError(const JsThrow& e);
  std::string lastError;  // text of the most recent uncaught error
  void collect();
  size_t heapBytes() const { return liveBytes + allocBytes; }
  static bool isIncomplete(const std::string& src);  // for multi-line REPL input

  // ------------------------------------------------------------ helpers for bindings
  void print(const std::string& s) { if (write) write(s.data(), s.size()); }
  Value str(const std::string& s) { return Value::str(allocStr(s.data(), s.size())); }
  Value str(const char* s) { return Value::str(allocStr(s, strlen(s))); }
  Object* newObject() { return newObject(ObjectProto); }
  Object* newObject(Object* proto);
  Array* newArray();
  Native* newNative(NativeFn fn, const char* name, Value data = Value());
  Native* def(Object* o, const char* name, NativeFn fn);  // define a method
  void setOwn(Object* o, const char* k, Value v) { setOwn(o, k, strlen(k), v); }
  void setOwn(Object* o, const char* k, size_t n, Value v);
  void setGlobal(const char* name, Value v);
  void lazyGlobals(const NativeEntry* table) { globalTables.push_back(table); }  // globals on first use
  Value getGlobal(const char* name);
  Value get(Value obj, const char* key) { return getProp(obj, key, strlen(key)); }
  void put(Value obj, const char* key, Value v) { putProp(obj, key, strlen(key), v); }
  Value call(Value fn, Value self, Value* args, int argc);
  Value construct(Value fn, Value* args, int argc);
  [[noreturn]] void throwError(const char* type, const std::string& msg);
  double toNum(Value v);
  int32_t toInt32(Value v);
  std::string toStr(Value v);
  Value toStrV(Value v) { return v.isStr() ? v : str(toStr(v)); }
  bool truthy(Value v);
  bool isCallable(Value v) const {
    return v.isObj() && (v.o()->kind == K::Function || v.o()->kind == K::Native);
  }
  std::string inspect(Value v, int depth = 0);
  std::string errorString(Value v);
  void rootTemp(Value v) { if (v.isGc()) temps.push_back(v.g); }
  std::vector<Value> toList(Value v);
  double random();

  // ------------------------------------------------------------ internals
  GcObj* heap = nullptr;
  size_t allocBytes = 0, liveBytes = 0, gcThreshold = 0;
  std::vector<GcObj*> temps;
  std::deque<GcObj*> gray;  // mark stack; a deque grows in small chunks (no big contiguous blocks)
  std::vector<Arena*> arenas;
  std::vector<const NativeEntry*> globalTables;
  Prop* resolveLazy(Object* o, const char* atom);
  std::vector<Value> roots;
  Env* global = nullptr;
  Object *ObjectProto = nullptr, *FunctionProto = nullptr, *ArrayProto = nullptr,
         *StringProto = nullptr, *NumberProto = nullptr, *BooleanProto = nullptr,
         *ErrorProto = nullptr, *TypeErrorProto = nullptr, *RangeErrorProto = nullptr,
         *ReferenceErrorProto = nullptr, *SyntaxErrorProto = nullptr, *MapProto = nullptr,
         *SetProto = nullptr, *DateProto = nullptr;
  Str* typeStr[6] = {};
  Str* charCache[128] = {};
  Value retVal, lastResult;
  Object* oomError = nullptr;  // pre-made "out of memory" error
  Arena* curArena = nullptr;
  Native* curNative = nullptr;
  bool constructing = false, optShort = false;
  int line = 0, entryDepth = 0;
  uint32_t steps = 0, allocCount = 0;
  uintptr_t stackBase = 0;
  std::vector<Timer> timers;
  int nextTimerId = 1;
  uint64_t rng = 0x9E3779B97F4A7C15ull;

  template <class O> O* gcNew() {
    O* o = new O();
    o->gcNext = heap;
    heap = o;
    temps.push_back(o);
    allocBytes += sizeof(O) + 48;  // + allocator overhead and member buffers
    if (freeMemory && (++allocCount & 31) == 0 && freeMemory() < lowMemory) gcPending = true;
    return o;
  }
  Str* allocStr(const char* s, size_t n);
  Value charStr(unsigned char c);
  Env* newEnv(Env* parent);
  void markValue(Value v) { if (v.isGc()) markObj(v.g); }
  void markObj(GcObj* o) { if (o && !o->marked) { o->marked = true; gray.push_back(o); } }
  void stackCheck();
  void setupBuiltins();

  // properties
  Value getProp(Value base, const char* k, size_t n, const char* atom = nullptr);
  Value getIndex(Value base, Value key);
  Value lookupFrom(Object* o, const char* atom, Value receiver);
  bool specialGet(Object* o, const char* k, size_t n, Value& r);
  void putProp(Value base, const char* k, size_t n, Value v, const char* atom = nullptr);
  void putIndex(Value base, Value key, Value v);
  void setIndex(Array* a, size_t i, Value v);
  bool delProp(Value base, const std::string& k);
  bool hasProp(Value base, const std::string& k);
  void defineAccessor(Object* o, const std::string& k, Value fn, bool setter);
  std::vector<std::string> ownKeys(Value v);
  std::string toKey(Value v);
  Value toPrimitive(Value v, bool numberHint);
  bool strictEq(Value a, Value b);
  bool looseEq(Value a, Value b);
  const char* typeOf(Value v);
  bool instanceOf(Value v, Value ctor);

  // evaluation
  Value ev(Node* n, Env* e);
  Value evTemplate(Node* n, Env* e);
  Value evArray(Node* n, Env* e);
  Value evObject(Node* n, Env* e);
  Value evFuncExpr(Node* n, Env* e);
  Value evUnary(Node* n, Env* e);
  Value evUpdate(Node* n, Env* e);
  Value evAssign(Node* n, Env* e);
  Value evCall(Node* n, Env* e);
  Value evNew(Node* n, Env* e);
  Value evMember(Node* n, Env* e);
  Value binop(int op, Value a, Value b);
  Value getRef(Node* t, Env* e, Value& obj, Value& key);
  void setRef(Node* t, Env* e, Value obj, Value key, Value v);
  void evalArgs(Node* n, Env* e, PVec<Value>& out);
  std::string describe(Node* n);
  Value thisOf(Env* e);
  Function* homeFunc(Env* e);
  Value superLookup(Env* e, Node* member, Value self);
  Binding* lookup(Env* e, const char* name);
  void declare(Env* e, const char* name, Value v, int kind);
  void assignVar(Env* e, const char* name, Value v);
  void bindPattern(Node* p, Value v, Env* e, int kind);
  void nameIfAnon(Value v, Node* init, const char* name);
  Function* makeFunction(Node* fn, Env* e);
  Function* makeClass(Node* n, Env* e);
  void initFields(Function* C, Object* obj);
  void superConstruct(Function* f, Object* obj, Value* args, int argc);
  Value runCtor(Function* f, Object* obj, Value* args, int argc);
  Value invoke(Function* f, Value self, Value* args, int argc);
  Completion exec(Node* s, Env* e);
  Completion execStatements(Node** kids, int n, Env* e);
  void hoist(Node** kids, int n, Env* e);
  void execVar(Node* s, Env* e);
  Completion execFor(Node* s, Env* e);
  Completion execLoop(Node* s, Env* e);
  Completion execForInOf(Node* s, Env* e);
  Completion execTry(Node* s, Env* e);
  Completion execSwitch(Node* s, Env* e);
  Env* copyEnv(Env* e);
};

}  // namespace js
