/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gui-browse.c — 壓縮管理員的瀏覽視窗（雙後端：Wayland 工作階段走原生視窗，X11 或 POMELO_BACKEND=x11 走 X，事件處理共用）
 *
 * 工具列（開啟、解壓縮、新增、測試、資訊）、路徑列（可以點）、可以排序的清單（只畫看得到的列，上萬筆也不卡）、狀態列。
 * 右鍵選單、拖放（把壓縮檔拖進來開啟、把檔案拖進來加入或建立）、鍵盤操作。
 * 耗時的事都另外開子程序做（解壓縮、加入檔案、測試、問密碼、把檔案解到暫存再打開），視窗不會卡住。 */
#define _GNU_SOURCE
#include "gui.h"
#include <pomelo-wl.h>
#include <poll.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/wait.h>

#define TB_H 60
#define PATH_H 38
#define HEAD_H 28
#define ROW_H 32
#define ST_H 32
#define PAD 16
#define SBW 10

/* ---------- 顏色 ---------- */

static unsigned c_bg, c_tool, c_line, c_text, c_dim, c_stripe, c_hl, c_selin, c_warn, c_ok;

static void apply_colors(void)
{
	pm_set_theme(pm_conf_int("appearance.dark", 0), pm_conf_color("appearance.accent", 0));
	if (pm_dark) {
		c_bg = 0x1e1e1e; c_tool = 0x2f2f31; c_line = 0x3a3a3c; c_text = 0xf2f2f2; c_dim = 0x98989d; c_stripe = 0x242426;
		c_hl = 0x3a3a3c; c_selin = 0x48484a; c_warn = 0xff9f0a; c_ok = 0x30d158;
	} else {
		c_bg = 0xffffff; c_tool = 0xf7f7f7; c_line = 0xdcdcdc; c_text = 0x1d1d1f; c_dim = 0x8e8e93; c_stripe = 0xf6f6f8;
		c_hl = 0xe4e4e7; c_selin = 0xdcdcdf; c_warn = 0xc77700; c_ok = 0x1f9d3a;
	}
}

/* ---------- 狀態 ---------- */

typedef struct {
	char *name;
	int dir, link, enc;
	long long size, csize;
	time_t mtime;
	long nchild;
	long idx;           /* 對應 AcList 的項目；-1 = 沒有明寫的資料夾（從檔案路徑推出來的） */
} Row;

static Window win;
static PmWl *wl;   /* 原生 Wayland 視窗（NULL＝X11） */
static int W = 960, H = 640, focused = 1;
static AcList L;
static int have;
static char arcfile[PATH_MAX], arcpw[256], arccs[16];
static char cwd[PATH_MAX];
static Row *rows;
static int nrows;
static unsigned char *sel;
static int cursor = -1, anchor = -1, scrolly, sortcol, sortdesc;
static int hover_row = -1, hover_btn = -1;
static char status_msg[300];
static long status_until;
static Atom a_delete, a_clipboard, a_utf8, a_targets, a_netname, a_neticon, a_netpid, a_string;
static char *clip_text;
static long last_click_ms;
static int last_click_row = -1;
static char tmpdirs[8][PATH_MAX];
static int ntmp;

typedef struct { int x, y, w, h; } R;
static R r_list, r_path_up;
static R r_btn[6];
static int nbtn;
static R r_cs, r_hdr[4];
static R r_crumb[32];
static char crumb_path[32][PATH_MAX];
static int ncrumb;

static int in_r(R r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(status_msg, sizeof status_msg, fmt, ap);
	va_end(ap);
	status_until = pm_now_ms() + 6000;
}

/* ---------- 子程序 ---------- */

enum { K_PW, K_OPENFILE, K_TEST, K_EXTRACT, K_ADD };

typedef struct {
	pid_t pid;
	int fd, kind;
	char buf[2048];
	size_t n;
	char path[PATH_MAX];   /* K_OPENFILE：要打開的檔案；K_PW：正在開的壓縮檔 */
	char cs[16];
} Kid;
static Kid kids[8];

static Kid *kid_new(int kind)
{
	for (int i = 0; i < 8; i++) if (!kids[i].pid) { memset(&kids[i], 0, sizeof kids[i]); kids[i].kind = kind; kids[i].fd = -1; return &kids[i]; }
	return NULL;
}

static int any_kid(void) { for (int i = 0; i < 8; i++) if (kids[i].pid) return 1; return 0; }

static void load_archive(const char *path, const char *pw, const char *cs);
static void draw(void);

static void kid_done(Kid *k, int status)
{
	int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
	switch (k->kind) {
	case K_PW: {
		char pw[256];
		snprintf(pw, sizeof pw, "%.*s", (int)MIN(k->n, sizeof pw - 1), k->buf);
		pw[strcspn(pw, "\r\n")] = 0;
		if (rc == 0 && pw[0]) load_archive(k->path, pw, k->cs[0] ? k->cs : NULL);
		else { set_status(_("沒有輸入密碼：可以看到檔名，但不能打開或解壓縮裡面的檔案")); }
		ac_wipe(pw, sizeof pw);
		break;
	}
	case K_OPENFILE:
		if (rc == 0 && k->path[0]) { pm_open(k->path); set_status(_("已經解壓縮並用預設程式打開")); }
		else { k->buf[MIN(k->n, sizeof k->buf - 1)] = 0; set_status(_("打開失敗：%s"), k->buf); }
		break;
	case K_TEST:
		k->buf[MIN(k->n, sizeof k->buf - 1)] = 0;
		if (rc == 0) set_status(_("測試通過：壓縮檔完整、沒有損壞"));
		else { const char *m = strstr(k->buf, "pomelo-archive："); const char *msgs[] = { m ? m + strlen("pomelo-archive：") : _("測試沒有通過") }; const char *b[] = { _("好") }; pm_ask(_("測試結果"), "pomelo-archive", pa_icon, msgs[0], b, 1, 0, 0); set_status(_("測試沒有通過")); }
		break;
	case K_EXTRACT:
		if (rc == 0) set_status(_("解壓縮完成"));
		break;
	case K_ADD:
		if (rc == 0) {
			char f[PATH_MAX], p[256], c[16];   /* 先複製：load_archive 會改寫 arcfile／arcpw／arccs，來源與目的相同的 snprintf 結果是空的 */
			snprintf(f, sizeof f, "%s", arcfile); snprintf(p, sizeof p, "%s", arcpw); snprintf(c, sizeof c, "%s", arccs);
			load_archive(f, p[0] ? p : NULL, c[0] ? c : NULL);
			ac_wipe(p, sizeof p);
			set_status(_("已加入檔案"));
		}
		break;
	}
}

static void reap_kids(void)
{
	int st;
	pid_t p;
	while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
		for (int i = 0; i < 8; i++) if (kids[i].pid == p) {
			Kid *k = &kids[i];
			/* 把管子裡剩下的讀完 */
			while (k->fd >= 0) {
				ssize_t n = read(k->fd, k->buf + k->n, sizeof k->buf - 1 - k->n);
				if (n <= 0) break;
				k->n += (size_t)n;
			}
			if (k->fd >= 0) close(k->fd);
			Kid copy = *k;
			memset(k, 0, sizeof *k);
			kid_done(&copy, st);
			ac_wipe(copy.buf, sizeof copy.buf);
			break;
		}
	}
}

