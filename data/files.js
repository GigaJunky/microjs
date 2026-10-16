// File manager: browse the board's flash and the SD card; run, edit, view, play, copy, move,
// rename and delete files, and make folders.
// Keys: up / down move, Enter or right opens (a file opens its menu), Backspace or left goes up,
// m menu for the selection (folders too), c copy, x cut, v paste, Delete deletes, r rename,
// n new folder, Escape back to the launcher.
// Touch: tap a folder to open it, tap a file for its menu, swipe up / down to scroll; the buttons
// along the bottom (Menu also works on folders).
// Pictures (.jpg .png .bmp) open full screen: left / right (or tap the sides, or swipe) for the
// previous / next one in the folder, r (or tap the top strip) turns it 90 degrees, Enter / Escape
// (or tap the middle) to go back.
// Renaming needs a keyboard; on touch-only boards new folders get numbered names.
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2;
const TOUCH = board.hasTouch && !SMALL;  // finger-sized rows and buttons
const TH = SMALL ? 11 : 18, TOP = TH + 4;  // text line / header height
const LH = TOUCH ? 30 : TH, BAR = TOUCH ? 36 : board.hasTouch ? 16 : SMALL ? 10 : 14;
const ROWS = Math.floor((H - TOP - BAR) / LH), CHW = F === 1 ? 6 : 8, TY = Math.floor((LH - TH) / 2) + 1;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48), SEL = lcd.color(30, 70, 120), DIR = lcd.color(255, 200, 80);
const PICS = [".jpg", ".jpeg", ".png", ".bmp"];
const TEXT = [".txt", ".json", ".conf", ".cfg", ".csv", ".md", ".log", ".ini", ".hi", ".js", ".htm", ".html", ".xml", ".bas"];
keyboard.capture(true);  // Escape and Ctrl+C come here (the rename prompt needs them)

let pic = null;  // the picture being shown: { list, i, turn }
const ROT0 = lcd.getRotation();  // the screen's normal rotation, restored after a turned picture
let path = "", items = [], sel = 0, top = 0, clip = null, mode = "list", menu = null, view = null, prompt = null, msg = "";

// ---- helpers
const ext = (n) => (n.lastIndexOf(".") > 0 ? n.slice(n.lastIndexOf(".")).toLowerCase() : "");
const join = (dir, n) => (dir === "/" ? "/" + n : dir + "/" + n);
const parent = (p) => (p === "/" || p === "/sd" ? "" : p.slice(0, p.lastIndexOf("/")) || "/");
const fit = (s, px) => { const n = Math.floor(px / CHW); return s.length > n ? s.slice(0, Math.max(1, n - 1)) + "~" : s; };
function size(b) {
  if (b < 1024) return b + " B";
  if (b < 1048576) return (b / 1024).toFixed(b < 10240 ? 1 : 0) + " KB";
  if (b < 1073741824) return (b / 1048576).toFixed(1) + " MB";
  return (b / 1073741824).toFixed(1) + " GB";
}
const label = (p) => (p === "" ? "Storage" : p.startsWith("/sd") ? "SD:" + (p.slice(3) || "/") : "Flash:" + p);
function say(t) { msg = t; draw(); setTimeout(() => { if (msg === t) { msg = ""; draw(); } }, 2500); }

function load(p, keep) {
  path = p;
  if (p === "") {  // the two drives
    const f = fs.space("/"), s = fs.space("/sd");
    items = [{ name: "Flash", dir: true, size: 0, to: "/", info: `${size(f.total - f.used)} free of ${size(f.total)}` }];
    if (s.total) items.push({ name: "SD card", dir: true, size: 0, to: "/sd", info: `${size(s.total - s.used)} free of ${size(s.total)}` });
  } else {
    items = fs.dir(p).sort((a, b) => (a.dir !== b.dir ? (a.dir ? -1 : 1) : a.name.toLowerCase() < b.name.toLowerCase() ? -1 : 1));
    items.unshift({ name: "..", dir: true, up: true });
  }
  if (!keep) sel = top = 0;
  sel = Math.min(sel, Math.max(0, items.length - 1));
  draw();
}
const target = (it) => it.to || (it.up ? parent(path) : join(path, it.name));

