/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-viewer — Pomelo 看圖
 *
 * 打開一張圖，就能用 ←／→ 看同一個資料夾的其他圖片。支援 PNG、JPEG（手機照片自動轉正）、
 * GIF 與 WebP（含動畫）、BMP、PNM。可以縮放、拖曳、旋轉、全螢幕、幻燈片、丟到垃圾桶（可復原）、
 * 拷貝到剪貼簿、設為桌布、看照片資訊（拍攝時間、相機）。
 *
 * 用法：pomelo-viewer [圖片或資料夾…]
 * 快捷鍵：←／→ 上一張／下一張、Home／End 第一張／最後一張、+／− 縮放、0 符合視窗、1 實際大小、
 *   R／Shift+R 旋轉、F／F11 全螢幕、S 幻燈片、I 資訊、Delete 丟到垃圾桶、Ctrl+Z 復原、
 *   Ctrl+C 拷貝圖片、F5／Ctrl+R 重新載入、Ctrl+W／Q 關閉
 * 滑鼠：滾輪縮放、拖曳移動、按兩下切換「符合視窗／實際大小」、右鍵選單
 *
 * 解碼在背景執行緒做（看完這張時下一張通常已經準備好了），畫面不會卡住。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-wl.h>
#include <pomelo-dnd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/wait.h>

#include "formats.c"

#define TB_H 46
#define NCACHE 8

/* ------------------------------------------------------------------ */
/* 檔案清單                                                            */
/* ------------------------------------------------------------------ */

static char **files;
static int nfiles, cur = -1;

static const char *base_of(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

/* 自然排序：IMG_2 排在 IMG_10 前面，不分大小寫 */
static int cmp_files(const void *a, const void *b)
{
	char x[NAME_MAX + 1], y[NAME_MAX + 1];
	snprintf(x, sizeof x, "%s", base_of(*(char *const *)a));
	snprintf(y, sizeof y, "%s", base_of(*(char *const *)b));
	for (char *p = x; *p; p++) *p = (char)tolower((unsigned char)*p);
	for (char *p = y; *p; p++) *p = (char)tolower((unsigned char)*p);
	int r = strverscmp(x, y);
	return r ? r : strcmp(*(char *const *)a, *(char *const *)b);
}

static void add_file(const char *p)
{
	files = realloc(files, (size_t)(nfiles + 1) * sizeof *files);
	files[nfiles++] = strdup(p);
}

static void scan_dir(const char *dir, const char *keep)
{
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	int start = nfiles;
	while ((e = readdir(d)) && nfiles - start < 50000) {
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", e->d_name);
		if (keep && !strcmp(p, keep)) continue;
		if (!is_image_name(e->d_name)) continue;
		struct stat st;
		if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
		add_file(p);
	}
	closedir(d);
	if (keep) add_file(keep);   /* 副檔名不認得也照樣列進來（例如沒有副檔名的圖） */
	qsort(files + start, (size_t)(nfiles - start), sizeof *files, cmp_files);
}

static void abs_path(const char *in, char *out, size_t n)
{
	char *r = realpath(in, NULL);
	if (r) { snprintf(out, n, "%s", r); free(r); return; }
	if (in[0] == '/') { snprintf(out, n, "%s", in); return; }
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
	snprintf(out, n, "%s/%s", cwd, in);
}

static void build_list(int argc, char **argv)
{
	int nargs = 0;
	for (int i = 1; i < argc; i++) if (argv[i][0] != '-' || !strcmp(argv[i], "-")) nargs++;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') continue;
		char p[PATH_MAX];
		abs_path(argv[i], p, sizeof p);
		struct stat st;
		if (stat(p, &st)) continue;
		if (S_ISDIR(st.st_mode)) { scan_dir(p, NULL); if (cur < 0 && nfiles) cur = 0; continue; }
		if (nargs == 1) {
			/* 只開一張：把同一個資料夾的圖都列進來，從這張開始 */
			char dir[PATH_MAX];
			snprintf(dir, sizeof dir, "%s", p);
			char *sl = strrchr(dir, '/');
			if (sl == dir) sl[1] = 0; else if (sl) *sl = 0;
			scan_dir(dir, p);
			for (int k = 0; k < nfiles; k++) if (!strcmp(files[k], p)) cur = k;
		} else {
			if (cur < 0) cur = nfiles;
			add_file(p);
		}
	}
	if (cur < 0 && nfiles) cur = 0;
}

static void remove_file(int i)
{
	free(files[i]);
	memmove(files + i, files + i + 1, (size_t)(nfiles - i - 1) * sizeof *files);
	nfiles--;
}

/* ------------------------------------------------------------------ */
/* 背景解碼與快取                                                      */
/* ------------------------------------------------------------------ */

typedef struct { char path[PATH_MAX]; Image *img; } Cached;
static Cached cache[NCACHE];
static int ncache;
static char want[3][PATH_MAX];     /* 要解的：目前這張、下一張、上一張（依順序） */
static char loading[PATH_MAX];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int wake_fd[2];

static Image *cache_find(const char *p)   /* 要先拿 mu */
{
	for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].path, p)) return cache[i].img;
	return NULL;
}

static Image *shown;           /* 畫面上的圖（換下一張時，新的還沒好之前先留著舊的） */
static int shown_idx;
static char shown_path[PATH_MAX];

static void *worker(void *arg)
{
	(void)arg;
	for (;;) {
		char p[PATH_MAX] = "";
		pthread_mutex_lock(&mu);
		for (;;) {
			for (int i = 0; i < 3 && !p[0]; i++)
				if (want[i][0] && !cache_find(want[i])) snprintf(p, sizeof p, "%s", want[i]);
			if (p[0] && ncache < NCACHE) break;
			p[0] = 0;
			pthread_cond_wait(&cv, &mu);
		}
		snprintf(loading, sizeof loading, "%s", p);
		pthread_mutex_unlock(&mu);
		Image *im = image_load(p);
		pthread_mutex_lock(&mu);
		loading[0] = 0;
		if (im && ncache < NCACHE) { snprintf(cache[ncache].path, sizeof cache[0].path, "%s", p); cache[ncache++].img = im; }
		else image_free(im);
		pthread_mutex_unlock(&mu);
		if (write(wake_fd[1], "x", 1) < 0) { /* 管線滿了也沒關係 */ }
	}
	return NULL;
}



/* 設定要解的圖，順便把用不到的快取丟掉（只有主執行緒會釋放，正在畫的圖不會被丟掉） */
static void request(int dir)
{
	pthread_mutex_lock(&mu);
	for (int i = 0; i < 3; i++) want[i][0] = 0;
	if (cur >= 0 && cur < nfiles) {
		snprintf(want[0], PATH_MAX, "%s", files[cur]);
		if (nfiles > 1) {
			int nx = (cur + (dir < 0 ? -1 : 1) + nfiles) % nfiles, pv = (cur + (dir < 0 ? 1 : -1) + nfiles) % nfiles;
			snprintf(want[1], PATH_MAX, "%s", files[nx]);
			if (pv != nx) snprintf(want[2], PATH_MAX, "%s", files[pv]);
		}
	}
	for (int i = 0; i < ncache; ) {
		int keep = cache[i].img == shown;
		for (int k = 0; k < 3; k++) if (!strcmp(cache[i].path, want[k])) keep = 1;
		if (keep) { i++; continue; }
		image_free(cache[i].img);
		cache[i] = cache[--ncache];
	}
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
}

/* 重新讀目前這張（檔案改過、或剛裝好 libheif）：連畫面上的也丟掉 */
static void reload_current(void)
{
	if (cur < 0 || cur >= nfiles) return;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ncache; i++)
		if (!strcmp(cache[i].path, files[cur])) { image_free(cache[i].img); cache[i] = cache[--ncache]; break; }
	shown = NULL;
	shown_idx = -1;
	pthread_mutex_unlock(&mu);
	request(0);
}

static void forget(const char *p)   /* 檔案被刪除或改名：從快取拿掉 */
{
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ncache; i++)
		if (!strcmp(cache[i].path, p) && cache[i].img != shown) { image_free(cache[i].img); cache[i] = cache[--ncache]; break; }
	pthread_mutex_unlock(&mu);
}

/* ------------------------------------------------------------------ */
/* 畫面狀態                                                            */
/* ------------------------------------------------------------------ */

static Window win;
static PmWl *wl;   /* 原生 Wayland 的視窗（NULL＝X11） */
static int W = 1000, H = 720;
static Atom a_delete, a_utf8, a_netname, a_state, a_fs, a_clipboard, a_targets, a_png, a_incr;
static Cursor blank_cursor;
static int fullscreen, toolbar_shown = 1, cursor_hidden;
static long toolbar_hide_at;

static int frame;
static long next_frame_at;
static int fit = 1, rot;           /* rot：0～3，每次 90° 順時針 */
static double zoom = 1, panx, pany;
static cairo_surface_t *hq;        /* 縮小用的高畫質版本（每次縮放後重算） */
static Image *hq_img;
static int hq_rot, hq_frame;
static double hq_scale;
static long hq_due;
static long load_started;

static int slideshow, slide_was_fs;
static long next_slide_at;
static int show_info;
static Exif exif;
static char exif_path[PATH_MAX];

