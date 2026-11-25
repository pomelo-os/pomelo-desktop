#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 產生 Pomelo 各套件的 .rpm（Fedora、Rocky／Alma／RHEL 9 系），放在 dist/<發行版>/（例如 dist/fedora44/）
#
#   docker run --rm -v "$PWD":/src -u $(id -u):$(id -g) -e HOME=/tmp pomelo-build-rpm:fedora44 ./packaging/build-rpm.sh
#
# 跟 build-deb.sh 用同一份 packages.txt：套件名稱與相依照 packaging/rpm-names.txt 換成這邊的名稱，
# 執行檔需要的函式庫由 rpmbuild 自動算出。PAM 設定用 Red Hat 的寫法（PAMSTYLE=redhat）。
set -e

VERSION=${VERSION:-0.42}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# 在暫存的副本裡編譯：同時建好幾個發行版的套件也不會互相干擾
if [ -z "$POMELO_IN_COPY" ]; then
	W=$(mktemp -d)
	tar -C "$ROOT" --exclude=./build --exclude=./dist --exclude='./*.tar.gz' -cf - . | tar -C "$W" -xf -
	set +e
	POMELO_IN_COPY=1 OUT="$ROOT/dist" "$W/packaging/build-rpm.sh" "$@"
	rc=$?
	rm -rf "$W"
	exit $rc
fi
DISTRO=$(. /etc/os-release && echo "$ID$VERSION_ID")
ARCH=$(uname -m)   # x86_64 或 aarch64（arm64 在 qemu-user 下 uname -m 也是 aarch64）
DD="$DISTRO"; [ "$ARCH" = x86_64 ] || DD="$DISTRO-arm64"   # arm64 的套件放 dist/<發行版>-arm64，不跟 x86_64 混在一起
EL=0
grep -Eq '^ID_LIKE=.*(rhel|centos)' /etc/os-release && EL=1
LIBDIR=/usr/lib64
B="$ROOT/build"
D="${OUT:-$ROOT/dist}/$DD"
T="$ROOT/build/rpmbuild"
LIB="$ROOT/libpomelo"

# Rocky／RHEL 9 沒有 xcb-imdkit：用建置映像在 /opt/xcb-imdkit 編好的版本，跟著 pomelo-input 一起裝
if [ -d /opt/xcb-imdkit ]; then
	export PKG_CONFIG_PATH=/opt/xcb-imdkit/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
fi

# 1. 以 /usr 為安裝位置重新編譯
make -C "$ROOT" clean >/dev/null
rm -rf "$B" "$D"
mkdir -p "$B" "$D" "$T/SPECS"
make -C "$ROOT/libpomelo" PREFIX=/usr LIBDIR=$LIBDIR VERSION="$VERSION" >/dev/null
make -C "$ROOT" all PREFIX=/usr VERSION="$VERSION" >/dev/null
if [ -d /opt/xcb-imdkit ]; then
	rm -f "$ROOT/pomelo-input/pomelo-input"
	make -C "$ROOT/pomelo-input" POMELO_CFLAGS="-I$LIB $(pkg-config --cflags x11 xext xrandr cairo)" \
		POMELO_LIBS="-L$LIB -lpomelo $(pkg-config --libs x11 xext xrandr cairo)" \
		LDFLAGS="-Wl,-rpath,$LIBDIR/pomelo-input" >/dev/null
fi
# wlroots 不是系統套件（故意的，五個發行版釘死同一版本，見 packaging/third-party/wlroots/NOTICE.txt）：
# pomelo-compositor 自己帶一份私有的 libwlroots.so，跟 Rocky 附帶私有 xcb-imdkit 是同一招。
if [ -d /opt/wlroots ]; then
	rm -f "$ROOT/pomelo-compositor/pomelo-compositor"
	make -C "$ROOT/pomelo-compositor" LDFLAGS="-Wl,-rpath,$LIBDIR/pomelo-compositor" >/dev/null