static Kid *spawn_kid(int kind, char *const argv[], const char *pw, int capture)
{
	Kid *k = kid_new(kind);
	if (!k) { set_status(_("同時開太多工作了，等一下再試")); return NULL; }
	int fd = -1;
	pid_t pid = pa_spawn(argv, pw, capture ? &fd : NULL);
	if (pid <= 0) { memset(k, 0, sizeof *k); set_status(_("沒辦法啟動")); return NULL; }
	k->pid = pid;
	k->fd = fd;
	if (fd >= 0) { int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
	return k;
}

/* ---------- 清單模型 ---------- */

static void free_rows(void)
{
	for (int i = 0; i < nrows; i++) free(rows[i].name);
	free(rows);
	free(sel);
	rows = NULL; sel = NULL; nrows = 0;
}

static int row_cmp(const void *pa, const void *pb)
{
	const Row *a = pa, *b = pb;
	if (a->dir != b->dir) return b->dir - a->dir;
	int c = 0;
	switch (sortcol) {
	case 1: c = a->size < b->size ? -1 : a->size > b->size; break;
	case 2: c = a->csize < b->csize ? -1 : a->csize > b->csize; break;
	case 3: c = a->mtime < b->mtime ? -1 : a->mtime > b->mtime; break;
	}
	if (!c) c = strcoll(a->name, b->name);
	return sortdesc ? -c : c;
}

static unsigned hash_str(const char *s, size_t n)
{
	unsigned h = 2166136261u;
	for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
	return h;
}

static void build_rows(void)
{
	free_rows();
	if (!have) return;
	size_t cl = strlen(cwd);
	size_t cap = 64;
	while (cap < L.n * 2 + 16) cap <<= 1;
	int *table = malloc(cap * sizeof *table);
	for (size_t i = 0; i < cap; i++) table[i] = -1;
	rows = malloc((L.n + 1) * sizeof *rows);
	for (size_t i = 0; i < L.n; i++) {
		const AcEntry *e = &L.e[i];
		const char *rel = e->path;
		if (cl) { if (strncmp(rel, cwd, cl) || rel[cl] != '/') continue; rel += cl + 1; }
		if (!*rel) continue;
		const char *sl = strchr(rel, '/');
		size_t nl = sl ? (size_t)(sl - rel) : strlen(rel);
		unsigned h = hash_str(rel, nl) & (cap - 1);
		int ri = -1;
		while (table[h] >= 0) {
			Row *r = &rows[table[h]];
			if (strlen(r->name) == nl && !strncmp(r->name, rel, nl)) { ri = table[h]; break; }
			h = (h + 1) & (cap - 1);
		}
		if (ri < 0) {
			ri = nrows++;
			table[h] = ri;
			Row *r = &rows[ri];
			memset(r, 0, sizeof *r);
			r->name = strndup(rel, nl);
			r->idx = -1;
			r->csize = -1;
		}
		Row *r = &rows[ri];
		if (sl) {   /* 底下的東西：這一層是資料夾 */
			r->dir = 1;
			r->nchild++;
			if (e->type == AE_FILE && e->size > 0) r->size += e->size;
			if (e->enc) r->enc = 1;
			if (e->mtime > r->mtime && r->idx < 0) r->mtime = e->mtime;
		} else {
			r->idx = (long)i;
			r->dir = e->type == AE_DIR;
			r->link = e->type == AE_LINK;
			r->enc = e->enc;
			r->mtime = e->mtime;
			if (!r->dir) { r->size = e->size; r->csize = e->csize; }
		}
	}
	free(table);
	qsort(rows, (size_t)nrows, sizeof *rows, row_cmp);
	sel = calloc((size_t)nrows + 1, 1);
	cursor = anchor = -1;
	scrolly = 0;
}

static int has_csize(void)
{
	for (int i = 0; i < nrows; i++) if (rows[i].csize >= 0) return 1;
	return 0;
}

static void row_path(const Row *r, char *out, size_t n)
{
	if (cwd[0]) snprintf(out, n, "%s/%s", cwd, r->name); else snprintf(out, n, "%s", r->name);
}

static int nsel(void) { int n = 0; for (int i = 0; i < nrows; i++) n += sel[i]; return n; }

static void clear_sel(void) { if (sel) memset(sel, 0, (size_t)nrows); }

/* ---------- 視窗標題 ---------- */

static void set_title(void)
{
	char t[PATH_MAX + 64];
	if (have) { const char *b = strrchr(arcfile, '/'); snprintf(t, sizeof t, "%s — %s", b ? b + 1 : arcfile, _("壓縮管理員")); }
	else snprintf(t, sizeof t, "%s", _("壓縮管理員"));
	if (wl) { pmwl_set_title(wl, t); return; }
	XStoreName(dpy, win, t);
	XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (unsigned char *)t, (int)strlen(t));
}

/* ---------- 開啟壓縮檔 ---------- */

static void close_archive(void)
{
	if (have) ac_close(&L);
	memset(&L, 0, sizeof L);
	have = 0;
	free_rows();
	cwd[0] = 0;
}

static void load_archive(const char *path, const char *pw, const char *cs)
{
	char err[512] = "";
	AcList nl;
	set_status(_("正在讀取…"));
	draw();
	if (dpy) XFlush(dpy);
	int rc = ac_open(&nl, path, pw, cs, err, sizeof err);
	if (rc == AC_NEED_PASSWORD || rc == AC_BAD_PASSWORD) {
		char wid[24]; snprintf(wid, sizeof wid, "%lu", wl ? 0UL : (unsigned long)win);
		char *argv[] = { (char *)pa_exe(), "--ui-password", (char *)path, "--parent", wid, rc == AC_BAD_PASSWORD ? "--wrong" : NULL, NULL };
		Kid *k = spawn_kid(K_PW, argv, NULL, 1);
		if (k) { snprintf(k->path, sizeof k->path, "%s", path); snprintf(k->cs, sizeof k->cs, "%s", cs ? cs : ""); status_msg[0] = 0; }
		return;
	}
	if (rc != AC_OK) {
		const char *msg = err[0] ? err : _("打不開這個檔案");
		const char *b[] = { _("好") };
		pm_ask(_("打不開"), "pomelo-archive", pa_icon, msg, b, 1, 0, 0);
		status_msg[0] = 0;
		return;
	}
	/* 資料有加密但清單看得到（zip）：先確認密碼，沒有就問 */
	if (nl.enc_data && nl.backend != AB_GPG) {
		int pr = ac_check_password(&nl, pw);
		if (pr == AC_NO_BACKEND) { const char *b[] = { _("好") }; pm_ask(_("需要額外的程式"), "pomelo-archive", pa_icon, nl.note, b, 1, 0, 0); }
		if (pr == AC_NEED_PASSWORD || pr == AC_BAD_PASSWORD) {
			char wid[24]; snprintf(wid, sizeof wid, "%lu", wl ? 0UL : (unsigned long)win);
			char *argv[] = { (char *)pa_exe(), "--ui-password", (char *)path, "--parent", wid, pr == AC_BAD_PASSWORD ? "--wrong" : NULL, NULL };
			if (!(pw == NULL && 0)) {
				Kid *k = spawn_kid(K_PW, argv, NULL, 1);
				if (k) { snprintf(k->path, sizeof k->path, "%s", path); snprintf(k->cs, sizeof k->cs, "%s", cs ? cs : ""); }
			}
			/* 先把檔名列出來讓人看；輸入密碼後會重新載入 */
		}
	}
	close_archive();
	L = nl;
	have = 1;
	snprintf(arcfile, sizeof arcfile, "%s", path);
	snprintf(arcpw, sizeof arcpw, "%s", pw ? pw : "");
	snprintf(arccs, sizeof arccs, "%s", L.charset);
	cwd[0] = 0;
	build_rows();
	set_title();
	status_msg[0] = 0;
	if (L.note[0]) set_status("%s", L.note);
}

/* ---------- 工具列與動作 ---------- */

static void a_open_dialog(void)
{
	char out[PATH_MAX];
	const char *home = getenv("HOME");
	if (pm_file_dialog(_("開啟壓縮檔"), "pomelo-archive", pa_icon, 0, home ? home : "/", NULL, NULL, 0, NULL, out, sizeof out)) load_archive(out, NULL, NULL);
}

static void sel_paths(char **out, int max, int *n)
{
	*n = 0;
	for (int i = 0; i < nrows && *n < max; i++) if (sel[i]) { char p[PATH_MAX]; row_path(&rows[i], p, sizeof p); out[(*n)++] = strdup(p); }
}

static void kid_extract_to(const char *dest, int only_sel)
{
	char *only[256];
	int no = 0;
	if (only_sel) sel_paths(only, 256, &no);
	char *argv[600];
	int k = 0;
	argv[k++] = (char *)pa_exe(); argv[k++] = "--extract-to"; argv[k++] = arcfile; argv[k++] = (char *)dest;
	if (arcpw[0]) argv[k++] = "--password-stdin";
	if (arccs[0] && L.charset_guessed) { argv[k++] = "--charset"; argv[k++] = arccs; }
	for (int i = 0; i < no; i++) { argv[k++] = "--only"; argv[k++] = only[i]; }
	argv[k] = NULL;
	spawn_kid(K_EXTRACT, argv, arcpw[0] ? arcpw : NULL, 0);
	for (int i = 0; i < no; i++) free(only[i]);
}

static void a_extract_here_all(void)
{
	if (!have) return;
	char *argv[8];
	int k = 0;
	argv[k++] = (char *)pa_exe(); argv[k++] = "--extract-here"; argv[k++] = arcfile;
	if (arcpw[0]) argv[k++] = "--password-stdin";
	if (arccs[0] && L.charset_guessed) { argv[k++] = "--charset"; argv[k++] = arccs; }
	argv[k] = NULL;
	spawn_kid(K_EXTRACT, argv, arcpw[0] ? arcpw : NULL, 0);
}

/* 常用的目的地 */
static char dests[8][PATH_MAX];
static char dest_labels[8][160];
static int ndest, dest_only_sel;

static void user_dir(const char *key, const char *fallback, char *out, size_t n)
{
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	snprintf(out, n, "%s/%s", home, fallback);
	char f[PATH_MAX], line[PATH_MAX];
	snprintf(f, sizeof f, "%s/.config/user-dirs.dirs", home);
	FILE *fp = fopen(f, "r");
	if (!fp) return;
	size_t kl = strlen(key);
	while (fgets(line, sizeof line, fp))
		if (!strncmp(line, key, kl) && line[kl] == '=') {
			char *q = strchr(line, '"');
			if (!q) continue;
			char *e = strrchr(q + 1, '"');
			if (!e) continue;
			*e = 0;
			if (!strncmp(q + 1, "$HOME", 5)) snprintf(out, n, "%s%s", home, q + 6); else snprintf(out, n, "%s", q + 1);
		}
	fclose(fp);
}

