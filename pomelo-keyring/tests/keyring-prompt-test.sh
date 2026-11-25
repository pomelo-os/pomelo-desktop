#!/bin/sh
# pomelo-keyring 密碼提示窗（--daemon 開的）原生 Wayland 測試：私有 D-Bus（abstract socket、沒有 servicedir，不會啟動 gnome-keyring）、
# 全新的空鑰匙圈（XDG_DATA_HOME 在測試資料夾）、密碼是測試用的假密碼。不碰使用者的鑰匙圈。
# 用法：POMELO_TEST_DIR=/home/pake/.p4 sh pomelo-keyring/tests/keyring-prompt-test.sh
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
key() { wtype -k Shift_L; sleep 0.3; wtype -d 90 "$@"; sleep 0.5; }   # 先送一個 Shift 讓虛擬鍵盤就緒（wtype 的第一個鍵會掉），再慢慢打字
start; fresh
sed "s/pomelo-nt-test4/pomelo-p4-kr$$/" $TL/../../../libpomelo/tests/wl-apps/bus.conf > $P/bus.conf 2>/dev/null || cat > $P/bus.conf <<EOT
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:abstract=pomelo-p4-kr$$</listen>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
EOT
dbus-daemon --config-file=$P/bus.conf --print-pid --fork > $P/bus.pid
export DBUS_SESSION_BUS_ADDRESS=unix:abstract=pomelo-p4-kr$$
KR=$POMELO_ROOT/pomelo-keyring/pomelo-keyring
env -u DISPLAY $KR --daemon > $P/kr.out 2>&1 & KP=$!
sleep 1
S=org.freedesktop.secrets; O=/org/freedesktop/secrets
unlock() { busctl --user call $S $O org.freedesktop.Secret.Service Unlock ao 1 $O/collection/login | sed 's/.*"\(\/org[^"]*prompt[^"]*\)".*/\1/'; }
prompt() { busctl --user call $S "$1" org.freedesktop.Secret.Prompt Prompt s "" ; }
status() { busctl --user call $S $O org.pomelo.Keyring Status; }
echo "== 狀態（全新）：$(status)"
echo "== 1. 建立鑰匙圈視窗（兩個欄位）"
PR=$(unlock); echo "prompt=$PR"; prompt $PR; sleep 1; snap kr-create
key abc; keyk Tab; key abd; sleep 0.3; snap kr-typed
keyk Return; sleep 0.6; snap kr-mismatch
echo "== 2. 兩次一樣才建立（欄位 2 重打；Tab 回第一欄重打）"
key abc; keyk Return; sleep 0.8; snap kr-after-create
echo "狀態：$(status)"
echo "== 3. 鎖定後再解鎖：解鎖視窗；Esc 取消"
busctl --user call $S $O org.pomelo.Keyring Lock >/dev/null; echo "狀態：$(status)"
PR=$(unlock); prompt $PR; sleep 1; snap kr-unlock; keyk Escape; sleep 0.6; snap kr-cancelled; echo "狀態：$(status)"
echo "== 4. 解鎖視窗：錯誤密碼 → 提示；正確密碼 → 解鎖"
PR=$(unlock); prompt $PR; sleep 1; keyk Shift_L; sleep 0.3; key wrong; keyk Return; sleep 0.6; snap kr-wrong
key abc; keyk Return; sleep 1; echo "狀態：$(status)"; snap kr-unlocked
echo "== 5. 滑鼠：按取消"
busctl --user call $S $O org.pomelo.Keyring Lock >/dev/null; PR=$(unlock); prompt $PR; sleep 1; snap kr-before-click
echo "daemon 的 fd 中連到 X 的數量（應為 0）：$(ls -l /proc/$KP/fd | grep -c X11)"
kill $KP; kill $(cat $P/bus.pid); stop; cat $P/kr.out
