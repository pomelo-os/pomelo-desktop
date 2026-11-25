/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pkg.c — 套件管理：執行時判斷是 apt（Debian、Ubuntu）還是 dnf（Fedora、Rocky、RHEL），
 * 讓軟體中心、系統更新、儲存空間清理、語言設定用同一份程式碼。
 *
 * 程式裡一律用 Debian 的套件名稱，在 dnf 的系統上由 pm_pkg_name() 換成那邊的名稱
 * （例如 sane-utils → sane-backends、ffmpeg → ffmpeg-free；Rocky 9 沒有 plocate，用 mlocate）。
 */
#define _GNU_SOURCE
#include "pomelo.h"
#include "pomelo-widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int os_has(const char *id)
{
	static char ids[512];
	static int loaded;
	if (!loaded) {
		loaded = 1;
		FILE *f = fopen("/etc/os-release", "r");
		char line[256];
		while (f && fgets(line, sizeof line, f)) {
			if (strncmp(line, "ID=", 3) && strncmp(line, "ID_LIKE=", 8)) continue;
			char *v = strchr(line, '=') + 1;
			v[strcspn(v, "\n")] = 0;
			for (char *c = v; *c; c++) if (*c == '"' || *c == '\'') *c = ' ';
			size_t l = strlen(ids);
			snprintf(ids + l, sizeof ids - l, " %s ", v);
		}
		if (f) fclose(f);
	}
	char k[64];
	snprintf(k, sizeof k, " %s ", id);
	return strstr(ids, k) != NULL;
}

int pm_os_like(const char *id) { return os_has(id); }

int pm_pkg_kind(void)
{
	static int k = -1;
	const char *e = getenv("POMELO_PKG");   /* 測試用：apt 或 dnf */
	if (k < 0 && e && *e) k = !strcmp(e, "dnf") ? PM_PKG_DNF : PM_PKG_APT;
	if (k < 0) k = !access("/usr/bin/apt-get", X_OK) ? PM_PKG_APT : !access("/usr/bin/dnf", X_OK) ? PM_PKG_DNF : PM_PKG_NONE;
	return k;
}

/* Debian 名稱 → Fedora、RHEL 9 系（Rocky、Alma）的名稱；NULL = 那邊沒有 */
static const struct { const char *deb, *fedora, *el; } names[] = {
	{ "sane-utils", "sane-backends", "sane-backends" },
	{ "ffmpeg", "ffmpeg-free", "ffmpeg-free" },
	{ "libheif1", "libheif", "libheif" },                  /* 看圖的 HEIC／AVIF；Rocky：EPEL */
	{ "libheif-plugin-libde265", NULL, NULL },             /* HEVC 解碼器：Fedora／Rocky 官方沒有（RPM Fusion 的 libheif-freeworld） */
	{ "xinput", "xinput", "xorg-x11-server-utils" },
	{ "x11-xserver-utils", "xrandr", "xorg-x11-server-utils" },
	{ "plocate", "plocate", "mlocate" },
	{ "network-manager", "NetworkManager", "NetworkManager" },
	{ "network-manager-openvpn", "NetworkManager-openvpn", "NetworkManager-openvpn" },   /* 只裝基本外掛，不要 -gnome；Rocky：EPEL */
	{ "wireguard-tools", "wireguard-tools", "wireguard-tools" },
	{ "libnotify-bin", "libnotify", "libnotify" },
	{ "fonts-noto-cjk", "google-noto-sans-cjk-fonts", "google-noto-sans-cjk-ttc-fonts" },
	{ "ubuntu-drivers-common", NULL, NULL },
	{ "fcitx5", "fcitx5", NULL },
	{ "fcitx5-chewing", "fcitx5-chewing", NULL },
	{ "fcitx5-frontend-gtk3", "fcitx5-gtk3", NULL },
	{ "fcitx5-frontend-gtk4", "fcitx5-gtk4", NULL },
	{ "fcitx5-frontend-qt5", "fcitx5-qt5", NULL },
	{ "ibus-chewing", "ibus-chewing", "ibus-chewing" },   /* Rocky：EPEL */
	{ "transmission-gtk", "transmission-gtk", NULL },
	{ "vlc", "vlc", NULL },
	{ "kdenlive", "kdenlive", NULL },
	{ "obs-studio", "obs-studio", NULL },
	{ "audacious", "audacious", NULL },
	{ "gnome-text-editor", "gnome-text-editor", NULL },
	/* pomelo-input 的引擎（dlopen 載入；Rocky 9 沒有 librime） */
	{ "libhangul1", "libhangul", "libhangul" },
	{ "librime1t64", "librime", NULL },
	{ "librime1", "librime", NULL },
	{ "libanthy1t64", "anthy-unicode", "anthy-unicode" },
	{ "libanthy1", "anthy-unicode", "anthy-unicode" },
	{ "anthy-common", NULL, NULL },
	/* 看圖的 TIFF（dlopen 載入） */
	{ "libtiff6", "libtiff", "libtiff" },
	{ "libtiff5", "libtiff", "libtiff" },
	/* 病毒掃描（ClamAV；Rocky：EPEL） */
	{ "clamav", "clamav", "clamav" },
	{ "clamav-freshclam", "clamav-update", "clamav-update" },
		/* 視窗陰影與動畫（Rocky 9 連 EPEL 都沒有 picom） */
	{ "picom", "picom", NULL },
};

