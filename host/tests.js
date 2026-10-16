// Test suite for the interpreter core. Run: make -C host test
let passed = 0, failed = 0;
function eq(actual, expected, label) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a === e) passed++;
  else { failed++; console.log(`FAIL ${label}: got ${a}, expected ${e}`); }
}
function throws(fn, type, label) {
  try { fn(); failed++; console.log(`FAIL ${label}: did not throw`); }
  catch (e) {
    if (type && !(e instanceof type)) { failed++; console.log(`FAIL ${label}: wrong error ${e}`); }
    else passed++;
  }
}

// --- numbers & operators
eq(1 + 2 * 3, 7, "precedence");
eq(2 ** 10, 1024, "exponent");
eq(2 ** 3 ** 2, 512, "exponent right assoc");
eq(7 % 3, 1, "modulo");
eq(-7 % 3, -1, "negative modulo");
eq(0.1 + 0.2, 0.30000000000000004, "float");
eq(String(0.1 + 0.2), "0.30000000000000004", "float string");
eq(String(1e21), "1e+21", "big exp");
eq(String(1.5e-7), "1.5e-7", "small exp");
eq(String(123.456), "123.456", "decimal");
eq(String(-0.000001), "-0.000001", "small decimal");
eq(5 / 2, 2.5, "division");
eq(1 / 0, null, "infinity json");
eq(String(1 / 0), "Infinity", "infinity string");
eq(isNaN(0 / 0), true, "nan");
eq(7 & 3, 3, "and");
eq(5 | 2, 7, "or");
eq(5 ^ 1, 4, "xor");
eq(~5, -6, "not");
eq(1 << 4, 16, "shl");
eq(-16 >> 2, -4, "shr");
eq(-1 >>> 28, 15, "ushr");
eq(0xff + 0b101 + 0o17, 255 + 5 + 15, "radix literals");
eq(1_000_000, 1000000, "numeric separators");
eq("5" * "2", 10, "string mult");
eq("5" + 2, "52", "string concat");
eq(5 + "2", "52", "concat 2");
eq(+"3.5", 3.5, "unary plus");
eq(+"abc", null, "NaN to json");
eq(+"", 0, "empty to num");
eq(+" 42 ", 42, "trim to num");
eq(+"0x1f", 31, "hex string");
eq(null == undefined, true, "null == undefined");
eq(null === undefined, false, "strict");
eq("1" == 1, true, "loose eq");
eq(0 == "", true, "loose eq empty");
eq(NaN == NaN, false, "nan eq");
eq("a" < "b", true, "string compare");
eq("10" < "9", true, "string compare lexical");
eq(10 < 9, false, "num compare");
eq("10" < 9, false, "mixed compare");
eq(typeof 1, "number", "typeof num");
eq(typeof "x", "string", "typeof str");
eq(typeof undefined, "undefined", "typeof undef");
eq(typeof notDefined, "undefined", "typeof undeclared");
eq(typeof null, "object", "typeof null");
eq(typeof {}, "object", "typeof obj");
eq(typeof function () {}, "function", "typeof fn");
eq(typeof Math.sin, "function", "typeof native");
eq(void 0, undefined, "void");
eq(1 && 2, 2, "and value");
eq(0 || "x", "x", "or value");
eq(null ?? 5, 5, "nullish");
eq(0 ?? 5, 0, "nullish zero");
eq(true ? 1 : 2, 1, "ternary");
let x = 5;
x += 3; eq(x, 8, "+=");
x -= 1; eq(x, 7, "-=");
x *= 2; eq(x, 14, "*=");
x **= 2; eq(x, 196, "**=");
x %= 10; eq(x, 6, "%=");
x <<= 1; eq(x, 12, "<<=");
let y = null; y ??= 3; eq(y, 3, "??=");
y ||= 9; eq(y, 3, "||=");
y &&= 4; eq(y, 4, "&&=");
let i = 0;
eq(i++, 0, "postfix"); eq(i, 1, "postfix 2"); eq(++i, 2, "prefix");
eq(i--, 2, "postfix dec"); eq(--i, 0, "prefix dec");
eq((1, 2, 3), 3, "comma");

