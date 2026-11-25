# 檔案管理測試用的假家目錄：files-setup.sh <HOME>
H=$1
rm -rf "$H"; mkdir -p "$H/文件" "$H/圖片" "$H/下載" "$H/空資料夾" "$H/.local/share/Trash/files" "$H/.local/share/Trash/info" "$H/.config/pomelo"
P=${POMELO_TEST_DIR:-/tmp/pomelo-wl-tests}
cp $P/pics/p1.png $P/pics/p2.png $P/pics/two.jpg "$H/圖片/"
printf 'hello\n' > "$H/文件/筆記.txt"; printf 'a\n' > "$H/文件/a.txt"; printf 'b\n' > "$H/文件/b.md"
printf 'x\n' > "$H/下載/setup.sh"; chmod +x "$H/下載/setup.sh"
cp $P/pics/three.pdf "$H/文件/三頁.pdf"
printf 'one\n' > "$H/readme.txt"; printf 'two\n' > "$H/data.csv"; printf '#!/bin/sh\n' > "$H/run.sh"
