// Launcher: shows every .js file on the flash and on the SD card. Tap a button to run it; on
// boards with a keyboard and no touch (Cardputer) it's a list: arrows (or ; and .) to move,
// Enter to run, 1-9 to run an item directly, left/right (or , and /) to change page.
// Press BOOT at any time to stop a script and come back here (hold BOOT while resetting to skip it).
// Sound: the speaker button (top right) mutes, - and + set the volume; with a keyboard m mutes and
// - / = set the volume. The setting is kept in /sound.json and applies to every app.
// Boards with a battery show its charge next to the title (updated every 10 seconds), with a
// lightning bolt (or a +) while it charges from a computer's USB port.
// Hide apps you don't use: "edit list" (the last entry) ticks each app shown or hidden, and
// "+ N hidden" shows the hidden ones (in orange) for a while; kept in /launcher.json. A board too
// small for the big apps (@needs-more-memory: the classic CYD) starts with those hidden; apps that
// can't run at all on a board with little memory (@needs-ram) aren't listed there.
const W = lcd.width(), H = lcd.height();

// ---- sound setting
let sound = { volume: 100, muted: false };
try { const t = fs.read("/sound.json"); if (t) sound = Object.assign(sound, JSON.parse(t)); } catch (e) {}
audio.volume(sound.volume);
audio.mute(sound.muted);
function setSound(volume, muted) {
  sound.volume = Math.max(0, Math.min(100, volume));
  sound.muted = muted;
  audio.volume(sound.volume);
  audio.mute(sound.muted);
  fs.write("/sound.json", JSON.stringify(sound));
  if (!sound.muted) tone(1200, 25);  // a click at the new level
}
const soundLabel = () => (sound.muted ? "muted" : sound.volume + "%");

// ---- battery
let battery = board.hasBattery ? sys.battery() : null;
const batteryColor = (p) => (p > 50 ? lcd.GREEN : p > 20 ? lcd.YELLOW : lcd.RED);
// a battery outline filled to the charge, then the percentage; returns the width used
function drawBattery(x, y, font) {
  if (!battery) return 0;
  const p = battery.percent, c = batteryColor(p);
  lcd.drawRect(x, y, 20, 10, lcd.LIGHTGREY);
  lcd.fillRect(x + 20, y + 3, 2, 4, lcd.LIGHTGREY);
  lcd.fillRect(x + 2, y + 2, Math.max(1, Math.round((16 * p) / 100)), 6, c);
  if (battery.charging) {  // a lightning bolt over the battery
    lcd.fillTriangle(x + 11, y - 1, x + 6, y + 6, x + 10, y + 5, lcd.YELLOW);
    lcd.fillTriangle(x + 9, y + 11, x + 14, y + 4, x + 10, y + 5, lcd.YELLOW);
  }
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(c, lcd.BLACK);
  lcd.drawString(p + "%", x + 25, y + 5, font);
  return 25 + lcd.textWidth(p + "%", font);
}
// the speaker symbol, crossed out when muted
function speaker(x, y, c) {
  lcd.fillRect(x, y + 6, 5, 8, c);
  lcd.fillTriangle(x + 4, y + 10, x + 11, y + 2, x + 11, y + 18, c);
  if (sound.muted) {
    lcd.drawLine(x + 14, y + 6, x + 21, y + 13, lcd.RED);
    lcd.drawLine(x + 21, y + 6, x + 14, y + 13, lcd.RED);
    lcd.drawLine(x + 15, y + 6, x + 22, y + 13, lcd.RED);
    lcd.drawLine(x + 22, y + 6, x + 15, y + 13, lcd.RED);
  } else {
    lcd.drawArc(x + 11, y + 10, 6, 5, 230, 310, c, lcd.BLACK, false);
    lcd.drawArc(x + 11, y + 10, 10, 9, 225, 315, c, lcd.BLACK, false);
  }
}
const TOUCH_ONLY = ["controls.js", "calibrate.js"];  // hidden on boards without a touch screen
const isScript = (f) => f.endsWith(".js") && f !== "main.js" && (board.hasTouch || !TOUCH_ONLY.includes(f));
// apps marked @needs-wifi don't show on a board without WiFi, @needs-esp32 on other chips (they
// can't work there)
const isEsp = !board.chip || board.chip.startsWith("esp32");
// @needs-network apps only fetch from the web, which also works through a computer (board.network "usb")
// @needs-ram apps (the dungeon and shooting games) want about 135 KB to themselves - free heap and PSRAM
// together, as board.ramKB measures at boot - and fail with "out of memory" on a board with less (the
// Cardputer), so they're left out there
const roomKB = board.ramKB || 0;
const usable = (sc) => (roomKB >= 135 || !fs.contains(sc.path, "@needs-ram")) &&
  (board.hasWifi !== false || !fs.contains(sc.path, "@needs-wifi")) &&
  (board.hasWifi !== false || board.network || !fs.contains(sc.path, "@needs-network")) &&
  (isEsp || !fs.contains(sc.path, "@needs-esp32"));