static char toast[256];
static long toast_until;
static int dragging, drag_moved, drag_x, drag_y;
static double drag_px, drag_py;
static Time last_click;
static int hover_btn = -1, hover_edge;   /* hover_edge：-1 左、1 右 */
static Rect install_btn;                 /* HEIC 的「安裝」按鈕（沒顯示時寬度 0） */
static int waiting_install;              /* 按過「安裝」：回到視窗時重新試 */
static int mouse_x, mouse_y;

/* 上一次丟到垃圾桶的檔案（Ctrl+Z 復原） */
static char undo_from[PATH_MAX], undo_to[PATH_MAX], undo_info[PATH_MAX];
static int undo_index = -1;

static unsigned col_bg, col_bar, col_text, col_dim, col_line;
static int dirty;
static void draw(void);

static void colors(void)
{
	col_bg = pm_dark ? 0x1b1b1d : 0xebebed;
	col_bar = pm_dark ? 0x2c2c2e : 0xf6f6f8;
	col_text = pm_dark ? 0xf2f2f2 : 0x1d1d1f;
	col_dim = pm_dark ? 0x98989d : 0x86868b;
	col_line = pm_dark ? 0x3a3a3c : 0xd1d1d6;
}

static int top_h(void) { return fullscreen ? 0 : TB_H; }

static void say(const char *m) { snprintf(toast, sizeof toast, "%s", m); toast_until = pm_now_ms() + 2600; }

static int cur_ready(void) { return shown && shown_idx == cur && cur >= 0 && cur < nfiles && !strcmp(shown_path, files[cur]); }

static void img_dims(double *rw, double *rh)
{
	double w = shown && !shown->err ? shown->w : 1, h = shown && !shown->err ? shown->h : 1;
	*rw = rot & 1 ? h : w;
	*rh = rot & 1 ? w : h;
}

static double fit_scale(void)
{
	double rw, rh;
	img_dims(&rw, &rh);
	double vw = W, vh = H - top_h();
	double s = MIN(vw / rw, vh / rh);
	return MIN(s, 1.0);
}

static double scale_now(void) { return fit ? fit_scale() : zoom; }

/* 解碼後的一個像素等於原圖幾個像素（大照片解碼時縮小過） */
static double pix(void) { return shown && !shown->err && shown->ow > 0 ? (double)shown->w / shown->ow : 1; }

static void clamp_pan(void)
{
	double rw, rh, s = scale_now();
	img_dims(&rw, &rh);
	double vw = W, vh = H - top_h();
	double mx = MAX(0, (rw * s - vw) / 2), my = MAX(0, (rh * s - vh) / 2);
	panx = CLAMP(panx, -mx, mx);
	pany = CLAMP(pany, -my, my);
}

/* 以畫面上的 (px, py) 為中心縮放 */
static void zoom_to(double s, double px, double py)
{
	double old = scale_now();
	s = CLAMP(s, 0.02, 32.0);
	double fs = fit_scale();
	double cx = W / 2.0 + panx, cy = top_h() + (H - top_h()) / 2.0 + pany;
	double ncx = px - (px - cx) * s / old, ncy = py - (py - cy) * s / old;
	panx = ncx - W / 2.0;
	pany = ncy - (top_h() + (H - top_h()) / 2.0);
	fit = fabs(s - fs) < 1e-9;
	zoom = s;
	clamp_pan();
	hq_due = pm_now_ms() + 150;
}

static void zoom_step(int dir, double px, double py)
{
	static const double steps[] = { 0.05, 0.1, 0.15, 0.2, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.25, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
	double k = pix(), s = scale_now() * k, ns = s;   /* 照原圖的比例一格一格跳 */
	if (dir > 0) { for (unsigned i = 0; i < LEN(steps); i++) if (steps[i] > s * 1.01) { ns = steps[i]; break; } }
	else { for (int i = (int)LEN(steps) - 1; i >= 0; i--) if (steps[i] < s * 0.99) { ns = steps[i]; break; } }
	/* 經過「符合視窗」的大小時停一下 */
	double fs = fit_scale() * k;
	if ((s < fs && ns > fs) || (s > fs && ns < fs)) ns = fs;
	zoom_to(ns / k, px, py);
}

static void reset_view(void) { fit = 1; zoom = 1; panx = pany = 0; rot = 0; frame = 0; }

/* ------------------------------------------------------------------ */
/* 換圖                                                                */
/* ------------------------------------------------------------------ */

static void set_title(void)
{
	char t[PATH_MAX + 64];
	if (cur >= 0 && cur < nfiles) snprintf(t, sizeof t, "%s", base_of(files[cur]));
	else snprintf(t, sizeof t, "%s", _("看圖"));
	if (wl) { pmwl_set_title(wl, t); return; }
	XStoreName(dpy, win, t);
	XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (const unsigned char *)t, (int)strlen(t));
}

/* 背景執行緒解好了：目前這張好了就換上 */
static void poll_ready(void)
{
	if (cur < 0 || cur >= nfiles) return;
	if (cur_ready()) return;
	pthread_mutex_lock(&mu);
	Image *im = cache_find(files[cur]);
	pthread_mutex_unlock(&mu);
	if (!im) return;
	shown = im;
	shown_idx = cur;
	snprintf(shown_path, sizeof shown_path, "%s", files[cur]);
	reset_view();
	next_frame_at = shown->n > 1 ? pm_now_ms() + shown->delays[0] : 0;
	hq_due = 1;   /* 第一次顯示就畫好畫質 */
	if (slideshow) next_slide_at = pm_now_ms() + (long)(pm_conf_num("viewer.slideshow_seconds", 4) * 1000);
	request(0);
}

static void go(int idx, int dir)
{
	if (!nfiles) return;
	cur = (idx % nfiles + nfiles) % nfiles;
	load_started = pm_now_ms();
	request(dir);
	poll_ready();
	set_title();
}

/* ------------------------------------------------------------------ */
/* 垃圾桶                                                              */
/* ------------------------------------------------------------------ */

static void mkdirs(const char *p, mode_t m)
{
	char t[PATH_MAX];
	snprintf(t, sizeof t, "%s", p);
	for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, m); *q = '/'; }
	mkdir(t, m);
}

static void url_escape(const char *s, char *out, size_t n)
{
	size_t k = 0;
	for (; *s && k + 4 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || strchr("/-_.~", c) || c >= 0x80) out[k++] = (char)c;
		else k += (size_t)snprintf(out + k, n - k, "%%%02X", c);
	}
	out[k] = 0;
}

/* 依 freedesktop 規範丟到垃圾桶。同一顆磁碟用家目錄的垃圾桶，外接硬碟用它自己的 .Trash-UID */
static int trash(const char *path, char *dest, char *info, size_t n)
{
	const char *xd = getenv("XDG_DATA_HOME");
	char home_trash[PATH_MAX], top[PATH_MAX] = "", relpath[PATH_MAX];
	if (xd && *xd) snprintf(home_trash, sizeof home_trash, "%s/Trash", xd);
	else snprintf(home_trash, sizeof home_trash, "%s/.local/share/Trash", getenv("HOME") ? getenv("HOME") : "/tmp");
	char fdir[PATH_MAX], idir[PATH_MAX];
	snprintf(fdir, sizeof fdir, "%s/files", home_trash);
	snprintf(idir, sizeof idir, "%s/info", home_trash);
	mkdirs(fdir, 0700);
	mkdirs(idir, 0700);
	struct stat a, b;
	if (stat(path, &a)) return -1;
	snprintf(relpath, sizeof relpath, "%s", path);
	if (stat(fdir, &b) || a.st_dev != b.st_dev) {
		/* 找出這顆磁碟的最上層（掛載點） */
		snprintf(top, sizeof top, "%s", path);
		for (;;) {
			char *sl = strrchr(top, '/');
			if (!sl || sl == top) { strcpy(top, "/"); break; }
			*sl = 0;
			struct stat c;
			char parent[PATH_MAX];
			snprintf(parent, sizeof parent, "%s", top);
			char *ps = strrchr(parent, '/');
			if (ps == parent) parent[1] = 0; else if (ps) *ps = 0;
			if (stat(parent, &c) || c.st_dev != a.st_dev) break;
		}
		char t[PATH_MAX];
		snprintf(t, sizeof t, "%s/.Trash-%u", strcmp(top, "/") ? top : "", (unsigned)getuid());
		snprintf(fdir, sizeof fdir, "%s/files", t);
		snprintf(idir, sizeof idir, "%s/info", t);
		mkdirs(fdir, 0700);
		mkdirs(idir, 0700);
		size_t tl = strlen(top);
		snprintf(relpath, sizeof relpath, "%s", path + (strcmp(top, "/") && !strncmp(path, top, tl) && path[tl] == '/' ? tl + 1 : 0));
	}
	/* 找一個沒用過的名稱，先建 .trashinfo（O_EXCL）佔住 */
	const char *bn = base_of(path);
	char name[NAME_MAX + 16];
	for (int i = 1; i < 10000; i++) {
		if (i == 1) snprintf(name, sizeof name, "%s", bn);
		else {
			const char *dot = strrchr(bn, '.');
			if (dot && dot != bn) snprintf(name, sizeof name, "%.*s.%d%s", (int)(dot - bn), bn, i, dot);
			else snprintf(name, sizeof name, "%s.%d", bn, i);
		}
		snprintf(info, n, "%s/%s.trashinfo", idir, name);
		snprintf(dest, n, "%s/%s", fdir, name);
		int fd = open(info, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0) { if (errno == EEXIST) continue; return -1; }
		struct stat d;
		if (!lstat(dest, &d)) { close(fd); unlink(info); continue; }
		char esc[PATH_MAX * 3], ts[32], buf[PATH_MAX * 3 + 128];
		url_escape(relpath, esc, sizeof esc);
		time_t now = time(NULL);
		strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&now));
		int len = snprintf(buf, sizeof buf, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", esc, ts);
		int ok = write(fd, buf, (size_t)len) == len;
		close(fd);
		if (!ok || rename(path, dest)) { unlink(info); return -1; }
		return 0;
	}
	return -1;
}

