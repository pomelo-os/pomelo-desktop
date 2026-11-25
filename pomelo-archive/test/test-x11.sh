#!/bin/sh
# pomelo-archive X11 後端對照（POMELO_BACKEND=x11，Xvfb :97）：開窗、雙擊、右鍵選單
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
PA=${PA:-$POMELO_ROOT/pomelo-archive/pomelo-archive}
stop; fresh; xstart; mkdir -p $P/arch
bash "$(dirname "$0")/make-data.sh" $P/arch/data >/dev/null
rm -f $P/arch/t.zip; (cd $P/arch/data && zip -qr ../t.zip .)
$PA $P/arch/t.zip > $P/archx.log 2>&1 & XP=$!; sleep 2; xsnap arch-x11-open
xdotool mousemove 300 187 click --repeat 2 --delay 90 1; sleep 0.6; xsnap arch-x11-dir
xdotool mousemove 300 215 click 3; sleep 0.5; xsnap arch-x11-menu
kill -0 $XP && echo "OK 程式還活著"; kill $XP; cat $P/archx.log; xstop
