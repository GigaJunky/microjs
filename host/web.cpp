// web.cpp - serves the simulated CYD in a browser: live screen, mouse as touch, console,
// a JavaScript prompt and a BOOT button. Single-threaded: requests are handled between
// interpreter steps, so nothing touches the interpreter concurrently.
#include "web.h"
#include "sim.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static const char* PAGE = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>micro-js Simulator</title>
<style>
  :root { color-scheme: dark; --bg:#14161a; --panel:#1d2026; --line:#2c313a; --text:#e6e8eb; --dim:#8b93a1; --accent:#f5c518; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font:14px/1.4 system-ui, sans-serif; }
  main { max-width: 1100px; margin: 0 auto; padding: 20px 16px; display:flex; gap:20px; flex-wrap:wrap; align-items:flex-start; }
  h1 { font-size:16px; margin:0 0 12px; color:var(--accent); letter-spacing:.02em; }
  .device { background:#e8c63a; padding:18px 18px 14px; border-radius:14px; box-shadow:0 6px 24px #0008; }
  canvas { display:block; width:640px; height:480px; max-width:calc(100vw - 68px); height:auto; image-rendering:pixelated;
           background:#000; border-radius:4px; cursor:crosshair; touch-action:none; }
  .bar { display:flex; align-items:center; gap:12px; margin-top:12px; color:#3a3000; font-weight:600; }
  .led { width:16px; height:16px; border-radius:50%; background:#111; box-shadow:0 0 0 2px #3a3000 inset; }
  button { font:inherit; font-weight:600; padding:6px 14px; border-radius:6px; border:0; cursor:pointer; background:#3a3000; color:#f2d24b; }
  .side { flex:1; min-width:300px; display:flex; flex-direction:column; gap:10px; }
  #log { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:10px; height:420px; overflow:auto;
         font:12px/1.45 ui-monospace, monospace; white-space:pre-wrap; word-break:break-word; margin:0; }
  form { display:flex; gap:8px; }
  input { flex:1; font:13px ui-monospace, monospace; padding:8px 10px; border-radius:6px; border:1px solid var(--line);
          background:var(--panel); color:var(--text); }
  .hint { color:var(--dim); font-size:12px; }
</style></head>
<body><main>
  <div>
    <h1>micro-js Simulator</h1>
    <div class="device">
      <canvas id="screen" width="320" height="240"></canvas>
      <div class="bar"><span>RGB LED</span><span class="led" id="led"></span><span style="flex:1"></span>
        <button id="boot" title="Stop the script and restart">BOOT</button></div>
    </div>
  </div>
  <div class="side">
    <pre id="log"></pre>
    <form id="repl"><input id="code" placeholder="JavaScript, e.g. lcd.fillScreen(lcd.RED)" autocomplete="off" spellcheck="false"><button>Run</button></form>
    <div class="hint">Click or drag on the screen to touch it; your keyboard is the board's keyboard (click the screen first). Fonts are approximated; colours and layout match the board.</div>
  </div>
</main>
<script>
const cv = document.getElementById("screen"), ctx = cv.getContext("2d");
const logEl = document.getElementById("log"), led = document.getElementById("led");
let img = null, logPos = 0;
async function frame() {
  try {
    const buf = await (await fetch("/frame")).arrayBuffer();
    const v = new DataView(buf), w = v.getUint16(0, true), h = v.getUint16(2, true);
    if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; cv.style.width = w * 2 + "px"; img = null; }
    if (!img) img = ctx.createImageData(w, h);
    const px = new Uint16Array(buf, 4, w * h), d = img.data;
    for (let i = 0, j = 0; i < px.length; i++, j += 4) {
      const c = px[i];
      d[j] = ((c >> 11) & 31) * 255 / 31; d[j + 1] = ((c >> 5) & 63) * 255 / 63; d[j + 2] = (c & 31) * 255 / 31; d[j + 3] = 255;
    }
    ctx.putImageData(img, 0, 0);
    const l = new Uint8Array(buf, 4 + w * h * 2, 3);
    led.style.background = `rgb(${Math.min(255, l[0] * 4)},${Math.min(255, l[1] * 4)},${Math.min(255, l[2] * 4)})`;
  } catch (e) {}
  setTimeout(frame, 40);
}
async function pollLog() {
  try {
    const t = await (await fetch("/log?from=" + logPos)).text(), nl = t.indexOf("\n");
    logPos = +t.slice(0, nl);
    const add = t.slice(nl + 1);
    if (add) { const atEnd = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 4; logEl.textContent += add; if (atEnd) logEl.scrollTop = logEl.scrollHeight; }
  } catch (e) {}
  setTimeout(pollLog, 250);
}
let down = false, lastMove = 0;
function touch(type, e) {
  const r = cv.getBoundingClientRect();
  const x = Math.round((e.clientX - r.left) * cv.width / r.width), y = Math.round((e.clientY - r.top) * cv.height / r.height);
  fetch("/touch", { method: "POST", body: `${type} ${x} ${y}` });
}
cv.addEventListener("pointerdown", e => { down = true; cv.setPointerCapture(e.pointerId); touch("down", e); });
cv.addEventListener("pointermove", e => { if (down && performance.now() - lastMove > 30) { lastMove = performance.now(); touch("move", e); } });
cv.addEventListener("pointerup", e => { if (down) { down = false; touch("up", e); } });
document.getElementById("boot").onclick = () => fetch("/boot", { method: "POST" });
// your keyboard is the board's keyboard (except while typing in the prompt)
const sendKey = (type, e) => {
  if (e.target === code || e.metaKey || e.ctrlKey) return;
  const k = e.key === "Unidentified" ? "" : e.key;
  if (!k || k === "Shift" || k === "Control" || k === "Alt" || k === "Meta") return;
  if (type === "down" && e.repeat && !k.startsWith("Arrow")) return;
  fetch("/key", { method: "POST", body: `${type} ${k}` });
  if (k.startsWith("Arrow") || k === " " || k === "Tab" || k === "Backspace") e.preventDefault();
};
document.addEventListener("keydown", e => sendKey("down", e));
document.addEventListener("keyup", e => sendKey("up", e));
const history = []; let hpos = 0;
const code = document.getElementById("code");
document.getElementById("repl").onsubmit = e => {
  e.preventDefault();
  if (!code.value.trim()) return;
  history.push(code.value); hpos = history.length;
  fetch("/run", { method: "POST", body: code.value }); code.value = "";
};
code.onkeydown = e => {
  if (e.key === "ArrowUp" && hpos > 0) { code.value = history[--hpos]; e.preventDefault(); }
  if (e.key === "ArrowDown" && hpos < history.length) { code.value = history[++hpos] ?? ""; e.preventDefault(); }
};
frame(); pollLog();
</script></body></html>
)HTML";

static void sendAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += n;
  }
}

static void respond(int fd, const char* type, const std::string& body) {
  sendAll(fd, std::string("HTTP/1.1 200 OK\r\nContent-Type: ") + type + "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
}

static int listenOn(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this computer only
  if (bind(fd, (sockaddr*)&a, sizeof a) < 0 || listen(fd, 16) < 0) {
    perror("simulator web server");
    exit(2);
  }
  fcntl(fd, F_SETFL, O_NONBLOCK);
  return fd;
}

// Handle one request. Returns false when the browser asked for BOOT.
static bool handle(int fd, WebSim& sim) {
  std::string req;
  char buf[8192];
  size_t headerEnd = std::string::npos, need = 0;
  for (;;) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    req.append(buf, n);
    if (headerEnd == std::string::npos && (headerEnd = req.find("\r\n\r\n")) != std::string::npos) {
      size_t cl = req.find("Content-Length:");
      if (cl == std::string::npos) cl = req.find("content-length:");
      need = headerEnd + 4 + (cl != std::string::npos && cl < headerEnd ? atoi(req.c_str() + cl + 15) : 0);
    }
    if (headerEnd != std::string::npos && req.size() >= need) break;
  }
  if (headerEnd == std::string::npos) return true;
  std::string method = req.substr(0, req.find(' '));
  std::string path = req.substr(method.size() + 1, req.find(' ', method.size() + 1) - method.size() - 1);
  std::string body = req.substr(headerEnd + 4);
  bool keepRunning = true;
  if (path == "/") respond(fd, "text/html; charset=utf-8", PAGE);
  else if (path == "/frame") {
    int w, h, r, g, b;
    const uint16_t* px = simScreen(w, h);
    simLed(r, g, b);
    std::string out(4 + w * h * 2 + 3, '\0');
    uint16_t dims[2] = {(uint16_t)w, (uint16_t)h};
    memcpy(&out[0], dims, 4);
    memcpy(&out[4], px, w * h * 2);
    out[4 + w * h * 2] = (char)std::min(255, std::max(0, r));
    out[5 + w * h * 2] = (char)std::min(255, std::max(0, g));
    out[6 + w * h * 2] = (char)std::min(255, std::max(0, b));
    respond(fd, "application/octet-stream", out);
  } else if (path.rfind("/log", 0) == 0) {
    size_t from = 0, q = path.find("from=");
    if (q != std::string::npos) from = strtoul(path.c_str() + q + 5, nullptr, 10);
    if (from > sim.log.size()) from = 0;
    respond(fd, "text/plain; charset=utf-8", std::to_string(sim.log.size()) + "\n" + sim.log.substr(from));
  } else if (path == "/touch" && method == "POST") {
    char type[8] = {0};
    int x = 0, y = 0;
    if (sscanf(body.c_str(), "%7s %d %d", type, &x, &y) == 3) {
      const char* t = !strcmp(type, "down") ? "down" : !strcmp(type, "move") ? "move" : !strcmp(type, "up") ? "up" : nullptr;
      if (t) simTouchNow(t, x, y);
    }
    respond(fd, "text/plain", "ok");
  } else if (path == "/key" && method == "POST") {  // "down Enter" / "up a"
    size_t sp = body.find(' ');
    if (sp != std::string::npos) simKeyNow(body.substr(0, sp) == "down", body.substr(sp + 1));
    respond(fd, "text/plain", "ok");
  } else if (path == "/run" && method == "POST") {
    respond(fd, "text/plain", "ok");
    sim.runCode(body);
  } else if (path == "/boot" && method == "POST") {
    respond(fd, "text/plain", "ok");
    keepRunning = false;
  } else {
    sendAll(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  }
  return keepRunning;
}

void serveSim(WebSim& sim, int port) {
  int ls = listenOn(port);
  printf("Simulator running: open http://localhost:%d in your browser (Ctrl+C to stop)\n", port);
  fflush(stdout);
  for (;;) {
    sim.boot();
    bool running = true;
    while (running) {
      for (;;) {  // handle pending requests
        int fd = accept(ls, nullptr, nullptr);
        if (fd < 0) break;
        timeval tv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        bool ok = handle(fd, sim);
        close(fd);
        if (!ok) { running = false; break; }
      }
      sim.step();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sim.log += "\n--- BOOT: restarting ---\n";
  }
}