static void m_dest(const Arg *a)
{
	int i = a->i;
	if (i == 99) {   /* 其他位置：用「另存新檔」的對話窗選一個資料夾名稱（沒有的話會建立） */
		char out[PATH_MAX], base[256];
		ac_base_noext(arcfile, base, sizeof base);
		const char *home = getenv("HOME");
		int t = 0;
		if (pm_file_dialog(_("選擇解壓縮到哪個資料夾（輸入名稱，沒有就會建立）"), "pomelo-archive", pa_icon, 1, home ? home : "/", base, NULL, 0, &t, out, sizeof out)) kid_extract_to(out, dest_only_sel);
		return;
	}
	if (i >= 0 && i < ndest) kid_extract_to(dests[i], dest_only_sel);
}

static void a_extract_menu(int x, int y, int only_sel)
{
	if (!have) return;
	char dl[PATH_MAX], ds[PATH_MAX], dd[PATH_MAX], dh[PATH_MAX], here[PATH_MAX];
	user_dir("XDG_DOWNLOAD_DIR", "Downloads", dl, sizeof dl);
	user_dir("XDG_DESKTOP_DIR", "Desktop", ds, sizeof ds);
	user_dir("XDG_DOCUMENTS_DIR", "Documents", dd, sizeof dd);
	snprintf(dh, sizeof dh, "%s", getenv("HOME") ? getenv("HOME") : "/");
	snprintf(here, sizeof here, "%s", arcfile);
	char *sl = strrchr(here, '/');
	if (sl) *sl = 0;
	ndest = 0;
	struct { const char *label; const char *path; } cand[] = { { _("壓縮檔所在的資料夾"), here }, { _("下載"), dl }, { _("桌面"), ds }, { _("文件"), dd }, { _("家目錄"), dh } };
	MenuItem m[8];
	int k = 0;
	for (unsigned i = 0; i < LEN(cand); i++) {
		struct stat st;
		if (stat(cand[i].path, &st) || !S_ISDIR(st.st_mode)) continue;
		snprintf(dests[ndest], PATH_MAX, "%s", cand[i].path);
		snprintf(dest_labels[ndest], sizeof dest_labels[ndest], "%s%s", only_sel ? _("選取的項目解壓縮到：") : _("全部解壓縮到："), cand[i].label);
		m[k++] = (MenuItem){ dest_labels[ndest], m_dest, { .i = ndest } };
		ndest++;
	}
	m[k++] = (MenuItem){ _("其他位置…"), m_dest, { .i = 99 } };
	dest_only_sel = only_sel;
	PMWL_POPUP(wl, m, k, x, y);
}

static void a_add_files(char **paths, int n)
{
	if (!have || n < 1) return;
	char why[200];
	if (!ac_can_modify(&L, why, sizeof why)) {
		const char *b[] = { _("好") };
		pm_ask(_("不能加入檔案"), "pomelo-archive", pa_icon, why, b, 1, 0, 0);
		return;
	}
	char *argv[600];
	int k = 0;
	argv[k++] = (char *)pa_exe(); argv[k++] = "--add-gui"; argv[k++] = arcfile;
	if (arcpw[0]) argv[k++] = "--password-stdin";
	for (int i = 0; i < n && k < 590; i++) argv[k++] = paths[i];
	argv[k] = NULL;
	spawn_kid(K_ADD, argv, arcpw[0] ? arcpw : NULL, 0);
}

static void a_add_dialog(void)
{
	char out[PATH_MAX];
	const char *home = getenv("HOME");
	if (!have) return;
	if (pm_file_dialog(_("選擇要加入的檔案"), "pomelo-archive", pa_icon, 0, home ? home : "/", NULL, NULL, 0, NULL, out, sizeof out)) { char *p[1] = { out }; a_add_files(p, 1); }
}

static void a_test(void)
{
	if (!have) return;
	char *argv[8];
	int k = 0;
	argv[k++] = (char *)pa_exe(); argv[k++] = "--test"; argv[k++] = arcfile;
	if (arcpw[0]) argv[k++] = "--password-stdin";
	argv[k] = NULL;
	if (spawn_kid(K_TEST, argv, arcpw[0] ? arcpw : NULL, 1)) set_status(_("正在測試…"));
}

static void fmt_time(time_t t, char *out, size_t n)
{
	if (!t) { snprintf(out, n, "—"); return; }
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, n, "%Y-%m-%d %H:%M", &tm);
}

static void a_info(void)
{
	char msg[3000], sz[32], cz[32], fs[32];
	if (!have) return;
	int ns = nsel();
	if (ns == 1) {
		int i = 0;
		while (i < nrows && !sel[i]) i++;
		Row *r = &rows[i];
		char p[PATH_MAX], t[40], s1[32], s2[32] = "", mode[16] = "";
		row_path(r, p, sizeof p);
		ac_human(s1, sizeof s1, (double)r->size);
		fmt_time(r->mtime, t, sizeof t);
		const AcEntry *e = r->idx >= 0 ? &L.e[r->idx] : NULL;
		if (e && e->csize >= 0) ac_human(s2, sizeof s2, (double)e->csize);
		if (e && e->mode) snprintf(mode, sizeof mode, "%04o", e->mode & 07777);
		snprintf(msg, sizeof msg, "%s\n\n%s：%s\n%s：%s%s\n%s：%s\n%s：%s%s%s%s%s%s%s",
		         p, _("類型"), r->dir ? _("資料夾") : r->link ? _("符號連結") : _("檔案"),
		         r->dir ? _("內容") : _("大小"), r->dir ? "" : s1, r->dir ? "" : "", _("修改時間"), t, _("加密"), r->enc ? _("有") : _("沒有"),
		         s2[0] ? "\n" : "", s2[0] ? _("壓縮後：") : "", s2, mode[0] ? "\n" : "", mode[0] ? _("權限：") : "", mode);
		if (r->dir) snprintf(msg, sizeof msg, "%s\n\n%s：%s\n%s：%ld\n%s：%s\n%s：%s", p, _("類型"), _("資料夾"), _("裡面的項目"), r->nchild, _("解開後大小"), (ac_human(sz, sizeof sz, (double)r->size), sz), _("加密"), r->enc ? _("有") : _("沒有"));
		else if (e && e->link) { size_t l = strlen(msg); snprintf(msg + l, sizeof msg - l, "\n%s：%s", _("指向"), e->link); }
	} else {
		ac_human(sz, sizeof sz, (double)L.total);
		ac_human(cz, sizeof cz, (double)L.file_size);
		ac_human(fs, sizeof fs, (double)L.file_size);
		const char *be = L.backend == AB_LA ? "libarchive" : L.backend == AB_7Z ? "7-Zip" : "gpg + libarchive";
		double ratio = L.total > 0 ? 100.0 * (double)L.file_size / (double)L.total : 0;
		snprintf(msg, sizeof msg, "%s\n\n%s：%s%s%s\n%s：%zu\n%s：%s\n%s：%s（%.0f%%）\n%s：%s%s\n%s：%s\n%s：%s%s%s",
		         arcfile, _("格式"), L.fmt, L.codec[0] ? " + " : "", L.codec, _("項目"), L.n, _("解開後大小"), sz, _("壓縮檔大小"), cz, ratio,
		         _("加密"), L.enc_data ? (L.enc_names ? _("有（連檔名都加密）") : _("有")) : _("沒有"), "", _("檔名編碼"),
		         L.charset[0] ? L.charset : "UTF-8", _("讀取用的程式"), be, L.note[0] ? "\n" : "", L.note);
		(void)fs;
	}
	const char *b[] = { _("好") };
	pm_ask(_("內容資訊"), "pomelo-archive", pa_icon, msg, b, 1, 0, 0);
}

static void a_open_row(int i)
{
	Row *r = &rows[i];
	char p[PATH_MAX];
	row_path(r, p, sizeof p);
	if (r->dir) {
		snprintf(cwd, sizeof cwd, "%s", p);
		build_rows();
		return;
	}
	if (r->link) { set_status(_("符號連結：不能直接打開")); return; }
	/* 解到暫存資料夾再用預設程式打開 */
	if (ntmp >= 8) ntmp = 0;
	char td[PATH_MAX];
	if (ac_tmpdir(td, sizeof td)) { set_status(_("沒辦法建立暫存資料夾")); return; }
	snprintf(tmpdirs[ntmp], PATH_MAX, "%s/pa-open-XXXXXX", td);
	if (!mkdtemp(tmpdirs[ntmp])) { set_status(_("沒辦法建立暫存資料夾")); return; }
	char *argv[12];
	int k = 0;
	argv[k++] = (char *)pa_exe(); argv[k++] = "--extract"; argv[k++] = arcfile; argv[k++] = "-C"; argv[k++] = tmpdirs[ntmp];
	if (arcpw[0]) argv[k++] = "--password-stdin";
	if (arccs[0] && L.charset_guessed) { argv[k++] = "--charset"; argv[k++] = arccs; }
	argv[k++] = "--only"; argv[k++] = p;
	argv[k] = NULL;
	Kid *kd = spawn_kid(K_OPENFILE, argv, arcpw[0] ? arcpw : NULL, 1);
	if (kd) { snprintf(kd->path, sizeof kd->path, "%s/%s", tmpdirs[ntmp], p); set_status(_("正在解壓縮「%s」…"), r->name); }
	ntmp++;
}

