#!/usr/bin/env bash
# Plays 16 grey bars (BT.709, limited range) in Android's Gallery player, screenshots its window and
# prints each bar's value next to the value a display without colour conversion shows.  Shows
# whether Android lifts the shadows of SDR video.  Needs a running Waydroid session, adb connected
# to it (ANDROID_SERIAL), Hyprland (hyprctl) and grim.  The clip is pushed once, to
# /storage/emulated/0/Movies/ramp.mp4.
# usage: ramp-check.sh [SCREENSHOT.png]
set -euo pipefail

shot=${1:-ramp-check.png}
file=/storage/emulated/0/Movies/ramp.mp4
lookup() {
   adb shell content query --uri content://media/external/video/media --projection _id \
      --where "_data=\'$file\'" | sed -n 's/.*_id=\([0-9]*\).*/\1/p'
}

if [ -z "$(adb shell ls "$file" 2> /dev/null)" ]; then
   clip=$(mktemp --suffix .mp4)
   ffmpeg -v error -y -f lavfi -i color=black:s=1920x1080:r=30:d=30 \
      -vf "geq=lum='16+floor(X*16/W)*219/15':cb=128:cr=128,format=yuv420p" \
      -c:v libx264 -profile:v high -qp 2 -preset veryfast \
      -bsf:v h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1:video_full_range_flag=0 \
      "$clip"
   adb push "$clip" "$file" > /dev/null
   rm -f "$clip"
   adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d "file://$file" > /dev/null
   sleep 3
fi
# Android's media server cannot open shared storage by path, so Gallery gets the media library entry
id=$(lookup)
[ -n "$id" ] || { echo "$file is not in the media library" >&2; exit 1; }

window() {
   hyprctl clients -j | python3 -c "
import json, sys
c = [c for c in json.load(sys.stdin) if 'gallery3d' in c['class']]
print(f\"{c[0]['at'][0]},{c[0]['at'][1]} {c[0]['size'][0]}x{c[0]['size'][1]}\" if c else '')"
}
if [ -z "$(window)" ]; then
   setsid -f waydroid app launch com.android.gallery3d > /dev/null 2>&1 < /dev/null
   sleep 6
fi
adb shell am start -a android.intent.action.VIEW -d "content://media/external/video/media/$id" \
   -t video/mp4 -n com.android.gallery3d/.app.MovieActivity > /dev/null
sleep 4
geometry=$(window)
[ -n "$geometry" ] || { echo "no Gallery window" >&2; exit 1; }
grim -g "$geometry" "$shot"

python3 - "$shot" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
h = a.shape[0]
row = a[h // 2]
grey = np.where((abs(row[:, 0] - row[:, 1]) < 3) & (abs(row[:, 1] - row[:, 2]) < 3))[0]
x0, x1 = grey.min(), grey.max()
shown = []
for k in range(16):
    xs = slice(int(x0 + (k + 0.3) * (x1 - x0) / 16), int(x0 + (k + 0.7) * (x1 - x0) / 16))
    shown.append(int(round(a[h // 2 - 20:h // 2 + 20, xs].mean())))
print('shown   ', shown)
print('expected', [round(k * 255 / 15) for k in range(16)])
PY
adb shell dumpsys SurfaceFlinger | grep -oE "activeBuffer=\[ *1920x *1080:[0-9]+,[^]]+\]" | sort -u |
   sed 's/^/video buffer: /'
