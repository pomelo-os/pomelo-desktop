#!/bin/bash
# 在乾淨的發行版容器裡：裝編譯與測試用的套件 → 裝 libpomelo（/dist）→ make → 跑核心與介面測試
# 用法（由 run-distro.sh 呼叫）：bash in-container.sh deb|rpm 發行版名稱
# 掛載：/src＝pomelo-archive 原始碼（唯讀）、/dist＝~/wm/dist/<發行版>（唯讀）、/res＝結果輸出
KIND=$1; DIST=$2
export LC_ALL=C.UTF-8
tryinst() {   # 一次裝；失敗就一個一個裝，缺的套件只記下來不中斷
  if [ $KIND = deb ]; then
    apt-get install -y -q --no-install-recommends "$@" >/tmp/inst.log 2>&1 && return 0
    for p in "$@"; do apt-get install -y -q --no-install-recommends "$p" >/dev/null 2>&1 || echo "（沒有套件 $p）"; done
  else
    # strict=0：找不到的套件略過，其他照裝（一個一個裝每次都要重載套件庫，很慢）
    dnf install -y -q --setopt=install_weak_deps=False --setopt=strict=0 "$@" >/tmp/inst.log 2>&1
    grep -o "No match for argument: .*" /tmp/inst.log | sed 's/No match for argument: /（沒有套件 /; s/$/）/' | sort -u
    return 0
  fi
}
echo "### $DIST：安裝套件"
if [ $KIND = deb ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -q >/dev/null 2>&1
  tryinst build-essential pkg-config libarchive-dev libx11-dev libxext-dev libxrandr-dev libcairo2-dev ca-certificates
  tryinst zip unzip gnupg mtools dosfstools squashfs-tools xorriso genisoimage lzip zstd xz-utils bzip2 cpio e2fsprogs python3 bc binutils diffutils findutils procps
  tryinst xvfb xdotool imagemagick fonts-noto-cjk x11-utils
  # 7-Zip：新的發行版叫 7zip（7zz），舊的叫 p7zip-full（7z）
  tryinst 7zip >/dev/null; command -v 7zz >/dev/null || command -v 7z >/dev/null || tryinst p7zip-full
  apt-get install -y -q --no-install-recommends /dist/libpomelo0_*.deb /dist/libpomelo-dev_*.deb >/tmp/pm.log 2>&1 || { echo "libpomelo 安裝失敗："; tail -5 /tmp/pm.log; dpkg -i --force-depends /dist/libpomelo0_*.deb /dist/libpomelo-dev_*.deb >/dev/null 2>&1; }
else
  if [ "$DIST" = rocky9.6 ]; then
    dnf install -y -q epel-release >/dev/null 2>&1; dnf config-manager --set-enabled crb >/dev/null 2>&1 || dnf install -y -q dnf-plugins-core >/dev/null 2>&1 && dnf config-manager --set-enabled crb >/dev/null 2>&1
  fi
  tryinst gcc make pkgconf-pkg-config libarchive-devel libX11-devel libXext-devel libXrandr-devel cairo-devel
  tryinst zip unzip gnupg2 mtools dosfstools squashfs-tools xorriso genisoimage lzip zstd xz bzip2 cpio e2fsprogs python3 bc binutils diffutils findutils procps-ng which
  tryinst xorg-x11-server-Xvfb xdotool ImageMagick google-noto-sans-cjk-ttc-fonts xorg-x11-utils
  if [ "$DIST" = rocky9.6 ]; then tryinst p7zip p7zip-plugins; else tryinst 7zip; command -v 7zz >/dev/null || command -v 7z >/dev/null || tryinst p7zip p7zip-plugins; fi
  dnf install -y -q --setopt=install_weak_deps=False /dist/libpomelo-0*.rpm /dist/libpomelo-devel-*.rpm >/tmp/pm.log 2>&1 || { echo "libpomelo 安裝失敗："; tail -5 /tmp/pm.log; rpm -i --nodeps /dist/libpomelo-0*.rpm /dist/libpomelo-devel-*.rpm 2>&1 | tail -2; }
fi
echo "libarchive：$(pkg-config --modversion libarchive)　pomelo：$(pkg-config --modversion pomelo 2>&1 | head -1)"
for c in 7zz 7z 7za gpg gpg2 zip mksquashfs xorriso genisoimage mcopy lzip zstd Xvfb xdotool; do printf '%s=%s ' $c "$(command -v $c >/dev/null && echo Y || echo n)"; done; echo
echo "### 編譯"
rm -rf /tmp/build && mkdir /tmp/build && cp -r /src/. /tmp/build/ && cd /tmp/build && make 2>&1 | grep -E "warning|error|Error" | head -20
[ -x pomelo-archive ] || { echo "編譯失敗"; exit 1; }
echo "編譯完成：$(ls -la pomelo-archive | awk '{print $5}') 位元組；防護："
readelf -lW pomelo-archive 2>/dev/null | grep -q "GNU_RELRO" && echo "  RELRO" ; readelf -dW pomelo-archive 2>/dev/null | grep -q "BIND_NOW\|FLAGS.*NOW" && echo "  BIND_NOW"; readelf -hW pomelo-archive | grep -q "DYN" && echo "  PIE"; readelf -sW pomelo-archive | grep -q "__stack_chk_fail" && echo "  stack-protector"
echo "### 能力（--formats）"
./pomelo-archive --formats 2>&1 | tee /res/formats-$DIST.txt | head -30
echo "### 核心測試"
PA=/tmp/build/pomelo-archive bash /src/test/test-core.sh 2>&1 | tee /res/core-$DIST.txt | grep -vE "^== |^  ok " | tail -40
echo "### 介面測試"
PA=/tmp/build/pomelo-archive SHOTS=/res/shots-$DIST bash /src/test/test-gui.sh 2>&1 | tee /res/gui-$DIST.txt | grep -vE "^== |^  ok " | tail -20
echo "### $DIST 完成"
