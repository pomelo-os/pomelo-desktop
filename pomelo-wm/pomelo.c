/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pomelo — 輕量、macOS 風格的 X11 視窗管理器
 *
 * 事件驅動：沒有事件時不消耗 CPU。選單列與 Dock 由 pomelo-panel 負責，
 * 兩者透過 EWMH 標準溝通（_NET_ACTIVE_WINDOW、_NET_WM_STRUT_PARTIAL…）。
 * 依賴：Xlib、Xext(Shape)、Xrandr、Xcomposite、cairo。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <ctype.h>
#include <dirent.h>
#include <libintl.h>
#include <locale.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>

#define ICON_SZ 96   /* 視窗圖示快取大小 */
#define CLEANMASK(m) ((m) & ~(numlockmask | LockMask) & \
	(ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask))
#define EDGE_L 1
#define EDGE_R 2
#define EDGE_T 4
#define EDGE_B 8
#define GRIP   6     /* 視窗外圍可拖曳縮放的寬度 */

typedef struct { unsigned int mod; KeySym sym; void (*fn)(const Arg *); Arg arg; } Key;

typedef struct Client Client;
struct Client {
	Window win, frame, grip;
	int x, y, w, h;               /* 外框位置、內容大小 */
	int sx, sy, sw, sh;           /* 縮放 / 全螢幕前的位置 */
	int minw, minh, maxw, maxh;
	int minimized, zoomed, fullscreen, noframe, neverfocus;
	int ignore_unmap, hover, pressed, shp_w, shp_h;
	unsigned long seq;
	Window transient;
	char title[256], cls[64], inst[64];
	cairo_surface_t *icon;
	Client *next;
};

enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMChangeState,
       NetSupported, NetWMName, NetWMState, NetWMStateFS, NetWMStateHidden,
       NetWMStateMaxV, NetWMStateMaxH, NetWMStateSkipTaskbar, NetActiveWindow,
       NetWMWindowType, NetWMTypeDock, NetWMTypeDesktop, NetWMTypeDialog,
       NetWMTypeSplash, NetWMTypeUtility, NetClientList, NetWMCheck, NetWMIcon,
       NetCloseWindow, NetWMMoveResize, NetFrameExtents, NetWorkarea,
       NetWMStrut, NetWMStrutPartial, PomeloAction,
       MotifHints, Utf8String, XRootPmap, ESetRoot, AtomLast };

static const char *atomnames[AtomLast] = {
	"WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_STATE", "WM_TAKE_FOCUS", "WM_CHANGE_STATE",
	"_NET_SUPPORTED", "_NET_WM_NAME", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
	"_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ",
	"_NET_WM_STATE_SKIP_TASKBAR", "_NET_ACTIVE_WINDOW", "_NET_WM_WINDOW_TYPE",
	"_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_DIALOG",
	"_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_UTILITY", "_NET_CLIENT_LIST",
	"_NET_SUPPORTING_WM_CHECK", "_NET_WM_ICON", "_NET_CLOSE_WINDOW", "_NET_WM_MOVERESIZE",
	"_NET_FRAME_EXTENTS", "_NET_WORKAREA", "_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL",
	"_POMELO_ACTION", "_MOTIF_WM_HINTS", "UTF8_STRING",
	"_XROOTPMAP_ID", "ESETROOT_PMAP_ID",
};

/* 函式宣告（config.h 會用到） */
static void spawn(const Arg *a);
static void spotlight(const Arg *a);
static void killclient(const Arg *a);
static void minimize(const Arg *a);
static void zoom(const Arg *a);
static void unzoom(const Arg *a);
static void tile(const Arg *a);
static void togglefs(const Arg *a);
static void cycle(const Arg *a);
static void restart(const Arg *a);
static void quit(const Arg *a);
static void overview(const Arg *a);

#include "config.h"

/* 全域狀態 */
static int running = 1, do_restart, composite_ok;
static Window checkwin;
static Atom atom[AtomLast];
static Cursor cur_normal, cur_move, cur_edge[16];
static unsigned int numlockmask;
static Client *clients, *sel;
static unsigned long seqno;
static char **g_argv;
static Pixmap wallpm;
static Time last_click;
static Window last_click_win;
static XIM xim;

/* 其他程式的面板（_NET_WM_WINDOW_TYPE_DOCK），例如 pomelo-panel */
typedef struct { Window w; long strut[12]; } Dock;
static Dock docks[16];
static int ndocks;

static void handle(XEvent *ev);

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int xerror(Display *d, XErrorEvent *e)
{
	/* 視窗在處理途中消失是常態，忽略這類錯誤 */
	(void)d;
	if (e->error_code == BadWindow || e->error_code == BadMatch ||
	    e->error_code == BadDrawable || e->error_code == BadAccess ||
	    e->error_code == BadValue)
		return 0;
	fprintf(stderr, "pomelo: X error %d (request %d)\n", e->error_code, e->request_code);
	return 0;
}

static int xerror_start(Display *d, XErrorEvent *e)
{
	(void)d; (void)e;
	fputs(_("pomelo: 已有其他視窗管理器在執行\n"), stderr);
	exit(1);
}

static int bw(Client *c) { return (c->noframe || c->fullscreen) ? 0 : border_w; }
static int th(Client *c) { return (c->noframe || c->fullscreen) ? 0 : title_h; }
static int fw(Client *c) { return c->w + 2 * bw(c); }
static int fh(Client *c) { return c->h + th(c) + bw(c); }


static int mon_of(Client *c) { return pm_mon_at(c->x + fw(c) / 2, c->y + fh(c) / 2); }
static Rect workarea(int i);
static void apply_geom(Client *c);

/* 拔掉螢幕後：視窗的標題列在任何一個螢幕上都看不到（至少 60×10），就是被留在已經不存在的地方了 */
static int reachable(Client *c)
{
	for (int i = 0; i < nmons; i++) {
		Rect m = mons[i];
		int x0 = MAX(c->x, m.x), x1 = MIN(c->x + fw(c), m.x + m.w);
		int y0 = MAX(c->y, m.y), y1 = MIN(c->y + MAX(th(c), 20), m.y + m.h);
		if (x1 - x0 >= 60 && y1 - y0 >= 10) return 1;
	}
	return 0;
}

/* 移到主要顯示器（太大就縮到放得下） */
static void rescue(Client *c)
{
	Rect r = workarea(pmon);
	if (fw(c) > r.w) c->w = MAX(100, r.w - 2 * bw(c));
	if (fh(c) > r.h) c->h = MAX(60, r.h - th(c) - bw(c));
	c->x = r.x + (r.w - fw(c)) / 2;
	c->y = r.y + (r.h - fh(c)) / 2;
	apply_geom(c);
}

/* ------------------------------------------------------------------ */
/* 插上、拔掉螢幕：記住每個輸出有沒有接上，真的變了才交給 pomelo-display --hotplug   */
/* （自己或設定程式改解析度、排列也會有 RandR 事件，那些不要管）                   */
/* ------------------------------------------------------------------ */

static struct { RROutput id; int conn; } outc[32];
static int noutc;
static long hotplug_at;       /* 等一下再處理：插拔時事件會連續來好幾個 */
static char hp_names[512];    /* 新接上的輸出，空白分隔 */

static void outputs_check(int init)
{
	XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, pm_root);
	if (!res) return;
	for (int i = 0; i < res->noutput; i++) {
		XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		if (!oi) continue;
		int conn = oi->connection == RR_Connected, k;
		for (k = 0; k < noutc && outc[k].id != res->outputs[i]; k++) ;
		int was = k < noutc ? outc[k].conn : 0;
		if (k == noutc && noutc < (int)LEN(outc)) { outc[k].id = res->outputs[i]; noutc++; }
		if (k < noutc) outc[k].conn = conn;
		if (!init && conn != was) {
			if (conn && strlen(hp_names) + strlen(oi->name) + 2 < sizeof hp_names) {
				strcat(hp_names, oi->name);
				strcat(hp_names, " ");
			}
			hotplug_at = pm_now_ms() + 800;
		}
		XRRFreeOutputInfo(oi);
	}
	XRRFreeScreenResources(res);
}

static int in_path(const char *cmd);

static void hotplug_run(void)
{
	hotplug_at = 0;
	char names[512];
	snprintf(names, sizeof names, "%s", hp_names);
	hp_names[0] = 0;
	if (!in_path("pomelo-display")) return;
	char *argv[24] = { "pomelo-display", "--hotplug" };
	int k = 2;
	char *save = NULL;
	for (char *t = strtok_r(names, " ", &save); t && k < 23; t = strtok_r(NULL, " ", &save)) argv[k++] = t;
	argv[k] = NULL;
	if (fork() == 0) {
		close(ConnectionNumber(dpy));
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp(argv[0], argv);
		_exit(127);
	}
}

/* 可放視窗的區域：扣掉面板(Dock 類型視窗)用 _NET_WM_STRUT_PARTIAL 保留的空間 */
static Rect workarea(int i)
{
	Rect m = mons[i];
	int x1 = m.x, y1 = m.y, x2 = m.x + m.w, y2 = m.y + m.h;
	for (int d = 0; d < ndocks; d++) {
		long *st = docks[d].strut;
		/* left, right, top, bottom, left_start_y, left_end_y, right_start_y, right_end_y,
		 * top_start_x, top_end_x, bottom_start_x, bottom_end_x */
		if (st[2] > 0 && st[8] < m.x + m.w && st[9] >= m.x && st[2] > y1 - 0)
			y1 = MAX(y1, (int)st[2]);
		if (st[3] > 0 && st[10] < m.x + m.w && st[11] >= m.x)
			y2 = MIN(y2, sh - (int)st[3]);
		if (st[0] > 0 && st[4] < m.y + m.h && st[5] >= m.y)
			x1 = MAX(x1, (int)st[0]);
		if (st[1] > 0 && st[6] < m.y + m.h && st[7] >= m.y)
			x2 = MIN(x2, sw - (int)st[1]);
	}
	if (x2 - x1 < 100 || y2 - y1 < 100)
		return m;
	return (Rect){ x1, y1, x2 - x1, y2 - y1 };
}

static Client *find(Window w)
{
	for (Client *c = clients; c; c = c->next)
		if (c->win == w || c->frame == w || c->grip == w)
			return c;
	return NULL;
}

static int alive(Client *c)
{
	for (Client *i = clients; i; i = i->next)
		if (i == c)
			return 1;
	return 0;
}

static void spawn(const Arg *a)
{
	const char *cmd = (a && a->s) ? a->s : terminal;
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static int has_proto(Client *c, Atom proto)
{
	Atom *protos; int n, found = 0;
	if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
		while (!found && n--)
			found = protos[n] == proto;
		XFree(protos);
	}
	return found;
}

static void send_msg(Window w, Atom type, long d0, long d1)
{
	XEvent ev = { .type = ClientMessage };
	ev.xclient.window = w;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	XSendEvent(dpy, w, False, NoEventMask, &ev);
}

/* ------------------------------------------------------------------ */
/* 客戶端屬性                                                          */
/* ------------------------------------------------------------------ */

static void update_title(Client *c) { pm_title(c->win, c->title, sizeof c->title); }

static void update_hints(Client *c)
{
	XSizeHints h; long supplied;
	c->minw = 120; c->minh = 40; c->maxw = c->maxh = 0;
	if (XGetWMNormalHints(dpy, c->win, &h, &supplied)) {
		if (h.flags & PMinSize) { c->minw = MAX(h.min_width, 20); c->minh = MAX(h.min_height, 20); }
		if (h.flags & PMaxSize) { c->maxw = h.max_width; c->maxh = h.max_height; }
	}
	XWMHints *wh = XGetWMHints(dpy, c->win);
	c->neverfocus = 0;
	if (wh) {
		if (wh->flags & InputHint)
			c->neverfocus = !wh->input;
		XFree(wh);
	}
}

static void update_icon(Client *c)
{
	if (c->icon) cairo_surface_destroy(c->icon);
	c->icon = pm_net_icon(c->win, ICON_SZ);
	const char *names[] = { c->inst, c->cls };
	for (unsigned i = 0; i < LEN(names) && !c->icon; i++) {
		char lc[64];
		snprintf(lc, sizeof lc, "%s", names[i]);
		for (char *q = lc; *q; q++) *q = (char)tolower((unsigned char)*q);
		c->icon = pm_icon_load(lc, ICON_SZ);
	}
}

static void set_wmstate(Client *c, long state)
{
	long data[] = { state, None };
	XChangeProperty(dpy, c->win, atom[WMState], atom[WMState], 32, PropModeReplace, (unsigned char *)data, 2);
}