static void delete_current(void)
{
	if (cur < 0 || cur >= nfiles) return;
	char dest[PATH_MAX], info[PATH_MAX];
	if (trash(files[cur], dest, info, sizeof dest)) {
		char m[400];
		snprintf(m, sizeof m, _("無法丟到垃圾桶：%s"), strerror(errno));
		say(m);
		return;
	}
	snprintf(undo_from, sizeof undo_from, "%s", files[cur]);
	snprintf(undo_to, sizeof undo_to, "%s", dest);
	snprintf(undo_info, sizeof undo_info, "%s", info);
	undo_index = cur;
	char m[400];
	snprintf(m, sizeof m, _("已把「%s」丟到垃圾桶　Ctrl+Z 復原"), base_of(files[cur]));
	say(m);
	forget(files[cur]);
	remove_file(cur);
	if (!nfiles) { cur = -1; shown = NULL; shown_idx = -1; request(0); set_title(); return; }
	shown_idx = -1;   /* 畫面上的圖已經不在清單裡 */
	go(MIN(cur, nfiles - 1), 1);
}

static void undo_delete(void)
{
	if (undo_index < 0) return;
	struct stat st;
	if (!lstat(undo_from, &st) || rename(undo_to, undo_from)) { say(_("無法復原")); undo_index = -1; return; }
	unlink(undo_info);
	files = realloc(files, (size_t)(nfiles + 1) * sizeof *files);
	int at = MIN(undo_index, nfiles);
	memmove(files + at + 1, files + at, (size_t)(nfiles - at) * sizeof *files);
	files[at] = strdup(undo_from);
	nfiles++;
	undo_index = -1;
	say(_("已復原"));
	go(at, 1);
}

/* ------------------------------------------------------------------ */
/* 剪貼簿（PNG；大的圖用 INCR 分段傳）                                   */
/* ------------------------------------------------------------------ */

static unsigned char *clip;
static size_t clip_n, clip_cap;
typedef struct { Window w; Atom prop; size_t off; } Incr;
static Incr incr[8];
#define INCR_CHUNK (256 * 1024)

static cairo_status_t png_append(void *c, const unsigned char *d, unsigned int n)
{
	(void)c;
	if (clip_n + n > clip_cap) {
		size_t nc = MAX(clip_cap * 2, clip_n + n + 65536);
		unsigned char *p = realloc(clip, nc);
		if (!p) return CAIRO_STATUS_NO_MEMORY;
		clip = p;
		clip_cap = nc;
	}
	memcpy(clip + clip_n, d, n);
	clip_n += n;
	return CAIRO_STATUS_SUCCESS;
}

/* 目前這張（照畫面的旋轉方向）的完整解析度版本 */
static cairo_surface_t *rotated_copy(void)
{
	cairo_surface_t *src = shown->frames[MIN(frame, shown->n - 1)];
	double rw, rh;
	img_dims(&rw, &rh);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)rw, (int)rh);
	cairo_t *cr = cairo_create(s);
	cairo_translate(cr, rw / 2, rh / 2);
	cairo_rotate(cr, rot * M_PI / 2);
	cairo_translate(cr, -shown->w / 2.0, -shown->h / 2.0);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_destroy(cr);
	return s;
}

static void copy_image(void)
{
	if (!cur_ready() || shown->err) return;
	say(_("正在拷貝…"));
	draw();
	clip_n = 0;
	cairo_surface_t *s = rotated_copy();
	cairo_status_t st = cairo_surface_write_to_png_stream(s, png_append, NULL);
	cairo_surface_destroy(s);
	if (st != CAIRO_STATUS_SUCCESS) { say(_("無法拷貝圖片")); return; }
	if (wl) pmwl_clip_set_type("image/png", clip, clip_n);   /* 資料函式庫自己複製，不用 INCR */
	else XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
	say(_("已拷貝圖片"));
}

static void on_selrequest(XSelectionRequestEvent *e)
{
	XSelectionEvent r = { .type = SelectionNotify, .display = e->display, .requestor = e->requestor,
	                      .selection = e->selection, .target = e->target, .property = None, .time = e->time };
	Atom prop = e->property ? e->property : e->target;
	if (e->target == a_targets) {
		Atom ts[] = { a_targets, a_png };
		XChangeProperty(dpy, e->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)ts, 2);
		r.property = prop;
	} else if (e->target == a_png && clip_n) {
		if (clip_n <= INCR_CHUNK) {
			XChangeProperty(dpy, e->requestor, prop, a_png, 8, PropModeReplace, clip, (int)clip_n);
			r.property = prop;
		} else {
			for (unsigned i = 0; i < LEN(incr); i++)
				if (!incr[i].w) {
					incr[i] = (Incr){ e->requestor, prop, 0 };
					XSelectInput(dpy, e->requestor, PropertyChangeMask);
					long len = (long)clip_n;
					XChangeProperty(dpy, e->requestor, prop, a_incr, 32, PropModeReplace, (unsigned char *)&len, 1);
					r.property = prop;
					break;
				}
		}
	}
	XSendEvent(dpy, e->requestor, False, 0, (XEvent *)&r);
}

