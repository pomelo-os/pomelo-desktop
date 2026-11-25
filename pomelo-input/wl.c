/* SPDX-License-Identifier: GPL-3.0-or-later */
/* wl.c — 服務原生 Wayland 程式（0.40 起）：GTK3／GTK4、Firefox 這些原生程式不經過 X，沒有 XIM。
 *
 * pomelo-compositor 有 input-method-v2（另一頭是程式的 text-input-v3）：程式點進輸入框時 compositor 送 activate，
 * 我們就抓住鍵盤（keyboard grab），實體鍵盤的按鍵先送到這裡，用 xkbcommon 轉成 keysym 交給引擎（跟 XIM、IBus 共用的
 * ic_key）；引擎不要的鍵用自己的虛擬鍵盤（virtual-keyboard）原樣送回程式，打好的字用 commit_string 送出。
 * 選字列畫在 compositor 的 input popup surface 上（它會擺在程式的文字游標下面），畫法跟 X 的選字列共用 ui_draw。
 * 輸入框是密碼欄（content purpose = password／pin）時不轉換，按鍵全部送回去。
 * 抓住鍵盤的期間按住不放的鍵要自己重複（被引擎吃掉的鍵程式收不到，不會幫我們重複，例如組字中按住倒退鍵）。 */
#define _GNU_SOURCE
#include "ic.h"
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#define CONTENT_PASSWORD 8   /* text-input-v3 的 content_purpose */
#define CONTENT_PIN 9

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwp_input_method_manager_v2 *imm;
static struct zwp_virtual_keyboard_manager_v1 *vkm;
static struct zwp_input_method_v2 *im;
static struct zwp_input_method_keyboard_grab_v2 *kgrab;
static struct zwp_virtual_keyboard_v1 *vk;
static struct wl_pointer *pointer;
static struct wl_surface *psurf;
static struct zwp_input_popup_surface_v2 *popup;
static int popup_shown, pointer_in;

static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xstate;
static int vk_has_keymap;

static IC *ic;                       /* 原生程式這邊只有一個輸入環境（協定一次只有一個作用中的輸入框） */
static uint32_t done_serial;         /* 收到幾次 done（commit 時要帶） */
static int pend_active, active, pend_purpose, purpose;
static unsigned char forwarded[256]; /* 按下時送回程式的鍵：放開也要送回去 */

static int rep_rate = 25, rep_delay = 600;
static uint32_t rep_key;             /* 正在重複的鍵（evdev 代碼＋1；0 = 沒有） */
static long rep_next;

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* xkb 的修飾鍵 → X 的 state（引擎跟 XIM 那邊一樣看 X 的位元） */
static unsigned x_state(void)
{
	if (!xstate) return 0;
	static const struct { const char *name; unsigned bit; } m[] = {
		{ XKB_MOD_NAME_SHIFT, 1 }, { XKB_MOD_NAME_CAPS, 2 }, { XKB_MOD_NAME_CTRL, 4 },
		{ XKB_MOD_NAME_ALT, 8 }, { XKB_MOD_NAME_NUM, 0x10 }, { XKB_MOD_NAME_LOGO, 0x40 },
	};
	unsigned s = 0;
	for (unsigned i = 0; i < sizeof m / sizeof *m; i++)
		if (xkb_state_mod_name_is_active(xstate, m[i].name, XKB_STATE_MODS_EFFECTIVE) > 0) s |= m[i].bit;
	return s;
}

static void bypass_key(uint32_t time, uint32_t key, uint32_t state)
{
	if (vk && vk_has_keymap) zwp_virtual_keyboard_v1_key(vk, time, key, state);
}

/* ---- 選字列（input popup surface） ---- */

struct buf { struct wl_buffer *b; void *data; size_t size; };

static void buf_release(void *data, struct wl_buffer *b)
{
	struct buf *x = data;
	wl_buffer_destroy(b);
	munmap(x->data, x->size);
	free(x);
}
static const struct wl_buffer_listener buf_listener = { buf_release };

static void popup_rect(void *d, struct zwp_input_popup_surface_v2 *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)d; (void)p; (void)x; (void)y; (void)w; (void)h;
}
static const struct zwp_input_popup_surface_v2_listener popup_listener = { popup_rect };

