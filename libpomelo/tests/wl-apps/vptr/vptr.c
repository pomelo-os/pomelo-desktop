/* 測試用：虛擬滑鼠，指令 "abs X Y" "rel DX DY" "down [btn]" "up [btn]" "click [btn]" "wait MS" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "vp.h"
static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat *seat;
static void reg(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, "zwlr_virtual_pointer_manager_v1")) mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void unreg(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener rl = { reg, unreg };
static uint32_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int main(int argc, char **argv) {
	struct wl_display *d = wl_display_connect(NULL);
	if (!d) { fprintf(stderr, "no display\n"); return 1; }
	struct wl_registry *r = wl_display_get_registry(d);
	wl_registry_add_listener(r, &rl, NULL);
	wl_display_roundtrip(d);
	if (!mgr) { fprintf(stderr, "no virtual pointer\n"); return 1; }
	struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	int W = getenv("VW") ? atoi(getenv("VW")) : 1280, H = getenv("VH") ? atoi(getenv("VH")) : 720;
	for (int i = 1; i < argc; i++) {
		const char *c = argv[i];
		if (!strcmp(c, "abs")) { int x = atoi(argv[++i]), y = atoi(argv[++i]); zwlr_virtual_pointer_v1_motion_absolute(p, ms(), x, y, W, H); }
		else if (!strcmp(c, "rel")) { double x = atof(argv[++i]), y = atof(argv[++i]); zwlr_virtual_pointer_v1_motion(p, ms(), wl_fixed_from_double(x), wl_fixed_from_double(y)); }
		else if (!strcmp(c, "down") || !strcmp(c, "up") || !strcmp(c, "click")) {
			int b = 0x110; if (i + 1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9') b = 0x110 + atoi(argv[++i]);
			if (strcmp(c, "up")) { zwlr_virtual_pointer_v1_button(p, ms(), b, 1); zwlr_virtual_pointer_v1_frame(p); wl_display_roundtrip(d); usleep(30000); }
			if (strcmp(c, "down")) zwlr_virtual_pointer_v1_button(p, ms(), b, 0);
		}
		else if (!strcmp(c, "scroll")) { double v = atof(argv[++i]); zwlr_virtual_pointer_v1_axis_source(p, 0); zwlr_virtual_pointer_v1_axis_discrete(p, ms(), 0, wl_fixed_from_double(v * 15), v > 0 ? 1 : -1); }
		else if (!strcmp(c, "hscroll")) { double v = atof(argv[++i]); zwlr_virtual_pointer_v1_axis_source(p, 0); zwlr_virtual_pointer_v1_axis_discrete(p, ms(), 1, wl_fixed_from_double(v * 15), v > 0 ? 1 : -1); }
		else if (!strcmp(c, "smooth")) { double v = atof(argv[++i]); zwlr_virtual_pointer_v1_axis_source(p, 1); zwlr_virtual_pointer_v1_axis(p, ms(), 0, wl_fixed_from_double(v)); zwlr_virtual_pointer_v1_frame(p); }
		else if (!strcmp(c, "wait")) { zwlr_virtual_pointer_v1_frame(p); wl_display_roundtrip(d); usleep(atoi(argv[++i]) * 1000); continue; }
		zwlr_virtual_pointer_v1_frame(p);
		wl_display_roundtrip(d);
		usleep(20000);
	}
	usleep(100000);
	zwlr_virtual_pointer_v1_destroy(p);
	wl_display_roundtrip(d);
	return 0;
}
