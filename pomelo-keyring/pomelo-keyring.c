/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-keyring — Pomelo 鑰匙圈
 *
 * 幫 Chrome、VS Code 這類程式保管密碼（實作標準的 Secret Service，程式不用改就能用），
 * 取代 gnome-keyring，不需要任何 GNOME 元件。
 *
 *   pomelo-keyring --daemon     鑰匙圈服務（pomelo-session 登入時啟動）
 *   pomelo-keyring --stash      （PAM 模組用）從標準輸入收下登入密碼，等服務來拿
 *   pomelo-keyring --lock       鎖定
 *   pomelo-keyring --settings   設定與儲存的項目（預設）
 *
 * 登入流程：PAM 模組（pam_pomelo_keyring.so）在驗證密碼時記下密碼，開啟工作階段時以使用者身分
 * 啟動 --stash 把密碼交給它暫存；之後 pomelo-session 啟動 --daemon，服務從暫存程式取回密碼、
 * 自動解鎖鑰匙圈。用 startx 進桌面（沒經過 PAM）或改過登入密碼時，第一次要用密碼時會跳出解鎖視窗。
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <pomelo-wl.h>
#include <dbus/dbus.h>
#include "store.h"
#include "service.h"
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <X11/Xatom.h>

#define BUS_NAME "org.freedesktop.secrets"
#define SVC "/org/freedesktop/secrets"
#define IF_PRIV "org.pomelo.Keyring"

static char store_path[PATH_MAX], stash_path[PATH_MAX];

static void runtime_dir(char *out, size_t n)
{
	const char *r = getenv("XDG_RUNTIME_DIR");
	struct stat st;
	if (r && *r && !stat(r, &st) && S_ISDIR(st.st_mode)) { snprintf(out, n, "%s", r); return; }
	snprintf(out, n, "/run/user/%d", (int)getuid());
	if (!stat(out, &st) && S_ISDIR(st.st_mode) && st.st_uid == getuid()) return;
	snprintf(out, n, "/tmp/pomelo-keyring-%d", (int)getuid());
	mkdir(out, 0700);
}

static void init_paths(void)
{
	const char *d = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (!h || !*h) { struct passwd *pw = getpwuid(getuid()); h = pw ? pw->pw_dir : "/tmp"; }
	if (d && *d) snprintf(store_path, sizeof store_path, "%s/pomelo/keyring/login.keyring", d);
	else snprintf(store_path, sizeof store_path, "%s/.local/share/pomelo/keyring/login.keyring", h);
	char rd[PATH_MAX];
	runtime_dir(rd, sizeof rd);
	snprintf(stash_path, sizeof stash_path, "%s/pomelo-keyring-stash", rd);
	/* UNIX socket 的路徑最多 107 個字元；太長就改放 /tmp（跟 PAM 模組算出來的一樣） */
	struct sockaddr_un probe;
	if (strlen(stash_path) >= sizeof probe.sun_path) {
		snprintf(rd, sizeof rd, "/tmp/pomelo-keyring-%d", (int)getuid());
		mkdir(rd, 0700);
		snprintf(stash_path, sizeof stash_path, "%s/pomelo-keyring-stash", rd);
	}
}

static int peer_is_me(int fd)
{
	struct ucred cr;
	socklen_t len = sizeof cr;
	return !getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) && cr.uid == getuid();
}

/* ------------------------------------------------------------------ */
/* --stash：暫存登入密碼，等服務來拿（最多 15 分鐘）                      */
/* ------------------------------------------------------------------ */

static int stash(void)
{
	static char pw[512];
	mlock(pw, sizeof pw);
	size_t n = 0;
	ssize_t r;
	while (n < sizeof pw - 1 && (r = read(0, pw + n, sizeof pw - 1 - n)) > 0) n += (size_t)r;
	pw[n] = 0;
	if (n && pw[n - 1] == '\n') pw[--n] = 0;
	if (!n) return 0;
	pid_t pid = fork();
	if (pid < 0) return 1;
	if (pid > 0) return 0;   /* PAM 那邊不用等 */
	setsid();
	int nul = open("/dev/null", O_RDWR);
	if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", stash_path);
	unlink(stash_path);
	umask(077);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 1)) { kc_wipe(pw, sizeof pw); return 1; }
	struct pollfd p = { fd, POLLIN, 0 };
	if (poll(&p, 1, 15 * 60 * 1000) > 0) {
		int c = accept(fd, NULL, NULL);
		if (c >= 0) {
			if (peer_is_me(c)) { ssize_t w = write(c, pw, n); (void)w; }
			close(c);
		}
	}
	unlink(stash_path);
	kc_wipe(pw, sizeof pw);
	return 0;
}

