// js.cpp - lexer, parser and evaluator
#include "js.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <unordered_set>
#include <string_view>
#include <algorithm>

namespace js {

// Interned strings live forever; property names and identifiers are compared by pointer.
// They (and the hash set's nodes) are packed into pool memory: as individual heap blocks they
// would be permanent crumbs scattered all over the heap.
using AtomSet = std::unordered_set<std::string_view, std::hash<std::string_view>, std::equal_to<std::string_view>,
                                   PoolAlloc<std::string_view>>;
static AtomSet& atomTable() {
  static auto* tab = new AtomSet();
  return *tab;
}
static char* atomChars(size_t n) {  // bump-allocate atom text in 256-byte pool chunks
  static char *chunk = nullptr, *end = nullptr;
  if (n > 128) return (char*)poolAlloc(n);
  if (!chunk || chunk + n > end) {
    chunk = (char*)poolAlloc(256);
    end = chunk + 256;
  }
  char* p = chunk;
  chunk += n;
  return p;
}
const char* findAtom(const char* s, size_t n) {
  auto& t = atomTable();
  auto it = t.find(std::string_view(s, n));
  return it == t.end() ? nullptr : it->data();
}
const char* intern(const char* s, size_t n) {
  if (const char* a = findAtom(s, n)) return a;
  char* p = atomChars(n + 1);
  memcpy(p, s, n);
  p[n] = 0;
  atomTable().insert(std::string_view(p, n));
  return p;
}
const char* intern(const char* s) { return intern(s, strlen(s)); }

static const char* A_arguments;
#define JS_NOINLINE __attribute__((noinline))

// =================================================================== memory pool
void* (*poolSystemAlloc)(size_t) = nullptr;
static void* sysAlloc(size_t n) { return poolSystemAlloc ? poolSystemAlloc(n) : malloc(n); }
#ifdef JS_NO_POOL  // sanitizer builds: every allocation separate, so ASan sees overflows
void* poolAlloc(size_t n) { return ::operator new(n); }
void poolFree(void* p, size_t) { ::operator delete(p); }
void poolReserve(size_t, void*) {}
size_t poolRegionLeft() { return 0; }
#else
static const size_t POOL_MAX = 256;           // larger blocks go to the system allocator
static void* poolFreeList[POOL_MAX / 8 + 1];  // one free list per 8-byte size class
static char *poolRegion, *poolRegionEnd;      // optional contiguous reserve for chunks

size_t poolRegionLeft() { return poolRegion ? poolRegionEnd - poolRegion : 0; }

void poolReserve(size_t bytes, void* mem) {
  // Carve chunks from one block taken early, while the heap is unfragmented: small objects
  // then stay packed together and the rest of the heap keeps large free blocks.
  if (!poolRegion && (poolRegion = (char*)(mem ? mem : malloc(bytes)))) poolRegionEnd = poolRegion + bytes;
}

// A fresh chunk for size class `size`: from the reserved region, else the system heap.
// Returns the chunk and stores its length in `len`.
static char* poolChunk(size_t size, size_t& len) {
  len = size <= 128 ? 1024 : 2048;
  if (poolRegion && poolRegion + len <= poolRegionEnd) {
    char* c = poolRegion;
    poolRegion += len;
    return c;
  }
  // fragmented heap: settle for a smaller chunk before giving up
  for (size_t want = len;; want = want / 2 > size ? want / 2 : size) {
    if (char* c = (char*)sysAlloc(want)) { len = want; return c; }
    if (want == size) break;
  }
  len = size;
  return (char*)::operator new(size);  // last resort: may release the out-of-memory reserve
}

// Refill an empty size class by splitting a free block of a larger class.
static bool poolSteal(size_t cls) {
  size_t size = cls << 3;
  for (size_t big = cls + 1; big <= POOL_MAX / 8; big++) {
    char* b = (char*)poolFreeList[big];
    if (!b) continue;
    poolFreeList[big] = *(void**)b;
    for (size_t off = 0; off + size <= (big << 3); off += size) {
      *(void**)(b + off) = poolFreeList[cls];
      poolFreeList[cls] = b + off;
    }
    return true;
  }
  return false;
}

void* poolAlloc(size_t n) {
  if (n == 0) n = 1;
  if (n > POOL_MAX) {
    if (poolSystemAlloc)
      if (void* p = poolSystemAlloc(n)) return p;  // freed with ::operator delete -> free(): fine
    return ::operator new(n);
  }
  size_t cls = (n + 7) >> 3, size = cls << 3;
  void* p = poolFreeList[cls];
  if (!p && !(poolRegion && poolRegion + 1024 <= poolRegionEnd) && poolSteal(cls)) p = poolFreeList[cls];
  if (!p) {  // carve a new chunk into blocks of this size
    size_t chunk;
    char* c = poolChunk(size, chunk);
    for (size_t off = 0; off + size <= chunk; off += size) {
      *(void**)(c + off) = poolFreeList[cls];
      poolFreeList[cls] = c + off;
    }
    p = poolFreeList[cls];
  }
  poolFreeList[cls] = *(void**)p;
  return p;
}

void poolFree(void* p, size_t n) {
  if (!p) return;
  if (n == 0) n = 1;
  if (n > POOL_MAX) { ::operator delete(p); return; }
  size_t cls = (n + 7) >> 3;
  *(void**)p = poolFreeList[cls];
  poolFreeList[cls] = p;
}
#endif

// =================================================================== AST
enum NT : uint8_t {
  N_NUM, N_STR, N_TMPL, N_ID, N_THIS, N_SUPER, N_TRUE, N_FALSE, N_NULL,
  N_ARR, N_OBJ, N_PROP, N_FUNC, N_CLASS, N_MEMBER,
  N_UNARY, N_UPDATE, N_BIN, N_LOGIC, N_ASSIGN, N_COND, N_CALL, N_NEW, N_DOT, N_INDEX,
  N_SEQ, N_SPREAD, N_OPTCHAIN,
  N_OBJPAT, N_ARRPAT, N_PATPROP, N_DEFAULT, N_REST,
  N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_WHILE, N_DO, N_FOR, N_FORIN, N_FOROF,
  N_BREAK, N_CONTINUE, N_RETURN, N_THROW, N_TRY, N_SWITCH, N_CASE, N_EMPTY, N_PROGRAM
};

enum Op : uint8_t {
  O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_EXP,
  O_EQ, O_NE, O_SEQ, O_SNE, O_LT, O_GT, O_LE, O_GE,
  O_BAND, O_BOR, O_BXOR, O_SHL, O_SHR, O_USHR, O_IN, O_INSTANCEOF,
  O_LAND, O_LOR, O_NULLISH,
  O_NOT, O_NEG, O_PLUS, O_BNOT, O_TYPEOF, O_VOID, O_DELETE,
  O_ASSIGN
};

enum : uint16_t {
  F_ARROW = 1, F_CONCISE = 2, F_STATIC = 4, F_OPTIONAL = 8, F_COMPUTED = 16, F_SCOPE = 32,
  F_ARGS = 64, F_GET = 128, F_SET = 256, F_METHOD = 512, F_DECL = 1024, F_PREFIX = 2048,
  F_FIELD = 4096, F_SPREAD = 8192, F_CLOSURE = 16384
};

enum { V_VAR = 0, V_LET = 1, V_CONST = 2, V_ASSIGN = 3 };

// AST node. Nodes are variable-sized: each type is allocated only up to the last field it
// uses (see nodeSize), which roughly halves the memory a parsed program needs on the ESP32.
// The field order matters: str, a, kids, b, c, d.
struct Node {
  uint8_t type, op;
  uint16_t flags;
  uint16_t n;     // number of kids (or name length for N_DOT)
  uint16_t line;
  union { const char* str; Str* lit; uint32_t lo; };  // lo/hi hold N_NUM's double
  union { Node* a; uint32_t hi; };
  Node** kids;
  Node *b, *c, *d;
  double num() const {
    uint64_t v = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &v, sizeof d);
    return d;
  }
  void setNum(double d) {
    uint64_t v;
    memcpy(&v, &d, sizeof v);
    lo = (uint32_t)v;
    hi = (uint32_t)(v >> 32);
  }
};

static size_t nodeSize(uint8_t type) {
  switch (type) {
    case N_THIS: case N_SUPER: case N_TRUE: case N_FALSE: case N_NULL: case N_EMPTY:
    case N_BREAK: case N_CONTINUE:
      return offsetof(Node, str);
    case N_ID: case N_STR:
      return offsetof(Node, a);
    case N_NUM: case N_DOT: case N_UNARY: case N_UPDATE: case N_SPREAD: case N_OPTCHAIN:
    case N_REST: case N_EXPR: case N_RETURN: case N_THROW:
      return offsetof(Node, kids);
    case N_TMPL: case N_ARR: case N_OBJ: case N_SEQ: case N_BLOCK: case N_PROGRAM: case N_VAR:
    case N_OBJPAT: case N_ARRPAT: case N_CALL: case N_NEW: case N_FUNC: case N_CASE: case N_SWITCH:
      return offsetof(Node, b);
    case N_PROP: case N_MEMBER: case N_BIN: case N_LOGIC: case N_ASSIGN: case N_INDEX:
    case N_PATPROP: case N_DEFAULT: case N_DECL: case N_CLASS:
      return offsetof(Node, c);
    case N_COND: case N_IF:
      return offsetof(Node, d);
    default:  // loops, try
      return sizeof(Node);
  }
}

// Parsed programs get their own region of 1 KB blocks (reserved at startup), so syntax trees
// don't fragment the general heap that WiFi, sprites and network buffers need.
static const size_t ARENA_BLOCK = 1024;
static char *arenaRegionLo, *arenaRegionHi;
static void* arenaFreeBlocks;

void arenaReserve(size_t bytes, void* mem) {
  if (arenaRegionLo) return;
  bytes -= bytes % ARENA_BLOCK;
  if (!(arenaRegionLo = (char*)(mem ? mem : malloc(bytes)))) return;
  arenaRegionHi = arenaRegionLo + bytes;
  for (char* b = arenaRegionLo; b < arenaRegionHi; b += ARENA_BLOCK) {
    *(void**)b = arenaFreeBlocks;
    arenaFreeBlocks = b;
  }
}

static char* arenaBlockAlloc() {
  if (!arenaFreeBlocks) return (char*)sysAlloc(ARENA_BLOCK);
  char* b = (char*)arenaFreeBlocks;
  arenaFreeBlocks = *(void**)b;
  return b;
}

static void arenaBlockFree(char* b) {
  if (b >= arenaRegionLo && b < arenaRegionHi) {
    *(void**)b = arenaFreeBlocks;
    arenaFreeBlocks = b;
  } else free(b);
}

struct Arena : std::enable_shared_from_this<Arena> {
  Interp* I;
  std::vector<char*> blocks;
  size_t used = 0, cap = 0;
  std::vector<Str*> lits;  // string literals, kept alive while the AST lives
  explicit Arena(Interp* i) : I(i) { I->arenas.push_back(this); }
  ~Arena() {
    auto& v = I->arenas;
    auto it = std::find(v.begin(), v.end(), this);
    if (it != v.end()) v.erase(it);
    for (char* b : blocks) arenaBlockFree(b);
  }
  void* alloc(size_t n) {
    n = (n + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    if (blocks.empty() || used + n > cap) {
      size_t sz = n > ARENA_BLOCK ? n : ARENA_BLOCK;
      char* b = sz == ARENA_BLOCK ? arenaBlockAlloc() : (char*)malloc(sz);
      if (!b) b = (char*)malloc(sz = n > 64 ? n : 64);  // fragmented heap: take a small piece
      if (!b) throw std::bad_alloc();
      blocks.push_back(b);
      used = 0;
      cap = sz;
    }
    void* p = blocks.back() + used;
    used += n;
    return p;
  }
  const char* dup(const char* s, size_t n) {
    char* p = (char*)alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
  }
};

// =================================================================== numbers
std::string numToStr(double d) {
  if (std::isnan(d)) return "NaN";
  if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
  if (d == 0) return "0";
  char buf[64];
  if (std::fabs(d) < 9007199254740992.0 && d == std::floor(d)) {
    snprintf(buf, sizeof buf, "%.0f", d);
    return buf;
  }
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
    if (strtod(buf, nullptr) == d) break;
  }
  std::string digits;
  bool neg = false;
  const char* p = buf;
  if (*p == '-') { neg = true; p++; }
  while (*p && *p != 'e') { if (isdigit((unsigned char)*p)) digits += *p; p++; }
  int exp = *p ? atoi(p + 1) : 0;
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  int k = (int)digits.size(), n = exp + 1;
  std::string r = neg ? "-" : "";
  if (k <= n && n <= 21) { r += digits; r.append(n - k, '0'); }
  else if (0 < n && n <= 21) { r += digits.substr(0, n); r += '.'; r += digits.substr(n); }
  else if (-6 < n && n <= 0) { r += "0."; r.append(-n, '0'); r += digits; }
  else {
    r += digits[0];
    if (k > 1) { r += '.'; r += digits.substr(1); }
    r += 'e';
    r += (n - 1 >= 0) ? '+' : '-';
    r += std::to_string(std::abs(n - 1));
  }
  return r;
}

static void utf8(std::string& out, uint32_t c) {
  if (c < 0x80) out += (char)c;
  else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
  else if (c < 0x10000) {
    out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63));
  } else {
    out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63));
    out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63));
  }
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

// =================================================================== lexer
enum TT : uint8_t { TK_EOF, TK_NUM, TK_STR, TK_TMPL, TK_ID, TK_P };

struct Tok {
  TT type = TK_EOF;
  const char* p = "";
  int len = 0;
  double num = 0;
  std::string sv;
  int line = 1;
  bool nl = false;
  std::vector<std::string> quasis;
  std::vector<int> exprStart, exprLen, exprLine;
  bool is(const char* s) const {
    if (type != TK_P) return false;
    size_t n = strlen(s);
    return (size_t)len == n && memcmp(p, s, n) == 0;
  }
  bool kw(const char* s) const {
    if (type != TK_ID) return false;
    size_t n = strlen(s);
    return (size_t)len == n && memcmp(p, s, n) == 0;
  }
};

static const char* PUNCTS[] = {
  ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
  "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=",
  "%=", "&=", "|=", "^=", "**", "<<", ">>",
  "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^",
  "!", "~", "?", ":", "=", ".", nullptr};

