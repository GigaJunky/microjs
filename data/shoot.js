// shoot.js - a tribute to Tandy's Shooting Gallery ROM pak (TRS-80 Color Computer, 1980).
// Rows of ducks, rabbits and owls stream across the screen; move the gun along the bottom and
// shoot them. Ducks that reach the end of the bottom row sometimes swoop down and eat bullets.
// The CoCo moved a few blocky targets a step at a time; here every target is drawn smooth and
// animated every frame, with dozens on screen at once - the frame rate is shown at the top.
// Keys: left / right (or , /) move, Space fires. Gamepad: stick / D-pad moves, button 1 fires.
// Touch: tap where you want to shoot.
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
const S = SMALL ? 0.7 : 1;  // size of everything
let BAND = 40;
while (H % BAND) BAND--;
const band = lcd.createSprite(W, BAND, 8);
band.setTextDatum(lcd.TC);
const HAT = [[0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1]];
const hatXY = (h) => (h >= 0 && h < 8 ? HAT[h] : [0, 0]);
const stick = (v) => (Math.abs(v) > 0.15 ? v : 0);
const ARROW = { ",": "left", "/": "right", ArrowLeft: "left", ArrowRight: "right", a: "left", d: "right" };
joystick.keys(false);

const TOP = SMALL ? 14 : 20, GUN_Y = H - (SMALL ? 14 : 20);
const ROWS = [  // y, speed (px/s, sign = direction), kind, spacing
  { y: TOP + 32 * S, speed: 70, kind: "owl", gap: 46 * S, pts: 30 },
  { y: TOP + 80 * S, speed: -55, kind: "rabbit", gap: 40 * S, pts: 20 },
  { y: TOP + 124 * S, speed: 45, kind: "duck", gap: 34 * S, pts: 10 },
];
const BG = lcd.color(10, 20, 70), CURTAIN = lcd.color(150, 20, 30), SHELF = lcd.color(120, 80, 40);

let state = "title", score = 0, best = 0, ammo = 0, wave = 1, gunX = W / 2;
let targets = [], bullets = [], sparks = [], divers = [], lastFire = 0, msg = "", msgUntil = 0;
try { best = parseInt(fs.read("/shoot.hi")) || 0; } catch (e) {}

function fillRows() {
  targets = [];
  const mult = 1 + (wave - 1) * 0.18;
  for (const r of ROWS) {
    for (let x = -r.gap; x < W + r.gap; x += r.gap)
      targets.push({ row: r, x, y: r.y, alive: true, phase: Math.random() * 6, mult });
  }
}
function start() {
  score = 0; wave = 1; ammo = 60; bullets = []; sparks = []; divers = [];
  fillRows();
  state = "play";
  say("WAVE 1", 1500);
}
function say(t, ms) { msg = t; msgUntil = millis() + ms; }

// ---- display list, replayed into horizontal bands (no flicker, little memory)
let ops = [];
const R = (x, y, w, h, c) => ops.push([0, y, y + h, x, y, w, h, c]);
const C = (x, y, r, c) => ops.push([1, y - r, y + r, x, y, r, c]);
const E = (x, y, rx, ry, c) => ops.push([2, y - ry, y + ry, x, y, rx, ry, c]);
const T = (x1, y1, x2, y2, x3, y3, c) => ops.push([3, Math.min(y1, y2, y3), Math.max(y1, y2, y3), x1, y1, x2, y2, x3, y3, c]);
const TXT = (s, x, y, f, c) => ops.push([4, y, y + (f === 4 ? 26 : f === 2 ? 16 : 8), s, x, y, f, c]);
function render() {
  for (let top = 0; top < H; top += BAND) {
    band.fillScreen(BG);
    const bot = top + BAND;
    for (let i = 0; i < ops.length; i++) {
      const o = ops[i];
      if (o[2] < top || o[1] >= bot) continue;
      switch (o[0]) {
        case 0: band.fillRect(o[3], o[4] - top, o[5], o[6], o[7]); break;
        case 1: band.fillCircle(o[3], o[4] - top, o[5], o[6]); break;
        case 2: band.fillEllipse(o[3], o[4] - top, o[5], o[6], o[7]); break;
        case 3: band.fillTriangle(o[3], o[4] - top, o[5], o[6] - top, o[7], o[8] - top, o[9]); break;
        case 4: band.setTextColor(o[7]); band.drawString(o[3], o[4], o[5] - top, o[6]); break;
      }
    }
    band.push(0, top);
  }
}

