// Asteroids, one-finger edition: while you touch the screen the ship turns towards your finger
// and fires. Touch far away from the ship to also thrust in that direction.
// Keys: left/right (, /) turn, up (;) thrust, Space fire, Enter play again.
// Gamepad: stick or D-pad turns (the stick in proportion), button 1 fires, button 2 / up thrusts.
const W = lcd.width(), H = lcd.height();
const HI = "/asteroids.hi";
const K = H < 200 ? 0.65 : 1;                  // smaller rocks on small screens
const SIZES = [0, 7, 13, 22].map((r) => r * K); // radius by size class 1..3
// Gamepad (USB joystick 1): the D-pad as x/y, and a dead zone for the analog stick.
const HAT = [[0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1]];
const hatXY = (h) => (h >= 0 && h < 8 ? HAT[h] : [0, 0]);
const stick = (v) => (Math.abs(v) > 0.15 ? v : 0);
joystick.keys(false);  // read the gamepad directly (analog turning) instead of as arrow keys
// Keyboard (Cardputer): ; . , / are the printed arrow keys (Fn + them sends Arrow* names).
const ARROW = { ";": "up", ".": "down", ",": "left", "/": "right", ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
const POINTS = [0, 100, 50, 20];

let ship, rocks, shots, score, lives, wave, best = Number(fs.read(HI) ?? 0);
let finger = null, lastShot = 0, state = "play", respawnAt = 0, frames = 0, overAt = 0;

const wrap = (v, max) => (v < 0 ? v + max : v >= max ? v - max : v);

function makeRock(x, y, size) {
  const n = 8, pts = [];
  for (let i = 0; i < n; i++) {
    const a = (i / n) * Math.PI * 2, r = SIZES[size] * (0.7 + Math.random() * 0.3);
    pts.push(Math.cos(a) * r, Math.sin(a) * r);   // fixed outline, flattened [x0, y0, x1, y1, ...]
  }
  const speed = (0.4 + Math.random() * 0.8) * (4 - size) * 0.5 + wave * 0.1;
  const dir = Math.random() * Math.PI * 2;
  return { x, y, vx: Math.cos(dir) * speed, vy: Math.sin(dir) * speed, size, pts, px: x, py: y };
}

function drawRock(r, x, y, color) {
  const p = r.pts;
  for (let i = 0; i < p.length; i += 2) {
    const j = (i + 2) % p.length;
    lcd.drawLine(x + p[i], y + p[i + 1], x + p[j], y + p[j + 1], color);
  }
}

function shipPoints(s) {
  const c = Math.cos(s.a), n = Math.sin(s.a);
  return [s.x + c * 10, s.y + n * 10, s.x - c * 7 + n * 6, s.y - n * 7 - c * 6, s.x - c * 7 - n * 6, s.y - n * 7 + c * 6];
}

function drawShip(pts, color, flame, flameColor) {
  lcd.drawTriangle(pts[0], pts[1], pts[2], pts[3], pts[4], pts[5], color);
  if (flame) {
    const mx = (pts[2] + pts[4]) / 2, my = (pts[3] + pts[5]) / 2;
    lcd.drawLine(mx, my, mx - (pts[0] - mx) * 0.5, my - (pts[1] - my) * 0.5, flameColor);
  }
}

function hud() {
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE, lcd.BLACK);
  lcd.drawString(`${score}  `, 4, 4, 2);
  lcd.setTextDatum(lcd.TR);
  lcd.drawString(`  ${"A ".repeat(Math.max(0, lives))} best ${best}`, W - 4, 4, 2);
}

function newWave() {
  wave++;
  rocks = [];
  for (let i = 0; i < 2 + wave; i++) {
    const edge = Math.random() < 0.5;  // spawn along the screen edges, away from the ship
    rocks.push(makeRock(edge ? Math.random() * W : 0, edge ? 0 : Math.random() * H, 3));
  }
}

function newShip() {
  ship = { x: W / 2, y: H / 2, vx: 0, vy: 0, a: -Math.PI / 2, pts: null, thrust: false, safeUntil: millis() + 2000 };
}

function start() {
  score = 0; lives = 3; wave = 0; shots = []; state = "play";
  lcd.fillScreen(lcd.BLACK);
  newShip();
  newWave();
  hud();
}

function explode(x, y, big) {
  tone(big ? 90 : 140, big ? 250 : 80);
  lcd.drawCircle(x, y, big ? 12 : 6, lcd.ORANGE);
  setTimeout(() => lcd.drawCircle(x, y, big ? 12 : 6, lcd.BLACK), 120);
}

function fire(now) {
  if (now - lastShot <= 220 || shots.length >= 6) return;
  lastShot = now;
  shots.push({ x: ship.x + Math.cos(ship.a) * 10, y: ship.y + Math.sin(ship.a) * 10,
               vx: Math.cos(ship.a) * 6 + ship.vx, vy: Math.sin(ship.a) * 6 + ship.vy, die: now + 900 * K });
  tone(1800, 8);
}

