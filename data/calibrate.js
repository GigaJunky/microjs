// Touch calibration: touch the centre of each target (a stylus or fingertip, held still for a
// moment). Then try it out - a dot follows your touch - and Save, or Retry, or Reset to the board's
// built-in setting. Saved to /touchcal.json, which the firmware loads at every start.
const W = lcd.width(), H = lcd.height(), M = 24;  // targets this far in from the edges
const TARGETS = [[M, M], [W - 1 - M, M], [W - 1 - M, H - 1 - M], [M, H - 1 - M], [W >> 1, H >> 1]];
const BG = lcd.BLACK, FG = lcd.WHITE, HOT = lcd.color(255, 140, 40);
const before = touch.calibrate();  // put back if we leave without saving
let step = 0, samples = [], gone = 0, raw = [], mode = "targets", saved = false;

function centreText(lines, y) {
  lcd.setTextDatum(lcd.MC);
  lines.forEach((t, i) => { lcd.setTextColor(i ? lcd.LIGHTGREY : FG); lcd.drawString(t, W / 2, y + i * 20, 2); });
  lcd.setTextDatum(lcd.TL);
}
function drawTarget() {
  lcd.fillScreen(BG);
  centreText([`Touch the target  (${step + 1} of ${TARGETS.length})`, "hold still a moment, then lift"], H / 2 + (step === 4 ? 40 : 0));
  const [x, y] = TARGETS[step];
  lcd.drawCircle(x, y, 10, HOT);
  lcd.drawCircle(x, y, 4, HOT);
  lcd.drawFastHLine(x - 16, y, 33, FG);
  lcd.drawFastVLine(x, y - 16, 33, FG);
}
const median = (a) => { const s = a.slice().sort((p, q) => p - q); return s[s.length >> 1]; };

// least squares: screen = a*rx + b*ry + c, solved for x and y separately
function solve(pts, k) {
  const A = [[0, 0, 0], [0, 0, 0], [0, 0, 0]], v = [0, 0, 0];
  for (const p of pts) {
    const r = [p.rx, p.ry, 1];
    for (let i = 0; i < 3; i++) { v[i] += r[i] * p[k]; for (let j = 0; j < 3; j++) A[i][j] += r[i] * r[j]; }
  }
  for (let c = 0; c < 3; c++) {  // Gaussian elimination with pivoting
    let piv = c;
    for (let r = c + 1; r < 3; r++) if (Math.abs(A[r][c]) > Math.abs(A[piv][c])) piv = r;
    [A[c], A[piv]] = [A[piv], A[c]];
    [v[c], v[piv]] = [v[piv], v[c]];
    if (Math.abs(A[c][c]) < 1e-9) return null;
    for (let r = 0; r < 3; r++) {
      if (r === c) continue;
      const f = A[r][c] / A[c][c];
      for (let j = c; j < 3; j++) A[r][j] -= f * A[c][j];
      v[r] -= f * v[c];
    }
  }
  return [v[0] / A[0][0], v[1] / A[1][1], v[2] / A[2][2]];
}

function finish() {
  const pts = raw.map((r, i) => ({ rx: r[0], ry: r[1], x: TARGETS[i][0], y: TARGETS[i][1] }));
  const mx = solve(pts, "x"), my = solve(pts, "y");
  let worst = 999;
  if (mx && my) {
    worst = 0;
    for (const p of pts) worst = Math.max(worst, Math.hypot(mx[0] * p.rx + mx[1] * p.ry + mx[2] - p.x, my[0] * p.rx + my[1] * p.ry + my[2] - p.y));
  }
  if (worst > 30) {  // the touches don't fit a straight mapping: a slip, probably
    lcd.fillScreen(BG);
    centreText(["That didn't line up - let's try again.", "Touch each target's centre."], H / 2 - 10);
    tone(200, 200);
    return setTimeout(restart, 1800);
  }
  touch.calibrate([...mx, ...my], lcd.getRotation());
  mode = "test";
  drawTest(`Calibrated (within ${Math.round(worst)} px). Try it:`);
}

// ---- test screen with buttons
const BH = 34, BY = H - BH - 4, BUTTONS = [["Save", lcd.DARKGREEN], ["Retry", lcd.NAVY], ["Reset", lcd.MAROON]];
const bw = (W - 8) / BUTTONS.length;
function drawTest(msg) {
  lcd.fillScreen(BG);
  lcd.setTextColor(FG);
  lcd.setTextDatum(lcd.TC);
  lcd.drawString(msg, W / 2, 6, 2);
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString("draw here - the dot should be under your touch", W / 2, 28, 1);
  for (const [x, y] of TARGETS) lcd.drawCircle(x, y, 3, lcd.DARKGREY);
  lcd.setTextDatum(lcd.MC);
  BUTTONS.forEach(([t, c], i) => {
    lcd.fillRoundRect(4 + i * bw + 2, BY, bw - 4, BH, 6, c);
    lcd.setTextColor(FG);
    lcd.drawString(t, 4 + i * bw + bw / 2, BY + BH / 2 + 1, 2);
  });
  lcd.setTextDatum(lcd.TL);
}
function testTouch(x, y, first) {
  if (mode !== "test") return;
  if (y >= BY - 2) {
    if (!first) return;
    const i = Math.min(BUTTONS.length - 1, Math.floor((x - 4) / bw));
    if (i === 0) save();
    else if (i === 1) restart();
    else reset();
    return;
  }
  lcd.fillCircle(x, y, 3, HOT);
}
function save() {
  const c = touch.calibrate();
  fs.write("/touchcal.json", JSON.stringify(c));
  saved = true;
  tone(1200, 60);
  drawTest("Saved - it's used from now on.");
  setTimeout(() => launch("/main.js"), 1200);
}
function reset() {
  touch.calibrate(null);
  fs.remove("/touchcal.json");
  saved = true;
  tone(600, 60);
  drawTest("Back to the built-in setting.");
}
function restart() {
  if (before) touch.calibrate(before.matrix, before.rotation); else touch.calibrate(null);
  step = 0; raw = []; samples = []; gone = 0; mode = "targets";
  drawTarget();
}

// raw touches for the targets, polled (the mapped positions can't be trusted yet)
setInterval(() => {
  if (mode !== "targets") return;
  const r = touch.raw();
  if (r) { samples.push([r.x, r.y]); gone = 0; return; }
  if (!samples.length || ++gone < 3) return;  // lifted (and stayed lifted a moment)
  if (samples.length >= 4) {
    const use = samples.slice(1, -1);  // skip the touch-down and lift-off readings
    raw.push([median(use.map((s) => s[0])), median(use.map((s) => s[1]))]);
    tone(1000, 30);
    step++;
    if (step === TARGETS.length) finish(); else drawTarget();
  }
  samples = [];
}, 20);

touch.on("down", ({ x, y }) => testTouch(x, y, true));
touch.on("move", ({ x, y }) => testTouch(x, y, false));
keyboard.on("key", ({ key }) => { if (key === "Escape" || key === "q") { if (!saved) restart(); launch("/main.js"); } });

if (!board.hasTouch) {
  lcd.fillScreen(BG);
  centreText(["This board has no touch screen."], H / 2);
} else drawTarget();
