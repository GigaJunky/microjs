// ESP-NOW remote: what remote.js does, board to board over the radio, no router. Run it on two or
// more ESP32 boards in range: the LED colour mixer is shared (move a slider on one, every LED
// follows), Beep beeps the other boards, and chat (type + Enter on a keyboard board; the Say
// button sends ready-made messages on a touch board). Broadcast, unencrypted, radio channel 1.
// Keys: arrows / Enter work the mixer, Tab beeps, anything else types.
// @needs-wifi (ESP-NOW uses the ESP32's WiFi radio, so it isn't on the RP2350 board)
// The full mixer needs a block of memory the classic CYD lacks once the radio is up (loading it
// there aborts the board), so a board without room uses the small one in lib/litemixer.js.
let mixerLoaded = false;
if (board.psram || sys.heap().largest >= 14000) try { load("/lib/mixer.js"); mixerLoaded = true; } catch (e) {}
if (!mixerLoaded) load("/lib/litemixer.js");
const W = lcd.width(), H = lcd.height(), SMALL = H < 200, LH = 9;
const TOP = SMALL ? 11 : 18, MIXH = SMALL ? 78 : 122, CHATY = TOP + MIXH + 4;
const BH = board.hasTouch ? 28 : 0;  // the Beep / Say buttons along the bottom of a touch screen
const me = espnow.mac(), TAG = me.slice(-5).replace(":", "");  // this board in the chat: the end of its address, e.g. "9AE4"
const PRESETS = ["Hello!", "Hi there", "Look at this", "Yes", "No", "Thanks", "On my way", "Done"];
if (!espnow.begin(1)) throw new Error("espnow didn't start");

const heard = new Set();
function status(msg, color) {
  lcd.fillRect(0, 0, W, TOP, lcd.color(20, 24, 48));
  lcd.setTextDatum(lcd.ML);
  lcd.setTextColor(color || lcd.WHITE);
  lcd.drawString(msg, 4, TOP / 2 + 1, SMALL ? 1 : 2);
}
const headline = () => `ESP-NOW ${TAG}  heard ${heard.size} board${heard.size === 1 ? "" : "s"}`;

// ---- what goes over the air: small JSON messages (a message is 250 bytes at most)
const send = (m) => espnow.send("*", JSON.stringify(m));
let mixer, chat = [], typing = "", say = 0, flash = 0, lastSig = "", lastSent = 0;

const sig = (st) => (st.auto ? ["a", st.bright, st.speed, st.ledOn] : [st.r, st.g, st.b, st.bright, st.speed, st.ledOn]).join();
function sendLed(force) {  // only what a person changed: not the colours Auto cycles through
  const st = mixer.state, s = sig(st);
  if (!force && (s === lastSig || millis() - lastSent < 80)) return;
  lastSig = s;
  lastSent = millis();
  send({ t: "led", r: st.r, g: st.g, b: st.b, v: st.bright, s: st.speed, a: st.auto, o: st.ledOn });
}

// ---- chat, on the screen below the mixer
function addChat(name, text) {
  chat.push({ name, text });
  if (chat.length > 30) chat.shift();
  drawChat();
}
function wrap(line, maxPx) {  // split into lines that fit maxPx (measured in font 1)
  const out = [];
  while (lcd.textWidth(line, 1) > maxPx) {
    let cut = line.length;
    while (cut > 1 && lcd.textWidth(line.slice(0, cut), 1) > maxPx) cut--;
    const sp = line.lastIndexOf(" ", cut);
    if (sp > 0) cut = sp;
    out.push(line.slice(0, cut));
    line = line.slice(cut).trim();
  }
  out.push(line);
  return out;
}
function drawChat() {
  const inputRows = board.hasKeyboard ? 1 : 0, rows = Math.floor((H - BH - CHATY) / LH) - inputRows, maxPx = W - 6;
  let lines = [];
  for (const m of chat) for (const l of wrap(m.name + ": " + m.text, maxPx)) lines.push([m.name, l]);
  lines = lines.slice(-rows);
  lcd.fillRect(0, CHATY, W, H - BH - CHATY, lcd.BLACK);
  lcd.setTextDatum(lcd.TL);
  lines.forEach(([name, l], i) => {
    lcd.setTextColor(name === TAG ? lcd.CYAN : lcd.WHITE);
    lcd.drawString(l, 3, CHATY + i * LH, 1);
  });
  if (inputRows) {
    lcd.fillRect(0, H - LH - 1, W, LH + 1, lcd.color(25, 25, 35));
    lcd.setTextColor(lcd.YELLOW);
    let shown = typing;  // the end of what's typed, as much as fits
    while (shown && lcd.textWidth("> " + shown + "_", 1) > maxPx) shown = shown.slice(1);
    lcd.drawString("> " + shown + "_", 3, H - LH, 1);
  }
}
function talk(text) {
  text = text.trim().slice(0, 200);
  if (!text) return;
  addChat(TAG, text);
  send({ t: "chat", n: TAG, m: text });
}
function beep() {
  tone(1400, 30);
  send({ t: "beep", f: 880, ms: 200 });
  status("Beep sent", lcd.YELLOW);
  flash = millis() + 400;
}

