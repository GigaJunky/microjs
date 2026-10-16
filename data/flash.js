// Firmware menu: install a firmware file from the SD card into the board's spare firmware slot.
//   .FLH  the ESP32-CoCo emulator's format, so you can switch between micro-js and the CoCo
//         emulators (and back from the emulator's own Flash menu, with MICROJS_<version>.FLH on the card)
//   .bin  a firmware.bin, or a full flash image (only its firmware part is installed)
// Files in folders on the card (up to three deep) are listed too.
// The scripts on the board's flash are kept. Boards without a spare slot (the classic CYD) can't.
// Keys: up / down choose, Enter (or Space / button 1) shows the file, Y installs, N or Escape goes back.
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2, LH = SMALL ? 11 : 18;
const isFirmware = (f) => [".FLH", ".BIN"].includes(f.slice(-4).toUpperCase());
const SKIP = ["System Volume Information/", "FOUND.000/", "LOST.DIR/"];
const files = [];  // paths under /sd, folders included ("coco/CC3.FLH")
(function scan(dir, depth) {
  let names = [];
  try { names = fs.list("/sd" + (dir ? "/" + dir : "")); } catch (e) {}
  for (const n of names.sort()) {
    if (n.endsWith("/")) {
      if (depth < 3 && !n.startsWith(".") && !SKIP.includes(n)) scan(dir + n.slice(0, -1) + "/", depth + 1);
    } else if (isFirmware(n)) files.push(dir + n);
  }
})("", 0);
let pick = 0, top = 0, info = null, rows = Math.floor((H - 2 * LH - 8) / LH);
keyboard.capture(true);  // Escape comes here (back out of a file) instead of to the menu

function header(t) {
  lcd.fillScreen(lcd.BLACK);
  lcd.fillRect(0, 0, W, LH + 2, lcd.NAVY);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(t, 4, 2, F);
}

function drawList() {
  header("Install firmware from SD");
  if (!files.length) {
    lcd.setTextColor(lcd.LIGHTGREY);
    lcd.drawString(board.hasSd === false ? "No SD card." : "No .FLH or .bin files on the SD card.", 4, LH * 2, F);
    lcd.drawString(board.hasTouch ? "Tap to go back to the menu" : "Escape: back to the menu", 4, LH * 3, F);
    return;
  }
  if (pick < top) top = pick;
  if (pick >= top + rows) top = pick - rows + 1;
  for (let i = top; i < Math.min(files.length, top + rows); i++) {
    const y = LH + 6 + (i - top) * LH;
    if (i === pick) lcd.fillRect(0, y - 1, W, LH, lcd.DARKGREEN);
    lcd.setTextColor(i === pick ? lcd.WHITE : lcd.LIGHTGREY);
    lcd.drawString(files[i], 6, y, F);
  }
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString(board.hasTouch ? "Tap a file   Back: tap here" : "Enter: details   Escape: back", 4, H - LH, F);
}

// word-wrap the release notes into the space above the prompt
function drawInfo() {
  header(files[pick]);
  const chars = Math.floor(W / (F === 1 ? 6 : 8)) - 1, maxLines = Math.floor((H - 4 * LH) / LH);
  let y = LH + 6, lines = 0;
  lcd.setTextColor(info.ok ? lcd.GREEN : lcd.RED);
  lcd.drawString(info.ok ? `valid, ${Math.round(info.size / 1024)} KB` : info.error, 4, y, F);
  y += LH + 2;
  lcd.setTextColor(lcd.LIGHTGREY);
  for (const para of info.notes.split("\n")) {
    let line = para.trim();
    while (line.length && lines < maxLines) {
      let cut = line.length > chars ? line.lastIndexOf(" ", chars) : line.length;
      if (cut <= 0) cut = chars;
      lcd.drawString(line.slice(0, cut), 4, y, F);
      line = line.slice(cut).trim();
      y += LH; lines++;
    }
  }
  lcd.setTextColor(lcd.YELLOW);
  if (!board.hasTouch) return lcd.drawString(info.ok ? "Install this firmware?  Y / N" : "N or Escape: back", 4, H - LH, F);
  const bh = LH + 8;  // touch: Install / Back buttons along the bottom
  if (info.ok) {
    lcd.fillRoundRect(4, H - bh - 2, W / 2 - 8, bh, 6, lcd.DARKGREEN);
    lcd.setTextDatum(lcd.MC);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString("Install", W / 4, H - bh / 2 - 2, F);
  }
  lcd.fillRoundRect(W / 2 + 4, H - bh - 2, W / 2 - 8, bh, 6, lcd.MAROON);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString("Back", (W * 3) / 4, H - bh / 2 - 2, F);
  lcd.setTextDatum(lcd.TL);
}

function install() {
  header("Installing " + files[pick]);
  lcd.setTextColor(lcd.WHITE);
  lcd.setTextDatum(lcd.MC);
  lcd.drawString("Writing firmware - don't power off", W / 2, H / 2 - 4, F);
  const err = sys.installFirmware("/sd/" + files[pick]);  // restarts the board when it works
  lcd.fillRect(0, H / 2 - 14, W, 40, lcd.BLACK);
  lcd.setTextColor(lcd.RED);
  lcd.drawString("Failed: " + err, W / 2, H / 2, SMALL ? 1 : 2);
  lcd.setTextDatum(lcd.TL);
  tone(200, 300);
  info = null;
  setTimeout(drawList, 3000);
}

function key(k) {
  if (info) {
    if ((k === "y" || k === "Y") && info.ok) install();
    else if (k === "n" || k === "N" || k === "Escape") { info = null; drawList(); }
    return;
  }
  if (k === "Escape") return launch("/main.js");
  if (!files.length) return;
  if (k === "ArrowUp" || k === ";") pick = (pick + files.length - 1) % files.length;
  else if (k === "ArrowDown" || k === ".") pick = (pick + 1) % files.length;
  else if (k === "Enter" || k === " ") {
    header("Checking " + files[pick] + "...");
    info = sys.firmwareInfo("/sd/" + files[pick]);
    return drawInfo();
  } else return;
  drawList();
}

keyboard.on("key", ({ key: k }) => key(k));
touch.on("down", ({ x, y }) => {
  if (info) return key(y > H - LH - 12 ? (x < W / 2 ? "y" : "n") : "");
  if (!files.length || y > H - LH - 4) return key("Escape");
  const i = top + Math.floor((y - LH - 6) / LH);
  if (y > LH + 4 && i >= top && i < Math.min(files.length, top + rows)) { pick = i; key("Enter"); }
});
drawList();