fi

# 2. 把每個套件安裝到 build/<套件>/（名稱先用 packages.txt 裡的 Debian 名稱）
make -C "$LIB" install DESTDIR="$B/_lib" PREFIX=/usr LIBDIR=$LIBDIR >/dev/null
mkdir -p "$B/libpomelo0$LIBDIR" "$B/libpomelo-dev$LIBDIR" "$B/libpomelo0/usr/share"
cp -a "$B/_lib$LIBDIR/libpomelo.so.0" "$B/libpomelo0$LIBDIR/"
cp -a "$B/_lib/usr/share/pomelo" "$B/libpomelo0/usr/share/"   # 介面翻譯
mkdir -p "$B/libpomelo0/usr/bin"
cp -a "$B/_lib/usr/bin/pomelo-open" "$B/libpomelo0/usr/bin/"   # 用預設程式開啟（取代 xdg-open）
cp -a "$B/_lib/usr/include" "$B/libpomelo-dev/usr/"
cp -a "$B/_lib$LIBDIR/libpomelo.so" "$B/_lib$LIBDIR/pkgconfig" "$B/libpomelo-dev$LIBDIR/"
rm -rf "$B/_lib"

for dir in pomelo-wm pomelo-compositor pomelo-panel pomelo-session pomelo-terminal pomelo-files pomelo-theme \
           pomelo-sound pomelo-network pomelo-control-center pomelo-system \
           pomelo-lock pomelo-notify pomelo-power pomelo-screensaver pomelo-language \
           pomelo-launcher pomelo-software pomelo-display pomelo-bluetooth pomelo-monitor pomelo-storage pomelo-polkit pomelo-assistant pomelo-keyring pomelo-edit pomelo-input pomelo-greeter pomelo-dm pomelo-screenshot pomelo-calc pomelo-calendar pomelo-viewer pomelo-player pomelo-pdf pomelo-paint pomelo-remote pomelo-writer pomelo-sheet pomelo-slides pomelo-vm pomelo-docker pomelo-cluster pomelo-archive pomelo-packets pomelo-doctor pomelo-firewall pomelo-netscan pomelo-gpu pomelo-backup pomelo-vpn; do
	make -C "$ROOT/$dir" install DESTDIR="$B/$dir" PREFIX=/usr PAMSTYLE=redhat PAMDIR=$LIBDIR/security \
		POMELO_CFLAGS="-I$LIB $(pkg-config --cflags x11 xext xrandr cairo)" \
		POMELO_LIBS="-L$LIB -lpomelo $(pkg-config --libs x11 xext xrandr cairo)" \
		LD_LIBRARY_PATH="$LIB" >/dev/null
done

# 小柚瀏覽器：要 WebKitGTK 6.0，建置環境沒有（make all 就不會編）就不裝、也不打包
if [ -x "$ROOT/pomelo-browser/pomelo-browser" ]; then
	make -C "$ROOT/pomelo-browser" install DESTDIR="$B/pomelo-browser" PREFIX=/usr \
		POMELO_CFLAGS="-I$LIB $(pkg-config --cflags x11 xext xrandr cairo)" \
		POMELO_LIBS="-L$LIB -lpomelo $(pkg-config --libs x11 xext xrandr cairo)" \
		LD_LIBRARY_PATH="$LIB" >/dev/null