/* 對方讀走（刪掉）屬性之後送下一段；最後送一段空的代表結束 */
static int incr_event(XPropertyEvent *e)
{
	if (e->state != PropertyDelete) return 0;
	for (unsigned i = 0; i < LEN(incr); i++) {
		Incr *x = &incr[i];
		if (x->w != e->window || x->prop != e->atom) continue;
		size_t n = MIN((size_t)INCR_CHUNK, clip_n > x->off ? clip_n - x->off : 0);
		XChangeProperty(dpy, x->w, x->prop, a_png, 8, PropModeReplace, clip + x->off, (int)n);
		x->off += n;
		if (!n) { XSelectInput(dpy, x->w, 0); x->w = 0; }
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* 其他動作                                                            */
/* ------------------------------------------------------------------ */

static void spawn_argv(char *const argv[])
{
	if (fork() == 0) {
		if (fork() == 0) {
			setsid();
			if (dpy) close(ConnectionNumber(dpy));
			execvp(argv[0], argv);
			_exit(127);
		}
		_exit(0);
	}
	wait(NULL);
}

static void set_fullscreen(int on)
{
	if (on == fullscreen) return;
	fullscreen = on;
	if (wl) pmwl_fullscreen(wl, on, -1);
	else {
		XEvent ev = { .type = ClientMessage };
		ev.xclient.window = win;
		ev.xclient.message_type = a_state;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = on ? 1 : 0;
		ev.xclient.data.l[1] = (long)a_fs;
		ev.xclient.data.l[3] = 1;
		XSendEvent(dpy, pm_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
	}
	toolbar_shown = !on;
	if (!on && cursor_hidden) { if (wl) pmwl_cursor(wl, PMWL_CURSOR_DEFAULT); else XUndefineCursor(dpy, win); cursor_hidden = 0; }
	fit = 1; panx = pany = 0;
	hq_due = pm_now_ms() + 200;
}

static void start_slideshow(void)
{
	if (nfiles < 1) return;
	slideshow = 1;
	slide_was_fs = fullscreen;
	set_fullscreen(1);
	next_slide_at = pm_now_ms() + (long)(pm_conf_num("viewer.slideshow_seconds", 4) * 1000);
	say(_("幻燈片播放中　按 Esc 結束"));
}

static void stop_slideshow(void)
{
	if (!slideshow) return;
	slideshow = 0;
	if (!slide_was_fs) set_fullscreen(0);
	say(_("幻燈片已結束"));
}

static void rotate(int d) { rot = (rot + d + 4) % 4; fit = 1; panx = pany = 0; hq_due = 1; }

static void set_wallpaper(void)
{
	if (cur < 0 || cur >= nfiles) return;
	pm_conf_set("desktop.wallpaper", files[cur]);
	say(_("已設為桌布"));
}

static void show_in_files(void)
{
	if (cur < 0 || cur >= nfiles) return;
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", files[cur]);
	char *sl = strrchr(dir, '/');
	if (sl && sl != dir) *sl = 0;
	char *v[] = { pm_have("pomelo-files") ? "pomelo-files" : "pomelo-open", dir, NULL };
	spawn_argv(v);
}

/* HEIC／AVIF 要裝的套件（Debian 名稱，空白分隔）；這個系統裝不了回傳 NULL */
static const char *heif_packages(int need)
{
	if (need == NEED_LIBTIFF) {   /* Ubuntu 22.04 是 libtiff5，之後是 libtiff6；Fedora／Rocky 都叫 libtiff */
		static int six = -1;
		if (pm_pkg_kind() == PM_PKG_APT && six < 0) {
			char *o = pm_run("apt-cache show --no-all-versions libtiff6");
			six = o && strstr(o, "Package:") ? 1 : 0;
			free(o);
		}
		return pm_pkg_kind() == PM_PKG_APT && !six ? "libtiff5" : "libtiff6";
	}
	static int plugin = -1;   /* Ubuntu 24.04 以後 HEVC 解碼器是另外的套件 libheif-plugin-libde265（22.04 內建在 libheif1） */
	if (pm_pkg_kind() == PM_PKG_APT && plugin < 0) {
		char *o = pm_run("apt-cache show --no-all-versions libheif-plugin-libde265");
		plugin = o && strstr(o, "Package:") ? 1 : 0;
		free(o);
	}
	if (pm_pkg_kind() == PM_PKG_APT) {
		if (need == NEED_HEVC) return plugin ? "libheif-plugin-libde265" : NULL;
		return plugin ? "libheif1 libheif-plugin-libde265" : "libheif1";
	}
	if (pm_pkg_kind() == PM_PKG_DNF && need == NEED_LIBHEIF) return "libheif1";
	return NULL;
}

static void install_heif(void)
{
	if (!cur_ready() || !shown->need) return;
	const char *pk = heif_packages(shown->need);
	if (!pk) return;
	char full[700], *cmd = pm_pkg_cmdline("install-min", pk);
	snprintf(full, sizeof full, "%s; echo; echo '%s'; read x", cmd ? cmd : "false", _("完成，按 Enter 關閉這個視窗"));
	free(cmd);
	char *v[] = { "pomelo-terminal", "-t", (char *)_("安裝"), "-e", "sh", "-c", full, NULL };
	if (pm_have("pomelo-terminal")) spawn_argv(v);
	else { char *x[] = { "x-terminal-emulator", "-e", "sh", "-c", full, NULL }; spawn_argv(x); }
	waiting_install = 1;
}

static void m_full(const Arg *a) { (void)a; set_fullscreen(!fullscreen); }
static void m_slide(const Arg *a) { (void)a; start_slideshow(); }
static void m_rotr(const Arg *a) { (void)a; rotate(1); }
static void m_rotl(const Arg *a) { (void)a; rotate(-1); }
static void m_actual(const Arg *a) { (void)a; zoom_to(1 / pix(), W / 2.0, top_h() + (H - top_h()) / 2.0); }
static void m_fit(const Arg *a) { (void)a; fit = 1; panx = pany = 0; hq_due = 1; }
static void m_copy(const Arg *a) { (void)a; copy_image(); }
static void m_wall(const Arg *a) { (void)a; set_wallpaper(); }
static void m_files(const Arg *a) { (void)a; show_in_files(); }
static void m_info(const Arg *a) { (void)a; show_info = !show_info; }
static void m_edit(const Arg *a) { (void)a; char *v[] = { "pomelo-paint", files[cur], NULL }; spawn_argv(v); }
static void m_trash(const Arg *a) { (void)a; delete_current(); }

static void open_menu(int x, int y)
{
	int ok = cur_ready() && !shown->err;
	MenuItem m[] = {
		{ fullscreen ? N_("離開全螢幕") : N_("全螢幕"), m_full, {0} },
		{ N_("幻燈片"), m_slide, {0} },
		{ NULL, NULL, {0} },
		{ N_("向右旋轉"), ok ? m_rotr : NULL, {0} },
		{ N_("向左旋轉"), ok ? m_rotl : NULL, {0} },
		{ N_("實際大小"), ok ? m_actual : NULL, {0} },
		{ N_("符合視窗"), ok ? m_fit : NULL, {0} },
		{ NULL, NULL, {0} },
		{ N_("拷貝圖片"), ok ? m_copy : NULL, {0} },
		{ N_("用繪圖編輯"), ok && pm_have("pomelo-paint") ? m_edit : NULL, {0} },
		{ N_("設為桌布"), ok ? m_wall : NULL, {0} },
		{ N_("在檔案管理員中顯示"), m_files, {0} },
		{ N_("資訊"), m_info, {0} },
		{ NULL, NULL, {0} },
		{ N_("丟到垃圾桶"), m_trash, {0} },
	};
	PMWL_POPUP(wl, m, LEN(m), x, y);
}

/* ------------------------------------------------------------------ */
/* 畫圖                                                                */
/* ------------------------------------------------------------------ */

enum { B_PREV, B_NEXT, B_ZOOMOUT, B_ZOOM, B_ZOOMIN, B_ROTATE, B_SLIDE, B_TRASH, B_INFO, B_MENU, NBTN };
static Rect btn[NBTN];

static void icon_chevron(cairo_t *cr, double cx, double cy, int dir, double k)
{
	cairo_new_path(cr);
	cairo_set_line_width(cr, 2 * k);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, cx + 3 * dir * k, cy - 7 * k);
	cairo_line_to(cr, cx - 4 * dir * k, cy);
	cairo_line_to(cr, cx + 3 * dir * k, cy + 7 * k);
	cairo_stroke(cr);
}

static void draw_icon(cairo_t *cr, int b, double cx, double cy)
{
	cairo_new_path(cr);
	cairo_set_line_width(cr, 1.6);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (b) {
	case B_PREV: icon_chevron(cr, cx, cy, 1, 1); break;
	case B_NEXT: icon_chevron(cr, cx, cy, -1, 1); break;
	case B_ZOOMOUT: case B_ZOOMIN:
		cairo_arc(cr, cx - 1.5, cy - 1.5, 6, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, cx + 3, cy + 3); cairo_line_to(cr, cx + 7.5, cy + 7.5);
		cairo_move_to(cr, cx - 4.5, cy - 1.5); cairo_line_to(cr, cx + 1.5, cy - 1.5);
		if (b == B_ZOOMIN) { cairo_move_to(cr, cx - 1.5, cy - 4.5); cairo_line_to(cr, cx - 1.5, cy + 1.5); }
		cairo_stroke(cr);
		break;
	case B_ROTATE:   /* 順時針的箭頭 */
		cairo_arc_negative(cr, cx, cy + 0.5, 6.5, -M_PI * 0.05, M_PI * 0.4);
		cairo_stroke(cr);
		cairo_move_to(cr, cx + 9.5, cy - 4); cairo_line_to(cr, cx + 6.4, cy + 0.5); cairo_line_to(cr, cx + 2.5, cy - 3);
		cairo_stroke(cr);
		break;
	case B_SLIDE:
		pm_rrect(cr, cx - 9, cy - 6.5, 18, 13, 2.5, 2.5);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 2.5, cy - 3.5); cairo_line_to(cr, cx + 3.5, cy); cairo_line_to(cr, cx - 2.5, cy + 3.5);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case B_TRASH:
		cairo_move_to(cr, cx - 7.5, cy - 5); cairo_line_to(cr, cx + 7.5, cy - 5);
		cairo_move_to(cr, cx - 2.5, cy - 5); cairo_line_to(cr, cx - 2, cy - 8); cairo_line_to(cr, cx + 2, cy - 8); cairo_line_to(cr, cx + 2.5, cy - 5);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 5.5, cy - 5); cairo_line_to(cr, cx - 4.5, cy + 8); cairo_line_to(cr, cx + 4.5, cy + 8); cairo_line_to(cr, cx + 5.5, cy - 5);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 1.5, cy - 2); cairo_line_to(cr, cx - 1.3, cy + 5);
		cairo_move_to(cr, cx + 1.5, cy - 2); cairo_line_to(cr, cx + 1.3, cy + 5);
		cairo_stroke(cr);
		break;
	case B_INFO:
		cairo_arc(cr, cx, cy, 8, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy - 4, 1.2, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_line_width(cr, 1.9);
		cairo_move_to(cr, cx, cy - 1); cairo_line_to(cr, cx, cy + 4.5);
		cairo_stroke(cr);
		break;
	case B_MENU:
		for (int i = -1; i <= 1; i++) { cairo_arc(cr, cx + i * 5.5, cy, 1.6, 0, 2 * M_PI); cairo_fill(cr); }
		break;
	}
}

static void fmt_size(char *out, size_t n, double bytes)
{
	if (bytes < 1000) snprintf(out, n, "%.0f B", bytes);
	else if (bytes < 1e6) snprintf(out, n, "%.0f KB", bytes / 1e3);
	else if (bytes < 1e9) snprintf(out, n, "%.1f MB", bytes / 1e6);
	else snprintf(out, n, "%.2f GB", bytes / 1e9);
}

