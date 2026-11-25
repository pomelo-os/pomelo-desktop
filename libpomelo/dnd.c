/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 收別的程式拖進來的檔案（XDND 第 5 版；只收 text/uri-list 裡的本機檔案）
 *
 * 用法：建立視窗後呼叫 pm_dnd_accept(dpy, win)；每個事件先交給 pm_dnd_event()，
 * 回傳 > 0 就是放下了幾個檔案，處理完用 pm_dnd_free() 釋放。
 * 檔案管理員自己有完整的拖放（拖出去、反白目的地），不用這個。
 */
#define _GNU_SOURCE
#include "pomelo-dnd.h"
#include <X11/Xatom.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { A_AWARE, A_ENTER, A_POSITION, A_STATUS, A_LEAVE, A_DROP, A_FINISHED, A_SEL, A_TYPES, A_COPY, A_URILIST, A_PROP, A_N };
static const char *names[A_N] = {
	"XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop", "XdndFinished",
	"XdndSelection", "XdndTypeList", "XdndActionCopy", "text/uri-list", "POMELO_DND_DROP",
};
static Atom a[A_N];

/* 目前拖進來的這一次 */
static struct { Display *d; Window win, src; int ver, ok, fetching; } st;

int pm_parse_uris(char *data, char **paths, int max)
{
	int np = 0;
	char *save = NULL;
	for (char *l = strtok_r(data, "\r\n", &save); l && np < max; l = strtok_r(NULL, "\r\n", &save)) {
		if (strncmp(l, "file://", 7)) continue;
		char *p = l + 7, *o;
		while (*p && *p != '/') p++;   /* 略過主機名稱 */
		if (!*p) continue;
		paths[np] = o = malloc(strlen(p) + 1);
		for (; *p; p++) {
			if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
				char hx[3] = { p[1], p[2], 0 };
				*o++ = (char)strtol(hx, NULL, 16);
				p += 2;
			} else *o++ = *p;
		}
		*o = 0;
		/* 結尾的 / 拿掉（資料夾有時候會這樣寫），根目錄除外 */
		while (o - paths[np] > 1 && o[-1] == '/') *--o = 0;
		if (!paths[np][0] || strchr(paths[np], 0) != o) { free(paths[np]); continue; }   /* 中間有 %00 */
		np++;
	}
	return np;
}

void pm_dnd_accept(Display *d, Window w)
{
	if (!a[0]) XInternAtoms(d, (char **)names, A_N, False, a);
	st.d = d;
	long v = 5;
	XChangeProperty(d, w, a[A_AWARE], XA_ATOM, 32, PropModeReplace, (unsigned char *)&v, 1);
}

/* 來源視窗的 XdndTypeList（超過 3 種格式時放在這裡） */
static int type_list(Window src, Atom *out, int max)
{
	Atom type; int fmt; unsigned long n = 0, after;
	unsigned char *data = NULL;
	if (XGetWindowProperty(st.d, src, a[A_TYPES], 0, max, False, XA_ATOM, &type, &fmt, &n, &after, &data) != Success || !data) return 0;
	int k = 0;
	if (type == XA_ATOM && fmt == 32) for (; k < (int)n && k < max; k++) out[k] = (Atom)((long *)data)[k];
	XFree(data);
	return k;
}

static void send(Atom type, long l1, long l2, long l3, long l4)
{
	XEvent ev;
	memset(&ev, 0, sizeof ev);
	ev.xclient.type = ClientMessage;
	ev.xclient.window = st.src;
	ev.xclient.message_type = type;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = (long)st.win;
	ev.xclient.data.l[1] = l1;
	ev.xclient.data.l[2] = l2;
	ev.xclient.data.l[3] = l3;
	ev.xclient.data.l[4] = l4;
	XSendEvent(st.d, st.src, False, NoEventMask, &ev);
	XFlush(st.d);
}

void pm_dnd_free(char **paths, int n)
{
	for (int i = 0; i < n; i++) free(paths[i]);
	free(paths);
}

int pm_dnd_event(Display *d, XEvent *ev, char ***paths)
{
	if (!a[0] || d != st.d) return -1;
	if (ev->type == ClientMessage) {
		XClientMessageEvent *e = &ev->xclient;
		Atom t = e->message_type;
		Window src = (Window)e->data.l[0];
		if (t == a[A_ENTER]) {
			st.win = e->window;
			st.src = src;
			st.ver = (int)(((unsigned long)e->data.l[1] >> 24) & 0xff);
			st.ok = st.fetching = 0;
			Atom types[64];
			int nt = 0;
			if (e->data.l[1] & 1) nt = type_list(src, types, 64);
			else for (int k = 2; k < 5; k++) if (e->data.l[k]) types[nt++] = (Atom)e->data.l[k];
			for (int k = 0; k < nt; k++) if (types[k] == a[A_URILIST]) st.ok = 1;
			return 0;
		}
		if (t == a[A_POSITION]) {
			if (src == st.src) send(a[A_STATUS], st.ok ? 1 : 0, 0, 0, st.ok ? (long)a[A_COPY] : (long)None);
			return 0;
		}
		if (t == a[A_LEAVE]) {
			if (src == st.src) st.src = None;
			return 0;
		}
		if (t == a[A_DROP]) {
			if (src != st.src) return 0;
			if (!st.ok) { send(a[A_FINISHED], 0, (long)None, 0, 0); st.src = None; return 0; }
			XConvertSelection(st.d, a[A_SEL], a[A_URILIST], a[A_PROP], st.win, st.ver >= 1 ? (Time)e->data.l[2] : CurrentTime);
			st.fetching = 1;
			return 0;
		}
		if (t == a[A_STATUS] || t == a[A_FINISHED]) return 0;
		return -1;
	}
	if (ev->type == SelectionNotify && ev->xselection.selection == a[A_SEL] && ev->xselection.requestor == st.win) {
		if (!st.fetching || !st.src) return 0;
		st.fetching = 0;
		char **v = malloc(1024 * sizeof *v);
		int n = 0;
		if (ev->xselection.property != None) {
			Atom type; int fmt; unsigned long cnt, after;
			unsigned char *data = NULL;
			if (XGetWindowProperty(st.d, st.win, a[A_PROP], 0, 1 << 22, True, AnyPropertyType, &type, &fmt, &cnt, &after, &data) == Success && data) {
				if (fmt == 8) n = pm_parse_uris((char *)data, v, 1024);
				XFree(data);
			}
		}
		send(a[A_FINISHED], st.ver >= 5 && n > 0 ? 1 : 0, st.ver >= 5 && n > 0 ? (long)a[A_COPY] : (long)None, 0, 0);
		st.src = None;
		if (n <= 0) { free(v); return 0; }
		*paths = v;
		return n;
	}
	return -1;
}
