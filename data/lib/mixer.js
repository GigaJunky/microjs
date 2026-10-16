// Colour mixer, shared by controls.js and bruce.js (load("/lib/mixer.js"), then makeMixer(...)):
// sliders for red, green, blue, brightness and cycle speed, an Auto button that cycles the hue,
// an LED on/off button, and a swatch showing the colour - on the screen and on the board's LED.
//   const m = makeMixer({ x, y, w, h })   the area it draws in
//   m.draw()  m.tick()  (call every 40 ms or so; it animates in Auto)
//   m.down(x, y)  m.move(x, y)  m.up()   touch;   m.key(name)  "up" "down" "left" "right" "enter"
//   m.off()   LED off (when leaving)
function makeMixer(area) {
  const { x: X, y: Y, w: AW, h: AH } = area;
  const small = AH < 150, F = small ? 1 : 2;
  const SW = Math.floor(AW * (small ? 0.24 : 0.27));  // swatch column
  const SX = X + AW - SW;
  const ROWS = 6, RH = Math.floor(AH / ROWS), PAD = small ? 1 : 3, TW = AW - SW - 6;
  const st = { r: 255, g: 60, b: 0, bright: 30, speed: 40, auto: false, ledOn: true, hue: 0, sel: 0 };
  const sliders = [
    { key: "r", label: "Red", max: 255, color: lcd.RED },
    { key: "g", label: "Green", max: 255, color: lcd.GREEN },
    { key: "b", label: "Blue", max: 255, color: lcd.BLUE },
    { key: "bright", label: "Bright", max: 100, color: lcd.WHITE, unit: "%" },
    { key: "speed", label: "Speed", max: 100, color: lcd.ORANGE, unit: "%" },
  ];
  let dragging = -1;

  function hsv(h) {  // hue (0-360) -> [r, g, b], full colour
    const f = (n) => { const k = (n + h / 60) % 6; return Math.round(255 * (1 - Math.max(0, Math.min(k, 4 - k, 1)))); };
    return [f(5), f(3), f(1)];
  }
  const scaled = () => [st.r, st.g, st.b].map((v) => Math.round((v * st.bright) / 100));
  function output() {
    const [r, g, b] = scaled();
    if (board.hasLed) led(st.ledOn ? r : 0, st.ledOn ? g : 0, st.ledOn ? b : 0);
    // the swatch: the colour, dimmed with the brightness (but never so dark you can't see the hue)
    const sc = (v) => Math.round(v * (0.3 + (0.7 * st.bright) / 100));
    lcd.fillRoundRect(SX + 2, Y + PAD, SW - 4, RH * 5 - 2 * PAD, 8, lcd.color(sc(st.r), sc(st.g), sc(st.b)));
    lcd.drawRoundRect(SX + 2, Y + PAD, SW - 4, RH * 5 - 2 * PAD, 8, lcd.DARKGREY);
  }
  function drawSlider(i) {
    const s = sliders[i], y = Y + i * RH + PAD, h = RH - 2 * PAD, v = st[s.key];
    const on = i === st.sel && !board.hasTouch;
    lcd.fillRoundRect(X, y, TW, h, 5, on ? lcd.color(55, 55, 80) : lcd.color(35, 35, 45));
    const fw = Math.round(((TW - 4) * v) / s.max);
    if (fw > 0) lcd.fillRoundRect(X + 2, y + 2, Math.max(fw, 4), h - 4, 4, s.color);
    lcd.setTextDatum(lcd.ML);
    // dark text where the bar is under it and the bar is a light colour, else white
    const under = fw > 7 + lcd.textWidth(s.label, F) / 2;
    lcd.setTextColor(under && (s.key === "bright" || s.key === "g" || s.key === "speed") ? lcd.BLACK : lcd.WHITE);
    lcd.drawString(s.label, X + 7, y + h / 2 + 1, F);
    lcd.setTextDatum(lcd.MR);
    lcd.setTextColor(lcd.WHITE);
    lcd.drawString(v + (s.unit || ""), X + TW - 6, y + h / 2 + 1, F);
  }
  const BTN = () => {  // the bottom row: Auto, LED
    const y = Y + 5 * RH + PAD, h = RH - 2 * PAD, bw = Math.floor((AW - 6) / 2);
    return [
      { x: X, y, w: bw, h, label: st.auto ? "Auto: cycling" : "Auto: off", color: st.auto ? lcd.color(120, 60, 200) : lcd.color(50, 50, 60) },
      { x: X + bw + 6, y, w: bw, h, label: st.ledOn ? "LED: on" : "LED: off", color: st.ledOn ? lcd.color(30, 110, 60) : lcd.color(50, 50, 60) },
    ];
  };
  function drawButtons() {
    BTN().forEach((b, i) => {
      const on = st.sel === 5 + i && !board.hasTouch;
      lcd.fillRoundRect(b.x, b.y, b.w, b.h, 6, b.color);
      if (on) lcd.drawRoundRect(b.x, b.y, b.w, b.h, 6, lcd.WHITE);
      lcd.setTextDatum(lcd.MC);
      lcd.setTextColor(lcd.WHITE);
      lcd.drawString(b.label + (!board.hasLed && i === 1 ? " (none)" : ""), b.x + b.w / 2, b.y + b.h / 2 + 1, F);
    });
  }
  function draw() {
    lcd.fillRect(X, Y, AW, AH, lcd.BLACK);
    sliders.forEach((_, i) => drawSlider(i));
    drawButtons();
    output();
  }
  function setFromX(i, px) {
    const s = sliders[i], v = Math.round((Math.max(0, Math.min(TW - 4, px - X - 2)) / (TW - 4)) * s.max);
    if (v === st[s.key]) return;
    st[s.key] = v;
    if (i < 3 && st.auto) { st.auto = false; drawButtons(); }  // moving a colour takes it out of Auto
    drawSlider(i);
    output();
  }
  function press(i) {
    if (i === 0) st.auto = !st.auto;
    else st.ledOn = !st.ledOn;
    tone(st.auto || st.ledOn ? 1100 : 700, 20);
    drawButtons();
    output();
  }
  return {
    draw,
    tick() {
      if (!st.auto) return;
      st.hue = (st.hue + 0.3 + st.speed / 8) % 360;
      const c = hsv(st.hue);
      st.r = c[0]; st.g = c[1]; st.b = c[2];
      for (let i = 0; i < 3; i++) drawSlider(i);
      output();
    },
    down(px, py) {
      if (px < X || py < Y || py >= Y + AH) return false;
      const row = Math.floor((py - Y) / RH);
      if (row < 5 && px < X + TW) { dragging = row; setFromX(row, px); return true; }
      if (row === 5) { press(px < X + AW / 2 ? 0 : 1); return true; }
      return false;
    },
    move(px) { if (dragging >= 0) setFromX(dragging, px); },
    up() { dragging = -1; },
    key(k) {
      const before = st.sel;
      if (k === "up") st.sel = (st.sel + 6) % 7;
      else if (k === "down") st.sel = (st.sel + 1) % 7;
      else if ((k === "left" || k === "right") && st.sel < 5) {
        const s = sliders[st.sel], step = s.max === 255 ? 15 : 5;
        st[s.key] = Math.max(0, Math.min(s.max, st[s.key] + (k === "right" ? step : -step)));
        if (st.sel < 3) st.auto = false;
        drawSlider(st.sel);
        drawButtons();
        output();
        return;
      } else if ((k === "enter" || k === "space") && st.sel >= 5) return press(st.sel - 5);
      else if (k === "a") return press(0);
      else return;
      for (const i of [before, st.sel]) i < 5 ? drawSlider(i) : drawButtons();
    },
    off() { if (board.hasLed) led(0, 0, 0); },
    state: st,
  };
}
