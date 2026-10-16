// edit.js - a JavaScript editor for boards with a keyboard. Open it from the console with
//   .edit /myscript.js        (or launch("/edit.js", "/myscript.js"))
// Arrows, Home/End, PgUp/PgDn move; typing inserts; Enter keeps the indentation; Tab = 2 spaces.
// Ctrl+S (or F2) save, Ctrl+R save and run, Ctrl+Q / Escape (or F10) quit back to the console.
const W = lcd.width(), H = lcd.height(), CW = 6, CH = 8;
const COLS = Math.floor(W / CW), ROWS = Math.floor(H / CH), TEXT = ROWS - 2;
const BG = lcd.BLACK, FG = lcd.WHITE, BAR = lcd.NAVY, CURSOR = lcd.GREEN;
const C_KEYWORD = lcd.CYAN, C_STRING = lcd.YELLOW, C_NUMBER = lcd.ORANGE, C_COMMENT = lcd.DARKGREY;
const KEYWORDS = new Set(("break case catch class const continue default delete do else export extends " +
  "finally for function if import in instanceof let new of return switch this throw try typeof var void " +
  "while yield async await true false null undefined").split(" "));

const path = typeof ARG === "string" && ARG ? ARG : "/untitled.js";
const original = fs.read(path);
let lines = (original ?? "").split("\n");
let cy = 0, cx = 0, top = 0, left = 0, wantX = 0;
let modified = false, confirmQuit = false, closed = false, message = original === null ? "new file" : "";
let full = true;            // redraw everything on the next paint
const dirty = new Set();    // or just these lines