/* 服務啟動時：向暫存程式拿登入密碼（拿不到回傳 NULL） */
static char *fetch_stash(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return NULL;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", stash_path);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) || !peer_is_me(fd)) { close(fd); return NULL; }
	char *pw = malloc(512);
	mlock(pw, 512);
	size_t n = 0;
	ssize_t r;
	struct pollfd p = { fd, POLLIN, 0 };
	while (n < 511 && poll(&p, 1, 3000) > 0 && (r = read(fd, pw + n, 511 - n)) > 0) n += (size_t)r;
	close(fd);
	pw[n] = 0;
	if (!n) { free(pw); return NULL; }
	return pw;
}

/* ------------------------------------------------------------------ */
/* 解鎖／建立鑰匙圈的視窗                                               */
/* ------------------------------------------------------------------ */

#define W 440

static Window win;
static PmWl *wl;              /* 原生 Wayland 的提示窗（layer-shell overlay、獨佔鍵盤；X 後端用 win） */
static int native, ui_inited;
static int win_h = 230, create_mode, field;
static unsigned cur_prompt;
static char app_name[64], pw1[256], pw2[256], errmsg[200];
static Atom a_delete;
static double btn_x[2], btn_w[2], btn_y, btn_h = 30, fld_y[2];
static long shake_until;

static double wrap(cairo_t *cr, const char *text, double x, double y, double maxw)
{
	char line[512];
	const char *p = text;
	while (*p) {
		size_t l = strlen(p), cut = 0, k = 0;
		while (k < l) {
			size_t c = 1;
			while (k + c < l && ((unsigned char)p[k + c] & 0xc0) == 0x80) c++;
			snprintf(line, sizeof line, "%.*s", (int)(k + c), p);
			if (k && pm_textw(cr, line) > maxw) break;
			k += c;
			cut = k;
		}
		if (!cut) cut = l;
		snprintf(line, sizeof line, "%.*s", (int)cut, p);
		pm_text(cr, x, y + 9, line);
		y += 19;
		p += cut;
	}
	return y;
}

static void draw_field(cairo_t *cr, double x, double y, double w, const char *buf, int active, const char *hint)
{
	pm_rrect(cr, x, y, w, 32, 7, 7);
	pm_rgb(cr, pm_dark ? 0x1c1c1e : 0xffffff);
	cairo_fill_preserve(cr);
	pm_rgb(cr, active ? T->sel : (pm_dark ? 0x48484a : 0xd1d1d6));
	cairo_set_line_width(cr, active ? 2 : 1);
	cairo_stroke(cr);
	size_t n = 0;
	for (const char *p = buf; *p; p++) if (((unsigned char)*p & 0xc0) != 0x80) n++;
	if (!n) {
		pm_font(cr, 12.5, 0);
		pm_rgb(cr, T->dim);
		pm_text(cr, x + 12, y + 16, hint);
	}
	size_t dots = MIN(n, (size_t)((w - 30) / 12));
	pm_rgb(cr, T->menu_text);
	for (size_t i = 0; i < dots; i++) { cairo_arc(cr, x + 16 + i * 12, y + 16, 3.6, 0, 2 * M_PI); cairo_fill(cr); }
	if (active) {
		pm_rgb(cr, T->sel);
		cairo_rectangle(cr, x + (n ? 12 + dots * 12 : 10), y + 8, 1.6, 16);
		cairo_fill(cr);
	}
}

