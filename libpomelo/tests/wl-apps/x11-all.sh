#!/bin/sh
# X11 後端回歸：每個程式在 Xvfb :97（POMELO_BACKEND=x11）開起來、截圖、做一點操作
. "$(dirname "$0")/lib.sh"
fresh; xstart; H=$P/home; mkdir -p $H/pics $H/docs; cp $P/pics/* $H/pics/; cd $H
cp $P/pics/rt.docx $H/ 2>/dev/null
B=$POMELO_ROOT
run() { # 名稱 指令… ：開起來、截圖、結束
  n=$1; shift; "$@" > $P/x-$n.log 2>&1 & p=$!; sleep 3; xfocus_any; xsnap x11-$n; echo "$n rc-alive=$(kill -0 $p 2>/dev/null && echo yes || echo NO) log=$(head -c 100 $P/x-$n.log)"; kill $p 2>/dev/null; wait $p 2>/dev/null; }
xfocus_any() { :; }
run calc $B/pomelo-calc/pomelo-calc
run edit $B/pomelo-edit/pomelo-edit $H/docs/../pics/one.png
run viewer $P/bin/pomelo-viewer $H/pics/one.png
run paint $P/bin/pomelo-paint
run writer $B/pomelo-writer/pomelo-writer $H/rt.docx
run sheet $B/pomelo-sheet/pomelo-sheet
run slides $B/pomelo-slides/pomelo-slides
run pdf $B/pomelo-pdf/pomelo-pdf $P/pics/three.pdf
xstop
