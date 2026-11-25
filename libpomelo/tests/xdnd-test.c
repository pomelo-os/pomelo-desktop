/* SPDX-License-Identifier: GPL-3.0-or-later */
/* xdnd-test.c — 最小的 X11 XDND 來源／目標（測 pomelo-compositor 的 X↔原生 Wayland 拖放橋用，不裝進套件）
 *
 *   xdnd-test 名稱 src|dst
 *   src：在視窗上按住拖動就開始拖 text/uri-list（DND_FILES=路徑:路徑…，預設 /etc/hostname）
 *   dst：XdndAware，收 text/uri-list，回報位置與放下的內容
 *   事件寫一行到 DND_LOG（沒設就 stderr）。編譯：cc -o xdnd-test xdnd-test.c -lX11 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *d;
static Window win;
static const char *name;
static FILE *lf;
static Atom aware, enter, position, status, leave, drop, finished, sel, uri, copy, typelist, prop;

static void note(const char *fmt, ...)
{
	char b[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (lf) { fprintf(lf, "%s: %s\n", name, b); fflush(lf); }
	fprintf(stderr, "%s: %s\n", name, b);
}

static void send_msg(Window to, Atom type, long a, long b, long c, long e)
{
	XEvent ev;
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = to;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = (long)win;
	ev.xclient.data.l[1] = a; ev.xclient.data.l[2] = b; ev.xclient.data.l[3] = c; ev.xclient.data.l[4] = e;
	XSendEvent(d, to, False, NoEventMask, &ev);
	XFlush(d);
}

static char uris[4096];

/* 游標底下最深的、有 XdndAware 的視窗（沿 XdndAware 往上找頂層） */
static Window find_target(int rx, int ry)
{
	Window root = DefaultRootWindow(d), w = root, child, found = None;
	int x = rx, y = ry;
	for (int i = 0; i < 20; i++) {
		Atom t; int f; unsigned long n, a; unsigned char *p = NULL;
		if (XGetWindowProperty(d, w, aware, 0, 1, False, AnyPropertyType, &t, &f, &n, &a, &p) == Success && p) { if (t != None) found = w; XFree(p); }
		int nx, ny;
		if (!XTranslateCoordinates(d, root, w, rx, ry, &nx, &ny, &child)) break;
		(void)x; (void)y;
		if (child == None) break;
		w = child;
	}
	return found;
}

