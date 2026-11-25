#define _GNU_SOURCE
/* 測試用：虛擬鍵盤，送「標準 evdev 按鍵碼」（wtype 會自己換一份 keymap，按鍵碼不是真鍵盤的，測掃描碼用這個）。
 * 用法：vkbd 指令…  指令：數字＝按下再放開那個 evdev 碼（30＝A、28＝Enter、107＝End）；dN 按下；uN 放開；wN 等 N 毫秒。
 * 修飾鍵狀態由 xkbcommon 追蹤並一併送出（Ctrl＝29、Shift＝42、Alt＝56、Super＝125）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "vk.h"
static struct zwp_virtual_keyboard_manager_v1 *mgr;
static struct wl_seat *seat;
static void reg(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1")) mgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
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
	if (!mgr || !seat) { fprintf(stderr, "no virtual keyboard\n"); return 1; }
	struct zwp_virtual_keyboard_v1 *k = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(mgr, seat);
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us" };
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	char *txt = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	size_t len = strlen(txt) + 1;
	int fd = memfd_create("vkbd", 0);
	ftruncate(fd, len);
	void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memcpy(m, txt, len);
	zwp_virtual_keyboard_v1_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
	struct xkb_state *st = xkb_state_new(km);
	wl_display_roundtrip(d);
	usleep(600000);
	for (int i = 1; i < argc; i++) {
		const char *c = argv[i];
		if (c[0] == 'w') { wl_display_roundtrip(d); usleep(atoi(c + 1) * 1000); continue; }
		int down = 1, up = 1, code;
		if (c[0] == 'd') { up = 0; code = atoi(c + 1); } else if (c[0] == 'u') { down = 0; code = atoi(c + 1); } else code = atoi(c);
		for (int phase = 0; phase < 2; phase++) {
			if (phase == 0 && !down) continue;
			if (phase == 1 && !up) continue;
			int press = phase == 0 && down;
			if (!press && phase == 0) continue;
			zwp_virtual_keyboard_v1_key(k, ms(), (uint32_t)code, press ? 1 : 0);
			xkb_state_update_key(st, (xkb_keycode_t)code + 8, press ? XKB_KEY_DOWN : XKB_KEY_UP);
			zwp_virtual_keyboard_v1_modifiers(k, xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED), xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED),
			                                  xkb_state_serialize_mods(st, XKB_STATE_MODS_LOCKED), xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE));
			wl_display_roundtrip(d);
			usleep(30000);
		}
	}
	wl_display_roundtrip(d);
	return 0;
}
