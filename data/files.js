// File manager: browse the flash and the SD card; run, edit, view, play, copy, move, rename and delete files, make
// folders. Keys: arrows, Enter, Backspace (up), m menu, c copy, x cut, v paste, d delete, r rename, n new folder, f filter,
// Escape leaves. Touch: tap, long press for the menu, swipe to scroll. The menu, the viewers and the file
// operations are launched apps (lib/fmenu.js, fview.js, fops.js, fplay.js): this file has to stay small to load on a classic CYD.
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2;
const TOUCH = board.hasTouch && !SMALL;
const TH = SMALL ? 11 : 18, TOP = TH + 4;
const LH = TOUCH ? 30 : TH, BAR = TOUCH ? 36 : board.hasTouch ? 16 : SMALL ? 10 : 14;
const ROWS = Math.floor((H - TOP - BAR) / LH), CHW = F === 1 ? 6 : 8, TY = Math.floor((LH - TH) / 2) + 1;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48), SEL = lcd.color(30, 70, 120), DIR = lcd.color(255, 200, 80);
const PICS = [".jpg", ".jpeg", ".png", ".bmp"];
const FILTERS = [["All", null], ["Music", [".mp3"]], ["Video", [".avi"]], ["Pics", PICS], ["Apps", [".js"]], ["Flash", [".flh", ".efw", ".bin"]]];  // f / the Type button: show only these files (folders always)
let filter = 0;
keyboard.capture(true);  // Escape and Ctrl+C come here

const PAGE = 40;  // entries read from a folder at a time: a big folder listed whole runs a small board out of memory
let path = "", off = 0, items = [], sel = 0, top = 0, clip = null, msg = "";

// ---- helpers
const STATE = "/files.state";
function go(app, arg) {
  fs.write(STATE, JSON.stringify({ path, off, name: items[sel] && items[sel].name, top, filter, clip }));
  launch(app, typeof arg === "string" ? arg : JSON.stringify(arg));
}
function quit() { fs.remove(STATE); launch("/main.js"); }
const ext = (n) => (n.lastIndexOf(".") > 0 ? n.slice(n.lastIndexOf(".")).toLowerCase() : "");
const join = (dir, n) => (dir === "/" ? "/" + n : dir + "/" + n);
const parent = (p) => (p === "/" || p === "/sd" ? "" : p.slice(0, p.lastIndexOf("/")) || "/");
const fit = (s, px) => { const n = Math.floor(px / CHW); return s.length > n ? s.slice(0, Math.max(1, n - 1)) + "~" : s; };
const size = (b) => (b < 1024 ? b + " B" : b < 1048576 ? (b / 1024).toFixed(b < 10240 ? 1 : 0) + " KB" : b < 1073741824 ? (b / 1048576).toFixed(1) + " MB" : (b / 1073741824).toFixed(1) + " GB");
const label = (p) => (p === "" ? "Storage" : p.startsWith("/sd") ? "SD:" + (p.slice(3) || "/") : "Flash:" + p);
function say(t) { msg = t; draw(); setTimeout(() => { if (msg === t) { msg = ""; draw(); } }, 2500); }

function loadDir(p, keep, at) {
  path = p;
  off = at || 0;
  if (p === "") {  // the two drives
    const f = fs.space("/"), s = fs.space("/sd");
    items = [{ name: "Flash", dir: true, size: 0, to: "/", info: `${size(f.total - f.used)} free of ${size(f.total)}` }];
    if (s.total && s.total !== f.total) items.push({ name: "SD card", dir: true, size: 0, to: "/sd", info: s.used < 0 ? size(s.total) : `${size(s.total - s.used)} free of ${size(s.total)}` });
  } else {
    const want = FILTERS[filter][1];
    items = [{ name: "..", dir: true, up: true }];  // (built in place: a big folder's copies would run a small board out of memory)
    const page = fs.dir(p, off, PAGE);
    for (const e of page) if (e.dir || !want || want.includes(ext(e.name))) items.push(e);
    items.sort((a, b) => (a.up ? -1 : b.up ? 1 : a.dir !== b.dir ? (a.dir ? -1 : 1) : a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1));
    if (page.length === PAGE) items.push({ name: "More...", more: true, info: " " });
  }
  if (!keep) sel = top = 0;
  sel = Math.min(sel, Math.max(0, items.length - 1));
  draw();
}
const target = (it) => it.to || (it.up ? parent(path) : join(path, it.name));

