/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-files — Pomelo 檔案管理員（Finder 風格）
 *
 * 側邊欄（個人收藏 / 位置）、圖示與列表兩種檢視、即時更新（inotify）、
 * 複製 / 剪下 / 貼上（與 Nautilus 等相容的剪貼簿格式）、拖放（XDND，跟別的程式也能互拖）、
 * 垃圾桶、重新命名、搜尋過濾、輸入法輸入中文檔名。
 * 依賴：Xlib、Xext(Shape)、Xrandr、cairo。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-wl.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#include "files-config.h"

#define TB_H   52     /* 工具列 */
#define SB_W   190    /* 側邊欄 */
#define ST_H   26     /* 狀態列 */
#define CELL_W 108
#define CELL_H 104
#define BIG    56     /* 圖示檢視的圖示大小 */
#define ROW_H  24
#define HEAD_H 24

typedef struct {
	char *name;
	int dir, link, exec;
	off_t size;
	time_t mtime;
	const char *mime;
	int sel;
	cairo_surface_t *thumb;
	int thumb_tried;
} Entry;

typedef struct {
	char label[96]; char path[PATH_MAX]; const char *icon; int header;
	char dev[64]; int eject, unmounted;   /* 外接裝置：可退出／尚未掛載（點了才掛載） */
} Place;

/* 視窗與狀態 */
static Window win;
static PmWl *wl;                     /* 原生 Wayland 視窗（NULL＝X 後端） */
static int mouse_x, mouse_y;         /* 最近一次滑鼠位置（視窗座標；原生的彈出選單要用） */
static char preedit[256];            /* 原生：輸入法還沒確定的字 */
static int ime_x, ime_y, ime_h;      /* 輸入框的游標位置（給輸入法擺選字列） */
/* 子行程不要繼承 X 連線（原生後端沒有 X 連線） */
#define CLOSE_X() do { if (dpy) close(ConnectionNumber(dpy)); } while (0)
static int W = 960, H = 600, focused = 1, view, hidden, scrolly;
static Entry *ents;
static int nents;
static int *vis, nvis;               /* 套用搜尋過濾後要顯示的項目 */
static char cwd[PATH_MAX];
static char *hist[64];
static int nhist, hpos = -1;
static Place places[32];
static int nplaces;
static char trash_files[PATH_MAX], trash_info[PATH_MAX];
static char status_msg[256];
static long status_until;
static unsigned long long free_bytes;
static int sort_key, sort_desc;      /* 0 名稱 1 修改日期 2 大小 3 種類 */
static int anchor = -1;              /* Shift 選取的起點 */
static int cursor_i = -1;
static Time last_click;
static int last_click_i = -2;
static char typeahead[64];
static long typeahead_at;
static int ino_fd = -1, ino_wd = -1;
static long reload_at;
static XIM xim;
static XIC xic;

/* 文字輸入（搜尋、重新命名、前往路徑） */
enum { IN_NONE, IN_SEARCH, IN_RENAME, IN_PATH };
static struct { int kind, idx, cur, sel0, sel1; char buf[1024]; } in;   /* sel0..sel1：反白的範圍 */
static char filter[256];

/* 剪貼簿 */
static char **clip;
static int nclip, clip_cut;
static char *clip_text;

/* 顏色 */
static unsigned c_bg, c_side, c_tool, c_line, c_text, c_dim, c_stripe, c_hl, c_selin;

static Atom a_delete, a_clipboard, a_utf8, a_targets, a_gnome, a_urilist, a_paste, a_netname, a_neticon, a_netpid;

/* 拖放（XDND 第 5 版） */
static Atom a_xdndaware, a_xdndproxy, a_xdndenter, a_xdndposition, a_xdndstatus, a_xdndleave, a_xdnddrop,
            a_xdndfinished, a_xdndsel, a_xdndtypes, a_xdndactions, a_xdndcopy, a_xdndmove, a_xdndask,
            a_xdnddata, a_textplain;

/* 拖出去 */
static struct {
	int pending, defer, rename, click_i;   /* pending：按下了、還沒移動夠遠；defer：放開時才決定點選的結果 */
	int px, py, active;
	char **paths, *text;                   /* 拖的檔案（拖到文字欄位時給純文字的路徑） */
	int n;
	Window icon;
	int icon_state;                        /* 圖示角落的標記：0 移動、1 拷貝、2 不能放、-1 還不知道 */
	cairo_surface_t *img;                  /* 圖示（不含標記） */
	Window target, to;                     /* 游標下面收拖放的視窗；to 是實際收訊息的（XdndProxy） */
	int ver, waiting, moved, accepted;     /* waiting：送了位置、還沒收到回覆；moved：等回覆時又移動了 */
	Atom action, answer;                   /* 我們建議的動作、對方回覆的動作 */
	int rx, ry;
	Time t;
	long drop_deadline;                    /* 放開了但還在等回覆 */
} dr = { .click_i = -1 };

/* 拖進來 */
static struct {
	Window src;
	int ver, ok_type, fetching, fetched, dropped;
	char **paths;
	int n;
	int x, y;                              /* 螢幕座標 */
	Time t;
	Atom suggested;
	int hl_item, hl_place, hl_here;        /* 反白的目的地：資料夾項目、側邊欄、目前的資料夾 */
	char dir[PATH_MAX];
	int accept, move, to_trash;
	unsigned act;                          /* 原生：放下時協商好的動作 PMWL_DND_* */
	char spring_dir[PATH_MAX];             /* 停在資料夾上一下就打開它 */
	long spring_at, scroll_at, drop_deadline;
	int scroll_dir;
} dp = { .hl_item = -1, .hl_place = -1 };

static void draw(void);
static void load_dir(const char *path, int push);
static void dnd_selnotify(XSelectionEvent *e);
static void drag_cancel(void);
static void drag_build_img(void);
static void popup_at_pointer(MenuItem *m, int n);

static int xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static void set_status(const char *msg)
{
	snprintf(status_msg, sizeof status_msg, "%s", msg);
	status_until = pm_now_ms() + 4000;
}

/* ------------------------------------------------------------------ */
/* MIME 類型與圖示                                                     */
/* ------------------------------------------------------------------ */

typedef struct { char *key, *val; } KV;
static KV *globs, *generic;
static int nglobs, ngeneric;

static int kvcmp(const void *a, const void *b) { return strcmp(((const KV *)a)->key, ((const KV *)b)->key); }

static void load_mime_tables(void)
{
	char line[512];
	FILE *f = fopen("/usr/share/mime/globs2", "r");
	int cap = 0;
	while (f && fgets(line, sizeof line, f)) {
		/* 權重:類型:*.副檔名 */
		if (line[0] == '#') continue;
		line[strcspn(line, "\n")] = 0;
		char *m = strchr(line, ':'), *g = m ? strchr(m + 1, ':') : NULL;
		if (!g || strncmp(g + 1, "*.", 2)) continue;
		*g = 0;
		char *ext = g + 3;
		char *flags = strchr(ext, ':');
		if (flags) *flags = 0;
		if (strpbrk(ext, "*?[")) continue;
		if (nglobs == cap) { cap = cap ? cap * 2 : 1024; globs = realloc(globs, cap * sizeof *globs); }
		for (char *q = ext; *q; q++) *q = (char)tolower((unsigned char)*q);
		globs[nglobs].key = strdup(ext);
		globs[nglobs].val = strdup(m + 1);
		nglobs++;
	}
	if (f) fclose(f);
	/* 權重高的在前，排序後保留第一個 */
	if (nglobs) qsort(globs, nglobs, sizeof *globs, kvcmp);
	f = fopen("/usr/share/mime/generic-icons", "r");
	cap = 0;
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *c = strchr(line, ':');
		if (!c) continue;
		*c = 0;
		if (ngeneric == cap) { cap = cap ? cap * 2 : 512; generic = realloc(generic, cap * sizeof *generic); }
		generic[ngeneric].key = strdup(line);
		generic[ngeneric].val = strdup(c + 1);
		ngeneric++;
	}
	if (f) fclose(f);
	if (ngeneric) qsort(generic, ngeneric, sizeof *generic, kvcmp);
}

static const char *kv_find(KV *t, int n, const char *key)
{
	KV k = { (char *)key, NULL };
	KV *r = n ? bsearch(&k, t, n, sizeof *t, kvcmp) : NULL;
	return r ? r->val : NULL;
}

static const char *mime_of(const char *name, int dir, int exec)
{
	if (dir) return "inode/directory";
	char lc[256];
	snprintf(lc, sizeof lc, "%s", name);
	for (char *q = lc; *q; q++) *q = (char)tolower((unsigned char)*q);
	/* 由最長的副檔名開始試：tar.gz → gz */
	for (char *d = strchr(lc + 1, '.'); d; d = strchr(d + 1, '.')) {
		const char *m = kv_find(globs, nglobs, d + 1);
		if (m) return m;
	}
	return exec ? "application/x-executable" : "application/octet-stream";
}

static const char *kind_of(const char *mime)
{
	if (!strcmp(mime, "inode/directory")) return _("資料夾");
	if (!strncmp(mime, "image/", 6)) return _("影像");
	if (!strncmp(mime, "audio/", 6)) return _("音訊");
	if (!strncmp(mime, "video/", 6)) return _("影片");
	if (!strcmp(mime, "application/pdf")) return _("PDF 文件");
	if (strstr(mime, "zip") || strstr(mime, "tar") || strstr(mime, "compressed") || strstr(mime, "rar") ||
	    strstr(mime, "7z") || strstr(mime, "xz") || strstr(mime, "bzip") || strstr(mime, "gzip"))
		return _("壓縮檔");
	if (strstr(mime, "executable") || strstr(mime, "sharedlib") || strstr(mime, "x-shellscript")) return _("程式");
	if (strstr(mime, "document") || strstr(mime, "msword") || strstr(mime, "spreadsheet") ||
	    strstr(mime, "presentation") || strstr(mime, "ms-excel") || strstr(mime, "ms-powerpoint"))
		return _("文件");
	if (!strncmp(mime, "text/", 5)) return _("文字");
	return _("檔案");
}

/* 圖示快取：名稱 + 大小 → 圖片（找不到也記下來，避免重複搜尋） */
typedef struct { char key[96]; int size; cairo_surface_t *s; } IconC;
static IconC icache[512];
static int nicache;

static cairo_surface_t *get_icon(const char *name, int size)
{
	for (int i = 0; i < nicache; i++)
		if (icache[i].size == size && !strcmp(icache[i].key, name))
			return icache[i].s;
	cairo_surface_t *s = pm_icon_load(name, size);
	if (nicache < (int)LEN(icache)) {
		snprintf(icache[nicache].key, sizeof icache[0].key, "%s", name);
		icache[nicache].size = size;
		icache[nicache].s = s;
		nicache++;
	}
	return s;
}

static const char *special_folder(const char *path)
{
	for (int i = 0; i < nplaces; i++)
		if (!places[i].header && !strcmp(places[i].path, path) && places[i].icon && strcmp(places[i].icon, "drive-harddisk"))
			return places[i].icon;
	return NULL;
}

static cairo_surface_t *icon_for(Entry *e, int size)
{
	if (e->thumb) return e->thumb;
	if (e->dir) {
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", strcmp(cwd, "/") ? cwd : "", e->name);
		const char *sp = special_folder(p);
		cairo_surface_t *s = sp ? get_icon(sp, size) : NULL;
		return s ? s : get_icon("folder", size);
	}
	char name[128];
	snprintf(name, sizeof name, "%s", e->mime);
	for (char *q = name; *q; q++) if (*q == '/') *q = '-';
	cairo_surface_t *s = get_icon(name, size);
	if (s) return s;
	const char *g = kv_find(generic, ngeneric, e->mime);
	if (g && (s = get_icon(g, size))) return s;
	char media[64];
	snprintf(media, sizeof media, "%s", e->mime);
	media[strcspn(media, "/")] = 0;
	snprintf(name, sizeof name, "%s-x-generic", media);
	if ((s = get_icon(name, size))) return s;
	return get_icon(e->exec ? "application-x-executable" : "text-x-generic", size);
}

/* PNG 影像直接做縮圖（小於 8MB） */
static void load_thumb(Entry *e)
{
	e->thumb_tried = 1;
	if ((strcmp(e->mime, "image/png") && strcmp(e->mime, "image/jpeg")) || e->size > 30 * 1024 * 1024) return;
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s/%s", cwd, e->name);
	e->thumb = pm_image_load_fit(p, BIG, BIG, 1);
}

/* ------------------------------------------------------------------ */
/* 側邊欄位置                                                          */
/* ------------------------------------------------------------------ */

static Place *add_place(const char *label, const char *path, const char *icon, int header)
{
	static Place dummy;
	if (nplaces >= (int)LEN(places)) return &dummy;
	Place *p = &places[nplaces++];
	memset(p, 0, sizeof *p);
	snprintf(p->label, sizeof p->label, "%s", label);
	snprintf(p->path, sizeof p->path, "%s", path ? path : "");
	p->icon = icon;
	p->header = header;
	return p;
}

static void build_places(void)
{
	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	nplaces = 0;
	add_place(_("個人收藏"), NULL, NULL, 1);
	add_place(_("家目錄"), home, "user-home", 0);

	/* XDG 使用者資料夾（桌面、文件、下載…） */
	static const struct { const char *key, *icon; } xdg[] = {
		{ "XDG_DESKTOP_DIR", "user-desktop" }, { "XDG_DOCUMENTS_DIR", "folder-documents" },
		{ "XDG_DOWNLOAD_DIR", "folder-download" }, { "XDG_PICTURES_DIR", "folder-pictures" },
		{ "XDG_MUSIC_DIR", "folder-music" }, { "XDG_VIDEOS_DIR", "folder-videos" },
	};
	char cfg[PATH_MAX], line[PATH_MAX];
	snprintf(cfg, sizeof cfg, "%s/.config/user-dirs.dirs", home);
	char found[LEN(xdg)][PATH_MAX];
	memset(found, 0, sizeof found);
	FILE *f = fopen(cfg, "r");
	while (f && fgets(line, sizeof line, f)) {
		for (unsigned i = 0; i < LEN(xdg); i++) {
			size_t kl = strlen(xdg[i].key);
			if (strncmp(line, xdg[i].key, kl) || line[kl] != '=') continue;
			char *v = line + kl + 1;
			v[strcspn(v, "\n")] = 0;
			if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
			if (!strncmp(v, "$HOME", 5)) snprintf(found[i], PATH_MAX, "%s%s", home, v + 5);
			else snprintf(found[i], PATH_MAX, "%s", v);
		}
	}
	if (f) fclose(f);
	for (unsigned i = 0; i < LEN(xdg); i++) {
		struct stat st;
		if (!found[i][0] || !strcmp(found[i], home) || stat(found[i], &st) || !S_ISDIR(st.st_mode)) continue;
		const char *base = strrchr(found[i], '/') ? strrchr(found[i], '/') + 1 : found[i];
		add_place(base, found[i], xdg[i].icon, 0);
	}

	add_place(_("位置"), NULL, NULL, 1);
	add_place(_("電腦"), "/", "drive-harddisk", 0);
	/* 外接裝置（透過 udisks2）：已掛載的可以退出，還沒掛載的點一下就掛載 */
	if (!pm_disks_scan()) {
		int nv;
		PmVolume *v = pm_volumes(&nv);
		for (int i = 0; i < nv; i++) {
			if (!v[i].external || !v[i].is_fs || v[i].ignore) continue;
			char name[160];
			pm_volume_name(&v[i], name, sizeof name);
			Place *p = add_place(name, v[i].mount[0] ? v[i].mount : NULL, "drive-removable-media", 0);
			snprintf(p->dev, sizeof p->dev, "%s", v[i].dev);
			p->eject = v[i].mount[0] != 0;
			p->unmounted = !v[i].mount[0];
		}
	}
	/* 其他掛在 /media、/mnt 底下的（沒有 udisks2，或手動掛載的） */
	f = fopen("/proc/mounts", "r");
	while (f && fgets(line, sizeof line, f)) {
		char dev[256], mnt[PATH_MAX];
		if (sscanf(line, "%255s %4095s", dev, mnt) != 2) continue;
		if (strncmp(mnt, "/media/", 7) && strncmp(mnt, "/run/media/", 11) && strncmp(mnt, "/mnt/", 5)) continue;
		/* /proc/mounts 的空白寫成 \040 */
		char dec[PATH_MAX], *o = dec;
		for (char *s2 = mnt; *s2 && o < dec + sizeof dec - 1; s2++) {
			if (s2[0] == '\\' && isdigit((unsigned char)s2[1]) && isdigit((unsigned char)s2[2]) && isdigit((unsigned char)s2[3])) {
				*o++ = (char)((s2[1] - '0') * 64 + (s2[2] - '0') * 8 + (s2[3] - '0'));
				s2 += 3;
			} else *o++ = *s2;
		}
		*o = 0;
		int dup = 0;
		for (int k = 0; k < nplaces; k++) if (!strcmp(places[k].path, dec)) dup = 1;
		if (!dup) add_place(strrchr(dec, '/') + 1, dec, "drive-removable-media", 0);
	}
	if (f) fclose(f);
	add_place(_("垃圾桶"), trash_files, "user-trash", 0);
}

