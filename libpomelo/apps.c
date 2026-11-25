/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 已安裝的應用程式（.desktop 檔） */
#define _GNU_SOURCE
#include "pomelo.h"
#include <dirent.h>
#include <libintl.h>
#include <stdint.h>
#include <sys/stat.h>

static PmApp *apps;
static int napps, scanned;

static char *xstrdup(const char *s) { return s && *s ? strdup(s) : NULL; }

/* 去掉 Exec 裡的 %f %U 等參數代號 */
static char *clean_exec(const char *in)
{
	char *out = malloc(strlen(in) + 1), *o = out;
	for (const char *p = in; *p; p++) {
		if (*p == '%' && p[1]) { if (p[1] == '%') *o++ = '%'; p++; continue; }
		*o++ = *p;
	}
	while (o > out && o[-1] == ' ') o--;
	*o = 0;
	return out;
}

static void parse(const char *path, const char *id, const char *lfull, const char *lshort)
{
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[4096], k_name[48], k_sname[48], k_gen[48], k_com[48], k_kw[48];
	snprintf(k_name, sizeof k_name, "Name[%s]", lfull);
	snprintf(k_sname, sizeof k_sname, "Name[%s]", lshort);
	snprintf(k_gen, sizeof k_gen, "GenericName[%s]", lfull);
	snprintf(k_com, sizeof k_com, "Comment[%s]", lfull);
	snprintf(k_kw, sizeof k_kw, "Keywords[%s]", lfull);
	char *v[16] = { 0 };   /* 0 Name 1 本地Name 2 短本地Name 3 Generic 4 本地Generic 5 Comment 6 本地Comment
	                          7 Exec 8 Icon 9 Keywords 10 本地Keywords 11 WMClass 12 Categories 13 Domain 14 Type */
	int in = 0, hidden = 0, term = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') { if (in) break; in = !strcmp(line, "[Desktop Entry]"); continue; }
		char *eq = strchr(line, '=');
		if (!in || !eq || line[0] == '#') continue;
		*eq = 0;
		const char *k = line, *val = eq + 1;
		int slot = -1;
		if (!strcmp(k, "Name")) slot = 0;
		else if (!strcmp(k, k_name)) slot = 1;
		else if (!strcmp(k, k_sname)) slot = 2;
		else if (!strcmp(k, "GenericName")) slot = 3;
		else if (!strcmp(k, k_gen)) slot = 4;
		else if (!strcmp(k, "Comment")) slot = 5;
		else if (!strcmp(k, k_com)) slot = 6;
		else if (!strcmp(k, "Exec")) slot = 7;
		else if (!strcmp(k, "Icon")) slot = 8;
		else if (!strcmp(k, "Keywords")) slot = 9;
		else if (!strcmp(k, k_kw)) slot = 10;
		else if (!strcmp(k, "StartupWMClass")) slot = 11;
		else if (!strcmp(k, "Categories")) slot = 12;
		else if (!strcmp(k, "X-Ubuntu-Gettext-Domain") || !strcmp(k, "X-GNOME-Gettext-Domain")) slot = 13;
		else if (!strcmp(k, "Type")) slot = 14;
		else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) && !strcmp(val, "true")) hidden = 1;
		else if (!strcmp(k, "Terminal")) term = !strcmp(val, "true");
		if (slot >= 0 && !v[slot]) v[slot] = strdup(val);
	}
	fclose(f);
	/* 標成隱藏（NoDisplay、Hidden）的檔案即使沒有 Name、Exec 也要登記：XDG 的慣例是在使用者目錄放一個只寫
	 * Hidden=true 的同名檔來隱藏系統的程式，沒登記的話系統那份就會冒出來 */
	if ((v[0] && v[7] && (!v[14] || !strcmp(v[14], "Application"))) || hidden) {
		PmApp a = { 0 };
		if (!v[0]) v[0] = strdup(id);
		if (!v[7]) v[7] = strdup("");
		a.id = strdup(id);
		a.path = strdup(path);
		const char *name = v[1] ? v[1] : v[2];
		const char *gen = v[4], *com = v[6];
		if (v[13]) {   /* Ubuntu 把翻譯放在 .mo 檔裡 */
			bind_textdomain_codeset(v[13], "UTF-8");
			if (!name) { const char *t = dgettext(v[13], v[0]); if (t != v[0]) name = t; }
			if (!gen && v[3]) { const char *t = dgettext(v[13], v[3]); if (t != v[3]) gen = t; }
			if (!com && v[5]) { const char *t = dgettext(v[13], v[5]); if (t != v[5]) com = t; }
		}
		a.name = strdup(name ? name : v[0]);
		a.name_en = strdup(v[0]);
		a.generic = xstrdup(gen ? gen : v[3]);
		a.comment = xstrdup(com ? com : v[5]);
		a.exec = clean_exec(v[7]);
		a.icon = xstrdup(v[8]);
		size_t kl = (v[9] ? strlen(v[9]) : 0) + (v[10] ? strlen(v[10]) : 0) + 4;
		a.keywords = malloc(kl);
		snprintf(a.keywords, kl, "%s;%s", v[9] ? v[9] : "", v[10] ? v[10] : "");
		a.wmclass = xstrdup(v[11]);
		a.categories = xstrdup(v[12]);
		a.terminal = term;
		a.hidden = hidden;
		apps = realloc(apps, (napps + 1) * sizeof *apps);
		apps[napps++] = a;
	}
	for (int i = 0; i < 16; i++) free(v[i]);
}

