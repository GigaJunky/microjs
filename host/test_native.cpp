// Native code looking up lazy built-ins by names no script has used yet (regression test).
#include "../src/js.h"
#include <cstdio>
using namespace js;
int main() {
  Interp I; I.write = [](const char* s, size_t n){ fwrite(s,1,n,stdout); };
  // native code looks up built-ins whose names no script has mentioned yet
  Value json = I.getGlobal("JSON"), text = I.str("{\"a\":[1,2]}");
  Value parsed = I.call(I.get(json, "parse"), json, &text, 1);
  Value out = I.call(I.get(json, "stringify"), json, &parsed, 1);
  Value o = Value::obj(I.newObject());
  printf("%s %s\n", I.toStr(out).c_str(), I.toStr(o).c_str());   // String(obj) needs a lazy toString
  return I.toStr(out) == "{\"a\":[1,2]}" && I.toStr(o) == "[object Object]" ? 0 : 1;
}