/* ------------------------------------------------------------------ */
/* 讀取資料夾                                                          */
/* ------------------------------------------------------------------ */

static int natcmp(const char *a, const char *b)
{
	char la[512], lb[512];
	snprintf(la, sizeof la, "%s", a);
	snprintf(lb, sizeof lb, "%s", b);
	for (char *q = la; *q; q++) *q = (char)tolower((unsigned char)*q);
	for (char *q = lb; *q; q++) *q = (char)tolower((unsigned char)*q);
	int r = strverscmp(la, lb);
	return r ? r : strcmp(a, b);
}

static int entcmp(const void *pa, const void *pb)
{
	const Entry *a = pa, *b = pb;
	if (a->dir != b->dir) return b->dir - a->dir;   /* 資料夾在前 */
	int r = 0;
	switch (sort_key) {
	case 1: r = a->mtime < b->mtime ? -1 : a->mtime > b->mtime; break;
	case 2: r = a->size < b->size ? -1 : a->size > b->size; break;
	case 3: r = strcmp(kind_of(a->mime), kind_of(b->mime)); break;
	}
	if (!r) r = natcmp(a->name, b->name);
	return sort_desc ? -r : r;
}

static void free_entries(void)
{
	for (int i = 0; i < nents; i++) {
		free(ents[i].name);
		if (ents[i].thumb) cairo_surface_destroy(ents[i].thumb);
	}
	free(ents);
	ents = NULL;
	nents = 0;
}

static void apply_filter(void)
{
	free(vis);
	vis = malloc((nents + 1) * sizeof *vis);
	nvis = 0;
	for (int i = 0; i < nents; i++)
		if (!filter[0] || strcasestr(ents[i].name, filter))
			vis[nvis++] = i;
	cursor_i = MIN(cursor_i, nvis - 1);
}

static void set_title(void)
{
	const char *t = !strcmp(cwd, "/") ? _("電腦") : !strcmp(cwd, trash_files) ? _("垃圾桶") :
	                strrchr(cwd, '/') + 1;
	for (int i = 0; i < nplaces; i++)
		if (!places[i].header && !strcmp(places[i].path, cwd)) t = places[i].label;
	if (wl) { pmwl_set_title(wl, t); return; }
	XStoreName(dpy, win, t);
	XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (const unsigned char *)t, strlen(t));
}

/* 重新讀取目前資料夾，盡量保留選取與捲動位置 */
static void read_dir(int keep)
{
	char **was = NULL;
	int nwas = 0;
	if (keep) {
		was = malloc((nents + 1) * sizeof *was);
		for (int i = 0; i < nents; i++)
			if (ents[i].sel) was[nwas++] = strdup(ents[i].name);
	}
	free_entries();
	DIR *d = opendir(cwd);
	if (!d) {
		set_status(strerror(errno));
	} else {
		int cap = 0, dfd = dirfd(d);
		struct dirent *de;
		while ((de = readdir(d))) {
			const char *n = de->d_name;
			if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
			if (n[0] == '.' && !hidden) continue;
			struct stat st;
			if (fstatat(dfd, n, &st, AT_SYMLINK_NOFOLLOW)) continue;
			Entry e = { 0 };
			e.link = S_ISLNK(st.st_mode);
			if (e.link) { struct stat t; if (!fstatat(dfd, n, &t, 0)) st = t; }
			e.name = strdup(n);
			e.dir = S_ISDIR(st.st_mode);
			e.exec = !e.dir && (st.st_mode & 0111);
			e.size = st.st_size;
			e.mtime = st.st_mtime;
			e.mime = mime_of(n, e.dir, e.exec);
			if (e.link && !e.dir && !strcmp(e.mime, "application/octet-stream")) {
				/* 捷徑沒有副檔名時，看它指向的檔名 */
				char target[PATH_MAX];
				ssize_t tl = readlinkat(dfd, n, target, sizeof target - 1);
				if (tl > 0) {
					target[tl] = 0;
					const char *tb = strrchr(target, '/') ? strrchr(target, '/') + 1 : target;
					e.mime = mime_of(tb, 0, e.exec);
				}
			}
			for (int k = 0; k < nwas; k++)
				if (!strcmp(was[k], n)) e.sel = 1;
			if (nents == cap) { cap = cap ? cap * 2 : 256; ents = realloc(ents, cap * sizeof *ents); }
			ents[nents++] = e;
		}
		closedir(d);
	}
	for (int k = 0; k < nwas; k++) free(was[k]);
	free(was);
	if (nents) qsort(ents, nents, sizeof *ents, entcmp);
	apply_filter();
	struct statvfs vs;
	free_bytes = statvfs(cwd, &vs) == 0 ? (unsigned long long)vs.f_bavail * vs.f_frsize : 0;
}

static void load_dir(const char *path, int push)
{
	char real[PATH_MAX];
	if (!realpath(path, real)) {
		set_status(_("找不到這個位置"));
		return;
	}
	struct stat st;
	if (stat(real, &st) || !S_ISDIR(st.st_mode)) {
		set_status(_("這不是資料夾"));
		return;
	}
	if (access(real, R_OK | X_OK)) {
		set_status(_("沒有權限打開這個資料夾"));
		return;
	}
	snprintf(cwd, sizeof cwd, "%s", real);
	if (push) {
		/* 往前的歷史在新瀏覽時丟掉 */
		for (int i = hpos + 1; i < nhist; i++) free(hist[i]);
		nhist = hpos + 1;
		if (nhist == (int)LEN(hist)) { free(hist[0]); memmove(hist, hist + 1, --nhist * sizeof *hist); }
		hist[nhist++] = strdup(cwd);
		hpos = nhist - 1;
	}
	filter[0] = 0;
	if (in.kind) in.kind = IN_NONE;
	scrolly = 0;
	cursor_i = -1;
	anchor = -1;
	read_dir(0);
	if (ino_fd >= 0) {
		if (ino_wd >= 0) inotify_rm_watch(ino_fd, ino_wd);
		ino_wd = inotify_add_watch(ino_fd, cwd, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
		                                       IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
	}
	set_title();
}

static void go_back(void) { if (hpos > 0) { hpos--; load_dir(hist[hpos], 0); } }
static void go_forward(void) { if (hpos < nhist - 1) { hpos++; load_dir(hist[hpos], 0); } }

static void go_parent(void)
{
	if (!strcmp(cwd, "/")) return;
	char child[PATH_MAX], p[PATH_MAX];
	snprintf(child, sizeof child, "%s", strrchr(cwd, '/') + 1);
	snprintf(p, sizeof p, "%s", cwd);
	char *s = strrchr(p, '/');
	if (s == p) s[1] = 0; else *s = 0;
	load_dir(p, 1);
	/* 選取剛剛離開的資料夾 */
	for (int i = 0; i < nvis; i++)
		if (!strcmp(ents[vis[i]].name, child)) { ents[vis[i]].sel = 1; cursor_i = anchor = i; }
}

/* ------------------------------------------------------------------ */
/* 檔案動作                                                            */
/* ------------------------------------------------------------------ */

static void path_of(Entry *e, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", strcmp(cwd, "/") ? cwd : "", e->name);
}

static int nsel(void)
{
	int n = 0;
	for (int i = 0; i < nvis; i++) n += ents[vis[i]].sel;
	return n;
}

static void exec_detached(char *const argv[])
{
	if (fork() == 0) {
		if (fork() == 0) {
			CLOSE_X();
			setsid();
			execvp(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	wait(NULL);
}

static void new_window(const char *path)
{
	char self[PATH_MAX];
	ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
	self[k > 0 ? k : 0] = 0;
	char *argv[] = { k > 0 ? self : "pomelo-files", (char *)path, NULL };
	exec_detached(argv);
}

static void open_entry(Entry *e)
{
	char p[PATH_MAX];
	path_of(e, p, sizeof p);
	if (e->dir) {
		load_dir(p, 1);
		return;
	}
	char *argv[] = { (char *)opener, p, NULL };
	exec_detached(argv);
}

static void open_selected(void)
{
	Entry *firstdir = NULL;
	for (int i = 0; i < nvis; i++) {
		Entry *e = &ents[vis[i]];
		if (!e->sel) continue;
		if (e->dir) { if (!firstdir) firstdir = e; }
		else open_entry(e);
	}
	if (firstdir) open_entry(firstdir);
}

static void open_terminal(const char *dir)
{
	char *argv[] = { (char *)terminal, "-d", (char *)dir, NULL };
	exec_detached(argv);
}

/* 目的地已有同名檔案時，產生「名稱 2.副檔名」 */
static void unique_path(const char *dir, const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", dir, name);
	struct stat st;
	if (lstat(out, &st)) return;
	char base[NAME_MAX + 1], ext[NAME_MAX + 1] = "";
	snprintf(base, sizeof base, "%s", name);
	char *dot = strrchr(base, '.');
	if (dot && dot != base) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
	for (int i = 2; i < 10000; i++) {
		snprintf(out, n, "%s/%s %d%s", dir, base, i, ext);
		if (lstat(out, &st)) return;
	}
}

static int have(const char *cmd)
{
	char *path = strdup(getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin"), *save = NULL;
	int found = 0;
	for (char *d = strtok_r(path, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
		char full[PATH_MAX];
		snprintf(full, sizeof full, "%s/%s", d, cmd);
		found = !access(full, X_OK);
	}
	free(path);
	return found;
}

/* 在背景行程中複製 / 移動到 dir，畫面不會卡住；完成後 inotify 會更新畫面 */
static void paste_into(const char *dir, char **src, int n, int move)
{
	if (!n) return;
	const char *d = strcmp(dir, "/") ? dir : "";
	char **dst = malloc(n * sizeof *dst);
	for (int i = 0; i < n; i++) {
		dst[i] = malloc(PATH_MAX);
		const char *base = strrchr(src[i], '/') ? strrchr(src[i], '/') + 1 : src[i];
		char d0[PATH_MAX];
		snprintf(d0, sizeof d0, "%s/%s", d, base);
		if (move && !strcmp(d0, src[i])) { dst[i][0] = 0; continue; }   /* 原地剪下貼上：不動 */
		unique_path(d, base, dst[i], PATH_MAX);
		size_t sl = strlen(src[i]);
		if (!strncmp(dst[i], src[i], sl) && dst[i][sl] == '/') {
			set_status(_("無法把資料夾放進它自己裡面"));
			dst[i][0] = 0;
		}
	}
	size_t tl = strlen(trash_files);
	pid_t pid = fork();
	if (pid == 0) {
		CLOSE_X();
		setsid();
		for (int i = 0; i < n; i++) {
			if (!dst[i][0]) continue;
			int done = move && rename(src[i], dst[i]) == 0;
			if (!done) {
				pid_t c = fork();
				if (c == 0) { execlp("cp", "cp", "-a", "--", src[i], dst[i], (char *)NULL); _exit(127); }
				int st = 1;
				waitpid(c, &st, 0);
				if (move && WIFEXITED(st) && WEXITSTATUS(st) == 0) {
					c = fork();
					if (c == 0) { execlp("rm", "rm", "-rf", "--", src[i], (char *)NULL); _exit(127); }
					waitpid(c, NULL, 0);
					done = 1;
				}
			}
			/* 從垃圾桶拿出來：說明檔也刪掉，不然垃圾桶會多一筆找不到檔案的紀錄 */
			if (done && !strncmp(src[i], trash_files, tl) && src[i][tl] == '/' && !strchr(src[i] + tl + 1, '/')) {
				char info[PATH_MAX];
				snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, src[i] + tl + 1);
				unlink(info);
			}
		}
		_exit(0);
	}
	for (int i = 0; i < n; i++) free(dst[i]);
	free(dst);
	set_status(move ? _("正在移動…") : _("正在拷貝…"));
}

static void paste_files(char **src, int n, int move) { paste_into(cwd, src, n, move); }

static char **selected_paths(int *n)
{
	char **v = malloc((nvis + 1) * sizeof *v);
	*n = 0;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel) {
			v[*n] = malloc(PATH_MAX);
			path_of(&ents[vis[i]], v[*n], PATH_MAX);
			(*n)++;
		}
	return v;
}

static void free_paths(char **v, int n)
{
	for (int i = 0; i < n; i++) free(v[i]);
	free(v);
}

static void trash_paths(char **v, int n)
{
	if (!n) return;
	if (have("gio")) {
		char **argv = malloc((n + 4) * sizeof *argv);
		argv[0] = "gio"; argv[1] = "trash"; argv[2] = "--";
		for (int i = 0; i < n; i++) argv[3 + i] = v[i];
		argv[3 + n] = NULL;
		pid_t p = fork();
		if (p == 0) { CLOSE_X(); execvp("gio", argv); _exit(127); }
		free(argv);
	} else {
		/* 依 freedesktop 規範自己放進 ~/.local/share/Trash */
		for (int i = 0; i < n; i++) {
			const char *base = strrchr(v[i], '/') + 1;
			char dest[PATH_MAX], info[PATH_MAX];
			unique_path(trash_files, base, dest, sizeof dest);
			if (rename(v[i], dest)) { set_status(_("無法移到垃圾桶（可能在不同的磁碟上）")); continue; }
			snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, strrchr(dest, '/') + 1);
			FILE *f = fopen(info, "w");
			if (f) {
				char ts[32];
				time_t now = time(NULL);
				strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&now));
				fprintf(f, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", v[i], ts);
				fclose(f);
			}
		}
	}
	char msg[64];
	snprintf(msg, sizeof msg, _("已將 %d 個項目丟到垃圾桶"), n);
	set_status(msg);
}

static void move_to_trash(void)
{
	int n;
	char **v = selected_paths(&n);
	trash_paths(v, n);
	free_paths(v, n);
}

static void delete_now(const Arg *a)
{
	(void)a;
	int n;
	char **v = selected_paths(&n);
	char **argv = malloc((n + 4) * sizeof *argv);
	argv[0] = "rm"; argv[1] = "-rf"; argv[2] = "--";
	for (int i = 0; i < n; i++) argv[3 + i] = v[i];
	argv[3 + n] = NULL;
	pid_t p = fork();
	if (p == 0) { CLOSE_X(); execvp("rm", argv); _exit(127); }
	free(argv);
	free_paths(v, n);
	/* 垃圾桶裡的項目一併刪掉說明檔 */
	if (!strcmp(cwd, trash_files))
		for (int i = 0; i < nvis; i++)
			if (ents[vis[i]].sel) {
				char info[PATH_MAX];
				snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, ents[vis[i]].name);
				unlink(info);
			}
}

/* 在游標附近彈出選單（X：螢幕座標；原生：視窗座標） */
static void popup_at_pointer(MenuItem *m, int n)
{
	if (wl) { pmwl_menu(wl, m, n, mouse_x - 60, mouse_y - 10); return; }
	Window r, c; int rx, ry, wx, wy; unsigned mk;
	XQueryPointer(dpy, win, &r, &c, &rx, &ry, &wx, &wy, &mk);
	pm_popup(m, n, rx - 60, ry - 10);
}

static void confirm_delete(void)
{
	static char label[64];
	int n = nsel();
	if (!n) return;
	snprintf(label, sizeof label, _("永久刪除 %d 個項目"), n);
	MenuItem m[] = { { label, delete_now, {0} }, { NULL, NULL, {0} }, { _("取消"), NULL, {0} } };
	popup_at_pointer(m, LEN(m));
}

static void restore_selected(const Arg *a)
{
	(void)a;
	for (int i = 0; i < nvis; i++) {
		Entry *e = &ents[vis[i]];
		if (!e->sel) continue;
		char info[PATH_MAX], line[PATH_MAX], orig[PATH_MAX] = "";
		snprintf(info, sizeof info, "%s/%s.trashinfo", trash_info, e->name);
		FILE *f = fopen(info, "r");
		while (f && fgets(line, sizeof line, f))
			if (!strncmp(line, "Path=", 5)) {
				line[strcspn(line, "\n")] = 0;
				/* 解開 %XX */
				char *o = orig;
				for (char *s = line + 5; *s && o < orig + sizeof orig - 1; s++) {
					if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
						char hx[3] = { s[1], s[2], 0 };
						*o++ = (char)strtol(hx, NULL, 16);
						s += 2;
					} else *o++ = *s;
				}
				*o = 0;
			}
		if (f) fclose(f);
		if (!orig[0]) { set_status(_("找不到原本的位置")); continue; }
		char from[PATH_MAX];
		path_of(e, from, sizeof from);
		if (rename(from, orig) == 0) unlink(info);
		else set_status(strerror(errno));
	}
}

