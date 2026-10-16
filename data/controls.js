// Controls: the LED colour mixer (red / green / blue / brightness sliders, auto colour cycling at a
// speed you set, LED on/off) with a swatch showing the colour, plus the screen backlight and a
// beep. Touch: drag the sliders, tap the buttons. Keys: up / down pick a row, left / right change
// it, Enter presses a button, a toggles Auto.
load("/lib/mixer.js");
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2;
const TITLE = SMALL ? 14 : 30, BOTTOM = SMALL ? 18 : 40;

const mixer = makeMixer({ x: 6, y: TITLE + 2, w: W - 12, h: H - TITLE - BOTTOM - 6 });

// ---- backlight slider and beep button along the bottom
let backlightLevel = 100;
const BY = H - BOTTOM + 2, BH = BOTTOM - 6, BLW = Math.floor((W - 18) * 0.68);
function drawBottom() {
  lcd.fillRoundRect(6, BY, BLW, BH, 6, lcd.color(35, 35, 45));
  lcd.fillRoundRect(8, BY + 2, Math.max(6, Math.round(((BLW - 4) * backlightLevel) / 100)), BH - 4, 5, lcd.SKYBLUE);
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(lcd.BLACK);
  lcd.drawString(`Backlight ${backlightLevel}%`, 14, BY + BH / 2 + 1, F);
  lcd.fillRoundRect(12 + BLW, BY, W - 18 - BLW, BH, 6, lcd.color(160, 140, 20));
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.BLACK);
  lcd.drawString("Beep!", 12 + BLW + (W - 18 - BLW) / 2, BY + BH / 2 + 1, F);
}
function setBacklight(px) {
  backlightLevel = Math.max(5, Math.min(100, Math.round(((px - 8) / (BLW - 4)) * 100)));
  backlight(Math.round((backlightLevel * 255) / 100));
  drawBottom();
}

lcd.fillScreen(lcd.BLACK);
lcd.setTextDatum(lcd.TC);
lcd.setTextColor(lcd.WHITE);
lcd.drawString("Controls", W / 2, SMALL ? 2 : 4, SMALL ? 2 : 4);
mixer.draw();
drawBottom();

let draggingBacklight = false;
touch.on("down", ({ x, y }) => {
  if (mixer.down(x, y)) return;
  if (y >= BY && y < BY + BH) {
    if (x < 6 + BLW) { draggingBacklight = true; setBacklight(x); }
    else tone(880, 120);
  }
});
touch.on("move", ({ x, y }) => (draggingBacklight ? setBacklight(x) : mixer.move(x, y)));
touch.on("up", () => { draggingBacklight = false; mixer.up(); });
const KEYS = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right", Enter: "enter", " ": "space" };
keyboard.on("key", ({ key }) => mixer.key(KEYS[key] || key));
setInterval(() => mixer.tick(), 40);
