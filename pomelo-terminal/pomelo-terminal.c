/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pomelo 終端機 — 輕量的 X11 終端機模擬器
 *
 * 相容 xterm-256color：256 色 / 全彩、替代畫面、滑鼠回報、括號貼上、
 * 中文寬字元、XIM 輸入法、捲動緩衝、選取複製。
 * 兩個後端：X11（Xlib＋Xft 畫字）；Wayland 工作階段走原生視窗（libpomelo 的 pmwl_*），用 cairo（FreeType＋fontconfig）畫字，
 * 字型選擇、備用字型、格子大小的算法跟 Xft 版一樣。POMELO_BACKEND=x11 可強制走 X。
 * 依賴：Xlib、Xft、fontconfig、cairo。
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <pomelo.h>
#include <pomelo-settings.h>
#include <pomelo-dnd.h>
#include <pomelo-wl.h>
#include <poll.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo-ft.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#include "terminal-config.h"

#define LEN(a)         (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)      ((a) > (b) ? (a) : (b))
#define MIN(a, b)      ((a) < (b) ? (a) : (b))
#define CLAMP(v, a, b) ((v) < (a) ? (a) : (v) > (b) ? (b) : (v))

/* 儲存格屬性 */
enum {
	A_BOLD = 1 << 0, A_DIM = 1 << 1, A_ITALIC = 1 << 2, A_UNDER = 1 << 3,
	A_BLINK = 1 << 4, A_REV = 1 << 5, A_INVIS = 1 << 6, A_STRIKE = 1 << 7,
	A_WIDE = 1 << 8, A_WDUMMY = 1 << 9, A_WRAP = 1 << 10,
};
#define DEFFG 256u
#define DEFBG 257u
#define TRUEC(r, g, b) (0x1000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define ISTRUE(c)      ((c) & 0x1000000u)

/* 終端機模式 */
enum {
	M_INSERT = 1 << 0, M_ORIGIN = 1 << 1, M_WRAP = 1 << 2, M_APPCUR = 1 << 3,
	M_APPKP = 1 << 4, M_ALT = 1 << 5, M_HIDE = 1 << 6, M_BRPASTE = 1 << 7,
	M_MOUSE = 1 << 8, M_MOUSEBTN = 1 << 9, M_MOUSEANY = 1 << 10, M_MOUSESGR = 1 << 11,
	M_FOCUS = 1 << 12, M_REVERSE = 1 << 13, M_SYNC = 1 << 14, M_MOUSEX10 = 1 << 15,
};
#define M_MOUSEMASK (M_MOUSE | M_MOUSEBTN | M_MOUSEANY | M_MOUSEX10)

typedef struct { uint32_t u, fg, bg; uint16_t a; } Cell;
typedef struct { Cell *c; int len; } HLine;
typedef struct { int x, y, wrapnext, origin, gl, charset[4]; Cell pen; } Saved;

typedef struct {
	int cols, rows;
	Cell **line, **pri, **alt;
	int cx, cy, wrapnext;
	Cell pen;
	int top, bot;
	unsigned mode;
	int charset[4], gl, icharset;
	Saved sav[2];
	char *tabs;
	char *dirty;
	uint32_t lastc;
	int cursor_style;
} Term;
static Term t;

/* 捲動緩衝（環狀） */
static HLine *hist;
static int hcap, hn, hhead, scrolloff;

/* 解析器 */
enum { S_GROUND, S_ESC, S_CSI, S_STR, S_CHARSET, S_ESCHASH, S_IGNORE1 };
static int pstate;
typedef struct { int p[32], sub[32], n; char priv, inter; } Csi;
static Csi csi;
static char strbuf[8192];
static int strn, strtype;
static uint32_t utf_cp;
static int utf_need;

/* X 相關（dpy、scr 用 libpomelo 的：右鍵選單 pm_popup 要用同一條連線） */
static Visual *vis;
static Colormap cmap;
static Window win;
static PmWl *wl;                /* 原生 Wayland 視窗；NULL＝X 後端 */
static int native;
static char preedit[512];       /* 原生：輸入法還沒確定的字，畫在游標處 */
static Pixmap buf;
static XftDraw *xd;
static GC tgc;
static XIM xim;
static XIC xic;
static int winw, winh, focused = 1, full_redraw = 1, ocx = -1, ocy = -1;
static XftFont *fnt[4], *tabfont;
static FcPattern *fpat[4];
static struct { XftFont *f; int style; } fb[48];
static int nfb;
static uint32_t missing[256];
static int cw, chh, ascent;
static double fsize, fsize0;
/* 彩色 emoji：Xft 畫不出彩色的字，這些字改用 cairo（FreeType）畫在同一張 pixmap 上 */
static cairo_font_face_t *emoji_face;
static cairo_scaled_font_t *emoji_font;
static int emoji_tried;
static cairo_surface_t *bufsurf;
static cairo_t *bufcr;
typedef struct { uint32_t u; unsigned long idx; double xb, yb, w, h; } EGlyph;
static EGlyph ecache[256];
static uint32_t palrgb[256], fgcol, bgcol, curcol, selcol;
static Atom a_delete, a_utf8, a_targets, a_clipboard, a_paste, a_netname, a_neticon, a_netpid, a_text;
static long sync_since;

/* 選取 */
typedef struct { int active, dragging, snap; int sx, sy, ex, ey; } Sel;
static Sel sel;
static char *selbuf, *clipbuf;
static Time lastclick, evtime;
static int clicks, lastcx = -1, lastcy = -1;
static int mouse_btn = -1, mouse_lx = -1, mouse_ly = -1;

/* 子行程 */
static pid_t child;
static int ptyfd = -1;
static int in_feed;   /* 正在解析輸出時不能再讀，否則會蓋掉還沒處理完的資料 */
static char **cmd_argv;
static const char *cmd_dir, *cmd_title;
static int pty_dead;     /* shell 結束了（這個分頁要關掉） */

/* 分頁：每個分頁一份完整的終端機狀態。目前的分頁放在上面那些全域變數裡，
 * 切換或處理背景分頁的輸出時整份換進換出，原本的程式碼不用改。 */
typedef struct {
	Term t;
	HLine *hist;
	int hcap, hn, hhead, scrolloff;
	int pstate, strn, strtype, utf_need;
	Csi csi;
	char strbuf[8192];
	uint32_t utf_cp;
	Sel sel;
	int mouse_btn, mouse_lx, mouse_ly;
	pid_t child;
	int ptyfd, dead;
	long sync_since;
	char title[256];
} Tab;
static Tab *tab[32];
static int ntabs, cur;
static int toff;          /* 分頁列高度（只有一個分頁時是 0） */
static int bg_ctx;        /* 正在處理背景分頁：不要動視窗標題 */
static char cur_title[256];

static void ttywrite(const char *s, size_t n);
static void tresize(int cols, int rows);
static void draw_tabbar(void);
static void relayout(void);
static void sel_clear(void);
static void handle(XEvent *e);
static void term_clear(void);

static void die(const char *msg)
{
	fprintf(stderr, "pomelo-terminal: %s\n", msg);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p) die(_("記憶體不足"));
	return p;
}

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------ */
/* 畫面緩衝                                                            */
/* ------------------------------------------------------------------ */

static Cell blank(void)
{
	/* 清除時沿用目前的背景色(BCE) */
	Cell c = { ' ', t.pen.fg, t.pen.bg, 0 };
	return c;
}

static const Cell defcell = { ' ', DEFFG, DEFBG, 0 };

static void setdirty(int a, int b)
{
	for (int i = MAX(a, 0); i <= MIN(b, t.rows - 1); i++)
		t.dirty[i] = 1;
}

static void alldirty(void) { setdirty(0, t.rows - 1); }

static void clear_region(int x1, int y1, int x2, int y2)
{
	x1 = CLAMP(x1, 0, t.cols - 1); x2 = CLAMP(x2, 0, t.cols - 1);
	y1 = CLAMP(y1, 0, t.rows - 1); y2 = CLAMP(y2, 0, t.rows - 1);
	Cell b = blank();
	for (int y = y1; y <= y2; y++) {
		for (int x = x1; x <= x2; x++)
			t.line[y][x] = b;
		t.dirty[y] = 1;
	}
}

static void hist_clear(void)
{
	for (int i = 0; i < hn; i++)
		free(hist[(hhead + i) % hcap].c);
	hn = hhead = scrolloff = 0;
	sel_clear();
}

static void hist_push(Cell *src)
{
	if (!hcap) return;
	int len = t.cols;
	while (len > 0 && src[len - 1].u == ' ' && src[len - 1].bg == DEFBG &&
	       !(src[len - 1].a & (A_REV | A_UNDER | A_WRAP)))
		len--;
	HLine *h;
	if (hn < hcap) {
		h = &hist[(hhead + hn) % hcap];
		hn++;
	} else {
		h = &hist[hhead];
		hhead = (hhead + 1) % hcap;
		free(h->c);
		/* 最舊的一行被丟掉，選取範圍跟著往上移 */
		if (sel.active) { sel.sy--; sel.ey--; if (sel.sy < 0 || sel.ey < 0) sel_clear(); }
	}
	h->len = len;
	h->c = len ? xmalloc(len * sizeof(Cell)) : NULL;
	if (len) memcpy(h->c, src, len * sizeof(Cell));
	if (scrolloff) scrolloff = MIN(scrolloff + 1, hn);
}

/* 第 v 行（0 = 捲動緩衝中最舊的一行） */
static Cell *vline(int v, int *len)
{
	if (v < hn) {
		HLine *h = &hist[(hhead + v) % hcap];
		*len = h->len;
		return h->c;
	}
	*len = t.cols;
	return t.line[CLAMP(v - hn, 0, t.rows - 1)];
}

static void scroll_up(int top, int n, int save)
{
	n = CLAMP(n, 0, t.bot - top + 1);
	if (n <= 0) return;
	Cell *tmp[n];
	for (int i = 0; i < n; i++) {
		if (save && top == 0 && t.line == t.pri)
			hist_push(t.line[top + i]);
		tmp[i] = t.line[top + i];
	}
	if (!(save && top == 0 && t.line == t.pri) && sel.active && !sel.dragging)
		sel_clear();
	memmove(&t.line[top], &t.line[top + n], (t.bot - top + 1 - n) * sizeof(Cell *));
	for (int i = 0; i < n; i++)
		t.line[t.bot - n + 1 + i] = tmp[i];
	clear_region(0, t.bot - n + 1, t.cols - 1, t.bot);
	setdirty(top, t.bot);
}

static void scroll_down(int top, int n)
{
	n = CLAMP(n, 0, t.bot - top + 1);
	if (n <= 0) return;
	Cell *tmp[n];
	for (int i = 0; i < n; i++)
		tmp[i] = t.line[t.bot - n + 1 + i];
	memmove(&t.line[top + n], &t.line[top], (t.bot - top + 1 - n) * sizeof(Cell *));
	for (int i = 0; i < n; i++)
		t.line[top + i] = tmp[i];
	clear_region(0, top, t.cols - 1, top + n - 1);
	setdirty(top, t.bot);
	if (sel.active && !sel.dragging) sel_clear();
}

static void moveto(int x, int y)
{
	int miny = 0, maxy = t.rows - 1;
	if (t.mode & M_ORIGIN) { miny = t.top; maxy = t.bot; }
	t.cx = CLAMP(x, 0, t.cols - 1);
	t.cy = CLAMP(y, miny, maxy);
	t.wrapnext = 0;
}

static void moveto_abs(int x, int y) { moveto(x, y + ((t.mode & M_ORIGIN) ? t.top : 0)); }

static void newline(int cr)
{
	if (t.cy == t.bot)
		scroll_up(t.top, 1, 1);
	else if (t.cy < t.rows - 1)
		t.cy++;
	if (cr) t.cx = 0;
	t.wrapnext = 0;
}

static void save_cursor(void)
{
	Saved *s = &t.sav[!!(t.mode & M_ALT)];
	s->x = t.cx; s->y = t.cy; s->wrapnext = t.wrapnext; s->pen = t.pen;
	s->origin = !!(t.mode & M_ORIGIN); s->gl = t.gl;
	memcpy(s->charset, t.charset, sizeof t.charset);
}

static void restore_cursor(void)
{
	Saved *s = &t.sav[!!(t.mode & M_ALT)];
	t.pen = s->pen; t.gl = s->gl;
	memcpy(t.charset, s->charset, sizeof t.charset);
	if (s->origin) t.mode |= M_ORIGIN; else t.mode &= ~M_ORIGIN;
	t.cx = CLAMP(s->x, 0, t.cols - 1);
	t.cy = CLAMP(s->y, 0, t.rows - 1);
	t.wrapnext = s->wrapnext;
}

static void swap_screen(int to_alt)
{
	if (to_alt == !!(t.mode & M_ALT)) return;
	t.line = to_alt ? t.alt : t.pri;
	t.mode ^= M_ALT;
	scrolloff = 0;
	sel_clear();
	alldirty();
}

static void reset_tabs(void)
{
	for (int i = 0; i < t.cols; i++)
		t.tabs[i] = i && i % 8 == 0;
}

static void treset(void)
{
	t.pen = defcell;
	t.mode = M_WRAP;
	t.top = 0; t.bot = t.rows - 1;
	memset(t.charset, 0, sizeof t.charset);
	t.gl = 0;
	t.cursor_style = 1;
	reset_tabs();
	swap_screen(1);
	clear_region(0, 0, t.cols - 1, t.rows - 1);
	swap_screen(0);
	clear_region(0, 0, t.cols - 1, t.rows - 1);
	t.cx = t.cy = t.wrapnext = 0;
	save_cursor();
	t.sav[1] = t.sav[0];
}

static Cell **alloc_screen(int cols, int rows)
{
	Cell **s = xmalloc(rows * sizeof(Cell *));
	for (int y = 0; y < rows; y++) {
		s[y] = xmalloc(cols * sizeof(Cell));
		for (int x = 0; x < cols; x++) s[y][x] = defcell;
	}
	return s;
}

static void free_screen(Cell **s, int rows)
{
	if (!s) return;
	for (int y = 0; y < rows; y++) free(s[y]);
	free(s);
}

static void tresize(int cols, int rows)
{
	cols = MAX(cols, 2);
	rows = MAX(rows, 1);
	if (cols == t.cols && rows == t.rows)
		return;
	int onalt = !!(t.mode & M_ALT);
	Cell **np = alloc_screen(cols, rows), **na = alloc_screen(cols, rows);

	/* 主畫面：變矮時游標以上的行推進捲動緩衝；變高時從緩衝拉回來 */
	int shift = 0, pull = 0;
	int pcy = onalt ? t.sav[0].y : t.cy;
	if (t.pri) {
		shift = MAX(0, pcy - (rows - 1));
		for (int i = 0; i < shift; i++)
			hist_push(t.pri[i]);
		if (rows > t.rows && !onalt)
			pull = MIN(hn, rows - t.rows);
		for (int i = 0; i < pull; i++) {
			HLine *h = &hist[(hhead + hn - pull + i) % hcap];
			memcpy(np[i], h->c, MIN(h->len, cols) * sizeof(Cell));
		}
		for (int i = 0; i < pull; i++) {
			HLine *h = &hist[(hhead + hn - 1 - i) % hcap];
			free(h->c); h->c = NULL;
		}
		hn -= pull;
		for (int y = 0; y + pull < rows && y + shift < t.rows; y++)
			memcpy(np[y + pull], t.pri[y + shift], MIN(cols, t.cols) * sizeof(Cell));
		int acy = onalt ? t.cy : t.sav[1].y, ashift = MAX(0, acy - (rows - 1));
		for (int y = 0; y < rows && y + ashift < t.rows; y++)
			memcpy(na[y], t.alt[y + ashift], MIN(cols, t.cols) * sizeof(Cell));
		if (onalt) { t.cy -= ashift; t.sav[0].y += pull - shift; }
		else { t.cy += pull - shift; t.sav[1].y -= ashift; }
	}
	free_screen(t.pri, t.rows);
	free_screen(t.alt, t.rows);
	t.pri = np; t.alt = na;
	t.line = onalt ? na : np;
	t.cols = cols; t.rows = rows;
	free(t.tabs); t.tabs = xmalloc(cols);
	free(t.dirty); t.dirty = xmalloc(rows);
	reset_tabs();
	t.top = 0; t.bot = rows - 1;
	t.cx = CLAMP(t.cx, 0, cols - 1); t.cy = CLAMP(t.cy, 0, rows - 1);
	for (int i = 0; i < 2; i++) {
		t.sav[i].x = CLAMP(t.sav[i].x, 0, cols - 1);
		t.sav[i].y = CLAMP(t.sav[i].y, 0, rows - 1);
	}
	t.wrapnext = 0;
	scrolloff = 0;
	sel_clear();
	alldirty();
	full_redraw = 1;
}

