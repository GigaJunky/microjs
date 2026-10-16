// Demo: a little demoscene show - plasma, a starfield, a spinning shaded cube, and copper bars
// with a wavy scroller - to an original chiptune played on the speaker (one voice, switching
// quickly between bass and arpeggio, as 8-bit computers did). Effects change every 12 seconds.
// Tap or Space: next effect. m: music on / off. If /sd/demo.mp3 exists (on a board that plays
// music), that plays instead of the chiptune.
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200;
let BAND = 40;
while (H % BAND) BAND--;
const band = lcd.createSprite(W, BAND, board.lowMemory ? 8 : 16);  // 8-bit colour fits a small board
const TAU = Math.PI * 2;

// sine table: SIN[i & 255] = sin(i / 256 turn) * 127
const SIN = [];
for (let i = 0; i < 256; i++) SIN.push(Math.round(Math.sin((i / 256) * TAU) * 127));
const s8 = (i) => SIN[i & 255];

// a palette that runs round the colour wheel, for the plasma
const PAL = [];
for (let i = 0; i < 64; i++) {
  const a = (i / 64) * TAU;
  PAL.push(lcd.color(128 + 127 * Math.sin(a), 128 + 127 * Math.sin(a + 2.1), 128 + 127 * Math.sin(a + 4.2)));
}

// ---------------------------------------------------------------- effects
// Each has prepare(t) (once a frame) and band(top, t) (draw rows top .. top + BAND).
const CELL = SMALL ? 12 : 16;
// each cell's fixed part, worked out once: its four phases packed into one number (a byte each),
// in one flat list by row - small enough for a board without PSRAM
const COLS_N = Math.ceil(W / CELL), ROWS_N = Math.ceil(H / CELL), CELLS = [];
for (let cy = 0; cy < ROWS_N; cy++)
  for (let cx = 0; cx < COLS_N; cx++)
    CELLS.push(((cx * 9) & 255) | (((cy * 11) & 255) << 8) | ((((cx + cy) * 6) & 255) << 16) | (((Math.sqrt(cx * cx + cy * cy) * 12) & 255) * 16777216));
const plasma = {
  name: "plasma",
  band(top, t) {
    const T = t >> 4, T2 = T * 2, T3 = T * 3, last = Math.min(ROWS_N - 1, ((top + BAND - 1) / CELL) | 0);
    for (let r = (top / CELL) | 0; r <= last; r++) {  // only the rows in this band
      const y = r * CELL - top;
      for (let cx = 0, i = r * COLS_N; cx < COLS_N; cx++, i++) {
        const p = CELLS[i];
        const v = SIN[((p & 255) + T) & 255] + SIN[(((p >> 8) & 255) - T2) & 255] + SIN[(((p >> 16) & 255) + T3) & 255] + SIN[(((p / 16777216) | 0) - T2) & 255];
        band.fillRect(cx * CELL, y, CELL, CELL, PAL[((v >> 3) + T) & 63]);
      }
    }
  },
};

const STARS = [];
for (let i = 0; i < (SMALL ? 70 : 140); i++) STARS.push({ x: Math.random() * 2 - 1, y: Math.random() * 2 - 1, z: Math.random() });
const stars = {
  name: "starfield",
  pts: [],
  prepare(t, dt) {
    this.pts = [];
    const sp = dt * 0.0006, tw = Math.sin(t / 1500) * 0.4;
    for (const s of STARS) {
      s.z -= sp;
      if (s.z <= 0.02) { s.z += 1; s.x = Math.random() * 2 - 1; s.y = Math.random() * 2 - 1; }
      const px = W / 2 + ((s.x + tw * s.y) / s.z) * (W / 4), py = H / 2 + ((s.y - tw * s.x) / s.z) * (H / 4);
      if (px < 0 || py < 0 || px >= W || py >= H) continue;
      const b = Math.min(255, Math.round(320 * (1 - s.z)));
      this.pts.push([px, py, s.z < 0.3 ? 3 : s.z < 0.6 ? 2 : 1, lcd.color(b, b, Math.min(255, b + 40))]);
    }
  },
  band(top) {
    band.fillRect(0, 0, W, BAND, lcd.color(0, 0, 18));
    for (const p of this.pts) if (p[1] >= top - 3 && p[1] < top + BAND) band.fillRect(p[0], p[1] - top, p[2], p[2], p[3]);
  },
};

