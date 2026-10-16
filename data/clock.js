// Analog + digital clock. With WiFi set in micro-js.json it gets the real time over NTP, shifted
// by utcOffsetHours (e.g. -5); without WiFi it starts at 12:00:00 and counts from there.
const cfg = readConfig();
const WIFI = cfg.wifi ?? {};
const OFFSET_S = (cfg.utcOffsetHours ?? 0) * 3600;

const W = lcd.width(), H = lcd.height();
const R = Math.min(95, Math.floor(H / 2) - 6), CX = H < 200 ? R + 8 : 110, CY = H / 2;
const TX = Math.round((CX + R + 4 + W) / 2);  // digital time, centred in the space on the right
const BG = lcd.BLACK;

let offset = 12 * 3600 * 1000;  // fallback time base
lcd.fillScreen(BG);
lcd.setTextDatum(lcd.MC);
lcd.setTextColor(lcd.WHITE, BG);
if (WIFI.ssid) {
  lcd.drawString("connecting...", W / 2, H / 2, 2);
  if (wifi.connect(WIFI.ssid, WIFI.pass) && wifi.ntp("UTC0")) console.log("time synced");
  else console.log("no network time, using fallback");
  lcd.fillScreen(BG);
}

function now() {
  const ms = Date.now();  // epoch milliseconds once NTP has synced, else time since boot
  const s = ms > 1.6e12 ? Math.floor(ms / 1000) + OFFSET_S : Math.floor((millis() + offset) / 1000);
  return { hour: Math.floor(s / 3600) % 24, minute: Math.floor(s / 60) % 60, second: s % 60 };
}

// face
lcd.fillSmoothCircle(CX, CY, R + 4, lcd.DARKGREY, BG);
lcd.fillSmoothCircle(CX, CY, R, lcd.NAVY, lcd.DARKGREY);
for (let i = 0; i < 60; i++) {
  const a = (i * 6 * Math.PI) / 180;
  const inner = i % 5 === 0 ? R - 12 : R - 5;
  lcd.drawLine(CX + Math.sin(a) * inner, CY - Math.cos(a) * inner, CX + Math.sin(a) * (R - 2), CY - Math.cos(a) * (R - 2), lcd.WHITE);
}

const hands = { h: null, m: null, s: null };
function hand(key, angleDeg, len, width, color) {
  const old = hands[key];
  if (old) lcd.drawWideLine(CX, CY, old.x, old.y, old.w, lcd.NAVY, lcd.NAVY);  // erase
  const a = (angleDeg * Math.PI) / 180;
  const h = { x: CX + Math.sin(a) * len, y: CY - Math.cos(a) * len, w: width };
  hands[key] = h;
  return () => lcd.drawWideLine(CX, CY, h.x, h.y, width, color, lcd.NAVY);
}

const pad = (n) => String(n).padStart(2, "0");
lcd.setTextPadding(90);
let lastSecond = -1;
setInterval(() => {
  const t = now();
  if (t.second === lastSecond) return;
  lastSecond = t.second;
  const draws = [
    hand("h", (t.hour % 12) * 30 + t.minute / 2, R * 0.5, 7, lcd.WHITE),
    hand("m", t.minute * 6 + t.second / 10, R * 0.75, 5, lcd.WHITE),
    hand("s", t.second * 6, R * 0.85, 2, lcd.RED),
  ];
  draws.forEach((d) => d());
  lcd.fillSmoothCircle(CX, CY, 5, lcd.RED, lcd.NAVY);
  lcd.setTextColor(lcd.YELLOW, BG);
  lcd.drawString(`${pad(t.hour)}:${pad(t.minute)}`, TX, CY - 16, 4);
  lcd.setTextColor(lcd.LIGHTGREY, BG);
  lcd.drawString(pad(t.second), TX, CY + 16, 4);
}, 100);
