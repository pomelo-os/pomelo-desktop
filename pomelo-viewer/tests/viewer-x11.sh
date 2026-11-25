#!/bin/sh
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
fresh; xstart
H=$P/home; mkdir -p $H/pics; cp $P/pics/* $H/pics/; cd $H
$P/bin/pomelo-viewer $H/pics/one.png > $P/viewerx.log 2>&1 &
VP=$!
sleep 2.5; xfocus pomelo-viewer; xsnap viewer-x11-open
xdotool key Right; sleep 0.8; xsnap viewer-x11-next
xdotool mousemove 512 330 click 1; xdotool key plus plus; sleep 0.4; xdotool key r; sleep 0.5; xsnap viewer-x11-rot
cat $P/viewerx.log; kill $VP; xstop