static void go_up(void)
{
	char *s = strrchr(cwd, '/');
	if (s) *s = 0; else cwd[0] = 0;
	build_rows();
}

/* ---------- 剪貼簿（拷貝路徑） ---------- */

static void clip_set(const char *t)
{
	free(clip_text);
	clip_text = strdup(t);
	if (wl) { pmwl_clip_set(t); pmwl_primary_set(t); return; }
	XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
	XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
}

static void on_selrequest(XSelectionRequestEvent *e)
{
	XSelectionEvent r = { .type = SelectionNotify, .display = e->display, .requestor = e->requestor, .selection = e->selection,
	                      .target = e->target, .property = e->property ? e->property : e->target, .time = e->time };
	if (!clip_text) r.property = None;
	else if (e->target == a_targets) {
		Atom t[] = { a_targets, a_utf8, a_string };
		XChangeProperty(dpy, e->requestor, r.property, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
	} else if (e->target == a_utf8 || e->target == a_string) {
		XChangeProperty(dpy, e->requestor, r.property, e->target, 8, PropModeReplace, (unsigned char *)clip_text, (int)strlen(clip_text));
	} else r.property = None;
	XSendEvent(dpy, e->requestor, False, 0, (XEvent *)&r);
}

static void m_copypath(const Arg *a)
{
	(void)a;
	char buf[8192] = "";
	size_t l = 0;
	for (int i = 0; i < nrows; i++) if (sel[i]) { char p[PATH_MAX]; row_path(&rows[i], p, sizeof p); l += (size_t)snprintf(buf + l, sizeof buf - l, "%s%s", l ? "\n" : "", p); }
	if (!buf[0]) snprintf(buf, sizeof buf, "%s", arcfile);
	clip_set(buf);
	set_status(_("已拷貝路徑"));
}

/* ---------- 選單 ---------- */

static int menu_x, menu_y;
static void m_open(const Arg *a) { (void)a; if (cursor >= 0) a_open_row(cursor); }
static void m_extract_sel(const Arg *a) { (void)a; a_extract_menu(menu_x, menu_y, 1); }
static void m_extract_all(const Arg *a) { (void)a; a_extract_menu(menu_x, menu_y, 0); }
static void m_extract_here(const Arg *a) { (void)a; a_extract_here_all(); }
static void m_info(const Arg *a) { (void)a; a_info(); }
static void m_test(const Arg *a) { (void)a; a_test(); }
static void m_add(const Arg *a) { (void)a; a_add_dialog(); }
static void m_reload(const Arg *a) { (void)a; if (have) { char f[PATH_MAX], p[256], c[16]; snprintf(f, sizeof f, "%s", arcfile); snprintf(p, sizeof p, "%s", arcpw); snprintf(c, sizeof c, "%s", arccs); load_archive(f, p[0] ? p : NULL, c[0] && L.charset_guessed ? c : NULL); } }
static void m_openarc(const Arg *a) { (void)a; a_open_dialog(); }
static void m_up(const Arg *a) { (void)a; go_up(); }
static void m_charset(const Arg *a)
{
	const char *cs = ac_charsets(a->i);
	if (!cs || !have) return;
	char f[PATH_MAX], p[256];
	snprintf(f, sizeof f, "%s", arcfile);
	snprintf(p, sizeof p, "%s", arcpw);
	load_archive(f, p[0] ? p : NULL, cs);
}

static void context_menu(int x, int y, int on_row, int rx, int ry)
{
	menu_x = rx + 2; menu_y = ry + 2;
	MenuItem m[12];
	int k = 0;
	if (on_row) {
		m[k++] = (MenuItem){ rows[cursor].dir ? _("進入") : _("打開"), m_open, { 0 } };
		m[k++] = (MenuItem){ NULL, NULL, { 0 } };
		m[k++] = (MenuItem){ _("解壓縮選取的項目到…"), m_extract_sel, { 0 } };
		m[k++] = (MenuItem){ _("解壓縮全部到…"), m_extract_all, { 0 } };
		m[k++] = (MenuItem){ NULL, NULL, { 0 } };
		m[k++] = (MenuItem){ _("拷貝路徑"), m_copypath, { 0 } };
		m[k++] = (MenuItem){ _("內容資訊"), m_info, { 0 } };
	} else if (have) {
		m[k++] = (MenuItem){ _("解壓縮全部到此處"), m_extract_here, { 0 } };
		m[k++] = (MenuItem){ _("解壓縮全部到…"), m_extract_all, { 0 } };
		m[k++] = (MenuItem){ NULL, NULL, { 0 } };
		m[k++] = (MenuItem){ _("加入檔案…"), m_add, { 0 } };
		m[k++] = (MenuItem){ _("測試壓縮檔"), m_test, { 0 } };
		m[k++] = (MenuItem){ NULL, NULL, { 0 } };
		if (cwd[0]) m[k++] = (MenuItem){ _("上一層"), m_up, { 0 } };
		m[k++] = (MenuItem){ _("重新載入"), m_reload, { 0 } };
		m[k++] = (MenuItem){ _("內容資訊"), m_info, { 0 } };
	} else {
		m[k++] = (MenuItem){ _("開啟壓縮檔…"), m_openarc, { 0 } };
	}
	(void)x; (void)y;
	PMWL_POPUP(wl, m, k, rx + 2, ry + 2);
}

/* ---------- 畫面 ---------- */

typedef struct { char name[64]; int size; cairo_surface_t *s; } IC;
static IC icache[48];
static int nic;

static cairo_surface_t *theme_icon(const char *name, int size)
{
	for (int i = 0; i < nic; i++) if (icache[i].size == size && !strcmp(icache[i].name, name)) return icache[i].s;
	cairo_surface_t *s = pm_icon_load(name, size);
	if (nic < 48) { snprintf(icache[nic].name, sizeof icache[nic].name, "%s", name); icache[nic].size = size; icache[nic].s = s; nic++; }
	return s;
}

static const char *icon_name_for(const Row *r)
{
	if (r->dir) return "folder";
	const char *d = strrchr(r->name, '.');
	if (!d) return "text-x-generic";
	d++;
	static const struct { const char *ext, *icon; } m[] = {
		{ "png", "image-x-generic" }, { "jpg", "image-x-generic" }, { "jpeg", "image-x-generic" }, { "gif", "image-x-generic" }, { "webp", "image-x-generic" },
		{ "bmp", "image-x-generic" }, { "svg", "image-x-generic" }, { "mp3", "audio-x-generic" }, { "wav", "audio-x-generic" }, { "flac", "audio-x-generic" },
		{ "ogg", "audio-x-generic" }, { "m4a", "audio-x-generic" }, { "mp4", "video-x-generic" }, { "mkv", "video-x-generic" }, { "avi", "video-x-generic" },
		{ "mov", "video-x-generic" }, { "webm", "video-x-generic" }, { "pdf", "application-pdf" }, { "zip", "package-x-generic" }, { "7z", "package-x-generic" },
		{ "gz", "package-x-generic" }, { "xz", "package-x-generic" }, { "zst", "package-x-generic" }, { "tar", "package-x-generic" },
		{ "deb", "package-x-generic" }, { "rpm", "package-x-generic" }, { "doc", "x-office-document" }, { "docx", "x-office-document" }, { "odt", "x-office-document" },
		{ "xls", "x-office-spreadsheet" }, { "xlsx", "x-office-spreadsheet" }, { "ods", "x-office-spreadsheet" }, { "ppt", "x-office-presentation" },
		{ "pptx", "x-office-presentation" }, { "odp", "x-office-presentation" }, { "sh", "text-x-script" }, { "py", "text-x-script" }, { "exe", "application-x-executable" } };
	for (unsigned i = 0; i < LEN(m); i++) if (!strcasecmp(d, m[i].ext)) return m[i].icon;
	return "text-x-generic";
}

/* 找不到圖示主題時自己畫的圖示 */
static void glyph_file(cairo_t *cr, double x, double y, int dir, int link)
{
	if (dir) {
		pm_rrect(cr, x, y + 3, 9, 4, 2, 0);
		pm_rgb(cr, 0x5aa9f7); cairo_fill(cr);
		pm_rrect(cr, x, y + 5, 20, 14, 3, 3);
		pm_rgb(cr, 0x3b8be8); cairo_fill(cr);
	} else {
		pm_rrect(cr, x + 3, y + 1, 14, 18, 2, 2);
		pm_rgb(cr, pm_dark ? 0x6e6e73 : 0xf4f4f6); cairo_fill_preserve(cr);
		pm_rgb(cr, pm_dark ? 0x8e8e93 : 0xb5b5ba); cairo_set_line_width(cr, 1); cairo_stroke(cr);
		if (link) { pm_rgb(cr, 0x3b8be8); cairo_move_to(cr, x + 7, y + 13); cairo_line_to(cr, x + 13, y + 7); cairo_set_line_width(cr, 2); cairo_stroke(cr); }
		else { pm_rgb(cr, pm_dark ? 0x8e8e93 : 0xc8c8cd); for (int i = 0; i < 3; i++) { cairo_rectangle(cr, x + 6, y + 6 + i * 4, 8, 1.5); cairo_fill(cr); } }
	}
}

static void draw_row_icon(cairo_t *cr, const Row *r, double x, double y)
{
	cairo_surface_t *s = theme_icon(icon_name_for(r), 22);
	if (s) {
		cairo_save(cr);
		cairo_translate(cr, x - 1, y - 1);
		double k = 22.0 / cairo_image_surface_get_width(s);
		cairo_scale(cr, k, k);
		cairo_set_source_surface(cr, s, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	} else glyph_file(cr, x, y, r->dir, r->link);
	if (r->enc) {   /* 小鎖頭 */
		pm_rrect(cr, x + 12, y + 11, 9, 8, 2, 2);
		pm_rgb(cr, 0xf0a020); cairo_fill(cr);
		cairo_arc(cr, x + 16.5, y + 11, 3, M_PI, 2 * M_PI);
		cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
	}
}

static void glyph_btn(cairo_t *cr, int kind, double x, double y, unsigned col)
{
	cairo_new_path(cr);
	pm_rgb(cr, col);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (kind) {
	case 0:   /* 開啟：資料夾 */
		cairo_move_to(cr, x + 2, y + 6); cairo_line_to(cr, x + 2, y + 18); cairo_line_to(cr, x + 20, y + 18); cairo_line_to(cr, x + 22, y + 9);
		cairo_line_to(cr, x + 10, y + 9); cairo_line_to(cr, x + 8, y + 5); cairo_line_to(cr, x + 3, y + 5); cairo_close_path(cr); cairo_stroke(cr);
		break;
	case 1:   /* 解壓縮：往外的箭頭＋盤子 */
		cairo_move_to(cr, x + 11, y + 3); cairo_line_to(cr, x + 11, y + 14); cairo_stroke(cr);
		cairo_move_to(cr, x + 6, y + 10); cairo_line_to(cr, x + 11, y + 15); cairo_line_to(cr, x + 16, y + 10); cairo_stroke(cr);
		cairo_move_to(cr, x + 3, y + 18); cairo_line_to(cr, x + 3, y + 21); cairo_line_to(cr, x + 19, y + 21); cairo_line_to(cr, x + 19, y + 18); cairo_stroke(cr);
		break;
	case 2:   /* 新增：加號 */
		cairo_move_to(cr, x + 11, y + 4); cairo_line_to(cr, x + 11, y + 20); cairo_stroke(cr);
		cairo_move_to(cr, x + 3, y + 12); cairo_line_to(cr, x + 19, y + 12); cairo_stroke(cr);
		break;
	case 3:   /* 測試：盾牌打勾 */
		cairo_move_to(cr, x + 11, y + 2); cairo_line_to(cr, x + 19, y + 5); cairo_line_to(cr, x + 19, y + 12); cairo_curve_to(cr, x + 19, y + 17, x + 15, y + 20, x + 11, y + 22);
		cairo_curve_to(cr, x + 7, y + 20, x + 3, y + 17, x + 3, y + 12); cairo_line_to(cr, x + 3, y + 5); cairo_close_path(cr); cairo_stroke(cr);
		cairo_move_to(cr, x + 7, y + 12); cairo_line_to(cr, x + 10, y + 15); cairo_line_to(cr, x + 15, y + 9); cairo_stroke(cr);
		break;
	case 4:   /* 資訊：圓圈 i */
		cairo_new_path(cr);
		cairo_arc(cr, x + 11, y + 12, 9, 0, 2 * M_PI); cairo_stroke(cr);
		cairo_move_to(cr, x + 11, y + 11); cairo_line_to(cr, x + 11, y + 17); cairo_stroke(cr);
		cairo_new_path(cr);
		cairo_arc(cr, x + 11, y + 7.5, 0.9, 0, 2 * M_PI); cairo_fill(cr);
		break;
	}
}

static void draw_toolbar(cairo_t *cr)
{
	pm_rgb(cr, c_tool);
	cairo_rectangle(cr, 0, 0, W, TB_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, 0, TB_H - 1, W, 1);
	cairo_fill(cr);
	static const char *labels[5];
	labels[0] = _("開啟"); labels[1] = _("解壓縮"); labels[2] = _("加入"); labels[3] = _("測試"); labels[4] = _("資訊");
	pm_font(cr, pm_size, 0);
	double x = PAD;
	nbtn = 5;
	for (int i = 0; i < 5; i++) {
		int enabled = i == 0 || have;
		double tw = pm_textw(cr, labels[i]);
		double w = 34 + tw + 10, h = 38, y = (TB_H - h) / 2;
		r_btn[i] = (R){ (int)x, (int)y, (int)w, (int)h };
		if (hover_btn == i && enabled) {
			pm_rrect(cr, x, y, w, h, 8, 8);
			pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.08);
			cairo_fill(cr);
		}
		unsigned col = enabled ? (i == 1 ? T->sel : c_text) : c_dim;
		glyph_btn(cr, i, x + 8, y + 7, col);
		pm_rgb(cr, enabled ? c_text : c_dim);
		pm_text(cr, x + 34, y + h / 2, labels[i]);
		x += w + 4;
	}
	/* 右邊：加密與編碼的標示 */
	double rx = W - PAD;
	r_cs = (R){ 0, 0, 0, 0 };
	if (have) {
		if (L.enc_data) {
			const char *t = L.enc_names ? _("已加密（含檔名）") : _("已加密");
			double tw = pm_textw(cr, t);
			rx -= tw;
			pm_rgb(cr, c_warn);
			pm_text(cr, rx, TB_H / 2, t);
			pm_rrect(cr, rx - 22, TB_H / 2 - 4, 12, 10, 2, 2);
			cairo_fill(cr);
			cairo_arc(cr, rx - 16, TB_H / 2 - 4, 4, M_PI, 2 * M_PI);
			cairo_set_line_width(cr, 1.8); cairo_stroke(cr);
			rx -= 34;
		}
		if (L.backend != AB_7Z) {
			char t[80];
			snprintf(t, sizeof t, "%s：%s%s", _("檔名編碼"), L.charset[0] ? L.charset : "UTF-8", L.charset_guessed ? _("（自動偵測）") : "");
			double tw = pm_textw(cr, t);
			r_cs = (R){ (int)(rx - tw - 16), (TB_H - 28) / 2, (int)tw + 16, 28 };
			pm_rrect(cr, r_cs.x, r_cs.y, r_cs.w, r_cs.h, 6, 6);
			pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.06);
			cairo_fill(cr);
			pm_rgb(cr, c_dim);
			pm_text(cr, r_cs.x + 8, TB_H / 2, t);
		}
	}
}

