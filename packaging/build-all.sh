#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# 一次建好所有發行版的套件與安裝包（在有 Docker 的電腦上執行；編譯在容器裡做，不需要裝開發套件）
#
#   ./packaging/build-all.sh                 Ubuntu 24.04／26.04、Debian 13、Fedora 44／45、Rocky 9.8
#   ./packaging/build-all.sh fedora44        只建其中幾個
#   ./packaging/build-all.sh fedora45-arm64      Fedora 45 的 aarch64 版（同樣用 qemu-user 模擬，需要先註冊 binfmt，見下）
#   ./packaging/build-all.sh ubuntu26.04-arm64   arm64 版：在 x86 主機上用 qemu-user 模擬編譯，約 30 分鐘。要先註冊 binfmt，且 QEMU 要 10 以上
#       （Ubuntu 24.04 apt 的 qemu-user-static 8.2 沒有 openat2，26.04 的 tar 會失敗）：docker run --privileged --rm tonistiigi/binfmt --install arm64
#
# 產生：dist/<發行版>/ 的套件、pomelo-<版本>-<發行版>-<架構>.tar.gz，以及 pomelo-<版本>-source.tar.gz
# （原始碼：GPL 規定散布安裝包時要一起提供，請跟安裝包放在一起）
# （裡面有套件、安裝說明.txt、使用手冊.md／.html／.pdf、手冊用的圖、docs/ 的說明文件與 LICENSE）
# 使用手冊.html 是單一檔案（圖片內嵌），用 pandoc 產生；這台電腦沒有 pandoc 的話只附 .md
# 使用手冊.pdf 再用 weasyprint 轉（python3 -m pip install weasyprint 或 sudo apt install weasyprint）；沒有的話不附 PDF
set -e
VERSION=${VERSION:-$(sed -n 's/^VERSION ?= //p' "$(dirname "$0")/../Makefile")}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTDIR=${OUTDIR:-$ROOT}
LOGS="$ROOT/dist/logs"
TARGETS=${*:-"ubuntu24.04 ubuntu26.04 debian13 fedora44 fedora45 rocky9.8"}
mkdir -p "$LOGS"
command -v pandoc >/dev/null || echo "沒有 pandoc（sudo apt install pandoc）：安裝包只會附 使用手冊.md，沒有 HTML 版" >&2

# 發行版 → 建置映像、Docker 基底、Dockerfile、打包腳本
spec() {
	case "$1" in
		# 基底用 arm64v8/ubuntu（單一架構的官方映像）：舊版建置器在 --platform 下 FROM ubuntu:xx 仍會拿本機的 amd64 映像
		ubuntu*-arm64) v=${1#ubuntu}; v=${v%-arm64}; echo "pomelo-build:$v-arm64 arm64v8/ubuntu:$v Dockerfile.build build-deb.sh linux/arm64" ;;
		ubuntu*) v=${1#ubuntu}; echo "pomelo-build:$v ubuntu:$v Dockerfile.build build-deb.sh" ;;
		debian*) v=${1#debian}; echo "pomelo-build:$1 debian:$v Dockerfile.build build-deb.sh" ;;
		# Fedora arm64 同理用 arm64v8/fedora；建置映像標籤加 -arm64，輸出放 dist/fedora<版>-arm64
		fedora*-arm64) v=${1#fedora}; v=${v%-arm64}; echo "pomelo-build-rpm:fedora$v-arm64 arm64v8/fedora:$v Dockerfile.rpm build-rpm.sh linux/arm64" ;;
		fedora*) v=${1#fedora}; echo "pomelo-build-rpm:$1 fedora:$v Dockerfile.rpm build-rpm.sh" ;;
		rocky*) v=${1#rocky}; echo "pomelo-build-rpm:$1 rockylinux/rockylinux:$v Dockerfile.rpm build-rpm.sh" ;;
		*) echo "不認得的發行版：$1" >&2; exit 2 ;;
	esac
}