// ---- drawing
function header(t, right) {
  right = right || "";
  lcd.fillRect(0, 0, W, TOP - 2, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(fit(t, W - 16 - right.length * CHW), 4, 2, F);
  lcd.setTextDatum(lcd.TR);
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString(right, W - 4, 2, F);
  lcd.setTextDatum(lcd.TL);
}
const BUTTONS = [["Up", "up"], ["New", "new"], ["Paste", "paste"], ["Type", "filter"], ["Menu", "menu"]];
function footer() {
  const y = H - BAR;
  lcd.fillRect(0, y, W, BAR, PANEL);
  lcd.setTextDatum(lcd.ML);
  if (msg) { lcd.setTextColor(lcd.YELLOW); lcd.drawString(fit(msg, W - 8), 4, y + BAR / 2, 1); lcd.setTextDatum(lcd.TL); return; }
  if (!board.hasTouch) {
    lcd.setTextColor(lcd.DARKGREY);
    lcd.drawString(clip ? `${clip.cut ? "cut" : "copied"}: ${clip.name} - v pastes` : "m menu  c/x/v  d  r  n  f type", 4, y + BAR / 2, 1);
    lcd.setTextDatum(lcd.TL);
    return;
  }
  const bw = W / BUTTONS.length;
  lcd.setTextDatum(lcd.MC);
  BUTTONS.forEach(([t, a], i) => {
    const off = (a === "paste" && !clip) || ((a === "new" || a === "paste") && path === "");
    lcd.fillRoundRect(i * bw + 2, y + 2, bw - 4, BAR - 4, 5, off ? lcd.color(30, 30, 40) : lcd.color(45, 50, 90));
    lcd.setTextColor(off ? lcd.DARKGREY : lcd.WHITE);
    lcd.drawString(a === "filter" ? FILTERS[filter][0] : t, i * bw + bw / 2, y + BAR / 2 + 1, F);
  });
  lcd.setTextDatum(lcd.TL);
}
function drawList() {
  header(label(path), (filter && path !== "" ? FILTERS[filter][0] + " " : "") + (items.length > ROWS ? `${sel + 1}/${items.length}` : ""));
  lcd.fillRect(0, TOP - 2, W, H - TOP + 2 - BAR, BG);
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;
  top = Math.max(0, Math.min(top, items.length - ROWS));
  for (let i = top; i < Math.min(items.length, top + ROWS); i++) {
    const it = items[i], y = TOP + (i - top) * LH;
    if (i === sel) lcd.fillRect(0, y - 1, W, LH, SEL);
    else if (TOUCH) lcd.drawFastHLine(0, y + LH - 1, W, lcd.color(25, 25, 35));
    const right = it.info || (it.dir ? "" : size(it.size));
    lcd.setTextColor(it.dir ? DIR : lcd.WHITE);
    lcd.drawString(fit((it.dir && !it.up && !it.to ? "/" : "") + it.name, W - 16 - right.length * CHW), 4, y + TY, F);
    lcd.setTextDatum(lcd.TR);
    lcd.setTextColor(lcd.LIGHTGREY);
    lcd.drawString(right, W - 8, y + TY, F);
    lcd.setTextDatum(lcd.TL);
  }
  if (items.length > ROWS) {  // scrollbar
    const th = H - TOP - BAR, bh = Math.max(10, (th * ROWS) / items.length);
    lcd.fillRect(W - 4, TOP + ((th - bh) * top) / (items.length - ROWS), 3, bh, lcd.DARKGREY);
  }
  footer();
}
const draw = drawList;

// ---- actions
function open(it) {
  if (!it) return;
  if (it.more) return loadDir(path, false, off + PAGE);
  if (it.dir) return loadDir(target(it));
  if (PICS.includes(ext(it.name))) return go("/lib/fview.js", target(it));
  if (ext(it.name) === ".avi") return go("/lib/fplay.js", target(it));
  showMenu(it);
}
function showMenu(it) { if (it && !it.up && !it.to && !it.more) go("/lib/fmenu.js", { p: target(it), name: it.name, dir: it.dir, size: it.size }); }  // (lib/fmenu.js)
function paste() { if (clip && path !== "") go("/lib/fmenu.js", { kind: "paste", clip, to: path }); }
function newFolder() { if (path !== "") go("/lib/fops.js", { op: "mkdir", dir: path }); }
// ---- input
function key(k) {
  const it = items[sel], real = it && !it.up && !it.to && !it.more;
  if (k === "ArrowDown" || k === ".") { sel = Math.min(items.length - 1, sel + 1); draw(); }
  else if (k === "ArrowUp" || k === ";") { sel = Math.max(0, sel - 1); draw(); }
  else if (k === "PageDown") { sel = Math.min(items.length - 1, sel + ROWS); draw(); }
  else if (k === "PageUp") { sel = Math.max(0, sel - ROWS); draw(); }
  else if (k === "Enter" || k === "ArrowRight" || k === "/") open(it);
  else if (k === "Backspace" || k === "ArrowLeft" || k === ",") { if (path !== "") loadDir(parent(path)); }
  else if (k === "Escape") quit();
  else if (k === "m" || k === " ") showMenu(it);
  else if ((k === "c" || k === "x") && real) { clip = { from: target(it), name: it.name, dir: it.dir, cut: k === "x" }; say((k === "x" ? "Cut " : "Copied ") + it.name); }
  else if (k === "v") paste();
  else if ((k === "Delete" || k === "d") && real) go("/lib/fmenu.js", { kind: "delete", p: target(it), name: it.name, dir: it.dir });
  else if (k === "r" && real) go("/lib/fops.js", { op: "rename", p: target(it), name: it.name });
  else if (k === "n") newFolder();
  else if (k === "f") { filter = (filter + 1) % FILTERS.length; loadDir(path, true); }
}
keyboard.on("key", ({ key: k }) => key(k));
let press = null;
touch.on("down", ({ x, y }) => {
  press = { x, y, at: millis(), moved: false, top };
  if (y >= TOP && y < H - BAR) {  // light up the row under the finger
    const i = top + Math.floor((y - TOP) / LH);
    if (i < items.length && i !== sel) { sel = i; drawList(); }
  }
});
touch.on("move", ({ x, y }) => {
  if (!press) return;
  const dy = y - press.y;
  if (Math.abs(dy) > 10 || Math.abs(x - press.x) > 10) press.moved = true;
  if (!press.moved) return;
  if (press.y >= TOP && press.y < H - BAR && items.length > ROWS) {
    const t = Math.max(0, Math.min(items.length - ROWS, press.top - Math.round(dy / LH)));
    if (t !== top) { top = t; sel = Math.max(top, Math.min(sel, top + ROWS - 1)); drawList(); }
  }
});
touch.on("up", () => {
  const p = press;
  press = null;
  if (!p || p.moved) return;
  const { x, y } = p, long = millis() - p.at > 500;
  if (y >= H - BAR) {
    const a = BUTTONS[Math.min(BUTTONS.length - 1, Math.floor(x / (W / BUTTONS.length)))][1];
    if (a === "up") { if (path !== "") loadDir(parent(path)); else quit(); }
    else if (a === "new") newFolder();
    else if (a === "paste") paste();
    else if (a === "filter") { filter = (filter + 1) % FILTERS.length; loadDir(path, true); }
    else if (a === "menu") showMenu(items[sel]);
    return;
  }
  if (y < TOP) return;
  const i = top + Math.floor((y - TOP) / LH);
  if (i >= items.length) return;
  sel = i;
  if (long) showMenu(items[i]);
  else open(items[i]);
});

const card = fs.space("/sd").total > 0 && fs.space("/sd").total !== fs.space("/").total;
const raw = typeof ARG === "string" && ARG === "flash" ? null : fs.read(STATE);
const st = raw ? JSON.parse(raw) : null;
if (raw) fs.remove(STATE);
if (typeof ARG === "string" && ARG === "flash") filter = FILTERS.length - 1;
lcd.fillScreen(BG);
if (st) {
  filter = st.filter;
  clip = st.clip;
  top = st.top;
  loadDir(st.path, true, st.off);
  const at = items.findIndex((e) => e.name === st.name);
  if (at >= 0) sel = at;
  drawList();
  if (st.msg) say(st.msg);
} else loadDir(filter && card ? "/sd" : card ? "" : "/");
