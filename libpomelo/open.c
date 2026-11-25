/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 用預設的程式開啟檔案或網址（0.40 起取代 xdg-open）
 *
 * xdg-open 在 Pomelo 這種「沒有人認得的桌面」上走它的通用路線，xdg-utils 又建議裝一整串 perl 模組
 * （Ubuntu 上約 50 個套件、60 MB），只為了開一個檔案。這裡照 freedesktop 的規則自己做：
 *   1. 判斷類型：網址看 scheme（x-scheme-handler/https）；檔案看 shared-mime-info 的副檔名表（globs2），
 *      資料夾是 inode/directory，沒有副檔名的看內容是不是文字；
 *   2. 找預設程式：mimeapps.list（使用者的、系統的；也看 pomelo-mimeapps.list）的 [Default Applications]，
 *      再來是舊的 defaults.list、[Added Associations]、各程式自己宣告的 mimeinfo.cache；
 *      text/x-csrc 這類找不到就退一步用 text/plain（mime 的 subclasses 表）；
 *   3. 照 .desktop 的 Exec 執行：%f／%u 換成檔案或網址，Terminal=true 的在 pomelo-terminal 裡跑。
 * 都找不到時，裝了 xdg-open 的話交給它。 */
#define _GNU_SOURCE
#include "pomelo.h"
#include "pomelo-widgets.h"
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ---- 類型 ---- */

/* globs2 裡這個副檔名的類型（權重高的寫在前面，第一個符合的就是）；只比對 *.副檔名 這種 */
static int glob_mime(const char *ext, char *out, size_t n)
{
	FILE *f = fopen("/usr/share/mime/globs2", "r");
	if (!f) f = fopen("/usr/local/share/mime/globs2", "r");
	if (!f) return 0;
	char line[512];
	int found = 0;
	while (!found && fgets(line, sizeof line, f)) {
		if (line[0] == '#') continue;
		line[strcspn(line, "\n")] = 0;
		char *m = strchr(line, ':'), *g = m ? strchr(m + 1, ':') : NULL;
		if (!g || strncmp(g + 1, "*.", 2)) continue;
		char *e = g + 3, *flags = strchr(e, ':');
		if (flags) *flags = 0;
		if (strcasecmp(e, ext)) continue;
		*g = 0;
		snprintf(out, n, "%s", m + 1);
		found = 1;
	}
	fclose(f);
	return found;
}

static void file_mime(const char *path, char *out, size_t n)
{
	struct stat st;
	if (!stat(path, &st) && S_ISDIR(st.st_mode)) { snprintf(out, n, "inode/directory"); return; }
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	/* 由最長的副檔名開始試：a.tar.gz → tar.gz → gz */
	for (const char *d = strchr(*base ? base + 1 : base, '.'); d; d = strchr(d + 1, '.'))   /* .bashrc 這種不算副檔名 */
		if (d[1] && glob_mime(d + 1, out, n)) return;
	/* 沒有副檔名（或不認得）：前 512 bytes 沒有 NUL 就當文字 */
	FILE *f = fopen(path, "rb");
	unsigned char buf[512];
	size_t k = f ? fread(buf, 1, sizeof buf, f) : 0;
	if (f) fclose(f);
	int text = k > 0 && !memchr(buf, 0, k);
	snprintf(out, n, "%s", text ? "text/plain" : "application/octet-stream");
}

/* ---- 預設程式 ---- */

/* 把 a.desktop;b.desktop; 裡第一個有安裝的程式放進 out（去掉 .desktop） */
static int first_installed(char *list, char *out, size_t n)
{
	char *save = NULL;
	for (char *id = strtok_r(list, ";", &save); id; id = strtok_r(NULL, ";", &save)) {
		while (*id == ' ') id++;
		char tmp[256];
		snprintf(tmp, sizeof tmp, "%s", id);
		char *dot = strstr(tmp, ".desktop");
		if (dot) *dot = 0;
		if (tmp[0] && pm_app_by_id(tmp)) { snprintf(out, n, "%s", tmp); return 1; }
	}
	return 0;
}

