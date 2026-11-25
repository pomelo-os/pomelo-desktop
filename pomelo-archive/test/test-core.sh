#!/bin/bash
# pomelo-archive 核心測試：往返、加密、跨工具、編碼、選取與覆蓋、惡意封存檔、大檔
# 用法：PA=/path/to/pomelo-archive bash test-core.sh [區段…]（區段：formats roundtrip crypt foreign charset select security big）
# 需要的工具缺了會「略過」並說明，不會算失敗
cd "$(dirname "$0")" || exit 1
HERE=$PWD
. ./lib.sh
PA=${PA:-pomelo-archive}
export GNUPGHOME=$(mktemp -d); export LC_ALL=C.UTF-8
W=$(mktemp -d /tmp/patest.XXXXXX); trap 'rm -rf "$W" "$GNUPGHOME"' EXIT
cd "$W" || exit 1
SECTIONS=${*:-formats roundtrip crypt foreign rar charset select security big}
want() { [[ " $SECTIONS " == *" $1 "* ]]; }
PW='p@ss w0rd 密碼!'
bash "$HERE/make-data.sh" "$W/data"
head -c 2000000 /dev/urandom > "$W/single.bin"; seq 1 100000 >> "$W/single.bin"

if want formats; then
  echo "== 能力"
  $PA --formats > formats.txt 2>&1 && ok "--formats 能執行" || bad "--formats"
  grep -q "libarchive" formats.txt && ok "--formats 有列出 libarchive 版本" || bad "--formats 內容"
  sed -n 1,14p formats.txt
fi

if want roundtrip; then
  echo "== 往返（建立→列出→測試→解壓→逐位元比對）"
  for f in zip 7z tar tar.gz tar.bz2 tar.xz tar.zst tar.lz tar.lz4 cpio iso squashfs; do
    out=$W/rt.$f; rm -f "$out"; rm -rf x_$f
    $PA --create "$out" --format=$f data >/dev/null 2>cr.err; rc=$?
    if [ $rc = 4 ] || { [ $rc = 1 ] && grep -qE "需要|不能|沒有" cr.err; }; then skip "建立 $f" "$(head -c 120 cr.err)"; continue; fi
    [ $rc = 0 ] || { bad "建立 $f" "rc=$rc $(head -c 160 cr.err)"; continue; }
    n=$($PA --list "$out" 2>/dev/null | wc -l)
    [ "$n" -ge 10 ] && ok "列出 $f（$n 項）" || bad "列出 $f" "只有 $n 項"
    $PA --test "$out" >/dev/null 2>&1 && ok "測試 $f" || bad "測試 $f"
    $PA --extract "$out" -C x_$f >/dev/null 2>ex.err; rc=$?
    [ $rc = 0 ] || { bad "解壓 $f" "rc=$rc $(head -c 160 ex.err)"; continue; }
    case $f in
      iso|squashfs|cpio) diff -r --no-dereference data x_$f/data >/dev/null 2>&1 && ok "內容一致 $f" || { diff -rq --no-dereference data x_$f/data | head -3; bad "內容一致 $f"; } ;;
      *) tree_eq data x_$f/data; r=$?; [ $r = 0 ] && ok "內容、權限、連結一致 $f" || { diff -rq --no-dereference data x_$f/data | head -3; bad "一致 $f" "代碼 $r"; } ;;
    esac
  done
  # 單一檔案
  for f in gz bz2 xz zst lz; do
    rm -f single.$f; rm -rf xs_$f
    $PA --create single.$f --format=$f single.bin >/dev/null 2>cr.err; rc=$?
    if [ $rc != 0 ]; then skip "單檔 $f" "$(head -c 100 cr.err)"; continue; fi
    $PA --extract single.$f -C xs_$f >/dev/null 2>&1 && cmp -s single.bin xs_$f/single && ok "單檔 $f 往返" || bad "單檔 $f 往返"
  done
  # 空資料夾保留、只有一個空檔
  mkdir -p only/empty; : > only/e.txt
  $PA --create only.zip --format=zip only >/dev/null 2>&1 && $PA --extract only.zip -C xo >/dev/null 2>&1 && [ -d xo/only/empty ] && [ -f xo/only/e.txt ] && ok "空資料夾與空檔保留" || bad "空資料夾與空檔保留"
  # 輸出檔不會把自己壓進去
  mkdir self; echo a > self/a; $PA --create self/self.zip --format=zip self >/dev/null 2>&1; [ "$($PA --list self/self.zip 2>/dev/null | grep -c self.zip)" = 0 ] && ok "輸出檔不會壓進自己" || bad "輸出檔壓進自己"
  # 失敗不留半成品
  $PA --create /nonexistent-dir/x.zip --format=zip data >/dev/null 2>&1; [ $? != 0 ] && ok "輸出資料夾不存在會失敗" || bad "輸出資料夾不存在"
  [ -z "$(ls /tmp/*.pomelo-tmp-* 2>/dev/null)" ] && ok "沒有留下暫存檔" || bad "留下暫存檔"
fi