static void set_netstate(Client *c)
{
	Atom st[4]; int n = 0;
	if (c->fullscreen) st[n++] = atom[NetWMStateFS];
	if (c->minimized)  st[n++] = atom[NetWMStateHidden];
	if (c->zoomed)   { st[n++] = atom[NetWMStateMaxV]; st[n++] = atom[NetWMStateMaxH]; }
	XChangeProperty(dpy, c->win, atom[NetWMState], XA_ATOM, 32, PropModeReplace, (unsigned char *)st, n);
	long ext[] = { bw(c), bw(c), th(c), bw(c) };
	XChangeProperty(dpy, c->win, atom[NetFrameExtents], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)ext, 4);
}

static int cmpseq(const void *a, const void *b)
{
	unsigned long x = (*(Client *const *)a)->seq, y = (*(Client *const *)b)->seq;
	return x < y ? -1 : x > y;
}

/* _NET_CLIENT_LIST 依開啟順序排列，面板的 Dock 才不會亂跳 */
static void update_clientlist(void)
{
	Client *v[512];
	int n = 0;
	for (Client *c = clients; c && n < (int)LEN(v); c = c->next)
		v[n++] = c;
	qsort(v, n, sizeof *v, cmpseq);
	Window w[512];
	for (int i = 0; i < n; i++)
		w[i] = v[i]->win;
	XChangeProperty(dpy, pm_root, atom[NetClientList], XA_WINDOW, 32, PropModeReplace, (unsigned char *)w, n);
}

static void update_workarea(void)
{
	Rect r = workarea(pmon);
	long wa[] = { r.x, r.y, r.w, r.h };
	XChangeProperty(dpy, pm_root, atom[NetWorkarea], XA_CARDINAL, 32, PropModeReplace, (unsigned char *)wa, 4);
}

/* ------------------------------------------------------------------ */
/* 繪製：標題列                                                        */
/* ------------------------------------------------------------------ */

static const unsigned int btn_color[3] = { 0xff5f57, 0xfebc2e, 0x28c840 };
static const unsigned int btn_edge[3]  = { 0xe0443e, 0xdea123, 0x1aab29 };

/* 視窗按鈕放右邊（預設，由左到右：縮小、放大、關閉）或左邊（關閉、縮小、放大）：appearance.buttons_right */
static int btns_right = 1;
#define BTN_ZONE (border_w + 14 + 2 * 20 + 10)   /* 三顆按鈕從視窗邊緣算起佔的寬度 */

/* 第 i 顆按鈕（0 關閉、1 縮小、2 放大）的中心 */
static int btn_cx(Client *c, int i)
{
	static const int from_right[3] = { 0, 2, 1 };
	return btns_right ? fw(c) - border_w - 14 - from_right[i] * 20 : border_w + 14 + i * 20;
}

static int btn_at(Client *c, int x, int y)
{
	if (y < 0 || y >= title_h)
		return -1;
	for (int i = 0; i < 3; i++)
		if (abs(x - btn_cx(c, i)) <= 8)
			return i;
	return -1;
}

static void draw_frame(Client *c)
{
	if (!th(c))
		return;
	int W = fw(c), act = (c == sel);
	Paint p;
	cairo_t *cr = pm_paint_begin(&p, c->frame, W, title_h);
	pm_rgb(cr, T->border);
	cairo_paint(cr);
	pm_rrect(cr, border_w, border_w, W - 2 * border_w, title_h, corner_r - border_w, 0);
	pm_rgb(cr, act ? T->title : T->title_in);
	cairo_fill(cr);
	pm_rgb(cr, T->sep);
	cairo_rectangle(cr, border_w, title_h - 1, W - 2 * border_w, 1);
	cairo_fill(cr);

	/* 紅綠燈按鈕 */
	double cy = title_h / 2.0;
	for (int i = 0; i < 3; i++) {
		double cx = btn_cx(c, i);
		int colored = act || c->hover;
		cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
		pm_rgb(cr, colored ? btn_color[i] : T->btn_in);
		cairo_fill_preserve(cr);
		pm_rgba(cr, colored ? btn_edge[i] : 0x000000, colored ? 1 : 0.12);
		cairo_set_line_width(cr, 0.8);
		cairo_stroke(cr);
		if (c->pressed == i + 1) {
			cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
			pm_rgba(cr, 0, 0.25);
			cairo_fill(cr);
		}
		if (!c->hover)
			continue;
		pm_rgba(cr, 0x000000, 0.55);
		cairo_set_line_width(cr, 1.2);
		if (i == 0) {
			cairo_move_to(cr, cx - 2.6, cy - 2.6); cairo_line_to(cr, cx + 2.6, cy + 2.6);
			cairo_move_to(cr, cx + 2.6, cy - 2.6); cairo_line_to(cr, cx - 2.6, cy + 2.6);
			cairo_stroke(cr);
		} else if (i == 1) {
			cairo_move_to(cr, cx - 3.2, cy); cairo_line_to(cr, cx + 3.2, cy);
			cairo_stroke(cr);
		} else {
			cairo_move_to(cr, cx - 3.2, cy - 3.2); cairo_line_to(cr, cx + 1.4, cy - 3.2);
			cairo_line_to(cr, cx - 3.2, cy + 1.4); cairo_close_path(cr);
			cairo_move_to(cr, cx + 3.2, cy + 3.2); cairo_line_to(cr, cx - 1.4, cy + 3.2);
			cairo_line_to(cr, cx + 3.2, cy - 1.4); cairo_close_path(cr);
			cairo_fill(cr);
		}
	}

	/* 置中標題 */
	char buf[300];
	pm_font(cr, pm_size, act);
	int room = W - 2 * (BTN_ZONE + 6);
	if (room > 20) {
		pm_ellipsize(cr, c->title, room, buf, sizeof buf);
		double tw = pm_textw(cr, buf);
		pm_rgb(cr, act ? T->text : T->text_in);
		pm_text(cr, (W - tw) / 2, cy, buf);
	}
	pm_paint_end(&p, cr, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 疊放順序與焦點                                                      */
/* ------------------------------------------------------------------ */

static void restack(void)
{
	if (sel && !sel->minimized) {
		XRaiseWindow(dpy, sel->frame);
		for (Client *c = clients; c; c = c->next)
			if (c->transient == sel->win && !c->minimized)
				XRaiseWindow(dpy, c->frame);
	}
	if (!(sel && sel->fullscreen))
		for (int i = 0; i < ndocks; i++)
			XRaiseWindow(dpy, docks[i].w);
	/* 縮放把手緊貼在外框下方 */
	for (Client *c = clients; c; c = c->next) {
		XWindowChanges wc = { .sibling = c->frame, .stack_mode = Below };
		XConfigureWindow(dpy, c->grip, CWSibling | CWStackMode, &wc);
	}
}

static void grab_buttons(Client *c, int focused)
{
	unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask | LockMask };
	XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
	if (!focused)
		XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, ButtonPressMask,
		            GrabModeSync, GrabModeSync, None, None);
	for (unsigned i = 0; i < LEN(mods); i++) {
		XGrabButton(dpy, Button1, MODKEY | mods[i], c->win, False, ButtonPressMask,
		            GrabModeAsync, GrabModeAsync, None, None);
		XGrabButton(dpy, Button3, MODKEY | mods[i], c->win, False, ButtonPressMask,
		            GrabModeAsync, GrabModeAsync, None, None);
	}
}

static void focus(Client *c)
{
	Client *old = sel;
	if (c && c->minimized)
		c = NULL;
	if (!c)
		for (c = clients; c && c->minimized; c = c->next);
	sel = c;
	if (old && old != c && alive(old)) {
		grab_buttons(old, 0);
		draw_frame(old);
	}
	if (c) {
		/* 移到焦點堆疊最前面 */
		Client **pp = &clients;
		while (*pp != c) pp = &(*pp)->next;
		*pp = c->next;
		c->next = clients;
		clients = c;
		grab_buttons(c, 1);
		if (!c->neverfocus)
			XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		if (has_proto(c, atom[WMTakeFocus]))
			send_msg(c->win, atom[WMProtocols], atom[WMTakeFocus], CurrentTime);
		XChangeProperty(dpy, pm_root, atom[NetActiveWindow], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&c->win, 1);
		draw_frame(c);
	} else {
		XSetInputFocus(dpy, pm_root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, pm_root, atom[NetActiveWindow]);
	}
	restack();
}

/* ------------------------------------------------------------------ */
/* 幾何                                                                */
/* ------------------------------------------------------------------ */