// --- strings
eq("hello".length, 5, "length");
eq("hello"[1], "e", "index");
eq("hello".toUpperCase(), "HELLO", "upper");
eq("a,b,c".split(","), ["a", "b", "c"], "split");
eq("abc".split(""), ["a", "b", "c"], "split chars");
eq("a-b-c".split("-", 2), ["a", "b"], "split limit");
eq("  hi  ".trim(), "hi", "trim");
eq("hello".slice(1, -1), "ell", "slice");
eq("hello".substring(3, 1), "el", "substring swap");
eq("hello".indexOf("l"), 2, "indexOf");
eq("hello".lastIndexOf("l"), 3, "lastIndexOf");
eq("a b c d".lastIndexOf(" ", 2), 1, "lastIndexOf from");
eq("abcabc".lastIndexOf("b", 3), 1, "lastIndexOf from, before");
eq("abc".lastIndexOf("c", -5), -1, "lastIndexOf negative from");
eq("abc".lastIndexOf("a", 99), 0, "lastIndexOf large from");
eq("hello".includes("ell"), true, "includes");
eq("hello".startsWith("he"), true, "startsWith");
eq("hello".endsWith("lo"), true, "endsWith");
eq("ab".repeat(3), "ababab", "repeat");
eq("5".padStart(3, "0"), "005", "padStart");
eq("5".padEnd(3, "-"), "5--", "padEnd");
eq("a.b.c".replace(".", "/"), "a/b.c", "replace");
eq("a.b.c".replaceAll(".", "/"), "a/b/c", "replaceAll");
eq("abc".replace("b", m => m.toUpperCase()), "aBc", "replace fn");
eq("abc".charCodeAt(1), 98, "charCodeAt");
eq(String.fromCharCode(72, 105), "Hi", "fromCharCode");
eq("abc".at(-1), "c", "at");
eq(`a${1 + 1}b${"c"}`, "a2bc", "template");
eq(`nested ${`inner ${2 * 3}`}`, "nested inner 6", "nested template");
eq(`obj ${ {a: 1}.a }`, "obj 1", "template with braces");
eq("\x41B\u{43}\n".length, 4, "escapes");
eq("é".length, 2, "utf8 bytes");
eq([..."héllo"].length, 5, "utf8 iteration");
eq("x".concat("y", 1), "xy1", "concat");

