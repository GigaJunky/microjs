// Tetris: buttons on the right - rotate, left, right, drop. Tapping the board rotates too.
// Keys: up (;) rotate, left/right (, /) move, down (.) soft drop, Space hard drop, Enter restart.
// Gamepad: D-pad / stick moves (held = auto-repeat), down soft-drops, up or button 1 rotates,
// button 2 hard-drops, any button restarts.
const W = lcd.width(), H = lcd.height();
const SMALL = H < 200, BUTTONS_ON = board.hasTouch && !SMALL;
const COLS = 10, ROWS = 20, C = SMALL ? Math.floor((H - 6) / ROWS) : 11;  // cells, cell size in px
const BX = 8, BY = (H - ROWS * C) / 2;            // board position
const PX = BX + COLS * C + 14;                    // side panel x
const HI = "/tetris.hi";
// Gamepad (USB joystick 1): the D-pad as x/y, and a dead zone for the analog stick.
const HAT = [[0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1]];
const hatXY = (h) => (h >= 0 && h < 8 ? HAT[h] : [0, 0]);
const stick = (v) => (Math.abs(v) > 0.15 ? v : 0);
joystick.keys(false);  // gamepad handled below, with its own auto-repeat
// Keyboard (Cardputer): ; . , / are the printed arrow keys (Fn + them sends Arrow* names).
const ARROW = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };


const SHAPES = {
  I: { cells: [[0, 1], [1, 1], [2, 1], [3, 1]], size: 4, color: lcd.CYAN },
  O: { cells: [[0, 0], [1, 0], [0, 1], [1, 1]], size: 2, color: lcd.YELLOW },
  T: { cells: [[1, 0], [0, 1], [1, 1], [2, 1]], size: 3, color: lcd.MAGENTA },
  S: { cells: [[1, 0], [2, 0], [0, 1], [1, 1]], size: 3, color: lcd.GREEN },
  Z: { cells: [[0, 0], [1, 0], [1, 1], [2, 1]], size: 3, color: lcd.RED },
  J: { cells: [[0, 0], [0, 1], [1, 1], [2, 1]], size: 3, color: lcd.BLUE },
  L: { cells: [[2, 0], [0, 1], [1, 1], [2, 1]], size: 3, color: lcd.ORANGE },
};
const NAMES = Object.keys(SHAPES);

// board[i] = colour or 0; shown[i] = what is currently on screen (draw only differences)
let board, shown, piece, next, score, lines, level, best = Number(fs.read(HI) ?? 0);
let timer = 0, over = false, overAt = 0;

const BUTTONS = [
  { label: "rotate", x: PX, y: 140, w: 90, h: 44, act: () => rotate() },
  { label: "drop", x: PX + 96, y: 140, w: 90, h: 44, act: () => hardDrop() },
  { label: "<", x: PX, y: 190, w: 90, h: 44, act: () => move(-1) },
  { label: ">", x: PX + 96, y: 190, w: 90, h: 44, act: () => move(1) },
];

function randomPiece() {
  const s = SHAPES[NAMES[Math.floor(Math.random() * NAMES.length)]];
  return { cells: s.cells.map(([x, y]) => [x, y]), size: s.size, color: s.color, x: 3, y: 0 };
}

function fits(cells, px, py) {
  for (const [x, y] of cells) {
    const bx = px + x, by = py + y;
    if (bx < 0 || bx >= COLS || by >= ROWS) return false;
    if (by >= 0 && board[by * COLS + bx]) return false;
  }
  return true;
}

function paint(i, color) {  // draw one cell, only if it changed
  if (shown[i] === color) return;
  shown[i] = color;
  const x = BX + (i % COLS) * C, y = BY + Math.floor(i / COLS) * C;
  if (color) lcd.fillRoundRect(x + 1, y + 1, C - 2, C - 2, 2, color);
  else lcd.fillRect(x, y, C, C, lcd.BLACK);
}

// Moving the piece only repaints the few cells it left and entered; the whole board is
// checked only after a piece lands (lines may have been cleared).
let pieceShown = [];
function drawPiece() {
  const now = piece ? piece.cells.map(([x, y]) => (piece.y + y) * COLS + piece.x + x).filter((i) => i >= 0) : [];
  for (const i of pieceShown) if (!now.includes(i)) paint(i, board[i]);
  for (const i of now) paint(i, piece.color);
  pieceShown = now;
}

function render() {
  for (let i = 0; i < board.length; i++) paint(i, board[i]);
  pieceShown = [];
  drawPiece();
}

function panel() {
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE, lcd.BLACK);
  lcd.setTextPadding(90);
  const L = SMALL ? 16 : 18;
  lcd.drawString(`score ${score}`, PX, 10, 2);
  lcd.drawString(`lines ${lines}`, PX, 10 + L, 2);
  lcd.drawString(`level ${level}`, PX, 10 + 2 * L, 2);
  lcd.setTextColor(lcd.LIGHTGREY, lcd.BLACK);
  lcd.drawString(`best ${best}`, PX, 10 + 3 * L, 2);
  lcd.setTextPadding(0);
  // next piece preview
  const NX = SMALL ? W - 4 * C - 6 : PX + 100;
  lcd.fillRect(NX, 10, 4 * C, 4 * C, lcd.BLACK);
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString("next", NX, 0, 1);
  for (const [x, y] of next.cells) lcd.fillRoundRect(NX + x * C + 1, 12 + y * C + 1, C - 2, C - 2, 2, next.color);
}