static void draw_pathbar(cairo_t *cr)
{
	int y0 = TB_H;
	pm_rgb(cr, c_bg);
	cairo_rectangle(cr, 0, y0, W, PATH_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, 0, y0 + PATH_H - 1, W, 1);
	cairo_fill(cr);
	if (!have) return;
	pm_font(cr, pm_size, 0);
	double x = PAD;
	ncrumb = 0;
	/* 上一層 */
	r_path_up = (R){ (int)x, y0 + 5, 28, PATH_H - 10 };
	pm_rgb(cr, cwd[0] ? c_text : c_dim);
	cairo_set_line_width(cr, 1.8);
	cairo_move_to(cr, x + 16, y0 + PATH_H / 2 - 5); cairo_line_to(cr, x + 10, y0 + PATH_H / 2); cairo_line_to(cr, x + 16, y0 + PATH_H / 2 + 5);
	cairo_stroke(cr);
	x += 34;
	/* 麵包屑：壓縮檔名 › 資料夾 › … */
	const char *b = strrchr(arcfile, '/');
	b = b ? b + 1 : arcfile;
	char seg[PATH_MAX], accum[PATH_MAX] = "";
	snprintf(seg, sizeof seg, "%s", b);
	for (int part = 0;; part++) {
		char shown[160];
		pm_ellipsize(cr, seg, 220, shown, sizeof shown);
		double w = pm_textw(cr, shown);
		int last = 0;
		const char *rest = NULL;
		if (part == 0) rest = cwd[0] ? cwd : NULL;
		pm_rgb(cr, rest ? T->sel : c_text);
		if (part > 0 && !rest) last = 1;
		r_crumb[ncrumb] = (R){ (int)x - 4, y0 + 4, (int)w + 8, PATH_H - 8 };
		snprintf(crumb_path[ncrumb], PATH_MAX, "%s", accum);
		ncrumb++;
		pm_text(cr, x, y0 + PATH_H / 2, shown);
		x += w;
		(void)last;
		/* 下一段 */
		static const char *cursor_rest;
		if (part == 0) cursor_rest = cwd;
		if (!cursor_rest || !*cursor_rest || ncrumb >= 31) break;
		const char *sl = strchr(cursor_rest, '/');
		size_t n = sl ? (size_t)(sl - cursor_rest) : strlen(cursor_rest);
		snprintf(seg, sizeof seg, "%.*s", (int)n, cursor_rest);
		size_t al = strlen(accum);
		snprintf(accum + al, sizeof accum - al, "%s%.*s", al ? "/" : "", (int)n, cursor_rest);
		cursor_rest = sl ? sl + 1 : NULL;
		pm_rgb(cr, c_dim);
		pm_text(cr, x + 8, y0 + PATH_H / 2, "›");
		x += 26;
		if (x > W - 100) break;
	}
}