/* 在 path 這個 ini 檔的 [section] 找 mime= 那一行 */
static int lookup(const char *path, const char *section, const char *mime, char *out, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[4096];
	int in = 0, found = 0;
	size_t ml = strlen(mime);
	while (!found && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') { in = !strcmp(line, section); continue; }
		if (in && !strncmp(line, mime, ml) && line[ml] == '=') found = first_installed(line + ml + 1, out, n);
	}
	fclose(f);
	return found;
}

/* 依序要找的資料夾：使用者設定、系統設定（XDG_CONFIG_DIRS），使用者資料、系統資料（XDG_DATA_DIRS）的 applications */
static int dirs(char out[][PATH_MAX], int max)
{
	int k = 0;
	const char *h = getenv("HOME") ? getenv("HOME") : "/";
	const char *ch = getenv("XDG_CONFIG_HOME"), *dh = getenv("XDG_DATA_HOME");
	if (ch && *ch) snprintf(out[k++], PATH_MAX, "%s", ch); else snprintf(out[k++], PATH_MAX, "%s/.config", h);
	char list[2048];
	snprintf(list, sizeof list, "%s", getenv("XDG_CONFIG_DIRS") && *getenv("XDG_CONFIG_DIRS") ? getenv("XDG_CONFIG_DIRS") : "/etc/xdg");
	char *save = NULL;
	for (char *d = strtok_r(list, ":", &save); d && k < max; d = strtok_r(NULL, ":", &save)) snprintf(out[k++], PATH_MAX, "%s", d);
	if (k < max) { if (dh && *dh) snprintf(out[k++], PATH_MAX, "%s/applications", dh); else snprintf(out[k++], PATH_MAX, "%s/.local/share/applications", h); }
	snprintf(list, sizeof list, "%s", getenv("XDG_DATA_DIRS") && *getenv("XDG_DATA_DIRS") ? getenv("XDG_DATA_DIRS") : "/usr/local/share:/usr/share");
	save = NULL;
	for (char *d = strtok_r(list, ":", &save); d && k < max; d = strtok_r(NULL, ":", &save)) snprintf(out[k++], PATH_MAX, "%s/applications", d);
	return k;
}

/* 沒有 mimeinfo.cache（系統沒跑過 update-desktop-database）：直接看每個 .desktop 的 MimeType=，
 * Pomelo 自己的程式優先（跟 xdg-open 的最後手段一樣） */
static int scan_desktop_files(const char *mime, char *out, size_t n)
{
	int napps, found = 0;
	PmApp *v = pm_apps(&napps);
	size_t ml = strlen(mime);
	for (int pass = 0; pass < 2 && !found; pass++)
		for (int i = 0; i < napps && !found; i++) {
			if (!v[i].path || (pass == 0) != !strncmp(v[i].id, "pomelo-", 7)) continue;
			FILE *f = fopen(v[i].path, "r");
			if (!f) continue;
			char line[4096];
			while (!found && fgets(line, sizeof line, f)) {
				if (strncmp(line, "MimeType=", 9)) continue;
				for (char *q = line + 9; (q = strstr(q, mime)); q += ml)
					if ((q == line + 9 || q[-1] == ';') && (q[ml] == ';' || q[ml] == '\n' || !q[ml])) {
						snprintf(out, n, "%s", v[i].id);
						found = 1;
						break;
					}
			}
			fclose(f);
		}
	return found;
}