function frame() {
  const now = millis();

  // ---- erase last frame
  for (const r of rocks) drawRock(r, r.px, r.py, lcd.BLACK);
  for (const s of shots) lcd.fillRect(s.px - 1, s.py - 1, 3, 3, lcd.BLACK);
  if (ship && ship.pts) drawShip(ship.pts, lcd.BLACK, ship.thrust, lcd.BLACK);

  if (state === "over") return;

  // ---- ship control
  if (ship) {
    ship.thrust = false;
    const keys = board.hasKeyboard ? keyboard.held().map((k) => ARROW[k] ?? k) : [];
    const pad = joystick.read(0), [hx, hy] = hatXY(pad.hat);
    const turn = (keys.includes("right") ? 1 : 0) - (keys.includes("left") ? 1 : 0) + stick(pad.x) + hx;
    const push = keys.includes("up") || pad.y < -0.5 || hy < 0 || (pad.buttons & 2);
    const shoot = keys.includes(" ") || (pad.buttons & 1);
    if (turn || push || shoot || keys.length) {
      ship.a += 0.13 * Math.max(-1, Math.min(1, turn));
      if (push) {
        ship.vx += Math.cos(ship.a) * 0.18;
        ship.vy += Math.sin(ship.a) * 0.18;
        ship.thrust = true;
      }
      if (shoot) fire(now);
    } else if (finger) {
      let target = Math.atan2(finger.y - ship.y, finger.x - ship.x);
      let d = target - ship.a;
      while (d > Math.PI) d -= Math.PI * 2;
      while (d < -Math.PI) d += Math.PI * 2;
      ship.a += Math.max(-0.2, Math.min(0.2, d));
      if (Math.hypot(finger.x - ship.x, finger.y - ship.y) > 90) {
        ship.vx += Math.cos(ship.a) * 0.18;
        ship.vy += Math.sin(ship.a) * 0.18;
        ship.thrust = true;
      }
      if (Math.abs(d) < 0.5) fire(now);
    }
    ship.vx *= 0.99;
    ship.vy *= 0.99;
    ship.x = wrap(ship.x + ship.vx, W);
    ship.y = wrap(ship.y + ship.vy, H);
  } else if (now > respawnAt) {
    newShip();
  }

  // ---- move rocks & shots
  for (const r of rocks) {
    r.x = wrap(r.x + r.vx, W);
    r.y = wrap(r.y + r.vy, H);
  }
  shots = shots.filter((s) => s.die > now);
  for (const s of shots) {
    s.x = wrap(s.x + s.vx, W);
    s.y = wrap(s.y + s.vy, H);
  }

  // ---- collisions: shots vs rocks
  const born = [];
  for (const s of shots) {
    for (const r of rocks) {
      if (r.dead || (s.x - r.x) ** 2 + (s.y - r.y) ** 2 > SIZES[r.size] ** 2) continue;
      r.dead = s.dead = true;
      score += POINTS[r.size];
      explode(r.x, r.y, r.size === 3);
      if (r.size > 1) born.push(makeRock(r.x, r.y, r.size - 1), makeRock(r.x, r.y, r.size - 1));
      break;
    }
  }
  if (born.length || shots.some((s) => s.dead)) {
    rocks = rocks.filter((r) => !r.dead).concat(born);
    shots = shots.filter((s) => !s.dead);
    hud();
  }

  // ---- ship vs rocks
  if (ship && now > ship.safeUntil) {
    for (const r of rocks) {
      if ((ship.x - r.x) ** 2 + (ship.y - r.y) ** 2 < (SIZES[r.size] + 6) ** 2) {
        explode(ship.x, ship.y, true);
        ship = null;
        lives--;
        hud();
        if (lives <= 0) return gameOver();
        respawnAt = now + 1500;
        break;
      }
    }
  }
  if (!rocks.length) newWave();

  if (++frames % 20 === 0) hud();  // rocks drifting over the score line erase it

  // ---- draw this frame
  for (const r of rocks) {
    drawRock(r, r.x, r.y, lcd.LIGHTGREY);
    r.px = r.x;
    r.py = r.y;
  }
  for (const s of shots) {
    lcd.fillRect(s.x - 1, s.y - 1, 3, 3, lcd.YELLOW);
    s.px = s.x;
    s.py = s.y;
  }
  if (ship) {
    ship.pts = shipPoints(ship);
    const blink = now < ship.safeUntil && Math.floor(now / 150) % 2;
    drawShip(ship.pts, blink ? lcd.DARKGREY : lcd.WHITE, ship.thrust, lcd.ORANGE);
  }
}

function gameOver() {
  state = "over";
  overAt = millis();
  if (score > best) {
    best = score;
    fs.write(HI, String(best));
  }
  hud();
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.YELLOW, lcd.BLACK);
  lcd.drawString(" GAME OVER ", W / 2, H / 2 - 12, 4);
  lcd.drawString(board.hasTouch ? " tap to play again   (BOOT = menu) " : " Enter: play again ", W / 2, H / 2 + 16, 2);
}

touch.on("down", (p) => {
  if (state === "over") return start();
  finger = p;
});
touch.on("move", (p) => (finger = p));
touch.on("up", () => (finger = null));
keyboard.on("key", ({ key }) => (state === "over" && key === "Enter" ? start() : null));
// any gamepad button plays again (after a moment, so a held fire button doesn't skip the score)
joystick.on("press", () => (state === "over" && millis() - overAt > 1000 ? start() : null));

start();
setInterval(frame, 30);