static bool isIdStart(unsigned char c) { return isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
static bool isIdPart(unsigned char c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

struct Lexer {
  Interp* I = nullptr;
  const char* src = "";
  int pos = 0, end = 0, line = 1;
  const char* fname = "";
  Tok tok;

  [[noreturn]] void errAt(int ln, const std::string& m) {
    I->line = 0;
    I->throwError("SyntaxError", std::string(fname) + ":" + std::to_string(ln) + ": " + m);
  }
  [[noreturn]] void err(const std::string& m) { errAt(line, m); }

  void readEscape(std::string& out) {
    if (pos >= end) err("bad escape");
    char c = src[pos++];
    switch (c) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'v': out += '\v'; break;
      case '0': out += '\0'; break;
      case '\r': if (pos < end && src[pos] == '\n') pos++; line++; break;
      case '\n': line++; break;
      case 'x': {
        if (pos + 2 > end || hexVal(src[pos]) > 15 || hexVal(src[pos + 1]) > 15) err("bad \\x escape");
        utf8(out, hexVal(src[pos]) * 16 + hexVal(src[pos + 1]));
        pos += 2;
        break;
      }
      case 'u': {
        uint32_t v = 0;
        if (pos < end && src[pos] == '{') {
          pos++;
          while (pos < end && src[pos] != '}') {
            if (hexVal(src[pos]) > 15) err("bad \\u escape");
            v = v * 16 + hexVal(src[pos++]);
          }
          pos++;
        } else {
          for (int i = 0; i < 4; i++) {
            if (pos >= end || hexVal(src[pos]) > 15) err("bad \\u escape");
            v = v * 16 + hexVal(src[pos++]);
          }
          // surrogate pair
          if (v >= 0xD800 && v < 0xDC00 && pos + 6 <= end && src[pos] == '\\' && src[pos + 1] == 'u') {
            uint32_t lo = 0;
            bool ok = true;
            for (int i = 0; i < 4; i++) { int h = hexVal(src[pos + 2 + i]); if (h > 15) ok = false; lo = lo * 16 + h; }
            if (ok && lo >= 0xDC00 && lo < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); pos += 6; }
          }
        }
        utf8(out, v);
        break;
      }
      default: out += c;
    }
  }

  void skipString(char q) {
    pos++;
    while (pos < end && src[pos] != q) {
      if (src[pos] == '\\') pos++;
      else if (src[pos] == '\n') line++;
      pos++;
    }
    if (pos >= end) err("unterminated string");
    pos++;
  }
  void skipTemplate() {  // at '`'
    pos++;
    while (pos < end && src[pos] != '`') {
      if (src[pos] == '\\') { pos += 2; continue; }
      if (src[pos] == '\n') line++;
      if (src[pos] == '$' && pos + 1 < end && src[pos + 1] == '{') { pos += 2; skipBraces(); pos++; continue; }
      pos++;
    }
    if (pos >= end) err("unterminated template");
    pos++;
  }
  void skipBraces() {  // after '{'; stops at the matching '}'
    int depth = 1;
    while (pos < end) {
      char c = src[pos];
      if (c == '{') depth++;
      else if (c == '}') { if (--depth == 0) return; }
      else if (c == '"' || c == '\'') { skipString(c); continue; }
      else if (c == '`') { skipTemplate(); continue; }
      else if (c == '/' && pos + 1 < end && src[pos + 1] == '/') { while (pos < end && src[pos] != '\n') pos++; continue; }
      else if (c == '/' && pos + 1 < end && src[pos + 1] == '*') {
        pos += 2;
        while (pos + 1 < end && !(src[pos] == '*' && src[pos + 1] == '/')) { if (src[pos] == '\n') line++; pos++; }
        pos += 2;
        continue;
      } else if (c == '\n') line++;
      pos++;
    }
    err("unterminated template expression");
  }

  void next() {
    tok.nl = false;
    tok.sv.clear();
    tok.quasis.clear();
    tok.exprStart.clear(); tok.exprLen.clear(); tok.exprLine.clear();
    for (;;) {
      if (pos >= end) break;
      char c = src[pos];
      if (c == '\n') { line++; tok.nl = true; pos++; }
      else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') pos++;
      else if (c == '/' && pos + 1 < end && src[pos + 1] == '/') { while (pos < end && src[pos] != '\n') pos++; }
      else if (c == '/' && pos + 1 < end && src[pos + 1] == '*') {
        pos += 2;
        while (pos + 1 < end && !(src[pos] == '*' && src[pos + 1] == '/')) {
          if (src[pos] == '\n') { line++; tok.nl = true; }
          pos++;
        }
        if (pos + 1 >= end) err("unterminated comment");
        pos += 2;
      } else if (c == '#' && pos == 0 && pos + 1 < end && src[1] == '!') {
        while (pos < end && src[pos] != '\n') pos++;
      } else if ((unsigned char)c == 0xEF && pos + 2 < end && (unsigned char)src[pos + 1] == 0xBB && (unsigned char)src[pos + 2] == 0xBF) {
        pos += 3;  // BOM
      } else break;
    }
    tok.line = line;
    tok.p = src + pos;
    if (pos >= end) { tok.type = TK_EOF; tok.len = 0; return; }
    unsigned char c = src[pos];
    int start = pos;
    if (isdigit(c) || (c == '.' && pos + 1 < end && isdigit((unsigned char)src[pos + 1]))) {
      lexNumber();
      tok.len = pos - start;
      return;
    }
    if (isIdStart(c) || (c == '#' && pos + 1 < end && isIdStart(src[pos + 1]))) {
      pos++;
      while (pos < end && isIdPart(src[pos])) pos++;
      tok.type = TK_ID;
      tok.len = pos - start;
      return;
    }
    if (c == '"' || c == '\'') {
      pos++;
      for (;;) {
        if (pos >= end || src[pos] == '\n') err("unterminated string");
        char ch = src[pos++];
        if (ch == (char)c) break;
        if (ch == '\\') readEscape(tok.sv);
        else tok.sv += ch;
      }
      tok.type = TK_STR;
      tok.len = pos - start;
      return;
    }
    if (c == '`') {
      pos++;
      std::string cur;
      for (;;) {
        if (pos >= end) err("unterminated template");
        char ch = src[pos];
        if (ch == '`') { pos++; break; }
        if (ch == '\\') { pos++; readEscape(cur); continue; }
        if (ch == '$' && pos + 1 < end && src[pos + 1] == '{') {
          tok.quasis.push_back(cur);
          cur.clear();
          pos += 2;
          int s = pos, ln = line;
          skipBraces();
          tok.exprStart.push_back(s);
          tok.exprLen.push_back(pos - s);
          tok.exprLine.push_back(ln);
          pos++;
          continue;
        }
        if (ch == '\n') line++;
        if (ch == '\r') { pos++; continue; }
        cur += ch;
        pos++;
      }
      tok.quasis.push_back(cur);
      tok.type = TK_TMPL;
      tok.len = pos - start;
      return;
    }
    for (const char** p = PUNCTS; *p; p++) {
      size_t n = strlen(*p);
      if (pos + (int)n <= end && memcmp(src + pos, *p, n) == 0) {
        if (n == 2 && (*p)[0] == '?' && (*p)[1] == '.' && pos + 2 < end && isdigit((unsigned char)src[pos + 2])) continue;
        tok.type = TK_P;
        tok.len = (int)n;
        pos += (int)n;
        return;
      }
    }
    if (c == '/') err("regular expressions are not supported");
    err(std::string("unexpected character '") + (char)c + "'");
  }

  void lexNumber() {
    tok.type = TK_NUM;
    if (src[pos] == '0' && pos + 1 < end && strchr("xXoObB", src[pos + 1]) && src[pos + 1]) {
      char k = src[pos + 1] | 32;
      int base = k == 'x' ? 16 : k == 'o' ? 8 : 2;
      pos += 2;
      double v = 0;
      int digits = 0;
      while (pos < end) {
        char d = src[pos];
        if (d == '_') { pos++; continue; }
        int dv = hexVal(d);
        if (dv >= base) break;
        v = v * base + dv;
        pos++;
        digits++;
      }
      if (!digits) err("bad number");
      tok.num = v;
    } else {
      std::string buf;
      while (pos < end && (isdigit((unsigned char)src[pos]) || src[pos] == '_')) { if (src[pos] != '_') buf += src[pos]; pos++; }
      if (pos < end && src[pos] == '.') {
        buf += src[pos++];
        while (pos < end && (isdigit((unsigned char)src[pos]) || src[pos] == '_')) { if (src[pos] != '_') buf += src[pos]; pos++; }
      }
      if (pos < end && (src[pos] == 'e' || src[pos] == 'E')) {
        int save = pos;
        std::string e = "e";
        pos++;
        if (pos < end && (src[pos] == '+' || src[pos] == '-')) e += src[pos++];
        if (pos < end && isdigit((unsigned char)src[pos])) {
          while (pos < end && isdigit((unsigned char)src[pos])) e += src[pos++];
          buf += e;
        } else pos = save;
      }
      tok.num = strtod(buf.c_str(), nullptr);
    }
    if (pos < end && isIdStart(src[pos])) err("invalid number literal");
  }
};

// =================================================================== parser
struct Parser {
  Interp& I;
  Arena* A;
  const char* fname;
  const char* src;
  Lexer L;
  bool noIn = false;
  int funcCount = 0;
  std::vector<Node*> funcs;  // enclosing non-arrow functions

  Parser(Interp& i, Arena* a, const char* s, size_t len, const char* fn) : I(i), A(a), fname(fn), src(s) {
    L.I = &I;
    L.src = s;
    L.pos = 0;
    L.end = (int)len;
    L.line = 1;
    L.fname = fn;
    L.next();
  }

  Tok& t() { return L.tok; }
  void next() { L.next(); }
  [[noreturn]] void err(const std::string& m) { L.errAt(t().line, m); }
  std::string tokDesc() {
    if (t().type == TK_EOF) return "end of input";
    return "'" + std::string(t().p, t().len) + "'";
  }
  bool eat(const char* p) {
    if (t().is(p)) { next(); return true; }
    return false;
  }
  void expect(const char* p) {
    if (!eat(p)) err(std::string("expected '") + p + "' but found " + tokDesc());
  }
  bool peekIs(const char* p) {
    Lexer save = L;
    L.next();
    bool r = t().is(p);
    L = save;
    return r;
  }
  Node* mk(uint8_t type, int line) {
    size_t sz = nodeSize(type);
#ifdef JS_NODE_DEBUG  // every node its own allocation, so ASan catches reads past a node's end
    Node* n = (Node*)malloc(sz);
#else
    Node* n = (Node*)A->alloc(sz);
#endif
    memset((void*)n, 0, sz);
    n->type = type;
    n->line = (uint16_t)line;
    if (type == N_FUNC || type == N_CLASS) funcCount++;
    return n;
  }
  Node** list(const std::vector<Node*>& v) {
    if (v.size() > 65535) err("too many items");
    Node** p = (Node**)A->alloc(sizeof(Node*) * (v.empty() ? 1 : v.size()));
    for (size_t i = 0; i < v.size(); i++) p[i] = v[i];
    return p;
  }
  void setKids(Node* n, const std::vector<Node*>& v) {
    n->kids = list(v);
    n->n = (uint16_t)v.size();
  }
  Node* strLit(const std::string& s, int line) {
    Node* n = mk(N_STR, line);
    Str* st = I.allocStr(s.data(), s.size());
    A->lits.push_back(st);
    n->lit = st;
    return n;
  }
  const char* atom() { return intern(t().p, t().len); }
  bool isReserved() {
    static const char* rw[] = {"if", "else", "for", "while", "do", "return", "break", "continue", "var", "let",
                               "const", "function", "class", "new", "delete", "typeof", "void", "in", "of",
                               "instanceof", "this", "super", "null", "true", "false", "throw", "try", "catch",
                               "finally", "switch", "case", "default", "extends", "static", nullptr};
    for (const char** p = rw; *p; p++)
      if (t().kw(*p)) return true;
    return false;
  }
  Node* ident() {
    if (t().type != TK_ID) err("expected identifier but found " + tokDesc());
    Node* n = mk(N_ID, t().line);
    n->str = atom();
    if (n->str == A_arguments && !funcs.empty()) funcs.back()->flags |= F_ARGS;
    next();
    return n;
  }

  // ------------------------------------------------------------ program / statements
  Node* program() {
    Node* p = mk(N_PROGRAM, 1);
    std::vector<Node*> v;
    while (t().type != TK_EOF) v.push_back(statement());
    setKids(p, v);
    return p;
  }

  void semi() {
    if (eat(";")) return;
    if (t().is("}") || t().type == TK_EOF || t().nl) return;
    err("expected ';' but found " + tokDesc());
  }

  Node* block() {
    Node* b = mk(N_BLOCK, t().line);
    expect("{");
    std::vector<Node*> v;
    while (!t().is("}")) {
      if (t().type == TK_EOF) err("expected '}'");
      v.push_back(statement());
    }
    next();
    setKids(b, v);
    for (Node* s : v)
      if ((s->type == N_VAR && s->op != V_VAR) || ((s->type == N_FUNC || s->type == N_CLASS) && (s->flags & F_DECL)))
        b->flags |= F_SCOPE;
    return b;
  }

  Node* statement() {
    I.stackCheck();
    Tok& k = t();
    int line = k.line;
    if (k.type == TK_P) {
      if (k.is("{")) return block();
      if (k.is(";")) { next(); return mk(N_EMPTY, line); }
    } else if (k.type == TK_ID) {
      if (k.kw("var") || k.kw("const") || (k.kw("let") && !peekIs("=") && !peekIs("."))) {
        Node* v = varDecl();
        semi();
        return v;
      }
      if (k.kw("function")) {
        next();
        Node* f = mk(N_FUNC, line);
        f->flags |= F_DECL;
        if (t().is("*")) err("generators are not supported");
        if (t().type != TK_ID) err("function name expected");
        f->str = atom();
        next();
        funcRest(f);
        return f;
      }
      if (k.kw("async")) err("async functions are not supported");
      if (k.kw("class")) {
        Node* c = classDef(true);
        return c;
      }
      if (k.kw("if")) {
        next();
        Node* n = mk(N_IF, line);
        expect("(");
        n->a = expression();
        expect(")");
        n->b = statement();
        if (t().kw("else")) { next(); n->c = statement(); }
        return n;
      }
      if (k.kw("while")) {
        next();
        Node* n = mk(N_WHILE, line);
        expect("(");
        n->a = expression();
        expect(")");
        n->d = statement();
        return n;
      }
      if (k.kw("do")) {
        next();
        Node* n = mk(N_DO, line);
        n->d = statement();
        if (!t().kw("while")) err("expected 'while'");
        next();
        expect("(");
        n->a = expression();
        expect(")");
        eat(";");
        return n;
      }
      if (k.kw("for")) return forStatement();
      if (k.kw("return")) {
        next();
        Node* n = mk(N_RETURN, line);
        if (!t().nl && !t().is(";") && !t().is("}") && t().type != TK_EOF) n->a = expression();
        semi();
        return n;
      }
      if (k.kw("break") || k.kw("continue")) {
        Node* n = mk(k.kw("break") ? N_BREAK : N_CONTINUE, line);
        next();
        if (t().type == TK_ID && !t().nl) err("labels are not supported");
        semi();
        return n;
      }
      if (k.kw("throw")) {
        next();
        Node* n = mk(N_THROW, line);
        n->a = expression();
        semi();
        return n;
      }
      if (k.kw("try")) {
        next();
        Node* n = mk(N_TRY, line);
        n->a = block();
        if (t().kw("catch")) {
          next();
          if (eat("(")) {
            n->b = bindingTarget();
            expect(")");
          }
          n->c = block();
        }
        if (t().kw("finally")) { next(); n->d = block(); }
        if (!n->c && !n->d) err("missing catch or finally");
        return n;
      }
      if (k.kw("switch")) {
        next();
        Node* n = mk(N_SWITCH, line);
        expect("(");
        n->a = expression();
        expect(")");
        expect("{");
        std::vector<Node*> cases;
        while (!eat("}")) {
          Node* c = mk(N_CASE, t().line);
          if (t().kw("case")) { next(); c->a = expression(); }
          else if (t().kw("default")) next();
          else err("expected 'case' or 'default'");
          expect(":");
          std::vector<Node*> body;
          while (!t().kw("case") && !t().kw("default") && !t().is("}")) {
            if (t().type == TK_EOF) err("expected '}'");
            body.push_back(statement());
          }
          setKids(c, body);
          cases.push_back(c);
        }
        setKids(n, cases);
        return n;
      }
    }
    Node* n = mk(N_EXPR, line);
    n->a = expression();
    semi();
    return n;
  }

