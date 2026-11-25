/* SPDX-License-Identifier: GPL-3.0-or-later */
/* xdnd-drop — 測試用的 XDND 來源：把檔案「拖」到指定視窗上並放下（xdotool 沒辦法模擬拖放）
 * 用法：xdnd-drop 目標視窗編號 檔案…   （編號用 xdotool search 找，十進位或 0x 開頭）
 * 流程照 XDND 第 5 版：擁有 XdndSelection → XdndEnter → XdndPosition → 等 XdndStatus → XdndDrop → 回應 SelectionRequest → 等 XdndFinished */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "用法：xdnd-drop 視窗編號 檔案…\n"); return 2; }
	Display *d = XOpenDisplay(NULL);
	if (!d) return 1;
	Window target = strtoul(argv[1], NULL, 0);
	char uris[8192] = "";
	for (int i = 2; i < argc; i++) { strcat(uris, "file://"); strcat(uris, argv[i]); strcat(uris, "\r\n"); }
	Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
	Atom enter = XInternAtom(d, "XdndEnter", False), position = XInternAtom(d, "XdndPosition", False), status = XInternAtom(d, "XdndStatus", False);
	Atom drop = XInternAtom(d, "XdndDrop", False), finished = XInternAtom(d, "XdndFinished", False), sel = XInternAtom(d, "XdndSelection", False);
	Atom urilist = XInternAtom(d, "text/uri-list", False), copy = XInternAtom(d, "XdndActionCopy", False);
	XSetSelectionOwner(d, sel, w, CurrentTime);
	XEvent e = { .xclient = { .type = ClientMessage, .display = d, .window = target, .message_type = enter, .format = 32 } };
	e.xclient.data.l[0] = (long)w; e.xclient.data.l[1] = 5L << 24; e.xclient.data.l[2] = (long)urilist;
	XSendEvent(d, target, False, NoEventMask, &e);
	e.xclient.message_type = position;
	e.xclient.data.l[1] = 0; e.xclient.data.l[2] = (100L << 16) | 300; e.xclient.data.l[3] = CurrentTime; e.xclient.data.l[4] = (long)copy;
	XSendEvent(d, target, False, NoEventMask, &e);
	XFlush(d);
	int accepted = 0, sent_drop = 0, done = 0;
	for (int spins = 0; spins < 200 && !done; spins++) {
		while (XPending(d)) {
			XEvent ev;
			XNextEvent(d, &ev);
			if (ev.type == ClientMessage && ev.xclient.message_type == status) {
				accepted = (int)(ev.xclient.data.l[1] & 1);
				if (!sent_drop) {
					e.xclient.message_type = drop; e.xclient.data.l[1] = 0; e.xclient.data.l[2] = CurrentTime;
					XSendEvent(d, target, False, NoEventMask, &e);
					XFlush(d);
					sent_drop = 1;
				}
			} else if (ev.type == SelectionRequest) {
				XSelectionRequestEvent *r = &ev.xselectionrequest;
				XSelectionEvent n = { .type = SelectionNotify, .display = d, .requestor = r->requestor, .selection = r->selection, .target = r->target, .property = r->property, .time = r->time };
				XChangeProperty(d, r->requestor, r->property, r->target, 8, PropModeReplace, (unsigned char *)uris, (int)strlen(uris));
				XSendEvent(d, r->requestor, False, 0, (XEvent *)&n);
				XFlush(d);
			} else if (ev.type == ClientMessage && ev.xclient.message_type == finished) done = 1;
		}
		struct timeval tv = { 0, 50000 };
		fd_set fds;
		FD_ZERO(&fds); FD_SET(ConnectionNumber(d), &fds);
		select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
	}
	printf("accepted=%d dropped=%d finished=%d\n", accepted, sent_drop, done);
	return accepted && sent_drop ? 0 : 1;
}