const char *pm_pkg_name(const char *deb)
{
	if (pm_pkg_kind() != PM_PKG_DNF) return deb;
	int el = os_has("rhel") || os_has("centos");
	for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strcmp(names[i].deb, deb)) return el ? names[i].el : names[i].fedora;
	return deb;
}

int pm_pkg_argv(const char *action, const char *const *debs, int n, char **argv, int max)
{
	int k = 0, dnf = pm_pkg_kind() == PM_PKG_DNF;
	if (max < 8) return 0;
	argv[k++] = dnf ? "dnf" : "apt-get";
	if (!strcmp(action, "install") || !strcmp(action, "install-min")) {
		argv[k++] = "install";
		argv[k++] = "-y";
		if (!strcmp(action, "install-min")) argv[k++] = dnf ? "--setopt=install_weak_deps=False" : "--no-install-recommends";
	} else if (!strcmp(action, "remove")) { argv[k++] = "remove"; argv[k++] = "-y"; }
	else if (!strcmp(action, "refresh")) { argv[k++] = dnf ? "makecache" : "update"; if (dnf) argv[k++] = "--refresh"; }
	else if (!strcmp(action, "upgrade")) { argv[k++] = dnf ? "upgrade" : "upgrade"; argv[k++] = "-y"; }
	else if (!strcmp(action, "autoremove")) { argv[k++] = "autoremove"; argv[k++] = "-y"; }
	else if (!strcmp(action, "clean")) { argv[k++] = "clean"; if (dnf) argv[k++] = "packages"; }
	else return 0;
	int any = 0;
	for (int i = 0; i < n && k < max - 1; i++) {
		const char *p = pm_pkg_name(debs[i]);
		if (p) { argv[k++] = (char *)p; any = 1; }
	}
	if (n && !any) return 0;   /* 要裝的東西這個系統都沒有 */
	argv[k] = NULL;
	return k;
}

char *pm_pkg_hint(const char *debs)
{
	char buf[512], one[128];
	int dnf = pm_pkg_kind() == PM_PKG_DNF;
	snprintf(buf, sizeof buf, "sudo %s install", dnf ? "dnf" : "apt");
	const char *p = debs;
	int any = 0;
	while (*p) {
		while (*p == ' ') p++;
		size_t l = strcspn(p, " ");
		if (!l) break;
		snprintf(one, sizeof one, "%.*s", (int)l, p);
		const char *m = pm_pkg_name(one);
		if (m) {
			size_t b = strlen(buf);
			snprintf(buf + b, sizeof buf - b, " %s", m);
			any = 1;
		}
		p += l;
	}
	return any ? strdup(buf) : NULL;
}

void pm_pkg_check(const char *const *debs, int n, int *installed)
{
	if (n <= 0) return;
	int dnf = pm_pkg_kind() == PM_PKG_DNF;
	size_t cap = 96;
	for (int i = 0; i < n; i++) cap += strlen(debs[i]) + 3;
	char *cmd = malloc(cap), *names[256];
	if (!cmd) return;
	strcpy(cmd, dnf ? "rpm -q --qf '%{NAME} installed\\n'" : "dpkg-query -W -f='${Package} ${Status}\\n'");
	for (int i = 0; i < n && i < 256; i++) {
		names[i] = (char *)(pm_pkg_name(debs[i]) ? pm_pkg_name(debs[i]) : debs[i]);
		strcat(cmd, " ");
		strcat(cmd, names[i]);
	}
	char *t = pm_run(cmd);
	free(cmd);
	for (int i = 0; i < n && i < 256; i++) {
		installed[i] = 0;
		if (!t) continue;
		char key[200];
		snprintf(key, sizeof key, dnf ? "%s installed" : "%s install ok installed", names[i]);
		for (char *p = strstr(t, key); p; p = strstr(p + 1, key))
			if ((p == t || p[-1] == '\n') && (p[strlen(key)] == '\n' || !p[strlen(key)])) { installed[i] = 1; break; }
	}
	free(t);
}

