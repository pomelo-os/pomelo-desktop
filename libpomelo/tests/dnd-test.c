/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dnd-test.c — libpomelo 原生 Wayland 拖放的測試程式（不裝進套件）
 *
 *   dnd-test 名稱 模式 [x y]      模式：src（左半邊按住拖出去）、dst（收檔案）、both（左半邊來源、右半邊目標）
 *   環境變數：DND_FILES=路徑:路徑…（來源拖出的檔案，預設 /etc/hostname）、DND_MIME=custom（改拖自訂格式
 *   application/x-pomelo-test，資料 "hello-custom"）、DND_ACTIONS=copy|move|both、DND_LOG=檔案（事件記錄）
 * 編譯：cc -o dnd-test dnd-test.c -I.. -L.. -lpomelo $(pkg-config --cflags --libs cairo x11) -Wl,-rpath,..
 * 每個事件寫一行到記錄檔與視窗上，端到端測試腳本再比對。 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pomelo.h"
#include "../pomelo-wl.h"
#include "../pomelo-dnd.h"

static char lines[10][160];
static int nlines;
static const char *name, *mode;
static FILE *logfile;
static int hot_state;   /* 目標：游標在視窗上時的顏色提示 */

static void logf_(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (nlines == 10) { memmove(lines[0], lines[1], sizeof lines[0] * 9); nlines--; }
	snprintf(lines[nlines++], sizeof lines[0], "%s", buf);
	if (logfile) { fprintf(logfile, "%s: %s\n", name, buf); fflush(logfile); }
	fprintf(stderr, "%s: %s\n", name, buf);
}

static void draw(PmWl *w)
{
	int W, H;
	cairo_t *cr = pmwl_begin(w, &W, &H);
	int src = strcmp(mode, "dst"), dst = strcmp(mode, "src");
	cairo_set_source_rgb(cr, hot_state ? 0.78 : 0.93, hot_state ? 0.93 : 0.93, hot_state ? 0.78 : 0.95);
	cairo_paint(cr);
	if (src) {
		cairo_set_source_rgb(cr, 0.25, 0.55, 0.85);
		cairo_rectangle(cr, 10, 40, dst ? W / 2 - 15 : W - 20, 90);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_select_font_face(cr, "sans", 0, 0);
		cairo_set_font_size(cr, 15);
		cairo_move_to(cr, 20, 90);
		cairo_show_text(cr, "SOURCE: press and drag");
	}
	if (dst) {
		cairo_set_source_rgb(cr, 0.35, 0.7, 0.4);
		cairo_rectangle(cr, src ? W / 2 + 5 : 10, 40, src ? W / 2 - 15 : W - 20, 90);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_set_font_size(cr, 15);
		cairo_move_to(cr, (src ? W / 2 + 5 : 10) + 10, 90);
		cairo_show_text(cr, "TARGET: drop here");
	}
	cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
	cairo_set_font_size(cr, 18);
	cairo_move_to(cr, 10, 25);
	cairo_show_text(cr, name);
	cairo_set_font_size(cr, 12);
	for (int i = 0; i < nlines; i++) { cairo_move_to(cr, 10, 155 + i * 15); cairo_show_text(cr, lines[i]); }
	pmwl_end(w, cr);
}

static cairo_surface_t *make_icon(void)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgba(cr, 0.95, 0.6, 0.1, 0.9);
	cairo_rectangle(cr, 4, 4, 56, 56);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_font_size(cr, 14);
	cairo_move_to(cr, 10, 38);
	cairo_show_text(cr, "FILES");
	cairo_destroy(cr);
	return s;
}

static char *custom_data(const char *mime, size_t *len, void *user)
{
	(void)user;
	logf_("source-data-callback mime=%s", mime);
	*len = strlen("hello-custom");
	return strdup("hello-custom");
}