/* ------------------------------------------------------------------ */
/* 寫入文字                                                            */
/* ------------------------------------------------------------------ */

static const uint16_t vt100_gfx[32] = {
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1, 0x2424, 0x240b,
	0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c,
	0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static void clear_wide(int x, int y)
{
	Cell *l = t.line[y];
	if ((l[x].a & A_WDUMMY) && x > 0) { l[x - 1].u = ' '; l[x - 1].a &= ~A_WIDE; }
	if ((l[x].a & A_WIDE) && x + 1 < t.cols) { l[x + 1].u = ' '; l[x + 1].a &= ~A_WDUMMY; }
}

static void tputc(uint32_t u)
{
	if (t.charset[t.gl] && u >= 0x5f && u <= 0x7e)
		u = vt100_gfx[u - 0x5f];
	int w = u < 0x80 ? 1 : wcwidth((wchar_t)u);
	if (w < 0) w = 1;
	if (w == 0) return;              /* 組合字元：略過 */
	if (w > 2) w = 2;

	if (t.wrapnext && (t.mode & M_WRAP)) {
		t.line[t.cy][t.cols - 1].a |= A_WRAP;
		newline(1);
	}
	if (w == 2 && t.cx == t.cols - 1) {
		if (t.mode & M_WRAP) {
			t.line[t.cy][t.cx] = blank();
			t.line[t.cy][t.cx].a |= A_WRAP;
			newline(1);
		} else {
			w = 1;
		}
	}
	Cell *l = t.line[t.cy];
	if ((t.mode & M_INSERT) && t.cx + w < t.cols)
		memmove(&l[t.cx + w], &l[t.cx], (t.cols - t.cx - w) * sizeof(Cell));
	clear_wide(t.cx, t.cy);
	if (w == 2) clear_wide(t.cx + 1, t.cy);
	Cell *c = &l[t.cx];
	*c = t.pen;
	c->u = u;
	c->a &= ~(A_WIDE | A_WDUMMY | A_WRAP);
	if (w == 2) {
		c->a |= A_WIDE;
		c[1] = t.pen;
		c[1].u = 0;
		c[1].a = (c[1].a & ~(A_WIDE | A_WRAP)) | A_WDUMMY;
	}
	t.dirty[t.cy] = 1;
	t.lastc = u;
	if (t.cx + w < t.cols) {
		t.cx += w;
	} else {
		t.cx = t.cols - 1;
		t.wrapnext = 1;
	}
}

/* ------------------------------------------------------------------ */
/* 控制序列                                                            */
/* ------------------------------------------------------------------ */

#define ARG(i, d) (csi.n > (i) && csi.p[i] ? csi.p[i] : (d))

static int ext_color(int i, uint32_t *out)
{
	if (csi.sub[i]) {   /* 冒號格式：38:2::r:g:b 或 38:5:n */
		int g[8], k = 0, j = i + 1;
		for (; j < csi.n && k < 8; j++) {
			g[k++] = csi.p[j];
			if (!csi.sub[j]) break;
		}
		if (k >= 2 && g[0] == 5) *out = g[1] & 255;
		else if (k >= 5 && g[0] == 2) *out = TRUEC(g[2] & 255, g[3] & 255, g[4] & 255);
		else if (k == 4 && g[0] == 2) *out = TRUEC(g[1] & 255, g[2] & 255, g[3] & 255);
		return MIN(j, csi.n - 1) - i;
	}
	if (i + 2 < csi.n && csi.p[i + 1] == 5) {
		*out = csi.p[i + 2] & 255;
		return 2;
	}
	if (i + 4 < csi.n && csi.p[i + 1] == 2) {
		*out = TRUEC(csi.p[i + 2] & 255, csi.p[i + 3] & 255, csi.p[i + 4] & 255);
		return 4;
	}
	return csi.n - 1 - i;
}

static void sgr(void)
{
	if (csi.n == 0) {
		t.pen.a = 0; t.pen.fg = DEFFG; t.pen.bg = DEFBG;
		return;
	}
	for (int i = 0; i < csi.n; i++) {
		int v = csi.p[i];
		uint32_t col;
		switch (v) {
		case 0: t.pen.a = 0; t.pen.fg = DEFFG; t.pen.bg = DEFBG; break;
		case 1: t.pen.a |= A_BOLD; break;
		case 2: t.pen.a |= A_DIM; break;
		case 3: t.pen.a |= A_ITALIC; break;
		case 4:
			if (csi.sub[i] && i + 1 < csi.n) {
				if (csi.p[i + 1]) t.pen.a |= A_UNDER; else t.pen.a &= ~A_UNDER;
				i++;
			} else {
				t.pen.a |= A_UNDER;
			}
			break;
		case 5: case 6: t.pen.a |= A_BLINK; break;
		case 7: t.pen.a |= A_REV; break;
		case 8: t.pen.a |= A_INVIS; break;
		case 9: t.pen.a |= A_STRIKE; break;
		case 21: t.pen.a |= A_UNDER; break;
		case 22: t.pen.a &= ~(A_BOLD | A_DIM); break;
		case 23: t.pen.a &= ~A_ITALIC; break;
		case 24: t.pen.a &= ~A_UNDER; break;
		case 25: t.pen.a &= ~A_BLINK; break;
		case 27: t.pen.a &= ~A_REV; break;
		case 28: t.pen.a &= ~A_INVIS; break;
		case 29: t.pen.a &= ~A_STRIKE; break;
		case 38: col = DEFFG; i += ext_color(i, &col); t.pen.fg = col; break;
		case 39: t.pen.fg = DEFFG; break;
		case 48: col = DEFBG; i += ext_color(i, &col); t.pen.bg = col; break;
		case 49: t.pen.bg = DEFBG; break;
		case 58: col = 0; i += ext_color(i, &col); break;   /* 底線顏色：不支援 */
		default:
			if (v >= 30 && v <= 37) t.pen.fg = v - 30;
			else if (v >= 40 && v <= 47) t.pen.bg = v - 40;
			else if (v >= 90 && v <= 97) t.pen.fg = v - 90 + 8;
			else if (v >= 100 && v <= 107) t.pen.bg = v - 100 + 8;
		}
	}
}

static void set_mode(int set)
{
	for (int i = 0; i < MAX(csi.n, 1); i++) {
		int v = csi.p[i];
		if (csi.priv != '?') {
			if (v == 4) t.mode = set ? t.mode | M_INSERT : t.mode & ~M_INSERT;
			continue;
		}
		unsigned f = 0;
		switch (v) {
		case 1: f = M_APPCUR; break;
		case 5: f = M_REVERSE; alldirty(); break;
		case 6: f = M_ORIGIN; break;
		case 7: f = M_WRAP; break;
		case 25:
			if (set) t.mode &= ~M_HIDE; else t.mode |= M_HIDE;
			t.dirty[t.cy] = 1;
			continue;
		case 9: case 1000: case 1002: case 1003:
			t.mode &= ~M_MOUSEMASK;
			f = v == 9 ? M_MOUSEX10 : v == 1000 ? M_MOUSE : v == 1002 ? M_MOUSEBTN : M_MOUSEANY;
			break;
		case 1004: f = M_FOCUS; break;
		case 1006: f = M_MOUSESGR; break;
		case 2004: f = M_BRPASTE; break;
		case 2026: f = M_SYNC; if (set) sync_since = now_ms(); break;
		case 47: case 1047:
			if (!set && v == 1047 && (t.mode & M_ALT)) clear_region(0, 0, t.cols - 1, t.rows - 1);
			swap_screen(set);
			continue;
		case 1048:
			if (set) save_cursor(); else restore_cursor();
			continue;
		case 1049:
			if (set) {
				save_cursor();
				swap_screen(1);
				Cell keep = t.pen;
				t.pen = defcell;
				clear_region(0, 0, t.cols - 1, t.rows - 1);
				t.pen = keep;
			} else {
				swap_screen(0);
				restore_cursor();
			}
			continue;
		default: continue;
		}
		if (set) t.mode |= f; else t.mode &= ~f;
		if (v == 6) moveto_abs(0, 0);
	}
	if (native) return;   /* 原生視窗一律收滑鼠移動，報告與否在 bmotion 判斷 */
	/* 只在需要時才接收滑鼠移動事件 */
	XSetWindowAttributes wa;
	wa.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | ExposureMask |
	                StructureNotifyMask | FocusChangeMask |
	                ((t.mode & M_MOUSEANY) ? PointerMotionMask : ButtonMotionMask);
	if (xic) {
		long fe = 0;
		XGetICValues(xic, XNFilterEvents, &fe, NULL);
		wa.event_mask |= fe;
	}
	XChangeWindowAttributes(dpy, win, CWEventMask, &wa);
}

static void csi_dispatch(int f)
{
	char out[64];
	int n = ARG(0, 1);
	Cell *l = t.line[t.cy];
	if (csi.priv == '>' || csi.priv == '<' || csi.priv == '=') {
		if (f == 'c' && csi.priv == '>') ttywrite("\033[>1;10;0c", 10);
		return;
	}
	switch (f) {
	case '@':
		n = MIN(n, t.cols - t.cx);
		memmove(&l[t.cx + n], &l[t.cx], (t.cols - t.cx - n) * sizeof(Cell));
		clear_region(t.cx, t.cy, t.cx + n - 1, t.cy);
		break;
	case 'A': moveto(t.cx, t.cy - n); break;
	case 'B': case 'e': moveto(t.cx, t.cy + n); break;
	case 'C': case 'a': moveto(t.cx + n, t.cy); break;
	case 'D': moveto(t.cx - n, t.cy); break;
	case 'E': moveto(0, t.cy + n); break;
	case 'F': moveto(0, t.cy - n); break;
	case 'G': case '`': moveto(n - 1, t.cy); break;
	case 'H': case 'f': moveto_abs(ARG(1, 1) - 1, ARG(0, 1) - 1); break;
	case 'I': while (n--) { while (t.cx < t.cols - 1 && !t.tabs[++t.cx]); } break;
	case 'Z': while (n--) { while (t.cx > 0 && !t.tabs[--t.cx]); } break;
	case 'J':
		switch (csi.n ? csi.p[0] : 0) {
		case 0:
			clear_region(t.cx, t.cy, t.cols - 1, t.cy);
			if (t.cy < t.rows - 1) clear_region(0, t.cy + 1, t.cols - 1, t.rows - 1);
			break;
		case 1:
			if (t.cy > 0) clear_region(0, 0, t.cols - 1, t.cy - 1);
			clear_region(0, t.cy, t.cx, t.cy);
			break;
		case 2: clear_region(0, 0, t.cols - 1, t.rows - 1); break;
		case 3: if (!(t.mode & M_ALT)) { hist_clear(); alldirty(); } break;
		}
		break;
	case 'K':
		switch (csi.n ? csi.p[0] : 0) {
		case 0: clear_region(t.cx, t.cy, t.cols - 1, t.cy); break;
		case 1: clear_region(0, t.cy, t.cx, t.cy); break;
		case 2: clear_region(0, t.cy, t.cols - 1, t.cy); break;
		}
		break;
	case 'L': if (t.cy >= t.top && t.cy <= t.bot) scroll_down(t.cy, n); break;
	case 'M': if (t.cy >= t.top && t.cy <= t.bot) scroll_up(t.cy, n, 0); break;
	case 'P':
		n = MIN(n, t.cols - t.cx);
		memmove(&l[t.cx], &l[t.cx + n], (t.cols - t.cx - n) * sizeof(Cell));
		clear_region(t.cols - n, t.cy, t.cols - 1, t.cy);
		break;
	case 'S': scroll_up(t.top, n, 0); break;
	case 'T': scroll_down(t.top, n); break;
	case 'X': clear_region(t.cx, t.cy, t.cx + n - 1, t.cy); break;
	case 'b': n = MIN(n, 65535); while (n--) tputc(t.lastc ? t.lastc : ' '); break;
	case 'c': if (!csi.priv && !(csi.n ? csi.p[0] : 0)) ttywrite("\033[?62;22c", 9); break;
	case 'd': moveto_abs(t.cx, n - 1); break;
	case 'g':
		if (!(csi.n ? csi.p[0] : 0)) t.tabs[t.cx] = 0;
		else if (csi.p[0] == 3) memset(t.tabs, 0, t.cols);
		break;
	case 'h': set_mode(1); break;
	case 'l': set_mode(0); break;
	case 'm': if (!csi.priv) sgr(); break;
	case 'n':
		if ((csi.n ? csi.p[0] : 0) == 5) ttywrite("\033[0n", 4);
		else if ((csi.n ? csi.p[0] : 0) == 6) {
			int y = t.cy + 1 - ((t.mode & M_ORIGIN) ? t.top : 0);
			int k = snprintf(out, sizeof out, csi.priv == '?' ? "\033[?%d;%dR" : "\033[%d;%dR", y, t.cx + 1);
			ttywrite(out, k);
		}
		break;
	case 'p':
		if (csi.inter == '!') {   /* DECSTR 軟重置 */
			t.pen = defcell;
			t.mode = M_WRAP | (t.mode & M_ALT);
			t.top = 0; t.bot = t.rows - 1;
			t.charset[0] = t.gl = 0;
		}
		break;
	case 'q':
		if (csi.inter == ' ') { t.cursor_style = ARG(0, 1); t.dirty[t.cy] = 1; }
		break;
	case 'r':
		if (!csi.priv) {
			int top = ARG(0, 1) - 1, bot = ARG(1, t.rows) - 1;
			top = CLAMP(top, 0, t.rows - 1); bot = CLAMP(bot, 0, t.rows - 1);
			if (top < bot) { t.top = top; t.bot = bot; }
			moveto_abs(0, 0);
		}
		break;
	case 's': if (!csi.priv) save_cursor(); break;
	case 'u': if (!csi.priv) restore_cursor(); break;
	case 't':
		if ((csi.n ? csi.p[0] : 0) == 18) {
			int k = snprintf(out, sizeof out, "\033[8;%d;%dt", t.rows, t.cols);
			ttywrite(out, k);
		}
		break;
	}
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64decode(const char *s)
{
	size_t n = strlen(s);
	char *out = xmalloc(n * 3 / 4 + 4), *o = out;
	unsigned v = 0; int bits = 0;
	for (; *s; s++) {
		const char *p = strchr(b64tab, *s);
		if (!p || *s == '=') continue;
		v = (v << 6) | (unsigned)(p - b64tab);
		bits += 6;
		if (bits >= 8) { bits -= 8; *o++ = (char)((v >> bits) & 255); }
	}
	*o = 0;
	return out;
}

static void set_title(const char *s)
{
	snprintf(cur_title, sizeof cur_title, "%s", s);
	full_redraw = 1;   /* 分頁列上的標題 */
	if (bg_ctx) return;
	if (native) { pmwl_set_title(wl, s); return; }
	XStoreName(dpy, win, s);
	XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (const unsigned char *)s, strlen(s));
}

static void own_selection(Atom which, char *text);

static void str_dispatch(void)
{
	strbuf[strn] = 0;
	if (strtype != ']')
		return;   /* DCS / APC / PM / SOS：忽略 */
	char *semi = strchr(strbuf, ';');
	int code = atoi(strbuf);
	char *arg = semi ? semi + 1 : (char *)"";
	char out[96];
	switch (code) {
	case 0: case 2:
		if (!cmd_title) set_title(arg);
		break;
	case 10: case 11: case 12:
		if (!strcmp(arg, "?")) {
			uint32_t c = code == 10 ? fgcol : code == 11 ? bgcol : curcol;
			int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
			int k = snprintf(out, sizeof out, "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", code, r, r, g, g, b, b);
			ttywrite(out, k);
		}
		break;
	case 52: {   /* 程式（例如 tmux、nvim）設定剪貼簿 */
		char *data = strchr(arg, ';');
		if (data && strcmp(data + 1, "?"))
			own_selection(a_clipboard, b64decode(data + 1));
		break;
	}
	}
}

static void esc_dispatch(uint32_t u)
{
	pstate = S_GROUND;
	switch (u) {
	case '[': pstate = S_CSI; memset(&csi, 0, sizeof csi); break;
	case ']': case 'P': case '_': case '^': case 'X': pstate = S_STR; strtype = (int)u; strn = 0; break;
	case '(': case ')': case '*': case '+': t.icharset = (int)u - '('; pstate = S_CHARSET; break;
	case '#': pstate = S_ESCHASH; break;
	case '%': case ' ': pstate = S_IGNORE1; break;
	case '7': save_cursor(); break;
	case '8': restore_cursor(); break;
	case 'D': newline(0); break;
	case 'E': newline(1); break;
	case 'H': t.tabs[t.cx] = 1; break;
	case 'M':
		if (t.cy == t.top) scroll_down(t.top, 1);
		else if (t.cy > 0) t.cy--;
		t.wrapnext = 0;
		break;
	case 'c': treset(); hist_clear(); break;
	case '=': t.mode |= M_APPKP; break;
	case '>': t.mode &= ~M_APPKP; break;
	case 'Z': ttywrite("\033[?62;22c", 9); break;
	}
}

static void tcontrol(uint32_t c)
{
	switch (c) {
	case '\a':
		if (pstate == S_STR) { str_dispatch(); pstate = S_GROUND; }
		break;
	case '\b': if (t.cx > 0) t.cx--; t.wrapnext = 0; break;
	case '\t': while (t.cx < t.cols - 1 && !t.tabs[++t.cx]); t.wrapnext = 0; break;
	case '\n': case '\v': case '\f': newline(0); break;
	case '\r': t.cx = 0; t.wrapnext = 0; break;
	case 0x0e: t.gl = 1; break;
	case 0x0f: t.gl = 0; break;
	case 0x18: case 0x1a: pstate = S_GROUND; break;
	case 0x1b:
		if (pstate == S_STR) str_dispatch();
		pstate = S_ESC;
		break;
	}
}

static void tput(uint32_t u)
{
	if (pstate == S_STR) {
		if (u == 0x07 || u == 0x1b || u == 0x18 || u == 0x1a) { tcontrol(u); return; }
		if (u < 0x20) return;
		char tmp[4]; int k;
		if (u < 0x80) { tmp[0] = (char)u; k = 1; }
		else if (u < 0x800) { tmp[0] = (char)(0xc0 | (u >> 6)); tmp[1] = (char)(0x80 | (u & 63)); k = 2; }
		else if (u < 0x10000) { tmp[0] = (char)(0xe0 | (u >> 12)); tmp[1] = (char)(0x80 | ((u >> 6) & 63)); tmp[2] = (char)(0x80 | (u & 63)); k = 3; }
		else { tmp[0] = (char)(0xf0 | (u >> 18)); tmp[1] = (char)(0x80 | ((u >> 12) & 63)); tmp[2] = (char)(0x80 | ((u >> 6) & 63)); tmp[3] = (char)(0x80 | (u & 63)); k = 4; }
		if (strn + k < (int)sizeof strbuf - 1) { memcpy(strbuf + strn, tmp, k); strn += k; }
		return;
	}
	if (u < 0x20) { tcontrol(u); return; }
	if (u == 0x7f || (u >= 0x80 && u < 0xa0)) return;
	switch (pstate) {
	case S_GROUND: tputc(u); break;
	case S_ESC: esc_dispatch(u); break;
	case S_CSI:
		if (u >= '0' && u <= '9') {
			if (!csi.n) csi.n = 1;
			csi.p[csi.n - 1] = MIN(csi.p[csi.n - 1] * 10 + (int)(u - '0'), 65535);
		} else if (u == ';' || u == ':') {
			if (!csi.n) csi.n = 1;
			if (csi.n < (int)LEN(csi.p)) {
				csi.sub[csi.n - 1] = u == ':';
				csi.p[csi.n++] = 0;
			}
		} else if (u >= 0x3c && u <= 0x3f) {
			csi.priv = (char)u;
		} else if (u >= 0x20 && u <= 0x2f) {
			csi.inter = (char)u;
		} else if (u >= 0x40 && u <= 0x7e) {
			pstate = S_GROUND;
			csi_dispatch((int)u);
		} else {
			pstate = S_GROUND;
		}
		break;
	case S_CHARSET:
		t.charset[t.icharset & 3] = u == '0';
		pstate = S_GROUND;
		break;
	case S_ESCHASH:
		if (u == '8') {   /* DECALN：整個畫面填滿 E */
			for (int y = 0; y < t.rows; y++)
				for (int x = 0; x < t.cols; x++) { t.line[y][x] = defcell; t.line[y][x].u = 'E'; }
			alldirty();
		}
		pstate = S_GROUND;
		break;
	default:
		pstate = S_GROUND;
	}
}

static void feed(const unsigned char *s, ssize_t n)
{
	for (ssize_t i = 0; i < n; i++) {
		unsigned b = s[i];
		if (utf_need) {
			if ((b & 0xc0) == 0x80) {
				utf_cp = (utf_cp << 6) | (b & 0x3f);
				if (--utf_need == 0) tput(utf_cp);
				continue;
			}
			utf_need = 0;
			tput(0xfffd);
		}
		if (b < 0x80) tput(b);
		else if ((b & 0xe0) == 0xc0) { utf_cp = b & 0x1f; utf_need = 1; }
		else if ((b & 0xf0) == 0xe0) { utf_cp = b & 0x0f; utf_need = 2; }
		else if ((b & 0xf8) == 0xf0) { utf_cp = b & 0x07; utf_need = 3; }
		else tput(0xfffd);
	}
}

/* ------------------------------------------------------------------ */
/* PTY                                                                 */
/* ------------------------------------------------------------------ */

static int ttyread(void)
{
	static unsigned char rb[65536];
	ssize_t n = read(ptyfd, rb, sizeof rb);
	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	if (n <= 0) {
		/* shell 結束了：關掉這個分頁（最後一個分頁時整個視窗關閉） */
		pty_dead = 1;
		return -1;
	}
	in_feed = 1;
	feed(rb, n);
	in_feed = 0;
	return (int)n;
}

static void ttywrite(const char *s, size_t n)
{
	while (n > 0) {
		fd_set rf, wf;
		FD_ZERO(&rf); FD_ZERO(&wf);
		if (!in_feed) FD_SET(ptyfd, &rf);
		FD_SET(ptyfd, &wf);
		if (select(ptyfd + 1, &rf, &wf, NULL, NULL) < 0) {
			if (errno == EINTR) continue;
			die(_("select 失敗"));
		}
		if (FD_ISSET(ptyfd, &wf)) {
			ssize_t k = write(ptyfd, s, MIN(n, 256));
			if (k < 0 && errno != EINTR && errno != EAGAIN) die(_("寫入 pty 失敗"));
			if (k > 0) { s += k; n -= (size_t)k; }
		}
		/* 一邊寫一邊讀，避免大量貼上時雙方互相卡住（回應查詢時例外） */
		if (n > 0 && !in_feed && FD_ISSET(ptyfd, &rf) && ttyread() < 0)
			return;
	}
}

static void tty_winsize(void)
{
	struct winsize ws = { (unsigned short)t.rows, (unsigned short)t.cols,
	                      (unsigned short)(t.cols * cw), (unsigned short)(t.rows * chh) };
	if (ptyfd >= 0)
		ioctl(ptyfd, TIOCSWINSZ, &ws);
}

static void spawn_shell(void)
{
	struct winsize ws = { (unsigned short)t.rows, (unsigned short)t.cols,
	                      (unsigned short)(t.cols * cw), (unsigned short)(t.rows * chh) };
	child = forkpty(&ptyfd, NULL, NULL, &ws);
	if (child < 0) die(_("無法建立 pty"));
	if (child == 0) {
		if (!native) {   /* 原生視窗沒有 X 視窗編號 */
			char wid[32];
			snprintf(wid, sizeof wid, "%lu", (unsigned long)win);
			setenv("WINDOWID", wid, 1);
		}
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		setenv("TERM_PROGRAM", "pomelo-terminal", 1);
		unsetenv("COLUMNS");
		unsetenv("LINES");
		int sigs[] = { SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM, SIGPIPE };
		for (unsigned i = 0; i < LEN(sigs); i++) signal(sigs[i], SIG_DFL);
		if (cmd_dir && chdir(cmd_dir) != 0) { /* 目錄不存在就留在原地 */ }
		if (cmd_argv) {
			execvp(cmd_argv[0], cmd_argv);
			perror(cmd_argv[0]);
			_exit(127);
		}
		const char *sh = getenv("SHELL");
		struct passwd *pw = getpwuid(getuid());
		if (!sh || !*sh) sh = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
		const char *base = strrchr(sh, '/') ? strrchr(sh, '/') + 1 : sh;
		char arg0[PATH_MAX];
		snprintf(arg0, sizeof arg0, "%s%s", login_shell ? "-" : "", login_shell ? base : sh);
		execl(sh, arg0, (char *)NULL);
		_exit(127);
	}
}

/* ------------------------------------------------------------------ */
/* 字型與顏色                                                          */
/* ------------------------------------------------------------------ */

/* 系統的彩色 emoji 字型（例如 Noto Color Emoji），縮放到剛好兩格寬、一行高 */
static void emoji_load(void)
{
	if (emoji_font) cairo_scaled_font_destroy(emoji_font);
	emoji_font = NULL;
	memset(ecache, 0, sizeof ecache);
	if (!emoji_tried) {
		emoji_tried = 1;
		FcPattern *p = FcNameParse((const FcChar8 *)"emoji");
#ifdef FC_COLOR
		FcPatternAddBool(p, FC_COLOR, FcTrue);
#endif
		FcConfigSubstitute(NULL, p, FcMatchPattern);
		FcDefaultSubstitute(p);
		FcResult r;
		FcPattern *m = FcFontMatch(NULL, p, &r);
		FcPatternDestroy(p);
		FcBool color = FcFalse;
#ifdef FC_COLOR
		if (m && FcPatternGetBool(m, FC_COLOR, 0, &color) != FcResultMatch) color = FcFalse;
#endif
		if (m && color) emoji_face = cairo_ft_font_face_create_for_pattern(m);
		if (m) FcPatternDestroy(m);
	}
	if (!emoji_face) return;
	cairo_matrix_t fm, ctm;
	cairo_matrix_init_identity(&ctm);
	cairo_font_options_t *fo = cairo_font_options_create();
	double sz = chh;
	cairo_matrix_init_scale(&fm, sz, sz);
	cairo_scaled_font_t *sf = cairo_scaled_font_create(emoji_face, &fm, &ctm, fo);
	cairo_text_extents_t e;
	cairo_scaled_font_text_extents(sf, "\xf0\x9f\x98\x80", &e);   /* 😀 */
	cairo_scaled_font_destroy(sf);
	if (e.width > 0 && e.height > 0) sz *= MIN(2.0 * cw / e.width, (double)chh / e.height) * 0.92;
	cairo_matrix_init_scale(&fm, sz, sz);
	emoji_font = cairo_scaled_font_create(emoji_face, &fm, &ctm, fo);
	cairo_font_options_destroy(fo);
	if (cairo_scaled_font_status(emoji_font)) { cairo_scaled_font_destroy(emoji_font); emoji_font = NULL; }
}

/* 寬字元裡平常用彩色顯示的 emoji 範圍（CJK 文字不在這裡面） */
static int emoji_range(uint32_t u)
{
	return (u >= 0x1F000 && u <= 0x1FAFF) || (u >= 0x2300 && u <= 0x23FF) || (u >= 0x2600 && u <= 0x27BF) ||
	       (u >= 0x2B00 && u <= 0x2BFF) || u == 0x3030 || u == 0x303D || u == 0x3297 || u == 0x3299;
}

/* 畫一個彩色 emoji（佔 cells 格），字型裡沒有這個字回傳 0（照舊用 Xft 畫） */
static int draw_emoji(uint32_t u, int x, int y, int cells)
{
	if (!emoji_font) return 0;
	EGlyph *g = &ecache[u & 255];
	if (g->u != u) {
		char s[8];
		int n = 0;
		if (u < 0x10000) { s[n++] = (char)(0xe0 | (u >> 12)); }
		else { s[n++] = (char)(0xf0 | (u >> 18)); s[n++] = (char)(0x80 | ((u >> 12) & 0x3f)); }
		s[n++] = (char)(0x80 | ((u >> 6) & 0x3f));
		s[n++] = (char)(0x80 | (u & 0x3f));
		cairo_glyph_t *gl = NULL;
		int ng = 0;
		g->u = u;
		g->idx = 0;
		if (cairo_scaled_font_text_to_glyphs(emoji_font, 0, 0, s, n, &gl, &ng, NULL, NULL, NULL) == CAIRO_STATUS_SUCCESS && ng == 1 && gl[0].index) {
			cairo_text_extents_t e;
			cairo_scaled_font_glyph_extents(emoji_font, gl, 1, &e);
			g->idx = gl[0].index;
			g->xb = e.x_bearing; g->yb = e.y_bearing; g->w = e.width; g->h = e.height;
		}
		cairo_glyph_free(gl);
	}
	if (!g->idx) return 0;
	if (!bufcr) {
		bufsurf = cairo_xlib_surface_create(dpy, buf, vis, winw, winh);
		bufcr = cairo_create(bufsurf);
	}
	cairo_set_scaled_font(bufcr, emoji_font);
	/* 字型的大小是照兩格算的；一格的字（例如國旗的字母）縮小放進去 */
	double k = cells >= 2 || g->w <= 0 ? 1 : MIN(1.0, cw * 0.96 / g->w);
	cairo_save(bufcr);
	cairo_translate(bufcr, x + cells * cw / 2.0, y + chh / 2.0);
	cairo_scale(bufcr, k, k);
	cairo_glyph_t gl = { g->idx, -g->w / 2 - g->xb, -g->h / 2 - g->yb };
	cairo_show_glyphs(bufcr, &gl, 1);
	cairo_restore(bufcr);
	cairo_surface_flush(bufsurf);
	return 1;
}

/* ---- 原生 Wayland 後端的字型：cairo（FreeType）＋fontconfig，選字、備用字型的邏輯跟下面的 Xft 版一樣 ---- */
typedef struct { cairo_scaled_font_t *sf; int id; } CFont;
static CFont cfnt[4], cfb[48];
static int cfb_style[48], ncfb, cfont_ids;
static double cpx[4];                 /* 各樣式的像素大小 */
static int fasc0;                     /* 主字型的 ascent（刪除線用） */
static struct { uint32_t u; unsigned g; int id; } gcache[2048];

static int u8enc(uint32_t u, char *s)
{
	int n = 0;
	if (u < 0x80) s[n++] = (char)u;
	else if (u < 0x800) { s[n++] = (char)(0xc0 | (u >> 6)); s[n++] = (char)(0x80 | (u & 63)); }
	else if (u < 0x10000) { s[n++] = (char)(0xe0 | (u >> 12)); s[n++] = (char)(0x80 | ((u >> 6) & 63)); s[n++] = (char)(0x80 | (u & 63)); }
	else { s[n++] = (char)(0xf0 | (u >> 18)); s[n++] = (char)(0x80 | ((u >> 12) & 63)); s[n++] = (char)(0x80 | ((u >> 6) & 63)); s[n++] = (char)(0x80 | (u & 63)); }
	return n;
}

/* 這個字型裡 u 的字形編號（0＝沒有這個字）；結果快取起來 */
static unsigned cglyph(CFont *f, uint32_t u)
{
	unsigned h = (u * 2654435761u + (unsigned)f->id * 40503u) & 2047;
	if (gcache[h].u == u && gcache[h].id == f->id) return gcache[h].g;
	unsigned g = 0;
	char s[8];
	int n = u8enc(u, s);
	cairo_glyph_t *gl = NULL;
	int ng = 0;
	if (cairo_scaled_font_text_to_glyphs(f->sf, 0, 0, s, n, &gl, &ng, NULL, NULL, NULL) == CAIRO_STATUS_SUCCESS && ng == 1)
		g = (unsigned)gl[0].index;
	cairo_glyph_free(gl);
	gcache[h].u = u; gcache[h].id = f->id; gcache[h].g = g;
	return g;
}

static cairo_scaled_font_t *cfont_make(FcPattern *m, double px)
{
	cairo_font_face_t *face = cairo_ft_font_face_create_for_pattern(m);
	cairo_matrix_t fm, ctm;
	cairo_matrix_init_scale(&fm, px, px);
	cairo_matrix_init_identity(&ctm);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_scaled_font_t *sf = cairo_scaled_font_create(face, &fm, &ctm, fo);
	cairo_font_options_destroy(fo);
	cairo_font_face_destroy(face);
	return sf;
}

static void load_fonts_native(double size)
{
	for (int i = 0; i < 4; i++) {
		if (cfnt[i].sf) cairo_scaled_font_destroy(cfnt[i].sf);
		cfnt[i].sf = NULL;
		if (fpat[i]) FcPatternDestroy(fpat[i]);
	}
	for (int i = 0; i < ncfb; i++) cairo_scaled_font_destroy(cfb[i].sf);
	ncfb = 0;
	memset(missing, 0, sizeof missing);
	memset(gcache, 0, sizeof gcache);
	for (int s = 0; s < 4; s++) {
		FcPattern *p = FcNameParse((const FcChar8 *)tfont);
		if (!p) die(_("字型名稱格式錯誤"));
		if (!fsize0) {
			double d;
			fsize0 = FcPatternGetDouble(p, FC_SIZE, 0, &d) == FcResultMatch ? d : 11;
		}
		FcPatternDel(p, FC_SIZE);
		FcPatternDel(p, FC_PIXEL_SIZE);
		FcPatternAddDouble(p, FC_SIZE, size);
		if (s & 1) { FcPatternDel(p, FC_WEIGHT); FcPatternAddInteger(p, FC_WEIGHT, FC_WEIGHT_BOLD); }
		if (s & 2) { FcPatternDel(p, FC_SLANT); FcPatternAddInteger(p, FC_SLANT, FC_SLANT_ITALIC); }
		FcConfigSubstitute(NULL, p, FcMatchPattern);
		FcPatternAddDouble(p, FC_DPI, 96);   /* Xft 用螢幕的 dpi，Wayland 沒有，跟一般的 96 一樣 */
		FcDefaultSubstitute(p);
		FcResult r;
		FcPattern *m = FcFontMatch(NULL, p, &r);
		double px = 0;
		if (FcPatternGetDouble(p, FC_PIXEL_SIZE, 0, &px) != FcResultMatch || px <= 0) px = size * 96.0 / 72.0;
		if (!m || !(cfnt[s].sf = cfont_make(m, px)) || cairo_scaled_font_status(cfnt[s].sf)) die(_("無法載入字型"));
		cfnt[s].id = ++cfont_ids;
		cpx[s] = px;
		FcPatternDestroy(m);
		fpat[s] = p;
	}
	cairo_text_extents_t te;
	cairo_font_extents_t fe;
	cairo_scaled_font_text_extents(cfnt[0].sf, "M", &te);
	cairo_scaled_font_extents(cfnt[0].sf, &fe);
	cw = MAX((int)(te.x_advance + 0.5), 1);
	fasc0 = (int)ceil(fe.ascent - 1e-6);
	int natural = fasc0 + (int)ceil(fe.descent - 1e-6);
	chh = MAX((int)(natural * line_spacing + 0.5), 1);
	ascent = fasc0 + (chh - natural) / 2;
	fsize = size;
	emoji_load();
}

/* 找能顯示 u 的字型（原生後端）；主字型沒有就請 fontconfig 找備用字型 */
static CFont *font_for_native(uint32_t u, int style, unsigned *glyph)
{
	CFont *f = &cfnt[style];
	if ((*glyph = cglyph(f, u))) return f;
	for (int i = 0; i < ncfb; i++)
		if (cfb_style[i] == style && (*glyph = cglyph(&cfb[i], u))) return &cfb[i];
	if (missing[u & 255] == u || ncfb >= (int)LEN(cfb)) {
		*glyph = cglyph(f, 0xfffd);
		return f;
	}
	FcPattern *p = FcPatternDuplicate(fpat[style]);
	FcCharSet *cs = FcCharSetCreate();
	FcCharSetAddChar(cs, u);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcPatternAddBool(p, FC_SCALABLE, FcTrue);
#ifdef FC_COLOR
	FcPatternAddBool(p, FC_COLOR, FcFalse);   /* 彩色 emoji 字型另外用 cairo 畫 */
#endif
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	FcResult r;
	FcPattern *m = FcFontMatch(NULL, p, &r);
	FcCharSetDestroy(cs);
	FcPatternDestroy(p);
	if (m) {
		cairo_scaled_font_t *sf = cfont_make(m, cpx[style]);
		FcPatternDestroy(m);
		if (sf && !cairo_scaled_font_status(sf)) {
			cfb[ncfb].sf = sf;
			cfb[ncfb].id = ++cfont_ids;
			if ((*glyph = cglyph(&cfb[ncfb], u))) { cfb_style[ncfb] = style; return &cfb[ncfb++]; }
		}
		if (sf) cairo_scaled_font_destroy(sf);
	}
	missing[u & 255] = u;
	*glyph = cglyph(f, 0xfffd);
	return f;
}

static void load_fonts(double size)
{
	if (native) { load_fonts_native(size); return; }
	for (int i = 0; i < 4; i++) {
		if (fnt[i]) XftFontClose(dpy, fnt[i]);
		if (fpat[i]) FcPatternDestroy(fpat[i]);
	}
	for (int i = 0; i < nfb; i++) XftFontClose(dpy, fb[i].f);
	nfb = 0;
	memset(missing, 0, sizeof missing);

	for (int s = 0; s < 4; s++) {
		FcPattern *p = FcNameParse((const FcChar8 *)tfont);
		if (!p) die(_("字型名稱格式錯誤"));
		if (!fsize0) {
			double d;
			fsize0 = FcPatternGetDouble(p, FC_SIZE, 0, &d) == FcResultMatch ? d : 11;
		}
		FcPatternDel(p, FC_SIZE);
		FcPatternDel(p, FC_PIXEL_SIZE);
		FcPatternAddDouble(p, FC_SIZE, size);
		if (s & 1) { FcPatternDel(p, FC_WEIGHT); FcPatternAddInteger(p, FC_WEIGHT, FC_WEIGHT_BOLD); }
		if (s & 2) { FcPatternDel(p, FC_SLANT); FcPatternAddInteger(p, FC_SLANT, FC_SLANT_ITALIC); }
		FcConfigSubstitute(NULL, p, FcMatchPattern);
		XftDefaultSubstitute(dpy, scr, p);
		FcResult r;
		FcPattern *m = FcFontMatch(NULL, p, &r);
		if (!m || !(fnt[s] = XftFontOpenPattern(dpy, m))) die(_("無法載入字型"));
		fpat[s] = p;
	}
	XGlyphInfo gi;
	XftTextExtentsUtf8(dpy, fnt[0], (const FcChar8 *)"M", 1, &gi);
	cw = MAX(gi.xOff, 1);
	int natural = fnt[0]->ascent + fnt[0]->descent;
	chh = MAX((int)(natural * line_spacing + 0.5), 1);
	ascent = fnt[0]->ascent + (chh - natural) / 2;
	fsize = size;
	emoji_load();
	/* 分頁列用一般的介面字型（不是等寬字） */
	if (!tabfont) tabfont = XftFontOpenName(dpy, scr, "Noto Sans CJK TC,sans-serif:size=10");
}

/* 找能顯示這個字的字型；主字型沒有就請 fontconfig 找備用字型 */
static XftFont *font_for(uint32_t u, int style, FT_UInt *glyph)
{
	XftFont *f = fnt[style];
	if ((*glyph = XftCharIndex(dpy, f, u)))
		return f;
	for (int i = 0; i < nfb; i++)
		if (fb[i].style == style && (*glyph = XftCharIndex(dpy, fb[i].f, u)))
			return fb[i].f;
	if (missing[u & 255] == u || nfb >= (int)LEN(fb)) {
		*glyph = XftCharIndex(dpy, f, 0xfffd);
		return f;
	}
	FcPattern *p = FcPatternDuplicate(fpat[style]);
	FcCharSet *cs = FcCharSetCreate();
	FcCharSetAddChar(cs, u);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcPatternAddBool(p, FC_SCALABLE, FcTrue);
#ifdef FC_COLOR
	FcPatternAddBool(p, FC_COLOR, FcFalse);   /* 彩色 emoji 字型在 Xft 上不穩定 */
#endif
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	FcResult r;
	FcPattern *m = FcFontMatch(NULL, p, &r);
	FcCharSetDestroy(cs);
	FcPatternDestroy(p);
	XftFont *nf = m ? XftFontOpenPattern(dpy, m) : NULL;
	if (!nf) {
		if (m) FcPatternDestroy(m);
	} else if ((*glyph = XftCharIndex(dpy, nf, u))) {
		fb[nfb].f = nf;
		fb[nfb].style = style;
		nfb++;
		return nf;
	} else {
		XftFontClose(dpy, nf);
	}
	missing[u & 255] = u;
	*glyph = XftCharIndex(dpy, f, 0xfffd);
	return f;
}

/* 配色：terminal.theme = system（跟著系統深淺色）/ dark / light */
static int use_dark(void)
{
	const char *th = pm_conf_get("terminal.theme", "system");
	if (!strcmp(th, "dark")) return 1;
	if (!strcmp(th, "light")) return 0;
	return pm_conf_int("appearance.dark", dark_theme);
}

static void load_colors(void)
{
	int dark = use_dark();
	const unsigned int *pal = dark ? dark_pal : light_pal;
	for (int i = 0; i < 16; i++) palrgb[i] = pal[i];
	static const int lv[6] = { 0, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
	for (int i = 0; i < 216; i++)
		palrgb[16 + i] = (lv[i / 36] << 16) | (lv[(i / 6) % 6] << 8) | lv[i % 6];
	for (int i = 0; i < 24; i++) {
		int v = 8 + i * 10;
		palrgb[232 + i] = (v << 16) | (v << 8) | v;
	}
	fgcol = dark ? dark_fg : light_fg;
	bgcol = dark ? dark_bg : light_bg;
	curcol = dark ? dark_cursor : light_cursor;
	selcol = dark ? dark_sel : light_sel;
}

static uint32_t to_rgb(uint32_t c)
{
	if (ISTRUE(c)) return c & 0xffffff;
	if (c == DEFFG) return fgcol;
	if (c == DEFBG) return bgcol;
	return palrgb[c & 255];
}

static uint32_t blend(uint32_t a, uint32_t b, double k)
{
	int r = (int)(((a >> 16) & 255) * (1 - k) + ((b >> 16) & 255) * k);
	int g = (int)(((a >> 8) & 255) * (1 - k) + ((b >> 8) & 255) * k);
	int bl = (int)((a & 255) * (1 - k) + (b & 255) * k);
	return (uint32_t)((r << 16) | (g << 8) | bl);
}

static XftColor *xcolor(uint32_t rgb, XftColor *out)
{
	XRenderColor rc = { (unsigned short)(((rgb >> 16) & 255) * 257), (unsigned short)(((rgb >> 8) & 255) * 257),
	                    (unsigned short)((rgb & 255) * 257), 0xffff };
	XftColorAllocValue(dpy, vis, cmap, &rc, out);
	return out;
}

/* 繪圖原語：X 後端用 Xft 畫在 pixmap（xd），原生後端用 cairo 畫在影像緩衝（bufcr），畫面上看到的一樣 */
static void P_rect(uint32_t rgb, int x, int y, int w, int h)
{
	if (native) {
		cairo_set_source_rgb(bufcr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
		cairo_rectangle(bufcr, x, y, w, h);
		cairo_fill(bufcr);
	} else {
		XftColor c;
		XftDrawRect(xd, xcolor(rgb, &c), x, y, w, h);
	}
}

/* 一排字形（同一種顏色）；font 在 X 是 XftFont*，原生是 CFont* */
typedef struct { void *font; unsigned glyph; int x, y; } GSpec;
static void P_glyphs(const GSpec *sp, int n, uint32_t rgb)
{
	if (native) {
		cairo_glyph_t gl[1024];
		cairo_set_source_rgb(bufcr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
		for (int i = 0; i < n;) {
			int j = i;
			CFont *cf = sp[i].font;
			while (j < n && sp[j].font == (void *)cf) { gl[j - i] = (cairo_glyph_t){ sp[j].glyph, sp[j].x, sp[j].y }; j++; }
			cairo_set_scaled_font(bufcr, cf->sf);
			cairo_show_glyphs(bufcr, gl, j - i);
			i = j;
		}
	} else {
		XftColor c;
		XftGlyphFontSpec xs[1024];
		for (int i = 0; i < n; i++) xs[i] = (XftGlyphFontSpec){ .font = sp[i].font, .glyph = sp[i].glyph, .x = (short)sp[i].x, .y = (short)sp[i].y };
		XftDrawGlyphFontSpec(xd, xcolor(rgb, &c), xs, n);
	}
}

/* ------------------------------------------------------------------ */
/* 繪製                                                                */
/* ------------------------------------------------------------------ */

static void sel_norm(int *x1, int *y1, int *x2, int *y2)
{
	if (sel.sy < sel.ey || (sel.sy == sel.ey && sel.sx <= sel.ex)) {
		*x1 = sel.sx; *y1 = sel.sy; *x2 = sel.ex; *y2 = sel.ey;
	} else {
		*x1 = sel.ex; *y1 = sel.ey; *x2 = sel.sx; *y2 = sel.sy;
	}
}

static int selected(int v, int x)
{
	if (!sel.active) return 0;
	int x1, y1, x2, y2;
	sel_norm(&x1, &y1, &x2, &y2);
	if (v < y1 || v > y2) return 0;
	if (v == y1 && x < x1) return 0;
	if (v == y2 && x > x2) return 0;
	return 1;
}

static void resolve(Cell c, int v, int x, int cursor, uint32_t *F, uint32_t *B)
{
	uint32_t f = c.fg, b = c.bg;
	if ((c.a & A_BOLD) && f < 8) f += 8;   /* 粗體 = 亮色 */
	uint32_t fr = to_rgb(f), br = to_rgb(b), tmp;
	if (c.a & A_REV) { tmp = fr; fr = br; br = tmp; }
	if (t.mode & M_REVERSE) { tmp = fr; fr = br; br = tmp; }
	if (c.a & A_DIM) fr = blend(fr, br, 0.45);
	if (selected(v, x)) br = selcol;
	if (cursor) { fr = br; br = curcol; if (fr == br) fr = bgcol; }
	if (c.a & A_INVIS) fr = br;
	*F = fr; *B = br;
}

/* ------------------------------------------------------------------ */
/* 方框線與方塊字元自己畫，行距加大時線條才會接得起來                  */
/* ------------------------------------------------------------------ */

/* U+2500–257F：左右上下四個方向的粗細（0 無、1 細、2 粗、3 雙線） */
static const char *boxarms[128] = {
	"1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022",
	"1100", "2200", "0011", "0022", "0101", "0201", "0102", "0202",
	"1001", "2001", "1002", "2002", "0110", "0210", "0120", "0220",
	"1010", "2010", "1020", "2020", "0111", "0211", "0121", "0112",
	"0122", "0221", "0212", "0222", "1011", "2011", "1021", "1012",
	"1022", "2021", "2012", "2022", "1101", "2101", "1201", "2201",
	"1102", "2102", "1202", "2202", "1110", "2110", "1210", "2210",
	"1120", "2120", "1220", "2220", "1111", "2111", "1211", "2211",
	"1121", "1112", "1122", "2121", "1221", "2112", "1212", "2221",
	"2212", "2122", "1222", "2222", "1100", "2200", "0011", "0022",
	"3300", "0033", "0301", "0103", "0303", "3001", "1003", "3003",
	"0310", "0130", "0330", "3010", "1030", "3030", "0311", "0133",
	"0333", "3011", "1033", "3033", "3301", "1103", "3303", "3310",
	"1130", "3330", "3311", "1133", "3333", "0101", "1001", "1010",
	"0110", "",     "",     "",     "1000", "0010", "0100", "0001",
	"2000", "0020", "0200", "0002", "1200", "0012", "2100", "0021",
};

static void hline(uint32_t c, int x1, int x2, int y, int th) { P_rect(c, x1, y, MAX(x2 - x1, 1), th); }
static void vline_(uint32_t c, int y1, int y2, int x, int th) { P_rect(c, x, y1, th, MAX(y2 - y1, 1)); }

/* 成功畫出來就回傳 1，否則交給字型 */
static int draw_box(uint32_t u, int x0, int y0, uint32_t fg, uint32_t bg)
{
	int w = cw, h = chh;
	if (u >= 0x2500 && u <= 0x257f) {
		const char *a = boxarms[u - 0x2500];
		if (!a[0]) return 0;
		int lt = MAX(1, (cw + 5) / 10), hv = lt * 2 + 1;
		int mx = x0 + w / 2, my = y0 + h / 2;
		for (int i = 0; i < 4; i++) {
			int k = a[i] - '0';
			if (!k) continue;
			int th = k == 2 ? hv : lt, o = th / 2;
			int horiz = i < 2;
			int from = horiz ? (i == 0 ? x0 : mx - o) : (i == 2 ? y0 : my - o);
			int to = horiz ? (i == 0 ? mx + th - o : x0 + w) : (i == 2 ? my + th - o : y0 + h);
			if (k == 3) {   /* 雙線：兩條細線 */
				int g = lt + 1;
				if (horiz) {
					int f2 = i == 0 ? x0 : mx - g, t2 = i == 0 ? mx + g + lt : x0 + w;
					hline(fg, f2, t2, my - g, lt);
					hline(fg, f2, t2, my + g, lt);
				} else {
					int f2 = i == 2 ? y0 : my - g, t2 = i == 2 ? my + g + lt : y0 + h;
					vline_(fg, f2, t2, mx - g, lt);
					vline_(fg, f2, t2, mx + g, lt);
				}
			} else if (horiz) {
				hline(fg, from, to, my - o, th);
			} else {
				vline_(fg, from, to, mx - o, th);
			}
		}
		return 1;
	}
	if (u >= 0x2580 && u <= 0x259f) {
		if (u == 0x2580) P_rect(fg, x0, y0, w, h / 2);
		else if (u <= 0x2588) { int k = (int)(u - 0x2580); P_rect(fg, x0, y0 + h - h * k / 8, w, h * k / 8); }
		else if (u <= 0x258f) { int k = 8 - (int)(u - 0x2588); P_rect(fg, x0, y0, w * k / 8, h); }
		else if (u == 0x2590) P_rect(fg, x0 + w / 2, y0, w - w / 2, h);
		else if (u <= 0x2593) P_rect(blend(bg, fg, (u - 0x2590) * 0.25), x0, y0, w, h);
		else if (u == 0x2594) P_rect(fg, x0, y0, w, MAX(h / 8, 1));
		else if (u == 0x2595) P_rect(fg, x0 + w - MAX(w / 8, 1), y0, MAX(w / 8, 1), h);
		else {
			static const int q[] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };
			int m = q[u - 0x2596], hw = w / 2, hh = h / 2;
			if (m & 1) P_rect(fg, x0, y0, hw, hh);
			if (m & 2) P_rect(fg, x0 + hw, y0, w - hw, hh);
			if (m & 4) P_rect(fg, x0, y0 + hh, hw, h - hh);
			if (m & 8) P_rect(fg, x0 + hw, y0 + hh, w - hw, h - hh);
		}
		return 1;
	}
	return 0;
}

static void draw_row(int r)
{
	int len, v = hn - scrolloff + r;
	Cell *l = vline(v, &len);
	int y = tpad + toff + r * chh;
	int curx = -1;
	if (!scrolloff && r == t.cy && !(t.mode & M_HIDE))
		curx = t.cx;
	int block = curx >= 0 && focused && t.cursor_style <= 2;
	int curw = curx >= 0 && curx < len && (l[curx].a & A_WIDE) ? 2 : 1;

#define CELL(x) ((x) < len ? l[x] : defcell)
#define ISCUR(x) (block && (x) >= curx && (x) < curx + curw)
	/* 背景：同色的連續儲存格一次畫完 */
	for (int x = 0; x < t.cols;) {
		uint32_t f, b, f2, b2;
		resolve(CELL(x), v, x, ISCUR(x), &f, &b);
		int x0 = x++;
		while (x < t.cols) {
			resolve(CELL(x), v, x, ISCUR(x), &f2, &b2);
			if (b2 != b) break;
			x++;
		}
		P_rect(b, tpad + x0 * cw, y, (x - x0) * cw, chh);
	}
	/* 文字：同色的字一次送出 */
	GSpec specs[1024];
	int ns = 0;
	uint32_t curfg = 0xffffffff;
	int asc0 = native ? fasc0 : fnt[0]->ascent;
	for (int x = 0; x < t.cols; x++) {
		Cell c = CELL(x);
		if (c.a & A_WDUMMY) continue;
		uint32_t f, b;
		resolve(c, v, x, ISCUR(x), &f, &b);
		int w = (c.a & A_WIDE) ? 2 : 1;
		if (c.u >= 0x2500 && c.u <= 0x259f && !(c.a & A_INVIS) &&
		    draw_box(c.u, tpad + x * cw, y, f, b)) {
			/* 已自行畫好 */
		} else if ((c.a & A_WIDE) && !(c.a & A_INVIS) && emoji_range(c.u) && draw_emoji(c.u, tpad + x * cw, y, 2)) {
			/* 彩色 emoji */
		} else if (c.u > ' ' && !(c.a & A_INVIS)) {
			int style = ((c.a & A_BOLD) ? 1 : 0) | ((c.a & A_ITALIC) ? 2 : 0);
			unsigned g;
			void *fo = native ? (void *)font_for_native(c.u, style, &g) : (void *)font_for(c.u, style, &g);
			/* 一般字型都沒有、彩色字型有（例如國旗的字母）：用彩色的 */
			if (!(missing[c.u & 255] == c.u && emoji_range(c.u) && draw_emoji(c.u, tpad + x * cw, y, w))) {
				if ((f != curfg || ns == (int)LEN(specs)) && ns) {
					P_glyphs(specs, ns, curfg);
					ns = 0;
				}
				curfg = f;
				specs[ns].font = fo;
				specs[ns].glyph = g;
				specs[ns].x = tpad + x * cw;
				specs[ns].y = y + ascent;
				ns++;
			}
		}
		if (c.a & A_UNDER)
			P_rect(f, tpad + x * cw, y + ascent + 2, w * cw, 1);
		if (c.a & A_STRIKE)
			P_rect(f, tpad + x * cw, y + ascent - (asc0 / 3), w * cw, 1);
	}
	if (ns)
		P_glyphs(specs, ns, curfg);
	/* 非方塊游標 */
	if (curx >= 0 && !block) {
		int cx = tpad + curx * cw, W = curw * cw;
		if (!focused) {
			P_rect(curcol, cx, y, W, 1);
			P_rect(curcol, cx, y + chh - 1, W, 1);
			P_rect(curcol, cx, y, 1, chh);
			P_rect(curcol, cx + W - 1, y, 1, chh);
		} else if (t.cursor_style <= 4) {
			P_rect(curcol, cx, y + chh - 2, W, 2);
		} else {
			P_rect(curcol, cx, y, 2, chh);
		}
	}
#undef CELL
#undef ISCUR
}

static void im_spot(void)
{
	if (native) return;   /* 原生：draw 結束時用 pmwl_ime 告訴輸入法游標在哪 */
	if (!xic) return;
	XPoint spot = { (short)(tpad + t.cx * cw), (short)(tpad + toff + (t.cy + 1) * chh) };
	XVaNestedList attr = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
	XSetICValues(xic, XNPreeditAttributes, attr, NULL);
	XFree(attr);
}

/* UTF-8 的下一個字元（回傳位元組數） */
static int u8next(const unsigned char *q, uint32_t *out)
{
	uint32_t u;
	int n = (*q < 0x80) ? (u = *q, 1) : (*q < 0xe0) ? (u = *q & 0x1f, 2) : (*q < 0xf0) ? (u = *q & 0x0f, 3) : (u = *q & 7, 4);
	for (int k = 1; k < n && q[k]; k++) u = (u << 6) | (q[k] & 63);
	*out = u;
	return n;
}

/* 原生：輸入法還沒確定的字，畫在游標的格子上（有底色、底線），寬字元佔兩格 */
static void draw_preedit(cairo_t *cr)
{
	if (!preedit[0] || scrolloff) return;
	int x = tpad + t.cx * cw, y = tpad + toff + t.cy * chh;
	int total = 0;
	for (const unsigned char *q = (const unsigned char *)preedit; *q;) {   /* 先算寬度 */
		uint32_t u;
		q += u8next(q, &u);
		int w = u < 0x80 ? 1 : wcwidth((wchar_t)u);
		total += w < 0 ? 1 : w > 2 ? 2 : w;
	}
	cairo_set_source_rgb(cr, ((bgcol >> 16) & 255) / 255.0, ((bgcol >> 8) & 255) / 255.0, (bgcol & 255) / 255.0);
	cairo_rectangle(cr, x, y, total * cw, chh);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, ((fgcol >> 16) & 255) / 255.0, ((fgcol >> 8) & 255) / 255.0, (fgcol & 255) / 255.0);
	int cx = x;
	for (const unsigned char *q = (const unsigned char *)preedit; *q;) {
		uint32_t u;
		q += u8next(q, &u);
		int w = u < 0x80 ? 1 : wcwidth((wchar_t)u);
		w = w < 0 ? 1 : w > 2 ? 2 : w;
		if (u > ' ') {
			unsigned g;
			CFont *f = font_for_native(u, 0, &g);
			cairo_glyph_t gl = { g, cx, y + ascent };
			cairo_set_scaled_font(cr, f->sf);
			cairo_show_glyphs(cr, &gl, 1);
		}
		cx += w * cw;
	}
	cairo_rectangle(cr, x, y + chh - 2, total * cw, 1);
	cairo_fill(cr);
}

/* 原生：把緩衝送給 compositor，順便告訴輸入法游標位置 */
static void present(void)
{
	int W, H;
	cairo_t *cr = pmwl_begin(wl, &W, &H);
	if (!cr) return;
	cairo_set_source_rgb(cr, ((bgcol >> 16) & 255) / 255.0, ((bgcol >> 8) & 255) / 255.0, (bgcol & 255) / 255.0);
	cairo_paint(cr);
	cairo_set_source_surface(cr, bufsurf, 0, 0);
	cairo_paint(cr);
	draw_preedit(cr);
	pmwl_end(wl, cr);
	pmwl_ime(wl, 1, tpad + t.cx * cw, tpad + toff + t.cy * chh, chh);
}

static void draw(void)
{
	if (full_redraw) {
		P_rect((t.mode & M_REVERSE) ? fgcol : bgcol, 0, 0, winw, winh);
		draw_tabbar();
		alldirty();
	}
	/* 游標移動時，新舊位置都要重畫 */
	if (ocy >= 0 && ocy < t.rows) t.dirty[ocy] = 1;
	t.dirty[t.cy] = 1;
	for (int r = 0; r < t.rows; r++)
		if (t.dirty[r]) {
			draw_row(r);
			t.dirty[r] = 0;
		}
	if (native) {
		cairo_surface_flush(bufsurf);
		present();
	} else {
		XCopyArea(dpy, buf, win, tgc, 0, 0, winw, winh, 0, 0);
		if (ocx != t.cx || ocy != t.cy)
			im_spot();
		XFlush(dpy);
	}
	ocx = t.cx;
	ocy = t.cy;
	full_redraw = 0;
}

/* ------------------------------------------------------------------ */
/* 選取與剪貼簿                                                        */
/* ------------------------------------------------------------------ */

static void sel_clear(void)
{
	if (!sel.active) return;
	sel.active = 0;
	if (t.dirty) alldirty();
}

static int is_delim(uint32_t u)
{
	return u == ' ' || u == 0 || (u < 0x80 && strchr("\t()[]{}<>'\"`|;,", (int)u));
}

static void sel_snap(void)
{
	if (sel.snap == 2) {   /* 雙擊：選一個字 */
		int len, x1, y1, x2, y2;
		sel_norm(&x1, &y1, &x2, &y2);
		Cell *l = vline(y1, &len);
		if (x1 < len && !is_delim(l[x1].u))
			while (x1 > 0 && x1 - 1 < len && !is_delim(l[x1 - 1].u)) x1--;
		l = vline(y2, &len);
		if (x2 < len && !is_delim(l[x2].u))
			while (x2 + 1 < len && !is_delim(l[x2 + 1].u)) x2++;
		sel.sx = x1; sel.sy = y1; sel.ex = x2; sel.ey = y2;
	} else if (sel.snap == 3) {   /* 三擊：選整行 */
		int x1, y1, x2, y2;
		sel_norm(&x1, &y1, &x2, &y2);
		sel.sx = 0; sel.sy = y1; sel.ex = t.cols - 1; sel.ey = y2;
	}
}

static char *sel_text(void)
{
	if (!sel.active) return NULL;
	int x1, y1, x2, y2;
	sel_norm(&x1, &y1, &x2, &y2);
	size_t cap = (size_t)(y2 - y1 + 1) * (t.cols * 4 + 1) + 1, n = 0;
	char *out = xmalloc(cap);
	for (int v = y1; v <= y2; v++) {
		int len;
		Cell *l = vline(v, &len);
		int a = v == y1 ? x1 : 0, b = v == y2 ? x2 : t.cols - 1;
		int end = MIN(b, len - 1);
		/* 去掉行尾空白 */
		while (end >= a && (l[end].u == ' ' || l[end].u == 0) && !(l[end].a & A_WDUMMY)) end--;
		for (int x = a; x <= end; x++) {
			if (l[x].a & A_WDUMMY) continue;
			uint32_t u = l[x].u ? l[x].u : ' ';
			if (u < 0x80) out[n++] = (char)u;
			else if (u < 0x800) { out[n++] = (char)(0xc0 | (u >> 6)); out[n++] = (char)(0x80 | (u & 63)); }
			else if (u < 0x10000) { out[n++] = (char)(0xe0 | (u >> 12)); out[n++] = (char)(0x80 | ((u >> 6) & 63)); out[n++] = (char)(0x80 | (u & 63)); }
			else { out[n++] = (char)(0xf0 | (u >> 18)); out[n++] = (char)(0x80 | ((u >> 12) & 63)); out[n++] = (char)(0x80 | ((u >> 6) & 63)); out[n++] = (char)(0x80 | (u & 63)); }
		}
		int wrapped = len > 0 && b >= len - 1 && (l[len - 1].a & A_WRAP);
		if (v < y2 && !wrapped) out[n++] = '\n';
	}
	out[n] = 0;
	return out;
}

static void own_selection(Atom which, char *text)
{
	if (!text) return;
	if (which == XA_PRIMARY) { free(selbuf); selbuf = text; }
	else { free(clipbuf); clipbuf = text; }
	if (native) {   /* 原生：剪貼簿／primary selection 都交給 compositor，資料函式庫自己複製 */
		if (which == XA_PRIMARY) pmwl_primary_set(text);
		else pmwl_clip_set(text);
		return;
	}
	XSetSelectionOwner(dpy, which, win, evtime);
}

static void on_selrequest(XSelectionRequestEvent *e)
{
	XSelectionEvent r = { .type = SelectionNotify, .requestor = e->requestor, .selection = e->selection,
	                      .target = e->target, .property = None, .time = e->time };
	Atom prop = e->property ? e->property : e->target;
	const char *s = e->selection == XA_PRIMARY ? selbuf : clipbuf;
	if (e->target == a_targets) {
		Atom ts[] = { a_targets, a_utf8, XA_STRING, a_text };
		XChangeProperty(dpy, e->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)ts, LEN(ts));
		r.property = prop;
	} else if (s && (e->target == a_utf8 || e->target == XA_STRING || e->target == a_text)) {
		XChangeProperty(dpy, e->requestor, prop, e->target == XA_STRING ? XA_STRING : a_utf8, 8,
		                PropModeReplace, (const unsigned char *)s, strlen(s));
		r.property = prop;
	}
	XSendEvent(dpy, e->requestor, True, 0, (XEvent *)&r);
}

/* 貼上的內容：換行改成 Enter；括號貼上模式下包起來讓 shell 知道是貼上的 */
static void paste_data(unsigned char *data, unsigned long n, int first)
{
	for (unsigned long i = 0; i < n; i++)
		if (data[i] == '\n') data[i] = '\r';
	if (first && (t.mode & M_BRPASTE)) ttywrite("\033[200~", 6);
	ttywrite((const char *)data, n);
}

static void paste(Atom which)
{
	if (native) {
		char *txt = which == XA_PRIMARY ? pmwl_primary_get() : pmwl_clip_get();
		if (!txt) return;
		size_t n = strlen(txt);
		if (n) {
			paste_data((unsigned char *)txt, n, 1);
			if (t.mode & M_BRPASTE) ttywrite("\033[201~", 6);
			if (scrolloff) { scrolloff = 0; alldirty(); }
		}
		free(txt);
		return;
	}
	XConvertSelection(dpy, which, a_utf8, a_paste, win, evtime);
}

static void on_selnotify(XSelectionEvent *e)
{
	if (e->property == None) {
		if (e->target == a_utf8)
			XConvertSelection(dpy, e->selection, XA_STRING, a_paste, win, evtime);
		return;
	}
	Atom type; int fmt; unsigned long n, after, off = 0;
	unsigned char *data;
	int first = 1;
	do {
		if (XGetWindowProperty(dpy, win, e->property, off, 65536, False, AnyPropertyType,
		                       &type, &fmt, &n, &after, &data) != Success)
			break;
		if (fmt != 8) { XFree(data); break; }
		paste_data(data, n, first);
		first = 0;
		off += n / 4;
		XFree(data);
	} while (after > 0);
	if (!first && (t.mode & M_BRPASTE)) ttywrite("\033[201~", 6);
	XDeleteProperty(dpy, win, e->property);
	if (scrolloff) { scrolloff = 0; alldirty(); }
}

/* ------------------------------------------------------------------ */
/* 鍵盤與滑鼠                                                          */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 分頁                                                                */
/* ------------------------------------------------------------------ */

#define TABBAR_H 30

static void tab_save(Tab *b)
{
	b->t = t;
	b->hist = hist; b->hcap = hcap; b->hn = hn; b->hhead = hhead; b->scrolloff = scrolloff;
	b->pstate = pstate; b->strn = strn; b->strtype = strtype; b->utf_need = utf_need; b->utf_cp = utf_cp;
	b->csi = csi;
	memcpy(b->strbuf, strbuf, sizeof strbuf);
	b->sel = sel;
	b->mouse_btn = mouse_btn; b->mouse_lx = mouse_lx; b->mouse_ly = mouse_ly;
	b->child = child; b->ptyfd = ptyfd; b->dead = pty_dead;
	b->sync_since = sync_since;
	snprintf(b->title, sizeof b->title, "%s", cur_title);
}

static void tab_load(Tab *b)
{
	t = b->t;
	hist = b->hist; hcap = b->hcap; hn = b->hn; hhead = b->hhead; scrolloff = b->scrolloff;
	pstate = b->pstate; strn = b->strn; strtype = b->strtype; utf_need = b->utf_need; utf_cp = b->utf_cp;
	csi = b->csi;
	memcpy(strbuf, b->strbuf, sizeof strbuf);
	sel = b->sel;
	mouse_btn = b->mouse_btn; mouse_lx = b->mouse_lx; mouse_ly = b->mouse_ly;
	child = b->child; ptyfd = b->ptyfd; pty_dead = b->dead;
	sync_since = b->sync_since;
	snprintf(cur_title, sizeof cur_title, "%s", b->title);
}

static void show_title(void)
{
	if (native) { pmwl_set_title(wl, cur_title); return; }
	XStoreName(dpy, win, cur_title);
	XChangeProperty(dpy, win, a_netname, a_utf8, 8, PropModeReplace, (const unsigned char *)cur_title, strlen(cur_title));
}

/* 視窗大小、字型或分頁數改變：每個分頁都重新計算行列並通知 shell */
static void relayout(void)
{
	toff = ntabs > 0 ? TABBAR_H : 0;
	int cols = (winw - 2 * tpad) / cw, rows = (winh - 2 * tpad - toff) / chh;
	if (cols < 2) cols = 2;   /* 原生視窗：compositor 不一定擋得住最小大小 */
	if (rows < 1) rows = 1;
	if (ntabs == 0) { tresize(cols, rows); tty_winsize(); full_redraw = 1; return; }
	tab_save(tab[cur]);
	for (int i = 0; i < ntabs; i++) {
		tab_load(tab[i]);
		tresize(cols, rows);
		tty_winsize();
		tab_save(tab[i]);
	}
	tab_load(tab[cur]);
	alldirty();
	full_redraw = 1;
}

static void switch_tab(int i)
{
	if (i < 0 || i >= ntabs || i == cur) return;
	tab_save(tab[cur]);
	cur = i;
	tab_load(tab[cur]);
	show_title();
	alldirty();
	full_redraw = 1;
	ocx = ocy = -1;
}

static void child_cwd(char *out, size_t n)
{
	char link[64];
	snprintf(link, sizeof link, "/proc/%d/cwd", (int)child);
	ssize_t k = readlink(link, out, n - 1);
	out[k > 0 ? k : 0] = 0;
}

static void new_tab(void)
{
	if (ntabs >= (int)LEN(tab)) return;
	char cwd[PATH_MAX];
	child_cwd(cwd, sizeof cwd);
	tab_save(tab[cur]);
	/* 全新的終端機狀態 */
	memset(&t, 0, sizeof t);
	hist = hcap ? calloc(hcap, sizeof *hist) : NULL;
	hn = hhead = scrolloff = 0;
	pstate = 0; strn = 0; utf_need = 0;
	memset(&csi, 0, sizeof csi);
	memset(&sel, 0, sizeof sel);
	mouse_btn = mouse_lx = mouse_ly = -1;
	pty_dead = 0;
	snprintf(cur_title, sizeof cur_title, "%s", _("終端機"));
	int rows = (winh - 2 * tpad - TABBAR_H) / chh;
	tresize((winw - 2 * tpad) / cw, rows);
	treset();
	cmd_argv = NULL;
	cmd_dir = cwd[0] ? cwd : NULL;
	spawn_shell();
	cmd_dir = NULL;
	tab[ntabs] = calloc(1, sizeof(Tab));
	cur = ntabs++;
	tab_save(tab[cur]);
	relayout();
	show_title();
}

static void free_tab(Tab *b)
{
	if (b->child > 0) kill(b->child, SIGHUP);
	if (b->ptyfd >= 0) close(b->ptyfd);
	free_screen(b->t.pri, b->t.rows);
	free_screen(b->t.alt, b->t.rows);
	free(b->t.tabs);
	free(b->t.dirty);
	for (int i = 0; i < b->hn; i++) free(b->hist[(b->hhead + i) % b->hcap].c);
	free(b->hist);
	free(b);
}

static void close_tab(int i)
{
	if (i < 0 || i >= ntabs) return;
	if (ntabs == 1) {
		if (child > 0) kill(child, SIGHUP);
		exit(0);
	}
	tab_save(tab[cur]);
	free_tab(tab[i]);
	memmove(&tab[i], &tab[i + 1], (ntabs - i - 1) * sizeof *tab);
	ntabs--;
	if (cur > i || cur == ntabs) cur--;
	tab_load(tab[cur]);
	show_title();
	relayout();
	ocx = ocy = -1;
}

/* 背景分頁有輸出：暫時換進來處理，再換回目前的分頁 */
static void read_background(int i)
{
	tab_save(tab[cur]);
	tab_load(tab[i]);
	bg_ctx = 1;
	long start = now_ms();
	do {
		if (ttyread() < 0) break;
		struct timeval z = { 0, 0 };
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(ptyfd, &rf);
		if (select(ptyfd + 1, &rf, NULL, NULL, &z) <= 0) break;
	} while (now_ms() - start < 6);
	bg_ctx = 0;
	tab_save(tab[i]);
	tab_load(tab[cur]);
	if (tab[i]->dead) close_tab(i);
	else full_redraw = 1;   /* 分頁列可能要更新標題 */
}

/* 分頁列的位置：回傳第幾個分頁，-2 = 「＋」，*close = 點到「×」 */
static int tab_width(void) { return MIN(240, (winw - 40) / MAX(ntabs, 1)); }

static int tab_hit(int x, int y, int *close)
{
	*close = 0;
	if (ntabs < 1 || y >= toff) return -1;
	if (x >= winw - 36) return -2;
	int w = tab_width(), i = x / w;
	if (i >= ntabs) return -1;
	*close = x >= (i + 1) * w - 24;
	return i;
}

static void draw_tabbar_x(void)
{
	if (ntabs < 1) return;
	XftColor col;
	int dark = ((bgcol >> 16 & 255) + (bgcol >> 8 & 255) + (bgcol & 255)) < 384;
	uint32_t barbg = dark ? 0x101012 : 0xdedee2, line = dark ? 0x3a3a3c : 0xc7c7cc;
	XftDrawRect(xd, xcolor(barbg, &col), 0, 0, winw, toff);
	XftDrawRect(xd, xcolor(line, &col), 0, toff - 1, winw, 1);
	int w = tab_width();
	XftFont *f = tabfont ? tabfont : fnt[0];
	for (int i = 0; i < ntabs; i++) {
		int x = i * w;
		const char *title = i == cur ? cur_title : tab[i]->title;
		if (i == cur) XftDrawRect(xd, xcolor(bgcol, &col), x, 0, w, toff);
		else if (i) XftDrawRect(xd, xcolor(line, &col), x, 7, 1, toff - 14);
		/* 標題太長就從後面截斷，加上「…」 */
		char tt[260];
		snprintf(tt, sizeof tt, "%s", title);
		XGlyphInfo gi;
		XftTextExtentsUtf8(dpy, f, (const FcChar8 *)tt, (int)strlen(tt), &gi);
		for (size_t len = strlen(title); gi.xOff > w - 44 && len > 0;) {
			do len--; while (len && ((unsigned char)title[len] & 0xc0) == 0x80);
			snprintf(tt, sizeof tt, "%.*s…", (int)len, title);
			XftTextExtentsUtf8(dpy, f, (const FcChar8 *)tt, (int)strlen(tt), &gi);
		}
		XftTextExtentsUtf8(dpy, f, (const FcChar8 *)tt, (int)strlen(tt), &gi);
		int ty = (toff - f->ascent - f->descent) / 2 + f->ascent;
		XftDrawStringUtf8(xd, xcolor(i == cur ? fgcol : (dark ? 0x98989d : 0x6e6e73), &col), f,
		                  x + (w - gi.xOff) / 2, ty, (const FcChar8 *)tt, (int)strlen(tt));
		/* × */
		XftDrawStringUtf8(xd, xcolor(dark ? 0x8e8e93 : 0x8e8e93, &col), f, x + w - 20, ty, (const FcChar8 *)"×", 2);
	}
	/* ＋ */
	XGlyphInfo gi;
	XftTextExtentsUtf8(dpy, f, (const FcChar8 *)"+", 1, &gi);
	int ty = (toff - f->ascent - f->descent) / 2 + f->ascent;
	XftDrawStringUtf8(xd, xcolor(fgcol, &col), f, winw - 18 - gi.xOff / 2, ty, (const FcChar8 *)"+", 1);
}

/* 原生：分頁列用 libpomelo 的字型（跟其他 Pomelo 程式的介面字一樣，Noto Sans CJK），畫在影像緩衝上 */
static void draw_tabbar_native(void)
{
	if (ntabs < 1) return;
	cairo_t *cr = bufcr;
	int dark = ((bgcol >> 16 & 255) + (bgcol >> 8 & 255) + (bgcol & 255)) < 384;
	uint32_t barbg = dark ? 0x101012 : 0xdedee2, line = dark ? 0x3a3a3c : 0xc7c7cc;
	P_rect(barbg, 0, 0, winw, toff);
	P_rect(line, 0, toff - 1, winw, 1);
	int w = tab_width();
	pm_font(cr, 13, 0);
	for (int i = 0; i < ntabs; i++) {
		int x = i * w;
		const char *title = i == cur ? cur_title : tab[i]->title;
		if (i == cur) P_rect(bgcol, x, 0, w, toff);
		else if (i) P_rect(line, x, 7, 1, toff - 14);
		char tt[260];
		pm_ellipsize(cr, title, w - 44, tt, sizeof tt);   /* 標題太長就從後面截斷，加上「…」 */
		double tw = pm_textw(cr, tt);
		pm_rgb(cr, i == cur ? fgcol : (dark ? 0x98989d : 0x6e6e73));
		pm_text(cr, x + (w - tw) / 2, toff / 2.0, tt);
		pm_rgb(cr, 0x8e8e93);
		pm_text(cr, x + w - 20, toff / 2.0, "×");
	}
	pm_rgb(cr, fgcol);
	pm_text(cr, winw - 18 - pm_textw(cr, "+") / 2, toff / 2.0, "+");
}

static void draw_tabbar(void)
{
	if (native) draw_tabbar_native();
	else draw_tabbar_x();
}

static void new_window(void)
{
	char cwd[PATH_MAX] = "", link[64];
	snprintf(link, sizeof link, "/proc/%d/cwd", (int)child);
	ssize_t k = readlink(link, cwd, sizeof cwd - 1);
	cwd[k > 0 ? k : 0] = 0;
	pid_t mid = fork();
	if (mid == 0) {
		if (fork() == 0) {
			close(ptyfd);
			if (dpy) close(ConnectionNumber(dpy));
			setsid();
			execlp("pomelo-terminal", "pomelo-terminal", "-d", cwd[0] ? cwd : ".", (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (mid > 0) waitpid(mid, NULL, 0);
}

static void zoom_font(double size)
{
	size = CLAMP(size, 6, 48);
	load_fonts(size);
	if (native) {
		pmwl_set_min_size(wl, 2 * tpad + 10 * cw, 2 * tpad + 2 * chh + (ntabs > 0 ? TABBAR_H : 0));
	} else {
		XSizeHints *h = XAllocSizeHints();
		h->flags = PMinSize | PResizeInc | PBaseSize;
		h->base_width = h->base_height = 2 * tpad;
		h->width_inc = cw; h->height_inc = chh;
		h->min_width = 2 * tpad + 10 * cw; h->min_height = 2 * tpad + 2 * chh;
		XSetWMNormalHints(dpy, win, h);
		XFree(h);
	}
	relayout();
}

static void view_scroll(int n)
{
	if (t.mode & M_ALT) return;
	int ns = CLAMP(scrolloff + n, 0, hn);
	if (ns != scrolloff) { scrolloff = ns; alldirty(); }
}

static void keypress(XKeyEvent *e)
{
	char b[64], seq[32];
	KeySym ks = NoSymbol, kn = NoSymbol;
	Status st = 0;
	int len = native ? pmwl_xkey(e, b, sizeof b, &ks, &kn)
	                 : xic ? Xutf8LookupString(xic, e, b, sizeof b - 1, &ks, &st)
	                       : XLookupString(e, b, sizeof b - 1, &ks, NULL);
	if (!native && xic && st == XBufferOverflow) return;
	if (len < 0) len = 0;
#define K0(e) (native ? kn : XLookupKeysym((e), 0))
	unsigned m = e->state;
	int shift = !!(m & ShiftMask), ctrl = !!(m & ControlMask), alt = !!(m & Mod1Mask), sup = !!(m & Mod4Mask);

	/* 切換分頁：Ctrl+Tab／Ctrl+Shift+Tab、Ctrl+PageUp／PageDown、Super+1～9 */
	if (ntabs > 1) {
		KeySym k0 = K0(e);
		int to = -1;
		if (ctrl && (k0 == XK_Tab || ks == XK_ISO_Left_Tab)) to = (cur + (shift ? ntabs - 1 : 1)) % ntabs;
		else if (ctrl && !shift && k0 == XK_Page_Down) to = (cur + 1) % ntabs;
		else if (ctrl && !shift && k0 == XK_Page_Up) to = (cur + ntabs - 1) % ntabs;
		else if (sup && k0 >= XK_1 && k0 <= XK_9) to = MIN((int)(k0 - XK_1), ntabs - 1);
		if (to >= 0) { switch_tab(to); return; }
	}

	/* 快捷鍵：Super（⌘）或 Ctrl+Shift */
	if (sup || (ctrl && shift)) {
		KeySym k = K0(e);
		switch (k) {
		case XK_c: own_selection(a_clipboard, sel_text()); return;
		case XK_v: paste(a_clipboard); return;
		case XK_n: new_window(); return;
		case XK_t: new_tab(); return;
		case XK_w: if (ctrl && shift) { close_tab(cur); return; } break;
		case XK_k: term_clear(); return;
		case XK_equal: case XK_plus: case XK_KP_Add: zoom_font(fsize + 1); return;
		case XK_minus: case XK_KP_Subtract: zoom_font(fsize - 1); return;
		case XK_0: zoom_font(fsize0); return;
		}
		if (sup) return;
	}
	if (shift && !ctrl && !alt) {
		switch (ks) {
		case XK_Prior: view_scroll(t.rows - 1); return;
		case XK_Next: view_scroll(-(t.rows - 1)); return;
		case XK_Home: view_scroll(hn); return;
		case XK_End: view_scroll(-hn); return;
		case XK_Insert: paste(XA_PRIMARY); return;
		}
	}

	int mod = 1 + shift + alt * 2 + ctrl * 4;
	const char *s = NULL;
	char fin = 0;
	int tilde = 0;
	switch (ks) {
	case XK_Up: case XK_KP_Up: fin = 'A'; break;
	case XK_Down: case XK_KP_Down: fin = 'B'; break;
	case XK_Right: case XK_KP_Right: fin = 'C'; break;
	case XK_Left: case XK_KP_Left: fin = 'D'; break;
	case XK_Home: case XK_KP_Home: fin = 'H'; break;
	case XK_End: case XK_KP_End: fin = 'F'; break;
	case XK_Insert: case XK_KP_Insert: tilde = 2; break;
	case XK_Delete: case XK_KP_Delete: tilde = 3; break;
	case XK_Prior: case XK_KP_Prior: tilde = 5; break;
	case XK_Next: case XK_KP_Next: tilde = 6; break;
	case XK_F5: tilde = 15; break;
	case XK_F6: tilde = 17; break;
	case XK_F7: tilde = 18; break;
	case XK_F8: tilde = 19; break;
	case XK_F9: tilde = 20; break;
	case XK_F10: tilde = 21; break;
	case XK_F11: tilde = 23; break;
	case XK_F12: tilde = 24; break;
	case XK_F1: case XK_F2: case XK_F3: case XK_F4:
		if (mod > 1) snprintf(seq, sizeof seq, "\033[1;%d%c", mod, (char)('P' + ks - XK_F1));
		else snprintf(seq, sizeof seq, "\033O%c", (char)('P' + ks - XK_F1));
		s = seq;
		break;
	case XK_ISO_Left_Tab: s = "\033[Z"; break;
	case XK_BackSpace: s = ctrl ? "\b" : alt ? "\033\177" : "\177"; break;
	case XK_Return: case XK_KP_Enter: s = alt ? "\033\r" : "\r"; break;
	}
	if (fin) {
		if (mod > 1) snprintf(seq, sizeof seq, "\033[1;%d%c", mod, fin);
		else snprintf(seq, sizeof seq, (t.mode & M_APPCUR) ? "\033O%c" : "\033[%c", fin);
		s = seq;
	} else if (tilde) {
		if (mod > 1) snprintf(seq, sizeof seq, "\033[%d;%d~", tilde, mod);
		else snprintf(seq, sizeof seq, "\033[%d~", tilde);
		s = seq;
	}
	if (s) {
		ttywrite(s, strlen(s));
	} else if (len > 0) {
		if (alt && len == 1) ttywrite("\033", 1);
		ttywrite(b, (size_t)len);
	} else {
		return;
	}
	if (scrolloff) { scrolloff = 0; alldirty(); }
}

static void mouse_report(int code, int x, int y, int release, int motion, unsigned state)
{
	char s[48];
	int b = code;
	if (!(t.mode & M_MOUSEX10)) {
		if (state & ShiftMask) b += 4;
		if (state & Mod1Mask) b += 8;
		if (state & ControlMask) b += 16;
	}
	if (motion) b += 32;
	int k;
	if (t.mode & M_MOUSESGR) {
		k = snprintf(s, sizeof s, "\033[<%d;%d;%d%c", b, x + 1, y + 1, release ? 'm' : 'M');
	} else {
		if (x > 222 || y > 222) return;
		if (release) b = 3 + (b & ~3);
		k = snprintf(s, sizeof s, "\033[M%c%c%c", 32 + b, 33 + x, 33 + y);
	}
	ttywrite(s, (size_t)k);
}

static int btn_code(unsigned b)
{
	switch (b) {
	case Button1: return 0;
	case Button2: return 1;
	case Button3: return 2;
	case 4: return 64;
	case 5: return 65;
	case 6: return 66;
	case 7: return 67;
	}
	return -1;
}

static void mouse_pos(int px, int py, int *x, int *row)
{
	*x = CLAMP((px - tpad) / cw, 0, t.cols - 1);
	*row = CLAMP((py - tpad - toff) / chh, 0, t.rows - 1);
}

/* ------------------------------------------------------------------ */
/* 右鍵選單                                                            */
/* ------------------------------------------------------------------ */

/* 清除畫面與捲動紀錄（Ctrl+Shift+K、右鍵選單） */
static void term_clear(void)
{
	hist_clear();
	scroll_up(0, t.cy, 0);
	t.cy = 0;
	alldirty();
}

static char menu_url[2048];
static void m_copy(const Arg *a) { (void)a; own_selection(a_clipboard, sel_text()); }
static void m_paste(const Arg *a) { (void)a; paste(a_clipboard); }
static void m_selall(const Arg *a)
{
	(void)a;
	sel.active = 1; sel.dragging = 0; sel.snap = 0;
	sel.sx = 0; sel.sy = 0;
	sel.ex = t.cols - 1; sel.ey = hn + t.rows - 1;   /* 捲動紀錄加上畫面 */
	alldirty();
	own_selection(XA_PRIMARY, sel_text());
}
static void m_open_url(const Arg *a) { (void)a; pm_open(menu_url); }
static void m_files(const Arg *a)
{
	(void)a;
	char d[PATH_MAX] = "";
	child_cwd(d, sizeof d);
	if (d[0]) pm_open(d);
}
static void m_newtab(const Arg *a) { (void)a; new_tab(); }
static void m_newwin(const Arg *a) { (void)a; new_window(); }
static void m_zoomin(const Arg *a) { (void)a; zoom_font(fsize + 1); }
static void m_zoomout(const Arg *a) { (void)a; zoom_font(fsize - 1); }
static void m_zoom0(const Arg *a) { (void)a; zoom_font(fsize0); }
static void m_clear(const Arg *a) { (void)a; term_clear(); }
static void m_closetab(const Arg *a) { (void)a; close_tab(cur); }

/* 選單文字「名稱<Tab>快捷鍵」：名稱是原文，pm_popup 畫的時候才翻譯 */
static const char *menu_label(const char *name, const char *key)
{
	static char pool[16][160];
	static int k;
	char *b = pool[k++ % 16];
	snprintf(b, sizeof pool[0], "%s%s%s", name, key ? "\t" : "", key ? key : "");
	return b;
}

static void term_menu(int x, int y)
{
	/* 選取的是一個網址：多一項「開啟連結」 */
	menu_url[0] = 0;
	char *s = sel_text();
	if (s) {
		char *u = s;
		while (*u == ' ') u++;
		u[strcspn(u, " \t\r\n")] = 0;
		if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) snprintf(menu_url, sizeof menu_url, "%s", u);
		else if (!strncmp(u, "www.", 4)) snprintf(menu_url, sizeof menu_url, "https://%s", u);
		free(s);
	}
	MenuItem m[16];
	int k = 0;
	m[k++] = (MenuItem){ menu_label(N_("複製"), "Ctrl+Shift+C"), sel.active ? m_copy : pm_menu_off, {0} };
	m[k++] = (MenuItem){ menu_label(N_("貼上"), "Ctrl+Shift+V"), m_paste, {0} };
	m[k++] = (MenuItem){ menu_label(N_("全選"), NULL), m_selall, {0} };
	if (menu_url[0]) m[k++] = (MenuItem){ menu_label(N_("開啟連結"), NULL), m_open_url, {0} };
	m[k++] = (MenuItem){ NULL, NULL, {0} };
	m[k++] = (MenuItem){ menu_label(N_("新分頁"), "Ctrl+Shift+T"), m_newtab, {0} };
	m[k++] = (MenuItem){ menu_label(N_("新視窗"), "Ctrl+Shift+N"), m_newwin, {0} };
	m[k++] = (MenuItem){ menu_label(N_("在檔案管理員中開啟這個資料夾"), NULL), m_files, {0} };
	m[k++] = (MenuItem){ NULL, NULL, {0} };
	m[k++] = (MenuItem){ menu_label(N_("放大"), "Ctrl+Shift++"), m_zoomin, {0} };
	m[k++] = (MenuItem){ menu_label(N_("縮小"), "Ctrl+Shift+-"), m_zoomout, {0} };
	m[k++] = (MenuItem){ menu_label(N_("實際大小"), "Ctrl+Shift+0"), m_zoom0, {0} };
	m[k++] = (MenuItem){ NULL, NULL, {0} };
	m[k++] = (MenuItem){ menu_label(N_("清除畫面"), "Ctrl+Shift+K"), m_clear, {0} };
	if (ntabs > 1) m[k++] = (MenuItem){ menu_label(N_("關閉分頁"), "Ctrl+Shift+W"), m_closetab, {0} };
	PMWL_POPUP(wl, m, k, x, y);
}

static void bpress(XButtonEvent *e)
{
	if (ntabs > 0 && e->y < toff) {
		int close, i = tab_hit(e->x, e->y, &close);
		if (e->button == Button1) {
			if (i == -2) new_tab();
			else if (i >= 0 && close) close_tab(i);
			else if (i >= 0) switch_tab(i);
		} else if (e->button == Button2 && i >= 0) {
			close_tab(i);
		} else if (e->button == 4 || e->button == 5) {
			switch_tab((cur + (e->button == 4 ? ntabs - 1 : 1)) % ntabs);
		}
		return;
	}
	int x, row;
	mouse_pos(e->x, e->y, &x, &row);
	evtime = e->time;
	if ((t.mode & M_MOUSEMASK) && !(e->state & ShiftMask)) {
		int code = btn_code(e->button);
		if (code < 0) return;
		mouse_report(code, x, row, 0, 0, e->state);
		if (code < 64) { mouse_btn = code; mouse_lx = x; mouse_ly = row; }
		return;
	}
	if (e->button == 4 || e->button == 5) {
		int up = e->button == 4;
		if (t.mode & M_ALT) {   /* 替代畫面（less、man）：滾輪變成方向鍵 */
			const char *k = up ? ((t.mode & M_APPCUR) ? "\033OA" : "\033[A") : ((t.mode & M_APPCUR) ? "\033OB" : "\033[B");
			for (int i = 0; i < 3; i++) ttywrite(k, 3);
		} else {
			view_scroll(up ? 3 : -3);
		}
		return;
	}
	/* 右鍵選單（裡面的程式要滑鼠時給程式，按住 Shift 再按右鍵一樣出選單） */
	if (e->button == Button3) {
		term_menu(e->x_root, e->y_root);
		return;
	}
	if (e->button == Button2) {
		paste(XA_PRIMARY);
		return;
	}
	if (e->button != Button1)
		return;
	int v = hn - scrolloff + row;
	if (e->time - lastclick < 350 && x == lastcx && v == lastcy) clicks = clicks % 3 + 1;
	else clicks = 1;
	lastclick = e->time; lastcx = x; lastcy = v;
	sel_clear();
	sel.sx = sel.ex = x;
	sel.sy = sel.ey = v;
	sel.snap = clicks;
	sel.dragging = 1;
	if (clicks > 1) {
		sel.active = 1;
		sel_snap();
		alldirty();
	}
}

static void brelease(XButtonEvent *e)
{
	int x, row;
	mouse_pos(e->x, e->y, &x, &row);
	evtime = e->time;
	if ((t.mode & M_MOUSEMASK) && !(e->state & ShiftMask)) {
		int code = btn_code(e->button);
		if (code >= 0 && code < 64 && !(t.mode & M_MOUSEX10))
			mouse_report(code, x, row, 1, 0, e->state);
		mouse_btn = -1;
		return;
	}
	if (e->button == Button1 && sel.dragging) {
		sel.dragging = 0;
		if (sel.active)
			own_selection(XA_PRIMARY, sel_text());
	}
}

static void bmotion(XMotionEvent *e)
{
	int x, row;
	mouse_pos(e->x, e->y, &x, &row);
	if ((t.mode & M_MOUSEMASK) && !(e->state & ShiftMask)) {
		if (!(t.mode & (M_MOUSEBTN | M_MOUSEANY)) || (x == mouse_lx && row == mouse_ly)) return;
		if (!(t.mode & M_MOUSEANY) && mouse_btn < 0) return;
		mouse_lx = x; mouse_ly = row;
		mouse_report(mouse_btn >= 0 ? mouse_btn : 3, x, row, 0, 1, e->state);
		return;
	}
	if (!sel.dragging) return;
	/* 拖到視窗外時順便捲動 */
	if (e->y < tpad + toff) view_scroll(1);
	else if (e->y >= winh - tpad) view_scroll(-1);
	int v = hn - scrolloff + row;
	if (x == sel.ex && v == sel.ey && sel.active) return;
	sel.ex = x; sel.ey = v;
	sel.active = 1;
	if (sel.snap > 1)
		sel_snap();   /* 雙擊 / 三擊後拖曳：以字或行為單位延伸 */
	alldirty();
}

/* ------------------------------------------------------------------ */
/* 視窗                                                                */
/* ------------------------------------------------------------------ */

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

/* 程式圖示：灰色外框裡的黑色螢幕，上面一個 >_ */
static cairo_surface_t *make_icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 8, 0, 120);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.93, 0.93, 0.94);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.72, 0.72, 0.75);
	rrect(cr, 8, 10, 112, 108, 24);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	rrect(cr, 15, 17, 98, 94, 15);
	cairo_set_source_rgb(cr, 0.11, 0.11, 0.12);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 7);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_move_to(cr, 33, 44);
	cairo_line_to(cr, 49, 58);
	cairo_line_to(cr, 33, 72);
	cairo_stroke(cr);
	cairo_move_to(cr, 58, 74);
	cairo_line_to(cr, 82, 74);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

