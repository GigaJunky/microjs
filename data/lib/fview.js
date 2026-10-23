// The file manager's viewer: files.js launches this with a file's path, and it comes back to the file manager
// when you are done. Pictures: left / right (or tap the sides, or swipe) for the previous / next one, r turns it
// 90 degrees, Enter / Escape (or tap the middle) to go back. Text: up / down, Space / PageDown for a page, any
// other key (or a tap on the title) goes back; tap the top or bottom half, or swipe, to scroll. (A launched app
// starts with a clean heap, which is why the viewer is not part of files.js.)
const P = ARG, S = "/files.state";
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, TH = SMALL ? 11 : 18, TOP = TH + 4, F = SMALL ? 1 : 2, CHW = F === 1 ? 6 : 8;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48);
const PICS = [".jpg", ".jpeg", ".png", ".bmp"];
const ROT0 = lcd.getRotation();  // the screen's normal rotation, restored after a turned picture
const ext = (n) => (n.lastIndexOf(".") > 0 ? n.slice(n.lastIndexOf(".")).toLowerCase() : "");
const base = (n) => n.slice(n.lastIndexOf("/") + 1);
const fit = (s, px) => { const n = Math.floor(px / CHW); return s.length > n ? s.slice(0, Math.max(1, n - 1)) + "~" : s; };
keyboard.capture(true);  // Escape comes here

let pic = null, view = null;  // the picture being shown: { list, i, turn }; the text: { name, top, lines }
function back(name) {
  lcd.setRotation(ROT0);
  try { const s = JSON.parse(fs.read(S)); if (name) s.name = name; fs.write(S, JSON.stringify(s)); } catch (e) {}
  launch("/files.js");
}

// ---- text
function drawView() {
  lcd.fillRect(0, 0, W, TOP - 2, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(fit(view.name, W - 80), 4, 2, F);
  lcd.setTextDatum(lcd.TR);
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString(`${view.top + 1}/${view.lines.length}`, W - 4, 2, F);
  lcd.setTextDatum(lcd.TL);
  lcd.fillRect(0, TOP - 2, W, H - TOP + 2, BG);
  const rows = Math.floor((H - TOP) / 9), cols = Math.floor(W / 6) - 1;
  for (let i = 0; i < rows && view.top + i < view.lines.length; i++)
    lcd.drawString(view.lines[view.top + i].slice(0, cols), 3, TOP + i * 9, 1);
}
function viewFile() {
  const st = fs.stat(P), t = st.size > 64 * 1024 ? null : fs.read(P);
  let bin = 0;
  for (let i = 0; t && i < Math.min(t.length, 512); i++) { const c = t.charCodeAt(i); if (c < 9 || (c > 13 && c < 32)) bin++; }
  view = { name: base(P), top: 0, lines: t === null ? ["(too big to view: over 64 KB, or can't be read)"] : bin > 8 ? ["(binary file - " + st.size + " bytes)"] : t.split("\n") };
  lcd.setTextColor(lcd.LIGHTGREY);
  drawView();
}
function viewKey(k) {
  const rows = Math.floor((H - TOP) / 9), max = Math.max(0, view.lines.length - rows);
  if (k === "ArrowDown" || k === ".") view.top = Math.min(max, view.top + 1);
  else if (k === "ArrowUp" || k === ";") view.top = Math.max(0, view.top - 1);
  else if (k === "PageDown" || k === " " || k === "ArrowRight" || k === "/") view.top = Math.min(max, view.top + rows);
  else if (k === "PageUp" || k === "ArrowLeft" || k === ",") view.top = Math.max(0, view.top - rows);
  else return back();
  drawView();
}

// ---- pictures
function drawPicture() {
  const it = pic.list[pic.i], pw = lcd.width(), ph = lcd.height();
  lcd.fillScreen(BG);
  let info = "";
  try {
    const r = lcd.drawImage(P.slice(0, P.lastIndexOf("/") + 1) + it);
    info = `${r.width}x${r.height} ${Math.round(r.zoom * 100)}%`;
  } catch (e) {
    lcd.setTextColor(lcd.RED);
    lcd.setTextDatum(lcd.MC);
    lcd.drawString(String(e.message || e).replace("drawImage: ", ""), pw / 2, ph / 2, 1);
    lcd.setTextDatum(lcd.TL);
  }
  lcd.setTextColor(lcd.WHITE, BG);  // name and position, small, along the bottom
  lcd.setTextDatum(lcd.BL);
  lcd.drawString(fit(`${pic.i + 1}/${pic.list.length} ${it} ${info}`, pw * 1.5), 2, ph - 1, 1);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
}
function step(d) { pic.i = (pic.i + d + pic.list.length) % pic.list.length; drawPicture(); }
function turn() { pic.turn = (pic.turn + 1) % 4; lcd.setRotation((ROT0 + pic.turn) % 4); drawPicture(); }
function picKey(k) {
  if (k === "ArrowRight" || k === "/" || k === " " || k === "ArrowDown" || k === ".") step(1);
  else if (k === "ArrowLeft" || k === "," || k === "ArrowUp" || k === ";") step(-1);
  else if (k === "r" || k === "R") turn();
  else back(pic.list[pic.i]);
}

// ---- input: a tap, or a swipe
keyboard.on("key", ({ key: k }) => (pic ? picKey(k) : viewKey(k)));
let press = null;
touch.on("down", ({ x, y }) => { press = { x, y, top: view ? view.top : 0, moved: false }; });
touch.on("move", ({ x, y }) => {
  if (!press) return;
  if (Math.abs(y - press.y) > 10 || Math.abs(x - press.x) > 10) press.moved = true;
  press.lx = x;
  if (press.moved && view) {
    const rows = Math.floor((H - TOP) / 9), t = Math.max(0, Math.min(Math.max(0, view.lines.length - rows), press.top - Math.round((y - press.y) / 9)));
    if (t !== view.top) { view.top = t; drawView(); }
  }
});
touch.on("up", () => {
  const p = press;
  press = null;
  if (!p) return;
  if (pic) {
    if (p.moved) { const dx = (p.lx ?? p.x) - p.x; if (Math.abs(dx) > 30) step(dx < 0 ? 1 : -1); return; }
    const pw = lcd.width(), ph = lcd.height();
    if (p.y < ph / 5) return turn();
    return p.x < pw / 3 ? step(-1) : p.x > (pw * 2) / 3 ? step(1) : back(pic.list[pic.i]);
  }
  if (p.moved) return;
  if (p.y < TOP) return back();
  viewKey(p.y < H / 2 ? "PageUp" : "PageDown");
});

lcd.fillScreen(BG);
if (PICS.includes(ext(P))) {
  const dir = P.slice(0, P.lastIndexOf("/")) || "/";
  const list = fs.dir(dir, 0, 200).filter((e) => !e.dir && PICS.includes(ext(e.name))).sort((a, b) => (a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1)).map((e) => e.name);
  pic = { list, i: Math.max(0, list.indexOf(base(P))), turn: 0 };
  drawPicture();
} else viewFile();
