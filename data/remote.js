// Remote: a web page for the board. Connects to your WiFi (micro-js.json) and serves a page you
// open on a phone or computer on the same network: the LED colour mixer (red / green / blue /
// brightness / cycle speed, Auto, LED on/off), a beep button with a pitch slider, and a chat.
// The board's screen shows the address, the same mixer (changes on either side show on both) and
// the chat; on boards with a keyboard, type a message and press Enter to send it.
// If it can't join your network (none set, wrong password, out of range) it starts its own: join
// the WiFi network it shows ("micro-js-xxxx", or "ap": {"ssid": ..., "pass": ...} in
// micro-js.json - a password needs 8+ characters) and the page opens by itself on most phones,
// or go to http://192.168.4.1/. (launch("/remote.js", "ap") goes straight to its own network.)
// Anyone on your network can open the page - there's no password. The page itself is
// /remote.html, sent as it is (kept out of this script so small boards have the memory).
// The on-screen mixer needs ~30 KB to load. It's loaded after WiFi is up, and only when that much
// is still free; on boards without PSRAM (the CYD, the Cardputer) there usually isn't, so the screen
// shows a simple swatch and the page does the mixing.
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
// WiFi first (it needs ~50 KB of its own), then the full mixer if there's still room for it
function loadMixer() {
  const h = sys.heap();
  if (!board.psram && (h.free < 70000 || h.largest < 30000)) return false;
  try { load("/lib/mixer.js"); return true; } catch (e) { return false; }
}
function simpleMixer(area) {
  const st = { r: 255, g: 60, b: 0, bright: 30, speed: 40, auto: false, ledOn: true, hue: 0 };
  function out() {
    const k = st.bright / 100, f = 0.3 + 0.7 * k;
    if (board.hasLed) led(st.ledOn ? Math.round(st.r * k) : 0, st.ledOn ? Math.round(st.g * k) : 0, st.ledOn ? Math.round(st.b * k) : 0);
    lcd.fillRoundRect(area.x, area.y, area.w * 0.4, area.h, 8, lcd.color(st.r * f, st.g * f, st.b * f));
    lcd.fillRect(area.x + area.w * 0.4 + 6, area.y, area.w * 0.6 - 6, area.h, lcd.BLACK);
    lcd.setTextDatum(lcd.TL);
    lcd.setTextColor(lcd.WHITE);
    [`red ${st.r}  green ${st.g}  blue ${st.b}`, `brightness ${st.bright}%`, `auto ${st.auto ? "cycling, speed " + st.speed + "%" : "off"}`,
     `LED ${st.ledOn ? "on" : "off"}`, "", "Change it from the web page."].forEach((t, i) => lcd.drawString(t, area.x + area.w * 0.4 + 10, area.y + 4 + i * 12, 1));
  }
  return {
    state: st, draw: out, down() {}, move() {}, up() {}, key() {},
    tick() {
      if (!st.auto) return;
      st.hue = (st.hue + 0.3 + st.speed / 8) % 360;
      const f = (n) => { const k = (n + st.hue / 60) % 6; return Math.round(255 * (1 - Math.max(0, Math.min(k, 4 - k, 1)))); };
      st.r = f(5); st.g = f(3); st.b = f(1);
      out();
    },
  };
}
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, LH = 9;
const TOP = SMALL ? 11 : 18, MIXH = SMALL ? 78 : 122, CHATY = TOP + MIXH + 4;
const cfg = readConfig();

function status(msg, color) {
  lcd.fillRect(0, 0, W, TOP, lcd.color(20, 24, 48));
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(color || lcd.WHITE);
  lcd.drawString(msg, 4, TOP / 2 + 1, SMALL ? 1 : 2);
}

let mixer, chat = [], nextId = 1, typing = "", beepFlash = 0;

let ap = null;  // our own network, when there's no other: { ssid, ip, password }
function headline() {
  return ap ? `WiFi ${ap.ssid}  ${web.url()}` : web.url();
}
function start() {
  web.start(80);
  status(headline(), ap ? lcd.CYAN : lcd.GREEN);
  mixer = (loadMixer() ? makeMixer : simpleMixer)({ x: 4, y: TOP + 2, w: W - 8, h: MIXH });
  mixer.draw();
  if (ap) addChat("board", `Join the WiFi network "${ap.ssid}"${ap.password ? " (password " + ap.password + ")" : ""}, then open ${web.url()} - most phones open it by themselves.`);
  else addChat("board", "Open " + web.url() + " on a phone or computer on this network.");
  web.on("/", () => ({ file: "/remote.html" }));  // streamed from flash: never in memory
  // any other page: the remote too, so a phone's "is there internet?" check (on our own network)
  // gets it and shows it as a sign-in page
  web.on("*", (req) => (req.path.startsWith("/api/") ? { status: 404, body: "not found" } : { file: "/remote.html" }));
  web.on("/api/state", (req) => state(+req.query.since || 0));
  web.on("/api/led", (req) => {
    const st = mixer.state, q = req.query;
    for (const k of ["r", "g", "b", "bright", "speed"]) if (q[k] !== undefined) st[k] = Math.max(0, Math.min(k === "bright" || k === "speed" ? 100 : 255, +q[k] | 0));
    if (q.r !== undefined || q.g !== undefined || q.b !== undefined) st.auto = false;
    if (q.auto !== undefined) st.auto = q.auto === "1";
    if (q.led !== undefined) st.ledOn = q.led === "1";
    mixer.draw();
    return state(1e9);
  });
  web.on("/api/beep", (req) => {
    const f = Math.max(100, Math.min(4000, +req.query.f || 880)), ms = Math.max(20, Math.min(1000, +req.query.ms || 150));
    tone(f, ms);
    beepFlash = millis() + 400;
    status("Beep " + f + " Hz from " + req.ip, lcd.YELLOW);
    return { ok: true };
  });
  web.on("/api/chat", (req) => {
    const text = String(req.body || req.query.text || "").trim().slice(0, 200), name = String(req.query.name || "web").trim().slice(0, 16) || "web";
    if (text) { addChat(name, text); tone(1400, 30); }
    return state(+req.query.since || 0);
  });
}

