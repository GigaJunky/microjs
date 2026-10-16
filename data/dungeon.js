// dungeon.js - a first-person dungeon crawl, a tribute to Dungeons of Daggorath (Tandy Color
// Computer, 1982). The CoCo drew its wireframe corridors on a 0.89 MHz 6809, a line at a time;
// here the whole view is redrawn every frame - shaded walls, smooth steps and turns - in
// JavaScript, with the frame rate on screen. Press R for "1982 mode" to compare.
//
// Keys: arrows / WASD (or ; , . / on the Cardputer) walk and turn, Space attacks, M or Enter shows
// the map, R toggles 1982 mode. Gamepad: D-pad walks, button 1 attacks, button 2 map, button 3 1982.
// Touch: left / right third turns, top middle walks, bottom middle attacks.
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
const HUD = SMALL ? 15 : 24, VW = W, VH = H - HUD, CX = VW / 2, CY = VH / 2;
let BAND = 40;
while (VH % BAND) BAND--;  // bands must tile the view exactly
const band = lcd.createSprite(VW, BAND, 8);
band.setTextDatum(lcd.TC);
const HW = VW / 2, HH = VH / 2, MAXD = 7;
const DIRS = [[0, -1], [1, 0], [0, 1], [-1, 0]];
const COMPASS = "NESW";

const CEIL = lcd.color(35, 28, 28), FLOOR = lcd.color(52, 48, 44), FLOOR_LINE = lcd.color(78, 72, 66);
const STONE = [150, 135, 190], WHITE = lcd.WHITE, BLACK = lcd.BLACK;
const TYPES = [
  { name: "spider", hp: 2, dmg: 1, speed: 650, color: lcd.color(170, 95, 40) },
  { name: "viper", hp: 3, dmg: 2, speed: 850, color: lcd.color(60, 200, 60) },
  { name: "blob", hp: 5, dmg: 2, speed: 1300, color: lcd.color(90, 220, 120) },
  { name: "knight", hp: 7, dmg: 3, speed: 1000, color: lcd.color(190, 190, 210) },
  { name: "wraith", hp: 6, dmg: 4, speed: 750, color: lcd.color(170, 170, 255) },
];

// ---- game state
let state = "title", level = 1, score = 0, retro = false, mapMode = false;
let MS, maze, seen, monsters, flasks, exit;
const p = { x: 1, y: 1, dir: 1, hp: 10, maxHp: 10 };
let anim = null, queued = null, nextSwing = 0, swingUntil = 0, hurtUntil = 0;
let exertion = 0, nextBeat = 0, beatAt = 0, lastTick = 0;
let msg = "", msgUntil = 0;

const at = (x, y) => (x < 0 || y < 0 || x >= MS || y >= MS ? 1 : maze[y * MS + x]);
const monsterAt = (x, y) => monsters.find((m) => m.x === x && m.y === y);
function say(text, ms) { msg = text; msgUntil = millis() + (ms || 1600); }

// ---- maze: recursive backtracker on an odd grid (1 = rock), plus a few loops
function makeMaze(size) {
  const m = new Array(size * size).fill(1);
  const stack = [[1, 1]];
  m[size + 1] = 0;
  while (stack.length) {
    const [x, y] = stack[stack.length - 1], opts = [];
    for (const [dx, dy] of DIRS) {
      const nx = x + dx * 2, ny = y + dy * 2;
      if (nx > 0 && ny > 0 && nx < size - 1 && ny < size - 1 && m[ny * size + nx]) opts.push([dx, dy]);
    }
    if (!opts.length) { stack.pop(); continue; }
    const [dx, dy] = opts[Math.floor(Math.random() * opts.length)];
    m[(y + dy) * size + x + dx] = 0;
    m[(y + 2 * dy) * size + x + 2 * dx] = 0;
    stack.push([x + 2 * dx, y + 2 * dy]);
  }
  for (let i = 0; i < size; i++) {  // knock through some walls so there are loops to escape by
    const x = 1 + Math.floor(Math.random() * (size - 2)), y = 1 + Math.floor(Math.random() * (size - 2));
    if (m[y * size + x] && (x % 2) !== (y % 2)) m[y * size + x] = 0;
  }
  return m;
}

