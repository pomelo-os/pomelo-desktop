#!/bin/bash
# 產生測試資料夾：$1 = 目錄。中文檔名、空檔、深層目錄、可執行檔、符號連結、空格與括號、一個 300 KB 的亂數檔
d=$1; rm -rf "$d"; mkdir -p "$d/dir/sub/deep" "$d/空資料夾"
printf '你好，世界\nhello world\n' > "$d/你好 世界.txt"
: > "$d/empty.txt"
head -c 300000 /dev/urandom > "$d/dir/sub/deep/file.bin"
seq 1 30000 > "$d/dir/numbers.txt"
printf '#!/bin/sh\necho hi\n' > "$d/dir/exec.sh"; chmod 755 "$d/dir/exec.sh"
printf 'secret\n' > "$d/dir/readonly.txt"; chmod 444 "$d/dir/readonly.txt"
ln -s dir/exec.sh "$d/link-to-exec"
ln -s sub "$d/dir/linkdir"
printf 'x\n' > "$d/spaces and (parens) & more.txt"
touch -d '2024-03-05 12:34:56' "$d/你好 世界.txt"