/* 畫提示窗（X 與原生共用），回傳需要的高度 */
static int paint_prompt(cairo_t *cr)
{
	pm_rgb(cr, T->menu);
	cairo_paint(cr);
	/* 鑰匙圖示 */
	double ix = 24, iy = 24;
	pm_rrect(cr, ix, iy, 44, 44, 10, 10);
	pm_rgb(cr, 0xff9f0a);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 3.2);
	cairo_arc(cr, ix + 16, iy + 20, 7, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_move_to(cr, ix + 21, iy + 25); cairo_line_to(cr, ix + 35, iy + 39); cairo_stroke(cr);
	cairo_move_to(cr, ix + 31, iy + 35); cairo_line_to(cr, ix + 35, iy + 31); cairo_stroke(cr);
	cairo_move_to(cr, ix + 27, iy + 31); cairo_line_to(cr, ix + 30, iy + 28); cairo_stroke(cr);

	double tx = 84, tw = W - tx - 24, y = 26;
	pm_font(cr, 15, 1);
	pm_rgb(cr, T->menu_text);
	pm_text(cr, tx, y + 9, create_mode ? _("建立鑰匙圈") : _("解鎖鑰匙圈"));
	y += 26;
	pm_font(cr, 12.5, 0);
	pm_rgb(cr, T->dim);
	char msg[400];
	if (create_mode)
		snprintf(msg, sizeof msg, "%s", _("這台電腦還沒有鑰匙圈。設定一個密碼來保護裡面儲存的密碼；設成跟登入密碼一樣，以後登入時就會自動解鎖。"));
	else
		snprintf(msg, sizeof msg, _("「%s」需要使用鑰匙圈裡儲存的密碼。請輸入鑰匙圈密碼（通常是登入密碼）。"), app_name);
	y = wrap(cr, msg, tx, y, tw) + 10;
	double shake = shake_until > pm_now_ms() ? ((pm_now_ms() / 40) % 2 ? 4 : -4) : 0;
	fld_y[0] = y;
	draw_field(cr, tx + shake, y, tw, pw1, field == 0, create_mode ? _("密碼") : _("鑰匙圈密碼"));
	y += 40;
	if (create_mode) {
		fld_y[1] = y;
		draw_field(cr, tx + shake, y, tw, pw2, field == 1, _("再輸入一次"));
		y += 40;
	}
	if (errmsg[0]) {
		pm_font(cr, 12, 0);
		pm_rgb(cr, 0xff3b30);
		y = wrap(cr, errmsg, tx, y, tw);
	}
	y += 10;
	pm_font(cr, 13, 0);
	const char *labels[2] = { _("取消"), create_mode ? _("建立") : _("解鎖") };
	double bx = W - 24;
	for (int i = 1; i >= 0; i--) {
		btn_w[i] = MAX(80, pm_textw(cr, labels[i]) + 32);
		bx -= btn_w[i];
		btn_x[i] = bx;
		bx -= 10;
	}
	btn_y = y;
	for (int i = 0; i < 2; i++) {
		pm_rrect(cr, btn_x[i], btn_y, btn_w[i], btn_h, 7, 7);
		pm_rgb(cr, i ? T->sel : (pm_dark ? 0x48484a : 0xe5e5ea));
		cairo_fill(cr);
		pm_rgb(cr, i ? 0xffffff : T->menu_text);
		pm_text(cr, btn_x[i] + (btn_w[i] - pm_textw(cr, labels[i])) / 2, btn_y + btn_h / 2, labels[i]);
	}
	return (int)(btn_y + btn_h + 20);
}

static void draw(void)
{
	if (native) {
		if (!wl) return;
		cairo_t *cr = pmwl_begin(wl, NULL, NULL);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
		cairo_save(cr);
		pm_rrect(cr, 0, 0, W, win_h, 12, 12);
		cairo_clip(cr);
		int need = paint_prompt(cr);
		cairo_restore(cr);
		pm_rrect(cr, 0.5, 0.5, W - 1, win_h - 1, 12, 12);
		pm_rgba(cr, 0x000000, 0.35);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		pmwl_end(wl, cr);
		if (need != win_h) { win_h = need; pmwl_layer_resize(wl, W, win_h); draw(); }
		return;
	}
	if (!win) return;
	Paint p;
	cairo_t *cr = pm_paint_begin(&p, win, W, win_h);
	int need = paint_prompt(cr);
	pm_paint_end(&p, cr, 0, 0);
	if (need != win_h) {
		win_h = need;
		XResizeWindow(dpy, win, W, win_h);
		draw();
	}
	XFlush(dpy);
}

static void wipe_fields(void)
{
	kc_wipe(pw1, sizeof pw1);
	kc_wipe(pw2, sizeof pw2);
}

