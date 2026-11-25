/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 共用的繪圖、圖示、選單與螢幕工具 */
#define _GNU_SOURCE
#include "internal.h"
#include <X11/Xresource.h>   /* XUniqueContext()（新版 libX11 的 Xutil.h 不會自動帶進來） */

/* SVG 圖示：內嵌的 nanosvg（zlib 授權），符號不對外公開，免得跟程式自己的衝突 */
#pragma GCC visibility push(hidden)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "third-party/nanosvg.h"
#include "third-party/nanosvgrast.h"
#pragma GCC diagnostic pop
#pragma GCC visibility pop

Display *dpy;
int scr, sw, sh, shape_ok, rr_base = -1000;
Window pm_root;
GC gc;
static Theme cur_theme;
const Theme *T = &cur_theme;
Cursor pm_cursor;
const char *pm_family = "Noto Sans CJK TC";
double pm_size = 13.0;
int pm_dark;
const char *pm_icon_theme = "Yaru";
void (*pm_dispatch)(XEvent *);
int pm_popup_dir;
Rect mons[16];
int nmons = 1, pmon;

static const Theme light = {
	0xb8b8b8, 0xe8e6e8, 0xf6f6f6, 0x3c3c3c, 0xa8a8a8, 0xc9c9c9, 0xf2f2f4, 0x1d1d1f,
	0xeceaee, 0xc7c7cc, 0xf5f5f7, 0x1d1d1f, 0x0a64d8, 0xffffff, 0x8e8e93, 0xd4d4d4,
};
static const Theme darkt = {
	0x101010, 0x38363a, 0x2a2a2c, 0xdcdcdc, 0x7a7a7a, 0x1e1e1e, 0x2b2a2e, 0xf2f2f2,
	0x333236, 0x4a4a4e, 0x2e2d31, 0xf2f2f2, 0x0a64d8, 0xffffff, 0x98989d, 0x555555,
};

void pm_set_theme(int dark, unsigned accent)
{
	pm_dark = dark;
	cur_theme = dark ? darkt : light;
	if (accent)
		cur_theme.sel = accent;
}

/* X 的部分：原生 Wayland 的程式（pm_ui_native）沒有 X 連線時不做，之後要用 X 時再補（pm__init_x） */
void pm__init_x(void)
{
	if (!dpy || gc) return;
	scr = DefaultScreen(dpy);
	pm_root = RootWindow(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);
	gc = XCreateGC(dpy, pm_root, 0, NULL);
	XSetGraphicsExposures(dpy, gc, False);   /* XCopyArea 不要每次回一個 NoExpose：事件迴圈每個事件都重畫的程式會因此空轉吃 CPU */
	int ev, er;
	shape_ok = XShapeQueryExtension(dpy, &ev, &er);
	if (!XRRQueryExtension(dpy, &rr_base, &er))
		rr_base = -1000;
	pm_cursor = XCreateFontCursor(dpy, XC_left_ptr);
}

void pm_init(const char *family, double size, int dark, const char *icon_theme)
{
	pm__init_x();
	pm_family = family;
	pm_size = size;
	pm_dark = dark;
	pm_icon_theme = icon_theme;
	pm_set_theme(dark, 0);
}

