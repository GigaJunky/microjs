// Breakout: drag your finger anywhere to move the paddle, tap to launch the ball.
// Keys: left/right (, /) move the paddle, Space launches.
// Gamepad: the stick moves the paddle in proportion (the D-pad at full speed), any button launches.
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
const COLS = 10, ROWS = 5, BW = Math.min(30, Math.floor(W / COLS)), BH = SMALL ? 6 : 10, GAP = 2, TOPY = SMALL ? 22 : 30;
const OFFX = (W - COLS * BW) / 2;
const PW = SMALL ? 40 : 50, PH = SMALL ? 4 : 6, PY = H - (SMALL ? 10 : 18), R = SMALL ? 2 : 3;
const ACT = board.hasTouch ? "tap" : "Space";
// Gamepad (USB joystick 1): the D-pad as x/y, and a dead zone for the analog stick.
const HAT = [[0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1]];
const hatXY = (h) => (h >= 0 && h < 8 ? HAT[h] : [0, 0]);
const stick = (v) => (Math.abs(v) > 0.15 ? v : 0);
joystick.keys(false);  // read the gamepad directly for analog paddle speed
// Keyboard (Cardputer): ; . , / are the printed arrow keys (Fn + them sends Arrow* names).
const ARROW = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
const ROW_COLORS = [lcd.RED, lcd.ORANGE, lcd.YELLOW, lcd.GREEN, lcd.CYAN];
const HI = "/breakout.hi";

let bricks = [], left = 0, paddleX = (W - PW) / 2, ball = null;
let score = 0, lives = 3, level = 1, best = Number(fs.read(HI) ?? 0);
let state = "ready";  // ready | playing | over

const brickX = (c) => OFFX + c * BW + 1;
const brickY = (r) => TOPY + r * (BH + GAP);

function drawBrick(i) {
  const c = i % COLS, r = Math.floor(i / COLS);
  lcd.fillRect(brickX(c), brickY(r), BW - 2, BH, bricks[i] ? ROW_COLORS[r] : lcd.BLACK);
}

// the ball's eraser can nick neighbouring bricks; repaint any that overlap (x, y)
function repairBricks(x, y) {
  if (y - R > brickY(ROWS)) return;
  const c0 = Math.floor((x - R - OFFX) / BW), c1 = Math.floor((x + R - OFFX) / BW);
  const r0 = Math.floor((y - R - TOPY) / (BH + GAP)), r1 = Math.floor((y + R - TOPY) / (BH + GAP));
  for (let r = Math.max(0, r0); r <= Math.min(ROWS - 1, r1); r++)
    for (let c = Math.max(0, c0); c <= Math.min(COLS - 1, c1); c++)
      if (bricks[r * COLS + c]) drawBrick(r * COLS + c);
}

function hud() {
  lcd.setTextColor(lcd.WHITE, lcd.BLACK);
  lcd.setTextPadding(0);
  lcd.setTextDatum(lcd.TL);
  lcd.drawString(`score ${score}   `, 4, 6, 2);
  lcd.setTextDatum(lcd.TR);
  lcd.drawString(`   lives ${lives}  best ${best}`, W - 4, 6, 2);
}

const MSGY = Math.round((brickY(ROWS) + PY) / 2);
function message(text) {
  lcd.fillRect(0, MSGY - 15, W, 30, lcd.BLACK);
  if (!text) return;
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.YELLOW);
  lcd.drawString(text, W / 2, MSGY, 2);
}

function drawPaddle(x, color) { lcd.fillRoundRect(x, PY, PW, PH, 3, color); }

function newBall() {
  ball = { x: paddleX + PW / 2, y: PY - R - 1, vx: 0, vy: 0 };
  lcd.fillCircle(ball.x, ball.y, R, lcd.WHITE);
}

function newLevel() {
  bricks = new Array(COLS * ROWS).fill(1);
  left = bricks.length;
  lcd.fillScreen(lcd.BLACK);
  bricks.forEach((_, i) => drawBrick(i));
  hud();
  drawPaddle(paddleX, lcd.SKYBLUE);
  newBall();
  state = "ready";
  message(SMALL ? `level ${level} - ${ACT} to launch` : `level ${level} - ${ACT} to launch   (BOOT = menu)`);
}

function serve() {
  const speed = (2.8 + level * 0.4) * (SMALL ? 0.7 : 1);
  ball.vx = speed * 0.6 * (Math.random() < 0.5 ? -1 : 1);
  ball.vy = -speed;
  state = "playing";
  message("");
}