function drawTarget(t, now) {
  const x = t.x, y = t.y, dir = t.row.speed > 0 ? 1 : -1, k = S;
  const wob = Math.sin(now / 120 + t.phase);
  if (t.row.kind === "duck" || t.dive) {
    E(x, y, 12 * k, 7 * k, lcd.YELLOW);
    C(x + dir * 9 * k, y - 7 * k, 5 * k, lcd.YELLOW);
    T(x + dir * 13 * k, y - 8 * k, x + dir * 13 * k, y - 4 * k, x + dir * 19 * k, y - 6 * k, lcd.ORANGE);
    C(x + dir * 10 * k, y - 8 * k, 1.5 * k, lcd.BLACK);
    T(x - dir * 4 * k, y - 2 * k, x - dir * 12 * k, y - (6 + wob * 5) * k, x + dir * 2 * k, y - 3 * k, lcd.color(220, 180, 0));
  } else if (t.row.kind === "rabbit") {
    const hop = Math.abs(wob) * 4 * k;
    E(x, y - hop, 10 * k, 8 * k, lcd.WHITE);
    C(x + dir * 9 * k, y - 6 * k - hop, 5 * k, lcd.WHITE);
    E(x + dir * 7 * k, y - 16 * k - hop, 2 * k, 7 * k, lcd.WHITE);
    E(x + dir * 11 * k, y - 15 * k - hop, 2 * k, 7 * k, lcd.LIGHTGREY);
    C(x + dir * 11 * k, y - 7 * k - hop, 1.5 * k, lcd.RED);
    C(x - dir * 10 * k, y - 3 * k - hop, 3 * k, lcd.WHITE);
  } else {  // owl
    E(x, y, 11 * k, 13 * k, lcd.color(160, 110, 60));
    E(x, y + 3 * k, 7 * k, 8 * k, lcd.color(220, 190, 140));
    T(x - 10 * k, y - 8 * k, x - 6 * k, y - 18 * k, x - 3 * k, y - 10 * k, lcd.color(160, 110, 60));
    T(x + 10 * k, y - 8 * k, x + 6 * k, y - 18 * k, x + 3 * k, y - 10 * k, lcd.color(160, 110, 60));
    const blink = wob > 0.95;
    for (const s of [-1, 1]) {
      C(x + s * 5 * k, y - 5 * k, 4 * k, lcd.WHITE);
      if (!blink) C(x + s * 5 * k + (t.row.speed > 0 ? 1 : -1), y - 5 * k, 2 * k, lcd.BLACK);
    }
    T(x - 2 * k, y - 1 * k, x + 2 * k, y - 1 * k, x, y + 3 * k, lcd.ORANGE);
  }
}

// ---- game update
let lastT = millis(), frames = 0, fps = 0, fpsAt = 0;
function frame() {
  const now = millis(), dt = Math.min(0.05, (now - lastT) / 1000);
  lastT = now;
  ops = [];
  // the booth
  R(0, 0, W, TOP, CURTAIN);
  for (let x = 0; x < W; x += 16) C(x + 8, TOP, 8, CURTAIN);
  for (const r of ROWS) R(0, r.y + 14 * S, W, 3, SHELF);

  if (state === "play") update(now, dt);
  else for (const t of targets) t.x += t.row.speed * dt * 0.5, wrap(t);

  for (const t of targets) if (t.alive) drawTarget(t, now);
  for (const d of divers) drawTarget(d, now);
  for (const b of bullets) R(b.x - 1, b.y, 3, 7, lcd.WHITE);
  for (const s of sparks) C(s.x, s.y, s.r, s.c);

  // the gun
  R(gunX - 3, GUN_Y - 12 * S, 6, 12 * S, lcd.LIGHTGREY);
  E(gunX, GUN_Y + 3, 16 * S, 7 * S, lcd.color(90, 90, 110));

  TXT(`${score}`, W * 0.15, 3, SMALL ? 1 : 2, lcd.WHITE);
  TXT(`ammo ${ammo}`, W / 2, 3, SMALL ? 1 : 2, ammo < 10 ? lcd.YELLOW : lcd.WHITE);
  TXT(`${fps} fps`, W * 0.86, 3, SMALL ? 1 : 2, lcd.CYAN);
  if (state === "title" || state === "over") {
    const y = H * 0.5;
    R(0, y - 4, W, SMALL ? 58 : 84, lcd.BLACK);
    TXT(state === "over" ? "OUT OF AMMO" : "SHOOTING GALLERY", W / 2, y, SMALL ? 2 : 4, lcd.YELLOW);
    TXT(state === "over" ? `score ${score}   best ${best}` : "for the CoCo ROM pak, 1980", W / 2, y + (SMALL ? 18 : 30), SMALL ? 1 : 2, lcd.LIGHTGREY);
    TXT(`${targets.length} targets at ${fps} fps (a 6809 did ~8)`, W / 2, y + (SMALL ? 30 : 50), 1, lcd.CYAN);
    TXT("Space / button 1 to start", W / 2, y + (SMALL ? 42 : 66), SMALL ? 1 : 2, lcd.WHITE);
  }
  if (msgUntil > now) TXT(msg, W / 2, H * 0.62, SMALL ? 2 : 4, lcd.WHITE);
  render();
  frames++;
  if (now - fpsAt >= 1000) { fps = frames; frames = 0; fpsAt = now; }
}