  Node* varDecl() {
    Node* n = mk(N_VAR, t().line);
    n->op = t().kw("var") ? V_VAR : t().kw("let") ? V_LET : V_CONST;
    next();
    std::vector<Node*> v;
    do {
      Node* d = mk(N_DECL, t().line);
      d->a = bindingTarget();
      if (eat("=")) d->b = assign();
      v.push_back(d);
    } while (eat(","));
    setKids(n, v);
    return n;
  }

  Node* forStatement() {
    int line = t().line;
    next();
    if (t().kw("await")) err("for await is not supported");
    expect("(");
    Node* init = nullptr;
    int fc0 = funcCount;
    if (!t().is(";")) {
      noIn = true;
      if (t().kw("var") || t().kw("let") || t().kw("const")) init = varDecl();
      else init = expression();
      noIn = false;
      if (t().kw("of") || t().kw("in")) {
        bool of = t().kw("of");
        next();
        Node* n = mk(of ? N_FOROF : N_FORIN, line);
        if (init->type == N_VAR) {
          if (init->n != 1 || init->kids[0]->b) err("invalid for-in/of declaration");
        } else init = toPattern(init);
        n->a = init;
        n->b = of ? assign() : expression();
        expect(")");
        n->d = statement();
        if (funcCount != fc0) n->flags |= F_CLOSURE;
        return n;
      }
    }
    Node* n = mk(N_FOR, line);
    n->a = init;
    expect(";");
    if (!t().is(";")) n->b = expression();
    expect(";");
    if (!t().is(")")) n->c = expression();
    expect(")");
    n->d = statement();
    if (funcCount != fc0) n->flags |= F_CLOSURE;
    return n;
  }

  // ------------------------------------------------------------ patterns
  Node* propKey(Node* holder) {
    int line = t().line;
    if (t().type == TK_ID) { std::string s(t().p, t().len); next(); return strLit(s, line); }
    if (t().type == TK_STR) { std::string s = t().sv; next(); return strLit(s, line); }
    if (t().type == TK_NUM) { double d = t().num; next(); return strLit(numToStr(d), line); }
    if (eat("[")) {
      Node* e = assign();
      expect("]");
      holder->flags |= F_COMPUTED;
      return e;
    }
    err("unexpected " + tokDesc() + " in property name");
  }

  Node* bindingTarget() {
    int line = t().line;
    if (eat("[")) {
      Node* p = mk(N_ARRPAT, line);
      std::vector<Node*> v;
      while (!eat("]")) {
        if (t().is(",")) { next(); v.push_back(nullptr); continue; }
        if (eat("...")) {
          Node* r = mk(N_REST, t().line);
          r->a = bindingTarget();
          v.push_back(r);
        } else v.push_back(bindingElem());
        if (!eat(",")) { expect("]"); break; }
      }
      setKids(p, v);
      return p;
    }
    if (eat("{")) {
      Node* p = mk(N_OBJPAT, line);
      std::vector<Node*> v;
      while (!eat("}")) {
        if (eat("...")) {
          Node* r = mk(N_REST, t().line);
          r->a = ident();
          v.push_back(r);
        } else {
          Node* pp = mk(N_PATPROP, t().line);
          bool shorthand = t().type == TK_ID;
          const char* nm = shorthand ? atom() : nullptr;
          int ln = t().line;
          pp->a = propKey(pp);
          if (eat(":")) pp->b = bindingElem();
          else {
            if (!shorthand || (pp->flags & F_COMPUTED)) err("expected ':'");
            Node* id = mk(N_ID, ln);
            id->str = nm;
            pp->b = id;
            if (eat("=")) {
              Node* d = mk(N_DEFAULT, ln);
              d->a = id;
              d->b = assign();
              pp->b = d;
            }
          }
          v.push_back(pp);
        }
        if (!eat(",")) { expect("}"); break; }
      }
      setKids(p, v);
      return p;
    }
    return ident();
  }

  Node* bindingElem() {
    Node* tg = bindingTarget();
    if (t().is("=")) {
      Node* d = mk(N_DEFAULT, t().line);
      next();
      d->a = tg;
      d->b = assign();
      return d;
    }
    return tg;
  }

  // convert an expression (array/object literal) into an assignment pattern
  Node* toPattern(Node* e) {
    if (!e) return e;
    switch (e->type) {
      case N_ID: case N_DOT: case N_INDEX: return e;
      case N_DEFAULT: e->a = toPattern(e->a); return e;
      case N_ASSIGN:
        if (e->op != O_ASSIGN) break;
        e->type = N_DEFAULT;
        e->a = toPattern(e->a);
        return e;
      case N_ARR:
        e->type = N_ARRPAT;
        for (int i = 0; i < e->n; i++) {
          Node* k = e->kids[i];
          if (k && k->type == N_SPREAD) { k->type = N_REST; k->a = toPattern(k->a); }
          else e->kids[i] = toPattern(k);
        }
        return e;
      case N_OBJ:
        e->type = N_OBJPAT;
        for (int i = 0; i < e->n; i++) {
          Node* p = e->kids[i];
          if (p->flags & F_SPREAD) { p->type = N_REST; p->a = toPattern(p->b); continue; }
          if (p->flags & (F_GET | F_SET | F_METHOD)) err("invalid destructuring target");
          p->type = N_PATPROP;
          p->b = toPattern(p->b);
        }
        return e;
    }
    err("invalid assignment target");
  }

  // ------------------------------------------------------------ functions & classes
  void params(Node* f) {
    expect("(");
    std::vector<Node*> v;
    while (!eat(")")) {
      if (eat("...")) {
        Node* r = mk(N_REST, t().line);
        r->a = bindingTarget();
        v.push_back(r);
      } else v.push_back(bindingElem());
      if (!eat(",")) { expect(")"); break; }
    }
    setKids(f, v);
  }

  Node* funcBody(Node* f) {
    bool saveIn = noIn;
    noIn = false;
    if (!(f->flags & F_ARROW)) funcs.push_back(f);
    Node* b = block();
    b->flags &= ~F_SCOPE;  // the function env is the scope
    if (!(f->flags & F_ARROW)) funcs.pop_back();
    noIn = saveIn;
    return b;
  }

  void funcRest(Node* f) {  // params + body
    if (!(f->flags & F_ARROW)) funcs.push_back(f);
    params(f);
    if (!(f->flags & F_ARROW)) funcs.pop_back();
    f->a = funcBody(f);
  }

  Node* arrow(bool paren) {
    Node* f = mk(N_FUNC, t().line);
    f->flags |= F_ARROW;
    if (paren) params(f);
    else {
      std::vector<Node*> v{ident()};
      setKids(f, v);
    }
    if (t().nl) err("line break before '=>'");
    expect("=>");
    if (t().is("{")) f->a = funcBody(f);
    else {
      f->flags |= F_CONCISE;
      bool saveIn = noIn;
      noIn = false;
      f->a = assign();
      noIn = saveIn;
    }
    return f;
  }

  bool arrowAhead() {  // at '(' : is this an arrow function parameter list?
    Lexer save = L;
    int depth = 0;
    bool r = false;
    for (;;) {
      if (t().type == TK_EOF) break;
      if (t().is("(") || t().is("[") || t().is("{")) depth++;
      else if (t().is(")") || t().is("]") || t().is("}")) {
        if (--depth == 0) { L.next(); r = t().is("=>") && !t().nl; break; }
      }
      L.next();
    }
    L = save;
    return r;
  }

  Node* classDef(bool isDecl) {
    int line = t().line;
    next();
    Node* c = mk(N_CLASS, line);
    if (isDecl) c->flags |= F_DECL;
    if (t().type == TK_ID && !t().kw("extends")) { c->str = atom(); next(); }
    else if (isDecl) err("class name expected");
    if (t().kw("extends")) { next(); c->a = callExpr(); }
    expect("{");
    std::vector<Node*> members;
    while (!eat("}")) {
      if (eat(";")) continue;
      Node* m = mk(N_MEMBER, t().line);
      if (t().kw("static") && !peekIs("(") && !peekIs("=")) { next(); m->flags |= F_STATIC; }
      if ((t().kw("get") || t().kw("set")) && !peekIs("(") && !peekIs("=") && !peekIs(";") && !peekIs("}")) {
        m->flags |= t().kw("get") ? F_GET : F_SET;
        next();
      }
      if (t().kw("async")) err("async methods are not supported");
      if (t().is("*")) err("generators are not supported");
      m->a = propKey(m);
      if (t().is("(")) {
        Node* fn = mk(N_FUNC, m->line);
        fn->flags |= F_METHOD;
        if (!(m->flags & F_COMPUTED)) fn->str = intern(m->a->lit->s.c_str());
        funcRest(fn);
        if (!(m->flags & (F_STATIC | F_COMPUTED | F_GET | F_SET)) && m->a->lit->s == "constructor") {
          c->b = fn;
          continue;
        }
        m->b = fn;
      } else {
        m->flags |= F_FIELD;
        if (eat("=")) {
          funcs.push_back(m);  // `arguments` is not valid here, just keep the stack sane
          m->b = assign();
          funcs.pop_back();
        }
        semi();
      }
      members.push_back(m);
    }
    setKids(c, members);
    return c;
  }

  // ------------------------------------------------------------ expressions
  Node* expression() {
    Node* e = assign();
    if (!t().is(",")) return e;
    Node* s = mk(N_SEQ, e->line);
    std::vector<Node*> v{e};
    while (eat(",")) v.push_back(assign());
    setKids(s, v);
    return s;
  }

  int assignOp() {
    if (t().type != TK_P) return -1;
    static const struct { const char* p; int op; } ops[] = {
      {"=", O_ASSIGN}, {"+=", O_ADD}, {"-=", O_SUB}, {"*=", O_MUL}, {"/=", O_DIV}, {"%=", O_MOD},
      {"**=", O_EXP}, {"<<=", O_SHL}, {">>=", O_SHR}, {">>>=", O_USHR}, {"&=", O_BAND}, {"|=", O_BOR},
      {"^=", O_BXOR}, {"&&=", O_LAND}, {"||=", O_LOR}, {"?\?=", O_NULLISH}, {nullptr, 0}};
    for (int i = 0; ops[i].p; i++)
      if (t().is(ops[i].p)) return ops[i].op;
    return -1;
  }

  Node* assign() {
    I.stackCheck();
    if (t().type == TK_ID && !isReserved() && peekIs("=>")) return arrow(false);
    if (t().kw("async")) err("async functions are not supported");
    if (t().is("(") && arrowAhead()) return arrow(true);
    int line = t().line;
    Node* left = conditional();
    int op = assignOp();
    if (op < 0) return left;
    if ((left->type == N_ARR || left->type == N_OBJ) && op == O_ASSIGN) left = toPattern(left);
    else if (left->type != N_ID && left->type != N_DOT && left->type != N_INDEX) err("invalid assignment target");
    next();
    Node* n = mk(N_ASSIGN, line);
    n->op = (uint8_t)op;
    n->a = left;
    n->b = assign();
    return n;
  }

  Node* conditional() {
    Node* c = binary(1);
    if (!t().is("?")) return c;
    Node* n = mk(N_COND, t().line);
    next();
    bool saveIn = noIn;
    noIn = false;
    n->a = c;
    n->b = assign();
    noIn = saveIn;
    expect(":");
    n->c = assign();
    return n;
  }

  int binPrec(int& op) {
    Tok& k = t();
    if (k.type == TK_ID) {
      if (k.kw("instanceof")) { op = O_INSTANCEOF; return 8; }
      if (k.kw("in") && !noIn) { op = O_IN; return 8; }
      return -1;
    }
    if (k.type != TK_P) return -1;
    static const struct { const char* p; int op, prec; } ops[] = {
      {"??", O_NULLISH, 1}, {"||", O_LOR, 2}, {"&&", O_LAND, 3}, {"|", O_BOR, 4}, {"^", O_BXOR, 5},
      {"&", O_BAND, 6}, {"==", O_EQ, 7}, {"!=", O_NE, 7}, {"===", O_SEQ, 7}, {"!==", O_SNE, 7},
      {"<", O_LT, 8}, {">", O_GT, 8}, {"<=", O_LE, 8}, {">=", O_GE, 8}, {"<<", O_SHL, 9},
      {">>", O_SHR, 9}, {">>>", O_USHR, 9}, {"+", O_ADD, 10}, {"-", O_SUB, 10}, {"*", O_MUL, 11},
      {"/", O_DIV, 11}, {"%", O_MOD, 11}, {"**", O_EXP, 12}, {nullptr, 0, 0}};
    for (int i = 0; ops[i].p; i++)
      if (k.is(ops[i].p)) { op = ops[i].op; return ops[i].prec; }
    return -1;
  }

  Node* binary(int minPrec) {
    Node* left = unary();
    for (;;) {
      int op;
      int p = binPrec(op);
      if (p < 0 || p < minPrec) break;
      int line = t().line;
      next();
      Node* right = op == O_EXP ? binary(p) : binary(p + 1);
      Node* n = mk((op == O_LAND || op == O_LOR || op == O_NULLISH) ? N_LOGIC : N_BIN, line);
      n->op = (uint8_t)op;
      n->a = left;
      n->b = right;
      left = n;
    }
    return left;
  }