// ---- syntax colouring: one line at a time (a /* comment runs to the end of its line)
const isDigit = (c) => c >= "0" && c <= "9";
const isIdStart = (c) => (c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || c === "_" || c === "$";
const isIdChar = (c) => isIdStart(c) || isDigit(c);
const isNumChar = (c) => isIdChar(c) || c === ".";
const isPlain = (c) => !isIdChar(c) && c !== '"' && c !== "'" && c !== "`" && c !== "/";
function segments(s) {
  const out = [];
  let i = 0;
  while (i < s.length) {
    const c = s[i];
    let j = i + 1, color = FG;
    if (c === "/" && (s[i + 1] === "/" || s[i + 1] === "*")) { j = s.length; color = C_COMMENT; }
    else if (c === '"' || c === "'" || c === "`") {
      while (j < s.length && s[j] !== c) j += s[j] === "\\" ? 2 : 1;
      j = Math.min(j + 1, s.length);
      color = C_STRING;
    } else if (c >= "0" && c <= "9") {
      while (j < s.length && isNumChar(s[j])) j++;
      color = C_NUMBER;
    } else if (isIdStart(c)) {
      while (j < s.length && isIdChar(s[j])) j++;
      if (KEYWORDS.has(s.slice(i, j))) color = C_KEYWORD;
    } else {
      while (j < s.length && isPlain(s[j])) j++;
    }
    out.push([i, s.slice(i, j), color]);
    i = j;
  }
  return out;
}

lcd.setTextFont(1);
lcd.setTextSize(1);
lcd.setTextDatum(lcd.TL);
lcd.setTextPadding(0);

function drawLine(n) {  // file line n at its screen row
  const row = n - top + 1;
  if (row < 1 || row > TEXT) return;
  const y = row * CH;
  lcd.fillRect(0, y, W, CH, BG);
  const s = lines[n];
  if (s === undefined) return;
  for (const [start, text, color] of segments(s)) {
    const a = Math.max(start, left), b = Math.min(start + text.length, left + COLS);
    if (a >= b) continue;
    lcd.setTextColor(color);
    lcd.drawString(text.slice(a - start, b - start), (a - left) * CW, y);
  }
  if (n === cy) {  // cursor: a block with the character under it in black
    const x = (cx - left) * CW;
    lcd.fillRect(x, y, CW, CH, CURSOR);
    const ch = s[cx];
    if (ch && ch !== " ") {
      lcd.setTextColor(BG);
      lcd.drawString(ch, x, y);
    }
  }
}

function bars() {
  lcd.fillRect(0, 0, W, CH, BAR);
  lcd.fillRect(0, (ROWS - 1) * CH, W, CH, BAR);
  lcd.setTextColor(FG);
  const name = path.length > COLS - 14 ? "..." + path.slice(-(COLS - 17)) : path;
  lcd.drawString(` ${name}${modified ? " *" : ""}`, 0, 0);
  lcd.setTextDatum(lcd.TR);
  lcd.drawString(`${cy + 1}:${cx + 1} `, W, 0);
  lcd.setTextDatum(lcd.TL);
  lcd.setTextColor(message ? lcd.YELLOW : lcd.LIGHTGREY);
  lcd.drawString(" " + (message || "^S save  ^R run  ^Q quit  (F2 / F10)"), 0, (ROWS - 1) * CH);
}

function paint() {
  // keep the cursor on screen
  if (cy < top) { top = cy; full = true; }
  if (cy >= top + TEXT) { top = cy - TEXT + 1; full = true; }
  if (cx < left) { left = Math.max(0, cx - 8); full = true; }
  if (cx >= left + COLS - 1) { left = cx - COLS + 9; full = true; }
  if (full) {
    for (let n = top; n < top + TEXT; n++) drawLine(n);
    full = false;
  } else for (const n of dirty) drawLine(n);
  dirty.clear();
  bars();
}

function moveTo(y, x, keepWant) {
  dirty.add(cy);
  cy = Math.max(0, Math.min(lines.length - 1, y));
  cx = Math.max(0, Math.min(lines[cy].length, x));
  if (!keepWant) wantX = cx;
  dirty.add(cy);
}

function changed(n) {
  modified = true;
  dirty.add(n);
}

function insert(text) {
  const s = lines[cy];
  lines[cy] = s.slice(0, cx) + text + s.slice(cx);
  changed(cy);
  moveTo(cy, cx + text.length);
}

function newline() {
  const s = lines[cy];
  let n = 0;
  while (s[n] === " " || s[n] === "\t") n++;
  const indent = s.slice(0, n);
  lines.splice(cy + 1, 0, indent + s.slice(cx));
  lines[cy] = s.slice(0, cx);
  modified = full = true;
  moveTo(cy + 1, indent.length);
}

function backspace() {
  if (cx > 0) {
    const s = lines[cy];
    lines[cy] = s.slice(0, cx - 1) + s.slice(cx);
    changed(cy);
    moveTo(cy, cx - 1);
  } else if (cy > 0) {
    const prevLen = lines[cy - 1].length;
    lines[cy - 1] += lines[cy];
    lines.splice(cy, 1);
    modified = full = true;
    moveTo(cy - 1, prevLen);
  }
}

function del() {
  const s = lines[cy];
  if (cx < s.length) {
    lines[cy] = s.slice(0, cx) + s.slice(cx + 1);
    changed(cy);
  } else if (cy < lines.length - 1) {
    lines[cy] += lines[cy + 1];
    lines.splice(cy + 1, 1);
    modified = full = true;
  }
}

function save() {
  if (fs.write(path, lines.join("\n"))) {
    modified = false;
    message = `saved ${path}`;
  } else message = `cannot write ${path}`;
}

function quit(confirmed) {
  if (modified && !confirmed) {
    confirmQuit = true;  // the next key decides: quit again discards, anything else carries on
    message = "unsaved changes: quit again to discard, ^S to save";
    return;
  }
  closed = true;
  keyboard.off();
  keyboard.capture(false);
  lcd.fillScreen(lcd.BLACK);
  console.log(modified ? `closed ${path} without saving` : `closed ${path}`);
  // nothing is running any more, so the console comes back
}

keyboard.capture(true);  // Escape and Ctrl+C are ours while editing
keyboard.on("key", (e) => {
  const k = e.key, confirmed = confirmQuit;
  confirmQuit = false;
  message = "";
  if (e.ctrl && k.length === 1) {
    const c = k.toLowerCase();
    if (c === "s") save();
    else if (c === "q") quit(confirmed);
    else if (c === "r") { save(); if (!modified) { keyboard.capture(false); launch(path); return; } }
    else if (c === "a") moveTo(cy, 0);
    else if (c === "e") moveTo(cy, lines[cy].length);
    else if (c === "k") { lines[cy] = lines[cy].slice(0, cx); changed(cy); }
  } else if (k === "Escape" || k === "F10") quit(confirmed);
  else if (k === "F2") save();
  else if (k === "ArrowLeft") cx > 0 ? moveTo(cy, cx - 1) : cy > 0 && moveTo(cy - 1, lines[cy - 1].length);
  else if (k === "ArrowRight") cx < lines[cy].length ? moveTo(cy, cx + 1) : cy < lines.length - 1 && moveTo(cy + 1, 0);
  else if (k === "ArrowUp") moveTo(cy - 1, wantX, true);
  else if (k === "ArrowDown") moveTo(cy + 1, wantX, true);
  else if (k === "Home") moveTo(cy, 0);
  else if (k === "End") moveTo(cy, lines[cy].length);
  else if (k === "PageUp") moveTo(cy - TEXT, wantX, true);
  else if (k === "PageDown") moveTo(cy + TEXT, wantX, true);
  else if (k === "Enter") newline();
  else if (k === "Backspace") backspace();
  else if (k === "Delete") del();
  else if (k === "Tab") insert("  ");
  else if (k.length === 1) insert(k);
  if (!closed) paint();
});

lcd.fillScreen(BG);
paint();
