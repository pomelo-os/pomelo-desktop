/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gui-common.c — 圖示與子程序 */
#define _GNU_SOURCE
#include "gui.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

/* 紙箱（棕橘色漸層）＋中間一條拉鍊＋右下角一片柚子 */
cairo_surface_t *pa_icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	/* 影子 */
	pm_rrect(cr, 14, 30, 100, 86, 12, 12);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
	cairo_translate(cr, 0, 3);
	cairo_fill(cr);
	cairo_translate(cr, 0, -3);
	/* 箱身 */
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 30, 0, 114);
	cairo_pattern_add_color_stop_rgb(g, 0, 1.00, 0.72, 0.30);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.97, 0.52, 0.18);
	pm_rrect(cr, 14, 36, 100, 78, 8, 12);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	/* 箱蓋 */
	g = cairo_pattern_create_linear(0, 16, 0, 44);
	cairo_pattern_add_color_stop_rgb(g, 0, 1.00, 0.82, 0.45);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.99, 0.66, 0.26);
	pm_rrect(cr, 8, 18, 112, 28, 10, 4);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
	pm_rrect(cr, 12, 20, 104, 5, 3, 3);
	cairo_fill(cr);
	/* 拉鍊：兩排交錯的齒 */
	for (int i = 0; i < 9; i++) {
		double y = 22 + i * 7;
		cairo_set_source_rgb(cr, 0.35, 0.22, 0.10);
		cairo_rectangle(cr, i % 2 ? 62 : 56, y, 10, 4);
		cairo_fill(cr);
	}
	cairo_set_source_rgba(cr, 0.35, 0.22, 0.10, 0.9);
	cairo_rectangle(cr, 63, 18, 2, 56);
	cairo_fill(cr);
	/* 拉片 */
	pm_rrect(cr, 55, 72, 18, 24, 7, 7);
	cairo_set_source_rgb(cr, 0.93, 0.93, 0.95);
	cairo_fill(cr);
	pm_rrect(cr, 60, 79, 8, 10, 3, 3);
	cairo_set_source_rgb(cr, 0.55, 0.40, 0.25);
	cairo_fill(cr);
	/* 柚子切片 */
	double cx = 96, cy = 92, r = 20;
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_arc(cr, cx, cy, r + 3, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.60, 0.82, 0.30);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1.0, 0.95, 0.78);
	cairo_arc(cr, cx, cy, r - 3.5, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1.0, 0.70, 0.40);
	for (int i = 0; i < 8; i++) {
		double a0 = i * M_PI / 4 + 0.14, a1 = (i + 1) * M_PI / 4 - 0.14;
		cairo_move_to(cr, cx, cy);
		cairo_arc(cr, cx, cy, r - 5.5, a0, a1);
		cairo_close_path(cr);
	}
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

void pa_center(PmUI *u, unsigned long parent)
{
	static int done;
	if (done || u->wl || !dpy || !u->win) return;
	done = 1;
	int cx, cy, cw, ch;
	XWindowAttributes wa, pa;
	XGetWindowAttributes(dpy, u->win, &wa);
	if (parent && XGetWindowAttributes(dpy, (Window)parent, &pa)) {
		Window c;
		int rx, ry;
		XTranslateCoordinates(dpy, (Window)parent, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &c);
		cx = rx; cy = ry; cw = pa.width; ch = pa.height;
	} else { cx = 0; cy = 0; cw = DisplayWidth(dpy, DefaultScreen(dpy)); ch = DisplayHeight(dpy, DefaultScreen(dpy)); }
	int x = cx + (cw - wa.width) / 2, y = cy + (ch - wa.height) / 2;
	XMoveWindow(dpy, u->win, MAX(0, x), MAX(0, y));
}

const char *pa_exe(void)
{
	static char p[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
	if (n <= 0) { snprintf(p, sizeof p, "pomelo-archive"); return p; }
	p[n] = 0;
	const char *del = strstr(p, " (deleted)");   /* 程式更新過：舊的檔案被換掉了，用路徑找新的 */
	if (del) p[del - p] = 0;
	return p;
}

int pa_spawn(char *const argv[], const char *pw, int *out_fd)
{
	int in[2] = { -1, -1 }, ot[2] = { -1, -1 };
	if (pw && pipe2(in, O_CLOEXEC)) return -1;
	if (out_fd && pipe2(ot, O_CLOEXEC)) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (!pid) {
		if (in[0] >= 0) dup2(in[0], 0);
		if (ot[1] >= 0) { dup2(ot[1], 1); dup2(ot[1], 2); }   /* 輸出與錯誤訊息都收進同一條管子 */
		if (dpy) close(ConnectionNumber(dpy));   /* 子程式自己連 X，不要共用 */
		execv(argv[0], argv);
		_exit(127);
	}
	if (in[0] >= 0) { close(in[0]); signal(SIGPIPE, SIG_IGN); if (write(in[1], pw, strlen(pw)) < 0) {} if (write(in[1], "\n", 1) < 0) {} close(in[1]); }
	if (ot[1] >= 0) { close(ot[1]); *out_fd = ot[0]; }
	return (int)pid;
}
