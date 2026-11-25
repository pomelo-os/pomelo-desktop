#!/bin/sh
# pomelo-vm 畫面視窗（--console）原生 Wayland 端到端測試：fake-qemu.py 當 QEMU（VNC＋QMP），不碰使用者的虛擬機器
# 前置：sh libpomelo/tests/wl-apps/setup.sh；POMELO_TEST_DIR 要很短
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
L=$P/vmlog.txt; rm -f $L
start; fresh
export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent
D=$P/data/pomelo/vm/t1; rm -rf $RUN/pomelo-vm; mkdir -p $D $RUN/pomelo-vm/t1
printf 'name = 測試機\narch = x86_64\nos = linux\nmemory = 1024\ncpus = 2\ndisk = disk.qcow2\ndisplay = virtio\ncreated = 1\n' > $D/vm.conf
RD=$RUN/pomelo-vm/t1
python3 -I $POMELO_ROOT/pomelo-vm/tests/fake-qemu.py $RD/ctl.sock $RD/qmp.sock $RD/vnc.sock $L 800 600 "hello-from-guest" > $P/fakeqemu.out 2>&1 &
QP=$!
for i in $(seq 30); do [ -S $RD/vnc.sock ] && break; sleep 0.1; done
WAYLAND_DEBUG=${WLDBG:-0} env -u DISPLAY POMELO_VM_DEBUG=1 $POMELO_ROOT/pomelo-vm/pomelo-vm --console t1 > $P/vmc.log 2>&1 &
FP=$!
sleep 3; snap vm-wl-1-open
mark() { echo "== $1" >> $L; }
VK() { $P/bin/vkbd "$@"; sleep 0.4; }
# 游標：游標在視窗上時是虛擬機器送來的黃圈十字
vp abs 600 280 wait 150 abs 640 300 wait 150 rel 3 3 wait 300; sleep 0.6; snap vm-wl-2-cursor
# 鍵盤（QEMU 延伸按鍵，直接送 PC 掃描碼）：A(30)→0x1e、右方向鍵(106)→0xcd、Print(99)→0xb7、Pause(119)→0xc6、左 Win(125)→0xdb
mark keys; VK 30 30 106 99 119 125
mark ctrl-alt-end; VK d29 d56 107 u56 u29
mark mouse; click 640 300 0; click 640 300 1; vp abs 640 300 wait 100 scroll -1 wait 200 scroll 1 wait 200; sleep 0.5
snap vm-wl-3-after-input
# 工具列選單：檢視（右邊數來第一個）
click 770 67 0; sleep 0.8; snap vm-wl-4-view-menu; keyk Escape; sleep 0.5
# 視窗模式 Alt+Tab 是 compositor 的；全螢幕（Ctrl+Alt+Enter）後交給虛擬機器
mark alttab-windowed; VK d56 15 u56; sleep 0.5
mark fullscreen; VK d29 d56 28 u56 u29; sleep 2; snap vm-wl-5-fullscreen
mark alttab-fullscreen; VK d56 15 u56; sleep 0.8
vp abs 640 0 wait 100 rel 0 -3 wait 800; sleep 1; snap vm-wl-6-fs-toolbar     # 滑鼠頂住上緣：浮動工具列
mark exit-fullscreen; VK d29 d56 28 u56 u29; sleep 1.5; snap vm-wl-7-windowed-again
# 剪貼簿：虛擬機器宣告了 hello-from-guest（ServerCutText）→ 本機剪貼簿
mark clip-from-guest; click 640 300 0; sleep 1; $P/bin/clipget > $P/vclip1.txt 2>&1; echo "虛擬機器→本機剪貼簿：$(grep text= $P/vclip1.txt)"
# 本機→虛擬機器：clipget 設好文字、點虛擬機器視窗標題列回到它（PMW_CLIP → ClientCutText）
mark clip-to-guest; ( $P/bin/clipget --set "local-text-to-vm" > $P/vclip2.txt 2>&1 & ); sleep 1.5; keyk a; sleep 0.8
click 400 40 0; sleep 2
echo "本機→虛擬機器：$(grep -a 'CUT' $L | tr '\n' ' ')"
snap vm-wl-8-clip