function distances(sx, sy) {  // breadth-first walking distance from (sx, sy) to every open cell
  const d = new Array(MS * MS).fill(-1), q = [[sx, sy]];
  d[sy * MS + sx] = 0;
  for (let i = 0; i < q.length; i++) {
    const [x, y] = q[i];
    for (const [dx, dy] of DIRS)
      if (!at(x + dx, y + dy) && d[(y + dy) * MS + x + dx] < 0) {
        d[(y + dy) * MS + x + dx] = d[y * MS + x] + 1;
        q.push([x + dx, y + dy]);
      }
  }
  return d;
}

function newLevel() {
  MS = Math.min(25, 11 + level * 2);
  maze = makeMaze(MS);
  seen = new Array(MS * MS).fill(0);
  Object.assign(p, { x: 1, y: 1, dir: at(2, 1) ? 2 : 1 });
  const d = distances(1, 1), open = [];
  let far = 0;
  for (let i = 0; i < d.length; i++) {
    if (d[i] > d[far]) far = i;
    if (d[i] >= 6) open.push(i);
  }
  exit = { x: far % MS, y: Math.floor(far / MS) };
  const take = () => {
    for (let tries = 0; tries < 50; tries++) {
      const i = open[Math.floor(Math.random() * open.length)], x = i % MS, y = Math.floor(i / MS);
      if ((x !== exit.x || y !== exit.y) && !monsterAt(x, y) && !flasks.some((f) => f.x === x && f.y === y)) return { x, y };
    }
    return null;
  };
  monsters = [];
  flasks = [];
  const kinds = Math.min(TYPES.length, level + 1);
  for (let n = 0; n < Math.min(14, 2 + level * 2); n++) {
    const c = take();
    if (!c) continue;
    const type = TYPES[Math.floor(Math.random() * kinds)];
    monsters.push({ x: c.x, y: c.y, type, hp: type.hp, nextMove: 0, nextAttack: 0, flashUntil: 0, phase: Math.random() * 6 });
  }
  for (let n = 0; n < 2 + Math.floor(level / 2); n++) { const c = take(); if (c) flasks.push(c); }
  say(`LEVEL ${level}`, 2000);
}

function startGame() {
  level = 1; score = 0; p.hp = p.maxHp = 10; exertion = 0;
  anim = queued = null;
  mapMode = false;
  newLevel();
  state = "play";
  lcd.fillScreen(BLACK);
}

// ---- drawing: a display list (built once per frame) replayed into each horizontal band
const OP_RECT = 0, OP_TRI = 1, OP_LINE = 2, OP_CIRC = 3, OP_ELL = 4, OP_RING = 5, OP_BOX = 6, OP_TEXT = 7;
let ops = [];
function rect(x, y, w, h, c) { if (w > 0 && h > 0) ops.push([OP_RECT, y, y + h, x, y, w, h, c]); }
function box(x, y, w, h, c) { if (w > 0 && h > 0) ops.push([OP_BOX, y, y + h, x, y, w, h, c]); }
function tri(x1, y1, x2, y2, x3, y3, c) { ops.push([OP_TRI, Math.min(y1, y2, y3), Math.max(y1, y2, y3), x1, y1, x2, y2, x3, y3, c]); }
function line(x1, y1, x2, y2, c) { ops.push([OP_LINE, Math.min(y1, y2), Math.max(y1, y2), x1, y1, x2, y2, c]); }
function circ(x, y, r, c) { if (r > 0) ops.push([retro ? OP_RING : OP_CIRC, y - r, y + r, x, y, r, retro ? WHITE : c]); }
function ell(x, y, rx, ry, c) { if (rx > 0 && ry > 0) ops.push([OP_ELL, y - ry, y + ry, x, y, rx, ry, c]); }
function text(s, x, y, font, c) { ops.push([OP_TEXT, y, y + (font === 4 ? 26 : font === 2 ? 16 : 8), s, x, y, font, c]); }

function render() {
  for (let top = 0; top < VH; top += BAND) {
    band.fillScreen(BLACK);
    const bottom = top + BAND;
    for (let i = 0; i < ops.length; i++) {
      const o = ops[i];
      if (o[2] < top || o[1] >= bottom) continue;  // not in this band
      switch (o[0]) {
        case OP_RECT: band.fillRect(o[3], o[4] - top, o[5], o[6], o[7]); break;
        case OP_TRI: band.fillTriangle(o[3], o[4] - top, o[5], o[6] - top, o[7], o[8] - top, o[9]); break;
        case OP_LINE: band.drawLine(o[3], o[4] - top, o[5], o[6] - top, o[7]); break;
        case OP_CIRC: band.fillCircle(o[3], o[4] - top, o[5], o[6]); break;
        case OP_ELL: band.fillEllipse(o[3], o[4] - top, o[5], o[6], o[7]); break;
        case OP_RING: band.drawCircle(o[3], o[4] - top, o[5], o[6]); break;
        case OP_BOX: band.drawRect(o[3], o[4] - top, o[5], o[6], o[7]); break;
        case OP_TEXT: band.setTextColor(o[7]); band.drawString(o[3], o[4], o[5] - top, o[6]); break;
      }
    }
    band.push(0, top);
  }
}

