/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sbom.c — 套件清單（SBOM）與已知漏洞（CVE）比對
 *
 * 清單：dpkg 或 rpm 裝了哪些套件：名稱、版本、來源套件、授權、一句說明。deb 的授權從 /usr/share/doc/套件/copyright 讀
 * （機器可讀格式的 License: 欄位；舊格式用關鍵字猜），rpm 直接有 License 欄位。
 *
 * 漏洞資料：使用者按「更新漏洞資料」時才下載各發行版自己公開的清單（Debian 的 debsecan 資料、Ubuntu 的 OVAL、
 * Rocky／AlmaLinux 的 OSV），整份存到本機再比對——電腦上裝了什麼不會送出去。Fedora 沒有逐套件的公開資料，
 * 改用 dnf 的 updateinfo（只知道「有更新可裝」的那些）。各種來源都先轉成同一種文字格式（vuln.tsv）：
 *   # pomelo-vuln 1<TAB>src=來源<TAB>date=時間<TAB>dist=發行版
 *   C<TAB>CVE 編號<TAB>嚴重度<TAB>說明                         （每個漏洞一行）
 *   s｜b<TAB>套件<TAB>CVE 編號<TAB>修正版本<TAB>其他已修正版本<TAB>旗標   （s＝來源套件名、b＝二進位套件名；旗標 F 有更新可裝、P 要 Ubuntu Pro）
 * 比對：裝的版本 < 修正版本（而且不在「其他已修正版本」裡）才算有漏洞；沒有修正版本＝尚未修補。
 * 版本比較照 dpkg 與 rpm 的規則自己實作（不用每個套件叫一次外部程式）。
 *
 * 測試：POMELO_VULN_DATA＝vuln.tsv 路徑、POMELO_VULN_URL＝本機檔案路徑（代替下載）、POMELO_SBOM_LIST＝假的套件清單
 * （dpkg-query 的輸出格式）。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <zlib.h>
#include "pomelo.h"
#include "pomelo-check.h"
#include "check-int.h"
#include "internal.h"

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---------- 版本比較 ---------- */

static int deb_order(int c)
{
	if (isdigit(c)) return 0;
	if (isalpha(c)) return c;
	if (c == '~') return -1;
	if (c) return c + 256;
	return 0;
}

/* dpkg 的 verrevcmp */
static int deb_frag(const char *a, const char *b)
{
	while (*a || *b) {
		int first = 0;
		while ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
			int ac = deb_order((unsigned char)*a), bc = deb_order((unsigned char)*b);
			if (ac != bc) return ac - bc;
			a++; b++;
		}
		while (*a == '0') a++;
		while (*b == '0') b++;
		while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			if (!first) first = *a - *b;
			a++; b++;
		}
		if (isdigit((unsigned char)*a)) return 1;
		if (isdigit((unsigned char)*b)) return -1;
		if (first) return first;
	}
	return 0;
}

/* dpkg --compare-versions：epoch:upstream-revision */
static int deb_vercmp(const char *a, const char *b)
{
	long ea = 0, eb = 0;
	const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
	if (ca) { ea = atol(a); a = ca + 1; }
	if (cb) { eb = atol(b); b = cb + 1; }
	if (ea != eb) return ea < eb ? -1 : 1;
	char ua[200], ub[200], *ra, *rb;
	snprintf(ua, sizeof ua, "%s", a);
	snprintf(ub, sizeof ub, "%s", b);
	ra = strrchr(ua, '-'); if (ra) *ra++ = 0; else ra = "";
	rb = strrchr(ub, '-'); if (rb) *rb++ = 0; else rb = "";
	int r = deb_frag(ua, ub);
	if (r) return r;
	return deb_frag(ra, rb);
}

/* rpmvercmp */
static int rpm_frag(const char *a, const char *b)
{
	if (!strcmp(a, b)) return 0;
	char *one = strdup(a), *two = strdup(b), *p = one, *q = two;
	int rc = 0, decided = 0;
	while ((*p || *q) && !decided) {
		while (*p && !isalnum((unsigned char)*p) && *p != '~' && *p != '^') p++;
		while (*q && !isalnum((unsigned char)*q) && *q != '~' && *q != '^') q++;
		if (*p == '~' || *q == '~') {
			if (*p != '~') { rc = 1; break; }
			if (*q != '~') { rc = -1; break; }
			p++; q++;
			continue;
		}
		if (*p == '^' || *q == '^') {
			if (!*p) { rc = -1; break; }
			if (!*q) { rc = 1; break; }
			if (*p != '^') { rc = 1; break; }
			if (*q != '^') { rc = -1; break; }
			p++; q++;
			continue;
		}
		if (!(*p && *q)) break;
		char *s1 = p, *s2 = q;
		int isnum = isdigit((unsigned char)*s1);
		if (isnum) { while (isdigit((unsigned char)*p)) p++; while (isdigit((unsigned char)*q)) q++; }
		else { while (isalpha((unsigned char)*p)) p++; while (isalpha((unsigned char)*q)) q++; }
		char c1 = *p, c2 = *q;
		*p = 0; *q = 0;
		if (s2 == q) { rc = isnum ? 1 : -1; decided = 1; }
		else if (isnum) {
			while (*s1 == '0') s1++;
			while (*s2 == '0') s2++;
			size_t l1 = strlen(s1), l2 = strlen(s2);
			if (l1 != l2) { rc = l1 > l2 ? 1 : -1; decided = 1; }
		}
		if (!decided) {
			int c = strcmp(s1, s2);
			if (c) { rc = c < 0 ? -1 : 1; decided = 1; }
		}
		*p = c1; *q = c2;
	}
	if (!decided && !rc) rc = (!*p && !*q) ? 0 : !*p ? -1 : 1;
	free(one); free(two);
	return rc;
}

/* epoch:version-release（epoch 可省略＝0） */
static int rpm_vercmp(const char *a, const char *b)
{
	long ea = 0, eb = 0;
	const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
	if (ca) { ea = atol(a); a = ca + 1; }
	if (cb) { eb = atol(b); b = cb + 1; }
	if (ea != eb) return ea < eb ? -1 : 1;
	char va[200], vb[200], *ra, *rb;
	snprintf(va, sizeof va, "%s", a);
	snprintf(vb, sizeof vb, "%s", b);
	ra = strrchr(va, '-'); if (ra) *ra++ = 0; else ra = "";
	rb = strrchr(vb, '-'); if (rb) *rb++ = 0; else rb = "";
	int r = rpm_frag(va, vb);
	if (r) return r;
	return rpm_frag(ra, rb);
}

int pm_vercmp(int kind, const char *a, const char *b)
{
	return kind == PM_PKG_DNF ? rpm_vercmp(a, b) : deb_vercmp(a, b);
}

/* ---------- 套件清單 ---------- */

static void utf8_trim(char *s);

static void set(char *dst, size_t n, const char *s)
{
	snprintf(dst, n, "%s", s ? s : "");
	for (char *p = dst; *p; p++) if (*p == '\t' || *p == '\n') *p = ' ';
	utf8_trim(dst);
}