void pm_spawn(const char *cmd)
{
	if (!cmd || !*cmd)
		return;
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

int pm_getatoms(Window w, Atom prop, Atom *out, int max)
{
	Atom type; int fmt; unsigned long n = 0, extra;
	unsigned char *p = NULL;
	if (XGetWindowProperty(dpy, w, prop, 0, max, False, XA_ATOM, &type, &fmt, &n, &extra, &p) != Success || !p)
		return 0;
	for (unsigned long i = 0; i < n; i++)
		out[i] = ((Atom *)p)[i];
	XFree(p);
	return (int)n;
}

void pm_send(Window to, Window about, Atom type, long d0, long d1, long d2)
{
	XEvent ev = { .type = ClientMessage };
	ev.xclient.window = about;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	ev.xclient.data.l[2] = d2;
	XSendEvent(dpy, to, False, to == pm_root ? SubstructureRedirectMask | SubstructureNotifyMask : NoEventMask, &ev);
}

/* pomelo-compositor 底下沒有 pomelo-wm 可以收「結束」的訊息（compositor 結束會連登入畫面一起重開），
 * 所以直接請 pomelo-session 收尾。先確認那個 PID 真的是 pomelo-session，免得環境變數是舊的。 */
void pm_logout(void)
{
	const char *s = getenv("POMELO_SESSION_PID");
	int pid = s ? atoi(s) : 0;
	char path[64], comm[32] = "";
	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	FILE *f = pid > 1 ? fopen(path, "r") : NULL;
	if (f) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
	if (!strcmp(comm, "pomelo-session\n") && kill(pid, SIGTERM) == 0) return;
	if (!dpy) {   /* 原生 Wayland 的程式（pm_ui_native）：臨時開 X 連線送給 pomelo-wm */
		if (!(dpy = XOpenDisplay(NULL))) return;
		pm__init_x();
	}
	pm_send(pm_root, pm_root, XInternAtom(dpy, "_POMELO_ACTION", False), 4, 0, 0);
	XFlush(dpy);
}

/* 原生 Wayland 的程式（pm_ui_native）沒有 X 連線：臨時開一條看一次就記住（build() 每次重畫都會問） */
int pm_under_compositor(void)
{
	static int native = -1;
	Display *d = dpy;
	if (!d) {
		if (native >= 0) return native;
		if (!(d = XOpenDisplay(NULL))) return native = 0;
	}
	Atom a = XInternAtom(d, "_POMELO_COMPOSITOR", True), type;
	int fmt;
	unsigned long n = 0, extra;
	unsigned char *p = NULL;
	int r = a != None && XGetWindowProperty(d, DefaultRootWindow(d), a, 0, 1, False, AnyPropertyType,
	                                        &type, &fmt, &n, &extra, &p) == Success && n > 0;
	if (p) XFree(p);
	if (d != dpy) { XCloseDisplay(d); native = r; }
	return r;
}

/* 讀視窗標題（_NET_WM_NAME 優先，UTF-8） */
void pm_title(Window w, char *out, size_t n)
{
	XTextProperty tp;
	char **list; int cnt;
	out[0] = 0;
	if (!XGetTextProperty(dpy, w, &tp, XInternAtom(dpy, "_NET_WM_NAME", False)) || !tp.nitems)
		if (!XGetWMName(dpy, w, &tp) || !tp.nitems)
			return;
	if (tp.encoding == XA_STRING || tp.encoding == XInternAtom(dpy, "UTF8_STRING", False)) {
		snprintf(out, n, "%s", (char *)tp.value);
	} else if (Xutf8TextPropertyToTextList(dpy, &tp, &list, &cnt) >= Success && cnt > 0 && *list) {
		snprintf(out, n, "%s", *list);
		XFreeStringList(list);
	}
	XFree(tp.value);
}

/* 讀 _NET_WM_ICON，挑接近 size 的一張，縮放成 size×size */
cairo_surface_t *pm_net_icon(Window win, int size)
{
	Atom type; int fmt; unsigned long n = 0, extra;
	unsigned char *p = NULL;
	if (XGetWindowProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), 0, LONG_MAX, False,
	                       AnyPropertyType, &type, &fmt, &n, &extra, &p) != Success || !p)
		return NULL;
	unsigned long *d = (unsigned long *)p, best = 0, bestsz = 0, i = 0;
	while (i + 2 < n) {
		unsigned long w = d[i], h = d[i + 1];
		if (!w || !h || w > 1024 || h > 1024 || i + 2 + w * h > n)
			break;
		unsigned long sz = MIN(w, h);
		if (!bestsz || (bestsz < (unsigned)size ? sz > bestsz : (sz >= (unsigned)size && sz < bestsz)))
			{ best = i; bestsz = sz; }
		i += 2 + w * h;
	}
	cairo_surface_t *out = NULL;
	if (bestsz) {
		int w = (int)d[best], h = (int)d[best + 1];
		cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		unsigned char *px = cairo_image_surface_get_data(src);
		int stride = cairo_image_surface_get_stride(src);
		for (int y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(px + y * stride);
			for (int x = 0; x < w; x++) {
				unsigned long v = d[best + 2 + y * w + x];
				unsigned a = (v >> 24) & 255, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
				r = r * a / 255; g = g * a / 255; b = b * a / 255;
				row[x] = (a << 24) | (r << 16) | (g << 8) | b;
			}
		}
		cairo_surface_mark_dirty(src);
		out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
		cairo_t *cr = cairo_create(out);
		cairo_scale(cr, (double)size / w, (double)size / h);
		cairo_set_source_surface(cr, src, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_destroy(src);
	}
	XFree(p);
	return out;
}

long pm_now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* 找出包含 (x,y) 的螢幕；都不包含時取最近的 */
int pm_mon_at(int x, int y)
{
	int best = pmon;
	long bd = LONG_MAX;
	for (int i = 0; i < nmons; i++) {
		Rect *m = &mons[i];
		long dx = x < m->x ? m->x - x : x >= m->x + m->w ? x - (m->x + m->w) + 1 : 0;
		long dy = y < m->y ? m->y - y : y >= m->y + m->h ? y - (m->y + m->h) + 1 : 0;
		if (dx * dx + dy * dy < bd) { bd = dx * dx + dy * dy; best = i; }
	}
	return best;
}

int pm_pointer_mon(void)
{
	Window r, ch; int rx = 0, ry = 0, wx, wy; unsigned int m;
	XQueryPointer(dpy, pm_root, &r, &ch, &rx, &ry, &wx, &wy, &m);
	return pm_mon_at(rx, ry);
}

void pm_update_monitors(void)
{
	int n = 0;
	XRRMonitorInfo *m = rr_base >= 0 ? XRRGetMonitors(dpy, pm_root, True, &n) : NULL;
	nmons = 0;
	pmon = 0;
	for (int i = 0; i < n && nmons < (int)LEN(mons); i++) {
		mons[nmons] = (Rect){ m[i].x, m[i].y, m[i].width, m[i].height };
		if (m[i].primary) pmon = nmons;
		nmons++;
	}
	if (m) XRRFreeMonitors(m);
	if (!nmons) {
		mons[0] = (Rect){ 0, 0, sw, sh };
		nmons = 1;
	}
}

void pm_rgb(cairo_t *cr, unsigned int c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

void pm_rgba(cairo_t *cr, unsigned int c, double a)
{
	cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0, a);
}