static int appcmp(const void *a, const void *b)
{
	return strcoll(((const PmApp *)a)->name, ((const PmApp *)b)->name);
}

void pm_apps_free(void)
{
	for (int i = 0; i < napps; i++) {
		PmApp *a = &apps[i];
		free(a->id); free(a->path); free(a->name); free(a->name_en); free(a->generic); free(a->comment);
		free(a->exec); free(a->icon); free(a->keywords); free(a->wmclass); free(a->categories);
	}
	free(apps);
	apps = NULL;
	napps = 0;
	scanned = 0;
}

/* ---- 掃描結果的快取（0.41 效能）----
 * 每個程式第一次 pm_apps() 都要開 150 個左右的 .desktop 檔、查每個程式的 .mo 翻譯（約 5～15 ms CPU，加上翻譯檔載入的記憶體）。
 * 結果存成 ~/.cache/pomelo/apps.bin；鍵 = 語言＋所有 .desktop 的（目錄、檔名、修改時間、大小）加總雜湊＋語言包目錄的修改時間。
 * 只要有檔案新增、刪除、改過就整個重掃；檔案壞掉、版本不對、讀不到都當作沒有快取。寫入先寫暫存檔再 rename（多個程式同時掃也安全）。 */
#define AC_MAGIC "POMELOAP2"

static void ac_path(char *out, size_t n)
{
	const char *cd = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	if (cd && *cd) snprintf(out, n, "%s/pomelo/apps.bin", cd);
	else if (home && *home) snprintf(out, n, "%s/.cache/pomelo/apps.bin", home);
	else out[0] = 0;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	for (const unsigned char *c = p; n--; c++) h = (h ^ *c) * 1099511628211ULL;
	return h;
}

static char *ac_str(char **p, char *end)
{
	if (end - *p < 4) return (char *)-1;
	uint32_t l;
	memcpy(&l, *p, 4);
	*p += 4;
	if (!l) return NULL;
	if (l > 65536 || (size_t)(end - *p) < l || (*p)[l - 1]) return (char *)-1;
	char *r = malloc(l);
	if (!r) return (char *)-1;
	memcpy(r, *p, l);
	*p += l;
	return r;
}

