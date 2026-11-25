#!/bin/sh
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
start; fresh; H=$P/home; cd $H
cp $P/pics/rt.docx $H/ 2>/dev/null
env -u DISPLAY $POMELO_ROOT/pomelo-writer/pomelo-writer > $P/writer.log 2>&1 &
WP=$!
sleep 2.5
click 222 42; snap writer-wl-menu2; keyk Escape; sleep 0.3
click 640 300 1; snap writer-wl-nomenu; 
click 480 78; snap writer-wl-fontmenu; keyk Escape
DND_FILES=$H/rt.docx $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 &
DP=$!
sleep 1.5; movewin -450 150
read -r sx sy sw sh <<EOT2
$(topwin)
EOT2
dragto $((sx + 60)) $((sy + 110)) 800 400
kill $DP; sleep 0.5
snap writer-wl-dnd
kill $WP; stop