void pm_rrect(cairo_t *cr, double x, double y, double w, double h, double rt, double rb)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - rt, y + rt, rt, -M_PI / 2, 0);
	cairo_arc(cr, x + w - rb, y + h - rb, rb, 0, M_PI / 2);
	cairo_arc(cr, x + rb, y + h - rb, rb, M_PI / 2, M_PI);
	cairo_arc(cr, x + rt, y + rt, rt, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void pm_font(cairo_t *cr, double size, int bold)
{
	cairo_select_font_face(cr, pm_family, CAIRO_FONT_SLANT_NORMAL,
	                       bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

double pm_textw(cairo_t *cr, const char *s)
{
	cairo_text_extents_t e;
	cairo_text_extents(cr, s, &e);
	return e.x_advance;
}

/* 文字太長時以「…」截斷（以 UTF-8 字元為單位） */
void pm_ellipsize(cairo_t *cr, const char *s, double maxw, char *out, size_t n)
{
	snprintf(out, n, "%s", s);
	if (pm_textw(cr, out) <= maxw)
		return;
	size_t len = strlen(out);
	while (len > 0) {
		do len--; while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80);
		if (len + 4 > n) continue;
		memcpy(out + len, "…", 4);
		if (pm_textw(cr, out) <= maxw || len == 0)
			return;
	}
}

/* 取出 s[*i] 開始的一個 UTF-8 字元（不合法的位元組當成一個字元），*i 移到下一個字元 */
static unsigned utf8_next(const char *s, size_t len, size_t *i)
{
	unsigned char c = (unsigned char)s[*i];
	int n = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
	unsigned cp = n == 1 ? c : n == 2 ? c & 0x1f : n == 3 ? c & 0x0f : c & 0x07;
	size_t k = 1;
	for (; k < (size_t)n && *i + k < len && ((unsigned char)s[*i + k] & 0xc0) == 0x80; k++)
		cp = cp << 6 | ((unsigned char)s[*i + k] & 0x3f);
	*i += k;
	return cp;
}

/* 中日文（漢字、假名、全形符號）：字與字之間都可以斷行；韓文有空白，不算在內 */
static int cjk(unsigned c) { return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef); }
/* 不可以放在行首的標點（，。這類）、不可以放在行尾的（「（這類） */
static int no_start(unsigned c) { return c && strchr("!),.:;?]}", c < 0x80 ? (int)c : 1) ? 1 : c >= 0x80 && (c == 0x3001 || c == 0x3002 || c == 0xff0c || c == 0xff0e || c == 0xff1a || c == 0xff1b || c == 0xff01 || c == 0xff1f || c == 0xff09 || c == 0x300d || c == 0x300f || c == 0x3011 || c == 0x3009 || c == 0x300b || c == 0x2026 || c == 0x30fc); }
static int no_end(unsigned c) { return c == 0xff08 || c == 0x300c || c == 0x300e || c == 0x3010 || c == 0x3008 || c == 0x300a; }
/* 泰文寫在子音上下的母音、聲調符號（不能跟前面的子音拆開）、寫在子音前面的母音（不能跟後面的拆開） */
static int thai_mark(unsigned c) { return c == 0x0e31 || (c >= 0x0e34 && c <= 0x0e3a) || (c >= 0x0e47 && c <= 0x0e4e); }
static int thai_lead(unsigned c) { return c >= 0x0e40 && c <= 0x0e44; }
static int thai(unsigned c) { return c >= 0x0e01 && c <= 0x0e7f; }

/* a、b 兩個字中間可以換行嗎：2＝好的斷點（空白後面、中日文字之間），1＝不得已的斷點（泰文詞的中間：
 * 泰文詞跟詞之間沒有空白、要字典才知道詞在哪裡斷，一行裡有空白就在空白斷，沒有才拆詞），0＝不行 */
static int can_break(unsigned a, unsigned b)
{
	if (b == ' ' || b == '\t') return 0;   /* 空白跟著前面那行，從空白後面斷 */
	if (a == ' ' || a == '\t' || a == '-' || a == '/') return no_start(b) ? 0 : 2;
	if (no_start(b) || no_end(a)) return 0;
	if (cjk(a) || cjk(b)) return 2;
	if (thai(a) && thai(b)) return !thai_mark(b) && !thai_lead(a);
	return 0;
}

size_t pm_text_fit(cairo_t *cr, const char *s, size_t len, double maxw)
{
	char buf[1024];
	size_t i = 0, fit = 0, brk = 0, weak = 0;
	unsigned prev = 0;
	while (i < len) {
		size_t start = i;
		unsigned c = utf8_next(s, len, &i);
		int k = start ? can_break(prev, c) : 0;
		if (k == 2) brk = start;
		else if (k == 1) weak = start;
		snprintf(buf, sizeof buf, "%.*s", (int)MIN(i, sizeof buf - 1), s);
		/* 行尾的空白不算寬度（不然剛好放得下的一行會因為後面的空白被擠到下一行） */
		if (start && c != ' ' && pm_textw(cr, buf) > maxw) break;
		fit = i;
		prev = c;
		if (i >= sizeof buf - 1) break;
	}
	if (fit >= len) return len;
	if (brk) return brk;
	if (weak) return weak;
	return fit ? fit : (len ? 1 : 0);
}

void pm_text(cairo_t *cr, double x, double yc, const char *s)
{
	cairo_font_extents_t fe;
	cairo_font_extents(cr, &fe);
	cairo_move_to(cr, x, yc + (fe.ascent - fe.descent) / 2 - 1);
	cairo_show_text(cr, s);
}

/* 圓角視窗：Shape 擴充裁圓角在 Xorg 上沒問題，但 pomelo-compositor 內嵌的 XWayland 只拿 Shape
 * 判斷點擊、不會照它裁掉畫面，圓角外面就露出黑色小角（選單、Dock、通知都會）。有合成器的時候
 * （XWayland 或 picom，都會佔用 _NET_WM_CM_Sn）改用 32 位元 ARGB 視窗，每次畫完由 pm_paint_end
 * 把圓角外面清成透明。Shape 照樣設定：點擊才會穿過角落，沒有合成器時也維持原本的樣子。 */
typedef struct { int w, h, rt, rb; } RoundInfo;
static XContext round_ctx;
static Visual *argb_vis;
static Colormap argb_cmap;
static GC argb_gc;

static int argb_ready(void)
{
	static int checked;
	if (checked) return argb_vis != NULL;
	checked = 1;
	char name[32];
	snprintf(name, sizeof name, "_NET_WM_CM_S%d", scr);
	XVisualInfo vi;
	if (XGetSelectionOwner(dpy, XInternAtom(dpy, name, False)) == None || !XMatchVisualInfo(dpy, scr, 32, TrueColor, &vi))
		return 0;
	argb_vis = vi.visual;
	argb_cmap = XCreateColormap(dpy, pm_root, argb_vis, AllocNone);
	Pixmap tmp = XCreatePixmap(dpy, pm_root, 1, 1, 32);
	argb_gc = XCreateGC(dpy, tmp, 0, NULL);
	XSetGraphicsExposures(dpy, argb_gc, False);
	XFreePixmap(dpy, tmp);
	round_ctx = XUniqueContext();
	return 1;
}

static RoundInfo *round_info(Window w)
{
	XPointer p;
	if (!argb_vis || XFindContext(dpy, w, round_ctx, &p)) return NULL;
	return (RoundInfo *)p;
}

Window pm_round_window(int x, int y, int w, int h, unsigned long mask, XSetWindowAttributes *wa)
{
	if (!argb_ready())
		return XCreateWindow(dpy, pm_root, x, y, w, h, 0, CopyFromParent, InputOutput, CopyFromParent, mask, wa);
	wa->colormap = argb_cmap;
	wa->border_pixel = 0;
	wa->background_pixel = 0;   /* 畫好之前整個透明，不會先閃一下方角 */
	Window win = XCreateWindow(dpy, pm_root, x, y, w, h, 0, 32, InputOutput, argb_vis,
	                           mask | CWColormap | CWBorderPixel | CWBackPixel, wa);
	XSaveContext(dpy, win, round_ctx, (XPointer)calloc(1, sizeof(RoundInfo)));
	return win;
}

void pm_destroy_window(Window w)
{
	RoundInfo *ri = round_info(w);
	if (ri) { XDeleteContext(dpy, w, round_ctx); free(ri); }
	XDestroyWindow(dpy, w);
}

cairo_t *pm_paint_begin(Paint *p, Window w, int width, int height)
{
	int argb = round_info(w) != NULL;
	p->w = w; p->width = MAX(width, 1); p->height = MAX(height, 1);
	p->pm = XCreatePixmap(dpy, w, p->width, p->height, argb ? 32 : DefaultDepth(dpy, scr));
	p->s = cairo_xlib_surface_create(dpy, p->pm, argb ? argb_vis : DefaultVisual(dpy, scr), p->width, p->height);
	return cairo_create(p->s);
}

void pm_paint_end(Paint *p, cairo_t *cr, int dx, int dy)
{
	RoundInfo *ri = round_info(p->w);
	if (ri && (ri->rt > 0 || ri->rb > 0)) {
		/* 圓角外面清成透明（座標換回整個視窗：pixmap 的 (0,0) 會貼到視窗的 (dx,dy)） */
		cairo_save(cr);
		cairo_identity_matrix(cr);
		cairo_reset_clip(cr);
		cairo_new_path(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
		cairo_rectangle(cr, -dx, -dy, ri->w, ri->h);
		pm_rrect(cr, -dx, -dy, ri->w, ri->h, ri->rt, ri->rb);
		cairo_fill(cr);
		cairo_restore(cr);
	}
	cairo_destroy(cr);
	cairo_surface_flush(p->s);
	XCopyArea(dpy, p->pm, p->w, ri ? argb_gc : gc, 0, 0, p->width, p->height, dx, dy);
	cairo_surface_destroy(p->s);
	XFreePixmap(dpy, p->pm);
}

/* 以 Shape 擴充把視窗裁成圓角。pm_round_window 建的 ARGB 視窗另外記下來，畫的時候把角落清成
 * 透明，Shape 只設「點擊」的形狀（ShapeInput）：實機（GPU）的 XWayland 會把外觀形狀
 * （ShapeBounding）外面畫成不透明的黑色，角落反而變黑。 */
void pm_shape_round(Window w, int width, int height, int rt, int rb)
{
	RoundInfo *ri = round_info(w);
	if (ri) { ri->w = width; ri->h = height; ri->rt = rt; ri->rb = rb; }
	if (!shape_ok)
		return;
	int kind = ri ? ShapeInput : ShapeBounding;
	if (rt <= 0 && rb <= 0) {
		XShapeCombineMask(dpy, w, kind, 0, 0, None, ShapeSet);
		return;
	}
	Pixmap m = XCreatePixmap(dpy, w, width, height, 1);
	GC g = XCreateGC(dpy, m, 0, NULL);
	XSetForeground(dpy, g, 0);
	XFillRectangle(dpy, m, g, 0, 0, width, height);
	XSetForeground(dpy, g, 1);
	XFillRectangle(dpy, m, g, 0, rt, width, height - rt - rb);
	XFillRectangle(dpy, m, g, rt, 0, width - 2 * rt, rt);
	XFillRectangle(dpy, m, g, rb, height - rb, width - 2 * rb, rb);
	if (rt > 0) {
		XFillArc(dpy, m, g, 0, 0, 2 * rt, 2 * rt, 0, 360 * 64);
		XFillArc(dpy, m, g, width - 2 * rt - 1, 0, 2 * rt, 2 * rt, 0, 360 * 64);
	}
	if (rb > 0) {
		XFillArc(dpy, m, g, 0, height - 2 * rb - 1, 2 * rb, 2 * rb, 0, 360 * 64);
		XFillArc(dpy, m, g, width - 2 * rb - 1, height - 2 * rb - 1, 2 * rb, 2 * rb, 0, 360 * 64);
	}
	XShapeCombineMask(dpy, w, kind, 0, 0, m, ShapeSet);
	XFreeGC(dpy, g);
	XFreePixmap(dpy, m);
}

/* 從圖示主題中找圖示檔（PNG 優先；以 SVG=1 編譯時也支援 SVG） */
int pm_icon_find(const char *name, const char *ext, char *out, size_t n, int want)
{
	static const char *small[] = { "32x32", "24x24", "48x48", "32x32@2x", "24x24@2x", "64x64", "128x128",
	                               "256x256", "scalable", "16x16@2x", "16x16", "symbolic" };
	static const char *big[] = { "128x128", "96x96", "64x64", "256x256", "48x48@2x", "48x48",
	                               "512x512", "scalable", "32x32@2x", "32x32", "24x24", "16x16", "symbolic" };
	const char *themes[] = { pm_icon_theme, "hicolor", "Adwaita", "Yaru", "HighContrast" };
	char bases[4][PATH_MAX];
	const char *home = getenv("HOME");
	snprintf(bases[0], PATH_MAX, "%s/.local/share/icons", home ? home : "/nonexistent");
	snprintf(bases[1], PATH_MAX, "/usr/local/share/icons");
	snprintf(bases[2], PATH_MAX, "/usr/share/icons");
	snprintf(bases[3], PATH_MAX, "/var/lib/flatpak/exports/share/icons");
	struct stat st;
	for (unsigned b = 0; b < LEN(bases); b++)
		for (unsigned t = 0; t < LEN(themes); t++) {
			char dir[PATH_MAX];
			snprintf(dir, sizeof dir, "%s/%s", bases[b], themes[t]);
			if (stat(dir, &st) || !S_ISDIR(st.st_mode))
				continue;
			static const char *ctx[] = { "apps", "places", "mimetypes", "devices", "status", "panel", "actions", "legacy" };
			const char **sizes = want <= 32 ? small : big;
			unsigned nsz = want <= 32 ? LEN(small) : LEN(big);
			for (unsigned z = 0; z < nsz; z++)
				for (unsigned c = 0; c < LEN(ctx); c++) {
					snprintf(out, n, "%s/%s/%s/%s.%s", dir, sizes[z], ctx[c], name, ext);
					if (!access(out, R_OK))
						return 1;
				}
		}
	snprintf(out, n, "/usr/share/pixmaps/%s.%s", name, ext);
	return !access(out, R_OK);
}

cairo_surface_t *pm_icon_scale(cairo_surface_t *src, int w, int h, int size)
{
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(dst);
	double k = MIN((double)size / w, (double)size / h);
	cairo_translate(cr, (size - w * k) / 2, (size - h * k) / 2);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return dst;
}

cairo_surface_t *pm_icon_file(const char *path, int size)
{
	size_t l = strlen(path);
	if (l > 4 && !strcasecmp(path + l - 4, ".png")) {
		cairo_surface_t *png = cairo_image_surface_create_from_png(path), *r = NULL;
		if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS)
			r = pm_icon_scale(png, cairo_image_surface_get_width(png), cairo_image_surface_get_height(png), size);
		cairo_surface_destroy(png);
		return r;
	}
	if (l > 4 && !strcasecmp(path + l - 4, ".svg")) {
		/* nanosvg 畫成點陣圖（不支援文字與濾鏡，圖示用不到） */
		NSVGimage *im = nsvgParseFromFile(path, "px", 96);
		if (!im) return NULL;
		cairo_surface_t *r = NULL;
		if (im->width > 0 && im->height > 0 && size > 0 && size <= 1024) {
			NSVGrasterizer *ras = nsvgCreateRasterizer();
			unsigned char *px = ras ? malloc((size_t)size * size * 4) : NULL;
			if (px) {
				float k = (float)size / (im->width > im->height ? im->width : im->height);
				memset(px, 0, (size_t)size * size * 4);
				nsvgRasterize(ras, im, (size - im->width * k) / 2, (size - im->height * k) / 2, k, px, size, size, size * 4);
				r = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
				unsigned char *d = cairo_image_surface_get_data(r);
				int stride = cairo_image_surface_get_stride(r);
				for (int y = 0; y < size; y++)
					for (int x = 0; x < size; x++) {   /* RGBA（沒預乘）→ cairo 的 ARGB（預乘） */
						const unsigned char *q = px + ((size_t)y * size + x) * 4;
						unsigned a = q[3];
						((uint32_t *)(d + y * stride))[x] = (a << 24) | ((q[0] * a / 255) << 16) | ((q[1] * a / 255) << 8) | (q[2] * a / 255);
					}
				cairo_surface_mark_dirty(r);
			}
			free(px);
			nsvgDeleteRasterizer(ras);
		}
		nsvgDelete(im);
		return r;
	}
	return NULL;
}

/* 依圖示名稱（或絕對路徑）載入並縮放成 size×size */
cairo_surface_t *pm_icon_load(const char *name, int size)
{
	char path[PATH_MAX], base[256];
	if (!name || !*name)
		return NULL;
	if (name[0] == '/')
		return access(name, R_OK) ? NULL : pm_icon_file(name, size);
	snprintf(base, sizeof base, "%s", name);
	char *dot = strrchr(base, '.');
	if (dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".svg") || !strcasecmp(dot, ".xpm")))
		*dot = 0;
	if (pm_icon_find(base, "png", path, sizeof path, size))
		return pm_icon_file(path, size);
	if (pm_icon_find(base, "svg", path, sizeof path, size))
		return pm_icon_file(path, size);
	return NULL;
}