int ui_prompt_open(unsigned id, const char *app, int create)
{
	if (win || wl) return -2;
	if (!ui_inited) {
		/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生提示窗，不開 X 連線 */
		native = pmwl_connect() && pmwl_has_layer();
		if (!native && !(dpy = XOpenDisplay(NULL))) return -1;
		ui_inited = 1;
		pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
		        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
		mlock(pw1, sizeof pw1);
		mlock(pw2, sizeof pw2);
	}
	cur_prompt = id;
	create_mode = create;
	field = 0;
	errmsg[0] = 0;
	wipe_fields();
	snprintf(app_name, sizeof app_name, "%s", app ? app : "");
	if (native) {   /* overlay 層、獨佔鍵盤（輸入密碼時別的程式收不到按鍵）、螢幕上方置中；不啟用輸入法 */
		wl = pmwl_layer("pomelo-keyring", 3, -1, PMWL_ANCHOR_TOP, W, win_h, 160, 0, 0, 0, -1, 1);
		if (!wl) return -1;
		draw();
		return 0;
	}
	pm_update_monitors();
	Rect M = mons[pm_pointer_mon()];
	XSetWindowAttributes wa = { .background_pixel = T->menu,
	                            .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask };
	win = XCreateWindow(dpy, pm_root, M.x + (M.w - W) / 2, M.y + M.h / 4, W, win_h, 0, CopyFromParent, InputOutput,
	                    CopyFromParent, CWBackPixel | CWEventMask, &wa);
	XClassHint ch = { "pomelo-keyring", "Pomelo-keyring" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PPosition;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	const char *title = create ? _("建立鑰匙圈") : _("解鎖鑰匙圈");
	XStoreName(dpy, win, title);
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_NAME", False), XInternAtom(dpy, "UTF8_STRING", False), 8,
	                PropModeReplace, (const unsigned char *)title, strlen(title));
	Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False), above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
	XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32, PropModeReplace, (unsigned char *)&above, 1);
	a_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &a_delete, 1);
	XMapRaised(dpy, win);
	XSync(dpy, False);
	for (int i = 0; i < 100; i++) {
		if (XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
		usleep(5000);
	}
	XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
	draw();
	return 0;
}

void ui_prompt_error(unsigned id, const char *msg)
{
	if (id != cur_prompt || !(win || wl)) return;
	snprintf(errmsg, sizeof errmsg, "%s", msg);
	wipe_fields();
	field = 0;
	shake_until = pm_now_ms() + 300;
	draw();
}

void ui_prompt_close(unsigned id)
{
	if (id != cur_prompt || !(win || wl)) return;
	if (native) { pmwl_destroy(wl); wl = NULL; wipe_fields(); return; }
	XUngrabKeyboard(dpy, CurrentTime);
	XDestroyWindow(dpy, win);
	win = None;
	wipe_fields();
	XFlush(dpy);
}

static void submit(void)
{
	if (!pw1[0]) { snprintf(errmsg, sizeof errmsg, "%s", _("請輸入密碼。")); draw(); return; }
	if (create_mode && strcmp(pw1, pw2)) {
		snprintf(errmsg, sizeof errmsg, "%s", _("兩次輸入的密碼不一樣。"));
		kc_wipe(pw2, sizeof pw2);
		field = 1;
		draw();
		return;
	}
	service_prompt_done(cur_prompt, pw1);
}

static void key_sym(KeySym ks, const char *buf, int n, unsigned state)
{
	char *f = field ? pw2 : pw1;
	size_t len = strlen(f);
	if (ks == XK_Escape) { service_prompt_done(cur_prompt, NULL); return; }
	if (ks == XK_Return || ks == XK_KP_Enter) {
		if (create_mode && field == 0 && !pw2[0]) field = 1;
		else { submit(); return; }   /* submit 自己會重畫；不能接著清掉它設的提示（兩次密碼不一樣、沒輸入） */
	} else if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
		if (create_mode) field = !field;
	} else if (ks == XK_BackSpace) {
		if (state & ControlMask) kc_wipe(f, 256);
		else { while (len && ((unsigned char)f[len - 1] & 0xc0) == 0x80) f[--len] = 0; if (len) f[--len] = 0; }
	} else if ((state & ControlMask) && ks == XK_u) {
		kc_wipe(f, 256);
	} else if (n > 0 && (unsigned char)buf[0] >= 32 && buf[0] != 127 && !(state & ControlMask) && len + (size_t)n < 255) {
		memcpy(f + len, buf, (size_t)n);
		f[len + n] = 0;
	}
	errmsg[0] = 0;
	draw();
}

