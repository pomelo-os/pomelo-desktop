. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
startp; fresh
H=$P/home; sh "$(dirname "$0")/files-setup.sh" $H
export XDG_DATA_HOME=$H/.local/share
env -u DISPLAY $POMELO_ROOT/pomelo-files/pomelo-files $H > $P/files.log 2>&1 &
FP=$!
sleep 2
click 1005 160   # data? 
vp abs 897 160 down wait 100 rel 20 0 wait 100 abs 700 165 wait 100 abs 573 165 wait 700
snap files-wl-drag-hover
vp abs 573 400 wait 400; snap files-wl-drag-here
vp abs 573 165 wait 100 up wait 1200
ls $H/圖片 | tr '\n' ' '; echo
kill $FP; stop