static int ac_load(const char *lockey, uint64_t sig)
{
	char path[PATH_MAX];
	ac_path(path, sizeof path);
	if (!path[0]) return 0;
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	struct stat st;
	if (fstat(fileno(f), &st) || st.st_size < (off_t)(sizeof AC_MAGIC + 8 + 4 + 64) || st.st_size > (64 << 20)) { fclose(f); return 0; }
	char *buf = malloc((size_t)st.st_size);
	int ok = buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
	fclose(f);
	if (!ok) { free(buf); return 0; }
	char *p = buf, *end = buf + st.st_size;
	uint64_t fsig = 0;
	uint32_t n = 0;
	char lk[64];
	ok = !memcmp(p, AC_MAGIC, sizeof AC_MAGIC);
	if (ok) {
		p += sizeof AC_MAGIC;
		memcpy(&fsig, p, 8); memcpy(&n, p + 8, 4); memcpy(lk, p + 12, 64);
		p += 76;
		if (fsig != sig || n > 100000 || memcmp(lk, lockey, 64)) ok = 0;
	}
	PmApp *v = ok ? calloc(n ? n : 1, sizeof *v) : NULL;
	if (ok && !v) ok = 0;
	uint32_t i = 0;
	for (; ok && i < n; i++) {
		PmApp *a = &v[i];
		char **fl[11] = { &a->id, &a->path, &a->name, &a->name_en, &a->generic, &a->comment, &a->exec, &a->icon, &a->keywords, &a->wmclass, &a->categories };
		int k = 0;
		for (; k < 11; k++) { *fl[k] = ac_str(&p, end); if (*fl[k] == (char *)-1) { *fl[k] = NULL; ok = 0; break; } }
		if (ok && end - p >= 8) { memcpy(&a->terminal, p, 4); memcpy(&a->hidden, p + 4, 4); p += 8; }
		else ok = 0;
		if (ok && (!a->id || !a->name || !a->name_en || !a->exec || !a->keywords || !a->path)) ok = 0;   /* 這些欄位其他程式不檢查 NULL */
	}
	if (!ok) {   /* 壞掉：已經讀到的全部放掉 */
		for (uint32_t j = 0; v && j <= i && j < n; j++) {
			PmApp *a = &v[j];
			free(a->id); free(a->path); free(a->name); free(a->name_en); free(a->generic); free(a->comment);
			free(a->exec); free(a->icon); free(a->keywords); free(a->wmclass); free(a->categories);
		}
		free(v);
		free(buf);
		if (v) unlink(path);   /* 內容壞掉才刪；只是過期（簽名、語言不同）的由下一次存檔蓋掉 */
		return 0;
	}
	free(buf);
	apps = v;
	napps = (int)n;
	return 1;
}

static void ac_put_str(FILE *f, const char *s)
{
	uint32_t l = s ? (uint32_t)strlen(s) + 1 : 0;
	fwrite(&l, 4, 1, f);
	if (l) fwrite(s, 1, l, f);
}

static void ac_save(const char *lockey, uint64_t sig)
{
	char path[PATH_MAX], tmp[PATH_MAX + 16], d[PATH_MAX];
	ac_path(path, sizeof path);
	if (!path[0]) return;
	snprintf(d, sizeof d, "%s", path);
	for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0700); *q = '/'; }
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) return;
	uint32_t n = (uint32_t)napps;
	fwrite(AC_MAGIC, 1, sizeof AC_MAGIC, f);
	fwrite(&sig, 8, 1, f);
	fwrite(&n, 4, 1, f);
	fwrite(lockey, 1, 64, f);
	for (int i = 0; i < napps; i++) {
		const PmApp *a = &apps[i];
		const char *fl[11] = { a->id, a->path, a->name, a->name_en, a->generic, a->comment, a->exec, a->icon, a->keywords, a->wmclass, a->categories };
		for (int k = 0; k < 11; k++) ac_put_str(f, fl[k]);
		fwrite(&a->terminal, 4, 1, f);
		fwrite(&a->hidden, 4, 1, f);
	}
	int bad = ferror(f);
	if (fclose(f) || bad || rename(tmp, path)) unlink(tmp);
}