static void set_icon(void)
{
	int sizes[] = { 32, 64, 128 };
	size_t total = 0;
	for (unsigned i = 0; i < LEN(sizes); i++) total += 2 + (size_t)sizes[i] * sizes[i];
	unsigned long *d = xmalloc(total * sizeof *d), *p = d;
	for (unsigned i = 0; i < LEN(sizes); i++) {
		int n = sizes[i];
		cairo_surface_t *s = make_icon(n);
		cairo_surface_flush(s);
		unsigned char *px = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		*p++ = (unsigned long)n;
		*p++ = (unsigned long)n;
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

static void resize_window(int w, int h)
{
	if (w == winw && h == winh) return;
	winw = w; winh = h;
	if (native) {   /* 原生：影像緩衝跟著視窗大小 */
		if (bufcr) cairo_destroy(bufcr);
		if (bufsurf) cairo_surface_destroy(bufsurf);
		bufsurf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, MAX(w, 1), MAX(h, 1));
		bufcr = cairo_create(bufsurf);
		relayout();
		return;
	}
	if (xd) XftDrawDestroy(xd);
	if (bufcr) { cairo_destroy(bufcr); cairo_surface_destroy(bufsurf); bufcr = NULL; bufsurf = NULL; }
	if (buf) XFreePixmap(dpy, buf);
	buf = XCreatePixmap(dpy, win, w, h, DefaultDepth(dpy, scr));
	xd = XftDrawCreate(dpy, buf, vis, cmap);
	relayout();
}

/* 原生 Wayland：沒有 X 連線，字型用 cairo 載入（見 load_fonts_native），畫在自己的影像緩衝上 */
static void ninit(void)
{
	if (!FcInit()) die(_("fontconfig 初始化失敗"));
	load_fonts(0);   /* 先取得預設大小 */
	double want = pm_conf_num("terminal.font_size", 0);
	if (want >= 6) fsize0 = want;
	load_fonts(fsize0);
	load_colors();
	winw = 2 * tpad + tcols * cw;
	winh = 2 * tpad + trows * chh;
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	pm_set_theme(pm_conf_int("appearance.dark", 0), pm_conf_color("appearance.accent", 0));
	wl = pmwl_window(cmd_title ? cmd_title : _("終端機"), "pomelo-terminal", winw, winh,
	                 2 * tpad + 10 * cw, 2 * tpad + 2 * chh + TABBAR_H, NULL);
	if (!wl) die(_("無法開啟 Wayland 視窗"));
	win = PMWL_XID(wl);
	a_delete = PMWL_XCLOSE;
	pmwl_want(wl, PMWL_IN_PREEDIT | PMWL_IN_SCROLL_FINE);
	pmwl_ime_purpose(PMWL_IME_TERMINAL);
	static const char *const uri[] = { "text/uri-list" };
	pmwl_dnd_target(wl, uri, 1, PMWL_DND_COPY);   /* 拖檔案進來＝打出路徑 */
	bufsurf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, winw, winh);
	bufcr = cairo_create(bufsurf);
	set_title(cmd_title ? cmd_title : _("終端機"));
}