/* deb 的授權：/usr/share/doc/套件/copyright */
static void deb_license(const char *pkg, char *out, size_t n)
{
	char path[300];
	snprintf(path, sizeof path, "%s/usr/share/doc/%s/copyright", getenv("POMELO_CHECK_ROOT") ? getenv("POMELO_CHECK_ROOT") : "", pkg);
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (!f) return;
	char line[512], names[8][40];
	int nn = 0, dep5 = 0;
	long got = 0;
	char head[4096] = "";
	while (fgets(line, sizeof line, f)) {
		got += (long)strlen(line);
		if (got < (long)sizeof head - 1) strcat(head, line);
		if (!strncmp(line, "Format:", 7) && strstr(line, "copyright-format")) dep5 = 1;
		if (dep5 && !strncmp(line, "License:", 8)) {
			char *v = line + 8;
			while (*v == ' ' || *v == '\t') v++;
			v[strcspn(v, "\n")] = 0;
			/* 只要名稱（「GPL-2+ with Font exception」保留，太長的切掉） */
			if (!*v || strlen(v) > 38) continue;
			int dup = 0;
			for (int i = 0; i < nn; i++) if (!strcasecmp(names[i], v)) dup = 1;
			if (!dup && nn < 8) snprintf(names[nn++], sizeof names[0], "%s", v);
		}
		if (got > 200000) break;
	}
	fclose(f);
	if (nn) {
		/* 主要的授權放前面：通常檔案裡第一個「Files: *」的就是 */
		for (int i = 0; i < nn && i < 3; i++) {
			if (i) strncat(out, ", ", n - strlen(out) - 1);
			strncat(out, names[i], n - strlen(out) - 1);
		}
		if (nn > 3) strncat(out, "…", n - strlen(out) - 1);
		return;
	}
	/* 舊格式：用關鍵字猜 */
	struct { const char *key, *name; } guess[] = {
		{ "GNU Lesser General Public", "LGPL" }, { "GNU Library General Public", "LGPL-2" },
		{ "GNU Affero General Public", "AGPL-3" }, { "GNU General Public License", "GPL" },
		{ "Apache License", "Apache-2.0" }, { "Mozilla Public License", "MPL-2.0" },
		{ "Permission is hereby granted, free of charge", "MIT" }, { "MIT License", "MIT" },
		{ "Redistribution and use in source and binary forms", "BSD" },
		{ "Boost Software License", "BSL-1.0" }, { "public domain", "public-domain" }, { "Public Domain", "public-domain" },
	};
	for (size_t i = 0; i < LEN(guess); i++) {
		const char *at = strstr(head, guess[i].key);
		if (!at) continue;
		snprintf(out, n, "%s", guess[i].name);
		if (!strcmp(guess[i].name, "GPL") || !strcmp(guess[i].name, "LGPL")) {
			if (strstr(at, "version 3") || strstr(at, "Version 3")) strncat(out, "-3", n - strlen(out) - 1);
			else if (strstr(at, "version 2") || strstr(at, "Version 2")) strncat(out, "-2", n - strlen(out) - 1);
		}
		return;
	}
}

static int cmp_pkg(const void *a, const void *b) { return strcmp(((const PmPkg *)a)->name, ((const PmPkg *)b)->name); }

static PmPkg *grow(PmSbom *s)
{
	if (s->n % 512 == 0) {
		PmPkg *np = realloc(s->p, (s->n + 512) * sizeof *np);
		if (!np) return NULL;
		s->p = np;
	}
	PmPkg *p = &s->p[s->n++];
	memset(p, 0, sizeof *p);
	return p;
}

PmSbom *pm_sbom_load(void)
{
	PmSbom *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->kind = pm_pkg_kind();
	char os[256] = "";
	if (chk_read("/etc/os-release", os, sizeof os)) {
		const char *p = strstr(os, "PRETTY_NAME=");
		if (p) {
			p += 12;
			if (*p == '"') p++;
			size_t k = strcspn(p, "\"\n");
			snprintf(s->os, sizeof s->os, "%.*s", (int)k, p);
		}
	}
	const char *fake = getenv("POMELO_SBOM_LIST");
	char *out;
	if (fake) {
		FILE *f = fopen(fake, "r");
		out = NULL;
		if (f) {
			fseek(f, 0, SEEK_END);
			long k = ftell(f);
			fseek(f, 0, SEEK_SET);
			out = malloc(k + 1);
			if (out) { size_t got = fread(out, 1, k, f); out[got] = 0; }
			fclose(f);
		}
	} else if (s->kind == PM_PKG_DNF) {
		out = pm_run("rpm -qa --qf '%{NAME}\\t%|EPOCH?{%{EPOCH}}:{0}|:%{VERSION}-%{RELEASE}\\t%{SOURCERPM}\\t%{ARCH}\\t%{LICENSE}\\t%{SUMMARY}\\n' 2>/dev/null");
	} else {
		/* 只要真的裝著的（移除但設定檔還在的「rc」不算） */
		out = pm_run("dpkg-query -W -f '${Package}\\t${Version}\\t${source:Package}\\t${Architecture}\\t\\t${binary:Summary}\\t${source:Version}\\t${db:Status-Abbrev}\\n' 2>/dev/null");
	}
	if (!out) return s;
	for (char *l = out, *nx; l && *l; l = nx) {
		nx = chk_line(l);
		char *f[8] = { 0 };
		int k = 0;
		for (char *t = l; t && k < 8; k++) { f[k] = t; t = strchr(t, '\t'); if (t) *t++ = 0; }
		if (k < 2 || !f[0][0]) continue;
		if (k > 7 && f[7][0] && strncmp(f[7], "ii", 2) && strncmp(f[7], "hi", 2)) continue;   /* dpkg 狀態不是 installed */
		PmPkg *p = grow(s);
		if (!p) break;
		set(p->name, sizeof p->name, f[0]);
		set(p->version, sizeof p->version, f[1]);
		set(p->source, sizeof p->source, f[2]);
		set(p->arch, sizeof p->arch, f[3]);
		set(p->license, sizeof p->license, f[4]);
		set(p->summary, sizeof p->summary, f[5]);
		set(p->srcver, sizeof p->srcver, f[6]);
		if (!p->srcver[0]) snprintf(p->srcver, sizeof p->srcver, "%s", p->version);
		if (s->kind == PM_PKG_DNF) {
			/* SOURCERPM：name-version-release.src.rpm → name */
			char *e = strstr(p->source, ".src.rpm");
			if (e) *e = 0;
			char *d = strrchr(p->source, '-');
			if (d) { *d = 0; d = strrchr(p->source, '-'); if (d) { *d = 0; snprintf(p->srcver, sizeof p->srcver, "%s-%s", d + 1, d + strlen(d + 1) + 2); } }
		} else {
			/* 「glibc (2.39-0ubuntu8)」→ glibc */
			char *sp = strchr(p->source, ' ');
			if (sp) *sp = 0;
			if (!p->source[0]) snprintf(p->source, sizeof p->source, "%s", p->name);
			if (!fake) deb_license(p->name, p->license, sizeof p->license);
		}
	}
	free(out);
	qsort(s->p, s->n, sizeof *s->p, cmp_pkg);
	return s;
}

void pm_sbom_free(PmSbom *s)
{
	if (!s) return;
	for (int i = 0; i < s->n; i++) free(s->p[i].v);
	free(s->p);
	free(s);
}

/* ---------- 漏洞資料檔 ---------- */

static const char *data_path(char *buf, size_t n)
{
	const char *e = getenv("POMELO_VULN_DATA");
	if (e && *e) { snprintf(buf, n, "%s", e); return buf; }
	const char *c = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
	if (c && *c) snprintf(buf, n, "%s/pomelo/vuln.tsv", c);
	else snprintf(buf, n, "%s/.cache/pomelo/vuln.tsv", h ? h : "/tmp");
	return buf;
}

static void os_field(const char *key, char *out, size_t n)
{
	char os[1024] = "";
	out[0] = 0;
	if (!chk_read("/etc/os-release", os, sizeof os)) return;
	for (char *l = os, *nx; l && *l; l = nx) {
		nx = chk_line(l);
		size_t kl = strlen(key);
		if (strncmp(l, key, kl) || l[kl] != '=') continue;
		const char *v = l + kl + 1;
		if (*v == '"') v++;
		size_t k = strcspn(v, "\"");
		snprintf(out, n, "%.*s", (int)k, v);
		return;
	}
}

