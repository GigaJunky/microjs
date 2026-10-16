// Joystick tester: live view of up to two USB gamepads - both sticks, the D-pad (hat), 16
// buttons and the raw report bytes. F12 or Escape goes back to the menu.
//   joystick.read(n)  -> { connected, x, y, x2, y2 (-1..1), hat (-1 or 0..7), buttons (bit mask), raw }
//   joystick.on("press" | "release", ({ joystick, button }) => ...)
//   joystick.keys(false) stops the D-pad / buttons also typing arrow keys, Space and Enter
const W = lcd.width(), H = lcd.height(), PW = W / 2, SMALL = H < 200;
const BOX = Math.min(56, Math.floor((PW - 40) / 2) - 4), BTN = SMALL ? 5 : 7;
const RAW_PER_LINE = Math.floor((Math.floor((PW - 4) / 6) - 4) / 3);
const BG = lcd.BLACK, DIM = lcd.DARKGREY;
joystick.keys(false);  // we want the raw state here, not key presses

lcd.fillScreen(BG);
lcd.setTextFont(1);
lcd.setTextDatum(lcd.TL);
let last = ["", ""];

function panel(n) {
  const j = joystick.read(n), x0 = n * PW, y0 = 2;
  const key = JSON.stringify(j);
  if (key === last[n]) return;  // redraw only what changed
  last[n] = key;
  lcd.fillRect(x0, 0, PW - 1, H, BG);  // keep the divider
  lcd.setTextColor(j.connected ? lcd.GREEN : DIM);
  const state = j.connected ? (SMALL ? "on" : "connected") : (SMALL ? "--" : "not connected");
  lcd.drawString(`${SMALL ? "Joy" : "Joystick"} ${n + 1}: ${state}`, x0 + 4, y0);
  if (!j.connected) return;
  // the two sticks
  [[j.x, j.y, SMALL ? "1" : "stick 1"], [j.x2, j.y2, SMALL ? "2" : "stick 2"]].forEach(([x, y, label], i) => {
    const bx = x0 + 4 + i * (BOX + 8), by = y0 + 14;
    lcd.drawRect(bx, by, BOX, BOX, DIM);
    lcd.drawFastHLine(bx, by + BOX / 2, BOX, DIM);
    lcd.drawFastVLine(bx + BOX / 2, by, BOX, DIM);
    lcd.fillCircle(bx + BOX / 2 + x * (BOX / 2 - 3), by + BOX / 2 + y * (BOX / 2 - 3), 3, lcd.YELLOW);
    lcd.setTextColor(lcd.LIGHTGREY);
    lcd.drawString(label, bx, by + BOX + 2);
  });
  // D-pad / hat: 8 directions around a centre
  const hx = x0 + PW - 22, hy = y0 + 14 + BOX / 2;
  for (let d = 0; d < 8; d++) {
    const a = (d * Math.PI) / 4;
    lcd.fillCircle(hx + Math.sin(a) * 12, hy - Math.cos(a) * 12, 3, d === j.hat ? lcd.CYAN : DIM);
  }
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString("hat", hx - 8, hy + 18);
  // buttons 1-16
  const gy = y0 + BOX + 30;
  lcd.drawString("buttons", x0 + 4, gy);
  for (let b = 0; b < 16; b++) {
    const cx = x0 + 8 + (b % 8) * (BTN * 2 + 3), cy = gy + 16 + Math.floor(b / 8) * (BTN * 2 + 4);
    const on = (j.buttons >>> b) & 1;
    lcd.fillCircle(cx, cy, BTN, on ? lcd.RED : DIM);
    if (!SMALL) {  // numbers fit in the bigger circles
      lcd.setTextColor(on ? lcd.WHITE : lcd.BLACK);
      lcd.setTextDatum(lcd.MC);
      lcd.drawString(String(b + 1), cx, cy + 1);
      lcd.setTextDatum(lcd.TL);
    }
  }
  // raw report bytes, two lines of hex
  const hex = j.raw.map((v) => v.toString(16).padStart(2, "0"));
  lcd.setTextColor(DIM);
  lcd.drawString("raw " + hex.slice(0, RAW_PER_LINE).join(" "), x0 + 4, H - 20);
  if (hex.length > RAW_PER_LINE) lcd.drawString("    " + hex.slice(RAW_PER_LINE, 2 * RAW_PER_LINE).join(" "), x0 + 4, H - 10);
}

joystick.on("press", ({ joystick: n, button }) => tone(600 + 60 * button + 300 * n, 20));
lcd.drawFastVLine(PW - 1, 0, H, DIM);
setInterval(() => { panel(0); panel(1); }, 50);