static void xinit(void)
{
	if (!(dpy = XOpenDisplay(NULL))) die(_("無法開啟 X display"));
	scr = DefaultScreen(dpy);
	vis = DefaultVisual(dpy, scr);
	cmap = DefaultColormap(dpy, scr);
	if (!FcInit()) die(_("fontconfig 初始化失敗"));
	load_fonts(0);   /* 先取得預設大小 */
	double want = pm_conf_num("terminal.font_size", 0);
	if (want >= 6) fsize0 = want;
	load_fonts(fsize0);
	load_colors();

	winw = 2 * tpad + tcols * cw;
	winh = 2 * tpad + trows * chh;
	XSetWindowAttributes wa = {
		.background_pixel = bgcol,
		.bit_gravity = NorthWestGravity, .backing_store = WhenMapped,
		.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask |
		              ExposureMask | StructureNotifyMask | FocusChangeMask,
	};
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0, winw, winh, 0, CopyFromParent, InputOutput,
	                    vis, CWBackPixel | CWBitGravity | CWBackingStore | CWEventMask, &wa);
	tgc = XCreateGC(dpy, win, 0, NULL);
	XSetGraphicsExposures(dpy, tgc, False);
	/* 右鍵選單用 libpomelo 的 pm_popup：字型、深淺色跟其他 Pomelo 程式一樣；選單開著時終端機自己的事件
	 * （重畫、別的程式要剪貼簿）照樣交給 handle */
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	pm_dispatch = handle;
	buf = XCreatePixmap(dpy, win, winw, winh, DefaultDepth(dpy, scr));
	xd = XftDrawCreate(dpy, buf, vis, cmap);

	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_targets = XInternAtom(dpy, "TARGETS", False);
	a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
	a_paste = XInternAtom(dpy, "POMELO_PASTE", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_neticon = XInternAtom(dpy, "_NET_WM_ICON", False);
	a_netpid = XInternAtom(dpy, "_NET_WM_PID", False);
	a_text = XInternAtom(dpy, "TEXT", False);

	XClassHint ch = { "pomelo-terminal", "Pomelo-terminal" };
	XWMHints wm = { .flags = InputHint, .input = True };
	XSizeHints *sh = XAllocSizeHints();
	sh->flags = PMinSize | PResizeInc | PBaseSize;
	sh->base_width = sh->base_height = 2 * tpad;
	sh->width_inc = cw; sh->height_inc = chh;
	sh->min_width = 2 * tpad + 10 * cw; sh->min_height = 2 * tpad + 2 * chh;
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sh, &wm, &ch);
	XFree(sh);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	pm_dnd_accept(dpy, win);   /* 收拖進來的檔案 */
	long pid = getpid();
	XChangeProperty(dpy, win, a_netpid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	set_icon();
	set_title(cmd_title ? cmd_title : _("終端機"));

	/* 輸入法 */
	/* 輸入法：伺服器結束或重新啟動時自動處理（不然繼續用舊的連線會當掉）；
	 * 現在沒有輸入法伺服器就先用本機的（組合鍵），之後伺服器起來也會自動連上 */
	pm_xim_attach(dpy, &xim, &xic, win);
	if (!xim) {
		XSetLocaleModifiers("@im=local");
		if ((xim = XOpenIM(dpy, NULL, NULL, NULL)))
			xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win, XNFocusWindow, win, NULL);
		XSetLocaleModifiers("");
	}
	XMapWindow(dpy, win);
	XSync(dpy, False);
}

