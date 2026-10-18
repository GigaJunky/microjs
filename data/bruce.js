// Bruce-style device multi-tool: a categorized menu of on-device diagnostics, in the spirit of
// the Bruce ESP32 firmware. Tools are benign: a WiFi analyzer, a system monitor, LED control and
// a sensor readout. Works on touch (CYD) and keyboard (Cardputer). BOOT returns to the launcher.
//   Keys: ; . move, Enter open/act, , back. Touch: tap an item; tap the top bar to go back.
//   RGB LED tool: sliders for red, green, blue, brightness and cycle speed, Auto colour cycling,
//   LED on/off (shared with controls.js); there , / change a slider and Backspace goes back.
load("/lib/mixer.js");
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
const TOP = SMALL ? 16 : 26, ROW = SMALL ? 16 : 30, F = SMALL ? 1 : 2;
const ACCENT = lcd.color(130, 90, 230), BG = lcd.BLACK, BAR = lcd.color(30, 20, 60);

function text(str, x, y, font, color, datum) {
  lcd.setTextDatum(datum ?? lcd.TL);
  lcd.setTextColor(color ?? lcd.WHITE, BG);
  lcd.drawString(String(str), x, y, font);
}

function header(title) {
  lcd.fillRect(0, 0, W, TOP, BAR);
  text(title, 4, (TOP - 8 * F) / 2, F, ACCENT);
  if (screen !== menu) text(board.hasTouch ? "< back" : screen.ownsArrows ? "< Bksp" : "< ,", W - 4, (TOP - 8 * F) / 2, F, lcd.DARKGREY, lcd.TR);
}

// ---- a screen is { title, enter(), draw(), key(name), tap(x,y), tick() }
let screen = null, sel = 0, timer = 0;

function go(s) {
  clearInterval(timer);
  timer = 0;
  screen = s;
  sel = 0;
  lcd.fillScreen(BG);
  if (s.enter) s.enter();
  header(s.title);
  s.draw();
  if (s.tick) timer = setInterval(() => (screen === s ? s.tick() : null), s.every ?? 1000);
}

// ---- main menu: a scrolling list of tools
const tools = [];  // filled in below
const menu = {
  title: "Bruce  -  tools",
  draw() {
    const per = Math.floor((H - TOP) / ROW);
    const first = Math.floor(sel / per) * per;
    lcd.fillRect(0, TOP, W, H - TOP, BG);
    tools.slice(first, first + per).forEach((t, i) => {
      const y = TOP + i * ROW, on = first + i === sel;
      if (on) lcd.fillRect(0, y, W, ROW, ACCENT);
      text(`${t.icon}  ${t.name}`, 6, y + (ROW - 8 * F) / 2, F, on ? lcd.WHITE : lcd.LIGHTGREY);
    });
  },
  key(k) {
    if (k === "up") sel = (sel + tools.length - 1) % tools.length;
    else if (k === "down") sel = (sel + 1) % tools.length;
    else if (k === "enter" || k === "right") return go(tools[sel]);
    else if (k >= "1" && k <= "9" && +k <= tools.length) { sel = +k - 1; return go(tools[sel]); }
    else return;
    this.draw();
  },
  tap(x, y) {
    if (y < TOP) return;
    const per = Math.floor((H - TOP) / ROW), first = Math.floor(sel / per) * per;
    const i = first + Math.floor((y - TOP) / ROW);
    if (i < tools.length) { sel = i; go(tools[i]); }
  },
};

// ---- bars helper for signal strength
function bars(x, y, rssi) {
  const n = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : rssi > -88 ? 1 : 0;
  for (let i = 0; i < 4; i++) {
    const h = 3 + i * 3;
    lcd.fillRect(x + i * 4, y + 12 - h, 3, h, i < n ? lcd.GREEN : lcd.color(40, 40, 40));
  }
}