// --- arrays
const arr = [3, 1, 2];
eq(arr.length, 3, "array length");
arr.push(5); eq(arr, [3, 1, 2, 5], "push");
eq(arr.pop(), 5, "pop");
eq(arr.shift(), 3, "shift");
arr.unshift(0); eq(arr, [0, 1, 2], "unshift");
eq([1, 2, 3].map(x => x * 2), [2, 4, 6], "map");
eq([1, 2, 3, 4].filter(x => x % 2 == 0), [2, 4], "filter");
eq([1, 2, 3].reduce((a, b) => a + b, 0), 6, "reduce");
eq([1, 2, 3].reduce((a, b) => a + b), 6, "reduce no init");
eq(["a", "b"].reduceRight((a, b) => a + b), "ba", "reduceRight");
eq([1, 2, 3].find(x => x > 1), 2, "find");
eq([1, 2, 3].findIndex(x => x > 5), -1, "findIndex");
eq([1, 2, 3].some(x => x > 2), true, "some");
eq([1, 2, 3].every(x => x > 2), false, "every");
eq([1, 2, 3].includes(2), true, "includes");
eq([NaN].includes(NaN), true, "includes NaN");
eq([1, 2, 3].indexOf(3), 2, "indexOf");
eq([1, 2, 1, 2].lastIndexOf(2, 2), 1, "array lastIndexOf from");
eq([1, 2, 1, 2].lastIndexOf(1, -2), 2, "array lastIndexOf negative from");
eq([1, 2, 3].join("-"), "1-2-3", "join");
eq([1, [2, [3]]].join(), "1,2,3", "join nested");
eq([3, 1, 10, 2].sort(), [1, 10, 2, 3], "default sort is lexical");
eq([3, 1, 10, 2].sort((a, b) => a - b), [1, 2, 3, 10], "numeric sort");
eq([{ n: "b", v: 1 }, { n: "a", v: 1 }, { n: "c", v: 0 }].sort((a, b) => a.v - b.v).map(o => o.n), ["c", "b", "a"], "stable sort");
eq([1, 2, 3].reverse(), [3, 2, 1], "reverse");
eq([1, 2, 3, 4].slice(1, 3), [2, 3], "slice");
eq([1, 2, 3, 4].slice(-2), [3, 4], "slice neg");
const sp = [1, 2, 3, 4, 5];
eq(sp.splice(1, 2, "a", "b", "c"), [2, 3], "splice ret");
eq(sp, [1, "a", "b", "c", 4, 5], "splice result");
eq([1, 2].concat([3], 4), [1, 2, 3, 4], "concat");
eq([1, [2, [3, [4]]]].flat(), [1, 2, [3, [4]]], "flat");
eq([1, 2].flatMap(x => [x, x * 10]), [1, 10, 2, 20], "flatMap");
eq(new Array(3).fill(0), [0, 0, 0], "fill");
eq(Array.from({ length: 3 }, (_, i) => i * i), [0, 1, 4], "Array.from length");
eq(Array.from("abc"), ["a", "b", "c"], "Array.from string");
eq(Array.isArray([]), true, "isArray");
eq([..."ab", ...[1, 2]], ["a", "b", 1, 2], "spread");
const holes = []; holes[3] = 1; eq(holes.length, 4, "sparse length");
const trunc = [1, 2, 3]; trunc.length = 1; eq(trunc, [1], "length truncate");
eq([1, 2, 3].at(-1), 3, "array at");
eq([5, 6].entries(), [[0, 5], [1, 6]], "entries");
let fe = 0; [1, 2, 3].forEach(v => fe += v); eq(fe, 6, "forEach");

// --- objects
const o = { a: 1, "b-c": 2, [`k${1}`]: 3, f() { return this.a; } };
eq(o.a, 1, "dot");
eq(o["b-c"], 2, "bracket");
eq(o.k1, 3, "computed key");
eq(o.f(), 1, "method this");
eq(Object.keys(o), ["a", "b-c", "k1", "f"], "keys order");
eq(Object.values({ x: 1, y: 2 }), [1, 2], "values");
eq(Object.entries({ x: 1 }), [["x", 1]], "entries");
eq(Object.assign({}, { a: 1 }, { b: 2 }), { a: 1, b: 2 }, "assign");
eq({ ...{ a: 1, b: 2 }, b: 3 }, { a: 1, b: 3 }, "object spread");
const short = 5; eq({ short }, { short: 5 }, "shorthand");
eq("a" in { a: 1 }, true, "in");
eq("z" in { a: 1 }, false, "in false");
const del = { a: 1, b: 2 }; delete del.a; eq(del, { b: 2 }, "delete");
eq({ a: 1 }.hasOwnProperty("a"), true, "hasOwnProperty");
eq(o.missing, undefined, "missing prop");
eq(o?.missing?.deep, undefined, "optional chain");
eq(null?.x, undefined, "optional null");
const oc = { f: null }; eq(oc.f?.(), undefined, "optional call");
eq(oc.g?.x.y.z, undefined, "optional short circuit");
const getset = { _v: 1, get v() { return this._v * 10; }, set v(n) { this._v = n; } };
eq(getset.v, 10, "getter");
getset.v = 5; eq(getset.v, 50, "setter");
const proto = { hi() { return "hi " + this.name; } };
const child = Object.create(proto); child.name = "bob";
eq(child.hi(), "hi bob", "Object.create");
eq(Object.getPrototypeOf(child) === proto, true, "getPrototypeOf");
eq(JSON.stringify({ a: [1, "x", null, true], b: { c: undefined } }), '{"a":[1,"x",null,true],"b":{}}', "stringify");
eq(JSON.stringify([1, { a: 2 }], null, 2), '[\n  1,\n  {\n    "a": 2\n  }\n]', "stringify indent");
eq(JSON.parse('{"a":[1,2,{"b":"c\\n"}],"d":-1.5e2,"e":null}'), { a: [1, 2, { b: "c\n" }], d: -150, e: null }, "parse");
throws(() => JSON.parse("{bad}"), SyntaxError, "parse error");