static void key(XKeyEvent *e)
{
	char buf[32];
	KeySym ks;
	int n = XLookupString(e, buf, sizeof buf - 1, &ks, NULL);
	key_sym(ks, buf, n, e->state);
	explicit_bzero(buf, sizeof buf);
}

static void click(int x, int y)
{
	for (int i = 0; i < 2; i++)
		if (x >= btn_x[i] && x < btn_x[i] + btn_w[i] && y >= btn_y && y < btn_y + btn_h) {
			if (i) submit(); else service_prompt_done(cur_prompt, NULL);
			return;
		}
	for (int i = 0; i < (create_mode ? 2 : 1); i++)
		if (y >= fld_y[i] && y < fld_y[i] + 32) { field = i; draw(); }
}

static void x_events(void)
{
	if (native) {   /* 原生：沒有 fd 可交給 select，主迴圈每 40 毫秒（有提示窗時）來取一次 */
		PmwEvent ev;
		while (wl && pmwl_next(&ev, 0)) {
			if (ev.win != wl) { explicit_bzero(ev.text, sizeof ev.text); continue; }
			switch (ev.type) {
			case PMW_EXPOSE: case PMW_SIZE: draw(); break;
			case PMW_KEY: key_sym(ev.key, ev.text, ev.textlen, ev.state); break;
			case PMW_DOWN: if (ev.button == 1) click(ev.x, ev.y); break;
			case PMW_CLOSE: service_prompt_done(cur_prompt, NULL); break;
			}
			explicit_bzero(ev.text, sizeof ev.text);
		}
		return;
	}
	while (dpy && XPending(dpy)) {
		XEvent ev;
		XNextEvent(dpy, &ev);
		if (!win || ev.xany.window != win) continue;
		switch (ev.type) {
		case Expose: if (!ev.xexpose.count) draw(); break;
		case MapNotify: draw(); break;
		case KeyPress: key(&ev.xkey); break;
		case ButtonPress: click(ev.xbutton.x, ev.xbutton.y); break;
		case ClientMessage: if ((Atom)ev.xclient.data.l[0] == a_delete) service_prompt_done(cur_prompt, NULL); break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* --daemon                                                            */
/* ------------------------------------------------------------------ */

static int daemon_main(void)
{
	prctl(PR_SET_DUMPABLE, 0);
	signal(SIGPIPE, SIG_IGN);
	pm_conf_load();
	static KStore store;
	ks_init(&store, store_path);
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!bus) { fprintf(stderr, "pomelo-keyring: %s\n", err.message ? err.message : "no session bus"); return 1; }
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	if (dbus_bus_request_name(bus, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "pomelo-keyring: %s\n", _("已經有別的鑰匙圈服務在執行"));
		return 0;
	}
	service_init(bus, &store, fetch_stash());
	int dfd = -1;
	dbus_connection_get_unix_fd(bus, &dfd);
	for (;;) {
		fd_set fds;
		FD_ZERO(&fds);
		if (dfd >= 0) FD_SET(dfd, &fds);
		int xfd = dpy ? ConnectionNumber(dpy) : -1;
		if (xfd >= 0) FD_SET(xfd, &fds);
		struct timeval tv = { 0, win || wl ? 40000 : 500000 };   /* 視窗在抖動時要重畫 */
		select(MAX(dfd, xfd) + 1, &fds, NULL, NULL, &tv);
		do dbus_connection_read_write_dispatch(bus, 0);
		while (dbus_connection_get_dispatch_status(bus) == DBUS_DISPATCH_DATA_REMAINS);
		dbus_connection_flush(bus);
		if (!dbus_connection_get_is_connected(bus)) break;   /* 工作階段結束 */
		x_events();
		if ((win || wl) && shake_until > pm_now_ms() - 60) draw();
	}
	ks_forget(&store);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 設定程式（D-Bus 客戶端）                                             */
/* ------------------------------------------------------------------ */

static DBusConnection *cbus;

static DBusMessage *call(DBusMessage *m)
{
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(cbus, m, 10000, &err);
	dbus_message_unref(m);
	dbus_error_free(&err);
	return r;
}

static int kr_status(int *exists, int *locked, unsigned *n)
{
	if (!cbus) return -1;
	DBusMessage *r = call(dbus_message_new_method_call(BUS_NAME, SVC, IF_PRIV, "Status"));
	dbus_bool_t e = 0, l = 1;
	dbus_uint32_t k = 0;
	if (!r || !dbus_message_get_args(r, NULL, DBUS_TYPE_BOOLEAN, &e, DBUS_TYPE_BOOLEAN, &l, DBUS_TYPE_UINT32, &k, DBUS_TYPE_INVALID)) {
		if (r) dbus_message_unref(r);
		return -1;
	}
	dbus_message_unref(r);
	*exists = e; *locked = l; *n = k;
	return 0;
}

static struct { unsigned id; char label[160], sum[300]; } items[500];
static int nitems;

static void kr_list(void)
{
	nitems = 0;
	DBusMessage *r = call(dbus_message_new_method_call(BUS_NAME, SVC, IF_PRIV, "ListItems"));
	if (!r) return;
	DBusMessageIter it, a, st;
	if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&it, &a);
		while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRUCT && nitems < (int)LEN(items)) {
			dbus_message_iter_recurse(&a, &st);
			dbus_uint32_t id;
			const char *lab, *sum;
			dbus_message_iter_get_basic(&st, &id); dbus_message_iter_next(&st);
			dbus_message_iter_get_basic(&st, &lab); dbus_message_iter_next(&st);
			dbus_message_iter_get_basic(&st, &sum);
			items[nitems].id = id;
			snprintf(items[nitems].label, sizeof items[0].label, "%s", lab);
			snprintf(items[nitems].sum, sizeof items[0].sum, "%s", sum);
			nitems++;
			dbus_message_iter_next(&a);
		}
	}
	dbus_message_unref(r);
}