// An app on the SD card with the same name as one in flash is left out (usually an older copy); on a
// small board every extra entry costs memory the menu may not have.
const flashApps = fs.list("/").filter(isScript).sort();
const allScripts = [
  ...flashApps.map((f) => ({ name: f, path: "/" + f, sd: false })),
  ...fs.list("/sd").filter((f) => isScript(f) && !flashApps.includes(f)).sort().map((f) => ({ name: f, path: "/sd/" + f, sd: true })),
].filter(usable);
// big apps on a small board (fs.contains searches without loading the file)
if (board.lowMemory) for (const sc of allScripts) sc.big = fs.contains(sc.path, "@needs-more-memory");

// ---- which apps show: /launcher.json { hidden: [paths], showHidden }. You choose with "edit list";
// until then a small board hides the big apps.
let prefs = null;
try { prefs = JSON.parse(fs.read("/launcher.json") || "null"); } catch (e) {}
if (!prefs || !Array.isArray(prefs.hidden)) prefs = { hidden: allScripts.filter((sc) => sc.big).map((sc) => sc.path), showHidden: false };
const isHidden = (sc) => prefs.hidden.includes(sc.path);
const savePrefs = () => fs.write("/launcher.json", JSON.stringify(prefs));
// ---- the list editor is its own app (lib/listedit.js), so the menu stays small: it gets the app
// list as ARG and comes back here when done
const EDITOR = ["/lib/listedit.js", "/sd/lib/listedit.js"].find((p) => fs.exists(p));
function openEditor() {
  tone(1000, 20);
  launch(EDITOR, JSON.stringify(allScripts.map(({ name, path, sd, big }) => ({ name, path, sd, big }))));
}

// a "flash" entry (install firmware from the SD card) where there is a spare firmware slot: the S3 boards, not the classic ESP32 CYDs
const CAN_FLASH = isEsp && board.chip && board.chip !== "esp32" && board.hasSd !== false && flashApps.includes("files.js");
let scripts = [];
function applyFilter() {
  const nHidden = allScripts.filter(isHidden).length;
  scripts = allScripts.filter((sc) => prefs.showHidden || !isHidden(sc)).map((sc) => ({ ...sc, hidden: isHidden(sc) }));
  if (CAN_FLASH) scripts.push({ name: "flash", path: "/files.js", arg: "flash" });  // the file manager showing only firmware files
  if (nHidden) scripts.push({ name: prefs.showHidden ? "hide hidden apps" : `+ ${nHidden} hidden`, toggle: true });
  if (EDITOR) scripts.push({ name: "edit list", edit: true });
}
applyFilter();
function redraw() { if (LIST) drawList(); else drawPage(); }
function toggleHidden() {
  prefs.showHidden = !prefs.showHidden;
  savePrefs();
  tone(1000, 20);
  applyFilter();
  sel = 0;
  page = 0;
  redraw();
}

const COLS = 2, BW = (W - 30) / COLS, BH = 32, GAP = 6, TOP = 40, NAV = 34;
const ROWS = Math.floor((H - TOP - NAV) / (BH + GAP));
const PER_PAGE = COLS * ROWS;
const pageCount = () => Math.max(1, Math.ceil(scripts.length / PER_PAGE));
let page = 0, buttons = [];

function drawButton(b, pressed) {
  const extra = b.toggle || b.edit;
  const fill = pressed ? lcd.ORANGE : extra ? lcd.color(45, 45, 55) : b.hidden ? lcd.color(50, 30, 30) : b.sd ? lcd.DARKGREEN : lcd.NAVY;
  lcd.fillRoundRect(b.x, b.y, BW, BH, 8, fill);
  lcd.drawRoundRect(b.x, b.y, BW, BH, 8, b.hidden ? lcd.ORANGE : extra ? lcd.DARKGREY : b.sd ? lcd.GREENYELLOW : lcd.SKYBLUE);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(b.name.replace(".js", ""), b.x + BW / 2, b.y + BH / 2, 2);
  if (b.sd) {
    lcd.setTextColor(lcd.GREENYELLOW);
    lcd.drawString("SD", b.x + BW - 14, b.y + BH / 2, 1);
  }
}