static void draw_toolbar(cairo_t *cr, int overlay)
{
	unsigned fg = overlay ? 0xffffff : col_text;
	if (overlay) {
		cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, TB_H + 20);
		cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0.7);
		cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0);
		cairo_rectangle(cr, 0, 0, W, TB_H + 20);
		cairo_set_source(cr, g);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
	} else {
		pm_rgb(cr, col_bar);
		cairo_rectangle(cr, 0, 0, W, TB_H);
		cairo_fill(cr);
		pm_rgb(cr, col_line);
		cairo_rectangle(cr, 0, TB_H - 1, W, 1);
		cairo_fill(cr);
	}
	int ok = cur_ready() && !shown->err;
	/* 左邊：上一張、下一張 */
	double x = 10, bw = 34, bh = 30, by = (TB_H - bh) / 2;
	for (int b = B_PREV; b <= B_NEXT; b++, x += bw + 2) btn[b] = (Rect){ (int)x, (int)by, (int)bw, (int)bh };
	/* 右邊：縮小、比例、放大、旋轉、幻燈片、垃圾桶、資訊、選單（由右往左排） */
	double rx = W - 10;
	for (int b = B_MENU; b >= B_ZOOMOUT; b--) {
		double w = bw;
		if (b == B_ZOOM) { pm_font(cr, 12, 0); w = MAX(52, pm_textw(cr, "100%") + 18); }
		rx -= w;
		btn[b] = (Rect){ (int)rx, (int)by, (int)w, (int)bh };
		rx -= b == B_ROTATE || b == B_ZOOMOUT ? 10 : 2;
	}
	for (int b = 0; b < NBTN; b++) {
		Rect r = btn[b];
		int disabled = (b <= B_NEXT && nfiles < 2) || (b >= B_ZOOMOUT && b <= B_ROTATE && !ok) || ((b == B_TRASH || b == B_SLIDE) && cur < 0);
		if (b == hover_btn && !disabled) {
			pm_rrect(cr, r.x, r.y, r.w, r.h, 7, 7);
			pm_rgba(cr, overlay || pm_dark ? 0xffffff : 0x000000, overlay ? 0.18 : 0.07);
			cairo_fill(cr);
		}
		if (b == B_INFO && show_info) {
			pm_rrect(cr, r.x, r.y, r.w, r.h, 7, 7);
			pm_rgba(cr, T->sel, 0.22);
			cairo_fill(cr);
		}
		pm_rgba(cr, fg, disabled ? 0.3 : 0.9);
		if (b == B_ZOOM) {
			char z[16];
			snprintf(z, sizeof z, "%d%%", ok ? (int)lround(scale_now() * pix() * 100) : 100);
			pm_font(cr, 12, 0);
			pm_text(cr, r.x + (r.w - pm_textw(cr, z)) / 2, r.y + r.h / 2.0, z);
		} else draw_icon(cr, b, r.x + r.w / 2.0, r.y + r.h / 2.0);
	}
	/* 中間：檔名與第幾張 */
	if (cur >= 0 && cur < nfiles) {
		double left = btn[B_NEXT].x + btn[B_NEXT].w + 14, right = btn[B_ZOOMOUT].x - 14;
		char cnt[32] = "";
		if (nfiles > 1) snprintf(cnt, sizeof cnt, "%d / %d", cur + 1, nfiles);
		pm_font(cr, 12, 0);
		double cw = cnt[0] ? pm_textw(cr, cnt) + 10 : 0;
		char name[PATH_MAX];
		pm_font(cr, 13, 1);
		pm_ellipsize(cr, base_of(files[cur]), MAX(40, right - left - cw), name, sizeof name);
		double nw = pm_textw(cr, name), tx = MAX(left, (W - nw - cw) / 2);
		if (tx + nw + cw > right) tx = MAX(left, right - nw - cw);
		pm_rgb(cr, fg);
		pm_text(cr, tx, TB_H / 2.0, name);
		if (cnt[0]) {
			pm_font(cr, 12, 0);
			pm_rgba(cr, fg, 0.55);
			pm_text(cr, tx + nw + 10, TB_H / 2.0, cnt);
		}
	}
}

static void build_hq(double s)
{
	if (hq) cairo_surface_destroy(hq);
	hq = NULL;
	double rw, rh;
	img_dims(&rw, &rh);
	int w = MAX(1, (int)lround(rw * s)), h = MAX(1, (int)lround(rh * s));
	cairo_surface_t *src = shown->frames[MIN(frame, shown->n - 1)];
	hq = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(hq);
	cairo_scale(cr, w / rw, h / rh);
	cairo_translate(cr, rw / 2, rh / 2);
	cairo_rotate(cr, rot * M_PI / 2);
	cairo_translate(cr, -shown->w / 2.0, -shown->h / 2.0);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	hq_img = shown; hq_scale = s; hq_rot = rot; hq_frame = frame;
}

/* 透明的部分畫成棋盤格 */
static void checker(cairo_t *cr, double x, double y, double w, double h)
{
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	pm_rgb(cr, pm_dark ? 0x3a3a3c : 0xffffff);
	cairo_paint(cr);
	pm_rgb(cr, pm_dark ? 0x2c2c2e : 0xe5e5ea);
	for (double yy = floor(y / 10) * 10; yy < y + h; yy += 10)
		for (double xx = floor(x / 10) * 10 + ((int)(yy / 10) % 2 ? 10 : 0); xx < x + w; xx += 20)
			cairo_rectangle(cr, xx, yy, 10, 10);
	cairo_fill(cr);
	cairo_restore(cr);
}