/* 這個發行版的漏洞資料從哪裡來；回傳 PM_VULN_SRC_*，url／label 可為 NULL */
int pm_sbom_source(char *url, size_t un, char *label, size_t ln, long *size_mb)
{
	char id[64], code[64], ver[32];
	os_field("ID", id, sizeof id);
	os_field("VERSION_CODENAME", code, sizeof code);
	os_field("VERSION_ID", ver, sizeof ver);
	int type = PM_VULN_SRC_NONE;
	char u[300] = "", l[160] = "";
	long mb = 0;
	if (!strcmp(id, "debian")) {
		type = PM_VULN_SRC_DEBSECAN;
		snprintf(u, sizeof u, "https://security-tracker.debian.org/tracker/debsecan/release/1/%s", code[0] ? code : "GENERIC");
		snprintf(l, sizeof l, "Debian Security Tracker（%s）", code[0] ? code : "GENERIC");
		mb = 6;
	} else if (!strcmp(id, "ubuntu") && code[0]) {
		type = PM_VULN_SRC_OVAL;
		snprintf(u, sizeof u, "https://security-metadata.canonical.com/oval/com.ubuntu.%s.cve.oval.xml.bz2", code);
		snprintf(l, sizeof l, "Ubuntu Security OVAL（%s）", code);
		mb = 11;
	} else if (!strcmp(id, "rocky") || !strcmp(id, "almalinux")) {
		type = PM_VULN_SRC_OSV;
		snprintf(u, sizeof u, "https://osv-vulnerabilities.storage.googleapis.com/%s/all.zip", !strcmp(id, "rocky") ? "Rocky%20Linux" : "AlmaLinux");
		snprintf(l, sizeof l, "OSV（%s %d）", !strcmp(id, "rocky") ? "Rocky Linux" : "AlmaLinux", atoi(ver));
		mb = 6;
	} else if (pm_pkg_kind() == PM_PKG_DNF) {
		type = PM_VULN_SRC_DNF;
		snprintf(l, sizeof l, "dnf updateinfo");
	}
	if (url) snprintf(url, un, "%s", u);
	if (label) snprintf(label, ln, "%s", l);
	if (size_mb) *size_mb = mb;
	return type;
}

int pm_sbom_data_info(time_t *when, char *src, size_t n)
{
	char path[600];
	FILE *f = fopen(data_path(path, sizeof path), "r");
	if (when) *when = 0;
	if (src) src[0] = 0;
	if (!f) return 0;
	char line[600] = "";
	if (!fgets(line, sizeof line, f) || strncmp(line, "# pomelo-vuln 1", 15)) { fclose(f); return 0; }
	fclose(f);
	const char *d = strstr(line, "date="), *s = strstr(line, "src=");
	if (when && d) *when = (time_t)atol(d + 5);
	if (src && s) { size_t k = strcspn(s + 4, "\t\n"); snprintf(src, n, "%.*s", (int)k, s + 4); }
	return 1;
}

/* ---------- 轉換：各種來源 → vuln.tsv ---------- */

static int sev_from(const char *s)
{
	if (!s || !*s) return 0;
	if (!strncasecmp(s, "crit", 4)) return 4;
	if (!strncasecmp(s, "high", 4) || !strncasecmp(s, "import", 6) || *s == 'H') return 3;
	if (!strncasecmp(s, "med", 3) || !strncasecmp(s, "moder", 5) || *s == 'M') return 2;
	if (!strncasecmp(s, "low", 3) || !strncasecmp(s, "negl", 4) || *s == 'L') return 1;
	return 0;
}

static void tsv_clean(char *s)
{
	for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

/* snprintf 截斷可能切在 UTF-8 多位元組字元中間：把不完整的尾巴拿掉 */
static void utf8_trim(char *s)
{
	size_t n = strlen(s), i = n;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80 && n - i < 3) i--;   /* 跳過結尾的接續位元組 */
	if (i == 0) { if (n < 4) s[0] = 0; return; }
	unsigned char lead = (unsigned char)s[i - 1];
	size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (lead >= 0xC0 && n - i + 1 < need) s[i - 1] = 0;
}

/* 文字裡的 \uXXXX（debsecan 的說明有）→ UTF-8 */
static void unescape_u(char *s)
{
	char *o = s;
	while (*s) {
		if (s[0] == '\\' && s[1] == 'u' && isxdigit((unsigned char)s[2]) && isxdigit((unsigned char)s[3]) && isxdigit((unsigned char)s[4]) && isxdigit((unsigned char)s[5])) {
			unsigned c = (unsigned)strtoul((char[]){ s[2], s[3], s[4], s[5], 0 }, NULL, 16);
			if (c < 0x80) *o++ = (char)c;
			else if (c < 0x800) { *o++ = (char)(0xC0 | (c >> 6)); *o++ = (char)(0x80 | (c & 0x3F)); }
			else { *o++ = (char)(0xE0 | (c >> 12)); *o++ = (char)(0x80 | ((c >> 6) & 0x3F)); *o++ = (char)(0x80 | (c & 0x3F)); }
			s += 6;
			continue;
		}
		*o++ = *s++;
	}
	*o = 0;
}

/* XML 的 &amp; &lt; &gt; &quot; &apos; 還原 */
static void xml_unescape(char *s)
{
	char *o = s;
	for (; *s; ) {
		if (*s == '&') {
			if (!strncmp(s, "&amp;", 5)) { *o++ = '&'; s += 5; continue; }
			if (!strncmp(s, "&lt;", 4)) { *o++ = '<'; s += 4; continue; }
			if (!strncmp(s, "&gt;", 4)) { *o++ = '>'; s += 4; continue; }
			if (!strncmp(s, "&quot;", 6)) { *o++ = '"'; s += 6; continue; }
			if (!strncmp(s, "&apos;", 6)) { *o++ = '\''; s += 6; continue; }
		}
		*o++ = *s++;
	}
	*o = 0;
}

static void write_header(FILE *o, const char *src, const char *dist)
{
	fprintf(o, "# pomelo-vuln 1\tsrc=%s\tdate=%ld\tdist=%s\n", src, (long)time(NULL), dist);
}

/* Debian debsecan：zlib 壓縮的文字，三種行：「CVE,旗標,說明」（依出現順序編號）、「來源套件,編號,旗標,修正版,其他已修正版本」
 * （旗標：第 1 字 B＝二進位套件名、第 2 字 L/M/H 緊急度、第 4 字 F＝修正已經可以裝）、「來源套件,二進位套件…」（不用） */