/* 拖進來的檔案：把路徑打進去（需要的話加單引號），跟別的終端機一樣 */
static void type_paths(char **paths, int n)
{
	if (pty_dead) return;
	if (t.mode & M_BRPASTE) ttywrite("\033[200~", 6);
	for (int i = 0; i < n; i++) {
		const char *p = paths[i];
		int plain = 1;
		for (const char *q = p; *q; q++)
			if (!isalnum((unsigned char)*q) && !strchr("/._-+,:@%=", *q) && (unsigned char)*q < 0x80) plain = 0;
		if (plain) ttywrite(p, strlen(p));
		else {
			ttywrite("'", 1);
			for (const char *q = p; *q; q++) {
				if (*q == '\'') ttywrite("'\\''", 4);
				else ttywrite(q, 1);
			}
			ttywrite("'", 1);
		}
		ttywrite(" ", 1);
	}
	if (t.mode & M_BRPASTE) ttywrite("\033[201~", 6);
	if (scrolloff) { scrolloff = 0; alldirty(); }
}

/* 設定程式改了外觀：換配色、字型大小 */
static void conf_changed(void)
{
	load_colors();
	if (!native) XSetWindowBackground(dpy, win, bgcol);
	pm_set_theme(pm_conf_int("appearance.dark", 0), pm_conf_color("appearance.accent", 0));   /* 右鍵選單的顏色 */
	double want = pm_conf_num("terminal.font_size", 0);
	if (want >= 6 && want != fsize0) { fsize0 = want; zoom_font(want); }
	full_redraw = 1;
}