static void empty_trash(const Arg *a)
{
	(void)a;
	if (fork() == 0) {
		CLOSE_X();
		execlp("sh", "sh", "-c", "rm -rf -- \"$1\"/* \"$1\"/.[!.]* \"$2\"/*", "sh", trash_files, trash_info, (char *)NULL);
		_exit(127);
	}
}

static void ask_empty_trash(const Arg *a)
{
	(void)a;
	MenuItem m[] = { { _("清空垃圾桶（無法復原）"), empty_trash, {0} }, { NULL, NULL, {0} }, { _("取消"), NULL, {0} } };
	popup_at_pointer(m, LEN(m));
}

/* ------------------------------------------------------------------ */
/* 剪貼簿（與 Nautilus 相容）                                          */
/* ------------------------------------------------------------------ */

static char *uri_encode(const char *path)
{
	char *out = malloc(strlen(path) * 3 + 8), *o = out;
	o += sprintf(o, "file://");
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		if (isalnum(*p) || strchr("/-_.~", *p)) *o++ = (char)*p;
		else o += sprintf(o, "%%%02X", *p);
	}
	*o = 0;
	return out;
}

/* 原生：把檔案清單放上剪貼簿（跟 Nautilus 一樣：x-special/gnome-copied-files、text/uri-list，還有純文字路徑） */
static void native_clip_put(char **list, int nl, int cut, const char *text)
{
	char *gn = NULL, *ul = NULL;
	size_t cap = 16;
	for (int i = 0; i < nl; i++) cap += strlen(list[i]) * 3 + 16;
	gn = malloc(cap); ul = malloc(cap);
	strcpy(gn, cut ? "cut" : "copy");
	ul[0] = 0;
	for (int i = 0; i < nl; i++) {
		char *u = uri_encode(list[i]);
		strcat(gn, "\n"); strcat(gn, u);
		strcat(ul, u); strcat(ul, "\r\n");
		free(u);
	}
	const char *mimes[] = { "x-special/gnome-copied-files", "text/uri-list", "text/plain;charset=utf-8", "text/plain", "UTF8_STRING" };
	const void *datas[] = { gn, ul, text, text, text };
	size_t lens[] = { strlen(gn), strlen(ul), text ? strlen(text) : 0, text ? strlen(text) : 0, text ? strlen(text) : 0 };
	if (nl) pmwl_clip_set_types(mimes, datas, lens, 5);
	else pmwl_clip_set_types(mimes + 2, datas + 2, lens + 2, 3);
	free(gn); free(ul);
}

static void clip_set(int cut)
{
	for (int i = 0; i < nclip; i++) free(clip[i]);
	free(clip);
	free(clip_text);
	clip = selected_paths(&nclip);
	clip_cut = cut;
	size_t len = 1;
	for (int i = 0; i < nclip; i++) len += strlen(clip[i]) + 1;
	clip_text = malloc(len);
	clip_text[0] = 0;
	for (int i = 0; i < nclip; i++) { strcat(clip_text, clip[i]); if (i < nclip - 1) strcat(clip_text, "\n"); }
	if (wl) native_clip_put(clip, nclip, clip_cut, clip_text);
	else XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
	char msg[64];
	snprintf(msg, sizeof msg, cut ? _("已剪下 %d 個項目") : _("已拷貝 %d 個項目"), nclip);
	set_status(msg);
}

static void copy_path(void)
{
	clip_set(0);
	for (int i = 0; i < nclip; i++) free(clip[i]);
	nclip = 0;   /* 只留文字，不當成檔案 */
	set_status(_("已拷貝路徑"));
}

/* 剪貼簿與拖放（XdndSelection）都從這裡給資料 */
static void on_selrequest(XSelectionRequestEvent *e)
{
	XSelectionEvent r = { .type = SelectionNotify, .requestor = e->requestor, .selection = e->selection,
	                      .target = e->target, .property = None, .time = e->time };
	Atom prop = e->property ? e->property : e->target;
	int dnd = e->selection == a_xdndsel;
	char **list = dnd ? dr.paths : clip, *text = dnd ? dr.text : clip_text;
	int nl = dnd ? dr.n : nclip, cut = dnd ? 0 : clip_cut;
	if (e->target == a_targets) {
		Atom ts[] = { a_targets, a_gnome, a_urilist, a_utf8, XA_STRING, a_textplain };
		int nt = nl ? 6 : 4;
		if (!nl) { ts[1] = a_utf8; ts[2] = XA_STRING; ts[3] = a_textplain; }
		XChangeProperty(dpy, e->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)ts, nt);
		r.property = prop;
	} else if ((e->target == a_gnome || e->target == a_urilist) && nl) {
		size_t cap = 16;
		for (int i = 0; i < nl; i++) cap += strlen(list[i]) * 3 + 16;
		char *buf = malloc(cap);
		buf[0] = 0;
		if (e->target == a_gnome) strcat(buf, cut ? "cut" : "copy");
		for (int i = 0; i < nl; i++) {
			char *u = uri_encode(list[i]);
			if (e->target == a_gnome || i) strcat(buf, e->target == a_gnome ? "\n" : "\r\n");
			strcat(buf, u);
			free(u);
		}
		if (e->target == a_urilist) strcat(buf, "\r\n");
		XChangeProperty(dpy, e->requestor, prop, e->target, 8, PropModeReplace, (unsigned char *)buf, strlen(buf));
		free(buf);
		r.property = prop;
	} else if ((e->target == a_utf8 || e->target == XA_STRING || e->target == a_textplain) && text) {
		XChangeProperty(dpy, e->requestor, prop, e->target, 8, PropModeReplace,
		                (unsigned char *)text, strlen(text));
		r.property = prop;
	}
	XSendEvent(dpy, e->requestor, True, 0, (XEvent *)&r);
}

/* 原生：從剪貼簿貼上檔案（自己放的也一樣讀回來；剪下貼上之後清掉，不會再貼第二次） */
static void native_paste(void)
{
	size_t len = 0;
	int cut = 0;
	char *data = NULL;
	if (pmwl_clip_has_type("x-special/gnome-copied-files")) {
		data = pmwl_clip_get_type("x-special/gnome-copied-files", &len);
		if (data && !strncmp(data, "cut", 3)) cut = 1;
	}
	if (!data && pmwl_clip_has_type("text/uri-list")) data = pmwl_clip_get_type("text/uri-list", &len);
	if (!data) return;
	char *paths[512];
	int np = pm_parse_uris(data, paths, 512);
	free(data);
	if (np <= 0) return;
	paste_files(paths, np, cut);
	for (int i = 0; i < np; i++) free(paths[i]);
	if (cut) {
		pmwl_clip_set("");
		for (int i = 0; i < nclip; i++) free(clip[i]);
		nclip = 0;
	}
}

static void paste(void)
{
	if (wl) { native_paste(); return; }
	if (XGetSelectionOwner(dpy, a_clipboard) == win) {
		if (!nclip) return;
		paste_files(clip, nclip, clip_cut);
		if (clip_cut) { for (int i = 0; i < nclip; i++) free(clip[i]); nclip = 0; }
		return;
	}
	/* 從其他程式（例如 Nautilus）貼上 */
	XConvertSelection(dpy, a_clipboard, a_gnome, a_paste, win, CurrentTime);
}

static void on_selnotify(XSelectionEvent *e)
{
	if (e->selection == a_xdndsel) { dnd_selnotify(e); return; }
	if (e->property == None) {
		if (e->target == a_gnome)
			XConvertSelection(dpy, a_clipboard, a_urilist, a_paste, win, CurrentTime);
		return;
	}
	Atom type; int fmt; unsigned long n, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(dpy, win, a_paste, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &after, &data) != Success || !data)
		return;
	int cut = e->target == a_gnome && !strncmp((char *)data, "cut", 3);
	char *paths[512];
	int np = pm_parse_uris((char *)data, paths, 512);
	paste_files(paths, np, cut);
	for (int i = 0; i < np; i++) free(paths[i]);
	XFree(data);
}

/* ------------------------------------------------------------------ */
/* 版面                                                                */
/* ------------------------------------------------------------------ */

static int content_x(void) { return SB_W; }
static int content_y(void) { return TB_H + (view ? HEAD_H : 0); }
static int content_w(void) { return W - SB_W; }
static int content_h(void) { return H - TB_H - ST_H - (view ? HEAD_H : 0); }
static int cols(void) { return MAX(1, (content_w() - 16) / CELL_W); }

static int total_h(void)
{
	if (view) return nvis * ROW_H + 8;
	return ((nvis + cols() - 1) / cols()) * CELL_H + 16;
}

static void clamp_scroll(void)
{
	scrolly = CLAMP(scrolly, 0, MAX(0, total_h() - content_h()));
}

static void item_rect(int i, int *x, int *y, int *w, int *h)
{
	if (view) {
		*x = content_x(); *y = content_y() + 4 + i * ROW_H - scrolly; *w = content_w(); *h = ROW_H;
	} else {
		int c = cols(), pad = (content_w() - c * CELL_W) / 2;
		*x = content_x() + pad + (i % c) * CELL_W;
		*y = content_y() + 10 + (i / c) * CELL_H - scrolly;
		*w = CELL_W; *h = CELL_H;
	}
}

static int hit_item(int px, int py)
{
	if (px < content_x() || py < content_y() || py >= H - ST_H) return -1;
	for (int i = 0; i < nvis; i++) {
		int x, y, w, h;
		item_rect(i, &x, &y, &w, &h);
		if (view) {
			if (py >= y && py < y + h) return i;
		} else if (px >= x + 6 && px < x + w - 6 && py >= y && py < y + h - 4) {
			return i;
		}
	}
	return -1;
}

static void ensure_visible(int i)
{
	if (i < 0) return;
	int x, y, w, h;
	item_rect(i, &x, &y, &w, &h);
	int top = content_y(), bot = H - ST_H;
	if (y < top) scrolly -= top - y + 4;
	else if (y + h > bot) scrolly += y + h - bot + 4;
	clamp_scroll();
}

/* 工具列上的按鈕位置 */
enum { TB_BACK, TB_FWD, TB_ICONS, TB_LIST, TB_SEARCH, TB_N };
static void tb_rect(int k, int *x, int *y, int *w, int *h)
{
	*y = 12; *h = 28;
	switch (k) {
	case TB_BACK: *x = SB_W + 12; *w = 28; break;
	case TB_FWD: *x = SB_W + 42; *w = 28; break;
	case TB_SEARCH: *x = W - 200; *w = 184; break;
	case TB_ICONS: *x = W - 278; *w = 34; break;
	case TB_LIST: *x = W - 244; *w = 34; break;
	}
}