static int import_debsecan(const char *in, FILE *o, const char *label, char *err, size_t en)
{
	FILE *f = fopen(in, "rb");
	if (!f) { snprintf(err, en, "%s", _("讀不到下載的檔案")); return -1; }
	fseek(f, 0, SEEK_END);
	long zn = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *z = malloc(zn > 0 ? zn : 1);
	if (!z || fread(z, 1, zn, f) != (size_t)zn) { fclose(f); free(z); snprintf(err, en, "%s", _("讀不到下載的檔案")); return -1; }
	fclose(f);
	uLongf cap = (uLongf)zn * 8 + 1024;
	char *txt = NULL;
	for (int tries = 0; tries < 6; tries++) {
		char *nt = realloc(txt, cap);
		if (!nt) { free(txt); txt = NULL; break; }
		txt = nt;
		uLongf got = cap - 1;
		int r = uncompress((Bytef *)txt, &got, z, zn);
		if (r == Z_OK) { txt[got] = 0; break; }
		if (r != Z_BUF_ERROR) { free(txt); txt = NULL; break; }
		cap *= 2;
	}
	free(z);
	if (!txt || strncmp(txt, "VERSION 1", 9)) { free(txt); snprintf(err, en, "%s", _("下載的檔案格式不對")); return -1; }
	write_header(o, label, "debian");
	char **ids = NULL;
	int nid = 0, cap_id = 0, n = 0;
	for (char *l = txt, *nx; l && *l; l = nx) {
		nx = chk_line(l);
		if (!strncmp(l, "VERSION", 7)) continue;
		char *c1 = strchr(l, ',');
		if (!c1) continue;
		*c1 = 0;
		char *rest = c1 + 1;
		if (!strncmp(l, "CVE-", 4) || !strncmp(l, "DSA-", 4) || !strncmp(l, "DLA-", 4) || !strncmp(l, "TEMP-", 5)) {
			char *c2 = strchr(rest, ',');
			if (!c2) continue;
			char *desc = c2 + 1;
			if (nid >= cap_id) {
				cap_id = cap_id ? cap_id * 2 : 4096;
				char **ni = realloc(ids, cap_id * sizeof *ids);
				if (!ni) break;
				ids = ni;
			}
			ids[nid++] = l;
			tsv_clean(desc);
			unescape_u(desc);
			fprintf(o, "C\t%s\t0\t%s\n", l, desc);
			continue;
		}
		/* 套件行：編號,旗標,修正版,其他版本（二進位套件清單行只有兩欄，跳過） */
		char *f4[4] = { 0 };
		int k = 0;
		for (char *t = rest; t && k < 4; k++) { f4[k] = t; t = k < 3 ? strchr(t, ',') : NULL; if (t) *t++ = 0; }
		if (k < 4 || !isdigit((unsigned char)f4[0][0])) continue;
		int idx = atoi(f4[0]);
		if (idx < 0 || idx >= nid) continue;
		const char *flags = f4[1];
		char kind = flags[0] == 'B' ? 'b' : 's';
		int sev = flags[1] == 'L' ? 1 : flags[1] == 'M' ? 2 : flags[1] == 'H' ? 3 : 0;
		fprintf(o, "%c\t%s\t%s\t%s\t%s\t%s\t%d\n", kind, l, ids[idx], f4[2], f4[3], strlen(flags) > 3 && flags[3] == 'F' ? "F" : "", sev);
		n++;
	}
	free(ids);
	free(txt);
	return n;
}

/* Ubuntu OVAL（bzip2 的 XML，解開有兩百多 MB）：用 libbz2 串流解壓（執行期載入，不用建置時的標頭），
 * 只看 <definition> 區塊裡的 CVE、priority、說明與 criterion 的 comment：
 *   「PKG source package in noble, is affected and has been fixed (note: '版本')」
 *   「PKG source package in esm-apps/noble, …」（修正要 Ubuntu Pro）
 *   「PKG source package in noble, might be affected and may need fixing」（還沒修）
 *   「'linux-xxx' kernel in noble was vulnerable but has been fixed (note: '版本')」 */
typedef struct {
	char *next_in; unsigned avail_in, total_in_lo32, total_in_hi32;
	char *next_out; unsigned avail_out, total_out_lo32, total_out_hi32;
	void *state; void *(*bzalloc)(void *, int, int); void (*bzfree)(void *, void *); void *opaque;
} bz_stream_t;

static int oval_definition(char *d, FILE *o, int *ncve)
{
	char *ref = strstr(d, "ref_id=\"");
	if (!ref) return 0;
	ref += 8;
	char cve[48];
	snprintf(cve, sizeof cve, "%.*s", (int)strcspn(ref, "\""), ref);
	if (strncmp(cve, "CVE-", 4)) return 0;
	const char *pr = strstr(d, "priority=\"");
	int sev = pr ? sev_from(pr + 10) : 0;
	char desc[240] = "";
	char *ds = strstr(d, "<description>");
	if (ds) {
		ds += 13;
		snprintf(desc, sizeof desc, "%.*s", (int)strcspn(ds, "<"), ds);
		utf8_trim(desc);
		xml_unescape(desc);
		tsv_clean(desc);
		char *ui = strstr(desc, " Update Instructions:");   /* Ubuntu 附的「sudo pro fix」說明，不用 */
		if (!ui) ui = strstr(desc, "Update Instructions:");
		if (ui) *ui = 0;
	}
	fprintf(o, "C\t%s\t%d\t%s\n", cve, sev, desc);
	(*ncve)++;
	int n = 0;
	for (char *c = strstr(d, "comment=\""); c; c = strstr(c, "comment=\"")) {
		c += 9;
		char *e = strchr(c, '"');
		if (!e) break;
		*e = 0;
		char pkg[128] = "", ver[96] = "", fl[4] = "";
		int ok = 0;
		if (c[0] == '\'') {
			/* 'linux-xxx' kernel in noble was vulnerable but has been fixed (note: '…') */
			char *q = strchr(c + 1, '\'');
			if (q && strstr(q, " kernel in ")) { snprintf(pkg, sizeof pkg, "%.*s", (int)(q - c - 1), c + 1); ok = 1; }
		} else {
			char *sp = strstr(c, " source package in ");
			if (sp) { snprintf(pkg, sizeof pkg, "%.*s", (int)(sp - c), c); ok = 1; }
		}
		if (ok) {
			const char *note = strstr(c, "(note: '");
			if (note) { note += 8; snprintf(ver, sizeof ver, "%.*s", (int)strcspn(note, "'"), note); }
			if (strstr(c, " esm-")) strcat(fl, "P");
			else if (ver[0]) strcat(fl, "F");
			xml_unescape(ver);
			fprintf(o, "s\t%s\t%s\t%s\t\t%s\t%d\n", pkg, cve, ver, fl, sev);
			n++;
		}
		*e = '"';
		c = e;
	}
	return n;
}

static int import_oval(const char *in, FILE *o, const char *label, const char *code, char *err, size_t en)
{
	void *lib = dlopen("libbz2.so.1", RTLD_NOW);
	if (!lib) lib = dlopen("libbz2.so.1.0", RTLD_NOW);
	if (!lib) { snprintf(err, en, "%s", _("這台電腦沒有 libbz2，沒辦法解開 Ubuntu 的漏洞資料")); return -1; }
	int (*init)(bz_stream_t *, int, int) = dlsym(lib, "BZ2_bzDecompressInit");
	int (*dec)(bz_stream_t *) = dlsym(lib, "BZ2_bzDecompress");
	int (*end)(bz_stream_t *) = dlsym(lib, "BZ2_bzDecompressEnd");
	if (!init || !dec || !end) { dlclose(lib); snprintf(err, en, "%s", _("這台電腦沒有 libbz2，沒辦法解開 Ubuntu 的漏洞資料")); return -1; }
	FILE *f = fopen(in, "rb");
	if (!f) { dlclose(lib); snprintf(err, en, "%s", _("讀不到下載的檔案")); return -1; }
	bz_stream_t bz;
	memset(&bz, 0, sizeof bz);
	if (init(&bz, 0, 0) != 0) { fclose(f); dlclose(lib); snprintf(err, en, "bzip2"); return -1; }
	write_header(o, label, code);
	size_t cap = 4 << 20, len = 0;
	char *buf = malloc(cap + 1);
	char inb[256 << 10];
	int n = 0, ncve = 0, done = 0, rc;
	while (!done) {
		if (bz.avail_in == 0) {
			size_t got = fread(inb, 1, sizeof inb, f);
			if (!got) break;
			bz.next_in = inb;
			bz.avail_in = (unsigned)got;
		}
		if (len + (1 << 20) > cap) { cap *= 2; buf = realloc(buf, cap + 1); if (!buf) break; }
		bz.next_out = buf + len;
		bz.avail_out = (unsigned)(cap - len);
		rc = dec(&bz);
		len = cap - bz.avail_out;
		buf[len] = 0;
		if (rc != 0 && rc != 4) break;
		/* 處理已經完整的 <definition …>…</definition> */
		char *p = buf;
		for (;;) {
			char *s = strstr(p, "<definition ");
			if (!s) break;
			char *e = strstr(s, "</definition>");
			if (!e) { p = s; break; }
			*e = 0;
			if (strstr(s, "class=\"vulnerability\"")) n += oval_definition(s, o, &ncve);
			p = e + 13;
		}
		if (strstr(p, "</definitions>")) { done = 1; break; }
		/* 留下還沒處理完的尾巴 */
		size_t rest = len - (p - buf);
		if (rest > (64 << 20)) break;   /* 不應該：單一 definition 不會這麼大 */
		memmove(buf, p, rest);
		len = rest;
		buf[len] = 0;
		if (rc == 4) break;
	}
	end(&bz);
	free(buf);
	fclose(f);
	dlclose(lib);
	if (!ncve) { snprintf(err, en, "%s", _("下載的檔案格式不對")); return -1; }
	return n;
}

