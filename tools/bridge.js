#!/usr/bin/env node
// Network and clock for a micro-js board without WiFi (the RP2350-PiZero), through the computer it's
// plugged into - and a REPL terminal at the same time.
//
//   npm install                              (once: gets the serialport package)
//   node tools/bridge.js                     finds the board by itself
//   node tools/bridge.js /dev/ttyACM0        or name its port (COM5 on Windows)
//
// While it runs, the board's clock is set from this computer (and its time zone, unless micro-js.json
// sets utcOffsetHours), wifi.connect() succeeds, and http.get / http.post are fetched by this computer
// - HTTPS too - so apps written for WiFi (weather, fetch, clock) work unchanged. Everything the board
// prints shows here, and what you type goes to its REPL. Ctrl-] quits.
//
// The board's side is MICROJS_BRIDGE in src/hw.cpp. Messages are lines that start with STX (0x02);
// fields are separated by spaces and anything that could hold a space travels as base64:
//   to the board:    STX "MJS ping" | STX "MJS time <unix ms> <UTC offset, minutes>" | STX "MJS res <id> <status> <body>"
//   from the board:  STX "MJS req <id> <method> <url> <headers as JSON> <body>" | STX "MJS gettime"
import { readFileSync } from "node:fs";

let SerialPort;
try {
  ({ SerialPort } = await import("serialport"));
} catch (e) {
  console.error("This needs the serialport package: run  npm install  in the project folder first.");
  process.exit(1);
}

const args = process.argv.slice(2);
if (args.includes("-h") || args.includes("--help")) {
  console.log(readFileSync(new URL(import.meta.url), "utf8").split("\n").slice(1, 13).map((l) => l.replace(/^\/\/ ?/, "")).join("\n"));
  process.exit(0);
}
const MAX_BODY = 400 * 1024;  // what the board can take in one answer
const STX = "\x02";
const dim = (s) => (process.stderr.isTTY ? `\x1b[2m${s}\x1b[0m` : s);
const note = (s) => process.stderr.write(dim(`[bridge] ${s}`) + "\n");
// the log shows URLs, but not the API keys and tokens in them
const safe = (url) => url.replace(/([?&][^=&]*(?:key|token|secret|password|pass)[^=&]*=)[^&]*/gi, "$1***");

// ---- the board's port: given, or the first one that looks like a micro-js board
async function findPort() {
  if (args[0]) return args[0];
  const ports = await SerialPort.list();
  const known = ["2e8a", "303a", "1a86", "10c4"];  // Raspberry Pi, Espressif, CH340, CP210x
  const p = ports.find((p) => known.includes((p.vendorId || "").toLowerCase()));
  if (!p) {
    console.error("No board found. Plug it in, or name its port: node tools/bridge.js /dev/ttyACM0");
    process.exit(1);
  }
  return p.path;
}

const path = await findPort();
const port = new SerialPort({ path, baudRate: 115200, autoOpen: false, hupcl: false });
await new Promise((ok, fail) => port.open((e) => (e ? fail(e) : ok())));
note(`connected to ${path}. Ctrl-] quits.`);

const send = (line) => port.write(STX + line + "\n", "latin1");
const sendTime = () => send(`MJS time ${Date.now()} ${-new Date().getTimezoneOffset()}`);
const b64 = (buf) => Buffer.from(buf).toString("base64");
const unb64 = (s) => (s === "-" ? "" : Buffer.from(s, "base64").toString("utf8"));

// Only a board that reaches the network this way (board.network "usb") understands these messages;
// to any other they'd be typed into its REPL. So ask first ("@" + "@" keeps the board's echo of the
// question from looking like its answer).
let ping = null, clock = null, probing = true;
function startBridge() {
  sendTime();
  send("MJS ping");
  ping = setInterval(() => send("MJS ping"), 3000);
  clock = setInterval(sendTime, 10 * 60 * 1000);
  note("giving the board the clock and the network");
}
port.write('\r\nconsole.log("@" + "@net:" + (board.network || "") + ":@@")\r\n');
const probeTimer = setTimeout(() => { probing = false; note("the board didn't say how it reaches the network (old firmware?) - terminal only"); }, 4000);
function probeAnswer(net) {
  probing = false;
  clearTimeout(probeTimer);
  if (net === "usb") startBridge();
  else note(net ? `this board has its own network (${net}) - terminal only` : "this board has no network to give - terminal only");
}

// ---- a request from the board: fetch it here, send the answer back
async function request(id, method, url, headers, body) {
  const t0 = Date.now();
  try {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 25000);
    const res = await fetch(url, { method, headers, body: method === "GET" ? undefined : body, signal: ctl.signal });
    let data = Buffer.from(await res.arrayBuffer());
    clearTimeout(timer);
    let cut = "";
    if (data.length > MAX_BODY) { data = data.subarray(0, MAX_BODY); cut = ", cut to 400 KB"; }
    send(`MJS res ${id} ${res.status} ${data.length ? b64(data) : "-"}`);
    note(`${method} ${safe(url)} -> ${res.status} (${(data.length / 1024).toFixed(1)} KB${cut}, ${Date.now() - t0} ms)`);
  } catch (e) {
    const msg = e.name === "AbortError" ? "timed out" : (e.cause && e.cause.code) || e.message;
    send(`MJS res ${id} 0 ${b64(msg)}`);
    note(`${method} ${safe(url)} -> failed: ${msg}`);
  }
}

function boardMessage(line) {
  const f = line.split(" ");
  if (f[0] !== "MJS") return;
  if (f[1] === "gettime") sendTime();
  else if (f[1] === "req" && f.length >= 7) {
    let headers = {};
    try { headers = JSON.parse(unb64(f[5]) || "{}"); } catch (e) { /* no headers */ }
    request(f[2], f[3], unb64(f[4]), headers, unb64(f[6]));
  }
}

// ---- board -> screen, with the bridge's messages taken out
let msg = null, probeBuf = "";
port.on("data", (d) => {
  let text = "";
  for (const ch of d.toString("latin1")) {
    if (msg !== null) {
      if (ch === "\n" || ch === "\r") { boardMessage(msg); msg = null; }
      else msg += ch;
    } else if (ch === STX) msg = "";
    else text += ch;
  }
  if (probing) {  // until the board has answered: hold its output back, and take the answer out
    probeBuf += text;
    const m = probeBuf.match(/@@net:(\w*):@@/);
    if (!m) return;
    text = probeBuf.slice(probeBuf.indexOf("\n", m.index) + 1);
    probeBuf = "";
    probeAnswer(m[1]);
  }
  if (text) process.stdout.write(Buffer.from(text, "latin1"));
});

// ---- keyboard -> board's REPL (Ctrl-] quits)
if (process.stdin.isTTY) process.stdin.setRawMode(true);
process.stdin.on("data", (d) => {
  if (d.includes(0x1d)) quit();
  else port.write(d);
});

function quit() {
  if (ping) clearInterval(ping);
  if (clock) clearInterval(clock);
  if (process.stdin.isTTY) process.stdin.setRawMode(false);
  note("bye - the board goes offline in a few seconds");
  port.close(() => process.exit(0));
}
port.on("close", () => { note("the board went away"); process.exit(0); });
process.on("SIGINT", quit);