static void send_configure(Client *c)
{
	XConfigureEvent ce = {
		.type = ConfigureNotify, .display = dpy, .event = c->win, .window = c->win,
		.x = c->x + bw(c), .y = c->y + th(c), .width = c->w, .height = c->h,
		.border_width = 0, .above = None, .override_redirect = False,
	};
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static void apply_geom(Client *c)
{
	c->w = MAX(c->w, 1);
	c->h = MAX(c->h, 1);
	int W = fw(c), H = fh(c);
	XMoveResizeWindow(dpy, c->frame, c->x, c->y, W, H);
	XMoveResizeWindow(dpy, c->win, bw(c), th(c), c->w, c->h);
	XMoveResizeWindow(dpy, c->grip, c->x - GRIP, c->y - GRIP, W + 2 * GRIP, H + 2 * GRIP);
	if (W != c->shp_w || H != c->shp_h) {
		int r = th(c) ? corner_r : 0;
		pm_shape_round(c->frame, W, H, r, r);
		c->shp_w = W; c->shp_h = H;
		draw_frame(c);
	}
	send_configure(c);
}

static void save_geom(Client *c)
{
	c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
}

static void restore_geom(Client *c)
{
	c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
}

static void set_fullscreen(Client *c, int on)
{
	if (on == c->fullscreen)
		return;
	if (on) {
		if (!c->zoomed) save_geom(c);
		c->fullscreen = 1;
		Rect m = mons[mon_of(c)];
		c->x = m.x; c->y = m.y; c->w = m.w; c->h = m.h;
	} else {
		c->fullscreen = 0;
		restore_geom(c);
		c->zoomed = 0;
	}
	c->shp_w = -1;
	apply_geom(c);
	set_netstate(c);
	restack();
}

static void set_zoom(Client *c, int on)
{
	if (c->fullscreen || on == c->zoomed)
		return;
	if (on) {
		save_geom(c);
		Rect r = workarea(mon_of(c));
		c->x = r.x; c->y = r.y;
		c->w = r.w - 2 * bw(c);
		c->h = r.h - th(c) - bw(c);
	} else {
		restore_geom(c);
	}
	c->zoomed = on;
	apply_geom(c);
	set_netstate(c);
}

/* ------------------------------------------------------------------ */
/* 管理視窗                                                            */
/* ------------------------------------------------------------------ */

static void read_strut(Dock *d)
{
	Atom type; int fmt; unsigned long n = 0, extra;
	unsigned char *p = NULL;
	memset(d->strut, 0, sizeof d->strut);
	if (XGetWindowProperty(dpy, d->w, atom[NetWMStrutPartial], 0, 12, False, XA_CARDINAL,
	                       &type, &fmt, &n, &extra, &p) == Success && p && n == 12) {
		memcpy(d->strut, p, sizeof d->strut);
	} else {
		if (p) { XFree(p); p = NULL; }
		/* 舊式 _NET_WM_STRUT：整條邊 */
		if (XGetWindowProperty(dpy, d->w, atom[NetWMStrut], 0, 4, False, XA_CARDINAL,
		                       &type, &fmt, &n, &extra, &p) == Success && p && n == 4) {
			memcpy(d->strut, p, 4 * sizeof(long));
			d->strut[5] = d->strut[7] = sh - 1;
			d->strut[9] = d->strut[11] = sw - 1;
		}
	}
	if (p) XFree(p);
}

static void workarea_changed(void)
{
	update_workarea();
	/* 已放大的視窗跟著調整 */
	for (Client *c = clients; c; c = c->next)
		if (c->zoomed && !c->fullscreen) {
			Rect r = workarea(mon_of(c));
			c->x = r.x; c->y = r.y; c->w = r.w - 2 * bw(c); c->h = r.h - th(c) - bw(c);
			apply_geom(c);
		}
}

static Dock *find_dock(Window w)
{
	for (int i = 0; i < ndocks; i++)
		if (docks[i].w == w)
			return &docks[i];
	return NULL;
}

static void dock_add(Window w)
{
	XMapWindow(dpy, w);
	Dock *d = find_dock(w);
	if (!d && ndocks < (int)LEN(docks)) {
		d = &docks[ndocks++];
		d->w = w;
		XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
	}
	if (d) {
		read_strut(d);
		workarea_changed();
	}
	XRaiseWindow(dpy, w);
}

static void dock_remove(Window w)
{
	for (int i = 0; i < ndocks; i++)
		if (docks[i].w == w) {
			docks[i] = docks[--ndocks];
			workarea_changed();
			return;
		}
}

static void manage(Window w, XWindowAttributes *wa)
{
	Atom types[8];
	int nt = pm_getatoms(w, atom[NetWMWindowType], types, 8);
	for (int i = 0; i < nt; i++) {
		if (types[i] == atom[NetWMTypeDock]) {
			dock_add(w);
			return;
		}
		if (types[i] == atom[NetWMTypeDesktop]) {
			XMapWindow(dpy, w);
			XLowerWindow(dpy, w);
			return;
		}
	}

	Client *c = calloc(1, sizeof *c);
	c->win = w;
	c->seq = ++seqno;
	c->w = wa->width; c->h = wa->height;
	update_title(c);
	update_hints(c);

	XClassHint ch = { 0 };
	if (XGetClassHint(dpy, w, &ch)) {
		if (ch.res_class) snprintf(c->cls, sizeof c->cls, "%s", ch.res_class);
		if (ch.res_name)  snprintf(c->inst, sizeof c->inst, "%s", ch.res_name);
		if (ch.res_class) XFree(ch.res_class);
		if (ch.res_name)  XFree(ch.res_name);
	}
	update_icon(c);
	XGetTransientForHint(dpy, w, &c->transient);

	/* 自行繪製標題列(CSD)的程式不加外框 */
	{
		Atom type; int fmt; unsigned long n = 0, extra;
		unsigned char *p = NULL;
		if (XGetWindowProperty(dpy, w, atom[MotifHints], 0, 5, False, AnyPropertyType,
		                       &type, &fmt, &n, &extra, &p) == Success && p) {
			unsigned long *m = (unsigned long *)p;
			if (n >= 3 && (m[0] & 2) && m[2] == 0)
				c->noframe = 1;
			XFree(p);
		}
		for (int i = 0; i < nt; i++)
			if (types[i] == atom[NetWMTypeSplash])
				c->noframe = 1;
	}

	/* 初始位置 */
	XSizeHints h; long supplied;
	/* 已經顯示中的視窗（例如重新啟動 WM 時）維持原位置 */
	int userpos = wa->map_state == IsViewable ||
	              (XGetWMNormalHints(dpy, w, &h, &supplied) && (h.flags & (USPosition | PPosition)) &&
	               (wa->x || wa->y));
	Client *parent = c->transient ? find(c->transient) : NULL;
	Rect r = workarea(parent ? mon_of(parent) : userpos ? pm_mon_at(wa->x, wa->y) : pm_pointer_mon());
	c->w = MIN(c->w, r.w - 2 * bw(c));
	c->h = MIN(c->h, r.h - th(c) - bw(c));
	if (parent) {
		c->x = parent->x + (fw(parent) - fw(c)) / 2;
		c->y = parent->y + (fh(parent) - fh(c)) / 3;
	} else if (userpos) {
		c->x = wa->x - bw(c);
		c->y = wa->y - th(c);
	} else {
		c->x = r.x + (r.w - fw(c)) / 2;
		c->y = r.y + (r.h - fh(c)) / 3;
		/* 避免視窗完全重疊：往右下錯開 */
		for (int tries = 0; tries < 20; tries++) {
			int clash = 0;
			for (Client *o = clients; o; o = o->next)
				if (!o->minimized && o->x == c->x && o->y == c->y)
					clash = 1;
			if (!clash) break;
			c->x += 26; c->y += 26;
		}
	}
	c->x = MAX(r.x, MIN(c->x, r.x + r.w - fw(c)));
	c->y = MAX(r.y, MIN(c->y, r.y + r.h - fh(c)));
	save_geom(c);

	XSetWindowAttributes swa = {
		.background_pixel = T->border,
		.event_mask = SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask |
		              ButtonReleaseMask | ExposureMask | PointerMotionMask | LeaveWindowMask,
	};
	c->frame = XCreateWindow(dpy, pm_root, c->x, c->y, fw(c), fh(c), 0, CopyFromParent, InputOutput,
	                         CopyFromParent, CWBackPixel | CWEventMask, &swa);
	swa.event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
	c->grip = XCreateWindow(dpy, pm_root, c->x - GRIP, c->y - GRIP, fw(c) + 2 * GRIP, fh(c) + 2 * GRIP, 0,
	                        0, InputOnly, CopyFromParent, CWEventMask, &swa);

	XAddToSaveSet(dpy, w);
	XSetWindowBorderWidth(dpy, w, 0);
	if (wa->map_state == IsViewable)
		c->ignore_unmap++;
	XReparentWindow(dpy, w, c->frame, bw(c), th(c));
	XSelectInput(dpy, w, PropertyChangeMask);

	c->next = clients;
	clients = c;

	c->shp_w = -1;
	apply_geom(c);
	XMapWindow(dpy, w);
	XMapWindow(dpy, c->frame);
	XMapWindow(dpy, c->grip);
	set_wmstate(c, NormalState);

	Atom st[16];
	int ns = pm_getatoms(w, atom[NetWMState], st, 16);
	for (int i = 0; i < ns; i++)
		if (st[i] == atom[NetWMStateFS])
			set_fullscreen(c, 1);
	set_netstate(c);

	update_clientlist();
	focus(c);
}

static void unmanage(Client *c, int destroyed)
{
	XGrabServer(dpy);
	if (!destroyed) {
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		XReparentWindow(dpy, c->win, pm_root, c->x + bw(c), c->y + th(c));
		XRemoveFromSaveSet(dpy, c->win);
		set_wmstate(c, WithdrawnState);
	}
	XDestroyWindow(dpy, c->frame);
	XDestroyWindow(dpy, c->grip);
	XSync(dpy, False);
	XUngrabServer(dpy);

	Client **pp = &clients;
	while (*pp && *pp != c) pp = &(*pp)->next;
	if (*pp) *pp = c->next;
	if (c->icon) cairo_surface_destroy(c->icon);
	if (sel == c) sel = NULL;
	free(c);
	update_clientlist();
	if (!sel)
		focus(NULL);
}

static void set_minimized(Client *c, int on)
{
	if (c->minimized == on)
		return;
	c->minimized = on;
	if (on) {
		XUnmapWindow(dpy, c->frame);
		XUnmapWindow(dpy, c->grip);
		set_wmstate(c, IconicState);
		if (sel == c) {
			sel = NULL;
			focus(NULL);
		}
	} else {
		XMapWindow(dpy, c->frame);
		XMapWindow(dpy, c->grip);
		set_wmstate(c, NormalState);
	}
	set_netstate(c);
}

static void activate(Client *c)
{
	if (c->minimized)
		set_minimized(c, 0);
	focus(c);
}

/* ------------------------------------------------------------------ */
/* 滑鼠拖曳                                                            */
/* ------------------------------------------------------------------ */

static void move_mouse(Client *c, int rx, int ry)
{
	if (c->fullscreen)
		return;
	if (XGrabPointer(dpy, pm_root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync, None, cur_move, CurrentTime) != GrabSuccess)
		return;
	int ox = c->x, oy = c->y;
	Time last = 0;
	XEvent ev;
	for (;;) {
		if (!pm_modal_next(&ev)) {
			if (!alive(c)) break;
			continue;
		}
		if (ev.type == ButtonRelease)
			break;
		if (ev.type != MotionNotify || ev.xmotion.time - last < 1000 / 60)
			continue;
		last = ev.xmotion.time;
		int nx = ox + ev.xmotion.x_root - rx, ny = oy + ev.xmotion.y_root - ry;
		if (c->zoomed && (abs(nx - ox) > 10 || abs(ny - oy) > 10)) {
			/* 拖曳縮放中的視窗：恢復原大小，並讓滑鼠維持在標題列相對位置 */
			double fx = (double)(rx - c->x) / fw(c);
			c->zoomed = 0;
			c->w = c->sw; c->h = c->sh;
			ox = rx - (int)(fx * fw(c));
			nx = ox + ev.xmotion.x_root - rx;
			set_netstate(c);
		} else if (c->zoomed) {
			continue;
		}
		c->x = nx;
		c->y = MAX(ny, workarea(pm_mon_at(ev.xmotion.x_root, ev.xmotion.y_root)).y);
		apply_geom(c);
	}
	XUngrabPointer(dpy, CurrentTime);
}

static void resize_mouse(Client *c, int edges, int rx, int ry)
{
	if (c->fullscreen)
		return;
	if (XGrabPointer(dpy, pm_root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync, None, cur_edge[edges], CurrentTime) != GrabSuccess)
		return;
	int ox = c->x, oy = c->y, ow = c->w, oh = c->h, top = workarea(mon_of(c)).y;
	Time last = 0;
	XEvent ev;
	c->zoomed = 0;
	for (;;) {
		if (!pm_modal_next(&ev)) {
			if (!alive(c)) break;
			continue;
		}
		if (ev.type == ButtonRelease)
			break;
		if (ev.type != MotionNotify || ev.xmotion.time - last < 1000 / 60)
			continue;
		last = ev.xmotion.time;
		int dx = ev.xmotion.x_root - rx, dy = ev.xmotion.y_root - ry;
		int nw = ow, nh = oh;
		if (edges & EDGE_R) nw = ow + dx;
		if (edges & EDGE_L) nw = ow - dx;
		if (edges & EDGE_B) nh = oh + dy;
		if (edges & EDGE_T) nh = oh - dy;
		nw = MAX(nw, c->minw); nh = MAX(nh, c->minh);
		if (c->maxw) nw = MIN(nw, c->maxw);
		if (c->maxh) nh = MIN(nh, c->maxh);
		c->x = (edges & EDGE_L) ? ox + ow - nw : ox;
		c->y = (edges & EDGE_T) ? oy + oh - nh : oy;
		if (c->y < top) { nh -= top - c->y; c->y = top; }
		c->w = nw; c->h = nh;
		apply_geom(c);
	}
	set_netstate(c);
	XUngrabPointer(dpy, CurrentTime);
}

static int grip_edges(Client *c, int rx, int ry)
{
	int e = 0, k = 16;
	if (rx < c->x + k) e |= EDGE_L;
	else if (rx >= c->x + fw(c) - k) e |= EDGE_R;
	if (ry < c->y + k) e |= EDGE_T;
	else if (ry >= c->y + fh(c) - k) e |= EDGE_B;
	return e ? e : EDGE_R | EDGE_B;
}

/* ------------------------------------------------------------------ */
/* 彈出選單                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Spotlight 啟動器                                                    */
/* ------------------------------------------------------------------ */

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char **scan_path(int *count)
{
	char **v = NULL;
	int n = 0, cap = 0;
	char *path = strdup(getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	for (char *dir = strtok(path, ":"); dir; dir = strtok(NULL, ":")) {
		DIR *d = opendir(dir);
		if (!d) continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char full[PATH_MAX];
			struct stat st;
			snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
			if (stat(full, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
				continue;
			if (n == cap) { cap = cap ? cap * 2 : 1024; v = realloc(v, cap * sizeof *v); }
			v[n++] = strdup(e->d_name);
		}
		closedir(d);
	}
	free(path);
	if (n) qsort(v, n, sizeof *v, cmpstr);
	int m = 0;
	for (int i = 0; i < n; i++) {
		if (m && !strcmp(v[m - 1], v[i])) { free(v[i]); continue; }
		v[m++] = v[i];
	}
	*count = m;
	return v;
}

/* 已安裝的應用程式（.desktop 檔） */
typedef struct {
	char *id, *name, *alt, *exec, *icon, *kw;
	int term, tried;
	cairo_surface_t *surf;
} App;

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

/* 去掉 Exec 裡的 %f %U 等參數代號 */
static char *clean_exec(const char *in)
{
	char *out = malloc(strlen(in) + 1), *o = out;
	for (const char *p = in; *p; p++) {
		if (*p == '%' && p[1]) {
			if (p[1] == '%') *o++ = '%';
			p++;
			continue;
		}
		*o++ = *p;
	}
	*o = 0;
	char *t = trim(out);
	memmove(out, t, strlen(t) + 1);
	return out;
}

static int parse_desktop(const char *path, const char *id, App *a, const char *lfull, const char *lshort)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[4096], kfull[64], kshort[64], gfull[64], kwfull[64];
	snprintf(kfull, sizeof kfull, "Name[%s]", lfull);
	snprintf(kshort, sizeof kshort, "Name[%s]", lshort);
	snprintf(gfull, sizeof gfull, "GenericName[%s]", lfull);
	snprintf(kwfull, sizeof kwfull, "Keywords[%s]", lfull);
	char *name = NULL, *lname = NULL, *sname = NULL, *gen = NULL, *lgen = NULL, *exec = NULL,
	     *icon = NULL, *kw = NULL, *lkw = NULL, *domain = NULL;
	int in = 0, ok = 1, isapp = 0, term = 0;
	while (fgets(line, sizeof line, f)) {
		char *l = trim(line);
		if (*l == '[') {
			if (in) break;
			in = !strcmp(l, "[Desktop Entry]");
			continue;
		}
		char *eq = strchr(l, '=');
		if (!in || *l == '#' || !eq) continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1), **dst = NULL;
		if (!strcmp(k, "Type")) isapp = !strcmp(v, "Application");
		else if (!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) { if (!strcmp(v, "true")) ok = 0; }
		else if (!strcmp(k, "Terminal")) term = !strcmp(v, "true");
		else if (!strcmp(k, "Name")) dst = &name;
		else if (!strcmp(k, kfull)) dst = &lname;
		else if (!strcmp(k, kshort)) dst = &sname;
		else if (!strcmp(k, "GenericName")) dst = &gen;
		else if (!strcmp(k, gfull)) dst = &lgen;
		else if (!strcmp(k, "Exec")) dst = &exec;
		else if (!strcmp(k, "Icon")) dst = &icon;
		else if (!strcmp(k, "Keywords")) dst = &kw;
		else if (!strcmp(k, kwfull)) dst = &lkw;
		else if (!strcmp(k, "X-Ubuntu-Gettext-Domain") || !strcmp(k, "X-GNOME-Gettext-Domain")) dst = &domain;
		if (dst && !*dst) *dst = strdup(v);
	}
	fclose(f);
	int good = ok && isapp && name && exec;
	if (good) {
		const char *disp = lname ? lname : sname;
		if (!disp && domain) {
			/* Ubuntu 把翻譯放在 .mo 檔裡 */
			bind_textdomain_codeset(domain, "UTF-8");
			const char *t = dgettext(domain, name);
			if (t != name) disp = t;
			if (!lgen && gen) {
				const char *g = dgettext(domain, gen);
				if (g != gen) lgen = strdup(g);
			}
		}
		a->id = strdup(id);
		a->name = strdup(disp ? disp : name);
		a->alt = strcmp(a->name, name) ? strdup(name) : (lgen ? strdup(lgen) : gen ? strdup(gen) : NULL);
		a->exec = clean_exec(exec);
		a->icon = icon ? strdup(icon) : NULL;
		size_t kl = (kw ? strlen(kw) : 0) + (lkw ? strlen(lkw) : 0) + (lgen ? strlen(lgen) : 0) + (gen ? strlen(gen) : 0) + 8;
		a->kw = malloc(kl);
		snprintf(a->kw, kl, "%s;%s;%s;%s", kw ? kw : "", lkw ? lkw : "", gen ? gen : "", lgen ? lgen : "");
		a->term = term;
		a->surf = NULL;
		a->tried = 0;
	}
	free(name); free(lname); free(sname); free(gen); free(lgen); free(exec); free(icon); free(kw); free(lkw); free(domain);
	return good;
}

static App *scan_apps(int *count)
{
	char dirs[6][PATH_MAX];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_DATA_HOME");
	int nd = 0;
	if (xdg && *xdg) snprintf(dirs[nd++], PATH_MAX, "%s/applications", xdg);
	else if (home) snprintf(dirs[nd++], PATH_MAX, "%s/.local/share/applications", home);
	if (home) snprintf(dirs[nd++], PATH_MAX, "%s/.local/share/flatpak/exports/share/applications", home);
	snprintf(dirs[nd++], PATH_MAX, "/var/lib/flatpak/exports/share/applications");
	snprintf(dirs[nd++], PATH_MAX, "/var/lib/snapd/desktop/applications");
	snprintf(dirs[nd++], PATH_MAX, "/usr/local/share/applications");
	snprintf(dirs[nd++], PATH_MAX, "/usr/share/applications");

	/* 語系：zh_TW.UTF-8 → zh_TW / zh */
	char lfull[32] = "", lshort[32] = "";
	const char *loc = setlocale(LC_MESSAGES, NULL);
	snprintf(lfull, sizeof lfull, "%s", loc ? loc : "C");
	lfull[strcspn(lfull, ".@")] = 0;
	snprintf(lshort, sizeof lshort, "%s", lfull);
	lshort[strcspn(lshort, "_")] = 0;

	App *v = NULL;
	int n = 0, cap = 0;
	/* 看過的檔名（包括隱藏的）：使用者目錄的同名檔優先。使用者用 NoDisplay／Hidden 隱藏的程式
	 * （例如在「應用程式選單」關掉「在啟動器與搜尋中顯示」）也要記下來，不然系統那份會冒出來 */
	char **seen = NULL;
	int nseen = 0, seencap = 0;
	for (int d = 0; d < nd; d++) {
		DIR *dir = opendir(dirs[d]);
		if (!dir) continue;
		struct dirent *e;
		while ((e = readdir(dir))) {
			size_t l = strlen(e->d_name);
			if (l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
			int dup = 0;
			for (int i = 0; i < nseen && !dup; i++)
				dup = !strcmp(seen[i], e->d_name);
			if (dup) continue;
			if (nseen == seencap) { seencap = seencap ? seencap * 2 : 256; seen = realloc(seen, seencap * sizeof *seen); }
			seen[nseen++] = strdup(e->d_name);
			char path[PATH_MAX];
			snprintf(path, sizeof path, "%s/%s", dirs[d], e->d_name);
			if (n == cap) { cap = cap ? cap * 2 : 256; v = realloc(v, cap * sizeof *v); }
			if (parse_desktop(path, e->d_name, &v[n], lfull, lshort))
				n++;
		}
		closedir(dir);
	}
	for (int i = 0; i < nseen; i++) free(seen[i]);
	free(seen);
	*count = n;
	return v;
}

static void free_apps(App *v, int n)
{
	for (int i = 0; i < n; i++) {
		free(v[i].id); free(v[i].name); free(v[i].alt); free(v[i].exec); free(v[i].icon); free(v[i].kw);
		if (v[i].surf) cairo_surface_destroy(v[i].surf);
	}
	free(v);
}

static int app_score(App *a, const char *q, size_t ql)
{
	const char *p;
	if (!strncasecmp(a->name, q, ql)) return 100;
	if ((p = strcasestr(a->name, q))) return p[-1] == ' ' ? 90 : 75;
	if (a->alt && !strncasecmp(a->alt, q, ql)) return 85;
	char bin[256];
	snprintf(bin, sizeof bin, "%s", a->exec);
	bin[strcspn(bin, " ")] = 0;
	const char *b = strrchr(bin, '/') ? strrchr(bin, '/') + 1 : bin;
	if (!strncasecmp(b, q, ql)) return 70;
	if (a->alt && strcasestr(a->alt, q)) return 60;
	if (ql > 1 && strcasestr(a->kw, q)) return 50;
	return 0;
}

static void draw_cmd_icon(cairo_t *cr, double x, double y, double s)
{
	pm_rrect(cr, x, y, s, s, s * 0.22, s * 0.22);
	pm_rgb(cr, 0x2b2b2b);
	cairo_fill(cr);
	pm_rgb(cr, 0xffffff);
	cairo_set_line_width(cr, s / 14);
	cairo_move_to(cr, x + s * 0.24, y + s * 0.34);
	cairo_line_to(cr, x + s * 0.42, y + s * 0.5);
	cairo_line_to(cr, x + s * 0.24, y + s * 0.66);
	cairo_move_to(cr, x + s * 0.48, y + s * 0.68);
	cairo_line_to(cr, x + s * 0.74, y + s * 0.68);
	cairo_stroke(cr);
}

/* Spotlight 計算機：+ - * / % ^ 與括號 */
static const char *cp;
static int calc_err;
static double calc_expr(void);
static void calc_ws(void) { while (*cp == ' ') cp++; }
static double calc_atom(void)
{
	calc_ws();
	if (*cp == '(') { cp++; double v = calc_expr(); calc_ws(); if (*cp == ')') cp++; else calc_err = 1; return v; }
	if (*cp == '-') { cp++; return -calc_atom(); }
	if (*cp == '+') { cp++; return calc_atom(); }
	char *e;
	double v = strtod(cp, &e);
	if (e == cp) calc_err = 1;
	cp = e;
	return v;
}
static double calc_pow(void)
{
	double v = calc_atom();
	calc_ws();
	if (*cp == '^') { cp++; return pow(v, calc_pow()); }
	return v;
}
static double calc_term(void)
{
	double v = calc_pow();
	for (;;) {
		calc_ws();
		if (*cp == '*' || *cp == 'x') { cp++; v *= calc_pow(); }
		else if (*cp == '/') { cp++; v /= calc_pow(); }
		else if (*cp == '%') { cp++; v = fmod(v, calc_pow()); }
		else return v;
	}
}
static double calc_expr(void)
{
	double v = calc_term();
	for (;;) {
		calc_ws();
		if (*cp == '+') { cp++; v += calc_term(); }
		else if (*cp == '-') { cp++; v -= calc_term(); }
		else return v;
	}
}
/* 是算式就把答案寫進 out 並回傳 1 */
static int calculate(const char *q, char *out, size_t n)
{
	int op = 0, digit = 0;
	for (const char *c = q; *c; c++) {
		if (strchr("+-*/%^x", *c) && c != q) op = 1;
		if (isdigit((unsigned char)*c)) digit = 1;
		else if (!strchr("+-*/%^x(). ", *c)) return 0;
	}
	if (!op || !digit) return 0;
	cp = q;
	calc_err = 0;
	double v = calc_expr();
	calc_ws();
	if (calc_err || *cp || isnan(v) || isinf(v)) return 0;
	if (fabs(v - llround(v)) < 1e-9 && fabs(v) < 1e15) snprintf(out, n, "%lld", llround(v));
	else snprintf(out, n, "%.10g", v);
	return 1;
}

static void spotlight(const Arg *arg)
{
	(void)arg;
	enum { MAXR = 8, W = 680, HEAD = 56, ROW = 42, ISZ = 30 };
	int ncmd = 0, napps = 0;
	char **cmds = scan_path(&ncmd);
	App *apps = scan_apps(&napps);
	struct { int app, cmd, file, calc, ai; } res[MAXR];
	int have_ai = pm_have("pomelo-assistant");   /* 最後一行可以「問小幫手」 */
	int nr = 0, hot = 0;
	char q[256] = "";
	/* 設定：搜尋指令、檔案、計算機 */
	int want_cmds = pm_conf_int("spotlight.commands", 1), want_files = pm_conf_int("spotlight.files", 1);
	int want_calc = pm_conf_int("spotlight.calculator", 1);
	const char *locate = want_files ? (pm_have("plocate") ? "plocate" : pm_have("locate") ? "locate" : NULL) : NULL;
	static char files[MAXR][PATH_MAX];
	char answer[64] = "";
	const char *home = getenv("HOME") ? getenv("HOME") : "/";

	Rect M = mons[pm_pointer_mon()];
	XSetWindowAttributes swa = { .override_redirect = True, .background_pixel = T->menu,
	                             .event_mask = ExposureMask | KeyPressMask | FocusChangeMask };
	int x = M.x + (M.w - W) / 2, y = M.y + M.h / 5;
	Window w = XCreateWindow(dpy, pm_root, x, y, W, HEAD, 0, CopyFromParent, InputOutput, CopyFromParent,
	                         CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	pm_shape_round(w, W, HEAD, 12, 12);
	XMapRaised(dpy, w);
	XSync(dpy, False);

	/* 輸入法(fcitx/ibus 的 XIM)支援，可以直接打中文搜尋 */
	/* 輸入法伺服器結束或重新啟動時 pm_xim_watch 會把 xim 設成 NULL、之後自動重連；
	 * 用舊連線建的 IC 不能再碰，所以記下世代編號 */
	static int watched;
	if (!watched) { pm_xim_watch(dpy, &xim); watched = 1; }
	unsigned im_gen = pm_xim_generation();
	XIC xic = xim ? XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                          XNClientWindow, w, XNFocusWindow, w, NULL) : NULL;
	XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
	if (xic) XSetICFocus(xic);
	int grabbed = 0;
	for (int i = 0; i < 200 && !grabbed; i++) {
		grabbed = XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
		if (!grabbed) usleep(1000);
	}
	XGrabPointer(dpy, pm_root, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, cur_normal, CurrentTime);

	int done = 0, run = 0, changed = 1;
	XEvent ev;
	while (grabbed && !done) {
		if (changed) {
			size_t ql = strlen(q);
			nr = 0;
			memset(res, 0, sizeof res);
			if (ql && want_calc && calculate(q, answer, sizeof answer)) {
				res[0].app = res[0].cmd = res[0].file = -1;
				res[0].calc = 1;
				nr = 1;
			}
			if (ql) {
				/* 應用程式依分數排序，再補上檔案與 PATH 裡的指令 */
				int sc[MAXR], base = nr;
				for (int i = 0; i < napps; i++) {
					int s = app_score(&apps[i], q, ql);
					if (!s) continue;
					int k = nr < MAXR ? nr++ : MAXR;
					if (k == MAXR && s <= sc[MAXR - 1]) continue;
					if (k == MAXR) k = MAXR - 1;
					while (k > base && sc[k - 1] < s) { sc[k] = sc[k - 1]; res[k] = res[k - 1]; k--; }
					sc[k] = s; res[k].app = i; res[k].cmd = res[k].file = -1; res[k].calc = 0;
				}
				/* 檔案：plocate 搜尋家目錄裡的檔名（不含隱藏資料夾），最多 3 筆 */
				if (locate && ql >= 2 && nr < MAXR && !strchr(q, '\'')) {
					char cmd[600];
					snprintf(cmd, sizeof cmd, "%s -i -b -l 60 -- '%s'", locate, q);
					char *out = pm_run(cmd), *save = NULL;
					int nf = 0;
					size_t hl = strlen(home);
					for (char *l = out ? strtok_r(out, "\n", &save) : NULL; l && nf < 3 && nr < MAXR; l = strtok_r(NULL, "\n", &save)) {
						if (strncmp(l, home, hl) || l[hl] != '/' || strstr(l + hl, "/.")) continue;
						if (access(l, F_OK)) continue;   /* 資料庫還沒更新到的已刪除檔案 */
						snprintf(files[nf], sizeof files[0], "%s", l);
						res[nr].app = res[nr].cmd = -1; res[nr].file = nf; res[nr].calc = 0;
						nr++; nf++;
					}
					free(out);
				}
				for (int pass = 0; want_cmds && pass < 2 && nr < MAXR; pass++)
					for (int i = 0; i < ncmd && nr < MAXR; i++) {
						int pre = !strncasecmp(cmds[i], q, ql);
						if (pass == 0 ? !pre : (pre || !strcasestr(cmds[i], q)))
							continue;
						int dup = 0;
						for (int k = 0; k < nr && !dup; k++)
							if (res[k].app >= 0) {
								char bin[256];
								snprintf(bin, sizeof bin, "%s", apps[res[k].app].exec);
								bin[strcspn(bin, " ")] = 0;
								const char *b = strrchr(bin, '/') ? strrchr(bin, '/') + 1 : bin;
								dup = !strcmp(b, cmds[i]);
							}
						if (!dup) { res[nr].app = res[nr].file = -1; res[nr].cmd = i; res[nr].calc = 0; nr++; }
					}
			}
			/* 問小幫手：一定排在最後（滿了就取代最後一個） */
			if (have_ai && ql >= 2 && !(nr && res[0].calc)) {
				int k = nr < MAXR ? nr++ : MAXR - 1;
				res[k].app = res[k].cmd = res[k].file = -1;
				res[k].calc = 0;
				res[k].ai = 1;
			}
			hot = MIN(hot, MAX(nr - 1, 0));
			int H = HEAD + (nr ? nr * ROW + 12 : 0);
			XResizeWindow(dpy, w, W, H);
			pm_shape_round(w, W, H, 12, 12);

			Paint p;
			cairo_t *cr = pm_paint_begin(&p, w, W, H);
			pm_rgb(cr, T->menu);
			cairo_paint(cr);
			pm_rrect(cr, 0.5, 0.5, W - 1, H - 1, 12, 12);
			pm_rgba(cr, 0x000000, 0.2);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			/* 放大鏡 */
			pm_rgb(cr, T->dim);
			cairo_set_line_width(cr, 2);
			cairo_arc(cr, 27, HEAD / 2.0 - 2, 7.5, 0, 2 * M_PI);
			cairo_stroke(cr);
			cairo_move_to(cr, 32.5, HEAD / 2.0 + 3.5);
			cairo_line_to(cr, 38, HEAD / 2.0 + 9);
			cairo_stroke(cr);
			pm_font(cr, 22, 0);
			double tx = 50;
			if (q[0]) {
				pm_rgb(cr, T->menu_text);
				pm_text(cr, tx, HEAD / 2.0, q);
				tx += pm_textw(cr, q);
			} else {
				pm_rgb(cr, T->dim);
				pm_text(cr, tx, HEAD / 2.0, _("搜尋"));
			}
			pm_rgb(cr, T->sel);
			cairo_rectangle(cr, q[0] ? tx + 1 : 49, HEAD / 2.0 - 13, 2, 26);
			cairo_fill(cr);
			if (nr) {
				pm_rgba(cr, 0x000000, pm_dark ? 0.5 : 0.1);
				cairo_rectangle(cr, 0, HEAD, W, 1);
				cairo_fill(cr);
			}
			for (int i = 0; i < nr; i++) {
				double ry = HEAD + 6 + i * ROW, iy = ry + (ROW - ISZ) / 2.0;
				if (i == hot) {
					pm_rrect(cr, 6, ry, W - 12, ROW, 7, 7);
					pm_rgb(cr, T->sel);
					cairo_fill(cr);
				}
				const char *label, *kind;
				char kindbuf[PATH_MAX];
				char aibuf[300];
				if (res[i].ai) {
					static cairo_surface_t *aicon;
					static int aitried;
					if (!aitried) { aicon = pm_icon_load("pomelo-assistant", ISZ * 2); aitried = 1; }
					if (aicon) {
						cairo_save(cr);
						cairo_translate(cr, 16, iy);
						cairo_scale(cr, 0.5, 0.5);
						cairo_set_source_surface(cr, aicon, 0, 0);
						cairo_paint(cr);
						cairo_restore(cr);
					} else draw_cmd_icon(cr, 16, iy, ISZ);
					snprintf(aibuf, sizeof aibuf, _("問小幫手「%s」"), q);
					label = aibuf;
					kind = _("小幫手");
				} else if (res[i].calc) {
					draw_cmd_icon(cr, 16, iy, ISZ);
					static char eq[80];
					snprintf(eq, sizeof eq, "= %s", answer);
					label = eq;
					kind = _("計算機");
				} else if (res[i].file >= 0) {
					const char *f = files[res[i].file];
					struct stat fst;
					int isdir = !stat(f, &fst) && S_ISDIR(fst.st_mode);
					cairo_surface_t *ic = pm_icon_load(isdir ? "folder" : "text-x-generic", ISZ * 2);
					if (ic) {
						cairo_save(cr);
						cairo_translate(cr, 16, iy);
						cairo_scale(cr, 0.5, 0.5);
						cairo_set_source_surface(cr, ic, 0, 0);
						cairo_paint(cr);
						cairo_restore(cr);
						cairo_surface_destroy(ic);
					} else draw_cmd_icon(cr, 16, iy, ISZ);
					label = strrchr(f, '/') + 1;
					/* 種類欄顯示所在資料夾，家目錄寫成 ~ */
					snprintf(kindbuf, sizeof kindbuf, "~%.*s", (int)(strrchr(f, '/') - f - strlen(home)), f + strlen(home));
					if (!strcmp(kindbuf, "~")) snprintf(kindbuf, sizeof kindbuf, "%s", _("家目錄"));
					kind = kindbuf;
				} else if (res[i].app >= 0) {
					App *a = &apps[res[i].app];
					if (!a->tried) { a->surf = pm_icon_load(a->icon, ISZ * 2); a->tried = 1; }
					if (a->surf) {
						cairo_save(cr);
						cairo_translate(cr, 16, iy);
						cairo_scale(cr, 0.5, 0.5);
						cairo_set_source_surface(cr, a->surf, 0, 0);
						cairo_paint(cr);
						cairo_restore(cr);
					} else {
						draw_cmd_icon(cr, 16, iy, ISZ);
					}
					label = a->name;
					kind = a->alt ? a->alt : _("應用程式");
				} else {
					draw_cmd_icon(cr, 16, iy, ISZ);
					label = cmds[res[i].cmd];
					kind = _("指令");
				}
				char buf[200];
				pm_font(cr, 12, 0);
				pm_ellipsize(cr, kind, 180, buf, sizeof buf);
				double kw = pm_textw(cr, buf);
				pm_rgba(cr, i == hot ? T->sel_text : T->dim, i == hot ? 0.85 : 1);
				pm_text(cr, W - 22 - kw, ry + ROW / 2.0, buf);
				pm_font(cr, 15, 0);
				pm_ellipsize(cr, label, W - 90 - kw, buf, sizeof buf);
				pm_rgb(cr, i == hot ? T->sel_text : T->menu_text);
				pm_text(cr, 58, ry + ROW / 2.0, buf);
			}
			pm_paint_end(&p, cr, 0, 0);
			changed = 0;
		}
		if (!pm_modal_next(&ev)) {
			if (ev.type == Expose && ev.xexpose.window == w)
				changed = 1;
			continue;
		}
		if (ev.type == ButtonPress) {
			done = 1;
			continue;
		}
		if (ev.type != KeyPress)
			continue;
		char buf[64];
		KeySym ks = NoSymbol;
		Status st = XLookupNone;
		if (xic && im_gen != pm_xim_generation()) xic = NULL;   /* 輸入法剛剛消失了 */
		int len = xic ? Xutf8LookupString(xic, &ev.xkey, buf, sizeof buf - 1, &ks, &st)
		              : XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, NULL);
		if (xic && st == XBufferOverflow) len = 0;
		buf[MAX(len, 0)] = 0;
		size_t ql = strlen(q);
		switch (ks) {
		case XK_Escape: done = 1; break;
		case XK_Return: case XK_KP_Enter: done = run = 1; break;
		case XK_Down: if (nr) hot = (hot + 1) % nr; changed = 1; break;
		case XK_Up:   if (nr) hot = (hot + nr - 1) % nr; changed = 1; break;
		case XK_Tab:
			if (nr && !res[hot].calc && !res[hot].ai && res[hot].file < 0)
				snprintf(q, sizeof q, "%s", res[hot].app >= 0 ? apps[res[hot].app].name : cmds[res[hot].cmd]);
			hot = 0; changed = 1;
			break;
		case XK_BackSpace:
			if (ev.xkey.state & ControlMask) q[0] = 0;
			else {
				/* 刪掉一個完整的 UTF-8 字元 */
				while (ql && ((unsigned char)q[ql - 1] & 0xc0) == 0x80) q[--ql] = 0;
				if (ql) q[--ql] = 0;
			}
			hot = 0; changed = 1;
			break;
		default:
			if ((ev.xkey.state & ControlMask) && ks == XK_u) { q[0] = 0; changed = 1; break; }
			if (len > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127 && ql + len < sizeof q - 1) {
				strcat(q, buf);
				hot = 0; changed = 1;
			}
		}
	}
	if (xic && im_gen == pm_xim_generation()) XDestroyIC(xic);
	XUngrabPointer(dpy, CurrentTime);
	XUngrabKeyboard(dpy, CurrentTime);
	XDestroyWindow(dpy, w);
	focus(sel);
	if (run && q[0]) {
		/* 有選中項目且沒有輸入參數時執行選中的；否則把整行當指令 */
		char cmd[PATH_MAX + 64];
		if (nr && res[hot].calc) {
			cmd[0] = 0;   /* 計算結果只顯示 */
		} else if (nr && res[hot].ai) {
			/* 問題放進單引號（裡面的單引號寫成 '\''） */
			char qq[1100], *o = qq;
			for (const char *c = q; *c && o < qq + sizeof qq - 5; c++) {
				if (*c == '\'') { memcpy(o, "'\\''", 4); o += 4; } else *o++ = *c;
			}
			*o = 0;
			snprintf(cmd, sizeof cmd, "pomelo-assistant '%s'", qq);
		} else if (nr && res[hot].file >= 0) {
			/* 資料夾用檔案管理員開，檔案用預設程式開；路徑放進單引號 */
			const char *f = files[res[hot].file];
			char qf[PATH_MAX + 32], *o = qf;
			for (const char *c = f; *c && o < qf + sizeof qf - 5; c++) {
				if (*c == '\'') { memcpy(o, "'\\''", 4); o += 4; } else *o++ = *c;
			}
			*o = 0;
			struct stat fst;
			int isdir = !stat(f, &fst) && S_ISDIR(fst.st_mode);
			snprintf(cmd, sizeof cmd, isdir ? "pomelo-files '%s'" : "pomelo-open '%s'", qf);
		} else if (nr && (res[hot].app >= 0 || !strchr(q, ' '))) {
			if (res[hot].app >= 0) {
				App *a = &apps[res[hot].app];
				if (a->term) snprintf(cmd, sizeof cmd, "%s %s", terminal_exec, a->exec);
				else snprintf(cmd, sizeof cmd, "%s", a->exec);
			} else {
				snprintf(cmd, sizeof cmd, "%s", cmds[res[hot].cmd]);
			}
		} else {
			snprintf(cmd, sizeof cmd, "%s", q);
		}
		if (cmd[0]) {
			Arg ra = { .s = cmd };
			spawn(&ra);
		}
	}
	for (int i = 0; i < ncmd; i++)
		free(cmds[i]);
	free(cmds);
	free_apps(apps, napps);
	malloc_trim(0);   /* 把掃描時用掉的記憶體還給系統 */
}

/* ------------------------------------------------------------------ */
/* 視窗總覽（Mission Control）                                         */
/* ------------------------------------------------------------------ */

static Pixmap root_pixmap(void)
{
	Atom type; int fmt; unsigned long n = 0, extra;
	unsigned char *p = NULL;
	Pixmap pm = None;
	if (XGetWindowProperty(dpy, pm_root, atom[XRootPmap], 0, 1, False, XA_PIXMAP, &type, &fmt, &n, &extra, &p) == Success && p) {
		if (n) pm = *(Pixmap *)p;
		XFree(p);
	}
	return pm;
}

static void overview(const Arg *arg)
{
	(void)arg;
	enum { MAXW = 64, TOP = 56, PAD = 36, LABEL = 30 };
	Client *v[MAXW];
	int n = 0;
	for (Client *c = clients; c && n < MAXW; c = c->next)
		if (!c->minimized)
			v[n++] = c;
	if (!n)
		return;
	qsort(v, n, sizeof *v, cmpseq);     /* 固定順序，位置才不會每次跳動 */
	Rect M = mons[pm_pointer_mon()];

	/* 暫時把視窗導向離屏緩衝，才能取得被遮住的內容 */
	if (composite_ok) {
		XCompositeRedirectSubwindows(dpy, pm_root, CompositeRedirectAutomatic);
		for (int i = 0; i < n; i++)
			draw_frame(v[i]);   /* 我們自己的標題列也要補畫 */
		XSync(dpy, False);
		usleep(90000);   /* 給程式一點時間重繪被遮住的部分 */
	}

	/* 版面：接近正方形的格子，最後一列置中 */
	int cols = 1;
	while (cols * cols < n) cols++;
	int rows = (n + cols - 1) / cols;
	double cw = (M.w - 2.0 * PAD) / cols, ch = (M.h - TOP - PAD) / (double)rows;
	struct { double x, y, w, h, r; } cell[MAXW];
	for (int i = 0; i < n; i++) {
		int col = i % cols, row = i / cols;
		int inrow = row == rows - 1 ? n - row * cols : cols;
		double ox = PAD + (cols - inrow) * cw / 2;
		double k = MIN((cw - 40) / fw(v[i]), (ch - LABEL - 30) / fh(v[i]));
		k = MIN(k, 1.0);
		cell[i].w = fw(v[i]) * k;
		cell[i].h = fh(v[i]) * k;
		cell[i].x = ox + col * cw + (cw - cell[i].w) / 2;
		cell[i].y = TOP + row * ch + (ch - LABEL - cell[i].h) / 2;
		cell[i].r = th(v[i]) ? corner_r * k + 1 : 4;
	}

	/* 背景（變暗的桌布 + 縮圖）只畫一次 */
	Pixmap bg = XCreatePixmap(dpy, pm_root, M.w, M.h, DefaultDepth(dpy, scr));
	cairo_surface_t *bs = cairo_xlib_surface_create(dpy, bg, DefaultVisual(dpy, scr), M.w, M.h);
	cairo_t *cr = cairo_create(bs);
	Pixmap rp = root_pixmap();
	if (rp) {
		cairo_surface_t *rs = cairo_xlib_surface_create(dpy, rp, DefaultVisual(dpy, scr), sw, sh);
		cairo_set_source_surface(cr, rs, -M.x, -M.y);
		cairo_paint(cr);
		cairo_surface_destroy(rs);
	} else {
		pm_rgb(cr, 0x3a3a3c);
		cairo_paint(cr);
	}
	pm_rgba(cr, 0x000000, 0.45);
	cairo_paint(cr);
	for (int i = 0; i < n; i++) {
		Client *c = v[i];
		double r = cell[i].r;
		/* 陰影 */
		for (int k = 1; k <= 4; k++) {
			pm_rrect(cr, cell[i].x - k, cell[i].y - k + 3, cell[i].w + 2 * k, cell[i].h + 2 * k, r + k, r + k);
			pm_rgba(cr, 0x000000, 0.06);
			cairo_fill(cr);
		}
		cairo_save(cr);
		pm_rrect(cr, cell[i].x, cell[i].y, cell[i].w, cell[i].h, r, r);
		cairo_clip(cr);
		Pixmap pm = composite_ok ? XCompositeNameWindowPixmap(dpy, c->frame) : None;
		if (pm) {
			cairo_surface_t *ws = cairo_xlib_surface_create(dpy, pm, DefaultVisual(dpy, scr), fw(c), fh(c));
			cairo_translate(cr, cell[i].x, cell[i].y);
			cairo_scale(cr, cell[i].w / fw(c), cell[i].h / fh(c));
			cairo_set_source_surface(cr, ws, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_surface_destroy(ws);
			XFreePixmap(dpy, pm);
		} else {
			/* 沒有 Composite 擴充：畫圖示代替縮圖 */
			pm_rgb(cr, T->title);
			cairo_paint(cr);
			if (c->icon) {
				double s = MIN(MIN(cell[i].w, cell[i].h) * 0.5, ICON_SZ);
				cairo_translate(cr, cell[i].x + (cell[i].w - s) / 2, cell[i].y + (cell[i].h - s) / 2);
				cairo_scale(cr, s / (ICON_SZ), s / (ICON_SZ));
				cairo_set_source_surface(cr, c->icon, 0, 0);
				cairo_paint(cr);
			}
		}
		cairo_restore(cr);
		char buf[300];
		pm_font(cr, pm_size, 0);
		pm_ellipsize(cr, c->title[0] ? c->title : c->cls, cw - 20, buf, sizeof buf);
		double tw = pm_textw(cr, buf);
		double lx = cell[i].x + (cell[i].w - tw) / 2, ly = cell[i].y + cell[i].h + 18;
		pm_rrect(cr, lx - 9, ly - 11, tw + 18, 22, 11, 11);
		pm_rgba(cr, 0x000000, 0.35);
		cairo_fill(cr);
		pm_rgb(cr, 0xffffff);
		pm_text(cr, lx, ly, buf);
	}
	cairo_destroy(cr);
	cairo_surface_flush(bs);
	cairo_surface_destroy(bs);

	XSetWindowAttributes swa = { .override_redirect = True, .background_pixmap = bg,
	                             .event_mask = ExposureMask | KeyPressMask };
	Window w = XCreateWindow(dpy, pm_root, M.x, M.y, M.w, M.h, 0, CopyFromParent, InputOutput, CopyFromParent,
	                         CWOverrideRedirect | CWBackPixmap | CWEventMask, &swa);
	XMapRaised(dpy, w);
	XGrabPointer(dpy, pm_root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync, None, cur_normal, CurrentTime);
	for (int i = 0; i < 200; i++) {
		if (XGrabKeyboard(dpy, pm_root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
		usleep(1000);
	}

	int hot = -1, chosen = -1, done = 0, redraw = 1;
	for (int i = 0; i < n; i++)
		if (v[i] == sel) hot = i;
	XEvent ev;
	while (!done) {
		if (redraw) {
			Paint p;
			cairo_t *c2 = pm_paint_begin(&p, w, M.w, M.h);
			cairo_surface_t *src = cairo_xlib_surface_create(dpy, bg, DefaultVisual(dpy, scr), M.w, M.h);
			cairo_set_source_surface(c2, src, 0, 0);
			cairo_paint(c2);
			cairo_surface_destroy(src);
			if (hot >= 0) {
				double r = cell[hot].r;
				pm_rrect(c2, cell[hot].x - 3, cell[hot].y - 3, cell[hot].w + 6, cell[hot].h + 6, r + 3, r + 3);
				pm_rgb(c2, T->sel);
				cairo_set_line_width(c2, 4);
				cairo_stroke(c2);
			}
			pm_paint_end(&p, c2, 0, 0);
			redraw = 0;
		}
		if (!pm_modal_next(&ev)) {
			if (ev.type == Expose && ev.xexpose.window == w)
				redraw = 1;
			continue;
		}
		if (ev.type == MotionNotify || ev.type == ButtonPress) {
			int px = (ev.type == MotionNotify ? ev.xmotion.x_root : ev.xbutton.x_root) - M.x;
			int py = (ev.type == MotionNotify ? ev.xmotion.y_root : ev.xbutton.y_root) - M.y;
			int idx = -1;
			for (int i = 0; i < n; i++)
				if (px >= cell[i].x && px < cell[i].x + cell[i].w && py >= cell[i].y && py < cell[i].y + cell[i].h + LABEL)
					idx = i;
			if (ev.type == ButtonPress) { chosen = idx; done = 1; }
			else if (idx >= 0 && idx != hot) { hot = idx; redraw = 1; }
		} else if (ev.type == KeyPress) {
			KeySym ks = XLookupKeysym(&ev.xkey, 0);
			if (ks == XK_Escape) done = 1;
			else if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_space) { chosen = hot; done = 1; }
			else if (ks == XK_Right || ks == XK_Tab) { hot = (hot + 1) % n; redraw = 1; }
			else if (ks == XK_Left) { hot = (hot + n - 1) % n; redraw = 1; }
			else if (ks == XK_Down) { hot = MIN(n - 1, (hot < 0 ? 0 : hot) + cols); redraw = 1; }
			else if (ks == XK_Up) { hot = MAX(0, (hot < 0 ? 0 : hot) - cols); redraw = 1; }
		}
	}
	XUngrabKeyboard(dpy, CurrentTime);
	XUngrabPointer(dpy, CurrentTime);
	XDestroyWindow(dpy, w);
	XFreePixmap(dpy, bg);
	if (composite_ok)
		XCompositeUnredirectSubwindows(dpy, pm_root, CompositeRedirectAutomatic);
	if (chosen >= 0 && alive(v[chosen]))
		activate(v[chosen]);
	else
		focus(sel);
}

/* ------------------------------------------------------------------ */
/* 動作                                                                */
/* ------------------------------------------------------------------ */

static void close_client(Client *c)
{
	if (has_proto(c, atom[WMDelete]))
		send_msg(c->win, atom[WMProtocols], atom[WMDelete], CurrentTime);
	else
		XKillClient(dpy, c->win);
}

static void killclient(const Arg *a) { (void)a; if (sel) close_client(sel); }

static void minimize(const Arg *a) { (void)a; if (sel) set_minimized(sel, 1); }
static void zoom(const Arg *a)     { (void)a; if (sel) set_zoom(sel, !sel->zoomed); }
static void unzoom(const Arg *a)   { (void)a; if (sel) set_zoom(sel, 0); }
static void togglefs(const Arg *a) { (void)a; if (sel) set_fullscreen(sel, !sel->fullscreen); }

static void tile(const Arg *a)
{
	Client *c = sel;
	if (!c || c->fullscreen) return;
	if (!c->zoomed) save_geom(c);
	c->zoomed = 1;
	Rect r = workarea(mon_of(c));
	int half = r.w / 2;
	c->x = a->i < 0 ? r.x : r.x + half;
	c->y = r.y;
	c->w = (a->i < 0 ? half : r.w - half) - 2 * bw(c);
	c->h = r.h - th(c) - bw(c);
	apply_geom(c);
	set_netstate(c);
}

static void cycle(const Arg *a)
{
	(void)a;
	Client *last = NULL;
	for (Client *c = clients; c; c = c->next)
		if (!c->minimized && c != sel)
			last = c;
	if (last)
		focus(last);
}

static void restart(const Arg *a) { (void)a; do_restart = 1; running = 0; }

static void quit(const Arg *a)    { (void)a; running = 0; }

/* ------------------------------------------------------------------ */
/* 事件處理                                                            */
/* ------------------------------------------------------------------ */

static void on_buttonpress(XButtonEvent *e)
{
	if (e->window == pm_root) {   /* 點在桌面（桌布）上 */
		if (e->button == Button3) pm_desktop_menu(e->x_root, e->y_root);
		return;
	}
	Client *c = find(e->window);
	if (!c) return;

	if (e->window == c->win) {
		/* 點在內容區域：聚焦，並把點擊交還給程式 */
		unsigned int mod = CLEANMASK(e->state);
		if (mod == MODKEY && (e->button == Button1 || e->button == Button3)) {
			focus(c);
			if (e->button == Button1) move_mouse(c, e->x_root, e->y_root);
			else resize_mouse(c, EDGE_R | EDGE_B, e->x_root, e->y_root);
			XAllowEvents(dpy, AsyncPointer, CurrentTime);
			return;
		}
		focus(c);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		return;
	}
	if (e->window == c->grip) {
		focus(c);
		resize_mouse(c, grip_edges(c, e->x_root, e->y_root), e->x_root, e->y_root);
		return;
	}
	/* 標題列（內容區域冒泡上來的點擊不處理） */
	if (e->y >= th(c))
		return;
	if (e->button != Button1) {
		if (e->button == Button3) { focus(c); pm_popup(app_menu, LEN(app_menu), e->x_root, e->y_root); }
		return;
	}
	int b = btn_at(c, e->x, e->y);
	if (b >= 0) {
		c->pressed = b + 1;
		draw_frame(c);
		return;
	}
	focus(c);
	if (last_click_win == c->frame && e->time - last_click < 400) {
		last_click = 0;
		set_zoom(c, !c->zoomed);
		return;
	}
	last_click = e->time;
	last_click_win = c->frame;
	move_mouse(c, e->x_root, e->y_root);
}

static void on_buttonrelease(XButtonEvent *e)
{
	Client *c = find(e->window);
	if (!c || e->window != c->frame || !c->pressed)
		return;
	int b = c->pressed - 1;
	c->pressed = 0;
	draw_frame(c);
	if (btn_at(c, e->x, e->y) != b)
		return;
	if (b == 0) close_client(c);
	else if (b == 1) set_minimized(c, 1);
	else set_zoom(c, !c->zoomed);
}

static void on_motion(XMotionEvent *e)
{
	Client *c = find(e->window);
	if (!c) return;
	if (e->window == c->grip) {
		static Window lastw;
		static Cursor lastc;
		Cursor cur = cur_edge[grip_edges(c, e->x_root, e->y_root)];
		if (cur != lastc || c->grip != lastw) { XDefineCursor(dpy, c->grip, cur); lastc = cur; lastw = c->grip; }
		return;
	}
	if (e->window == c->frame) {
		int hover = e->y < title_h && (btns_right ? e->x > fw(c) - BTN_ZONE : e->x < BTN_ZONE);
		if (hover != c->hover) { c->hover = hover; draw_frame(c); }
	}
}

static void on_leave(XCrossingEvent *e)
{
	Client *c = find(e->window);
	if (c && e->window == c->frame && c->hover) {
		c->hover = 0;
		draw_frame(c);
	}
}

static void on_configurerequest(XConfigureRequestEvent *e)
{
	Client *c = find(e->window);
	if (c && e->window == c->win) {
		if (!c->fullscreen) {
			if (e->value_mask & CWWidth)  c->w = e->width;
			if (e->value_mask & CWHeight) c->h = e->height;
			if (e->value_mask & CWX) c->x = e->x - bw(c);
			if (e->value_mask & CWY) c->y = MAX(e->y - th(c), workarea(pm_mon_at(e->x, e->y)).y);
			if (e->value_mask & (CWWidth | CWHeight | CWX | CWY))
				c->zoomed = 0;
		}
		apply_geom(c);
		return;
	}
	XWindowChanges wc = {
		.x = e->x, .y = e->y, .width = e->width, .height = e->height,
		.border_width = e->border_width, .sibling = e->above, .stack_mode = e->detail,
	};
	XConfigureWindow(dpy, e->window, e->value_mask, &wc);
}

static void on_clientmessage(XClientMessageEvent *e)
{
	if (e->message_type == atom[PomeloAction]) {
		static void (*const acts[])(const Arg *) = { NULL, spotlight, overview, restart, quit, cycle };
		long a = e->data.l[0];
		if (a > 0 && a < (long)LEN(acts))
			acts[a](&(Arg){0});
		return;
	}
	Client *c = find(e->window);
	if (!c || e->window != c->win)
		return;
	if (e->message_type == atom[NetWMState]) {
		Atom a1 = (Atom)e->data.l[1], a2 = (Atom)e->data.l[2];
		if (a1 == atom[NetWMStateFS] || a2 == atom[NetWMStateFS])
			set_fullscreen(c, e->data.l[0] == 1 || (e->data.l[0] == 2 && !c->fullscreen));
		else if (a1 == atom[NetWMStateMaxV] || a1 == atom[NetWMStateMaxH] ||
		         a2 == atom[NetWMStateMaxV] || a2 == atom[NetWMStateMaxH])
			set_zoom(c, e->data.l[0] == 1 || (e->data.l[0] == 2 && !c->zoomed));
	} else if (e->message_type == atom[NetActiveWindow]) {
		activate(c);
	} else if (e->message_type == atom[NetCloseWindow]) {
		close_client(c);
	} else if (e->message_type == atom[WMChangeState] && e->data.l[0] == IconicState) {
		set_minimized(c, 1);
	} else if (e->message_type == atom[NetWMMoveResize]) {
		/* 自繪標題列的程式(GTK 等)用這個請求移動 / 縮放 */
		static const int dir[] = { EDGE_L | EDGE_T, EDGE_T, EDGE_R | EDGE_T, EDGE_R,
		                           EDGE_R | EDGE_B, EDGE_B, EDGE_L | EDGE_B, EDGE_L };
		long d = e->data.l[2];
		Window r, ch; int rx, ry, wx, wy; unsigned int mask;
		XQueryPointer(dpy, pm_root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
		XUngrabPointer(dpy, CurrentTime);
		focus(c);
		if (d == 8) move_mouse(c, rx, ry);
		else if (d >= 0 && d < 8) resize_mouse(c, dir[d], rx, ry);
	}
}

static void on_property(XPropertyEvent *e)
{
	Dock *d = find_dock(e->window);
	if (d && (e->atom == atom[NetWMStrutPartial] || e->atom == atom[NetWMStrut])) {
		read_strut(d);
		workarea_changed();
		return;
	}
	Client *c = find(e->window);
	if (!c || e->window != c->win || e->state == PropertyDelete)
		return;
	if (e->atom == XA_WM_NAME || e->atom == atom[NetWMName]) {
		update_title(c);
		draw_frame(c);
	} else if (e->atom == atom[NetWMIcon]) {
		update_icon(c);
	} else if (e->atom == XA_WM_NORMAL_HINTS || e->atom == XA_WM_HINTS) {
		update_hints(c);
	}
}

static void on_key(XKeyEvent *e)
{
	KeySym ks = XLookupKeysym(e, 0);
	for (unsigned i = 0; i < LEN(keys); i++)
		if (ks == keys[i].sym && CLEANMASK(keys[i].mod) == CLEANMASK(e->state))
			keys[i].fn(&keys[i].arg);
}

static void draw_wallpaper(void);

/* ------------------------------------------------------------------ */
/* 視窗陰影與動畫：交給 picom（設定 appearance.compositor = 1）          */
/* ------------------------------------------------------------------ */

static int in_path(const char *cmd)
{
	const char *path = getenv("PATH");
	char buf[4096], full[PATH_MAX];
	snprintf(buf, sizeof buf, "%s", path ? path : "/usr/bin:/bin");
	char *save = NULL;
	for (char *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		snprintf(full, sizeof full, "%s/%s", d, cmd);
		if (!access(full, X_OK)) return 1;
	}
	return 0;
}

/* pid 是不是還在跑的 picom */
static int is_picom(long pid)
{
	char p[64], c[32] = "";
	snprintf(p, sizeof p, "/proc/%ld/comm", pid);
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
	fclose(f);
	return !strcmp(c, "picom");
}

/* 設定開著、有 picom、還沒有別的合成器 → 啟動；關掉 → 只停我們啟動的那個。
 * 啟動的 picom 記在根視窗的 _POMELO_COMPOSITOR，視窗管理器重新啟動後還認得 */
static void compositor_update(void)
{
	static Atom a_cm, a_pid;
	if (!a_cm) {
		char nm[32];
		snprintf(nm, sizeof nm, "_NET_WM_CM_S%d", scr);
		a_cm = XInternAtom(dpy, nm, False);
		a_pid = XInternAtom(dpy, "_POMELO_COMPOSITOR", False);
	}
	int want = pm_conf_int("appearance.compositor", 0) && in_path("picom");
	long pid = 0;
	Atom type; int fmt; unsigned long n = 0, after;
	unsigned char *d = NULL;
	if (XGetWindowProperty(dpy, pm_root, a_pid, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &d) == Success && d) {
		if (n == 1) pid = *(long *)d;
		XFree(d);
	}
	int ours = pid > 0 && is_picom(pid);
	if (pid && !ours) XDeleteProperty(dpy, pm_root, a_pid);
	if (want && !ours && XGetSelectionOwner(dpy, a_cm) == None) {
		/* 使用者自己的設定優先，再來是 Pomelo 附的 */
		char conf[PATH_MAX] = "";
		const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
		if (cfg && *cfg) snprintf(conf, sizeof conf, "%s/pomelo/picom.conf", cfg);
		else snprintf(conf, sizeof conf, "%s/.config/pomelo/picom.conf", home ? home : "/");
		if (access(conf, R_OK)) snprintf(conf, sizeof conf, "/usr/share/pomelo/picom.conf");
		if (access(conf, R_OK)) snprintf(conf, sizeof conf, "/usr/local/share/pomelo/picom.conf");
		if (access(conf, R_OK)) conf[0] = 0;
		pid_t p = fork();
		if (p == 0) {
			close(ConnectionNumber(dpy));
			setsid();
			signal(SIGCHLD, SIG_DFL);
			if (conf[0]) execlp("picom", "picom", "--config", conf, (char *)NULL);
			else execlp("picom", "picom", (char *)NULL);
			_exit(127);
		}
		if (p > 0) {
			long v = p;
			XChangeProperty(dpy, pm_root, a_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&v, 1);
		}
	} else if (!want && ours) {
		kill((pid_t)pid, SIGTERM);
		XDeleteProperty(dpy, pm_root, a_pid);
	}
}

/* 設定程式改了設定：即時套用主題、字型、桌布 */
static void apply_settings(void)
{
	pm_size = pm_conf_num("appearance.font_size", font_size);
	pm_family = pm_conf_get("appearance.font", font_family);
	pm_icon_theme = pm_conf_get("appearance.icon_theme", icon_theme);
	pm_set_theme(pm_conf_int("appearance.dark", dark_mode), pm_conf_color("appearance.accent", 0));
	btns_right = pm_conf_int("appearance.buttons_right", 1);
	for (Client *c = clients; c; c = c->next) {
		XSetWindowBackground(dpy, c->frame, T->border);
		XClearWindow(dpy, c->frame);
		draw_frame(c);
	}
	draw_wallpaper();
	compositor_update();
}

static void handle(XEvent *ev)
{
	switch (ev->type) {
	case MapRequest: {
		XWindowAttributes wa;
		Client *c = find(ev->xmaprequest.window);
		if (c) { activate(c); break; }
		if (XGetWindowAttributes(dpy, ev->xmaprequest.window, &wa) && !wa.override_redirect)
			manage(ev->xmaprequest.window, &wa);
		break;
	}
	case UnmapNotify: {
		if (find_dock(ev->xunmap.window)) { dock_remove(ev->xunmap.window); break; }
		Client *c = find(ev->xunmap.window);
		if (!c || ev->xunmap.window != c->win) break;
		if (c->ignore_unmap) c->ignore_unmap--;
		else unmanage(c, 0);
		break;
	}
	case DestroyNotify: {
		if (find_dock(ev->xdestroywindow.window)) { dock_remove(ev->xdestroywindow.window); break; }
		Client *c = find(ev->xdestroywindow.window);
		if (c && ev->xdestroywindow.window == c->win)
			unmanage(c, 1);
		break;
	}
	case ConfigureRequest: on_configurerequest(&ev->xconfigurerequest); break;
	case ClientMessage:    on_clientmessage(&ev->xclient); break;
	case PropertyNotify:
		if (pm_conf_changed(dpy, ev)) apply_settings();
		else on_property(&ev->xproperty);
		break;
	case ButtonPress:      on_buttonpress(&ev->xbutton); break;
	case ButtonRelease:    on_buttonrelease(&ev->xbutton); break;
	case MotionNotify:     on_motion(&ev->xmotion); break;
	case LeaveNotify:      on_leave(&ev->xcrossing); break;
	case KeyPress:         on_key(&ev->xkey); break;
	case MappingNotify:
		XRefreshKeyboardMapping(&ev->xmapping);
		break;
	case Expose:
		if (ev->xexpose.count) break;
		{ Client *c = find(ev->xexpose.window); if (c) draw_frame(c); }
		break;
	default:
		if (ev->type == rr_base + RRScreenChangeNotify) {
			XRRUpdateConfiguration(ev);
			sw = DisplayWidth(dpy, scr);
			sh = DisplayHeight(dpy, scr);
			pm_update_monitors();
			update_workarea();
			draw_wallpaper();
			for (Client *c = clients; c; c = c->next) {
				if (c->fullscreen) {
					Rect m = mons[mon_of(c)];
					c->x = m.x; c->y = m.y; c->w = m.w; c->h = m.h;
					apply_geom(c);
				} else if (c->zoomed) {
					Rect r = workarea(mon_of(c));
					c->x = r.x; c->y = r.y; c->w = r.w - 2 * bw(c); c->h = r.h - th(c) - bw(c);
					apply_geom(c);
				} else if (!reachable(c)) {
					rescue(c);   /* 原本在拔掉的螢幕上 */
				}
			}
			outputs_check(0);
		} else if (ev->type == rr_base + RRNotify && ((XRRNotifyEvent *)ev)->subtype == RRNotify_OutputChange) {
			outputs_check(0);
		}
	}
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

/* 桌布：desktop.wallpaper = gradient（內建漸層）/ none（不處理，交給 feh 等）/ 圖片路徑 */
static void draw_wallpaper(void)
{
	const char *wp = pm_conf_get("desktop.wallpaper", wallpaper ? "gradient" : "none");
	int mode = pm_conf_int("desktop.wallpaper_mode", 0);
	if (!strcmp(wp, "none"))
		return;
	if (wallpm) XFreePixmap(dpy, wallpm);
	wallpm = XCreatePixmap(dpy, pm_root, sw, sh, DefaultDepth(dpy, scr));
	cairo_surface_t *s = cairo_xlib_surface_create(dpy, wallpm, DefaultVisual(dpy, scr), sw, sh);
	cairo_t *cr = cairo_create(s);
	pm_rgb(cr, 0x000000);
	cairo_paint(cr);
	for (int i = 0; i < nmons; i++) {
		Rect m = mons[i];
		cairo_save(cr);
		cairo_rectangle(cr, m.x, m.y, m.w, m.h);
		cairo_clip(cr);
		cairo_translate(cr, m.x, m.y);
		cairo_surface_t *img = strcmp(wp, "gradient") ? pm_wallpaper_load(wp, m.w, m.h, mode) : NULL;
		if (img) {
			cairo_set_source_surface(cr, img, 0, 0);
			cairo_paint(cr);
			cairo_surface_destroy(img);
			cairo_restore(cr);
			continue;
		}
		cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, m.w, m.h);
		if (pm_dark) {
			cairo_pattern_add_color_stop_rgb(g, 0.0, 0.07, 0.09, 0.20);
			cairo_pattern_add_color_stop_rgb(g, 0.5, 0.20, 0.13, 0.32);
			cairo_pattern_add_color_stop_rgb(g, 1.0, 0.42, 0.20, 0.26);
		} else {
			cairo_pattern_add_color_stop_rgb(g, 0.0, 0.20, 0.36, 0.66);
			cairo_pattern_add_color_stop_rgb(g, 0.5, 0.52, 0.42, 0.74);
			cairo_pattern_add_color_stop_rgb(g, 1.0, 0.98, 0.62, 0.48);
		}
		cairo_set_source(cr, g);
		cairo_paint(cr);
		cairo_pattern_destroy(g);
		/* 柔和的光暈 */
		cairo_pattern_t *r = cairo_pattern_create_radial(m.w * 0.7, m.h * 0.35, 0, m.w * 0.7, m.h * 0.35, m.w * 0.55);
		cairo_pattern_add_color_stop_rgba(r, 0, 1, 1, 1, pm_dark ? 0.10 : 0.22);
		cairo_pattern_add_color_stop_rgba(r, 1, 1, 1, 1, 0);
		cairo_set_source(cr, r);
		cairo_paint(cr);
		cairo_pattern_destroy(r);
		cairo_restore(cr);
	}
	cairo_destroy(cr);
	cairo_surface_flush(s);
	cairo_surface_destroy(s);
	XSetWindowBackgroundPixmap(dpy, pm_root, wallpm);
	XClearWindow(dpy, pm_root);
	XChangeProperty(dpy, pm_root, atom[XRootPmap], XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&wallpm, 1);
	XChangeProperty(dpy, pm_root, atom[ESetRoot], XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&wallpm, 1);
}

static void grab_keys(void)
{
	XModifierKeymap *mm = XGetModifierMapping(dpy);
	numlockmask = 0;
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < mm->max_keypermod; j++)
			if (mm->modifiermap[i * mm->max_keypermod + j] == XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = 1u << i;
	XFreeModifiermap(mm);
	unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask | LockMask };
	XUngrabKey(dpy, AnyKey, AnyModifier, pm_root);
	for (unsigned i = 0; i < LEN(keys); i++) {
		KeyCode kc = XKeysymToKeycode(dpy, keys[i].sym);
		if (!kc) continue;
		for (unsigned j = 0; j < LEN(mods); j++)
			XGrabKey(dpy, kc, keys[i].mod | mods[j], pm_root, True, GrabModeAsync, GrabModeAsync);
	}
}

static void setup(void)
{
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", font_family), pm_conf_num("appearance.font_size", font_size),
	        pm_conf_int("appearance.dark", dark_mode), pm_conf_get("appearance.icon_theme", icon_theme));
	pm_set_theme(pm_dark, pm_conf_color("appearance.accent", 0));
	btns_right = pm_conf_int("appearance.buttons_right", 1);
	pm_dispatch = handle;
	int ev, er;
	if (rr_base >= 0)
		XRRSelectInput(dpy, pm_root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask);
	int cmaj = 0, cmin = 4;
	composite_ok = XCompositeQueryExtension(dpy, &ev, &er) && XCompositeQueryVersion(dpy, &cmaj, &cmin) &&
	               (cmaj > 0 || cmin >= 2);
	pm_update_monitors();

	XInternAtoms(dpy, (char **)atomnames, AtomLast, False, atom);

	cur_normal = pm_cursor;
	cur_move = XCreateFontCursor(dpy, XC_fleur);
	cur_edge[EDGE_L] = XCreateFontCursor(dpy, XC_left_side);
	cur_edge[EDGE_R] = XCreateFontCursor(dpy, XC_right_side);
	cur_edge[EDGE_T] = XCreateFontCursor(dpy, XC_top_side);
	cur_edge[EDGE_B] = XCreateFontCursor(dpy, XC_bottom_side);
	cur_edge[EDGE_L | EDGE_T] = XCreateFontCursor(dpy, XC_top_left_corner);
	cur_edge[EDGE_R | EDGE_T] = XCreateFontCursor(dpy, XC_top_right_corner);
	cur_edge[EDGE_L | EDGE_B] = XCreateFontCursor(dpy, XC_bottom_left_corner);
	cur_edge[EDGE_R | EDGE_B] = XCreateFontCursor(dpy, XC_bottom_right_corner);

	/* 先要求成為視窗管理器（若已有其他 WM 會立刻失敗） */
	XSetErrorHandler(xerror_start);
	XSetWindowAttributes swa = { .cursor = cur_normal,   /* ButtonPress：桌面按右鍵的選單 */
		.event_mask = SubstructureRedirectMask | SubstructureNotifyMask | PropertyChangeMask | ButtonPressMask };
	XChangeWindowAttributes(dpy, pm_root, CWCursor | CWEventMask, &swa);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	pm_conf_watch(dpy);

	/* EWMH */
	checkwin = XCreateSimpleWindow(dpy, pm_root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, checkwin, atom[NetWMCheck], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&checkwin, 1);
	XChangeProperty(dpy, checkwin, atom[NetWMName], atom[Utf8String], 8, PropModeReplace, (unsigned char *)"Pomelo", 6);
	XChangeProperty(dpy, pm_root, atom[NetSupported], XA_ATOM, 32, PropModeReplace,
	                (unsigned char *)&atom[NetSupported], PomeloAction - NetSupported);
	XDeleteProperty(dpy, pm_root, atom[NetClientList]);
	update_workarea();
	draw_wallpaper();
	grab_keys();

	/* 接管已存在的視窗 */
	Window r, p, *wins = NULL;
	unsigned int n;
	if (XQueryTree(dpy, pm_root, &r, &p, &wins, &n)) {
		for (unsigned i = 0; i < n; i++) {
			XWindowAttributes wa;
			if (wins[i] == checkwin)
				continue;
			if (!XGetWindowAttributes(dpy, wins[i], &wa) || wa.override_redirect || wa.map_state != IsViewable)
				continue;
			manage(wins[i], &wa);
		}
		if (wins) XFree(wins);
	}
	/* 最後才設定 check 視窗：pomelo-session 看到它就知道 WM 已就緒 */
	XChangeProperty(dpy, pm_root, atom[NetWMCheck], XA_WINDOW, 32, PropModeReplace, (unsigned char *)&checkwin, 1);
	focus(NULL);
}

static void cleanup(void)
{
	/* 把所有視窗交還給根視窗，讓重新啟動或其他 WM 能接手 */
	while (clients) {
		Client *c = clients;
		if (c->minimized) XMapWindow(dpy, c->win);
		if (c->fullscreen) { c->fullscreen = 0; restore_geom(c); }
		unmanage(c, 0);
	}
	XDeleteProperty(dpy, pm_root, atom[NetWMCheck]);
	XDestroyWindow(dpy, checkwin);
	XDeleteProperty(dpy, pm_root, atom[NetActiveWindow]);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XSync(dpy, False);
}

/* 收到 SIGTERM（登出）、SIGHUP（重新啟動）：XNextEvent 會自己重試被打斷的 read，
 * 所以要透過 pipe 叫醒主迴圈的 select，不然要等到下一個 X 事件才會結束 */
static int sig_pipe[2] = { -1, -1 };
static void on_signal(int s)
{
	int e = errno;
	if (s == SIGHUP) do_restart = 1;
	running = 0;
	if (sig_pipe[1] >= 0 && write(sig_pipe[1], "x", 1) < 0) { /* 滿了也沒關係 */ }
	errno = e;
}

int main(int argc, char *argv[])
{
	g_argv = argv;
	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("pomelo " POMELO_VERSION);
		return 0;
	}
	setlocale(LC_ALL, "");
	if (!(dpy = XOpenDisplay(NULL))) {
		fputs(_("pomelo: 無法開啟 X display\n"), stderr);
		return 2;
	}
	if (pipe2(sig_pipe, O_CLOEXEC | O_NONBLOCK)) sig_pipe[0] = sig_pipe[1] = -1;
	/* 子行程自動回收，不留殭屍 */
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT | SA_RESTART };
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = on_signal;
	sa.sa_flags = 0;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	setup();
	compositor_update();   /* 視窗陰影與動畫（設定開著才會啟動 picom） */
	if (rr_base >= 0) outputs_check(1);

	XEvent ev;
	int xfd = ConnectionNumber(dpy);
	while (running) {
		if (!XPending(dpy)) {
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(xfd, &fds);
			if (sig_pipe[0] >= 0) FD_SET(sig_pipe[0], &fds);
			long now = pm_now_ms();
			if (hotplug_at && now >= hotplug_at) { hotplug_run(); continue; }
			struct timeval tv = { 0, 0 };
			if (hotplug_at) { long w = hotplug_at - now; tv.tv_sec = w / 1000; tv.tv_usec = (w % 1000) * 1000; }
			if (select(MAX(xfd, sig_pipe[0]) + 1, &fds, NULL, NULL, hotplug_at ? &tv : NULL) < 0 && errno != EINTR) break;
			continue;
		}
		XNextEvent(dpy, &ev);
		if (!XFilterEvent(&ev, None))
			handle(&ev);
	}
	cleanup();
	XCloseDisplay(dpy);
	if (do_restart) {
		execvp(g_argv[0], g_argv);
		perror("pomelo: restart");
	}
	return 0;
}