PmApp *pm_apps(int *count)
{
	if (scanned) { *count = napps; return apps; }
	scanned = 1;
	char lfull[32], lshort[32];
	const char *loc = setlocale(LC_MESSAGES, NULL);
	snprintf(lfull, sizeof lfull, "%s", loc ? loc : "C");
	lfull[strcspn(lfull, ".@")] = 0;
	snprintf(lshort, sizeof lshort, "%s", lfull);
	lshort[strcspn(lshort, "_")] = 0;
	const char *home = getenv("HOME") ? getenv("HOME") : "", *xdg = getenv("XDG_DATA_HOME");
	char dirs[6][PATH_MAX];
	int nd = 0;
	if (xdg && *xdg) snprintf(dirs[nd++], PATH_MAX, "%s/applications", xdg);
	else snprintf(dirs[nd++], PATH_MAX, "%s/.local/share/applications", home);
	snprintf(dirs[nd++], PATH_MAX, "%s/.local/share/flatpak/exports/share/applications", home);
	snprintf(dirs[nd++], PATH_MAX, "/var/lib/flatpak/exports/share/applications");
	snprintf(dirs[nd++], PATH_MAX, "/var/lib/snapd/desktop/applications");
	snprintf(dirs[nd++], PATH_MAX, "/usr/local/share/applications");
	snprintf(dirs[nd++], PATH_MAX, "/usr/share/applications");
	/* 第一遍：列出所有 .desktop 並算簽名（只有 stat，不開檔） */
	struct dfile { int d; char name[256]; } *files = NULL;
	int nf = 0, capf = 0;
	uint64_t sig = 0;
	for (int d = 0; d < nd; d++) {
		DIR *dp = opendir(dirs[d]);
		struct dirent *e;
		while (dp && (e = readdir(dp))) {
			size_t l = strlen(e->d_name);
			if (l < 9 || l >= 256 || strcmp(e->d_name + l - 8, ".desktop")) continue;
			struct stat st;
			if (fstatat(dirfd(dp), e->d_name, &st, 0)) continue;
			uint64_t h = fnv(1469598103934665603ULL, &d, sizeof d);
			h = fnv(h, e->d_name, l);
			h = fnv(h, &st.st_mtim, sizeof st.st_mtim);
			h = fnv(h, &st.st_size, sizeof st.st_size);
			sig += h;   /* 加總：不受 readdir 順序影響 */
			if (nf == capf) {
				struct dfile *nv = realloc(files, (size_t)(capf = capf ? capf * 2 : 256) * sizeof *files);
				if (!nv) break;
				files = nv;
			}
			files[nf].d = d;
			memcpy(files[nf].name, e->d_name, l + 1);
			nf++;
		}
		if (dp) closedir(dp);
	}
	/* 翻譯來源（Ubuntu 的 .mo 在語言包目錄）有變也要重掃；語言與排序規則也算進去 */
	const char *mo[] = { "/usr/share/locale-langpack", "/usr/share/locale" };
	for (unsigned i = 0; i < LEN(mo); i++) {
		struct stat st;
		if (!stat(mo[i], &st)) sig = fnv(sig ^ i, &st.st_mtim, sizeof st.st_mtim);
	}
	char lockey[64] = "";
	const char *coll = setlocale(LC_COLLATE, NULL), *lang = getenv("LANGUAGE");
	snprintf(lockey, sizeof lockey, "%s|%s|%s", lfull, coll ? coll : "", lang ? lang : "");
	if (ac_load(lockey, sig)) { free(files); *count = napps; return apps; }
	for (int i = 0; i < nf; i++) {
		size_t l = strlen(files[i].name);
		char id[256], path[PATH_MAX + 260];
		snprintf(id, sizeof id, "%.*s", (int)(l - 8), files[i].name);
		int dup = 0;   /* 使用者目錄的同名檔優先 */
		for (int k = 0; k < napps && !dup; k++) dup = !strcmp(apps[k].id, id);
		if (dup) continue;
		snprintf(path, sizeof path, "%s/%s", dirs[files[i].d], files[i].name);
		parse(path, id, lfull, lshort);
	}
	free(files);
	if (napps) qsort(apps, napps, sizeof *apps, appcmp);
	ac_save(lockey, sig);
	*count = napps;
	return apps;
}

PmApp *pm_app_by_id(const char *id)
{
	int n;
	PmApp *v = pm_apps(&n);
	for (int i = 0; i < n; i++)
		if (!strcmp(v[i].id, id)) return &v[i];
	return NULL;
}

