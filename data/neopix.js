// NeoPixel: drives a WS2812 / NeoPixel strip or matrix from a GPIO (on the CYD: the CN1 connector,
// GPIO 27 or 22). Setup picks the pin, the number of LEDs, the grid (columns; 1 = a plain strip),
// the wiring (zigzag or all rows the same way), the pattern, colour, speed and brightness; the
// screen shows a live preview of what the LEDs show. Settings are kept in /neopix.json.
// Touch: tap the bottom buttons; in Setup hold - / + to repeat. Keys: left / right change the
// pattern (Setup: the value), up / down brightness (Setup: pick a row), Enter = Setup / Run.
// Wire the strip's data line to the pin through ~330 ohm, 5 V power and ground from the board's
// 5 V / GND pins (CN1 only has 3.3 V: power a long strip from its own supply, common ground).
// @needs-esp32 (the NeoPixel output uses the ESP32's RMT peripheral)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2;
const TOP = SMALL ? 14 : 24, BOT = SMALL ? 22 : 40, CFG = "/neopix.json";
const KEYS = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right", Enter: "enter" };

// ---- settings
const CYD = board.name.startsWith("CYD");
const S3CYD = board.name.startsWith("ESP32-S3 CYD");
// the S3 CYD: GPIO 2 is free on its header (tested). Left out: 1 4-8 (audio codec), 10-13 45 46 (display),
// 15 16 18 (touch), 26-37 (flash and PSRAM); the SD card's pins aren't known here, so check the board's pin table
const S3_USED = [1, 4, 5, 6, 7, 8, 10, 11, 12, 13, 15, 16, 18, 45, 46];
const PINS = CYD ? [27, 22] : board.hasKeyboard ? [1, 2, 3, 4, 5, 6, 7, 8]
  : Array.from({ length: 48 }, (_, i) => i + 1).filter((p) => !S3CYD || (!S3_USED.includes(p) && (p < 26 || p > 37)));
const DEFAULT_PIN = S3CYD ? 2 : PINS[0];
const cfg = Object.assign({ pin: DEFAULT_PIN, leds: 64, cols: 8, zigzag: true, pattern: 1, color: 0, speed: 50, bright: 20 }, JSON.parse(fs.read(CFG) || "{}"));
const save = () => fs.write(CFG, JSON.stringify(cfg));

const COLORS = [
  ["Red", 0], ["Orange", 25], ["Yellow", 55], ["Green", 120], ["Cyan", 180], ["Blue", 240],
  ["Purple", 275], ["Pink", 320], ["White", -1], ["Cycle", -2],
];
function hsv(h, s, v) {  // hue 0-360, s and v 0-1 -> 0xRRGGBB
  const f = (n) => { const k = (n + h / 60) % 6; return v * (1 - s * Math.max(0, Math.min(k, 4 - k, 1))); };
  return (Math.round(f(5) * 255) << 16) | (Math.round(f(3) * 255) << 8) | Math.round(f(1) * 255);
}
const scale = (c, k) => (Math.round(((c >> 16) & 255) * k) << 16) | (Math.round(((c >> 8) & 255) * k) << 8) | Math.round((c & 255) * k);
function base(t) {  // the chosen colour (the hue drifts for Cycle)
  const h = COLORS[cfg.color][1];
  return h === -1 ? 0xffffff : hsv(h === -2 ? (t * 40) % 360 : h, 1, 1);
}

// ---- the LED layout: a strip is N x 1, a grid is cols x rows (the last row may be partial)
let N, GW, GH, cur = [], raw = [], shown = [], heat = [];
function layout() {
  N = cfg.leds;
  cfg.cols = Math.max(1, Math.min(cfg.cols, N));
  GW = cfg.cols === 1 ? N : cfg.cols;
  GH = Math.ceil(N / GW);
  cur = new Array(N).fill(0);
  raw = new Array(N).fill(0);
  shown = new Array(N).fill(-1);
  heat = new Array(N).fill(0);
}
const phys = (x, y) => (cfg.zigzag && y % 2 === 1 ? y * GW + GW - 1 - x : y * GW + x);