int main(int argc, char **argv)
{
	name = argc > 1 ? argv[1] : "x";
	int src = argc > 2 && !strcmp(argv[2], "src");
	if (getenv("DND_LOG")) lf = fopen(getenv("DND_LOG"), "a");
	if (!(d = XOpenDisplay(NULL))) return 1;
	aware = XInternAtom(d, "XdndAware", False); enter = XInternAtom(d, "XdndEnter", False);
	position = XInternAtom(d, "XdndPosition", False); status = XInternAtom(d, "XdndStatus", False);
	leave = XInternAtom(d, "XdndLeave", False); drop = XInternAtom(d, "XdndDrop", False);
	finished = XInternAtom(d, "XdndFinished", False); sel = XInternAtom(d, "XdndSelection", False);
	uri = XInternAtom(d, "text/uri-list", False); copy = XInternAtom(d, "XdndActionCopy", False);
	typelist = XInternAtom(d, "XdndTypeList", False); prop = XInternAtom(d, "XDND_TEST_PROP", False);
	win = XCreateSimpleWindow(d, DefaultRootWindow(d), 100, 100, 360, 200, 0, 0, src ? 0x4080d0 : 0x50b060);
	XStoreName(d, win, name);
	XSelectInput(d, win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
	if (!src) { long v = 5; XChangeProperty(d, win, aware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&v, 1); }
	XMapWindow(d, win);
	const char *fl = getenv("DND_FILES");
	char *files = strdup(fl && *fl ? fl : "/etc/hostname");
	for (char *sv = NULL, *t = strtok_r(files, ":", &sv); t; t = strtok_r(NULL, ":", &sv)) {
		strcat(uris, "file://");
		for (const unsigned char *c = (const unsigned char *)t; *c; c++) {
			char e[4];
			if ((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || strchr("/-._~", *c)) { e[0] = (char)*c; e[1] = 0; }
			else snprintf(e, sizeof e, "%%%02X", *c);
			strcat(uris, e);
		}
		strcat(uris, "\r\n");
	}
	note("ready");
	int pressed = 0, px = 0, py = 0, dragging = 0;
	Window cur = None;       /* 目前的目標 */
	int accepted = 0;
	Window drop_src = None;  /* dst：現在的來源 */
	for (;;) {
		XEvent ev;
		XNextEvent(d, &ev);
		switch (ev.type) {
		case ButtonPress:
			if (src && ev.xbutton.button == 1) { pressed = 1; px = ev.xbutton.x_root; py = ev.xbutton.y_root; }
			break;
		case MotionNotify:
			if (src && pressed && !dragging && (abs(ev.xmotion.x_root - px) > 6 || abs(ev.xmotion.y_root - py) > 6)) {
				dragging = 1;
				XSetSelectionOwner(d, sel, win, CurrentTime);
				XGrabPointer(d, win, False, ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
				note("drag_start");
			}
			if (src && dragging) {
				Window t = find_target(ev.xmotion.x_root, ev.xmotion.y_root);
				if (t != cur) {
					if (cur) send_msg(cur, leave, 0, 0, 0, 0);
					cur = t;
					accepted = 0;
					if (cur) { send_msg(cur, enter, 5L << 24, (long)uri, 0, 0); note("sent XdndEnter to 0x%lx", cur); }
				}
				if (cur) send_msg(cur, position, 0, (ev.xmotion.x_root << 16) | ev.xmotion.y_root, CurrentTime, (long)copy);
			}
			break;
		case ButtonRelease:
			if (src && dragging) {
				XUngrabPointer(d, CurrentTime);
				if (cur && accepted) { send_msg(cur, drop, 0, 0, CurrentTime, 0); note("sent XdndDrop (accepted)"); }
				else { if (cur) send_msg(cur, leave, 0, 0, 0, 0); note("released without accepted target (cur=0x%lx accepted=%d)", cur, accepted); XSetSelectionOwner(d, sel, None, CurrentTime); }
				dragging = 0; cur = None;
			}
			pressed = 0;
			break;
		case SelectionRequest: {
			XSelectionRequestEvent *r = &ev.xselectionrequest;
			XEvent o;
			memset(&o, 0, sizeof o);
			o.xselection.type = SelectionNotify; o.xselection.requestor = r->requestor; o.xselection.selection = r->selection;
			o.xselection.target = r->target; o.xselection.time = r->time; o.xselection.property = None;
			if (r->target == uri) {
				XChangeProperty(d, r->requestor, r->property, uri, 8, PropModeReplace, (unsigned char *)uris, (int)strlen(uris));
				o.xselection.property = r->property;
				note("served text/uri-list to 0x%lx", r->requestor);
			} else if (r->target == XInternAtom(d, "TARGETS", False)) {
				Atom t[2] = { uri, XInternAtom(d, "TARGETS", False) };
				XChangeProperty(d, r->requestor, r->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 2);
				o.xselection.property = r->property;
			}
			XSendEvent(d, r->requestor, False, NoEventMask, &o);
			XFlush(d);
			break;
		}
		case ClientMessage: {
			Atom t = ev.xclient.message_type;
			Window from = (Window)ev.xclient.data.l[0];
			if (t == status && src) {
				accepted = ev.xclient.data.l[1] & 1;
				note("XdndStatus accept=%d action=%s", accepted, ev.xclient.data.l[4] ? XGetAtomName(d, (Atom)ev.xclient.data.l[4]) : "none");
			} else if (t == finished && src) {
				note("XdndFinished ok=%ld", ev.xclient.data.l[1] & 1);
				XSetSelectionOwner(d, sel, None, CurrentTime);
			} else if (t == enter) {
				drop_src = from;
				note("XdndEnter from 0x%lx ver=%ld", from, (ev.xclient.data.l[1] >> 24) & 0xff);
			} else if (t == position) {
				note("XdndPosition (%ld,%ld)", (ev.xclient.data.l[2] >> 16) & 0xffff, ev.xclient.data.l[2] & 0xffff);
				send_msg(drop_src, status, 1, 0, 0, (long)copy);
			} else if (t == leave) note("XdndLeave");
			else if (t == drop) {
				note("XdndDrop");
				XConvertSelection(d, sel, uri, prop, win, ev.xclient.data.l[2]);
			}
			break;
		}
		case SelectionNotify:
			if (!src) {
				Atom ty; int f; unsigned long n, a; unsigned char *p = NULL;
				if (ev.xselection.property != None && XGetWindowProperty(d, win, prop, 0, 65536, True, AnyPropertyType, &ty, &f, &n, &a, &p) == Success && p) {
					for (char *l = strtok((char *)p, "\r\n"); l; l = strtok(NULL, "\r\n")) note("  uri=%s", l);
					XFree(p);
					send_msg(drop_src, finished, 1, (long)copy, 0, 0);
					note("XdndFinished sent");
				} else { note("selection conversion failed"); send_msg(drop_src, finished, 0, 0, 0, 0); }
			}
			break;
		}
	}
}