const VERSION = sys.version ? "v" + sys.version : "";
function drawNav() {
  const y = H - NAV + 4;
  lcd.fillRect(0, H - NAV, W, NAV, lcd.BLACK);
  lcd.setTextDatum(lcd.BC);  // the firmware version, small, along the bottom
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString(VERSION, W / 2, H, 1);
  const pages = pageCount();
  if (pages < 2) return;
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  if (page > 0) {
    lcd.fillRoundRect(10, y, 80, NAV - 8, 6, lcd.DARKGREY);
    lcd.drawString("< prev", 50, y + (NAV - 8) / 2, 2);
  }
  if (page < pages - 1) {
    lcd.fillRoundRect(W - 90, y, 80, NAV - 8, 6, lcd.DARKGREY);
    lcd.drawString("next >", W - 50, y + (NAV - 8) / 2, 2);
  }
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString(`${page + 1} / ${pages}`, W / 2, y + (NAV - 8) / 2 - 3, 2);
}

// sound controls in the header: [speaker] [-] 80% [+]
const SB = { mute: [W - 136, 30], minus: [W - 102, 26], label: [W - 74, 42], plus: [W - 30, 26] };
function drawSound() {
  lcd.fillRect(SB.mute[0] - 2, 2, W - SB.mute[0], 32, lcd.BLACK);
  lcd.fillRoundRect(SB.mute[0], 4, SB.mute[1], 28, 6, sound.muted ? lcd.MAROON : lcd.color(40, 45, 80));
  speaker(SB.mute[0] + 5, 8, lcd.WHITE);
  lcd.setTextDatum(lcd.MC);
  for (const k of ["minus", "plus"]) {
    lcd.fillRoundRect(SB[k][0], 4, SB[k][1], 28, 6, lcd.color(40, 45, 80));
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString(k === "minus" ? "-" : "+", SB[k][0] + SB[k][1] / 2, 18, 4);
  }
  lcd.setTextColor(sound.muted ? lcd.RED : lcd.LIGHTGREY);
  lcd.drawString(soundLabel(), SB.label[0] + SB.label[1] / 2, 18, sound.muted ? 1 : 2);
}
// the battery, between the title and the sound buttons
const BAT_X = W >= 300 ? 128 : 44;
function drawBatteryTop() {
  if (!battery) return;
  lcd.fillRect(BAT_X, 6, SB.mute[0] - BAT_X - 2, 24, lcd.BLACK);
  drawBattery(BAT_X, 13, 2);
}
function soundTouch(x) {
  if (x >= SB.mute[0] && x < SB.mute[0] + SB.mute[1]) setSound(sound.volume, !sound.muted);
  else if (x >= SB.minus[0] && x < SB.minus[0] + SB.minus[1]) setSound(sound.volume - 10, false);
  else if (x >= SB.plus[0] && x < SB.plus[0] + SB.plus[1]) setSound(sound.volume + 10, false);
  else return false;
  drawSound();
  return true;
}

function drawPage() {
  lcd.fillScreen(lcd.BLACK);
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(lcd.YELLOW);
  lcd.drawString(W >= 300 ? "micro-js" : "JS", 8, 18, 4);
  drawSound();
  drawBatteryTop();
  buttons = scripts.slice(page * PER_PAGE, (page + 1) * PER_PAGE).map((s, i) => ({
    ...s,
    x: 10 + (i % COLS) * (BW + 10),
    y: TOP + Math.floor(i / COLS) * (BH + GAP),
  }));
  buttons.forEach((b) => drawButton(b, false));
  if (!scripts.length) {
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString("no scripts found - upload some!", W / 2, H / 2, 2);
  }
  drawNav();
}

function run(b) {
  if (b.toggle) return toggleHidden();
  if (b.edit) return openEditor();
  drawButton(b, true);
  tone(1500, 30);
  if (b.arg) launch(b.path, b.arg); else launch(b.path);  // starts the script in a fresh interpreter once this handler returns
}

const hit = (b, x, y) => x >= b.x && x < b.x + BW && y >= b.y && y < b.y + BH;
touch.on("down", ({ x, y }) => {
  if (y < TOP - 4 && soundTouch(x)) return;
  const pages = pageCount();
  if (y >= H - NAV && pages > 1) {
    if (x < W / 3 && page > 0) page--;
    else if (x > (W * 2) / 3 && page < pages - 1) page++;
    else return;
    tone(1200, 15);
    drawPage();
    return;
  }
  const b = buttons.find((b) => hit(b, x, y));
  if (b) run(b);
});

