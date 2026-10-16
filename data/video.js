// Video player: plays .avi files (Motion-JPEG video, MP3 sound) from the SD card and the flash.
// Tap (or Space) pauses / resumes, hold (or Enter / Escape) stops; + / - volume.
// Up to about 25 frames a second at 320x240 on the ESP32-S3 boards. Convert videos (mp4, mkv, ...)
// on a computer with tools/convert-videos.sh, or with ffmpeg directly:
//   ffmpeg -i in.mp4 -vf "scale=320:-2,fps=20" -c:v mjpeg -q:v 6 -c:a libmp3lame -b:a 96k -ar 44100 out.avi
//   video.play(path) -> "" or why not; video.pause([on]); video.stop()
//   video.status() -> { playing, paused, frame, frames, shown, dropped, fps, width, height, seconds, duration, sound, error }
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, F = SMALL ? 1 : 2;
const TOUCH = board.hasTouch && !SMALL, LH = TOUCH ? 30 : SMALL ? 11 : 18, TOP = (SMALL ? 11 : 18) + 4;
const ROWS = Math.floor((H - TOP - (SMALL ? 12 : 18)) / LH), TY = Math.floor((LH - (SMALL ? 11 : 18)) / 2) + 1;
const BG = lcd.BLACK, PANEL = lcd.color(20, 24, 48), SEL = lcd.color(30, 70, 120), ACCENT = lcd.color(255, 140, 40);
keyboard.capture(true);

// ---- find the videos
const videos = [];
function scan(dir, depth) {
  let names = [];
  try { names = fs.list(dir); } catch (e) {}
  for (const n of names.sort()) {
    if (n.endsWith("/")) {
      if (depth < 2 && !n.startsWith(".") && n !== "System Volume Information/") scan((dir === "/" ? "" : dir) + "/" + n.slice(0, -1), depth + 1);
    } else if (n.toLowerCase().endsWith(".avi")) videos.push((dir === "/" ? "" : dir) + "/" + n);
  }
}
scan("/sd", 0);
scan("/", 1);  // the flash: just its top level

let sel = 0, top = 0, playing = false, note = "", volume = audio.volume();
const fit = (s, px) => { const n = Math.floor(px / (F === 1 ? 6 : 8)); return s.length > n ? s.slice(0, n - 1) + "~" : s; };
const nameOf = (p) => p.slice(p.lastIndexOf("/") + 1, p.length - 4);
const clock = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

function drawList() {
  lcd.fillScreen(BG);
  lcd.fillRect(0, 0, W, TOP - 2, PANEL);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(ACCENT);
  lcd.drawString("Videos", 4, 2, F);
  if (!videos.length) {
    lcd.setTextColor(lcd.LIGHTGREY);
    lcd.drawString("No .avi files on the SD card.", 4, TOP + 4, F);
    lcd.drawString("Make one with ffmpeg - see video.js", 4, TOP + 4 + (SMALL ? 11 : 18), 1);
  }
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;
  for (let i = top; i < Math.min(videos.length, top + ROWS); i++) {
    const y = TOP + (i - top) * LH;
    if (i === sel) lcd.fillRect(0, y - 1, W, LH, SEL);
    lcd.setTextColor(i === sel ? lcd.WHITE : lcd.LIGHTGREY);
    lcd.drawString(fit(nameOf(videos[i]) + (videos[i].startsWith("/sd") ? "" : "  (flash)"), W - 8), 4, y + TY, F);
  }
  lcd.setTextColor(lcd.DARKGREY);
  lcd.drawString(fit(note || (TOUCH ? "Tap to play. While playing: tap pauses, hold stops." : "Enter plays. Space pauses, Enter stops."), W - 8), 4, H - (SMALL ? 10 : 14), 1);
}

function play(i) {
  if (!videos.length) return;
  sel = i;
  const err = video.play(videos[i]);
  if (err) { note = "Can't play: " + err; return drawList(); }
  playing = true;
}
function banner(text) {  // drawn over the held picture while paused
  lcd.fillRect(0, H - 22, W, 22, PANEL);
  lcd.setTextColor(lcd.WHITE);
  lcd.setTextDatum(lcd.ML);
  lcd.drawString(fit(text, W * 1.5), 6, H - 11, 1);
  lcd.setTextDatum(lcd.TL);
}
// Pausing waits until the video has stopped drawing, so the banner can go on the screen; resuming
// clears it first (a widescreen video doesn't cover the bottom of the screen).
function pause() {
  if (video.status().paused) {
    lcd.fillRect(0, H - 22, W, 22, BG);
    video.pause(false);
    return;
  }
  if (!video.pause(true)) return;
  const st = video.status();
  banner(`Paused ${clock(st.seconds)} / ${clock(st.duration)} - ${TOUCH ? "tap to go on, hold to stop" : "Space goes on, Enter stops"}`);
}
function stop() { video.stop(); }
function setVolume(v) { volume = audio.volume(Math.max(0, Math.min(100, v))); }

keyboard.on("key", ({ key }) => {
  if (playing) {
    if (key === " ") pause();
    else if (key === "Enter" || key === "Escape" || key === "Backspace") stop();
    else if (key === "+" || key === "=") setVolume(volume + 10);
    else if (key === "-" || key === "_") setVolume(volume - 10);
    return;
  }
  if (key === "ArrowUp" || key === ";") { sel = Math.max(0, sel - 1); drawList(); }
  else if (key === "ArrowDown" || key === ".") { sel = Math.min(videos.length - 1, sel + 1); drawList(); }
  else if (key === "Enter" || key === " ") play(sel);
  else if (key === "Escape") launch("/main.js");
});
let downAt = 0, downY = 0;
touch.on("down", ({ y }) => { downAt = millis(); downY = y; });
touch.on("up", () => {
  const long = millis() - downAt > 600;
  if (playing) return long ? stop() : pause();
  if (downY < TOP || downY >= TOP + ROWS * LH) return;
  const i = top + Math.floor((downY - TOP) / LH);
  if (i < videos.length) play(i);
});

// back to the list when a video ends (or is stopped)
setInterval(() => {
  if (!playing) return;
  const st = video.status();
  if (st.playing) return;
  playing = false;
  note = st.error ? "Error: " + st.error : `Played ${st.shown} frames${st.dropped ? `, skipped ${st.dropped} to keep up` : ""}`;
  drawList();
}, 200);

drawList();
if (typeof ARG === "string" && ARG) {  // launched with a file (from files.js)
  const i = videos.indexOf(ARG);
  if (i >= 0) play(i);
  else { videos.unshift(ARG); play(0); }
}