function gameOver() {
  state = "over";
  if (score > best) {
    best = score;
    fs.write(HI, String(best));
  }
  hud();
  message(SMALL ? `GAME OVER - ${ACT} to play again` : `GAME OVER - score ${score} - ${ACT} to play again`);
  tone(150, 400);
}

function step() {
  if (state !== "playing") return;
  const b = ball;
  lcd.fillCircle(b.x, b.y, R, lcd.BLACK);
  repairBricks(b.x, b.y);
  b.x += b.vx;
  b.y += b.vy;
  if (b.x < R) { b.x = R; b.vx = -b.vx; tone(900, 8); }
  if (b.x > W - R) { b.x = W - R; b.vx = -b.vx; tone(900, 8); }
  if (b.y < TOPY - 6 + R) { b.y = TOPY - 6 + R; b.vy = -b.vy; tone(900, 8); }

  // paddle: the hit position decides the bounce angle
  if (b.vy > 0 && b.y + R >= PY && b.y + R <= PY + PH + b.vy && b.x >= paddleX - R && b.x <= paddleX + PW + R) {
    const speed = Math.hypot(b.vx, b.vy);
    const off = (b.x - (paddleX + PW / 2)) / (PW / 2);  // -1 .. 1
    b.vx = speed * 0.85 * Math.max(-0.95, Math.min(0.95, off));
    b.vy = -Math.sqrt(speed * speed - b.vx * b.vx);
    b.y = PY - R;
    tone(600, 15);
  }

  // bricks
  const c = Math.floor((b.x - OFFX) / BW), r = Math.floor((b.y - TOPY) / (BH + GAP));
  if (c >= 0 && c < COLS && r >= 0 && r < ROWS && bricks[r * COLS + c]) {
    const i = r * COLS + c;
    bricks[i] = 0;
    drawBrick(i);
    b.vy = -b.vy;
    score += (ROWS - r) * 10;
    hud();
    tone(1200 + (ROWS - r) * 150, 20);
    if (--left === 0) {
      level++;
      newLevel();
      return;
    }
  }

  if (b.y > H + R) {
    lives--;
    hud();
    if (lives === 0) return gameOver();
    tone(200, 200);
    newBall();
    state = "ready";
    message(`${ACT} to launch`);
    return;
  }
  lcd.fillCircle(b.x, b.y, R, lcd.WHITE);
}

function movePaddle(x) {
  const nx = Math.max(0, Math.min(W - PW, Math.round(x - PW / 2)));
  if (nx === paddleX) return;
  // erase only the strip the paddle uncovers
  if (nx > paddleX) lcd.fillRect(paddleX, PY, Math.min(nx - paddleX, PW), PH, lcd.BLACK);
  else lcd.fillRect(Math.max(nx + PW, paddleX), PY, Math.min(paddleX - nx, PW), PH, lcd.BLACK);
  paddleX = nx;
  drawPaddle(paddleX, lcd.SKYBLUE);
  if (state === "ready") {  // the ball rides on the paddle until launch
    lcd.fillCircle(ball.x, ball.y, R, lcd.BLACK);
    ball.x = paddleX + PW / 2;
    lcd.fillCircle(ball.x, ball.y, R, lcd.WHITE);
  }
}

touch.on("down", ({ x }) => {
  if (state === "over") {
    score = 0; lives = 3; level = 1;
    return newLevel();
  }
  movePaddle(x);
  if (state === "ready") serve();
});
touch.on("move", ({ x }) => movePaddle(x));

function restartOrServe() {
  if (state === "over") {
    score = 0; lives = 3; level = 1;
    return newLevel();
  }
  if (state === "ready") serve();
}
keyboard.on("key", ({ key }) => (key === " " || key === "Enter" ? restartOrServe() : null));
joystick.on("press", () => restartOrServe());
setInterval(() => {  // held arrow keys or the gamepad slide the paddle
  const keys = board.hasKeyboard ? keyboard.held().map((k) => ARROW[k]) : [];
  const pad = joystick.read(0), [hx] = hatXY(pad.hat);
  let dx = (keys.includes("right") ? 4 : 0) - (keys.includes("left") ? 4 : 0);
  dx += Math.round(6 * Math.max(-1, Math.min(1, stick(pad.x) + hx)));
  if (dx) movePaddle(paddleX + PW / 2 + dx);
}, 16);

newLevel();
setInterval(step, 16);
