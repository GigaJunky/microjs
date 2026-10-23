// The file manager's file operations: files.js launches this with a JSON argument and it comes back to the
// file manager with a message when done (a launched app starts with a clean heap, which copying a folder tree
// and the name prompt need on a board without much memory).
//   {op: "delete", p, name}   {op: "rename", p, name}   {op: "mkdir", dir}   {op: "paste", clip, to}
const A = JSON.parse(ARG), S = "/files.state";
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, TH = SMALL ? 11 : 18, F = SMALL ? 1 : 2, CHW = F === 1 ? 6 : 8;
const LH = board.hasTouch && !SMALL ? 30 : TH;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48), DIR = lcd.color(255, 200, 80);
const join = (dir, n) => (dir === "/" ? "/" + n : dir + "/" + n);
const parent = (p) => (p === "/" || p === "/sd" ? "" : p.slice(0, p.lastIndexOf("/")) || "/");
keyboard.capture(true);  // Escape comes here
function done(msg, patch) {
  try { const s = JSON.parse(fs.read(S)); s.msg = msg; Object.assign(s, patch); fs.write(S, JSON.stringify(s)); } catch (e) {}
  launch("/files.js");
}
function busy(t) { lcd.fillScreen(BG); lcd.setTextColor(lcd.WHITE); lcd.setTextDatum(lcd.TL); lcd.drawString(t, 8, H / 2 - TH, F); }

function removeTree(p) {  // (a page at a time: a big folder listed whole runs a small board out of memory)
  for (let page = fs.dir(p, 0, 40); page.length; page = fs.dir(p, 0, 40))
    for (const e of page) if (!(e.dir ? removeTree(join(p, e.name)) : fs.remove(join(p, e.name)))) return false;
  return fs.rmdir(p);
}
function copyTree(from, to) {
  if (!fs.stat(from).dir) return fs.copy(from, to);
  if (!fs.stat(to) && !fs.mkdir(to)) return false;
  for (let off = 0, page = fs.dir(from, 0, 40); page.length; off += 40, page = fs.dir(from, off, 40))
    for (const e of page) if (!copyTree(join(from, e.name), join(to, e.name))) return false;
  return true;
}
function paste(clip, dir) {
  let to = join(dir, clip.name);
  if (to === clip.from && clip.cut) return done("", { clip: null });
  for (let n = 2; fs.exists(to); n++) {  // never overwrite: "name (2).ext"
    const dot = clip.dir || clip.name.lastIndexOf(".") <= 0 ? clip.name.length : clip.name.lastIndexOf(".");
    to = join(dir, clip.name.slice(0, dot) + ` (${n})` + clip.name.slice(dot));
  }
  if (clip.cut && to.startsWith(clip.from + "/")) return done("Can't move a folder into itself");
  busy((clip.cut ? "Moving " : "Copying ") + clip.name + "...");
  const sameDrive = clip.from.startsWith("/sd") === dir.startsWith("/sd");
  let ok = clip.cut && sameDrive ? fs.rename(clip.from, to) : copyTree(clip.from, to);
  if (ok && clip.cut && !sameDrive) ok = fs.stat(clip.from).dir ? removeTree(clip.from) : fs.remove(clip.from);
  done(ok ? (clip.cut ? "Moved " : "Copied ") + clip.name : "Failed - is there room?", clip.cut ? { clip: null } : {});
}
function rename(p, n) {
  if (!n || n.includes("/")) return done("Not a valid name");
  const to = join(parent(p) || "/", n);
  if (fs.exists(to)) return done(n + " already exists");
  done(fs.rename(p, to) ? "Renamed to " + n : "Couldn't rename");
}
function mkdir(dir, n) {
  if (!n || n.includes("/")) return done("Not a valid name");
  done(fs.mkdir(join(dir, n)) ? "Made " + n : "Couldn't make " + n, { name: n });
}

// ---- the name prompt
let prompt = null;
function drawPrompt() {
  const y = H / 2 - LH * 2;
  lcd.fillScreen(BG);
  lcd.fillRoundRect(6, y, W - 12, LH * 3 + 8, 6, PANEL);
  lcd.drawRoundRect(6, y, W - 12, LH * 3 + 8, 6, lcd.LIGHTGREY);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(DIR);
  lcd.drawString(prompt.title, 12, y + 4, F);
  lcd.fillRect(12, y + LH + 6, W - 24, LH + 2, BG);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(prompt.text.slice(-Math.floor((W - 30) / CHW)) + "_", 14, y + LH + 7, F);
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString("Enter OK   Escape cancel", 12, y + LH * 2 + 10, 1);
}
function ask(title, text, fn) {
  prompt = { title, text, fn };
  drawPrompt();
  keyboard.on("key", ({ key: k }) => {
    if (k === "Enter") fn(prompt.text.trim());
    else if (k === "Escape") done("");
    else if (k === "Backspace") { prompt.text = prompt.text.slice(0, -1); drawPrompt(); }
    else if (k.length === 1) { prompt.text += k; drawPrompt(); }
  });
}

if (A.op === "delete") {
  busy("Deleting " + A.name + "...");
  done((fs.stat(A.p).dir ? removeTree(A.p) : fs.remove(A.p)) ? "Deleted " + A.name : "Couldn't delete " + A.name);
} else if (A.op === "paste") paste(A.clip, A.to);
else if (A.op === "rename") ask("Rename to:", A.name, (n) => rename(A.p, n));
else if (A.op === "mkdir") {
  if (board.hasKeyboard) ask("New folder name:", "", (n) => mkdir(A.dir, n));
  else { let n = 1; while (fs.exists(join(A.dir, "folder" + n))) n++; mkdir(A.dir, "folder" + n); }
}