// ---- drawing
function header(t, right) {
  lcd.fillRect(0, 0, W, TOP - 2, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(fit(t, W - 8 - (right ? right.length * CHW + 8 : 0)), 4, 2, F);
  if (right) { lcd.setTextDatum(lcd.TR); lcd.setTextColor(lcd.LIGHTGREY); lcd.drawString(right, W - 4, 2, F); lcd.setTextDatum(lcd.TL); }
}
const BUTTONS = [["Up", "up"], ["New", "new"], ["Paste", "paste"], ["Menu", "menu"]];
function footer() {
  const y = H - BAR;
  lcd.fillRect(0, y, W, BAR, PANEL);
  lcd.setTextDatum(lcd.ML);
  if (msg) { lcd.setTextColor(lcd.YELLOW); lcd.drawString(fit(msg, W - 8), 4, y + BAR / 2, 1); lcd.setTextDatum(lcd.TL); return; }
  if (!board.hasTouch) {
    lcd.setTextColor(lcd.DARKGREY);
    lcd.drawString(clip ? `${clip.cut ? "cut" : "copied"}: ${clip.name} - v pastes` : "Enter open  m menu  c/x/v  Del  r  n", 4, y + BAR / 2, 1);
    lcd.setTextDatum(lcd.TL);
    return;
  }
  const bw = W / BUTTONS.length;
  lcd.setTextDatum(lcd.MC);
  BUTTONS.forEach(([t, a], i) => {
    const off = (a === "paste" && !clip) || ((a === "new" || a === "paste") && path === "");
    lcd.fillRoundRect(i * bw + 2, y + 2, bw - 4, BAR - 4, 5, off ? lcd.color(30, 30, 40) : lcd.color(45, 50, 90));
    lcd.setTextColor(off ? lcd.DARKGREY : lcd.WHITE);
    lcd.drawString(t, i * bw + bw / 2, y + BAR / 2 + 1, F);
  });
  lcd.setTextDatum(lcd.TL);
}
function drawList() {
  header(label(path), items.length > ROWS ? `${sel + 1}/${items.length}` : "");
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
function drawMenu() {
  const n = menu.items.length, ML = TOUCH ? Math.min(30, Math.floor((H - TH - 14) / n)) : LH;
  const mh = Math.min(H, n * ML + TH + 12), mw = Math.min(W - 16, SMALL ? 150 : 230);
  menu.ml = ML;
  const x = (W - mw) / 2, y = Math.max(0, Math.floor((H - mh) / 2));
  menu.box = { x, y, w: mw, h: mh };
  lcd.fillRoundRect(x, y, mw, mh, 6, PANEL);
  lcd.drawRoundRect(x, y, mw, mh, 6, lcd.LIGHTGREY);
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
function drawView() {
  header(view.name, `${view.top + 1}/${view.lines.length}`);
  lcd.fillRect(0, TOP - 2, W, H - TOP + 2, BG);
  lcd.setTextColor(lcd.LIGHTGREY);
  const rows = Math.floor((H - TOP) / 9), cols = Math.floor(W / 6) - 1;
  for (let i = 0; i < rows && view.top + i < view.lines.length; i++)
    lcd.drawString(view.lines[view.top + i].slice(0, cols), 3, TOP + i * 9, 1);
}
function drawPrompt() {
  const y = H / 2 - LH * 2;
  lcd.fillRoundRect(6, y, W - 12, LH * 3 + 8, 6, PANEL);
  lcd.drawRoundRect(6, y, W - 12, LH * 3 + 8, 6, lcd.LIGHTGREY);
  lcd.setTextColor(DIR);
  lcd.drawString(prompt.title, 12, y + 4, F);
  lcd.fillRect(12, y + LH + 6, W - 24, LH + 2, BG);
  lcd.setTextColor(lcd.WHITE);
  const shown = prompt.text.slice(-Math.floor((W - 30) / CHW));
  lcd.drawString(shown + "_", 14, y + LH + 7, F);
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString("Enter OK   Escape cancel", 12, y + LH * 2 + 10, 1);
}
function draw() {
  if (mode === "view") return drawView();
  drawList();
  if (mode === "menu") drawMenu();
  if (mode === "prompt") drawPrompt();
}

// ---- actions
function open(it) {
  if (!it) return;
  if (it.dir) return load(target(it));
  if (PICS.includes(ext(it.name))) return showPicture(it);
  if (ext(it.name) === ".avi") return launch("/video.js", target(it));
  showMenu(it);
}
function showMenu(it) {
  if (!it || it.up || it.to) return;
  const p = target(it), e = ext(it.name), list = [];
  if (!it.dir) {
    if (e === ".js") list.push(["Run", () => launch(p)]);
    if (e === ".mp3" && board.hasMusic) list.push(["Play", () => { const err = audio.play(p); say(err || "Playing " + it.name + " - s stops"); }]);
    if ((e === ".flh" || e === ".bin") && p.startsWith("/sd")) list.push(["Install firmware...", () => confirm("Open the firmware installer?", "open", () => launch("/flash.js"))]);
    if (PICS.includes(e)) list.push(["View picture", () => showPicture(it)]);
    else if (e === ".avi") list.push(["Play video", () => launch("/video.js", p)]);
    else if (TEXT.includes(e) || it.size < 4096) list.push(["View", () => viewFile(p, it)]);
    if (board.hasKeyboard && (TEXT.includes(e) || !e)) list.push(["Edit", () => launch("/edit.js", p)]);
  }
  list.push(["Copy", () => { clip = { from: p, name: it.name, dir: it.dir, cut: false }; say("Copied " + it.name); }]);
  list.push(["Cut", () => { clip = { from: p, name: it.name, dir: it.dir, cut: true }; say("Cut " + it.name); }]);
  if (board.hasKeyboard) list.push(["Rename", () => ask("Rename to:", it.name, (n) => rename(p, n))]);
  list.push(["Delete", () => confirmDelete(it, p)]);
  list.push(["Info", () => say(it.dir ? `${it.name}: folder, ${fs.dir(p).length} items` : `${it.name}: ${size(it.size)} (${it.size} bytes)`)]);
  menu = { title: it.name + (it.dir ? "/" : ""), items: list, sel: 0 };
  mode = "menu";
  draw();
}
// Ask before doing something: a two-item menu, No first (so Enter or a stray tap is safe).
function confirm(title, yes, fn) {
  menu = { title, items: [["No", () => {}], ["Yes, " + yes, fn]], sel: 0 };
  mode = "menu";
  draw();
}
function confirmDelete(it, p) {
  confirm(`Delete ${it.name}${it.dir ? " and all in it" : ""}?`, "delete", () => {
    const ok = it.dir ? removeTree(p) : fs.remove(p);
    load(path, true);
    say(ok ? "Deleted " + it.name : "Couldn't delete " + it.name);
  });
}
function removeTree(p) {
  for (const e of fs.dir(p)) if (!(e.dir ? removeTree(join(p, e.name)) : fs.remove(join(p, e.name)))) return false;
  return fs.rmdir(p);
}
function copyTree(from, to) {
  if (!fs.stat(from).dir) return fs.copy(from, to);
  if (!fs.stat(to) && !fs.mkdir(to)) return false;
  for (const e of fs.dir(from)) if (!copyTree(join(from, e.name), join(to, e.name))) return false;
  return true;
}
function paste() {
  if (!clip || path === "") return;
  confirm(`${clip.cut ? "Move" : "Copy"} ${clip.name} here?`, clip.cut ? "move" : "copy", doPaste);
}
function doPaste() {
  let to = join(path, clip.name);
  if (to === clip.from && clip.cut) { clip = null; return draw(); }
  for (let n = 2; fs.exists(to); n++) {  // never overwrite: "name (2).ext"
    const dot = clip.dir || clip.name.lastIndexOf(".") <= 0 ? clip.name.length : clip.name.lastIndexOf(".");
    to = join(path, clip.name.slice(0, dot) + ` (${n})` + clip.name.slice(dot));
  }
  if (clip.cut && to.startsWith(clip.from + "/")) return say("Can't move a folder into itself");
  say((clip.cut ? "Moving " : "Copying ") + clip.name + "...");
  const sameDrive = clip.from.startsWith("/sd") === path.startsWith("/sd");
  let ok = clip.cut && sameDrive ? fs.rename(clip.from, to) : copyTree(clip.from, to);
  if (ok && clip.cut && !sameDrive) ok = fs.stat(clip.from).dir ? removeTree(clip.from) : fs.remove(clip.from);
  const name = clip.name, cut = clip.cut;
  if (cut) clip = null;
  load(path, true);
  say(ok ? (cut ? "Moved " : "Copied ") + name : "Failed - is there room?");
}
function rename(p, n) {
  if (!n || n.includes("/")) return say("Not a valid name");
  const to = join(parent(p) || "/", n);
  if (fs.exists(to)) return say(n + " already exists");
  say(fs.rename(p, to) ? "Renamed to " + n : "Couldn't rename");
  load(path, true);
}
function newFolder() {
  if (path === "") return;
  const make = (n) => say(fs.mkdir(join(path, n)) ? "Made " + n : "Couldn't make " + n) || load(path, true);
  if (board.hasKeyboard) return ask("New folder name:", "", (n) => (n && !n.includes("/") ? make(n) : say("Not a valid name")));
  let n = 1;
  while (fs.exists(join(path, "folder" + n))) n++;
  make("folder" + n);
}
function viewFile(p, it) {
  if (it.size > 64 * 1024) return say("Too big to view (over 64 KB)");
  const t = fs.read(p);
  if (t === null) return say("Couldn't read " + it.name);
  let bin = 0;
  for (let i = 0; i < Math.min(t.length, 512); i++) { const c = t.charCodeAt(i); if (c < 9 || (c > 13 && c < 32)) bin++; }
  view = { name: it.name, top: 0, lines: bin > 8 ? ["(binary file - " + size(it.size) + ")"] : t.split("\n") };
  mode = "view";
  draw();
}
function showPicture(it) {
  const list = items.filter((e) => !e.dir && PICS.includes(ext(e.name)));
  pic = { list, i: Math.max(0, list.indexOf(it)), turn: 0 };
  mode = "pic";
  drawPicture();
}
function drawPicture() {
  const it = pic.list[pic.i], pw = lcd.width(), ph = lcd.height();
  lcd.fillScreen(BG);
  let info = "";
  try {
    const r = lcd.drawImage(join(path, it.name));
    info = `${r.width}x${r.height} ${Math.round(r.zoom * 100)}%`;
  } catch (e) {
    lcd.setTextColor(lcd.RED);
    lcd.setTextDatum(lcd.MC);
    lcd.drawString(String(e.message || e).replace("drawImage: ", ""), pw / 2, ph / 2, 1);
    lcd.setTextDatum(lcd.TL);
  }
  lcd.setTextColor(lcd.WHITE, BG);  // name and position, small, along the bottom
  lcd.setTextDatum(lcd.BL);
  lcd.drawString(fit(`${pic.i + 1}/${pic.list.length} ${it.name} ${info}`, pw * 1.5), 2, ph - 1, 1);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
}
function turnPicture() {
  pic.turn = (pic.turn + 1) % 4;
  lcd.setRotation((ROT0 + pic.turn) % 4);
  drawPicture();
}
function closePicture() {
  pic = null;
  mode = "list";
  lcd.setRotation(ROT0);
  lcd.fillScreen(BG);
  draw();
}
function stepPicture(d) {
  pic.i = (pic.i + d + pic.list.length) % pic.list.length;
  sel = items.indexOf(pic.list[pic.i]);  // the list follows, for when you go back
  drawPicture();
}
function ask(title, text, done) { prompt = { title, text, done }; mode = "prompt"; draw(); }
function act(i) {
  const f = menu.items[i][1];
  mode = "list";
  menu = null;
  draw();
  f();
}

// ---- input
function key(k) {
  if (mode === "prompt") {
    if (k === "Enter") { const p = prompt; prompt = null; mode = "list"; draw(); p.done(p.text.trim()); }
    else if (k === "Escape") { prompt = null; mode = "list"; draw(); }
    else if (k === "Backspace") { prompt.text = prompt.text.slice(0, -1); drawPrompt(); }
    else if (k.length === 1) { prompt.text += k; drawPrompt(); }
    return;
  }
  if (mode === "pic") {
    if (k === "ArrowRight" || k === "/" || k === " " || k === "ArrowDown" || k === ".") stepPicture(1);
    else if (k === "ArrowLeft" || k === "," || k === "ArrowUp" || k === ";") stepPicture(-1);
    else if (k === "r" || k === "R") turnPicture();
    else closePicture();
    return;
  }
  if (mode === "view") {
    const rows = Math.floor((H - TOP) / 9), max = Math.max(0, view.lines.length - rows);
    if (k === "ArrowDown" || k === ".") view.top = Math.min(max, view.top + 1);
    else if (k === "ArrowUp" || k === ";") view.top = Math.max(0, view.top - 1);
    else if (k === "PageDown" || k === " " || k === "ArrowRight" || k === "/") view.top = Math.min(max, view.top + rows);
    else if (k === "PageUp" || k === "ArrowLeft" || k === ",") view.top = Math.max(0, view.top - rows);
    else { view = null; mode = "list"; }
    return draw();
  }
  if (mode === "menu") {
    if (k === "ArrowDown" || k === ".") menu.sel = (menu.sel + 1) % menu.items.length;
    else if (k === "ArrowUp" || k === ";") menu.sel = (menu.sel + menu.items.length - 1) % menu.items.length;
    else if (k === "Enter" || k === " " || k === "ArrowRight" || k === "/") return act(menu.sel);
    else if (k === "Escape" || k === "ArrowLeft" || k === "," || k === "Backspace") { menu = null; mode = "list"; }
    return draw();
  }
  const it = items[sel];
  if (k === "ArrowDown" || k === ".") { sel = Math.min(items.length - 1, sel + 1); draw(); }
  else if (k === "ArrowUp" || k === ";") { sel = Math.max(0, sel - 1); draw(); }
  else if (k === "PageDown") { sel = Math.min(items.length - 1, sel + ROWS); draw(); }
  else if (k === "PageUp") { sel = Math.max(0, sel - ROWS); draw(); }
  else if (k === "Enter" || k === "ArrowRight" || k === "/") open(it);
  else if (k === "Backspace" || k === "ArrowLeft" || k === ",") { if (path !== "") load(parent(path)); }
  else if (k === "Escape") { audio.stop(); launch("/main.js"); }
  else if (k === "m" || k === " ") showMenu(it);
  else if (k === "c" && it && !it.up && !it.to) { clip = { from: target(it), name: it.name, dir: it.dir, cut: false }; say("Copied " + it.name); }
  else if (k === "x" && it && !it.up && !it.to) { clip = { from: target(it), name: it.name, dir: it.dir, cut: true }; say("Cut " + it.name); }
  else if (k === "v") paste();
  else if ((k === "Delete" || k === "d") && it && !it.up && !it.to) confirmDelete(it, target(it));
  else if (k === "r" && it && !it.up && !it.to) ask("Rename to:", it.name, (n) => rename(target(it), n));
  else if (k === "n") newFolder();
  else if (k === "s") { audio.stop(); say("Stopped"); }
}
keyboard.on("key", ({ key: k }) => key(k));
// Touch acts when the finger lifts: a tap opens, a long press (half a second) shows the menu, and
// dragging up / down scrolls the list or the text without opening anything.
let press = null;
touch.on("down", ({ x, y }) => {
  press = { x, y, at: millis(), moved: false, top, vtop: view ? view.top : 0 };
  if (mode === "list" && y >= TOP && y < H - BAR) {  // light up the row under the finger
    const i = top + Math.floor((y - TOP) / LH);
    if (i < items.length && i !== sel) { sel = i; drawList(); }
  }
});
touch.on("move", ({ x, y }) => {
  if (!press) return;
  const dy = y - press.y;
  if (Math.abs(dy) > 10 || Math.abs(x - press.x) > 10) press.moved = true;
  press.lx = x;
  if (!press.moved) return;
  if (mode === "list" && press.y >= TOP && press.y < H - BAR && items.length > ROWS) {
    const t = Math.max(0, Math.min(items.length - ROWS, press.top - Math.round(dy / LH)));
    if (t !== top) { top = t; sel = Math.max(top, Math.min(sel, top + ROWS - 1)); drawList(); }
  } else if (mode === "view") {
    const rows = Math.floor((H - TOP) / 9), t = Math.max(0, Math.min(Math.max(0, view.lines.length - rows), press.vtop - Math.round(dy / 9)));
    if (t !== view.top) { view.top = t; drawView(); }
  }
});
touch.on("up", () => {
  const p = press;
  press = null;
  if (p && mode === "pic" && p.moved) {  // a sideways swipe changes picture
    const dx = (p.lx ?? p.x) - p.x;
    if (Math.abs(dx) > 30) stepPicture(dx < 0 ? 1 : -1);
    return;
  }
  if (!p || p.moved) return;
  const { x, y } = p, long = millis() - p.at > 500;
  if (mode === "pic") {
    const pw = lcd.width(), ph = lcd.height();
    if (y < ph / 5) return turnPicture();
    return x < pw / 3 ? stepPicture(-1) : x > (pw * 2) / 3 ? stepPicture(1) : closePicture();
  }
  if (mode === "view") {
    if (y < TOP) return key("Escape");
    return key(y < H / 2 ? "PageUp" : "PageDown");
  }
  if (mode === "menu") {
    const b = menu.box, i = Math.floor((y - b.y - TH - 8) / menu.ml);
    if (x < b.x || x > b.x + b.w || y < b.y || y > b.y + b.h) { menu = null; mode = "list"; return draw(); }
    if (i >= 0 && i < menu.items.length) act(i);
    return;
  }
  if (mode !== "list") return;
  if (y >= H - BAR) {
    const a = BUTTONS[Math.min(BUTTONS.length - 1, Math.floor(x / (W / BUTTONS.length)))][1];
    if (a === "up") { if (path !== "") load(parent(path)); else launch("/main.js"); }
    else if (a === "new") newFolder();
    else if (a === "paste") paste();
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

lcd.fillScreen(BG);
load(fs.space("/sd").total ? "" : "/");
