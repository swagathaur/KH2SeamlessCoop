#!/bin/bash
# both.sh: a screenshot from the host and the client side by side -> scratch/view/both.png (prints the frames).
cd "$HOME/mnt/KH2 Decomp project" || exit 1
d=tools/live
h=$($d/ctl.sh host "shot" "state" | tee /dev/stderr | grep -o "frame_[0-9]*.png")
c=$($d/ctl.sh client "shot" "state" | tee /dev/stderr | grep -o "frame_[0-9]*.png")
python3 - "$h" "$c" <<'PY'
import sys
from PIL import Image
a=Image.open('shots/live-host/'+sys.argv[1]).convert('RGB'); b=Image.open('shots/live-client/'+sys.argv[2]).convert('RGB')
S=Image.new('RGB',(a.width+b.width,max(a.height,b.height))); S.paste(a,(0,0)); S.paste(b,(a.width,0)); S.save('scratch/view/both.png')
PY
