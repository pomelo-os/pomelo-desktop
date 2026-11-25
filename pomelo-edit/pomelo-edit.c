/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-edit — Pomelo 文字編輯器
 *
 * 純文字編輯器：多個分頁、左邊的檔案清單（資料夾樹）、選單列（檔案／編輯／檢視）、
 * 尋找與取代、跳到第幾行、自動換行、行號、復原／重做、剪貼簿、中文輸入法。
 * Big5 的舊文字檔會自動轉碼顯示，存檔時照樣存回 Big5。
 * 同時只開一個視窗：從檔案管理員再打開別的文字檔，會變成這個視窗的新分頁（「新視窗」除外）。
 * 檔案被別的程式改了（例如小柚在最後面加字）：沒修改過的分頁自動重新載入（可以復原），有修改過的只提醒。
 * 視窗屬性 _POMELO_EDIT_DOCS 列出開著的檔案，給小柚找「這個檔案」、避免蓋掉還沒儲存的修改。
 *
 * 用法：pomelo-edit [--new-window] [檔案或資料夾…]
 * 快捷鍵（Ctrl 或 Super）：N 新分頁、Shift+N 新視窗、O 開啟、S 儲存、Shift+S 另存新檔、W 關閉分頁、Q 結束、
 *   Tab／PageDown 下一個分頁、Shift+Tab／PageUp 上一個（Alt+1～9 直接跳）、Z 復原、Shift+Z／Y 重做、
 *   X／C／V 剪下拷貝貼上、A 全選、F 尋找、H 取代、G／F3 找下一個、Shift+G／Shift+F3 找上一個、L 跳到第幾行、
 *   B 檔案清單、加號／減號／0 字型大小；Alt+F／E／V（或 F10）打開選單列
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-wl.h>
#include <pomelo-dnd.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <iconv.h>
#include <locale.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#define PAD 14
#define MENU_H 30
#define TAB_H 34
#define BAR_H 40
#define STATUS_H 26
#define SIDE_ROW 26
#define MAX_UNDO 300
#define UNDO_BYTES (64u << 20)   /* 每個分頁的復原紀錄最多佔這麼多記憶體 */
#define MAX_DOCS 64

/* ------------------------------------------------------------------ */
/* 文件                                                                */
/* ------------------------------------------------------------------ */

typedef struct { char *s; int n, cap; } Line;   /* 一行（不含換行），UTF-8 */
typedef struct { char *text; size_t n; int cy, cx; } Snap;
typedef struct { int line, start, end; } Row;    /* 畫面上的一列（自動換行時一行會切成好幾列） */

typedef struct {
	Line *lines;
	int nlines, lcap;
	int cy, cx;               /* 游標：行、位元組位移 */
	int ay, ax;               /* 選取的起點（ay = -1：沒有選取） */
	int modified, crlf, big5;
	char path[PATH_MAX];      /* 空的 = 未命名 */
	int untitled;             /* 「未命名 2」的 2 */
	Snap undo[MAX_UNDO], redo[MAX_UNDO];
	int nundo, nredo;
	size_t undo_bytes;
	long last_edit_ms;
	int last_edit_kind;       /* 1 打字、2 刪除、0 其他：同類連續的編輯合併成一步 */
	Row *rows;
	int nrows, rcap, layout_dirty;
	int scroll_row;
	double scroll_x, goal_x;  /* 不自動換行時的水平捲動；上下移動時想要的 x（< 0 = 用目前的） */
	unsigned version;         /* 每次修改加一 */
	int big;                  /* 很大的檔案：不自動換行（換行要量每一行，太慢） */
	unsigned chars_ver;       /* 狀態列的字數（文件沒變就不重算） */
	long chars;
	struct timespec disk_mt;  /* 上次讀寫時磁碟上的樣子：別的程式（例如小柚）改了檔案就看得出來 */
	off_t disk_size;
	ino_t disk_ino;
	int disk_ok;
} Doc;

static Doc *docs[MAX_DOCS];
static int ndocs, cur;
#define D (docs[cur])

static int u8len(const char *s) { unsigned char c = (unsigned char)*s; return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }
static int prev_char(const char *s, int i) { if (i <= 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--; return i; }
static int next_char(const char *s, int n, int i) { if (i >= n) return n; i += u8len(s + i); return i > n ? n : i; }
static int is_word(const char *s, int i) { unsigned char c = (unsigned char)s[i]; return c >= 0x80 || isalnum(c) || c == '_'; }
static const char *base_of(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }

static void line_reserve(Line *l, int n)
{
	if (n + 1 > l->cap) { l->cap = MAX(l->cap * 2, n + 16); l->s = realloc(l->s, (size_t)l->cap); }
}

static void lines_insert(Doc *d, int at, const char *s, int n)
{
	if (d->nlines == d->lcap) { d->lcap = MAX(d->lcap * 2, 64); d->lines = realloc(d->lines, (size_t)d->lcap * sizeof *d->lines); }
	memmove(&d->lines[at + 1], &d->lines[at], (size_t)(d->nlines - at) * sizeof *d->lines);
	d->lines[at] = (Line){ 0 };
	line_reserve(&d->lines[at], n);
	memcpy(d->lines[at].s, s, (size_t)n);
	d->lines[at].s[n] = 0;
	d->lines[at].n = n;
	d->nlines++;
}

static void lines_delete(Doc *d, int at)
{
	free(d->lines[at].s);
	memmove(&d->lines[at], &d->lines[at + 1], (size_t)(d->nlines - at - 1) * sizeof *d->lines);
	d->nlines--;
}

static void doc_set_text(Doc *d, const char *t, size_t n)
{
	for (int i = 0; i < d->nlines; i++) free(d->lines[i].s);
	d->nlines = 0;
	d->crlf = 0;
	size_t i = 0;
	while (i <= n) {
		const char *nl = memchr(t + i, '\n', n - i);
		size_t end = nl ? (size_t)(nl - t) : n, len = end - i;
		if (len && t[end - 1] == '\r') { len--; d->crlf = 1; }
		lines_insert(d, d->nlines, t + i, (int)len);
		if (!nl) break;
		i = end + 1;
	}
	if (!d->nlines) lines_insert(d, 0, "", 0);
	d->layout_dirty = 1;
	d->version++;
}

static char *doc_text(Doc *d, size_t *out)
{
	size_t total = 0;
	for (int i = 0; i < d->nlines; i++) total += (size_t)d->lines[i].n + 1;
	char *t = malloc(total + 1), *p = t;
	for (int i = 0; i < d->nlines; i++) {
		memcpy(p, d->lines[i].s, (size_t)d->lines[i].n);
		p += d->lines[i].n;
		if (i < d->nlines - 1) *p++ = '\n';
	}
	*p = 0;
	*out = (size_t)(p - t);
	return t;
}

static Doc *doc_new(void)
{
	Doc *d = calloc(1, sizeof *d);
	d->ay = -1;
	d->goal_x = -1;
	lines_insert(d, 0, "", 0);
	d->layout_dirty = 1;
	return d;
}

static void snap_free(Snap *s) { free(s->text); s->text = NULL; }

static void doc_free(Doc *d)
{
	for (int i = 0; i < d->nlines; i++) free(d->lines[i].s);
	free(d->lines);
	for (int i = 0; i < d->nundo; i++) snap_free(&d->undo[i]);
	for (int i = 0; i < d->nredo; i++) snap_free(&d->redo[i]);
	free(d->rows);
	free(d);
}

/* 空白、沒改過、沒有檔名的分頁（打開檔案時直接拿來用） */
static int doc_pristine(Doc *d) { return !d->path[0] && !d->modified && d->nlines == 1 && !d->lines[0].n; }

/* ---------------- 復原／重做：整份文件的快照 ---------------- */

static void undo_drop_oldest(Doc *d)
{
	d->undo_bytes -= d->undo[0].n;
	snap_free(&d->undo[0]);
	memmove(d->undo, d->undo + 1, (size_t)(d->nundo - 1) * sizeof *d->undo);
	d->nundo--;
}

static void push_undo(Doc *d, int kind)
{
	long now = pm_now_ms();
	if (kind && kind == d->last_edit_kind && now - d->last_edit_ms < 800 && d->nundo) { d->last_edit_ms = now; return; }
	d->last_edit_kind = kind;
	d->last_edit_ms = now;
	if (d->nundo == MAX_UNDO) undo_drop_oldest(d);
	Snap *s = &d->undo[d->nundo++];
	s->text = doc_text(d, &s->n);
	s->cy = d->cy; s->cx = d->cx;
	d->undo_bytes += s->n;
	while (d->undo_bytes > UNDO_BYTES && d->nundo > 1) undo_drop_oldest(d);
	for (int i = 0; i < d->nredo; i++) snap_free(&d->redo[i]);
	d->nredo = 0;
}

static void restore(Doc *d, Snap *s)
{
	doc_set_text(d, s->text, s->n);
	d->cy = MIN(s->cy, d->nlines - 1);
	d->cx = MIN(s->cx, d->lines[d->cy].n);
	d->ay = -1;
	d->modified = 1;
}

static void do_undo(Doc *d)
{
	if (!d->nundo) return;
	if (d->nredo == MAX_UNDO) { snap_free(&d->redo[0]); memmove(d->redo, d->redo + 1, (MAX_UNDO - 1) * sizeof *d->redo); d->nredo--; }
	Snap cur_s = { NULL, 0, d->cy, d->cx };
	cur_s.text = doc_text(d, &cur_s.n);
	d->redo[d->nredo++] = cur_s;
	Snap *u = &d->undo[--d->nundo];
	d->undo_bytes -= u->n;
	restore(d, u);
	snap_free(u);
	d->last_edit_kind = 0;
}

static void do_redo(Doc *d)
{
	if (!d->nredo) return;
	if (d->nundo == MAX_UNDO) undo_drop_oldest(d);
	Snap cur_s = { NULL, 0, d->cy, d->cx };
	cur_s.text = doc_text(d, &cur_s.n);
	d->undo[d->nundo++] = cur_s;
	d->undo_bytes += cur_s.n;
	restore(d, &d->redo[--d->nredo]);
	snap_free(&d->redo[d->nredo]);
	d->last_edit_kind = 0;
}

/* ---------------- 選取與編輯 ---------------- */

/* 選取起點可能因為文件變動而過期（例如合併行之後），每次都先驗證、夾住 */
static int has_sel(Doc *d)
{
	if (d->ay < 0) return 0;
	if (d->ay >= d->nlines) { d->ay = -1; return 0; }
	if (d->ax > d->lines[d->ay].n) d->ax = d->lines[d->ay].n;
	if (d->ax < 0) d->ax = 0;
	return !(d->ay == d->cy && d->ax == d->cx);
}

static void sel_bounds(Doc *d, int *y1, int *x1, int *y2, int *x2)
{
	if (d->ay < d->cy || (d->ay == d->cy && d->ax < d->cx)) { *y1 = d->ay; *x1 = d->ax; *y2 = d->cy; *x2 = d->cx; }
	else { *y1 = d->cy; *x1 = d->cx; *y2 = d->ay; *x2 = d->ax; }
}

static char *sel_text(Doc *d, size_t *n)
{
	if (!has_sel(d)) { *n = 0; return NULL; }
	int y1, x1, y2, x2;
	sel_bounds(d, &y1, &x1, &y2, &x2);
	size_t total = 0;
	for (int y = y1; y <= y2; y++) total += (size_t)d->lines[y].n + 1;
	char *t = malloc(total + 1), *p = t;
	for (int y = y1; y <= y2; y++) {
		int from = y == y1 ? x1 : 0, to = y == y2 ? x2 : d->lines[y].n;
		memcpy(p, d->lines[y].s + from, (size_t)(to - from));
		p += to - from;
		if (y < y2) *p++ = '\n';
	}
	*p = 0;
	*n = (size_t)(p - t);
	return t;
}

static void touched(Doc *d) { d->modified = 1; d->layout_dirty = 1; d->version++; d->goal_x = -1; }

static void delete_sel(Doc *d)
{
	if (!has_sel(d)) return;
	int y1, x1, y2, x2;
	sel_bounds(d, &y1, &x1, &y2, &x2);
	Line *a = &d->lines[y1], *b = &d->lines[y2];
	int tail = b->n - x2;
	line_reserve(a, x1 + tail);
	b = &d->lines[y2];
	memmove(a->s + x1, b->s + x2, (size_t)tail);
	a->n = x1 + tail;
	a->s[a->n] = 0;
	for (int y = y2; y > y1; y--) lines_delete(d, y);
	d->cy = y1; d->cx = x1; d->ay = -1;
	touched(d);
}

static void insert_text(Doc *d, const char *t, size_t n)
{
	delete_sel(d);
	d->ay = -1;   /* 編輯之後不留選取起點，否則下一個鍵會把剛打的字當成選取刪掉 */
	size_t i = 0;
	while (i < n) {
		const char *nl = memchr(t + i, '\n', n - i);
		size_t end = nl ? (size_t)(nl - t) : n, len = end - i;
		Line *l = &d->lines[d->cy];
		line_reserve(l, l->n + (int)len);
		memmove(l->s + d->cx + len, l->s + d->cx, (size_t)(l->n - d->cx));
		memcpy(l->s + d->cx, t + i, len);
		l->n += (int)len;
		l->s[l->n] = 0;
		d->cx += (int)len;
		if (nl) {   /* 換行：把游標後面的搬到新的一行 */
			l = &d->lines[d->cy];
			lines_insert(d, d->cy + 1, l->s + d->cx, l->n - d->cx);
			l = &d->lines[d->cy];
			l->n = d->cx;
			l->s[l->n] = 0;
			d->cy++;
			d->cx = 0;
		}
		i = end + 1;
	}
	touched(d);
}

static void backspace(Doc *d, int forward)
{
	if (has_sel(d)) { push_undo(d, 0); delete_sel(d); return; }
	d->ay = -1;
	push_undo(d, 2);
	Line *l = &d->lines[d->cy];
	if (forward) {
		if (d->cx < l->n) { int e = next_char(l->s, l->n, d->cx); memmove(l->s + d->cx, l->s + e, (size_t)(l->n - e)); l->n -= e - d->cx; l->s[l->n] = 0; }
		else if (d->cy < d->nlines - 1) { Line *m = &d->lines[d->cy + 1]; line_reserve(l, l->n + m->n); m = &d->lines[d->cy + 1]; memcpy(l->s + l->n, m->s, (size_t)m->n); l->n += m->n; l->s[l->n] = 0; lines_delete(d, d->cy + 1); }
		else return;
	} else {
		if (d->cx > 0) { int s = prev_char(l->s, d->cx); memmove(l->s + s, l->s + d->cx, (size_t)(l->n - d->cx)); l->n -= d->cx - s; l->s[l->n] = 0; d->cx = s; }
		else if (d->cy > 0) { Line *p = &d->lines[d->cy - 1]; int at = p->n; line_reserve(p, p->n + l->n); l = &d->lines[d->cy]; memcpy(p->s + p->n, l->s, (size_t)l->n); p->n += l->n; p->s[p->n] = 0; lines_delete(d, d->cy); d->cy--; d->cx = at; }
		else return;
	}
	touched(d);
}

/* ------------------------------------------------------------------ */
/* 檔案（UTF-8；不是 UTF-8 的話當成 Big5 等舊編碼）                     */
/* ------------------------------------------------------------------ */

static char legacy_cs[24] = "CP950";

static int valid_utf8(const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n; ) {
		unsigned c = s[i];
		int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
		if (k < 0 || (k && i + (size_t)k >= n)) return 0;
		for (int j = 1; j <= k; j++) if ((s[i + j] & 0xc0) != 0x80) return 0;
		i += (size_t)k + 1;
	}
	return 1;
}

/* from → to；回傳新配置的字串，失敗回傳 NULL */
static char *convert(const char *from, const char *to, const char *in, size_t n, size_t *outn)
{
	iconv_t cd = iconv_open(to, from);
	if (cd == (iconv_t)-1) return NULL;
	size_t cap = n * 4 + 16, ol = cap;
	char *out = malloc(cap + 1), *op = out, *ip = (char *)in;
	size_t il = n;
	size_t r = iconv(cd, &ip, &il, &op, &ol);
	iconv_close(cd);
	if (r == (size_t)-1 || il) { free(out); return NULL; }
	*op = 0;
	*outn = (size_t)(op - out);
	return out;
}

static const char *legacy_name(void) { return !strcmp(legacy_cs, "CP950") ? "Big5" : !strcmp(legacy_cs, "CP932") ? "Shift_JIS" : !strcmp(legacy_cs, "CP949") ? "EUC-KR" : legacy_cs; }

static void disk_stamp(Doc *d)
{
	struct stat st;
	d->disk_ok = d->path[0] && !stat(d->path, &st);
	if (!d->disk_ok) return;
	d->disk_mt = st.st_mtim;
	d->disk_size = st.st_size;
	d->disk_ino = st.st_ino;
}