// ---- patterns: (x, y, t) -> colour; t is time in seconds scaled by the speed. step(t, dt) runs
// once a frame first (for patterns that keep a state).
const rnd = Math.random;
const PATTERNS = [
  { name: "Solid", at: (x, y, t) => base(t) },
  { name: "Rainbow", at: (x, y, t) => hsv((((x + y) / (GW + GH - 1)) * 360 + t * 90) % 360, 1, 1) },
  {
    name: "Chase",  // a dot with a fading tail runs along the LEDs in wiring order
    at(x, y, t) {
      const L = y * GW + x, head = (t * 18) % N, d = (head - L + N) % N;
      return d < 8 ? scale(base(t), 1 - d / 8) : 0;
    },
  },
  {
    name: "Scanner",  // a bar sweeps left and right, like KITT
    at(x, y, t) {
      const span = GW - 1, p = (t * 6) % (2 * span + 2), pos = p <= span ? p : 2 * span + 1 - p;
      return scale(base(t), Math.max(0, 1 - Math.abs(x - pos) / 2.5));
    },
  },
  { name: "Breathe", at: (x, y, t) => scale(base(t), 0.5 - 0.5 * Math.cos(t * 3)) },
  {
    name: "Twinkle",
    step(t, dt) {
      for (let i = 0; i < N; i++) heat[i] = Math.max(0, heat[i] - dt * 1.2);
      for (let k = 0; k < Math.max(1, N / 40); k++) if (rnd() < dt * 12) heat[Math.floor(rnd() * N)] = 1;
    },
    at: (x, y, t) => scale(base(t), heat[y * GW + x]),
  },
  {
    name: "Fire",  // heat rises from the bottom row (the last row is the base on a grid)
    step(t, dt) {
      for (let x = 0; x < GW; x++) {
        for (let y = 0; y < GH; y++) {
          const i = y * GW + x;
          heat[i] = Math.max(0, heat[i] - rnd() * dt * 2.2 * (4 / GH + 0.4));
          if (y < GH - 1) heat[i] = Math.max(heat[i] * 0.4, (heat[i] + heat[i + GW] * 1.6) / 2.6);
        }
        if (rnd() < dt * 20) heat[Math.min(N - 1, (GH - 1) * GW + x)] = 0.7 + rnd() * 0.3;
      }
    },
    at(x, y) {
      const v = heat[y * GW + x];
      return hsv(v * 55, 1, Math.min(1, v * 1.4));
    },
  },
  { name: "Plasma", at: (x, y, t) => hsv((180 + 90 * (Math.sin(x * 0.7 + t * 2) + Math.sin(y * 0.9 - t * 1.6) + Math.sin((x + y) * 0.5 + t))) % 360, 1, 1) },
  {
    name: "Ripple",  // rings spreading from the middle
    at(x, y, t) {
      const d = Math.hypot(x - (GW - 1) / 2, (y - (GH - 1) / 2) * (GH > 1 ? 1 : 0));
      return scale(base(t), 0.5 + 0.5 * Math.sin(d * 1.3 - t * 6));
    },
  },
  {
    name: "Rain",  // drops fall down the columns (down the strip when it is one)
    step(t, dt) {
      for (let i = 0; i < N; i++) heat[i] = Math.max(0, heat[i] - dt * 3);
      if (GH === 1) { if (rnd() < dt * 10) heat[Math.floor(rnd() * N)] = 1; return; }
      if (rnd() < dt * GW * 1.5) heat[Math.floor(rnd() * GW)] = 1;
      if (Math.floor((t - dt) * 8) !== Math.floor(t * 8)) {
        for (let y = GH - 1; y > 0; y--) for (let x = 0; x < GW; x++) if (y * GW + x < N) heat[y * GW + x] = Math.max(heat[y * GW + x], heat[(y - 1) * GW + x] - 0.1);
      }
    },
    at: (x, y, t) => scale(base(t), heat[y * GW + x]),
  },
  {
    name: "Wipe",  // fills with the colour, then clears
    at(x, y, t) {
      const L = y * GW + x, p = (t * 12) % (2 * N);
      return (p < N ? L < p : L >= p - N) ? base(t) : 0;
    },
  },
  { name: "Strobe", at: (x, y, t) => (Math.floor(t * 8) % 2 ? 0 : base(t)) },
];

