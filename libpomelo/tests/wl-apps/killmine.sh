#!/bin/sh
# 只結束這個測試環境（私有 XDG_RUNTIME_DIR＝$1）裡的行程，不碰使用者真正的工作階段
RT=${1:?用法：killmine.sh XDG_RUNTIME_DIR}
for p in $(pgrep -u "$(id -u)" .); do
	[ "$p" = "$$" ] && continue
	{ tr "\0" "\n" < /proc/$p/environ; } 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$RT" || continue
	case "$(cat /proc/$p/comm 2>/dev/null)" in bash|sh|killmine.sh) continue;; esac
	kill -9 "$p" 2>/dev/null && echo "killed $p $(cat /proc/$p/comm 2>/dev/null)"
done
true
