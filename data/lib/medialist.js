// medialist.js - the files of one kind on the SD card (mp3.js, video.js: load("/lib/medialist.js")).
// Finding them means listing every folder two levels down, which takes seconds on a card with many
// files. So the result is kept as a standard .m3u playlist on the card, and its first line records
// the card's "used" figure (fs.space answers at once). If the figure is still the same, nothing was
// added or removed since, and the playlist is read instead of scanning. Where the figure can't be had
// quickly (the RP2350: the library reads the whole table of a 32 GB card for 25 s, and fs.space
// says -1), the saved playlist is trusted until it is rescanned (r in the players).
//
//   mediaFiles(ext, listFile, onDir, rescan) -> array of paths on the SD card
//     ext       ".mp3"
//     listFile  "/sd/playlist.m3u"
//     onDir     optional: onDir(folder, foundSoFar) is called before each folder is listed (a first
//               call means a scan is happening, so the app can say so)
//     rescan    true: ignore the saved playlist and scan again
//   mediaFrom   "playlist" or "scan": where the last list came from
const MEDIA_HEAD = "#microjs-playlist 1 ";
let mediaFrom = "";

const MEDIA_NONE = "--------------";  // in a playlist saved where the figure wasn't available
function mediaUsed() {  // -1: not available here
  try { const u = fs.space("/sd").used; return u > 0 ? u : -1; } catch (e) { return -1; }
}

function mediaScan(ext, dir, depth, out, onDir) {
  if (onDir) onDir(dir, out.length);
  let names = [];
  try { names = fs.list(dir); } catch (e) {}
  for (const n of names.sort()) {
    if (n.endsWith("/")) {
      if (depth < 2 && !n.startsWith(".") && n !== "System Volume Information/") mediaScan(ext, dir + "/" + n.slice(0, -1), depth + 1, out, onDir);
    } else if (n.toLowerCase().endsWith(ext)) out.push(dir + "/" + n);
  }
  return out;
}

// The "used" figure has to include the playlist file itself, so: write it with a placeholder, read the
// figure, then write it again with the figure in place (a fixed-width field: the same size).
function mediaSave(listFile, list) {
  const body = list.join("\n") + "\n";
  const head = (n) => MEDIA_HEAD + (n < 0 ? MEDIA_NONE : String(n).padStart(14, "0")) + "\n";
  try {
    fs.write(listFile, head(0) + body);
    const used = mediaUsed();
    if (used >= 0) fs.write(listFile, head(used) + body);
    else fs.write(listFile, head(-1) + body);
  } catch (e) {}
}

function mediaFiles(ext, listFile, onDir, rescan) {
  mediaFrom = "";
  if (board.hasSd === false) return [];
  const used = mediaUsed();
  if (!rescan) {
    let text = null;
    try { text = fs.read(listFile); } catch (e) {}
    const at = MEDIA_HEAD.length;
    // the figure matches - or there is no figure to compare here, so the saved list is believed
    if (text && text.startsWith(MEDIA_HEAD) && (used < 0 || Number(text.slice(at, at + 14)) === used)) {
      mediaFrom = "playlist";
      return text.slice(at + 15).split("\n").map((l) => l.trim()).filter((l) => l);
    }
  }
  const list = mediaScan(ext, "/sd", 0, [], onDir);
  mediaFrom = "scan";
  mediaSave(listFile, list);
  return list;
}