int pm_pkg_installed(const char *deb)
{
	int r = 0;
	pm_pkg_check(&deb, 1, &r);
	return r;
}

char *pm_pkg_upgradable(void)
{
	int dnf = pm_pkg_kind() == PM_PKG_DNF;
	/* dnf 不用管理員權限也能列出（用使用者自己的快取）；apt 讀系統的套件清單 */
	char *t = pm_run(dnf ? "dnf -q --cacheonly list --upgrades 2>/dev/null || dnf -q list --upgrades" : "apt list --upgradable");
	if (!t) return NULL;
	size_t cap = strlen(t) + 1, o = 0;
	char *out = malloc(cap), *save = NULL;
	if (!out) { free(t); return NULL; }
	out[0] = 0;
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char name[160] = "", ver[96] = "";
		if (dnf) {
			/* 名稱.架構  新版本  套件庫（標題行與「Available Upgrades」略過） */
			if (sscanf(l, "%159s %95s", name, ver) != 2 || !strchr(name, '.') || !strpbrk(ver, "0123456789")) continue;
			*strrchr(name, '.') = 0;
		} else {
			/* 名稱/來源 新版本 架構 [upgradable from: 舊版本] */
			char *slash = strchr(l, '/');
			if (!slash || !strstr(l, "upgradable")) continue;
			snprintf(name, sizeof name, "%.*s", (int)(slash - l), l);
			sscanf(strchr(l, ' ') ? strchr(l, ' ') + 1 : "", "%95s", ver);
		}
		o += (size_t)snprintf(out + o, cap - o, "%s\t%s\n", name, ver);
		if (o >= cap) break;
	}
	free(t);
	return out;
}

char *pm_pkg_owners(const char *const *paths, int n)
{
	int dnf = pm_pkg_kind() == PM_PKG_DNF;
	if (dnf) {
		/* 一次列出所有套件裡的 .desktop 檔：「路徑<Tab>套件」 */
		return pm_run("rpm -qa --qf '[%{FILENAMES}\\t%{=NAME}\\n]' | grep '\\.desktop\t'");
	}
	size_t cap = 32;
	for (int i = 0; i < n; i++) cap += strlen(paths[i]) + 4;
	char *cmd = malloc(cap);
	if (!cmd) return NULL;
	strcpy(cmd, "dpkg -S");
	int any = 0;
	for (int i = 0; i < n; i++) {
		if (strchr(paths[i], '\'')) continue;   /* 放在單引號裡交給 shell */
		strcat(cmd, " '");
		strcat(cmd, paths[i]);
		strcat(cmd, "'");
		any = 1;
	}
	char *t = any ? pm_run(cmd) : NULL;
	free(cmd);
	if (!t) return NULL;
	/* 「套件: 路徑」→「路徑<Tab>套件」 */
	size_t tl = strlen(t);
	char *out = malloc(tl + 2), *save = NULL;
	size_t o = 0;
	if (!out) { free(t); return NULL; }
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char *colon = strstr(l, ": ");
		if (!colon) continue;
		*colon = 0;
		char *pkg = l, *c = strchr(pkg, ':');   /* 「套件:架構」只留名稱；「a, b: 路徑」取第一個 */
		if (c) *c = 0;
		if ((c = strchr(pkg, ','))) *c = 0;
		o += (size_t)snprintf(out + o, tl + 2 - o, "%s\t%s\n", colon + 2, pkg);
	}
	out[o] = 0;
	free(t);
	return out;
}

char *pm_pkg_cmdline(const char *action, const char *debs)
{
	char *hint = pm_pkg_hint(debs);
	if (!hint) return NULL;
	/* hint 是「sudo apt install a b」：補上 -y 與不裝建議套件 */
	int dnf = pm_pkg_kind() == PM_PKG_DNF;
	const char *rest = hint + strlen(dnf ? "sudo dnf install" : "sudo apt install");
	char buf[600];
	snprintf(buf, sizeof buf, "sudo %s install -y%s%s", dnf ? "dnf" : "apt",
	         !strcmp(action, "install-min") ? (dnf ? " --setopt=install_weak_deps=False" : " --no-install-recommends") : "", rest);
	free(hint);
	return strdup(buf);
}