PmApp *pm_app_by_class(const char *cls, const char *inst)
{
	int n;
	PmApp *v = pm_apps(&n);
	if (!cls) cls = "";
	if (!inst) inst = "";
	if (!*cls && !*inst) return NULL;
	for (int i = 0; i < n; i++)   /* 1. StartupWMClass 完全符合 */
		if (v[i].wmclass && (!strcasecmp(v[i].wmclass, cls) || !strcasecmp(v[i].wmclass, inst)))
			return &v[i];
	for (int i = 0; i < n; i++) {  /* 2. 檔名符合，例如 firefox、org.gnome.Nautilus、snap 的 firefox_firefox */
		const char *tail = strrchr(v[i].id, '.') ? strrchr(v[i].id, '.') + 1 : v[i].id;
		const char *snap = strchr(v[i].id, '_') ? strchr(v[i].id, '_') + 1 : NULL;
		if (!strcasecmp(v[i].id, inst) || !strcasecmp(v[i].id, cls) || (*inst && !strcasecmp(tail, inst)) ||
		    (snap && (!strcasecmp(snap, inst) || !strcasecmp(snap, cls))))
			return &v[i];
	}
	return NULL;
}

/* 搜尋分數：名稱開頭 > 名稱包含 > 英文名稱 > 指令 > 關鍵字 */
int pm_app_match(const PmApp *a, const char *q)
{
	size_t ql = strlen(q);
	if (!ql) return 1;
	const char *p;
	if (!strncasecmp(a->name, q, ql)) return 100;
	if ((p = strcasestr(a->name, q))) return p[-1] == ' ' ? 90 : 75;
	if (!strncasecmp(a->name_en, q, ql)) return 85;
	if (strcasestr(a->name_en, q)) return 60;
	char bin[256];
	snprintf(bin, sizeof bin, "%s", a->exec);
	bin[strcspn(bin, " ")] = 0;
	const char *b = strrchr(bin, '/') ? strrchr(bin, '/') + 1 : bin;
	if (!strncasecmp(b, q, ql)) return 70;
	if (a->generic && strcasestr(a->generic, q)) return 55;
	if (ql > 1 && strcasestr(a->keywords, q)) return 50;
	if (ql > 1 && a->comment && strcasestr(a->comment, q)) return 30;
	return 0;
}

/* ---- 分類 ---- */

const char *pm_category_name(int cat)
{
	switch (cat) {
	case PM_CAT_TOOLS: return pm_tr("工具類");
	case PM_CAT_OFFICE: return pm_tr("辦公類");
	case PM_CAT_DOCS: return pm_tr("文書類");
	case PM_CAT_FUN: return pm_tr("娛樂類");
	case PM_CAT_PROD: return pm_tr("生產力類");
	case PM_CAT_OTHER: return pm_tr("其他");
	}
	return pm_tr("全部");
}

/* Categories 字串（分號分隔）裡有沒有某個詞 */
static int has_cat(const char *cats, const char *w)
{
	if (!cats) return 0;
	size_t wl = strlen(w);
	for (const char *p = cats; *p; ) {
		size_t l = strcspn(p, ";");
		if (l == wl && !strncmp(p, w, wl)) return 1;
		p += l + (p[l] ? 1 : 0);
	}
	return 0;
}

static int has_any(const char *cats, const char *const *words)
{
	for (; *words; words++) if (has_cat(cats, *words)) return 1;
	return 0;
}