/* keep：重新載入（別的程式改了檔案）時保留復原紀錄與游標，由呼叫的人自己放游標 */
static int load_file(Doc *d, const char *p, int keep)
{
	struct stat st;
	if (!stat(p, &st) && S_ISDIR(st.st_mode)) { errno = EISDIR; return -1; }
	FILE *f = fopen(p, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	if (n < 0 || n > 64 * 1024 * 1024) { fclose(f); errno = EFBIG; return -1; }
	char *t = malloc((size_t)n + 1);
	size_t got = fread(t, 1, (size_t)n, f);
	fclose(f);
	t[got] = 0;
	d->big5 = 0;
	char *text = t;
	size_t tn = got;
	if (tn >= 3 && !memcmp(text, "\xef\xbb\xbf", 3)) { text += 3; tn -= 3; }   /* 拿掉 BOM */
	char *conv = NULL;
	if (!valid_utf8((unsigned char *)text, tn)) {
		size_t cn;
		conv = convert(legacy_cs, "UTF-8", text, tn, &cn);
		if (conv) { text = conv; tn = cn; d->big5 = 1; }
	}
	d->big = tn > 4u * 1024 * 1024;
	doc_set_text(d, text, tn);
	free(t);
	free(conv);
	if (p != d->path) snprintf(d->path, sizeof d->path, "%s", p);   /* 同一個緩衝區重疊的 snprintf 在 FORTIFY_SOURCE 下會變成空字串 */
	d->modified = 0;
	disk_stamp(d);
	if (keep) return 0;
	d->cy = d->cx = 0; d->ay = -1; d->scroll_row = 0; d->scroll_x = 0;
	for (int i = 0; i < d->nundo; i++) snap_free(&d->undo[i]);
	for (int i = 0; i < d->nredo; i++) snap_free(&d->redo[i]);
	d->nundo = d->nredo = 0;
	d->undo_bytes = 0;
	return 0;
}

static int save_lost_big5;   /* 剛才存檔時有 Big5 沒有的字，改存成 UTF-8 了 */

static int save_file(Doc *d, const char *p)
{
	size_t n;
	char *t = doc_text(d, &n);
	if (d->crlf) {   /* 原本是 Windows 換行就照樣存 */
		size_t lines = 0;
		for (size_t i = 0; i < n; i++) lines += t[i] == '\n';
		char *c = malloc(n + lines + 1), *o = c;
		for (size_t i = 0; i < n; i++) { if (t[i] == '\n') *o++ = '\r'; *o++ = t[i]; }
		*o = 0;
		free(t);
		t = c;
		n += lines;
	}
	save_lost_big5 = 0;
	if (d->big5) {
		size_t cn;
		char *c = convert("UTF-8", legacy_cs, t, n, &cn);
		if (c) { free(t); t = c; n = cn; }
		else { d->big5 = 0; save_lost_big5 = 1; }
	}
	char tmp[PATH_MAX + 16];
	snprintf(tmp, sizeof tmp, "%s.pomelo-edit~", p);
	struct stat st;
	mode_t m = stat(p, &st) ? 0644 : st.st_mode & 07777;
	FILE *f = fopen(tmp, "wb");
	if (!f) { free(t); return -1; }
	int ok = fwrite(t, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
	fclose(f);
	free(t);
	if (!ok || chmod(tmp, m) || rename(tmp, p)) { int e = errno; unlink(tmp); errno = e; return -1; }
	if (p != d->path) snprintf(d->path, sizeof d->path, "%s", p);   /* 同一個緩衝區重疊的 snprintf 在 FORTIFY_SOURCE 下會變成空字串 */
	d->modified = 0;
	disk_stamp(d);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 版面：每個邏輯行切成好幾個顯示列（自動換行）                          */
/* ------------------------------------------------------------------ */

static double fsize = 14, lh, textw_avail;
static int wrap = 1, show_numbers = 1, mono;
static cairo_t *mcr;   /* 量文字寬度用 */
static char mono_family[128] = "Noto Sans Mono CJK TC";

static void efont(cairo_t *cr, double size, int bold)
{
	cairo_select_font_face(cr, mono ? mono_family : pm_family, CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

/* tab 當 4 個空白 */
static void expand_tabs(const char *s, int n, char *out, size_t cap)
{
	size_t k = 0;
	for (int i = 0; i < n && k + 5 < cap; i++) {
		if (s[i] == '\t') { memcpy(out + k, "    ", 4); k += 4; } else out[k++] = s[i];
	}
	out[k] = 0;
}

static double textw(const char *s, int n)
{
	static char out[40000];
	if (n > 8000) n = 8000;
	expand_tabs(s, n, out, sizeof out);
	efont(mcr, fsize, 0);
	return pm_textw(mcr, out);
}

static void add_row(Doc *d, int line, int start, int end)
{
	if (d->nrows == d->rcap) { d->rcap = MAX(d->rcap * 2, 256); d->rows = realloc(d->rows, (size_t)d->rcap * sizeof *d->rows); }
	d->rows[d->nrows++] = (Row){ line, start, end };
}

static void layout(Doc *d)
{
	d->nrows = 0;
	for (int y = 0; y < d->nlines; y++) {
		Line *l = &d->lines[y];
		int start = 0;
		if (!l->n || !wrap || d->big) { add_row(d, y, 0, l->n); continue; }
		while (start < l->n) {
			/* 從 start 開始，找出放得下的最長一段 */
			int lo = start, hi = l->n, fit = start;
			if (textw(l->s + start, l->n - start) <= textw_avail) { add_row(d, y, start, l->n); break; }
			while (lo < hi) {   /* 二分搜尋放得下的長度 */
				int mid = (lo + hi + 1) / 2, m = mid;
				while (m > start && m < l->n && ((unsigned char)l->s[m] & 0xc0) == 0x80) m--;
				if (m <= start) { lo = mid; continue; }
				if (textw(l->s + start, m - start) <= textw_avail) { fit = m; lo = mid; } else hi = mid - 1;
			}
			if (fit <= start) fit = next_char(l->s, l->n, start);
			/* 英文盡量在空白斷行 */
			int brk = fit;
			if ((unsigned char)l->s[fit] < 0x80 && (unsigned char)l->s[fit - 1] < 0x80) {
				int sp = fit;
				while (sp > start && l->s[sp] != ' ') sp--;
				if (sp > start + (fit - start) / 3) brk = sp + 1;
			}
			add_row(d, y, start, brk);
			start = brk;
		}
	}
	d->layout_dirty = 0;
}

static void layout_all(void) { for (int i = 0; i < ndocs; i++) docs[i]->layout_dirty = 1; }

static int row_of(Doc *d, int y, int x)
{
	/* 二分搜尋第 y 行的第一列 */
	int lo = 0, hi = d->nrows - 1;
	while (lo < hi) { int mid = (lo + hi) / 2; if (d->rows[mid].line < y) lo = mid + 1; else hi = mid; }
	for (int r = lo; r < d->nrows && d->rows[r].line == y; r++)
		if (x >= d->rows[r].start && (x < d->rows[r].end || (x == d->rows[r].end && (r + 1 == d->nrows || d->rows[r + 1].line != y))))
			return r;
	return lo;
}

/* ------------------------------------------------------------------ */
/* 尋找                                                                */
/* ------------------------------------------------------------------ */

static char find_s[512], repl_s[512];
static int match_case;

/* 在 hay（長度 n）裡從 from 開始找；回傳位置或 -1 */
static int find_in(const char *hay, int n, int from, const char *needle)
{
	int m = (int)strlen(needle);
	if (!m || m > n) return -1;
	for (int i = from; i + m <= n; i++) {
		if (match_case ? !memcmp(hay + i, needle, (size_t)m) : !strncasecmp(hay + i, needle, (size_t)m)) return i;
		while (i + 1 < n && ((unsigned char)hay[i + 1] & 0xc0) == 0x80) i++;
	}
	return -1;
}

static int find_rev(const char *hay, int upto, const char *needle)
{
	int m = (int)strlen(needle), best = -1;
	for (int i = 0; (i = find_in(hay, upto, i, needle)) >= 0; i += MAX(1, m)) best = i;
	(void)m;
	return best;
}

/* 找下一個（dir 1）或上一個（dir -1）；找到就選取，回傳 1 */
static int find_step(Doc *d, int dir)
{
	if (!find_s[0]) return 0;
	int m = (int)strlen(find_s);
	int y = d->cy, x = d->cx;
	if (has_sel(d)) {
		int y1, x1, y2, x2;
		sel_bounds(d, &y1, &x1, &y2, &x2);
		if (dir > 0) { y = y2; x = x2; } else { y = y1; x = x1; }
	}
	for (int pass = 0; pass <= d->nlines; pass++) {
		Line *l = &d->lines[y];
		int hit = dir > 0 ? find_in(l->s, l->n, x, find_s) : find_rev(l->s, x, find_s);
		if (hit >= 0) {
			d->ay = y; d->ax = hit;
			d->cy = y; d->cx = hit + m;
			d->goal_x = -1;
			return 1;
		}
		if (dir > 0) { y = (y + 1) % d->nlines; x = 0; }
		else { y = (y - 1 + d->nlines) % d->nlines; x = d->lines[y].n; }
	}
	return 0;
}

/* 符合的總數、目前是第幾個（快取：文件與搜尋字串沒變就不重算） */
static void match_stats(Doc *d, int *total, int *index)
{
	static Doc *cd;
	static unsigned cv;
	static char cs[512];
	static int ccase, ctotal;
	if (cd != d || cv != d->version || strcmp(cs, find_s) || ccase != match_case) {
		ctotal = 0;
		int m = (int)strlen(find_s);
		for (int y = 0; y < d->nlines && m && ctotal < 99999; y++)
			for (int i = 0; (i = find_in(d->lines[y].s, d->lines[y].n, i, find_s)) >= 0; i += m) ctotal++;
		cd = d; cv = d->version; ccase = match_case;
		snprintf(cs, sizeof cs, "%s", find_s);
	}
	*total = ctotal;
	*index = 0;
	if (!has_sel(d) || !find_s[0]) return;
	int y1, x1, y2, x2;
	sel_bounds(d, &y1, &x1, &y2, &x2);
	int m = (int)strlen(find_s);
	if (y1 != y2 || x2 - x1 != m || find_in(d->lines[y1].s, d->lines[y1].n, x1, find_s) != x1) return;
	int k = 0;
	for (int y = 0; y <= y1; y++)
		for (int i = 0; (i = find_in(d->lines[y].s, d->lines[y].n, i, find_s)) >= 0 && (y < y1 || i <= x1); i += m) k++;
	*index = k;
}

/* 目前選取的就是要找的字：換掉它 */
static int replace_one(Doc *d)
{
	if (!find_s[0] || !has_sel(d)) return 0;
	int y1, x1, y2, x2;
	sel_bounds(d, &y1, &x1, &y2, &x2);
	int m = (int)strlen(find_s);
	if (y1 != y2 || x2 - x1 != m || find_in(d->lines[y1].s, d->lines[y1].n, x1, find_s) != x1) return 0;
	push_undo(d, 0);
	insert_text(d, repl_s, strlen(repl_s));
	return 1;
}

static int replace_all(Doc *d)
{
	int m = (int)strlen(find_s), rl = (int)strlen(repl_s), count = 0;
	if (!m) return 0;
	for (int y = 0; y < d->nlines; y++) if (find_in(d->lines[y].s, d->lines[y].n, 0, find_s) >= 0) { count = -1; break; }
	if (!count) return 0;
	push_undo(d, 0);
	count = 0;
	for (int y = 0; y < d->nlines; y++) {
		Line *l = &d->lines[y];
		if (find_in(l->s, l->n, 0, find_s) < 0) continue;
		Line nl = { 0 };
		line_reserve(&nl, l->n + 16);
		int i = 0, hit;
		while ((hit = find_in(l->s, l->n, i, find_s)) >= 0) {
			line_reserve(&nl, nl.n + (hit - i) + rl);
			memcpy(nl.s + nl.n, l->s + i, (size_t)(hit - i)); nl.n += hit - i;
			memcpy(nl.s + nl.n, repl_s, (size_t)rl); nl.n += rl;
			i = hit + m;
			count++;
		}
		line_reserve(&nl, nl.n + (l->n - i));
		memcpy(nl.s + nl.n, l->s + i, (size_t)(l->n - i)); nl.n += l->n - i;
		nl.s[nl.n] = 0;
		free(l->s);
		*l = nl;
	}
	d->cx = MIN(d->cx, d->lines[d->cy].n);
	d->ay = -1;
	touched(d);
	return count;
}

/* ------------------------------------------------------------------ */
/* 左邊的檔案清單（資料夾樹）                                            */
/* ------------------------------------------------------------------ */

typedef struct { char path[PATH_MAX]; char name[256]; int dir, depth; } Node;
static Node *nodes;
static int nnodes, ncap_nodes;
static char side_root[PATH_MAX];
static char **expanded;
static int nexpanded;
static int show_side = 1, side_w = 230, side_scroll;

static int is_expanded(const char *p) { for (int i = 0; i < nexpanded; i++) if (!strcmp(expanded[i], p)) return 1; return 0; }

static void set_expanded(const char *p, int on)
{
	for (int i = 0; i < nexpanded; i++)
		if (!strcmp(expanded[i], p)) { if (!on) { free(expanded[i]); expanded[i] = expanded[--nexpanded]; } return; }
	if (on) { expanded = realloc(expanded, (size_t)(nexpanded + 1) * sizeof *expanded); expanded[nexpanded++] = strdup(p); }
}

typedef struct { char name[256]; int dir; } Ent;

static int cmp_ent(const void *a, const void *b)
{
	const Ent *x = a, *y = b;
	if (x->dir != y->dir) return y->dir - x->dir;   /* 資料夾在前面 */
	char p[256], q[256];
	size_t i;
	for (i = 0; x->name[i] && i < 255; i++) p[i] = (char)tolower((unsigned char)x->name[i]);
	p[i] = 0;
	for (i = 0; y->name[i] && i < 255; i++) q[i] = (char)tolower((unsigned char)y->name[i]);
	q[i] = 0;
	return strverscmp(p, q);
}

static void add_nodes(const char *dir, int depth)
{
	if (depth > 12 || nnodes > 20000) return;
	DIR *dp = opendir(dir);
	if (!dp) return;
	Ent *v = NULL;
	int n = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(dp)) && n < 5000) {
		if (e->d_name[0] == '.') continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", strcmp(dir, "/") ? dir : "", e->d_name);
		struct stat st;
		if (stat(p, &st)) continue;
		if (n == cap) { cap = MAX(64, cap * 2); v = realloc(v, (size_t)cap * sizeof *v); }
		snprintf(v[n].name, sizeof v[0].name, "%s", e->d_name);
		v[n].dir = S_ISDIR(st.st_mode);
		n++;
	}
	closedir(dp);
	qsort(v, (size_t)n, sizeof *v, cmp_ent);
	for (int i = 0; i < n && nnodes <= 20000; i++) {
		if (nnodes == ncap_nodes) { ncap_nodes = MAX(256, ncap_nodes * 2); nodes = realloc(nodes, (size_t)ncap_nodes * sizeof *nodes); }
		Node *nd = &nodes[nnodes++];
		snprintf(nd->path, sizeof nd->path, "%s/%s", strcmp(dir, "/") ? dir : "", v[i].name);
		snprintf(nd->name, sizeof nd->name, "%s", v[i].name);
		nd->dir = v[i].dir;
		nd->depth = depth;
		if (nd->dir && is_expanded(nd->path)) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s", nd->path); add_nodes(p, depth + 1); }
	}
	free(v);
}

static void build_nodes(void)
{
	nnodes = 0;
	if (side_root[0]) add_nodes(side_root, 0);
}

static void set_root(const char *dir)
{
	char *r = realpath(dir, NULL);
	snprintf(side_root, sizeof side_root, "%s", r ? r : dir);
	free(r);
	side_scroll = 0;
	build_nodes();
}

/* ------------------------------------------------------------------ */
/* 視窗與畫面                                                          */
/* ------------------------------------------------------------------ */

static Window win;
static PmWl *wl;   /* 原生 Wayland 的視窗（NULL＝X11） */
static char preedit[512];   /* 原生：輸入法還沒確定的字（畫在游標處） */
static int W = 960, H = 640, focused = 1;
static XIM xim;
static XIC xic;
static Atom a_delete, a_utf8, a_targets, a_clipboard, a_paste, a_netname, a_owner, a_open, a_active, a_docs;
static char *clipbuf;
static int dragging, clicks, side_drag;
static Time last_click;
static int last_cx = -1, last_cy = -1;
static int hover_menu = -1, hover_tab = -1, hover_close = -1, hover_side = -1, hover_bar = -1;
static int new_window;
static char cwd[PATH_MAX];

/* 上方的列：尋找、取代、輸入路徑、跳到第幾行、還沒儲存的確認 */
enum { M_NONE, M_FIND, M_REPLACE, M_OPEN, M_SAVEAS, M_FOLDER, M_GOTO, M_NEWFILE, M_CONFIRM };
static int mode, bar_focus;           /* bar_focus：鍵盤在列上的輸入框（否則在文字上） */
static int field;                     /* 取代模式：0 尋找框、1 取代框 */
static char entry[PATH_MAX];          /* 開啟、另存、資料夾、跳行用的輸入框 */
static int entry_selall;              /* 輸入框重開時舊字全選：打字就取代 */
static char message[400];
static long message_until;
static int find_fail;
/* 確認對話：要關的分頁、按完之後要做什麼 */
enum { AFTER_NONE, AFTER_CLOSE, AFTER_QUIT };
static int confirm_doc = -1, after_save = AFTER_NONE;

/* 各區塊的位置（畫的時候記下來，點滑鼠時用） */
static Rect menu_r[3], tab_r[MAX_DOCS], tab_close_r[MAX_DOCS], newtab_r, side_r, text_r, bar_btn[8];
static int nbar_btn;

static unsigned col_bg, col_text, col_dim, col_bar, col_line, col_side, col_hl, col_gutter;

static void colors(void)
{
	col_bg = pm_dark ? 0x1e1e20 : 0xffffff;
	col_text = pm_dark ? 0xf2f2f2 : 0x1d1d1f;
	col_dim = pm_dark ? 0x98989d : 0x8e8e93;
	col_bar = pm_dark ? 0x2c2c2e : 0xf2f2f7;
	col_line = pm_dark ? 0x3a3a3c : 0xd8d8dd;
	col_side = pm_dark ? 0x252527 : 0xf7f7f9;
	col_hl = pm_dark ? 0x5a4a10 : 0xfff1a8;
	col_gutter = pm_dark ? 0x6e6e73 : 0xaeaeb2;
}

static void say(const char *m) { snprintf(message, sizeof message, "%s", m); message_until = pm_now_ms() + 3500; }

static void doc_title(Doc *d, char *out, size_t n)
{
	if (d->path[0]) snprintf(out, n, "%s", base_of(d->path));
	else if (d->untitled > 1) snprintf(out, n, _("未命名 %d"), d->untitled);
	else snprintf(out, n, "%s", _("未命名"));
}

/* 原生 Wayland 沒有視窗屬性可放：開著的檔案寫在 $XDG_RUNTIME_DIR/pomelo-edit-docs.<PID>（小柚讀；程式結束時刪掉） */
static char docs_file[PATH_MAX];
static void write_docs_file(const char *s)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt) return;
	if (!docs_file[0]) snprintf(docs_file, sizeof docs_file, "%s/pomelo-edit-docs.%d", rt, (int)getpid());
	FILE *f = fopen(docs_file, "w");
	if (!f) return;
	fputs(s, f);
	fclose(f);
}

static void set_title(void)
{
	char t[PATH_MAX + 64], dt[300];
	doc_title(D, dt, sizeof dt);
	snprintf(t, sizeof t, "%s%s — %s", D->modified ? "• " : "", dt, _("文字編輯器"));
	if (wl) pmwl_set_title(wl, t);
	else {
		XStoreName(dpy, win, t);
		XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (const unsigned char *)t, (int)strlen(t));
	}
	/* 開著的檔案（小柚用來找「這個檔案」、避免蓋掉還沒儲存的修改）：每行「旗標<Tab>路徑」，a = 目前的分頁、m = 還沒儲存 */
	static char *last_docs;
	size_t len = 1;
	for (int i = 0; i < ndocs; i++) len += strlen(docs[i]->path) + 5;
	char *s = malloc(len), *o = s;
	for (int i = 0; i < ndocs; i++)
		if (docs[i]->path[0])
			o += sprintf(o, "%s%s\t%s\n", i == cur ? "a" : "", docs[i]->modified ? "m" : i == cur ? "" : "-", docs[i]->path);
	*o = 0;
	if (last_docs && !strcmp(last_docs, s)) { free(s); return; }
	if (wl) write_docs_file(s);
	else XChangeProperty(dpy, win, a_docs, a_utf8, 8, PropModeReplace, (const unsigned char *)s, (int)(o - s));
	free(last_docs);
	last_docs = s;
}