// ---- frame
let t = 0, last = 0, paused = false, ok = true, wasOk = true;
function frame() {
  const now = millis(), dt = Math.min(0.1, (now - last) / 1000);
  last = now;
  const p = PATTERNS[cfg.pattern];
  if (!paused) t += dt * (0.1 + cfg.speed / 25);
  if (p.step && !paused) p.step(t, dt * (0.1 + cfg.speed / 25));
  const k = (cfg.bright / 100) ** 2;  // brightness feels more even with a curve
  for (let y = 0; y < GH; y++) {
    for (let x = 0; x < GW; x++) {
      if (y * GW + x >= N) break;
      const c = p.at(x, y, t);
      raw[y * GW + x] = c;
      cur[phys(x, y)] = scale(c, k);
    }
  }
  ok = neopixel(cfg.pin, cur);
}

// ---- screens
let screen = "run", sel = 0;
const LAYOUT = () => {  // the preview: a cell size and how many fit across
  const aw = W - 8, ah = H - TOP - BOT - 6;
  const per = cfg.cols === 1 ? Math.max(1, Math.floor(aw / Math.max(5, Math.min(24, Math.floor(Math.sqrt((aw * ah) / N)))))) : GW;
  const rows = Math.ceil(N / per), d = Math.max(2, Math.min(Math.floor(aw / per), Math.floor(ah / rows), 40));
  return { per, d, x0: Math.floor((W - per * d) / 2), y0: TOP + 3 + Math.floor((ah - rows * d) / 2) };
};
let L = null;
function button(x, y, w, h, label, color) {
  lcd.fillRoundRect(x, y, w, h, 6, color);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(label, x + w / 2, y + h / 2 + 1, F);
}
function title(text, right) {
  lcd.fillRect(0, 0, W, TOP, lcd.NAVY);
  lcd.setTextColor(lcd.WHITE, lcd.NAVY);
  lcd.setTextDatum(lcd.ML);
  lcd.drawString(text, 4, TOP / 2, F);
  lcd.setTextDatum(lcd.MR);
  lcd.drawString(right, W - 4, TOP / 2, SMALL ? 1 : 2);
}
const BTN = () => {  // run screen: [<] [pattern] [>] [Setup]
  const y = H - BOT + 3, h = BOT - 6, s = SMALL ? 24 : 44, setup = SMALL ? 50 : 78;
  return { prev: [4, y, s, h], next: [4 + s + (W - 16 - 2 * s - setup), y, s, h], setup: [W - 4 - setup, y, setup, h], name: [4 + s, y, W - 16 - 2 * s - setup, h] };
};
function drawRun() {
  lcd.fillScreen(lcd.BLACK);
  title("NeoPixel", `GPIO ${cfg.pin} - ${N} LEDs${cfg.cols > 1 ? " - " + GW + "x" + GH : ""}`);
  L = LAYOUT();
  shown.fill(-1);
  const b = BTN(), c = lcd.color(45, 45, 60);
  button(...b.prev, "<", c);
  button(...b.next, ">", c);
  button(...b.setup, "Setup", lcd.color(30, 100, 160));
  drawName();
}
function drawName() {
  const b = BTN().name;
  lcd.fillRect(b[0], b[1], b[2], b[3], lcd.BLACK);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(PATTERNS[cfg.pattern].name + (paused ? " (paused)" : ""), b[0] + b[2] / 2, b[1] + b[3] / 2 + 1, F);
}
function drawPreview() {
  if (!L) return;
  for (let y = 0; y < GH; y++) {
    for (let x = 0; x < GW; x++) {
      const i = y * GW + x;
      if (i >= N) break;
      const c = raw[i];
      if (c === shown[i]) continue;
      shown[i] = c;
      const col = lcd.color(...[16, 8, 0].map((sh) => Math.round((((c >> sh) & 255) * (30 + cfg.bright * 0.7)) / 100)));  // dim LEDs stay visible
      const px = cfg.cols === 1 ? i % L.per : x, py = cfg.cols === 1 ? Math.floor(i / L.per) : y;
      const cx = L.x0 + px * L.d, cy = L.y0 + py * L.d;
      if (L.d >= 6) { lcd.fillRect(cx, cy, L.d, L.d, lcd.BLACK); lcd.fillCircle(cx + L.d / 2, cy + L.d / 2, L.d / 2 - 1, col); }
      else lcd.fillRect(cx, cy, L.d - 1, L.d - 1, col);
    }
  }
}