/* ---------- OSV（zip 裡的 JSON；Rocky、AlmaLinux） ---------- */

/* 極簡 JSON：只找需要的欄位。找 key 的值（在同一層或更深都可以：OSV 的結構固定，不會搞錯） */
static const char *json_skip_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static const char *json_str_end(const char *p)   /* p 指向開頭的引號之後；回傳結尾引號的位置 */
{
	for (; *p; p++) {
		if (*p == '\\' && p[1]) { p++; continue; }
		if (*p == '"') return p;
	}
	return p;
}

/* 跳過一個值（字串、數字、物件、陣列、true/false/null），回傳值之後的位置 */
static const char *json_skip(const char *p)
{
	p = json_skip_ws(p);
	if (*p == '"') return json_str_end(p + 1) + 1;
	if (*p == '{' || *p == '[') {
		int depth = 0;
		for (; *p; p++) {
			if (*p == '"') { p = json_str_end(p + 1); continue; }
			if (*p == '{' || *p == '[') depth++;
			else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
		}
		return p;
	}
	while (*p && *p != ',' && *p != '}' && *p != ']') p++;
	return p;
}

/* 在物件 obj（指向 '{'）的第一層找 key；回傳值的位置（NULL＝沒有） */
static const char *json_get(const char *obj, const char *key)
{
	const char *p = json_skip_ws(obj);
	if (*p != '{') return NULL;
	p++;
	size_t kl = strlen(key);
	for (;;) {
		p = json_skip_ws(p);
		if (*p != '"') return NULL;
		const char *ke = json_str_end(p + 1);
		int hit = (size_t)(ke - p - 1) == kl && !strncmp(p + 1, key, kl);
		p = json_skip_ws(ke + 1);
		if (*p != ':') return NULL;
		p = json_skip_ws(p + 1);
		if (hit) return p;
		p = json_skip(p);
		p = json_skip_ws(p);
		if (*p == ',') p++;
		else return NULL;
	}
}

static void json_copy(const char *v, char *out, size_t n)
{
	out[0] = 0;
	if (!v || *v != '"') return;
	const char *e = json_str_end(v + 1);
	size_t k = 0;
	for (const char *p = v + 1; p < e && k + 1 < n; p++) {
		if (*p == '\\' && p + 1 < e) { p++; if (*p == 'n' || *p == 't') { out[k++] = ' '; continue; } }
		out[k++] = *p;
	}
	out[k] = 0;
}

/* 陣列（指向 '['）的第 i 個元素；回傳 NULL＝沒有 */
static const char *json_at(const char *arr, int i)
{
	const char *p = json_skip_ws(arr);
	if (*p != '[') return NULL;
	p = json_skip_ws(p + 1);
	if (*p == ']') return NULL;
	for (int k = 0; ; k++) {
		if (k == i) return p;
		p = json_skip_ws(json_skip(p));
		if (*p != ',') return NULL;
		p = json_skip_ws(p + 1);
	}
}

static int osv_record(const char *j, FILE *o, const char *eco, int *ncve)
{
	const char *aff = json_get(j, "affected");
	if (!aff) return 0;
	/* 這份公告有沒有我們這個版本的 */
	int mine = 0;
	for (int i = 0; ; i++) {
		const char *a = json_at(aff, i);
		if (!a) break;
		const char *pk = json_get(a, "package");
		char e[64];
		json_copy(pk ? json_get(pk, "ecosystem") : NULL, e, sizeof e);
		if (!strcmp(e, eco)) mine = 1;
	}
	if (!mine) return 0;
	char id[48], summary[240] = "", sev[16] = "";
	json_copy(json_get(j, "id"), id, sizeof id);
	json_copy(json_get(j, "summary"), summary, sizeof summary);
	utf8_trim(summary);
	tsv_clean(summary);
	/* 嚴重度：摘要開頭「Important: …」 */
	const char *col = strchr(summary, ':');
	if (col && col - summary < 12) snprintf(sev, sizeof sev, "%.*s", (int)(col - summary), summary);
	int s = sev_from(sev);
	/* CVE 編號：upstream（沒有就用公告編號） */
	char cves[16][32];
	int nc = 0;
	const char *up = json_get(j, "upstream");
	if (!up) up = json_get(j, "aliases");
	for (int i = 0; up && nc < 16; i++) {
		const char *v = json_at(up, i);
		if (!v) break;
		json_copy(v, cves[nc], sizeof cves[0]);
		if (!strncmp(cves[nc], "CVE-", 4)) nc++;
	}
	if (!nc) { snprintf(cves[0], sizeof cves[0], "%s", id); nc = 1; }
	for (int c = 0; c < nc; c++) {
		fprintf(o, "C\t%s\t%d\t%s\n", cves[c], s, summary);
		(*ncve)++;
	}
	int n = 0;
	for (int i = 0; ; i++) {
		const char *a = json_at(aff, i);
		if (!a) break;
		const char *pk = json_get(a, "package");
		char e[64], name[128], fixed[96] = "";
		json_copy(pk ? json_get(pk, "ecosystem") : NULL, e, sizeof e);
		if (strcmp(e, eco)) continue;
		json_copy(pk ? json_get(pk, "name") : NULL, name, sizeof name);
		const char *ranges = json_get(a, "ranges");
		for (int r = 0; ranges; r++) {
			const char *rg = json_at(ranges, r);
			if (!rg) break;
			const char *ev = json_get(rg, "events");
			for (int k = 0; ev; k++) {
				const char *e1 = json_at(ev, k);
				if (!e1) break;
				const char *fx = json_get(e1, "fixed");
				if (fx) json_copy(fx, fixed, sizeof fixed);
			}
		}
		for (int c = 0; c < nc; c++) {
			fprintf(o, "b\t%s\t%s\t%s\t\t%s\t%d\n", name, cves[c], fixed, fixed[0] ? "F" : "", s);
			n++;
		}
	}
	return n;
}

