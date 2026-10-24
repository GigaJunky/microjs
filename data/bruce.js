// Bruce-style device multi-tool: a categorized menu of on-device diagnostics, in the spirit of
// the Bruce ESP32 firmware. Tools are benign: a WiFi analyzer, a system monitor, LED control and
// a sensor readout. Works on touch (CYD) and keyboard (Cardputer). BOOT returns to the launcher.
//   Keys: ; . move, Enter open/act, , back. Touch: tap an item; tap the top bar to go back.
//   RGB LED tool: sliders for red, green, blue, brightness and cycle speed, Auto colour cycling,
//   LED on/off (shared with controls.js); there , / change a slider and Backspace goes back.
load("/lib/mixer.js");
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, TOUCH = board.hasTouch;
// touch screens get big targets for a finger: a 34 px title bar (Back and the page arrows live there) and 40 px rows
const TOP = TOUCH ? 34 : SMALL ? 16 : 26, ROW = TOUCH ? 40 : SMALL ? 16 : 30, F = SMALL ? 1 : 2;
const ACCENT = lcd.color(130, 90, 230), BG = lcd.BLACK, BAR = lcd.color(30, 20, 60);

function text(str, x, y, font, color, datum) {
  lcd.setTextDatum(datum ?? lcd.TL);
  lcd.setTextColor(color ?? lcd.WHITE, BG);
  lcd.drawString(String(str), x, y, font);
}

// a rounded button; label centred (transparent text, so it sits on any colour)
function button(x, y, w, h, label, on) {
  lcd.fillRoundRect(x, y, w, h, 8, on ? ACCENT : lcd.color(50, 50, 80));
  lcd.setTextDatum(lcd.MC); lcd.setTextColor(lcd.WHITE); lcd.drawString(label, x + w / 2, y + h / 2, F);
}

// ---- the title bar. Touch: "< Back" on the right (a screen with a parent), and next to it ▲ ▼ when the screen has
// more than a page (screen.paging() true; screen.page(-1 | 1) turns the page) - they sit up here to leave the screen free.
const BACKW = 80, ARW = 40;
function zones(s) { const back = s.parent ? W - BACKW : W, down = back - ARW; return { back, down, up: down - ARW }; }
function header(title) {
  lcd.fillRect(0, 0, W, TOP, BAR);
  text(title, 4, (TOP - 8 * F) / 2, F, ACCENT);
  if (!TOUCH) { if (screen.parent) text(screen.ownsArrows ? "< Bksp" : "< ,", W - 4, (TOP - 8 * F) / 2, F, lcd.DARKGREY, lcd.TR); return; }
  const z = zones(screen);
  if (screen.parent) { lcd.fillRoundRect(z.back + 2, 3, BACKW - 6, TOP - 6, 8, lcd.color(80, 50, 140)); lcd.setTextDatum(lcd.MC); lcd.setTextColor(lcd.WHITE); lcd.drawString("< Back", z.back + BACKW / 2 - 1, TOP / 2, F); }
  if (screen.paging && screen.paging()) [[z.up, -1], [z.down, 1]].forEach(([x, d]) => {
    lcd.fillRoundRect(x + 2, 3, ARW - 4, TOP - 6, 8, lcd.color(50, 50, 80));
    const cx = x + ARW / 2, cy = TOP / 2, v = d < 0 ? 6 : -6;
    lcd.fillTriangle(cx - 8, cy + v, cx + 8, cy + v, cx, cy - v, lcd.WHITE);
  });
}

// ---- a screen is { title, parent, enter(), draw(), key(name), tap(x,y), tick(), paging(), page(d) }
let screen = null, timer = 0;

function go(s) {
  if (screen && screen.leave) screen.leave();
  clearInterval(timer);
  timer = 0;
  screen = s;
  lcd.fillScreen(BG);
  if (s.enter) s.enter();
  header(s.title);
  s.draw();
  if (s.tick) timer = setInterval(() => (screen === s ? s.tick() : null), s.every ?? 1000);
}