// --- functions & closures
function add(a, b = 10) { return a + b; }
eq(add(1), 11, "default param");
eq(add(1, 2), 3, "params");
eq(add.length, 1, "fn length");
eq(add.name, "add", "fn name");
const anon = () => 1; eq(anon.name, "anon", "inferred name");
function rest(a, ...more) { return more; }
eq(rest(1, 2, 3), [2, 3], "rest");
eq(Math.max(...[1, 5, 3]), 5, "spread call");
function counter() { let c = 0; return () => ++c; }
const c1 = counter(), c2 = counter();
c1(); c1();
eq(c1(), 3, "closure");
eq(c2(), 1, "closure independent");
eq(hoisted(), "ok", "hoisting");
function hoisted() { return "ok"; }
const fact = function f(n) { return n <= 1 ? 1 : n * f(n - 1); };
eq(fact(10), 3628800, "named fn expr recursion");
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
eq(fib(20), 6765, "recursion");
function argsFn() { return arguments.length; }
eq(argsFn(1, 2, 3), 3, "arguments");
const bound = function () { return this.v; }.bind({ v: 42 });
eq(bound(), 42, "bind");
eq(function (a, b) { return this.x + a + b; }.call({ x: 1 }, 2, 3), 6, "call");
eq(function (a, b) { return a * b; }.apply(null, [4, 5]), 20, "apply");
const objArrow = { v: 7, get() { return [1].map(() => this.v); } };
eq(objArrow.get(), [7], "arrow this");
const fns = [];
for (let k = 0; k < 3; k++) fns.push(() => k);
eq(fns.map(f => f()), [0, 1, 2], "let per-iteration");
const fns2 = [];
for (const k of [5, 6]) fns2.push(() => k);
eq(fns2.map(f => f()), [5, 6], "for-of closures");
const iife = (function () { return "iife"; })();
eq(iife, "iife", "iife");
eq(((a, { b, c = 3 }, [d, ...e]) => [a, b, c, d, e])(1, { b: 2 }, [4, 5, 6]), [1, 2, 3, 4, [5, 6]], "destructuring params");

// --- destructuring
const { p, q: renamed, r = 9, ...others } = { p: 1, q: 2, s: 3, t: 4 };
eq([p, renamed, r, others], [1, 2, 9, { s: 3, t: 4 }], "object destructure");
const [d1, , d3 = 30, ...drest] = [1, 2, undefined, 4, 5];
eq([d1, d3, drest], [1, 30, [4, 5]], "array destructure");
let sa = 1, sb = 2;
[sa, sb] = [sb, sa];
eq([sa, sb], [2, 1], "swap");
const { deep: { inner } } = { deep: { inner: "x" } };
eq(inner, "x", "nested destructure");
let ao, bo;
({ ao, bo } = { ao: 1, bo: 2 });
eq([ao, bo], [1, 2], "assignment destructure");
for (const [k, v] of Object.entries({ m: 1 })) eq([k, v], ["m", 1], "for-of destructure");