function wrap(t) {
  const span = Math.ceil((W + 2 * t.row.gap) / t.row.gap) * t.row.gap;
  if (t.row.speed > 0 && t.x > W + t.row.gap) { t.x -= span; return true; }
  if (t.row.speed < 0 && t.x < -t.row.gap) { t.x += span; return true; }
  return false;
}

function update(now, dt) {
  // move the gun
  const keys = board.hasKeyboard ? keyboard.held().map((k) => ARROW[k] ?? k) : [];
  const pad = joystick.read(0), [hx] = hatXY(pad.hat);
  const mv = (keys.includes("right") ? 1 : 0) - (keys.includes("left") ? 1 : 0) + stick(pad.x) + hx;
  gunX = Math.max(10, Math.min(W - 10, gunX + Math.max(-1, Math.min(1, mv)) * 260 * dt));
  if (keys.includes(" ") || (pad.buttons & 1)) fire(now);

  for (const t of targets) {
    t.x += t.row.speed * t.mult * dt;
    if (wrap(t)) {
      if (!t.alive && Math.random() < 0.5) t.alive = true;  // targets come back round
      else if (t.alive && t.row.kind === "duck" && Math.random() < 0.12 * t.mult) {  // a duck swoops
        divers.push({ row: t.row, x: Math.random() * W, y: t.y, vx: (Math.random() - 0.5) * 120, vy: 60 * t.mult, phase: 0, dive: true });
      }
    }
  }
  for (const d of divers) {
    d.x += d.vx * dt; d.y += d.vy * dt;
    if (d.x < 10 || d.x > W - 10) d.vx = -d.vx;
    if (d.y > GUN_Y - 10 && Math.abs(d.x - gunX) < 24) {
      ammo = Math.max(0, ammo - 5);
      say("-5 AMMO", 900);
      tone(140, 120);
      d.y = H + 99;
    }
  }
  divers = divers.filter((d) => d.y < H);

  for (const b of bullets) {
    b.y -= 420 * dt;
    for (const t of targets.concat(divers)) {
      if (t.alive === false || Math.abs(t.x - b.x) > 12 * S || Math.abs(t.y - b.y) > 12 * S) continue;
      b.y = -99;
      boom(t.x, t.y, t.dive ? lcd.YELLOW : t.row.kind === "owl" ? lcd.ORANGE : lcd.WHITE);
      if (t.dive) { t.y = H + 99; score += 50; ammo += 3; say("+50  +3 AMMO", 900); tone(1400, 60); }
      else { t.alive = false; score += t.row.pts * wave; tone(700 + t.row.pts * 20, 30); }
      break;
    }
  }
  bullets = bullets.filter((b) => b.y > TOP);
  for (const s of sparks) { s.x += s.vx * dt; s.y += s.vy * dt; s.vy += 300 * dt; s.life -= dt; }
  sparks = sparks.filter((s) => s.life > 0);

  if (targets.every((t) => !t.alive)) {  // cleared the board
    wave++;
    ammo += 30;
    say(`WAVE ${wave}  +30 AMMO`, 1600);
    tone(800, 80); setTimeout(() => tone(1200, 120), 100);
    fillRows();
  }
  if (ammo <= 0 && !bullets.length) {
    state = "over";
    if (score > best) { best = score; try { fs.write("/shoot.hi", String(best)); } catch (e) {} }
    tone(200, 300);
  }
}

function fire(now) {
  if (ammo <= 0 || now - lastFire < 160) return;
  lastFire = now;
  ammo--;
  bullets.push({ x: gunX, y: GUN_Y - 16 * S });
  tone(1800, 8);
}
function boom(x, y, c) {
  for (let i = 0; i < 8; i++) {
    const a = Math.random() * 6.28, v = 40 + Math.random() * 90;
    sparks.push({ x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v - 40, r: 1 + Math.random() * 2, c, life: 0.4 + Math.random() * 0.3 });
  }
}

keyboard.on("key", ({ key }) => { if (key === " " && state !== "play") start(); });
joystick.on("press", ({ button }) => { if (button === 0 && state !== "play") start(); });
touch.on("down", ({ x }) => {
  if (state !== "play") return start();
  gunX = x;
  fire(millis());
});

fillRows();
setInterval(frame, 10);