/* 在拖曳 / 選單等「模態」迴圈中，把無關事件交給一般處理函式 */
int pm_modal_next(XEvent *ev)
{
	for (;;) {
		XNextEvent(dpy, ev);
		if (XFilterEvent(ev, None))
			continue;
		if (ev->type == MotionNotify || ev->type == ButtonPress || ev->type == ButtonRelease ||
		    ev->type == KeyPress || ev->type == KeyRelease)
			return 1;
		if (ev->type == EnterNotify || ev->type == LeaveNotify)
			continue;
		if (pm_dispatch) pm_dispatch(ev);
		return 0;
	}
}

void pm_menu_off(const Arg *a) { (void)a; }

/* 選單項目的文字：「名稱<Tab>快捷鍵」，快捷鍵靠右、顏色淡一點；開頭是 \1 = 打勾、\2 = 可以打勾但沒勾 */
static void menu_split(const char *label, char *name, size_t n, const char **key)
{
	if (*label == 1 || *label == 2) label++;
	const char *tab = strchr(label, '\t');
	if (tab) {
		char raw[256];
		snprintf(raw, sizeof raw, "%.*s", (int)(tab - label), label);
		snprintf(name, n, "%s", pm_tr(raw));
		*key = tab + 1;
	} else {
		snprintf(name, n, "%s", pm_tr(label));
		*key = NULL;
	}
}

