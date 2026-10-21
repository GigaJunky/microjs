// Video playback for the file manager (files.js loads this with the first video it plays, so the file manager
// itself stays small): video.play draws the whole screen; a tap or Space pauses, holding or Enter stops, and
// when it ends the list comes back.
function startVideo(p) {
  audio.stop();
  const err = video.play(p);
  if (err) return say("Can't play: " + err);
  mode = "video";
}
function pauseVideo() {
  const banner = (t) => { lcd.fillRect(0, H - 22, W, 22, PANEL); lcd.setTextColor(lcd.WHITE); lcd.setTextDatum(lcd.ML); lcd.drawString(fit(t, W * 1.5), 6, H - 11, 1); lcd.setTextDatum(lcd.TL); };
  if (video.status().paused) { lcd.fillRect(0, H - 22, W, 22, BG); video.pause(false); return; }
  if (!video.pause(true)) return;
  const st = video.status(), clock = (n) => `${Math.floor(n / 60)}:${String(Math.floor(n % 60)).padStart(2, "0")}`;
  banner(`Paused ${clock(st.seconds)} / ${clock(st.duration)} - ${board.hasTouch ? "tap to go on, hold to stop" : "Space goes on, Enter stops"}`);
}
// back to the list when a video ends or is stopped
setInterval(() => {
  if (mode !== "video") return;
  const st = video.status();
  if (st.playing) return;
  mode = "list";
  lcd.fillScreen(BG);
  draw();
  say(st.error ? "Error: " + st.error : `Played ${st.shown} frames${st.dropped ? `, skipped ${st.dropped}` : ""}`);
}, 200);