void wl_show(int w, int h)
{
	if (!im || !comp || !shm || w <= 0 || h <= 0) return;
	if (!psurf) {
		psurf = wl_compositor_create_surface(comp);
		popup = zwp_input_method_v2_get_input_popup_surface(im, psurf);
		zwp_input_popup_surface_v2_add_listener(popup, &popup_listener, NULL);
	}
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("pomelo-input", MFD_CLOEXEC);
	if (fd < 0) return;
	if (ftruncate(fd, (off_t)size) < 0) { close(fd); return; }
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) { close(fd); return; }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct buf *x = calloc(1, sizeof *x);
	x->b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	x->data = data;
	x->size = size;
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(x->b, &buf_listener, x);

	cairo_surface_t *cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, w, h, stride);
	cairo_t *cr = cairo_create(cs);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	/* 圓角：X 那邊用 Shape 切，這裡直接畫成透明 */
	double r = 9;
	cairo_new_sub_path(cr);
	cairo_arc(cr, w - r, r, r, -M_PI / 2, 0);
	cairo_arc(cr, w - r, h - r, r, 0, M_PI / 2);
	cairo_arc(cr, r, h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_clip(cr);
	ui_draw(cr);
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	wl_surface_attach(psurf, x->b, 0, 0);
	wl_surface_damage_buffer(psurf, 0, 0, w, h);
	wl_surface_commit(psurf);
	popup_shown = 1;
	wl_display_flush(dpy);
}

void wl_hide(void)
{
	if (!popup_shown || !psurf) return;
	wl_surface_attach(psurf, NULL, 0, 0);
	wl_surface_commit(psurf);
	popup_shown = 0;
	wl_display_flush(dpy);
}

/* ---- 送出 ---- */

void wl_commit(IC *c, const char *s)
{
	(void)c;
	if (!im || !s || !*s) return;
	zwp_input_method_v2_commit_string(im, s);
	zwp_input_method_v2_commit(im, done_serial);
	wl_display_flush(dpy);
}

/* ---- 鍵盤（抓住的期間所有按鍵都先到這裡） ---- */

static void grab_keymap(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t format, int32_t fd, uint32_t size)
{
	(void)d; (void)g;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) { close(fd); return; }
	struct xkb_keymap *km = xkb_keymap_new_from_buffer(xkb, map, strnlen(map, size), XKB_KEYMAP_FORMAT_TEXT_V1,
	                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (km) {
		if (xstate) xkb_state_unref(xstate);
		if (keymap) xkb_keymap_unref(keymap);
		keymap = km;
		xstate = xkb_state_new(km);
		/* 送回程式的虛擬鍵盤用同一份配置 */
		if (vk) { zwp_virtual_keyboard_v1_keymap(vk, format, fd, size); vk_has_keymap = 1; }
	}
	close(fd);
}

static int eats(void)
{
	return ic && active && purpose != CONTENT_PASSWORD && purpose != CONTENT_PIN;
}

static void grab_key(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t serial, uint32_t time,
                     uint32_t key, uint32_t state)
{
	(void)d; (void)g; (void)serial;
	int press = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (key < sizeof forwarded && !press && rep_key == key + 1) rep_key = 0;
	if (!xstate || !eats()) { bypass_key(time, key, state); return; }
	xkb_keysym_t ks = xkb_state_key_get_one_sym(xstate, key + 8);
	int ate = ic_key(ic, ks, x_state(), press);
	if (key >= sizeof forwarded) { if (!ate) bypass_key(time, key, state); return; }
	if (press) {
		rep_key = 0;
		if (ate) {
			forwarded[key] = 0;
			if (xkb_keymap_key_repeats(keymap, key + 8) && rep_rate > 0) { rep_key = key + 1; rep_next = now_ms() + rep_delay; }
		} else { forwarded[key] = 1; bypass_key(time, key, state); }
	} else if (forwarded[key]) {   /* 按下時送回去的，放開也送回去；按下被吃掉的，放開也吃掉 */
		forwarded[key] = 0;
		bypass_key(time, key, state);
	}
}

static void grab_modifiers(void *d, struct zwp_input_method_keyboard_grab_v2 *g, uint32_t serial,
                           uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)d; (void)g; (void)serial;
	if (xstate) xkb_state_update_mask(xstate, dep, lat, lock, 0, 0, group);
	if (vk && vk_has_keymap) zwp_virtual_keyboard_v1_modifiers(vk, dep, lat, lock, group);
}

static void grab_repeat(void *d, struct zwp_input_method_keyboard_grab_v2 *g, int32_t rate, int32_t delay)
{
	(void)d; (void)g;
	rep_rate = rate;
	rep_delay = delay;
}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
	grab_keymap, grab_key, grab_modifiers, grab_repeat,
};

int wl_timeout(void)
{
	if (!rep_key) return -1;
	long t = rep_next - now_ms();
	return t < 0 ? 0 : (int)t;
}

void wl_tick(void)
{
	if (!rep_key || now_ms() < rep_next) return;
	if (!eats() || !xstate) { rep_key = 0; return; }
	xkb_keysym_t ks = xkb_state_key_get_one_sym(xstate, rep_key - 1 + 8);
	ic_key(ic, ks, x_state(), 1);
	rep_next = now_ms() + 1000 / rep_rate;
}

static void release_grab(void)
{
	if (kgrab) { zwp_input_method_keyboard_grab_v2_release(kgrab); kgrab = NULL; }
	/* 還按著的、已經送回程式的鍵：補送放開，不然程式以為一直按著 */
	for (unsigned k = 0; k < sizeof forwarded; k++)
		if (forwarded[k]) { forwarded[k] = 0; bypass_key(0, k, WL_KEYBOARD_KEY_STATE_RELEASED); }
	rep_key = 0;
}