static int top_bars(void) { return mode == M_NONE ? 0 : mode == M_REPLACE ? 2 * BAR_H : BAR_H; }
static int text_top(void) { return MENU_H + TAB_H + top_bars(); }
static int side_width(void) { return show_side ? side_w : 0; }
static double gutter_w(void)
{
	if (!show_numbers) return 0;
	char b[16];
	snprintf(b, sizeof b, "%d", MAX(99, D->nlines));
	efont(mcr, fsize * 0.85, 0);
	return pm_textw(mcr, b) + 18;
}
static double text_x0(void) { return side_width() + gutter_w() + PAD; }
static int visible_rows(void) { return MAX(1, (int)((H - STATUS_H - text_top() - 2 * 6) / lh)); }

static void update_avail(void) { textw_avail = W - side_width() - gutter_w() - 2 * PAD - 10; layout_all(); }

static void ensure_visible(Doc *d)
{
	if (d->layout_dirty) layout(d);
	int r = row_of(d, d->cy, d->cx), vis = visible_rows();
	if (r < d->scroll_row) d->scroll_row = r;
	if (r >= d->scroll_row + vis) d->scroll_row = r - vis + 1;
	if (d->scroll_row > MAX(0, d->nrows - vis)) d->scroll_row = MAX(0, d->nrows - vis);
	if (d->scroll_row < 0) d->scroll_row = 0;
	if (!wrap) {
		Row *rw = &d->rows[r];
		double cxp = textw(d->lines[d->cy].s + rw->start, d->cx - rw->start), area = textw_avail;
		if (cxp - d->scroll_x > area - 20) d->scroll_x = cxp - area + 40;
		if (cxp < d->scroll_x + 10) d->scroll_x = MAX(0, cxp - 40);
	} else if (!d->big) d->scroll_x = 0;
	if (d->big) {
		Row *rw = &d->rows[r];
		double cxp = textw(d->lines[d->cy].s + rw->start, d->cx - rw->start), area = textw_avail;
		if (cxp - d->scroll_x > area - 20) d->scroll_x = cxp - area + 40;
		if (cxp < d->scroll_x + 10) d->scroll_x = MAX(0, cxp - 40);
	}
}

/* 一個輸入框：label 在左邊，focus 時有游標 */
static void draw_field(cairo_t *cr, double x, double y, double w, const char *text, const char *hint, int focus, int selall, unsigned border)
{
	pm_rrect(cr, x, y + 7, w, BAR_H - 14, 6, 6);
	pm_rgb(cr, pm_dark ? 0x1c1c1e : 0xffffff);
	cairo_fill_preserve(cr);
	pm_rgb(cr, focus ? T->sel : border);
	cairo_set_line_width(cr, focus ? 1.5 : 1);
	cairo_stroke(cr);
	cairo_save(cr);
	cairo_rectangle(cr, x + 4, y, w - 8, BAR_H);
	cairo_clip(cr);
	pm_font(cr, 13, 0);
	double tw = pm_textw(cr, text), shift = MAX(0, tw - (w - 20));
	if (!text[0]) { pm_rgb(cr, col_dim); pm_text(cr, x + 10, y + BAR_H / 2.0, hint); }
	if (selall && text[0] && focus) {
		pm_rgba(cr, T->sel, 0.3);
		pm_rrect(cr, x + 7 - shift, y + 11, tw + 4, BAR_H - 22, 3, 3);
		cairo_fill(cr);
	}
	pm_rgb(cr, col_text);
	pm_text(cr, x + 8 - shift, y + BAR_H / 2.0, text);
	if (focus) {
		pm_rgb(cr, T->sel);
		cairo_rectangle(cr, x + 9 + tw - shift, y + 12, 1.5, BAR_H - 24);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

/* 列上的小按鈕（文字或圖示）；回傳右邊界 */
static double bar_button(cairo_t *cr, double x, double y, const char *label, int id, int on, int primary)
{
	pm_font(cr, 12.5, 0);
	double w = pm_textw(cr, label) + 22;
	pm_rrect(cr, x, y + 8, w, BAR_H - 16, 6, 6);
	if (primary) pm_rgb(cr, T->sel);
	else if (on) pm_rgba(cr, T->sel, 0.2);
	else pm_rgb(cr, pm_dark ? 0x3a3a3c : 0xe5e5ea);
	cairo_fill(cr);
	if (hover_bar == id) { pm_rrect(cr, x, y + 8, w, BAR_H - 16, 6, 6); pm_rgba(cr, primary ? 0xffffff : 0x000000, primary ? 0.15 : 0.06); cairo_fill(cr); }
	pm_rgb(cr, primary ? 0xffffff : on ? T->sel : col_text);
	pm_text(cr, x + 11, y + BAR_H / 2.0, label);
	if (nbar_btn < (int)LEN(bar_btn)) bar_btn[nbar_btn++] = (Rect){ (int)x, (int)y + 8, (int)w, BAR_H - 16 };
	(void)id;
	return x + w;
}

enum { BB_PREV, BB_NEXT, BB_CASE, BB_CLOSE, BB_REPL, BB_REPLALL, BB_SAVE, BB_DISCARD, BB_CANCEL };
static int bar_ids[8];

static void draw_bars(cairo_t *cr, double x0, double y)
{
	nbar_btn = 0;
	if (mode == M_NONE) return;
	double w = W - x0;
	int rowsn = mode == M_REPLACE ? 2 : 1;
	pm_rgb(cr, col_bar);
	cairo_rectangle(cr, x0, y, w, BAR_H * rowsn);
	cairo_fill(cr);
	pm_rgb(cr, col_line);
	cairo_rectangle(cr, x0, y + BAR_H * rowsn - 1, w, 1);
	cairo_fill(cr);
	pm_font(cr, 13, 0);
	if (mode == M_CONFIRM) {
		char t[PATH_MAX + 100], dt[300];
		doc_title(docs[confirm_doc], dt, sizeof dt);
		snprintf(t, sizeof t, _("「%s」還沒儲存，要儲存嗎？"), dt);
		pm_rgb(cr, col_text);
		pm_text(cr, x0 + 14, y + BAR_H / 2.0, t);
		double bx = W - 14;
		const char *lb[3] = { _("取消"), _("不儲存"), _("儲存") };
		int ids[3] = { BB_CANCEL, BB_DISCARD, BB_SAVE };
		double xs[3];
		pm_font(cr, 12.5, 0);
		for (int i = 0; i < 3; i++) { bx -= pm_textw(cr, lb[i]) + 22; xs[i] = bx; bx -= 8; }
		for (int i = 2; i >= 0; i--) { bar_ids[nbar_btn] = ids[i]; bar_button(cr, xs[i], y, lb[i], ids[i], 0, i == 2); }
		return;
	}
	const char *label = mode == M_FIND || mode == M_REPLACE ? _("尋找") : mode == M_OPEN ? _("開啟") : mode == M_SAVEAS ? _("另存新檔") :
	                    mode == M_FOLDER ? _("開啟資料夾") : mode == M_NEWFILE ? _("新增檔案") : _("跳到第幾行");
	double lw = 0;
	pm_font(cr, 13, 0);
	lw = MAX(pm_textw(cr, label), mode == M_REPLACE ? pm_textw(cr, _("取代為")) : 0);
	pm_rgb(cr, col_text);
	pm_text(cr, x0 + 14, y + BAR_H / 2.0, label);
	double fx = x0 + 14 + lw + 12;
	if (mode == M_FIND || mode == M_REPLACE) {
		/* 尋找框＋「3／10」＋↑↓＋Aa＋× */
		int total, idx;
		match_stats(D, &total, &idx);
		char cnt[64] = "";
		if (find_s[0]) {
			if (!total) snprintf(cnt, sizeof cnt, "%s", _("找不到"));
			else if (idx) snprintf(cnt, sizeof cnt, "%d / %d", idx, total);
			else snprintf(cnt, sizeof cnt, _("%d 個"), total);
		}
		double right = W - 14;
		pm_font(cr, 12.5, 0);
		double bw_close = pm_textw(cr, "×") + 22, bw_case = pm_textw(cr, "Aa") + 22, bw_ud = pm_textw(cr, "↓") + 22;
		double bx = right - bw_close - 6 - bw_case - 6 - bw_ud * 2 - 4;
		pm_font(cr, 12, 0);
		double cw = pm_textw(cr, cnt);
		double fw = MAX(120, bx - 12 - cw - 12 - fx);
		draw_field(cr, fx, y, fw, find_s, _("要找的文字"), bar_focus && field == 0, entry_selall && field == 0, (find_fail && find_s[0]) ? 0xff453a : col_line);
		pm_font(cr, 12, 0);
		pm_rgb(cr, total || !find_s[0] ? col_dim : 0xff453a);
		pm_text(cr, fx + fw + 10, y + BAR_H / 2.0, cnt);
		bar_ids[nbar_btn] = BB_PREV; bx = bar_button(cr, bx, y, "↑", BB_PREV, 0, 0) + 4;
		bar_ids[nbar_btn] = BB_NEXT; bx = bar_button(cr, bx, y, "↓", BB_NEXT, 0, 0) + 6;
		bar_ids[nbar_btn] = BB_CASE; bx = bar_button(cr, bx, y, "Aa", BB_CASE, match_case, 0) + 6;
		bar_ids[nbar_btn] = BB_CLOSE; bar_button(cr, bx, y, "×", BB_CLOSE, 0, 0);
		if (mode == M_REPLACE) {
			double y2 = y + BAR_H;
			pm_font(cr, 13, 0);
			pm_rgb(cr, col_text);
			pm_text(cr, x0 + 14, y2 + BAR_H / 2.0, _("取代為"));
			pm_font(cr, 12.5, 0);
			double b1 = pm_textw(cr, _("取代")) + 22, b2 = pm_textw(cr, _("全部取代")) + 22;
			double rx = W - 14 - b2 - 6 - b1;
			draw_field(cr, fx, y2, MAX(120, rx - 12 - fx), repl_s, _("換成的文字（可以留空）"), bar_focus && field == 1, entry_selall && field == 1, col_line);
			bar_ids[nbar_btn] = BB_REPL; rx = bar_button(cr, rx, y2, _("取代"), BB_REPL, 0, 0) + 6;
			bar_ids[nbar_btn] = BB_REPLALL; bar_button(cr, rx, y2, _("全部取代"), BB_REPLALL, 0, 1);
		}
		return;
	}
	const char *hint = mode == M_GOTO ? _("行號，例如 120") : _("檔案路徑（Tab 可以補完檔名）");
	draw_field(cr, fx, y, W - 14 - 30 - 8 - fx, entry, hint, 1, entry_selall, col_line);
	bar_ids[nbar_btn] = BB_CLOSE;
	bar_button(cr, W - 14 - (pm_textw(cr, "×") + 22), y, "×", BB_CLOSE, 0, 0);
}

static void draw_menubar(cairo_t *cr)
{
	pm_rgb(cr, col_bar);
	cairo_rectangle(cr, 0, 0, W, MENU_H);
	cairo_fill(cr);
	const char *names[3] = { _("{選單}檔案"), _("編輯"), _("檢視") };
	pm_font(cr, 13, 0);
	double x = 8;
	for (int i = 0; i < 3; i++) {
		double w = pm_textw(cr, names[i]) + 22;
		menu_r[i] = (Rect){ (int)x, 3, (int)w, MENU_H - 6 };
		if (hover_menu == i) {
			pm_rrect(cr, x, 3, w, MENU_H - 6, 5, 5);
			pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.08);
			cairo_fill(cr);
		}
		pm_rgb(cr, col_text);
		pm_text(cr, x + 11, MENU_H / 2.0, names[i]);
		x += w;
	}
}

static void draw_tabs(cairo_t *cr)
{
	double y = MENU_H;
	pm_rgb(cr, col_bar);
	cairo_rectangle(cr, 0, y, W, TAB_H);
	cairo_fill(cr);
	pm_rgb(cr, col_line);
	cairo_rectangle(cr, 0, y + TAB_H - 1, W, 1);
	cairo_fill(cr);
	double avail = W - 16 - 34, tw = MIN(220.0, MAX(90.0, avail / MAX(1, ndocs)));
	/* 放不下：讓目前的分頁看得到 */
	double off = 0;
	if (tw * ndocs > avail) { double need = (cur + 1) * tw; if (need > avail) off = need - avail; }
	cairo_save(cr);
	cairo_rectangle(cr, 0, y, W - 34, TAB_H);
	cairo_clip(cr);
	for (int i = 0; i < ndocs; i++) {
		double x = 8 + i * tw - off;
		tab_r[i] = (Rect){ (int)x, (int)y + 4, (int)tw - 4, TAB_H - 4 };
		int active = i == cur;
		pm_rrect(cr, x, y + 4, tw - 4, TAB_H - 4, 7, 0);
		if (active) pm_rgb(cr, col_bg);
		else if (hover_tab == i) pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.05);
		else pm_rgba(cr, 0, 0);
		cairo_fill(cr);
		if (active) {
			pm_rgb(cr, T->sel);
			cairo_rectangle(cr, x + 6, y + 4, tw - 16, 2);
			cairo_fill(cr);
		}
		char t[300], b[300];
		doc_title(docs[i], t, sizeof t);
		pm_font(cr, 12.5, active);
		pm_ellipsize(cr, t, tw - 44, b, sizeof b);
		pm_rgb(cr, active ? col_text : col_dim);
		pm_text(cr, x + 12, y + 4 + (TAB_H - 4) / 2.0, b);
		/* 右邊：改過是圓點，滑鼠移上去變成 × */
		double cx = x + tw - 20, cyy = y + 4 + (TAB_H - 4) / 2.0;
		tab_close_r[i] = (Rect){ (int)cx - 9, (int)cyy - 9, 18, 18 };
		if (hover_tab == i || active) {
			if (hover_close == i) { cairo_arc(cr, cx, cyy, 9, 0, 2 * M_PI); pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.1); cairo_fill(cr); }
			if (docs[i]->modified && hover_close != i) { cairo_arc(cr, cx, cyy, 4, 0, 2 * M_PI); pm_rgb(cr, col_dim); cairo_fill(cr); }
			else {
				pm_rgb(cr, col_dim);
				cairo_set_line_width(cr, 1.4);
				cairo_move_to(cr, cx - 3.5, cyy - 3.5); cairo_line_to(cr, cx + 3.5, cyy + 3.5);
				cairo_move_to(cr, cx + 3.5, cyy - 3.5); cairo_line_to(cr, cx - 3.5, cyy + 3.5);
				cairo_stroke(cr);
			}
		} else if (docs[i]->modified) { cairo_arc(cr, cx, cyy, 4, 0, 2 * M_PI); pm_rgb(cr, col_dim); cairo_fill(cr); }
	}
	cairo_restore(cr);
	/* 新分頁 */
	newtab_r = (Rect){ W - 32, (int)y + 5, 26, TAB_H - 10 };
	if (hover_tab == -2) { pm_rrect(cr, newtab_r.x, newtab_r.y, 26, newtab_r.h, 6, 6); pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.08); cairo_fill(cr); }
	pm_rgb(cr, col_dim);
	cairo_set_line_width(cr, 1.6);
	double px = W - 19, py = y + TAB_H / 2.0;
	cairo_move_to(cr, px - 5.5, py); cairo_line_to(cr, px + 5.5, py);
	cairo_move_to(cr, px, py - 5.5); cairo_line_to(cr, px, py + 5.5);
	cairo_stroke(cr);
}