const CUBE_V = [[-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1], [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1]];
const CUBE_F = [[0, 1, 2, 3, lcd.RED], [5, 4, 7, 6, lcd.CYAN], [4, 0, 3, 7, lcd.YELLOW], [1, 5, 6, 2, lcd.MAGENTA], [4, 5, 1, 0, lcd.GREEN], [3, 2, 6, 7, lcd.BLUE]];
const shadeOf = (c, k) => lcd.color(((c >> 11) * 8 * k) | 0, (((c >> 5) & 63) * 4 * k) | 0, ((c & 31) * 8 * k) | 0);
const cube = {
  name: "cube",
  faces: [],
  prepare(t) {
    const a = t / 900, b = t / 1300, c = t / 2100, S = Math.min(W, H) * 0.24 * (1 + 0.15 * Math.sin(t / 600));
    const [sa, ca, sb, cb, sc, cc] = [Math.sin(a), Math.cos(a), Math.sin(b), Math.cos(b), Math.sin(c), Math.cos(c)];
    const P = CUBE_V.map(([x, y, z]) => {
      let y1 = y * ca - z * sa, z1 = y * sa + z * ca;          // about x
      let x2 = x * cb + z1 * sb, z2 = -x * sb + z1 * cb;       // about y
      let x3 = x2 * cc - y1 * sc, y3 = x2 * sc + y1 * cc;      // about z
      const f = 3.2 / (z2 + 4.2);
      return [W / 2 + x3 * S * f, H / 2 + y3 * S * f, z2];
    });
    this.faces = [];
    for (const [i, j, k, l, col] of CUBE_F) {
      const [ax, ay] = P[i], [bx, by] = P[j], [cx, cy] = P[k];
      const cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
      if (cross >= 0) continue;  // facing away
      const light = Math.min(1, 0.35 + Math.abs(cross) / (S * S * 3.2));
      this.faces.push({ p: [P[i], P[j], P[k], P[l]], c: shadeOf(col, light), z: P[i][2] + P[j][2] + P[k][2] + P[l][2] });
    }
    this.faces.sort((u, v) => v.z - u.z);
  },
  band(top, t) {
    for (let y = 0; y < BAND; y += 4) {  // a slow gradient behind
      const k = (top + y + (t >> 5)) & 255;
      band.fillRect(0, y, W, 4, lcd.color(20 + (s8(k) >> 3), 0, 40 + (s8(k + 64) >> 3)));
    }
    for (const f of this.faces) {
      const [p0, p1, p2, p3] = f.p;
      band.fillTriangle(p0[0], p0[1] - top, p1[0], p1[1] - top, p2[0], p2[1] - top, f.c);
      band.fillTriangle(p0[0], p0[1] - top, p2[0], p2[1] - top, p3[0], p3[1] - top, f.c);
    }
  },
};

const MESSAGE = "    micro-js  *  JavaScript on a 240 MHz microcontroller  *  plasma, stars and a cube in a few hundred lines  *  greetings to everyone who ever typed RUN on a CoCo  *  ";
const BARS = [lcd.RED, lcd.ORANGE, lcd.YELLOW, lcd.GREEN, lcd.CYAN, lcd.BLUE, lcd.MAGENTA];
const scroller = {
  name: "scroller",
  charW: 0, font: SMALL ? 2 : 4,
  prepare(t) {
    this.bars = BARS.map((c, i) => [H / 2 + Math.sin(t / 700 + i * 0.6) * H * 0.35, c]);
    this.offset = (t / (SMALL ? 14 : 9)) | 0;
    if (!this.charW) this.charW = SMALL ? 12 : 22;
  },
  band(top, t) {
    band.fillRect(0, 0, W, BAND, lcd.BLACK);
    const BH = SMALL ? 10 : 16;
    for (const [y, c] of this.bars) {  // copper bars: bright middle, dark edges
      if (y + BH < top || y - BH > top + BAND) continue;
      for (let i = -BH; i < BH; i += 2) {
        const k = 1 - Math.abs(i) / BH;
        band.fillRect(0, y + i - top, W, 2, shadeOf(c, 0.25 + 0.75 * k));
      }
    }
    const cw = this.charW, first = (this.offset / cw) | 0, shift = this.offset % cw, baseY = H * 0.5;
    band.setTextDatum(lcd.MC);
    for (let i = 0; i <= W / cw + 1; i++) {
      const ch = MESSAGE[(first + i) % MESSAGE.length], x = i * cw - shift + cw / 2;
      const y = baseY + s8((x >> 1) + (t >> 3)) * (H / 600);
      if (y + 16 < top || y - 16 > top + BAND || ch === " ") continue;
      band.setTextColor(lcd.BLACK);
      band.drawString(ch, x + 2, y - top + 2, this.font);
      band.setTextColor(PAL[((x >> 3) + (t >> 4)) & 63]);
      band.drawString(ch, x, y - top, this.font);
    }
  },
};