// perspective: the corridor edge at depth z (0 = the screen edge, 1 = one cell ahead, ...)
const scale = (z) => 1 / (1 + z * 0.9);
const shades = new Map();
function shade(z, side) {
  const key = Math.round(Math.max(0, z) * 4) * 2 + (side ? 1 : 0);
  let c = shades.get(key);
  if (c === undefined) {
    const f = (side ? 0.75 : 1) / (1 + Math.max(0, z) * 0.45);
    c = lcd.color(Math.round(STONE[0] * f), Math.round(STONE[1] * f), Math.round(STONE[2] * f));
    shades.set(key, c);
  }
  return c;
}

function face(x1, y1, x2, y2, c) {  // a wall seen head-on
  if (retro) return box(x1, y1, x2 - x1, y2 - y1, WHITE);
  rect(x1, y1, x2 - x1, y2 - y1, c);
  box(x1, y1, x2 - x1, y2 - y1, BLACK);
}
function side(xa, ta, ba, xb, tb, bb, c) {  // a side wall from the near edge (a) to the far edge (b)
  if (retro) {
    line(xa, ta, xb, tb, WHITE); line(xa, ba, xb, bb, WHITE); line(xb, tb, xb, bb, WHITE);
    return;
  }
  tri(xa, ta, xb, tb, xb, bb, c);
  tri(xa, ta, xb, bb, xa, ba, c);
  line(xb, tb, xb, bb, BLACK);
}

// Everything seen from cell (px, py) facing dir; t = how far through a step forward (0..1),
// xs = horizontal shift (for turning: the old and new views slide across).
function view(px, py, dir, t, xs) {
  const [fx, fy] = DIRS[dir], [lx, ly] = DIRS[(dir + 3) % 4], [rx, ry] = DIRS[(dir + 1) % 4];
  const now = millis();
  let kmax = MAXD - 1;
  for (let k = 0; k < MAXD - 1; k++) if (at(px + fx * (k + 1), py + fy * (k + 1))) { kmax = k; break; }
  if (!retro) {
    rect(xs, 0, VW, CY, CEIL);
    rect(xs, CY, VW, VH - CY, FLOOR);
  }
  for (let k = kmax; k >= 0; k--) {
    const cx = px + fx * k, cy = py + fy * k;
    const sn = scale(k - t), sf = scale(k + 1 - t);
    const nl = CX + xs - HW * sn, nr = CX + xs + HW * sn, nt = CY - HH * sn, nb = CY + HH * sn;
    const fl = CX + xs - HW * sf, fr = CX + xs + HW * sf, ft = CY - HH * sf, fb = CY + HH * sf;
    if (!t && !xs) seen[cy * MS + cx] = 1;
    if (!retro) line(fl, fb, fr, fb, FLOOR_LINE);
    if (k === kmax && at(cx + fx, cy + fy)) face(fl, ft, fr, fb, shade(k + 1 - t, false));
    // left: a wall, or an opening showing the far wall of the side passage
    if (at(cx + lx, cy + ly)) side(nl, nt, nb, fl, ft, fb, shade(k + 0.5 - t, true));
    else if (at(cx + lx + fx, cy + ly + fy)) face(Math.max(nl, fl - (fr - fl)), ft, fl, fb, shade(k + 1 - t, false));
    if (at(cx + rx, cy + ry)) side(nr, nt, nb, fr, ft, fb, shade(k + 0.5 - t, true));
    else if (at(cx + rx + fx, cy + ry + fy)) face(fr, ft, Math.min(nr, fr + (fr - fl)), fb, shade(k + 1 - t, false));
    if (k === 0) continue;
    if (!t && !xs) { seen[(cy + ly) * MS + cx + lx] = 1; seen[(cy + ry) * MS + cx + rx] = 1; }
    // things in this cell, standing on the floor at the cell's middle
    const z = k + 0.5 - t, u = HH * scale(z), sx = CX + xs, floorY = CY + u;
    if (exit.x === cx && exit.y === cy) {
      ell(sx, floorY - u * 0.12, u * 0.5, u * 0.12, retro ? WHITE : lcd.ORANGE);
      ell(sx, floorY - u * 0.12, u * 0.44, u * 0.09, BLACK);
    }
    if (flasks.some((f) => f.x === cx && f.y === cy)) {
      rect(sx - u * 0.07, floorY - u * 0.32, u * 0.14, u * 0.24, retro ? WHITE : lcd.CYAN);
      rect(sx - u * 0.03, floorY - u * 0.42, u * 0.06, u * 0.1, retro ? WHITE : lcd.LIGHTGREY);
    }
    const m = monsterAt(cx, cy);
    if (m) drawMonster(m, sx, floorY, u * 1.6, now);  // monsters loom larger than the perspective says
  }
}