static int tb_hit(int px, int py)
{
	for (int k = 0; k < TB_N; k++) {
		int x, y, w, h;
		tb_rect(k, &x, &y, &w, &h);
		if (px >= x && px < x + w && py >= y && py < y + h) return k;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* 繪製                                                                */
/* ------------------------------------------------------------------ */

static void fmt_size(off_t s, char *out, size_t n)
{
	if (s < 1000) snprintf(out, n, _("%ld 位元組"), (long)s);
	else if (s < 1000000) snprintf(out, n, "%.1f KB", s / 1e3);
	else if (s < 1000000000) snprintf(out, n, "%.1f MB", s / 1e6);
	else snprintf(out, n, "%.2f GB", s / 1e9);
}

static void fmt_date(time_t t, char *out, size_t n)
{
	time_t now = time(NULL);
	struct tm a, b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	char hm[16];
	strftime(hm, sizeof hm, "%H:%M", &a);
	if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(out, n, _("今天 %s"), hm);
	else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1) snprintf(out, n, _("昨天 %s"), hm);
	else strftime(out, n, _("%Y年%-m月%-d日 %H:%M"), &a);
}

static void paint_icon(cairo_t *cr, cairo_surface_t *s, double x, double y, int size)
{
	if (!s) return;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	double k = (double)size / cairo_image_surface_get_width(s);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void draw_input(cairo_t *cr, double x, double y, double w, double h, const char *placeholder)
{
	pm_rrect(cr, x, y, w, h, 6, 6);
	pm_rgb(cr, c_bg);
	cairo_fill_preserve(cr);
	pm_rgb(cr, in.kind ? T->sel : c_line);
	cairo_set_line_width(cr, in.kind ? 2 : 1);
	cairo_stroke(cr);
	pm_font(cr, pm_size, 0);
	cairo_save(cr);
	cairo_rectangle(cr, x + 4, y, w - 8, h);
	cairo_clip(cr);
	if (!in.buf[0] && placeholder && !(wl && preedit[0])) {
		pm_rgb(cr, c_dim);
		pm_text(cr, x + 8, y + h / 2, placeholder);
		if (in.kind) { ime_x = (int)x + 8; ime_y = (int)y; ime_h = (int)h; }
	} else {
		char pre[1024];
		snprintf(pre, sizeof pre, "%.*s", in.cur, in.buf);
		double cx = pm_textw(cr, pre), tw = pm_textw(cr, in.buf);
		double pw = preedit[0] && wl ? pm_textw(cr, preedit) : 0;   /* 輸入法還沒確定的字，畫在游標處 */
		double shift = MAX(0, cx - (w - 20));   /* 游標超出時往左捲 */
		if (in.sel1 > in.sel0) {
			char a[1024], b[1024];
			snprintf(a, sizeof a, "%.*s", in.sel0, in.buf);
			snprintf(b, sizeof b, "%.*s", in.sel1, in.buf);
			double xa = pm_textw(cr, a), xb = pm_textw(cr, b);
			cairo_rectangle(cr, x + 8 + xa - shift, y + 4, xb - xa, h - 8);
			pm_rgba(cr, T->sel, 0.3);
			cairo_fill(cr);
		}
		pm_rgb(cr, c_text);
		if (pw > 0) {
			char all[1536];
			snprintf(all, sizeof all, "%s%s%s", pre, preedit, in.buf + in.cur);
			pm_text(cr, x + 8 - shift, y + h / 2, all);
			cairo_set_line_width(cr, 1);
			cairo_move_to(cr, x + 8 + cx - shift, y + h - 6.5);
			cairo_line_to(cr, x + 8 + cx + pw - shift, y + h - 6.5);
			cairo_stroke(cr);
			cx += pw;
		} else pm_text(cr, x + 8 - shift, y + h / 2, in.buf);
		(void)tw;
		pm_rgb(cr, T->sel);
		cairo_rectangle(cr, x + 8 + cx - shift, y + 5, 1.5, h - 10);
		cairo_fill(cr);
		if (in.kind) { ime_x = (int)(x + 8 + cx - shift - pw); ime_y = (int)y; ime_h = (int)h; }
	}
	cairo_restore(cr);
}

static void draw_sidebar(cairo_t *cr)
{
	pm_rgb(cr, c_side);
	cairo_rectangle(cr, 0, 0, SB_W, H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, SB_W - 1, 0, 1, H);
	cairo_fill(cr);
	int y = 16;
	for (int i = 0; i < nplaces; i++) {
		Place *p = &places[i];
		if (p->header) {
			y += i ? 10 : 0;
			pm_font(cr, 11, 1);
			pm_rgb(cr, c_dim);
			pm_text(cr, 16, y + 10, p->label);
			y += 22;
			continue;
		}
		if (!strcmp(p->path, cwd)) {
			pm_rrect(cr, 8, y, SB_W - 16, 26, 6, 6);
			pm_rgba(cr, 0x000000, pm_dark ? 0.35 : 0.09);
			cairo_fill(cr);
		}
		if (i == dp.hl_place) {   /* 拖放的目的地 */
			pm_rrect(cr, 8.5, y + 0.5, SB_W - 17, 25, 6, 6);
			pm_rgba(cr, T->sel, 0.25);
			cairo_fill_preserve(cr);
			pm_rgb(cr, T->sel);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		paint_icon(cr, get_icon(p->icon, 18), 18, y + 4, 18);
		pm_font(cr, pm_size, 0);
		pm_rgb(cr, p->unmounted ? c_dim : c_text);
		char buf[128];
		pm_ellipsize(cr, p->label, SB_W - (p->eject ? 82 : 60), buf, sizeof buf);
		pm_text(cr, 44, y + 13, buf);
		if (p->eject) {   /* ⏏ */
			double cx = SB_W - 26, cy = y + 13;
			pm_rgb(cr, c_dim);
			cairo_new_path(cr);
			cairo_move_to(cr, cx, cy - 5);
			cairo_line_to(cr, cx + 5.5, cy + 1);
			cairo_line_to(cr, cx - 5.5, cy + 1);
			cairo_close_path(cr);
			cairo_fill(cr);
			cairo_rectangle(cr, cx - 5.5, cy + 3, 11, 2);
			cairo_fill(cr);
		}
		y += 28;
	}
}

static int place_hit(int py)
{
	int y = 16;
	for (int i = 0; i < nplaces; i++) {
		if (places[i].header) { y += (i ? 10 : 0) + 22; continue; }
		if (py >= y && py < y + 28) return i;
		y += 28;
	}
	return -1;
}

static void chevron(cairo_t *cr, double cx, double cy, int left, int enabled)
{
	pm_rgb(cr, enabled ? c_text : c_dim);
	cairo_set_line_width(cr, 2);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, cx + (left ? 3 : -3), cy - 6);
	cairo_line_to(cr, cx + (left ? -3 : 3), cy);
	cairo_line_to(cr, cx + (left ? 3 : -3), cy + 6);
	cairo_stroke(cr);
}

static void draw_toolbar(cairo_t *cr)
{
	pm_rgb(cr, c_tool);
	cairo_rectangle(cr, SB_W, 0, W - SB_W, TB_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, SB_W, TB_H - 1, W - SB_W, 1);
	cairo_fill(cr);
	int x, y, w, h;
	tb_rect(TB_BACK, &x, &y, &w, &h);
	chevron(cr, x + w / 2.0, y + h / 2.0, 1, hpos > 0);
	tb_rect(TB_FWD, &x, &y, &w, &h);
	chevron(cr, x + w / 2.0, y + h / 2.0, 0, hpos < nhist - 1);

	/* 標題，或「前往」輸入框 */
	if (in.kind == IN_PATH) {
		draw_input(cr, SB_W + 82, 12, W - SB_W - 82 - 290, 28, NULL);
	} else {
		char t[256], buf[256];
		snprintf(t, sizeof t, "%s", !strcmp(cwd, "/") ? _("電腦") : !strcmp(cwd, trash_files) ? _("垃圾桶") : strrchr(cwd, '/') + 1);
		for (int i = 0; i < nplaces; i++)
			if (!places[i].header && !strcmp(places[i].path, cwd)) snprintf(t, sizeof t, "%s", places[i].label);
		pm_font(cr, 15, 1);
		pm_rgb(cr, c_text);
		pm_ellipsize(cr, t, W - SB_W - 82 - 300, buf, sizeof buf);
		pm_text(cr, SB_W + 84, TB_H / 2.0, buf);
	}

	/* 檢視切換 */
	for (int k = TB_ICONS; k <= TB_LIST; k++) {
		tb_rect(k, &x, &y, &w, &h);
		int on = (k == TB_LIST) == (view == 1);
		pm_rrect(cr, x, y, w, h, 6, 6);
		pm_rgba(cr, 0x000000, on ? (pm_dark ? 0.4 : 0.1) : 0);
		cairo_fill(cr);
		pm_rgb(cr, c_text);
		if (k == TB_ICONS) {
			for (int q = 0; q < 4; q++) {
				pm_rrect(cr, x + 10 + (q % 2) * 8, y + 7 + (q / 2) * 8, 6, 6, 1.5, 1.5);
				cairo_fill(cr);
			}
		} else {
			for (int q = 0; q < 3; q++) {
				cairo_rectangle(cr, x + 10, y + 8.5 + q * 5, 14, 2);
				cairo_fill(cr);
			}
		}
	}
	/* 搜尋框 */
	tb_rect(TB_SEARCH, &x, &y, &w, &h);
	if (in.kind == IN_SEARCH) {
		draw_input(cr, x, y, w, h, _("搜尋"));
	} else {
		pm_rrect(cr, x, y, w, h, 6, 6);
		pm_rgba(cr, 0x000000, pm_dark ? 0.3 : 0.06);
		cairo_fill(cr);
		pm_rgb(cr, c_dim);
		cairo_set_line_width(cr, 1.5);
		cairo_arc(cr, x + 15, y + h / 2.0 - 1, 5, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, x + 18.5, y + h / 2.0 + 2.5);
		cairo_line_to(cr, x + 22, y + h / 2.0 + 6);
		cairo_stroke(cr);
		pm_font(cr, pm_size, 0);
		pm_text(cr, x + 28, y + h / 2.0, filter[0] ? filter : _("搜尋"));
	}
}

/* 列表檢視各欄的位置（從內容區左邊算起）：名稱、修改日期、大小、種類；-1 = 收起來。
 * 視窗窄的時候先收起「種類」，再收起「修改日期」，名稱才不會被擠掉 */
static void list_cols(int xs[4])
{
	int cw = content_w();
	xs[0] = 12;
	if (cw >= 730) { xs[1] = cw - 470; xs[2] = cw - 270; xs[3] = cw - 160; }
	else if (cw >= 620) { xs[1] = cw - 360; xs[2] = cw - 160; xs[3] = -1; }
	else { xs[1] = -1; xs[2] = cw - 150; xs[3] = -1; }
}

/* 名稱欄可以用的寬度 */
static int name_w(void)
{
	int xs[4];
	list_cols(xs);
	return (xs[1] >= 0 ? xs[1] : xs[2]) - 60;
}

static void draw_list_header(cairo_t *cr)
{
	int x0 = content_x(), y = TB_H, cw = content_w();
	pm_rgb(cr, c_bg);
	cairo_rectangle(cr, x0, y, cw, HEAD_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, x0, y + HEAD_H - 1, cw, 1);
	cairo_fill(cr);
	static const char *names[] = { N_("名稱"), N_("修改日期"), N_("大小"), N_("種類") };
	int xs[4];
	list_cols(xs);
	for (int k = 0; k < 4; k++) if (xs[k] >= 0) xs[k] += x0;
	pm_font(cr, 12, 0);
	for (int k = 0; k < 4; k++) {
		if (xs[k] < 0) continue;
		pm_rgb(cr, sort_key == k ? c_text : c_dim);
		pm_text(cr, xs[k], y + HEAD_H / 2.0, _(names[k]));
		if (sort_key == k) {
			double ax = xs[k] + pm_textw(cr, _(names[k])) + 8, ay = y + HEAD_H / 2.0;
			cairo_move_to(cr, ax - 4, sort_desc ? ay - 2 : ay + 2);
			cairo_line_to(cr, ax, sort_desc ? ay + 2 : ay - 2);
			cairo_line_to(cr, ax + 4, sort_desc ? ay - 2 : ay + 2);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		if (k) {
			pm_rgb(cr, c_line);
			cairo_rectangle(cr, xs[k] - 10, y + 5, 1, HEAD_H - 10);
			cairo_fill(cr);
		}
	}
}

static int header_hit(int px)
{
	int xs[4];
	list_cols(xs);
	for (int k = 3; k > 0; k--)
		if (xs[k] >= 0 && px >= content_x() + xs[k] - 10) return k;
	return 0;
}

static void draw_content(cairo_t *cr)
{
	int x0 = content_x(), y0 = content_y(), cw = content_w(), ch = content_h();
	pm_rgb(cr, c_bg);
	cairo_rectangle(cr, x0, TB_H, cw, H - TB_H - ST_H);
	cairo_fill(cr);
	cairo_save(cr);
	cairo_rectangle(cr, x0, y0, cw, ch);
	cairo_clip(cr);
	unsigned selbg = focused ? T->sel : c_selin;
	unsigned seltx = focused ? T->sel_text : c_text;
	char buf[512], sz[48], dt[64];
	for (int i = 0; i < nvis; i++) {
		int x, y, w, h;
		item_rect(i, &x, &y, &w, &h);
		if (y + h < y0 || y > y0 + ch) continue;
		Entry *e = &ents[vis[i]];
		if (!e->thumb_tried && !e->dir) load_thumb(e);
		int renaming = in.kind == IN_RENAME && in.idx == i;
		if (i == dp.hl_item) {   /* 拖放的目的地 */
			if (view) pm_rrect(cr, x + 6.5, y + 0.5, w - 13, h - 1, 5, 5);
			else pm_rrect(cr, x + 4.5, y + 0.5, w - 9, h - 5, 8, 8);
			pm_rgba(cr, T->sel, 0.22);
			cairo_fill_preserve(cr);
			pm_rgb(cr, T->sel);
			cairo_set_line_width(cr, 1.5);
			cairo_stroke(cr);
		}
		if (view) {
			if (e->sel) {
				pm_rrect(cr, x + 6, y, w - 12, h, 5, 5);
				pm_rgb(cr, selbg);
				cairo_fill(cr);
			} else if (i % 2) {
				pm_rrect(cr, x + 6, y, w - 12, h, 5, 5);
				pm_rgb(cr, c_stripe);
				cairo_fill(cr);
			}
			paint_icon(cr, icon_for(e, 18), x + 14, y + 3, 18);
			unsigned tc = e->sel ? seltx : c_text, dc = e->sel ? seltx : c_dim;
			pm_font(cr, pm_size, 0);
			if (renaming) {
				draw_input(cr, x + 38, y + 1, name_w() + 10, h - 2, NULL);
			} else {
				pm_rgb(cr, tc);
				pm_ellipsize(cr, e->name, name_w(), buf, sizeof buf);
				pm_text(cr, x + 40, y + h / 2.0, buf);
			}
			pm_font(cr, 12, 0);
			pm_rgb(cr, dc);
			fmt_date(e->mtime, dt, sizeof dt);
			int xs[4];
			list_cols(xs);
			if (xs[1] >= 0) pm_text(cr, x + xs[1], y + h / 2.0, dt);
			if (e->dir) snprintf(sz, sizeof sz, "--");
			else fmt_size(e->size, sz, sizeof sz);
			pm_text(cr, x + xs[2], y + h / 2.0, sz);
			if (xs[3] >= 0) pm_text(cr, x + xs[3], y + h / 2.0, kind_of(e->mime));
		} else {
			double ix = x + (w - BIG) / 2.0, iy = y + 4;
			if (e->sel) {
				pm_rrect(cr, ix - 6, iy - 4, BIG + 12, BIG + 8, 8, 8);
				pm_rgba(cr, 0x000000, pm_dark ? 0.35 : 0.1);
				cairo_fill(cr);
			}
			cairo_surface_t *ic = icon_for(e, BIG);
			if (e->thumb) {
				/* 縮圖加細框 */
				paint_icon(cr, ic, ix, iy, BIG);
				cairo_rectangle(cr, ix + 0.5, iy + 0.5, BIG - 1, BIG - 1);
				pm_rgba(cr, 0x000000, 0.15);
				cairo_set_line_width(cr, 1);
				cairo_stroke(cr);
			} else {
				paint_icon(cr, ic, ix, iy, BIG);
			}
			if (e->link) {
				/* 捷徑：左下角小箭頭 */
				pm_rrect(cr, ix + 2, iy + BIG - 16, 14, 14, 3, 3);
				pm_rgb(cr, 0xffffff);
				cairo_fill_preserve(cr);
				pm_rgba(cr, 0x000000, 0.3);
				cairo_set_line_width(cr, 1);
				cairo_stroke(cr);
				pm_rgb(cr, 0x333333);
				cairo_move_to(cr, ix + 6, iy + BIG - 5);
				cairo_line_to(cr, ix + 12, iy + BIG - 11);
				cairo_move_to(cr, ix + 8, iy + BIG - 11);
				cairo_line_to(cr, ix + 12, iy + BIG - 11);
				cairo_line_to(cr, ix + 12, iy + BIG - 7);
				cairo_stroke(cr);
			}
			pm_font(cr, 12, 0);
			if (renaming) {
				draw_input(cr, x + 2, iy + BIG + 6, w - 4, 22, NULL);
				continue;
			}
			/* 檔名最多兩行 */
			char l1[256] = "", l2[256] = "";
			const char *nm = e->name;
			size_t len = strlen(nm), cut = len;
			snprintf(l1, sizeof l1, "%s", nm);
			while (pm_textw(cr, l1) > w - 10 && cut > 0) {
				do cut--; while (cut > 0 && ((unsigned char)nm[cut] & 0xc0) == 0x80);
				snprintf(l1, sizeof l1, "%.*s", (int)cut, nm);
			}
			if (cut < len) pm_ellipsize(cr, nm + cut, w - 10, l2, sizeof l2);
			double ty = iy + BIG + 16;
			for (int ln = 0; ln < 2; ln++) {
				const char *s = ln ? l2 : l1;
				if (!s[0]) break;
				double tw = pm_textw(cr, s);
				if (e->sel) {
					pm_rrect(cr, x + (w - tw) / 2 - 5, ty + ln * 16 - 8, tw + 10, 16, 4, 4);
					pm_rgb(cr, selbg);
					cairo_fill(cr);
				}
				pm_rgb(cr, e->sel ? seltx : c_text);
				pm_text(cr, x + (w - tw) / 2, ty + ln * 16, s);
			}
		}
	}
	if (!nvis) {
		pm_font(cr, pm_size, 0);
		pm_rgb(cr, c_dim);
		const char *m = filter[0] ? _("沒有符合的項目") : !strcmp(cwd, trash_files) ? _("垃圾桶是空的") : _("這個資料夾是空的");
		pm_text(cr, x0 + (cw - pm_textw(cr, m)) / 2, y0 + ch / 2.0, m);
	}
	/* 捲軸 */
	int th = total_h();
	if (th > ch) {
		double bh = MAX(30.0, (double)ch * ch / th), by = y0 + (ch - bh) * scrolly / (th - ch);
		pm_rrect(cr, x0 + cw - 9, by + 2, 6, bh - 4, 3, 3);
		pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.25);
		cairo_fill(cr);
	}
	cairo_restore(cr);
	if (view) draw_list_header(cr);
	if (dp.hl_here) {   /* 拖進目前的資料夾：整塊內容加框 */
		pm_rrect(cr, x0 + 3, TB_H + 3, cw - 6, H - TB_H - ST_H - 6, 8, 8);
		pm_rgb(cr, T->sel);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
	}
}

static void draw_status(cairo_t *cr)
{
	int y = H - ST_H;
	pm_rgb(cr, c_tool);
	cairo_rectangle(cr, SB_W, y, W - SB_W, ST_H);
	cairo_fill(cr);
	pm_rgb(cr, c_line);
	cairo_rectangle(cr, SB_W, y, W - SB_W, 1);
	cairo_fill(cr);
	char buf[300];
	if (status_msg[0] && pm_now_ms() < status_until) {
		snprintf(buf, sizeof buf, "%s", status_msg);
	} else {
		int n = nsel();
		char fb[48] = "";
		if (free_bytes) {
			char s[32];
			fmt_size((off_t)free_bytes, s, sizeof s);
			snprintf(fb, sizeof fb, _("，可用 %s"), s);
		}
		if (n) snprintf(buf, sizeof buf, _("已選取 %d 個，共 %d 個項目%s"), n, nvis, fb);
		else snprintf(buf, sizeof buf, _("%d 個項目%s"), nvis, fb);
	}
	pm_font(cr, 12, 0);
	pm_rgb(cr, c_dim);
	pm_text(cr, SB_W + (W - SB_W - pm_textw(cr, buf)) / 2, y + ST_H / 2.0, buf);
}

/* 外觀設定：啟動時與設定改變時呼叫 */
static void apply_settings(void)
{
	pm_size = pm_conf_num("appearance.font_size", def_font_size);
	pm_family = pm_conf_get("appearance.font", def_font_family);
	const char *it = pm_conf_get("appearance.icon_theme", def_icon_theme);
	if (strcmp(it, pm_icon_theme)) {   /* 換了圖示主題：清掉快取 */
		for (int i = 0; i < nicache; i++) if (icache[i].s) cairo_surface_destroy(icache[i].s);
		nicache = 0;
	}
	pm_icon_theme = it;
	pm_set_theme(pm_conf_int("appearance.dark", def_dark_mode), pm_conf_color("appearance.accent", 0));
	if (pm_dark) {
		c_bg = 0x1e1e1e; c_side = 0x2a2a2c; c_tool = 0x2f2f31; c_line = 0x3a3a3c; c_text = 0xf2f2f2;
		c_dim = 0x98989d; c_stripe = 0x242426; c_hl = 0x3a3a3c; c_selin = 0x48484a;
	} else {
		c_bg = 0xffffff; c_side = 0xf0eff2; c_tool = 0xf7f7f7; c_line = 0xdcdcdc; c_text = 0x1d1d1f;
		c_dim = 0x8e8e93; c_stripe = 0xf6f6f8; c_hl = 0xe4e4e7; c_selin = 0xdcdcdf;
	}
}

static void draw(void)
{
	Paint p;
	cairo_t *cr;
	if (wl) { cr = pmwl_begin(wl, &W, &H); if (!cr) return; }
	else cr = pm_paint_begin(&p, win, W, H);
	ime_h = 0;
	draw_content(cr);
	draw_toolbar(cr);
	draw_status(cr);
	draw_sidebar(cr);
	if (wl) {
		pmwl_end(wl, cr);
		/* 輸入法的選字列跟著輸入框的游標；沒有在輸入文字就關掉 */
		if (in.kind && ime_h) pmwl_ime(wl, 1, ime_x, ime_y, ime_h);
		else pmwl_ime(wl, 0, 0, 0, 0);
		return;
	}
	pm_paint_end(&p, cr, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 輸入框                                                              */
/* ------------------------------------------------------------------ */

static void input_start(int kind, int idx, const char *init)
{
	in.kind = kind;
	in.idx = idx;
	snprintf(in.buf, sizeof in.buf, "%s", init ? init : "");
	in.cur = (int)strlen(in.buf);
	in.sel0 = 0;
	in.sel1 = kind == IN_SEARCH ? 0 : in.cur;
	if (kind == IN_RENAME) {
		/* 預設反白副檔名之前的部分，直接打字就能改名字 */
		char *dot = strrchr(in.buf, '.');
		if (dot && dot != in.buf && !ents[vis[idx]].dir) in.cur = in.sel1 = (int)(dot - in.buf);
	}
	if (xic) XSetICFocus(xic);
}

static void input_end(int commit)
{
	int kind = in.kind;
	in.kind = IN_NONE;
	if (kind == IN_SEARCH) {
		snprintf(filter, sizeof filter, "%s", commit ? in.buf : "");
		apply_filter();
		return;
	}
	if (!commit) return;
	if (kind == IN_RENAME && in.idx < nvis) {
		Entry *e = &ents[vis[in.idx]];
		if (!in.buf[0] || !strcmp(in.buf, e->name)) return;
		if (strchr(in.buf, '/')) { set_status(_("檔名不能包含 /")); return; }
		char from[PATH_MAX], to[PATH_MAX];
		path_of(e, from, sizeof from);
		snprintf(to, sizeof to, "%s/%s", strcmp(cwd, "/") ? cwd : "", in.buf);
		struct stat st;
		if (!lstat(to, &st)) { set_status(_("已經有同名的項目")); return; }
		if (rename(from, to)) { set_status(strerror(errno)); return; }
		free(e->name);
		e->name = strdup(in.buf);
	} else if (kind == IN_PATH) {
		char p[PATH_MAX];
		if (in.buf[0] == '~') snprintf(p, sizeof p, "%s%s", getenv("HOME"), in.buf + 1);
		else if (in.buf[0] != '/') snprintf(p, sizeof p, "%s/%s", cwd, in.buf);
		else snprintf(p, sizeof p, "%s", in.buf);
		load_dir(p, 1);
	}
}

static void input_key(XKeyEvent *e, KeySym ks, const char *buf, int len)
{
	if (ks >= XK_Shift_L && ks <= XK_Hyper_R) return;   /* 單獨按修飾鍵不算打字（不然會把預設反白的檔名取消掉） */
	char *b = in.buf;
	int n = (int)strlen(b);
	int hassel = in.sel1 > in.sel0;
	/* 刪除或打字時先把反白的部分拿掉 */
	if (hassel && (ks == XK_BackSpace || ks == XK_Delete ||
	               (len > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127 && !(e->state & ControlMask)))) {
		memmove(b + in.sel0, b + in.sel1, n - in.sel1 + 1);
		in.cur = in.sel0;
		in.sel0 = in.sel1 = 0;
		n = (int)strlen(b);
		if (ks == XK_BackSpace || ks == XK_Delete) ks = NoSymbol, len = 0;
	}
	if (ks != NoSymbol && !((e->state & ControlMask) && (ks == XK_a || ks == XK_A)))
		in.sel0 = in.sel1 = 0;
	switch (ks) {
	case XK_Escape: input_end(0); return;
	case XK_Return: case XK_KP_Enter: input_end(1); return;
	case XK_BackSpace:
		if (in.cur > 0) {
			int s = in.cur;
			do s--; while (s > 0 && ((unsigned char)b[s] & 0xc0) == 0x80);
			memmove(b + s, b + in.cur, n - in.cur + 1);
			in.cur = s;
		}
		break;
	case XK_Delete:
		if (in.cur < n) {
			int s = in.cur;
			do s++; while (s < n && ((unsigned char)b[s] & 0xc0) == 0x80);
			memmove(b + in.cur, b + s, n - s + 1);
		}
		break;
	case XK_Left:
		if (in.cur > 0) do in.cur--; while (in.cur > 0 && ((unsigned char)b[in.cur] & 0xc0) == 0x80);
		break;
	case XK_Right:
		if (in.cur < n) do in.cur++; while (in.cur < n && ((unsigned char)b[in.cur] & 0xc0) == 0x80);
		break;
	case XK_Home: in.cur = 0; break;
	case XK_End: in.cur = n; break;
	default:
		if ((e->state & ControlMask) && (ks == XK_a || ks == XK_A)) { in.sel0 = 0; in.sel1 = in.cur = n; break; }
		if (len > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127 && n + len < (int)sizeof in.buf - 1) {
			memmove(b + in.cur + len, b + in.cur, n - in.cur + 1);
			memcpy(b + in.cur, buf, len);
			in.cur += len;
		}
	}
	if (in.kind == IN_SEARCH) {
		snprintf(filter, sizeof filter, "%s", in.buf);
		apply_filter();
		scrolly = 0;
	}
}

/* ------------------------------------------------------------------ */
/* 選取與指令                                                          */
/* ------------------------------------------------------------------ */

static void select_only(int i)
{
	for (int k = 0; k < nents; k++) ents[k].sel = 0;
	if (i >= 0 && i < nvis) ents[vis[i]].sel = 1;
	cursor_i = anchor = i;
}

static void select_range(int a, int b)
{
	for (int k = 0; k < nents; k++) ents[k].sel = 0;
	if (a > b) { int t = a; a = b; b = t; }
	for (int i = MAX(a, 0); i <= b && i < nvis; i++) ents[vis[i]].sel = 1;
}

static void new_folder(void)
{
	char p[PATH_MAX];
	unique_path(strcmp(cwd, "/") ? cwd : "", _("未命名資料夾"), p, sizeof p);
	if (mkdir(p, 0755)) { set_status(strerror(errno)); return; }
	read_dir(0);
	const char *base = strrchr(p, '/') + 1;
	for (int i = 0; i < nvis; i++)
		if (!strcmp(ents[vis[i]].name, base)) {
			select_only(i);
			ensure_visible(i);
			input_start(IN_RENAME, i, base);
		}
}

static void start_rename(void)
{
	if (nsel() != 1) return;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel) { ensure_visible(i); input_start(IN_RENAME, i, ents[vis[i]].name); }
}

static void toggle_hidden(void) { hidden = !hidden; read_dir(1); }
static void set_view(int v) { view = v; scrolly = 0; ensure_visible(cursor_i); }

/* 右鍵選單用的包裝 */
static void m_open(const Arg *a) { (void)a; open_selected(); }

static int is_media(const Entry *e) { return e->mime && (!strncmp(e->mime, "audio/", 6) || !strncmp(e->mime, "video/", 6)); }

/* 選取的音樂、影片、資料夾一次交給影音播放器（成為一個播放清單） */
static void m_play(const Arg *a)
{
	(void)a;
	int n;
	char **v = selected_paths(&n);
	char **argv = malloc((size_t)(n + 2) * sizeof *argv);
	int k = 0;
	argv[k++] = "pomelo-player";
	for (int i = 0; i < n; i++) argv[k++] = v[i];
	argv[k] = NULL;
	exec_detached(argv);
	free(argv);
	free_paths(v, n);
}

static void m_play_here(const Arg *a) { (void)a; char *argv[] = { "pomelo-player", cwd, NULL }; exec_detached(argv); }
static void m_open_new(const Arg *a)
{
	(void)a;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel && ents[vis[i]].dir) {
			char p[PATH_MAX];
			path_of(&ents[vis[i]], p, sizeof p);
			new_window(p);
		}
}
static void m_term_sel(const Arg *a)
{
	(void)a;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel && ents[vis[i]].dir) {
			char p[PATH_MAX];
			path_of(&ents[vis[i]], p, sizeof p);
			open_terminal(p);
			return;
		}
}
static void m_term_here(const Arg *a) { (void)a; open_terminal(cwd); }

/* 病毒掃描：打開系統設定的病毒掃描頁（沒裝 ClamAV 時那邊會問要不要裝）；路徑當參數傳，不經過 shell */
static void scan_virus(const char *dir)
{
	pid_t p = fork();
	if (p == 0) {
		if (dpy) CLOSE_X();
		setsid();
		if (fork() == 0) { execlp("pomelo-system", "pomelo-system", "--page", "virus", "--path", dir, (char *)NULL); _exit(127); }
		_exit(0);
	}
	if (p > 0) waitpid(p, NULL, 0);
}
static void m_scan_sel(const Arg *a)
{
	(void)a;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel && ents[vis[i]].dir) {
			char p[PATH_MAX];
			path_of(&ents[vis[i]], p, sizeof p);
			scan_virus(p);
			return;
		}
}
static void m_scan_here(const Arg *a) { (void)a; scan_virus(cwd); }
/* 誤刪救援：右鍵「從備份還原此檔案的舊版本…」。只在有裝 pomelo-backup、而且這個檔案真的在某個備份計畫裡、備份位置現在讀得到時才顯示 */
static int backup_run(const char *opt, const char *path, int wait)
{
	pid_t p = fork();
	if (p < 0) return -1;
	if (p == 0) {
		if (!wait) { if (pm_dpy) close(ConnectionNumber(pm_dpy)); setsid(); }
		else { int dn = open("/dev/null", O_RDWR); if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); } }
		execlp("pomelo-backup", "pomelo-backup", opt, path, (char *)NULL);
		_exit(127);
	}
	if (!wait) return 0;
	int st = 0;
	waitpid(p, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static int selected_single_file(char *out, size_t n)
{
	int cnt = 0;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel) { if (ents[vis[i]].dir) return 0; path_of(&ents[vis[i]], out, n); cnt++; }
	return cnt == 1;
}
static void m_backup_versions(const Arg *a)
{
	(void)a;
	char p[PATH_MAX];
	if (selected_single_file(p, sizeof p)) backup_run("--restore-file", p, 0);
}
static void m_copy(const Arg *a) { (void)a; clip_set(0); }
static void m_cut(const Arg *a) { (void)a; clip_set(1); }
static void m_paste(const Arg *a) { (void)a; paste(); }
static void m_rename(const Arg *a) { (void)a; start_rename(); }
static void m_trash(const Arg *a) { (void)a; move_to_trash(); }
static void m_delete(const Arg *a) { (void)a; confirm_delete(); }
static void m_copypath(const Arg *a) { (void)a; copy_path(); }
static void m_newfolder(const Arg *a) { (void)a; new_folder(); }