/* ---- input-method 的事件（done 時才生效） ---- */

static void im_activate(void *d, struct zwp_input_method_v2 *m) { (void)d; (void)m; pend_active = 1; pend_purpose = 0; }
static void im_deactivate(void *d, struct zwp_input_method_v2 *m) { (void)d; (void)m; pend_active = 0; }
static void im_surrounding(void *d, struct zwp_input_method_v2 *m, const char *t, uint32_t c, uint32_t a) { (void)d; (void)m; (void)t; (void)c; (void)a; }
static void im_cause(void *d, struct zwp_input_method_v2 *m, uint32_t c) { (void)d; (void)m; (void)c; }
static void im_content(void *d, struct zwp_input_method_v2 *m, uint32_t hint, uint32_t p) { (void)d; (void)m; (void)hint; pend_purpose = (int)p; }

static void im_done(void *d, struct zwp_input_method_v2 *m)
{
	(void)d; (void)m;
	done_serial++;
	purpose = pend_purpose;
	if (pend_active == active) return;
	active = pend_active;
	if (active) {
		if (!kgrab) {
			kgrab = zwp_input_method_v2_grab_keyboard(im);
			zwp_input_method_keyboard_grab_v2_add_listener(kgrab, &grab_listener, NULL);
		}
		ic_focus(ic, 1);
	} else {
		ic_reset(ic);       /* 組到一半的字丟掉（compositor 會把程式那邊的組字區清掉） */
		ic_focus(ic, 0);
		wl_hide();
		release_grab();
	}
	wl_display_flush(dpy);
}

static void im_unavailable(void *d, struct zwp_input_method_v2 *m)
{
	(void)d; (void)m;
	fputs("pomelo-input: 已經有別的輸入法接上 compositor，原生 Wayland 程式不經過 Pomelo 輸入法\n", stderr);
	zwp_input_method_v2_destroy(im);
	im = NULL;
}

static const struct zwp_input_method_v2_listener im_listener = {
	im_activate, im_deactivate, im_surrounding, im_cause, im_content, im_done, im_unavailable,
};

/* ---- 選字列上的滑鼠 ---- */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)serial;
	pointer_in = s == psurf;
	if (pointer_in) ui_hover(wl_fixed_to_int(x), wl_fixed_to_int(y));
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) { (void)d; (void)p; (void)serial; (void)s; pointer_in = 0; }
static int px;
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)t;
	px = wl_fixed_to_int(x);
	if (pointer_in) ui_hover(px, wl_fixed_to_int(y));
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st)
{
	(void)d; (void)p; (void)serial; (void)t;
	if (pointer_in && b == BTN_LEFT && st == WL_POINTER_BUTTON_STATE_PRESSED) ui_click(px);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
	(void)d; (void)p; (void)t;
	if (pointer_in && axis == WL_POINTER_AXIS_VERTICAL_SCROLL && v) ui_scroll(v > 0);
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis,
	.frame = ptr_frame, .axis_source = ptr_source, .axis_stop = ptr_stop, .axis_discrete = ptr_discrete,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_release(pointer);
		pointer = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name)) imm = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) vkm = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

int wl_start(void)
{
	if (!getenv("WAYLAND_DISPLAY")) return -1;
	if (!(dpy = wl_display_connect(NULL))) return -1;
	struct wl_registry *r = wl_display_get_registry(dpy);
	wl_registry_add_listener(r, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!imm || !vkm || !seat) {   /* compositor 沒有輸入法協定（例如別的 compositor）：只用 XIM／IBus */
		wl_display_disconnect(dpy);
		dpy = NULL;
		return -1;
	}
	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	ic = ic_new();
	if (ic) ic->wl = 1;
	vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);
	im = zwp_input_method_manager_v2_get_input_method(imm, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);
	wl_display_roundtrip(dpy);
	return 0;
}

int wl_fds(void *rfds, int maxfd)
{
	if (!dpy) return maxfd;
	while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
	wl_display_flush(dpy);
	int fd = wl_display_get_fd(dpy);
	FD_SET(fd, (fd_set *)rfds);
	return fd > maxfd ? fd : maxfd;
}

/* select 之後一定要呼叫（prepare_read 要配 read_events 或 cancel_read） */
void wl_handle(void *rfds)
{
	if (!dpy) return;
	if (FD_ISSET(wl_display_get_fd(dpy), (fd_set *)rfds)) {
		if (wl_display_read_events(dpy) < 0) {
			fputs("pomelo-input: 跟 compositor 的連線斷了\n", stderr);
			wl_display_disconnect(dpy);
			dpy = NULL;
			im = NULL;
			return;
		}
	} else wl_display_cancel_read(dpy);
	wl_display_dispatch_pending(dpy);
	wl_display_flush(dpy);
}