function drawMonster(m, sx, floorY, u, now) {
  const col = retro || m.flashUntil > now ? WHITE : m.type.color;
  const bob = Math.sin(now / 180 + m.phase) * u * 0.04;
  const eye = retro ? WHITE : lcd.RED;
  switch (m.type.name) {
    case "spider": {
      const by = floorY - u * 0.32 + bob;
      for (let i = 0; i < 4; i++)
        for (const s of [-1, 1]) {
          const kx = sx + s * u * (0.32 + 0.12 * i), ky = by - u * (0.22 - 0.12 * i);
          line(sx + s * u * 0.12, by, kx, ky, col);
          line(kx, ky, sx + s * u * (0.42 + 0.14 * i), floorY, col);
        }
      if (retro) circ(sx, by, u * 0.24, col); else ell(sx, by, u * 0.28, u * 0.2, col);
      circ(sx, by - u * 0.24, u * 0.13, col);
      circ(sx - u * 0.05, by - u * 0.26, u * 0.03, eye);
      circ(sx + u * 0.05, by - u * 0.26, u * 0.03, eye);
      break;
    }
    case "viper": {
      let px = sx, py = floorY;
      for (let i = 1; i <= 6; i++) {
        const nx = sx + (i % 2 ? 1 : -1) * u * 0.2 * (1 - i / 9), ny = floorY - u * 0.15 * i + bob;
        line(px, py, nx, ny, col);
        line(px + 1, py, nx + 1, ny, col);
        line(px + 2, py, nx + 2, ny, col);
        px = nx; py = ny;
      }
      if (retro) circ(px, py - u * 0.06, u * 0.1, col);
      else tri(px - u * 0.12, py, px + u * 0.12, py, px, py - u * 0.16, col);
      circ(px, py - u * 0.05, u * 0.025, eye);
      break;
    }
    case "blob": {
      const by = floorY - u * 0.4 + bob;
      if (retro) circ(sx, by, u * 0.45, col); else ell(sx, by, u * 0.58, u * 0.4, col);
      for (const s of [-1, 1]) {
        circ(sx + s * u * 0.18, by - u * 0.1, u * 0.09, WHITE);
        if (!retro) circ(sx + s * u * 0.18, by - u * 0.08, u * 0.04, BLACK);
      }
      break;
    }
    case "knight": {
      const top = floorY - u * 1.2 + bob;
      line(sx - u * 0.1, floorY, sx - u * 0.08, top + u * 0.75, col);
      line(sx + u * 0.1, floorY, sx + u * 0.08, top + u * 0.75, col);
      if (retro) box(sx - u * 0.17, top + u * 0.25, u * 0.34, u * 0.5, col);
      else rect(sx - u * 0.17, top + u * 0.25, u * 0.34, u * 0.5, col);
      circ(sx, top + u * 0.13, u * 0.12, col);
      circ(sx - u * 0.24, top + u * 0.5, u * 0.14, retro ? WHITE : lcd.color(140, 40, 40));  // shield
      line(sx + u * 0.2, top + u * 0.55, sx + u * 0.42, top - u * 0.05, WHITE);  // sword
      break;
    }
    default: {  // wraith
      const top = floorY - u * 1.25 + bob * 3;
      if (retro) {
        line(sx, top, sx - u * 0.42, floorY - u * 0.1, col);
        line(sx, top, sx + u * 0.42, floorY - u * 0.1, col);
      } else tri(sx, top, sx - u * 0.42, floorY - u * 0.1, sx + u * 0.42, floorY - u * 0.1, col);
      circ(sx, top + u * 0.12, u * 0.16, col);
      circ(sx - u * 0.06, top + u * 0.1, u * 0.035, BLACK);
      circ(sx + u * 0.06, top + u * 0.1, u * 0.035, BLACK);
    }
  }
}