if want crypt; then
  echo "== 加密"
  for f in zip zip-zipcrypto 7z tar.gz tar.zst cpio; do
    out=$W/enc.$f; rm -f "$out" "$out.gpg"; rm -rf xe_$f
    printf '%s\n' "$PW" | $PA --create "$out" --format=$f --password-stdin data >/dev/null 2>cr.err; rc=$?
    if [ $rc != 0 ]; then
      if grep -qE "需要|不能|沒有" cr.err; then skip "加密建立 $f" "$(head -c 120 cr.err)"; else bad "加密建立 $f" "rc=$rc $(head -c 160 cr.err)"; fi
      continue
    fi
    real=$out; [ -f "$out.gpg" ] && real=$out.gpg
    # 沒給密碼：要回「需要密碼」(2)；密碼錯：3
    expect_rc "加密 $f：沒給密碼 → 2" 2 $PA --test "$real"
    printf 'wrong\n' | $PA --test "$real" --password-stdin >/dev/null 2>&1; rc=$?
    [ $rc = 3 ] && ok "加密 $f：密碼錯 → 3" || bad "加密 $f：密碼錯" "rc=$rc"
    printf '%s\n' "$PW" | $PA --test "$real" --password-stdin >/dev/null 2>&1 && ok "加密 $f：密碼對可測試" || bad "加密 $f：密碼對可測試"
    printf '%s\n' "$PW" | $PA --extract "$real" -C xe_$f --password-stdin >/dev/null 2>ex.err; rc=$?
    [ $rc = 0 ] || { bad "加密解壓 $f" "rc=$rc $(head -c 160 ex.err)"; continue; }
    case $f in cpio) diff -r --no-dereference data xe_$f/data >/dev/null 2>&1 && ok "加密 $f 內容一致" || bad "加密 $f 內容一致" ;; *) tree_eq data xe_$f/data && ok "加密 $f 內容與權限一致" || bad "加密 $f 內容一致" ;; esac
    # 密碼不能出現在命令列：建立時 ps 看不到（檢查建立指令本身沒有密碼；這裡只確認建立後的檔案不含明文）
    grep -qaF 'hello world' "$real" && bad "加密 $f：明文出現在檔案裡" || ok "加密 $f：檔案裡沒有明文"
  done
  # 連檔名一起加密的 7z
  rm -f names.7z
  printf '%s\n' "$PW" | $PA --create names.7z --format=7z --encrypt-names --password-stdin data >/dev/null 2>cr.err; rc=$?
  if [ $rc = 0 ]; then
    expect_rc "7z 加密檔名：沒密碼連清單都看不到 → 2" 2 $PA --list names.7z
    printf '%s\n' "$PW" | $PA --list names.7z --password-stdin 2>/dev/null | grep -q "你好 世界.txt" && ok "7z 加密檔名：有密碼看得到清單" || bad "7z 加密檔名：有密碼的清單"
    strings -n 4 names.7z 2>/dev/null | grep -q "deep" && bad "7z 加密檔名：檔名漏在外面" || ok "7z 加密檔名：檔名沒有漏在外面"
  else skip "7z 加密檔名" "$(head -c 100 cr.err)"; fi
  # 沒有加密的格式要求加密：iso、squashfs
  expect_rc "ISO 不能加密" 1 bash -c "printf 'x\n' | $PA --create e.iso --format=iso --password-stdin data"
  # 跨工具驗證：別人的軟體能不能開我們做的加密檔
  if have 7z || have 7za || have 7zz; then
    SZ=$(command -v 7zz || command -v 7z || command -v 7za)
    [ -f enc.zip ] && { printf '%s\n' "$PW" | $PA --create ex_aes.zip --format=zip --password-stdin data >/dev/null 2>&1; "$SZ" t "-p$PW" ex_aes.zip >/dev/null 2>&1 && ok "跨工具：7-Zip 能開我們的 AES-256 ZIP" || bad "跨工具：7-Zip 開 AES ZIP"; }
    [ -f enc.7z ] && { "$SZ" t "-p$PW" enc.7z >/dev/null 2>&1 && ok "跨工具：7-Zip 能開我們的加密 7z" || bad "跨工具：7-Zip 開加密 7z"; }
  else skip "跨工具 7-Zip" "沒有 7z"; fi
  if have unzip && [ -f enc.zip-zipcrypto ]; then unzip -tqq -P "$PW" enc.zip-zipcrypto >/dev/null 2>&1 && ok "跨工具：unzip 能開舊式加密 ZIP" || bad "跨工具：unzip 開舊式加密 ZIP"; fi
  if have gpg && [ -f enc.tar.gz.gpg ]; then printf '%s\n' "$PW" | gpg --batch --pinentry-mode loopback --passphrase-fd 0 -d enc.tar.gz.gpg 2>/dev/null | tar tz >/dev/null 2>&1 && ok "跨工具：gpg 能解我們的 tar.gz.gpg" || bad "跨工具：gpg 解 tar.gz.gpg"; fi
  # 加入檔案到既有封存檔
  echo "new" > added.txt
  for f in zip tar.gz 7z; do
    rm -f add.$f; rm -rf xa_$f
    $PA --create add.$f --format=$f data >/dev/null 2>&1 || continue
    $PA --add add.$f added.txt >/dev/null 2>add.err; rc=$?
    if [ $rc != 0 ]; then bad "加入檔案 $f" "rc=$rc $(head -c 120 add.err)"; continue; fi
    $PA --extract add.$f -C xa_$f >/dev/null 2>&1 && [ -f xa_$f/added.txt ] && tree_eq data xa_$f/data && ok "加入檔案 $f（原有內容不變）" || bad "加入檔案 $f"
  done
  printf '%s\n' "$PW" | $PA --create adde.zip --format=zip --password-stdin data >/dev/null 2>&1
  printf '%s\n' "$PW" | $PA --add adde.zip added.txt --password-stdin >/dev/null 2>add.err; rc=$?
  if [ $rc = 0 ]; then printf '%s\n' "$PW" | $PA --extract adde.zip -C xae --password-stdin >/dev/null 2>&1 && [ -f xae/added.txt ] && tree_eq data xae/data && ok "加入檔案到加密 ZIP（仍然加密）" || bad "加入檔案到加密 ZIP"; else bad "加入檔案到加密 ZIP" "$(head -c 100 add.err)"; fi