// ---- buttons (touch boards): Beep and Say
const BTN = () => {
  const y = H - BH + 2, h = BH - 4, bw = Math.floor((W - 18) * 0.35);
  return { beep: [6, y, bw, h], say: [12 + bw, y, W - 18 - bw, h] };
};
function drawButtons() {
  if (!BH) return;
  const b = BTN();
  for (const [r, label, color] of [[b.beep, "Beep!", lcd.color(160, 140, 20)], [b.say, "Say: " + PRESETS[say], lcd.color(30, 100, 160)]]) {
    lcd.fillRoundRect(r[0], r[1], r[2], r[3], 6, color);
    lcd.setTextDatum(lcd.MC);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString(label, r[0] + r[2] / 2, r[1] + r[3] / 2 + 1, SMALL ? 1 : 2);
  }
}

// ---- messages from the other boards
espnow.on("message", ({ mac, data }) => {
  let m;
  try { m = JSON.parse(data); } catch (e) { return; }
  if (!m || typeof m.t !== "string") return;
  console.log("espnow", mac, data);
  if (!heard.has(mac)) { heard.add(mac); status(headline(), lcd.GREEN); }
  if (m.t === "led") {
    const st = mixer.state;
    st.r = m.r | 0; st.g = m.g | 0; st.b = m.b | 0; st.bright = m.v | 0; st.speed = m.s | 0; st.auto = !!m.a; st.ledOn = !!m.o;
    lastSig = sig(st);  // what just arrived isn't ours to send again
    mixer.draw();
  } else if (m.t === "beep") {
    tone(Math.max(100, Math.min(4000, +m.f || 880)), Math.max(20, Math.min(1000, +m.ms || 150)));
    status("Beep from " + mac.slice(-5).replace(":", ""), lcd.YELLOW);
    flash = millis() + 400;
  } else if (m.t === "chat") {
    addChat(String(m.n || mac.slice(-5).replace(":", "")).slice(0, 16), String(m.m || "").slice(0, 200));
    tone(1400, 30);
  }
});

// ---- input on this board
const ARROWS = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right" };
keyboard.on("key", ({ key }) => {
  if (ARROWS[key]) mixer.key(ARROWS[key]);
  else if (key === "Tab") beep();
  else if (key === "Enter") {
    if (typing.trim()) { talk(typing); typing = ""; }
    else mixer.key("enter");
  } else if (key === "Backspace") typing = typing.slice(0, -1);
  else if (key.length === 1 && typing.length < 200) typing += key;
  else return;
  drawChat();
});
const inRect = (r, x, y) => x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
touch.on("down", ({ x, y }) => {
  const b = BTN();
  if (BH && inRect(b.beep, x, y)) beep();
  else if (BH && inRect(b.say, x, y)) { talk(PRESETS[say]); say = (say + 1) % PRESETS.length; drawButtons(); }
  else mixer.down(x, y);
});
touch.on("move", ({ x, y }) => mixer.move(x, y));
touch.on("up", () => mixer.up());

// ---- go
lcd.fillScreen(lcd.BLACK);
mixer = (mixerLoaded ? makeMixer : makeLiteMixer)({ x: 4, y: TOP + 2, w: W - 8, h: MIXH });
status(headline(), lcd.GREEN);
mixer.draw();
lastSig = sig(mixer.state);
addChat("--", "Radio on. Other boards running this will join in.");
drawButtons();
setInterval(() => {
  mixer.tick();
  sendLed(false);
  if (flash && millis() > flash) { flash = 0; status(headline(), lcd.GREEN); }
}, 40);