// --- control flow
let s = "";
for (let i = 0; i < 5; i++) { if (i == 1) continue; if (i == 4) break; s += i; }
eq(s, "023", "for continue break");
let w = 0; while (w < 10) w++; eq(w, 10, "while");
let dw = 0; do { dw++; } while (dw < 0); eq(dw, 1, "do-while");
const keys = []; for (const k in { a: 1, b: 2 }) keys.push(k); eq(keys, ["a", "b"], "for-in");
const idx = []; for (const k in [7, 8]) idx.push(k); eq(idx, ["0", "1"], "for-in array");
function sw(v) {
  switch (v) {
    case 1: return "one";
    case 2:
    case 3: return "two-three";
    default: return "other";
  }
}
eq([sw(1), sw(3), sw(9)], ["one", "two-three", "other"], "switch");
let fall = ""; switch (2) { case 1: fall += "1"; case 2: fall += "2"; case 3: fall += "3"; break; case 4: fall += "4"; }
eq(fall, "23", "switch fallthrough");
let nested = 0;
for (let a = 0; a < 3; a++) for (let b = 0; b < 3; b++) { if (b == 1) break; nested++; }
eq(nested, 3, "nested loops");
function loopReturn() { for (const v of [1, 2, 3]) if (v == 2) return v; }
eq(loopReturn(), 2, "return from loop");

// --- exceptions
try { throw new Error("boom"); } catch (e) { eq(e.message, "boom", "catch message"); eq(e instanceof Error, true, "instanceof"); }
try { null.x; } catch (e) { eq(e instanceof TypeError, true, "TypeError"); }
try { undefinedVar; } catch (e) { eq(e.name, "ReferenceError", "ReferenceError name"); }
let fin = [];
function tf() { try { fin.push(1); return "try"; } finally { fin.push(2); } }
eq(tf(), "try", "finally return"); eq(fin, [1, 2], "finally ran");
function tf2() { try { throw 1; } catch { return "caught"; } finally { fin.push(3); } }
eq(tf2(), "caught", "optional catch binding");
function tf3() { try { return 1; } finally { return 2; } }
eq(tf3(), 2, "finally override");
try { try { throw "inner"; } finally { fin.push(4); } } catch (e) { eq(e, "inner", "rethrow through finally"); }
throws(() => { const c = 1; c = 2; }, TypeError, "const assign");
throws(() => { (void 0)(); }, TypeError, "call undefined");
throws(() => { function r() { return r(); } r(); }, RangeError, "stack overflow");
class MyErr extends Error { constructor(m) { super(m); this.name = "MyErr"; } }
try { throw new MyErr("custom"); } catch (e) { eq([e instanceof MyErr, e instanceof Error, e.message, String(e)], [true, true, "custom", "MyErr: custom"], "custom error"); }