function drawMap() {
  const cs = Math.max(2, Math.floor(Math.min(VW, VH - 18) / MS));
  const ox = Math.floor((VW - cs * MS) / 2), oy = 16 + Math.floor((VH - 16 - cs * MS) / 2);
  text(`MAP - level ${level}`, CX, 2, 2, lcd.YELLOW);
  for (let y = 0; y < MS; y++)
    for (let x = 0; x < MS; x++) {
      if (!seen[y * MS + x]) continue;
      rect(ox + x * cs, oy + y * cs, cs, cs, maze[y * MS + x] ? lcd.color(110, 100, 150) : lcd.color(70, 70, 80));
    }
  if (seen[exit.y * MS + exit.x]) rect(ox + exit.x * cs, oy + exit.y * cs, cs, cs, lcd.ORANGE);
  for (const f of flasks) if (seen[f.y * MS + f.x]) rect(ox + f.x * cs + cs / 4, oy + f.y * cs + cs / 4, cs / 2, cs / 2, lcd.CYAN);
  const [dx, dy] = DIRS[p.dir], mx = ox + p.x * cs + cs / 2, my = oy + p.y * cs + cs / 2;
  circ(mx, my, Math.max(1, cs / 2 - 1), lcd.GREEN);
  line(mx, my, mx + dx * cs, my + dy * cs, lcd.GREEN);
}

// ---- actions
function act(a) {
  if (state !== "play") {
    if (a === "attack" || a === "map") startGame();
    return;
  }
  if (a === "retro") {
    retro = !retro;
    say(retro ? "1982 MODE: wireframe, about 2 frames a second" : "NOW: shaded and smooth", 2500);
    return;
  }
  if (a === "map") { mapMode = !mapMode; return; }
  if (anim) { queued = a; return; }  // finish the step or turn first
  const now = millis(), [dx, dy] = DIRS[p.dir];
  const slow = retro ? 1 : 0;
  if (a === "forward" || a === "back") {
    const s = a === "forward" ? 1 : -1, tx = p.x + dx * s, ty = p.y + dy * s;
    if (at(tx, ty) || monsterAt(tx, ty)) { tone(110, 40); return; }
    exertion += 0.6;
    anim = { kind: "move", back: s < 0, fromX: p.x, fromY: p.y, toX: tx, toY: ty, start: now, dur: slow ? 1 : 170 };
  } else if (a === "left" || a === "right") {
    const s = a === "right" ? 1 : -1;
    anim = { kind: "turn", sign: s, from: p.dir, to: (p.dir + s + 4) % 4, start: now, dur: slow ? 1 : 150 };
  } else if (a === "attack") {
    if (now < nextSwing) return;
    nextSwing = now + 420;
    swingUntil = now + 140;
    exertion += 1.2;
    const m = monsterAt(p.x + dx, p.y + dy);
    if (!m) { tone(260, 15); return; }
    if (Math.random() < 0.8) {
      m.hp -= Math.random() < 0.25 ? 2 : 1;
      m.flashUntil = now + 160;
      tone(650, 30);
      if (m.hp <= 0) {
        monsters.splice(monsters.indexOf(m), 1);
        score += m.type.hp * 10;
        say(`The ${m.type.name} dies!`);
        tone(1300, 70);
      }
    } else { say("You miss."); tone(180, 25); }
  }
}

function finishAnim() {
  const a = anim;
  anim = null;
  if (a.kind === "turn") p.dir = a.to;
  else {
    p.x = a.toX; p.y = a.toY;
    const f = flasks.findIndex((q) => q.x === p.x && q.y === p.y);
    if (f >= 0) {
      flasks.splice(f, 1);
      p.hp = Math.min(p.maxHp, p.hp + 4);
      say("You drink a flask. You feel better.");
      tone(900, 60); setTimeout(() => tone(1200, 80), 80);
    }
    if (p.x === exit.x && p.y === exit.y) {
      level++;
      score += 100;
      tone(500, 80); setTimeout(() => tone(350, 120), 100);
      newLevel();
    }
  }
  if (queued) { const q = queued; queued = null; act(q); }
}