  Node* unary() {
    I.stackCheck();
    Tok& k = t();
    int line = k.line;
    int op = -1;
    if (k.type == TK_P) {
      if (k.is("!")) op = O_NOT;
      else if (k.is("-")) op = O_NEG;
      else if (k.is("+")) op = O_PLUS;
      else if (k.is("~")) op = O_BNOT;
      else if (k.is("++") || k.is("--")) {
        Node* n = mk(N_UPDATE, line);
        n->op = k.is("++") ? O_ADD : O_SUB;
        n->flags |= F_PREFIX;
        next();
        n->a = unary();
        if (n->a->type != N_ID && n->a->type != N_DOT && n->a->type != N_INDEX) err("invalid update target");
        return n;
      }
    } else if (k.type == TK_ID) {
      if (k.kw("typeof")) op = O_TYPEOF;
      else if (k.kw("void")) op = O_VOID;
      else if (k.kw("delete")) op = O_DELETE;
      else if (k.kw("await")) err("await is not supported");
    }
    if (op >= 0) {
      next();
      Node* n = mk(N_UNARY, line);
      n->op = (uint8_t)op;
      n->a = unary();
      return n;
    }
    Node* e = callExpr();
    if (t().type == TK_P && (t().is("++") || t().is("--")) && !t().nl) {
      if (e->type != N_ID && e->type != N_DOT && e->type != N_INDEX) err("invalid update target");
      Node* n = mk(N_UPDATE, t().line);
      n->op = t().is("++") ? O_ADD : O_SUB;
      n->a = e;
      next();
      return n;
    }
    return e;
  }

  Node* dot(Node* obj, bool optional) {
    if (t().type != TK_ID) err("expected property name but found " + tokDesc());
    Node* n = mk(N_DOT, t().line);
    n->a = obj;
    n->str = intern(t().p, t().len);
    n->n = (uint16_t)t().len;
    if (optional) n->flags |= F_OPTIONAL;
    next();
    return n;
  }

  void args(Node* call) {
    expect("(");
    bool saveIn = noIn;
    noIn = false;
    std::vector<Node*> v;
    while (!eat(")")) {
      if (t().is("...")) {
        Node* s = mk(N_SPREAD, t().line);
        next();
        s->a = assign();
        v.push_back(s);
      } else v.push_back(assign());
      if (!eat(",")) { expect(")"); break; }
    }
    noIn = saveIn;
    setKids(call, v);
  }

  Node* callExpr() {
    Node* e;
    bool chain = false;
    if (t().kw("new")) e = newExpr();
    else e = primary();
    for (;;) {
      int line = t().line;
      if (eat(".")) e = dot(e, false);
      else if (t().is("?.")) {
        next();
        chain = true;
        if (t().is("(")) {
          Node* c = mk(N_CALL, line);
          c->a = e;
          c->flags |= F_OPTIONAL;
          args(c);
          e = c;
        } else if (eat("[")) {
          Node* x = mk(N_INDEX, line);
          x->a = e;
          x->b = expression();
          x->flags |= F_OPTIONAL;
          expect("]");
          e = x;
        } else e = dot(e, true);
      } else if (t().is("[")) {
        next();
        Node* x = mk(N_INDEX, line);
        x->a = e;
        bool saveIn = noIn;
        noIn = false;
        x->b = expression();
        noIn = saveIn;
        expect("]");
        e = x;
      } else if (t().is("(")) {
        Node* c = mk(N_CALL, line);
        c->a = e;
        args(c);
        e = c;
      } else if (t().type == TK_TMPL) err("tagged templates are not supported");
      else break;
    }
    if (chain) {
      Node* o = mk(N_OPTCHAIN, e->line);
      o->a = e;
      return o;
    }
    return e;
  }

  Node* newExpr() {
    int line = t().line;
    next();
    if (t().is(".")) err("new.target is not supported");
    Node* callee = t().kw("new") ? newExpr() : primary();
    for (;;) {
      int ln = t().line;
      if (eat(".")) callee = dot(callee, false);
      else if (eat("[")) {
        Node* x = mk(N_INDEX, ln);
        x->a = callee;
        x->b = expression();
        expect("]");
        callee = x;
      } else break;
    }
    Node* n = mk(N_NEW, line);
    n->a = callee;
    if (t().is("(")) args(n);
    return n;
  }

  Node* primary() {
    Tok& k = t();
    int line = k.line;
    switch (k.type) {
      case TK_NUM: {
        Node* n = mk(N_NUM, line);
        n->setNum(k.num);
        next();
        return n;
      }
      case TK_STR: {
        std::string s = k.sv;
        next();
        return strLit(s, line);
      }
      case TK_TMPL: {
        Tok tk = k;  // copy: we parse sub-expressions with the lexer
        next();
        Node* n = mk(N_TMPL, line);
        std::vector<Node*> v;
        for (size_t i = 0; i < tk.quasis.size(); i++) {
          if (!tk.quasis[i].empty()) v.push_back(strLit(tk.quasis[i], line));
          if (i < tk.exprStart.size()) {
            Lexer save = L;
            L.pos = tk.exprStart[i];
            L.end = tk.exprStart[i] + tk.exprLen[i];
            L.line = tk.exprLine[i];
            L.next();
            v.push_back(expression());
            if (t().type != TK_EOF) err("unexpected " + tokDesc() + " in template");
            L = save;
          }
        }
        setKids(n, v);
        return n;
      }
      case TK_ID: {
        if (k.kw("function")) {
          next();
          Node* f = mk(N_FUNC, line);
          if (t().is("*")) err("generators are not supported");
          if (t().type == TK_ID) { f->str = atom(); next(); }
          funcRest(f);
          return f;
        }
        if (k.kw("class")) return classDef(false);
        if (k.kw("this")) { next(); return mk(N_THIS, line); }
        if (k.kw("super")) { next(); return mk(N_SUPER, line); }
        if (k.kw("true")) { next(); return mk(N_TRUE, line); }
        if (k.kw("false")) { next(); return mk(N_FALSE, line); }
        if (k.kw("null")) { next(); return mk(N_NULL, line); }
        if (isReserved()) err("unexpected " + tokDesc());
        return ident();
      }
      case TK_P: {
        if (k.is("(")) {
          next();
          bool saveIn = noIn;
          noIn = false;
          Node* e = expression();
          noIn = saveIn;
          expect(")");
          return e;
        }
        if (k.is("[")) {
          next();
          Node* a = mk(N_ARR, line);
          std::vector<Node*> v;
          bool saveIn = noIn;
          noIn = false;
          while (!eat("]")) {
            if (t().is(",")) { next(); v.push_back(nullptr); continue; }
            if (t().is("...")) {
              Node* s = mk(N_SPREAD, t().line);
              next();
              s->a = assign();
              v.push_back(s);
            } else v.push_back(assign());
            if (!eat(",")) { expect("]"); break; }
          }
          noIn = saveIn;
          setKids(a, v);
          return a;
        }
        if (k.is("{")) return objectLit();
        break;
      }
      default: break;
    }
    err("unexpected " + tokDesc());
  }

  Node* objectLit() {
    Node* o = mk(N_OBJ, t().line);
    next();
    bool saveIn = noIn;
    noIn = false;
    std::vector<Node*> v;
    while (!eat("}")) {
      Node* p = mk(N_PROP, t().line);
      if (eat("...")) {
        p->flags |= F_SPREAD;
        p->b = assign();
      } else {
        if ((t().kw("get") || t().kw("set")) && !peekIs(":") && !peekIs("(") && !peekIs(",") && !peekIs("}")) {
          p->flags |= t().kw("get") ? F_GET : F_SET;
          next();
        }
        if (t().kw("async") && !peekIs(":") && !peekIs("(") && !peekIs(",") && !peekIs("}")) err("async methods are not supported");
        bool shorthand = t().type == TK_ID;
        const char* nm = shorthand ? atom() : nullptr;
        int ln = t().line;
        p->a = propKey(p);
        if (t().is("(")) {
          Node* fn = mk(N_FUNC, ln);
          fn->flags |= F_METHOD;
          if (!(p->flags & F_COMPUTED)) fn->str = intern(p->a->lit->s.c_str());
          funcRest(fn);
          p->b = fn;
          if (!(p->flags & (F_GET | F_SET))) p->flags |= F_METHOD;
        } else if (p->flags & (F_GET | F_SET)) err("expected '('");
        else if (eat(":")) p->b = assign();
        else if (shorthand && !(p->flags & F_COMPUTED)) {
          Node* id = mk(N_ID, ln);
          id->str = nm;
          if (nm == A_arguments && !funcs.empty()) funcs.back()->flags |= F_ARGS;
          p->b = id;
          if (t().is("=")) {  // only valid as a destructuring pattern
            Node* d = mk(N_DEFAULT, ln);
            next();
            d->a = id;
            d->b = assign();
            p->b = d;
          }
        } else err("expected ':' but found " + tokDesc());
      }
      v.push_back(p);
      if (!eat(",")) { expect("}"); break; }
    }
    noIn = saveIn;
    setKids(o, v);
    return o;
  }
};

bool Interp::isIncomplete(const std::string& src) {
  int depth = 0;
  size_t i = 0, n = src.size();
  std::vector<char> stack;
  while (i < n) {
    char c = src[i];
    if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') i++; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') {
      size_t e = src.find("*/", i + 2);
      if (e == std::string::npos) return true;
      i = e + 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      i++;
      while (i < n && src[i] != c && src[i] != '\n') { if (src[i] == '\\') i++; i++; }
      i++;
      continue;
    }
    if (c == '`') {
      i++;
      while (i < n && src[i] != '`') { if (src[i] == '\\') i++; i++; }
      if (i >= n) return true;
      i++;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') depth--;
    i++;
  }
  if (depth > 0) return true;
  // a trailing binary operator or comma means the expression continues on the next line
  size_t e = src.find_last_not_of(" \t\r\n");
  if (e == std::string::npos) return false;
  if (e >= 1 && ((src[e] == '+' && src[e - 1] == '+') || (src[e] == '-' && src[e - 1] == '-'))) return false;
  return strchr("+-*/%=&|^<>?:,.!", src[e]) != nullptr;
}

// =================================================================== GC tracing
void Object::trace(Interp& I) {
  I.markObj(proto);
  for (auto& p : props) I.markValue(p.v);
}
void Array::trace(Interp& I) {
  Object::trace(I);
  for (auto& v : items) I.markValue(v);
}
void Native::trace(Interp& I) {
  Object::trace(I);
  I.markValue(data);
}
void Function::trace(Interp& I) {
  Object::trace(I);
  I.markObj(env);
  I.markObj(home);
  I.markValue(superCls);
}
void MapObj::trace(Interp& I) {
  Object::trace(I);
  for (auto& e : entries) { I.markValue(e.first); I.markValue(e.second); }
}
void Env::trace(Interp& I) {
  I.markObj(parent);
  I.markObj(func);
  I.markValue(thisV);
  for (auto& b : vars) I.markValue(b.v);
}

void Interp::collect() {
  gcPending = false;
  markObj(global);
  for (GcObj* o : temps) markObj(o);
  markValue(retVal);
  markValue(lastResult);
  markObj(oomError);
  for (auto& v : roots) markValue(v);
  Object* protos[] = {ObjectProto, FunctionProto, ArrayProto, StringProto, NumberProto, BooleanProto,
                      ErrorProto, TypeErrorProto, RangeErrorProto, ReferenceErrorProto, SyntaxErrorProto,
                      MapProto, SetProto, DateProto};
  for (Object* p : protos) markObj(p);
  for (Str* s : typeStr) markObj(s);
  for (Str* s : charCache) markObj(s);
  for (Arena* a : arenas)
    for (Str* s : a->lits) markObj(s);
  for (auto& t : timers) {
    markValue(t.fn);
    for (auto& a : t.args) markValue(a);
  }
  while (!gray.empty()) {
    GcObj* o = gray.back();
    gray.pop_back();
    o->trace(*this);
  }
  size_t live = 0;
  GcObj** pp = &heap;
  while (*pp) {
    GcObj* o = *pp;
    if (!o->marked) {
      *pp = o->gcNext;
      delete o;
    } else {
      o->marked = false;
      live += o->gcSize();
      pp = &o->gcNext;
    }
  }
  liveBytes = live;
  allocBytes = 0;
  gcThreshold = gcStress ? 0 : std::max(gcMin, std::min(live / 2, gcMax));
  if (onCollect) onCollect();
}

// =================================================================== allocation
Str* Interp::allocStr(const char* s, size_t n) {
  Str* o = gcNew<Str>();
  o->s.assign(s, n);
  allocBytes += n;
  return o;
}

Value Interp::charStr(unsigned char c) {
  if (c < 128) {
    if (!charCache[c]) {
      char ch = (char)c;
      charCache[c] = allocStr(&ch, 1);
    }
    return Value::str(charCache[c]);
  }
  char ch = (char)c;
  return Value::str(allocStr(&ch, 1));
}

Object* Interp::newObject(Object* proto) {
  Object* o = gcNew<Object>();
  o->proto = proto;
  return o;
}

Array* Interp::newArray() {
  Array* a = gcNew<Array>();
  a->proto = ArrayProto;
  return a;
}

Native* Interp::newNative(NativeFn fn, const char* name, Value data) {
  Native* f = gcNew<Native>();
  f->proto = FunctionProto;
  f->fn = fn;
  f->name = name;
  f->data = data;
  return f;
}

Native* Interp::def(Object* o, const char* name, NativeFn fn) {
  Native* f = newNative(fn, name);
  setOwn(o, name, Value::obj(f));
  return f;
}

void Interp::setOwn(Object* o, const char* k, size_t n, Value v) {
  const char* a = intern(k, n);
  Prop* p = o->ownA(a);
  if (p) { p->v = v; p->accessor = false; return; }
  o->props.push_back(Prop{a, false, v});
}

void Interp::setGlobal(const char* name, Value v) { declare(global, intern(name), v, V_LET); }

Value Interp::getGlobal(const char* name) {
  Binding* b = lookup(global, intern(name));
  return b ? b->v : Value();
}

Env* Interp::newEnv(Env* parent) {
  Env* e = gcNew<Env>();
  e->parent = parent;
  return e;
}

void Interp::stackCheck() {
  char probe;
  uintptr_t sp = (uintptr_t)&probe;
  if (stackBase && stackBase > sp && stackBase - sp > stackLimit)
    throwError("RangeError", "Maximum call stack size exceeded");
}

[[noreturn]] void Interp::throwError(const char* type, const std::string& msg) {
  Object* proto = ErrorProto;
  if (!strcmp(type, "TypeError")) proto = TypeErrorProto;
  else if (!strcmp(type, "RangeError")) proto = RangeErrorProto;
  else if (!strcmp(type, "ReferenceError")) proto = ReferenceErrorProto;
  else if (!strcmp(type, "SyntaxError")) proto = SyntaxErrorProto;
  Object* o = newObject(proto);
  setOwn(o, "message", str(msg));
  throw JsThrow{Value::obj(o), line};
}