function state(since) {
  const st = mixer.state;
  return { r: st.r, g: st.g, b: st.b, bright: st.bright, speed: st.speed, auto: st.auto, led: st.ledOn, board: board.name, chat: chat.filter((m) => m.id > since) };
}

// ---- chat, on the screen below the mixer
function addChat(name, text) {
  chat.push({ id: nextId++, name, text });
  if (chat.length > 40) chat.shift();
  drawChat();
}
function wrap(line, maxPx) {  // split into lines that fit maxPx (measured in font 1)
  const out = [];
  while (lcd.textWidth(line, 1) > maxPx) {
    let cut = line.length;
    while (cut > 1 && lcd.textWidth(line.slice(0, cut), 1) > maxPx) cut--;
    const sp = line.lastIndexOf(" ", cut);
    if (sp > 0) cut = sp;
    out.push(line.slice(0, cut));
    line = line.slice(cut).trim();
  }
  out.push(line);
  return out;
}
function drawChat() {
  if (!mixer) return;
  const inputRows = board.hasKeyboard ? 1 : 0, rows = Math.floor((H - CHATY) / LH) - inputRows;
  const maxPx = W - 6;
  let lines = [];
  for (const m of chat) for (const l of wrap(m.name + ": " + m.text, maxPx)) lines.push([m.name, l]);
  lines = lines.slice(-rows);
  lcd.fillRect(0, CHATY, W, H - CHATY, lcd.BLACK);
  lcd.setTextDatum(lcd.TL);
  lines.forEach(([name, l], i) => {
    lcd.setTextColor(name === "board" ? lcd.DARKGREY : name === board.name ? lcd.CYAN : lcd.WHITE);
    lcd.drawString(l, 3, CHATY + i * LH, 1);
  });
  if (inputRows) {
    lcd.fillRect(0, H - LH - 1, W, LH + 1, lcd.color(25, 25, 35));
    lcd.setTextColor(lcd.YELLOW);
    let shown = typing;  // the end of what's typed, as much as fits
    while (shown && lcd.textWidth("> " + shown + "_", 1) > maxPx) shown = shown.slice(1);
    lcd.drawString("> " + shown + "_", 3, H - LH, 1);
  }
}

// ---- input on the board
const ARROWS = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
keyboard.on("key", ({ key }) => {
  if (!mixer) return;
  if (ARROWS[key]) return mixer.key(ARROWS[key]);
  if (key === "Enter") {
    if (typing.trim()) { addChat(board.name, typing.trim()); typing = ""; drawChat(); }
    else mixer.key("enter");
    return;
  }
  if (key === "Backspace") typing = typing.slice(0, -1);
  else if (key.length === 1 && typing.length < 200) typing += key;
  else return;
  drawChat();
});
touch.on("down", ({ x, y }) => mixer && mixer.down(x, y));
touch.on("move", ({ x, y }) => mixer && mixer.move(x, y));
touch.on("up", () => mixer && mixer.up());
setInterval(() => {
  if (!mixer) return;
  mixer.tick();
  if (beepFlash && millis() > beepFlash) { beepFlash = 0; status(headline(), ap ? lcd.CYAN : lcd.GREEN); }
}, 40);

// ---- go: join your network, or else make our own, then serve
lcd.fillScreen(lcd.BLACK);
let joined = false;
const forceAp = typeof ARG === "string" && ARG === "ap";  // launch("/remote.js", "ap"): own network only
if (cfg.wifi && cfg.wifi.ssid && !forceAp) {
  status("Connecting to " + cfg.wifi.ssid + "...", lcd.YELLOW);
  joined = wifi.connect(cfg.wifi.ssid, cfg.wifi.pass, 15000);
}
if (!joined) {
  status("Starting its own WiFi network...", lcd.YELLOW);
  const own = cfg.ap || {};
  try { ap = wifi.ap(own.ssid, own.pass); } catch (e) { status(String(e.message || e), lcd.RED); }
}
if (joined || ap) start();