static void draw_image(cairo_t *cr)
{
	double vt = top_h(), vh = H - vt;
	double s = scale_now();
	double rw, rh;
	img_dims(&rw, &rh);
	double cx = W / 2.0 + panx, cy = vt + vh / 2.0 + pany;
	double dw = rw * s, dh = rh * s;
	double x0 = round(cx - dw / 2), y0 = round(cy - dh / 2);
	cairo_surface_t *src = shown->frames[MIN(frame, shown->n - 1)];
	if (cairo_image_surface_get_format(src) == CAIRO_FORMAT_ARGB32 && !fullscreen) checker(cr, x0, y0, dw, dh);
	cairo_save(cr);
	cairo_rectangle(cr, 0, vt, W, vh);
	cairo_clip(cr);
	int use_hq = s < 0.999 && shown->n == 1 && hq && hq_img == shown && fabs(hq_scale - s) < 1e-9 && hq_rot == rot && hq_frame == frame;
	if (use_hq) {
		cairo_set_source_surface(cr, hq, x0, y0);
		cairo_paint(cr);
	} else {
		cairo_translate(cr, cx, cy);
		cairo_rotate(cr, rot * M_PI / 2);
		cairo_scale(cr, s, s);
		cairo_translate(cr, -shown->w / 2.0, -shown->h / 2.0);
		cairo_set_source_surface(cr, src, 0, 0);
		/* 放大很多時看得到像素（不要糊掉），縮小時先用快的，停下來再換高畫質 */
		cairo_pattern_set_filter(cairo_get_source(cr), s >= 3 ? CAIRO_FILTER_NEAREST : fabs(s - 1) < 1e-9 && !rot ? CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
		cairo_paint(cr);
		if (s < 0.999 && shown->n == 1 && !hq_due) hq_due = pm_now_ms() + 150;
	}
	cairo_restore(cr);
}

static void center_text(cairo_t *cr, double y, const char *s, double size, int bold, unsigned col)
{
	pm_font(cr, size, bold);
	char b[PATH_MAX];
	pm_ellipsize(cr, s, W - 60, b, sizeof b);
	pm_rgb(cr, col);
	pm_text(cr, (W - pm_textw(cr, b)) / 2, y, b);
}

static void draw_info(cairo_t *cr)
{
	if (cur < 0 || cur >= nfiles) return;
	if (strcmp(exif_path, files[cur])) { exif_read(files[cur], &exif); snprintf(exif_path, sizeof exif_path, "%s", files[cur]); }
	struct stat st;
	int have_st = !stat(files[cur], &st);
	const char *labels[10];
	char vals[10][PATH_MAX];
	int n = 0;
	labels[n] = _("名稱"); snprintf(vals[n++], PATH_MAX, "%s", base_of(files[cur]));
	if (cur_ready() && !shown->err) {
		int ow = shown->ow, oh = shown->oh;
		double mp = ow * (double)oh / 1e6;
		labels[n] = _("尺寸");
		if (mp >= 1) snprintf(vals[n++], PATH_MAX, _("%d × %d（%.1f 百萬像素）"), ow, oh, mp);
		else snprintf(vals[n++], PATH_MAX, "%d × %d", ow, oh);
		labels[n] = _("格式");
		if (shown->n > 1) snprintf(vals[n++], PATH_MAX, _("%s 動畫，%d 格"), shown->fmt, shown->n);
		else snprintf(vals[n++], PATH_MAX, "%s", shown->fmt);
	}
	if (have_st) {
		char sz[32];
		fmt_size(sz, sizeof sz, (double)st.st_size);
		labels[n] = _("檔案大小"); snprintf(vals[n++], PATH_MAX, "%s", sz);
		labels[n] = _("修改時間"); strftime(vals[n++], PATH_MAX, "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
	}
	if (exif.taken[0]) { labels[n] = _("拍攝時間"); snprintf(vals[n++], PATH_MAX, "%s", exif.taken); }
	if (exif.model[0]) { labels[n] = _("相機型號"); snprintf(vals[n++], PATH_MAX, "%s%s%s", exif.make, exif.make[0] ? " " : "", exif.model); }
	if (exif.fnumber[0] || exif.exposure[0] || exif.iso[0] || exif.focal[0]) {
		labels[n] = _("拍攝設定");
		snprintf(vals[n], PATH_MAX, "%s", "");
		const char *parts[] = { exif.fnumber, exif.exposure, exif.iso, exif.focal };
		for (unsigned i = 0; i < LEN(parts); i++)
			if (parts[i][0]) { if (vals[n][0]) strncat(vals[n], " · ", PATH_MAX - strlen(vals[n]) - 1); strncat(vals[n], parts[i], PATH_MAX - strlen(vals[n]) - 1); }
		n++;
	}
	labels[n] = _("所在資料夾");
	snprintf(vals[n], PATH_MAX, "%s", files[cur]);
	char *sl = strrchr(vals[n], '/');
	if (sl && sl != vals[n]) *sl = 0;
	n++;
	double pw = MIN(320, W - 40), rowh = 44, ph = 20 + n * rowh;
	double px = W - pw - 14, py = top_h() + 14;
	pm_rrect(cr, px, py, pw, ph, 12, 12);
	pm_rgba(cr, pm_dark ? 0x2c2c2e : 0xffffff, 0.95);
	cairo_fill_preserve(cr);
	pm_rgba(cr, 0x000000, pm_dark ? 0.5 : 0.12);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	for (int i = 0; i < n; i++) {
		double y = py + 12 + i * rowh;
		pm_font(cr, 11, 0);
		pm_rgb(cr, col_dim);
		pm_text(cr, px + 16, y + 10, labels[i]);
		char b[PATH_MAX];
		pm_font(cr, 13, 0);
		if (i == n - 1) {   /* 位置：保留後面的資料夾名稱 */
			const char *q = vals[i];
			while (*q && pm_textw(cr, q) > pw - 32 - pm_textw(cr, "…")) { q++; while (((unsigned char)*q & 0xc0) == 0x80) q++; }
			snprintf(b, sizeof b, "%s%s", q == vals[i] ? "" : "…", q);
		} else pm_ellipsize(cr, vals[i], pw - 32, b, sizeof b);
		pm_rgb(cr, col_text);
		pm_text(cr, px + 16, y + 28, b);
	}
}

static void draw(void)
{
	Paint p;
	cairo_t *cr = wl ? pmwl_begin(wl, &W, &H) : pm_paint_begin(&p, win, W, H);
	install_btn = (Rect){ 0, 0, 0, 0 };
	pm_rgb(cr, fullscreen ? 0x000000 : col_bg);
	cairo_paint(cr);
	long now = pm_now_ms();
	unsigned msg_col = fullscreen ? 0xdddddd : col_text, dim_col = fullscreen ? 0x999999 : col_dim;
	double mid = top_h() + (H - top_h()) / 2.0;
	if (!nfiles) {
		center_text(cr, mid - 12, _("沒有圖片"), 17, 1, msg_col);
		center_text(cr, mid + 16, _("在檔案管理員裡按兩下圖片，就會用「看圖」打開"), 13, 0, dim_col);
	} else if (shown && !shown->err) {
		draw_image(cr);
	} else if (shown && shown->err && cur_ready() && shown->need) {
		/* HEIC、AVIF：缺 libheif 或 HEVC 解碼器；TIFF：缺 libtiff */
		char m[PATH_MAX + 64];
		snprintf(m, sizeof m, _("要看 %s 圖片，需要另外安裝解碼程式"), shown->fmt);
		center_text(cr, mid - 30, m, 16, 1, msg_col);
		const char *pk = heif_packages(shown->need);
		if (pk) {
			if (shown->need == NEED_HEVC) center_text(cr, mid - 4, _("還需要 HEVC 解碼外掛（libheif-plugin-libde265）"), 12.5, 0, dim_col);
			else if (shown->need == NEED_LIBTIFF) center_text(cr, mid - 4, _("libtiff 是開放原始碼的 TIFF 解碼程式，不需要 GNOME，幾個小套件"), 12.5, 0, dim_col);
			else center_text(cr, mid - 4, _("libheif 是開放原始碼的 HEIC／AVIF 解碼程式，不需要 GNOME，大約 5 個套件"), 12.5, 0, dim_col);
			pm_font(cr, 13, 0);
			const char *l = _("安裝");
			double bw = pm_textw(cr, l) + 40;
			install_btn = (Rect){ (int)((W - bw) / 2), (int)mid + 18, (int)bw, 32 };
			pm_rrect(cr, install_btn.x, install_btn.y, install_btn.w, install_btn.h, 8, 8);
			pm_rgb(cr, T->sel);
			cairo_fill(cr);
			pm_rgb(cr, 0xffffff);
			pm_text(cr, (W - pm_textw(cr, l)) / 2, install_btn.y + 16, l);
			center_text(cr, mid + 72, _("裝好之後回到這個視窗就會重新載入（或按 F5）"), 12, 0, dim_col);
		} else if (pm_pkg_kind() == PM_PKG_DNF) {
			center_text(cr, mid - 4, _("Fedora／Rocky 的官方套件庫沒有 HEVC 解碼器（專利的關係）"), 12.5, 0, dim_col);
			center_text(cr, mid + 16, _("iPhone 的 HEIC 照片要靠 RPM Fusion 的 libheif-freeworld"), 12.5, 0, dim_col);
			center_text(cr, mid + 40, _("裝好之後按 F5 重新載入"), 12, 0, dim_col);
		} else center_text(cr, mid - 4, _("這台電腦的套件庫找不到需要的解碼程式"), 12.5, 0, dim_col);
	} else if (shown && shown->err && cur_ready()) {
		char m[PATH_MAX + 64];
		snprintf(m, sizeof m, _("無法打開「%s」"), base_of(files[cur]));
		center_text(cr, mid - 12, m, 16, 1, msg_col);
		if (shown->err == 2) snprintf(m, sizeof m, _("還不支援 %s 格式的圖片"), shown->fmt);
		else snprintf(m, sizeof m, "%s", _("檔案可能壞掉了，或不是圖片"));
		center_text(cr, mid + 16, m, 13, 0, dim_col);
	}
	/* 還在解下一張：超過 0.25 秒才顯示「正在載入…」 */
	if (nfiles && !cur_ready() && now - load_started > 250) {
		const char *m = _("正在載入…");
		pm_font(cr, 13, 0);
		double tw = pm_textw(cr, m);
		pm_rrect(cr, (W - tw) / 2 - 14, H - 56, tw + 28, 30, 15, 15);
		pm_rgba(cr, 0x000000, 0.6);
		cairo_fill(cr);
		pm_rgb(cr, 0xffffff);
		pm_text(cr, (W - tw) / 2, H - 41, m);
	}
	/* 左右兩邊的箭頭 */
	if (hover_edge && nfiles > 1 && !dragging) {
		double ex = hover_edge < 0 ? 40 : W - 40;
		cairo_arc(cr, ex, mid, 20, 0, 2 * M_PI);
		pm_rgba(cr, 0x000000, 0.45);
		cairo_fill(cr);
		pm_rgb(cr, 0xffffff);
		icon_chevron(cr, ex + (hover_edge < 0 ? -1 : 1), mid, hover_edge < 0 ? 1 : -1, 1.1);
	}
	if (show_info) draw_info(cr);
	if (!fullscreen) draw_toolbar(cr, 0);
	else if (toolbar_shown) draw_toolbar(cr, 1);
	if (toast_until > now) {
		pm_font(cr, 13, 0);
		char b[300];
		pm_ellipsize(cr, toast, W - 80, b, sizeof b);
		double tw = pm_textw(cr, b);
		pm_rrect(cr, (W - tw) / 2 - 16, H - 96, tw + 32, 34, 17, 17);
		pm_rgba(cr, 0x000000, 0.72);
		cairo_fill(cr);
		pm_rgb(cr, 0xffffff);
		pm_text(cr, (W - tw) / 2, H - 79, b);
	}
	if (wl) { pmwl_end(wl, cr); return; }
	pm_paint_end(&p, cr, 0, 0);
	XFlush(dpy);
}

/* ------------------------------------------------------------------ */
/* 事件                                                                */
/* ------------------------------------------------------------------ */

static int btn_at(int x, int y)
{
	if (fullscreen && !toolbar_shown) return -1;
	if (y >= TB_H) return -1;
	for (int b = 0; b < NBTN; b++)
		if (x >= btn[b].x && x < btn[b].x + btn[b].w && y >= btn[b].y && y < btn[b].y + btn[b].h) return b;
	return -1;
}

static int edge_at(int x, int y)
{
	if (nfiles < 2 || y < top_h() + (fullscreen && toolbar_shown ? TB_H : 0)) return 0;
	if (x < 80) return -1;
	if (x > W - 80) return 1;
	return 0;
}

static void press_btn(int b)
{
	double cx = W / 2.0, cy = top_h() + (H - top_h()) / 2.0;
	int ok = cur_ready() && !shown->err;
	switch (b) {
	case B_PREV: if (nfiles > 1) go(cur - 1, -1); break;
	case B_NEXT: if (nfiles > 1) go(cur + 1, 1); break;
	case B_ZOOMOUT: if (ok) zoom_step(-1, cx, cy); break;
	case B_ZOOMIN: if (ok) zoom_step(1, cx, cy); break;
	case B_ZOOM: if (ok) { if (fit && fabs(fit_scale() * pix() - 1) > 1e-9) m_actual(NULL); else m_fit(NULL); } break;
	case B_ROTATE: if (ok) rotate(1); break;
	case B_SLIDE: start_slideshow(); break;
	case B_TRASH: delete_current(); break;
	case B_INFO: show_info = !show_info; break;
	case B_MENU: {
		Window r, c; int rx, ry, wx, wy; unsigned mk;
		if (wl) { rx = mouse_x; ry = mouse_y; }   /* 視窗座標（pmwl_menu 用） */
		else XQueryPointer(dpy, win, &r, &c, &rx, &ry, &wx, &wy, &mk);
		open_menu(rx - 150, ry + 14);
		break;
	}
	}
}

static void keypress(XKeyEvent *e)
{
	char buf[16];
	KeySym ks = NoSymbol;
	KeySym k0;
	if (pmwl_xkey(e, buf, sizeof buf, &ks, &k0) < 0) { XLookupString(e, buf, sizeof buf, &ks, NULL); k0 = XLookupKeysym(e, 0); }
	int ctrl = !!(e->state & (ControlMask | Mod4Mask)), shift = !!(e->state & ShiftMask);
	double cx = W / 2.0, cy = top_h() + (H - top_h()) / 2.0;
	int ok = cur_ready() && !shown->err;
	if (slideshow && ks != XK_Right && ks != XK_Left && ks != XK_space) { stop_slideshow(); if (ks == XK_Escape) return; }
	if (ctrl) {
		switch (k0) {
		case XK_w: case XK_q: exit(0);
		case XK_c: copy_image(); return;
		case XK_z: undo_delete(); return;
		case XK_r: decoders_retry(); reload_current(); return;
		case XK_plus: case XK_equal: if (ok) zoom_step(1, cx, cy); return;
		case XK_minus: if (ok) zoom_step(-1, cx, cy); return;
		case XK_0: m_fit(NULL); return;
		}
		return;
	}
	switch (ks) {
	case XK_Left: case XK_Page_Up: case XK_BackSpace: case XK_KP_Left: go(cur - 1, -1); return;
	case XK_Right: case XK_Page_Down: case XK_space: case XK_KP_Right: if (shift && ks == XK_space) go(cur - 1, -1); else go(cur + 1, 1); return;
	case XK_Home: go(0, 1); return;
	case XK_End: go(nfiles - 1, -1); return;
	case XK_Up: case XK_Down: if (!fit) { pany += ks == XK_Up ? 60 : -60; clamp_pan(); } return;
	case XK_plus: case XK_equal: case XK_KP_Add: if (ok) zoom_step(1, cx, cy); return;
	case XK_minus: case XK_KP_Subtract: if (ok) zoom_step(-1, cx, cy); return;
	case XK_0: case XK_KP_0: m_fit(NULL); return;
	case XK_1: case XK_KP_1: if (ok) m_actual(NULL); return;
	case XK_f: case XK_F: case XK_F11: set_fullscreen(!fullscreen); return;
	case XK_Escape: if (show_info) show_info = 0; else if (fullscreen) set_fullscreen(0); return;
	case XK_r: if (ok) rotate(1); return;
	case XK_R: case XK_l: case XK_L: if (ok) rotate(-1); return;
	case XK_s: case XK_S: if (slideshow) stop_slideshow(); else start_slideshow(); return;
	case XK_i: case XK_I: show_info = !show_info; return;
	case XK_Delete: case XK_KP_Delete: delete_current(); return;
	case XK_F5: decoders_retry(); reload_current(); return;
	case XK_Menu: open_menu(e->x_root, e->y_root); return;
	}
}

static void show_toolbar_briefly(void)
{
	if (cursor_hidden) { if (wl) pmwl_cursor(wl, PMWL_CURSOR_DEFAULT); else XUndefineCursor(dpy, win); cursor_hidden = 0; }
	if (!fullscreen) return;
	toolbar_hide_at = pm_now_ms() + 2200;
}

static void bpress(XButtonEvent *e)
{
	show_toolbar_briefly();
	if (slideshow && e->button <= 3) { stop_slideshow(); return; }
	int ok = cur_ready() && !shown->err;
	switch (e->button) {
	case 4: case 5: if (ok) zoom_step(e->button == 4 ? 1 : -1, e->x, e->y); return;
	case 6: go(cur - 1, -1); return;
	case 7: go(cur + 1, 1); return;
	case 8: go(cur - 1, -1); return;
	case 9: go(cur + 1, 1); return;
	case 3: open_menu(e->x_root, e->y_root); return;
	case 1: break;
	default: return;
	}
	int b = btn_at(e->x, e->y);
	if (b >= 0) { press_btn(b); return; }
	if (install_btn.w && e->x >= install_btn.x && e->x < install_btn.x + install_btn.w && e->y >= install_btn.y && e->y < install_btn.y + install_btn.h) { install_heif(); return; }
	if (e->y < top_h()) return;
	if (e->time - last_click < 350 && ok) {   /* 按兩下：符合視窗 ⇄ 實際大小 */
		last_click = 0;
		if (fit && fabs(fit_scale() * pix() - 1) > 1e-9) zoom_to(1 / pix(), e->x, e->y);
		else m_fit(NULL);
		return;
	}
	last_click = e->time;
	dragging = 1; drag_moved = 0;
	drag_x = e->x; drag_y = e->y;
	drag_px = panx; drag_py = pany;
}

static void brelease(XButtonEvent *e)
{
	if (e->button != 1 || !dragging) return;
	dragging = 0;
	if (!drag_moved) {
		int ed = edge_at(e->x, e->y);
		if (ed) { last_click = 0; go(cur + ed, ed); }
	}
}

static void motion(int x, int y)
{
	mouse_x = x; mouse_y = y;
	show_toolbar_briefly();
	if (fullscreen && y < TB_H + 10) toolbar_shown = 1;
	if (dragging) {
		if (abs(x - drag_x) + abs(y - drag_y) > 4) drag_moved = 1;
		if (drag_moved) {
			panx = drag_px + (x - drag_x);
			pany = drag_py + (y - drag_y);
			clamp_pan();
		}
		return;
	}
	hover_btn = btn_at(x, y);
	hover_edge = edge_at(x, y);
}

/* ------------------------------------------------------------------ */

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	/* 一張照片：天空、太陽、兩座山 */
	pm_rrect(cr, 10, 20, 108, 88, 14, 14);
	cairo_set_source_rgb(cr, 0.97, 0.97, 0.98);
	cairo_fill(cr);
	cairo_save(cr);
	pm_rrect(cr, 18, 28, 92, 72, 8, 8);
	cairo_clip(cr);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 28, 0, 100);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.36, 0.67, 0.98);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.70, 0.86, 1.0);
	cairo_set_source(cr, g);
	cairo_paint(cr);
	cairo_pattern_destroy(g);
	cairo_arc(cr, 86, 48, 9, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, 1.0, 0.80, 0.25);
	cairo_fill(cr);
	cairo_move_to(cr, 18, 100); cairo_line_to(cr, 50, 58); cairo_line_to(cr, 82, 100); cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.20, 0.62, 0.38);
	cairo_fill(cr);
	cairo_move_to(cr, 56, 100); cairo_line_to(cr, 82, 70); cairo_line_to(cr, 112, 100); cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.13, 0.48, 0.30);
	cairo_fill(cr);
	cairo_restore(cr);
	pm_rrect(cr, 10, 20, 108, 88, 14, 14);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

