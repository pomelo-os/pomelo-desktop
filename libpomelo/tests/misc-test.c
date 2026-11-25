/* SPDX-License-Identifier: GPL-3.0-or-later */
/* misc-test.c — libpomelo 原生 Wayland 階段 4 新增功能的測試程式（不裝進套件）
 *
 * 一個視窗，每個事件寫一行到 MISC_LOG（沒設就 stderr），按鍵控制：
 *   a 放剪貼簿（text/plain＋image/png＋text/uri-list）  b 讀剪貼簿所有格式並記錄  p 放 primary selection  g 讀 primary
 *   l 鎖住滑鼠（PMW_RELMOTION）  u 放開  i 快捷鍵抑制開／關  f 全螢幕開／關  t 換標題  m 最大化開／關
 *   c 換游標形狀  s 開一個子畫面（紅色方塊）  e 輸入法用途改終端機  h 防止閒置（idle inhibit）開／關  q 結束
 * 編譯：cc -o misc-test misc-test.c -I.. -L.. -lpomelo $(pkg-config --cflags --libs cairo x11) -Wl,-rpath,.. */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pomelo.h"
#include "../pomelo-wl.h"

static FILE *lf;
static const char *name;
static void note(const char *fmt, ...)
{
	char b[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (lf) { fprintf(lf, "%s: %s\n", name, b); fflush(lf); }
	fprintf(stderr, "%s: %s\n", name, b);
}

static void draw(PmWl *w, int color)
{
	int W, H;
	cairo_t *cr = pmwl_begin(w, &W, &H);
	cairo_set_source_rgb(cr, color ? 0.9 : 0.94, color ? 0.3 : 0.94, color ? 0.3 : 0.96);
	cairo_paint(cr);
	if (!color) {
		cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
		cairo_set_font_size(cr, 14);
		cairo_move_to(cr, 10, 24);
		cairo_show_text(cr, name);
	}
	pmwl_end(w, cr);
}

int main(int argc, char **argv)
{
	name = argc > 1 ? argv[1] : "misc";
	if (getenv("MISC_LOG")) lf = fopen(getenv("MISC_LOG"), "a");
	if (!pmwl_connect()) return 1;
	PmWl *w = pmwl_window(name, "misc-test", 420, 300, 300, 200, NULL), *sub = NULL;
	if (!w) return 1;
	pmwl_want(w, getenv("MISC_NOFINE") ? PMWL_IN_KEYUP : PMWL_IN_KEYUP | PMWL_IN_SCROLL_FINE | PMWL_IN_PREEDIT);   /* MISC_NOFINE：測舊的（一格一次）捲動 */
	int inhibit = 0, fs = 0, mx = 0, shape = 0;
	note("ready");
	for (;;) {
		PmwEvent ev;
		if (!pmwl_next(&ev, -1) || (ev.win != w && ev.win != sub)) continue;
		switch (ev.type) {
		case PMW_EXPOSE: case PMW_SIZE: if (ev.win == w) draw(w, 0); else draw(sub, 1); if (ev.type == PMW_SIZE) note("SIZE %dx%d fullscreen=%d maximized=%d", ev.w, ev.h, pmwl_is_fullscreen(w), pmwl_is_maximized(w)); break;
		case PMW_CLOSE: return 0;
		case PMW_KEY: {
			note("KEY sym=0x%lx evdev=%d repeat=%d text='%s' state=0x%x", (long)ev.key, ev.button, ev.x, ev.text, ev.state);
			if (ev.textlen != 1) break;
			switch (ev.text[0]) {
			case 'a': {
				static const unsigned char png[] = { 0x89, 'P', 'N', 'G', 0, 1, 2, 3 };
				const char *m[3] = { "text/plain;charset=utf-8", "image/png", "text/uri-list" };
				const void *d[3] = { "hello clip", png, "file:///etc/hostname\r\n" };
				size_t l[3] = { 10, sizeof png, 22 };
				pmwl_clip_set_types(m, d, l, 3);
				note("clip set");
				break;
			}
			case 'b': {
				const char *t[32];
				int n = pmwl_clip_types(t, 32);
				char b[300] = "";
				for (int i = 0; i < n; i++) { strncat(b, t[i], sizeof b - strlen(b) - 2); strcat(b, " "); }
				note("clip types(%d): %s", n, b);
				size_t len = 0;
				char *s = pmwl_clip_get_type("image/png", &len);
				note("png len=%zu first=%s has_uri=%d", len, s ? s + 1 : "(null)", pmwl_clip_has_type("text/uri-list"));
				free(s);
				s = pmwl_clip_get();
				note("text='%s'", s ? s : "(null)");
				free(s);
				break;
			}
			case 'p': pmwl_primary_set("primary-selected"); note("primary set"); break;
			case 'g': { char *s = pmwl_primary_get(); note("primary='%s'", s ? s : "(null)"); free(s); break; }
			case 'l': note("lock request -> %d", pmwl_pointer_lock(w, 1)); break;
			case 'u': note("unlock -> %d", pmwl_pointer_lock(w, 0)); break;
			case 'i': inhibit = !inhibit; pmwl_shortcuts_inhibit(w, inhibit); note("shortcuts inhibit=%d", inhibit); break;
			case 'f': fs = !fs; pmwl_fullscreen(w, fs, -1); note("fullscreen request %d", fs); break;
			case 'm': mx = !mx; pmwl_maximize(w, mx); note("maximize request %d", mx); break;
			case 't': pmwl_set_title(w, "Retitled"); note("title set"); break;
			case 'c': shape = (shape + 1) % 5; pmwl_cursor(w, shape == 1 ? PMWL_CURSOR_TEXT : shape == 2 ? PMWL_CURSOR_POINTER : shape == 3 ? PMWL_CURSOR_CROSSHAIR : shape == 4 ? PMWL_CURSOR_RESIZE_EW : PMWL_CURSOR_DEFAULT); note("cursor shape %d", shape); break;
			case 's': if (!sub) { sub = pmwl_subsurface(w, 250, 120, 100, 60); note("subsurface -> %p", (void *)sub); if (sub) draw(sub, 1); pmwl_sub_move(sub, w, 260, 130); } break;
			case 'h': { static int ih; ih = !ih; pmwl_idle_inhibit(w, ih); note("idle inhibit=%d", ih); break; }
			case 'e': pmwl_ime_purpose(PMWL_IME_TERMINAL); note("ime purpose terminal"); break;
			case 'q': return 0;
			}
			break;
		}
		case PMW_KEYUP: note("KEYUP sym=0x%lx evdev=%d", (long)ev.key, ev.button); break;
		case PMW_SCROLL: note("SCROLL button=%d", ev.button); break;
		case PMW_SCROLL_FINE: note("SCROLL_FINE dx=%d dy=%d", ev.w, ev.h); break;
		case PMW_DOWN: note("DOWN button=%d (%d,%d) win=%s", ev.button, ev.x, ev.y, ev.win == sub ? "sub" : "main"); break;
		case PMW_RELMOTION: note("RELMOTION %d,%d", ev.x, ev.y); break;
		case PMW_POINTER_LOCK: note("POINTER_LOCK %d", ev.x); break;
		case PMW_FOCUS_IN: note("FOCUS_IN"); break;
		case PMW_FOCUS_OUT: note("FOCUS_OUT"); break;
		}
	}
}