static void draw_header(cairo_t *cr, int showc, int cw_size, int cw_csize, int cw_time)
{
	int y0 = TB_H + PATH_H;
	pm_rgb(cr, c_tool);
	cairo_rectangle(cr, 0, y0, W, HEAD_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, 0, y0 + HEAD_H - 1, W, 1);
	cairo_fill(cr);
	pm_font(cr, pm_size - 1, 0);
	static const char *names[4];
	names[0] = _("名稱"); names[1] = _("大小"); names[2] = _("壓縮後"); names[3] = _("修改時間");
	double x[4] = { PAD + 32, W - SBW - PAD - cw_time - cw_csize - cw_size, W - SBW - PAD - cw_time - cw_csize, W - SBW - PAD - cw_time };
	for (int i = 0; i < 4; i++) {
		if (i == 2 && !showc) { r_hdr[i] = (R){ 0, 0, 0, 0 }; continue; }
		double w = i == 0 ? x[1] - x[0] : i == 1 ? cw_size : i == 2 ? cw_csize : cw_time;
		r_hdr[i] = (R){ (int)(i == 0 ? PAD : x[i]), y0, (int)(i == 0 ? w + 32 : w), HEAD_H };
		pm_rgb(cr, sortcol == i ? c_text : c_dim);
		if (i == 0) pm_text(cr, x[0], y0 + HEAD_H / 2, names[i]);
		else if (i == 3) pm_text(cr, x[i] + 8, y0 + HEAD_H / 2, names[i]);
		else pm_text(cr, x[i] + w - 12 - pm_textw(cr, names[i]), y0 + HEAD_H / 2, names[i]);
		if (sortcol == i) {
			double ax = i == 0 ? x[0] + pm_textw(cr, names[i]) + 10 : i == 3 ? x[i] + 8 + pm_textw(cr, names[i]) + 10 : x[i] + w - 12 - pm_textw(cr, names[i]) - 10;
			double ay = y0 + HEAD_H / 2;
			cairo_move_to(cr, ax - 3.5, ay + (sortdesc ? -2 : 2)); cairo_line_to(cr, ax, ay + (sortdesc ? 2 : -2)); cairo_line_to(cr, ax + 3.5, ay + (sortdesc ? -2 : 2));
			cairo_set_line_width(cr, 1.5); cairo_stroke(cr);
		}
	}
}

static void draw_list(cairo_t *cr)
{
	int y0 = TB_H + PATH_H + HEAD_H;
	int vh = H - y0 - ST_H;
	r_list = (R){ 0, y0, W, vh };
	pm_rgb(cr, c_bg);
	cairo_rectangle(cr, 0, y0, W, vh);
	cairo_fill(cr);
	int showc = has_csize() && W > 700;
	int cw_size = 96, cw_csize = showc ? 96 : 0, cw_time = W > 560 ? 150 : 0;
	draw_header(cr, showc, cw_size, cw_csize, cw_time);
	int total = nrows * ROW_H;
	int maxs = MAX(0, total - vh);
	scrolly = CLAMP(scrolly, 0, maxs);
	cairo_save(cr);
	cairo_rectangle(cr, 0, y0, W, vh);
	cairo_clip(cr);
	pm_font(cr, pm_size, 0);
	int first = scrolly / ROW_H, last = MIN(nrows - 1, (scrolly + vh) / ROW_H);
	for (int i = first; i <= last; i++) {
		double y = y0 + i * ROW_H - scrolly;
		Row *r = &rows[i];
		if (sel[i]) { pm_rgb(cr, focused ? T->sel : c_selin); cairo_rectangle(cr, 0, y, W, ROW_H); cairo_fill(cr); }
		else if (i == hover_row) { pm_rgb(cr, c_hl); cairo_rectangle(cr, 0, y, W, ROW_H); cairo_fill(cr); }
		else if (i % 2) { pm_rgb(cr, c_stripe); cairo_rectangle(cr, 0, y, W, ROW_H); cairo_fill(cr); }
		if (i == cursor && !sel[i]) { pm_rgb(cr, T->sel); cairo_set_line_width(cr, 1); cairo_rectangle(cr, 1.5, y + 1.5, W - 3, ROW_H - 3); cairo_stroke(cr); }
		unsigned tc = sel[i] && focused ? 0xffffff : c_text, dc = sel[i] && focused ? 0xe8e8e8 : c_dim;
		draw_row_icon(cr, r, PAD, y + (ROW_H - 20) / 2.0);
		double nx = PAD + 32;
		double maxw = W - SBW - PAD - cw_time - cw_csize - cw_size - nx - 8;
		char shown[PATH_MAX];
		pm_ellipsize(cr, r->name, MAX(40, maxw), shown, sizeof shown);
		pm_rgb(cr, tc);
		pm_text(cr, nx, y + ROW_H / 2.0, shown);
		char b[48];
		if (r->dir) snprintf(b, sizeof b, r->nchild ? _("%ld 項") : "—", r->nchild);
		else if (r->size >= 0) ac_human(b, sizeof b, (double)r->size); else snprintf(b, sizeof b, "—");
		pm_rgb(cr, dc);
		double xs = W - SBW - PAD - cw_time - cw_csize;
		pm_text(cr, xs - 12 - pm_textw(cr, b), y + ROW_H / 2.0, b);
		if (showc) {
			if (r->csize >= 0 && !r->dir) ac_human(b, sizeof b, (double)r->csize); else snprintf(b, sizeof b, "—");
			pm_text(cr, xs + cw_csize - 12 - pm_textw(cr, b), y + ROW_H / 2.0, b);
		}
		if (cw_time) { fmt_time(r->mtime, b, sizeof b); pm_text(cr, W - SBW - PAD - cw_time + 8, y + ROW_H / 2.0, b); }
	}
	cairo_restore(cr);
	if (total > vh) {
		double bh = MAX(30.0, (double)vh * vh / total), by = y0 + (vh - bh) * scrolly / maxs;
		pm_rrect(cr, W - SBW + 1, by, SBW - 3, bh, 3, 3);
		pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.28);
		cairo_fill(cr);
	}
	if (!nrows) {
		pm_font(cr, pm_size + 1, 0);
		pm_rgb(cr, c_dim);
		const char *t = have ? _("這個資料夾是空的") : "";
		pm_text(cr, (W - pm_textw(cr, t)) / 2.0, y0 + 60, t);
	}
}

static void draw_empty(cairo_t *cr)
{
	int y0 = TB_H + PATH_H;
	pm_rgb(cr, c_bg);
	cairo_rectangle(cr, 0, y0, W, H - y0 - ST_H);
	cairo_fill(cr);
	double cx = W / 2.0, cy = y0 + (H - y0 - ST_H) / 2.0 - 30;
	cairo_surface_t *ic = pa_icon(112);
	cairo_set_source_surface(cr, ic, cx - 56, cy - 100);
	cairo_paint(cr);
	cairo_surface_destroy(ic);
	pm_font(cr, pm_size + 5, 1);
	pm_rgb(cr, c_text);
	const char *t1 = _("把壓縮檔拖到這裡");
	pm_text(cr, cx - pm_textw(cr, t1) / 2, cy + 36, t1);
	pm_font(cr, pm_size, 0);
	pm_rgb(cr, c_dim);
	const char *l1 = _("或按左上角的「開啟」選一個壓縮檔");
	const char *l2 = _("把一般的檔案或資料夾拖到這裡，會幫你建立新的壓縮檔");
	const char *l3 = _("支援 zip、7z、tar、gz、xz、zst、cpio、iso、squashfs、img 等；可以加密");
	pm_text(cr, cx - pm_textw(cr, l1) / 2, cy + 68, l1);
	pm_text(cr, cx - pm_textw(cr, l2) / 2, cy + 92, l2);
	pm_text(cr, cx - pm_textw(cr, l3) / 2, cy + 116, l3);
}

static void draw_status(cairo_t *cr)
{
	int y0 = H - ST_H;
	pm_rgb(cr, c_tool);
	cairo_rectangle(cr, 0, y0, W, ST_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, 0, y0, W, 1);
	cairo_fill(cr);
	pm_font(cr, pm_size - 1, 0);
	char t[400];
	if (status_msg[0] && pm_now_ms() < status_until) snprintf(t, sizeof t, "%s", status_msg);
	else if (have) {
		char sz[32], fs[32];
		ac_human(sz, sizeof sz, (double)L.total);
		ac_human(fs, sizeof fs, (double)L.file_size);
		int ns = nsel();
		if (ns) snprintf(t, sizeof t, _("已選 %d 項（共 %d 項）　%s %s%s%s　解開後 %s　檔案 %s"), ns, nrows, L.fmt, L.codec[0] ? "+ " : "", L.codec, "", sz, fs);
		else snprintf(t, sizeof t, _("%d 項　%s%s%s　解開後 %s　檔案 %s"), nrows, L.fmt, L.codec[0] ? " + " : "", L.codec, sz, fs);
	} else snprintf(t, sizeof t, "%s", _("沒有開啟壓縮檔"));
	pm_rgb(cr, c_dim);
	pm_text(cr, PAD, y0 + ST_H / 2.0 + 1, t);
}