int main(int argc, char **argv)
{
	name = argc > 1 ? argv[1] : "dnd";
	mode = argc > 2 ? argv[2] : "both";
	if (getenv("DND_LOG")) logfile = fopen(getenv("DND_LOG"), "a");
	if (!pmwl_connect()) { fprintf(stderr, "no wayland\n"); return 1; }
	PmWl *w = pmwl_window(name, name, 400, 330, 300, 200, NULL);
	if (!w) return 1;
	int src = strcmp(mode, "dst"), dst = strcmp(mode, "src");
	if (dst) {
		static const char *const m[] = { "text/uri-list", "application/x-pomelo-test", "text/plain;charset=utf-8" };
		pmwl_dnd_target(w, m, 3, PMWL_DND_COPY | PMWL_DND_MOVE);
	}
	const char *fl = getenv("DND_FILES");
	char *files = strdup(fl && *fl ? fl : "/etc/hostname");
	const char *paths[16];
	int np = 0;
	for (char *sv = NULL, *t = strtok_r(files, ":", &sv); t && np < 16; t = strtok_r(NULL, ":", &sv)) paths[np++] = t;
	unsigned acts = getenv("DND_ACTIONS") && !strcmp(getenv("DND_ACTIONS"), "copy") ? PMWL_DND_COPY :
	                getenv("DND_ACTIONS") && !strcmp(getenv("DND_ACTIONS"), "move") ? PMWL_DND_MOVE : PMWL_DND_COPY | PMWL_DND_MOVE;
	int pressed = 0, px = 0, py = 0;
	logf_("ready");
	for (;;) {
		PmwEvent ev;
		if (!pmwl_next(&ev, -1)) continue;
		if (ev.type == PMW_DND_END) { logf_("DND_END action=%d", ev.button); draw(w); continue; }
		if (ev.win != w) continue;
		switch (ev.type) {
		case PMW_EXPOSE: case PMW_SIZE: draw(w); break;
		case PMW_CLOSE: return 0;
		case PMW_DOWN:
			if (ev.button == 1 && src && (!dst || ev.x < 200)) { pressed = 1; px = ev.x; py = ev.y; }
			break;
		case PMW_UP: pressed = 0; break;
		case PMW_MOTION:
			if (pressed && (abs(ev.x - px) > 6 || abs(ev.y - py) > 6)) {
				pressed = 0;
				int ok;
				if (getenv("DND_MIME") && !strcmp(getenv("DND_MIME"), "custom")) {
					static const char *const m[] = { "application/x-pomelo-test" };
					PmwDragSrc ds = { .mimes = m, .nmimes = 1, .actions = acts, .data = custom_data, .icon = make_icon(), .hot_x = 10, .hot_y = 10 };
					ok = pmwl_drag_start(w, &ds);
				} else ok = pmwl_drag_start_files(w, paths, np, acts, make_icon(), 10, 10);
				logf_("drag_start -> %d", ok);
			}
			break;
		case PMW_LEAVE: pressed = 0; break;
		case PMW_DND_ENTER:
			hot_state = 1;
			{
				const char *mm[8];
				int n = pmwl_dnd_mimes(mm, 8);
				char b[140] = "";
				for (int i = 0; i < n; i++) { strncat(b, mm[i], sizeof b - strlen(b) - 2); strcat(b, " "); }
				logf_("DND_ENTER (%d,%d) action=%d mimes=%s", ev.x, ev.y, ev.button, b);
			}
			draw(w);
			break;
		case PMW_DND_MOTION: logf_("DND_MOTION (%d,%d) action=%d", ev.x, ev.y, ev.button); break;
		case PMW_DND_LEAVE: hot_state = 0; logf_("DND_LEAVE"); draw(w); break;
		case PMW_DND_DROP: {
			hot_state = 0;
			logf_("DND_DROP (%d,%d) action=%d", ev.x, ev.y, ev.button);
			char **dp = NULL;
			int n = pmwl_dnd_files(&dp);
			for (int i = 0; i < n; i++) logf_("  file[%d]=%s", i, dp[i]);
			if (n > 0) pm_dnd_free(dp, n);
			size_t len = 0;
			char *c = pmwl_dnd_fetch("application/x-pomelo-test", &len);
			if (c) { logf_("  custom=%s (%zu bytes)", c, len); free(c); }
			draw(w);
			break;
		}
		}
	}
}