static void draw_side(cairo_t *cr)
{
	if (!show_side) { side_r = (Rect){ 0, 0, 0, 0 }; return; }
	double y0 = MENU_H + TAB_H, h = H - STATUS_H - y0;
	side_r = (Rect){ 0, (int)y0, side_w, (int)h };
	pm_rgb(cr, col_side);
	cairo_rectangle(cr, 0, y0, side_w, h);
	cairo_fill(cr);
	pm_rgb(cr, col_line);
	cairo_rectangle(cr, side_w - 1, y0, 1, h);
	cairo_fill(cr);
	/* 標題：資料夾名稱 */
	pm_font(cr, 11.5, 1);
	pm_rgb(cr, col_dim);
	char t[300], b[300];
	snprintf(t, sizeof t, "%s", !strcmp(side_root, "/") ? "/" : base_of(side_root));
	pm_ellipsize(cr, t, side_w - 24, b, sizeof b);
	pm_text(cr, 12, y0 + 16, b);
	double ly = y0 + 30, lh2 = h - 30;
	int vis = (int)(lh2 / SIDE_ROW);
	side_scroll = CLAMP(side_scroll, 0, MAX(0, nnodes - vis));
	cairo_save(cr);
	cairo_rectangle(cr, 0, ly, side_w - 1, lh2);
	cairo_clip(cr);
	for (int r = 0; r <= vis && side_scroll + r < nnodes; r++) {
		int i = side_scroll + r;
		Node *nd = &nodes[i];
		double ry = ly + r * SIDE_ROW, x = 10 + nd->depth * 14;
		int open = !nd->dir && !strcmp(nd->path, D->path);
		if (open || hover_side == i) {
			pm_rrect(cr, 4, ry + 1, side_w - 9, SIDE_ROW - 2, 5, 5);
			if (open) pm_rgba(cr, T->sel, pm_dark ? 0.35 : 0.16); else pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.05);
			cairo_fill(cr);
		}
		double cy = ry + SIDE_ROW / 2.0;
		pm_rgb(cr, col_dim);
		cairo_new_path(cr);
		cairo_set_line_width(cr, 1.4);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		if (nd->dir) {
			/* ▸／▾ 加一個資料夾 */
			if (is_expanded(nd->path)) { cairo_move_to(cr, x, cy - 2); cairo_line_to(cr, x + 4, cy + 2); cairo_line_to(cr, x + 8, cy - 2); }
			else { cairo_move_to(cr, x + 2, cy - 4); cairo_line_to(cr, x + 6, cy); cairo_line_to(cr, x + 2, cy + 4); }
			cairo_stroke(cr);
			pm_rgb(cr, 0x5ac8fa);
			pm_rrect(cr, x + 12, cy - 5, 14, 10, 2, 2);
			cairo_fill(cr);
			pm_rrect(cr, x + 12, cy - 7, 6, 4, 1.5, 0);
			cairo_fill(cr);
		} else {
			/* 一張紙 */
			pm_rrect(cr, x + 14, cy - 7, 10, 14, 2, 2);
			cairo_stroke(cr);
		}
		pm_font(cr, 12.5, 0);
		pm_ellipsize(cr, nd->name, side_w - (x + 32) - 8, b, sizeof b);
		pm_rgb(cr, open ? T->sel : col_text);
		pm_text(cr, x + 32, cy, b);
	}
	cairo_restore(cr);
	if (!nnodes) {
		pm_font(cr, 12, 0);
		pm_rgb(cr, col_dim);
		const char *m = _("資料夾是空的");
		pm_text(cr, (side_w - pm_textw(cr, m)) / 2, ly + 24, m);
	}
	if (nnodes > vis && vis > 0) {
		double bh = MAX(24.0, lh2 * vis / nnodes), by = ly + (lh2 - bh) * side_scroll / MAX(1, nnodes - vis);
		pm_rrect(cr, side_w - 7, by + 2, 4, bh - 4, 2, 2);
		pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.22);
		cairo_fill(cr);
	}
}

/* 一列文字裡符合尋找的地方畫底色 */
static void draw_matches(cairo_t *cr, Doc *d, Row *rw, double x, double y)
{
	if (!find_s[0] || (mode != M_FIND && mode != M_REPLACE)) return;
	Line *l = &d->lines[rw->line];
	int m = (int)strlen(find_s);
	for (int i = MAX(0, rw->start - m + 1); (i = find_in(l->s, MIN(l->n, rw->end + m - 1), i, find_s)) >= 0 && i < rw->end; i += m) {
		int a = MAX(i, rw->start), b = MIN(i + m, rw->end);
		double sx = x + textw(l->s + rw->start, a - rw->start), ex = x + textw(l->s + rw->start, b - rw->start);
		pm_rgb(cr, col_hl);
		pm_rrect(cr, sx - 1, y + 2, ex - sx + 2, lh - 4, 3, 3);
		cairo_fill(cr);
	}
}

static void draw(void)
{
	Doc *d = D;
	if (d->layout_dirty) layout(d);
	ensure_visible(d);
	Paint p;
	cairo_t *cr = wl ? pmwl_begin(wl, &W, &H) : pm_paint_begin(&p, win, W, H);
	pm_rgb(cr, col_bg);
	cairo_paint(cr);
	draw_menubar(cr);
	draw_tabs(cr);
	draw_side(cr);
	double sx0 = side_width();
	draw_bars(cr, sx0, MENU_H + TAB_H);
	double top = text_top() + 6, gw = gutter_w(), x0 = sx0 + gw + PAD;
	text_r = (Rect){ (int)sx0, text_top(), W - (int)sx0, H - STATUS_H - text_top() };
	int y1 = -1, x1 = 0, y2 = -1, x2 = 0;
	if (has_sel(d)) sel_bounds(d, &y1, &x1, &y2, &x2);
	int vis = visible_rows();
	cairo_save(cr);
	cairo_rectangle(cr, sx0, text_top(), W - sx0, H - STATUS_H - text_top());
	cairo_clip(cr);
	int crow = row_of(d, d->cy, d->cx);
	double y = top;
	for (int r = d->scroll_row; r < d->nrows && r < d->scroll_row + vis + 1; r++, y += lh) {
		Row *rw = &d->rows[r];
		Line *l = &d->lines[rw->line];
		/* 行號（一行的第一列才畫） */
		if (show_numbers && (r == 0 || d->rows[r - 1].line != rw->line)) {
			char n[16];
			snprintf(n, sizeof n, "%d", rw->line + 1);
			efont(cr, fsize * 0.85, 0);
			pm_rgb(cr, rw->line == d->cy ? col_text : col_gutter);
			pm_text(cr, sx0 + gw - 8 - pm_textw(cr, n), y + lh / 2, n);
		}
		cairo_save(cr);
		cairo_rectangle(cr, sx0 + gw + 2, text_top(), W - sx0 - gw - 2, H);
		cairo_clip(cr);
		double tx = x0 - d->scroll_x;
		if (r == crow && !has_sel(d)) {   /* 游標所在的列淡淡的底色 */
			pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.035);
			cairo_rectangle(cr, sx0 + gw, y, W, lh);
			cairo_fill(cr);
		}
		draw_matches(cr, d, rw, tx, y);
		/* 選取底色 */
		if (y1 >= 0 && rw->line >= y1 && rw->line <= y2) {
			int from = rw->line == y1 ? MAX(x1, rw->start) : rw->start;
			int to = rw->line == y2 ? MIN(x2, rw->end) : rw->end;
			if (from < to || (rw->line < y2 && rw->end == l->n)) {
				double sx = tx + textw(l->s + rw->start, from - rw->start);
				double ex = from < to ? tx + textw(l->s + rw->start, to - rw->start) : sx;
				if (rw->line < y2 && rw->end == l->n) ex += 8;   /* 換行也算選到 */
				pm_rgba(cr, T->sel, focused ? 0.3 : 0.15);
				cairo_rectangle(cr, sx, y, ex - sx, lh);
				cairo_fill(cr);
			}
		}
		/* 不自動換行時只畫看得到的部分（很長的行不會慢） */
		int s = rw->start, e = rw->end;
		static char out[40000];
		expand_tabs(l->s + s, MIN(e - s, 8000), out, sizeof out);
		efont(cr, fsize, 0);
		pm_rgb(cr, col_text);
		pm_text(cr, tx, y + lh / 2, out);
		/* 游標 */
		if (rw->line == d->cy && d->cx >= rw->start && (d->cx < rw->end || (d->cx == rw->end && (r + 1 == d->nrows || d->rows[r + 1].line != d->cy))) && focused && !bar_focus) {
			double cxp = tx + textw(l->s + rw->start, d->cx - rw->start);
			pm_rgb(cr, T->sel);
			cairo_rectangle(cr, cxp, y + 2, 1.8, lh - 4);
			cairo_fill(cr);
		}
		cairo_restore(cr);
	}
	cairo_restore(cr);
	if (show_numbers) {
		pm_rgb(cr, col_line);
		cairo_rectangle(cr, sx0 + gw, text_top(), 1, H - STATUS_H - text_top());
		cairo_fill(cr);
	}
	/* 狀態列 */
	double sy = H - STATUS_H;
	pm_rgb(cr, col_bar);
	cairo_rectangle(cr, 0, sy, W, STATUS_H);
	cairo_fill(cr);
	pm_rgb(cr, col_line);
	cairo_rectangle(cr, 0, sy, W, 1);
	cairo_fill(cr);
	pm_font(cr, 11.5, 0);
	pm_rgb(cr, col_dim);
	char st[600];
	if (message_until > pm_now_ms()) { snprintf(st, sizeof st, "%s", message); pm_rgb(cr, col_text); }
	else {
		if (d->chars_ver != d->version + 1) {
			d->chars = 0;
			for (int i = 0; i < d->nlines; i++) for (int j = 0; j < d->lines[i].n; j++) if (((unsigned char)d->lines[i].s[j] & 0xc0) != 0x80) d->chars++;
			d->chars_ver = d->version + 1;
		}
		long chars = d->chars;
		int col = 0;
		for (int j = 0; j < d->cx; j++) if (((unsigned char)d->lines[d->cy].s[j] & 0xc0) != 0x80) col++;
		snprintf(st, sizeof st, _("第 %d 行，第 %d 字　·　%ld 字，%d 行"), d->cy + 1, col + 1, chars, d->nlines);
	}
	pm_text(cr, 12, sy + STATUS_H / 2.0, st);
	char enc[64];
	snprintf(enc, sizeof enc, "%s　%s", d->big5 ? legacy_name() : "UTF-8", d->crlf ? "CRLF" : "LF");
	pm_font(cr, 11.5, 0);
	pm_rgb(cr, col_dim);
	double ew = pm_textw(cr, enc);
	pm_text(cr, W - 12 - ew, sy + STATUS_H / 2.0, enc);
	if (d->path[0]) {
		/* 家目錄底下的寫成 ~/… */
		char shown[PATH_MAX], pb[PATH_MAX];
		const char *home = getenv("HOME");
		size_t hl = home ? strlen(home) : 0;
		if (hl > 1 && !strncmp(d->path, home, hl) && d->path[hl] == '/') snprintf(shown, sizeof shown, "~%s", d->path + hl);
		else snprintf(shown, sizeof shown, "%s", d->path);
		pm_ellipsize(cr, shown, W / 2.5, pb, sizeof pb);
		pm_text(cr, W - 12 - ew - 24 - pm_textw(cr, pb), sy + STATUS_H / 2.0, pb);
	}
	/* 捲軸 */
	if (d->nrows > vis) {
		double area = H - STATUS_H - text_top(), bh = MAX(24.0, area * vis / d->nrows), by = text_top() + (area - bh) * d->scroll_row / MAX(1, d->nrows - vis);
		pm_rrect(cr, W - 8, by + 2, 5, bh - 4, 2.5, 2.5);
		pm_rgba(cr, pm_dark ? 0xffffff : 0x000000, 0.25);
		cairo_fill(cr);
	}
	if (wl) {
		/* 輸入法的選字列跟著游標；還沒確定的字（預編輯）畫在游標處 */
		int ix = 120, iy = MENU_H + TAB_H, ih = BAR_H;
		if (!bar_focus) {
			int r = row_of(d, d->cy, d->cx);
			ix = (int)(x0 - d->scroll_x + textw(d->lines[d->cy].s + d->rows[r].start, d->cx - d->rows[r].start));
			iy = (int)(top + (r - d->scroll_row) * lh);
			ih = (int)lh;
			if (preedit[0] && focused) {
				pm_font(cr, fsize, 0);
				double pw = pm_textw(cr, preedit);
				pm_rgb(cr, col_bg);
				cairo_rectangle(cr, ix, iy, pw + 2, lh);
				cairo_fill(cr);
				pm_rgb(cr, col_text);
				pm_text(cr, ix, iy + lh / 2, preedit);
				cairo_set_line_width(cr, 1);
				cairo_move_to(cr, ix, iy + lh - 3.5); cairo_line_to(cr, ix + pw, iy + lh - 3.5);
				cairo_stroke(cr);
			}
		}
		pmwl_end(wl, cr);
		pmwl_ime(wl, 1, ix, iy, ih);
		return;
	}
	pm_paint_end(&p, cr, 0, 0);
	/* 輸入法視窗跟著游標 */
	if (xic && !bar_focus) {
		int r = row_of(d, d->cy, d->cx);
		XPoint spot = { (short)(x0 - d->scroll_x + textw(d->lines[d->cy].s + d->rows[r].start, d->cx - d->rows[r].start)), (short)(top + (r - d->scroll_row + 1) * lh) };
		XVaNestedList attr = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
		XSetICValues(xic, XNPreeditAttributes, attr, NULL);
		XFree(attr);
	}
	XFlush(dpy);
}

/* ------------------------------------------------------------------ */
/* 分頁                                                                */
/* ------------------------------------------------------------------ */

static void switch_to(int i)
{
	if (i < 0 || i >= ndocs) return;
	cur = i;
	set_title();
}

static int next_untitled(void)
{
	int n = 1;
	for (int again = 1; again; ) {
		again = 0;
		for (int i = 0; i < ndocs; i++) if (!docs[i]->path[0] && docs[i]->untitled == n) { n++; again = 1; }
	}
	return n;
}

static int new_tab(void)
{
	if (ndocs >= MAX_DOCS) { say(_("分頁太多了，先關掉一些吧")); return -1; }
	Doc *d = doc_new();
	d->untitled = next_untitled();
	int at = ndocs ? cur + 1 : 0;
	memmove(docs + at + 1, docs + at, (size_t)(ndocs - at) * sizeof *docs);
	docs[at] = d;
	ndocs++;
	switch_to(at);
	return at;
}

static void abs_path(const char *in, char *out, size_t n)
{
	if (in[0] == '~' && (in[1] == '/' || !in[1])) { snprintf(out, n, "%s%s", getenv("HOME") ? getenv("HOME") : "", in + 1); return; }
	if (in[0] == '/') { snprintf(out, n, "%s", in); return; }
	snprintf(out, n, "%s/%s", cwd, in);
}