// ---- Tool: WiFi analyzer (boards with WiFi)
if (board.hasWifi !== false) tools.push({
  icon: "W", name: "WiFi scan",
  title: "WiFi scan", every: 6000,
  nets: null,
  enter() { this.scan(); },
  scan() {
    text("scanning...", 6, TOP + 6, F, lcd.YELLOW);
    this.nets = wifi.scan().sort((a, b) => b.rssi - a.rssi);
    this.draw();
  },
  draw() {
    lcd.fillRect(0, TOP, W, H - TOP, BG);
    if (!this.nets || !this.nets.length) return text("no networks found", 6, TOP + 6, F, lcd.ORANGE);
    const per = Math.floor((H - TOP - 10) / (SMALL ? 14 : 20));
    this.nets.slice(0, per).forEach((net, i) => {
      const y = TOP + 4 + i * (SMALL ? 14 : 20);
      bars(4, y, net.rssi);
      text(net.ssid || "(hidden)", 24, y, F, net.open ? lcd.ORANGE : lcd.WHITE);
      text(`${net.rssi}`, W - 4, y, F, lcd.DARKGREY, lcd.TR);
    });
    text(`${this.nets.length} nets  -  ${board.hasTouch ? "tap" : "Enter"}: rescan`, 6, H - 10, 1, lcd.DARKGREY);
  },
  key(k) { if (k === "enter") this.scan(); },
  tick() { this.scan(); },
  tap(x, y) { if (y > TOP) this.scan(); },
});

// ---- Tool: system monitor
tools.push({
  icon: "i", name: "System",
  title: "System", every: 1000,
  draw() {
    const h = sys.heap(), kb = (n) => (n == null ? "-" : `${(n / 1024).toFixed(1)} KB`);
    lcd.fillRect(0, TOP, W, H - TOP, BG);
    const rows = [
      ["board", board.name],
      ["cpu", `${sys.cpuMhz()} MHz`],
      ["uptime", `${(millis() / 1000).toFixed(0)} s`],
      ["free heap", kb(h.free)],
      ["min free", kb(h.minFree)],
      ["largest blk", kb(h.largest)],
      ["js heap", kb(h.js)],
      ["wifi", board.hasWifi !== false ? wifi.status() : "none"],
    ];
    rows.forEach(([k, v], i) => {
      const y = TOP + 4 + i * (SMALL ? 14 : 18);
      text(k, 6, y, F, lcd.DARKGREY);
      text(v, W - 6, y, F, lcd.WHITE, lcd.TR);
    });
  },
  tick() { this.draw(); },
});

// ---- Tool: LED colour mixer (data/lib/mixer.js)
let mixer = null;
tools.push({
  icon: "*", name: "RGB LED",
  title: "RGB LED", every: 40,
  ownsArrows: true,  // left / right change sliders here; Backspace goes back
  enter() { if (!mixer) mixer = makeMixer({ x: 4, y: TOP + 3, w: W - 8, h: H - TOP - 5 }); },
  draw() { mixer.draw(); },
  tick() { mixer.tick(); },
  key(k) { mixer.key(k); },
  tap(x, y) { mixer.down(x, y); },
  drag(x, y) { mixer.move(x, y); },
  release() { mixer.up(); },
});

// ---- Tool: sensors
tools.push({
  icon: "~", name: "Sensors",
  title: "Sensors", every: 300,
  draw() {
    lcd.fillRect(0, TOP, W, H - TOP, BG);
    let y = TOP + 6;
    if (board.hasLight) { text(`light sensor: ${light()}`, 6, y, F, lcd.WHITE); y += SMALL ? 16 : 22; }
    text(`touch: ${board.hasTouch ? "yes" : "no"}   keyboard: ${board.hasKeyboard ? "yes" : "no"}`, 6, y, F, lcd.LIGHTGREY);
    y += SMALL ? 16 : 22;
    const p = touch.read && board.hasTouch ? touch.read() : null;
    text(p ? `touch at ${p.x},${p.y}` : "", 6, y, F, lcd.SKYBLUE);
    if (board.hasKeyboard) {
      const held = keyboard.held();
      text(held.length ? `held: ${held.join(" ")}` : "press any key...", 6, H - (SMALL ? 14 : 22), F, lcd.DARKGREY);
    }
  },
  tick() { this.draw(); },
});

// ---- dispatch
const ARROW = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
keyboard.on("key", ({ key }) => {
  const k = ARROW[key] ?? (key === "Enter" ? "enter" : key === " " ? "space" : key);
  if (screen !== menu && (key === "Backspace" || (k === "left" && !screen.ownsArrows))) return go(menu);  // back
  if (screen.key) screen.key(k);
});
touch.on("down", ({ x, y }) => {
  if (screen !== menu && y < TOP) return go(menu);  // tap the bar = back
  if (screen.tap) screen.tap(x, y);
});
touch.on("move", ({ x, y }) => (screen.drag ? screen.drag(x, y) : null));
touch.on("up", () => (screen.release ? screen.release() : null));

go(menu);
