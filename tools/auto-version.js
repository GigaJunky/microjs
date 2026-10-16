#!/usr/bin/env node
// Automatic version numbers. Run before each build (PlatformIO calls it through
// tools/auto_version.py); you can also run it by hand: node tools/auto-version.js
//
// It fingerprints the code that ends up on a board - src/, lib/, data/ and platformio.ini - and, if
// that changed since the version in src/version.h was set, bumps the version's last number
// (1.0.0 -> 1.0.1). So every build with different code has a different version, shown at boot,
// in the launcher and as sys.version.
//
// - One change, one bump: building several boards, or building again, doesn't bump again.
// - Your own bumps win: set src/version.h to e.g. 1.1.0 for a feature release (or 2.0.0 when
//   scripts would break) and that's recorded as is; changes after it count up from there.
// - Not counted: per-board state the apps write (high scores, sound, touch calibration, launcher
//   choices), your settings file, and the build output.
// The last version and fingerprint are kept in tools/version-state.json (commit it with the code).
import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = process.argv[2] || join(dirname(fileURLToPath(import.meta.url)), "..");
const VERSION_H = join(ROOT, "src", "version.h");
const STATE = join(ROOT, "tools", "version-state.json");
const SKIP_NAMES = new Set(["version.h", "micro-js.json", "esp32-js.json", "cyd-js.json", "sound.json", "touchcal.json", "launcher.json"]);
const SKIP_SUFFIXES = [".hi", ".part"];

function walk(dir, out) {
  if (!existsSync(dir)) return out;
  const names = readdirSync(dir).filter((n) => !n.startsWith(".")).sort();  // same order as Python's sorted()
  for (const n of names) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) continue;
    if (!SKIP_NAMES.has(n) && !SKIP_SUFFIXES.some((s) => n.endsWith(s))) out.push(p);
  }
  for (const n of names) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) walk(p, out);
  }
  return out;
}

function fingerprint() {
  const h = createHash("sha256");
  const paths = [join(ROOT, "platformio.ini")];
  for (const top of ["src", "lib", "data"]) walk(join(ROOT, top), paths);
  for (const p of paths) {
    h.update(relative(ROOT, p));
    h.update(readFileSync(p));
  }
  return h.digest("hex");
}

const text = readFileSync(VERSION_H, "utf8");
const m = text.match(/MICROJS_VERSION "(\d+)\.(\d+)\.(\d+)([^"]*)"/);
if (!m) {
  console.log("auto-version: no MICROJS_VERSION in src/version.h - leaving it alone");
  process.exit(0);
}
let version = `${m[1]}.${m[2]}.${m[3]}${m[4]}`;
let state = existsSync(STATE) ? JSON.parse(readFileSync(STATE, "utf8")) : {};
const fp = fingerprint();
if (state.version !== version || !state.hash) {
  state = { version, hash: fp };  // first run, or version.h was changed by hand: take it as it is
} else if (state.hash !== fp) {
  version = `${m[1]}.${m[2]}.${+m[3] + 1}`;  // a pre-release label is dropped
  writeFileSync(VERSION_H, text.replace(m[0], `MICROJS_VERSION "${version}"`));
  state = { version, hash: fingerprint() };
  console.log(`auto-version: the code changed - version is now ${version}`);
} else process.exit(0);
writeFileSync(STATE, JSON.stringify(state, null, 2) + "\n");