// --- classes
class Animal {
  static count = 0;
  legs = 4;
  #secret = "s";
  constructor(name) { this.name = name; Animal.count++; }
  speak() { return `${this.name} makes a sound`; }
  get desc() { return `${this.name} (${this.legs})`; }
  set nick(v) { this.name = v; }
  secret() { return this.#secret; }
  static create(n) { return new this(n); }
}
class Dog extends Animal {
  constructor(name) { super(name); this.tricks = []; }
  speak() { return super.speak() + " (woof)"; }
}
class Puppy extends Dog { legs = 3; }
const d = new Dog("Rex");
eq(d.speak(), "Rex makes a sound (woof)", "super method");
eq(d.legs, 4, "inherited field");
eq(d.desc, "Rex (4)", "getter on class");
d.nick = "Max"; eq(d.name, "Max", "setter on class");
eq(d instanceof Dog && d instanceof Animal, true, "instanceof chain");
eq(Animal.count, 1, "static field");
const pup = new Puppy("Bit");
eq([pup.legs, pup.name, pup.tricks], [3, "Bit", []], "implicit ctor");
eq(Animal.create("Cat").name, "Cat", "static method this");
eq(Dog.create("Fido") instanceof Dog, true, "inherited static");
eq(d.secret(), "s", "private-ish field");
eq(typeof Animal, "function", "class typeof");
throws(() => Animal("x"), TypeError, "class without new");
function OldStyle(v) { this.v = v; }
OldStyle.prototype.get = function () { return this.v; };
eq(new OldStyle(3).get(), 3, "prototype functions");
eq(new OldStyle(3).constructor === OldStyle, true, "constructor prop");
class Point { constructor(x, y) { this.x = x; this.y = y; } toString() { return `(${this.x}, ${this.y})`; } }
eq(`${new Point(1, 2)}`, "(1, 2)", "custom toString");
eq(String([new Point(1, 2)]), "(1, 2)", "toString in join");
const Expr = class { v() { return 1; } };
eq(new Expr().v(), 1, "class expression");

// --- Map & Set
const m = new Map([["a", 1]]);
m.set("b", 2).set("a", 3);
eq([m.get("a"), m.get("b"), m.size, m.has("c")], [3, 2, 2, false], "map");
eq([...m], [["a", 3], ["b", 2]], "map spread");
m.delete("a"); eq(m.size, 1, "map delete");
const objKey = {}; m.set(objKey, "obj"); eq(m.get(objKey), "obj", "object key");
const st = new Set([1, 2, 2, 3]);
eq([st.size, st.has(2), [...st]], [3, true, [1, 2, 3]], "set");
let mfe = []; new Map([[1, "x"]]).forEach((v, k) => mfe.push(k, v)); eq(mfe, [1, "x"], "map forEach");

// --- Math & Number
eq(Math.round(2.5), 3, "round");
eq(Math.round(-2.5), -2, "round neg");
eq(Math.floor(-1.5), -2, "floor");
eq(Math.min(3, 1, 2), 1, "min");
eq(Math.hypot(3, 4), 5, "hypot");
eq(Math.abs(-3), 3, "abs");
const rnd = Math.random(); eq(rnd >= 0 && rnd < 1, true, "random");
eq((3.14159).toFixed(2), "3.14", "toFixed");
eq((255).toString(16), "ff", "toString radix");
eq((5).toString(2), "101", "toString binary");
eq((1234.5678).toPrecision(6), "1234.57", "toPrecision");
eq(parseInt("42px"), 42, "parseInt");
eq(parseInt("ff", 16), 255, "parseInt radix");
eq(parseFloat("3.5e2xyz"), 350, "parseFloat");
eq(Number("12"), 12, "Number()");
eq(Number.isInteger(5.0), true, "isInteger");
eq(String(123), "123", "String()");
eq(Boolean(""), false, "Boolean()");
eq(!!"x", true, "double not");

// --- misc semantics
var hoistVar = 1;
{ var hoistVar = 2; }
eq(hoistVar, 2, "var function scope");
let shadow = 1;
{ let shadow = 2; eq(shadow, 2, "block shadow"); }
eq(shadow, 1, "block scope");
const noSemi = 1
const noSemi2 = 2
eq(noSemi + noSemi2, 3, "ASI")
function asiReturn() {
  return
  42;
}
eq(asiReturn(), undefined, "ASI return");
eq([1, 2, 3].map(String), ["1", "2", "3"], "native as callback");
eq(typeof globalFromAssign, "undefined", "pre global");
(function () { globalFromAssign = 1; })();
eq(globalFromAssign, 1, "implicit global");
eq(String(null), "null", "String null");
eq(String([1, [2, 3]]), "1,2,3", "array toString");
eq(String({}), "[object Object]", "object toString");
eq([] + [], "", "array plus");
eq([1] == 1, true, "array loose eq");
const label = { valueOf() { return 42; } };
eq(label + 1, 43, "valueOf");

// --- garbage collector stress: lots of garbage, closures, strings
let total = 0;
for (let n = 0; n < 20000; n++) {
  const tmp = { n, arr: [n, n + 1], s: "item" + n };
  total += tmp.arr[1] - tmp.n + tmp.s.length;
}
eq(total, 188890, "gc loop");
let chain = null;
for (let n = 0; n < 5000; n++) chain = { next: chain, v: n };
let cl = 0; while (chain) { cl++; chain = chain.next; }
eq(cl, 5000, "linked list survives gc");
const big = []; for (let n = 0; n < 3000; n++) big.push(() => n);
eq(big[1234](), 1234, "many closures");
let strs = ""; for (let n = 0; n < 2000; n++) strs += "x"; eq(strs.length, 2000, "string building");
const keep = [1, 2, 3].map(v => ({ v })); gc(); const junk = []; for (let n = 0; n < 5000; n++) junk.push({ n });
eq(keep.map(k => k.v), [1, 2, 3], "values survive explicit gc");

// --- Date
{
  const d = new Date(Date.UTC(2026, 9, 14, 23, 30, 5, 250));  // months are 0-based
  eq(d.getTime(), 1792020605250, "Date.UTC");
  eq([d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds()],
     [2026, 9, 14, 23, 30, 5, 250], "UTC getters");
  eq(d.getUTCDay(), 3, "weekday (Wednesday)");
  eq(d.toISOString(), "2026-10-14T23:30:05.250Z", "toISOString");
  eq(JSON.stringify({ d }), '{"d":"2026-10-14T23:30:05.250Z"}', "toJSON in stringify");
  eq(new Date("2026-10-14T23:30:05.250Z").getTime(), d.getTime(), "parse ISO Z");
  eq(new Date("2026-10-14").getTime(), Date.UTC(2026, 9, 14), "date-only is UTC");
  eq(Date.parse("2026-10-14T18:30:05.250-05:00"), d.getTime(), "parse offset");
  eq(isNaN(Date.parse("nonsense")), true, "parse invalid");
  eq(new Date(d).getTime(), d.getTime(), "copy");
  eq(new Date(0).toUTCString(), "Thu, 01 Jan 1970 00:00:00 GMT", "toUTCString epoch");
  eq(new Date(Date.UTC(1969, 11, 31)).getUTCDay(), 3, "before epoch");
  eq(new Date(Date.UTC(2024, 1, 29)).toISOString().slice(0, 10), "2024-02-29", "leap day");
  eq(new Date(Date.UTC(2026, 0, 32)).getUTCMonth(), 1, "day overflow rolls month");
  const local = new Date(2026, 6, 4, 9, 15, 30);
  eq([local.getFullYear(), local.getMonth(), local.getDate(), local.getHours(), local.getMinutes(), local.getSeconds()],
     [2026, 6, 4, 9, 15, 30], "local round trip");
  eq(new Date("2026-07-04T09:15:30").getTime(), local.getTime(), "local ISO without offset");
  eq(local.getTime() - local.getTimezoneOffset() * 60000 - Date.UTC(2026, 6, 4, 9, 15, 30), 0, "timezone offset");
  const s = new Date(Date.UTC(2026, 0, 31));
  s.setUTCMonth(1);
  eq(s.toISOString().slice(0, 10), "2026-03-03", "setUTCMonth overflow");
  s.setUTCFullYear(2030, 5, 15);
  eq(s.toISOString().slice(0, 10), "2030-06-15", "setUTCFullYear");
  s.setUTCHours(25);
  eq(s.getUTCDate(), 16, "setUTCHours overflow");
  eq(new Date(2026, 0, 1) < new Date(2026, 0, 2), true, "compare");
  eq(new Date(2026, 0, 2) - new Date(2026, 0, 1), 86400000, "subtract");
  eq(new Date(NaN).toString(), "Invalid Date", "invalid toString");
  throws(() => new Date(NaN).toISOString(), RangeError, "invalid toISOString");
  eq(typeof Date(), "string", "Date() is a string");
  eq(new Date() instanceof Date, true, "instanceof");
  eq(Object.keys(new Date()).length, 0, "no visible properties");
  eq(new Date(99, 0).getFullYear(), 1999, "two-digit year");
  eq(typeof new Date().toLocaleString(), "string", "toLocaleString");
}

// --- timers (checked after the event loop runs)
const order = [];
setTimeout(() => order.push("t2"), 20);
setTimeout(() => order.push("t1"), 5);
let ticks = 0;
const iv = setInterval(() => { if (++ticks == 3) clearInterval(iv); }, 5);
setTimeout((a, b) => {
  eq(order, ["t1", "t2"], "timer order");
  eq(ticks, 3, "interval cleared");
  eq(a + b, 3, "timer args");
  console.log(`${passed} passed, ${failed} failed`);
}, 60, 1, 2);