static int menu_usable(const MenuItem *m) { return m->label && m->fn != pm_menu_off; }

/* ---- 選單的版面與畫法（X 的 pm_popup 與 Wayland 的 pmwl_menu 共用） ---- */

void pm__menu_measure(const MenuItem *m, int n, PmMenuGeo *g)
{
	g->ih = 24; g->sh = 11; g->pad = 5;
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *mc = cairo_create(tmp);
	pm_font(mc, pm_size, 0);
	int W = 180, H = 2 * g->pad, keyw = 0, checks = 0;
	for (int i = 0; i < n; i++) {
		H += m[i].label ? g->ih : g->sh;
		if (!m[i].label) continue;
		if (m[i].label[0] == 1 || m[i].label[0] == 2) checks = 1;
		char name[256];
		const char *key;
		menu_split(m[i].label, name, sizeof name, &key);
		W = MAX(W, (int)pm_textw(mc, name) + 48);
		if (key) keyw = MAX(keyw, (int)pm_textw(mc, key) + 28);
	}
	g->W = W + keyw + (checks ? 16 : 0);
	g->H = H;
	g->tx = checks ? 32 : 16;
	cairo_destroy(mc);
	cairo_surface_destroy(tmp);
}

void pm__menu_draw(cairo_t *cr, const MenuItem *m, int n, const PmMenuGeo *g, int hot)
{
	int W = g->W, H = g->H, ih = g->ih;
	pm_rgb(cr, T->menu);
	cairo_paint(cr);
	pm_rrect(cr, 0.5, 0.5, W - 1, H - 1, 7, 7);
	pm_rgba(cr, 0x000000, 0.18);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	pm_font(cr, pm_size, 0);
	int yy = g->pad;
	for (int i = 0; i < n; i++) {
		if (!m[i].label) {
			pm_rgba(cr, 0x000000, pm_dark ? 0.5 : 0.12);
			cairo_rectangle(cr, 10, yy + g->sh / 2, W - 20, 1);
			cairo_fill(cr);
			yy += g->sh;
			continue;
		}
		if (i == hot) {
			pm_rrect(cr, 5, yy, W - 10, ih, 4, 4);
			pm_rgb(cr, T->sel);
			cairo_fill(cr);
		}
		char name[256];
		const char *key;
		menu_split(m[i].label, name, sizeof name, &key);
		if (!menu_usable(&m[i])) pm_rgba(cr, T->menu_text, 0.35);
		else pm_rgb(cr, i == hot ? T->sel_text : T->menu_text);
		if (m[i].label[0] == 1) {   /* 打勾 */
			cairo_new_path(cr);
			cairo_set_line_width(cr, 1.8);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
			double cy = yy + ih / 2.0;
			cairo_move_to(cr, 13, cy); cairo_line_to(cr, 17, cy + 4); cairo_line_to(cr, 24, cy - 4.5);
			cairo_stroke(cr);
		}
		pm_text(cr, g->tx, yy + ih / 2.0, name);
		if (key) {
			if (!menu_usable(&m[i])) pm_rgba(cr, T->menu_text, 0.25);
			else if (i == hot) pm_rgba(cr, T->sel_text, 0.8);
			else pm_rgba(cr, T->menu_text, 0.5);
			pm_text(cr, W - 16 - pm_textw(cr, key), yy + ih / 2.0, key);
		}
		yy += ih;
	}
}

