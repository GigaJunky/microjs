// The file manager's player: files.js launches this with the path of an .mp3 or .avi (a launched app starts with
// a clean heap, which playing needs and the file manager can't spare on a board without PSRAM), and it goes back
// to the file manager when the music or video ends. Space or a tap pauses; Enter, Escape or a long press stops.
const P = ARG, S = "/files.state", VIDEO = P.toLowerCase().endsWith(".avi");
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, TH = SMALL ? 11 : 18, F = SMALL ? 1 : 2, CHW = F === 1 ? 6 : 8;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48);
const name = P.slice(P.lastIndexOf("/") + 1);
const clock = (n) => `${Math.floor(n / 60)}:${String(Math.floor(n % 60)).padStart(2, "0")}`;
const status = () => (VIDEO ? video.status() : audio.status());
keyboard.capture(true);  // Escape comes here
function back(msg) {
  if (VIDEO) video.stop(); else audio.stop();
  try { const s = JSON.parse(fs.read(S)); s.msg = msg; fs.write(S, JSON.stringify(s)); } catch (e) {}
  launch("/files.js");
}
function banner(t) {  // a strip along the bottom
  lcd.fillRect(0, H - 22, W, 22, PANEL);
  lcd.setTextColor(lcd.WHITE);
  lcd.setTextDatum(lcd.ML);
  lcd.drawString(t.slice(0, Math.floor(W / 6) - 1), 6, H - 11, 1);
  lcd.setTextDatum(lcd.TL);
}
function pause() {
  const st = status();
  if (st.paused) { if (VIDEO) { lcd.fillRect(0, H - 22, W, 22, BG); video.pause(false); } else audio.pause(false); return; }
  if (VIDEO ? !video.pause(true) : (audio.pause(true), false)) return;  // (video.pause says when the screen is free)
  const t = status();
  banner(`Paused ${clock(t.seconds)} / ${clock(t.duration)} - ${board.hasTouch ? "tap to go on, hold to stop" : "Space goes on, Enter stops"}`);
}
keyboard.on("key", ({ key: k }) => { if (k === " ") pause(); else if (k === "Enter" || k === "Escape" || k === "Backspace") back(""); });
let at = 0;
touch.on("down", () => { at = millis(); });
touch.on("up", () => (millis() - at > 500 ? back("") : pause()));

lcd.fillScreen(BG);
const err = VIDEO ? video.play(P) : audio.play(P);
if (err) back("Can't play: " + err);
else if (!VIDEO) {
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(name.slice(0, Math.floor(W / CHW) - 1), 8, 20, F);
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString(board.hasTouch ? "Tap pauses, hold stops" : "Space pauses, Enter stops", 8, 20 + TH + 8, 1);
}
setInterval(() => {
  const st = status();
  if (!st.playing && !st.paused) return back(st.error ? "Error: " + st.error : VIDEO ? `Played ${st.shown} frames${st.dropped ? `, skipped ${st.dropped}` : ""}` : "");
  if (VIDEO || st.paused) return;
  const y = H / 2;  // the music screen: time and a progress bar
  lcd.fillRect(8, y - 12, W - 16, 24 + 8, BG);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  lcd.drawString(`${clock(st.seconds)} / ${clock(st.duration)}`, 8, y - 12, F);
  lcd.drawRect(8, y + 12, W - 16, 8, lcd.LIGHTGREY);
  lcd.fillRect(10, y + 14, Math.floor(((W - 20) * st.seconds) / Math.max(1, st.duration)), 4, lcd.GREEN);
}, 500);
