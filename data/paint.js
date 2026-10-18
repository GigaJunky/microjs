// Finger painting: pick a colour from the top bar, draw below it. CLR wipes the canvas.
// Keys: arrows (; . , /) move the cursor, hold Space to paint, 1-8 pick a colour, c clears.
// Gamepad: stick / D-pad moves (push the stick fully to go faster), hold button 1 to paint,
// button 2 picks the next colour, button 3 clears.
const W = lcd.width(), H = lcd.height();
const palette = [lcd.WHITE, lcd.RED, lcd.ORANGE, lcd.YELLOW, lcd.GREEN, lcd.CYAN, lcd.BLUE, lcd.MAGENTA];
const BAR = H < 200 ? 18 : 30;
// Gamepad (USB joystick 1): the D-pad as x/y, and a dead zone for the analog stick.
const HAT = [[0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1]];
const hatXY = (h) => (h >= 0 && h < 8 ? HAT[h] : [0, 0]);
const stick = (v) => (Math.abs(v) > 0.15 ? v : 0);
joystick.keys(false);
// Keyboard (Cardputer): ; . , / are the printed arrow keys (Fn + them sends Arrow* names).
const ARROW = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
const slot = W / (palette.length + 1);
let color = lcd.WHITE;
let last = null;
const brush = 3;

function drawBar() {
  palette.forEach((c, i) => {
    lcd.fillRect(i * slot, 0, slot, BAR, c);
    if (c === color) lcd.drawRect(i * slot + 3, 3, slot - 6, BAR - 6, lcd.BLACK);
  });
  lcd.fillRect(palette.length * slot, 0, slot, BAR, lcd.DARKGREY);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString("CLR", palette.length * slot + slot / 2, BAR / 2, BAR < 24 ? 1 : 2);
  if (board.hasKeyboard) {
    lcd.setTextColor(lcd.DARKGREY);
    palette.forEach((c, i) => lcd.drawString(String(i + 1), i * slot + slot / 2, BAR / 2, 1));
  }
}

// Keyboard painting works on a grid of G x G blocks, remembered in `blocks` so the cursor can
// be drawn over the picture and the block underneath repainted when it moves.
const G = 4, GC = Math.floor(W / G), GR = Math.floor((H - BAR) / G);
const blocks = new Array(board.hasKeyboard ? GC * GR : 0).fill(0);  // (~60 KB: not on a touch-only CYD)
let cx = Math.floor(GC / 2), cy = Math.floor(GR / 2);

function paintBlock(x, y, c) { lcd.fillRect(x * G, BAR + y * G, G, G, c); }
function drawCursor() { lcd.drawRect(cx * G - 1, BAR + cy * G - 1, G + 2, G + 2, lcd.DARKGREY); }
function hideCursor() {
  for (let y = cy - 1; y <= cy + 1; y++)
    for (let x = cx - 1; x <= cx + 1; x++)
      if (x >= 0 && y >= 0 && x < GC && y < GR) paintBlock(x, y, blocks[y * GC + x]);
  if (cy === 0) drawBar();
}

function clear() {
  lcd.fillScreen(lcd.BLACK);
  blocks.fill(0);
  drawBar();
  if (board.hasKeyboard) drawCursor();
}

function pick(i) {
  color = palette[i];
  drawBar();
  tone(1200, 20);
}

keyboard.on("key", ({ key }) => {
  if (key >= "1" && key <= String(palette.length)) pick(+key - 1);
  else if (key === "c" || key === "Backspace") { clear(); tone(500, 60); }
});
joystick.on("press", ({ button }) => {
  if (button === 1) pick((palette.indexOf(color) + 1) % palette.length);
  else if (button === 2) { clear(); tone(500, 60); }
});

setInterval(() => {  // held keys: move one block per tick, paint while Space is down
  if (!board.hasKeyboard) return;
  const keys = keyboard.held().map((k) => ARROW[k] ?? k);
  const pad = joystick.read(0), [hx, hy] = hatXY(pad.hat);
  const step = (v) => (Math.abs(v) > 0.85 ? 2 * Math.sign(v) : Math.abs(v) > 0.3 ? Math.sign(v) : 0);
  const dx = (keys.includes("right") ? 1 : 0) - (keys.includes("left") ? 1 : 0) + hx + step(pad.x);
  const dy = (keys.includes("down") ? 1 : 0) - (keys.includes("up") ? 1 : 0) + hy + step(pad.y);
  const pen = keys.includes(" ") || (pad.buttons & 1);
  if (!dx && !dy && !(pen && blocks[cy * GC + cx] !== color)) return;
  // move one block at a time, so painting at full speed still leaves a solid line
  const steps = Math.max(1, Math.abs(dx), Math.abs(dy));
  for (let i = 0; i < steps; i++) {
    hideCursor();
    if (pen) blocks[cy * GC + cx] = color;
    cx = Math.max(0, Math.min(GC - 1, cx + Math.sign(dx) * (i < Math.abs(dx) ? 1 : 0)));
    cy = Math.max(0, Math.min(GR - 1, cy + Math.sign(dy) * (i < Math.abs(dy) ? 1 : 0)));
    if (pen) blocks[cy * GC + cx] = color;
    hideCursor();  // repaints the new neighbourhood too, then the cursor goes on top
  }
  drawCursor();
}, 40);

touch.on("down", ({ x, y }) => {
  last = null;
  if (y < BAR) {
    const i = Math.floor(x / slot);
    if (i < palette.length) pick(i);
    else {
      clear();
      tone(500, 60);
    }
    return;
  }
  lcd.fillCircle(x, y, brush, color);
  last = { x, y };
});

touch.on("move", ({ x, y }) => {
  if (y < BAR + brush || !last) return;
  lcd.drawWideLine(last.x, last.y, x, y, brush * 2, color, lcd.BLACK);
  last = { x, y };
});

touch.on("up", () => (last = null));

clear();