/* 新增空白的文字檔，然後直接進入改名 */
static void new_file(void)
{
	char p[PATH_MAX];
	unique_path(strcmp(cwd, "/") ? cwd : "", _("未命名.txt"), p, sizeof p);
	int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) { set_status(strerror(errno)); return; }
	close(fd);
	read_dir(0);
	const char *base = strrchr(p, '/') + 1;
	for (int i = 0; i < nvis; i++)
		if (!strcmp(ents[vis[i]].name, base)) {
			select_only(i);
			ensure_visible(i);
			input_start(IN_RENAME, i, base);
		}
}
static void m_newfile(const Arg *a) { (void)a; new_file(); }
static void m_hidden(const Arg *a) { (void)a; toggle_hidden(); }
static void m_view(const Arg *a) { set_view(a->i); }
static void m_selall(const Arg *a) { (void)a; for (int i = 0; i < nvis; i++) ents[vis[i]].sel = 1; }

/* 壓縮管理員（pomelo-archive）：選到壓縮檔可以「解壓縮到這裡」「用壓縮管理員打開」，任何檔案都可以「壓縮…」。
 * 判斷看類型（shared-mime-info 的副檔名表）也看副檔名（有些系統的表沒有 tzst、tlz 這些）；RAR 不在名單裡（不支援） */
static int is_archive(const Entry *e)
{
	if (!e || e->dir) return 0;
	static const char *const mimes[] = {
		"application/zip", "application/x-zip-compressed", "application/java-archive", "application/x-java-archive",
		"application/epub+zip", "application/vnd.android.package-archive", "application/x-7z-compressed",
		"application/x-tar", "application/x-compressed-tar", "application/x-bzip-compressed-tar",
		"application/x-bzip2-compressed-tar", "application/x-xz-compressed-tar", "application/x-zstd-compressed-tar",
		"application/x-lzip-compressed-tar", "application/x-lz4-compressed-tar", "application/x-lzma-compressed-tar",
		"application/x-cpio", "application/x-archive", "application/x-deb", "application/vnd.debian.binary-package",
		"application/x-rpm", "application/x-iso9660-image", "application/x-cd-image", "application/x-squashfs",
		"application/vnd.squashfs", "application/x-raw-disk-image", "application/gzip", "application/x-gzip",
		"application/x-bzip", "application/x-bzip2", "application/x-xz", "application/zstd", "application/x-zstd",
		"application/x-lzip", "application/x-lzma", "application/x-lz4", "application/x-compress",
		"application/vnd.ms-cab-compressed", "application/x-lha", "application/x-lzh-compressed",
		"application/pgp-encrypted", NULL };
	for (int i = 0; mimes[i]; i++)
		if (e->mime && !strcmp(e->mime, mimes[i])) return 1;
	static const char *const exts[] = { "zip", "jar", "apk", "epub", "7z", "tar", "tgz", "tbz", "tbz2", "txz", "tzst", "tlz",
	                                    "cpio", "ar", "deb", "rpm", "iso", "squashfs", "sfs", "sq", "img", "gz", "bz2", "xz",
	                                    "zst", "lz", "lzma", "lz4", "z", "cab", "gpg", NULL };
	const char *dot = strrchr(e->name, '.');
	if (!dot || dot == e->name) return 0;
	for (int i = 0; exts[i]; i++)
		if (!strcasecmp(dot + 1, exts[i])) return 1;
	return 0;
}

/* 在背景執行 pomelo-archive（mode 可以是 NULL = 一般開啟）；paths 全部當參數傳，不經過 shell */
static void run_archive(const char *mode, char **v, int n)
{
	char **argv = malloc((size_t)(n + 3) * sizeof *argv);
	int k = 0;
	argv[k++] = "pomelo-archive";
	if (mode) argv[k++] = (char *)mode;
	for (int i = 0; i < n; i++) argv[k++] = v[i];
	argv[k] = NULL;
	exec_detached(argv);
	free(argv);
}
static void m_extract_here(const Arg *a)
{
	(void)a;
	int n;
	char **v = selected_paths(&n);
	if (n) run_archive("--extract-here", v, n);
	free_paths(v, n);
}
static void m_archive_open(const Arg *a)
{
	(void)a;
	int n;
	char **v = selected_paths(&n);
	for (int i = 0; i < n; i++) run_archive(NULL, &v[i], 1);   /* 每個壓縮檔一個視窗 */
	free_paths(v, n);
}
static void m_compress(const Arg *a)
{
	(void)a;
	int n;
	char **v = selected_paths(&n);
	if (n) run_archive("--compress", v, n);
	free_paths(v, n);
}

