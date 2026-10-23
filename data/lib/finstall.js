// The file manager's firmware install screens: fmenu.js launches this with the path of a .FLH / .bin on the SD
// card. It checks the file, shows its notes, and installs into the spare slot on Y (a tap on Install); the
// board restarts when that works. (A launched app has the clean heap that writing the slot needs: the
// updater wants a 4 KB block.) Back, N or Escape return to the file manager.
const P = ARG, S = "/files.state";
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, TH = SMALL ? 11 : 18, TOP = TH + 4, F = SMALL ? 1 : 2, CHW = F === 1 ? 6 : 8;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48);
const name = P.slice(P.lastIndexOf("/") + 1);
const fit = (s, px) => { const n = Math.floor(px / CHW); return s.length > n ? s.slice(0, Math.max(1, n - 1)) + "~" : s; };
keyboard.capture(true);  // Escape comes here
function back() { launch("/files.js"); }
function header(t) {
  lcd.fillRect(0, 0, W, TOP - 2, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(fit(t, W - 8), 4, 2, F);
}
lcd.fillScreen(BG);
header("Checking " + name + "...");
const info = sys.firmwareInfo(P);
function draw() {
  header(name);
  lcd.fillRect(0, TOP - 2, W, H - TOP + 2, BG);
  const chars = Math.floor(W / CHW) - 1, maxLines = Math.floor((H - 4 * TH) / TH);
  let y = TH + 8, lines = 0;
  lcd.setTextDatum(lcd.TL);
  const para = (text, color) => {  // word-wrapped, in a colour
    lcd.setTextColor(color);
    for (const p of text.split("\n")) {
      let line = p.trim();
      while (line.length && lines < maxLines) {
        let cut = line.length > chars ? line.lastIndexOf(" ", chars) : line.length;
        if (cut <= 0) cut = chars;
        lcd.drawString(line.slice(0, cut), 4, y, F);
        line = line.slice(cut).trim();
        y += TH; lines++;
      }
    }
  };
  para(info.ok ? `valid, ${Math.round(info.size / 1024)} KB` : info.error, info.ok ? lcd.GREEN : lcd.RED);
  y += 2;
  para(info.notes || "", lcd.LIGHTGREY);
  lcd.setTextColor(lcd.YELLOW);
  if (!board.hasTouch) return lcd.drawString(info.ok ? "Install this firmware?  Y / N" : "N or Escape: back", 4, H - TH, F);
  const bh = TH + 8;
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
  header("Installing " + name);
  lcd.setTextColor(lcd.WHITE);
  lcd.setTextDatum(lcd.MC);
  lcd.drawString("Writing firmware - don't power off", W / 2, H / 2 - 4, F);
  const err = sys.installFirmware(P);  // restarts the board when it works
  lcd.fillRect(0, H / 2 - 14, W, 40, BG);
  lcd.setTextColor(lcd.RED);
  lcd.drawString(fit("Failed: " + err, W - 8), W / 2, H / 2, 1);
  lcd.setTextDatum(lcd.TL);
  tone(200, 300);
  setTimeout(back, 4000);
}
function key(k) {
  if ((k === "y" || k === "Y") && info.ok) install();
  else if (k === "n" || k === "N" || k === "Escape") back();
}
keyboard.on("key", ({ key: k }) => key(k));
let pt = { x: 0, y: 0 };  // where the finger is (the "up" event doesn't say)
touch.on("down", (e) => { pt = e; });
touch.on("move", (e) => { pt = e; });
touch.on("up", () => { if (pt.y > H - TH - 14) key(pt.x < W / 2 ? "y" : "n"); });
draw();