fi

if want foreign; then
  echo "== 讀別的工具做的檔案"
  SZ=$(command -v 7zz || command -v 7z || command -v 7za)
  rd() {   # rd 名稱 檔案 [比對模式 full|names]：解壓並跟原資料比
    local n=$1 f=$2 mode=${3:-full} d=xf_$1; rm -rf "$d"
    $PA --extract "$f" -C "$d" >/dev/null 2>ex.err; local rc=$?
    [ $rc = 0 ] || { bad "讀 $n" "rc=$rc $(head -c 140 ex.err)"; return; }
    local top=$d/data; [ -d "$top" ] || top=$d
    if [ $mode = names ]; then [ "$(cd data && find . -type f | sort)" = "$(cd "$top" && find . -type f | sort)" ] && ok "讀 $n（檔案清單一致）" || bad "讀 $n" "檔案清單不一致"
    else tree_eq data "$top"; local r=$?; [ $r = 0 ] && ok "讀 $n" || { diff -rq --no-dereference data "$top" | head -3; bad "讀 $n" "代碼 $r"; }; fi
  }
  have zip && { rm -f f.zip; zip -qry f.zip data && rd "Info-ZIP zip" f.zip; } || skip "Info-ZIP zip" "沒有 zip"
  have tar && { tar czf f.tar.gz data && rd "tar.gz（GNU tar）" f.tar.gz; tar cjf f.tar.bz2 data && rd "tar.bz2" f.tar.bz2; tar cJf f.tar.xz data && rd "tar.xz" f.tar.xz; tar cf f.tar data && rd "tar" f.tar; tar czf f.tgz data && rd "tgz" f.tgz; }
  have zstd && { tar --zstd -cf f.tar.zst data 2>/dev/null && rd "tar.zst" f.tar.zst; } || skip "tar.zst" "沒有 zstd"
  have lzip && { tar --lzip -cf f.tar.lz data 2>/dev/null && rd "tar.lz" f.tar.lz; } || skip "tar.lz" "沒有 lzip"
  have cpio && { (cd "$W" && find data | cpio -o -H newc --quiet > f.cpio 2>/dev/null) && rd "cpio（newc）" f.cpio; (find data | cpio -o -H crc --quiet > f2.cpio 2>/dev/null) && rd "cpio（crc）" f2.cpio; }
  if [ -n "$SZ" ]; then
    "$SZ" a -t7z -snl f2.7z data >/dev/null 2>&1 && rd "7-Zip 的 7z" f2.7z
    "$SZ" a -tzip -snl f3.zip data >/dev/null 2>&1 && rd "7-Zip 的 zip" f3.zip
    "$SZ" a -tzip -snl -mem=AES256 "-p$PW" f4.zip data >/dev/null 2>&1 && { rm -rf xf_aes; printf '%s\n' "$PW" | $PA --extract f4.zip -C xf_aes --password-stdin >/dev/null 2>&1 && tree_eq data xf_aes/data && ok "讀 7-Zip 做的 AES ZIP" || bad "讀 7-Zip 做的 AES ZIP"; }
    "$SZ" a -t7z -snl "-p$PW" -mhe=on f5.7z data >/dev/null 2>&1 && { rm -rf xf_h7; printf '%s\n' "$PW" | $PA --extract f5.7z -C xf_h7 --password-stdin >/dev/null 2>&1 && tree_eq data xf_h7/data && ok "讀 7-Zip 做的檔名加密 7z" || bad "讀 7-Zip 做的檔名加密 7z"; }
  else skip "7-Zip 的檔案" "沒有 7z"; fi
  if have zip; then rm -f f6.zip; zip -qry -P "$PW" f6.zip data 2>/dev/null; rm -rf xf_zc; printf '%s\n' "$PW" | $PA --extract f6.zip -C xf_zc --password-stdin >/dev/null 2>&1 && diff -r -x link-to-exec -x linkdir data xf_zc/data >/dev/null 2>&1 && ok "讀舊式加密 ZIP（Info-ZIP -P；加密 ZIP 裡的符號連結 libarchive 讀不出目標，不還原）" || bad "讀舊式加密 ZIP"; fi
  have mksquashfs && { rm -f f.squashfs; mksquashfs data f.squashfs -quiet -noappend >/dev/null 2>&1; if [ -n "$SZ" ]; then rm -rf xf_sq; $PA --extract f.squashfs -C xf_sq >/dev/null 2>&1 && diff -r --no-dereference data xf_sq >/dev/null 2>&1 && ok "讀 SquashFS（mksquashfs）" || bad "讀 SquashFS"; else expect_rc "沒有 7-Zip 時讀 SquashFS 要說缺什麼 → 4" 4 $PA --list f.squashfs; fi; } || skip "SquashFS" "沒有 mksquashfs"
  ISOTOOL=$(command -v genisoimage || command -v mkisofs || command -v xorrisofs || command -v xorriso)
  if [ -n "$ISOTOOL" ]; then rm -f f.iso; case $ISOTOOL in *xorriso) xorriso -as mkisofs -R -J -o f.iso data >/dev/null 2>&1 ;; *) $ISOTOOL -R -J -o f.iso data >/dev/null 2>&1 ;; esac; rm -rf xf_iso; $PA --extract f.iso -C xf_iso >/dev/null 2>&1 && [ -f "xf_iso/你好 世界.txt" ] && ok "讀 ISO（Joliet／Rock Ridge）" || bad "讀 ISO"; else skip "ISO" "沒有 genisoimage／xorriso"; fi
  if have mkfs.vfat && have mcopy && [ -n "$SZ" ]; then
    rm -f f.img; truncate -s 16M f.img; mkfs.vfat -n TEST f.img >/dev/null 2>&1; MTOOLS_SKIP_CHECK=1 mcopy -i f.img -s data ::/ 2>/dev/null
    rm -rf xf_img; $PA --extract f.img -C xf_img >/dev/null 2>ex.err && [ -f xf_img/data/dir/numbers.txt ] && cmp -s data/dir/numbers.txt xf_img/data/dir/numbers.txt && ok "讀 FAT 磁碟映像（.img）" || bad "讀 FAT 磁碟映像" "$(head -c 100 ex.err)"
  else skip "FAT 映像" "缺 mkfs.vfat／mcopy／7z"; fi
  if have mke2fs && [ -n "$SZ" ]; then rm -f f2.img; truncate -s 16M f2.img; mke2fs -q -t ext4 -d data f2.img >/dev/null 2>&1; rm -rf xf_ext; $PA --extract f2.img -C xf_ext >/dev/null 2>&1 && cmp -s data/dir/numbers.txt xf_ext/dir/numbers.txt && ok "讀 ext4 磁碟映像" || bad "讀 ext4 磁碟映像"; else skip "ext 映像" "缺 mke2fs／7z"; fi
  for c in gzip:gz bzip2:bz2 xz:xz zstd:zst lzip:lz; do t=${c%%:*}; e=${c##*:}; if have $t; then rm -f sg.$e sg; cp single.bin sg; $t -k -q sg 2>/dev/null; [ -f sg.$e ] && { rm -rf xg_$e; $PA --extract sg.$e -C xg_$e >/dev/null 2>&1 && cmp -s single.bin xg_$e/sg && ok "讀單檔 .$e（$t）" || bad "讀單檔 .$e"; }; else skip "單檔 .$e" "沒有 $t"; fi; done
  have ar && { rm -f f.a; ar rc f.a data/empty.txt "data/你好 世界.txt" 2>/dev/null; $PA --list f.a 2>/dev/null | grep -q "你好" && ok "讀 ar" || bad "讀 ar"; }
fi

if want rar; then
  echo "== RAR 一律不支援（授權：unRAR 限制條款；不呼叫任何 rar／unrar，也不交給 libarchive／7-Zip）"
  # 假的 RAR：只有魔數（RAR4／RAR5），不需要 rar 程式
  printf 'Rar!\x1a\x07\x00\xcf\x90\x73\x00\x00\x0d\x00\x00\x00\x00\x00\x00\x00' > fake4.rar
  printf 'Rar!\x1a\x07\x01\x00\x33\x92\xb5\xe5\x0a\x01\x05\x06\x00\x05\x01\x01\x00' > fake5.rar
  cp fake4.rar noext.bin; cp "$HERE/../README.md" notrar.rar      # 副檔名是 .rar 但內容不是；魔數是 RAR 但副檔名不是
  for f in fake4.rar fake5.rar noext.bin notrar.rar; do
    for m in --list --test --extract; do
      out=$($PA $m $f 2>&1 >/dev/null); rc=$?
      [ $rc = 1 ] && echo "$out" | grep -q "不支援" && ok "RAR：$m $f → 不支援（1）" || bad "RAR：$m $f" "rc=$rc $out"
    done
  done
  $PA --formats 2>&1 | grep -qi "rar" && bad "--formats 不該提到 rar" || ok "--formats 沒有提到 rar"
  # 7-Zip 或 libarchive 能讀真的 RAR 也不放行（有 rar 時才做）
  if have rar; then rm -f real.rar; rar a -ma5 real.rar data >/dev/null 2>&1; expect_rc "真的 RAR5 檔：拒絕" 1 $PA --list real.rar; fi
  # 不能執行任何 rar／unrar：把假的 rar、unrar 放進 PATH，看有沒有被呼叫
  mkdir -p fakebin; for x in rar unrar unrar-nonfree; do printf '#!/bin/sh\ntouch %s/rar-called\n' "$W" > fakebin/$x; chmod +x fakebin/$x; done
  PATH=$W/fakebin:$PATH $PA --list fake4.rar >/dev/null 2>&1; PATH=$W/fakebin:$PATH $PA --extract fake5.rar -C xrf >/dev/null 2>&1
  [ ! -e "$W/rar-called" ] && ok "RAR：沒有呼叫 rar／unrar 可執行檔" || bad "RAR：呼叫了 rar／unrar"
fi

if want charset; then
  echo "== 檔名編碼（Windows 做的 zip／tar 常是 Big5、GBK、Shift-JIS）"
  mk() {   # mk 編碼 檔名(UTF-8) → 在 C locale 下建立那個編碼的檔名，打成 zip 與 tar
    local cs=$1 name=$2 raw; raw=$(printf '%s' "$name" | iconv -f UTF-8 -t "$cs")
    rm -rf cs_$cs; mkdir cs_$cs; (cd cs_$cs && LC_ALL=C sh -c 'echo data > "$1"' _ "$raw" && LC_ALL=C zip -q ../cs_$cs.zip "$raw" && LC_ALL=C tar cf ../cs_$cs.tar "$raw")
  }
  if have iconv && have zip; then
    mk BIG5 "測試文件報告.txt";  $PA --list cs_BIG5.zip 2>/dev/null | grep -q "測試文件報告.txt" && ok "自動偵測 Big5（zip）" || bad "自動偵測 Big5（zip）" "$($PA --list cs_BIG5.zip 2>&1 | tail -1)"
    $PA --list cs_BIG5.tar 2>/dev/null | grep -q "測試文件報告.txt" && ok "自動偵測 Big5（tar）" || bad "自動偵測 Big5（tar）"
    mk GBK "测试文件图片.txt";   $PA --list cs_GBK.zip 2>/dev/null | grep -q "测试文件图片.txt" && ok "自動偵測 GBK（簡體，zip）" || bad "自動偵測 GBK（簡體，zip）" "$($PA --list cs_GBK.zip 2>&1 | tail -1)"
    mk SHIFT_JIS "テストファイル日本語.txt"; $PA --list cs_SHIFT_JIS.zip 2>/dev/null | grep -q "テストファイル日本語.txt" && ok "自動偵測 Shift-JIS" || bad "自動偵測 Shift-JIS" "$($PA --list cs_SHIFT_JIS.zip 2>&1 | tail -1)"
    mk EUC-KR "한글파일문서.txt";  $PA --list cs_EUC-KR.zip 2>/dev/null | grep -q "한글파일문서.txt" && ok "自動偵測 EUC-KR" || bad "自動偵測 EUC-KR" "$($PA --list cs_EUC-KR.zip 2>&1 | tail -1)"
    # 手動指定覆蓋偵測
    $PA --list cs_BIG5.zip --charset BIG5 2>/dev/null | grep -q "測試文件報告.txt" && ok "手動指定 --charset BIG5" || bad "手動指定編碼"
    # 解壓出來的檔名是對的
    rm -rf xcs; $PA --extract cs_BIG5.zip -C xcs >/dev/null 2>&1; [ -f "xcs/測試文件報告.txt" ] && ok "解壓出來的檔名是正確的中文" || bad "解壓出來的檔名" "$(ls xcs)"
    # 本來就是 UTF-8 的不能被亂改
    $PA --list "$W/f.zip" 2>/dev/null | grep -q "你好 世界.txt" && ok "UTF-8 檔名不受影響" || skip "UTF-8 檔名" "沒有 f.zip（先跑 foreign）"
    $PA --list cs_BIG5.zip 2>&1 >/dev/null | grep -q "自動偵測" && ok "--list 標示編碼是自動偵測的" || bad "--list 沒標示自動偵測"
  else skip "編碼" "缺 iconv 或 zip"; fi
fi

if want select; then
  echo "== 選取、覆蓋、解壓位置"
  $PA --create sel.zip --format=zip data >/dev/null 2>&1
  rm -rf xs1; $PA --extract sel.zip -C xs1 --only data/dir/sub >/dev/null 2>&1; [ -f xs1/data/dir/sub/deep/file.bin ] && [ ! -e xs1/data/empty.txt ] && ok "只解選取的資料夾" || bad "只解選取的資料夾"
  rm -rf xs2; $PA --extract sel.zip -C xs2 --only "data/你好 世界.txt" --only data/empty.txt >/dev/null 2>&1; [ -f "xs2/data/你好 世界.txt" ] && [ -f xs2/data/empty.txt ] && [ ! -e xs2/data/dir ] && ok "只解選取的兩個檔案" || bad "只解選取的兩個檔案"
  rm -rf xo1; mkdir -p xo1/data; echo old > xo1/data/empty.txt; echo keep > xo1/data/only-here.txt
  $PA --extract sel.zip -C xo1 >/dev/null 2>ex.err; rc=$?; [ $rc = 9 ] && ok "同名項目 → 回報衝突（9），先不動手" || bad "衝突偵測" "rc=$rc"
  [ "$(cat xo1/data/only-here.txt)" = keep ] && ok "衝突時沒有動到現有檔案" || bad "衝突時動到現有檔案"
  $PA --extract sel.zip -C xo1 --overwrite=no >/dev/null 2>&1; [ -s xo1/data/numbers.txt ] || [ "$(cat xo1/data/empty.txt)" = old ] && ok "覆蓋＝略過：現有的 data 沒被動" || bad "略過"
  rm -rf xo2; mkdir -p xo2/data; echo old > xo2/data/empty.txt; echo keep > xo2/data/only-here.txt
  $PA --extract sel.zip -C xo2 --overwrite=yes >/dev/null 2>&1; [ ! -s xo2/data/empty.txt ] && [ "$(cat xo2/data/only-here.txt)" = keep ] && [ -f xo2/data/dir/numbers.txt ] && ok "覆蓋＝取代：合併資料夾、取代同名檔案、保留其他檔案" || bad "取代"
  rm -rf xo3; mkdir -p xo3/data; echo old > xo3/data/empty.txt
  $PA --extract sel.zip -C xo3 --overwrite=rename >/dev/null 2>&1; [ -d "xo3/data (2)" ] && [ "$(cat xo3/data/empty.txt)" = old ] && ok "覆蓋＝另存新名：data (2)，原本的不動" || bad "另存新名" "$(ls xo3)"
  # --into-folder：頂層只有一項就直接放；多項就建以檔名命名的資料夾
  rm -rf xi1; cp sel.zip multi.zip; rm -rf mt; mkdir mt; echo 1 > mt/a.txt; echo 2 > mt/b.txt; (cd mt && $PA --create ../multi.zip --format=zip a.txt b.txt >/dev/null 2>&1)
  rm -rf xi2; $PA --extract multi.zip -C xi2 --into-folder >/dev/null 2>&1; [ -f xi2/multi/a.txt ] && ok "--into-folder：多個頂層項目 → 建『multi』資料夾" || bad "--into-folder 多項" "$(ls xi2)"
  rm -rf xi3; $PA --extract sel.zip -C xi3 --into-folder >/dev/null 2>&1; [ -d xi3/data ] && [ ! -d xi3/sel ] && ok "--into-folder：單一頂層資料夾 → 直接用，不多包一層" || bad "--into-folder 單項" "$(ls xi3)"
  # 目的地不存在會建立
  rm -rf deep_dest; $PA --extract sel.zip -C deep_dest/a/b >/dev/null 2>&1 && [ -f deep_dest/a/b/data/empty.txt ] && ok "目的資料夾不存在會自動建立" || bad "建立目的資料夾"
  [ -z "$(ls -A xo1 | grep pomelo-extract)" ] && ok "沒有留下暫存資料夾" || bad "留下暫存資料夾"
fi

if want security; then
  echo "== 惡意封存檔"
  mkdir -p sec && cd sec || exit 1
  VICTIM=$W/victim; rm -rf "$VICTIM"; mkdir -p "$VICTIM"
  python3 - "$VICTIM" <<'PY'
import sys, zipfile, tarfile, io, os
victim = sys.argv[1]
# 1) zip-slip：.. 與絕對路徑
with zipfile.ZipFile('slip.zip', 'w') as z:
    z.writestr('ok/fine.txt', 'fine')
    z.writestr('../../escape1.txt', 'x')
    z.writestr('a/../../escape2.txt', 'x')
    z.writestr('/tmp/pomelo-escape3.txt', 'x')
    z.writestr('....//escape4.txt', 'x')
# 2) tar：符號連結指到外面，再透過它寫檔
with tarfile.open('linkslip.tar', 'w') as t:
    d = tarfile.TarInfo('evil'); d.type = tarfile.SYMTYPE; d.linkname = victim; t.addfile(d)
    f = tarfile.TarInfo('evil/pwn.txt'); data = b'pwned'; f.size = len(data); t.addfile(f, io.BytesIO(data))
# 3) tar：絕對符號連結、跳出去的相對連結（單獨存在，不透過它寫）
with tarfile.open('abslink.tar', 'w') as t:
    for n, tg in (('abs', '/etc/passwd'), ('up', '../../../../etc/shadow'), ('okrel', 'sub/x'), ('ok2', 'a/../b')):
        d = tarfile.TarInfo(n); d.type = tarfile.SYMTYPE; d.linkname = tg; t.addfile(d)
    s = tarfile.TarInfo('sub/x'); s.size = 1; t.addfile(s, io.BytesIO(b'x'))
# 4) tar：setuid、特殊檔、硬連結指到外面
with tarfile.open('special.tar', 'w') as t:
    f = tarfile.TarInfo('suid'); f.size = 3; f.mode = 0o4755; t.addfile(f, io.BytesIO(b'abc'))
    g = tarfile.TarInfo('sgid'); g.size = 3; g.mode = 0o2755; t.addfile(g, io.BytesIO(b'abc'))
    st = tarfile.TarInfo('sticky'); st.type = tarfile.DIRTYPE; st.mode = 0o1777; t.addfile(st)
    dev = tarfile.TarInfo('dev-null'); dev.type = tarfile.CHRTYPE; dev.devmajor = 1; dev.devminor = 3; t.addfile(dev)
    fifo = tarfile.TarInfo('fifo'); fifo.type = tarfile.FIFOTYPE; t.addfile(fifo)
    h = tarfile.TarInfo('hard'); h.type = tarfile.LNKTYPE; h.linkname = '/etc/passwd'; t.addfile(h)
    h2 = tarfile.TarInfo('hard2'); h2.type = tarfile.LNKTYPE; h2.linkname = '../../etc/passwd'; t.addfile(h2)
# 5) 壓縮炸彈：6 GB 的零（deflate 約 1000:1）
with zipfile.ZipFile('bomb.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    with z.open('zeros.bin', 'w', force_zip64=True) as w:
        chunk = b'\0' * (1 << 20)
        for _ in range(6 * 1024): w.write(chunk)
# 6) 很多小檔（2 萬個）
with tarfile.open('many.tar', 'w') as t:
    for i in range(20000):
        f = tarfile.TarInfo('d%d/f%d.txt' % (i % 100, i)); data = b'x%d\n' % i; f.size = len(data); t.addfile(f, io.BytesIO(data))
# 7) 路徑：很長、含換行與控制字元的檔名、空檔名
with tarfile.open('names.tar', 'w') as t:
    for n in ('weird\nname.txt', 'x' * 200 + '.txt', 'ctrl\x07bell.txt'):
        f = tarfile.TarInfo(n); f.size = 1; t.addfile(f, io.BytesIO(b'x'))
PY
  ESC=0
  # 1) zip-slip
  rm -rf out1; $PA --extract slip.zip -C out1/dest >/dev/null 2>sec.err; rc=$?
  [ ! -e out1/escape1.txt ] && [ ! -e out1/escape2.txt ] && [ ! -e /tmp/pomelo-escape3.txt ] && [ ! -e out1/dest/escape4.txt -o -f out1/dest/escape4.txt ] && ok "zip-slip：.. 與絕對路徑沒有逃出去" || { bad "zip-slip 逃出去了"; ESC=1; }
  [ -f out1/dest/ok/fine.txt ] && ok "zip-slip：正常的項目照常解出來" || bad "zip-slip：正常項目"
  find out1 /tmp -maxdepth 2 \( -name 'escape*' -o -name 'pomelo-escape*' \) 2>/dev/null | grep -q . && bad "zip-slip：外面出現了檔案" || ok "zip-slip：外面沒有任何新檔案"
  grep -q "略過" sec.err && ok "zip-slip：有告訴使用者略過了不安全的項目" || bad "zip-slip：沒有告知略過"
  # 2) 透過符號連結寫到外面
  rm -rf out2; $PA --extract linkslip.tar -C out2 >/dev/null 2>sec.err; rc=$?
  [ ! -e "$VICTIM/pwn.txt" ] && ok "符號連結逃逸：沒有寫進外面的資料夾" || { bad "符號連結逃逸：寫出去了"; ESC=1; }
  [ $rc = 6 ] && ok "符號連結逃逸：整包擋下（結束碼 6）" || bad "符號連結逃逸：結束碼" "rc=$rc"
  # 3) 絕對或往外的連結不留下，正常的相對連結保留
  rm -rf out3; $PA --extract abslink.tar -C out3 >/dev/null 2>sec.err
  [ ! -L out3/abs ] && [ ! -L out3/up ] && ok "絕對連結與往外的連結不留下" || bad "絕對／往外的連結被留下" "$(ls -la out3)"
  [ -L out3/okrel ] && ok "相對且在目錄內的連結保留" || bad "正常相對連結被刪"
  [ ! -L out3/ok2 ] && ok "a/../b（.. 夾在中間）的連結不留下：文字運算算不出實際走到哪" || bad "a/../b 連結被留下"
  # 4) setuid、特殊檔、硬連結
  rm -rf out4; $PA --extract special.tar -C out4 >/dev/null 2>sec.err
  [ "$(stat -c %a out4/suid 2>/dev/null)" = 644 ] || [ "$(stat -c %a out4/suid 2>/dev/null)" = 755 ] && ok "setuid 位元被拿掉（$(stat -c %a out4/suid)）" || bad "setuid 沒拿掉" "$(stat -c %a out4/suid 2>&1)"
  [ "$(stat -c %a out4/sgid 2>/dev/null | cut -c1)" != 2 ] && [ "$(stat -c %A out4/sgid | cut -c4-6)" != "sss" ] && ok "setgid 位元被拿掉" || bad "setgid 沒拿掉"
  [ -d out4/sticky ] && [ "$(stat -c %a out4/sticky)" != 1777 ] && ok "sticky 位元被拿掉" || bad "sticky 沒拿掉" "$(stat -c %a out4/sticky 2>&1)"
  [ ! -e out4/dev-null ] && [ ! -e out4/fifo ] && ok "裝置檔與管線被略過" || bad "裝置檔或管線被建立"
  [ ! -e out4/hard ] && [ ! -e out4/hard2 ] && ok "指到外面的硬連結被略過" || bad "指到外面的硬連結被建立"
  # 5) 壓縮炸彈
  $PA --create /dev/null >/dev/null 2>&1
  rm -rf out5; $PA --extract bomb.zip -C out5 >/dev/null 2>sec.err; rc=$?
  [ $rc = 8 ] && ok "壓縮炸彈：先警告，不動手（結束碼 8）" || bad "壓縮炸彈" "rc=$rc $(head -c 150 sec.err)"
  [ ! -e out5/zeros.bin ] && [ -z "$(ls -A out5 2>/dev/null)" ] && ok "壓縮炸彈：沒有寫出任何東西" || bad "壓縮炸彈：寫出了東西"
  rm -f bomb.zip
  # 5b) 空間不夠：用很小的 tmpfs 試（容器 --tmpfs /small:size=8m）
  if [ -d /small ] && mountpoint -q /small 2>/dev/null; then
    head -c 20000000 /dev/zero > /tmp/z20.bin; (cd /tmp && $PA --create z20.zip --format=zip z20.bin >/dev/null 2>&1)
    $PA --extract /tmp/z20.zip -C /small/d >/dev/null 2>sec.err; rc=$?
    [ $rc = 7 ] && ok "磁碟空間不夠：先告知（結束碼 7）" || bad "磁碟空間不夠" "rc=$rc"
    [ -z "$(ls -A /small/d 2>/dev/null)" ] && ok "磁碟空間不夠：沒有留下半成品" || bad "空間不夠留下半成品"
    rm -f /tmp/z20.bin /tmp/z20.zip
  else skip "磁碟空間不夠" "沒有 /small 這個小的 tmpfs（docker run --tmpfs /small:size=8m）"; fi
  # 6) 2 萬個檔案
  s=$(date +%s.%N); n=$($PA --list many.tar 2>/dev/null | wc -l); e=$(date +%s.%N)
  [ "$n" = 20000 ] && ok "列出 2 萬個項目（$(printf '%.2f' "$(echo "$e - $s" | bc 2>/dev/null || echo 0)") 秒）" || bad "列出 2 萬個項目" "只有 $n"
  rm -rf out6; s=$(date +%s.%N); $PA --extract many.tar -C out6 >/dev/null 2>&1; e=$(date +%s.%N); [ "$(find out6 -type f | wc -l)" = 20000 ] && ok "解壓 2 萬個檔案" || bad "解壓 2 萬個檔案"
  # 7) 奇怪的檔名
  rm -rf out7; $PA --extract names.tar -C out7 >/dev/null 2>&1; rc=$?; [ $rc = 0 ] && ok "含換行、控制字元、很長的檔名可以解壓" || bad "奇怪的檔名" "rc=$rc"
  # 8) 損毀的封存檔不能當掉
  head -c 3000 "$W/f.zip" > trunc.zip 2>/dev/null || head -c 3000 "$W/rt.zip" > trunc.zip
  $PA --list trunc.zip >/dev/null 2>&1; rc=$?; [ $rc -lt 128 ] && ok "截斷的 zip 不會當掉（結束碼 $rc）" || bad "截斷的 zip 當掉" "rc=$rc"
  head -c 5000 /dev/urandom > garbage.zip; $PA --list garbage.zip >/dev/null 2>&1; rc=$?; [ $rc -lt 128 ] && [ $rc != 0 ] && ok "亂碼檔案：乾淨地失敗（結束碼 $rc）" || bad "亂碼檔案" "rc=$rc"
  : > empty.zip; $PA --list empty.zip >/dev/null 2>&1; rc=$?; [ $rc -lt 128 ] && ok "0 位元組的檔案不會當掉（結束碼 $rc）" || bad "0 位元組檔案" "rc=$rc"
  $PA --list /nonexistent.zip >/dev/null 2>&1; [ $? = 1 ] && ok "不存在的檔案 → 1" || bad "不存在的檔案"
  [ $ESC = 0 ] && ok "整組惡意封存檔：沒有任何東西逃出解壓目錄" || bad "有東西逃出去了"
  cd "$W"
fi

if want big; then
  echo "== 大檔與記憶體"
  python3 - <<'PY'
import os
with open('big.dat','wb') as f:
    blk = (b'pomelo archive test block ' * 4000)[:100000]
    for i in range(2100): f.write(blk)   # 約 210 MB，重複內容
PY
  mem() { python3 - "$@" <<'PY'
import resource, subprocess, sys, time
t=time.time(); r=subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ru=resource.getrusage(resource.RUSAGE_CHILDREN)
print("%d %.1f %d" % (r.returncode, time.time()-t, ru.ru_maxrss//1024))
PY
  }
  for f in tar.gz zip tar.zst; do
    rm -f big.$f; rm -rf xb_$f
    res=$(mem $PA --create big.$f --format=$f big.dat); set -- $res
    [ "$1" = 0 ] && { ok "大檔建立 $f（$2 秒、記憶體 $3 MB）"; [ "$3" -lt 300 ] && ok "大檔建立 $f：記憶體 < 300 MB" || bad "大檔建立 $f 記憶體 $3 MB"; } || { [ "$1" = 4 ] && skip "大檔 $f" "缺後端" || bad "大檔建立 $f" "rc=$1"; continue; }
    res=$(mem $PA --extract big.$f -C xb_$f); set -- $res
    [ "$1" = 0 ] && cmp -s big.dat xb_$f/big.dat && ok "大檔解壓 $f 內容一致（$2 秒、記憶體 $3 MB）" || bad "大檔解壓 $f"
    [ "$3" -lt 300 ] && ok "大檔解壓 $f：記憶體 < 300 MB" || bad "大檔解壓 $f 記憶體 $3 MB"
  done
  rm -f big.dat big.* ; rm -rf xb_*
fi
echo
summary