/* 打開檔案：已經開著就切過去；目前是空白的未命名分頁就直接用它 */
static void open_path(const char *in)
{
	char p[PATH_MAX];
	abs_path(in, p, sizeof p);
	char *r = realpath(p, NULL);
	if (r) { snprintf(p, sizeof p, "%s", r); free(r); }
	struct stat st;
	if (!stat(p, &st) && S_ISDIR(st.st_mode)) { set_root(p); show_side = 1; update_avail(); return; }
	for (int i = 0; i < ndocs; i++) if (!strcmp(docs[i]->path, p)) { switch_to(i); return; }
	int reuse = ndocs && doc_pristine(D);
	if (!reuse && new_tab() < 0) return;
	Doc *d = D;
	if (load_file(d, p, 0)) {
		if (errno == ENOENT) {   /* 還不存在：當成新檔案，儲存時建立 */
			snprintf(d->path, sizeof d->path, "%s", p);
			d->modified = 0;
			d->disk_ok = 0;
		} else {
			char m[PATH_MAX + 100];
			snprintf(m, sizeof m, _("無法開啟「%s」：%s"), base_of(p), strerror(errno));
			say(m);
			if (!reuse && ndocs > 1) { doc_free(d); memmove(docs + cur, docs + cur + 1, (size_t)(ndocs - cur - 1) * sizeof *docs); ndocs--; cur = MAX(0, cur - 1); }
		}
	} else if (d->big5) { char m[200]; snprintf(m, sizeof m, _("這個檔案是 %s 編碼，存檔時會照樣存成 %s"), legacy_name(), legacy_name()); say(m); }
	set_title();
}

/* 別的程式改了檔案：沒有修改過就重新載入（Ctrl+Z 可以回到之前的內容），有修改過就只提醒一次 */
static void reload_doc(Doc *d)
{
	char m[PATH_MAX + 200];
	if (d->modified) {
		disk_stamp(d);
		snprintf(m, sizeof m, _("「%s」在別的地方被修改了；這裡還有沒儲存的修改，儲存會蓋掉別處的修改"), base_of(d->path));
		say(m);
		return;
	}
	size_t on;
	char *old = doc_text(d, &on);
	int cy = d->cy, cx = d->cx, sr = d->scroll_row;
	double sx = d->scroll_x;
	d->last_edit_kind = 0;
	push_undo(d, 0);
	char path[PATH_MAX];   /* load_file 會寫 d->path：不能把它自己傳進去 */
	snprintf(path, sizeof path, "%s", d->path);
	if (load_file(d, path, 1)) { free(old); disk_stamp(d); return; }
	size_t nn;
	char *now = doc_text(d, &nn);
	d->ay = -1;
	d->layout_dirty = 1;
	if (nn > on && !memcmp(now, old, on)) {   /* 加在最後面（例如小柚加的字）：游標移到最後，讓人看得到 */
		d->cy = d->nlines - 1;
		d->cx = d->lines[d->cy].n;
		if (d == D) ensure_visible(d);
	} else {
		d->cy = MIN(cy, d->nlines - 1);
		d->cx = MIN(cx, d->lines[d->cy].n);
		while (d->cx > 0 && ((unsigned char)d->lines[d->cy].s[d->cx] & 0xc0) == 0x80) d->cx--;   /* 不要停在中文字中間 */
		d->scroll_row = sr;
		d->scroll_x = sx;
	}
	free(old);
	free(now);
	snprintf(m, sizeof m, _("「%s」在別的地方被修改，已重新載入（可以復原）"), base_of(d->path));
	say(m);
}

static int check_disk(void)
{
	int changed = 0;
	for (int i = 0; i < ndocs; i++) {
		Doc *d = docs[i];
		struct stat st;
		if (!d->path[0] || !d->disk_ok || stat(d->path, &st)) continue;   /* 被刪掉或改名了：先不管 */
		if (st.st_mtim.tv_sec == d->disk_mt.tv_sec && st.st_mtim.tv_nsec == d->disk_mt.tv_nsec && st.st_size == d->disk_size &&
		    st.st_ino == d->disk_ino)
			continue;
		reload_doc(d);
		set_title();
		changed = 1;
	}
	return changed;
}

static void remove_tab(int i)
{
	doc_free(docs[i]);
	memmove(docs + i, docs + i + 1, (size_t)(ndocs - i - 1) * sizeof *docs);
	ndocs--;
	if (!ndocs) { docs[0] = doc_new(); docs[0]->untitled = 1; ndocs = 1; }
	if (cur >= ndocs) cur = ndocs - 1;
	else if (cur > i) cur--;
	set_title();
}

static void quit_flow(void);

/* 存檔；沒有檔名就問路徑（存好之後照 after_save 繼續） */
static int save_doc(int i)
{
	Doc *d = docs[i];
	if (!d->path[0]) {
		switch_to(i);
		mode = M_SAVEAS;
		bar_focus = 1;
		entry_selall = 0;
		snprintf(entry, sizeof entry, "%s/", side_root[0] ? side_root : cwd);
		return 0;
	}
	if (save_file(d, d->path)) {
		char m[PATH_MAX + 100];
		snprintf(m, sizeof m, _("無法儲存：%s"), strerror(errno));
		say(m);
		return -1;
	}
	char m[PATH_MAX + 100];
	if (save_lost_big5) snprintf(m, sizeof m, _("已儲存「%s」（有 %s 沒有的字，改存成 UTF-8）"), base_of(d->path), legacy_name());
	else snprintf(m, sizeof m, _("已儲存「%s」"), base_of(d->path));
	say(m);
	set_title();
	if (side_root[0]) build_nodes();
	return 1;
}

/* 存好之後要做的事（關分頁、繼續結束程式） */
static void after_saved(void)
{
	int a = after_save;
	after_save = AFTER_NONE;
	if (a == AFTER_CLOSE && confirm_doc >= 0 && confirm_doc < ndocs && !docs[confirm_doc]->modified) remove_tab(confirm_doc);
	else if (a == AFTER_QUIT) quit_flow();
	confirm_doc = -1;
}

static void close_tab(int i)
{
	if (i < 0 || i >= ndocs) return;
	if (docs[i]->modified) {
		switch_to(i);
		mode = M_CONFIRM;
		bar_focus = 1;
		confirm_doc = i;
		after_save = AFTER_CLOSE;
		return;
	}
	remove_tab(i);
}

/* 結束：一個一個問還沒儲存的分頁 */
static void quit_flow(void)
{
	for (int i = 0; i < ndocs; i++)
		if (docs[i]->modified) {
			switch_to(i);
			mode = M_CONFIRM;
			bar_focus = 1;
			confirm_doc = i;
			after_save = AFTER_QUIT;
			return;
		}
	exit(0);
}

static void confirm_answer(int ans)   /* 0 取消、1 儲存、2 不儲存 */
{
	if (mode != M_CONFIRM) return;
	mode = M_NONE;
	bar_focus = 0;
	int i = confirm_doc;
	if (ans == 0 || i < 0 || i >= ndocs) { after_save = AFTER_NONE; confirm_doc = -1; return; }
	if (ans == 2) {
		docs[i]->modified = 0;
		if (after_save == AFTER_CLOSE) remove_tab(i);
		int a = after_save;
		after_save = AFTER_NONE;
		confirm_doc = -1;
		if (a == AFTER_QUIT) quit_flow();
		return;
	}
	int r = save_doc(i);
	if (r > 0) after_saved();
	/* r == 0：正在問檔名，存好之後在 M_SAVEAS 裡呼叫 after_saved */
}

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

static void spawn_new_window(void)
{
	char self[PATH_MAX];
	ssize_t k = readlink("/proc/self/exe", self, sizeof self - 1);
	self[k > 0 ? k : 0] = 0;
	char *v[] = { k > 0 ? self : "pomelo-edit", "--new-window", NULL };
	spawn_argv(v);
}

/* ------------------------------------------------------------------ */
/* 剪貼簿                                                              */
/* ------------------------------------------------------------------ */

static void copy_sel(int cut)
{
	size_t n;
	char *t = sel_text(D, &n);
	if (!t) return;
	free(clipbuf);
	clipbuf = t;
	if (wl) pmwl_clip_set(clipbuf); else XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
	if (cut) { push_undo(D, 0); delete_sel(D); }
}

static void on_selrequest(XSelectionRequestEvent *e)
{
	XSelectionEvent r = { .type = SelectionNotify, .display = e->display, .requestor = e->requestor,
	                      .selection = e->selection, .target = e->target, .property = None, .time = e->time };
	const char *data = clipbuf ? clipbuf : "";
	Atom prop = e->property ? e->property : e->target;
	if (e->target == a_targets) {
		Atom ts[] = { a_targets, a_utf8, XA_STRING };
		XChangeProperty(dpy, e->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)ts, 3);
		r.property = prop;
	} else if (e->target == a_utf8 || e->target == XA_STRING) {
		XChangeProperty(dpy, e->requestor, prop, e->target, 8, PropModeReplace, (const unsigned char *)data, (int)strlen(data));
		r.property = prop;
	}
	XSendEvent(dpy, e->requestor, False, 0, (XEvent *)&r);
}

static void field_append(char *buf, size_t cap, const char *s, size_t n);

/* 貼上 d 開頭的 n 個位元組（X 的 SelectionNotify 與原生的 pmwl_clip_get 共用） */
static void paste_data(const unsigned char *d, unsigned long n)
{
	if (!n) return;
	if (bar_focus && mode != M_CONFIRM) {
		/* 貼到輸入框：只取第一行 */
		size_t k = strcspn((const char *)d, "\r\n");
		char *dst = (mode == M_FIND || mode == M_REPLACE) ? (field ? repl_s : find_s) : entry;
		size_t cap = (mode == M_FIND || mode == M_REPLACE) ? sizeof find_s : sizeof entry;
		if (entry_selall) { dst[0] = 0; entry_selall = 0; }
		field_append(dst, cap, (const char *)d, MIN(k, (size_t)n));
	} else {
		push_undo(D, 0);
		char *t = malloc(n + 1);
		size_t k = 0;
		for (unsigned long i = 0; i < n; i++) if (d[i] != '\r') t[k++] = (char)d[i];   /* \r\n → \n */
		insert_text(D, t, k);
		free(t);
	}
}

static void paste(Atom which)
{
	if (wl) {
		char *t = which == XA_PRIMARY ? pmwl_primary_get() : pmwl_clip_get();
		if (t) { paste_data((unsigned char *)t, strlen(t)); free(t); }
		return;
	}
	XConvertSelection(dpy, which, a_utf8, a_paste, win, CurrentTime);
}

static void on_selnotify(XSelectionEvent *e)
{
	if (e->property == None) return;
	Atom type;
	int fmt;
	unsigned long n, extra;
	unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, win, a_paste, 0, 1 << 22, True, AnyPropertyType, &type, &fmt, &n, &extra, &d) == Success && d) {
		paste_data(d, n);
		XFree(d);
	}
}

/* ------------------------------------------------------------------ */
/* 選單列                                                              */
/* ------------------------------------------------------------------ */

static void open_bar(int m);
static void act(int a);

enum {
	A_NEW, A_NEWWIN, A_OPEN, A_FOLDER, A_NEWFILE, A_SAVE, A_SAVEAS, A_SAVEALL, A_CLOSE, A_QUIT,
	A_UNDO, A_REDO, A_CUT, A_COPY, A_PASTE, A_DELETE, A_SELALL, A_FIND, A_FINDNEXT, A_FINDPREV, A_REPLACE, A_GOTO,
	A_SIDE, A_WRAP, A_NUMBERS, A_MONO, A_BIGGER, A_SMALLER, A_RESETFONT, A_SHOWFILES, A_REFRESH, A_UPDIR,
};

static void m_act(const Arg *a) { act(a->i); }

/* 選單項目的文字：名稱（之後翻譯）＋ Tab ＋快捷鍵；check：0 沒有勾選框、1 有勾、2 沒勾 */
static const char *L(const char *name, const char *key, int check)
{
	static char pool[48][160];
	static int k;
	char *b = pool[k++ % 48];
	snprintf(b, sizeof pool[0], "%s%s%s%s", check == 1 ? "\001" : check == 2 ? "\002" : "", name, key ? "\t" : "", key ? key : "");
	return b;
}

static int menu_build(int which, MenuItem *m)
{
	int k = 0;
	Doc *d = D;
#define ITEM(name, key, a, on) (m[k++] = (MenuItem){ L(name, key, 0), (on) ? m_act : pm_menu_off, { .i = (a) } })
#define CHECK(name, key, a, v) (m[k++] = (MenuItem){ L(name, key, (v) ? 1 : 2), m_act, { .i = (a) } })
#define SEP() (m[k++] = (MenuItem){ NULL, NULL, { 0 } })
	if (which == 0) {
		ITEM(N_("新分頁"), "Ctrl+N", A_NEW, 1);
		ITEM(N_("新視窗"), "Ctrl+Shift+N", A_NEWWIN, 1);
		SEP();
		ITEM(N_("開啟…"), "Ctrl+O", A_OPEN, 1);
		ITEM(N_("開啟資料夾…"), NULL, A_FOLDER, 1);
		ITEM(N_("在檔案清單的資料夾新增檔案…"), NULL, A_NEWFILE, side_root[0]);
		SEP();
		ITEM(N_("儲存"), "Ctrl+S", A_SAVE, 1);
		ITEM(N_("另存新檔…"), "Ctrl+Shift+S", A_SAVEAS, 1);
		ITEM(N_("全部儲存"), NULL, A_SAVEALL, 1);
		SEP();
		ITEM(N_("在檔案管理員中顯示"), NULL, A_SHOWFILES, d->path[0] || side_root[0]);
		SEP();
		ITEM(N_("關閉分頁"), "Ctrl+W", A_CLOSE, 1);
		ITEM(N_("結束"), "Ctrl+Q", A_QUIT, 1);
	} else if (which == 1) {
		ITEM(N_("{動作}復原"), "Ctrl+Z", A_UNDO, d->nundo > 0);
		ITEM(N_("重做"), "Ctrl+Shift+Z", A_REDO, d->nredo > 0);
		SEP();
		ITEM(N_("剪下"), "Ctrl+X", A_CUT, has_sel(d));
		ITEM(N_("拷貝"), "Ctrl+C", A_COPY, has_sel(d));
		ITEM(N_("貼上"), "Ctrl+V", A_PASTE, 1);
		ITEM(N_("刪除"), "Delete", A_DELETE, has_sel(d));
		SEP();
		ITEM(N_("全選"), "Ctrl+A", A_SELALL, 1);
		SEP();
		ITEM(N_("尋找…"), "Ctrl+F", A_FIND, 1);
		ITEM(N_("找下一個"), "F3", A_FINDNEXT, find_s[0] != 0);
		ITEM(N_("找上一個"), "Shift+F3", A_FINDPREV, find_s[0] != 0);
		ITEM(N_("取代…"), "Ctrl+H", A_REPLACE, 1);
		SEP();
		ITEM(N_("跳到第幾行…"), "Ctrl+L", A_GOTO, 1);
	} else {
		CHECK(N_("檔案清單"), "Ctrl+B", A_SIDE, show_side);
		CHECK(N_("自動換行"), NULL, A_WRAP, wrap);
		CHECK(N_("行號"), NULL, A_NUMBERS, show_numbers);
		CHECK(N_("等寬字型"), NULL, A_MONO, mono);
		SEP();
		ITEM(N_("放大字型"), "Ctrl++", A_BIGGER, 1);
		ITEM(N_("縮小字型"), "Ctrl+-", A_SMALLER, 1);
		ITEM(N_("原本的大小"), "Ctrl+0", A_RESETFONT, 1);
		SEP();
		ITEM(N_("檔案清單往上一層"), NULL, A_UPDIR, side_root[0] && strcmp(side_root, "/"));
		ITEM(N_("重新整理檔案清單"), NULL, A_REFRESH, side_root[0]);
	}
#undef ITEM
#undef CHECK
#undef SEP
	return k;
}

static void open_bar(int which)
{
	for (int guard = 0; guard < 20; guard++) {
		MenuItem m[32];
		int n = menu_build(which, m);
		int rx = menu_r[which].x, ry = MENU_H;
		Window ch;
		if (!wl) XTranslateCoordinates(dpy, win, pm_root, menu_r[which].x, MENU_H, &rx, &ry, &ch);
		hover_menu = which;
		pm_popup_dir = 0;
		PMWL_POPUP(wl, m, n, rx, ry);
		hover_menu = -1;
		if (!pm_popup_dir) break;
		which = (which + pm_popup_dir + 3) % 3;   /* ←／→：換到旁邊的選單 */
		draw();
	}
}

static void set_font_size(double s)
{
	fsize = CLAMP(s, 8, 40);
	lh = fsize * 1.6;
	pm_conf_set("editor.font_size", "");   /* 先清掉，下面寫數字 */
	char v[16];
	snprintf(v, sizeof v, "%.0f", fsize);
	pm_conf_set("editor.font_size", v);
	update_avail();
}

