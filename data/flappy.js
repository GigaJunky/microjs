// Flappy: tap anywhere (or press Space) to flap. Fly through the gaps.
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
const GROUND = H - (SMALL ? 10 : 16), BIRD_X = SMALL ? 50 : 70, R = SMALL ? 5 : 7;
const PIPE_W = SMALL ? 26 : 34, GAP = SMALL ? 50 : 74, SPEED = 2, SPACING = SMALL ? 120 : 150;
const GRAVITY = SMALL ? 0.25 : 0.35, FLAP = SMALL ? -3.3 : -4.6, MARGIN = SMALL ? 14 : 35;
const ACT = board.hasTouch ? "tap" : "Space";
const gapAt = () => MARGIN + Math.random() * (GROUND - GAP - 2 * MARGIN);
const SKY = lcd.color(80, 170, 230), PIPE = lcd.color(60, 190, 60), EDGE = lcd.DARKGREEN;
const HI = "/flappy.hi";

let bird, pipes, score, best = Number(fs.read(HI) ?? 0), state = "ready";

function drawGround() {
  lcd.fillRect(0, GROUND, W, H - GROUND, lcd.color(220, 200, 120));
  lcd.drawFastHLine(0, GROUND, W, lcd.color(120, 90, 40));
}

function hud() {
  lcd.setTextDatum(lcd.TC);
  lcd.setTextColor(lcd.WHITE, SKY);
  lcd.setTextPadding(60);
  lcd.drawString(String(score), W / 2, 8, 4);
  lcd.setTextPadding(0);
}

function drawPipeSlice(x, w, gapY, color) {  // one vertical strip of a pipe pair
  if (x >= W || x + w <= 0) return;
  lcd.fillRect(x, 0, w, gapY, color);
  lcd.fillRect(x, gapY + GAP, w, GROUND - gapY - GAP, color);
}

function start() {
  bird = { y: H / 2, vy: 0 };
  pipes = [];
  for (let i = 0; i < 3; i++) pipes.push({ x: W + 60 + i * SPACING, gapY: gapAt(), passed: false });
  score = 0;
  state = "ready";
  lcd.fillScreen(SKY);
  drawGround();
  hud();
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(`${ACT} to flap`, W / 2, H / 2 + (SMALL ? 16 : 40), 4);
  lcd.setTextColor(lcd.NAVY);
  lcd.drawString(`best ${best}   BOOT = menu`, W / 2, H / 2 + (SMALL ? 40 : 70), 2);
}

function crash() {
  state = "over";
  tone(120, 400);
  if (score > best) {
    best = score;
    fs.write(HI, String(best));
  }
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE, lcd.MAROON);
  lcd.drawString(` GAME OVER - ${score} `, W / 2, H / 2 - 10, 4);
  lcd.setTextColor(lcd.WHITE, lcd.NAVY);
  lcd.drawString(` best ${best} - ${board.hasTouch ? "tap" : "Enter"} to restart `, W / 2, H / 2 + 20, 2);
}

function frame() {
  if (state !== "play") return;
  // pipes scroll left: paint the new leading strip, erase the trailing strip
  for (const p of pipes) {
    p.x -= SPEED;
    drawPipeSlice(p.x, SPEED, p.gapY, EDGE);            // darker outline on the left edge...
    drawPipeSlice(p.x + SPEED, SPEED, p.gapY, PIPE);    // ...and last frame's outline becomes pipe
    drawPipeSlice(p.x + PIPE_W, SPEED, p.gapY, SKY);    // uncover the sky behind it
    if (p.x + PIPE_W < 0) {
      p.x += pipes.length * SPACING;
      p.gapY = gapAt();
      p.passed = false;
    }
    if (!p.passed && p.x + PIPE_W < BIRD_X) {
      p.passed = true;
      score++;
      tone(1600, 30);
      hud();
    }
  }
  // bird
  lcd.fillRect(BIRD_X - R, bird.y - R, 2 * R + 7, 2 * R + 1, SKY);  // erase bird incl. beak
  bird.vy += GRAVITY;
  bird.y += bird.vy;
  for (const p of pipes) {
    const inX = BIRD_X + R > p.x && BIRD_X - R < p.x + PIPE_W;
    if (inX && (bird.y - R < p.gapY || bird.y + R > p.gapY + GAP)) return crash();
  }
  if (bird.y + R >= GROUND || bird.y < R) return crash();
  lcd.fillCircle(BIRD_X, bird.y, R, lcd.YELLOW);
  lcd.fillCircle(BIRD_X + 3, bird.y - 2, 2, lcd.BLACK);   // eye
  lcd.fillTriangle(BIRD_X + R - 1, bird.y, BIRD_X + R + 5, bird.y + 2, BIRD_X + R - 1, bird.y + 4, lcd.ORANGE);
}

function flap() {
  if (state === "over") return start();
  if (state === "ready") {
    state = "play";
    lcd.fillRect(0, H / 2 + (SMALL ? 4 : 20), W, GROUND - H / 2 - (SMALL ? 4 : 20), SKY);
  }
  bird.vy = FLAP;
  tone(700, 15);
}
touch.on("down", flap);
keyboard.on("key", ({ key }) => {
  if (key === "Enter" && state === "over") start();
  else if (key === " " && state !== "over") flap();
});

start();
setInterval(frame, 20);