static void dispatch(XEvent *ev);
static void open_dropped(char **paths, int n);

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		printf("%s: pomelo-viewer [%s…]\n", _("用法"), _("圖片或資料夾"));
		return 0;
	}
	setlocale(LC_ALL, "");
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	int native = pmwl_connect();
	if (native) { pm_ui_native = 1; a_delete = PMWL_XCLOSE; }
	else if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "pomelo-viewer: %s\n", _("無法開啟 X display")); return 1; }
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	unsigned accent = (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16);
	pm_set_theme(pm_conf_int("appearance.dark", 0), accent);
	colors();
	if (!native) pm_dispatch = dispatch;
	shown_idx = -1;

	build_list(argc, argv);
	if (pipe(wake_fd) == 0) {
		fcntl(wake_fd[0], F_SETFL, O_NONBLOCK);
		fcntl(wake_fd[1], F_SETFL, O_NONBLOCK);
	}
	pthread_t th;
	pthread_create(&th, NULL, worker, NULL);

	Rect m = { 0, 0, 1280, 720 };
	if (native) { Rect o[8]; if (pmwl_outputs(o, 8) > 0) m = o[0]; }
	else { pm_update_monitors(); m = mons[pm_pointer_mon()]; }
	W = MIN(1100, (int)(m.w * 0.8));
	H = MIN(780, (int)(m.h * 0.85));
	if (native) {
		wl = pmwl_window(_("看圖"), "pomelo-viewer", W, H, 420, 300, NULL);
		if (!wl) { fprintf(stderr, "pomelo-viewer: %s\n", _("無法開啟 Wayland 視窗")); return 1; }
		win = PMWL_XID(wl);
		static const char *const uri[] = { "text/uri-list" };
		pmwl_dnd_target(wl, uri, 1, PMWL_DND_COPY);   /* 收拖進來的檔案 */
		if (cur >= 0) go(cur, 1);
		set_title();
		pmwl_watch_fd(wake_fd[0]);
		/* 只開一張圖時，給背景執行緒一點時間，第一個畫面就有圖 */
		for (int i = 0; i < 40 && nfiles && !cur_ready(); i++) { usleep(5000); poll_ready(); }
		goto loop;
	}
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_state = XInternAtom(dpy, "_NET_WM_STATE", False);
	a_fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_png = XInternAtom(dpy, "image/png", False);
	a_incr = XInternAtom(dpy, "INCR", False);
	/* backing store：被別的視窗（例如走過去的小柚）蓋住再露出來時，X 伺服器自己補回來，不用重畫 */
	XSetWindowAttributes wa = { .background_pixel = col_bg, .bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
	                            .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
	                                          PointerMotionMask | LeaveWindowMask | StructureNotifyMask | FocusChangeMask };
	win = XCreateWindow(dpy, pm_root, m.x + (m.w - W) / 2, m.y + (m.h - H) / 2, W, H, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	XClassHint ch = { "pomelo-viewer", "Pomelo-viewer" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 420; sz->min_height = 300;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	pm_dnd_accept(dpy, win);   /* 收拖進來的檔案 */
	long pid = getpid();
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	cairo_surface_t *is = icon(64);
	unsigned long *d = malloc((2 + 64 * 64) * sizeof(unsigned long));
	d[0] = 64; d[1] = 64;
	unsigned *px = (unsigned *)cairo_image_surface_get_data(is);
	for (int i = 0; i < 64 * 64; i++) d[2 + i] = px[i];
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)d, 2 + 64 * 64);
	free(d);
	cairo_surface_destroy(is);
	Pixmap bp = XCreatePixmap(dpy, win, 1, 1, 1);
	XColor black = { 0 };
	blank_cursor = XCreatePixmapCursor(dpy, bp, bp, &black, &black, 0, 0);
	XFreePixmap(dpy, bp);

	if (cur >= 0) go(cur, 1);
	set_title();
	pm_conf_watch(dpy);
	XMapWindow(dpy, win);

	/* 只開一張圖時，給背景執行緒一點時間，第一個畫面就有圖 */
	for (int i = 0; i < 40 && nfiles && !cur_ready(); i++) { usleep(5000); poll_ready(); }