static void open_entry_bar(int m, const char *init)
{
	mode = m;
	bar_focus = 1;
	entry_selall = 0;
	snprintf(entry, sizeof entry, "%s", init);
}

static void start_find(int replace)
{
	int was = mode == M_FIND || mode == M_REPLACE;
	mode = replace ? M_REPLACE : M_FIND;
	bar_focus = 1;
	field = 0;
	find_fail = 0;
	size_t n;
	char *t = sel_text(D, &n);
	if (t && n < sizeof find_s && !memchr(t, '\n', n)) snprintf(find_s, sizeof find_s, "%s", t);
	free(t);
	entry_selall = find_s[0] != 0;
	(void)was;
}

static void act(int a)
{
	Doc *d = D;
	switch (a) {
	case A_NEW: new_tab(); break;
	case A_NEWWIN: spawn_new_window(); break;
	case A_OPEN: {
		/* 從目前檔案的資料夾開始（沒有就用檔案清單的資料夾） */
		char init[PATH_MAX];
		if (d->path[0]) { snprintf(init, sizeof init, "%s", d->path); char *sl = strrchr(init, '/'); if (sl) sl[1] = 0; }
		else snprintf(init, sizeof init, "%s/", side_root[0] ? side_root : cwd);
		open_entry_bar(M_OPEN, init);
		break;
	}
	case A_FOLDER: { char init[PATH_MAX]; snprintf(init, sizeof init, "%s/", side_root[0] ? side_root : cwd); open_entry_bar(M_FOLDER, init); break; }
	case A_NEWFILE: { char init[PATH_MAX]; snprintf(init, sizeof init, "%s/", side_root); open_entry_bar(M_NEWFILE, init); break; }
	case A_SAVE: if (save_doc(cur) > 0) after_saved(); break;
	case A_SAVEAS: { char init[PATH_MAX]; snprintf(init, sizeof init, "%s", d->path[0] ? d->path : side_root[0] ? side_root : cwd); if (!d->path[0]) strncat(init, "/", sizeof init - strlen(init) - 1); open_entry_bar(M_SAVEAS, init); break; }
	case A_SAVEALL: { int saved = 0; for (int i = 0; i < ndocs; i++) if (docs[i]->modified && docs[i]->path[0] && !save_file(docs[i], docs[i]->path)) saved++;
		char m[96]; snprintf(m, sizeof m, _("已儲存 %d 個檔案"), saved); say(m); set_title(); break; }
	case A_CLOSE: close_tab(cur); break;
	case A_QUIT: quit_flow(); break;
	case A_UNDO: do_undo(d); break;
	case A_REDO: do_redo(d); break;
	case A_CUT: copy_sel(1); break;
	case A_COPY: copy_sel(0); break;
	case A_PASTE: paste(a_clipboard); break;
	case A_DELETE: if (has_sel(d)) { push_undo(d, 0); delete_sel(d); } break;
	case A_SELALL: d->ay = 0; d->ax = 0; d->cy = d->nlines - 1; d->cx = d->lines[d->cy].n; break;
	case A_FIND: start_find(0); break;
	case A_REPLACE: start_find(1); break;
	case A_FINDNEXT: case A_FINDPREV:
		if (!find_s[0]) { start_find(0); break; }
		find_fail = !find_step(d, a == A_FINDNEXT ? 1 : -1);
		if (find_fail) say(_("找不到"));
		break;
	case A_GOTO: open_entry_bar(M_GOTO, ""); break;
	case A_SIDE: show_side = !show_side; pm_conf_set_int("editor.sidebar", show_side); if (show_side && !side_root[0]) set_root(cwd); update_avail(); break;
	case A_WRAP: wrap = !wrap; pm_conf_set_int("editor.wrap", wrap); layout_all(); break;
	case A_NUMBERS: show_numbers = !show_numbers; pm_conf_set_int("editor.line_numbers", show_numbers); update_avail(); break;
	case A_MONO: mono = !mono; pm_conf_set_int("editor.monospace", mono); update_avail(); break;
	case A_BIGGER: set_font_size(fsize + 1); break;
	case A_SMALLER: set_font_size(fsize - 1); break;
	case A_RESETFONT: set_font_size(pm_conf_num("appearance.font_size", 13) + 1); break;
	case A_SHOWFILES: { char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", d->path[0] ? d->path : side_root); if (d->path[0]) { char *sl = strrchr(dir, '/'); if (sl && sl != dir) *sl = 0; }
		char *v[] = { pm_have("pomelo-files") ? "pomelo-files" : "pomelo-open", dir, NULL }; spawn_argv(v); break; }
	case A_REFRESH: build_nodes(); break;
	case A_UPDIR: { char p[PATH_MAX]; snprintf(p, sizeof p, "%s", side_root); char *sl = strrchr(p, '/'); if (sl == p) p[1] = 0; else if (sl) *sl = 0; set_root(p); break; }
	}
	set_title();
}

/* ------------------------------------------------------------------ */
/* 輸入框                                                              */
/* ------------------------------------------------------------------ */

static void field_append(char *buf, size_t cap, const char *s, size_t n)
{
	size_t l = strlen(buf);
	if (l + n >= cap) n = cap - l - 1;
	memcpy(buf + l, s, n);
	buf[l + n] = 0;
}

static void field_key(char *buf, size_t cap, KeySym ks, const char *in, int len)
{
	size_t n = strlen(buf);
	int printable = len > 0 && (unsigned char)in[0] >= 32 && in[0] != 127;
	if (entry_selall) {   /* 全選狀態：打字或退格先把舊字清掉 */
		entry_selall = 0;
		if (ks == XK_BackSpace || printable) { buf[0] = 0; n = 0; }
		if (ks == XK_BackSpace) return;
	}
	if (ks == XK_BackSpace) { while (n && ((unsigned char)buf[n - 1] & 0xc0) == 0x80) buf[--n] = 0; if (n) buf[--n] = 0; }
	else if (printable) field_append(buf, cap, in, (size_t)len);
}

/* 路徑輸入框按 Tab：補完檔名（只有一個符合就補完整，好幾個就補共同的開頭） */
static void complete_path(void)
{
	char p[PATH_MAX];
	abs_path(entry, p, sizeof p);
	char dir[PATH_MAX], pre[NAME_MAX + 1];
	char *sl = strrchr(p, '/');
	if (!sl) return;
	snprintf(pre, sizeof pre, "%s", sl + 1);
	*sl = 0;
	snprintf(dir, sizeof dir, "%s", p[0] ? p : "/");
	DIR *dp = opendir(dir);
	if (!dp) return;
	char best[NAME_MAX + 1] = "";
	int n = 0, isdir = 0;
	struct dirent *e;
	size_t pl = strlen(pre);
	while ((e = readdir(dp))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strncmp(e->d_name, pre, pl)) continue;
		if (e->d_name[0] == '.' && pre[0] != '.') continue;
		if (!n) { snprintf(best, sizeof best, "%s", e->d_name); char fp[PATH_MAX]; snprintf(fp, sizeof fp, "%s/%s", dir, e->d_name); struct stat st; isdir = !stat(fp, &st) && S_ISDIR(st.st_mode); }
		else { size_t k = 0; while (best[k] && best[k] == e->d_name[k]) k++; best[k] = 0; isdir = 0; }
		n++;
	}
	closedir(dp);
	if (!n) return;
	char *esl = strrchr(entry, '/');
	size_t keep = esl ? (size_t)(esl - entry + 1) : 0;
	char out[PATH_MAX];
	snprintf(out, sizeof out, "%.*s%s%s", (int)keep, entry, best, n == 1 && isdir ? "/" : "");
	snprintf(entry, sizeof entry, "%s", out);
}

static void entry_enter(void)
{
	char p[PATH_MAX];
	switch (mode) {
	case M_OPEN:
		abs_path(entry, p, sizeof p);
		if (!entry[0] || p[strlen(p) - 1] == '/') return;
		mode = M_NONE; bar_focus = 0;
		open_path(p);
		break;
	case M_FOLDER: {
		abs_path(entry, p, sizeof p);
		struct stat st;
		if (stat(p, &st) || !S_ISDIR(st.st_mode)) { say(_("找不到這個資料夾")); return; }
		mode = M_NONE; bar_focus = 0;
		set_root(p);
		show_side = 1;
		update_avail();
		break;
	}
	case M_NEWFILE: case M_SAVEAS: {
		abs_path(entry, p, sizeof p);
		if (!entry[0] || p[strlen(p) - 1] == '/') return;
		struct stat st;
		if (mode == M_NEWFILE) {
			if (!stat(p, &st)) { open_path(p); mode = M_NONE; bar_focus = 0; break; }
			mode = M_NONE; bar_focus = 0;
			if (new_tab() < 0) break;
			snprintf(D->path, sizeof D->path, "%s", p);
			if (save_file(D, p)) { char m[400]; snprintf(m, sizeof m, _("無法建立：%s"), strerror(errno)); say(m); }
			build_nodes();
			break;
		}
		mode = M_NONE; bar_focus = 0;
		if (save_file(D, p)) { char m[400]; snprintf(m, sizeof m, _("無法儲存：%s"), strerror(errno)); say(m); after_save = AFTER_NONE; break; }
		char m[PATH_MAX + 100];
		snprintf(m, sizeof m, _("已儲存「%s」"), base_of(p));
		say(m);
		if (side_root[0]) build_nodes();
		set_title();
		after_saved();
		break;
	}
	case M_GOTO: {
		long n = atol(entry);
		if (n < 1) return;
		mode = M_NONE; bar_focus = 0;
		D->cy = (int)MIN(n, D->nlines) - 1;
		D->cx = 0;
		D->ay = -1;
		D->goal_x = -1;
		D->scroll_row = MAX(0, row_of(D, D->cy, 0) - visible_rows() / 3);
		break;
	}
	}
}

/* ------------------------------------------------------------------ */
/* 鍵盤與滑鼠                                                          */
/* ------------------------------------------------------------------ */

static void move_to(Doc *d, int y, int x, int extend)
{
	if (extend) { if (d->ay < 0) { d->ay = d->cy; d->ax = d->cx; } }
	else d->ay = -1;
	d->cy = CLAMP(y, 0, d->nlines - 1);
	d->cx = CLAMP(x, 0, d->lines[d->cy].n);
}

static int x_to_offset(Doc *d, Row *rw, double px)
{
	Line *l = &d->lines[rw->line];
	double rel = px - (text_x0() - d->scroll_x);
	if (rel <= 0) return rw->start;
	/* 二分搜尋：第一個「中線」超過滑鼠位置的字（很長的一行也很快） */
	int lo = rw->start, hi = rw->end;
	while (lo < hi) {
		int mid = lo + (hi - lo) / 2;
		while (mid > lo && ((unsigned char)l->s[mid] & 0xc0) == 0x80) mid--;
		int nx = next_char(l->s, rw->end, mid);
		double a = textw(l->s + rw->start, mid - rw->start), b = textw(l->s + rw->start, nx - rw->start);
		if (rel < (a + b) / 2) hi = mid; else lo = nx;
	}
	return lo;
}

static int row_at_y(Doc *d, double py)
{
	if (d->layout_dirty) layout(d);
	int r = d->scroll_row + (int)((py - text_top() - 6) / lh);
	return CLAMP(r, 0, d->nrows - 1);
}

static void indent_of(Line *l, char *out, size_t n)
{
	size_t k = 0;
	while ((int)k < l->n && (l->s[k] == ' ' || l->s[k] == '\t') && k < n - 1) { out[k] = l->s[k]; k++; }
	out[k] = 0;
}

static void keypress(XKeyEvent *e)
{
	char buf[64];
	KeySym ks = NoSymbol;
	Status st = 0;
	KeySym k0;
	int len = pmwl_xkey(e, buf, sizeof buf - 1, &ks, &k0);   /* 原生 Wayland 轉出來的事件 */
	if (len < 0) {
		len = xic ? Xutf8LookupString(xic, e, buf, sizeof buf - 1, &ks, &st) : XLookupString(e, buf, sizeof buf - 1, &ks, NULL);
		k0 = XLookupKeysym(e, 0);
	}
	if (len < 0 || st == XBufferOverflow) len = 0;
	buf[len] = 0;
	int ctrl = !!(e->state & (ControlMask | Mod4Mask)), shift = !!(e->state & ShiftMask), alt = !!(e->state & Mod1Mask);
	Doc *d = D;

	/* 打開選單列 */
	if ((alt && !ctrl && (k0 == XK_f || k0 == XK_e || k0 == XK_v)) || ks == XK_F10) { open_bar(k0 == XK_e ? 1 : k0 == XK_v ? 2 : 0); return; }
	if (alt && !ctrl && k0 >= XK_1 && k0 <= XK_9) { switch_to((int)(k0 - XK_1)); return; }

	if (mode == M_CONFIRM) {
		if (ks == XK_Escape) confirm_answer(0);
		else if (ks == XK_Return || ks == XK_KP_Enter) confirm_answer(1);
		else if (k0 == XK_d || k0 == XK_n) confirm_answer(2);
		return;
	}
	if (ks == XK_F3) { act(shift ? A_FINDPREV : A_FINDNEXT); return; }
	if (ctrl) {
		switch (k0) {
		case XK_n: act(shift ? A_NEWWIN : A_NEW); return;
		case XK_o: act(A_OPEN); return;
		case XK_s: act(shift ? A_SAVEAS : A_SAVE); return;
		case XK_w: act(A_CLOSE); return;
		case XK_q: act(A_QUIT); return;
		case XK_Tab: case XK_ISO_Left_Tab: switch_to((cur + (shift ? -1 : 1) + ndocs) % ndocs); return;
		case XK_Page_Down: switch_to((cur + 1) % ndocs); return;
		case XK_Page_Up: switch_to((cur - 1 + ndocs) % ndocs); return;
		case XK_f: act(A_FIND); return;
		case XK_h: act(A_REPLACE); return;
		case XK_g: act(shift ? A_FINDPREV : A_FINDNEXT); return;
		case XK_l: act(A_GOTO); return;
		case XK_b: act(A_SIDE); return;
		case XK_plus: case XK_equal: case XK_KP_Add: act(A_BIGGER); return;
		case XK_minus: case XK_KP_Subtract: act(A_SMALLER); return;
		case XK_0: act(A_RESETFONT); return;
		}
		if (bar_focus) {
			if (k0 == XK_v) { paste(a_clipboard); return; }
			if (k0 == XK_a) { entry_selall = 1; return; }
			return;
		}
	}
	/* 列上的輸入框 */
	if (bar_focus && mode != M_NONE) {
		if (ks == XK_Escape) { mode = M_NONE; bar_focus = 0; find_fail = 0; entry_selall = 0; after_save = AFTER_NONE; return; }
		if (mode == M_FIND || mode == M_REPLACE) {
			if (ks == XK_Tab || ks == XK_ISO_Left_Tab) { if (mode == M_REPLACE) { field = !field; entry_selall = 0; } return; }
			if (ks == XK_Return || ks == XK_KP_Enter) {
				if (field == 1) { if (!replace_one(d)) find_step(d, 1); else find_step(d, 1); }
				else find_fail = !find_step(d, shift ? -1 : 1);
				return;
			}
			char *dst = field ? repl_s : find_s;
			field_key(dst, sizeof find_s, ks, buf, len);
			if (!field && len > 0) {   /* 邊打邊找：從目前的選取開頭開始找 */
				if (has_sel(d)) { int y1, x1, y2, x2; sel_bounds(d, &y1, &x1, &y2, &x2); d->cy = y1; d->cx = x1; d->ay = -1; }
				find_fail = find_s[0] && !find_step(d, 1);
			}
			return;
		}
		if (ks == XK_Tab) { if (mode != M_GOTO) complete_path(); return; }
		if (ks == XK_Return || ks == XK_KP_Enter) { entry_enter(); return; }
		if (mode == M_GOTO && len == 1 && !isdigit((unsigned char)buf[0])) return;
		field_key(entry, sizeof entry, ks, buf, len);
		return;
	}
	if (ks == XK_Escape) {
		if (mode != M_NONE) { mode = M_NONE; bar_focus = 0; find_fail = 0; return; }
		d->ay = -1;
		return;
	}
	if (ctrl) {
		switch (k0) {
		case XK_z: if (shift) do_redo(d); else do_undo(d); return;
		case XK_y: do_redo(d); return;
		case XK_a: act(A_SELALL); return;
		case XK_c: copy_sel(0); return;
		case XK_x: copy_sel(1); return;
		case XK_v: paste(a_clipboard); return;
		case XK_Home: move_to(d, 0, 0, shift); return;
		case XK_End: move_to(d, d->nlines - 1, d->lines[d->nlines - 1].n, shift); return;
		case XK_Left: case XK_Right: {
			/* 一次一個字（英文單字、中文一個字） */
			int y = d->cy, x = d->cx;
			Line *l = &d->lines[y];
			if (k0 == XK_Left) {
				if (x == 0) { if (y > 0) { y--; x = d->lines[y].n; } }
				else { x = prev_char(l->s, x); while (x > 0 && !is_word(l->s, x)) x = prev_char(l->s, x); while (x > 0 && is_word(l->s, prev_char(l->s, x)) && (unsigned char)l->s[x] < 0x80) x = prev_char(l->s, x); }
			} else {
				if (x >= l->n) { if (y < d->nlines - 1) { y++; x = 0; } }
				else { while (x < l->n && !is_word(l->s, x)) x = next_char(l->s, l->n, x); if ((unsigned char)l->s[x] >= 0x80) x = next_char(l->s, l->n, x); else while (x < l->n && is_word(l->s, x) && (unsigned char)l->s[x] < 0x80) x = next_char(l->s, l->n, x); }
			}
			move_to(d, y, x, shift);
			d->goal_x = -1;
			return;
		}
		}
		return;
	}
	Line *l = &d->lines[d->cy];
	switch (ks) {
	case XK_Left:
		if (shift || !has_sel(d)) { if (d->cx > 0) move_to(d, d->cy, prev_char(l->s, d->cx), shift); else if (d->cy > 0) move_to(d, d->cy - 1, d->lines[d->cy - 1].n, shift); }
		else { int y1, x1, y2, x2; sel_bounds(d, &y1, &x1, &y2, &x2); move_to(d, y1, x1, 0); }
		d->goal_x = -1;
		return;
	case XK_Right:
		if (shift || !has_sel(d)) { if (d->cx < l->n) move_to(d, d->cy, next_char(l->s, l->n, d->cx), shift); else if (d->cy < d->nlines - 1) move_to(d, d->cy + 1, 0, shift); }
		else { int y1, x1, y2, x2; sel_bounds(d, &y1, &x1, &y2, &x2); move_to(d, y2, x2, 0); }
		d->goal_x = -1;
		return;
	case XK_Up: case XK_Down: case XK_Page_Up: case XK_Page_Down: {
		if (d->layout_dirty) layout(d);
		int r = row_of(d, d->cy, d->cx);
		/* 上下移動時記住原本的水平位置（經過短的行也不會跑掉） */
		if (d->goal_x < 0) d->goal_x = text_x0() - d->scroll_x + textw(l->s + d->rows[r].start, d->cx - d->rows[r].start);
		int vis = visible_rows();
		int nr = ks == XK_Up ? r - 1 : ks == XK_Down ? r + 1 : ks == XK_Page_Up ? r - vis : r + vis;
		double gx = d->goal_x;
		if (nr < 0) { move_to(d, 0, 0, shift); d->goal_x = gx; return; }
		if (nr >= d->nrows) { move_to(d, d->nlines - 1, d->lines[d->nlines - 1].n, shift); d->goal_x = gx; return; }
		move_to(d, d->rows[nr].line, x_to_offset(d, &d->rows[nr], gx), shift);
		d->goal_x = gx;
		if (ks == XK_Page_Up || ks == XK_Page_Down) d->scroll_row = CLAMP(d->scroll_row + (ks == XK_Page_Up ? -vis : vis), 0, MAX(0, d->nrows - vis));
		return;
	}
	case XK_Home: case XK_KP_Home: {
		if (d->layout_dirty) layout(d);
		int r = row_of(d, d->cy, d->cx);
		/* 第一次按到第一個不是空白的字，再按一次到行首 */
		int first = 0;
		while (first < l->n && (l->s[first] == ' ' || l->s[first] == '\t')) first++;
		int to = d->rows[r].start == 0 && d->cx != first ? first : d->rows[r].start;
		move_to(d, d->cy, to, shift);
		d->goal_x = -1;
		return;
	}
	case XK_End: case XK_KP_End: { if (d->layout_dirty) layout(d); int r = row_of(d, d->cy, d->cx); move_to(d, d->cy, d->rows[r].end, shift); d->goal_x = -1; return; }
	case XK_BackSpace: backspace(d, 0); return;
	case XK_Delete: case XK_KP_Delete: backspace(d, 1); return;
	case XK_Return: case XK_KP_Enter: {
		/* 自動縮排：新的一行跟上一行一樣的開頭空白 */
		char ind[256];
		indent_of(l, ind, sizeof ind);
		size_t il = strlen(ind);
		if ((int)il > d->cx) il = (size_t)d->cx;
		char t[260];
		t[0] = '\n';
		memcpy(t + 1, ind, il);
		push_undo(d, 0);
		insert_text(d, t, il + 1);
		return;
	}
	case XK_Tab: push_undo(d, 1); insert_text(d, "\t", 1); return;
	}
	if (len > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
		push_undo(d, 1);
		insert_text(d, buf, (size_t)len);
	}
}

