#!/bin/sh
# pomelo-vm 畫面視窗 X11 後端對照（POMELO_BACKEND=x11，Xvfb :97，沒有視窗管理員）：fake-qemu.py 當 QEMU
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
L=$P/vmlogx.txt; rm -f $L
fresh; xstart
export XDG_RUNTIME_DIR=$RUN DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
D=$P/data/pomelo/vm/t1; rm -rf $RUN/pomelo-vm; mkdir -p $D $RUN/pomelo-vm/t1
printf 'name = 測試機\narch = x86_64\nos = linux\nmemory = 1024\ncpus = 2\ndisk = disk.qcow2\ndisplay = virtio\ncreated = 1\n' > $D/vm.conf
RD=$RUN/pomelo-vm/t1
python3 -I $POMELO_ROOT/pomelo-vm/tests/fake-qemu.py $RD/ctl.sock $RD/qmp.sock $RD/vnc.sock $L 800 600 "hello-from-guest" > $P/fakeqemux.out 2>&1 &
QP=$!
for i in $(seq 30); do [ -S $RD/vnc.sock ] && break; sleep 0.1; done
$POMELO_ROOT/pomelo-vm/pomelo-vm --console t1 > $P/vmcx.log 2>&1 &
FP=$!
sleep 3; xsnap vm-x11-1-open
xdotool mousemove 400 300; sleep 0.5; xdotool click 1; xdotool key a; xdotool key Right; sleep 0.8; xsnap vm-x11-2-input
echo "X11 事件記錄："; grep -a "^KEY\|^PTR\|ENC" $L | tr '\n' ' '; echo
kill $FP $QP; sleep 0.5; xstop