static void context_menu(int x, int y, int on_item)
{
	MenuItem m[24];
	int k = 0, in_trash = !strcmp(cwd, trash_files);
	int anydir = 0, anymedia = 0, here_media = 0, n = nsel();
	for (int i = 0; i < nvis; i++) {
		if (ents[vis[i]].sel && ents[vis[i]].dir) anydir = 1;
		if (ents[vis[i]].sel && is_media(&ents[vis[i]])) anymedia = 1;
		if (is_media(&ents[vis[i]])) here_media = 1;
	}
	int player = have("pomelo-player");
	int arch = have("pomelo-archive") && !in_trash, all_arch = n > 0;
	for (int i = 0; i < nvis; i++)
		if (ents[vis[i]].sel && !is_archive(&ents[vis[i]])) all_arch = 0;
	if (on_item && n) {
		m[k++] = (MenuItem){ _("打開"), m_open, {0} };
		if (player && !in_trash && (anymedia || anydir)) m[k++] = (MenuItem){ _("用影音播放器播放"), m_play, {0} };
		if (anydir) {
			m[k++] = (MenuItem){ _("在新視窗中打開"), m_open_new, {0} };
			m[k++] = (MenuItem){ _("在終端機中打開"), m_term_sel, {0} };
			if (!in_trash && have("pomelo-system")) m[k++] = (MenuItem){ _("掃描病毒…"), m_scan_sel, {0} };
		}
		if (arch) m[k++] = (MenuItem){ NULL, NULL, {0} };
		if (arch && all_arch) {
			m[k++] = (MenuItem){ _("解壓縮到這裡"), m_extract_here, {0} };
			m[k++] = (MenuItem){ _("用壓縮管理員打開"), m_archive_open, {0} };
		}
		if (arch) m[k++] = (MenuItem){ _("壓縮…"), m_compress, {0} };
		m[k++] = (MenuItem){ NULL, NULL, {0} };
		if (in_trash) {
			m[k++] = (MenuItem){ _("放回原處"), restore_selected, {0} };
			m[k++] = (MenuItem){ _("永久刪除…"), m_delete, {0} };
		} else {
			m[k++] = (MenuItem){ _("拷貝"), m_copy, {0} };
			m[k++] = (MenuItem){ _("剪下"), m_cut, {0} };
			if (n == 1) m[k++] = (MenuItem){ _("重新命名…"), m_rename, {0} };
			m[k++] = (MenuItem){ _("拷貝路徑"), m_copypath, {0} };
			{ char bp[PATH_MAX]; if (n == 1 && !anydir && have("pomelo-backup") && selected_single_file(bp, sizeof bp) && backup_run("--covers", bp, 1) == 0) m[k++] = (MenuItem){ _("從備份還原此檔案的舊版本…"), m_backup_versions, {0} }; }
			m[k++] = (MenuItem){ NULL, NULL, {0} };
			m[k++] = (MenuItem){ _("丟到垃圾桶"), m_trash, {0} };
		}
	} else {
		if (in_trash) {
			m[k++] = (MenuItem){ _("清空垃圾桶…"), ask_empty_trash, {0} };
		} else {
			m[k++] = (MenuItem){ _("新增資料夾"), m_newfolder, {0} };
			m[k++] = (MenuItem){ _("新增文件"), m_newfile, {0} };
			m[k++] = (MenuItem){ _("貼上"), m_paste, {0} };
			m[k++] = (MenuItem){ _("全選"), m_selall, {0} };
			if (player && here_media) m[k++] = (MenuItem){ _("播放這個資料夾的音樂與影片"), m_play_here, {0} };
		}
		m[k++] = (MenuItem){ _("在終端機中打開"), m_term_here, {0} };
		if (!in_trash && have("pomelo-system")) m[k++] = (MenuItem){ _("掃描這個資料夾的病毒…"), m_scan_here, {0} };
		m[k++] = (MenuItem){ NULL, NULL, {0} };
		m[k++] = (MenuItem){ hidden ? _("隱藏隱藏檔") : _("顯示隱藏檔"), m_hidden, {0} };
		m[k++] = (MenuItem){ view ? _("以圖示顯示") : _("以列表顯示"), m_view, { .i = !view } };
	}
	PMWL_POPUP(wl, m, k, x, y);
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */

static void move_cursor(int d)
{
	if (!nvis) return;
	int i = cursor_i < 0 ? 0 : CLAMP(cursor_i + d, 0, nvis - 1);
	select_only(i);
	ensure_visible(i);
}

static void keypress(XKeyEvent *e)
{
	if (dr.active) {   /* 拖曳中：Esc 取消（原生的拖曳由 compositor 處理 Esc） */
		if (!wl && XLookupKeysym(e, 0) == XK_Escape) drag_cancel();
		return;
	}
	char buf[64];
	KeySym ks = NoSymbol, k = NoSymbol;
	Status st = 0;
	int len = wl ? pmwl_xkey(e, buf, sizeof buf, &ks, &k)
	             : xic ? Xutf8LookupString(xic, e, buf, sizeof buf - 1, &ks, &st)
	                   : XLookupString(e, buf, sizeof buf - 1, &ks, NULL);
	if (!wl && xic && st == XBufferOverflow) return;
	if (len < 0) len = 0;
	buf[len] = 0;
	if (in.kind) { preedit[0] = 0; input_key(e, ks, buf, len); return; }

	int cmd = e->state & (ControlMask | Mod4Mask), shift = e->state & ShiftMask, alt = e->state & Mod1Mask;
	if (!wl) k = XLookupKeysym(e, 0);
	if (cmd) {
		switch (k) {
		case XK_a: m_selall(NULL); return;
		case XK_c: if (shift) copy_path(); else clip_set(0); return;
		case XK_x: clip_set(1); return;
		case XK_v: paste(); return;
		case XK_n: if (shift) new_folder(); else new_window(cwd); return;
		case XK_w: exit(0);
		case XK_h: toggle_hidden(); return;
		case XK_period: toggle_hidden(); return;
		case XK_l: input_start(IN_PATH, 0, cwd); return;
		case XK_f: input_start(IN_SEARCH, 0, filter); return;
		case XK_r: read_dir(1); return;
		case XK_1: set_view(0); return;
		case XK_2: set_view(1); return;
		case XK_Up: go_parent(); return;
		case XK_Down: open_selected(); return;
		case XK_bracketleft: go_back(); return;
		case XK_bracketright: go_forward(); return;
		case XK_BackSpace: move_to_trash(); return;
		case XK_t: m_term_here(NULL); return;
		}
		return;
	}
	if (alt) {
		if (ks == XK_Left) go_back();
		else if (ks == XK_Right) go_forward();
		else if (ks == XK_Up) go_parent();
		return;
	}
	int c = view ? 1 : cols();
	switch (ks) {
	case XK_Return: case XK_KP_Enter: open_selected(); return;
	case XK_BackSpace: go_parent(); return;
	case XK_Delete: if (shift || !strcmp(cwd, trash_files)) confirm_delete(); else move_to_trash(); return;
	case XK_F2: start_rename(); return;
	case XK_F5: read_dir(1); return;
	case XK_Escape:
		if (filter[0]) { filter[0] = 0; apply_filter(); }
		else select_only(-1);
		return;
	case XK_Up: move_cursor(-c); break;
	case XK_Down: move_cursor(c); break;
	case XK_Left: if (!view) move_cursor(-1); return;
	case XK_Right: if (!view) move_cursor(1); return;
	case XK_Home: move_cursor(-nvis); return;
	case XK_End: move_cursor(nvis); return;
	case XK_Prior: move_cursor(-c * MAX(1, content_h() / (view ? ROW_H : CELL_H))); return;
	case XK_Next: move_cursor(c * MAX(1, content_h() / (view ? ROW_H : CELL_H))); return;
	default:
		/* 直接打字：跳到名稱開頭符合的項目 */
		if (len > 0 && (unsigned char)buf[0] > 32) {
			long now = pm_now_ms();
			if (now - typeahead_at > 1000) typeahead[0] = 0;
			typeahead_at = now;
			if (strlen(typeahead) + len < sizeof typeahead - 1) strcat(typeahead, buf);
			/* 累積的字找不到時，改用這次打的字重新找 */
			for (int pass = 0; pass < 2; pass++) {
				int hit = -1;
				for (int i = 0; i < nvis && hit < 0; i++)
					if (!strncasecmp(ents[vis[i]].name, typeahead, strlen(typeahead))) hit = i;
				if (hit >= 0) { select_only(hit); ensure_visible(hit); break; }
				snprintf(typeahead, sizeof typeahead, "%s", buf);
			}
		}
	}
	if (shift && (ks == XK_Up || ks == XK_Down) && anchor >= 0) select_range(anchor, cursor_i);
}

/* ------------------------------------------------------------------ */
/* 拖放（XDND）：檔案拖到別的資料夾、側邊欄、別的視窗或別的程式；     */
/* 也收別的程式拖進來的檔案（text/uri-list）                           */
/* ------------------------------------------------------------------ */

#define DRAG_S 72   /* 拖曳時跟著游標的圖示 */
#define MAX_DROP 4096

static void xdnd_send(Window to, Window about, Atom type, long l1, long l2, long l3, long l4)
{
	XEvent ev;
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = about;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = (long)win;
	ev.xclient.data.l[1] = l1;
	ev.xclient.data.l[2] = l2;
	ev.xclient.data.l[3] = l3;
	ev.xclient.data.l[4] = l4;
	XSendEvent(dpy, to, False, NoEventMask, &ev);
}

static long prop_long(Window w, Atom prop, Atom type)
{
	Atom t; int fmt; unsigned long n, after;
	unsigned char *d = NULL;
	long v = 0;
	if (XGetWindowProperty(dpy, w, prop, 0, 1, False, type, &t, &fmt, &n, &after, &d) == Success && d) {
		if (t == type && fmt == 32 && n == 1) v = *(long *)d;
		XFree(d);
	}
	return v;
}

/* 游標下面第一個說自己收拖放的視窗（XdndAware）；to 是要送訊息的視窗（有 XdndProxy 就送給它） */
static Window xdnd_find(int rx, int ry, Window *to, int *ver)
{
	Window w = pm_root, child;
	int x, y;
	for (int depth = 0; depth < 32; depth++) {
		if (!XTranslateCoordinates(dpy, pm_root, w, rx, ry, &x, &y, &child) || child == None || child == dr.icon) break;
		w = child;
		Window p = (Window)prop_long(w, a_xdndproxy, XA_WINDOW);
		if (p && (Window)prop_long(p, a_xdndproxy, XA_WINDOW) != p) p = None;
		int v = (int)prop_long(p ? p : w, a_xdndaware, XA_ATOM);
		if (v >= 3) { *to = p ? p : w; *ver = MIN(v, 5); return w; }
	}
	*to = None;
	*ver = 0;
	return None;
}

/* 拖曳圖示：檔案圖示、數量，角落標記拷貝（＋）或不能放（⊘）。用 Shape 挖成圖示的形狀 */
static void drag_icon_set(int st)
{
	if (st == dr.icon_state || !dr.icon || !dr.img) return;
	dr.icon_state = st;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DRAG_S, DRAG_S);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_surface(cr, dr.img, 0, 0);
	cairo_paint(cr);
	if (st == 1 || st == 2) {
		double cx = DRAG_S - 14, cy = DRAG_S - 14;
		cairo_arc(cr, cx, cy, 10, 0, 2 * M_PI);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy, 8.5, 0, 2 * M_PI);
		if (st == 1) cairo_set_source_rgb(cr, 0.19, 0.69, 0.33);
		else cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_line_width(cr, 2);
		if (st == 1) {
			cairo_move_to(cr, cx - 4.5, cy); cairo_line_to(cr, cx + 4.5, cy);
			cairo_move_to(cr, cx, cy - 4.5); cairo_line_to(cr, cx, cy + 4.5);
		} else {
			cairo_arc(cr, cx, cy, 4.5, 0, 2 * M_PI);
			cairo_move_to(cr, cx - 3.2, cy + 3.2); cairo_line_to(cr, cx + 3.2, cy - 3.2);
		}
		cairo_stroke(cr);
	}
	cairo_destroy(cr);
	cairo_surface_flush(s);
	if (shape_ok) {
		int stride = cairo_image_surface_get_stride(s), bpl = (DRAG_S + 7) / 8;
		unsigned char *px = cairo_image_surface_get_data(s);
		char bits[DRAG_S * ((DRAG_S + 7) / 8)];
		memset(bits, 0, sizeof bits);
		for (int y = 0; y < DRAG_S; y++)
			for (int x = 0; x < DRAG_S; x++)
				if ((((uint32_t *)(px + y * stride))[x] >> 24) >= 96) bits[y * bpl + x / 8] |= (char)(1 << (x % 8));
		Pixmap mask = XCreateBitmapFromData(dpy, dr.icon, bits, DRAG_S, DRAG_S);
		XShapeCombineMask(dpy, dr.icon, ShapeBounding, 0, 0, mask, ShapeSet);
		XFreePixmap(dpy, mask);
	}
	/* 畫在背景圖上，不用處理 Expose */
	Pixmap pm = XCreatePixmap(dpy, dr.icon, DRAG_S, DRAG_S, DefaultDepth(dpy, scr));
	cairo_surface_t *xs = cairo_xlib_surface_create(dpy, pm, DefaultVisual(dpy, scr), DRAG_S, DRAG_S);
	cairo_t *xc = cairo_create(xs);
	pm_rgb(xc, c_bg);
	cairo_paint(xc);
	cairo_set_source_surface(xc, s, 0, 0);
	cairo_paint(xc);
	cairo_destroy(xc);
	cairo_surface_destroy(xs);
	XSetWindowBackgroundPixmap(dpy, dr.icon, pm);
	XFreePixmap(dpy, pm);
	XClearWindow(dpy, dr.icon);
	cairo_surface_destroy(s);
}

static void drag_start(int rx, int ry, Time t)
{
	if (dr.paths) free_paths(dr.paths, dr.n);
	free(dr.text);
	dr.text = NULL;
	dr.paths = selected_paths(&dr.n);
	dr.pending = dr.defer = 0;
	if (!dr.n) { free(dr.paths); dr.paths = NULL; return; }
	size_t len = 1;
	for (int i = 0; i < dr.n; i++) len += strlen(dr.paths[i]) + 1;
	dr.text = malloc(len);
	dr.text[0] = 0;
	for (int i = 0; i < dr.n; i++) { strcat(dr.text, dr.paths[i]); if (i < dr.n - 1) strcat(dr.text, "\n"); }

	dr.active = 1;
	dr.target = dr.to = None;
	dr.waiting = dr.moved = dr.accepted = 0;
	dr.drop_deadline = 0;
	XSetSelectionOwner(dpy, a_xdndsel, win, t);
	Atom acts[] = { a_xdndmove, a_xdndcopy };
	XChangeProperty(dpy, win, a_xdndactions, XA_ATOM, 32, PropModeReplace, (unsigned char *)acts, 2);
	drag_build_img();

	if (!dr.icon) {
		XSetWindowAttributes a = { .override_redirect = True, .save_under = True, .background_pixel = c_bg };
		dr.icon = XCreateWindow(dpy, pm_root, 0, 0, DRAG_S, DRAG_S, 0, CopyFromParent, InputOutput, CopyFromParent,
		                        CWOverrideRedirect | CWSaveUnder | CWBackPixel, &a);
		Atom wt = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), dnd = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);
		XChangeProperty(dpy, dr.icon, wt, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dnd, 1);
		if (shape_ok) XShapeCombineRectangles(dpy, dr.icon, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);   /* 點不到，也不會擋到找目標 */
	}
	dr.icon_state = -1;
	drag_icon_set(0);
	XMoveWindow(dpy, dr.icon, rx + 6, ry + 6);
	XMapRaised(dpy, dr.icon);
	XGrabPointer(dpy, win, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	             GrabModeAsync, GrabModeAsync, None, None, t);
}

/* 原生：開始一個 Wayland 拖放（圖示由 compositor 跟著游標畫；X 的角落標記做不到，目標接受什麼動作游標會告訴使用者） */
static void native_drag_begin(void)
{
	if (dr.paths) free_paths(dr.paths, dr.n);
	free(dr.text);
	dr.text = NULL;
	dr.paths = selected_paths(&dr.n);
	dr.pending = dr.defer = 0;
	if (!dr.n) { free(dr.paths); dr.paths = NULL; return; }
	drag_build_img();
	if (pmwl_drag_start_files(wl, (const char *const *)dr.paths, dr.n, PMWL_DND_COPY | PMWL_DND_MOVE, dr.img, 0, 0)) dr.active = 1;
}

static void drag_build_img(void)
{
	/* 圖示：按住的那一個（或第一個選取的），多個的話後面疊一個淡的，右上角是數量 */
	Entry *e0 = dr.click_i >= 0 && dr.click_i < nvis && ents[vis[dr.click_i]].sel ? &ents[vis[dr.click_i]] : NULL;
	for (int i = 0; i < nvis && !e0; i++) if (ents[vis[i]].sel) e0 = &ents[vis[i]];
	if (dr.img) cairo_surface_destroy(dr.img);
	dr.img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, DRAG_S, DRAG_S);
	cairo_t *cr = cairo_create(dr.img);
	cairo_surface_t *ic = e0 ? icon_for(e0, 48) : NULL;
	if (dr.n > 1 && ic) {
		cairo_push_group(cr);
		paint_icon(cr, ic, 16, 6, 48);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, 0.5);
	}
	if (ic) paint_icon(cr, ic, 8, 14, 48);
	else {
		pm_rrect(cr, 18, 14, 36, 46, 4, 4);
		pm_rgb(cr, 0xf2f2f2);
		cairo_fill_preserve(cr);
		pm_rgb(cr, 0x9a9a9a);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	if (dr.n > 1) {
		char num[16];
		snprintf(num, sizeof num, "%d", dr.n);
		pm_font(cr, 11, 1);
		double tw = pm_textw(cr, num), bw = MAX(20, tw + 10);
		pm_rrect(cr, DRAG_S - bw - 2, 2, bw, 20, 10, 10);
		cairo_set_source_rgb(cr, 0.90, 0.28, 0.30);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		pm_text(cr, DRAG_S - bw - 2 + (bw - tw) / 2, 12, num);
	}
	cairo_destroy(cr);
}

static long dr_sent_at;

static void drag_send_position(void)
{
	xdnd_send(dr.to, dr.target, a_xdndposition, 0, ((long)(dr.rx & 0xffff) << 16) | (dr.ry & 0xffff), (long)dr.t, (long)dr.action);
	dr.waiting = 1;
	dr.moved = 0;
	dr_sent_at = pm_now_ms();
}