static int side_row_at(int x, int y)
{
	if (!show_side || x < 0 || x >= side_w - 3 || y < side_r.y + 30 || y >= side_r.y + side_r.h) return -1;
	int i = side_scroll + (y - side_r.y - 30) / SIDE_ROW;
	return i < nnodes ? i : -1;
}

static int in_rect(Rect r, int x, int y) { return r.w && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static Node menu_node;
static void m_node_open(const Arg *a) { (void)a; open_path(menu_node.path); }
static void m_node_newfile(const Arg *a)
{
	(void)a;
	char init[PATH_MAX];
	if (menu_node.dir) snprintf(init, sizeof init, "%s/", menu_node.path);
	else { snprintf(init, sizeof init, "%s", menu_node.path); char *sl = strrchr(init, '/'); if (sl) sl[1] = 0; }
	open_entry_bar(M_NEWFILE, init);
}
static void m_node_root(const Arg *a) { (void)a; set_root(menu_node.path); }
static void m_node_files(const Arg *a)
{
	(void)a;
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", menu_node.path);
	if (!menu_node.dir) { char *sl = strrchr(dir, '/'); if (sl && sl != dir) *sl = 0; }
	char *v[] = { pm_have("pomelo-files") ? "pomelo-files" : "pomelo-open", dir, NULL };
	spawn_argv(v);
}
static void m_node_term(const Arg *a)
{
	(void)a;
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", menu_node.path);
	if (!menu_node.dir) { char *sl = strrchr(dir, '/'); if (sl && sl != dir) *sl = 0; }
	char *v[] = { "pomelo-terminal", "-d", dir, NULL };
	spawn_argv(v);
}

static void bpress(XButtonEvent *e)
{
	Doc *d = D;
	int x = e->x, y = e->y;
	if (e->button == 4 || e->button == 5) {
		int dir = e->button == 4 ? -3 : 3;
		if (show_side && x < side_w) { side_scroll += dir; return; }
		if (e->state & ShiftMask && !wrap) { d->scroll_x = MAX(0, d->scroll_x + dir * 20); return; }
		if (d->layout_dirty) layout(d);
		d->scroll_row = CLAMP(d->scroll_row + dir, 0, MAX(0, d->nrows - visible_rows()));
		return;
	}
	if (e->button == 6 || e->button == 7) { if (!wrap) d->scroll_x = MAX(0, d->scroll_x + (e->button == 6 ? -60 : 60)); return; }
	/* 選單列 */
	if (y < MENU_H) {
		for (int i = 0; i < 3; i++) if (in_rect(menu_r[i], x, y)) { open_bar(i); return; }
		return;
	}
	/* 分頁列 */
	if (y < MENU_H + TAB_H) {
		if (in_rect(newtab_r, x, y)) { new_tab(); return; }
		for (int i = 0; i < ndocs; i++) {
			if (!in_rect(tab_r[i], x, y)) continue;
			if (e->button == Button2 || in_rect(tab_close_r[i], x, y)) { close_tab(i); return; }
			if (e->button == Button1) switch_to(i);
			if (e->button == Button3) {
				switch_to(i);
				MenuItem m[] = {
					{ L(N_("關閉分頁"), "Ctrl+W", 0), m_act, { .i = A_CLOSE } },
					{ L(N_("另存新檔…"), NULL, 0), m_act, { .i = A_SAVEAS } },
					{ L(N_("在檔案管理員中顯示"), NULL, 0), D->path[0] ? m_act : pm_menu_off, { .i = A_SHOWFILES } },
				};
				PMWL_POPUP(wl, m, LEN(m), e->x_root, e->y_root);
			}
			return;
		}
		if (e->button == Button1 && e->time - last_click < 400) new_tab();   /* 空白的地方按兩下：新分頁 */
		last_click = e->time;
		return;
	}
	/* 左邊的檔案清單 */
	if (show_side && x < side_w) {
		if (x >= side_w - 4 && e->button == Button1) { side_drag = 1; return; }   /* 拖曳邊界調寬度 */
		int i = side_row_at(x, y);
		if (i < 0) {
			if (e->button == Button3) {
				menu_node = (Node){ 0 };
				snprintf(menu_node.path, sizeof menu_node.path, "%s", side_root);
				menu_node.dir = 1;
				MenuItem m[] = {
					{ L(N_("新增檔案…"), NULL, 0), m_node_newfile, { 0 } },
					{ L(N_("開啟資料夾…"), NULL, 0), m_act, { .i = A_FOLDER } },
					{ L(N_("檔案清單往上一層"), NULL, 0), strcmp(side_root, "/") ? m_act : pm_menu_off, { .i = A_UPDIR } },
					{ L(N_("重新整理檔案清單"), NULL, 0), m_act, { .i = A_REFRESH } },
				};
				PMWL_POPUP(wl, m, LEN(m), e->x_root, e->y_root);
			}
			return;
		}
		Node nd = nodes[i];
		if (e->button == Button3) {
			menu_node = nd;
			if (nd.dir) {
				MenuItem m[] = {
					{ L(N_("在這裡新增檔案…"), NULL, 0), m_node_newfile, { 0 } },
					{ L(N_("把這個資料夾當成檔案清單的最上層"), NULL, 0), m_node_root, { 0 } },
					{ NULL, NULL, { 0 } },
					{ L(N_("在檔案管理員中顯示"), NULL, 0), m_node_files, { 0 } },
					{ L(N_("在終端機中打開"), NULL, 0), pm_have("pomelo-terminal") ? m_node_term : pm_menu_off, { 0 } },
				};
				PMWL_POPUP(wl, m, LEN(m), e->x_root, e->y_root);
			} else {
				MenuItem m[] = {
					{ L(N_("開啟"), NULL, 0), m_node_open, { 0 } },
					{ L(N_("在同一個資料夾新增檔案…"), NULL, 0), m_node_newfile, { 0 } },
					{ NULL, NULL, { 0 } },
					{ L(N_("在檔案管理員中顯示"), NULL, 0), m_node_files, { 0 } },
				};
				PMWL_POPUP(wl, m, LEN(m), e->x_root, e->y_root);
			}
			return;
		}
		if (e->button != Button1) return;
		if (nd.dir) { set_expanded(nd.path, !is_expanded(nd.path)); build_nodes(); }
		else open_path(nd.path);
		return;
	}
	/* 上面的列：按鈕、輸入框 */
	if (mode != M_NONE && y < text_top()) {
		for (int i = 0; i < nbar_btn; i++) {
			if (!in_rect(bar_btn[i], x, y)) continue;
			switch (bar_ids[i]) {
			case BB_PREV: find_fail = !find_step(d, -1); break;
			case BB_NEXT: find_fail = !find_step(d, 1); break;
			case BB_CASE: match_case = !match_case; break;
			case BB_CLOSE: mode = M_NONE; bar_focus = 0; after_save = AFTER_NONE; break;
			case BB_REPL: if (!replace_one(d)) find_step(d, 1); else find_step(d, 1); break;
			case BB_REPLALL: { int n = replace_all(d); char m[96]; snprintf(m, sizeof m, _("已取代 %d 個"), n); say(m); break; }
			case BB_SAVE: confirm_answer(1); break;
			case BB_DISCARD: confirm_answer(2); break;
			case BB_CANCEL: confirm_answer(0); break;
			}
			return;
		}
		bar_focus = 1;
		if (mode == M_REPLACE) field = y >= MENU_H + TAB_H + BAR_H;
		entry_selall = 0;
		return;
	}
	if (y >= H - STATUS_H) return;
	/* 文字 */
	if (mode != M_CONFIRM) bar_focus = 0;
	if (e->button == Button2) { paste(XA_PRIMARY); return; }
	if (e->button == Button3) {
		MenuItem m[] = {
			{ L(N_("剪下"), "Ctrl+X", 0), has_sel(d) ? m_act : pm_menu_off, { .i = A_CUT } },
			{ L(N_("拷貝"), "Ctrl+C", 0), has_sel(d) ? m_act : pm_menu_off, { .i = A_COPY } },
			{ L(N_("貼上"), "Ctrl+V", 0), m_act, { .i = A_PASTE } },
			{ NULL, NULL, { 0 } },
			{ L(N_("全選"), "Ctrl+A", 0), m_act, { .i = A_SELALL } },
			{ L(N_("尋找…"), "Ctrl+F", 0), m_act, { .i = A_FIND } },
		};
		PMWL_POPUP(wl, m, LEN(m), e->x_root, e->y_root);
		return;
	}
	if (e->button != Button1 || !d->nrows) return;
	int r = row_at_y(d, y);
	int cx = x_to_offset(d, &d->rows[r], x), cy = d->rows[r].line;
	if (e->time - last_click < 400 && cx == last_cx && cy == last_cy) clicks = clicks % 3 + 1; else clicks = 1;
	last_click = e->time; last_cx = cx; last_cy = cy;
	d->goal_x = -1;
	if (clicks == 2) {   /* 選一個字 */
		Line *l = &d->lines[cy];
		int s = cx, en = cx;
		while (s > 0 && is_word(l->s, prev_char(l->s, s))) s = prev_char(l->s, s);
		while (en < l->n && is_word(l->s, en)) en = next_char(l->s, l->n, en);
		d->ay = cy; d->ax = s; d->cy = cy; d->cx = en;
	} else if (clicks == 3) {
		d->ay = cy; d->ax = 0; d->cy = cy; d->cx = d->lines[cy].n;
	} else {
		move_to(d, cy, cx, e->state & ShiftMask);
		dragging = 1;
	}
}

static void motion(int x, int y, unsigned state)
{
	if (side_drag) { side_w = CLAMP(x, 140, W / 2); update_avail(); return; }
	if (dragging && (state & Button1Mask)) {
		Doc *d = D;
		int r = row_at_y(d, y);
		move_to(d, d->rows[r].line, x_to_offset(d, &d->rows[r], x), 1);
		return;
	}
	hover_menu = -1;
	for (int i = 0; i < 3; i++) if (in_rect(menu_r[i], x, y)) hover_menu = i;
	hover_tab = hover_close = -1;
	if (y >= MENU_H && y < MENU_H + TAB_H) {
		if (in_rect(newtab_r, x, y)) hover_tab = -2;
		for (int i = 0; i < ndocs; i++) if (in_rect(tab_r[i], x, y)) { hover_tab = i; if (in_rect(tab_close_r[i], x, y)) hover_close = i; }
	}
	hover_side = side_row_at(x, y);
	hover_bar = -1;
	for (int i = 0; i < nbar_btn; i++) if (in_rect(bar_btn[i], x, y)) hover_bar = bar_ids[i];
}

/* ------------------------------------------------------------------ */
/* 同時只開一個視窗                                                    */
/* ------------------------------------------------------------------ */

static Window find_owner(void)
{
	Atom type; int fmt; unsigned long n = 0, after; unsigned char *p = NULL;
	Window w = None;
	if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), a_owner, 0, 1, False, XA_WINDOW, &type, &fmt, &n, &after, &p) == Success && p) {
		if (n) w = *(Window *)p;
		XFree(p);
	}
	if (!w) return None;
	XClassHint ch = { 0 };
	int ok = XGetClassHint(dpy, w, &ch) && ch.res_name && !strcmp(ch.res_name, "pomelo-edit");
	if (ch.res_name) XFree(ch.res_name);
	if (ch.res_class) XFree(ch.res_class);
	return ok ? w : None;
}

static int xerr_quiet(Display *dd, XErrorEvent *ev) { (void)dd; (void)ev; return 0; }

static void cleanup(void)
{
	if (!dpy || !win || new_window) return;
	if (find_owner() == win) XDeleteProperty(dpy, pm_root, a_owner);
	XFlush(dpy);
}