static void handle(XEvent *e)
{
	char **dropped;
	int nd = native ? -1 : pm_dnd_event(dpy, e, &dropped);
	if (nd >= 0) {
		if (nd > 0) { type_paths(dropped, nd); pm_dnd_free(dropped, nd); }
		return;
	}
	switch (e->type) {
	case KeyPress: evtime = e->xkey.time; keypress(&e->xkey); break;
	case ButtonPress: bpress(&e->xbutton); break;
	case ButtonRelease: brelease(&e->xbutton); break;
	case MotionNotify: bmotion(&e->xmotion); break;
	case ConfigureNotify: resize_window(e->xconfigure.width, e->xconfigure.height); break;
	case Expose:
		if (native) { full_redraw = 1; break; }
		XCopyArea(dpy, buf, win, tgc, e->xexpose.x, e->xexpose.y, e->xexpose.width, e->xexpose.height,
		          e->xexpose.x, e->xexpose.y);
		break;
	case FocusIn:
	case FocusOut:
		if (e->xfocus.mode == NotifyGrab) break;
		focused = e->type == FocusIn;
		if (!native && xic) { if (focused) XSetICFocus(xic); else XUnsetICFocus(xic); }
		if (t.mode & M_FOCUS) ttywrite(focused ? "\033[I" : "\033[O", 3);
		t.dirty[t.cy] = 1;
		break;
	case ClientMessage:
		if ((Atom)e->xclient.data.l[0] == a_delete) {
			tab_save(tab[cur]);
			for (int i = 0; i < ntabs; i++) if (tab[i]->child > 0) kill(tab[i]->child, SIGHUP);
			exit(0);
		}
		break;
	case SelectionRequest: on_selrequest(&e->xselectionrequest); break;
	case SelectionNotify: on_selnotify(&e->xselection); break;
	case SelectionClear:
		if (e->xselectionclear.selection == XA_PRIMARY) sel_clear();
		break;
	case MappingNotify: XRefreshKeyboardMapping(&e->xmapping); break;
	case PropertyNotify:
		if (!native && pm_conf_changed(dpy, e)) conf_changed();
		break;
	}
}

