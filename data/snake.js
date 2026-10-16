// Snake: tap the screen on the side you want to turn to (relative to the snake's head).
// Keys: the arrows (; . , /) steer, Enter restarts.
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
const CELL = SMALL ? 9 : 16, TOP = SMALL ? 14 : 20, FONT = SMALL ? 1 : 2;
// Keyboard (Cardputer): ; . , / are the printed arrow keys (Fn + them sends Arrow* names).
const ARROW = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
const COLS = Math.floor(W / CELL), ROWS = Math.floor((H - TOP) / CELL);
const HI = "/snake.hi";

let snake, dir, nextDir, food, grow, score, best = Number(fs.read(HI) ?? 0), timer = 0, alive;

const cellX = (c) => c * CELL, cellY = (r) => TOP + r * CELL;
const cellKey = (p) => p.y * COLS + p.x;

function drawCell(p, color, round) {
  if (round) lcd.fillRoundRect(cellX(p.x) + 1, cellY(p.y) + 1, CELL - 2, CELL - 2, 4, color);
  else lcd.fillRect(cellX(p.x), cellY(p.y), CELL, CELL, color);
}

function hud() {
  lcd.setTextColor(lcd.WHITE, lcd.NAVY);
  lcd.setTextDatum(lcd.TL);
  lcd.drawString(` score ${score}  `, 2, (TOP - 8 * FONT) / 2, FONT);
  lcd.setTextDatum(lcd.TR);
  lcd.drawString(`  best ${best}   BOOT = menu `, W - 2, (TOP - 8 * FONT) / 2, FONT);
}

function placeFood() {
  const used = new Set(snake.map(cellKey));
  do food = { x: Math.floor(Math.random() * COLS), y: Math.floor(Math.random() * ROWS) };
  while (used.has(cellKey(food)));
  lcd.fillCircle(cellX(food.x) + CELL / 2, cellY(food.y) + CELL / 2, CELL / 2 - 2, lcd.RED);
}

function start() {
  lcd.fillScreen(lcd.BLACK);
  lcd.fillRect(0, 0, W, TOP, lcd.NAVY);
  const cy = Math.floor(ROWS / 2);
  snake = [{ x: 6, y: cy }, { x: 5, y: cy }, { x: 4, y: cy }];  // head first
  dir = nextDir = { x: 1, y: 0 };
  grow = 0;
  score = 0;
  alive = true;
  snake.forEach((p) => drawCell(p, lcd.GREEN, true));
  placeFood();
  hud();
  schedule();
}

function schedule() {
  clearInterval(timer);
  timer = setInterval(tick, Math.max(70, 160 - score * 3));
}

function tick() {
  dir = nextDir;
  const head = snake[0];
  const h = { x: head.x + dir.x, y: head.y + dir.y };
  const hitSelf = snake.some((p, i) => i < snake.length - 1 && p.x === h.x && p.y === h.y);
  if (h.x < 0 || h.y < 0 || h.x >= COLS || h.y >= ROWS || hitSelf) return die();
  drawCell(head, lcd.DARKGREEN, true);  // old head becomes body
  snake.unshift(h);
  drawCell(h, lcd.GREEN, true);
  if (h.x === food.x && h.y === food.y) {
    score++;
    grow += 2;
    tone(1500, 30);
    hud();
    placeFood();
    if (score % 5 === 0) schedule();  // speed up
  }
  if (grow > 0) grow--;
  else drawCell(snake.pop(), lcd.BLACK, false);
}

function die() {
  alive = false;
  clearInterval(timer);
  tone(150, 500);
  snake.forEach((p) => drawCell(p, lcd.MAROON, true));
  if (score > best) {
    best = score;
    fs.write(HI, String(best));
  }
  hud();
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.YELLOW, lcd.BLACK);
  if (board.hasTouch && !SMALL) lcd.drawString(` GAME OVER - score ${score} - tap to restart `, W / 2, H / 2, 2);
  else {
    lcd.drawString(` GAME OVER - score ${score} `, W / 2, H / 2 - 10, 2);
    lcd.drawString(board.hasTouch ? " tap to restart " : " Enter to restart ", W / 2, H / 2 + 10, 2);
  }
}

touch.on("down", ({ x, y }) => {
  if (!alive) return start();
  const head = snake[0];
  const dx = x - (cellX(head.x) + CELL / 2), dy = y - (cellY(head.y) + CELL / 2);
  // turn towards the tap, along whichever axis the snake is not already moving on
  if (dir.x !== 0) nextDir = { x: 0, y: dy < 0 ? -1 : 1 };
  else nextDir = { x: dx < 0 ? -1 : 1, y: 0 };
});

const DIRS = { up: { x: 0, y: -1 }, down: { x: 0, y: 1 }, left: { x: -1, y: 0 }, right: { x: 1, y: 0 } };
keyboard.on("key", ({ key }) => {
  if (!alive) return key === "Enter" || key === " " ? start() : null;  // Space: a gamepad's button 1
  const d = DIRS[ARROW[key]];
  if (d && d.x !== -dir.x && d.y !== -dir.y) nextDir = d;  // no reversing into yourself
});

start();