int pm_app_auto_category(const PmApp *a)
{
	const char *c = a->categories;
	static const char *const pomelo_cat[PM_CAT_N] = { "X-Pomelo-Tools", "X-Pomelo-Office", "X-Pomelo-Docs", "X-Pomelo-Fun",
	                                                  "X-Pomelo-Productivity", "X-Pomelo-Other" };
	for (int i = 0; i < PM_CAT_N; i++) if (has_cat(c, pomelo_cat[i])) return i;
	/* 辦公：試算表、簡報、行事曆這類；文書：文書處理、文件閱讀、文字編輯 */
	static const char *const office_kind[] = { "Spreadsheet", "Presentation", "Calendar", "Chart", "Database", "ProjectManagement",
	                                           "Finance", "Flowchart", "FlowChart", NULL };
	static const char *const docs_kind[] = { "WordProcessor", "Viewer", "Publishing", "Dictionary", "TextEditor", "Documentation", NULL };
	if (has_any(c, office_kind)) return PM_CAT_OFFICE;
	/* 圖片檢視（Graphics;Viewer）算娛樂，文件檢視（Office;Viewer）算文書 */
	if (has_any(c, docs_kind) && (has_cat(c, "Office") || !has_cat(c, "Graphics"))) return PM_CAT_DOCS;
	if (has_cat(c, "Office")) return PM_CAT_OFFICE;
	static const char *const tools[] = { "Utility", "System", "Security", "Accessibility", "FileManager", "FileTools",
	                                     "TerminalEmulator", "PackageManager", "Monitor", "Archiving", "Calculator", NULL };
	int is_tool = has_any(c, tools);
	/* 純設定面板（沒有同時是工具）放「其他」，不然工具類會被幾十個設定頁塞滿 */
	if (has_cat(c, "Settings") && !is_tool) return PM_CAT_OTHER;
	static const char *const fun[] = { "Game", "AudioVideo", "Audio", "Video", "Player", "Music", "Amusement", NULL };
	if (has_any(c, fun)) return PM_CAT_FUN;
	if (is_tool) return PM_CAT_TOOLS;
	if (has_cat(c, "Graphics") || has_cat(c, "Photography")) return PM_CAT_FUN;
	static const char *const prod[] = { "Network", "Development", "Education", "Science", "Math", "WebBrowser", "Email", NULL };
	if (has_any(c, prod)) return PM_CAT_PROD;
	return PM_CAT_OTHER;
}

int pm_app_category(const PmApp *a)
{
	char key[300];
	snprintf(key, sizeof key, "launcher.cat.%s", a->id);
	const char *v = pm_conf_get(key, "");
	if (v[0] >= '0' && v[0] < '0' + PM_CAT_N && !v[1]) return v[0] - '0';
	return pm_app_auto_category(a);
}

/* ---- 自訂順序（launcher.order.<分類>：程式 ID 以逗號分開，排在前面；沒排過的依名稱接在後面） ---- */

static void order_key(int cat, char *key, size_t n) { snprintf(key, n, "launcher.order.%d", cat); }

int pm_apps_in_category(int cat, int hidden_mode, PmApp **out, int max)
{
	int n, k = 0;
	PmApp *v = pm_apps(&n);
	char used[n ? n : 1];
	memset(used, 0, sizeof used);
	char key[48];
	order_key(cat, key, sizeof key);
	const char *ord = cat >= 0 ? pm_conf_get(key, "") : "";
	/* 先照使用者排的順序放 */
	for (const char *p = ord; *p && k < max; ) {
		size_t l = strcspn(p, ",");
		for (int i = 0; i < n; i++) {
			if (used[i] || strlen(v[i].id) != l || strncmp(v[i].id, p, l)) continue;
			if (hidden_mode == 0 && v[i].hidden) break;
			if (hidden_mode == 2 && !v[i].hidden) break;
			if (cat >= 0 && pm_app_category(&v[i]) != cat) break;
			used[i] = 1;
			out[k++] = &v[i];
			break;
		}
		p += l + (p[l] ? 1 : 0);
	}
	for (int i = 0; i < n && k < max; i++) {
		if (used[i]) continue;
		if (hidden_mode == 0 && v[i].hidden) continue;
		if (hidden_mode == 2 && !v[i].hidden) continue;
		if (cat >= 0 && pm_app_category(&v[i]) != cat) continue;
		out[k++] = &v[i];
	}
	return k;
}

void pm_app_order_set(int cat, PmApp *const *list, int n)
{
	char key[48], val[8192] = "";
	order_key(cat, key, sizeof key);
	size_t l = 0;
	for (int i = 0; i < n && list; i++) {
		size_t il = strlen(list[i]->id);
		if (strchr(list[i]->id, ',') || l + il + 2 >= sizeof val) continue;
		l += snprintf(val + l, sizeof val - l, "%s%s", l ? "," : "", list[i]->id);
	}
	pm_conf_set(key, val);   /* 空字串 = 清掉自訂順序（pm_conf_get 把空值當作沒設定） */
}

void pm_app_launch(const PmApp *a)
{
	if (a->terminal) {
		char cmd[2048];
		snprintf(cmd, sizeof cmd, "pomelo-terminal -e %s", a->exec);
		pm_spawn(cmd);
	} else {
		pm_spawn(a->exec);
	}
}