/* 原生：精細捲動（滾輪一格＝15 像素，觸控板是連續的像素）。滑鼠回報模式送滾輪按鍵、替代畫面送方向鍵，
 * 其他情況捲動緩衝（一格 3 行，跟 X 後端一樣） */
static void fine_scroll(const PmwEvent *e)
{
	static double acc;
	acc += e->h / 256.0;   /* 正的＝往下 */
	if (ntabs > 0 && e->y < toff) {   /* 分頁列：一格換一個分頁 */
		int n = (int)(acc / 15);
		acc -= n * 15;
		if (n) switch_tab(((cur + (n > 0 ? 1 : ntabs - 1)) % ntabs));
		return;
	}
	int n = (int)(acc / 15);
	if ((t.mode & M_MOUSEMASK) && !(e->state & ShiftMask)) {
		acc -= n * 15;
		int x, row;
		mouse_pos(e->x, e->y, &x, &row);
		for (int k = 0; k < abs(n); k++) mouse_report(n > 0 ? 65 : 64, x, row, 0, 0, e->state);
	} else if (t.mode & M_ALT) {
		acc -= n * 15;
		const char *k = n < 0 ? ((t.mode & M_APPCUR) ? "\033OA" : "\033[A") : ((t.mode & M_APPCUR) ? "\033OB" : "\033[B");
		for (int i = 0; i < abs(n) * 3; i++) ttywrite(k, 3);
	} else {
		int lines = (int)(acc * 0.2);   /* 15 像素＝3 行 */
		acc -= lines * 5.0;
		view_scroll(-lines);
	}
}