const EFFECTS = [plasma, stars, cube, scroller];
let fx = 0, fxStart = millis(), last = millis(), frames = 0, fps = 0, fpsAt = millis();

function frame() {
  const t = millis(), dt = Math.min(60, t - last);
  last = t;
  if (t - fxStart > 12000) next();
  const e = EFFECTS[fx];
  if (e.prepare) e.prepare(t, dt);
  for (let top = 0; top < H; top += BAND) {
    e.band(top, t);
    if (top === 0) {  // caption in the first band
      band.setTextDatum(lcd.TL);
      band.setTextColor(lcd.WHITE);
      band.drawString(`micro-js demo - ${e.name}`, 4, 3, 1);
      band.setTextDatum(lcd.TR);
      band.setTextColor(lcd.LIGHTGREY);
      band.drawString(`${fps} fps`, W - 4, 3, 1);
    }
    band.push(0, top);
  }
  frames++;
  if (t - fpsAt >= 1000) { fps = frames; frames = 0; fpsAt = t; }
}
function next() { fx = (fx + 1) % EFFECTS.length; fxStart = millis(); }

// ---------------------------------------------------------------- music
// An original tune in A minor: four bars a chord (Am F C G), sixteen steps a bar. Each step plays
// the melody if there is a note, else the chord as a fast arpeggio; the bass sounds at the start of
// every other step. One voice, switched quickly, sounds like three.
const freq = (n) => 440 * Math.pow(2, (n - 69) / 12);
const CHORDS = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]];  // Am F C G
const MELODY = [
  [76, 0, 0, 74, 72, 0, 74, 0, 76, 0, 79, 0, 76, 74, 72, 0],
  [77, 0, 0, 76, 74, 0, 72, 0, 69, 0, 72, 0, 74, 0, 0, 0],
  [72, 0, 76, 0, 79, 0, 76, 0, 84, 0, 0, 83, 79, 0, 76, 0],
  [74, 0, 0, 72, 71, 0, 74, 0, 79, 0, 0, 0, 83, 0, 81, 79],
];
let step = 0, musicOn = true, mp3 = false;
const STEP_MS = 115;
function musicStep() {
  if (!musicOn || mp3) return;
  const bar = (step >> 4) & 3, s = step & 15, chord = CHORDS[bar], m = MELODY[bar][s];
  if (s % 2 === 0) {  // bass, then the melody or an arpeggio note
    tone(freq(chord[0] - 12), 32);
    setTimeout(() => tone(freq(m || chord[(step >> 1) % 3] + 12), m ? 70 : 40), 36);
  } else if (m) tone(freq(m), 90);
  else tone(freq(chord[step % 3] + 24), 30);
  step++;
}
if (board.hasMusic && fs.exists("/sd/demo.mp3") && audio.play("/sd/demo.mp3") === "") mp3 = true;
setInterval(musicStep, STEP_MS);
setInterval(() => { if (mp3 && !audio.status().playing) audio.play("/sd/demo.mp3"); }, 1000);  // loop the mp3

// ---------------------------------------------------------------- input
touch.on("down", () => next());
keyboard.on("key", ({ key }) => {
  if (key === " " || key === "Enter" || key === "ArrowRight" || key === "/") next();
  else if (key === "m") { musicOn = !musicOn; if (mp3) audio.pause(!musicOn); }
});
setInterval(frame, 5);