fi
# pomelo-dm 的 SELinux 規則（%post 用 semodule 載入）
install -Dm644 "$ROOT/pomelo-dm/pomelo-dm.cil" "$B/pomelo-dm/usr/share/selinux/packages/pomelo-dm.cil"
if [ -d /opt/xcb-imdkit ]; then
	mkdir -p "$B/pomelo-input$LIBDIR/pomelo-input"
	cp -a /opt/xcb-imdkit/lib64/libxcb-imdkit.so.1* "$B/pomelo-input$LIBDIR/pomelo-input/"
	# 它的授權（LGPL-2.1-only）與裡面保留的版權聲明；原始碼在 Pomelo 的原始碼包（packaging/third-party）
	mkdir -p "$B/pomelo-input/usr/share/licenses/pomelo-input/xcb-imdkit"
	cp "$ROOT/packaging/third-party/xcb-imdkit/LGPL-2.1-only.txt" "$ROOT/packaging/third-party/xcb-imdkit/NOTICE.txt" \
		"$B/pomelo-input/usr/share/licenses/pomelo-input/xcb-imdkit/"
fi
if [ -d /opt/wlroots ]; then
	mkdir -p "$B/pomelo-compositor$LIBDIR/pomelo-compositor"
	cp -a /opt/wlroots/lib64/*.so* "$B/pomelo-compositor$LIBDIR/pomelo-compositor/"
	mkdir -p "$B/pomelo-compositor/usr/share/licenses/pomelo-compositor/wlroots"
	cp "$ROOT/packaging/third-party/wlroots/MIT-LICENSE.txt" "$ROOT/packaging/third-party/wlroots/NOTICE.txt" \
		"$B/pomelo-compositor/usr/share/licenses/pomelo-compositor/wlroots/"
fi

# 3. 名稱與相依換成這邊的寫法
rpmname() {   # Debian 名稱 → 這邊的名稱（「-」= 沒有）
	local n=$1 col=2
	[ $EL = 1 ] && col=3
	local m
	m=$(awk -F';' -v n="$n" -v c=$col '!/^#/ && $1 == n { print $c; exit }' "$ROOT/packaging/rpm-names.txt")
	echo "${m:-$n}"
}
rpmdeps() {   # 「a (>= 1), b | c」→「a >= 1, b」（替代選項取第一個這邊有的）
	local out="" item alt name ver op v
	IFS=',' read -ra items <<< "$1"
	for item in "${items[@]}"; do
		local pick=""
		IFS='|' read -ra alts <<< "$item"
		for alt in "${alts[@]}"; do
			alt=$(echo "$alt" | sed 's/^ *//; s/ *$//')
			[ -n "$alt" ] || continue
			name=${alt%% *}
			ver=""
			if [[ "$alt" == *"("* ]]; then
				op=$(echo "$alt" | sed 's/.*(\([<>=]*\) *\(.*\)).*/\1/')
				v=$(echo "$alt" | sed 's/.*(\([<>=]*\) *\(.*\)).*/\2/')
				[ "$op" = "=" ] && v="%{version}-%{release}"
				ver=" $op $v"
			fi
			name=$(rpmname "$name")
			[ "$name" = "-" ] && continue
			pick="$name$ver"
			break
		done
		[ -n "$pick" ] && out="${out:+$out, }$pick"
	done
	echo "$out"
}

