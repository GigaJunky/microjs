// Bruce's Bluetooth tools (boards with ble: the S3 ones). bruce.js loads this and adds what it returns:
//   BLE scan      nearby devices, strongest first         Enter / "Scan again": scan again
//   BLE keyboard  the board is a Bluetooth keyboard       keys are typed on the paired computer / phone
//   BLE mouse     ... a mouse                              touch: drag moves, Left / Right buttons click; keys: arrows, Enter, Space
//   BLE gamepad   ... a gamepad                            a USB gamepad, the touch stick + A B X Y, or the arrow keys
//   BLE devices   pair a Bluetooth keyboard, mouse or gamepad TO the board (ble.host); paired ones reconnect by themselves
// Pair the board with a phone / computer from its Bluetooth settings: the name is "micro-js".
// On touch screens everything is big: rows 30-40 px, buttons 44 px high along the bottom.
function bleTools(c) {
  const { text, TOP, F, W, H, BG, ACCENT, bars, TOUCH } = c, BH = 44, GAP = 6, BY = H - BH - 4, CELL = lcd.color(50, 50, 80);
  const line = TOUCH ? 30 : c.SMALL ? 14 : 20;
  const clear = () => lcd.fillRect(0, TOP, W, H - TOP, BG);
  const button = (x, y, w, h, label, on) => { lcd.fillRoundRect(x, y, w, h, 8, on ? ACCENT : CELL); lcd.setTextDatum(lcd.MC); lcd.setTextColor(lcd.WHITE); lcd.drawString(label, x + w / 2, y + h / 2, F); };
  // a row of equal buttons along the bottom: returns a hit test (x, y) -> index, or -1
  const bottom = (labels) => {
    const w = Math.floor((W - GAP * (labels.length + 1)) / labels.length);
    labels.forEach((l, i) => button(GAP + i * (w + GAP), BY, w, BH, l));
    return (x, y) => (y >= BY - 4 ? Math.min(labels.length - 1, Math.max(0, Math.floor((x - GAP / 2) / (w + GAP)))) : -1);
  };
  const link = () => {
    const on = ble.hid.connected(), who = on && ble.hid.peer ? ble.hid.peer() : "";
    lcd.fillRect(0, TOP, W, 22, BG);
    text(on ? `connected${who ? " to " + who : ""}` : "waiting - pair \"micro-js\" in Bluetooth settings", 6, TOP + 4, 1, on ? lcd.GREEN : lcd.YELLOW);
  };
  const start = () => { ble.hid.begin("micro-js"); clear(); };
  const hint = (s) => text(s, 6, H - (c.SMALL ? 10 : 14), 1, lcd.DARKGREY);
  let rawLog = [], note = "", log = [], mouse = "up", layer = 0, shift = false, ctrl = false, found = [], seen = [], showAll = false, status = "", hit = null, held = 0, mx = 0, my = 0, px = 0, py = 0, btn = 0, cur = 0;
  const ACTS = [["Type hello", () => ble.hid.type("Hello from micro-js")], ["Enter", () => ble.hid.tap("enter")], ["Ctrl+C", () => ble.hid.tap("ctrl+c")], ["Ctrl+V", () => ble.hid.tap("ctrl+v")], ["Backspace", () => ble.hid.tap("backspace")], ["Esc", () => ble.hid.tap("esc")]];
  const CW = Math.floor((W - GAP * 3) / 2), cellAt = (i) => [GAP + (i % 2) * (CW + GAP), TOP + 26 + Math.floor(i / 2) * (BH + GAP)];
  const KEYS = { up: "up", down: "down", left: "left", right: "right", enter: "enter", space: "space" };
  return [
    {
      icon: "S", name: "Scan devices", title: "BLE scan",
      enter() { this.p = 0; found = []; this.scan(); },
      scan() { text("scanning...", 6, TOP + 4, 1, lcd.YELLOW); found = ble.scan(3); this.draw(); },
      draw() {
        c.paged(this, found, (d, y) => {
          bars(4, y, d.rssi);
          text(d.name || d.mac, 26, y, F, d.name ? lcd.WHITE : lcd.DARKGREY);
          text(`${d.rssi}`, W - 4, y, F, lcd.DARKGREY, lcd.TR);
        });
      },
      key(k) { if (k === "enter") this.scan(); else if (k === "up" || k === "down") this.page(k === "down" ? 1 : -1); },
      tap(x, y) { if (y >= H - 52) this.scan(); },
    },
    {
      icon: "K", name: "Keyboard", title: "BLE keyboard", every: 800,
      enter() { start(); layer = 0; shift = ctrl = false; },
      keys() {  // the on-screen keyboard: 32 x 36 px keys, two layers (letters / symbols)
        const KW = 32, KH = 36, y0 = TOP + 24, ks = [], row = (r, str, xo) => [...str].forEach((ch, i) => ks.push({ x: xo + i * KW, y: y0 + r * KH, w: KW, label: ch, ch }));
        const wide = (r, xo, list) => list.forEach(([label, w, act, ch]) => { ks.push({ x: xo, y: y0 + r * KH, w, label, act, ch }); xo += w; });
        row(0, layer ? "!@#$%^&*()" : "1234567890", 0);
        row(1, layer ? "-_=+[]{}|\\" : "qwertyuiop", 0);
        if (layer) { row(2, ";:'\",.<>/?", 0); wide(3, 0, [["Esc", KW, "esc"], ["Tab", KW, "tab"], ["<", KW, "left"], ["v", KW, "down"], ["^", KW, "up"], [">", KW, "right"], ["Ctrl", KW, "ctrl"], ["Bksp", 96, "backspace"]]); }
        else { row(2, "asdfghjkl", 16); wide(3, 0, [["Shift", 44, "shift"]]); row(3, "zxcvbnm", 44); wide(3, 44 + 7 * KW, [["Bksp", 44, "backspace"]]); }
        wide(4, 0, [[layer ? "abc" : "123", 56, "layer"], ["Space", 160, null, " "], ["Enter", 104, "enter"]]);
        return ks;
      },
      keyFace(k, on) {
        const lit = on || (k.act === "shift" && shift) || (k.act === "ctrl" && ctrl), up = shift && !layer && k.ch && k.ch !== " ";
        lcd.fillRoundRect(k.x + 1, k.y + 1, k.w - 2, 34, 6, lit ? ACCENT : CELL);
        lcd.setTextDatum(lcd.MC); lcd.setTextColor(lcd.WHITE);
        lcd.drawString(up ? k.label.toUpperCase() : k.label, k.x + k.w / 2, k.y + 18, k.label.length > 2 ? 1 : F);
      },
      draw() {
        link();
        if (TOUCH) { this.list = this.keys(); this.list.forEach((k) => this.keyFace(k)); }
        else { text("type here: the keys go to the host", 6, TOP + 30, F, lcd.LIGHTGREY); hint("Backspace goes back"); }
      },
      tick() { link(); },
      key(k) {
        if (k.length === 1) ble.hid.type(k);
        else if (k === "space") ble.hid.type(" ");
        else if (KEYS[k]) ble.hid.tap(KEYS[k]);
      },
      tap(x, y) {
        const k = (this.list || []).find((q) => x >= q.x && x < q.x + q.w && y >= q.y && y < q.y + 36);
        if (!k) return;
        this.keyFace(k, true);
        if (k.act === "shift") shift = !shift;
        else if (k.act === "ctrl") ctrl = !ctrl;
        else if (k.act === "layer") { layer ^= 1; return this.draw(); }
        else if (k.ch) {
          const ch = shift && !layer ? k.ch.toUpperCase() : k.ch;
          if (ctrl) ble.hid.tap("ctrl+" + k.ch.toLowerCase()); else ble.hid.type(ch);
          shift = ctrl = false;
        } else if (k.act) ble.hid.tap(k.act);
        if (k.act === "shift" || k.act === "ctrl" || k.ch) this.draw(); else this.keyFace(k);
      },
    },
    {
      icon: "M", name: "Mouse", title: "BLE mouse", every: 800, ownsArrows: true,
      enter() { start(); },
      draw() {
        link();
        if (TOUCH) { lcd.drawRoundRect(GAP, TOP + 26, W - 2 * GAP, BY - TOP - 34, 8, lcd.DARKGREY); text("drag here to move", W / 2, (TOP + BY) / 2 + 6, F, lcd.DARKGREY, lcd.MC); hit = bottom(["Left click", "Right click"]); }
        else hint("arrows: move   Enter: click   Space: right click");
      },
      tick() { link(); },
      key(k) {
        const step = 12;
        if (k === "up") ble.hid.move(0, -step); else if (k === "down") ble.hid.move(0, step);
        else if (k === "left") ble.hid.move(-step, 0); else if (k === "right") ble.hid.move(step, 0);
        else if (k === "enter") ble.hid.click(1); else if (k === "space") ble.hid.click(2);
      },
      tap(x, y) { const b = hit ? hit(x, y) : -1; if (b >= 0) { ble.hid.click(b ? 2 : 1); held = 0; } else { mx = x; my = y; held = millis(); } },
      drag(x, y) { if (Math.abs(x - mx) + Math.abs(y - my) > 2) held = 0; ble.hid.move(Math.round((x - mx) * 1.6), Math.round((y - my) * 1.6)); mx = x; my = y; },
      release() { if (held && millis() - held < 250) ble.hid.click(1); held = 0; },
    },
    {
      icon: "G", name: "Gamepad", title: "BLE gamepad", every: 30, ownsArrows: true,
      enter() { start(); px = py = btn = 0; held = 0; joystick.keys(false); },
      draw() {
        link();
        if (TOUCH) { lcd.drawRoundRect(GAP, TOP + 26, W - 2 * GAP, BY - TOP - 34, 8, lcd.DARKGREY); text("drag here: stick", W / 2, (TOP + BY) / 2 + 6, F, lcd.DARKGREY, lcd.MC); hit = bottom(["A", "B", "X", "Y"]); }
        else hint("arrows: stick   Enter: A   Space: B");
      },
      tick() {
        const j = joystick.read(0);
        if (j.connected) { px = Math.round(j.x * 127); py = Math.round(j.y * 127); btn = j.buttons; }
        ble.hid.pad({ x: px, y: py, buttons: btn });
        if (++held % 30 === 0) link();
      },
      key(k) {
        const d = { up: [0, -127], down: [0, 127], left: [-127, 0], right: [127, 0] }[k];
        if (d) { px = d[0]; py = d[1]; setTimeout(() => { px = py = 0; }, 150); }
        else if (k === "enter" || k === "space") { btn = k === "enter" ? 1 : 2; setTimeout(() => { btn = 0; }, 150); }
      },
      tap(x, y) { const b = hit ? hit(x, y) : -1; if (b >= 0) btn = 1 << b; else { mx = x; my = y; } },
      drag(x, y) { if (!btn) { px = Math.max(-127, Math.min(127, (x - mx) * 4)); py = Math.max(-127, Math.min(127, (y - my) * 4)); } },
      release() { px = py = btn = 0; },
    },
    {
      icon: "T", name: "Input test", title: "Input test", every: 300,
      enter() { log = []; mouse = "up"; if (keyboard.capture) keyboard.capture(true); if (ble.host) ble.host.begin(); },  // Escape shows up here instead of leaving
      leave() { if (keyboard.capture) keyboard.capture(false); },
      key(k) { log.push(k); if (log.length > 4) log.shift(); },
      tap(x, y) { mouse = `DOWN ${x},${y}`; },
      drag(x, y) { mouse = `DOWN ${x},${y}`; },
      release() { mouse = "up"; },
      draw() {
        clear();
        const j = joystick.read(0), st = ble.host ? ble.host.status() : null, R = 17;
        const row = (i, label, v, col) => { text(label, 4, TOP + 4 + i * R, 1, lcd.DARKGREY); text(v, 66, TOP + 4 + i * R, 1, col || lcd.WHITE); };
        row(0, "uptime", `${Math.round(millis() / 1000)} s   (back to 0 = the board reset)`, lcd.LIGHTGREY);
        row(1, "bluetooth", st ? (st.devices.length ? st.devices.map((d) => `${d.name || d.mac} (${d.kind})`).join(", ") : "no device connected") : "-", st && st.devices.length ? lcd.GREEN : lcd.ORANGE);
        row(2, "reports", st ? `${st.rx} received, ${st.pads} gamepad updates` : "-", lcd.WHITE);
        row(3, "mouse", mouse, mouse === "up" ? lcd.LIGHTGREY : lcd.GREEN);
        row(4, "stick L", j.connected ? `${j.x.toFixed(2)}  ${j.y.toFixed(2)}` : "-", j.connected ? lcd.GREEN : lcd.LIGHTGREY);
        row(5, "stick R", j.connected ? `${j.x2.toFixed(2)}  ${j.y2.toFixed(2)}   hat ${j.hat}` : "-", lcd.WHITE);
        row(6, "buttons", j.connected ? `0x${j.buttons.toString(16)}   L ${j.lt.toFixed(2)}  R ${j.rt.toFixed(2)}` : "-", lcd.WHITE);
        row(7, "keys", log.length ? log.join("  ") : "(press something)", lcd.YELLOW);
        row(8, "boot log", String(board.bootLog || "").split(";").slice(-4).join(" "), lcd.DARKGREY);
      },
      tick() { this.draw(); },
    },
    ...(ble.host && ble.host.raw ? [{
      icon: "R", name: "Raw reports", title: "Raw reports", every: 200,  // what a controller sends outside its keyboard / mouse reports
      enter() { rawLog = []; note = ""; ble.host.raw(true); },
      leave() { ble.host.raw(false); },
      tick() { const r = ble.host.raw(); if (r.length) { rawLog = rawLog.concat(r.map((x) => `${x.id}: ${x.hex}`)).slice(-7); this.draw(); } },
      send(via) {  // Valve's "no lizard mode" commands (from the USB protocol): clear the key mappings, then trackpad modes 7 (off)
        const pad = (a) => a.concat(Array(19 - a.length).fill(0)), cmds = [pad([0x81, 0]), pad([0x87, 9, 0x07, 7, 0, 0x08, 7, 0, 0x18, 0, 0])];
        try { cmds.forEach((m) => (via ? ble.host.valve("", m) : ble.host.feature("", 3, m))); note = "sent via " + (via ? "Valve char" : "HID feature"); } catch (e) { note = String(e.message || e); }
        this.draw();
      },
      draw() {
        clear();
        rawLog.forEach((l, i) => text(l, 4, TOP + 4 + i * 14, 1, lcd.YELLOW));
        if (!rawLog.length) text("press buttons on the controller", 4, TOP + 6, 1, lcd.DARKGREY);
        text(note, 4, BY - 16, 1, lcd.GREEN);
        if (TOUCH) hit = bottom(["Lizard off", "Off (Valve)", "Clear"]); else hint("L: lizard off   V: via Valve char   C: clear");
      },
      key(k) { if (k === "l") this.send(0); else if (k === "v") this.send(1); else if (k === "c") { rawLog = []; this.draw(); } },
      tap(x, y) { const b = hit ? hit(x, y) : -1; if (b === 0) this.send(0); else if (b === 1) this.send(1); else if (b === 2) { rawLog = []; note = ""; this.draw(); } },
    }] : []),
    ...(ble.host ? [{
      icon: "P", name: "Pair a device", title: "BLE devices", every: 600, ownsArrows: true,
      enter() { cur = 0; this.p = 0; ble.host.begin(); status = ""; this.scan(); },
      scan() {
        text("scanning...", 6, TOP + 6, F, lcd.YELLOW);
        seen = ble.scan(4).sort((a, b) => (b.hid - a.hid) || (b.rssi - a.rssi));  // (connectable is unreliable: a scan response can hide it)
        this.draw();
      },
      per() { return TOUCH ? 3 : Math.floor((H - TOP - 36) / line); },
      shown() { return seen.filter((d) => showAll || d.hid); },
      list() { return this.shown().slice((this.p || 0) * this.per(), (this.p || 0) * this.per() + this.per()); },
      paging() { return this.shown().length > this.per(); },
      page(d) { const pages = Math.ceil(this.shown().length / this.per()); this.p = ((this.p || 0) + d + pages) % pages; this.draw(); },
      pick(i) {
        const d = this.list()[i];
        if (!d) return;
        status = "connecting...";
        ble.host.connect(d.mac, d.type);
        this.draw();
      },
      draw() {
        clear();
        const rows = this.list(), rh = TOUCH ? 34 : line;
        if (!rows.length) text(seen.length ? "no keyboards / mice / gamepads here" : "nothing found", 6, TOP + 8, F, lcd.ORANGE);
        if (!rows.length) text("put the device in pairing mode", 6, TOP + 8 + 2 * line, 1, lcd.LIGHTGREY);
        rows.forEach((d, i) => {
          const y = TOP + 4 + i * rh, on = i === cur && !TOUCH;
          lcd.fillRoundRect(2, y - 2, W - 4, rh - 4, 6, on ? ACCENT : CELL);
          bars(8, y + (rh - 18) / 2, d.rssi);
          lcd.setTextDatum(lcd.TL); lcd.setTextColor(lcd.WHITE); lcd.drawString((d.hid ? "" : "? ") + (d.name || d.mac), 32, y + (rh - 8 * F) / 2 - 2, F);
        });
        this.status();
        c.header ? c.header(this.title) : 0;
        if (TOUCH) hit = bottom(["Rescan", showAll ? "All" : "HID only", "Forget all"]);
        else hint("Up/Down: choose   Enter: pair   R: rescan   F: forget all");
      },
      status() {
        const st = ble.host.status(), ok = st.devices.map((d) => `${d.name || d.mac} (${d.kind})`).join(", ");
        const msg = st.state === "pairing" ? (st.passkey ? `type ${st.passkey} on the keyboard` : "pairing...") : st.state === "failed" ? st.error : st.state === "connected" ? "" : st.state === "connecting" ? "connecting..." : status;
        const names = st.devices.map((d) => d.name || d.mac).join(", ");
        lcd.fillRect(0, BY - 48, W, 46, BG);
        text(names ? "Connected: " + names : "Connected: nothing yet", 6, BY - 46, 1, names ? lcd.GREEN : lcd.DARKGREY);
        text(`${this.shown().length} found`, W - 4, BY - 46, 1, lcd.DARKGREY, lcd.TR);
        text(msg, 6, BY - 22, F, st.state === "failed" ? lcd.RED : st.passkey ? lcd.YELLOW : lcd.GREEN);
      },
      tick() { this.status(); },
      key(k) {
        const n = this.list().length;
        if (k === "up") cur = (cur + n - 1) % Math.max(1, n);
        else if (k === "down") cur = (cur + 1) % Math.max(1, n);
        else if (k === "n") return this.page(1);
        else if (k === "enter" || k === "right") return this.pick(cur);
        else if (k === "r") return this.scan();
        else if (k === "a") showAll = !showAll;
        else if (k === "f") { ble.host.forget("*"); status = "forgot all paired devices"; }
        else return;
        this.draw();
      },
      tap(x, y) {
        const b = hit ? hit(x, y) : -1;
        if (b === 0) return this.scan();
        if (b === 1) { showAll = !showAll; return this.draw(); }
        if (b === 2) { ble.host.forget("*"); status = "forgot all paired devices"; return this.draw(); }
        const i = Math.floor((y - TOP - 2) / (TOUCH ? 34 : line));
        if (i >= 0 && i < this.list().length) { cur = i; this.pick(i); }
      },
    }] : []),
  ];
}