// =================================================================== conversions
static bool parseNumStr(const std::string& s, double& out) {
  size_t i = 0, n = s.size();
  while (i < n && isspace((unsigned char)s[i])) i++;
  while (n > i && isspace((unsigned char)s[n - 1])) n--;
  if (i == n) { out = 0; return true; }
  std::string t = s.substr(i, n - i);
  const char* p = t.c_str();
  bool neg = false;
  if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
  if (!strcmp(p, "Infinity")) { out = neg ? -INFINITY : INFINITY; return true; }
  if (p[0] == '0' && (p[1] | 32) && strchr("xob", p[1] | 32) && p[1]) {
    if (p != t.c_str()) return false;  // no sign allowed
    int base = (p[1] | 32) == 'x' ? 16 : (p[1] | 32) == 'o' ? 8 : 2;
    double v = 0;
    const char* q = p + 2;
    if (!*q) return false;
    for (; *q; q++) {
      int d = hexVal(*q);
      if (d >= base) return false;
      v = v * base + d;
    }
    out = v;
    return true;
  }
  // validate decimal literal
  const char* q = p;
  bool digits = false;
  while (isdigit((unsigned char)*q)) { q++; digits = true; }
  if (*q == '.') { q++; while (isdigit((unsigned char)*q)) { q++; digits = true; } }
  if (!digits) return false;
  if (*q == 'e' || *q == 'E') {
    q++;
    if (*q == '+' || *q == '-') q++;
    if (!isdigit((unsigned char)*q)) return false;
    while (isdigit((unsigned char)*q)) q++;
  }
  if (*q) return false;
  out = strtod(t.c_str(), nullptr);
  return true;
}

double Interp::toNum(Value v) {
  switch (v.t) {
    case T::Num: return v.n;
    case T::Undef: return NAN;
    case T::Null: return 0;
    case T::Bool: return v.b ? 1 : 0;
    case T::Str: {
      double d;
      return parseNumStr(v.s()->s, d) ? d : NAN;
    }
    case T::Obj: return toNum(toPrimitive(v, true));
  }
  return NAN;
}

int32_t Interp::toInt32(Value v) {
  double d = v.isNum() ? v.n : toNum(v);
  if (!std::isfinite(d)) return 0;
  d = std::trunc(d);
  if (d >= -2147483648.0 && d <= 2147483647.0) return (int32_t)d;
  d = std::fmod(d, 4294967296.0);
  if (d < 0) d += 4294967296.0;
  return (int32_t)(uint32_t)d;
}

std::string Interp::toStr(Value v) {
  switch (v.t) {
    case T::Undef: return "undefined";
    case T::Null: return "null";
    case T::Bool: return v.b ? "true" : "false";
    case T::Num: return numToStr(v.n);
    case T::Str: return v.s()->s;
    case T::Obj: return toStr(toPrimitive(v, false));
  }
  return "";
}

std::string Interp::toKey(Value v) {
  if (v.isStr()) return v.s()->s;
  if (v.isNum()) return numToStr(v.n);
  return toStr(v);
}

bool Interp::truthy(Value v) {
  switch (v.t) {
    case T::Undef: case T::Null: return false;
    case T::Bool: return v.b;
    case T::Num: return v.n != 0 && !std::isnan(v.n);
    case T::Str: return !v.s()->s.empty();
    case T::Obj: return true;
  }
  return false;
}

Value Interp::toPrimitive(Value v, bool numberHint) {
  if (!v.isObj()) return v;
  const char* order[2] = {"valueOf", "toString"};
  if (!numberHint) std::swap(order[0], order[1]);
  for (const char* m : order) {
    Value f = getProp(v, m, strlen(m));
    if (isCallable(f)) {
      Value r = call(f, v, nullptr, 0);
      if (!r.isObj()) return r;
    }
  }
  throwError("TypeError", "Cannot convert object to primitive value");
}

const char* Interp::typeOf(Value v) {
  switch (v.t) {
    case T::Undef: return "undefined";
    case T::Null: return "object";
    case T::Bool: return "boolean";
    case T::Num: return "number";
    case T::Str: return "string";
    case T::Obj: return isCallable(v) ? "function" : "object";
  }
  return "undefined";
}

bool Interp::strictEq(Value a, Value b) {
  if (a.t != b.t) return false;
  switch (a.t) {
    case T::Undef: case T::Null: return true;
    case T::Bool: return a.b == b.b;
    case T::Num: return a.n == b.n;
    case T::Str: return a.g == b.g || a.s()->s == b.s()->s;
    case T::Obj: return a.g == b.g;
  }
  return false;
}

bool Interp::looseEq(Value a, Value b) {
  if (a.t == b.t) return strictEq(a, b);
  if (a.isNullish() && b.isNullish()) return true;
  if (a.isNullish() || b.isNullish()) return false;
  if (a.isObj()) return looseEq(toPrimitive(a, true), b);
  if (b.isObj()) return looseEq(a, toPrimitive(b, true));
  return toNum(a) == toNum(b);
}

bool Interp::instanceOf(Value v, Value ctor) {
  if (!isCallable(ctor)) throwError("TypeError", "Right-hand side of 'instanceof' is not callable");
  if (!v.isObj()) return false;
  Value p = getProp(ctor, "prototype", 9);
  if (!p.isObj()) return false;
  for (Object* o = v.o()->proto; o; o = o->proto)
    if (o == p.o()) return true;
  return false;
}

// =================================================================== cold error paths
// Kept out of line so the hot evaluator functions have small stack frames.
#define JS_COLD __attribute__((noinline, cold))
[[noreturn]] JS_COLD static void fail(Interp& I, const char* type, const char* msg) { I.throwError(type, msg); }
[[noreturn]] JS_COLD static void failNullish(Interp& I, Value base, const char* k, size_t n, const char* verb,
                                             const char* ing) {
  I.throwError("TypeError", std::string("Cannot ") + verb + " properties of " + (base.isNull() ? "null" : "undefined") +
                                " (" + ing + " '" + std::string(k, n) + "')");
}
[[noreturn]] JS_COLD static void failUndefined(Interp& I, const char* name) {
  I.throwError("ReferenceError", std::string(name) + " is not defined");
}
[[noreturn]] JS_COLD static void failConst(Interp& I, const char* name) {
  I.throwError("TypeError", std::string("Assignment to constant variable '") + name + "'");
}
[[noreturn]] JS_COLD static void failNotCallable(Interp& I, Node* n, const char* what) {
  I.throwError("TypeError", I.describe(n) + what);
}
[[noreturn]] JS_COLD static void failCall(Interp& I, Value fv) {
  if (fv.isObj() && fv.o()->kind == K::Function) {
    Function* f = (Function*)fv.o();
    I.throwError("TypeError", std::string("Class constructor ") + (f->name ? f->name : "") + " cannot be invoked without 'new'");
  }
  I.throwError("TypeError", I.toStr(fv) + " is not a function");
}

// =================================================================== properties
static long arrayIndex(const char* k, size_t n) {
  if (n == 0 || n > 9) return -1;
  if (n > 1 && k[0] == '0') return -1;
  long v = 0;
  for (size_t i = 0; i < n; i++) {
    if (!isdigit((unsigned char)k[i])) return -1;
    v = v * 10 + (k[i] - '0');
  }
  return v;
}

Prop* Interp::resolveLazy(Object* o, const char* atom) {
  for (const NativeEntry* e = o->lazy; e->name; e++)
    if (!strcmp(e->name, atom)) {
      Native* f = newNative(e->fn, e->name);
      o->props.push_back(Prop{atom, false, Value::obj(f)});
      return &o->props.back();
    }
  return nullptr;
}

Value Interp::lookupFrom(Object* o, const char* atom, Value receiver) {
  for (; o; o = o->proto) {
    Prop* p = o->ownA(atom);
    if (!p && o->lazy) p = resolveLazy(o, atom);
    if (p) {
      if (p->accessor) {
        Value getter = ((Array*)p->v.o())->items[0];
        return getter.isUndef() ? Value() : call(getter, receiver, nullptr, 0);
      }
      return p->v;
    }
  }
  return Value();
}

bool Interp::specialGet(Object* o, const char* k, size_t n, Value& r) {
  switch (o->kind) {
    case K::Array: {
      Array* a = (Array*)o;
      if (n == 6 && !memcmp(k, "length", 6)) { r = Value::num((double)a->items.size()); return true; }
      long i = arrayIndex(k, n);
      if (i >= 0) { r = (size_t)i < a->items.size() ? a->items[i] : Value(); return true; }
      return false;
    }
    case K::Function: {
      Function* f = (Function*)o;
      if (o->own(k, n)) return false;
      if (n == 4 && !memcmp(k, "name", 4)) { r = str(f->name ? f->name : ""); return true; }
      if (n == 6 && !memcmp(k, "length", 6)) {
        int c = 0;
        if (f->node)
          for (int i = 0; i < f->node->n; i++) {
            uint8_t t = f->node->kids[i]->type;
            if (t == N_DEFAULT || t == N_REST) break;
            c++;
          }
        r = Value::num(c);
        return true;
      }
      if (n == 9 && !memcmp(k, "prototype", 9) && !f->isArrow) {
        Object* P = newObject();
        setOwn(P, "constructor", Value::obj(f));
        setOwn(f, "prototype", Value::obj(P));
        r = Value::obj(P);
        return true;
      }
      return false;
    }
    case K::Native: {
      if (n == 4 && !memcmp(k, "name", 4) && !o->own(k, n)) { r = str(((Native*)o)->name); return true; }
      return false;
    }
    case K::Map: case K::Set: {
      if (n == 4 && !memcmp(k, "size", 4)) { r = Value::num((double)((MapObj*)o)->entries.size()); return true; }
      return false;
    }
    default: return false;
  }
}

Value Interp::getProp(Value base, const char* k, size_t n, const char* atom) {
  Object* start;
  switch (base.t) {
    case T::Obj: {
      Object* o = base.o();
      if (o->kind != K::Plain) {
        Value r;
        if (specialGet(o, k, n, r)) return r;
      }
      start = o;
      break;
    }
    case T::Str: {
      const std::string& s = base.s()->s;
      if (n == 6 && !memcmp(k, "length", 6)) return Value::num((double)s.size());
      long i = arrayIndex(k, n);
      if (i >= 0) return (size_t)i < s.size() ? charStr((unsigned char)s[i]) : Value();
      start = StringProto;
      break;
    }
    case T::Num: start = NumberProto; break;
    case T::Bool: start = BooleanProto; break;
    default:
      failNullish(*this, base, k, n, "read", "reading");
  }
  if (!atom && !(atom = findAtom(k, n))) {
    // A name no script has used yet can still be a lazy built-in (e.g. native code asking for
    // JSON.parse); anything else can't exist.
    for (Object* o = start; o && !atom; o = o->proto)
      for (const NativeEntry* e = o->lazy; e && e->name; e++)
        if (strlen(e->name) == n && !memcmp(e->name, k, n)) { atom = intern(k, n); break; }
    if (!atom) return Value();
  }
  return lookupFrom(start, atom, base);
}

Value Interp::getIndex(Value base, Value key) {
  if (key.isNum()) {
    double d = key.n;
    if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) {
      size_t i = (size_t)d;
      if (base.isObj() && base.o()->kind == K::Array) {
        auto& it = ((Array*)base.o())->items;
        return i < it.size() ? it[i] : Value();
      }
      if (base.isStr()) {
        const std::string& s = base.s()->s;
        return i < s.size() ? charStr((unsigned char)s[i]) : Value();
      }
    }
  }
  std::string k = toKey(key);
  return getProp(base, k.data(), k.size());
}

void Interp::setIndex(Array* a, size_t i, Value v) {
  if (i >= a->items.size()) {
    if (i > 10000000) fail(*this, "RangeError", "Array index too large");
    a->items.resize(i + 1);
  }
  a->items[i] = v;
}

void Interp::putProp(Value base, const char* k, size_t n, Value v, const char* atom) {
  if (!base.isObj()) {
    if (base.isNullish())
      failNullish(*this, base, k, n, "set", "setting");
    return;
  }
  Object* o = base.o();
  if (o->kind == K::Array) {
    Array* a = (Array*)o;
    if (n == 6 && !memcmp(k, "length", 6)) {
      double d = toNum(v);
      if (d < 0 || d != std::floor(d) || d > 10000000) fail(*this, "RangeError", "Invalid array length");
      a->items.resize((size_t)d);
      return;
    }
    long i = arrayIndex(k, n);
    if (i >= 0) { setIndex(a, (size_t)i, v); return; }
  }
  if (!atom) atom = intern(k, n);
  for (Object* q = o; q; q = q->proto) {
    Prop* p = q->ownA(atom);
    if (!p) continue;
    if (p->accessor) {
      Value setter = ((Array*)p->v.o())->items[1];
      if (!setter.isUndef()) call(setter, base, &v, 1);
      return;
    }
    if (q == o) { p->v = v; return; }
    break;  // inherited data property: shadow it with an own one
  }
  o->props.push_back(Prop{atom, false, v});
}

void Interp::putIndex(Value base, Value key, Value v) {
  if (key.isNum() && base.isObj() && base.o()->kind == K::Array) {
    double d = key.n;
    if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) {
      setIndex((Array*)base.o(), (size_t)d, v);
      return;
    }
  }
  std::string k = toKey(key);
  putProp(base, k.data(), k.size(), v);
}

bool Interp::delProp(Value base, const std::string& k) {
  if (!base.isObj()) return true;
  Object* o = base.o();
  if (o->kind == K::Array) {
    long i = arrayIndex(k.data(), k.size());
    if (i >= 0) {
      auto& it = ((Array*)o)->items;
      if ((size_t)i < it.size()) it[i] = Value();
      return true;
    }
  }
  const char* a = findAtom(k.data(), k.size());
  for (size_t i = 0; a && i < o->props.size(); i++)
    if (o->props[i].k == a) { o->props.erase(o->props.begin() + i); return true; }
  return true;
}

bool Interp::hasProp(Value base, const std::string& k) {
  if (!base.isObj()) throwError("TypeError", "Cannot use 'in' operator to search for '" + k + "' in " + toStr(base));
  Object* o = base.o();
  if (o->kind == K::Array) {
    if (k == "length") return true;
    long i = arrayIndex(k.data(), k.size());
    if (i >= 0) return (size_t)i < ((Array*)o)->items.size();
  }
  Value r;
  if (o->kind != K::Plain && specialGet(o, k.data(), k.size(), r)) return true;
  const char* a = findAtom(k.data(), k.size());
  for (; o; o = o->proto) {
    if (a && o->ownA(a)) return true;
    if (o->lazy)
      for (const NativeEntry* e = o->lazy; e->name; e++)
        if (k == e->name) return true;
  }
  return false;
}

