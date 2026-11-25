#!/bin/sh
# pomelo-keyring 提示窗的 X11 後端對照（Xvfb :97，POMELO_BACKEND=x11；私有 D-Bus、全新空鑰匙圈、假密碼）
. "$(dirname "$0")/../../libpomelo/tests/wl-apps/lib.sh"
fresh; xstart
cat > $P/bus.conf <<EOT
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:abstract=pomelo-p4-krx$$</listen>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
EOT
dbus-daemon --config-file=$P/bus.conf --print-pid --fork > $P/bus.pid; export DBUS_SESSION_BUS_ADDRESS=unix:abstract=pomelo-p4-krx$$
$POMELO_ROOT/pomelo-keyring/pomelo-keyring --daemon > $P/krx.out 2>&1 & KP=$!; sleep 1
S=org.freedesktop.secrets; O=/org/freedesktop/secrets
PR=$(busctl --user call $S $O org.freedesktop.Secret.Service Unlock ao 1 $O/collection/login | sed 's/.*"\(\/org[^"]*prompt[^"]*\)".*/\1/'); busctl --user call $S "$PR" org.freedesktop.Secret.Prompt Prompt s ""; sleep 1
xdotool type --delay 80 abc; xdotool key Tab; xdotool type --delay 80 abd; xdotool key Return; sleep 0.6; xsnap krx-mismatch
xdotool type --delay 80 abc; xdotool key Return; sleep 1
echo "狀態：$(busctl --user call $S $O org.pomelo.Keyring Status)（bbu true false 0 = 已建立並解鎖）"
kill $KP $(cat $P/bus.pid); xstop
