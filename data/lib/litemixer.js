// A small colour mixer for boards too tight on memory for lib/mixer.js (the classic CYD with the
// radio on): the same sliders, Auto and LED buttons, touch and arrow keys, as makeLiteMixer(area).
// Same interface as makeMixer: state, draw(), tick(), down(x, y), move(x), up(), key(name).
function makeLiteMixer(area) {
  const { x: X, y: Y, w: AW, h: AH } = area, RH = Math.floor(AH / 6), F = AH < 150 ? 1 : 2, SW = Math.floor(AW * 0.27), TW = AW - SW - 6;
  const st = { r: 255, g: 60, b: 0, bright: 30, speed: 40, auto: false, ledOn: true, hue: 0 };
  const S = [["Red", "r", 255, lcd.RED], ["Green", "g", 255, lcd.GREEN], ["Blue", "b", 255, lcd.BLUE], ["Bright", "bright", 100, lcd.WHITE], ["Speed", "speed", 100, lcd.ORANGE]];
  let drag = -1, sel = 0;
  function out() {
    const k = st.bright / 100, f = 0.3 + 0.7 * k;
    if (board.hasLed) led(st.ledOn ? Math.round(st.r * k) : 0, st.ledOn ? Math.round(st.g * k) : 0, st.ledOn ? Math.round(st.b * k) : 0);
    lcd.fillRoundRect(X + AW - SW + 2, Y + 3, SW - 4, RH * 5 - 6, 8, lcd.color(st.r * f, st.g * f, st.b * f));
  }
  function slider(i) {
    const [name, key, max, color] = S[i], y = Y + i * RH + 2, h = RH - 4, v = st[key], on = i === sel && !board.hasTouch;
    lcd.fillRoundRect(X, y, TW, h, 5, on ? lcd.color(55, 55, 80) : lcd.color(35, 35, 45));
    const fw = Math.round(((TW - 4) * v) / max);
    if (fw > 0) lcd.fillRoundRect(X + 2, y + 2, Math.max(fw, 4), h - 4, 4, color);
    lcd.setTextDatum(lcd.ML);
    lcd.setTextColor(fw > 30 && (key === "g" || key === "bright" || key === "speed") ? lcd.BLACK : lcd.WHITE);
    lcd.drawString(name + "  " + v, X + 6, y + h / 2 + 1, F);
  }
  function buttons() {
    const y = Y + 5 * RH + 2, h = RH - 4, bw = Math.floor((AW - 6) / 2);
    [[X, st.auto ? "Auto: cycling" : "Auto: off", st.auto], [X + bw + 6, st.ledOn ? "LED: on" : "LED: off", st.ledOn]].forEach(([x, label, on], i) => {
      lcd.fillRoundRect(x, y, bw, h, 6, on ? (i ? lcd.color(30, 110, 60) : lcd.color(120, 60, 200)) : lcd.color(50, 50, 60));
      if (sel === 5 + i && !board.hasTouch) lcd.drawRoundRect(x, y, bw, h, 6, lcd.WHITE);
      lcd.setTextDatum(lcd.MC);
      lcd.setTextColor(lcd.WHITE);
      lcd.drawString(label, x + bw / 2, y + h / 2 + 1, F);
    });
  }
  function draw() { lcd.fillRect(X, Y, AW, AH, lcd.BLACK); S.forEach((_, i) => slider(i)); buttons(); out(); }
  function set(i, v) {
    st[S[i][1]] = Math.max(0, Math.min(S[i][2], v));
    if (i < 3) st.auto = false;
    slider(i); buttons(); out();
  }
  function press(i) { if (i === 0) st.auto = !st.auto; else st.ledOn = !st.ledOn; buttons(); out(); }
  return { state: st, draw,
    tick() {
      if (!st.auto) return;
      st.hue = (st.hue + 0.3 + st.speed / 8) % 360;
      const f = (n) => { const k = (n + st.hue / 60) % 6; return Math.round(255 * (1 - Math.max(0, Math.min(k, 4 - k, 1)))); };
      st.r = f(5); st.g = f(3); st.b = f(1);
      for (let i = 0; i < 3; i++) slider(i);
      out();
    },
    down(px, py) {
      if (px < X || py < Y || py >= Y + AH) return false;
      const row = Math.floor((py - Y) / RH);
      if (row < 5 && px < X + TW) { drag = row; set(row, Math.round(((px - X - 2) / (TW - 4)) * S[row][2])); return true; }
      if (row === 5) { press(px < X + AW / 2 ? 0 : 1); return true; }
      return false;
    },
    move(px) { if (drag >= 0) set(drag, Math.round(((px - X - 2) / (TW - 4)) * S[drag][2])); },
    up() { drag = -1; },
    key(k) {
      const was = sel;
      if (k === "up") sel = (sel + 6) % 7;
      else if (k === "down") sel = (sel + 1) % 7;
      else if ((k === "left" || k === "right") && sel < 5) return set(sel, st[S[sel][1]] + (k === "right" ? 1 : -1) * (S[sel][2] === 255 ? 15 : 5));
      else if ((k === "enter" || k === "space") && sel >= 5) return press(sel - 5);
      else return;
      for (const i of [was, sel]) i < 5 ? slider(i) : buttons();
    },
  };
}
