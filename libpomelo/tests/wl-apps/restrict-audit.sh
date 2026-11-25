#!/bin/sh
# 找「來源與目的重疊」的 snprintf／strcpy／sprintf：以 -O2 -D_FORTIFY_SOURCE=2 -Wrestrict 編每個 .c（不連結），印出警告
# 在 ~/wm 以外也能執行。缺開發檔的檔案列在 stderr 的「編不過」。
cd "${POMELO_ROOT:-$HOME/wm}"
export LC_ALL=C
INC="-Ilibpomelo $(pkg-config --cflags cairo x11 xext xrandr xft 2>/dev/null)"
for f in $(find . -name '*.c' -not -path '*/third-party/*' -not -path './build/*' -not -path './dist/*' | sort); do
  d=$(dirname $f)
  out=$(nice gcc -O2 -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -Wrestrict -Wformat-overflow=1 -c $INC -I$d -I$d/.. $f -o /dev/null 2>&1)
  echo "$out" | grep -E "restrict|overlap" | grep -E "warning" | sed "s|^|$f: |"
  echo "$out" | grep -qE "fatal error|No such file" && echo "編不過: $f" >&2
done