// ---- setup screen: a row per setting
const ROWS = [
  { label: "Pin", get: () => "GPIO " + cfg.pin, adj(d) { cfg.pin = PINS[(PINS.indexOf(cfg.pin) + d + PINS.length) % PINS.length]; } },
  { label: "LEDs", get: () => cfg.leds, adj(d) { cfg.leds = Math.max(1, Math.min(512, cfg.leds + d)); cfg.cols = Math.min(cfg.cols, cfg.leds); } },
  { label: "Columns", get: () => (cfg.cols === 1 ? "1 (strip)" : cfg.cols + " x " + Math.ceil(cfg.leds / cfg.cols)), adj(d) { cfg.cols = Math.max(1, Math.min(cfg.leds, cfg.cols + d)); } },
  { label: "Wiring", get: () => (cfg.zigzag ? "zigzag" : "same way"), adj() { cfg.zigzag = !cfg.zigzag; } },
  { label: "Pattern", get: () => PATTERNS[cfg.pattern].name, adj(d) { cfg.pattern = (cfg.pattern + d + PATTERNS.length) % PATTERNS.length; heat.fill(0); } },
  { label: "Colour", get: () => COLORS[cfg.color][0], adj(d) { cfg.color = (cfg.color + d + COLORS.length) % COLORS.length; } },
  { label: "Speed", get: () => cfg.speed + "%", adj(d) { cfg.speed = Math.max(0, Math.min(100, cfg.speed + d)); } },
  { label: "Bright", get: () => cfg.bright + "%", adj(d) { cfg.bright = Math.max(0, Math.min(100, cfg.bright + d)); } },
];
const RH = SMALL ? 17 : 22;
const VIS = () => Math.max(1, Math.floor((H - TOP - BOT) / RH));
const first = () => Math.max(0, Math.min(ROWS.length - VIS(), sel - Math.floor(VIS() / 2)));
const PW = SMALL ? 16 : 36;  // - and + button width
function drawSetup() {
  title("Setup", "BOOT = menu");
  lcd.fillRect(0, TOP, W, H - TOP, lcd.BLACK);
  const f0 = first();
  for (let r = 0; r < VIS() && f0 + r < ROWS.length; r++) {
    const i = f0 + r, y = TOP + r * RH, on = i === sel;
    lcd.fillRoundRect(2, y + 1, W - 4, RH - 2, 4, on ? lcd.color(55, 55, 85) : lcd.color(28, 28, 36));
    lcd.setTextDatum(lcd.ML);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString(ROWS[i].label, 8, y + RH / 2 + 1, F);
    const mx = W - 4 - PW * 2 - (SMALL ? 70 : 128);
    lcd.setTextDatum(lcd.MC);
    lcd.setTextColor(lcd.YELLOW);
    lcd.drawString(String(ROWS[i].get()), mx + PW + (SMALL ? 35 : 64), y + RH / 2 + 1, F);
    button(mx, y + 2, PW, RH - 4, "-", lcd.color(70, 50, 50));
    button(W - 4 - PW, y + 2, PW, RH - 4, "+", lcd.color(40, 90, 50));
  }
  const y = H - BOT + 3;
  button(4, y, W - 8, BOT - 6, "Run", lcd.color(30, 100, 160));
}
const openRun = () => { save(); layout(); screen = "run"; t = 0; drawRun(); };
const openSetup = () => { screen = "setup"; neopixel(cfg.pin, new Array(N).fill(0)); drawSetup(); };

