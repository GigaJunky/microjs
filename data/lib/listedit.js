// The menu's list editor ("edit list" in main.js): every app with a box - ticked shows it in the
// menu, crossed hides it. Kept in /launcher.json { hidden: [paths], showHidden }.
// It runs as its own app so the menu stays small (the classic CYD has little memory to spare). The
// menu starts it with the app list as ARG - JSON [{ name, path, sd, big }] - and "Done" (or
// Backspace) goes back to the menu.
const W = lcd.width(), H = lcd.height();
const MENU = fs.exists("/sd/main.js") ? "/sd/main.js" : "/main.js";
let apps = [];
try { apps = JSON.parse(typeof ARG === "string" && ARG ? ARG : "[]"); } catch (e) {}
let prefs = null;
try { prefs = JSON.parse(fs.read("/launcher.json") || "null"); } catch (e) {}
if (!prefs || !Array.isArray(prefs.hidden)) prefs = { hidden: apps.filter((a) => a.big).map((a) => a.path), showHidden: false };
const isHidden = (a) => prefs.hidden.includes(a.path);

const LIST_MODE = !board.hasTouch || H < 200;
const ER = LIST_MODE ? 18 : 26, ETOP = 24, EBOT = LIST_MODE ? 12 : 34;
const per = Math.max(1, Math.floor((H - ETOP - EBOT) / ER));
const pages = Math.max(1, Math.ceil(apps.length / per));
let esel = 0;

const save = () => fs.write("/launcher.json", JSON.stringify(prefs));  // at once: BOOT also leaves
function done() {
  tone(1200, 20);
  launch(MENU);
}
function flip(i) {
  const a = apps[i];
  if (!a) return;
  if (isHidden(a)) prefs.hidden = prefs.hidden.filter((p) => p !== a.path);
  else prefs.hidden.push(a.path);
  save();
  tone(isHidden(a) ? 600 : 1100, 15);
  draw();
}

function draw() {
  lcd.fillScreen(lcd.BLACK);
  lcd.fillRect(0, 0, W, ETOP - 4, lcd.NAVY);
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(lcd.YELLOW);
  lcd.drawString("Show which apps?", 4, (ETOP - 4) / 2, 2);
  if (!LIST_MODE) {  // Done button
    lcd.fillRoundRect(W - 64, 1, 60, ETOP - 6, 5, lcd.DARKGREEN);
    lcd.setTextDatum(lcd.MC);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString("Done", W - 34, (ETOP - 4) / 2, 2);
  }
  const first = Math.floor(esel / per) * per;
  apps.slice(first, first + per).forEach((a, k) => {
    const i = first + k, y = ETOP + k * ER, shown = !isHidden(a), bx = 6, by = y + (ER - 14) / 2;
    if (LIST_MODE && i === esel) lcd.fillRect(0, y, W, ER, lcd.color(30, 60, 90));
    if (shown) {
      lcd.fillRoundRect(bx, by, 14, 14, 3, lcd.color(30, 150, 70));
      lcd.drawLine(bx + 3, by + 7, bx + 6, by + 10, lcd.WHITE);
      lcd.drawLine(bx + 6, by + 10, bx + 11, by + 3, lcd.WHITE);
    } else {
      lcd.drawRoundRect(bx, by, 14, 14, 3, lcd.color(200, 60, 60));
      lcd.drawLine(bx + 3, by + 3, bx + 10, by + 10, lcd.color(200, 60, 60));
      lcd.drawLine(bx + 10, by + 3, bx + 3, by + 10, lcd.color(200, 60, 60));
    }
    lcd.setTextDatum(lcd.ML);
    lcd.setTextColor(shown ? lcd.WHITE : lcd.DARKGREY);
    lcd.drawString(a.name.replace(".js", ""), 28, y + ER / 2, 2);
    lcd.setTextDatum(lcd.MR);
    if (a.big) { lcd.setTextColor(lcd.ORANGE); lcd.drawString(a.sd ? "big  SD" : "big", W - 8, y + ER / 2, 1); }
    else if (a.sd) { lcd.setTextColor(lcd.GREENYELLOW); lcd.drawString("SD", W - 8, y + ER / 2, 1); }
  });
  if (!apps.length) {
    lcd.setTextDatum(lcd.MC);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString("no apps - start this from the menu", W / 2, H / 2, 2);
  }
  const pg = Math.floor(esel / per);
  lcd.setTextColor(lcd.DARKGREY);
  if (LIST_MODE) {
    lcd.setTextDatum(lcd.BL);
    lcd.drawString("; . move  Enter show/hide  , / page  Bksp done", 4, H - 1, 1);
  } else if (pages > 1) {
    lcd.setTextDatum(lcd.MC);
    const y = H - EBOT / 2;
    if (pg > 0) { lcd.fillRoundRect(10, y - 13, 80, 26, 6, lcd.DARKGREY); lcd.setTextColor(lcd.WHITE); lcd.drawString("< prev", 50, y, 2); }
    if (pg < pages - 1) { lcd.fillRoundRect(W - 90, y - 13, 80, 26, 6, lcd.DARKGREY); lcd.setTextColor(lcd.WHITE); lcd.drawString("next >", W - 50, y, 2); }
    lcd.setTextColor(lcd.LIGHTGREY);
    lcd.drawString(`${pg + 1} / ${pages}`, W / 2, y, 2);
  }
  lcd.setTextDatum(lcd.TL);
}

touch.on("down", ({ x, y }) => {
  if (y < ETOP - 2) { if (x > W - 70) done(); return; }
  const pg = Math.floor(esel / per);
  if (y >= H - EBOT) {
    if (x < W / 3 && pg > 0) esel = (pg - 1) * per;
    else if (x > (W * 2) / 3 && pg < pages - 1) esel = (pg + 1) * per;
    else return;
    return draw();
  }
  const k = Math.floor((y - ETOP) / ER);
  if (k >= 0 && k < per) flip(pg * per + k);
});

keyboard.on("key", ({ key }) => {
  const n = apps.length;
  if (key === "Backspace" || key === "q") return done();
  if (!n) return;
  if (key === "ArrowDown" || key === ".") esel = (esel + 1) % n;
  else if (key === "ArrowUp" || key === ";") esel = (esel + n - 1) % n;
  else if (key === "ArrowRight" || key === "/") esel = Math.min(n - 1, esel + per);
  else if (key === "ArrowLeft" || key === ",") esel = Math.max(0, esel - per);
  else if (key === "Enter" || key === " ") return flip(esel);
  else return;
  draw();
});

draw();