static int default_for(const char *mime, char *out, size_t n)
{
	char d[16][PATH_MAX], p[PATH_MAX + 32];
	int nd = dirs(d, 16);
	static const char *names[] = { "pomelo-mimeapps.list", "mimeapps.list" };
	for (int i = 0; i < nd; i++)
		for (int j = 0; j < 2; j++) {
			snprintf(p, sizeof p, "%s/%s", d[i], names[j]);
			if (lookup(p, "[Default Applications]", mime, out, n)) return 1;
		}
	for (int i = 0; i < nd; i++) {   /* Debian／Ubuntu 的舊格式 */
		snprintf(p, sizeof p, "%s/defaults.list", d[i]);
		if (lookup(p, "[Default Applications]", mime, out, n)) return 1;
	}
	for (int i = 0; i < nd; i++) {
		snprintf(p, sizeof p, "%s/mimeapps.list", d[i]);
		if (lookup(p, "[Added Associations]", mime, out, n)) return 1;
	}
	for (int i = 0; i < nd; i++) {
		snprintf(p, sizeof p, "%s/mimeinfo.cache", d[i]);
		if (lookup(p, "[MIME Cache]", mime, out, n)) return 1;
	}
	return scan_desktop_files(mime, out, n);
}

/* 找不到就退到上一層類型：mime 的 subclasses 表，最後 text/ 開頭的退到 text/plain */
int pm_default_app(const char *mime, char *out, size_t n)
{
	char cur[256];
	snprintf(cur, sizeof cur, "%s", mime);
	for (int depth = 0; depth < 4; depth++) {
		if (default_for(cur, out, n)) return 1;
		char parent[256] = "", line[512];
		FILE *f = fopen("/usr/share/mime/subclasses", "r");
		size_t cl = strlen(cur);
		while (f && fgets(line, sizeof line, f))
			if (!strncmp(line, cur, cl) && line[cl] == ' ') {
				line[strcspn(line, "\n")] = 0;
				snprintf(parent, sizeof parent, "%s", line + cl + 1);
				break;
			}
		if (f) fclose(f);
		if (!parent[0]) {
			if (!strncmp(cur, "text/", 5) && strcmp(cur, "text/plain")) snprintf(parent, sizeof parent, "text/plain");
			else return 0;
		}
		snprintf(cur, sizeof cur, "%s", parent);
	}
	return 0;
}

/* ---- 執行 ---- */

/* .desktop 的原始 Exec（PmApp 裡的已經拿掉 %f 這些代號了） */
static int raw_exec(const PmApp *a, char *out, size_t n)
{
	FILE *f = fopen(a->path, "r");
	if (!f) return 0;
	char line[4096];
	int in = 0, found = 0;
	while (!found && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') { in = !strcmp(line, "[Desktop Entry]"); continue; }
		if (in && !strncmp(line, "Exec=", 5)) { snprintf(out, n, "%s", line + 5); found = 1; }
	}
	fclose(f);
	return found;
}

/* Exec 切成參數（雙引號、反斜線跳脫照規範），%f %F %u %U 換成要開的東西；沒有代號就加在最後 */
static int build_argv(const char *exec, const char *file, const char *url, char **argv, int max, char *buf, size_t bn)
{
	int argc = 0, used = 0;
	size_t o = 0;
	const char *p = exec;
	while (*p && argc < max - 2) {
		while (*p == ' ') p++;
		if (!*p) break;
		size_t start = o;
		int quoted = 0;
		while (*p && (quoted || *p != ' ') && o < bn - 2) {
			if (*p == '"') { quoted = !quoted; p++; continue; }
			if (quoted && *p == '\\' && p[1]) { buf[o++] = p[1]; p += 2; continue; }
			if (*p == '%' && p[1]) {
				char c = p[1];
				p += 2;
				const char *rep = NULL;
				if (c == 'f' || c == 'F') rep = file ? file : url;
				else if (c == 'u' || c == 'U') rep = url ? url : file;
				else if (c == '%') rep = "%";
				if (rep) {
					if (c != '%') used = 1;
					size_t l = strlen(rep);
					if (o + l >= bn - 2) break;
					memcpy(buf + o, rep, l);
					o += l;
				}
				continue;   /* %i %c %k 這些不用 */
			}
			buf[o++] = *p++;
		}
		buf[o++] = 0;
		if (o - start > 1) argv[argc++] = buf + start;
	}
	if (!used && argc < max - 1 && o + strlen(file ? file : url) + 1 < bn) {
		argv[argc++] = strcpy(buf + o, file ? file : url);
	}
	argv[argc] = NULL;
	return argc;
}

