#!/usr/bin/env node
// A small web server for the project's pages (the web IDE needs a page served from localhost, or a
// file, for Chrome to allow Web Serial). Only this computer can reach it.
//
//   node tools/serve.js            then open http://localhost:8000/tools/web-ide.html
//   node tools/serve.js 9000       another port
//   npm run ide                    the same as the first line
import { createServer } from "node:http";
import { createReadStream, statSync } from "node:fs";
import { dirname, extname, join, normalize, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const PORT = +(process.argv[2] || 8000);
const TYPES = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8",
  ".json": "application/json", ".md": "text/plain; charset=utf-8", ".txt": "text/plain; charset=utf-8",
  ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".gif": "image/gif", ".svg": "image/svg+xml",
  ".bmp": "image/bmp", ".mp3": "audio/mpeg", ".avi": "video/x-msvideo", ".bin": "application/octet-stream",
};
const HIDDEN = ["node_modules", ".pio", ".git", "backup"];  // never served

const server = createServer((req, res) => {
  let path;
  try { path = decodeURIComponent(new URL(req.url, "http://localhost").pathname); }
  catch { res.writeHead(400).end("bad request"); return; }
  if (path === "/") { res.writeHead(302, { Location: "/tools/web-ide.html" }).end(); return; }
  const file = normalize(join(ROOT, path));
  const parts = file.slice(ROOT.length).split(sep);
  // stay inside the project, and keep personal settings and build folders private
  if (!file.startsWith(ROOT + sep) || parts.some((p) => HIDDEN.includes(p)) || /micro-js\.json$|esp32-js\.json$/.test(file)) {
    res.writeHead(404).end("not found");
    return;
  }
  let st;
  try { st = statSync(file); } catch { st = null; }
  if (!st || !st.isFile()) { res.writeHead(404).end("not found"); return; }
  res.writeHead(200, { "Content-Type": TYPES[extname(file).toLowerCase()] || "application/octet-stream", "Content-Length": st.size, "Cache-Control": "no-store" });
  createReadStream(file).pipe(res);
  console.log(`${req.method} ${path}`);
});
server.on("error", (e) => {
  console.error(e.code === "EADDRINUSE" ? `Port ${PORT} is in use - try: node tools/serve.js ${PORT + 1}` : e.message);
  process.exit(1);
});
server.listen(PORT, "127.0.0.1", () => {
  console.log(`micro-js pages at http://localhost:${PORT}/  -  the IDE: http://localhost:${PORT}/tools/web-ide.html`);
  console.log("Ctrl+C to stop.");
});