# 1. 建置映像（沒有，或 Dockerfile 改過了才建；映像上的標籤記著 Dockerfile 的雜湊值）
for t in $TARGETS; do
	set -- $(spec "$t")
	sum=$(sha256sum "$ROOT/packaging/docker/$3" | cut -c1-16)
	have=$(docker image inspect -f '{{index .Config.Labels "pomelo.dockerfile"}}' "$1" 2>/dev/null || true)
	if [ "$have" != "$sum" ]; then
		echo "建立建置環境 $1 …"
		docker build -q -t "$1" ${5:+--platform "$5"} --label "pomelo.dockerfile=$sum" --build-arg BASE="$2" \
			--build-arg WLROOTS_PATCH_B64="$(base64 -w0 "$ROOT/packaging/docker/wlroots-0.17.1-client-list-no-override-redirect.patch")" \
			-f "$ROOT/packaging/docker/$3" "$ROOT/packaging/docker" >"$LOGS/image-$t.log"
	fi
done

# 2. 同時編譯打包（每個打包腳本都在自己的暫存副本裡編譯，不會互相干擾）
for t in $TARGETS; do
	set -- $(spec "$t")
	(docker run --rm ${5:+--platform "$5"} -v "$ROOT":/src -w /src -u "$(id -u):$(id -g)" -e HOME=/tmp -e VERSION="$VERSION" "$1" \
		"./packaging/$4" >"$LOGS/build-$t.log" 2>&1 && echo "  $t：$(tail -n1 "$LOGS/build-$t.log")" \
		|| echo "  $t：失敗，見 $LOGS/build-$t.log") &
done
wait
for t in $TARGETS; do
	grep -q '^完成' "$LOGS/build-$t.log" || { echo "有發行版建置失敗，停止" >&2; exit 1; }
done