function drawButtons() {
  if (!BUTTONS_ON) {  // keyboard help instead
    lcd.setTextDatum(lcd.TL);
    lcd.setTextColor(lcd.DARKGREY);
    const y = SMALL ? H - 40 : 140;
    ["; rotate   , / move", ". down  space drop", "BOOT = menu"].forEach((t, i) => lcd.drawString(t, PX, y + i * 12, 1));
    return;
  }
  lcd.setTextDatum(lcd.MC);
  for (const b of BUTTONS) {
    lcd.fillRoundRect(b.x, b.y, b.w, b.h, 8, lcd.NAVY);
    lcd.drawRoundRect(b.x, b.y, b.w, b.h, 8, lcd.SKYBLUE);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2, b.label.length === 1 ? 4 : 2);
  }
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString("BOOT = menu", PX + 93, 108, 1);
}

function spawn() {
  piece = next;
  next = randomPiece();
  panel();
  if (!fits(piece.cells, piece.x, piece.y)) gameOver();
}

function lockPiece() {
  for (const [x, y] of piece.cells) {
    const by = piece.y + y;
    if (by < 0) return gameOver();
    board[by * COLS + piece.x + x] = piece.color;
  }
  // clear full rows
  let cleared = 0;
  for (let r = ROWS - 1; r >= 0; r--) {
    let full = true;
    for (let c = 0; c < COLS; c++) if (!board[r * COLS + c]) { full = false; break; }
    if (!full) continue;
    board.splice(r * COLS, COLS);
    for (let c = 0; c < COLS; c++) board.unshift(0);
    cleared++;
    r++;  // re-check the same row index
  }
  if (cleared) {
    score += [0, 40, 100, 300, 1200][cleared] * level;
    lines += cleared;
    level = 1 + Math.floor(lines / 10);
    tone(800 + cleared * 300, 120);
    schedule();
  } else tone(300, 20);
  spawn();
}

function tick() {
  if (over) return;
  if (fits(piece.cells, piece.x, piece.y + 1)) {
    piece.y++;
    drawPiece();
  } else {
    lockPiece();
    render();
  }
}

function move(dx) {
  if (fits(piece.cells, piece.x + dx, piece.y)) { piece.x += dx; drawPiece(); }
}

function rotate() {
  const n = piece.size;
  if (n === 2) return;
  const turned = piece.cells.map(([x, y]) => [n - 1 - y, x]);
  for (const kick of [0, -1, 1, -2, 2]) {  // simple wall kicks
    if (fits(turned, piece.x + kick, piece.y)) {
      piece.cells = turned;
      piece.x += kick;
      tone(1000, 10);
      return drawPiece();
    }
  }
}

function hardDrop() {
  while (fits(piece.cells, piece.x, piece.y + 1)) { piece.y++; score += 2; }
  lockPiece();
  render();
}

function schedule() {
  clearInterval(timer);
  timer = setInterval(tick, Math.max(90, 650 - (level - 1) * 60));
}

function gameOver() {
  over = true;
  overAt = millis();
  clearInterval(timer);
  if (score > best) {
    best = score;
    fs.write(HI, String(best));
  }
  tone(150, 600);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.YELLOW, lcd.BLACK);
  const cx = SMALL ? W / 2 : BX + COLS * C / 2;
  lcd.drawString(" GAME OVER ", cx, H / 2 - 10, 4);
  lcd.drawString(board.hasTouch ? " tap to restart " : " Enter to restart ", cx, H / 2 + 16, 2);
}

function start() {
  board = new Array(COLS * ROWS).fill(0);
  shown = new Array(COLS * ROWS).fill(-1);  // force a full first draw
  score = 0; lines = 0; level = 1; over = false;
  lcd.fillScreen(lcd.BLACK);
  lcd.drawRect(BX - 2, BY - 2, COLS * C + 4, ROWS * C + 4, lcd.DARKGREY);
  drawButtons();
  next = randomPiece();
  spawn();
  render();
  schedule();
}

touch.on("down", ({ x, y }) => {
  if (over) return start();
  const b = BUTTONS_ON && BUTTONS.find((b) => x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h);
  if (b) b.act();
  else if (x < PX - 6) rotate();
});

keyboard.on("key", ({ key }) => {
  if (over) return key === "Enter" ? start() : null;
  const d = ARROW[key];
  if (d === "up") rotate();
  else if (d === "left") move(-1);
  else if (d === "right") move(1);
  else if (d === "down") softDrop();
  else if (key === " ") hardDrop();
});

function softDrop() {
  if (fits(piece.cells, piece.x, piece.y + 1)) { piece.y++; score++; drawPiece(); }
}

// gamepad: left / right / down auto-repeat while held (a pause, then fast), up rotates once
const padRepeat = { left: 0, right: 0, down: 0 };
let padUp = false;
setInterval(() => {
  const p = joystick.read(0);
  if (over || !piece || !p.connected) return;
  const [hx, hy] = hatXY(p.hat), now = millis();
  const held = { left: hx < 0 || p.x < -0.5, right: hx > 0 || p.x > 0.5, down: hy > 0 || p.y > 0.5 };
  for (const d of ["left", "right", "down"]) {
    if (!held[d]) { padRepeat[d] = 0; continue; }
    if (padRepeat[d] && now < padRepeat[d]) continue;
    padRepeat[d] = now + (padRepeat[d] ? 50 : d === "down" ? 60 : 170);
    if (d === "left") move(-1);
    else if (d === "right") move(1);
    else softDrop();
  }
  const up = hy < 0 || p.y < -0.5;
  if (up && !padUp) rotate();
  padUp = up;
}, 20);
joystick.on("press", ({ button }) => {
  if (over) return millis() - overAt > 800 ? start() : null;
  if (button === 1) hardDrop();
  else rotate();
});

start();
