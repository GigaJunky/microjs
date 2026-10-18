// MP3 player: plays the .mp3 files on the SD card (the top level, /music and their folders).
// Keys: up / down choose, Enter plays, Space pauses, left / right previous / next song, s stops,
// + / - (or = / -) volume. Gamepad: D-pad moves, button 1 play / pause, button 2 plays the selection.
// Touch: tap a song to play it; the buttons along the bottom.
// Music needs an I2S speaker (CYD-S3, Cardputer); leaving the app stops it.
//   audio.play(path) -> "" or why not; audio.pause([on]); audio.stop(); audio.volume([0..100])
//   audio.status() -> { playing, paused, path, seconds, duration, rate, kbps, channels, error }
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2;
const LH = SMALL ? 11 : 18, BAR = SMALL ? 34 : 64, TOP = LH + 4;
const ROWS = Math.floor((H - TOP - BAR) / LH);
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48), ACCENT = lcd.color(255, 140, 40);

let sel = 0, top = 0, current = -1, userStopped = false, volume = audio.volume();
let songs = [], single = false, scanShown = false;
const title = (i) => (i >= 0 ? songs[i].name + (songs[i].path.split("/").length > 3 ? "  (" + songs[i].path.split("/").slice(2, -1).join("/") + ")" : "") : "");
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;
const fit = (s, px) => { const n = Math.floor(px / (F === 1 ? 6 : 8)); return s.length > n ? s.slice(0, n - 1) + "~" : s; };

// ---- find the songs: from the saved playlist while the card is unchanged, else by scanning (a big
// card takes seconds: say what is happening, so it doesn't look stuck)
load("/lib/medialist.js");
const PLAYLIST = "/sd/playlist.m3u";
const SY = TOP + 8 + (SMALL ? 14 : 24);
const songOf = (p) => ({ path: p, name: p.slice(p.lastIndexOf("/") + 1, p.length - 4) });
function scanStatus(dir, found) {
  if (!scanShown) {
    scanShown = true;
    lcd.fillScreen(BG);
    lcd.fillRect(0, 0, W, TOP - 2, PANEL);
    lcd.setTextDatum(lcd.TL);
    lcd.setTextColor(ACCENT);
    lcd.drawString("MP3 player", 4, 2, F);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString("Looking for songs...", 4, TOP + 4, F);
  }
  const h = SMALL ? 11 : 14;
  lcd.fillRect(0, SY, W, 2 * h + 2, BG);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString(fit(dir, W - 8), 4, SY, 1);  // before the listing: a big folder takes a while
  lcd.setTextColor(lcd.GREENYELLOW);
  lcd.drawString(found + (found === 1 ? " song" : " songs") + " found", 4, SY + h, 1);
}
function loadSongs(rescan) {
  const wasPlaying = current >= 0, keep = wasPlaying ? songs[current].path : single && songs.length ? songs[0].path : "";
  scanShown = false;
  songs = mediaFiles(".mp3", PLAYLIST, scanStatus, rescan).map(songOf);
  if (keep && !songs.some((x) => x.path === keep)) songs.unshift(songOf(keep));
  const at = keep ? songs.findIndex((x) => x.path === keep) : -1;
  current = wasPlaying ? at : -1;
  sel = Math.max(0, at);
  top = 0;
  single = false;
}
// r / tapping the title: scan the card again; from a single song (started in the file manager),
// l / the same tap: list them all. The music goes on meanwhile.
function reload(rescan) {
  loadSongs(rescan);
  shown = "";
  lcd.fillScreen(BG);
  drawList();
  drawBar(true);
}
const arg = typeof ARG === "string" && ARG ? ARG : "";
if (arg) { songs = [songOf(arg)]; single = true; } else loadSongs(false);  // started with a file: no search at all