static void reap_children(int sig)
{
	(void)sig;
	int e = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0) ;
	errno = e;
}

static void usage(void)
{
	puts(_("用法：pomelo-terminal [-t 標題] [-d 目錄] [-e 指令 [參數…]]\n"
	       "      pomelo-terminal --icon 檔案.png   （輸出程式圖示）"));
	exit(0);
}

int main(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-e") && i + 1 < argc) { cmd_argv = &argv[i + 1]; break; }
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) cmd_title = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) cmd_dir = argv[++i];
		else if (!strcmp(argv[i], "--icon") && i + 1 < argc) {
			cairo_surface_t *s = make_icon(256);
			int ok = cairo_surface_write_to_png(s, argv[i + 1]) == CAIRO_STATUS_SUCCESS;
			cairo_surface_destroy(s);
			return !ok;
		} else if (!strcmp(argv[i], "-v")) { puts("pomelo-terminal 0.1"); return 0; }
		else usage();
	}
	setlocale(LC_CTYPE, "");
	signal(SIGPIPE, SIG_IGN);
	/* 分頁關掉後，結束的 shell 要回收，不然會變成殭屍程序 */
	struct sigaction sa = { .sa_handler = reap_children, .sa_flags = SA_RESTART | SA_NOCLDSTOP };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);

	hcap = MAX(scrollback, 0);
	hist = hcap ? calloc(hcap, sizeof *hist) : NULL;
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	native = pmwl_connect();
	if (native) { pm_ui_native = 1; ninit(); }
	else xinit();
	tresize(tcols, trows);
	treset();
	set_mode(0);   /* 設定事件遮罩（含輸入法需要的事件） */
	if (!native) pm_conf_watch(dpy);
	spawn_shell();
	tab[0] = calloc(1, sizeof(Tab));
	ntabs = 1;
	cur = 0;
	snprintf(cur_title, sizeof cur_title, "%s", cmd_title ? cmd_title : _("終端機"));
	tab_save(tab[0]);
	relayout();   /* 一開始就有分頁列（右邊有「＋」） */

	if (native) {
		long lastdraw = 0;
		int pending = 1;
		for (;;) {
			struct pollfd pf[32];
			for (int i = 0; i < ntabs; i++) pf[i] = (struct pollfd){ i == cur ? ptyfd : tab[i]->ptyfd, POLLIN, 0 };   /* fd 是 -1 的 poll 會略過 */
			long wait = -1;
			if (pending) {
				wait = MAX(0, 8 - (now_ms() - lastdraw));
				if ((t.mode & M_SYNC) && now_ms() - sync_since < 150) wait = 10;
			}
			PmwEvent pe;
			int got = pmwl_next_fds(&pe, wait, pf, ntabs);
			/* 背景分頁（先記下哪些有資料，處理時 tab[] 可能因為關閉而變動） */
			int bg_ready[32], nbg = 0;
			for (int i = 0; i < ntabs; i++)
				if (i != cur && tab[i]->ptyfd >= 0 && (pf[i].revents & (POLLIN | POLLHUP | POLLERR))) bg_ready[nbg++] = tab[i]->ptyfd;
			if (pf[cur].revents & (POLLIN | POLLHUP | POLLERR)) {
				long start = now_ms();
				fd_set one;
				do {
					if (ttyread() < 0) break;
					struct timeval z = { 0, 0 };
					FD_ZERO(&one);
					FD_SET(ptyfd, &one);
					if (select(ptyfd + 1, &one, NULL, NULL, &z) <= 0) break;
				} while (now_ms() - start < 12);
				if (pty_dead) close_tab(cur);
				pending = 1;
			}
			for (int k = 0; k < nbg; k++)
				for (int i = 0; i < ntabs; i++)
					if (i != cur && tab[i]->ptyfd == bg_ready[k]) { read_background(i); pending = 1; break; }
			while (got) {
				XEvent ev;
				switch (pe.type) {
				case PMW_CONF: conf_changed(); break;
				case PMW_PREEDIT: snprintf(preedit, sizeof preedit, "%s", pe.text); break;
				case PMW_SCROLL_FINE: fine_scroll(&pe); break;
				case PMW_DND_DROP: {   /* 拖進來的檔案：把路徑打進去 */
					char **dropped;
					int nd = pmwl_dnd_files(&dropped);
					if (nd > 0) { type_paths(dropped, nd); pm_dnd_free(dropped, nd); }
					break;
				}
				case PMW_KEY: preedit[0] = 0; /* fallthrough */
				default:
					if (pmwl_to_xevent(&pe, &ev)) handle(&ev);
				}
				pending = 1;
				got = pmwl_next(&pe, 0);
			}
			if (pending && now_ms() - lastdraw >= 8 && !((t.mode & M_SYNC) && now_ms() - sync_since < 150)) {
				draw();
				lastdraw = now_ms();
				pending = 0;
			}
		}
	}
	int xfd = ConnectionNumber(dpy);
	long lastdraw = 0;
	int pending = 1;
	for (;;) {
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(xfd, &rf);
		int maxfd = xfd;
		for (int i = 0; i < ntabs; i++) {
			int fd = i == cur ? ptyfd : tab[i]->ptyfd;
			if (fd >= 0) { FD_SET(fd, &rf); maxfd = MAX(maxfd, fd); }
		}
		struct timeval tv, *tvp = NULL;
		if (pending) {
			long wait = MAX(0, 8 - (now_ms() - lastdraw));
			if ((t.mode & M_SYNC) && now_ms() - sync_since < 150) wait = 10;
			tv.tv_sec = 0;
			tv.tv_usec = wait * 1000;
			tvp = &tv;
		}
		if (select(maxfd + 1, &rf, NULL, NULL, tvp) < 0) {
			if (errno == EINTR) continue;
			die(_("select 失敗"));
		}
		/* 背景分頁（先記下哪些有資料，處理時 tab[] 可能因為關閉而變動） */
		int bg_ready[32], nbg = 0;
		for (int i = 0; i < ntabs; i++)
			if (i != cur && tab[i]->ptyfd >= 0 && FD_ISSET(tab[i]->ptyfd, &rf)) bg_ready[nbg++] = tab[i]->ptyfd;
		if (FD_ISSET(ptyfd, &rf)) {
			/* 一次多讀一些，但不要讓畫面卡住太久 */
			long start = now_ms();
			fd_set one;
			do {
				if (ttyread() < 0) break;
				struct timeval z = { 0, 0 };
				FD_ZERO(&one);
				FD_SET(ptyfd, &one);
				if (select(ptyfd + 1, &one, NULL, NULL, &z) <= 0) break;
			} while (now_ms() - start < 12);
			if (pty_dead) close_tab(cur);
			pending = 1;
		}
		for (int k = 0; k < nbg; k++)
			for (int i = 0; i < ntabs; i++)
				if (i != cur && tab[i]->ptyfd == bg_ready[k]) { read_background(i); pending = 1; break; }
		while (XPending(dpy)) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			if (XFilterEvent(&ev, None)) continue;
			handle(&ev);
			pending = 1;
		}
		if (pending && now_ms() - lastdraw >= 8 && !((t.mode & M_SYNC) && now_ms() - sync_since < 150)) {
			draw();
			lastdraw = now_ms();
			pending = 0;
		}
	}
}