// ---- monsters and the heart
function updateMonsters(now) {
  for (const m of monsters) {
    const dist = Math.abs(m.x - p.x) + Math.abs(m.y - p.y);
    if (dist === 1) {
      if (now >= m.nextAttack) {
        m.nextAttack = now + m.type.speed * 1.4;
        if (m.nextMove === 0) continue;  // a moment's grace when it first arrives
        if (Math.random() < 0.65) {
          p.hp -= m.type.dmg;
          hurtUntil = now + 220;
          exertion += 2;
          const rel = (((Math.atan2(m.y - p.y, m.x - p.x) * 2) / Math.PI + 5 - p.dir) % 4 + 4) % 4;
          say(`The ${m.type.name} hits you${["", " from the right", " from behind", " from the left"][Math.round(rel) % 4]}!`);
          tone(90, 140);
          if (p.hp <= 0) { state = "dead"; say("", 1); return; }
        } else say(`The ${m.type.name} misses.`);
      }
      m.nextMove = now + m.type.speed;
      continue;
    }
    if (now < m.nextMove) continue;
    m.nextMove = now + m.type.speed * (0.8 + Math.random() * 0.4);
    m.nextAttack = now + 500;
    const opts = DIRS.filter(([dx, dy]) => {
      const x = m.x + dx, y = m.y + dy;
      return !at(x, y) && !monsterAt(x, y) && !(x === p.x && y === p.y) && !(anim && anim.kind === "move" && x === anim.toX && y === anim.toY);
    });
    if (!opts.length) continue;
    let pick = opts[Math.floor(Math.random() * opts.length)];
    if (dist <= 7) {  // close: step towards the player
      let best = 1e9;
      for (const o of opts) {
        const d = Math.abs(m.x + o[0] - p.x) + Math.abs(m.y + o[1] - p.y) + Math.random() * 0.5;
        if (d < best) { best = d; pick = o; }
      }
    }
    m.x += pick[0];
    m.y += pick[1];
  }
}

function heartbeat(now) {
  exertion = Math.max(0, exertion - (now - lastTick) / 1000);
  lastTick = now;
  if (now < nextBeat) return;
  const interval = Math.max(250, 1150 - exertion * 70 - (p.maxHp - p.hp) * 60);
  nextBeat = now + interval;
  beatAt = now;
  tone(70, 35);
  setTimeout(() => tone(52, 40), Math.min(140, interval / 3));
}

// ---- HUD (drawn straight to the screen, below the view)
let frames = 0, fps = 0, fpsAt = 0, hudKey = "";
function hud(now) {
  const beat = now - beatAt < 140;
  const key = `${p.hp}|${level}|${score}|${p.dir}|${fps}|${beat}|${retro}|${state}`;
  if (key === hudKey) return;
  hudKey = key;
  const y = VH, f = SMALL ? 1 : 2;
  lcd.fillRect(0, y, W, HUD, lcd.color(20, 16, 30));
  const hx = 12, hy = y + HUD / 2, r = (beat ? 1.25 : 1) * (SMALL ? 3 : 5);  // the heart
  lcd.fillCircle(hx - r * 0.6, hy - r * 0.3, r * 0.7, lcd.RED);
  lcd.fillCircle(hx + r * 0.6, hy - r * 0.3, r * 0.7, lcd.RED);
  lcd.fillTriangle(hx - r * 1.3, hy - r * 0.1, hx + r * 1.3, hy - r * 0.1, hx, hy + r * 1.2, lcd.RED);
  const bw = SMALL ? 40 : 70, bx = 26;
  lcd.drawRect(bx, hy - 4, bw, 8, lcd.DARKGREY);
  lcd.fillRect(bx + 1, hy - 3, Math.max(0, (bw - 2) * p.hp / p.maxHp), 6, p.hp > 3 ? lcd.GREEN : lcd.RED);
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString(`L${level} ${COMPASS[p.dir]}  ${score}`, bx + bw + 8, hy, f);
  lcd.setTextDatum(lcd.MR);
  lcd.setTextColor(retro ? lcd.ORANGE : lcd.CYAN);
  lcd.drawString(retro ? "1982 mode" : `${fps} fps`, W - 4, hy, f);
  lcd.setTextDatum(lcd.TL);
}