/* 別的 pomelo-edit 行程轉過來的檔案（\0 分隔的路徑） */
static void handle_open(void)
{
	Atom type; int fmt; unsigned long n = 0, after; unsigned char *p = NULL;
	if (XGetWindowProperty(dpy, win, a_open, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n, &after, &p) != Success || !p) return;
	for (unsigned long i = 0; i < n; ) {
		const char *s = (const char *)p + i;
		size_t l = strnlen(s, n - i);
		if (l) open_path(s);
		i += l + 1;
	}
	XFree(p);
	XMapRaised(dpy, win);
	XEvent ev = { .type = ClientMessage };
	ev.xclient.window = win;
	ev.xclient.message_type = a_active;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = 2;
	XSendEvent(dpy, pm_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

/* 原生 Wayland 的「同時只開一個視窗」：沒有根視窗屬性可用，改成 $XDG_RUNTIME_DIR 裡的 Unix socket。
 * 第二個行程把路徑（\0 分隔）寫給第一個，第一個開成分頁並叫到前面 */
static int srv_fd = -1;

/* 抽象 socket（不佔檔案、行程結束就消失，XDG_RUNTIME_DIR 路徑再長也行）；名稱帶使用者與 XDG_RUNTIME_DIR 的雜湊，
 * 接受連線時用 SO_PEERCRED 確認是同一個使用者 */
static socklen_t sock_addr(struct sockaddr_un *a)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	unsigned h = 5381;
	for (const char *c = rt ? rt : ""; *c; c++) h = h * 33 + (unsigned char)*c;
	memset(a, 0, sizeof *a);
	a->sun_family = AF_UNIX;
	int n = snprintf(a->sun_path + 1, sizeof a->sun_path - 1, "pomelo-edit-%u-%08x", (unsigned)getuid(), h);
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
}

static int wl_send_owner(char **paths, int n)
{
	struct sockaddr_un a;
	socklen_t al = sock_addr(&a);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return 0;
	if (connect(fd, (struct sockaddr *)&a, al) < 0) { close(fd); return 0; }
	for (int i = 0; i < n; i++) { size_t l = strlen(paths[i]) + 1; if (write(fd, paths[i], l) != (ssize_t)l) break; }
	close(fd);
	return 1;
}

static void wl_listen(void)
{
	struct sockaddr_un a;
	socklen_t al = sock_addr(&a);
	if (new_window) return;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return;
	if (bind(fd, (struct sockaddr *)&a, al) < 0 || listen(fd, 4) < 0) { close(fd); return; }
	srv_fd = fd;
	pmwl_watch_fd(fd);
}

static void wl_raise(void)
{
	PmwToplevel t[64];
	int n = pmwl_toplevels(t, 64);
	for (int i = 0; i < n; i++)
		if (!strcmp(t[i].app_id, "pomelo-edit")) { pmwl_toplevel_do(t[i].id, PMWL_TL_ACTIVATE); return; }
}

static void wl_accept(void)
{
	int fd = accept4(srv_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0) return;
	struct ucred cr;
	socklen_t cl = sizeof cr;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) < 0 || cr.uid != getuid()) { close(fd); return; }
	char buf[65536];
	size_t n = 0;
	struct pollfd pf = { fd, POLLIN, 0 };
	while (n < sizeof buf - 1 && poll(&pf, 1, 200) > 0) {
		ssize_t k = read(fd, buf + n, sizeof buf - 1 - n);
		if (k <= 0) break;
		n += (size_t)k;
	}
	close(fd);
	buf[n] = 0;
	for (size_t i = 0; i < n; ) {
		size_t l = strnlen(buf + i, n - i);
		if (l) open_path(buf + i);
		i += l + 1;
	}
	wl_raise();
}

static void cleanup_wl(void)
{
	if (docs_file[0]) unlink(docs_file);
}

/* ------------------------------------------------------------------ */

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	/* 一張紙，上面幾行字和一支筆 */
	pm_rrect(cr, 22, 10, 84, 108, 10, 10);
	cairo_set_source_rgb(cr, 0.97, 0.97, 0.98);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.80, 0.80, 0.83);
	cairo_set_line_width(cr, 2);
	pm_rrect(cr, 22, 10, 84, 108, 10, 10);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, 0.55, 0.56, 0.60);
	for (int i = 0; i < 6; i++) { cairo_rectangle(cr, 36, 30 + i * 13, i == 5 ? 30 : 56, 4); cairo_fill(cr); }
	cairo_set_source_rgb(cr, 1.0, 0.62, 0.20);
	cairo_set_line_width(cr, 9);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, 112, 44); cairo_line_to(cr, 70, 98); cairo_stroke(cr);
	cairo_set_source_rgb(cr, 0.25, 0.25, 0.28);
	cairo_move_to(cr, 70, 98); cairo_line_to(cr, 64, 110); cairo_line_to(cr, 76, 104); cairo_close_path(cr); cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

static int dirty;

static void dispatch(XEvent *ev)
{
	char **dropped;
	int nd = wl ? -1 : pm_dnd_event(dpy, ev, &dropped);
	if (nd >= 0) {   /* 拖進來的檔案：每個開一個分頁 */
		for (int i = 0; i < nd; i++) open_path(dropped[i]);
		if (nd > 0) { pm_dnd_free(dropped, nd); set_title(); dirty = 1; }
		return;
	}
	switch (ev->type) {
	case Expose: if (!ev->xexpose.count) dirty = 1; break;
	case ConfigureNotify:
		if (ev->xconfigure.window == win && (ev->xconfigure.width != W || ev->xconfigure.height != H)) {
			W = ev->xconfigure.width; H = ev->xconfigure.height;
			if (side_w > W / 2) side_w = MAX(140, W / 2);
			update_avail();
			dirty = 1;
		}
		break;
	case KeyPress: keypress(&ev->xkey); set_title(); dirty = 1; break;
	case ButtonPress: bpress(&ev->xbutton); set_title(); dirty = 1; break;
	case ButtonRelease:
		dragging = 0;
		if (side_drag) { side_drag = 0; pm_conf_set_int("editor.sidebar_width", side_w); }
		if (has_sel(D)) { size_t n; char *t = sel_text(D, &n); if (t) { if (wl) { pmwl_primary_set(t); free(t); } else { XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime); free(clipbuf); clipbuf = t; } } }
		break;
	case MotionNotify: {
		int a = hover_menu, b = hover_tab, c = hover_close, s = hover_side, h = hover_bar;
		motion(ev->xmotion.x, ev->xmotion.y, ev->xmotion.state);
		if (dragging || side_drag || a != hover_menu || b != hover_tab || c != hover_close || s != hover_side || h != hover_bar) dirty = 1;
		break;
	}
	case LeaveNotify: hover_menu = hover_tab = hover_close = hover_side = hover_bar = -1; dirty = 1; break;
	case FocusIn: focused = 1; if (xic && !wl) XSetICFocus(xic); if (side_root[0]) build_nodes(); check_disk(); dirty = 1; break;
	case FocusOut: focused = 0; if (xic && !wl) XUnsetICFocus(xic); preedit[0] = 0; dirty = 1; break;
	case SelectionRequest: on_selrequest(&ev->xselectionrequest); break;
	case SelectionNotify: on_selnotify(&ev->xselection); set_title(); dirty = 1; break;
	case ClientMessage: if ((Atom)ev->xclient.data.l[0] == a_delete) quit_flow(); dirty = 1; break;
	case MappingNotify: XRefreshKeyboardMapping(&ev->xmapping); break;
	case PropertyNotify:
		if (ev->xproperty.window == win && ev->xproperty.atom == a_open && ev->xproperty.state == PropertyNewValue) { handle_open(); set_title(); dirty = 1; break; }
		if (pm_conf_changed(dpy, ev)) {   /* 深色模式切換 */
			unsigned accent = (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16);
			pm_set_theme(pm_conf_int("appearance.dark", 0), accent);
			colors();
			XSetWindowBackground(dpy, win, col_bg);
			dirty = 1;
		}
		break;
	}
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		printf("%s: pomelo-edit [--new-window] [%s…]\n", _("用法"), _("檔案或資料夾"));
		return 0;
	}
	setlocale(LC_ALL, "");
	signal(SIGCHLD, SIG_IGN);
	char **paths = malloc((size_t)argc * sizeof *paths);
	int npaths = 0;
	if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "%s", getenv("HOME") ? getenv("HOME") : "/");
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--new-window")) { new_window = 1; continue; }
		if (!argv[i][0]) continue;
		char p[PATH_MAX];
		abs_path(argv[i], p, sizeof p);
		paths[npaths++] = strdup(p);
	}
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	int native = pmwl_connect();
	if (native) {
		pm_ui_native = 1;
		a_delete = PMWL_XCLOSE;
		if (!new_window && npaths && wl_send_owner(paths, npaths)) return 0;   /* 已經有編輯器開著：把檔案交給它 */
	} else {
	if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "pomelo-edit: %s\n", _("無法開啟 X display")); return 1; }
	XSetErrorHandler(xerr_quiet);
	a_owner = XInternAtom(dpy, "_POMELO_EDIT", False);
	a_open = XInternAtom(dpy, "_POMELO_EDIT_OPEN", False);
	a_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	a_docs = XInternAtom(dpy, "_POMELO_EDIT_DOCS", False);
	}
	/* 已經有編輯器開著：把檔案交給它 */
	if (!native && !new_window && npaths) {
		Window other = find_owner();
		if (other) {
			size_t len = 0;
			for (int i = 0; i < npaths; i++) len += strlen(paths[i]) + 1;
			char *msg = malloc(len + 1), *q = msg;
			for (int i = 0; i < npaths; i++) { size_t l = strlen(paths[i]); memcpy(q, paths[i], l + 1); q += l + 1; }
			XChangeProperty(dpy, other, a_open, XInternAtom(dpy, "UTF8_STRING", False), 8, PropModeAppend, (unsigned char *)msg, (int)len);
			XSync(dpy, False);
			return 0;
		}
	}
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	unsigned accent = (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16);
	pm_set_theme(pm_conf_int("appearance.dark", 0), accent);
	colors();
	if (!native) pm_dispatch = dispatch;
	/* 舊文字檔的編碼照介面語言猜（台灣 Big5、大陸 GB18030、日本 Shift_JIS、韓國 EUC-KR） */
	const char *lg = pm_lang();
	if (!strncmp(lg, "zh_CN", 5) || !strncmp(lg, "zh_SG", 5)) snprintf(legacy_cs, sizeof legacy_cs, "GB18030");
	else if (!strncmp(lg, "ja", 2)) snprintf(legacy_cs, sizeof legacy_cs, "CP932");
	else if (!strncmp(lg, "ko", 2)) snprintf(legacy_cs, sizeof legacy_cs, "CP949");
	else if (!strncmp(lg, "zh", 2)) snprintf(legacy_cs, sizeof legacy_cs, "CP950");
	else snprintf(legacy_cs, sizeof legacy_cs, "CP1252");
	fsize = pm_conf_num("editor.font_size", 0);
	if (fsize < 8) fsize = pm_conf_num("appearance.font_size", 13) + 1;
	lh = fsize * 1.6;
	wrap = pm_conf_int("editor.wrap", 1);
	show_numbers = pm_conf_int("editor.line_numbers", 1);
	mono = pm_conf_int("editor.monospace", 0);
	show_side = pm_conf_int("editor.sidebar", 1);
	side_w = CLAMP(pm_conf_int("editor.sidebar_width", 230), 140, 600);
	cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	mcr = cairo_create(ms);

	Rect m = { 0, 0, 1280, 720 };
	if (native) { Rect o[8]; if (pmwl_outputs(o, 8) > 0) m = o[0]; }
	else { pm_update_monitors(); m = mons[pm_pointer_mon()]; }
	W = MIN(1040, (int)(m.w * 0.8));
	H = MIN(700, (int)(m.h * 0.85));
	if (native) {
		wl = pmwl_window(_("文字編輯器"), "pomelo-edit", W, H, 480, 300, NULL);
		if (!wl) { fprintf(stderr, "pomelo-edit: %s\n", _("無法開啟 Wayland 視窗")); return 1; }
		win = PMWL_XID(wl);
		pmwl_want(wl, PMWL_IN_PREEDIT);
		static const char *const uri[] = { "text/uri-list" };
		pmwl_dnd_target(wl, uri, 1, PMWL_DND_COPY);   /* 收拖進來的檔案 */
		wl_listen();
		atexit(cleanup_wl);
		goto started;
	}
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_paste = XInternAtom(dpy, "POMELO_EDIT_PASTE", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	/* backing store：被別的視窗（例如走過去的小柚）蓋住再露出來時，X 伺服器自己補回來，不用重畫 */
	XSetWindowAttributes wa = { .background_pixel = col_bg, .bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
	                            .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
	                                          LeaveWindowMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask };
	win = XCreateWindow(dpy, pm_root, m.x + (m.w - W) / 2, m.y + (m.h - H) / 2, W, H, 0, CopyFromParent, InputOutput, CopyFromParent,
	                    CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	XClassHint ch = { "pomelo-edit", "Pomelo-edit" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 480; sz->min_height = 300;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	pm_dnd_accept(dpy, win);   /* 收拖進來的檔案 */
	long pid = getpid();
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	cairo_surface_t *is = icon(64);
	unsigned long *dd = malloc((2 + 64 * 64) * sizeof(unsigned long));
	dd[0] = 64; dd[1] = 64;
	unsigned *px = (unsigned *)cairo_image_surface_get_data(is);
	for (int i = 0; i < 64 * 64; i++) dd[2 + i] = px[i];
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)dd, 2 + 64 * 64);
	free(dd);
	cairo_surface_destroy(is);
	if (!new_window && !find_owner()) XChangeProperty(dpy, pm_root, a_owner, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&win, 1);
	atexit(cleanup);

	pm_xim_attach(dpy, &xim, &xic, win);   /* 輸入法重新啟動也不會當掉 */
started:
	docs[0] = doc_new();
	docs[0]->untitled = 1;
	ndocs = 1;
	cur = 0;
	update_avail();
	for (int i = 0; i < npaths; i++) open_path(paths[i]);
	/* 檔案清單：第一個檔案的資料夾（或打開的資料夾），沒有的話用目前的資料夾 */
	if (!side_root[0]) {
		char dir[PATH_MAX];
		if (D->path[0]) { snprintf(dir, sizeof dir, "%s", D->path); char *sl = strrchr(dir, '/'); if (sl == dir) dir[1] = 0; else if (sl) *sl = 0; }
		else snprintf(dir, sizeof dir, "%s", strcmp(cwd, "/") ? cwd : (getenv("HOME") ? getenv("HOME") : "/"));
		set_root(dir);
	}
	update_avail();
	set_title();
	if (wl) {
		pmwl_toplevels_start();
		long next_check = 0;
		for (;;) {
			long now = pm_now_ms();
			if (now >= next_check) { if (check_disk()) dirty = 1; next_check = now + 1000; }   /* 每秒看一次檔案有沒有被別的程式改過 */
			if (dirty) { draw(); dirty = 0; }
			now = pm_now_ms();
			int any = 0;
			for (int i = 0; i < ndocs && !any; i++) any = docs[i]->disk_ok;
			long wait = message_until > now ? message_until - now + 5 : -1;
			if (any && (wait < 0 || next_check - now < wait)) wait = MAX(0, next_check - now);
			PmwEvent pe;
			if (!pmwl_next(&pe, wait)) {
				if (message_until && pm_now_ms() >= message_until) { message_until = 0; dirty = 1; }   /* 狀態列的訊息時間到了 */
				continue;
			}
			do {
				XEvent ev;
				switch (pe.type) {
				case PMW_CONF: {   /* 深色模式切換 */
					unsigned accent = (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a84ff") + 1, NULL, 16);
					pm_set_theme(pm_conf_int("appearance.dark", 0), accent);
					colors();
					dirty = 1;
					break;
				}
				case PMW_FD: if (pe.x == srv_fd) { wl_accept(); set_title(); dirty = 1; } break;
				case PMW_PREEDIT: snprintf(preedit, sizeof preedit, "%s", pe.text); dirty = 1; break;
				case PMW_KEY: preedit[0] = 0; if (pmwl_to_xevent(&pe, &ev)) dispatch(&ev); break;
				case PMW_DND_DROP: {   /* 拖進來的檔案：每個開一個分頁 */
					char **dropped;
					int nd = pmwl_dnd_files(&dropped);
					for (int i = 0; i < nd; i++) open_path(dropped[i]);
					if (nd > 0) { pm_dnd_free(dropped, nd); set_title(); dirty = 1; }
					break;
				}
				default: if (pmwl_to_xevent(&pe, &ev)) dispatch(&ev);
				}
			} while (pmwl_next(&pe, 0));
		}
	}
	pm_conf_watch(dpy);
	XMapWindow(dpy, win);

	int xfd = ConnectionNumber(dpy);
	long next_check = 0;
	for (;;) {
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (XFilterEvent(&ev, None)) continue;
			dispatch(&ev);
		}
		long now = pm_now_ms();
		if (now >= next_check) { if (check_disk()) dirty = 1; next_check = now + 1000; }   /* 每秒看一次檔案有沒有被別的程式改過 */
		if (dirty) { draw(); dirty = 0; }
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		now = pm_now_ms();
		int any = 0;
		for (int i = 0; i < ndocs && !any; i++) any = docs[i]->disk_ok;
		long wait = message_until > now ? message_until - now + 5 : -1;
		if (any && (wait < 0 || next_check - now < wait)) wait = MAX(0, next_check - now);
		struct timeval tv = { wait / 1000, (wait % 1000) * 1000 };
		if (select(xfd + 1, &fds, NULL, NULL, wait >= 0 ? &tv : NULL) == 0 && message_until && pm_now_ms() >= message_until) {
			message_until = 0;   /* 狀態列的訊息時間到了：重畫一次就好（每秒檢查檔案時不用重畫） */
			dirty = 1;
		}
	}
}