/* zip：從中央目錄逐個解開（stored 或 deflate） */
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static int import_osv(const char *in, FILE *o, const char *label, const char *eco, const char *dist, char *err, size_t en)
{
	FILE *f = fopen(in, "rb");
	if (!f) { snprintf(err, en, "%s", _("讀不到下載的檔案")); return -1; }
	fseek(f, 0, SEEK_END);
	long zn = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *z = malloc(zn > 0 ? zn : 1);
	if (!z || fread(z, 1, zn, f) != (size_t)zn) { fclose(f); free(z); snprintf(err, en, "%s", _("讀不到下載的檔案")); return -1; }
	fclose(f);
	/* 找 End of central directory */
	long eocd = -1;
	for (long i = zn - 22; i >= 0 && i > zn - 70000; i--)
		if (z[i] == 'P' && z[i + 1] == 'K' && z[i + 2] == 5 && z[i + 3] == 6) { eocd = i; break; }
	if (eocd < 0) { free(z); snprintf(err, en, "%s", _("下載的檔案格式不對")); return -1; }
	unsigned nent = rd16(z + eocd + 10), cd = rd32(z + eocd + 16);
	write_header(o, label, dist);
	int n = 0, ncve = 0;
	unsigned char *p = z + cd;
	char *buf = NULL;
	size_t bcap = 0;
	for (unsigned i = 0; i < nent && p + 46 <= z + zn; i++) {
		if (rd32(p) != 0x02014b50) break;
		unsigned method = rd16(p + 10), csize = rd32(p + 20), usize = rd32(p + 24);
		unsigned nl = rd16(p + 28), el = rd16(p + 30), cl = rd16(p + 32), off = rd32(p + 42);
		unsigned char *lh = z + off;
		p += 46 + nl + el + cl;
		if (lh + 30 > z + zn || rd32(lh) != 0x04034b50) continue;
		unsigned char *data = lh + 30 + rd16(lh + 26) + rd16(lh + 28);
		if (data + csize > z + zn || usize > (64u << 20)) continue;
		if (usize + 1 > bcap) { bcap = usize + 1; buf = realloc(buf, bcap); if (!buf) break; }
		if (method == 0) memcpy(buf, data, usize);
		else if (method == 8) {
			z_stream zs;
			memset(&zs, 0, sizeof zs);
			if (inflateInit2(&zs, -15) != Z_OK) continue;
			zs.next_in = data; zs.avail_in = csize;
			zs.next_out = (Bytef *)buf; zs.avail_out = usize;
			int r = inflate(&zs, Z_FINISH);
			inflateEnd(&zs);
			if (r != Z_STREAM_END && r != Z_OK) continue;
		} else continue;
		buf[usize] = 0;
		n += osv_record(buf, o, eco, &ncve);
	}
	free(buf);
	free(z);
	if (!ncve) { snprintf(err, en, "%s", _("下載的檔案格式不對")); return -1; }
	return n;
}

/* Fedora：dnf updateinfo（只有「有更新可裝」的） */
static int import_dnf(FILE *o, const char *label, char *err, size_t en)
{
	char *out = chk_run("timeout 120 dnf -q --cacheonly updateinfo list --with-cve 2>/dev/null");
	if (!out || !*out) { free(out); out = chk_run("timeout 300 dnf -q updateinfo list --with-cve 2>/dev/null"); }
	if (!out) { snprintf(err, en, "%s", _("dnf 沒有回應")); return -1; }
	write_header(o, label, "fedora");
	int n = 0;
	for (char *l = out, *nx; l && *l; l = nx) {
		nx = chk_line(l);
		/* 例如「FEDORA-2026-abc CVE-2026-1234 Important/Sec. foo-1.2-3.fc44.x86_64」或 dnf5 的表格 */
		char *cve = NULL, *sev = NULL, *nevra = NULL;
		for (char *t = strtok(l, " \t"); t; t = strtok(NULL, " \t")) {
			if (!strncmp(t, "CVE-", 4)) cve = t;
			else if (strstr(t, "/Sec")) sev = t;
			nevra = t;
		}
		if (!cve || !nevra || nevra == cve || nevra == sev) continue;
		/* name-ver-rel.arch → name、ver-rel */
		char name[128], ver[96];
		snprintf(name, sizeof name, "%s", nevra);
		char *dot = strrchr(name, '.');
		if (dot) *dot = 0;
		char *d1 = strrchr(name, '-');
		if (!d1) continue;
		*d1 = 0;
		char *d2 = strrchr(name, '-');
		if (!d2) continue;
		*d2 = 0;
		snprintf(ver, sizeof ver, "%s-%s", d2 + 1, d1 + 1);
		int s = sev ? sev_from(sev) : 0;
		fprintf(o, "C\t%s\t%d\t%s\n", cve, s, "");
		fprintf(o, "b\t%s\t%s\t%s\t\tF\t%d\n", name, cve, ver, s);
		n++;
	}
	free(out);
	return n;
}

/* 下載到暫存檔：curl，沒有就 wget */
static int download(const char *url, const char *to, char *err, size_t en)
{
	if (url[0] == '/') {   /* 測試：本機檔案 */
		char cmd[1400];
		snprintf(cmd, sizeof cmd, "cp '%s' '%s' && echo OK", url, to);
		char *r = pm_run(cmd);
		int ok = r && strstr(r, "OK");
		free(r);
		if (!ok) snprintf(err, en, "%s", _("讀不到下載的檔案"));
		return ok ? 0 : -1;
	}
	char cmd[1400];
	if (pm_have("curl")) snprintf(cmd, sizeof cmd, "curl -fsSL --max-time 900 -o '%s' '%s' && echo OK", to, url);
	else if (pm_have("wget")) snprintf(cmd, sizeof cmd, "wget -q -T 900 -O '%s' '%s' && echo OK", to, url);
	else { snprintf(err, en, "%s", _("這台電腦沒有 curl 或 wget，沒辦法下載")); return -1; }
	char *r = pm_run(cmd);
	int ok = r && strstr(r, "OK");
	free(r);
	if (!ok) { snprintf(err, en, "%s", _("下載失敗（請確認網路有通）")); unlink(to); return -1; }
	return 0;
}

int pm_sbom_update(char *err, size_t en)
{
	char url[300], label[160], code[64], id[64], ver[32];
	err[0] = 0;
	int type = pm_sbom_source(url, sizeof url, label, sizeof label, NULL);
	const char *test = getenv("POMELO_VULN_URL");
	if (test && *test) snprintf(url, sizeof url, "%s", test);
	if (type == PM_VULN_SRC_NONE) { snprintf(err, en, "%s", _("這個發行版沒有公開的漏洞資料")); return -1; }
	os_field("VERSION_CODENAME", code, sizeof code);
	os_field("ID", id, sizeof id);
	os_field("VERSION_ID", ver, sizeof ver);
	char path[600], tmp[620], dl[620];
	data_path(path, sizeof path);
	char *slash = strrchr(path, '/');
	if (slash) { *slash = 0; char mk[700]; snprintf(mk, sizeof mk, "mkdir -p '%s'", path); free(pm_run(mk)); *slash = '/'; }
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	snprintf(dl, sizeof dl, "%s.download", path);
	if (type != PM_VULN_SRC_DNF && download(url, dl, err, en)) return -1;
	FILE *o = fopen(tmp, "w");
	if (!o) { snprintf(err, en, "%s", _("沒辦法寫入快取資料夾")); unlink(dl); return -1; }
	int n = -1;
	if (type == PM_VULN_SRC_DEBSECAN) n = import_debsecan(dl, o, label, err, en);
	else if (type == PM_VULN_SRC_OVAL) n = import_oval(dl, o, label, code, err, en);
	else if (type == PM_VULN_SRC_OSV) {
		char eco[64];
		snprintf(eco, sizeof eco, "%s:%d", !strcmp(id, "rocky") ? "Rocky Linux" : "AlmaLinux", atoi(ver));
		n = import_osv(dl, o, label, eco, id, err, en);
	} else if (type == PM_VULN_SRC_DNF) n = import_dnf(o, label, err, en);
	fclose(o);
	unlink(dl);
	if (n < 0) { unlink(tmp); return -1; }
	if (rename(tmp, path)) { snprintf(err, en, "%s", _("沒辦法寫入快取資料夾")); unlink(tmp); return -1; }
	return n;
}

/* ---------- 比對 ---------- */

static PmPkg *find_pkg(PmSbom *s, const char *name)
{
	PmPkg key;
	snprintf(key.name, sizeof key.name, "%s", name);
	return bsearch(&key, s->p, s->n, sizeof *s->p, cmp_pkg);
}

