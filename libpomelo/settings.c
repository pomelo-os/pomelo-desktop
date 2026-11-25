/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 共用設定檔 */
#define _GNU_SOURCE
#include "pomelo-settings.h"
#include <X11/Xatom.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>

extern const char *pm_family, *pm_icon_theme;   /* pomelo.c：目前的字型與圖示主題 */

typedef struct { char *key, *val; } Pair;
static Pair *pairs;
static int npairs, loaded, serial;
static Display *watch_dpy;
static Atom a_settings;

static void conf_path(char *out, size_t n, int dir_only)
{
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (cfg && *cfg) snprintf(out, n, "%s/pomelo%s", cfg, dir_only ? "" : "/settings.conf");
	else snprintf(out, n, "%s/.config/pomelo%s", home ? home : "/tmp", dir_only ? "" : "/settings.conf");
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}

static Pair *find(const char *key)
{
	for (int i = 0; i < npairs; i++)
		if (!strcmp(pairs[i].key, key))
			return &pairs[i];
	return NULL;
}

static void put(const char *key, const char *val)
{
	Pair *p = find(key);
	if (p) {
		free(p->val);
		p->val = strdup(val);
		return;
	}
	pairs = realloc(pairs, (npairs + 1) * sizeof *pairs);
	pairs[npairs].key = strdup(key);
	pairs[npairs].val = strdup(val);
	npairs++;
}

void pm_conf_load(void)
{
	/* pm_family、pm_icon_theme 常常直接指著設定值（pm_init(pm_conf_get("appearance.font", …))）：
	 * 重新載入會釋放舊的字串，要改指新的值，不然之後畫字會用到已經釋放的記憶體 */
	char fam_key[64] = "", icon_key[64] = "";
	for (int i = 0; i < npairs; i++) {
		if (pm_family == pairs[i].val) snprintf(fam_key, sizeof fam_key, "%s", pairs[i].key);
		if (pm_icon_theme == pairs[i].val) snprintf(icon_key, sizeof icon_key, "%s", pairs[i].key);
	}
	for (int i = 0; i < npairs; i++) { free(pairs[i].key); free(pairs[i].val); }
	npairs = 0;
	loaded = 1;
	serial++;
	char path[PATH_MAX], line[16384];   /* 程式順序這類清單可能很長 */
	conf_path(path, sizeof path, 0);
	FILE *f = fopen(path, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (!*l || *l == '#') continue;
		char *eq = strchr(l, '=');
		if (!eq) continue;
		*eq = 0;
		put(trim(l), trim(eq + 1));
	}
	fclose(f);
	if (fam_key[0]) pm_family = pm_conf_get(fam_key, "Noto Sans CJK TC");
	if (icon_key[0]) pm_icon_theme = pm_conf_get(icon_key, "Yaru");
}

int pm_conf_serial(void) { if (!loaded) pm_conf_load(); return serial; }

const char *pm_conf_get(const char *key, const char *def)
{
	if (!loaded) pm_conf_load();
	Pair *p = find(key);
	return p && *p->val ? p->val : def;
}

int pm_conf_int(const char *key, int def)
{
	const char *v = pm_conf_get(key, NULL);
	if (!v) return def;
	if (!strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on")) return 1;
	if (!strcmp(v, "false") || !strcmp(v, "no") || !strcmp(v, "off")) return 0;
	return atoi(v);
}

double pm_conf_num(const char *key, double def)
{
	const char *v = pm_conf_get(key, NULL);
	return v ? atof(v) : def;
}

unsigned pm_conf_color(const char *key, unsigned def)
{
	const char *v = pm_conf_get(key, NULL);
	if (!v) return def;
	if (*v == '#') v++;
	char *end;
	unsigned long c = strtoul(v, &end, 16);
	return end != v && !*end ? (unsigned)c & 0xffffff : def;
}

static void save(void)
{
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX + 8];
	conf_path(dir, sizeof dir, 1);
	conf_path(path, sizeof path, 0);
	/* 建立 ~/.config/pomelo（上一層通常已存在） */
	char parent[PATH_MAX];
	snprintf(parent, sizeof parent, "%s", dir);
	char *slash = strrchr(parent, '/');
	if (slash) { *slash = 0; mkdir(parent, 0755); }
	mkdir(dir, 0755);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "# Pomelo 設定檔（由 Pomelo 設定程式維護，也可以手動編輯）\n");
	for (int i = 0; i < npairs; i++)
		fprintf(f, "%s = %s\n", pairs[i].key, pairs[i].val);
	fclose(f);
	rename(tmp, path);   /* 原子替換，其他程式不會讀到寫一半的檔案 */
}

/* 在根視窗的 _POMELO_SETTINGS 寫入新的計數值，所有在監看的程式都會收到通知。
 * 沒呼叫過 pm_conf_watch 的程式也要能通知別人，這時臨時開一個 X 連線。 */
static void notify(void)
{
	Display *d = watch_dpy ? watch_dpy : XOpenDisplay(NULL);
	if (!d) return;
	Atom a = XInternAtom(d, "_POMELO_SETTINGS", False);
	long counter = (long)time(NULL) ^ ((long)getpid() << 16) ^ (long)serial;
	XChangeProperty(d, DefaultRootWindow(d), a, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&counter, 1);
	if (d == watch_dpy) XFlush(d);
	else XCloseDisplay(d);
}