static void drag_move(int rx, int ry, unsigned state, Time t)
{
	dr.rx = rx;
	dr.ry = ry;
	dr.t = t;
	dr.action = state & ControlMask ? a_xdndcopy : a_xdndmove;   /* 按住 Ctrl 一律拷貝 */
	XMoveWindow(dpy, dr.icon, rx + 6, ry + 6);
	Window to;
	int ver;
	Window tw = xdnd_find(rx, ry, &to, &ver);
	if (tw != dr.target) {
		if (dr.target) xdnd_send(dr.to, dr.target, a_xdndleave, 0, 0, 0, 0);
		dr.target = tw;
		dr.to = to;
		dr.ver = ver;
		dr.waiting = dr.moved = dr.accepted = 0;
		if (!tw) drag_icon_set(2);
		else xdnd_send(to, tw, a_xdndenter, (long)ver << 24, (long)a_urilist, (long)a_utf8, (long)a_textplain);
	}
	if (!dr.target) return;
	if (dr.waiting && pm_now_ms() - dr_sent_at < 500) dr.moved = 1;   /* 等對方回覆完再送下一個位置 */
	else drag_send_position();
}

static void drag_finish(void)
{
	if (dr.target) {
		if (dr.accepted) xdnd_send(dr.to, dr.target, a_xdnddrop, 0, (long)dr.t, 0, 0);
		else xdnd_send(dr.to, dr.target, a_xdndleave, 0, 0, 0, 0);
	}
	dr.target = None;
	dr.waiting = 0;
	dr.drop_deadline = 0;
}

static void drag_stop_visual(void)
{
	XUngrabPointer(dpy, CurrentTime);
	if (dr.icon) XUnmapWindow(dpy, dr.icon);
	dr.active = 0;
}

static void drag_release(Time t)
{
	drag_stop_visual();
	dr.t = t;
	if (dr.target && dr.waiting) { dr.drop_deadline = pm_now_ms() + 1500; return; }   /* 等對方回覆最後的位置再放 */
	drag_finish();
}

static void drag_cancel(void)
{
	drag_stop_visual();
	dr.accepted = 0;
	drag_finish();
}

static void drag_status(XClientMessageEvent *e)
{
	if ((!dr.active && !dr.drop_deadline) || (Window)e->data.l[0] != dr.target) return;
	dr.waiting = 0;
	dr.accepted = e->data.l[1] & 1;
	dr.answer = dr.ver >= 2 ? (Atom)e->data.l[4] : a_xdndcopy;
	drag_icon_set(!dr.accepted ? 2 : dr.answer == a_xdndmove ? 0 : 1);
	if (dr.drop_deadline) drag_finish();
	else if (dr.moved) drag_send_position();
}

/* 拖進來 */

static void dp_clear(void)
{
	if (dp.paths) free_paths(dp.paths, dp.n);
	int had = dp.hl_item >= 0 || dp.hl_place >= 0 || dp.hl_here;
	memset(&dp, 0, sizeof dp);
	dp.hl_item = dp.hl_place = -1;
	if (had) draw();
}

static int same_dev(const char *a, const char *b)
{
	struct stat sa, sb;
	return !lstat(a, &sa) && !stat(b, &sb) && sa.st_dev == sb.st_dev;
}

/* 游標在哪裡：放進哪個資料夾、能不能放、移動還是拷貝；反白跟著換 */
static void dp_update(void)
{
	int wx = -1, wy = -1, item = -1, place = -1, here = 0;
	Window ch;
	if (wl) { wx = dp.x; wy = dp.y; }   /* 原生：已經是視窗座標 */
	else XTranslateCoordinates(dpy, pm_root, win, dp.x, dp.y, &wx, &wy, &ch);
	char dir[PATH_MAX] = "";
	if (wx < 0 || wy < 0 || wx >= W || wy >= H) {
		;
	} else if (wx < SB_W) {
		place = place_hit(wy);
		if (place >= 0 && (places[place].header || places[place].unmounted || !places[place].path[0])) place = -1;
		if (place >= 0) snprintf(dir, sizeof dir, "%s", places[place].path);
	} else {
		int i = wy >= content_y() && wy < H - ST_H ? hit_item(wx, wy) : -1;
		if (i >= 0 && ents[vis[i]].dir) { item = i; path_of(&ents[vis[i]], dir, sizeof dir); }
		else { here = 1; snprintf(dir, sizeof dir, "%s", cwd); }
	}
	int trash = dir[0] && !strcmp(dir, trash_files);
	int ok = dp.ok_type && dir[0] && (trash || !access(dir, W_OK));
	int move = trash || (wl ? 1 : dp.suggested != a_xdndcopy), inside = 0;
	if (dp.fetched && dir[0]) {
		if (!dp.n) ok = 0;
		int all_here = 1;
		size_t dl = strlen(dir);
		for (int k = 0; k < dp.n; k++) {
			const char *p = dp.paths[k], *sl = strrchr(p, '/');
			size_t pl = strlen(p), parent = sl == p ? 1 : (size_t)(sl - p);
			if (!strcmp(p, dir) || (!strncmp(dir, p, pl) && dir[pl] == '/')) inside = 1;   /* 資料夾放進自己裡面 */
			if (parent != dl || strncmp(p, dir, dl)) all_here = 0;
		}
		if (inside) ok = 0;
		if (move && !trash && dp.n && !same_dev(dp.paths[0], dir)) move = 0;   /* 不同的磁碟：拷貝 */
		if (move && all_here) ok = 0;   /* 本來就在這裡 */
	}
	/* 停在別的資料夾上一秒就打開它（就算這裡不能放，裡面的資料夾也許可以） */
	const char *spring = (item >= 0 || place >= 0) && !trash && !inside && strcmp(dir, cwd) ? dir : "";
	if (strcmp(spring, dp.spring_dir)) {
		snprintf(dp.spring_dir, sizeof dp.spring_dir, "%s", spring);
		dp.spring_at = spring[0] ? pm_now_ms() + 1000 : 0;
	}
	/* 停在清單的上下邊緣：自動捲動 */
	int edge = 0;
	if (wx >= SB_W && wx < W) {
		if (wy >= TB_H && wy < content_y() + 24 && scrolly > 0) edge = -1;
		else if (wy >= H - ST_H - 24 && wy < H && scrolly < total_h() - content_h()) edge = 1;
	}
	if (edge != dp.scroll_dir) {
		dp.scroll_dir = edge;
		dp.scroll_at = edge ? pm_now_ms() + 300 : 0;
	}
	if (!ok) item = place = -1, here = 0;
	dp.accept = ok;
	dp.move = move;
	dp.to_trash = trash;
	snprintf(dp.dir, sizeof dp.dir, "%s", dir);
	if (item != dp.hl_item || place != dp.hl_place || here != dp.hl_here) {
		dp.hl_item = item;
		dp.hl_place = place;
		dp.hl_here = here;
		draw();
	}
}

static void dp_status(void)
{
	if (!dp.src) return;
	if (wl) {   /* 原生：游標在能放的地方就接受（同一個磁碟偏好移動，不同的拷貝；實際動作由使用者按的修飾鍵決定） */
		if (dp.accept) pmwl_dnd_accept("text/uri-list", PMWL_DND_COPY | PMWL_DND_MOVE, dp.move ? PMWL_DND_MOVE : PMWL_DND_COPY);
		else pmwl_dnd_accept(NULL, 0, 0);
		return;
	}
	xdnd_send(dp.src, dp.src, a_xdndstatus, (dp.accept ? 1 : 0) | 2, 0, 0,
	          dp.accept ? (long)(dp.move ? a_xdndmove : a_xdndcopy) : (long)None);
}

static void dp_finish(int ok)
{
	if (dp.src && !wl)
		xdnd_send(dp.src, dp.src, a_xdndfinished, dp.ver >= 5 && ok ? 1 : 0,
		          dp.ver >= 5 && ok ? (long)(dp.move ? a_xdndmove : a_xdndcopy) : (long)None, 0, 0);
	dp_clear();
}

static void dp_perform(void)
{
	dp_update();
	if (!dp.accept) { dp_finish(0); return; }
	if (dp.to_trash) trash_paths(dp.paths, dp.n);
	else paste_into(dp.dir, dp.paths, dp.n, wl ? dp.act == PMWL_DND_MOVE : dp.move);
	dp_finish(1);
}

static void dnd_selnotify(XSelectionEvent *e)
{
	if (!dp.src || !dp.fetching) return;
	dp.fetching = 0;
	dp.fetched = 1;
	dp.paths = malloc(MAX_DROP * sizeof *dp.paths);
	dp.n = 0;
	if (e->property != None) {
		Atom type; int fmt; unsigned long n, after;
		unsigned char *data = NULL;
		if (XGetWindowProperty(dpy, win, a_xdnddata, 0, 1 << 22, True, AnyPropertyType, &type, &fmt, &n, &after, &data) == Success && data) {
			if (fmt == 8) dp.n = pm_parse_uris((char *)data, dp.paths, MAX_DROP);
			XFree(data);
		}
	}
	if (dp.dropped) dp_perform();
	else { dp_update(); dp_status(); }   /* 拿到檔案清單以後重新判斷一次 */
}

static int on_xdnd(XClientMessageEvent *e)
{
	Atom t = e->message_type;
	Window src = (Window)e->data.l[0];
	if (t == a_xdndenter) {
		dp_clear();
		dp.src = src;
		dp.ver = (int)(((unsigned long)e->data.l[1] >> 24) & 0xff);
		Atom types[64];
		int nt = 0;
		if (e->data.l[1] & 1) nt = pm_getatoms(src, a_xdndtypes, types, LEN(types));
		else for (int k = 2; k < 5; k++) if (e->data.l[k]) types[nt++] = (Atom)e->data.l[k];
		for (int k = 0; k < nt; k++) if (types[k] == a_urilist) dp.ok_type = 1;
		return 1;
	}
	if (t == a_xdndposition) {
		if (!dp.src || src != dp.src) return 1;
		dp.x = (short)((e->data.l[2] >> 16) & 0xffff);
		dp.y = (short)(e->data.l[2] & 0xffff);
		dp.t = dp.ver >= 1 ? (Time)e->data.l[3] : CurrentTime;
		dp.suggested = dp.ver >= 2 ? (Atom)e->data.l[4] : a_xdndcopy;
		if (dp.ok_type && !dp.fetched && !dp.fetching) {
			XConvertSelection(dpy, a_xdndsel, a_urilist, a_xdnddata, win, dp.t);
			dp.fetching = 1;
		}
		dp_update();
		dp_status();
		return 1;
	}
	if (t == a_xdndleave) {
		if (src == dp.src) dp_clear();
		return 1;
	}
	if (t == a_xdnddrop) {
		if (!dp.src || src != dp.src) return 1;
		if (dp.ver >= 1) dp.t = (Time)e->data.l[2];
		if (dp.fetched) dp_perform();
		else if (!dp.ok_type) dp_finish(0);
		else {
			dp.dropped = 1;
			dp.drop_deadline = pm_now_ms() + 5000;
			if (!dp.fetching) {
				XConvertSelection(dpy, a_xdndsel, a_urilist, a_xdnddata, win, dp.t);
				dp.fetching = 1;
			}
		}
		return 1;
	}
	if (t == a_xdndstatus) { drag_status(e); return 1; }
	if (t == a_xdndfinished) return 1;
	return 0;
}

/* 拖放用到的計時：放開後等回覆、停在資料夾上打開、邊緣自動捲動。回傳下一次要多久（毫秒），沒有回傳 -1 */
static long dnd_timers(long now)
{
	if (dr.drop_deadline && now >= dr.drop_deadline) { dr.accepted = 0; drag_finish(); }
	if (dp.drop_deadline && now >= dp.drop_deadline) dp_finish(0);
	if (dp.src && dp.spring_at && now >= dp.spring_at) {
		dp.spring_at = 0;
		char d[PATH_MAX];
		snprintf(d, sizeof d, "%s", dp.spring_dir);
		load_dir(d, 1);
		dp_update();
		dp_status();
		draw();
	}
	if (dp.src && dp.scroll_at && now >= dp.scroll_at) {
		scrolly += dp.scroll_dir * 24;
		clamp_scroll();
		dp.scroll_at = now + 50;
		dp_update();
		draw();
	}
	long next = -1;
	long ts[] = { dr.drop_deadline, dp.drop_deadline, dp.src ? dp.spring_at : 0, dp.src ? dp.scroll_at : 0 };
	for (unsigned k = 0; k < LEN(ts); k++)
		if (ts[k]) next = next < 0 ? MAX(0, ts[k] - now) : MIN(next, MAX(0, ts[k] - now));
	return next;
}

/* 原生拖放：目標事件（拖進來）。ENTER 就把檔案清單讀進來（拖的當中就知道能不能放），放下時照協商好的動作做 */
static void native_dnd_event(const PmwEvent *e)
{
	switch (e->type) {
	case PMW_DND_ENTER:
	case PMW_DND_MOTION:
		if (e->type == PMW_DND_ENTER || !dp.src) {
			dp_clear();
			dp.src = 1;
			dp.ok_type = pmwl_dnd_has("text/uri-list");
			if (dp.ok_type) {
				char **v = NULL;
				int n = pmwl_dnd_files(&v);
				if (n > 0) { dp.paths = v; dp.n = n; dp.fetched = 1; }
			}
		}
		dp.x = e->x;
		dp.y = e->y;
		dp_update();
		dp_status();
		break;
	case PMW_DND_LEAVE:
		dp_clear();
		break;
	case PMW_DND_DROP:
		if (!dp.src) { dp.src = 1; dp.ok_type = pmwl_dnd_has("text/uri-list"); }
		if (!dp.fetched && dp.ok_type) {
			char **v = NULL;
			int n = pmwl_dnd_files(&v);
			if (dp.paths) free_paths(dp.paths, dp.n);
			dp.paths = v; dp.n = n; dp.fetched = 1;
		}
		dp.x = e->x;
		dp.y = e->y;
		dp.act = (unsigned)e->button;
		if (!dp.act) { dp_finish(0); break; }
		dp_perform();
		break;
	}
}

static void bmotion(XMotionEvent *m)
{
	XMotionEvent last = *m;
	XEvent ev;
	if (!wl) while (XCheckTypedWindowEvent(dpy, win, MotionNotify, &ev)) last = ev.xmotion;   /* 只處理最新的位置 */
	if (wl) {
		if (!dr.active && dr.pending && (last.state & Button1Mask) && (abs(last.x - dr.px) > 5 || abs(last.y - dr.py) > 5))
			native_drag_begin();
		return;
	}
	if (dr.active) { drag_move(last.x_root, last.y_root, last.state, last.time); return; }
	if (dr.pending && (last.state & Button1Mask) && (abs(last.x - dr.px) > 5 || abs(last.y - dr.py) > 5)) {
		drag_start(last.x_root, last.y_root, last.time);
		if (dr.active) drag_move(last.x_root, last.y_root, last.state, last.time);
	}
}

static void brelease(XButtonEvent *e)
{
	if (e->button != Button1) return;
	if (dr.active) { drag_release(e->time); return; }
	if (dr.pending && dr.click_i >= 0 && dr.click_i < nvis) {
		if (dr.defer == 2) { ents[vis[dr.click_i]].sel = 0; cursor_i = anchor = dr.click_i; }
		else if (dr.defer == 1) { if (dr.rename) start_rename(); else select_only(dr.click_i); }
	}
	dr.pending = dr.defer = 0;
}

