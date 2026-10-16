#!/usr/bin/env node
// Copy files to a micro-js board over USB serial - scripts, pictures, sounds, anything - with no
// reflashing. (The same thing in a browser: tools/web-ide.html.)
//
//   npm install                                                      (once: gets the serialport package)
//   node tools/upload.js /dev/ttyUSB0 data/paint.js                  upload to the board's flash
//   node tools/upload.js /dev/ttyUSB0 data/paint.js --run            upload and run it, showing its output
//   node tools/upload.js COM5 myapp.js --as main.js                  upload as the boot script
//   node tools/upload.js /dev/ttyACM0 song.mp3 pic.jpg --to /sd/music    into a folder (here on the SD card)
//
// It talks to the board's REPL: each piece of a file goes as base64 (fs.writeBase64) to a temporary
// name, then replaces the real one, so an interrupted upload never leaves half a file.
import { readFileSync } from "node:fs";
import { basename } from "node:path";

let SerialPort;
try {
  ({ SerialPort } = await import("serialport"));
} catch (e) {
  console.error("This needs the serialport package: run  npm install  in the project folder first.");
  process.exit(1);
}

// ---- arguments
const args = process.argv.slice(2);
const opt = { run: false, as: null, to: "/", baud: 115200 };
const files = [];
let portPath = null;
for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "--run") opt.run = true;
  else if (a === "--as") opt.as = args[++i];
  else if (a === "--to") opt.to = args[++i];
  else if (a === "--baud") opt.baud = +args[++i];
  else if (a === "-h" || a === "--help") usage(0);
  else if (!portPath) portPath = a;
  else files.push(a);
}
function usage(code) {
  console.log(readFileSync(new URL(import.meta.url), "utf8").split("\n").slice(1, 12).map((l) => l.replace(/^\/\/ ?/, "")).join("\n"));
  process.exit(code);
}
if (!portPath || !files.length) usage(2);
if (opt.as && files.length !== 1) { console.error("--as works with a single file"); process.exit(2); }
const dir = opt.to.replace(/\/+$/, "") || "";

// ---- serial and the reply protocol (as in tools/web-ide.html)
const port = new SerialPort({ path: portPath, baudRate: opt.baud, autoOpen: false, hupcl: false });
let rx = "", seq = 0, streaming = false;
const pending = new Map();
port.on("data", (d) => {
  const text = d.toString("latin1");
  if (streaming) process.stdout.write(text);
  rx += text;
  let m;
  while ((m = rx.match(/@@(\d+):(.*?):@@/))) {
    rx = rx.slice(m.index + m[0].length);
    const p = pending.get(+m[1]);
    if (!p) continue;
    pending.delete(+m[1]);
    clearTimeout(p.timer);
    try {
      const r = JSON.parse(m[2]);
      if ("err" in r) p.reject(new Error(r.err)); else p.resolve(r.ok);
    } catch (e) { p.reject(e); }
  }
  if (rx.length > 20000) rx = rx.slice(-5000);
});
const write = (s) => new Promise((res, rej) => port.write(s, (e) => (e ? rej(e) : port.drain(res))));
function call(expr, timeout = 15000) {
  const id = ++seq;
  const line = `console.log("@" + "@${id}:" + JSON.stringify((() => { try { return { ok: (${expr}) }; } catch (e) { return { err: String(e && e.message || e) }; } })()) + ":@@")\r\n`;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error("the board didn't answer - is micro-js running on it?")); }, timeout);
    pending.set(id, { resolve, reject, timer });
    write(line).catch(reject);
  });
}
const q = JSON.stringify;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function upload(local, remote) {
  const bytes = readFileSync(local), tmp = remote + ".part", CHUNK = 480;
  if (!bytes.length && !(await call(`fs.writeBase64(${q(tmp)}, "")`))) throw new Error("can't write " + remote);
  for (let off = 0; off < bytes.length; off += CHUNK) {
    const b64 = bytes.subarray(off, off + CHUNK).toString("base64");
    if (!(await call(`fs.writeBase64(${q(tmp)}, "${b64}", ${off > 0})`))) throw new Error(`the board couldn't write ${remote} (full, or no such folder?)`);
    process.stdout.write(`\r${local} -> ${remote}  ${Math.min(100, Math.round(((off + CHUNK) / bytes.length) * 100))}%`);
  }
  await call(`(fs.exists(${q(remote)}) ? fs.remove(${q(remote)}) : true) && fs.rename(${q(tmp)}, ${q(remote)})`);
  console.log(`\r${local} -> ${remote}  ${bytes.length} bytes`);
}

// ---- go
await new Promise((res, rej) => port.open((e) => (e ? rej(e) : res())));
await new Promise((res) => port.set({ dtr: false, rts: false }, res));  // keep the ESP32 out of reset
await sleep(300);
await write("\x03");  // Ctrl-C: stop whatever is running
await sleep(300);
await write("\r\n");
await sleep(200);
try {
  const v = await call("sys.version || ''");
  if (!v) console.warn("note: this board's firmware is older than this tool expects - flash a current release");
  if (dir && dir !== "/" && !(await call(`fs.exists(${q(dir)})`))) await call(`fs.mkdir(${q(dir)})`);
  let last = null;
  for (const f of files) {
    last = (dir === "" ? "" : dir) + "/" + (opt.as || basename(f));
    await upload(f, last);
  }
  if (opt.run && last) {
    console.log(`running ${last} - its output follows (Ctrl+C to stop)`);
    streaming = true;
    await write(`launch(${q(last)})\r\n`);
    process.on("SIGINT", async () => { await write("\x03"); port.close(() => process.exit(0)); });
  } else port.close();
} catch (e) {
  console.error("\n" + e.message);
  port.close(() => process.exit(1));
}