// ---- a list of tools (the main menu and its sub menus: an item with .sub opens another list)
// Touch: swipe the list or use the ▲ ▼ in the title bar; a tap opens the item when the finger lifts.
// Keys: ; . move, Enter opens. The bar on the right edge shows where you are.
function makeList(title, items, parent) {
  const m = {
    title, parent, items, first: 0, sel: 0,
    per() { return Math.floor((H - TOP) / ROW); },
    paging() { return this.items.length > this.per(); },
    page(d) {
      const max = Math.max(0, this.items.length - this.per());
      this.first = d > 0 ? (this.first >= max ? 0 : Math.min(max, this.first + this.per())) : this.first <= 0 ? max : Math.max(0, this.first - this.per());
      this.draw();
    },
    open(i) { const it = this.items[i]; if (it.sub) it.sub.parent = this; else it.parent = this; go(it.sub || it); },
    draw() {
      const per = this.per(), n = this.items.length;
      this.first = Math.max(0, Math.min(this.first, Math.max(0, n - per)));
      lcd.fillRect(0, TOP, W, H - TOP, BG);
      this.items.slice(this.first, this.first + per).forEach((t, i) => {
        const y = TOP + i * ROW, on = this.first + i === this.sel || this.first + i === this.row;
        if (on) lcd.fillRect(0, y, W, ROW, ACCENT);
        text(`${t.icon}  ${t.name}${t.sub ? "  >" : ""}`, 6, y + (ROW - 8 * F) / 2, F, on ? lcd.WHITE : lcd.LIGHTGREY);
      });
      if (n > per) {  // scroll bar
        const h = per * ROW, th = Math.max(16, Math.round(h * per / n)), ty = TOP + Math.round((h - th) * this.first / (n - per));
        lcd.fillRect(W - 6, TOP, 6, h, lcd.color(30, 30, 40));
        lcd.fillRoundRect(W - 6, ty, 6, th, 3, lcd.color(150, 120, 230));
      }
      header(this.title);
    },
    key(k) {
      const n = this.items.length;
      if (k === "up") this.sel = (this.sel + n - 1) % n;
      else if (k === "down") this.sel = (this.sel + 1) % n;
      else if (k === "enter" || k === "right") return this.open(this.sel);
      else if (k >= "1" && k <= "9" && +k <= n) return this.open(+k - 1);
      else return;
      const per = this.per();
      if (this.sel < this.first) this.first = this.sel; else if (this.sel >= this.first + per) this.first = this.sel - per + 1;
      this.draw();
    },
    tap(x, y) {
      this.y0 = y; this.first0 = this.first; this.moved = false;
      this.row = this.first + Math.floor((y - TOP) / ROW);
      if (this.row >= this.items.length) this.row = -1; else this.draw();
    },
    drag(x, y) {
      if (this.y0 === undefined || this.y0 < 0) return;
      if (Math.abs(this.y0 - y) > 12) { this.moved = true; this.row = -1; }
      const f = this.first0 + Math.round((this.y0 - y) / ROW);
      if (this.moved && f !== this.first) { this.first = f; this.draw(); }
    },
    release() {
      const r = this.row;
      this.y0 = -1; this.row = -1;
      if (r >= 0 && !this.moved) { this.sel = r; this.open(r); }
    },
  };
  return m;
}
const tools = [];  // filled in below
const menu = makeList("Bruce  -  tools", tools, null);

// ---- a list of results (WiFi / Bluetooth scans): a count, then pages of rows; s.p is the page.
// row(item, y) draws one; touch gets a "Scan again" button along the bottom (returns its top y for hit tests).
function paged(s, items, row) {
  const rh = TOUCH ? 24 : SMALL ? 14 : 20, bot = TOUCH ? 52 : 14, per = Math.max(1, Math.floor((H - TOP - bot - 16) / rh)), pages = Math.ceil(items.length / per);
  s.p = Math.max(0, Math.min(s.p || 0, pages - 1));
  s.paging = () => items.length > per;
  s.page = (d) => { s.p = (s.p + d + pages) % pages; s.draw(); };
  lcd.fillRect(0, TOP, W, H - TOP, BG);
  if (!items.length) text("nothing found", 6, TOP + 8, F, lcd.ORANGE);
  items.slice(s.p * per, s.p * per + per).forEach((it, i) => row(it, TOP + 16 + i * rh));
  text(`${items.length} found${items.length > per ? `   showing ${s.p * per + 1}-${Math.min(items.length, s.p * per + per)}` : ""}${TOUCH ? "" : "   Enter: rescan"}`, 6, TOP + 4, 1, lcd.LIGHTGREY);
  if (TOUCH) button(6, H - 48, W - 12, 44, "Scan again");
  header(s.title);
}

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
  title: "WiFi scan",
  nets: [],
  enter() { this.p = 0; this.scan(); },
  scan() {
    text("scanning...", 6, TOP + 4, 1, lcd.YELLOW);
    this.nets = wifi.scan().sort((a, b) => b.rssi - a.rssi);
    this.draw();
  },
  draw() {
    paged(this, this.nets, (net, y) => {
      bars(4, y, net.rssi);
      text(net.ssid || "(hidden)", 24, y, F, net.open ? lcd.ORANGE : lcd.WHITE);
      text(`${net.rssi}`, W - 4, y, F, lcd.DARKGREY, lcd.TR);
    });
  },
  key(k) { if (k === "enter") this.scan(); else if (k === "up" || k === "down") this.page(k === "down" ? 1 : -1); },
  tap(x, y) { if (y >= H - 52) this.scan(); },
});

// ---- Tools: Bluetooth scan, keyboard, mouse, gamepad (boards with ble; data/lib/bruce-ble.js)
if (board.hasBle) { load("/lib/bruce-ble.js"); tools.push({ icon: "B", name: "Bluetooth", sub: makeList("Bluetooth", bleTools({ text, TOP, F, W, H, BG, ACCENT, bars, SMALL, TOUCH, button, paged, header }), null) }); }

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
  if (screen.parent && (key === "Backspace" || (k === "left" && !screen.ownsArrows))) return go(screen.parent);  // back
  if (screen.key) screen.key(k);
});
touch.on("down", ({ x, y }) => {
  if (y < TOP) {  // the title bar: Back, or the page arrows
    const z = zones(screen);
    if (screen.parent && x >= z.back) return go(screen.parent);
    if (screen.paging && screen.paging() && x >= z.up && x < z.back) return screen.page(x >= z.down ? 1 : -1);
    return;
  }
  if (screen.tap) screen.tap(x, y);
});
touch.on("move", ({ x, y }) => (screen.drag ? screen.drag(x, y) : null));
touch.on("up", () => (screen.release ? screen.release() : null));

go(menu);