static void draw(void)
{
	Paint p;
	cairo_t *cr = wl ? pmwl_begin(wl, &W, &H) : pm_paint_begin(&p, win, W, H);
	pm_rgb(cr, c_bg);
	cairo_paint(cr);
	if (have) { draw_pathbar(cr); draw_list(cr); } else { draw_pathbar(cr); draw_empty(cr); }
	draw_toolbar(cr);
	draw_status(cr);
	if (wl) { pmwl_end(wl, cr); return; }
	pm_paint_end(&p, cr, 0, 0);
}

/* ---------- 輸入 ---------- */

static int row_at(int y)
{
	if (y < r_list.y || y >= r_list.y + r_list.h) return -1;
	int i = (y - r_list.y + scrolly) / ROW_H;
	return i >= 0 && i < nrows ? i : -1;
}

static void ensure_visible(int i)
{
	if (i < 0) return;
	if (i * ROW_H < scrolly) scrolly = i * ROW_H;
	else if ((i + 1) * ROW_H > scrolly + r_list.h) scrolly = (i + 1) * ROW_H - r_list.h;
}

static void select_range(int a, int b, int additive)
{
	if (!additive) clear_sel();
	if (a > b) { int t = a; a = b; b = t; }
	for (int i = a; i <= b; i++) sel[i] = 1;
}

static void click_row(int i, int state, int dbl)
{
	if (i < 0) { clear_sel(); cursor = anchor = -1; return; }
	if (state & ShiftMask && anchor >= 0) select_range(anchor, i, state & ControlMask);
	else if (state & ControlMask) { sel[i] = !sel[i]; anchor = i; }
	else { clear_sel(); sel[i] = 1; anchor = i; }
	cursor = i;
	if (dbl && !(state & (ShiftMask | ControlMask))) a_open_row(i);
}

static void sort_by(int col)
{
	if (sortcol == col) sortdesc = !sortdesc; else { sortcol = col; sortdesc = col == 3 || col == 1; }
	/* 記住選取（用名稱） */
	char (*keep)[256] = NULL;
	int nk = 0;
	if (sel) { keep = malloc((size_t)nrows * 256 + 256); for (int i = 0; i < nrows; i++) if (sel[i]) snprintf(keep[nk++], 256, "%s", rows[i].name); }
	qsort(rows, (size_t)nrows, sizeof *rows, row_cmp);
	if (sel) {
		memset(sel, 0, (size_t)nrows);
		for (int i = 0; i < nrows; i++) for (int k = 0; k < nk; k++) if (!strcmp(rows[i].name, keep[k])) { sel[i] = 1; break; }
	}
	free(keep);
	cursor = anchor = -1;
}

static void bpress(XButtonEvent *e)
{
	hover_row = -1;
	if (e->button == Button4 || e->button == Button5) {
		scrolly += e->button == Button4 ? -3 * ROW_H : 3 * ROW_H;
		return;
	}
	/* 工具列 */
	if (e->y < TB_H) {
		if (e->button != Button1) return;
		for (int i = 0; i < nbtn; i++) if (in_r(r_btn[i], e->x, e->y) && (i == 0 || have)) {
			int rx = e->x_root - (e->x - r_btn[i].x), ry = e->y_root - e->y + TB_H - 4;
			switch (i) {
			case 0: a_open_dialog(); break;
			case 1: {
				menu_x = rx; menu_y = ry;
				MenuItem m[5];
				int k = 0;
				m[k++] = (MenuItem){ _("全部解壓縮到此處"), m_extract_here, { 0 } };
				m[k++] = (MenuItem){ _("全部解壓縮到…"), m_extract_all, { 0 } };
				m[k++] = nsel() ? (MenuItem){ _("選取的項目解壓縮到…"), m_extract_sel, { 0 } } : (MenuItem){ _("選取的項目解壓縮到…"), pm_menu_off, { 0 } };
				PMWL_POPUP(wl, m, k, rx, ry);
				break;
			}
			case 2: a_add_dialog(); break;
			case 3: a_test(); break;
			case 4: a_info(); break;
			}
			return;
		}
		if (have && in_r(r_cs, e->x, e->y)) {
			MenuItem m[12];
			char lab[12][40];
			int k = 0;
			for (int i = 0; ac_charsets(i) && k < 12; i++) {
				snprintf(lab[k], sizeof lab[k], "%c%s", !strcmp(ac_charsets(i), L.charset[0] ? L.charset : "UTF-8") ? '\1' : '\2', ac_charsets(i));
				m[k] = (MenuItem){ lab[k], m_charset, { .i = i } };
				k++;
			}
			PMWL_POPUP(wl, m, k, e->x_root - 120, e->y_root + 14);
		}
		return;
	}
	/* 路徑列 */
	if (e->y < TB_H + PATH_H) {
		if (!have || e->button != Button1) return;
		if (in_r(r_path_up, e->x, e->y) && cwd[0]) { go_up(); return; }
		for (int i = 0; i < ncrumb; i++) if (in_r(r_crumb[i], e->x, e->y)) { snprintf(cwd, sizeof cwd, "%s", crumb_path[i]); build_rows(); return; }
		return;
	}
	if (!have) {
		if (e->button == Button3) context_menu(e->x, e->y, 0, e->x_root, e->y_root);
		return;
	}
	/* 表頭 */
	if (e->y < TB_H + PATH_H + HEAD_H) {
		if (e->button != Button1) return;
		for (int i = 0; i < 4; i++) if (r_hdr[i].w && in_r(r_hdr[i], e->x, e->y)) { sort_by(i); return; }
		return;
	}
	/* 清單 */
	int i = row_at(e->y);
	if (e->button == Button1) {
		long now = pm_now_ms();
		int dbl = i >= 0 && i == last_click_row && now - last_click_ms < 450;
		last_click_ms = now;
		last_click_row = dbl ? -1 : i;
		click_row(i, (int)e->state, dbl);
	} else if (e->button == Button3) {
		if (i >= 0 && !sel[i]) { clear_sel(); sel[i] = 1; anchor = cursor = i; }
		else if (i >= 0) cursor = i;
		if (i < 0) clear_sel();
		draw();
		context_menu(e->x, e->y, i >= 0, e->x_root, e->y_root);
	}
}

static void bmotion(XMotionEvent *e)
{
	int hr = -1, hb = -1;
	if (e->y < TB_H) { for (int i = 0; i < nbtn; i++) if (in_r(r_btn[i], e->x, e->y)) hb = i; }
	else if (have && e->y >= r_list.y && e->y < r_list.y + r_list.h) hr = row_at(e->y);
	if (hr != hover_row || hb != hover_btn) { hover_row = hr; hover_btn = hb; draw(); }
}

static void keypress(XKeyEvent *e)
{
	KeySym k = NoSymbol, ks = NoSymbol;
	char kb[32];
	if (pmwl_xkey(e, kb, sizeof kb - 1, &ks, &k) < 0) k = XLookupKeysym(e, 0);   /* 原生 Wayland 轉出來的事件另外取 keysym */
	int ctrl = e->state & (ControlMask | Mod4Mask), shift = e->state & ShiftMask, alt = e->state & Mod1Mask;
	if (ctrl) {
		switch (k) {
		case XK_o: a_open_dialog(); return;
		case XK_w: case XK_q: exit(0);
		case XK_a: if (have) { for (int i = 0; i < nrows; i++) sel[i] = 1; } return;
		case XK_c: if (have) m_copypath(NULL); return;
		case XK_e: a_extract_here_all(); return;
		case XK_t: a_test(); return;
		case XK_i: a_info(); return;
		case XK_r: m_reload(NULL); return;
		case XK_Up: go_up(); return;
		case XK_Down: if (cursor >= 0) a_open_row(cursor); return;
		}
		return;
	}
	if (!have) return;
	if (alt && k == XK_Up) { go_up(); return; }
	int page = MAX(1, r_list.h / ROW_H - 1);
	int to = cursor;
	switch (k) {
	case XK_Up: to = cursor < 0 ? nrows - 1 : cursor - 1; break;
	case XK_Down: to = cursor < 0 ? 0 : cursor + 1; break;
	case XK_Page_Up: to = MAX(0, (cursor < 0 ? 0 : cursor) - page); break;
	case XK_Page_Down: to = MIN(nrows - 1, (cursor < 0 ? 0 : cursor) + page); break;
	case XK_Home: to = 0; break;
	case XK_End: to = nrows - 1; break;
	case XK_BackSpace: go_up(); return;
	case XK_Return: case XK_KP_Enter: if (cursor >= 0) a_open_row(cursor); return;
	case XK_Escape: clear_sel(); return;
	case XK_F5: m_reload(NULL); return;
	default: return;
	}
	if (nrows < 1) return;
	to = CLAMP(to, 0, nrows - 1);
	if (shift && anchor >= 0) select_range(anchor, to, 0);
	else { clear_sel(); sel[to] = 1; anchor = to; }
	cursor = to;
	ensure_visible(to);
}