static int spawn_detached(char **argv)
{
	if (getenv("POMELO_OPEN_DRYRUN")) {   /* 測試用：只印出會執行什麼 */
		for (int i = 0; argv[i]; i++) printf("%s%s", i ? " " : "", argv[i]);
		putchar('\n');
		return 0;
	}
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) {
		if (fork() == 0) {
			setsid();
			execvp(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(p, NULL, 0);
	return 0;
}

/* 打開檔案、資料夾或網址；回傳 0 = 交給某個程式了 */
int pm_open(const char *target)
{
	if (!target || !*target) return -1;
	char mime[256], url[PATH_MAX * 3 + 16], file[PATH_MAX];
	const char *f = NULL, *u = NULL;
	/* 網址：scheme 是字母開頭、只有字母數字＋-. */
	const char *colon = strchr(target, ':');
	int is_url = colon && colon > target && isalpha((unsigned char)target[0]);
	for (const char *q = target; is_url && q < colon; q++)
		if (!isalnum((unsigned char)*q) && *q != '+' && *q != '-' && *q != '.') is_url = 0;
	if (is_url && !strncasecmp(target, "file:", 5)) {   /* file:///路徑 → 一般檔案（%xx 解碼） */
		const char *s = target + 5;
		if (!strncmp(s, "//", 2)) { s += 2; s = strchr(s, '/') ? strchr(s, '/') : s; }
		size_t k = 0;
		for (; *s && k < sizeof file - 1; s++) {
			if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
				char hx[3] = { s[1], s[2], 0 };
				file[k++] = (char)strtol(hx, NULL, 16);
				s += 2;
			} else file[k++] = *s;
		}
		file[k] = 0;
		is_url = 0;
		f = file;
	} else if (is_url) {
		char scheme[32];
		size_t sl = (size_t)(colon - target);
		if (sl >= sizeof scheme) sl = sizeof scheme - 1;
		for (size_t i = 0; i < sl; i++) scheme[i] = (char)tolower((unsigned char)target[i]);
		scheme[sl] = 0;
		snprintf(mime, sizeof mime, "x-scheme-handler/%s", scheme);
		u = target;
	} else {
		f = target;
	}
	if (f) {
		file_mime(f, mime, sizeof mime);
		/* %u 要的是網址：file:// 加上路徑（不做百分比編碼，程式都看得懂一般路徑） */
		char abs[PATH_MAX];
		if (f[0] != '/' && getcwd(abs, sizeof abs)) {
			size_t l = strlen(abs);
			snprintf(abs + l, sizeof abs - l, "/%s", f);
			snprintf(file, sizeof file, "%s", abs);
			f = file;
		}
		snprintf(url, sizeof url, "file://%s", f);
		u = url;
	}
	char id[256];
	PmApp *a = pm_default_app(mime, id, sizeof id) ? pm_app_by_id(id) : NULL;
	char exec[4096];
	if (a && raw_exec(a, exec, sizeof exec)) {
		char *argv[66], buf[8192];
		int argc = 0;
		if (a->terminal) { argv[argc++] = "pomelo-terminal"; argv[argc++] = "-e"; }
		if (build_argv(exec, is_url ? NULL : f, u, argv + argc, 64, buf, sizeof buf) > 0) return spawn_detached(argv);
	}
	if (pm_have("xdg-open")) {   /* 我們找不到，裝了 xdg-utils 的話讓它試試 */
		char *argv[] = { "xdg-open", (char *)target, NULL };
		return spawn_detached(argv);
	}
	return -1;
}