function drawList() {
  lcd.fillRect(0, 0, W, TOP - 2, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(ACCENT);
  lcd.drawString("MP3 player", 4, 2, F);
  if (mediaFrom === "playlist" && !single) {
    lcd.setTextColor(lcd.DARKGREY);
    lcd.drawString("saved list", 12 + lcd.textWidth("MP3 player", F), 3, 1);
  }
  lcd.setTextDatum(lcd.TR);
  lcd.setTextColor(lcd.LIGHTGREY);
  lcd.drawString(songs.length ? `${sel + 1}/${songs.length}` : "", W - 4, 2, F);
  lcd.setTextDatum(lcd.TL);
  lcd.fillRect(0, TOP - 2, W, ROWS * LH + 2, BG);
  if (!songs.length) {
    lcd.setTextColor(lcd.LIGHTGREY);
    lcd.drawString(board.hasSd === false ? "No SD card." : "No .mp3 files on the SD card.", 4, TOP + 4, F);
    lcd.drawString("Put some in / or /music.", 4, TOP + 4 + LH, F);
    lcd.drawString(board.hasTouch ? "Tap the title to look again." : "Press r to look again.", 4, TOP + 4 + 2 * LH, F);
    return;
  }
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;
  for (let i = top; i < Math.min(songs.length, top + ROWS); i++) {
    const y = TOP + (i - top) * LH;
    if (i === sel) lcd.fillRect(0, y - 1, W, LH, lcd.color(30, 70, 120));
    lcd.setTextColor(i === current ? ACCENT : i === sel ? lcd.WHITE : lcd.LIGHTGREY);
    lcd.drawString((i === current ? "> " : "  ") + fit(songs[i].name, W - 24), 4, y, F);
  }
}

// the bottom panel: song, progress, buttons
const BTNS = ["|<", "||", ">|", "-", "+"];
const btnW = W / BTNS.length, btnY = H - (SMALL ? 14 : 26), btnH = SMALL ? 13 : 24;
let shown = "";
function drawBar(force) {
  const st = audio.status();
  const key = `${current}|${st.playing}|${st.paused}|${Math.floor(st.seconds)}|${volume}|${st.error}`;
  if (key === shown && !force) return st;
  shown = key;
  const y0 = H - BAR;
  lcd.fillRect(0, y0, W, BAR, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(lcd.WHITE);
  const what = st.error ? "Error: " + st.error : current >= 0 ? title(current) : board.hasMusic ? "Nothing playing - " + (board.hasTouch ? "tap the title to " + (single ? "list all songs" : "rescan the card") : single ? "l lists all songs" : "r rescans the card") : "No music output on this board";
  lcd.drawString(fit(what, W - 8), 4, y0 + 2, 1);
  const py = y0 + (SMALL ? 12 : 14), pw = W - 8;  // progress
  lcd.drawRect(4, py, pw, 5, lcd.DARKGREY);
  if (st.duration > 0) lcd.fillRect(5, py + 1, Math.min(pw - 2, (pw - 2) * st.seconds / st.duration), 3, ACCENT);
  if (!SMALL) {
    lcd.setTextColor(lcd.LIGHTGREY);
    const info = st.playing ? `${clock(st.seconds)} / ${clock(st.duration)}   ${st.kbps} kbps ${st.rate / 1000} kHz` : "";
    lcd.drawString(info, 4, py + 8, 1);
    lcd.setTextDatum(lcd.TR);
    lcd.drawString(`vol ${volume}%`, W - 4, py + 8, 1);
  }
  lcd.setTextDatum(lcd.MC);
  BTNS.forEach((b, i) => {
    const label = i === 1 ? (st.playing && !st.paused ? "||" : "|>") : b;
    if (board.hasTouch) lcd.fillRoundRect(i * btnW + 2, btnY, btnW - 4, btnH, 5, lcd.color(45, 50, 90));
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString(label, i * btnW + btnW / 2, btnY + btnH / 2 + 1, F);
  });
  lcd.setTextDatum(lcd.TL);
  return st;
}

// ---- playing
function play(i) {
  if (!songs.length) return;
  current = (i + songs.length) % songs.length;
  sel = current;
  userStopped = false;
  const err = audio.play(songs[current].path);
  if (err) { current = -1; shown = ""; lcd.fillRect(0, H - BAR, W, BAR, PANEL); lcd.setTextColor(lcd.RED); lcd.drawString(fit(err, W - 8), 4, H - BAR + 2, 1); return; }
  drawList();
  drawBar(true);
}
function toggle() {
  const st = audio.status();
  if (!st.playing) return play(current >= 0 ? current : sel);
  audio.pause();
  drawBar(true);
}
function setVolume(v) { volume = audio.volume(Math.max(0, Math.min(100, v))); drawBar(true); }
function move(d) { if (songs.length) { sel = (sel + d + songs.length) % songs.length; drawList(); } }

keyboard.on("key", ({ key }) => {
  if (key === "ArrowUp" || key === ";") move(-1);
  else if (key === "ArrowDown" || key === ".") move(1);
  else if (key === "Enter") play(sel);
  else if (key === " ") toggle();
  else if (key === "ArrowLeft" || key === ",") play(current - 1);
  else if (key === "ArrowRight" || key === "/") play(current + 1);
  else if (key === "+" || key === "=") setVolume(volume + 10);
  else if (key === "-" || key === "_") setVolume(volume - 10);
  else if (key === "s") { userStopped = true; audio.stop(); current = -1; drawList(); drawBar(true); }
  else if (key === "r") reload(true);
  else if (key === "l" && single) reload(false);
});
touch.on("down", ({ x, y }) => {
  if (y >= btnY - 2) return [() => play(current - 1), toggle, () => play(current + 1), () => setVolume(volume - 10), () => setVolume(volume + 10)][Math.min(BTNS.length - 1, Math.floor(x / btnW))]();
  if (y < TOP) return reload(!single);
  if (y >= H - BAR) return;
  const i = top + Math.floor((y - TOP) / LH);
  if (i < songs.length) play(i);
});

lcd.fillScreen(BG);
drawList();
drawBar(true);
if (arg) play(0);  // launched with a file (from files.js)
// redraw the bar as the song plays, and go on to the next song when one ends
let wasPlaying = false;
setInterval(() => {
  const st = drawBar(false);
  if (wasPlaying && !st.playing && !userStopped && !st.error && current >= 0) play(current + 1);
  wasPlaying = st.playing;
}, 250);