void Interp::defineAccessor(Object* o, const std::string& k, Value fn, bool setter) {
  const char* a = intern(k.data(), k.size());
  Prop* p = o->ownA(a);
  if (!p || !p->accessor) {
    Array* pair = newArray();
    pair->items.resize(2);
    if (p) { p->accessor = true; p->v = Value::obj(pair); }
    else o->props.push_back(Prop{a, true, Value::obj(pair)});
    p = o->ownA(a);
  }
  ((Array*)p->v.o())->items[setter ? 1 : 0] = fn;
}

std::vector<std::string> Interp::ownKeys(Value v) {
  std::vector<std::string> r;
  if (v.isStr()) {
    for (size_t i = 0; i < v.s()->s.size(); i++) r.push_back(std::to_string(i));
    return r;
  }
  if (!v.isObj()) return r;
  Object* o = v.o();
  if (o->kind == K::Array) {
    size_t n = ((Array*)o)->items.size();
    for (size_t i = 0; i < n; i++) r.push_back(std::to_string(i));
  }
  for (auto& p : o->props) r.push_back(p.k);
  if (o->lazy)
    for (const NativeEntry* e = o->lazy; e->name; e++)
      if (!o->own(e->name)) r.push_back(e->name);
  return r;
}

std::vector<Value> Interp::toList(Value v) {
  std::vector<Value> out;
  if (v.isObj()) {
    Object* o = v.o();
    if (o->kind == K::Array) {
      auto& it = ((Array*)o)->items;
      return std::vector<Value>(it.begin(), it.end());
    }
    if (o->kind == K::Map) {
      for (auto& e : ((MapObj*)o)->entries) {
        Array* pair = newArray();
        pair->items = {e.first, e.second};
        out.push_back(Value::obj(pair));
      }
      return out;
    }
    if (o->kind == K::Set) {
      for (auto& e : ((MapObj*)o)->entries) out.push_back(e.first);
      return out;
    }
  } else if (v.isStr()) {
    const std::string& s = v.s()->s;
    for (size_t i = 0; i < s.size();) {  // iterate by UTF-8 code point
      unsigned char c = s[i];
      size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
      if (i + len > s.size()) len = s.size() - i;
      out.push_back(len == 1 ? charStr(c) : str(s.substr(i, len)));
      i += len;
    }
    return out;
  }
  throwError("TypeError", toStr(v) + " is not iterable");
}