static void bpress(XButtonEvent *e)
{
	if (dr.active) {   /* 拖曳中：只能用滾輪捲動 */
		if ((e->button == 4 || e->button == 5) && e->x >= SB_W && e->x < W && e->y >= 0 && e->y < H) {
			scrolly += e->button == 4 ? -60 : 60;
			clamp_scroll();
			draw();
			drag_move(dr.rx, dr.ry, e->state, e->time);
		}
		return;
	}
	if (e->button == 4 || e->button == 5) {
		if (e->x >= SB_W) { scrolly += e->button == 4 ? -60 : 60; clamp_scroll(); }
		return;
	}
	if (in.kind && in.kind != IN_SEARCH) input_end(1);
	if (e->x < SB_W) {
		int i = place_hit(e->y);
		if (i < 0) return;
		Place *p = &places[i];
		if (p->eject && e->x >= SB_W - 40 && e->button == Button1) {
			/* 退出：交給 pomelo-automount（成功或失敗都會跳通知） */
			if (!strncmp(cwd, p->path, strlen(p->path))) load_dir(getenv("HOME") ? getenv("HOME") : "/", 1);
			char *argv[] = { "pomelo-automount", "--eject", p->dev, NULL };
			if (fork() == 0) {
				CLOSE_X();
				setsid();
				execvp(argv[0], argv);
				_exit(127);
			}
			set_status(_("正在退出…"));
			return;
		}
		if (p->unmounted) {
			char mp[512] = "", err[512] = "";
			set_status(_("正在掛載…"));
			draw();
			if (dpy) XFlush(dpy);
			if (pm_volume_mount(p->dev, mp, sizeof mp, err, sizeof err)) {
				char t[640];
				snprintf(t, sizeof t, _("無法掛載：%s"), err);
				set_status(t);
				return;
			}
			build_places();
			if (mp[0]) load_dir(mp, 1);
			return;
		}
		if (e->button == Button2) new_window(p->path);
		else load_dir(p->path, 1);
		return;
	}
	if (e->y < TB_H) {
		int k = tb_hit(e->x, e->y);
		if (in.kind == IN_SEARCH && k != TB_SEARCH) input_end(1);
		switch (k) {
		case TB_BACK: go_back(); break;
		case TB_FWD: go_forward(); break;
		case TB_ICONS: set_view(0); break;
		case TB_LIST: set_view(1); break;
		case TB_SEARCH: if (in.kind != IN_SEARCH) input_start(IN_SEARCH, 0, filter); break;
		default:
			/* 雙擊標題可輸入路徑 */
			if (e->x > SB_W + 80 && e->time - last_click < 400) input_start(IN_PATH, 0, cwd);
			last_click = e->time;
		}
		return;
	}
	if (in.kind == IN_SEARCH) input_end(1);
	if (view && e->y < TB_H + HEAD_H) {
		int k = header_hit(e->x);
		if (k == sort_key) sort_desc = !sort_desc;
		else { sort_key = k; sort_desc = 0; }
		read_dir(1);
		return;
	}
	if (e->y >= H - ST_H) return;
	int i = hit_item(e->x, e->y);
	if (e->button == Button3) {
		if (i >= 0 && !ents[vis[i]].sel) select_only(i);
		if (i < 0) select_only(-1);
		draw();
		context_menu(e->x_root, e->y_root, i >= 0);
		return;
	}
	if (e->button == Button2 && i >= 0 && ents[vis[i]].dir) {
		char p[PATH_MAX];
		path_of(&ents[vis[i]], p, sizeof p);
		new_window(p);
		return;
	}
	if (e->button != Button1) return;
	dr.pending = dr.defer = 0;
	if (i < 0) { select_only(-1); return; }
	if (e->state & (ControlMask | Mod4Mask)) {
		/* 已選取的先不取消：可能是要按著 Ctrl 拖曳拷貝，放開時才取消 */
		if (ents[vis[i]].sel) dr.defer = 2;
		else ents[vis[i]].sel = 1;
		cursor_i = anchor = i;
	} else if ((e->state & ShiftMask) && anchor >= 0) {
		select_range(anchor, i);
		cursor_i = i;
	} else {
		if (i == last_click_i && e->time - last_click < 400 && ents[vis[i]].sel) {
			last_click_i = -2;
			select_only(i);
			open_selected();
			return;
		}
		if (ents[vis[i]].sel) {
			/* 已選取的：可能是要拖曳，放開時才決定。點已選取的單一項目，停一下再點 → 重新命名；
			 * 選了好幾個時點其中一個 → 只選它 */
			dr.defer = 1;
			dr.rename = nsel() == 1 && i == last_click_i && e->time - last_click > 700 && e->time - last_click < 3000;
		} else {
			select_only(i);
		}
	}
	if (ents[vis[i]].sel) {   /* 按著移動就開始拖曳 */
		dr.pending = 1;
		dr.px = e->x;
		dr.py = e->y;
		dr.click_i = i;
	}
	last_click = e->time;
	last_click_i = i;
}

static void handle(XEvent *ev)
{
	switch (ev->type) {
	case Expose: if (!ev->xexpose.count) draw(); break;
	case ConfigureNotify:
		if (ev->xconfigure.width != W || ev->xconfigure.height != H) {
			W = ev->xconfigure.width;
			H = ev->xconfigure.height;
			clamp_scroll();
			draw();
		}
		break;
	case FocusIn: case FocusOut:
		focused = ev->type == FocusIn;
		if (xic) { if (focused) XSetICFocus(xic); else XUnsetICFocus(xic); }
		draw();
		break;
	case SelectionRequest: on_selrequest(&ev->xselectionrequest); break;
	case SelectionNotify: on_selnotify(&ev->xselection); break;
	case ClientMessage:
		if (!wl && on_xdnd(&ev->xclient)) break;
		if ((Atom)ev->xclient.data.l[0] == a_delete) exit(0);
		break;
	case PropertyNotify:
		if (pm_conf_changed(dpy, ev)) { apply_settings(); draw(); }
		break;
	}
}

/* 視窗圖示：藍色資料夾上一片柚子 */
static cairo_surface_t *make_icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 20, 0, 112);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.36, 0.67, 0.98);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.16, 0.48, 0.90);
	pm_rrect(cr, 12, 22, 46, 20, 6, 0);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	pm_rrect(cr, 12, 32, 104, 78, 10, 10);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	pm_rrect(cr, 12, 44, 104, 66, 10, 10);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
	cairo_fill(cr);
	/* 柚子切片 */
	double cx = 64, cy = 78, r = 22;
	cairo_set_source_rgb(cr, 1.0, 0.78, 0.30);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1.0, 0.95, 0.80);
	cairo_arc(cr, cx, cy, r - 4, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1.0, 0.62, 0.45);
	for (int i = 0; i < 8; i++) {
		double a0 = i * M_PI / 4 + 0.12, a1 = (i + 1) * M_PI / 4 - 0.12;
		cairo_move_to(cr, cx, cy);
		cairo_arc(cr, cx, cy, r - 6, a0, a1);
		cairo_close_path(cr);
	}
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

static void set_wm_icon(void)
{
	int sizes[] = { 32, 64 };
	size_t total = 0;
	for (unsigned i = 0; i < LEN(sizes); i++) total += 2 + (size_t)sizes[i] * sizes[i];
	unsigned long *d = malloc(total * sizeof *d), *p = d;
	for (unsigned i = 0; i < LEN(sizes); i++) {
		int n = sizes[i];
		cairo_surface_t *s = make_icon(n);
		cairo_surface_flush(s);
		unsigned char *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		*p++ = n; *p++ = n;
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				uint32_t v = ((uint32_t *)(px + y * stride))[x];
				unsigned a = v >> 24, r = (v >> 16) & 255, gg = (v >> 8) & 255, b = v & 255;
				if (a && a < 255) { r = r * 255 / a; gg = gg * 255 / a; b = b * 255 / a; }
				*p++ = ((unsigned long)a << 24) | (r << 16) | (gg << 8) | b;
			}
		cairo_surface_destroy(s);
	}
	XChangeProperty(dpy, win, a_neticon, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)d, (int)total);
	free(d);
}

/* 事件迴圈共用：下一次最多睡多久（毫秒，-1＝沒有計時的事） */
static long next_wait(void)
{
	long wait = -1, now = pm_now_ms();
	if (reload_at) wait = MAX(0, reload_at - now);
	if (status_until > now) wait = wait < 0 ? status_until - now : MIN(wait, status_until - now);
	long dt = dnd_timers(now);
	if (dt >= 0) wait = wait < 0 ? dt : MIN(wait, dt);
	return wait;
}

/* 插入或退出外接裝置：重建側邊欄；目前的資料夾不見了就回家目錄 */
static void mounts_changed(int mfd)
{
	char tmp[4096];
	lseek(mfd, 0, SEEK_SET);
	while (read(mfd, tmp, sizeof tmp) > 0) ;
	build_places();
	struct stat cst;
	if (stat(cwd, &cst)) load_dir(getenv("HOME") ? getenv("HOME") : "/", 1);
	draw();
}

static void inotify_ready(void)
{
	char ib[4096];
	int gone = 0;
	ssize_t n;
	while ((n = read(ino_fd, ib, sizeof ib)) > 0)
		for (char *p = ib; p < ib + n;) {
			struct inotify_event *ie = (struct inotify_event *)p;
			if (ie->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) gone = 1;
			p += sizeof *ie + ie->len;
		}
	if (gone) { go_parent(); draw(); }
	else if (!reload_at) reload_at = pm_now_ms() + 150;   /* 合併短時間內的多次變動 */
}

static void timers_tick(void)
{
	long now = pm_now_ms();
	if (reload_at && now >= reload_at) {
		reload_at = 0;
		if (!in.kind) read_dir(1);
		draw();
	}
	if (status_msg[0] && now >= status_until) { status_msg[0] = 0; draw(); }
}

/* 原生 Wayland：視窗與事件迴圈（事件轉成 XEvent 交給跟 X 共用的 keypress／bpress／brelease／bmotion） */
static void native_main(const char *start_dir)
{
	Rect m = { 0, 0, 1280, 720 };
	{ Rect o[8]; if (pmwl_outputs(o, 8) > 0) m = o[0]; }
	W = MIN(960, (int)(m.w * 0.8));
	H = MIN(600, (int)(m.h * 0.85));
	wl = pmwl_window(_("檔案"), "pomelo-files", W, H, 560, 320, NULL);
	if (!wl) { fprintf(stderr, "pomelo-files: %s\n", _("無法開啟 Wayland 視窗")); exit(1); }
	win = PMWL_XID(wl);
	pmwl_want(wl, PMWL_IN_PREEDIT);
	static const char *const uri[] = { "text/uri-list" };
	pmwl_dnd_target(wl, uri, 1, PMWL_DND_COPY | PMWL_DND_MOVE);   /* 收拖進來的檔案 */
	ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	load_dir(start_dir, 1);
	int mfd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
	draw();
	for (;;) {
		struct pollfd pf[2] = { { ino_fd, POLLIN, 0 }, { mfd, POLLPRI, 0 } };
		PmwEvent pe;
		long wait = next_wait();
		int got = pmwl_next_fds(&pe, wait, pf, 2);
		int dirty = 0;
		if (!got) {
			if (pf[1].revents & (POLLPRI | POLLERR)) mounts_changed(mfd);
			if (pf[0].revents & POLLIN) inotify_ready();
		}
		while (got) {
			XEvent ev;
			switch (pe.type) {
			case PMW_CONF: apply_settings(); dirty = 1; break;
			case PMW_PREEDIT: snprintf(preedit, sizeof preedit, "%s", pe.text); dirty = 1; break;
			case PMW_DND_ENTER: case PMW_DND_MOTION: case PMW_DND_LEAVE: case PMW_DND_DROP:
				native_dnd_event(&pe);
				dirty = 1;
				break;
			case PMW_DND_END:   /* 我們拖出去的結束了（成功、取消都是）：游標回來才收得到滑鼠事件 */
				dr.active = dr.pending = dr.defer = 0;
				if (dr.paths) { free_paths(dr.paths, dr.n); dr.paths = NULL; dr.n = 0; }
				if (pe.button) read_dir(1);
				dirty = 1;
				break;
			case PMW_LEAVE: if (dr.active) break; /* fallthrough */
			default:
				if (!pmwl_to_xevent(&pe, &ev)) break;
				if (ev.type == MotionNotify) { mouse_x = ev.xmotion.x; mouse_y = ev.xmotion.y; }
				else if (ev.type == ButtonPress || ev.type == ButtonRelease) { mouse_x = ev.xbutton.x; mouse_y = ev.xbutton.y; }
				if (ev.type == KeyPress) {
					keypress(&ev.xkey);
					dirty = 1;
				} else if (ev.type == ButtonPress) {
					if (pe.type == PMW_SCROLL) { bpress(&ev.xbutton); dirty = 1; }
					else { bpress(&ev.xbutton); dirty = 1; }
				} else if (ev.type == ButtonRelease) { brelease(&ev.xbutton); dirty = 1; }
				else if (ev.type == MotionNotify) bmotion(&ev.xmotion);
				else if (ev.type == KeyRelease) ;
				else handle(&ev);
			}
			got = pmwl_next(&pe, 0);
		}
		if (dirty) draw();
		timers_tick();
	}
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) {
		cairo_surface_t *s = make_icon(256);
		int ok = cairo_surface_write_to_png(s, argv[2]) == CAIRO_STATUS_SUCCESS;
		cairo_surface_destroy(s);
		return !ok;
	}
	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		puts(_("用法：pomelo-files [資料夾]"));
		return 0;
	}
	setlocale(LC_ALL, "");
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	int native = pmwl_connect();
	if (native) { pm_ui_native = 1; a_delete = PMWL_XCLOSE; }
	else if (!(dpy = XOpenDisplay(NULL))) {
		fputs(_("pomelo-files: 無法開啟 X display\n"), stderr);
		return 1;
	}
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);
	if (!native) XSetErrorHandler(xerror);
	pm_conf_load();
	pm_init(def_font_family, def_font_size, def_dark_mode, def_icon_theme);
	if (!native) pm_dispatch = handle;
	view = pm_conf_int("files.view", def_start_view);
	hidden = pm_conf_int("files.show_hidden", def_show_hidden);
	apply_settings();

	const char *home = getenv("HOME") ? getenv("HOME") : "/";
	snprintf(trash_files, sizeof trash_files, "%s/.local/share/Trash/files", home);
	snprintf(trash_info, sizeof trash_info, "%s/.local/share/Trash/info", home);
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s/.local/share/Trash", home);
	mkdir(tmp, 0700);
	mkdir(trash_files, 0700);
	mkdir(trash_info, 0700);
	load_mime_tables();
	build_places();

	if (native) { native_main(argc > 1 ? argv[1] : home); return 0; }
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_gnome = XInternAtom(dpy, "x-special/gnome-copied-files", False);
	a_urilist = XInternAtom(dpy, "text/uri-list", False);
	a_paste = XInternAtom(dpy, "POMELO_FILES_PASTE", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_neticon = XInternAtom(dpy, "_NET_WM_ICON", False);
	a_netpid = XInternAtom(dpy, "_NET_WM_PID", False);
	a_xdndaware = XInternAtom(dpy, "XdndAware", False);
	a_xdndproxy = XInternAtom(dpy, "XdndProxy", False);
	a_xdndenter = XInternAtom(dpy, "XdndEnter", False);
	a_xdndposition = XInternAtom(dpy, "XdndPosition", False);
	a_xdndstatus = XInternAtom(dpy, "XdndStatus", False);
	a_xdndleave = XInternAtom(dpy, "XdndLeave", False);
	a_xdnddrop = XInternAtom(dpy, "XdndDrop", False);
	a_xdndfinished = XInternAtom(dpy, "XdndFinished", False);
	a_xdndsel = XInternAtom(dpy, "XdndSelection", False);
	a_xdndtypes = XInternAtom(dpy, "XdndTypeList", False);
	a_xdndactions = XInternAtom(dpy, "XdndActionList", False);
	a_xdndcopy = XInternAtom(dpy, "XdndActionCopy", False);
	a_xdndmove = XInternAtom(dpy, "XdndActionMove", False);
	a_xdndask = XInternAtom(dpy, "XdndActionAsk", False);
	a_xdnddata = XInternAtom(dpy, "POMELO_FILES_DROP", False);
	a_textplain = XInternAtom(dpy, "text/plain;charset=utf-8", False);

	XSetWindowAttributes wa = {
		.background_pixel = c_bg, .bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
		.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask |
		              StructureNotifyMask | FocusChangeMask,
	};
	win = XCreateWindow(dpy, pm_root, 0, 0, W, H, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	XClassHint ch = { "pomelo-files", "Pomelo-files" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 560; sz->min_height = 320;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	long pid = getpid();
	XChangeProperty(dpy, win, a_netpid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	long xdnd_ver = 5;   /* 收拖放 */
	XChangeProperty(dpy, win, a_xdndaware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&xdnd_ver, 1);
	set_wm_icon();

	/* 輸入法：重新命名時可以打中文 */
	pm_xim_attach(dpy, &xim, &xic, win);   /* 輸入法重新啟動也不會當掉 */

	pm_conf_watch(dpy);
	ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	load_dir(argc > 1 ? argv[1] : home, 1);
	XMapWindow(dpy, win);

	int xfd = ConnectionNumber(dpy);
	int mfd = open("/proc/self/mounts", O_RDONLY | O_CLOEXEC);
	for (;;) {
		int dirty = 0;
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (XFilterEvent(&ev, None)) continue;
			if (ev.type == KeyPress) { keypress(&ev.xkey); dirty = 1; }
			else if (ev.type == ButtonPress) { bpress(&ev.xbutton); dirty = 1; }
			else if (ev.type == ButtonRelease) { brelease(&ev.xbutton); dirty = 1; }
			else if (ev.type == MotionNotify) bmotion(&ev.xmotion);
			else handle(&ev);
		}
		if (dirty) draw();
		XFlush(dpy);   /* 畫好的東西要先送出去再睡，不然要等到下一個事件才看得到 */
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		if (ino_fd >= 0) FD_SET(ino_fd, &fds);
		long wait = next_wait();
		struct timeval tv = { wait / 1000, (wait % 1000) * 1000 };
		fd_set efds;
		FD_ZERO(&efds);
		if (mfd >= 0) FD_SET(mfd, &efds);
		int r = select(MAX(MAX(xfd, ino_fd), mfd) + 1, &fds, NULL, &efds, wait >= 0 ? &tv : NULL);
		if (r > 0 && mfd >= 0 && FD_ISSET(mfd, &efds)) mounts_changed(mfd);
		if (r > 0 && ino_fd >= 0 && FD_ISSET(ino_fd, &fds)) inotify_ready();
		timers_tick();
	}
}