// ---- main loop
let lastDraw = 0, titleT = 0;
function frame() {
  const now = millis();
  if (state === "play") { updateMonsters(now); heartbeat(now); }
  let t = 0;
  if (anim) {
    t = Math.min(1, (now - anim.start) / anim.dur);
    if (t >= 1) { finishAnim(); t = 0; }
  }
  if (retro && now - lastDraw < 480) return;  // the 1982 pace
  lastDraw = now;
  ops = [];
  if (state === "title") {
    titleT = (titleT + 0.03) % 1;  // walk forever down the demo corridor
    view(p.x, p.y, p.dir, titleT, 0);
    rect(0, VH * 0.18 - 4, VW, SMALL ? 52 : 84, BLACK);
    text("DUNGEON CRAWL", CX, VH * 0.18, SMALL ? 2 : 4, lcd.ORANGE);
    text("for Dungeons of Daggorath (CoCo, 1982)", CX, VH * 0.18 + (SMALL ? 18 : 30), 1, lcd.LIGHTGREY);
    text(`6809 @ 0.89 MHz: wireframe, ~1 view/s`, CX, VH * 0.18 + (SMALL ? 28 : 44), 1, lcd.DARKGREY);
    text(`ESP32 @ 240 MHz in JS: shaded, ${fps} fps`, CX, VH * 0.18 + (SMALL ? 38 : 56), 1, lcd.CYAN);
    text("Space / button 1 to start", CX, VH - (SMALL ? 12 : 18), SMALL ? 1 : 2, lcd.YELLOW);
  } else if (mapMode) {
    drawMap();
  } else {
    if (!anim) view(p.x, p.y, p.dir, 0, 0);
    else if (anim.kind === "move") {
      if (anim.back) view(anim.toX, anim.toY, p.dir, 1 - t, 0);
      else view(anim.fromX, anim.fromY, p.dir, t, 0);
    } else {
      view(p.x, p.y, anim.from, 0, -anim.sign * t * VW);
      view(p.x, p.y, anim.to, 0, anim.sign * (1 - t) * VW);
    }
    if (swingUntil > now) {  // the sword swing
      line(VW * 0.85, VH, VW * 0.45, VH * 0.35, WHITE);
      line(VW * 0.86, VH, VW * 0.46, VH * 0.35, WHITE);
    }
    if (hurtUntil > now) { box(0, 0, VW, VH, lcd.RED); box(1, 1, VW - 2, VH - 2, lcd.RED); box(2, 2, VW - 4, VH - 4, lcd.RED); }
    if (state === "dead") {
      rect(0, CY - 30, VW, 60, BLACK);
      text("YOU HAVE DIED", CX, CY - 22, SMALL ? 2 : 4, lcd.RED);
      text(`level ${level}, score ${score} - Space to play again`, CX, CY + 10, 1, lcd.LIGHTGREY);
    }
  }
  if (msgUntil > now && msg) {
    rect(0, 0, VW, SMALL ? 12 : 18, BLACK);
    text(msg, CX, SMALL ? 2 : 1, SMALL ? 1 : 2, WHITE);
  }
  render();
  frames++;
  if (now - fpsAt >= 1000) { fps = frames; frames = 0; fpsAt = now; }
  hud(now);
}

// ---- input
const KEYS = {
  ArrowUp: "forward", w: "forward", ";": "forward",
  ArrowDown: "back", s: "back", ".": "back",
  ArrowLeft: "left", a: "left", ",": "left",
  ArrowRight: "right", d: "right", "/": "right",
  " ": "attack", Enter: "map", m: "map", r: "retro",
};
keyboard.on("key", ({ key }) => { const a = KEYS[key] || KEYS[key.toLowerCase()]; if (a) act(a); });
joystick.on("press", ({ button }) => (button === 2 ? act("retro") : null));  // 1 and 2 arrive as Space / Enter
touch.on("down", ({ x, y }) => {
  if (state !== "play") return act("attack");
  if (y >= VH) return act("map");
  if (x < W / 3) act("left");
  else if (x > (W * 2) / 3) act("right");
  else act(y < VH * 0.6 ? "forward" : "attack");
});

// title screen: a demo corridor
MS = 15; maze = makeMaze(MS); seen = new Array(MS * MS).fill(0);
monsters = []; flasks = []; exit = { x: -1, y: -1 };
p.x = 1; p.y = 1; p.dir = at(2, 1) ? 2 : 1;
lcd.fillScreen(BLACK);
setInterval(frame, 15);