// ---- input: touch (with auto-repeat on - and +) and keys
let held = null, heldAt = 0, repeatAt = 0;
function change(i, d) { ROWS[i].adj(d); drawSetup(); }
touch.on("down", ({ x, y }) => {
  if (screen === "run") {
    const b = BTN(), hit = (r) => x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
    if (hit(b.setup)) openSetup();
    else if (hit(b.prev) || hit(b.next) || hit(b.name)) {
      cfg.pattern = (cfg.pattern + (hit(b.prev) ? -1 : 1) + PATTERNS.length) % PATTERNS.length;
      heat.fill(0);
      t = 0;
      drawName();
    } else paused = !paused, drawName();
    return;
  }
  if (y >= H - BOT) return openRun();
  const r = Math.floor((y - TOP) / RH) + first();
  if (y < TOP || r < 0 || r >= ROWS.length) return;
  sel = r;
  const mx = W - 4 - PW * 2 - (SMALL ? 70 : 128);
  const d = x >= mx && x < mx + PW ? -1 : x >= W - 4 - PW ? 1 : 0;
  if (d) { held = { i: r, d }; heldAt = repeatAt = millis(); change(r, d); } else drawSetup();
});
touch.on("up", () => { held = null; });
keyboard.on("key", ({ key }) => {
  const k = KEYS[key] || key;
  if (screen === "run") {
    if (k === "enter" || k === "s") openSetup();
    else if (k === "left" || k === "right") { cfg.pattern = (cfg.pattern + (k === "left" ? -1 : 1) + PATTERNS.length) % PATTERNS.length; heat.fill(0); t = 0; drawName(); }
    else if (k === "up" || k === "down") cfg.bright = Math.max(0, Math.min(100, cfg.bright + (k === "up" ? 5 : -5)));
    else if (k === " " || k === "p") { paused = !paused; drawName(); }
    return;
  }
  if (k === "enter") openRun();
  else if (k === "up") { sel = (sel + ROWS.length - 1) % ROWS.length; drawSetup(); }
  else if (k === "down") { sel = (sel + 1) % ROWS.length; drawSetup(); }
  else if (k === "left" || k === "right") change(sel, k === "left" ? -1 : 1);
  else if (k === "[" || k === "]") change(sel, k === "[" ? -10 : 10);
});

layout();
drawRun();
last = millis();
setInterval(() => {
  const now = millis();
  if (held && now - heldAt > 400 && now - repeatAt > 70) {  // hold - or +: repeat, faster after a while
    repeatAt = now;
    change(held.i, held.d * (now - heldAt > 1800 ? 10 : 1));
  }
  if (screen !== "run") return;
  frame();
  drawPreview();
  if (ok !== wasOk) {  // flag a pin the RMT output couldn't take
    wasOk = ok;
    lcd.setTextColor(ok ? lcd.NAVY : lcd.RED, lcd.NAVY);
    lcd.setTextDatum(lcd.MC);
    lcd.drawString("no output", W / 2, TOP / 2, 1);
  }
}, 30);