# 3. 安裝包
for t in $TARGETS; do
	case "$t" in
		ubuntu*-arm64) arch=arm64; fam=ubuntu ;;
		ubuntu*) arch=amd64; fam=ubuntu ;;
		debian*) arch=amd64; fam=debian ;;
		fedora*-arm64) arch=aarch64; fam=fedora ;;
		fedora*) arch=x86_64; fam=fedora ;;
		rocky*) arch=x86_64; fam=rocky ;;
	esac
	name="pomelo-$VERSION-${t%-arm64}"   # arm64 的安裝包叫 pomelo-<版本>-ubuntu26.04-arm64.tar.gz（架構只寫一次）
	W=$(mktemp -d)
	mkdir "$W/$name"
	cp "$ROOT/dist/$t"/* "$W/$name/"
	# 開發用的 libpomelo-dev 放到子資料夾：「apt install ./*.deb」才不會連帶裝進編譯器標頭檔、cpp、python 這些
	# （Ubuntu 上多 60 個套件、70 MB，一般使用者用不到；0.40 起）
	mkdir "$W/$name/開發用"
	mv "$W/$name"/libpomelo-dev* "$W/$name/開發用/" 2>/dev/null || rmdir "$W/$name/開發用"
	python3 "$ROOT/packaging/make-manual.py" "$fam" "$VERSION" "$arch" >"$W/$name/使用手冊.md"
	cp -r "$ROOT/docs/圖" "$W/$name/"
	cp "$ROOT/LICENSE" "$W/$name/LICENSE"
	cp "$ROOT/CHANGELOG.txt" "$W/$name/CHANGELOG.txt"
	if command -v pandoc >/dev/null; then
		(cd "$W/$name" && pandoc 使用手冊.md -f markdown+lists_without_preceding_blankline+east_asian_line_breaks -t html5 \
			--standalone --embed-resources --shift-heading-level-by=-1 --toc --toc-depth=2 \
			--css "$ROOT/packaging/manual.css" --metadata lang=zh-Hant -o 使用手冊.html)
		# PDF：同一份內容再加 manual-pdf.css（A4、頁碼、目錄附頁碼、每章換頁），用 weasyprint 轉；
		# 樣式要嵌進 HTML（weasyprint 另外傳的樣式優先順序比較低，蓋不過 manual.css）
		if python3 -c "import weasyprint" 2>/dev/null; then
			(cd "$W/$name" && pandoc 使用手冊.md -f markdown+lists_without_preceding_blankline+east_asian_line_breaks -t html5 \
				--standalone --embed-resources --shift-heading-level-by=-1 --toc --toc-depth=2 \
				--css "$ROOT/packaging/manual.css" --css "$ROOT/packaging/manual-pdf.css" --metadata lang=zh-Hant -o .pdf.html &&
			 python3 -c "import sys, weasyprint; weasyprint.HTML(sys.argv[1]).write_pdf(sys.argv[2])" .pdf.html 使用手冊.pdf 2>/dev/null;
			 rm -f .pdf.html)
		fi
	fi
	cp "$ROOT/docs/AI小幫手.md" "$ROOT/docs/登入管理員.md" "$ROOT/docs/輸入法.md" "$ROOT/docs/鑰匙圈.md" "$W/$name/"
	[ "$fam" = ubuntu ] && cp "$ROOT/packaging/清理GNOME套件.sh" "$W/$name/"
	[ "$fam" = rocky ] || cp "$ROOT/docs/小柚瀏覽器效能比較.pdf" "$W/$name/"   # Rocky 9 沒有小柚瀏覽器
	{
		released=$(sed -n "s/^$VERSION 更新（\([0-9-]*\)）.*/\1/p" "$ROOT/CHANGELOG.txt" | head -1)
		echo "Pomelo $VERSION 安裝（$t）${released:+　$released 發行}"
		echo
		if [ "$fam" = debian ]; then
			echo "  cd $name"
			echo "  sudo apt install --no-install-recommends libwebkitgtk-6.0-4 gstreamer1.0-plugins-good gstreamer1.0-libav"
			echo "  sudo apt install ./*.deb"
			echo
			echo "（第一行先用精簡方式裝小柚瀏覽器的網頁引擎：Debian 的 WebKitGTK 會「建議」整套影音外掛與圖片工具"
			echo "　（ImageMagick、Ghostscript…，約 300 MB，瀏覽器用不到）。其他套件照常安裝建議的套件。）"
			echo
			echo "想裝到最精簡（不裝任何建議套件）："
			echo "  sudo apt install --no-install-recommends ./*.deb"
		elif [ "$fam" = ubuntu ]; then
			echo "  cd $name"
			echo "  sudo apt install ./*.deb"
			echo
			echo "想裝到最精簡（不裝任何建議套件）："
			echo "  sudo apt install --no-install-recommends ./*.deb"
			echo
			echo "小柚瀏覽器裡網站的影片（H.264）播不出來時：sudo apt install gstreamer1.0-libav"
		else
			[ "$fam" = rocky ] && printf '  sudo dnf install epel-release\n  sudo dnf config-manager --set-enabled crb\n'
			echo "  cd $name"
			echo "  sudo dnf install ./*.rpm"
			echo
			echo "想裝到最精簡（不裝任何建議套件）："
			echo "  sudo dnf install --setopt=install_weak_deps=False ./*.rpm"
		fi
		echo
		echo "裝好後登出，在登入畫面選「Pomelo」工作階段（GDM：點使用者名稱後按右下角的齒輪）。"
		echo "也可以改用 Pomelo 自己的登入畫面：sudo pomelo-dm-switch try（先試用）、on（改用）、off（換回原本的）。"
		echo "萬一登不進去：按 Ctrl+Alt+F3 用文字登入，執行 sudo pomelo-dm-switch off，再 sudo reboot。"
		echo
		if [ -f "$W/$name/使用手冊.pdf" ]; then
			echo "完整的說明（快速鍵、輸入法、小幫手、疑難排解、移除）請看「使用手冊.html」（用瀏覽器打開，有圖）、「使用手冊.pdf」（可以列印）或「使用手冊.md」。"
		elif [ -f "$W/$name/使用手冊.html" ]; then
			echo "完整的說明（快速鍵、輸入法、小幫手、疑難排解、移除）請看「使用手冊.html」（用瀏覽器打開，有圖）或「使用手冊.md」。"
		else
			echo "完整的說明（快速鍵、輸入法、小幫手、疑難排解、移除）請看「使用手冊.md」。"
		fi
		echo
		if [ "$fam" = ubuntu ]; then
			echo "裝過舊版（0.7 以前）、想移除當時順便裝上的 GNOME 元件：sh 清理GNOME套件.sh"
			echo "（腳本只把套件交給 apt autoremove 判斷，登入畫面等還需要的都會保留）"
			echo
		fi
		echo "要寫 Pomelo 程式的開發者：「開發用」資料夾裡是 libpomelo 的標頭檔套件（一般使用不用裝）。"
		echo
		echo "授權：GNU 通用公眾授權條款第 3 版或更新的版本（GPL-3.0-or-later），全文見 LICENSE。"
		echo "原始碼：pomelo-$VERSION-source.tar.gz（跟這個安裝包放在一起），或寫信到 pake@fedora.tw 索取。"
		echo "開發者：pake@fedora.tw　組織：https://fedora.tw"
		echo
		echo "--------------------------------------------------------------------------"
		echo
		if [ "$fam" = ubuntu ]; then
			sed '1,2d' "$ROOT/CHANGELOG.txt"
		elif [ "$fam" = debian ]; then   # Debian 從 0.35 開始才有，只附這一版起的紀錄
			sed -n '3,/^0\.34 更新/p' "$ROOT/CHANGELOG.txt" | sed '$d'
		else   # Fedora、Rocky 從 0.26 開始才有，更早的紀錄只跟 Ubuntu 版有關
			sed -n '3,/^0\.25 更新/p' "$ROOT/CHANGELOG.txt" | sed '$d'
		fi
	} >"$W/$name/安裝說明.txt"
	tar -C "$W" -czf "$OUTDIR/$name-$arch.tar.gz" "$name"
	rm -rf "$W"
	echo "  $OUTDIR/$name-$arch.tar.gz（$(du -h "$OUTDIR/$name-$arch.tar.gz" | cut -f1)）"
