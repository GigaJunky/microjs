#!/usr/bin/env bash
# Convert videos (.mp4 .mkv .mov .webm ... anything ffmpeg reads) into .avi files that the file manager (files.js)
# plays: Motion-JPEG video sized for the board's screen, with MP3 sound.
#
#   tools/convert-videos.sh [options] FILE_OR_FOLDER...
#
#   -b, --board NAME   cyd-s3 (default), cardputer, cyd, vga - sets the size and frame rate
#   -o, --out DIR      where the .avi files go (default: next to each original)
#   -c, --copy DIR     also copy each finished .avi there (e.g. the SD card's videos folder)
#   -f, --fps N        frames per second (default: the most the board keeps up with - on the
#                      CYD-S3 30 for widescreen and portrait videos, 24 for full-screen 4:3 ones)
#   -q, --quality N    picture quality 2 (best, biggest) .. 31 (worst, smallest); default 6
#   -m, --mute         no sound track
#       --force        convert again even if the .avi is already there and newer
#
# Folders are searched (subfolders too) for video files - not .avi, which is what this makes; name an
# .avi file directly to convert it. Examples:
#   tools/convert-videos.sh ~/Videos/cartoon.mp4
#   tools/convert-videos.sh -c /media/$USER/SDCARD/videos ~/Videos/clips
#   tools/convert-videos.sh -b cardputer -o out/ *.mkv
set -euo pipefail

board=cyd-s3 out="" copy="" fps="" quality=6 mute=0 force=0
inputs=()
while (($#)); do
  case "$1" in
    -b|--board) board=$2; shift ;;
    -o|--out) out=$2; shift ;;
    -c|--copy) copy=$2; shift ;;
    -f|--fps) fps=$2; shift ;;
    -q|--quality) quality=$2; shift ;;
    -m|--mute) mute=1 ;;
    --force) force=1 ;;
    -h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $1 (try --help)" >&2; exit 2 ;;
    *) inputs+=("$1") ;;
  esac
  shift
done
((${#inputs[@]})) || { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
command -v ffmpeg >/dev/null || { echo "ffmpeg is needed: sudo apt install ffmpeg (or brew install ffmpeg)" >&2; exit 1; }

# the screen each board shows video on, and a frame rate it keeps up with
case "$board" in
  cyd-s3|s3)      W=320 H=240 F=auto ;;  # ESP32-S3 CYD: 30 fps at 320x180, ~24 at 320x240 (measured)
  cardputer)      W=240 H=135 F=20 ;;
  cyd|cyd2usb)    W=320 H=240 F=10 ;;  # classic ESP32: slower, and no sound output for music
  vga|coco)       W=320 H=240 F=15 ;;
  *) echo "unknown board $board (cyd-s3, cardputer, cyd, vga)" >&2; exit 2 ;;
esac
want_fps=${fps:-$F}

# every video file named on the command line, or found in the folders named
files=()
for i in "${inputs[@]}"; do
  if [[ -d $i ]]; then
    while IFS= read -r -d '' f; do files+=("$f"); done < <(find "$i" -type f \( -iname '*.mp4' -o -iname '*.mkv' -o \
      -iname '*.mov' -o -iname '*.webm' -o -iname '*.m4v' -o -iname '*.wmv' -o -iname '*.flv' -o \
      -iname '*.mpg' -o -iname '*.mpeg' -o -iname '*.ts' -o -iname '*.3gp' \) -print0 | sort -z)
  elif [[ -f $i ]]; then files+=("$i")
  else echo "skipping $i: no such file or folder" >&2
  fi
done
((${#files[@]})) || { echo "no video files found" >&2; exit 1; }
[[ -n $out ]] && mkdir -p "$out"
[[ -n $copy ]] && { mkdir -p "$copy" || { echo "can't write to $copy" >&2; exit 1; }; }

audio=(-c:a libmp3lame -b:a 96k -ar 44100 -ac 1)
((mute)) && audio=(-an)
done_n=0 skip_n=0 fail_n=0
for f in "${files[@]}"; do
  base=$(basename "${f%.*}")
  dir=${out:-$(dirname "$f")}
  dest="$dir/$base.avi"
  if [[ $f -ef $dest ]]; then dest="$dir/$base-$board.avi"; fi  # don't overwrite an .avi input
  if ((!force)) && [[ -f $dest && $dest -nt $f ]]; then
    echo "= $base.avi is already converted (--force to redo)"
    skip_n=$((skip_n + 1))
  else
    fps=$want_fps
    if [[ $fps == auto ]]; then  # by the size it will be on screen: fewer pixels, more frames
      IFS=, read -r vw vh < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$f" || echo "0,0")
      vw=${vw//[^0-9]/} vh=${vh//[^0-9]/}
      fps=24
      if ((vw > 0 && vh > 0)); then
        if ((vw * H >= vh * W)); then ow=$W oh=$((vh * W / vw)); else oh=$H ow=$((vw * H / vh)); fi
        ((ow * oh <= 320 * 190)) && fps=30
      fi
    fi
    echo "> $f  ($fps fps)"
    # shrink to fit the screen keeping the shape (the player centres it), even sizes for the encoder
    if ffmpeg -nostdin -hide_banner -loglevel error -stats -y -i "$f" \
        -vf "scale=w=$W:h=$H:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=$fps" \
        -c:v mjpeg -q:v "$quality" -pix_fmt yuvj420p "${audio[@]}" "$dest"; then
      size=$(du -h "$dest" | cut -f1)
      echo "  -> $dest ($size)"
      done_n=$((done_n + 1))
    else
      echo "  !! failed: $f" >&2
      rm -f "$dest"
      fail_n=$((fail_n + 1))
      continue
    fi
  fi
  if [[ -n $copy ]]; then cp "$dest" "$copy/" && echo "  copied to $copy/"; fi
done
[[ -n $copy ]] && sync
echo "converted $done_n, already done $skip_n, failed $fail_n  (board $board: up to ${W}x$H)"
((fail_n == 0))
