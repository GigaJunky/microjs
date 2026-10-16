#!/usr/bin/env node
// Wrap an ESP32 app image (firmware.bin) as a .FLH file for the ESP32-CoCo flash menu.
//
//   node tools/make-flh.js firmware.bin MICROJS_1-0-0.FLH "release notes..."
//
// Format (from the ESP32-CoCo emulator): "<checksum>-COCO ESP32 CUSTOM FIRMWARE/<notes>~<image>",
// where checksum is the decimal sum of every byte from the '-' to the end of the file.
// Use an upper-case .FLH so the emulator's file list shows it.
import { readFileSync, writeFileSync } from "node:fs";

const [src, dst, notes = ""] = process.argv.slice(2);
if (!src || !dst) {
  console.error('usage: node tools/make-flh.js firmware.bin OUT.FLH "release notes"');
  process.exit(2);
}
const image = readFileSync(src);
if (image[0] !== 0xe9) {
  console.error(`${src}: not an ESP32 app image`);
  process.exit(1);
}
if (notes.includes("~")) {
  console.error("the notes can't contain '~'");
  process.exit(1);
}
const body = Buffer.concat([Buffer.from("-COCO ESP32 CUSTOM FIRMWARE/" + notes.replace(/\n/g, "\r\n") + "\r\n\r\n~", "latin1"), image]);
let sum = 0;
for (const b of body) sum += b;
writeFileSync(dst, Buffer.concat([Buffer.from(String(sum), "latin1"), body]));
console.log(`${dst}: ${image.length} byte image, checksum ${sum}`);