loop:;
	int xfd = dpy ? ConnectionNumber(dpy) : -1;
	for (;;) {
		while (dpy && XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			dispatch(&ev);
		}
		long now = pm_now_ms();
		/* 計時的事：動畫下一格、幻燈片、工具列自動隱藏、高畫質縮圖、提示消失 */
		if (cur_ready() && shown->n > 1 && next_frame_at && now >= next_frame_at) {
			frame = (frame + 1) % shown->n;
			next_frame_at = now + shown->delays[frame];
			dirty = 1;
		}
		if (slideshow && cur_ready() && next_slide_at && now >= next_slide_at) { next_slide_at = 0; go(cur + 1, 1); dirty = 1; }
		if (fullscreen && toolbar_hide_at && now >= toolbar_hide_at && !dragging) {
			toolbar_hide_at = 0;
			if (mouse_y > TB_H + 10 || slideshow) { toolbar_shown = 0; dirty = 1; }
			if (!cursor_hidden) { if (wl) pmwl_cursor(wl, PMWL_CURSOR_HIDDEN); else XDefineCursor(dpy, win, blank_cursor); cursor_hidden = 1; }
		}
		if (hq_due && now >= hq_due && cur_ready() && !shown->err) {
			hq_due = 0;
			double s = scale_now();
			if (s < 0.999 && shown->n == 1 && !(hq && hq_img == shown && fabs(hq_scale - s) < 1e-9 && hq_rot == rot && hq_frame == frame)) build_hq(s);
			dirty = 1;
		}
		if (toast_until && now >= toast_until) { toast_until = 0; dirty = 1; }
		if (nfiles && !cur_ready() && now - load_started > 250 && now - load_started < 400) dirty = 1;
		if (dirty) { draw(); dirty = 0; }

		long wake = 0;
#define WAKE(t) do { if ((t) > 0 && (!wake || (t) < wake)) wake = (t); } while (0)
		if (cur_ready() && shown->n > 1) WAKE(next_frame_at);
		if (slideshow) WAKE(next_slide_at);
		if (fullscreen) WAKE(toolbar_hide_at);
		if (cur_ready() && !shown->err) WAKE(hq_due);   /* 讀不出來的圖（err）不會做高畫質重畫：hq_due 永遠不清，不能拿它定時醒來（原本每毫秒空轉） */
		WAKE(toast_until);
		if (nfiles && !cur_ready() && now < load_started + 400) WAKE(load_started + 260);   /* 只在「載入中…」出現的那段時間醒來；之後讀不出來也不能每毫秒空轉（等 wake_fd） */
		if (wl) {
			/* 原生：等 Wayland 事件、背景執行緒的 wake_fd（pmwl_watch_fd）或時間到 */
			now = pm_now_ms();
			PmwEvent pe;
			int got = pmwl_next(&pe, wake ? MAX(0, wake - now) + 1 : -1);
			while (got) {
				XEvent ev;
				switch (pe.type) {
				case PMW_FD: {
					char b[64];
					while (read(wake_fd[0], b, sizeof b) > 0) {}
					Image *before = shown;
					int was_ready = cur_ready();
					poll_ready();
					if (shown != before || cur_ready() != was_ready) dirty = 1;
					break;
				}
				case PMW_CONF: {
					unsigned accent = (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16);
					pm_set_theme(pm_conf_int("appearance.dark", 0), accent);
					colors();
					dirty = 1;
					break;
				}
				case PMW_DND_DROP: {   /* 拖進來的圖 */
					char **dropped;
					int nd = pmwl_dnd_files(&dropped);
					if (nd > 0) { open_dropped(dropped, nd); pm_dnd_free(dropped, nd); }
					break;
				}
				default: if (pmwl_to_xevent(&pe, &ev)) dispatch(&ev);
				}
				got = pmwl_next(&pe, 0);
			}
			continue;
		}
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		FD_SET(wake_fd[0], &fds);
		struct timeval tv = { 0, 0 };
		now = pm_now_ms();
		if (wake) { long ms = MAX(0, wake - now) + 1; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000; }
		if (select(MAX(xfd, wake_fd[0]) + 1, &fds, NULL, NULL, wake ? &tv : NULL) > 0 && FD_ISSET(wake_fd[0], &fds)) {
			char b[64];
			while (read(wake_fd[0], b, sizeof b) > 0) {}
			Image *before = shown;
			int was_ready = cur_ready();
			poll_ready();
			if (shown != before || cur_ready() != was_ready) dirty = 1;
		}
	}
}

/* 拖進來的圖：換成這些（只有一張的話，同一個資料夾的圖也列進來，跟從檔案管理員打開一樣） */
static void open_dropped(char **paths, int n)
{
	for (int i = 0; i < nfiles; i++) free(files[i]);
	free(files);
	files = NULL;
	nfiles = 0;
	cur = -1;
	char **av = malloc((size_t)(n + 1) * sizeof *av);
	av[0] = "pomelo-viewer";
	for (int i = 0; i < n; i++) av[i + 1] = paths[i];
	build_list(n + 1, av);
	free(av);
	pthread_mutex_lock(&mu);
	shown = NULL;
	shown_idx = -1;
	pthread_mutex_unlock(&mu);
	if (cur >= 0) go(cur, 1);
	else { request(0); set_title(); }
	dirty = 1;
}

static void dispatch(XEvent *ev)
{
	char **dropped;
	int nd = wl ? -1 : pm_dnd_event(dpy, ev, &dropped);
	if (nd >= 0) {
		if (nd > 0) { open_dropped(dropped, nd); pm_dnd_free(dropped, nd); }
		return;
	}
	switch (ev->type) {
	case Expose: if (!ev->xexpose.count) dirty = 1; break;
	case ConfigureNotify:
		if (ev->xconfigure.window == win && (ev->xconfigure.width != W || ev->xconfigure.height != H)) {
			W = ev->xconfigure.width; H = ev->xconfigure.height;
			clamp_pan();
			hq_due = pm_now_ms() + 150;
			dirty = 1;
		}
		break;
	case KeyPress: keypress(&ev->xkey); dirty = 1; break;
	case ButtonPress: bpress(&ev->xbutton); dirty = 1; break;
	case ButtonRelease: brelease(&ev->xbutton); dirty = 1; break;
	case MotionNotify: {
		int ob = hover_btn, oe = hover_edge, ot = toolbar_shown;
		motion(ev->xmotion.x, ev->xmotion.y);
		if (dragging || ob != hover_btn || oe != hover_edge || ot != toolbar_shown) dirty = 1;
		break;
	}
	case LeaveNotify: if (!dragging) { hover_btn = -1; hover_edge = 0; dirty = 1; } break;
	case FocusIn:
		/* 裝完 libheif 回到視窗：重新載入 */
		if (waiting_install && cur_ready() && shown->need) { decoders_retry(); reload_current(); dirty = 1; }
		break;
	case SelectionRequest: on_selrequest(&ev->xselectionrequest); break;
	case SelectionClear: clip_n = 0; break;
	case ClientMessage: if ((Atom)ev->xclient.data.l[0] == a_delete) exit(0); break;
	case MappingNotify: XRefreshKeyboardMapping(&ev->xmapping); break;
	case PropertyNotify:
		if (incr_event(&ev->xproperty)) break;
		if (pm_conf_changed(dpy, ev)) {
			unsigned accent = (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16);
			pm_set_theme(pm_conf_int("appearance.dark", 0), accent);
			colors();
			XSetWindowBackground(dpy, win, col_bg);
			dirty = 1;
		}
		break;
	}
}
