#!/bin/sh
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
fresh; xstart
H=$P/home; mkdir -p $H/docs
printf 'hello 世界\n第二行 line two\nthird line\n' > $H/docs/a.txt
printf 'dropped file\n' > $H/docs/b.txt
cd $H
$POMELO_ROOT/pomelo-edit/pomelo-edit $H/docs/a.txt > $P/editx.log 2>&1 &
EP=$!
sleep 2; xfocus pomelo-edit
xsnap edit-x11-open
xclick 640 300
xdotool type --delay 60 XYZ
xdotool key ctrl+a ctrl+c; sleep 0.3
xclick 640 300
xdotool key End ctrl+v; sleep 0.3
xsnap edit-x11-typed
xdotool key ctrl+s; sleep 0.6
xsnap edit-x11-saved
xdotool key ctrl+o; sleep 0.6
xsnap edit-x11-opendlg
xdotool key Escape; sleep 0.3
$POMELO_ROOT/pomelo-edit/pomelo-edit $H/docs/b.txt; sleep 1
xsnap edit-x11-second
xdotool key ctrl+w; sleep 0.4; xsnap edit-x11-afterw
xdotool key ctrl+q; sleep 1; xsnap edit-x11-afterq
kill -0 $EP 2>/dev/null && echo STILL || echo closed
kill $EP 2>/dev/null; xstop