double Interp::random() {
  rng ^= rng >> 12;
  rng ^= rng << 25;
  rng ^= rng >> 27;
  return (double)((rng * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}

// =================================================================== scopes
Binding* Interp::lookup(Env* e, const char* name) {
  for (; e; e = e->parent)
    for (auto& b : e->vars)
      if (b.name == name) return &b;
  for (const NativeEntry* t : globalTables)  // built-in global functions, created on first use
    for (const NativeEntry* g = t; g->name; g++)
      if (!strcmp(g->name, name)) {
        global->vars.push_back(Binding{name, false, Value::obj(newNative(g->fn, g->name))});
        return &global->vars.back();
      }
  return nullptr;
}

void Interp::declare(Env* e, const char* name, Value v, int kind) {
  if (kind == V_VAR)
    while (!e->funcScope && e->parent) e = e->parent;
  for (auto& b : e->vars)
    if (b.name == name) { b.v = v; b.isConst = kind == V_CONST; return; }
  e->vars.push_back(Binding{name, kind == V_CONST, v});
}

void Interp::assignVar(Env* e, const char* name, Value v) {
  Binding* b = lookup(e, name);
  if (!b) { global->vars.push_back(Binding{name, false, v}); return; }
  if (b->isConst) failConst(*this, name);
  b->v = v;
}

Env* Interp::copyEnv(Env* e) {
  Env* c = newEnv(e->parent);
  c->vars = e->vars;
  return c;
}

Value Interp::thisOf(Env* e) {
  for (; e; e = e->parent)
    if (e->hasThis) return e->thisV;
  return Value();
}

Function* Interp::homeFunc(Env* e) {
  for (; e; e = e->parent)
    if (e->hasThis) return e->func;
  return nullptr;
}

void Interp::nameIfAnon(Value v, Node* init, const char* name) {
  if (!init || !v.isObj() || v.o()->kind != K::Function) return;
  if ((init->type == N_FUNC || init->type == N_CLASS) && !init->str) {
    Function* f = (Function*)v.o();
    if (!f->name) f->name = name;
  }
}

void Interp::bindPattern(Node* p, Value v, Env* e, int kind) {
  switch (p->type) {
    case N_ID:
      if (kind == V_ASSIGN) assignVar(e, p->str, v);
      else declare(e, p->str, v, kind);
      return;
    case N_DOT: case N_INDEX:
      if (kind != V_ASSIGN) break;
      setRef(p, e, p->type == N_DOT ? Value() : Value(), Value(), v);
      return;
    case N_DEFAULT:
      if (v.isUndef()) {
        v = ev(p->b, e);
        if (p->a->type == N_ID) nameIfAnon(v, p->b, p->a->str);
      }
      bindPattern(p->a, v, e, kind);
      return;
    case N_ARRPAT: {
      rootTemp(v);
      std::vector<Value> items = toList(v);
      for (int i = 0; i < p->n; i++) {
        Node* k = p->kids[i];
        if (!k) continue;
        if (k->type == N_REST) {
          Array* r = newArray();
          for (size_t j = i; j < items.size(); j++) r->items.push_back(items[j]);
          bindPattern(k->a, Value::obj(r), e, kind);
          break;
        }
        bindPattern(k, (size_t)i < items.size() ? items[i] : Value(), e, kind);
      }
      return;
    }
    case N_OBJPAT: {
      if (v.isNullish()) throwError("TypeError", "Cannot destructure '" + toStr(v) + "'");
      rootTemp(v);
      std::vector<std::string> used;
      for (int i = 0; i < p->n; i++) {
        Node* k = p->kids[i];
        if (k->type == N_REST) {
          Object* r = newObject();
          for (auto& key : ownKeys(v))
            if (std::find(used.begin(), used.end(), key) == used.end())
              setOwn(r, key.c_str(), key.size(), getProp(v, key.data(), key.size()));
          bindPattern(k->a, Value::obj(r), e, kind);
          break;
        }
        std::string key = (k->flags & F_COMPUTED) ? toKey(ev(k->a, e)) : k->a->lit->s;
        used.push_back(key);
        bindPattern(k->b, getProp(v, key.data(), key.size()), e, kind);
      }
      return;
    }
    default: break;
  }
  fail(*this, "SyntaxError", "invalid binding target");
}

// =================================================================== functions
Function* Interp::makeFunction(Node* fn, Env* e) {
  Function* f = gcNew<Function>();
  f->proto = FunctionProto;
  f->node = fn;
  f->env = e;
  f->arena = curArena->shared_from_this();
  f->isArrow = (fn->flags & F_ARROW) != 0;
  f->name = fn->str;
  return f;
}

struct ArenaScope {
  Interp& I;
  Arena* saved;
  int savedLine;
  ArenaScope(Interp& i, Arena* a) : I(i), saved(i.curArena), savedLine(i.line) { I.curArena = a; }
  ~ArenaScope() { I.curArena = saved; I.line = savedLine; }
};

JS_NOINLINE Function* Interp::makeClass(Node* n, Env* e) {
  Value superV;
  Object* protoParent = ObjectProto;
  Object* ctorParent = FunctionProto;
  if (n->a) {
    superV = ev(n->a, e);
    if (superV.isNull()) protoParent = nullptr;
    else if (isCallable(superV)) {
      Value pp = getProp(superV, "prototype", 9);
      protoParent = pp.isObj() ? pp.o() : nullptr;
      ctorParent = superV.o();
    } else throwError("TypeError", "Class extends value " + toStr(superV) + " is not a constructor or null");
  }
  Env* ce = newEnv(e);
  Function* C = gcNew<Function>();
  C->proto = ctorParent;
  C->node = n->b;
  C->cls = n;
  C->env = ce;
  C->arena = curArena->shared_from_this();
  C->isClass = true;
  C->isDerived = n->a != nullptr;
  C->superCls = superV;
  C->name = n->str;
  Object* P = newObject(protoParent);
  C->home = P;
  setOwn(C, "prototype", Value::obj(P));
  setOwn(P, "constructor", Value::obj(C));
  if (n->str) declare(ce, n->str, Value::obj(C), V_CONST);
  Env* se = nullptr;  // `this` = class for static initializers
  for (int i = 0; i < n->n; i++) {
    Node* m = n->kids[i];
    bool isStatic = m->flags & F_STATIC;
    Object* target = isStatic ? (Object*)C : P;
    std::string key = (m->flags & F_COMPUTED) ? toKey(ev(m->a, ce)) : m->a->lit->s;
    if (m->flags & F_FIELD) {
      if (!isStatic) continue;
      if (!se) {
        se = newEnv(ce);
        se->funcScope = se->hasThis = true;
        se->thisV = Value::obj(C);
        se->func = C;
      }
      Value v = m->b ? ev(m->b, se) : Value();
      nameIfAnon(v, m->b, intern(key.c_str()));
      setOwn(target, key.c_str(), key.size(), v);
      continue;
    }
    Function* f = makeFunction(m->b, ce);
    f->home = target;
    if (m->flags & (F_GET | F_SET)) defineAccessor(target, key, Value::obj(f), m->flags & F_SET);
    else setOwn(target, key.c_str(), key.size(), Value::obj(f));
  }
  return C;
}

void Interp::initFields(Function* C, Object* obj) {
  Node* cls = C->cls;
  if (!cls) return;
  Env* fe = nullptr;
  ArenaScope as(*this, C->arena.get());
  for (int i = 0; i < cls->n; i++) {
    Node* m = cls->kids[i];
    if (!(m->flags & F_FIELD) || (m->flags & F_STATIC)) continue;
    if (!fe) {
      fe = newEnv(C->env);
      fe->funcScope = fe->hasThis = true;
      fe->thisV = Value::obj(obj);
      fe->func = C;
    }
    std::string key = (m->flags & F_COMPUTED) ? toKey(ev(m->a, fe)) : m->a->lit->s;
    Value v = m->b ? ev(m->b, fe) : Value();
    setOwn(obj, key.c_str(), key.size(), v);
  }
}

void Interp::superConstruct(Function* f, Object* obj, Value* args, int argc) {
  Value sup = f->superCls;
  if (sup.isObj() && sup.o()->kind == K::Function) runCtor((Function*)sup.o(), obj, args, argc);
  else if (sup.isObj() && sup.o()->kind == K::Native) {
    Native* nf = (Native*)sup.o();
    curNative = nf;
    constructing = true;
    nf->fn(*this, Value::obj(obj), args, argc);
    constructing = false;
  }
  initFields(f, obj);
}

Value Interp::runCtor(Function* f, Object* obj, Value* args, int argc) {
  if (f->isClass) {
    if (!f->isDerived) initFields(f, obj);
    if (f->node) return invoke(f, Value::obj(obj), args, argc);
    if (f->isDerived) superConstruct(f, obj, args, argc);
    return Value();
  }
  return invoke(f, Value::obj(obj), args, argc);
}

// out of line: its stack frame is released before the function body runs
__attribute__((noinline)) static void bindParams(Interp& I, Node* fn, Env* env, Value* args, int argc) {
  env->vars.reserve(fn->n + 2);
  for (int i = 0; i < fn->n; i++) {
    Node* p = fn->kids[i];
    Value a = i < argc ? args[i] : Value();
    if (p->type == N_ID) env->vars.push_back(Binding{p->str, false, a});
    else if (p->type == N_REST) {
      Array* r = I.newArray();
      for (int j = i; j < argc; j++) r->items.push_back(args[j]);
      I.bindPattern(p->a, Value::obj(r), env, V_LET);
    } else I.bindPattern(p, a, env, V_LET);
  }
  if (fn->flags & F_ARGS) {
    Array* r = I.newArray();
    r->items.assign(args, args + argc);
    env->vars.push_back(Binding{A_arguments, false, Value::obj(r)});
  }
}

Value Interp::invoke(Function* f, Value self, Value* args, int argc) {
  stackCheck();
  // Calls are GC safe points too: an expression-bodied callback run thousands of times by
  // map/forEach never reaches a statement boundary.
  if (gcPending || allocBytes > gcThreshold) collect();
  Node* fn = f->node;
  size_t mark = temps.size();
  Env* env = newEnv(f->env);
  env->funcScope = true;
  if (!f->isArrow) {
    env->hasThis = true;
    env->thisV = self;
    env->func = f;
  }
  ArenaScope as(*this, f->arena.get());
  bindParams(*this, fn, env, args, argc);
  Value result;
  if (fn->flags & F_CONCISE) result = ev(fn->a, env);
  else if (execStatements(fn->a->kids, fn->a->n, env) == C_RETURN) {
    result = retVal;
    retVal = Value();
  }
  temps.resize(mark);
  rootTemp(result);
  return result;
}

Value Interp::call(Value fv, Value self, Value* args, int argc) {
  if (!fv.isObj()) failCall(*this, fv);
  Object* o = fv.o();
  if (o->kind == K::Native) {
    stackCheck();
    Native* nf = (Native*)o;
    curNative = nf;
    constructing = false;
    Value r = nf->fn(*this, self, args, argc);
    rootTemp(r);
    return r;
  }
  if (o->kind == K::Function) {
    Function* f = (Function*)o;
    if (f->isClass) failCall(*this, fv);
    return invoke(f, self, args, argc);
  }
  fail(*this, "TypeError", "object is not a function");
}

Value Interp::construct(Value fv, Value* args, int argc) {
  if (!isCallable(fv)) throwError("TypeError", toStr(fv) + " is not a constructor");
  Object* F = fv.o();
  Value protoV = getProp(fv, "prototype", 9);
  if (F->kind == K::Function && ((Function*)F)->isArrow) fail(*this, "TypeError", "arrow function is not a constructor");
  Object* obj = newObject(protoV.isObj() ? protoV.o() : ObjectProto);
  Value r;
  if (F->kind == K::Native) {
    stackCheck();
    Native* nf = (Native*)F;
    curNative = nf;
    constructing = true;
    r = nf->fn(*this, Value::obj(obj), args, argc);
    constructing = false;
    rootTemp(r);
  } else r = runCtor((Function*)F, obj, args, argc);
  return r.isObj() ? r : Value::obj(obj);
}

// =================================================================== expressions
Value Interp::ev(Node* n, Env* e) {
  switch (n->type) {
    case N_NUM: return Value::num(n->num());
    case N_STR: return Value::str(n->lit);
    case N_TRUE: return Value::boolean(true);
    case N_FALSE: return Value::boolean(false);
    case N_NULL: return Value::null();
    case N_TMPL: return evTemplate(n, e);
    case N_ID: {
      Binding* b = lookup(e, n->str);
      if (!b) failUndefined(*this, n->str);
      return b->v;
    }
    case N_THIS: return thisOf(e);
    case N_ARR: return evArray(n, e);
    case N_OBJ: return evObject(n, e);
    case N_FUNC: return evFuncExpr(n, e);
    case N_CLASS: return Value::obj(makeClass(n, e));
    case N_UNARY: return evUnary(n, e);
    case N_UPDATE: return evUpdate(n, e);
    case N_BIN: {
      Value a = ev(n->a, e);
      Value b = ev(n->b, e);
      if (a.isNum() && b.isNum()) {
        double x = a.n, y = b.n;
        switch (n->op) {
          case O_ADD: return Value::num(x + y);
          case O_SUB: return Value::num(x - y);
          case O_MUL: return Value::num(x * y);
          case O_DIV: return Value::num(x / y);
          case O_LT: return Value::boolean(x < y);
          case O_GT: return Value::boolean(x > y);
          case O_LE: return Value::boolean(x <= y);
          case O_GE: return Value::boolean(x >= y);
          case O_SEQ: case O_EQ: return Value::boolean(x == y);
          case O_SNE: case O_NE: return Value::boolean(x != y);
          default: break;
        }
      }
      return binop(n->op, a, b);
    }
    case N_LOGIC: {
      Value a = ev(n->a, e);
      switch (n->op) {
        case O_LAND: return truthy(a) ? ev(n->b, e) : a;
        case O_LOR: return truthy(a) ? a : ev(n->b, e);
        default: return a.isNullish() ? ev(n->b, e) : a;
      }
    }
    case N_ASSIGN: return evAssign(n, e);
    case N_COND: return truthy(ev(n->a, e)) ? ev(n->b, e) : ev(n->c, e);
    case N_CALL: return evCall(n, e);
    case N_NEW: return evNew(n, e);
    case N_DOT: case N_INDEX: return evMember(n, e);
    case N_SEQ: {
      Value v;
      for (int i = 0; i < n->n; i++) v = ev(n->kids[i], e);
      return v;
    }
    case N_OPTCHAIN: {
      Value v = ev(n->a, e);
      optShort = false;
      return v;
    }
    case N_SUPER: fail(*this, "SyntaxError", "'super' keyword unexpected here");
    case N_SPREAD: fail(*this, "SyntaxError", "unexpected spread");
    default: fail(*this, "SyntaxError", "unsupported expression");
  }
}

JS_NOINLINE Value Interp::evTemplate(Node* n, Env* e) {
  std::string out;
  for (int i = 0; i < n->n; i++) {
    Node* k = n->kids[i];
    if (k->type == N_STR) out += k->lit->s;
    else {
      Value v = ev(k, e);
      if (v.isStr()) out += v.s()->s;
      else out += toStr(v);
    }
  }
  return str(out);
}

JS_NOINLINE Value Interp::evArray(Node* n, Env* e) {
  Array* a = newArray();
  a->items.reserve(n->n);
  for (int i = 0; i < n->n; i++) {
    Node* k = n->kids[i];
    if (!k) a->items.push_back(Value());
    else if (k->type == N_SPREAD) {
      Value v = ev(k->a, e);
      rootTemp(v);
      for (auto& x : toList(v)) a->items.push_back(x);
    } else {
      Value v = ev(k, e);
      a->items.push_back(v);
    }
  }
  return Value::obj(a);
}

JS_NOINLINE Value Interp::evObject(Node* n, Env* e) {
  Object* o = newObject();
  for (int i = 0; i < n->n; i++) {
    Node* p = n->kids[i];
    if (p->flags & F_SPREAD) {
      Value src = ev(p->b, e);
      if (src.isNullish()) continue;
      rootTemp(src);
      for (auto& k : ownKeys(src)) setOwn(o, k.c_str(), k.size(), getProp(src, k.data(), k.size()));
      continue;
    }
    std::string key = (p->flags & F_COMPUTED) ? toKey(ev(p->a, e)) : p->a->lit->s;
    if (p->flags & (F_GET | F_SET)) {
      Function* f = makeFunction(p->b, e);
      f->home = o;
      defineAccessor(o, key, Value::obj(f), p->flags & F_SET);
      continue;
    }
    if (p->b->type == N_DEFAULT) fail(*this, "SyntaxError", "invalid shorthand property initializer");
    Value v = ev(p->b, e);
    if (p->flags & F_METHOD) ((Function*)v.o())->home = o;
    else nameIfAnon(v, p->b, intern(key.c_str()));
    setOwn(o, key.c_str(), key.size(), v);
  }
  return Value::obj(o);
}

JS_NOINLINE Value Interp::evFuncExpr(Node* n, Env* e) {
  Function* f = makeFunction(n, e);
  if (n->str && !(n->flags & (F_ARROW | F_METHOD))) {
    Env* fe = newEnv(e);
    fe->vars.push_back(Binding{n->str, true, Value::obj(f)});
    f->env = fe;
  }
  return Value::obj(f);
}

JS_NOINLINE Value Interp::evUnary(Node* n, Env* e) {
  switch (n->op) {
    case O_TYPEOF: {
      if (n->a->type == N_ID && !lookup(e, n->a->str)) return Value::str(typeStr[0]);
      const char* t = typeOf(ev(n->a, e));
      static const char* names[] = {"undefined", "object", "boolean", "number", "string", "function"};
      for (int i = 0; i < 6; i++)
        if (!strcmp(names[i], t)) return Value::str(typeStr[i]);
      return str(t);
    }
    case O_DELETE: {
      Node* t = n->a;
      if (t->type == N_DOT) return Value::boolean(delProp(ev(t->a, e), std::string(t->str, t->n)));
      if (t->type == N_INDEX) {
        Value o = ev(t->a, e);
        return Value::boolean(delProp(o, toKey(ev(t->b, e))));
      }
      ev(t, e);
      return Value::boolean(true);
    }
    case O_NOT: return Value::boolean(!truthy(ev(n->a, e)));
    case O_NEG: return Value::num(-toNum(ev(n->a, e)));
    case O_PLUS: return Value::num(toNum(ev(n->a, e)));
    case O_BNOT: return Value::num(~toInt32(ev(n->a, e)));
    case O_VOID: ev(n->a, e); return Value();
  }
  return Value();
}

Value Interp::getRef(Node* t, Env* e, Value& obj, Value& key) {
  switch (t->type) {
    case N_ID: {
      Binding* b = lookup(e, t->str);
      if (!b) failUndefined(*this, t->str);
      return b->v;
    }
    case N_DOT:
      if (t->a->type == N_SUPER) fail(*this, "SyntaxError", "assignment to super property is not supported");
      obj = ev(t->a, e);
      rootTemp(obj);
      return getProp(obj, t->str, t->n, t->str);
    case N_INDEX:
      obj = ev(t->a, e);
      rootTemp(obj);
      key = ev(t->b, e);
      rootTemp(key);
      return getIndex(obj, key);
  }
  fail(*this, "SyntaxError", "invalid assignment target");
}

void Interp::setRef(Node* t, Env* e, Value obj, Value key, Value v) {
  switch (t->type) {
    case N_ID: assignVar(e, t->str, v); return;
    case N_DOT:
      if (obj.isUndef()) { obj = ev(t->a, e); }
      putProp(obj, t->str, t->n, v, t->str);
      return;
    case N_INDEX:
      if (obj.isUndef()) {
        obj = ev(t->a, e);
        rootTemp(obj);
        key = ev(t->b, e);
      }
      putIndex(obj, key, v);
      return;
  }
  fail(*this, "SyntaxError", "invalid assignment target");
}

JS_NOINLINE Value Interp::evUpdate(Node* n, Env* e) {
  Value obj, key;
  Value cur = getRef(n->a, e, obj, key);
  double old = toNum(cur);
  double nv = n->op == O_ADD ? old + 1 : old - 1;
  if (n->a->type != N_ID && obj.isUndef()) obj = Value::null();  // never re-evaluate
  setRef(n->a, e, obj, key, Value::num(nv));
  return Value::num((n->flags & F_PREFIX) ? nv : old);
}

JS_NOINLINE Value Interp::evAssign(Node* n, Env* e) {
  Node* t = n->a;
  if (t->type == N_ARRPAT || t->type == N_OBJPAT) {
    Value v = ev(n->b, e);
    bindPattern(t, v, e, V_ASSIGN);
    return v;
  }
  if (n->op == O_ASSIGN) {
    switch (t->type) {
      case N_ID: {
        Value v = ev(n->b, e);
        nameIfAnon(v, n->b, t->str);
        assignVar(e, t->str, v);
        return v;
      }
      case N_DOT: {
        Value obj = ev(t->a, e);
        rootTemp(obj);
        Value v = ev(n->b, e);
        putProp(obj, t->str, t->n, v, t->str);
        return v;
      }
      case N_INDEX: {
        Value obj = ev(t->a, e);
        rootTemp(obj);
        Value key = ev(t->b, e);
        rootTemp(key);
        Value v = ev(n->b, e);
        putIndex(obj, key, v);
        return v;
      }
    }
    fail(*this, "SyntaxError", "invalid assignment target");
  }
  Value obj, key;
  Value cur = getRef(t, e, obj, key);
  if (t->type != N_ID && obj.isUndef()) obj = Value::null();
  Value v;
  if (n->op == O_LAND) { if (!truthy(cur)) return cur; v = ev(n->b, e); }
  else if (n->op == O_LOR) { if (truthy(cur)) return cur; v = ev(n->b, e); }
  else if (n->op == O_NULLISH) { if (!cur.isNullish()) return cur; v = ev(n->b, e); }
  else {
    rootTemp(cur);
    Value r = ev(n->b, e);
    v = binop(n->op, cur, r);
  }
  rootTemp(v);
  setRef(t, e, obj, key, v);
  return v;
}

static std::string concatStr(Interp& I, Value a, Value b) {
  std::string s = a.isStr() ? a.s()->s : I.toStr(a);
  if (b.isStr()) s += b.s()->s;
  else s += I.toStr(b);
  return s;
}

JS_NOINLINE Value Interp::binop(int op, Value a, Value b) {
  switch (op) {
    case O_ADD: {
      if (a.isNum() && b.isNum()) return Value::num(a.n + b.n);
      a = toPrimitive(a, true);
      b = toPrimitive(b, true);
      if (a.isStr() || b.isStr()) return str(concatStr(*this, a, b));
      return Value::num(toNum(a) + toNum(b));
    }
    case O_SUB: return Value::num(toNum(a) - toNum(b));
    case O_MUL: return Value::num(toNum(a) * toNum(b));
    case O_DIV: return Value::num(toNum(a) / toNum(b));
    case O_MOD: return Value::num(std::fmod(toNum(a), toNum(b)));
    case O_EXP: return Value::num(std::pow(toNum(a), toNum(b)));
    case O_EQ: return Value::boolean(looseEq(a, b));
    case O_NE: return Value::boolean(!looseEq(a, b));
    case O_SEQ: return Value::boolean(strictEq(a, b));
    case O_SNE: return Value::boolean(!strictEq(a, b));
    case O_LT: case O_GT: case O_LE: case O_GE: {
      a = toPrimitive(a, true);
      b = toPrimitive(b, true);
      if (a.isStr() && b.isStr()) {
        int c = a.s()->s.compare(b.s()->s);
        switch (op) {
          case O_LT: return Value::boolean(c < 0);
          case O_GT: return Value::boolean(c > 0);
          case O_LE: return Value::boolean(c <= 0);
          default: return Value::boolean(c >= 0);
        }
      }
      double x = toNum(a), y = toNum(b);
      switch (op) {
        case O_LT: return Value::boolean(x < y);
        case O_GT: return Value::boolean(x > y);
        case O_LE: return Value::boolean(x <= y);
        default: return Value::boolean(x >= y);
      }
    }
    case O_BAND: return Value::num(toInt32(a) & toInt32(b));
    case O_BOR: return Value::num(toInt32(a) | toInt32(b));
    case O_BXOR: return Value::num(toInt32(a) ^ toInt32(b));
    case O_SHL: return Value::num((int32_t)((uint32_t)toInt32(a) << (toInt32(b) & 31)));
    case O_SHR: return Value::num(toInt32(a) >> (toInt32(b) & 31));
    case O_USHR: return Value::num((uint32_t)toInt32(a) >> (toInt32(b) & 31));
    case O_IN: return Value::boolean(hasProp(b, toKey(a)));
    case O_INSTANCEOF: return Value::boolean(instanceOf(a, b));
  }
  fail(*this, "SyntaxError", "bad operator");
}

std::string Interp::describe(Node* n) {
  switch (n->type) {
    case N_ID: return n->str;
    case N_THIS: return "this";
    case N_SUPER: return "super";
    case N_DOT: return describe(n->a) + "." + std::string(n->str, n->n);
    case N_INDEX: return describe(n->a) + "[...]";
    case N_CALL: return describe(n->a) + "(...)";
    case N_OPTCHAIN: return describe(n->a);
    default: return "expression";
  }
}

void Interp::evalArgs(Node* n, Env* e, PVec<Value>& out) {
  out.reserve(n->n);
  for (int i = 0; i < n->n; i++) {
    Node* k = n->kids[i];
    if (k->type == N_SPREAD) {
      Value v = ev(k->a, e);
      rootTemp(v);
      for (auto& x : toList(v)) out.push_back(x);
    } else out.push_back(ev(k, e));
  }
}

Value Interp::superLookup(Env* e, Node* member, Value self) {
  Function* f = homeFunc(e);
  if (!f || !f->home) fail(*this, "SyntaxError", "'super' keyword unexpected here");
  std::string key = member->type == N_DOT ? std::string(member->str, member->n) : toKey(ev(member->b, e));
  const char* a = findAtom(key.data(), key.size());
  return a ? lookupFrom(f->home->proto, a, self) : Value();
}

JS_NOINLINE Value Interp::evMember(Node* n, Env* e) {
  if (n->a->type == N_SUPER) return superLookup(e, n, thisOf(e));
  Value obj = ev(n->a, e);
  if (optShort) return Value();
  if ((n->flags & F_OPTIONAL) && obj.isNullish()) { optShort = true; return Value(); }
  Value r;
  if (n->type == N_DOT) r = getProp(obj, n->str, n->n, n->str);
  else {
    rootTemp(obj);
    Value k = ev(n->b, e);
    r = getIndex(obj, k);
  }
  if (r.isObj()) temps.push_back(r.g);
  return r;
}

JS_NOINLINE Value Interp::evCall(Node* n, Env* e) {
  Node* c = n->a;
  Value fn, self;
  if (c->type == N_SUPER) {
    Function* f = homeFunc(e);
    Value th = thisOf(e);
    if (!f || !f->isDerived || !th.isObj()) fail(*this, "SyntaxError", "'super' keyword unexpected here");
    PVec<Value> args;
    evalArgs(n, e, args);
    superConstruct(f, th.o(), args.data(), (int)args.size());
    return Value();
  }
  if (c->type == N_DOT || c->type == N_INDEX) {
    if (c->a->type == N_SUPER) {
      self = thisOf(e);
      fn = superLookup(e, c, self);
    } else {
      self = ev(c->a, e);
      if (optShort) return Value();
      if ((c->flags & F_OPTIONAL) && self.isNullish()) { optShort = true; return Value(); }
      rootTemp(self);
      if (c->type == N_DOT) fn = getProp(self, c->str, c->n, c->str);
      else fn = getIndex(self, ev(c->b, e));
    }
  } else {
    fn = ev(c, e);
    if (optShort) return Value();
  }
  if ((n->flags & F_OPTIONAL) && fn.isNullish()) { optShort = true; return Value(); }
  if (!isCallable(fn)) failNotCallable(*this, c, " is not a function");
  rootTemp(fn);
  PVec<Value> args;
  evalArgs(n, e, args);
  return call(fn, self, args.data(), (int)args.size());
}

JS_NOINLINE Value Interp::evNew(Node* n, Env* e) {
  Value fn = ev(n->a, e);
  if (!isCallable(fn)) failNotCallable(*this, n->a, " is not a constructor");
  rootTemp(fn);
  PVec<Value> args;
  evalArgs(n, e, args);
  return construct(fn, args.data(), (int)args.size());
}

// =================================================================== statements
JS_NOINLINE void Interp::hoist(Node** kids, int n, Env* e) {
  for (int i = 0; i < n; i++) {
    Node* k = kids[i];
    if (k->type == N_FUNC && (k->flags & F_DECL)) declare(e, k->str, Value::obj(makeFunction(k, e)), V_LET);
  }
}

Completion Interp::execStatements(Node** kids, int n, Env* e) {
  hoist(kids, n, e);
  for (int i = 0; i < n; i++) {
    Completion c = exec(kids[i], e);
    if (c != C_NORMAL) return c;
  }
  return C_NORMAL;
}

JS_NOINLINE void Interp::execVar(Node* s, Env* e) {
  for (int i = 0; i < s->n; i++) {
    Node* d = s->kids[i];
    Node* tg = d->a;
    if (!d->b) {
      if (s->op == V_VAR) {
        if (tg->type == N_ID) {
          Env* fe = e;
          while (!fe->funcScope && fe->parent) fe = fe->parent;
          bool found = false;
          for (auto& b : fe->vars)
            if (b.name == tg->str) { found = true; break; }
          if (!found) fe->vars.push_back(Binding{tg->str, false, Value()});
          continue;
        }
      } else if (s->op == V_CONST) fail(*this, "SyntaxError", "Missing initializer in const declaration");
      bindPattern(tg, Value(), e, s->op);
      continue;
    }
    Value v = ev(d->b, e);
    if (tg->type == N_ID) nameIfAnon(v, d->b, tg->str);
    bindPattern(tg, v, e, s->op);
  }
}

JS_NOINLINE Completion Interp::execFor(Node* s, Env* e) {
  Env* loopEnv = e;
  bool perIter = false;
  if (s->a) {
    if (s->a->type == N_VAR) {
      if (s->a->op != V_VAR) {
        loopEnv = newEnv(e);
        perIter = (s->flags & F_CLOSURE) != 0;
      }
      execVar(s->a, loopEnv);
    } else ev(s->a, e);
  }
  size_t m = temps.size();
  Env* it = perIter ? copyEnv(loopEnv) : loopEnv;
  for (;;) {
    if (s->b && !truthy(ev(s->b, it))) break;
    Completion c = exec(s->d, it);
    if (c == C_BREAK) break;
    if (c == C_RETURN) return c;
    temps.resize(m);
    if (perIter) it = copyEnv(it);
    if (s->c) ev(s->c, it);
  }
  return C_NORMAL;
}

JS_NOINLINE Completion Interp::execForInOf(Node* s, Env* e) {
  Node* left = s->a;
  Value coll = ev(s->b, e);
  rootTemp(coll);
  bool decl = left->type == N_VAR;
  Node* target = decl ? left->kids[0]->a : left;
  int kind = decl ? (int)left->op : (int)V_ASSIGN;
  bool fresh = decl && kind != V_VAR;
  bool perIter = fresh && (s->flags & F_CLOSURE);
  Env* shared = fresh && !perIter ? newEnv(e) : e;
  size_t m = temps.size();
  auto step = [&](Value v) -> Completion {
    Env* it = perIter ? newEnv(e) : shared;
    bindPattern(target, v, it, kind);
    Completion c = exec(s->d, it);
    temps.resize(m);
    return c;
  };
  if (s->type == N_FORIN) {
    if (coll.isNullish()) return C_NORMAL;
    std::vector<std::string> keys = ownKeys(coll);
    for (auto& k : keys) {
      Completion c = step(str(k));
      if (c == C_BREAK) break;
      if (c == C_RETURN) return c;
    }
    return C_NORMAL;
  }
  if (coll.isObj() && coll.o()->kind == K::Array) {
    Array* a = (Array*)coll.o();
    for (size_t i = 0; i < a->items.size(); i++) {
      Completion c = step(a->items[i]);
      if (c == C_BREAK) break;
      if (c == C_RETURN) return c;
    }
    return C_NORMAL;
  }
  std::vector<Value> items = toList(coll);
  Array* hold = newArray();  // keep the items rooted
  hold->items.assign(items.begin(), items.end());
  m = temps.size();
  for (auto& v : items) {
    Completion c = step(v);
    if (c == C_BREAK) break;
    if (c == C_RETURN) return c;
  }
  return C_NORMAL;
}

JS_NOINLINE Completion Interp::execTry(Node* s, Env* e) {
  Completion r = C_NORMAL;
  bool pending = false;
  Value pendingV;
  int pendingLine = 0;
  try {
    r = exec(s->a, e);
  } catch (JsThrow& ex) {
    if (s->c) {
      Value v = ex.v;
      rootTemp(v);
      auto runCatch = [&]() {
        Env* ce = newEnv(e);
        if (s->b) bindPattern(s->b, v, ce, V_LET);
        return exec(s->c, ce);
      };
      if (!s->d) return runCatch();
      try {
        r = runCatch();
      } catch (JsThrow& ex2) {
        pending = true;
        pendingV = ex2.v;
        pendingLine = ex2.line;
        rootTemp(pendingV);
      }
    } else {
      pending = true;
      pendingV = ex.v;
      pendingLine = ex.line;
      rootTemp(pendingV);
    }
  }
  if (s->d) {
    Value savedRet = retVal;
    rootTemp(savedRet);
    Completion fr = exec(s->d, e);
    if (fr != C_NORMAL) return fr;
    retVal = savedRet;
  }
  if (pending) throw JsThrow{pendingV, pendingLine};
  return r;
}

JS_NOINLINE Completion Interp::execSwitch(Node* s, Env* e) {
  Value d = ev(s->a, e);
  rootTemp(d);
  Env* se = newEnv(e);
  int start = -1, def = -1;
  for (int i = 0; i < s->n; i++) {
    Node* cs = s->kids[i];
    if (!cs->a) { def = i; continue; }
    if (strictEq(d, ev(cs->a, se))) { start = i; break; }
  }
  if (start < 0) start = def;
  if (start < 0) return C_NORMAL;
  for (int i = start; i < s->n; i++) {
    Node* cs = s->kids[i];
    hoist(cs->kids, cs->n, se);
    for (int j = 0; j < cs->n; j++) {
      Completion c = exec(cs->kids[j], se);
      if (c == C_BREAK) return C_NORMAL;
      if (c != C_NORMAL) return c;
    }
  }
  return C_NORMAL;
}

JS_NOINLINE Completion Interp::execLoop(Node* s, Env* e) {
  size_t m = temps.size();
  bool isDo = s->type == N_DO;
  for (;;) {
    if (!isDo && !truthy(ev(s->a, e))) break;
    Completion r = exec(s->d, e);
    if (r == C_BREAK) break;
    if (r == C_RETURN) return r;
    temps.resize(m);
    if (isDo && !truthy(ev(s->a, e))) break;
  }
  return C_NORMAL;
}

Completion Interp::exec(Node* s, Env* e) {
  stackCheck();
  if (++steps >= 4096) {
    steps = 0;
    if (poll) poll();
  }
  if (gcPending || allocBytes > gcThreshold) collect();
  line = s->line;
  size_t mark = temps.size();
  Completion c = C_NORMAL;
  try {
  switch (s->type) {
    case N_EXPR: ev(s->a, e); break;
    case N_VAR: execVar(s, e); break;
    case N_FUNC: break;  // hoisted
    case N_CLASS: declare(e, s->str, Value::obj(makeClass(s, e)), V_LET); break;
    case N_BLOCK: c = execStatements(s->kids, s->n, (s->flags & F_SCOPE) ? newEnv(e) : e); break;
    case N_IF:
      if (truthy(ev(s->a, e))) c = exec(s->b, e);
      else if (s->c) c = exec(s->c, e);
      break;
    case N_WHILE: case N_DO: c = execLoop(s, e); break;
    case N_FOR: c = execFor(s, e); break;
    case N_FORIN: case N_FOROF: c = execForInOf(s, e); break;
    case N_BREAK: c = C_BREAK; break;
    case N_CONTINUE: c = C_CONTINUE; break;
    case N_RETURN:
      retVal = s->a ? ev(s->a, e) : Value();
      c = C_RETURN;
      break;
    case N_THROW: {
      Value v = ev(s->a, e);
      throw JsThrow{v, s->line};
    }
    case N_TRY: c = execTry(s, e); break;
    case N_SWITCH: c = execSwitch(s, e); break;
    case N_EMPTY: break;
    default: fail(*this, "SyntaxError", "unsupported statement");
  }
  } catch (std::bad_alloc&) {
    // Out of memory becomes an ordinary, catchable JS error. The error object was made in
    // advance: there may be no memory to make one now.
    gcPending = true;
    throw JsThrow{Value::obj(oomError), s->line};
  }
  temps.resize(mark);
  return c;
}

// =================================================================== top level
struct EntryGuard {
  Interp& I;
  explicit EntryGuard(Interp& i) : I(i) {
    if (I.entryDepth++ == 0) {
      char probe;
      I.stackBase = (uintptr_t)&probe;
    }
  }
  ~EntryGuard() { --I.entryDepth; }
};

Interp::Interp() {
  A_arguments = intern("arguments");
  gcThreshold = gcMin;
  global = newEnv(nullptr);
  global->funcScope = true;
  oomError = newObject();  // proto/message are set once the Error types exist
  setupBuiltins();
  temps.clear();
}

Interp::~Interp() {
  timers.clear();
  roots.clear();
  GcObj* o = heap;
  heap = nullptr;
  while (o) {
    GcObj* n = o->gcNext;
    delete o;
    o = n;
  }
}

Value Interp::run(const std::string& src, const char* name) {
  EntryGuard entry(*this);
  auto arena = std::make_shared<Arena>(this);
  size_t mark = temps.size();
  Node* prog;
  {
    Parser P(*this, arena.get(), src.data(), src.size(), name);
    prog = P.program();
  }
  if (dropSourceAfterParse) {  // the caller's copy of the source isn't needed any more
    dropSourceAfterParse = false;
    std::string& s = const_cast<std::string&>(src);
    s.clear();
    s.shrink_to_fit();
  }
  if (afterParse) {  // host hook, e.g. claiming memory the parser no longer needs
    auto hook = afterParse;
    afterParse = nullptr;
    hook();
  }
  ArenaScope as(*this, arena.get());
  Value last;
  lastResult = Value();
  hoist(prog->kids, prog->n, global);
  for (int i = 0; i < prog->n; i++) {
    Node* s = prog->kids[i];
    if (s->type == N_EXPR) {
      if (gcPending || allocBytes > gcThreshold) collect();
      line = s->line;
      size_t m = temps.size();
      last = lastResult = ev(s->a, global);
      temps.resize(m);
    } else {
      last = lastResult = Value();
      exec(s, global);
    }
  }
  temps.resize(mark);
  return last;
}

void Interp::reportError(const JsThrow& e) {
  lastError = errorString(e.v);
  if (e.line > 0) lastError += " (line " + std::to_string(e.line) + ")";
  std::string msg = "Uncaught " + lastError;
  print(msg + "\n");
}

bool Interp::runSafe(const std::string& src, const char* name, Value* result) {
  size_t mark = temps.size();
  try {
    Value v = run(src, name);
    if (result) *result = v;
    return true;
  } catch (JsThrow& ex) {
    reportError(ex);
  } catch (JsInterrupt&) {
    print("Interrupted\n");
  } catch (std::bad_alloc&) {
    print("Uncaught Error: out of memory\n");
    lastError = "Error: out of memory";
    gcPending = true;
  }
  temps.resize(mark);
  optShort = constructing = false;
  retVal = Value();
  return false;
}

bool Interp::callSafe(Value fn, Value self, const std::vector<Value>& args, Value* result) {
  EntryGuard entry(*this);
  size_t mark = temps.size();
  bool ok = false;
  try {
    std::vector<Value> a = args;
    for (auto& v : a) rootTemp(v);
    Value r = call(fn, self, a.data(), (int)a.size());
    if (result) *result = r;
    ok = true;
  } catch (JsThrow& ex) {
    reportError(ex);
  } catch (JsInterrupt&) {
    print("Interrupted\n");
    clearTimers();
  } catch (std::bad_alloc&) {
    print("Uncaught Error: out of memory\n");
    gcPending = true;
  }
  temps.resize(mark);
  optShort = constructing = false;
  retVal = Value();
  return ok;
}

int32_t Interp::msUntilNextTimer() {
  if (timers.empty()) return -1;
  uint32_t now = millis ? millis() : 0;
  int32_t best = INT32_MAX;
  for (auto& t : timers) best = std::min(best, (int32_t)(t.when - now));
  return best < 0 ? 0 : best;
}

void Interp::runTimers() {
  if (timers.empty()) return;
  uint32_t now = millis ? millis() : 0;
  std::vector<std::pair<uint32_t, int>> due;
  for (auto& t : timers)
    if ((int32_t)(now - t.when) >= 0) due.push_back({t.when - now, t.id});
  std::sort(due.begin(), due.end(), [](const std::pair<uint32_t, int>& a, const std::pair<uint32_t, int>& b) {
    return (int32_t)a.first < (int32_t)b.first;
  });
  for (auto& d : due) {
    auto it = std::find_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.id == d.second; });
    if (it == timers.end()) continue;
    Timer t = *it;
    size_t mark = temps.size();
    rootTemp(t.fn);
    for (auto& a : t.args) rootTemp(a);
    if (t.repeat) {
      it->when += t.interval ? t.interval : 1;
      if ((int32_t)(now - it->when) >= 0) it->when = now + (t.interval ? t.interval : 1);
    } else timers.erase(it);
    bool ok = callSafe(t.fn, Value(), t.args);
    temps.resize(mark);
    if (!ok && timers.empty()) return;  // interrupted
  }
}

}  // namespace js