typedef struct { char name[96]; int idx; } SrcIdx;
static int cmp_src(const void *a, const void *b) { return strcmp(((const SrcIdx *)a)->name, ((const SrcIdx *)b)->name); }

static void add_vuln(PmSbom *s, PmPkg *p, const char *cve, int sev, int status, const char *fixed)
{
	for (int i = 0; i < p->nv; i++) if (!strcmp(p->v[i].id, cve)) return;   /* 同一個 CVE 不重複（例如 kernel 有多個來源名） */
	if (p->nv % 8 == 0) {
		PmVuln *nv = realloc(p->v, (p->nv + 8) * sizeof *nv);
		if (!nv) return;
		p->v = nv;
	}
	PmVuln *v = &p->v[p->nv++];
	memset(v, 0, sizeof *v);
	snprintf(v->id, sizeof v->id, "%s", cve);
	v->sev = sev;
	v->status = status;
	snprintf(v->fixed, sizeof v->fixed, "%s", fixed);
	if (status == PM_VULN_FIXABLE) p->nfix++; else p->nopen++;
	if (sev > p->worst) p->worst = sev;
	(void)s;
}

static int cmp_vuln(const void *a, const void *b)
{
	const PmVuln *x = a, *y = b;
	if (x->status != y->status) return x->status == PM_VULN_FIXABLE ? -1 : y->status == PM_VULN_FIXABLE ? 1 : x->status - y->status;
	if (x->sev != y->sev) return y->sev - x->sev;
	return strcmp(y->id, x->id);   /* 新的在前 */
}

typedef struct { PmVuln *v; PmPkg *p; } VulnRef;
static int cmp_ref(const void *a, const void *b) { return strcmp(((const VulnRef *)a)->v->id, ((const VulnRef *)b)->v->id); }

int pm_sbom_match(PmSbom *s)
{
	char path[600];
	FILE *f = fopen(data_path(path, sizeof path), "r");
	if (!f) return -1;
	char line[2048];
	if (!fgets(line, sizeof line, f) || strncmp(line, "# pomelo-vuln 1", 15)) { fclose(f); return -1; }
	const char *d = strstr(line, "date="), *sr = strstr(line, "src=");
	s->data_time = d ? (time_t)atol(d + 5) : 0;
	if (sr) { size_t k = strcspn(sr + 4, "\t\n"); snprintf(s->data_src, sizeof s->data_src, "%.*s", (int)k, sr + 4); }
	for (int i = 0; i < s->n; i++) { free(s->p[i].v); s->p[i].v = NULL; s->p[i].nv = s->p[i].nfix = s->p[i].nopen = s->p[i].worst = 0; }
	/* 來源套件 → 套件 index（一個來源可能有很多二進位套件；kernel 的 linux-signed-xxx 對應 linux-xxx） */
	SrcIdx *src = malloc(s->n * 2 * sizeof *src);
	int ns = 0;
	for (int i = 0; i < s->n; i++) {
		snprintf(src[ns].name, sizeof src[ns].name, "%s", s->p[i].source);
		src[ns++].idx = i;
		if (!strncmp(s->p[i].source, "linux-signed", 12)) {
			/* Ubuntu：linux-signed-hwe-7.0 → linux-hwe-7.0；Debian：linux-signed-amd64 → linux */
			const char *rest = s->p[i].source + 12;
			static const char *archs[] = { "-amd64", "-arm64", "-i386", "-armhf", "-ppc64el", "-s390x", "-riscv64", "-loong64" };
			int is_arch = 0;
			for (size_t a = 0; a < LEN(archs); a++) if (!strcmp(rest, archs[a])) is_arch = 1;
			snprintf(src[ns].name, sizeof src[ns].name, "linux%s", is_arch ? "" : rest);
			src[ns++].idx = i;
		}
	}
	qsort(src, ns, sizeof *src, cmp_src);
	char relver[32];
	os_field("VERSION_ID", relver, sizeof relver);
	/* 第一輪：套件行 */
	long pos = ftell(f);
	while (fgets(line, sizeof line, f)) {
		if (line[0] != 's' && line[0] != 'b') continue;
		char *fl[8] = { 0 };
		int k = 0;
		for (char *t = line; t && k < 8; k++) { fl[k] = t; t = strchr(t, '\t'); if (t) *t++ = 0; }
		if (k < 6) continue;
		fl[k - 1][strcspn(fl[k - 1], "\n")] = 0;
		const char *pkg = fl[1], *cve = fl[2], *fixed = fl[3], *others = fl[4], *flags = fl[5];
		int sev = k > 6 ? atoi(fl[6]) : 0;
		int status = fixed[0] ? (strchr(flags, 'P') ? PM_VULN_PRO : strchr(flags, 'F') ? PM_VULN_FIXABLE : PM_VULN_OPEN) : PM_VULN_OPEN;
		if (line[0] == 'b') {
			PmPkg *p = find_pkg(s, pkg);
			if (!p) continue;
			int vul = fixed[0] ? pm_vercmp(s->kind, p->version, fixed) < 0 : 1;
			if (vul) add_vuln(s, p, cve, sev, status, fixed);
			continue;
		}
		SrcIdx key;
		snprintf(key.name, sizeof key.name, "%s", pkg);
		SrcIdx *hit = bsearch(&key, src, ns, sizeof *src, cmp_src);
		if (!hit) continue;
		/* 往前找到第一個同名的，再往後逐個處理 */
		while (hit > src && !strcmp((hit - 1)->name, pkg)) hit--;
		for (; hit < src + ns && !strcmp(hit->name, pkg); hit++) {
			PmPkg *p = &s->p[hit->idx];
			int vul;
			/* 來源套件的修正版本要跟來源套件的版本比（二進位套件的版本可能不一樣，例如 libldb2 2:2.8.0+samba4.19.5…） */
			if (fixed[0]) vul = pm_vercmp(s->kind, p->srcver, fixed) < 0;
			else vul = 1;
			char show[96];
			snprintf(show, sizeof show, "%s", fixed);
			if (vul && others[0]) {
				/* 其他已修正版本（安全性更新）：裝的版本剛好是其中之一就沒事；
				 * 顯示給使用者的修正版本用這個發行版自己的（例如 …deb13u3），不是 unstable 的 */
				char ov[1024], tag[32];
				snprintf(tag, sizeof tag, "deb%su", relver);
				snprintf(ov, sizeof ov, "%s", others);
				for (char *t = strtok(ov, " "); t; t = strtok(NULL, " ")) {
					if (!strcmp(t, p->srcver)) { vul = 0; break; }
					if (relver[0] && strstr(t, tag) && (!strcmp(show, fixed) || pm_vercmp(s->kind, t, show) > 0)) snprintf(show, sizeof show, "%s", t);
				}
			}
			if (vul) add_vuln(s, p, cve, sev, status, show);
		}
	}
	free(src);
	/* 第二輪：有比對到的 CVE 補說明（先把比對到的排好，每行用二分搜尋） */
	int ncve = 0;
	for (int i = 0; i < s->n; i++) ncve += s->p[i].nv;
	if (ncve) {
		VulnRef *refs = malloc(ncve * sizeof *refs);
		int nr = 0;
		for (int i = 0; i < s->n && refs; i++)
			for (int j = 0; j < s->p[i].nv; j++) { refs[nr].v = &s->p[i].v[j]; refs[nr].p = &s->p[i]; nr++; }
		if (refs) qsort(refs, nr, sizeof *refs, cmp_ref);
		fseek(f, pos, SEEK_SET);
		while (refs && fgets(line, sizeof line, f)) {
			if (line[0] != 'C') continue;
			char *t1 = strchr(line, '\t');
			if (!t1) continue;
			char *t2 = strchr(t1 + 1, '\t');
			if (!t2) continue;
			*t2 = 0;
			char *t3 = strchr(t2 + 1, '\t');
			if (!t3) continue;
			*t3++ = 0;
			t3[strcspn(t3, "\n")] = 0;
			const char *cve = t1 + 1;
			int sev = atoi(t2 + 1);
			PmVuln key;
			snprintf(key.id, sizeof key.id, "%s", cve);
			VulnRef k = { &key, NULL };
			VulnRef *hit = bsearch(&k, refs, nr, sizeof *refs, cmp_ref);
			if (!hit) continue;
			while (hit > refs && !strcmp((hit - 1)->v->id, cve)) hit--;
			for (; hit < refs + nr && !strcmp(hit->v->id, cve); hit++) {
				snprintf(hit->v->desc, sizeof hit->v->desc, "%s", t3);
				utf8_trim(hit->v->desc);
				if (!hit->v->sev && sev) { hit->v->sev = sev; if (sev > hit->p->worst) hit->p->worst = sev; }
			}
		}
		free(refs);
	}
	fclose(f);
	s->matched = 1;
	s->nvuln_pkgs = s->nfix_pkgs = s->nopen_pkgs = s->ncve = 0;
	for (int i = 0; i < s->n; i++) {
		PmPkg *p = &s->p[i];
		if (p->nv) qsort(p->v, p->nv, sizeof *p->v, cmp_vuln);
		if (p->nv) s->nvuln_pkgs++;
		if (p->nfix) s->nfix_pkgs++;
		if (p->nopen) s->nopen_pkgs++;
		s->ncve += p->nv;
	}
	return 0;
}

