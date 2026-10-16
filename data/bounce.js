// Bouncing balls without flicker. The play area is drawn in horizontal bands through one
// small off-screen sprite (240x40, 8-bit = 9.6 KB): clear the band, draw the balls that
// overlap it, push it to the screen, move on to the next band. Tap the screen (or press Space)
// to add a ball.
const SMALL = lcd.height() < 200;
const SW = SMALL ? lcd.width() - 2 : 240, SH = SMALL ? lcd.height() - 16 : 160, BAND = 40;
const OX = (lcd.width() - SW) / 2, OY = SMALL ? 15 : 50;   // where the play area goes on screen
const band = lcd.createSprite(SW, BAND, 8);
const colors = [lcd.RED, lcd.GREEN, lcd.CYAN, lcd.YELLOW, lcd.MAGENTA, lcd.ORANGE, lcd.WHITE];

class Ball {
  constructor(x, y) {
    this.x = x;
    this.y = y;
    this.r = (SMALL ? 4 : 6) + Math.floor(Math.random() * (SMALL ? 7 : 10));
    this.vx = (Math.random() * 4 + 1) * (Math.random() < 0.5 ? -1 : 1);
    this.vy = (Math.random() * 4 + 1) * (Math.random() < 0.5 ? -1 : 1);
    this.color = colors[Math.floor(Math.random() * colors.length)];
  }
  step() {
    this.vy += 0.15;  // gravity
    this.x += this.vx;
    this.y += this.vy;
    if (this.x < this.r) { this.x = this.r; this.vx = -this.vx; }
    if (this.x > SW - this.r) { this.x = SW - this.r; this.vx = -this.vx; }
    if (this.y > SH - this.r) { this.y = SH - this.r; this.vy = -this.vy * 0.95; }
    if (this.y < this.r) { this.y = this.r; this.vy = -this.vy; }
  }
}

const balls = [];
for (let i = 0; i < 5; i++) balls.push(new Ball(SW / 2, SH / 3));

lcd.fillScreen(lcd.BLACK);
lcd.setTextDatum(lcd.TC);
lcd.setTextColor(lcd.WHITE, lcd.BLACK);
if (SMALL) {
  lcd.setTextDatum(lcd.TL);
  lcd.drawString(board.hasTouch ? "tap: add ball" : "Space: add ball", 2, 3, 1);
  lcd.setTextDatum(lcd.TR);
} else lcd.drawString(`Bouncing balls - ${board.hasTouch ? "tap" : "Space"} to add`, lcd.width() / 2, 12, 2);
lcd.drawRect(OX - 1, OY - 1, SW + 2, SH + 2, lcd.DARKGREY);

function addBall(x) {
  if (balls.length < 30) balls.push(new Ball(Math.max(20, Math.min(SW - 20, x - OX)), 20));
  tone(2000, 10);
}
touch.on("down", ({ x }) => addBall(x));
keyboard.on("key", ({ key }) => (key === " " ? addBall(OX + 20 + Math.random() * (SW - 40)) : null));

let frames = 0, fpsTime = millis();
lcd.setTextPadding(SMALL ? 100 : 160);
setInterval(() => {
  for (const b of balls) b.step();
  for (let top = 0; top < SH; top += BAND) {
    band.fillScreen(lcd.NAVY);
    for (const b of balls)
      if (b.y + b.r >= top && b.y - b.r < top + BAND) band.fillCircle(b.x, b.y - top, b.r, b.color);
    band.push(OX, OY + top);
  }
  frames++;
  const now = millis();
  if (now - fpsTime >= 1000) {
    if (SMALL) lcd.drawString(`${balls.length} balls, ${frames} fps`, lcd.width() - 2, 3, 1);
    else lcd.drawString(`${balls.length} balls, ${frames} fps`, lcd.width() / 2, OY + SH + 12, 2);
    frames = 0;
    fpsTime = now;
  }
}, 10);
