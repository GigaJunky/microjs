// The file manager's menu: files.js launches this with {p, name, dir, size} (a file's menu), {kind: "delete", ...}
// or {kind: "paste", clip, to} (ask first), and it goes back to the file manager - or on to the viewer, the
// editor, the file operations - with what you chose. (A launched app starts with a clean heap, which the file
// manager itself can't spare on a board without PSRAM.)
const A = JSON.parse(ARG), S = "/files.state";
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2, TOUCH = board.hasTouch && !SMALL;
const TH = SMALL ? 11 : 18, CHW = F === 1 ? 6 : 8, LH = TOUCH ? 30 : TH;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48), SEL = lcd.color(30, 70, 120), DIR = lcd.color(255, 200, 80);
const PICS = [".jpg", ".jpeg", ".png", ".bmp"];
const TEXT = [".txt", ".json", ".conf", ".cfg", ".csv", ".md", ".log", ".ini", ".hi", ".js", ".htm", ".html", ".xml", ".bas"];
const ext = (n) => (n.lastIndexOf(".") > 0 ? n.slice(n.lastIndexOf(".")).toLowerCase() : "");
const fit = (s, px) => { const n = Math.floor(px / CHW); return s.length > n ? s.slice(0, Math.max(1, n - 1)) + "~" : s; };
const size = (b) => (b < 1024 ? b + " B" : b < 1048576 ? (b / 1024).toFixed(b < 10240 ? 1 : 0) + " KB" : (b / 1048576).toFixed(1) + " MB");
const fops = (o) => launch("/lib/fops.js", JSON.stringify(o));
function back(patch) {  // to the file manager, with a message, a new clipboard, or something for it to do
  try { const s = JSON.parse(fs.read(S)); Object.assign(s, patch); fs.write(S, JSON.stringify(s)); } catch (e) {}
  launch("/files.js");
}
keyboard.capture(true);  // Escape comes here

let menu = null;
// Ask before doing something: a two-item menu, No first (so Enter or a stray tap is safe).
function ask(title, yes, fn) { menu = { title, items: [["No", () => back({})], ["Yes, " + yes, fn]], sel: 0 }; }
function fileMenu() {
  const p = A.p, e = ext(A.name), list = [];
  if (!A.dir) {
    if (e === ".js") list.push(["Run", () => { fs.remove(S); launch(p); }]);
    if (e === ".mp3" && board.hasMusic) list.push(["Play", () => launch("/lib/fplay.js", p)]);
    if ((e === ".flh" || e === ".efw" || e === ".bin") && p.startsWith("/sd") && (!board.chip || board.chip.startsWith("esp32"))) list.push(["Install firmware...", () => launch("/lib/finstall.js", p)]);
    if (PICS.includes(e)) list.push(["View picture", () => launch("/lib/fview.js", p)]);
    else if (e === ".avi") list.push(["Play video", () => launch("/lib/fplay.js", p)]);
    else if (TEXT.includes(e) || A.size < 4096) list.push(["View", () => launch("/lib/fview.js", p)]);
    if (board.hasKeyboard && (TEXT.includes(e) || !e)) list.push(["Edit", () => { fs.remove(S); launch("/edit.js", p); }]);
  }
  list.push(["Copy", () => back({ clip: { from: p, name: A.name, dir: A.dir, cut: false }, msg: "Copied " + A.name })]);
  list.push(["Cut", () => back({ clip: { from: p, name: A.name, dir: A.dir, cut: true }, msg: "Cut " + A.name })]);
  if (board.hasKeyboard) list.push(["Rename", () => fops({ op: "rename", p, name: A.name })]);
  list.push(["Delete", () => askDelete()]);
  list.push(["Info", () => back({ msg: A.dir ? `${A.name}: folder` : `${A.name}: ${size(A.size)} (${A.size} bytes)` })]);
  menu = { title: A.name + (A.dir ? "/" : ""), items: list, sel: 0 };
}
function askDelete() { ask(`Delete ${A.name}${A.dir ? " and all in it" : ""}?`, "delete", () => fops({ op: "delete", p: A.p, name: A.name })); }

function drawMenu() {
  const n = menu.items.length, ML = TOUCH ? Math.min(30, Math.floor((H - TH - 14) / n)) : LH;
  const mh = Math.min(H, n * ML + TH + 12), mw = Math.min(W - 16, SMALL ? 150 : 230);
  const x = (W - mw) / 2, y = Math.max(0, Math.floor((H - mh) / 2));
  menu.ml = ML;
  menu.box = { x, y, w: mw, h: mh };
  lcd.fillScreen(BG);
  lcd.fillRoundRect(x, y, mw, mh, 6, PANEL);
  lcd.drawRoundRect(x, y, mw, mh, 6, lcd.LIGHTGREY);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(DIR);
  lcd.drawString(fit(menu.title, mw - 8), x + 6, y + 4, F);
  menu.items.forEach(([t], i) => {
    const iy = y + TH + 8 + i * ML;
    if (i === menu.sel) lcd.fillRect(x + 2, iy, mw - 4, ML, SEL);
    else if (TOUCH && i) lcd.drawFastHLine(x + 6, iy, mw - 12, lcd.color(45, 50, 80));
    lcd.setTextColor(t.startsWith("Delete") || t === "Yes, delete" ? lcd.color(255, 120, 110) : t.startsWith("Yes") ? lcd.GREEN : lcd.WHITE);
    lcd.drawString(t, x + 10, iy + Math.floor((ML - TH) / 2) + 1, F);
  });
}
keyboard.on("key", ({ key: k }) => {
  const n = menu.items.length;
  if (k === "ArrowDown" || k === ".") menu.sel = (menu.sel + 1) % n;
  else if (k === "ArrowUp" || k === ";") menu.sel = (menu.sel + n - 1) % n;
  else if (k === "Enter" || k === " " || k === "ArrowRight" || k === "/") return menu.items[menu.sel][1]();
  else if (k === "Escape" || k === "ArrowLeft" || k === "," || k === "Backspace") return back({});
  drawMenu();
});
let pt = { x: 0, y: 0 };  // where the finger is (the "up" event doesn't say)
touch.on("down", (e) => { pt = e; });
touch.on("move", (e) => { pt = e; });
touch.on("up", () => {
  const { x, y } = pt, b = menu.box, i = Math.floor((y - b.y - TH - 8) / menu.ml);
  if (x < b.x || x > b.x + b.w || y < b.y || y > b.y + b.h) return back({});
  if (i >= 0 && i < menu.items.length) menu.items[i][1]();
});

if (A.kind === "paste") ask(`${A.clip.cut ? "Move" : "Copy"} ${A.clip.name} here?`, A.clip.cut ? "move" : "copy", () => fops({ op: "paste", clip: A.clip, to: A.to }));
else if (A.kind === "delete") askDelete();
else fileMenu();
drawMenu();