/* ---------- 拖放 ---------- */

static void on_dropped(char **paths, int n)
{
	if (n == 1 && ac_is_archive_name(paths[0]) && (!have || strcmp(paths[0], arcfile))) { load_archive(paths[0], NULL, NULL); return; }
	if (!have) {
		/* 一般檔案：建立新的壓縮檔 */
		char *argv[600];
		int k = 0;
		argv[k++] = (char *)pa_exe(); argv[k++] = "--compress";
		for (int i = 0; i < n && k < 590; i++) argv[k++] = paths[i];
		argv[k] = NULL;
		spawn_kid(K_EXTRACT, argv, NULL, 0);
		return;
	}
	a_add_files(paths, n);
}

/* ---------- 收尾 ---------- */

static int rm_cb(const char *p, const struct stat *st, int f, struct FTW *w) { (void)st; (void)f; (void)w; return remove(p); }

static void cleanup(void)
{
	for (int i = 0; i < ntmp; i++) if (tmpdirs[i][0]) nftw(tmpdirs[i], rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static int xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static void set_wm_icon(void)
{
	int sizes[] = { 32, 64 };
	size_t total = 0;
	for (unsigned i = 0; i < LEN(sizes); i++) total += 2 + (size_t)sizes[i] * sizes[i];
	unsigned long *d = malloc(total * sizeof *d), *p = d;
	for (unsigned i = 0; i < LEN(sizes); i++) {
		int n = sizes[i];
		cairo_surface_t *s = pa_icon(n);
		cairo_surface_flush(s);
		unsigned char *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		*p++ = (unsigned long)n; *p++ = (unsigned long)n;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				uint32_t v = ((uint32_t *)(px + y * stride))[x];
				unsigned a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
				if (a && a < 255) { r = r * 255 / a; g = g * 255 / a; b = b * 255 / a; }
				*p++ = ((unsigned long)a << 24) | (r << 16) | (g << 8) | b;
			}
		cairo_surface_destroy(s);
	}
	XChangeProperty(dpy, win, a_neticon, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)d, (int)total);
	free(d);
}

static void handle(XEvent *ev)
{
	if (!wl) {
		char **dropped = NULL;
		int nd = pm_dnd_event(dpy, ev, &dropped);
		if (nd > 0) { on_dropped(dropped, nd); pm_dnd_free(dropped, nd); draw(); return; }
		if (nd == 0) return;
	}
	switch (ev->type) {
	case Expose: if (!ev->xexpose.count) draw(); break;
	case ConfigureNotify:
		if (ev->xconfigure.width != W || ev->xconfigure.height != H) { W = ev->xconfigure.width; H = ev->xconfigure.height; draw(); }
		break;
	case FocusIn: case FocusOut: focused = ev->type == FocusIn; draw(); break;
	case SelectionRequest: if (!wl) on_selrequest(&ev->xselectionrequest); break;
	case ClientMessage: if ((Atom)ev->xclient.data.l[0] == a_delete) { cleanup(); exit(0); } break;
	case PropertyNotify: if (!wl && pm_conf_changed(dpy, ev)) { apply_colors(); draw(); } break;
	}
}

/* 事件處理（X 與原生共用）：回傳是否要重畫 */
static int dispatch_ev(XEvent *ev)
{
	if (ev->type == KeyPress) { keypress(&ev->xkey); return 1; }
	if (ev->type == ButtonPress) { bpress(&ev->xbutton); return 1; }
	if (ev->type == MotionNotify) { bmotion(&ev->xmotion); return 0; }
	handle(ev);
	return 0;
}

/* 子程序的管子可以讀了：收進緩衝區 */
static void read_kid(Kid *k)
{
	ssize_t n = read(k->fd, k->buf + k->n, sizeof k->buf - 1 - k->n);
	if (n > 0) k->n += (size_t)n; else if (n == 0) { close(k->fd); k->fd = -1; }
}

int gui_browse(int argc, char **argv)
{
	setlocale(LC_ALL, "");
	ac_init_locale();
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	int native = pmwl_connect();
	if (native) pm_ui_native = 1;
	else {
		if (!(dpy = XOpenDisplay(NULL))) { fputs(_("pomelo-archive: 無法開啟 X display\n"), stderr); return 1; }
		XSetErrorHandler(xerror);
	}
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13), pm_conf_int("appearance.dark", 0),
	        pm_conf_get("appearance.icon_theme", "Yaru"));
	apply_colors();
	if (native) {
		a_delete = PMWL_XCLOSE;
		wl = pmwl_window(_("壓縮管理員"), "pomelo-archive", W, H, 560, 360, NULL);
		if (!wl) { fprintf(stderr, "pomelo-archive: %s\n", _("無法開啟 Wayland 視窗")); return 1; }
		win = PMWL_XID(wl);
		pmwl_want(wl, 0);
		static const char *const uri[] = { "text/uri-list" };
		pmwl_dnd_target(wl, uri, 1, PMWL_DND_COPY);   /* 把壓縮檔拖進來開啟、把檔案拖進來加入或建立 */
		set_title();
		if (argc > 1 && argv[1][0] != '-') { char f[PATH_MAX]; load_archive(realpath(argv[1], f) ? f : argv[1], NULL, NULL); }
		draw();
		for (;;) {
			reap_kids();
			if (status_msg[0] && pm_now_ms() > status_until) status_msg[0] = 0;
			struct pollfd pf[8];
			int np = 0;
			for (int i = 0; i < 8; i++) if (kids[i].pid && kids[i].fd >= 0) pf[np++] = (struct pollfd){ .fd = kids[i].fd, .events = POLLIN };
			long wait = -1;
			if (status_msg[0]) wait = MAX(50, status_until - pm_now_ms());
			if (any_kid()) wait = wait < 0 ? 300 : MIN(wait, 300);
			PmwEvent pe;
			int dirty = 0;
			int got = pmwl_next_fds(&pe, wait, pf, np);
			if (!got) {   /* 沒有視窗事件：看管子有沒有資料、時間到了重畫狀態列 */
				for (int i = 0, j = 0; i < 8; i++) if (kids[i].pid && kids[i].fd >= 0) { if (pf[j++].revents & (POLLIN | POLLHUP)) read_kid(&kids[i]); }
				reap_kids();
				draw();
				continue;
			}
			do {
				XEvent ev;
				switch (pe.type) {
				case PMW_CONF: apply_colors(); dirty = 1; break;
				case PMW_DND_DROP: {
					char **dropped;
					int nd = pmwl_dnd_files(&dropped);
					if (nd > 0) { on_dropped(dropped, nd); pm_dnd_free(dropped, nd); dirty = 1; }
					break;
				}
				default: if (pmwl_to_xevent(&pe, &ev)) dirty |= dispatch_ev(&ev);
				}
			} while (pmwl_next(&pe, 0));
			reap_kids();
			if (dirty) draw();
		}
	}
	pm_dispatch = handle;
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_string = XA_STRING;
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_neticon = XInternAtom(dpy, "_NET_WM_ICON", False);
	a_netpid = XInternAtom(dpy, "_NET_WM_PID", False);
	XSetWindowAttributes wa = {
		.background_pixel = c_bg, .bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
		.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask,
	};
	win = XCreateWindow(dpy, pm_root, 0, 0, W, H, 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	XClassHint ch = { "pomelo-archive", "Pomelo-archive" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 560; sz->min_height = 360;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	long pid = getpid();
	XChangeProperty(dpy, win, a_netpid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	pm_dnd_accept(dpy, win);
	set_wm_icon();
	pm_conf_watch(dpy);
	set_title();
	XMapWindow(dpy, win);
	if (argc > 1 && argv[1][0] != '-') { char f[PATH_MAX]; load_archive(realpath(argv[1], f) ? f : argv[1], NULL, NULL); }
	int xfd = ConnectionNumber(dpy);
	for (;;) {
		int dirty = 0;
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			dirty |= dispatch_ev(&ev);
		}
		reap_kids();
		if (dirty || (status_msg[0] && pm_now_ms() > status_until)) { if (pm_now_ms() > status_until) status_msg[0] = 0; draw(); }
		XFlush(dpy);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		int mx = xfd;
		for (int i = 0; i < 8; i++) if (kids[i].pid && kids[i].fd >= 0) { FD_SET(kids[i].fd, &fds); if (kids[i].fd > mx) mx = kids[i].fd; }
		struct timeval tv = { 0, 300000 };
		long wait = -1;
		if (status_msg[0]) wait = MAX(50, status_until - pm_now_ms());
		if (any_kid()) wait = wait < 0 ? 300 : MIN(wait, 300);
		if (wait >= 0) { tv.tv_sec = wait / 1000; tv.tv_usec = (wait % 1000) * 1000; }
		int r = select(mx + 1, &fds, NULL, NULL, wait >= 0 ? &tv : NULL);
		if (r > 0) for (int i = 0; i < 8; i++) if (kids[i].pid && kids[i].fd >= 0 && FD_ISSET(kids[i].fd, &fds)) {
			read_kid(&kids[i]);
		}
		if (any_kid()) reap_kids();
	}
}
