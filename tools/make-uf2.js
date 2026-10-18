#!/usr/bin/env node
// Make one UF2 file (the drag-and-drop format of the RP2040/RP2350 bootloader) out of binary
// images and their flash addresses - for the release, the firmware and the scripts' filesystem in
// a single file, so one drag onto the board's RP2350 drive installs both.
//
//   node tools/make-uf2.js out.uf2 firmware.bin@0x10000000 littlefs.bin@0x107ff000
//
// Pieces of 256 bytes that are all 0xFF (erased flash: most of an empty filesystem) are left out of
// all but the first image - the bootloader erases each 4 KB sector it writes to, and a filesystem
// treats sectors it isn't using as free, so the result is the same in a much smaller file.
//
// Each 512-byte UF2 block carries 256 bytes for one flash address; the family ID tells the
// bootloader which chip it's for (RP2350, Arm, secure: 0xe48bff59).
import { readFileSync, writeFileSync } from "node:fs";

const FAMILY = 0xe48bff59;
const [out, ...parts] = process.argv.slice(2);
if (!out || !parts.length) {
  console.error("usage: node tools/make-uf2.js out.uf2 file.bin@0xADDRESS [file2.bin@0xADDRESS ...]");
  process.exit(2);
}
const chunks = [];
const blank = (b) => b.every((x) => x === 0xff);
for (const [n, p] of parts.entries()) {
  const at = p.lastIndexOf("@");
  if (at < 0) { console.error(`${p}: give the address as file@0x...`); process.exit(2); }
  const data = readFileSync(p.slice(0, at)), addr = parseInt(p.slice(at + 1), 16);
  if (addr % 256) { console.error(`${p}: the address must be a multiple of 256`); process.exit(2); }
  for (let off = 0; off < data.length; off += 256) {
    const piece = data.subarray(off, off + 256);
    if (n > 0 && blank(piece)) continue;
    chunks.push({ addr: addr + off, data: piece });
  }
}
const uf2 = Buffer.alloc(chunks.length * 512);
chunks.forEach((c, i) => {
  const b = uf2.subarray(i * 512, i * 512 + 512);
  b.writeUInt32LE(0x0a324655, 0);    // "UF2\n"
  b.writeUInt32LE(0x9e5d5157, 4);
  b.writeUInt32LE(0x00002000, 8);    // flags: family ID present
  b.writeUInt32LE(c.addr, 12);
  b.writeUInt32LE(256, 16);          // payload size
  b.writeUInt32LE(i, 20);            // block number
  b.writeUInt32LE(chunks.length, 24);
  b.writeUInt32LE(FAMILY, 28);
  c.data.copy(b, 32);
  b.writeUInt32LE(0x0ab16f30, 508);  // end magic
});
writeFileSync(out, uf2);
console.log(`${out}: ${chunks.length} blocks (${(chunks.length * 256 / 1024).toFixed(0)} KB of flash)`);