/* ---------- 輸出 ---------- */

const char *pm_vuln_sev_name(int sev)
{
	static const char *n[] = { N_("不明"), N_("低"), N_("中"), N_("高"), N_("嚴重") };
	return _(n[sev < 0 || sev > 4 ? 0 : sev]);
}

const char *pm_vuln_status_name(int status)
{
	if (status == PM_VULN_FIXABLE) return _("有更新可裝");
	if (status == PM_VULN_PRO) return _("修正只在 Ubuntu Pro");
	return _("尚未修補");
}

static void json_s(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
		else if (c < 0x20) fprintf(f, "\\u%04x", c);
		else fputc(c, f);
	}
	fputc('"', f);
}

static void purl(const PmSbom *s, const PmPkg *p, char *out, size_t n)
{
	char id[64];
	os_field("ID", id, sizeof id);
	snprintf(out, n, "pkg:%s/%s/%s@%s?arch=%s", s->kind == PM_PKG_DNF ? "rpm" : "deb", id[0] ? id : "linux", p->name, p->version, p->arch);
}

static void csv_s(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) { if (*s == '"') fputc('"', f); fputc(*s, f); }
	fputc('"', f);
}

void pm_sbom_export(const PmSbom *s, FILE *f, int fmt)
{
	char ts[40];
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
	if (fmt == PM_SBOM_CYCLONEDX) {
		fprintf(f, "{\"bomFormat\":\"CycloneDX\",\"specVersion\":\"1.5\",\"version\":1,\"metadata\":{\"timestamp\":\"%s\",\"tools\":[{\"name\":\"pomelo-system\"}],\"component\":{\"type\":\"operating-system\",\"name\":", ts);
		json_s(f, s->os[0] ? s->os : "Linux");
		fputs("}},\"components\":[", f);
		for (int i = 0; i < s->n; i++) {
			const PmPkg *p = &s->p[i];
			char u[400];
			purl(s, p, u, sizeof u);
			fprintf(f, "%s{\"type\":\"library\",\"bom-ref\":", i ? "," : "");
			json_s(f, u);
			fputs(",\"name\":", f); json_s(f, p->name);
			fputs(",\"version\":", f); json_s(f, p->version);
			if (p->summary[0]) { fputs(",\"description\":", f); json_s(f, p->summary); }
			if (p->license[0]) { fputs(",\"licenses\":[{\"license\":{\"name\":", f); json_s(f, p->license); fputs("}}]", f); }
			fputs(",\"purl\":", f); json_s(f, u);
			fputs("}", f);
		}
		fputs("],\"vulnerabilities\":[", f);
		int k = 0;
		for (int i = 0; i < s->n; i++) {
			const PmPkg *p = &s->p[i];
			char u[400];
			purl(s, p, u, sizeof u);
			for (int j = 0; j < p->nv; j++, k++) {
				const PmVuln *v = &p->v[j];
				static const char *sevn[] = { "unknown", "low", "medium", "high", "critical" };
				fprintf(f, "%s{\"id\":", k ? "," : "");
				json_s(f, v->id);
				fputs(",\"source\":{\"name\":", f); json_s(f, s->data_src);
				fprintf(f, "},\"ratings\":[{\"severity\":\"%s\"}]", sevn[v->sev < 0 || v->sev > 4 ? 0 : v->sev]);
				if (v->desc[0]) { fputs(",\"description\":", f); json_s(f, v->desc); }
				fputs(",\"analysis\":{\"state\":\"exploitable\"}", f);
				if (v->fixed[0]) {
					char rec[200];
					snprintf(rec, sizeof rec, "%s %s", v->status == PM_VULN_PRO ? "Ubuntu Pro:" : "Upgrade to", v->fixed);
					fputs(",\"recommendation\":", f); json_s(f, rec);
				}
				fputs(",\"affects\":[{\"ref\":", f); json_s(f, u); fputs("}]}", f);
			}
		}
		fputs("]}\n", f);
		return;
	}
	if (fmt == PM_SBOM_CSV) {
		fprintf(f, "%s,%s,%s,%s,%s,%s,%s,%s\n", _("套件"), _("版本"), _("來源套件"), _("授權"), _("說明"), _("已知漏洞"), _("有更新可裝"), _("CVE"));
		for (int i = 0; i < s->n; i++) {
			const PmPkg *p = &s->p[i];
			csv_s(f, p->name); fputc(',', f);
			csv_s(f, p->version); fputc(',', f);
			csv_s(f, p->source); fputc(',', f);
			csv_s(f, p->license); fputc(',', f);
			csv_s(f, p->summary); fputc(',', f);
			fprintf(f, "%d,%d,", p->nv, p->nfix);
			fputc('"', f);
			for (int j = 0; j < p->nv; j++) fprintf(f, "%s%s", j ? " " : "", p->v[j].id);
			fputs("\"\n", f);
		}
		return;
	}
	/* 文字：摘要＋有漏洞的套件 */
	fprintf(f, _("%d 個套件"), s->n);
	if (s->matched) {
		char d[32];
		localtime_r(&s->data_time, &tm);
		strftime(d, sizeof d, "%Y-%m-%d", &tm);
		fprintf(f, _("；漏洞資料 %s（%s）：%d 個套件有已知漏洞，其中 %d 個有更新可裝、%d 個尚未修補"), d, s->data_src, s->nvuln_pkgs, s->nfix_pkgs, s->nopen_pkgs);
	} else fputs(_("；還沒有漏洞資料（請先更新）"), f);
	fputc('\n', f);
	for (int i = 0; i < s->n; i++) {
		const PmPkg *p = &s->p[i];
		if (!p->nv) continue;
		fprintf(f, "\n%s %s", p->name, p->version);
		if (p->license[0]) fprintf(f, " [%s]", p->license);
		fputc('\n', f);
		for (int j = 0; j < p->nv; j++) {
			const PmVuln *v = &p->v[j];
			fprintf(f, "  %s  %s  %s", v->id, pm_vuln_sev_name(v->sev), pm_vuln_status_name(v->status));
			if (v->fixed[0]) fprintf(f, " %s", v->fixed);
			if (v->desc[0]) fprintf(f, "\n      %s", v->desc);
			fputc('\n', f);
		}
	}
}