// ---- keyboard list mode (no touch screen, or too small for buttons)
const LIST = !board.hasTouch || H < 200;
const ROW = 18, LTOP = 22, PER_LIST = Math.floor((H - LTOP - 12) / ROW);
const listPages = () => Math.max(1, Math.ceil(scripts.length / PER_LIST));
let sel = 0;

function drawList() {
  lcd.fillScreen(lcd.BLACK);
  lcd.fillRect(0, 0, W, LTOP - 4, lcd.NAVY);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.YELLOW);
  lcd.drawString("micro-js", 4, 2, 2);
  let tx = 8 + lcd.textWidth("micro-js", 2);
  if (W >= 300 && sys.version) {  // the version, where there's room
    lcd.setTextColor(lcd.DARKGREY);
    lcd.drawString("v" + sys.version, tx, 6, 1);
    tx += lcd.textWidth("v" + sys.version, 1) + 6;
  }
  if (battery) {
    lcd.setTextColor(batteryColor(battery.percent));
    lcd.drawString(battery.percent + "%" + (battery.charging ? "+" : ""), tx, 2, 2);
  }
  lcd.setTextDatum(lcd.TR);
  lcd.setTextColor(lcd.LIGHTGREY);
  const pageText = `${Math.floor(sel / PER_LIST) + 1}/${listPages()}`;
  lcd.drawString(pageText, W - 4, 2, 2);
  lcd.setTextColor(sound.muted ? lcd.RED : lcd.LIGHTGREY);
  lcd.drawString("vol " + soundLabel(), W - 16 - lcd.textWidth(pageText, 2), 2, 2);
  const first = Math.floor(sel / PER_LIST) * PER_LIST;
  scripts.slice(first, first + PER_LIST).forEach((sc, i) => {
    const y = LTOP + i * ROW, on = first + i === sel;
    if (on) lcd.fillRect(0, y - 1, W, ROW, lcd.DARKCYAN);
    lcd.setTextDatum(lcd.TL);
    lcd.setTextColor(sc.toggle || sc.edit ? lcd.DARKGREY : sc.hidden ? lcd.ORANGE : on ? lcd.WHITE : lcd.LIGHTGREY);
    lcd.drawString(`${i + 1}  ${sc.name.replace(".js", "")}`, 6, y, 2);
    if (sc.sd) {
      lcd.setTextDatum(lcd.TR);
      lcd.setTextColor(lcd.GREENYELLOW);
      lcd.drawString("SD", W - 6, y + 4, 1);
    }
  });
  if (!scripts.length) lcd.drawString("no scripts found", 6, LTOP, 2);
  lcd.setTextDatum(lcd.BL);
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString(W >= 300 ? "; . move  Enter run  , / page  m mute  - = volume" : ";. move Enter run ,/ page m mute -= vol", 4, H - 1, 1);
}

function runList(i) {
  if (!scripts[i]) return;
  if (scripts[i].toggle) return toggleHidden();
  if (scripts[i].edit) return openEditor();
  tone(1500, 30);
  if (scripts[i].arg) launch(scripts[i].path, scripts[i].arg); else launch(scripts[i].path);
}

if (board.hasBattery)
  setInterval(() => {
    battery = sys.battery();
    if (LIST) drawList(); else drawBatteryTop();
  }, 10000);

if (LIST) {
  keyboard.on("key", ({ key }) => {
    if (key === "m" || key === "M") { setSound(sound.volume, !sound.muted); return drawList(); }
    if (key === "-" || key === "_") { setSound(sound.volume - 10, false); return drawList(); }
    if (key === "=" || key === "+") { setSound(sound.volume + 10, false); return drawList(); }
    const n = scripts.length;
    if (!n) return;
    if (key === "ArrowDown" || key === ".") sel = (sel + 1) % n;
    else if (key === "ArrowUp" || key === ";") sel = (sel + n - 1) % n;
    else if (key === "ArrowRight" || key === "/") sel = Math.min(n - 1, sel + PER_LIST);
    else if (key === "ArrowLeft" || key === ",") sel = Math.max(0, sel - PER_LIST);
    else if (key === "Enter" || key === " ") return runList(sel);  // Space: a gamepad's button 1
    else if (key >= "1" && key <= "9") return runList(Math.floor(sel / PER_LIST) * PER_LIST + (+key - 1));
    else return;
    drawList();
  });
  drawList();
} else drawPage();
