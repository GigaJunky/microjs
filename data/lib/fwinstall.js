// Firmware install screens for the file manager (files.js loads this when "Install firmware..." is chosen,
// so the file manager itself stays small): check the file, show its notes, install into the spare slot on Y.
let fw = null;
function firmware(p, it) {
  header("Checking " + it.name + "...");
  fw = { name: it.name, path: p, info: sys.firmwareInfo(p) };
  mode = "fw";
  drawFw();
}
function drawFw() {
  header(fw.name);
  lcd.fillRect(0, TOP - 2, W, H - TOP + 2, BG);
  const info = fw.info, chars = Math.floor(W / CHW) - 1, maxLines = Math.floor((H - 4 * TH) / TH);
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
function installFw() {
  header("Installing " + fw.name);
  lcd.setTextColor(lcd.WHITE);
  lcd.setTextDatum(lcd.MC);
  lcd.drawString("Writing firmware - don't power off", W / 2, H / 2 - 4, F);
  const err = sys.installFirmware(fw.path);  // restarts the board when it works
  lcd.fillRect(0, H / 2 - 14, W, 40, BG);
  lcd.setTextColor(lcd.RED);
  lcd.drawString("Failed: " + err, W / 2, H / 2, SMALL ? 1 : 2);
  lcd.setTextDatum(lcd.TL);
  tone(200, 300);
  setTimeout(() => { fw = null; mode = "list"; draw(); }, 3000);
}