int pm__menu_hit(const MenuItem *m, int n, const PmMenuGeo *g, int px, int py)
{
	if (px < 0 || px >= g->W || py < g->pad || py >= g->H - g->pad) return -1;
	int yy = g->pad;
	for (int i = 0; i < n; i++) {
		int hh = m[i].label ? g->ih : g->sh;
		if (py >= yy && py < yy + hh) return menu_usable(&m[i]) ? i : -1;
		yy += hh;
	}
	return -1;
}

int pm__menu_step(const MenuItem *m, int n, int hot, int dir)
{
	int i = hot < 0 ? (dir > 0 ? -1 : n) : hot;
	for (int k = 0; k < n; k++) {
		i = (i + dir + n) % n;
		if (i < 0) i = n - 1;
		if (menu_usable(&m[i])) break;
	}
	return i;
}

void pm_popup(const MenuItem *m, int n, int x, int y)
{
	PmMenuGeo g;
	pm__menu_measure(m, n, &g);
	int W = g.W, H = g.H;
	x = MAX(0, MIN(x, sw - W));
	if (y + H > sh) y = MAX(0, sh - H);

	XSetWindowAttributes swa = { .override_redirect = True, .background_pixel = T->menu,
	                             .event_mask = ExposureMask };
	Window w = pm_round_window(x, y, W, H, CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	pm_shape_round(w, W, H, 7, 7);
	XMapRaised(dpy, w);
	if (XGrabPointer(dpy, pm_root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync, None, pm_cursor, CurrentTime) != GrabSuccess) {
		pm_destroy_window(w);
		return;
	}
	XGrabKeyboard(dpy, pm_root, True, GrabModeAsync, GrabModeAsync, CurrentTime);

	int hot = -1, chosen = -1, done = 0, redraw = 1;
	/* 打開選單的那一下還沒放開：選單往上擠進螢幕時游標會剛好在某一項上，放開不能馬上算選了它
	 * （跟原生 Wayland 的選單一樣：在選單上按過，或按住超過 0.3 秒再放開才算） */
	long opened = pm_now_ms();
	int pressed_in = 0;
	XEvent ev;
	while (!done) {
		if (redraw) {
			Paint p;
			cairo_t *cr = pm_paint_begin(&p, w, W, H);
			pm__menu_draw(cr, m, n, &g, hot);
			pm_paint_end(&p, cr, 0, 0);
			redraw = 0;
		}
		if (!pm_modal_next(&ev)) {
			if (ev.type == Expose && ev.xexpose.window == w)
				redraw = 1;
			continue;
		}
		int px = 0, py = 0;
		if (ev.type == MotionNotify || ev.type == ButtonPress || ev.type == ButtonRelease) {
			px = (ev.type == MotionNotify ? ev.xmotion.x_root : ev.xbutton.x_root) - x;
			py = (ev.type == MotionNotify ? ev.xmotion.y_root : ev.xbutton.y_root) - y;
		}
		int idx = pm__menu_hit(m, n, &g, px, py);
		switch (ev.type) {
		case MotionNotify:
			if (idx != hot) { hot = idx; redraw = 1; }
			break;
		case ButtonRelease:
			if (idx >= 0 && (pressed_in || pm_now_ms() - opened > 300)) { chosen = idx; done = 1; }
			break;
		case ButtonPress:
			if (px < 0 || px >= W || py < 0 || py >= H) done = 1;
			else pressed_in = 1;
			break;
		case KeyPress: {
			KeySym ks = XLookupKeysym(&ev.xkey, 0);
			if (ks == XK_Escape) done = 1;
			else if (ks == XK_Down || ks == XK_Up) {
				hot = pm__menu_step(m, n, hot, ks == XK_Down ? 1 : -1); redraw = 1;
			} else if ((ks == XK_Return || ks == XK_KP_Enter) && hot >= 0) {
				chosen = hot; done = 1;
			} else if (ks == XK_Left || ks == XK_Right) {
				/* 選單列：←／→ 換到旁邊的選單（呼叫的程式看 pm_popup_dir） */
				pm_popup_dir = ks == XK_Left ? -1 : 1;
				done = 1;
			}
			break;
		}
		}
	}
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	pm_destroy_window(w);
	XSync(dpy, False);
	if (chosen >= 0 && m[chosen].fn)
		m[chosen].fn(&m[chosen].arg);
}


/* ------------------------------------------------------------------ */
/* 輸入法（XIM）連線：伺服器消失時不要再用，回來時重新連上                  */
/* ------------------------------------------------------------------ */

typedef struct { Display *d; XIM *im; XIC *ic; Window w; } PmImReg;
static unsigned im_generation;

unsigned pm_xim_generation(void) { return im_generation; }

static void im_destroyed(XIM im, XPointer client, XPointer call);
static void im_instantiate(Display *d, XPointer client, XPointer call);

static void im_setup(PmImReg *r)
{
	XIMCallback cb = { (XPointer)r, im_destroyed };
	XSetIMValues(*r->im, XNDestroyCallback, &cb, NULL);
	if (!r->ic) return;
	*r->ic = XCreateIC(*r->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, r->w, XNFocusWindow, r->w, NULL);
	if (!*r->ic) return;
	long fe = 0;
	XGetICValues(*r->ic, XNFilterEvents, &fe, NULL);
	XWindowAttributes wa;
	if (fe && XGetWindowAttributes(r->d, r->w, &wa)) XSelectInput(r->d, r->w, wa.your_event_mask | fe);
	Window f;
	int rv;
	XGetInputFocus(r->d, &f, &rv);
	if (f == r->w) XSetICFocus(*r->ic);
}

static void im_instantiate(Display *d, XPointer client, XPointer call)
{
	(void)call;
	PmImReg *r = (PmImReg *)client;
	if (*r->im) return;
	if (!(*r->im = XOpenIM(d, NULL, NULL, NULL))) return;
	XUnregisterIMInstantiateCallback(d, NULL, NULL, NULL, im_instantiate, client);
	im_setup(r);
}

/* 伺服器已經不在了：IC、IM 都不能再碰（XDestroyIC 也不行），等它回來 */
static void im_destroyed(XIM im, XPointer client, XPointer call)
{
	(void)im; (void)call;
	PmImReg *r = (PmImReg *)client;
	*r->im = NULL;
	if (r->ic) *r->ic = NULL;
	im_generation++;
	XRegisterIMInstantiateCallback(r->d, NULL, NULL, NULL, im_instantiate, client);
}

static void im_attach(Display *d, XIM *im, XIC *ic, Window w)
{
	PmImReg *r = calloc(1, sizeof *r);   /* 程式結束前都用得到，不釋放 */
	if (!r) return;
	r->d = d; r->im = im; r->ic = ic; r->w = w;
	XSetLocaleModifiers("");
	if (!*im) *im = XOpenIM(d, NULL, NULL, NULL);
	if (*im) im_setup(r);
	else XRegisterIMInstantiateCallback(d, NULL, NULL, NULL, im_instantiate, (XPointer)r);
}

void pm_xim_attach(Display *d, XIM *im, XIC *ic, Window w) { im_attach(d, im, ic, w); }
void pm_xim_watch(Display *d, XIM *im) { im_attach(d, im, NULL, None); }

/* ------------------------------------------------------------------ */
/* 狀態列右邊的縮放控制：−  ━━●━━  ＋  100%（文書、試算表、簡報共用）     */
/* ------------------------------------------------------------------ */

/* 滑桿位置 0..1 ↔ 縮放：中間是 100%，左半 lo～100%、右半 100%～hi */
static double zoom_to_t(const PmZoomBar *zb, double z)
{
	return z <= 1.0 ? (z - zb->lo) / (1.0 - zb->lo) * 0.5 : 0.5 + (z - 1.0) / (zb->hi - 1.0) * 0.5;
}

static double t_to_zoom(const PmZoomBar *zb, double t)
{
	return t <= 0.5 ? zb->lo + t * 2 * (1.0 - zb->lo) : 1.0 + (t - 0.5) * 2 * (zb->hi - 1.0);
}

double pm_zoom_width(cairo_t *cr) { (void)cr; return 22 + 110 + 22 + 44; }

void pm_zoom_draw(cairo_t *cr, PmZoomBar *zb, double x, double y, double h, double zoom)
{
	const double slider_w = 110, pct_w = 44;
	double cy = y + h / 2.0;
	pm_font(cr, 15, 0);
	pm_rgb(cr, T->bar_text);
	pm_text(cr, x + 6, cy, "−");
	zb->minus = (Rect){ (int)x, (int)y, 22, (int)h };
	x += 22;
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, x, cy - 1, slider_w, 2);
	cairo_fill(cr);
	cairo_rectangle(cr, x + slider_w / 2 - 0.5, cy - 4, 1, 8);   /* 100% 的刻度 */
	cairo_fill(cr);
	double t = CLAMP(zoom_to_t(zb, zoom), 0, 1);
	cairo_arc(cr, x + t * slider_w, cy, 5.5, 0, 2 * M_PI);
	pm_rgb(cr, T->sel);
	cairo_fill(cr);
	zb->slider = (Rect){ (int)x - 6, (int)y, (int)slider_w + 12, (int)h };
	x += slider_w;
	pm_font(cr, 15, 0);
	pm_rgb(cr, T->bar_text);
	pm_text(cr, x + 5, cy, "+");
	zb->plus = (Rect){ (int)x, (int)y, 22, (int)h };
	x += 22;
	char buf[16];
	snprintf(buf, sizeof buf, "%d%%", (int)(zoom * 100.0 + 0.5));
	pm_font(cr, 12, 0);
	pm_text(cr, x + pct_w - 4 - pm_textw(cr, buf), cy, buf);
	zb->pct = (Rect){ (int)x, (int)y, (int)pct_w, (int)h };
}

static int zin(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

double pm_zoom_slider_at(const PmZoomBar *zb, int x)
{
	double t = (x - (zb->slider.x + 6)) / (double)(zb->slider.w - 12);
	double z = t_to_zoom(zb, CLAMP(t, 0, 1));
	return fabs(z - 1.0) < 0.04 ? 1.0 : z;   /* 靠近中間就吸到 100% */
}

int pm_zoom_click(PmZoomBar *zb, int x, int y, double zoom, double *out)
{
	int pct = (int)(zoom * 100.0 + 0.5);
	if (zin(zb->minus, x, y)) { *out = CLAMP((pct - 1) / 10 * 10 / 100.0, zb->lo, zb->hi); return PM_ZOOM_SET; }
	if (zin(zb->plus, x, y)) { *out = CLAMP((pct / 10 * 10 + 10) / 100.0, zb->lo, zb->hi); return PM_ZOOM_SET; }
	if (zin(zb->slider, x, y)) { *out = pm_zoom_slider_at(zb, x); return PM_ZOOM_DRAG; }
	if (zin(zb->pct, x, y)) return PM_ZOOM_MENU;
	return PM_ZOOM_NONE;
}