void pm_conf_set(const char *key, const char *value)
{
	if (!loaded) pm_conf_load();
	put(key, value);
	save();
	notify();
}

void pm_conf_set_int(const char *key, int value)
{
	char buf[32];
	snprintf(buf, sizeof buf, "%d", value);
	pm_conf_set(key, buf);
}

void pm_conf_watch(Display *d)
{
	watch_dpy = d;
	a_settings = XInternAtom(d, "_POMELO_SETTINGS", False);
	XWindowAttributes wa;
	Window r = DefaultRootWindow(d);
	XGetWindowAttributes(d, r, &wa);
	XSelectInput(d, r, wa.your_event_mask | PropertyChangeMask);
}

int pm_conf_changed(Display *d, XEvent *ev)
{
	if (ev->type != PropertyNotify || ev->xproperty.window != DefaultRootWindow(d) ||
	    ev->xproperty.atom != a_settings || !a_settings)
		return 0;
	pm_conf_load();
	return 1;
}

/* ------------------------------------------------------------------ */
/* 介面翻譯                                                            */
/* ------------------------------------------------------------------ */

typedef struct { char *k, *v; } Tr;
static Tr *trs;
static int ntrs, tr_loaded;
static char cur_lang[32] = "zh_TW";

static int trcmp(const void *a, const void *b) { return strcmp(((const Tr *)a)->k, ((const Tr *)b)->k); }

/* 把 \t \n \\ 還原 */
static char *unescape(const char *s)
{
	char *o = malloc(strlen(s) + 1), *w = o;
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			*w++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s;
		} else {
			*w++ = *s;
		}
	}
	*w = 0;
	return o;
}

static int tr_try(const char *lang)
{
	const char *dirs[] = { getenv("POMELO_I18N_DIR"), "/usr/share/pomelo/i18n", "/usr/local/share/pomelo/i18n" };
	for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) {
		if (!dirs[i]) continue;
		char path[PATH_MAX];
		snprintf(path, sizeof path, "%s/%s.tsv", dirs[i], lang);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		char line[4096];
		int cap = 0;
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = 0;
			char *tab = strchr(line, '\t');
			if (line[0] == '#' || !tab || !tab[1]) continue;
			*tab = 0;
			if (ntrs == cap) { cap = cap ? cap * 2 : 512; trs = realloc(trs, cap * sizeof *trs); }
			trs[ntrs].k = unescape(line);
			trs[ntrs].v = unescape(tab + 1);
			ntrs++;
		}
		fclose(f);
		if (ntrs) qsort(trs, ntrs, sizeof *trs, trcmp);
		return 1;
	}
	return 0;
}

static void tr_load(void)
{
	tr_loaded = 1;
	const char *env[] = { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" };
	const char *v = NULL;
	for (unsigned i = 0; i < sizeof env / sizeof *env && !v; i++) {
		v = getenv(env[i]);
		if (v && !*v) v = NULL;
	}
	if (!v) return;
	/* LANGUAGE 可以是「en_US:en」這樣的清單，逐一嘗試 */
	char list[256];
	snprintf(list, sizeof list, "%s", v);
	char *save = NULL;
	for (char *l = strtok_r(list, ":", &save); l; l = strtok_r(NULL, ":", &save)) {
		char full[32];
		snprintf(full, sizeof full, "%s", l);
		full[strcspn(full, ".@")] = 0;
		if (!strcmp(full, "C") || !strcmp(full, "POSIX")) snprintf(full, sizeof full, "en");
		if (!strncmp(full, "zh_TW", 5) || !strncmp(full, "zh_HK", 5) || !strncmp(full, "zh_MO", 5)) {
			snprintf(cur_lang, sizeof cur_lang, "%s", full);   /* 原文就是繁體中文 */
			return;
		}
		if (!strncmp(full, "zh_SG", 5) || !strncmp(full, "zh_Hans", 7)) snprintf(full, sizeof full, "zh_CN");   /* 簡體共用一份 */
		char shortl[32];
		snprintf(shortl, sizeof shortl, "%s", full);
		shortl[strcspn(shortl, "_")] = 0;
		if (tr_try(full) || tr_try(shortl)) {
			snprintf(cur_lang, sizeof cur_lang, "%s", full);
			return;
		}
	}
}

/* 原文可以寫成「{情境}原文」：同一個字在不同地方意思不同時（例如選單的「檔案」與檔案管理員的「檔案」），
 * 翻譯檔用整串當原文；沒有翻譯（包括繁體中文介面）就顯示 } 後面的部分 */
const char *pm_tr(const char *s)
{
	if (!tr_loaded) tr_load();
	if (!s) return s;
	const char *plain = s;
	if (s[0] == '{') { const char *e = strchr(s, '}'); if (e) plain = e + 1; }
	if (!ntrs) return plain;
	Tr key = { (char *)s, NULL };
	Tr *r = bsearch(&key, trs, ntrs, sizeof *trs, trcmp);
	return r ? r->v : plain;
}

const char *pm_lang(void)
{
	if (!tr_loaded) tr_load();
	return cur_lang;
}