done

# 4. 原始碼包：專案裡的原始檔（不含 dist、build、編譯出來的程式與 .o、舊的安裝包），
#    含 Rocky／RHEL 9 跟著 pomelo-input 一起裝的 xcb-imdkit 原始碼（packaging/third-party）
src="pomelo-$VERSION-source"
W=$(mktemp -d)
python3 - "$ROOT" "$W/$src" <<'PY'
import os, shutil, sys
root, out = sys.argv[1], sys.argv[2]
for dp, dns, fns in os.walk(root):
    rel = os.path.relpath(dp, root)
    if rel == '.':
        dns[:] = [d for d in dns if d not in ('dist', 'build', '.git')]
    dns.sort()
    for fn in sorted(fns):
        f = os.path.join(dp, fn)
        if os.path.islink(f) or fn.endswith(('.o', '.so')) or '.so.' in fn: continue
        # 最上層只收這幾個（不收安裝包、另外產生的手冊、放在這裡的紀錄檔之類）
        if rel == '.' and fn not in ('CHANGELOG.txt', 'LICENSE', 'README.md', 'Makefile'): continue
        with open(f, 'rb') as h:
            if h.read(4) == b'\x7fELF': continue
        d = os.path.join(out, rel)
        os.makedirs(d, exist_ok=True)
        shutil.copy2(f, d)
PY
tar -C "$W" -czf "$OUTDIR/$src.tar.gz" "$src"
rm -rf "$W"
echo "  $OUTDIR/$src.tar.gz（$(du -h "$OUTDIR/$src.tar.gz" | cut -f1)）"
