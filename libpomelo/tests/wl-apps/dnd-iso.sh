. "$(dirname "$0")/lib.sh"
start; fresh
export DND_LOG=$P/dnd2.log; : > $DND_LOG
printf 'n\n' > $P/data/nat1.txt
echo "== native src -> X dst"
DND_FILES=$P/data/nat1.txt env -u DISPLAY $P/bin/dnd-test SRC src > $P/dndsrc.log 2>&1 &
DP=$!; sleep 1.2; movewin -300 100
env -u WAYLAND_DISPLAY $P/bin/xdnd-test XDST dst > $P/xdst.log 2>&1 &
XP=$!; sleep 1.5; movewin 300 100; topwin
read -r dx dy dw dh <<EOT
$(topwin)
EOT
echo top $dx $dy $dw $dh
snap iso1
kill $DP $XP; stop