grep -v '^#' "$ROOT/packaging/packages.txt" | while IFS=';' read -r name kind src depends recommends provides desc; do
	[ -n "$name" ] || continue
	rname=$(rpmname "$name")
	dir="$B/$name"
	if [ "$kind" = bin ] && [ ! -d "$dir" ]; then echo "  略過 $name（這個發行版沒有編，例如 Rocky 9 沒有 WebKitGTK 6.0）"; continue; fi
	mkdir -p "$dir/usr/share/doc/$rname"
	printf 'Pomelo %s\n' "$VERSION" > "$dir/usr/share/doc/$rname/README"
	# 授權：GPLv3 全文（pomelo-pdf 另附 Adobe Glyph List、pomelo-input 另附 librime 標頭檔的 BSD-3-Clause）
	mkdir -p "$dir/usr/share/licenses/$rname"
	cp "$ROOT/LICENSE" "$dir/usr/share/licenses/$rname/LICENSE"
	license="GPL-3.0-or-later"
	if [ "$name" = pomelo-pdf ]; then
		cp "$ROOT/pomelo-pdf/AGL-LICENSE.txt" "$dir/usr/share/licenses/$rname/"
		license="GPL-3.0-or-later AND BSD-3-Clause"
	fi
	if [ "$name" = libpomelo0 ] || [ "$name" = pomelo-viewer ] || [ "$name" = pomelo-paint ]; then   # 畫 SVG 用的 nanosvg（zlib）
		cp "$ROOT/libpomelo/third-party/nanosvg-LICENSE.txt" "$dir/usr/share/licenses/$rname/"
		license="GPL-3.0-or-later AND Zlib"
	fi
	if [ "$name" = pomelo-input ]; then   # librime 的 API 標頭檔（BSD-3-Clause）；Rocky 9 另附私有的 xcb-imdkit（LGPL-2.1-only）
		cp "$ROOT/pomelo-input/third-party/librime-LICENSE.txt" "$dir/usr/share/licenses/$rname/"
		license="GPL-3.0-or-later AND BSD-3-Clause"
		[ -d "$dir/usr/share/licenses/$rname/xcb-imdkit" ] && license="$license AND LGPL-2.1-only"
	fi
	if [ "$name" = pomelo-assistant ]; then   # 語音輸入的簡轉繁表（OpenCC）與讀音表（Unihan）
		cp "$ROOT/pomelo-assistant/third-party/OpenCC-LICENSE.txt" "$ROOT/pomelo-assistant/third-party/Unicode-LICENSE.txt" "$dir/usr/share/licenses/$rname/"
		license="GPL-3.0-or-later AND Apache-2.0 AND Unicode-3.0"
	fi
	if [ "$name" = pomelo-compositor ]; then   # 私有附帶的 wlroots（MIT）
		license="GPL-3.0-or-later AND MIT"
	fi
	# 「(= 版本)」在 rpmdeps 裡會換成 %{version}-%{release}
	depends=$(echo "$depends" | sed -e "s/@PKGVER@/@VERSION@/g" -e "s/@VERSION@/$VERSION/g")
	req=$(rpmdeps "$depends")
	rec=$(rpmdeps "$recommends")
	sug=$(rpmdeps "$(awk -F';' -v n="$name" '!/^#/ && $1 == n { print $2; exit }' "$ROOT/packaging/suggests.txt")")
	arch=noarch
	if [ "$kind" = bin ]; then
		arch=$ARCH
		find "$dir/usr" -type f \( -perm -u+x -o -name '*.so.*' -o -name '*.so' \) -exec sh -c \
			'file -b "$1" | grep -q ELF && strip --strip-unneeded "$1"' _ {} \; 2>/dev/null || true
		# Fedora／RHEL 的慣例：共用函式庫是 755；RHEL 9 的 rpm 只替有執行權限的 .so 產生「提供」宣告
		find "$dir/usr" -type f \( -name '*.so.*' -o -name '*.so' \) -exec chmod 755 {} +
	fi
	spec="$T/SPECS/$rname.spec"
	{
		echo "%global debug_package %{nil}"
		echo "%global __os_install_post %{nil}"
		echo "%define _build_id_links none"
		echo "Name: $rname"
		echo "Version: $VERSION"
		echo "Release: 1%{?dist}"
		echo "Summary: $desc"
		echo "License: $license"
		echo "URL: https://fedora.tw"
		echo "Packager: pake <pake@fedora.tw>"
		echo "Vendor: fedora.tw"
		echo "BuildArch: $arch"
		[ -n "$req" ] && echo "Requires: $req"
		[ -n "$rec" ] && echo "Recommends: $rec"
		[ -n "$sug" ] && echo "Suggests: $sug"
		[ "$name" = pomelo-dm ] && echo "Requires(pre): shadow-utils"
		# Fedora／Rocky 的 libseat 沒有內建 seatd，要 seatd 服務（%post 會啟用）
		[ "$name" = pomelo-compositor ] && echo "Requires: seatd"
		[ "$name" = libpomelo0 ] && echo "Provides: libpomelo0 = %{version}-%{release}"
		# 0.40 起 libpomelo-devel 不在「dnf install ./*.rpm」的範圍（放在「開發用」資料夾）：舊版裝過的話
		# 它嚴格相依舊的 libpomelo0，dnf 會直接報衝突；升級時讓 libpomelo0 把舊的開發套件換掉
		[ "$name" = libpomelo0 ] && echo "Obsoletes: libpomelo-devel < 0.40"
		if [ -d "$dir$LIBDIR/pomelo-input" ]; then   # 私有的 xcb-imdkit：不要對外宣稱提供，也不要求系統有
			echo "%global __provides_exclude_from ^$LIBDIR/pomelo-input/.*\$"
			echo "%global __requires_exclude ^libxcb-imdkit\\.so.*\$"
		fi
		if [ -d "$dir$LIBDIR/pomelo-compositor" ]; then   # 私有的 wlroots：不要對外宣稱提供，也不要求系統有
			# 在 Rocky／舊版發行版上，wlroots 要求的 wayland-server／pixman 版本比系統套件新，
			# meson 會用 wrap fallback 自己編一份一起裝進這個私有目錄（見 Dockerfile.rpm），
			# 所以這裡的排除規則要連這些一起排除，不只 libwlroots.so 本體。只排除私有目錄裡真的有的：
			# Fedora 用的是系統的 wayland-server／pixman，寫死的話相依會被排掉、乾淨系統裝完跑不起來。
			priv=$(cd "$dir$LIBDIR/pomelo-compositor" && ls *.so.* | sed 's/\.so\..*//' | sort -u | paste -sd'|')
			echo "%global __provides_exclude_from ^$LIBDIR/pomelo-compositor/.*\$"
			echo "%global __requires_exclude ^($priv)\\.so.*\$"
		fi
		echo
		echo "%description"
		echo "$desc"
		echo "Pomelo 是輕量、省資源的 X11 桌面。這個套件是其中的一個元件，可以單獨安裝，或安裝 pomelo-desktop 取得完整桌面。"
		echo
		echo "%install"
		echo "cp -a $dir/. %{buildroot}/"
		echo
		for m in pre post preun postun; do
			[ -f "$ROOT/packaging/scripts-rpm/$name.$m" ] && { echo "%$m"; cat "$ROOT/packaging/scripts-rpm/$name.$m"; echo; }
		done
		[ "$name" = libpomelo0 ] && printf '%%post -p /sbin/ldconfig\n%%postun -p /sbin/ldconfig\n\n'
		echo "%files"
		# 檔案；/etc 底下的是設定檔，使用者改過的不會被升級蓋掉；名稱有 pomelo 的資料夾歸這個套件
		(cd "$dir" && find . -mindepth 1 \( -type f -o -type l \) | sed 's|^\.||' | sort) | while read -r f; do
			case "$f" in
				/etc/*) echo "%config(noreplace) \"$f\"" ;;
				/usr/share/licenses/*) echo "%license \"$f\"" ;;
				*) echo "\"$f\"" ;;
			esac
		done
		(cd "$dir" && find . -mindepth 1 -type d | sed 's|^\.||' | sort) | grep -i pomelo | sed 's|.*|%dir "&"|'
	} > "$spec"
	rpmbuild -bb --quiet --define "_topdir $T" --define "_rpmdir $D" \
		--define "_build_name_fmt %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" "$spec" >/dev/null 2>"$T/$rname.log" \
		|| { echo "打包 $rname 失敗："; cat "$T/$rname.log"; exit 1; }
	echo "  $(cd "$D" && ls "$rname-$VERSION"-*.rpm)"
done

echo "完成：$(ls "$D" | wc -l) 個套件在 dist/$DD/"