static int kr_bool_call(const char *method, int first, ...)
{
	DBusMessage *m = dbus_message_new_method_call(BUS_NAME, SVC, IF_PRIV, method);
	va_list ap;
	va_start(ap, first);
	dbus_message_append_args_valist(m, first, ap);
	va_end(ap);
	DBusMessage *r = call(m);
	dbus_bool_t ok = 0;
	if (r) { dbus_message_get_args(r, NULL, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID); dbus_message_unref(r); }
	return ok;
}

static char ui_pw[256], old_pw[256], new_pw[256], new_pw2[256], status_msg[200];
static unsigned sel_item;
static int del_confirm;
static long last_poll;
static int st_exists, st_locked = 1, st_ok;
static unsigned st_n;

static void refresh(void)
{
	st_ok = !kr_status(&st_exists, &st_locked, &st_n);
	if (st_ok) kr_list();
	last_poll = pm_now_ms();
}

static void build(PmUI *u)
{
	pm_ui_title(u, _("密碼與鑰匙圈"), _("幫瀏覽器、編輯器等程式保管密碼；全部加密存在這台電腦上"));
	if (!st_ok) {
		pm_ui_group(u, NULL);
		pm_ui_info(u, _("鑰匙圈服務沒有在執行"), NULL);
		if (pm_ui_button(u, _("正常情況下登入時會自動啟動"), _("啟動"))) { pm_spawn("pomelo-keyring --daemon"); last_poll = 0; }
		pm_ui_end(u);
		return;
	}
	char t[128];
	pm_ui_group(u, NULL);
	pm_ui_info(u, _("狀態"), !st_exists ? _("尚未建立") : st_locked ? _("已鎖定") : _("已解鎖"));
	snprintf(t, sizeof t, _("%u 個"), st_n);
	pm_ui_info(u, _("儲存的密碼"), t);
	if (st_exists && !st_locked && pm_ui_button(u, _("鎖定後，程式要用密碼時會先要求輸入鑰匙圈密碼"), _("立即鎖定"))) {
		kr_bool_call("Lock", DBUS_TYPE_INVALID);
		refresh();
	}
	if (st_locked) {
		pm_ui_entry(u, st_exists ? _("鑰匙圈密碼") : _("設定鑰匙圈密碼"), ui_pw, sizeof ui_pw, 1);
		if (pm_ui_button(u, NULL, st_exists ? _("解鎖") : _("建立")) && ui_pw[0]) {
			const char *p = ui_pw;
			int ok = kr_bool_call("Unlock", DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
			snprintf(status_msg, sizeof status_msg, "%s", ok ? "" : _("密碼不正確。"));
			kc_wipe(ui_pw, sizeof ui_pw);
			refresh();
		}
	}
	pm_ui_end(u);
	if (status_msg[0]) pm_ui_note(u, status_msg);
	pm_ui_note(u, _("鑰匙圈密碼跟登入密碼一樣時，登入就會自動解鎖。用 passwd 改了登入密碼後，下次要用時會問一次舊密碼，之後就會改用新密碼。"));

	if (st_exists) {
		pm_ui_group(u, _("變更鑰匙圈密碼"));
		pm_ui_entry(u, _("目前密碼"), old_pw, sizeof old_pw, 1);
		pm_ui_entry(u, _("新密碼"), new_pw, sizeof new_pw, 1);
		pm_ui_entry(u, _("確認新密碼"), new_pw2, sizeof new_pw2, 1);
		if (pm_ui_button(u, NULL, _("變更"))) {
			if (!new_pw[0] || strcmp(new_pw, new_pw2)) snprintf(status_msg, sizeof status_msg, "%s", _("兩次輸入的新密碼不一樣。"));
			else {
				const char *o = old_pw, *n = new_pw;
				int ok = kr_bool_call("ChangePassword", DBUS_TYPE_STRING, &o, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID);
				snprintf(status_msg, sizeof status_msg, "%s", ok ? _("已變更鑰匙圈密碼。") : _("目前密碼不正確。"));
			}
			kc_wipe(old_pw, sizeof old_pw); kc_wipe(new_pw, sizeof new_pw); kc_wipe(new_pw2, sizeof new_pw2);
			refresh();
		}
		pm_ui_end(u);
	}

	if (nitems) {
		pm_ui_group(u, _("儲存的項目"));
		for (int i = 0; i < nitems; i++) {
			if (pm_ui_item(u, "dialog-password", items[i].label[0] ? items[i].label : _("（沒有名稱）"), items[i].sum[0] ? items[i].sum : NULL, NULL, sel_item == items[i].id)) {
				sel_item = sel_item == items[i].id ? 0 : items[i].id;
				del_confirm = 0;
				u->again = 1;
			}
		}
		pm_ui_end(u);
		if (sel_item && !st_locked) {
			pm_ui_group(u, NULL);
			if (!del_confirm) {
				if (pm_ui_button(u, _("刪除選取的項目"), _("刪除…"))) { del_confirm = 1; u->again = 1; }
			} else if (pm_ui_button(u, _("確定要刪除嗎？用到它的程式會需要重新登入。"), _("刪除"))) {
				dbus_uint32_t id = sel_item;
				kr_bool_call("DeleteItem", DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
				sel_item = 0;
				del_confirm = 0;
				refresh();
			}
			pm_ui_end(u);
		}
	} else if (st_exists && st_locked) {
		pm_ui_note(u, _("解鎖後才能看到儲存的項目。"));
	}
}

static void tick(PmUI *u)
{
	if (pm_now_ms() - last_poll > 2000) { refresh(); pm_ui_redraw(u); }
}

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 10, 0, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 1.0, 0.70, 0.25);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.90, 0.45, 0.05);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 9);
	cairo_arc(cr, 48, 52, 18, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_move_to(cr, 61, 65); cairo_line_to(cr, 98, 102); cairo_stroke(cr);
	cairo_move_to(cr, 88, 92); cairo_line_to(cr, 98, 82); cairo_stroke(cr);
	cairo_move_to(cr, 77, 81); cairo_line_to(cr, 86, 72); cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	init_paths();
	if (argc > 1 && !strcmp(argv[1], "--stash")) return stash();
	if (argc > 1 && !strcmp(argv[1], "--daemon")) return daemon_main();
	cbus = dbus_bus_get(DBUS_BUS_SESSION, NULL);
	if (argc > 1 && !strcmp(argv[1], "--lock")) {
		if (!cbus) return 1;
		DBusMessage *r = call(dbus_message_new_method_call(BUS_NAME, SVC, IF_PRIV, "Lock"));
		if (r) dbus_message_unref(r);
		return r ? 0 : 1;
	}
	prctl(PR_SET_DUMPABLE, 0);
	pm_conf_load();
	refresh();
	pm_ui_native = 1;   /* 管理視窗沒有直接呼叫 X（密碼提示窗在 --daemon 那個程序）：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_("密碼與鑰匙圈"), "pomelo-keyring", 640, 680, build, tick, 500, icon);
}
