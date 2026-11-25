/* SPDX-License-Identifier: GPL-3.0-or-later */
/* wl.c — libpomelo 的原生 Wayland 後端（Wayland 轉移階段 2，0.40 起）
 *
 * pm_ui_run 這類程式在 main() 設 pm_ui_native = 1 時，在 Wayland 工作階段直接用 Wayland 開視窗，不經過 XWayland：
 *   視窗：xdg_toplevel，外框請 compositor 畫（xdg-decoration 伺服器端，跟 X 程式一樣是 Pomelo 的標題列）；
 *   畫面：一樣先用 cairo 畫在記憶體影像，再交給 wl_shm（最多三張輪流用，compositor 用完才重畫那一張）；
 *   鍵盤：xkbcommon 轉成跟 X 一樣的 keysym 與修飾鍵位元（程式裡比對 XK_* 的程式碼不用改），按住不放自己重複；
 *   滑鼠：cursor-shape 協定（游標圖由 compositor 畫），滾輪與觸控板換成「上／下一格」；
 *   輸入法：text-input-v3（compositor 轉給 pomelo-input），文字輸入框有焦點時開、游標位置一起告訴它；
 *   選單：xdg_popup（compositor 擺位置、點外面自動關），畫法跟 X 的 pm_popup 共用；
 *   設定改了：inotify 監看 settings.conf（Wayland 程式收不到 X 根視窗的 _POMELO_SETTINGS）。
 * X 的程式碼路徑完全沒動；這裡只在程式明講可以時才用（見 pomelo-widgets.h 的 pm_ui_native）。 */
#define _GNU_SOURCE
#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "pomelo-workspace-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"

struct buf { struct wl_buffer *b; void *data; size_t size; int w, h, busy; };
static void dnd_forget(PmWl *win);

struct PmWl {
	struct wl_surface *surf;
	struct xdg_surface *xs;
	struct xdg_toplevel *top;
	struct xdg_popup *popup;
	struct zxdg_toplevel_decoration_v1 *deco;
	struct zwlr_layer_surface_v1 *layer;   /* 桌面外殼用的 layer-shell 視窗（這時沒有 xs） */
	struct wp_viewport *vp;                /* pmwl_clear：1×1 的透明點放大成整個視窗 */
	struct ext_session_lock_surface_v1 *lsurf;   /* 鎖定畫面（pmwl_lock_surface） */
	struct wl_output *out;                 /* 鎖定畫面、指定螢幕的 layer 視窗：在哪個螢幕 */
	int w, h, pend_w, pend_h, configured;
	struct wl_subsurface *sub;             /* pmwl_subsurface：貼在別的視窗上的小畫面（影片、虛擬機器畫面） */
	int cursor;                            /* pmwl_cursor：游標形狀 */
	struct buf cimg; int chx, chy;         /* pmwl_cursor_image：自訂游標圖（cimg.b 不是 NULL 就用這張） */
	struct wl_callback *frame_cb;          /* pmwl_frame_request：已送出、還在等 compositor 回的 frame callback */
	int frame_req;                         /* 要求了：下一次 pmwl_end 時掛上 frame callback */
	long frame_at;                         /* 最後一次掛上的時間 */
	int fullscreen, maximized;             /* compositor 最近一次 configure 給的狀態 */
	unsigned want;                         /* pmwl_want：要收的額外事件（PMWL_IN_*） */
	char **dnd_mimes;                      /* pmwl_dnd_target：這個視窗收得下的格式 */
	int dnd_n;
	unsigned dnd_actions;
	struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor;
	struct zwp_idle_inhibitor_v1 *idle_inhibitor;
	struct buf bufs[3];
	cairo_surface_t *cs;    /* pmwl_begin 給出去、pmwl_end 收回來的那張 */
	struct buf *cur;
	long last_commit;       /* 最後一次 pmwl_end 的時間：閒置一陣子後放掉多的緩衝（trim_idle_buffers） */
	PmWl *next;
};

static struct wl_display *wd;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct zxdg_decoration_manager_v1 *decomgr;
static struct wl_seat *seat;
static struct wl_subcompositor *subcomp;
static struct zwp_pointer_constraints_v1 *pconstraints;
static struct zwp_relative_pointer_manager_v1 *relmgr;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_mgr;
static struct zwp_idle_inhibit_manager_v1 *idle_mgr;
static struct ext_idle_notifier_v1 *idle_notifier;
static struct wl_pointer *ptr;
static struct wl_keyboard *kbd;
static struct wp_cursor_shape_manager_v1 *shape_mgr;
static struct wp_cursor_shape_device_v1 *shape_dev;
static struct zwp_text_input_manager_v3 *ti_mgr;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zxdg_output_manager_v1 *xo_mgr;
static struct wp_viewporter *viewporter;
static struct ext_session_lock_manager_v1 *lock_mgr;
static struct zwlr_screencopy_manager_v1 *scm;
static struct zwlr_output_manager_v1 *omgr;
static struct zwlr_output_power_manager_v1 *opm;
static struct wl_registry *reg;
static uint32_t ftl_name, ftl_ver;   /* 視窗清單的全域物件：有人要用才綁（pmelo_toplevels_start） */
static struct zwlr_foreign_toplevel_manager_v1 *ftl_mgr;
static uint32_t ws_name;   /* 多桌面的全域物件 */
static struct pomelo_workspace_manager_v1 *ws_mgr;
static int ws_count, ws_cur, ws_changed;
static int tl_changed;
static struct ext_session_lock_v1 *lock_obj;
static int lock_state;   /* 0 等待、1 鎖好了、-1 被拒絕或結束 */

/* 每個螢幕：xdg-output 給的邏輯位置與大小（跟 X 那邊的全域座標一樣的座標系） */
#define MAXOUT 8
static struct out { struct wl_output *o; struct zxdg_output_v1 *xo; uint32_t name; int x, y, w, h; } outs[MAXOUT];
static int nouts;
static struct zwp_text_input_v3 *ti;
static struct xkb_context *xkb;
static struct xkb_keymap *keymap;
static struct xkb_state *xstate;
static PmWl *wins, *ptr_focus, *kb_focus;
static double px, py, scroll_acc, scroll_acc_h;
static uint32_t last_serial, btn_serial;   /* btn_serial：最近一次按下滑鼠的序號（拖放要用） */
static int rep_rate = 25, rep_delay = 600;
static uint32_t rep_key;          /* evdev 代碼＋1；0 = 沒有在重複 */
static long rep_next;
static int ino = -1;
static int connected = -1;        /* -1 還沒試、0 不行、1 可以 */

/* 文字輸入（text-input-v3）：程式要的狀態，跟送出去的狀態 */
static PmWl *ime_win;
static int ime_on, ime_x, ime_y, ime_h, ime_enabled, ime_sent_x = -1, ime_sent_y = -1;
static char ime_commit[512];
static char ime_pre[512], ime_pre_new[512];
static int ime_pre_b, ime_pre_e, ime_pre_b_new, ime_pre_e_new;
static int ime_purpose;   /* pmwl_ime_purpose：zwp_text_input_v3 的 content purpose（PMWL_IME_*） */

#define QN 256
static PmwEvent q[QN];
static int qh, qt;

static void push(const PmwEvent *e)
{
	int nt = (qt + 1) % QN;
	if (nt == qh) return;   /* 滿了（不會發生：每次取一個就處理一個） */
	q[qt] = *e;
	qt = nt;
}

static void push_simple(int type, PmWl *w)
{
	PmwEvent e = { .type = type, .win = w };
	push(&e);
}

static long now_ms(void) { return pm_now_ms(); }

/* ---- 視窗 ---- */

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	PmWl *w = d;
	xdg_surface_ack_configure(xs, serial);
	if (w->pend_w > 0 && w->pend_h > 0 && (w->pend_w != w->w || w->pend_h != w->h)) {
		w->w = w->pend_w; w->h = w->pend_h;
		PmwEvent e = { .type = PMW_SIZE, .win = w, .w = w->w, .h = w->h };
		push(&e);
	}
	if (!w->configured) { w->configured = 1; }
	push_simple(PMW_EXPOSE, w);
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void top_configure(void *d, struct xdg_toplevel *t, int32_t width, int32_t height, struct wl_array *states)
{
	(void)t;
	PmWl *w = d;
	w->pend_w = width; w->pend_h = height;   /* 0＝程式自己決定（保持現在的大小） */
	w->fullscreen = w->maximized = 0;
	uint32_t *st;
	wl_array_for_each(st, states) {
		if (*st == XDG_TOPLEVEL_STATE_FULLSCREEN) w->fullscreen = 1;
		else if (*st == XDG_TOPLEVEL_STATE_MAXIMIZED) w->maximized = 1;
	}
}
static void top_close(void *d, struct xdg_toplevel *t) { (void)t; push_simple(PMW_CLOSE, d); }
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) { (void)d; (void)t; (void)w; (void)h; }
static void top_caps(void *d, struct xdg_toplevel *t, struct wl_array *a) { (void)d; (void)t; (void)a; }
static const struct xdg_toplevel_listener top_listener = { top_configure, top_close, top_bounds, top_caps };

static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)p; (void)x; (void)y;
	PmWl *win = d;
	win->pend_w = w; win->pend_h = h;
}
static void popup_done(void *d, struct xdg_popup *p) { (void)p; push_simple(PMW_CLOSE, d); }
static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t t) { (void)d; (void)p; (void)t; }
static const struct xdg_popup_listener popup_listener = { popup_configure, popup_done, popup_repositioned };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static PmWl *new_surface(int w, int h, int xdg)
{
	PmWl *win = calloc(1, sizeof *win);
	if (!win) return NULL;
	win->w = w; win->h = h;
	win->surf = wl_compositor_create_surface(comp);
	wl_surface_set_user_data(win->surf, win);
	if (xdg) {
		win->xs = xdg_wm_base_get_xdg_surface(wm, win->surf);
		xdg_surface_add_listener(win->xs, &xs_listener, win);
	}
	win->next = wins;
	wins = win;
	return win;
}

/* 等 compositor 送第一個 configure（之前不能畫） */
static void wait_configured(PmWl *win)
{
	for (int i = 0; i < 100 && !win->configured; i++)
		if (wl_display_roundtrip(wd) < 0) break;
}

PmWl *pmwl_window(const char *title, const char *app_id, int w, int h, int minw, int minh, PmWl *parent)
{
	if (!pmwl_connect()) return NULL;
	PmWl *win = new_surface(w, h, 1);
	if (!win) return NULL;
	win->top = xdg_surface_get_toplevel(win->xs);
	xdg_toplevel_add_listener(win->top, &top_listener, win);
	xdg_toplevel_set_title(win->top, title ? title : "");
	xdg_toplevel_set_app_id(win->top, app_id ? app_id : "pomelo");
	if (minw > 0 && minh > 0) xdg_toplevel_set_min_size(win->top, minw, minh);
	if (parent && parent->top) xdg_toplevel_set_parent(win->top, parent->top);
	if (decomgr) {   /* 外框請 compositor 畫：跟 X 程式一樣的 Pomelo 標題列 */
		win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(decomgr, win->top);
		zxdg_toplevel_decoration_v1_set_mode(win->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(win->surf);
	wait_configured(win);
	return win;
}

static void buf_free(struct buf *b)
{
	if (!b->b) return;
	wl_buffer_destroy(b->b);
	munmap(b->data, b->size);
	memset(b, 0, sizeof *b);
}

void pmwl_destroy(PmWl *win)
{
	if (!win) return;
	if (ptr_focus == win) ptr_focus = NULL;
	if (kb_focus == win) kb_focus = NULL;
	if (ime_win == win) { pmwl_ime(win, 0, 0, 0, 0); ime_win = NULL; }
	pmwl_pointer_lock(win, 0);
	pmwl_shortcuts_inhibit(win, 0);
	pmwl_idle_inhibit(win, 0);
	pmwl_dnd_target(win, NULL, 0, 0);
	dnd_forget(win);
	if (win->sub) wl_subsurface_destroy(win->sub);
	for (PmWl **p = &wins; *p; p = &(*p)->next) if (*p == win) { *p = win->next; break; }
	if (win->deco) zxdg_toplevel_decoration_v1_destroy(win->deco);
	if (win->top) xdg_toplevel_destroy(win->top);
	if (win->popup) xdg_popup_destroy(win->popup);
	if (win->vp) wp_viewport_destroy(win->vp);
	if (win->lsurf) ext_session_lock_surface_v1_destroy(win->lsurf);
	if (win->layer) zwlr_layer_surface_v1_destroy(win->layer);
	if (win->xs) xdg_surface_destroy(win->xs);
	wl_surface_destroy(win->surf);
	for (int i = 0; i < 3; i++) buf_free(&win->bufs[i]);
	buf_free(&win->cimg);
	if (win->frame_cb) wl_callback_destroy(win->frame_cb);
	/* 佇列裡還有這個視窗的事件：拿掉，免得之後用到已經釋放的指標 */
	for (int i = qh; i != qt; i = (i + 1) % QN) if (q[i].win == win) q[i].type = PMW_NONE;
	free(win);
	wl_display_flush(wd);
}

/* ---- 桌面外殼：layer-shell 視窗與螢幕 ---- */

static int buf_alloc(struct buf *b, int w, int h);

static void layer_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
	PmWl *win = d;
	zwlr_layer_surface_v1_ack_configure(l, serial);
	if (w > 0 && h > 0 && ((int)w != win->w || (int)h != win->h)) {
		win->w = (int)w; win->h = (int)h;
		PmwEvent e = { .type = PMW_SIZE, .win = win, .w = win->w, .h = win->h };
		push(&e);
	}
	win->configured = 1;
	push_simple(PMW_EXPOSE, win);
}
static void layer_closed(void *d, struct zwlr_layer_surface_v1 *l) { (void)l; push_simple(PMW_CLOSE, d); }
static const struct zwlr_layer_surface_v1_listener layer_listener = { layer_configure, layer_closed };

int pmwl_has_layer(void) { return pmwl_connect() && layer_shell; }

PmWl *pmwl_layer(const char *ns, int layer, int output, unsigned anchor, int w, int h,
                 int mt, int mr, int mb, int ml, int exclusive, int kb)
{
	if (!pmwl_has_layer()) return NULL;
	PmWl *win = new_surface(w, h, 0);
	if (!win) return NULL;
	struct wl_output *o = output >= 0 && output < nouts ? outs[output].o : NULL;   /* NULL：compositor 決定（游標所在的螢幕） */
	win->out = o;
	win->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, win->surf, o, (uint32_t)layer, ns);
	zwlr_layer_surface_v1_add_listener(win->layer, &layer_listener, win);
	zwlr_layer_surface_v1_set_size(win->layer, (uint32_t)MAX(w, 0), (uint32_t)MAX(h, 0));
	zwlr_layer_surface_v1_set_anchor(win->layer, anchor);
	zwlr_layer_surface_v1_set_margin(win->layer, mt, mr, mb, ml);
	zwlr_layer_surface_v1_set_exclusive_zone(win->layer, exclusive);
	zwlr_layer_surface_v1_set_keyboard_interactivity(win->layer, (uint32_t)kb);
	wl_surface_commit(win->surf);
	wait_configured(win);
	return win;
}

/* 整個視窗透明、只用來接滑鼠（例如彈出式下面墊的那張）：用 1×1 的透明點放大，不用配一整個螢幕大的緩衝 */
void pmwl_clear(PmWl *win)
{
	if (!win || win->w <= 0 || win->h <= 0) return;
	if (!viewporter) { cairo_t *cr = pmwl_begin(win, NULL, NULL); pmwl_end(win, cr); return; }
	struct buf *b = &win->bufs[0];
	if (!b->b || b->w != 1 || b->h != 1) buf_alloc(b, 1, 1);
	if (!b->b) return;
	memset(b->data, 0, 4);
	if (!win->vp) win->vp = wp_viewporter_get_viewport(viewporter, win->surf);
	wp_viewport_set_destination(win->vp, win->w, win->h);
	wl_surface_attach(win->surf, b->b, 0, 0);
	wl_surface_damage_buffer(win->surf, 0, 0, 1, 1);
	wl_surface_commit(win->surf);
	wl_display_flush(wd);
}

void pmwl_size(PmWl *win, int *w, int *h)
{
	if (w) *w = win ? win->w : 0;
	if (h) *h = win ? win->h : 0;
}

/* ---- 一般視窗的屬性：標題、應用程式 ID、大小限制、全螢幕 ---- */

void pmwl_set_title(PmWl *win, const char *title) { if (win && win->top) { xdg_toplevel_set_title(win->top, title ? title : ""); wl_display_flush(wd); } }
void pmwl_set_app_id(PmWl *win, const char *app_id) { if (win && win->top) { xdg_toplevel_set_app_id(win->top, app_id ? app_id : "pomelo"); wl_display_flush(wd); } }
void pmwl_set_min_size(PmWl *win, int w, int h)
{
	if (!win || !win->top) return;
	xdg_toplevel_set_min_size(win->top, MAX(w, 0), MAX(h, 0));
	wl_surface_commit(win->surf);
	wl_display_flush(wd);
}
void pmwl_set_max_size(PmWl *win, int w, int h)
{
	if (!win || !win->top) return;
	xdg_toplevel_set_max_size(win->top, MAX(w, 0), MAX(h, 0));   /* 0 = 不限 */
	wl_surface_commit(win->surf);
	wl_display_flush(wd);
}
void pmwl_fullscreen(PmWl *win, int on, int output)
{
	if (!win || !win->top) return;
	if (on) xdg_toplevel_set_fullscreen(win->top, output >= 0 && output < nouts ? outs[output].o : NULL);
	else xdg_toplevel_unset_fullscreen(win->top);
	wl_display_flush(wd);
}
void pmwl_maximize(PmWl *win, int on)
{
	if (!win || !win->top) return;
	if (on) xdg_toplevel_set_maximized(win->top); else xdg_toplevel_unset_maximized(win->top);
	wl_display_flush(wd);
}
void pmwl_minimize(PmWl *win) { if (win && win->top) { xdg_toplevel_set_minimized(win->top); wl_display_flush(wd); } }
int pmwl_is_fullscreen(PmWl *win) { return win && win->fullscreen; }
int pmwl_is_maximized(PmWl *win) { return win && win->maximized; }
void pmwl_want(PmWl *win, unsigned flags) { if (win) win->want = flags; }

/* 貼在別的視窗上的小畫面（影片、虛擬機器的畫面）：自己畫、自己送（pmwl_begin／pmwl_end），位置相對於 parent 的左上角，
 * 滑鼠與鍵盤事件的 win 是這個小畫面（座標相對它自己）。不同步：送了馬上顯示，不用等 parent */
PmWl *pmwl_subsurface(PmWl *parent, int x, int y, int w, int h)
{
	if (!parent || !subcomp) return NULL;
	PmWl *win = new_surface(w, h, 0);
	if (!win) return NULL;
	win->sub = wl_subcompositor_get_subsurface(subcomp, win->surf, parent->surf);
	wl_subsurface_set_position(win->sub, x, y);
	wl_subsurface_set_desync(win->sub);
	wl_surface_commit(parent->surf);
	win->configured = 1;
	return win;
}
void pmwl_sub_move(PmWl *sub, PmWl *parent, int x, int y)
{
	if (!sub || !sub->sub) return;
	wl_subsurface_set_position(sub->sub, x, y);
	if (parent) wl_surface_commit(parent->surf);   /* 位置在 parent 下一次 commit 才生效 */
	wl_display_flush(wd);
}
void pmwl_sub_resize(PmWl *sub, int w, int h) { if (sub) { sub->w = w; sub->h = h; } }

/* 改邊距（例如通知一則一則往下排）：馬上生效 */
void pmwl_layer_margin(PmWl *win, int mt, int mr, int mb, int ml)
{
	if (!win || !win->layer) return;
	zwlr_layer_surface_v1_set_margin(win->layer, mt, mr, mb, ml);
	wl_surface_commit(win->surf);
	wl_display_flush(wd);
}

/* 所有螢幕的位置與大小（全域座標）；回傳個數 */
int pmwl_outputs(Rect *r, int max)
{
	int n = 0;
	for (int i = 0; i < nouts && n < max; i++) r[n++] = (Rect){ outs[i].x, outs[i].y, outs[i].w, outs[i].h };
	return n;
}

/* 程式自己改大小（例如控制中心依內容調整高度）：下一次 pmwl_end 送出時一起生效 */
void pmwl_layer_resize(PmWl *win, int w, int h)
{
	if (!win || !win->layer) return;
	win->w = w; win->h = h;
	zwlr_layer_surface_v1_set_size(win->layer, (uint32_t)w, (uint32_t)h);
}

/* 只有這一塊收得到滑鼠（其他地方點下去會穿過去，給後面的視窗）；w 或 h 是 0 = 整個都收 */
void pmwl_input_region(PmWl *win, int x, int y, int w, int h)
{
	if (!win) return;
	if (w <= 0 || h <= 0) { wl_surface_set_input_region(win->surf, NULL); return; }
	struct wl_region *r = wl_compositor_create_region(comp);
	wl_region_add(r, x, y, w, h);
	wl_surface_set_input_region(win->surf, r);
	wl_region_destroy(r);
}

/* 只記下新的邊距、不送出：下一次 pmwl_end 跟新畫面一起生效（小柚移動＋重畫要同一格換上去） */
void pmwl_layer_margin_defer(PmWl *win, int mt, int mr, int mb, int ml)
{
	if (!win || !win->layer) return;
	zwlr_layer_surface_v1_set_margin(win->layer, mt, mr, mb, ml);
}

/* 可點的範圍用 1 位元的遮罩（每列 (w+7)/8 位元組，位元 0 是最左邊）：連續的點合成一段一段的矩形 */
void pmwl_input_mask(PmWl *win, const unsigned char *bits, int w, int h)
{
	if (!win || !bits || w <= 0 || h <= 0) return;
	struct wl_region *r = wl_compositor_create_region(comp);
	int bpl = (w + 7) / 8;
	for (int y = 0; y < h; y++) {
		const unsigned char *row = bits + (size_t)y * bpl;
		int x = 0;
		while (x < w) {
			while (x < w && !(row[x / 8] & (1 << (x & 7)))) x++;
			int x0 = x;
			while (x < w && (row[x / 8] & (1 << (x & 7)))) x++;
			if (x > x0) wl_region_add(r, x0, y, x - x0, 1);
		}
	}
	wl_surface_set_input_region(win->surf, r);
	wl_region_destroy(r);
}

void pmwl_layer_exclusive(PmWl *win, int exclusive)
{
	if (!win || !win->layer) return;
	zwlr_layer_surface_v1_set_exclusive_zone(win->layer, exclusive);
	wl_surface_commit(win->surf);
	wl_display_flush(wd);
}
void pmwl_layer_kb(PmWl *win, int kb)
{
	if (!win || !win->layer) return;
	zwlr_layer_surface_v1_set_keyboard_interactivity(win->layer, (uint32_t)kb);
	wl_surface_commit(win->surf);
	wl_display_flush(wd);
}

/* 全域座標（跟 X 那邊一樣）在哪個螢幕上：回傳編號（給 pmwl_layer 的 output），r 是那個螢幕的位置與大小 */
int pmwl_output_at(int x, int y, Rect *r)
{
	for (int i = 0; i < nouts; i++)
		if (outs[i].w > 0 && x >= outs[i].x && x < outs[i].x + outs[i].w && y >= outs[i].y && y < outs[i].y + outs[i].h) {
			if (r) *r = (Rect){ outs[i].x, outs[i].y, outs[i].w, outs[i].h };
			return i;
		}
	return -1;
}

static void xo_pos(void *d, struct zxdg_output_v1 *xo, int32_t x, int32_t y) { (void)xo; struct out *o = d; o->x = x; o->y = y; }
static void xo_size(void *d, struct zxdg_output_v1 *xo, int32_t w, int32_t h) { (void)xo; struct out *o = d; o->w = w; o->h = h; }
static void xo_done(void *d, struct zxdg_output_v1 *xo) { (void)d; (void)xo; }
static void xo_name(void *d, struct zxdg_output_v1 *xo, const char *n) { (void)d; (void)xo; (void)n; }
static void xo_desc(void *d, struct zxdg_output_v1 *xo, const char *n) { (void)d; (void)xo; (void)n; }
static const struct zxdg_output_v1_listener xo_listener = { xo_pos, xo_size, xo_done, xo_name, xo_desc };

static void out_watch(struct out *o)
{
	if (o->xo || !xo_mgr) return;
	o->xo = zxdg_output_manager_v1_get_xdg_output(xo_mgr, o->o);
	zxdg_output_v1_add_listener(o->xo, &xo_listener, o);
}

/* ---- 螢幕鎖定（ext-session-lock）：compositor 保證鎖住，鎖定程式當掉也一樣 ---- */

static void lock_locked(void *d, struct ext_session_lock_v1 *l) { (void)d; (void)l; lock_state = 1; }
static void lock_finished(void *d, struct ext_session_lock_v1 *l)
{
	(void)d; (void)l;
	if (lock_state == 1) { PmwEvent e = { .type = PMW_CLOSE }; push(&e); }   /* compositor 自己解開了 */
	lock_state = -1;
}
static const struct ext_session_lock_v1_listener lock_listener = { lock_locked, lock_finished };

int pmwl_has_lock(void) { return pmwl_connect() && lock_mgr; }

/* 要求鎖定：1 = 鎖好了（接著幫每個螢幕開 pmwl_lock_surface），0 = 不支援或被拒絕（已經有別的鎖定程式） */
int pmwl_lock(void)
{
	if (!pmwl_has_lock() || lock_obj) return lock_state == 1;
	lock_state = 0;
	lock_obj = ext_session_lock_manager_v1_lock(lock_mgr);
	ext_session_lock_v1_add_listener(lock_obj, &lock_listener, NULL);
	for (int i = 0; i < 100 && lock_state == 0; i++)
		if (wl_display_roundtrip(wd) < 0) break;
	if (lock_state != 1) {
		ext_session_lock_v1_destroy(lock_obj);
		lock_obj = NULL;
		return 0;
	}
	return 1;
}

static void lsurf_configure(void *d, struct ext_session_lock_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h)
{
	PmWl *win = d;
	ext_session_lock_surface_v1_ack_configure(ls, serial);
	if (w > 0 && h > 0 && ((int)w != win->w || (int)h != win->h)) {
		win->w = (int)w; win->h = (int)h;
		PmwEvent e = { .type = PMW_SIZE, .win = win, .w = win->w, .h = win->h };
		push(&e);
	}
	win->configured = 1;
	push_simple(PMW_EXPOSE, win);
}
static const struct ext_session_lock_surface_v1_listener lsurf_listener = { lsurf_configure };

/* 某個螢幕（pmwl_outputs 的編號）的鎖定畫面，大小由 compositor 給（就是螢幕大小） */
PmWl *pmwl_lock_surface(int output)
{
	if (!lock_obj || lock_state != 1 || output < 0 || output >= nouts) return NULL;
	PmWl *win = new_surface(1, 1, 0);
	if (!win) return NULL;
	win->out = outs[output].o;
	win->lsurf = ext_session_lock_v1_get_lock_surface(lock_obj, win->surf, win->out);
	ext_session_lock_surface_v1_add_listener(win->lsurf, &lsurf_listener, win);
	wait_configured(win);
	return win;
}

/* 視窗在哪個螢幕（pmwl_outputs 的編號）；螢幕拔掉了或沒指定 = -1 */
int pmwl_window_output(PmWl *win)
{
	for (int i = 0; win && win->out && i < nouts; i++) if (outs[i].o == win->out) return i;
	return -1;
}

/* 解鎖（之後不能再畫鎖定畫面；程式接著結束） */
void pmwl_unlock(void)
{
	if (!lock_obj) return;
	if (lock_state == 1) ext_session_lock_v1_unlock_and_destroy(lock_obj);
	else ext_session_lock_v1_destroy(lock_obj);
	lock_obj = NULL;
	lock_state = 0;
	wl_display_roundtrip(wd);
}

/* ---- 擷取畫面（wlr-screencopy）：截圖、錄影用，原生視窗、layer 視窗、X 視窗全部都拍得到 ---- */

struct cap {
	uint32_t fmt, w, h, stride;
	int got_buffer, buffer_done, ready, failed, yinvert;
};

static void cf_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
	(void)f;
	struct cap *c = d;
	if (c->got_buffer) return;   /* 只要第一個（shm）格式 */
	c->fmt = fmt; c->w = w; c->h = h; c->stride = stride; c->got_buffer = 1;
}
static void cf_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) { (void)f; ((struct cap *)d)->yinvert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT; }
static void cf_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c2) { (void)f; (void)a; (void)b; (void)c2; ((struct cap *)d)->ready = 1; }
static void cf_failed(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)f; ((struct cap *)d)->failed = 1; }
static void cf_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void cf_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h) { (void)d; (void)f; (void)fmt; (void)w; (void)h; }
static void cf_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)f; ((struct cap *)d)->buffer_done = 1; }
static const struct zwlr_screencopy_frame_v1_listener cf_listener = { cf_buffer, cf_flags, cf_ready, cf_failed, cf_damage, cf_dmabuf, cf_buffer_done };

int pmwl_can_capture(void) { return pmwl_connect() && scm; }

/* 一個螢幕裡的一塊（螢幕內的邏輯座標；緩衝區是實際像素大小；格式換成 cairo 的 RGB24）。錄影時只抓選的範圍 */
static cairo_surface_t *capture_output(struct wl_output *o, int cursor, int rx, int ry, int rw, int rh)
{
	struct cap c = { 0 };
	struct zwlr_screencopy_frame_v1 *f = zwlr_screencopy_manager_v1_capture_output_region(scm, cursor, o, rx, ry, rw, rh);
	zwlr_screencopy_frame_v1_add_listener(f, &cf_listener, &c);
	int v3 = zwlr_screencopy_manager_v1_get_version(scm) >= 3;
	for (int i = 0; i < 50 && !c.failed && !(v3 ? c.buffer_done : c.got_buffer); i++)
		if (wl_display_roundtrip(wd) < 0) break;
	cairo_surface_t *out = NULL;
	int fd = -1;
	void *data = MAP_FAILED;
	size_t size = (size_t)c.stride * c.h;
	int rgb = c.fmt == WL_SHM_FORMAT_XRGB8888 || c.fmt == WL_SHM_FORMAT_ARGB8888;
	int bgr = c.fmt == WL_SHM_FORMAT_XBGR8888 || c.fmt == WL_SHM_FORMAT_ABGR8888;
	if (c.failed || !c.got_buffer || !size || !(rgb || bgr)) goto done;
	if ((fd = memfd_create("pomelo-capture", MFD_CLOEXEC)) < 0 || ftruncate(fd, (off_t)size) < 0) goto done;
	if ((data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) goto done;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, (int32_t)c.w, (int32_t)c.h, (int32_t)c.stride, c.fmt);
	wl_shm_pool_destroy(pool);
	zwlr_screencopy_frame_v1_copy(f, b);
	for (int i = 0; i < 200 && !c.ready && !c.failed; i++)
		if (wl_display_dispatch(wd) < 0) break;
	if (c.ready) {
		out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)c.w, (int)c.h);
		unsigned char *dst = cairo_image_surface_get_data(out);
		int ds = cairo_image_surface_get_stride(out);
		cairo_surface_flush(out);
		for (uint32_t y = 0; y < c.h; y++) {
			const uint32_t *src = (const uint32_t *)((const unsigned char *)data + (size_t)(c.yinvert ? c.h - 1 - y : y) * c.stride);
			uint32_t *row = (uint32_t *)(dst + (size_t)y * ds);
			for (uint32_t x = 0; x < c.w; x++) {
				uint32_t p = src[x];
				row[x] = bgr ? 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff) : p | 0xff000000u;
			}
		}
		cairo_surface_mark_dirty(out);
	}
	wl_buffer_destroy(b);
done:
	if (data != MAP_FAILED) munmap(data, size);
	if (fd >= 0) close(fd);
	zwlr_screencopy_frame_v1_destroy(f);
	return out;
}

/* 全域座標（跟 X 那邊一樣）裡的一塊：每個螢幕各抓一張再拼起來；cursor：要不要畫滑鼠游標。不支援時回傳 NULL */
cairo_surface_t *pmwl_capture(int x, int y, int w, int h, int cursor)
{
	if (!pmwl_can_capture() || w <= 0 || h <= 0) return NULL;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(s);
	pm_rgb(cr, 0x000000);
	cairo_paint(cr);
	int got = 0;
	for (int i = 0; i < nouts; i++) {
		struct out *o = &outs[i];
		int ix = MAX(x, o->x), iy = MAX(y, o->y), ix2 = MIN(x + w, o->x + o->w), iy2 = MIN(y + h, o->y + o->h);
		if (o->w <= 0 || ix2 <= ix || iy2 <= iy) continue;
		cairo_surface_t *img = capture_output(o->o, cursor, ix - o->x, iy - o->y, ix2 - ix, iy2 - iy);
		if (!img) continue;
		cairo_save(cr);
		cairo_translate(cr, ix - x, iy - y);
		cairo_scale(cr, (double)(ix2 - ix) / cairo_image_surface_get_width(img), (double)(iy2 - iy) / cairo_image_surface_get_height(img));
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_surface_destroy(img);
		got = 1;
	}
	cairo_destroy(cr);
	if (!got) { cairo_surface_destroy(s); return NULL; }
	cairo_surface_flush(s);
	return s;
}

/* ---- 顯示器設定（wlr-output-management）與螢幕電源（wlr-output-power-management） ---- */

#define MAXHEAD 8
struct omode { struct zwlr_output_mode_v1 *m; int w, h, refresh, preferred; };
struct ohead {
	struct zwlr_output_head_v1 *h;
	PmwHead info;
	struct omode modes[64];
	int nm, cur;                     /* cur：目前模式在 modes[] 的編號（-1 = 沒有） */
};
static struct ohead heads[MAXHEAD];
static int nheads, om_done;
static uint32_t om_serial;


static void om_size(void *d, struct zwlr_output_mode_v1 *m, int32_t w, int32_t h) { (void)m; struct omode *o = d; o->w = w; o->h = h; }
static void om_refresh(void *d, struct zwlr_output_mode_v1 *m, int32_t r) { (void)m; ((struct omode *)d)->refresh = r; }
static void om_preferred(void *d, struct zwlr_output_mode_v1 *m) { (void)m; ((struct omode *)d)->preferred = 1; }
static void om_finished(void *d, struct zwlr_output_mode_v1 *m) { (void)d; (void)m; }
static const struct zwlr_output_mode_v1_listener omode_listener = { om_size, om_refresh, om_preferred, om_finished };

static void oh_name(void *d, struct zwlr_output_head_v1 *h, const char *n) { (void)h; snprintf(((struct ohead *)d)->info.name, sizeof ((struct ohead *)d)->info.name, "%s", n); }
static void oh_desc(void *d, struct zwlr_output_head_v1 *h, const char *n) { (void)h; snprintf(((struct ohead *)d)->info.desc, sizeof ((struct ohead *)d)->info.desc, "%s", n); }
static void oh_phys(void *d, struct zwlr_output_head_v1 *h, int32_t w, int32_t ht) { (void)d; (void)h; (void)w; (void)ht; }
static void oh_mode(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m)
{
	(void)h;
	struct ohead *o = d;
	if (o->nm >= (int)LEN(o->modes)) { zwlr_output_mode_v1_destroy(m); return; }
	struct omode *om = &o->modes[o->nm++];
	memset(om, 0, sizeof *om);
	om->m = m;
	zwlr_output_mode_v1_add_listener(m, &omode_listener, om);
}
static void oh_enabled(void *d, struct zwlr_output_head_v1 *h, int32_t e) { (void)h; ((struct ohead *)d)->info.enabled = e; }
static void oh_current(void *d, struct zwlr_output_head_v1 *h, struct zwlr_output_mode_v1 *m)
{
	(void)h;
	struct ohead *o = d;
	for (int i = 0; i < o->nm; i++) if (o->modes[i].m == m) o->cur = i;
}
static void oh_pos(void *d, struct zwlr_output_head_v1 *h, int32_t x, int32_t y) { (void)h; ((struct ohead *)d)->info.x = x; ((struct ohead *)d)->info.y = y; }
static void oh_transform(void *d, struct zwlr_output_head_v1 *h, int32_t t) { (void)h; ((struct ohead *)d)->info.transform = t; }
static void oh_scale(void *d, struct zwlr_output_head_v1 *h, wl_fixed_t sc) { (void)h; ((struct ohead *)d)->info.scale = wl_fixed_to_double(sc); }
static void oh_finished(void *d, struct zwlr_output_head_v1 *h)   /* 螢幕拔掉了 */
{
	struct ohead *o = d;
	for (int i = 0; i < o->nm; i++) zwlr_output_mode_v1_destroy(o->modes[i].m);
	zwlr_output_head_v1_destroy(h);
	int k = (int)(o - heads);
	memmove(&heads[k], &heads[k + 1], (size_t)(nheads - k - 1) * sizeof heads[0]);
	nheads--;
	for (int i = k; i < nheads; i++) {   /* 搬過位置：listener 的 user data 要跟著換 */
		zwlr_output_head_v1_set_user_data(heads[i].h, &heads[i]);
		for (int j = 0; j < heads[i].nm; j++) zwlr_output_mode_v1_set_user_data(heads[i].modes[j].m, &heads[i].modes[j]);
	}
}
static void oh_make(void *d, struct zwlr_output_head_v1 *h, const char *n) { (void)d; (void)h; (void)n; }
static void oh_model(void *d, struct zwlr_output_head_v1 *h, const char *n) { (void)d; (void)h; (void)n; }
static void oh_serial(void *d, struct zwlr_output_head_v1 *h, const char *n) { (void)d; (void)h; (void)n; }
static void oh_adaptive(void *d, struct zwlr_output_head_v1 *h, uint32_t a) { (void)d; (void)h; (void)a; }
static const struct zwlr_output_head_v1_listener ohead_listener = {
	oh_name, oh_desc, oh_phys, oh_mode, oh_enabled, oh_current, oh_pos, oh_transform, oh_scale, oh_finished,
	oh_make, oh_model, oh_serial, oh_adaptive,
};

static void omgr_head(void *d, struct zwlr_output_manager_v1 *m, struct zwlr_output_head_v1 *h)
{
	(void)d; (void)m;
	if (nheads >= MAXHEAD) { zwlr_output_head_v1_destroy(h); return; }
	struct ohead *o = &heads[nheads++];
	memset(o, 0, sizeof *o);
	o->h = h;
	o->cur = -1;
	o->info.scale = 1;
	zwlr_output_head_v1_add_listener(h, &ohead_listener, o);
}
static void omgr_done(void *d, struct zwlr_output_manager_v1 *m, uint32_t serial) { (void)d; (void)m; om_serial = serial; om_done = 1; }
static void omgr_finished(void *d, struct zwlr_output_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_output_manager_v1_listener omgr_listener = { omgr_head, omgr_done, omgr_finished };

/* 目前每個螢幕的設定與可以用的解析度；compositor 不支援時回傳 -1 */
int pmwl_heads(PmwHead *out, int max)
{
	if (!pmwl_connect() || !omgr) return -1;
	for (int i = 0; i < 20 && !om_done; i++) if (wl_display_roundtrip(wd) < 0) break;
	wl_display_roundtrip(wd);   /* 最新的狀態 */
	int n = 0;
	for (int i = 0; i < nheads && n < max; i++) {
		struct ohead *o = &heads[i];
		PmwHead *h = &out[n++];
		*h = o->info;
		h->nmodes = 0;
		for (int j = 0; j < o->nm && h->nmodes < (int)LEN(h->modes); j++)
			h->modes[h->nmodes++] = (PmwMode){ o->modes[j].w, o->modes[j].h, o->modes[j].refresh, o->modes[j].preferred };
		h->w = o->cur >= 0 ? o->modes[o->cur].w : 0;
		h->h = o->cur >= 0 ? o->modes[o->cur].h : 0;
		h->refresh = o->cur >= 0 ? o->modes[o->cur].refresh : 0;
	}
	return n;
}

struct oresult { int done, ok; };
static void oc_ok(void *d, struct zwlr_output_configuration_v1 *c) { (void)c; ((struct oresult *)d)->done = 1; ((struct oresult *)d)->ok = 1; }
static void oc_fail(void *d, struct zwlr_output_configuration_v1 *c) { (void)c; ((struct oresult *)d)->done = 1; }
static void oc_cancel(void *d, struct zwlr_output_configuration_v1 *c) { (void)c; ((struct oresult *)d)->done = 1; }
static const struct zwlr_output_configuration_v1_listener oconf_listener = { oc_ok, oc_fail, oc_cancel };

/* 套用設定（用名稱對到螢幕；模式找一樣的寬高與更新率，沒有的話用自訂模式）。test：只測試不套用。1 = 成功 */
int pmwl_heads_apply(const PmwHead *in, int n, int test)
{
	if (!pmwl_connect() || !omgr || !om_done) return 0;
	struct zwlr_output_configuration_v1 *c = zwlr_output_manager_v1_create_configuration(omgr, om_serial);
	struct oresult r = { 0 };
	zwlr_output_configuration_v1_add_listener(c, &oconf_listener, &r);
	for (int i = 0; i < nheads; i++) {
		const PmwHead *h = NULL;
		for (int k = 0; k < n; k++) if (!strcmp(in[k].name, heads[i].info.name)) h = &in[k];
		if (!h) h = &heads[i].info;   /* 沒提到的螢幕：照現在的 */
		if (!h->enabled) { zwlr_output_configuration_v1_disable_head(c, heads[i].h); continue; }
		struct zwlr_output_configuration_head_v1 *ch = zwlr_output_configuration_v1_enable_head(c, heads[i].h);
		struct omode *best = NULL;
		for (int j = 0; j < heads[i].nm; j++) {
			struct omode *m = &heads[i].modes[j];
			if (m->w == h->w && m->h == h->h && (!best || abs(m->refresh - h->refresh) < abs(best->refresh - h->refresh))) best = m;
		}
		if (h->w > 0 && h->h > 0) {
			if (best) zwlr_output_configuration_head_v1_set_mode(ch, best->m);
			else zwlr_output_configuration_head_v1_set_custom_mode(ch, h->w, h->h, h->refresh);
		}
		zwlr_output_configuration_head_v1_set_position(ch, h->x, h->y);
		zwlr_output_configuration_head_v1_set_transform(ch, h->transform);
		zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(h->scale > 0 ? h->scale : 1));
	}
	if (test) zwlr_output_configuration_v1_test(c);
	else zwlr_output_configuration_v1_apply(c);
	for (int i = 0; i < 50 && !r.done; i++) if (wl_display_roundtrip(wd) < 0) break;
	zwlr_output_configuration_v1_destroy(c);
	wl_display_roundtrip(wd);   /* 新的設定（compositor 會再送一次 done） */
	return r.ok;
}

/* 所有螢幕關掉／打開（閒置時關螢幕；有人動滑鼠鍵盤 compositor 也會自己打開） */
int pmwl_screens_power(int on)
{
	if (!pmwl_connect() || !opm) return 0;
	for (int i = 0; i < nouts; i++) {
		struct zwlr_output_power_v1 *p = zwlr_output_power_manager_v1_get_output_power(opm, outs[i].o);
		zwlr_output_power_v1_set_mode(p, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
		zwlr_output_power_v1_destroy(p);
	}
	wl_display_roundtrip(wd);
	return 1;
}

/* ---- 畫面 ---- */

static void buf_release(void *d, struct wl_buffer *b) { (void)b; ((struct buf *)d)->busy = 0; }
static const struct wl_buffer_listener buf_listener = { buf_release };

static int buf_alloc(struct buf *b, int w, int h)
{
	buf_free(b);
	size_t size = (size_t)w * 4 * h;
	int fd = memfd_create("pomelo", MFD_CLOEXEC);
	if (fd < 0) return 0;
	if (ftruncate(fd, (off_t)size) < 0) { close(fd); return 0; }
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) { close(fd); return 0; }
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	b->b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->b, &buf_listener, b);
	b->data = data; b->size = size; b->w = w; b->h = h; b->busy = 0;
	return 1;
}

cairo_t *pmwl_begin(PmWl *win, int *w, int *h)
{
	struct buf *b = NULL;
	for (int tries = 0; !b && tries < 50; tries++) {
		for (int i = 0; i < 3 && !b; i++) if (!win->bufs[i].busy) b = &win->bufs[i];
		if (!b) wl_display_roundtrip(wd);   /* 三張都還在 compositor 手上：等它還一張 */
	}
	if (!b) b = &win->bufs[0];
	/* 大小要等上面的 roundtrip 之後才讀：roundtrip 時可能收到並回覆了 configure（win->w 變了），
	 * 還用舊大小畫的話，回覆之後送出舊大小的畫面，compositor 會當成程式自己改大小而把視窗拉回舊大小 */
	int W = MAX(win->w, 1), H = MAX(win->h, 1);
	if (!b->b || b->w != W || b->h != H) buf_alloc(b, W, H);
	win->cur = b;
	win->cs = b->b ? cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32, W, H, W * 4)
	               : cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
	if (w) *w = W;
	if (h) *h = H;
	return cairo_create(win->cs);
}

/* 畫好了但不要送出（frame() 發現還要再畫一次）：下一次 pmwl_begin 會拿到同一張 */
void pmwl_cancel(PmWl *win, cairo_t *cr)
{
	cairo_destroy(cr);
	cairo_surface_destroy(win->cs);
	win->cs = NULL;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
	PmWl *w = d;
	wl_callback_destroy(cb);
	if (w->frame_cb == cb) w->frame_cb = NULL;
	PmwEvent e = { .type = PMW_FRAME, .win = w, .time = (long)t };
	push(&e);
}
static const struct wl_callback_listener frame_listener = { frame_done };

/* 下一次 pmwl_end 時順便要一個 frame callback；compositor 準備好接下一張畫面時（通常＝螢幕更新一次）收到 PMW_FRAME。
 * 影片、動畫用它對時與節流：回傳 1＝上一個還沒回來（先別畫，等 PMW_FRAME；視窗被遮住或最小化時 compositor 可能很久不回，
 * 所以超過 250 毫秒就當作回來了，回傳 0） */
int pmwl_frame_wait(PmWl *win)
{
	if (!win || !win->frame_cb) return 0;
	if (now_ms() - win->frame_at > 250) return 0;
	return 1;
}
void pmwl_frame_request(PmWl *win) { if (win) win->frame_req = 1; }

void pmwl_end(PmWl *win, cairo_t *cr)
{
	cairo_destroy(cr);
	cairo_surface_flush(win->cs);
	cairo_surface_destroy(win->cs);
	win->cs = NULL;
	struct buf *b = win->cur;
	if (!b || !b->b) return;
	wl_surface_attach(win->surf, b->b, 0, 0);
	wl_surface_damage_buffer(win->surf, 0, 0, b->w, b->h);
	if (win->frame_req && !win->frame_cb) {
		win->frame_cb = wl_surface_frame(win->surf);
		wl_callback_add_listener(win->frame_cb, &frame_listener, win);
		win->frame_at = now_ms();
	}
	win->frame_req = 0;
	wl_surface_commit(win->surf);
	b->busy = 1;
	win->last_commit = now_ms();
	wl_display_flush(wd);
}

static unsigned x_state(void);

/* ---- 滑鼠 ---- */

/* 螢幕保護程式用：游標在我們的視窗上時隱藏（wl_pointer.set_cursor 給 NULL 表面）。要記得最後一次 enter 的序號，
 * 才能在游標已經在視窗上時馬上生效 */
static int cursor_hidden;
static uint32_t enter_serial;
static const int cursor_shapes[] = {
	[PMWL_CURSOR_DEFAULT] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT, [PMWL_CURSOR_TEXT] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
	[PMWL_CURSOR_POINTER] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, [PMWL_CURSOR_GRAB] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB,
	[PMWL_CURSOR_GRABBING] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING, [PMWL_CURSOR_CROSSHAIR] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
	[PMWL_CURSOR_RESIZE_EW] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE, [PMWL_CURSOR_RESIZE_NS] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
	[PMWL_CURSOR_RESIZE_NESW] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE, [PMWL_CURSOR_RESIZE_NWSE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE,
	[PMWL_CURSOR_MOVE] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE, [PMWL_CURSOR_NOT_ALLOWED] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED,
	[PMWL_CURSOR_WAIT] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT, [PMWL_CURSOR_COPY] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY,
	[PMWL_CURSOR_HIDDEN] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
};
static struct wl_surface *cursor_surf;   /* 自訂游標圖共用的 surface（游標一次只有一個） */
static void apply_cursor(struct wl_pointer *p, uint32_t serial)
{
	int c = ptr_focus ? ptr_focus->cursor : 0;
	if (c < 0 || c >= (int)LEN(cursor_shapes)) c = 0;
	if (cursor_hidden || c == PMWL_CURSOR_HIDDEN) wl_pointer_set_cursor(p, serial, NULL, 0, 0);
	else if (ptr_focus && ptr_focus->cimg.b && cursor_surf) {
		/* 自訂游標圖：自己的 surface＋熱點（cursor-shape 做不到） */
		wl_surface_attach(cursor_surf, ptr_focus->cimg.b, 0, 0);
		wl_surface_damage_buffer(cursor_surf, 0, 0, ptr_focus->cimg.w, ptr_focus->cimg.h);
		wl_surface_commit(cursor_surf);
		wl_pointer_set_cursor(p, serial, cursor_surf, ptr_focus->chx, ptr_focus->chy);
	}
	else if (shape_dev) wp_cursor_shape_device_v1_set_shape(shape_dev, serial, (uint32_t)cursor_shapes[c]);
}
/* 這個視窗上的游標形狀（PMWL_CURSOR_*）；游標現在就在它上面的話馬上換。會取消 pmwl_cursor_image 設的圖 */
void pmwl_cursor(PmWl *win, int shape)
{
	if (!win) return;
	int had = win->cimg.b != NULL;
	if (had) buf_free(&win->cimg);
	if (win->cursor == shape && !had) return;
	win->cursor = shape;
	if (ptr && ptr_focus == win && enter_serial) { apply_cursor(ptr, enter_serial); wl_display_flush(wd); }
}
/* 自訂游標圖：img 是 ARGB32 的 cairo 影像（內容會複製），熱點 hot_x、hot_y 是圖上的位置；NULL＝回到 pmwl_cursor 的形狀。
 * 虛擬機器、遠端桌面把來賓的游標畫給使用者看。最大 256×256 */
void pmwl_cursor_image(PmWl *win, cairo_surface_t *img, int hot_x, int hot_y)
{
	if (!win || !pmwl_connect()) return;
	if (!img) { pmwl_cursor(win, win->cursor); return; }
	cairo_surface_flush(img);
	int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
	if (cairo_image_surface_get_format(img) != CAIRO_FORMAT_ARGB32 || w <= 0 || h <= 0 || w > 256 || h > 256) { pmwl_cursor(win, PMWL_CURSOR_DEFAULT); return; }
	if (!cursor_surf) cursor_surf = wl_compositor_create_surface(comp);
	buf_free(&win->cimg);
	if (!buf_alloc(&win->cimg, w, h)) return;
	const unsigned char *src = cairo_image_surface_get_data(img);
	int sst = cairo_image_surface_get_stride(img);
	for (int y = 0; y < h; y++) memcpy((char *)win->cimg.data + (size_t)y * w * 4, src + (size_t)y * sst, (size_t)w * 4);
	win->chx = CLAMP(hot_x, 0, w - 1); win->chy = CLAMP(hot_y, 0, h - 1);
	if (ptr && ptr_focus == win && enter_serial) { apply_cursor(ptr, enter_serial); wl_display_flush(wd); }
}
void pmwl_hide_cursor(int hide)
{
	cursor_hidden = hide;
	if (ptr && ptr_focus && enter_serial) { apply_cursor(ptr, enter_serial); wl_display_flush(wd); }
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	ptr_focus = s ? wl_surface_get_user_data(s) : NULL;
	px = wl_fixed_to_double(x); py = wl_fixed_to_double(y);
	enter_serial = serial;
	apply_cursor(p, serial);
	if (ptr_focus) {
		PmwEvent e = { .type = PMW_ENTER, .win = ptr_focus, .x = (int)px, .y = (int)py, .state = x_state() };
		push(&e);
		e.type = PMW_MOTION;
		push(&e);
	}
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)p; (void)serial;
	PmWl *w = s ? wl_surface_get_user_data(s) : ptr_focus;
	ptr_focus = NULL;
	if (w) { PmwEvent e = { .type = PMW_LEAVE, .win = w, .x = (int)px, .y = (int)py }; push(&e); }
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p;
	px = wl_fixed_to_double(x); py = wl_fixed_to_double(y);
	if (!ptr_focus) return;
	PmwEvent e = { .type = PMW_MOTION, .win = ptr_focus, .x = (int)px, .y = (int)py, .time = t, .state = x_state() };
	push(&e);
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t st)
{
	(void)d; (void)p;
	if (st == WL_POINTER_BUTTON_STATE_PRESSED) { last_serial = serial; btn_serial = serial; }
	if (!ptr_focus) return;
	int bn = b == BTN_LEFT ? 1 : b == BTN_MIDDLE ? 2 : b == BTN_RIGHT ? 3 : b == BTN_SIDE ? 8 : b == BTN_EXTRA ? 9 : 0;
	if (!bn) return;
	PmwEvent e = { .type = st == WL_POINTER_BUTTON_STATE_PRESSED ? PMW_DOWN : PMW_UP, .win = ptr_focus,
	               .x = (int)px, .y = (int)py, .button = bn, .time = t, .state = x_state() };
	push(&e);
}
/* 滾輪：compositor 先送 axis_discrete（幾格），再送 axis（數值）——有格數就照格數，一格捲一次；
 * 觸控板只有連續的數值，累積到 15（跟 XWayland 換成按鍵 4／5 的單位一樣）才算一格。
 * 垂直：按鍵 4（上）、5（下）；水平：按鍵 6（左）、7（右），跟 X 一樣。
 * 視窗用 pmwl_want(PMWL_IN_SCROLL_FINE) 要精細捲動時，改成一個 frame 一個 PMW_SCROLL_FINE（w、h 是水平、垂直的捲動量，
 * 單位 1/256 像素，滾輪一格 = 15 像素）*/
static int discrete[2], has_discrete[2];
static double fine[2];
static void scroll_steps(int n, int horiz, uint32_t t)
{
	for (int i = 0; i < abs(n) && ptr_focus; i++) {
		PmwEvent e = { .type = PMW_SCROLL, .win = ptr_focus, .x = (int)px, .y = (int)py,
		               .button = horiz ? (n < 0 ? 6 : 7) : (n < 0 ? 4 : 5), .time = t };
		push(&e);
	}
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
	(void)d; (void)p;
	int h = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
	if (!h && axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
	double dv = wl_fixed_to_double(v);
	if (ptr_focus && (ptr_focus->want & PMWL_IN_SCROLL_FINE)) { fine[h] += dv; has_discrete[h] = 0; discrete[h] = 0; return; }
	double *acc = h ? &scroll_acc_h : &scroll_acc;
	if (has_discrete[h]) { scroll_steps(discrete[h], h, t); has_discrete[h] = 0; discrete[h] = 0; *acc = 0; return; }
	*acc += dv;
	int n = (int)(*acc / 15);
	*acc -= n * 15;
	scroll_steps(n, h, t);
}
static void ptr_frame(void *d, struct wl_pointer *p)
{
	(void)d; (void)p;
	has_discrete[0] = has_discrete[1] = 0; discrete[0] = discrete[1] = 0;
	if (fine[0] != 0 || fine[1] != 0) {
		if (ptr_focus) {
			PmwEvent e = { .type = PMW_SCROLL_FINE, .win = ptr_focus, .x = (int)px, .y = (int)py,
			               .w = (int)(fine[1] * 256), .h = (int)(fine[0] * 256), .state = x_state() };   /* w：水平、h：垂直 */
			push(&e);
		}
		fine[0] = fine[1] = 0;
	}
}
static void ptr_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; scroll_acc = scroll_acc_h = 0; }
static void ptr_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{
	(void)d; (void)p;
	if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) { discrete[0] += v; has_discrete[0] = 1; }
	else if (a == WL_POINTER_AXIS_HORIZONTAL_SCROLL) { discrete[1] += v; has_discrete[1] = 1; }
}
static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis,
	.frame = ptr_frame, .axis_source = ptr_source, .axis_stop = ptr_stop, .axis_discrete = ptr_discrete,
};

/* ---- 鍵盤 ---- */

/* 鍵盤配置延後編譯（0.41 效能）：xkb_keymap_new_from_buffer 要 20～30 ms CPU，開視窗到第一幀不需要它；
 * 先把 compositor 傳來的文字存起來，第一次真的需要（按鍵、修飾鍵不是全空）才編，並補上之前收到的修飾鍵狀態 */
static char *km_text;
static size_t km_len;
static uint32_t km_dep, km_lat, km_lock, km_group;
static void km_ensure(void)
{
	if (!km_text) return;
	struct xkb_keymap *km = xkb_keymap_new_from_buffer(xkb, km_text, km_len, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	free(km_text);
	km_text = NULL;
	if (!km) return;
	if (xstate) xkb_state_unref(xstate);
	if (keymap) xkb_keymap_unref(keymap);
	keymap = km;
	xstate = xkb_state_new(km);
	xkb_state_update_mask(xstate, km_dep, km_lat, km_lock, 0, 0, km_group);
}

static unsigned x_state(void)
{
	if (km_text) { if (!(km_dep | km_lat | km_lock)) return 0; km_ensure(); }
	if (!xstate) return 0;
	static const struct { const char *name; unsigned bit; } m[] = {
		{ XKB_MOD_NAME_SHIFT, ShiftMask }, { XKB_MOD_NAME_CAPS, LockMask }, { XKB_MOD_NAME_CTRL, ControlMask },
		{ XKB_MOD_NAME_ALT, Mod1Mask }, { XKB_MOD_NAME_NUM, Mod2Mask }, { XKB_MOD_NAME_LOGO, Mod4Mask },
	};
	unsigned s = 0;
	for (unsigned i = 0; i < LEN(m); i++)
		if (xkb_state_mod_name_is_active(xstate, m[i].name, XKB_STATE_MODS_EFFECTIVE) > 0) s |= m[i].bit;
	return s;
}

/* button＝鍵盤的 evdev 代碼（KEY_*，虛擬機器、遠端桌面要原始掃描碼用）；x＝1 表示是按住不放的重複 */
static void key_event(uint32_t key, long t, int up, int repeat)
{
	km_ensure();
	if (!xstate || !kb_focus) return;
	PmwEvent e = { .type = up ? PMW_KEYUP : PMW_KEY, .win = kb_focus, .time = t, .state = x_state(), .button = (int)key, .x = repeat };
	e.key = xkb_state_key_get_one_sym(xstate, key + 8);
	if (!up) {
		e.textlen = xkb_state_key_get_utf8(xstate, key + 8, e.text, sizeof e.text);
		if (e.textlen < 0 || e.textlen >= (int)sizeof e.text) e.textlen = 0;
		e.text[e.textlen] = 0;
	}
	push(&e);
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
	(void)d; (void)k;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) return;
	size_t len = strnlen(map, size);
	free(km_text);
	km_text = malloc(len + 1);   /* 先存著，第一次要用才編 */
	if (km_text) { memcpy(km_text, map, len); km_text[len] = 0; km_len = len; }
	munmap(map, size);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys)
{
	(void)d; (void)k; (void)serial; (void)keys;
	kb_focus = s ? wl_surface_get_user_data(s) : NULL;
	if (kb_focus) push_simple(PMW_FOCUS_IN, kb_focus);
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)k; (void)serial; (void)s;
	if (kb_focus) push_simple(PMW_FOCUS_OUT, kb_focus);
	kb_focus = NULL;
	rep_key = 0;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t st)
{
	(void)d; (void)k;
	last_serial = serial;
	km_ensure();
	if (st != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (rep_key == key + 1) rep_key = 0;
		if (kb_focus && (kb_focus->want & PMWL_IN_KEYUP)) key_event(key, t, 1, 0);   /* 要收放開事件的視窗才給 */
		return;
	}
	key_event(key, t, 0, 0);
	if (keymap && xkb_keymap_key_repeats(keymap, key + 8) && rep_rate > 0) { rep_key = key + 1; rep_next = now_ms() + rep_delay; }
	else rep_key = 0;
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)d; (void)k; (void)serial;
	km_dep = dep; km_lat = lat; km_lock = lock; km_group = group;
	if (xstate && !km_text) xkb_state_update_mask(xstate, dep, lat, lock, 0, 0, group);
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) { (void)d; (void)k; rep_rate = rate; rep_delay = delay; }
static const struct wl_keyboard_listener kb_listener = { kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat };

/* 鍵盤或滑鼠全部拔掉時 compositor 會拿掉那項能力，舊的物件就廢了：要放掉，插回來時再拿新的
 * （不然 USB 鍵盤拔掉再插回去，這個程式就再也收不到按鍵） */
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
		ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(ptr, &ptr_listener, NULL);
		if (shape_mgr) shape_dev = wp_cursor_shape_manager_v1_get_pointer(shape_mgr, ptr);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ptr) {
		if (shape_dev) wp_cursor_shape_device_v1_destroy(shape_dev);
		shape_dev = NULL;
		wl_pointer_release(ptr);
		ptr = NULL;
		ptr_focus = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !kbd) {
		kbd = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(kbd, &kb_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && kbd) {
		if (kb_focus) push_simple(PMW_FOCUS_OUT, kb_focus);
		wl_keyboard_release(kbd);
		kbd = NULL;
		kb_focus = NULL;
		rep_key = 0;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ---- 輸入法（text-input-v3） ---- */

/* 拿到文字輸入焦點：程式重畫時會再呼叫 pmwl_ime 打開 */
static void ti_enter(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s)
{
	(void)d; (void)t;
	ime_enabled = 0; ime_sent_x = -1;
	if (s && wl_surface_get_user_data(s)) push_simple(PMW_FOCUS_IN, wl_surface_get_user_data(s));
}
static void ti_leave(void *d, struct zwp_text_input_v3 *t, struct wl_surface *s) { (void)d; (void)s; if (ime_enabled) { zwp_text_input_v3_disable(t); zwp_text_input_v3_commit(t); } ime_enabled = 0; ime_pre[0] = ime_pre_new[0] = 0; }
/* 預編輯字串（還沒確定的注音／拼音）：done 時才算數，跟確定的字一起交給程式 */
static void ti_preedit(void *d, struct zwp_text_input_v3 *t, const char *text, int32_t b, int32_t e)
{
	(void)d; (void)t;
	snprintf(ime_pre_new, sizeof ime_pre_new, "%s", text ? text : "");
	ime_pre_b_new = b; ime_pre_e_new = e;
}
static void ti_commit_string(void *d, struct zwp_text_input_v3 *t, const char *text)
{
	(void)d; (void)t;
	if (text) snprintf(ime_commit + strlen(ime_commit), sizeof ime_commit - strlen(ime_commit), "%s", text);
}
static void ti_delete(void *d, struct zwp_text_input_v3 *t, uint32_t b, uint32_t a) { (void)d; (void)t; (void)b; (void)a; }
/* 輸入法打好的字：當成一個沒有 keysym、只有文字的按鍵事件（輸入框照樣接到） */
static void ti_done(void *d, struct zwp_text_input_v3 *t, uint32_t serial)
{
	(void)d; (void)t; (void)serial;
	const char *p = ime_commit;
	while (*p && kb_focus) {
		PmwEvent e = { .type = PMW_KEY, .win = kb_focus, .time = now_ms() };
		size_t n = strlen(p);
		if (n >= sizeof e.text) {   /* 一次很長的字串：切成幾段，不切在 UTF-8 字元中間 */
			n = sizeof e.text - 1;
			while (n && ((unsigned char)p[n] & 0xc0) == 0x80) n--;
		}
		memcpy(e.text, p, n);
		e.text[n] = 0;
		e.textlen = (int)n;
		push(&e);
		p += n;
	}
	ime_commit[0] = 0;
	/* 預編輯：跟上一次不一樣才通知（text 空 = 清掉；x、y 是游標在字串裡的位元組範圍，-1 = 沒有）。
	 * 只給 pmwl_want(PMWL_IN_PREEDIT) 的視窗 */
	if (kb_focus && (kb_focus->want & PMWL_IN_PREEDIT) &&
	    (strcmp(ime_pre, ime_pre_new) || ime_pre_b != ime_pre_b_new || ime_pre_e != ime_pre_e_new)) {
		PmwEvent e = { .type = PMW_PREEDIT, .win = kb_focus, .time = now_ms(), .x = ime_pre_b_new, .y = ime_pre_e_new };
		snprintf(e.text, sizeof e.text, "%s", ime_pre_new);
		e.textlen = (int)strlen(e.text);
		push(&e);
	}
	memcpy(ime_pre, ime_pre_new, sizeof ime_pre);
	ime_pre_b = ime_pre_b_new; ime_pre_e = ime_pre_e_new;
}
static const struct zwp_text_input_v3_listener ti_listener = { ti_enter, ti_leave, ti_preedit, ti_commit_string, ti_delete, ti_done };

void pmwl_ime_purpose(int purpose)
{
	if (purpose == ime_purpose) return;
	ime_purpose = purpose;
	ime_sent_x = -1;   /* 下次 pmwl_ime 重送 */
}

void pmwl_ime(PmWl *win, int on, int x, int y, int h)
{
	if (!ti) return;
	ime_win = win; ime_on = on; ime_x = x; ime_y = y; ime_h = h;
	int want = on && kb_focus == win;
	if (want && (!ime_enabled || x != ime_sent_x || y != ime_sent_y)) {
		if (!ime_enabled) zwp_text_input_v3_enable(ti);
		zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
		                                   (uint32_t)ime_purpose);
		zwp_text_input_v3_set_cursor_rectangle(ti, x, y, 1, h);
		zwp_text_input_v3_commit(ti);
		ime_enabled = 1; ime_sent_x = x; ime_sent_y = y;
		wl_display_flush(wd);
	} else if (!want && ime_enabled) {
		zwp_text_input_v3_disable(ti);
		zwp_text_input_v3_commit(ti);
		ime_enabled = 0; ime_sent_x = -1;
		wl_display_flush(wd);
	}
}

/* ---- 資料裝置（wl_data_device）：剪貼簿（任何格式）、primary selection（選取就能中鍵貼上）、拖放 ---- */

static struct wl_data_device_manager *ddm;
static struct wl_data_device *ddev;
static struct zwp_primary_selection_device_manager_v1 *psm;
static struct zwp_primary_selection_device_v1 *pdev;

/* 每個 offer 有什麼格式（offer 的 user data）；拖放的來源動作也記在這裡 */
#define MAXMIME 40
struct offer { char *mimes[MAXMIME]; int n; unsigned src_actions, action; };

/* 文字的格式，前面的比較好 */
static const char *const text_mimes[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING" };

static void offer_add(struct offer *o, const char *mime)
{
	if (!o || o->n >= MAXMIME || !mime) return;
	o->mimes[o->n] = strdup(mime);
	if (o->mimes[o->n]) o->n++;
}
static int offer_has(const struct offer *o, const char *mime)
{
	for (int i = 0; o && i < o->n; i++) if (!strcmp(o->mimes[i], mime)) return 1;
	return 0;
}
static int offer_text(const struct offer *o)   /* 最好的文字格式的編號，-1 = 沒有文字 */
{
	for (unsigned i = 0; i < LEN(text_mimes); i++) if (offer_has(o, text_mimes[i])) return (int)i;
	return -1;
}
static void offer_free(struct offer *o)
{
	if (!o) return;
	for (int i = 0; i < o->n; i++) free(o->mimes[i]);
	free(o);
}
static void data_offer_drop(struct wl_data_offer *o)
{
	if (!o) return;
	offer_free(wl_data_offer_get_user_data(o));
	wl_data_offer_destroy(o);
}
#define INFO(o) ((struct offer *)wl_data_offer_get_user_data(o))

/* 從 fd 讀到結束（最多 max 位元組；對方 idle_ms 毫秒都沒有新資料就放棄）；結尾補 NUL，*len 是資料長度 */
static char *read_all(int fd, size_t *len, size_t max, int idle_ms)
{
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);
	while (buf) {
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, idle_ms) <= 0) break;
		if (n + 2049 > cap) {
			if (cap > max) break;
			char *nb = realloc(buf, cap * 2);
			if (!nb) break;
			buf = nb; cap *= 2;
		}
		ssize_t k = read(fd, buf + n, cap - n - 1);
		if (k <= 0) break;
		n += (size_t)k;
	}
	if (buf) buf[n] = 0;
	if (len) *len = n;
	return buf;
}

static char *offer_receive(struct wl_data_offer *o, const char *mime, size_t *len, size_t max)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) return NULL;
	wl_data_offer_receive(o, mime, p[1]);
	close(p[1]);
	wl_display_flush(wd);
	char *r = read_all(p[0], len, max, 1500);
	close(p[0]);
	return r;
}

/* 寫給對方（資料的接收端）；對方中途關掉管線時不要被 SIGPIPE 結束 */
static void write_fd(int fd, const char *t, size_t n)
{
	struct sigaction old;
	if (sigaction(SIGPIPE, NULL, &old) == 0 && old.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
	while (n > 0) {
		ssize_t k = write(fd, t, n);
		if (k < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN) { struct pollfd pf = { fd, POLLOUT, 0 }; if (poll(&pf, 1, 1000) > 0) continue; }
			break;
		}
		t += k; n -= (size_t)k;
	}
	close(fd);
}

/* 我們放上剪貼簿的東西：每個格式一份（文字會用所有常見的文字格式各放一個） */
struct item { char *mime; char *data; size_t len; int owner; };
static struct item clip_items[12];
static int nclip;
static struct wl_data_source *my_src;

static void clip_clear(void)
{
	for (int i = 0; i < nclip; i++) { free(clip_items[i].mime); if (clip_items[i].owner) free(clip_items[i].data); }
	nclip = 0;
}

/* 拖放（我們是來源）的狀態 */
static struct wl_data_source *drag_src;
static PmWl *drag_origin;
static char **drag_mimes;
static int drag_nmimes;
static char *(*drag_data)(const char *mime, size_t *len, void *user);
static void *drag_user;
static char *drag_uris, *drag_plain;       /* pmwl_drag_start_files 內建的資料 */
static unsigned drag_action;
static long drag_dropped_at;
static struct wl_surface *drag_icon;
static struct buf drag_icon_buf;

/* 拖放（我們是目標）的狀態 */
static struct wl_data_offer *dnd_offer, *drop_offer, *sel_offer;
static PmWl *dnd_win, *drop_win;
static uint32_t dnd_serial;
static int dnd_x, dnd_y;                   /* 最近一次 enter／motion 的位置（drop 事件沒有座標） */
static char *dnd_accepted;                 /* 目前接受的格式（NULL = 不接受） */
static int drop_ok, drop_event_given, in_drop_leave;

static void offer_mime(void *d, struct wl_data_offer *o, const char *mime) { (void)d; offer_add(INFO(o), mime); }
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; INFO(o)->src_actions = a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; INFO(o)->action = a; }
static const struct wl_data_offer_listener offer_listener = { offer_mime, offer_actions, offer_action };

static void dd_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
	(void)d; (void)dev;
	wl_data_offer_add_listener(o, &offer_listener, NULL);   /* add_listener 會把 user data 設成它的第三個參數：要在 set_user_data 之前 */
	wl_data_offer_set_user_data(o, calloc(1, sizeof(struct offer)));
}

static void dnd_auto(PmWl *win);
static void dd_enter(void *d, struct wl_data_device *dev, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
	(void)d; (void)dev;
	data_offer_drop(dnd_offer);
	dnd_offer = o;
	dnd_serial = serial;
	dnd_win = s ? wl_surface_get_user_data(s) : NULL;
	free(dnd_accepted); dnd_accepted = NULL;
	if (!o) return;
	dnd_auto(dnd_win);
	dnd_x = (int)wl_fixed_to_double(x); dnd_y = (int)wl_fixed_to_double(y);
	if (dnd_win) {
		PmwEvent e = { .type = PMW_DND_ENTER, .win = dnd_win, .x = dnd_x, .y = dnd_y, .button = (int)INFO(o)->action };
		push(&e);
	}
}
static void dd_leave(void *d, struct wl_data_device *dev)
{
	(void)d; (void)dev;
	if (in_drop_leave) { in_drop_leave = 0; return; }   /* 放下之後的 leave：offer 還要用來拿資料，也不用再通知程式 */
	if (dnd_win) push_simple(PMW_DND_LEAVE, dnd_win);
	data_offer_drop(dnd_offer);
	dnd_offer = NULL;
	dnd_win = NULL;
	free(dnd_accepted); dnd_accepted = NULL;
}
static void dd_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)dev;
	if (!dnd_win || !dnd_offer) return;
	dnd_x = (int)wl_fixed_to_double(x); dnd_y = (int)wl_fixed_to_double(y);
	PmwEvent e = { .type = PMW_DND_MOTION, .win = dnd_win, .x = dnd_x, .y = dnd_y, .time = t,
	               .button = (int)INFO(dnd_offer)->action };
	push(&e);
}
static void dd_drop(void *d, struct wl_data_device *dev)
{
	(void)d; (void)dev;
	if (!dnd_offer) return;
	/* 前一次放下的還沒收尾（程式沒再呼叫 pmwl_next）：先收掉 */
	if (drop_offer) { data_offer_drop(drop_offer); drop_offer = NULL; }
	drop_offer = dnd_offer; dnd_offer = NULL;
	drop_win = dnd_win;
	drop_ok = dnd_accepted != NULL && INFO(drop_offer)->action != 0;
	drop_event_given = 0;
	in_drop_leave = 1;   /* compositor 放下後會再送一個 leave */
	if (drop_win) {
		PmwEvent e = { .type = PMW_DND_DROP, .win = drop_win, .x = dnd_x, .y = dnd_y, .button = (int)INFO(drop_offer)->action };
		push(&e);
	}
	free(dnd_accepted); dnd_accepted = NULL;
}
static void dd_selection(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
	(void)d; (void)dev;
	data_offer_drop(sel_offer);
	sel_offer = o;
	PmwEvent e = { .type = PMW_CLIP, .x = o != NULL };   /* 剪貼簿換了（鍵盤焦點進來時也會送一次）；自己放的也會來，用 pmwl_clip_owned 分辨 */
	push(&e);
}
static const struct wl_data_device_listener dd_listener = { dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection };

/* 放下之後的收尾：告訴來源「我收好了」（來源才會知道可以結束拖放、移動的話才會刪原檔） */
static void drop_finish(void)
{
	if (!drop_offer) return;
	if (drop_ok && wl_data_offer_get_version(drop_offer) >= 3) wl_data_offer_finish(drop_offer);
	data_offer_drop(drop_offer);
	drop_offer = NULL; drop_win = NULL; drop_ok = 0;
	wl_display_flush(wd);
}

static void dnd_forget(PmWl *win)
{
	if (drag_origin == win) drag_origin = NULL;
	if (dnd_win == win) dnd_win = NULL;
	if (drop_win == win) drop_win = NULL;
}

/* ---- 拖放：我們是目標 ---- */

void pmwl_dnd_target(PmWl *win, const char *const *mimes, int n, unsigned actions)
{
	if (!win) return;
	for (int i = 0; i < win->dnd_n; i++) free(win->dnd_mimes[i]);
	free(win->dnd_mimes);
	win->dnd_mimes = NULL; win->dnd_n = 0;
	if (!mimes || n <= 0) return;
	win->dnd_mimes = calloc((size_t)n, sizeof *win->dnd_mimes);
	if (!win->dnd_mimes) return;
	for (int i = 0; i < n; i++) if ((win->dnd_mimes[win->dnd_n] = strdup(mimes[i]))) win->dnd_n++;
	win->dnd_actions = actions ? actions : PMWL_DND_COPY;
}

static unsigned pick_action(unsigned allowed, unsigned preferred)
{
	if (allowed & preferred) return preferred;
	for (unsigned a = 1; a <= 4; a <<= 1) if (allowed & a) return a;
	return 0;
}

/* 預設的反應：視窗登記過的格式有一個 offer 也有，就接受（動作優先「複製」）；程式在 PMW_DND_ENTER／MOTION 裡可以用 pmwl_dnd_accept 蓋掉 */
static void dnd_auto(PmWl *win)
{
	if (!win || !dnd_offer || !win->dnd_n) return;
	struct offer *io = INFO(dnd_offer);
	for (int i = 0; i < win->dnd_n; i++)
		if (offer_has(io, win->dnd_mimes[i])) { pmwl_dnd_accept(win->dnd_mimes[i], win->dnd_actions, PMWL_DND_COPY); return; }
}

void pmwl_dnd_accept(const char *mime, unsigned actions, unsigned preferred)
{
	if (!dnd_offer) return;
	wl_data_offer_accept(dnd_offer, dnd_serial, mime);
	free(dnd_accepted);
	dnd_accepted = mime ? strdup(mime) : NULL;
	if (wl_data_offer_get_version(dnd_offer) >= 3) {
		unsigned a = mime ? actions & 7 : 0;
		wl_data_offer_set_actions(dnd_offer, a, pick_action(a, preferred));
	}
	wl_display_flush(wd);
}

unsigned pmwl_dnd_action(void)
{
	struct wl_data_offer *o = dnd_offer ? dnd_offer : drop_offer;
	return o ? INFO(o)->action : 0;
}

int pmwl_dnd_has(const char *mime)
{
	struct wl_data_offer *o = dnd_offer ? dnd_offer : drop_offer;
	return o && offer_has(INFO(o), mime);
}

int pmwl_dnd_mimes(const char **out, int max)
{
	struct wl_data_offer *o = dnd_offer ? dnd_offer : drop_offer;
	int n = 0;
	for (int i = 0; o && i < INFO(o)->n && n < max; i++) out[n++] = INFO(o)->mimes[i];
	return n;
}

/* 拿拖進來的資料（呼叫者 free；最多 64 MB）。我們自己就是來源（同一個程式內拖放）時直接從來源的回呼拿——
 * 不能等：來源的資料要靠同一個事件迴圈送出，等的話只會卡到逾時 */
char *pmwl_dnd_fetch(const char *mime, size_t *len)
{
	struct wl_data_offer *o = dnd_offer ? dnd_offer : drop_offer;
	if (!o || !mime || !offer_has(INFO(o), mime)) return NULL;
	if (drag_src) {
		for (int i = 0; i < drag_nmimes; i++)
			if (!strcmp(drag_mimes[i], mime) && drag_data) {
				size_t n = 0;
				char *r = drag_data(mime, &n, drag_user);
				if (len) *len = n;
				return r;
			}
		return NULL;
	}
	return offer_receive(o, mime, len, 64u << 20);
}

/* 拖進來的檔案（text/uri-list 裡的本機路徑）：回傳個數，*paths 用 pm_dnd_free 釋放 */
int pmwl_dnd_files(char ***paths)
{
	size_t n = 0;
	char *data = pmwl_dnd_fetch("text/uri-list", &n);
	if (!data) return 0;
	char **v = malloc(1024 * sizeof *v);
	int k = v ? pm_parse_uris(data, v, 1024) : 0;
	free(data);
	if (k <= 0) { free(v); return 0; }
	*paths = v;
	return k;
}

/* ---- 拖放：我們是來源 ---- */

static void drag_cleanup(void)
{
	if (drag_src) wl_data_source_destroy(drag_src);
	drag_src = NULL;
	if (drag_icon) wl_surface_destroy(drag_icon);
	drag_icon = NULL;
	buf_free(&drag_icon_buf);
	for (int i = 0; i < drag_nmimes; i++) free(drag_mimes[i]);
	free(drag_mimes);
	drag_mimes = NULL; drag_nmimes = 0;
	free(drag_uris); free(drag_plain);
	drag_uris = drag_plain = NULL;
	drag_data = NULL; drag_user = NULL;
	drag_dropped_at = 0; drag_action = 0;
}

static void drag_end(int action)
{
	PmWl *o = drag_origin;
	drag_origin = NULL;
	drag_cleanup();
	PmwEvent e = { .type = PMW_DND_END, .win = o, .button = action };
	push(&e);
	wl_display_flush(wd);
}

static void src_target(void *d, struct wl_data_source *s, const char *mime) { (void)d; (void)s; (void)mime; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
	(void)d;
	const char *t = "";
	size_t n = 0;
	char *tmp = NULL;
	if (s == drag_src) {
		if (drag_data && mime) { tmp = drag_data(mime, &n, drag_user); if (tmp) t = tmp; else n = 0; }
	} else {
		for (int i = 0; i < nclip; i++)
			if (mime && !strcmp(clip_items[i].mime, mime)) { t = clip_items[i].data; n = clip_items[i].len; break; }
	}
	write_fd(fd, t, n);
	free(tmp);
}
static void src_cancelled(void *d, struct wl_data_source *s)   /* 別的程式放了新的東西上去；拖放的話是被取消、或沒有地方可放 */
{
	(void)d;
	if (s == drag_src) { drag_end(0); return; }
	wl_data_source_destroy(s);
	if (s == my_src) { my_src = NULL; clip_clear(); }
}
static void src_dnd_dropped(void *d, struct wl_data_source *s) { (void)d; if (s == drag_src) drag_dropped_at = now_ms(); }
static void src_dnd_finished(void *d, struct wl_data_source *s) { (void)d; if (s == drag_src) drag_end(drag_action ? (int)drag_action : PMWL_DND_COPY); }
static void src_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; if (s == drag_src) drag_action = a; }
static const struct wl_data_source_listener src_listener = { src_target, src_send, src_cancelled, src_dnd_dropped, src_dnd_finished, src_action };

unsigned pmwl_drag_action(void) { return drag_src ? drag_action : 0; }
int pmwl_drag_active(void) { return drag_src != NULL; }

/* 拖曳圖示：cairo 的圖畫進 shm 緩衝，掛在一張沒有角色的 surface 上交給 compositor（它會在游標旁邊畫）；hot 是游標在圖裡的位置 */
static void drag_icon_make(cairo_surface_t *img, int hot_x, int hot_y)
{
	int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
	if (w <= 0 || h <= 0 || w > 512 || h > 512 || !buf_alloc(&drag_icon_buf, w, h)) return;
	cairo_surface_t *cs = cairo_image_surface_create_for_data(drag_icon_buf.data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, img, 0, 0);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);
	drag_icon = wl_compositor_create_surface(comp);
	wl_surface_attach(drag_icon, drag_icon_buf.b, -hot_x, -hot_y);   /* 圖的左上角在游標的 (-hot_x, -hot_y) */
	wl_surface_damage_buffer(drag_icon, 0, 0, w, h);
	wl_surface_commit(drag_icon);
}

int pmwl_drag_start(PmWl *origin, const PmwDragSrc *src)
{
	if (!origin || !src || !ddm || !ddev || drag_src || src->nmimes <= 0 || !src->data || !btn_serial) return 0;
	drag_src = wl_data_device_manager_create_data_source(ddm);
	if (!drag_src) return 0;
	wl_data_source_add_listener(drag_src, &src_listener, NULL);
	drag_mimes = calloc((size_t)src->nmimes, sizeof *drag_mimes);
	for (int i = 0; drag_mimes && i < src->nmimes; i++) {
		if (!(drag_mimes[drag_nmimes] = strdup(src->mimes[i]))) continue;
		wl_data_source_offer(drag_src, src->mimes[i]);
		drag_nmimes++;
	}
	unsigned act = src->actions & 7 ? src->actions & 7 : PMWL_DND_COPY;
	if (wl_data_source_get_version(drag_src) >= 3) wl_data_source_set_actions(drag_src, act);
	drag_data = src->data;
	drag_user = src->user;
	drag_action = 0;
	drag_origin = origin;
	if (src->icon) drag_icon_make(src->icon, src->hot_x, src->hot_y);
	wl_data_device_start_drag(ddev, drag_src, origin->surf, drag_icon, btn_serial);
	btn_serial = 0;   /* 一個序號只能用一次 */
	wl_display_flush(wd);
	return 1;
}

/* 內建的檔案清單：text/uri-list（百分比編碼）與 text/plain（一行一個路徑，貼到終端機、文字框用） */
static char *drag_files_data(const char *mime, size_t *len, void *user)
{
	(void)user;
	const char *s = !strcmp(mime, "text/uri-list") ? drag_uris : drag_plain;
	if (!s) return NULL;
	*len = strlen(s);
	return strdup(s);
}

int pmwl_drag_start_files(PmWl *origin, const char *const *paths, int n, unsigned actions, cairo_surface_t *icon, int hot_x, int hot_y)
{
	if (n <= 0 || drag_src) return 0;
	size_t cap = 1;
	for (int i = 0; i < n; i++) cap += strlen(paths[i]) * 3 + 16;
	char *uris = malloc(cap), *plain = malloc(cap);
	if (!uris || !plain) { free(uris); free(plain); return 0; }
	char *u = uris, *p = plain;
	for (int i = 0; i < n; i++) {
		u += sprintf(u, "file://");
		for (const unsigned char *c = (const unsigned char *)paths[i]; *c; c++) {
			if (isalnum(*c) || strchr("/-._~", *c)) *u++ = (char)*c;
			else u += sprintf(u, "%%%02X", *c);
		}
		u += sprintf(u, "\r\n");
		p += sprintf(p, "%s\n", paths[i]);
	}
	*u = 0; *p = 0;
	static const char *const mimes[] = { "text/uri-list", "text/plain;charset=utf-8", "text/plain" };
	PmwDragSrc s = { .mimes = mimes, .nmimes = 3, .actions = actions, .data = drag_files_data, .icon = icon, .hot_x = hot_x, .hot_y = hot_y };
	free(drag_uris); free(drag_plain);
	drag_uris = uris; drag_plain = plain;
	if (!pmwl_drag_start(origin, &s)) { free(drag_uris); free(drag_plain); drag_uris = drag_plain = NULL; return 0; }
	return 1;
}

/* ---- 剪貼簿 ---- */

static void clip_publish(void)
{
	if (my_src) wl_data_source_destroy(my_src);
	my_src = NULL;
	if (!nclip) { wl_data_device_set_selection(ddev, NULL, last_serial); wl_display_flush(wd); return; }
	my_src = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(my_src, &src_listener, NULL);
	for (int i = 0; i < nclip; i++) wl_data_source_offer(my_src, clip_items[i].mime);
	wl_data_device_set_selection(ddev, my_src, last_serial);   /* serial 要是最近一次按鍵或按滑鼠的 */
	wl_display_flush(wd);
}
static void clip_add(const char *mime, const char *data, size_t len, int owner)
{
	if (nclip >= (int)LEN(clip_items)) return;
	struct item *it = &clip_items[nclip];
	it->mime = strdup(mime);
	if (owner) { it->data = malloc(len + 1); if (it->data) { memcpy(it->data, data, len); it->data[len] = 0; } }
	else it->data = (char *)data;
	if (!it->mime || !it->data) { free(it->mime); if (owner) free(it->data); return; }
	it->len = len; it->owner = owner;
	nclip++;
}

void pmwl_clip_set(const char *text)
{
	if (!ddm || !ddev) return;
	clip_clear();
	if (!text) text = "";
	size_t n = strlen(text);
	clip_add(text_mimes[0], text, n, 1);
	for (unsigned i = 1; i < LEN(text_mimes); i++) if (nclip) clip_add(text_mimes[i], clip_items[0].data, n, 0);
	clip_publish();
}

/* 把幾種格式一起放上剪貼簿（例如圖片 image/png、檔案清單 text/uri-list＋x-special/gnome-copied-files）；資料會複製。
 * mimes[0] 如果是文字格式（text/plain…），其他常見的文字格式也一起提供 */
void pmwl_clip_set_types(const char *const *mimes, const void *const *datas, const size_t *lens, int n)
{
	if (!ddm || !ddev) return;
	clip_clear();
	for (int i = 0; i < n; i++) {
		clip_add(mimes[i], datas[i], lens[i], 1);
		if (!i && nclip) for (unsigned k = 0; k < LEN(text_mimes); k++)
			if (!strcmp(mimes[0], text_mimes[k])) {
				for (unsigned j = 0; j < LEN(text_mimes); j++) if (j != k) clip_add(text_mimes[j], clip_items[0].data, lens[0], 0);
				break;
			}
	}
	clip_publish();
}
void pmwl_clip_set_type(const char *mime, const void *data, size_t len) { pmwl_clip_set_types(&mime, &data, &len, 1); }

/* 剪貼簿現在還是我們放的嗎（別的程式放了新東西就變 0；截圖拷貝後留著服務用） */
int pmwl_clip_owned(void) { return my_src != NULL; }

/* 剪貼簿現在有的格式（指標到下次處理事件前有效）；自己放的也算 */
int pmwl_clip_types(const char **out, int max)
{
	int n = 0;
	if (my_src) { for (int i = 0; i < nclip && n < max; i++) out[n++] = clip_items[i].mime; return n; }
	for (int i = 0; sel_offer && i < INFO(sel_offer)->n && n < max; i++) out[n++] = INFO(sel_offer)->mimes[i];
	return n;
}
int pmwl_clip_has_type(const char *mime)
{
	if (my_src) { for (int i = 0; i < nclip; i++) if (!strcmp(clip_items[i].mime, mime)) return 1; return 0; }
	return sel_offer && offer_has(INFO(sel_offer), mime);
}

/* 拿某個格式的內容（呼叫者 free；最多 64 MB）。是自己放的就直接給（不能等自己回應自己） */
char *pmwl_clip_get_type(const char *mime, size_t *len)
{
	if (my_src) {
		for (int i = 0; i < nclip; i++)
			if (!strcmp(clip_items[i].mime, mime)) {
				char *r = malloc(clip_items[i].len + 1);
				if (r) { memcpy(r, clip_items[i].data, clip_items[i].len); r[clip_items[i].len] = 0; if (len) *len = clip_items[i].len; }
				return r;
			}
		return NULL;
	}
	if (!sel_offer || !offer_has(INFO(sel_offer), mime)) return NULL;
	return offer_receive(sel_offer, mime, len, 64u << 20);
}

char *pmwl_clip_get(void)
{
	if (my_src) { int t = -1; for (unsigned i = 0; i < LEN(text_mimes) && t < 0; i++) if (pmwl_clip_has_type(text_mimes[i])) t = (int)i; return t < 0 ? NULL : pmwl_clip_get_type(text_mimes[t], NULL); }
	int t = sel_offer ? offer_text(INFO(sel_offer)) : -1;
	if (t < 0) return NULL;
	return offer_receive(sel_offer, text_mimes[t], NULL, 1u << 20);   /* 輸入框用不到超過 1 MB */
}

/* ---- primary selection：選取文字就放上去，中鍵貼上（終端機、文字編輯用）；只有文字 ---- */

static struct zwp_primary_selection_offer_v1 *psel_offer;
static struct zwp_primary_selection_source_v1 *my_psrc;
static char *my_ptext;

static void po_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *mime) { (void)d; offer_add(zwp_primary_selection_offer_v1_get_user_data(o), mime); }
static const struct zwp_primary_selection_offer_v1_listener po_listener = { po_offer };
static void poffer_free(struct zwp_primary_selection_offer_v1 *o)
{
	if (!o) return;
	offer_free(zwp_primary_selection_offer_v1_get_user_data(o));
	zwp_primary_selection_offer_v1_destroy(o);
}
static void pd_offer(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o)
{
	(void)d; (void)dev;
	zwp_primary_selection_offer_v1_add_listener(o, &po_listener, NULL);
	zwp_primary_selection_offer_v1_set_user_data(o, calloc(1, sizeof(struct offer)));
}
static void pd_selection(void *d, struct zwp_primary_selection_device_v1 *dev, struct zwp_primary_selection_offer_v1 *o)
{
	(void)d; (void)dev;
	poffer_free(psel_offer);
	psel_offer = o;
}
static const struct zwp_primary_selection_device_v1_listener pd_listener = { pd_offer, pd_selection };

static void ps_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *mime, int32_t fd)
{
	(void)d; (void)s; (void)mime;
	const char *t = my_ptext ? my_ptext : "";
	write_fd(fd, t, strlen(t));
}
static void ps_cancelled(void *d, struct zwp_primary_selection_source_v1 *s)
{
	(void)d;
	zwp_primary_selection_source_v1_destroy(s);
	if (s == my_psrc) { my_psrc = NULL; free(my_ptext); my_ptext = NULL; }
}
static const struct zwp_primary_selection_source_v1_listener ps_listener = { ps_send, ps_cancelled };

/* 把文字放上 primary selection（text = NULL 或空字串 = 清掉）；要在按鍵或按滑鼠之後呼叫 */
void pmwl_primary_set(const char *text)
{
	if (!psm || !pdev) return;
	if (my_psrc) zwp_primary_selection_source_v1_destroy(my_psrc);
	my_psrc = NULL;
	free(my_ptext); my_ptext = NULL;
	if (text && *text) {
		my_ptext = strdup(text);
		my_psrc = zwp_primary_selection_device_manager_v1_create_source(psm);
		zwp_primary_selection_source_v1_add_listener(my_psrc, &ps_listener, NULL);
		zwp_primary_selection_source_v1_offer(my_psrc, "text/plain;charset=utf-8");
		zwp_primary_selection_source_v1_offer(my_psrc, "text/plain");
		zwp_primary_selection_source_v1_offer(my_psrc, "UTF8_STRING");
	}
	zwp_primary_selection_device_v1_set_selection(pdev, my_psrc, last_serial);
	wl_display_flush(wd);
}

char *pmwl_primary_get(void)
{
	if (my_psrc && my_ptext) return strdup(my_ptext);
	if (!psel_offer) return NULL;
	struct offer *io = zwp_primary_selection_offer_v1_get_user_data(psel_offer);
	int t = offer_text(io);
	if (t < 0) return NULL;
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) return NULL;
	zwp_primary_selection_offer_v1_receive(psel_offer, text_mimes[t], p[1]);
	close(p[1]);
	wl_display_flush(wd);
	char *r = read_all(p[0], NULL, 1u << 20, 1500);
	close(p[0]);
	return r;
}

/* ---- 相對滑鼠（遊戲、虛擬機器、遠端桌面）與快捷鍵抑制 ---- */

static struct zwp_locked_pointer_v1 *lock_ptr;
static struct zwp_relative_pointer_v1 *rel_ptr;
static PmWl *lock_win;
static double rel_ax, rel_ay;

static void lp_locked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; PmwEvent e = { .type = PMW_POINTER_LOCK, .win = lock_win, .x = 1 }; push(&e); }
static void lp_unlocked(void *d, struct zwp_locked_pointer_v1 *l) { (void)d; (void)l; PmwEvent e = { .type = PMW_POINTER_LOCK, .win = lock_win, .x = 0 }; push(&e); }
static const struct zwp_locked_pointer_v1_listener lp_listener = { lp_locked, lp_unlocked };
/* 相對移動（沒有加速度的原始值）：小數累積起來，整數的部分才送出，不掉動作 */
static void rp_motion(void *d, struct zwp_relative_pointer_v1 *r, uint32_t hi, uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t udx, wl_fixed_t udy)
{
	(void)d; (void)r; (void)hi; (void)lo; (void)dx; (void)dy;
	if (!lock_win) return;
	rel_ax += wl_fixed_to_double(udx); rel_ay += wl_fixed_to_double(udy);
	int ix = (int)rel_ax, iy = (int)rel_ay;
	if (!ix && !iy) return;
	rel_ax -= ix; rel_ay -= iy;
	PmwEvent e = { .type = PMW_RELMOTION, .win = lock_win, .x = ix, .y = iy, .state = x_state() };
	push(&e);
}
static const struct zwp_relative_pointer_v1_listener rp_listener = { rp_motion };

/* 鎖住滑鼠（游標不動、只送相對移動 PMW_RELMOTION）；on = 0 放開。compositor 同意後收到 PMW_POINTER_LOCK（x = 1）；
 * 視窗失去焦點時 compositor 會自己放開（x = 0）。回傳 1 = 已經要求 */
int pmwl_pointer_lock(PmWl *win, int on)
{
	if (!on) {
		if (!lock_ptr || (win && lock_win != win)) return 0;
		if (rel_ptr) zwp_relative_pointer_v1_destroy(rel_ptr);
		zwp_locked_pointer_v1_destroy(lock_ptr);
		rel_ptr = NULL; lock_ptr = NULL; lock_win = NULL;
		wl_display_flush(wd);
		return 1;
	}
	if (!win || !pconstraints || !relmgr || !ptr || lock_ptr) return 0;
	lock_win = win;
	rel_ax = rel_ay = 0;
	lock_ptr = zwp_pointer_constraints_v1_lock_pointer(pconstraints, win->surf, ptr, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	zwp_locked_pointer_v1_add_listener(lock_ptr, &lp_listener, NULL);
	rel_ptr = zwp_relative_pointer_manager_v1_get_relative_pointer(relmgr, ptr);
	zwp_relative_pointer_v1_add_listener(rel_ptr, &rp_listener, NULL);
	wl_display_flush(wd);
	return 1;
}

void pmwl_pointer_hint(PmWl *win, double x, double y)
{
	if (!lock_ptr || !win || lock_win != win) return;
	zwp_locked_pointer_v1_set_cursor_position_hint(lock_ptr, wl_fixed_from_double(x), wl_fixed_from_double(y));
	wl_surface_commit(win->surf);   /* hint 要等下一次 commit 才生效 */
}

/* 這個視窗在畫面上的時候不要螢幕變暗、鎖定、休眠（播放影片、簡報、遠端桌面）。on = 0 放開 */
/* ---- 閒置通知（ext-idle-notify-v1）：電源管理用，取代 X 的 IDLETIME ---- */

#define NIDLE 8
static struct { struct ext_idle_notification_v1 *n; int id; } idle_w[NIDLE];
static int idle_next_id = 1;
static void idle_idled(void *d, struct ext_idle_notification_v1 *n) { PmwEvent e = { .type = PMW_IDLE, .x = 1, .button = (int)(intptr_t)d }; push(&e); }
static void idle_resumed(void *d, struct ext_idle_notification_v1 *n) { PmwEvent e = { .type = PMW_IDLE, .x = 0, .button = (int)(intptr_t)d }; push(&e); }
static const struct ext_idle_notification_v1_listener idle_listener = { idle_idled, idle_resumed };

int pmwl_can_idle(void) { return connected == 1 && idle_notifier != NULL; }

/* 使用者 timeout_ms 毫秒沒有輸入（也沒有程式要求防止閒置）就收到 PMW_IDLE（x＝1、button＝這裡回傳的編號）；
 * 之後一有輸入收到 PMW_IDLE（x＝0）。回傳編號（> 0），0＝不支援或太多 */
int pmwl_idle_watch(long timeout_ms)
{
	if (!pmwl_can_idle() || timeout_ms < 1) return 0;
	for (int i = 0; i < NIDLE; i++) {
		if (idle_w[i].n) continue;
		idle_w[i].id = idle_next_id++;
		idle_w[i].n = ext_idle_notifier_v1_get_idle_notification(idle_notifier, (uint32_t)timeout_ms, seat);
		ext_idle_notification_v1_add_listener(idle_w[i].n, &idle_listener, (void *)(intptr_t)idle_w[i].id);
		wl_display_flush(wd);
		return idle_w[i].id;
	}
	return 0;
}

/* 取消所有的閒置監看（設定改了要重新登記） */
void pmwl_idle_clear(void)
{
	for (int i = 0; i < NIDLE; i++) if (idle_w[i].n) { ext_idle_notification_v1_destroy(idle_w[i].n); idle_w[i].n = NULL; }
	if (wd) wl_display_flush(wd);
}

void pmwl_idle_inhibit(PmWl *win, int on)
{
	if (!win) return;
	if (!on) {
		if (win->idle_inhibitor) zwp_idle_inhibitor_v1_destroy(win->idle_inhibitor);
		win->idle_inhibitor = NULL;
		wl_display_flush(wd);
		return;
	}
	if (!idle_mgr || win->idle_inhibitor) return;
	win->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(idle_mgr, win->surf);
	wl_display_flush(wd);
}

static void ih_active(void *d, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) { (void)d; (void)i; }
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener ih_listener = { ih_active, ih_active };

/* 要 compositor 不要攔截系統快捷鍵（Alt+Tab、Super…），全部送給這個視窗（虛擬機器、遠端桌面的畫面；要有鍵盤焦點才有效，
 * compositor 一律保留自己的逃生組合鍵）。on = 0 還給 compositor */
void pmwl_shortcuts_inhibit(PmWl *win, int on)
{
	if (!win) return;
	if (!on) {
		if (win->inhibitor) zwp_keyboard_shortcuts_inhibitor_v1_destroy(win->inhibitor);
		win->inhibitor = NULL;
		wl_display_flush(wd);
		return;
	}
	if (!inhibit_mgr || win->inhibitor) return;
	win->inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(inhibit_mgr, win->surf, seat);
	zwp_keyboard_shortcuts_inhibitor_v1_add_listener(win->inhibitor, &ih_listener, win);
	wl_display_flush(wd);
}

/* ---- 連線 ---- */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name)) comp = wl_registry_bind(r, name, &wl_compositor_interface, MIN(ver, 4));
	else if (!strcmp(iface, wl_shm_interface.name)) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, MIN(ver, 4));
		xdg_wm_base_add_listener(wm, &wm_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
		decomgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 5));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name))
		shape_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
	else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name))
		ti_mgr = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
	else if (!strcmp(iface, wl_data_device_manager_interface.name))
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, MIN(ver, 3));
	else if (!strcmp(iface, wl_subcompositor_interface.name))
		subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
	else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name))
		psm = wl_registry_bind(r, name, &zwp_primary_selection_device_manager_v1_interface, 1);
	else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name))
		pconstraints = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
	else if (!strcmp(iface, zwp_relative_pointer_manager_v1_interface.name))
		relmgr = wl_registry_bind(r, name, &zwp_relative_pointer_manager_v1_interface, 1);
	else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name))
		idle_mgr = wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_idle_notifier_v1_interface.name))
		idle_notifier = wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, 1);
	else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
		inhibit_mgr = wl_registry_bind(r, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
	else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name))
		lock_mgr = wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		scm = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, MIN(ver, 3));
	else if (!strcmp(iface, zwlr_output_manager_v1_interface.name)) {
		omgr = wl_registry_bind(r, name, &zwlr_output_manager_v1_interface, MIN(ver, 4));
		zwlr_output_manager_v1_add_listener(omgr, &omgr_listener, NULL);
	} else if (!strcmp(iface, zwlr_output_power_manager_v1_interface.name))
		opm = wl_registry_bind(r, name, &zwlr_output_power_manager_v1_interface, 1);
	else if (!strcmp(iface, wp_viewporter_interface.name))
		viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) { ftl_name = name; ftl_ver = MIN(ver, 3); }
	else if (!strcmp(iface, pomelo_workspace_manager_v1_interface.name)) ws_name = name;
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, MIN(ver, 4));
	else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
		xo_mgr = wl_registry_bind(r, name, &zxdg_output_manager_v1_interface, MIN(ver, 3));
		for (int i = 0; i < nouts; i++) out_watch(&outs[i]);
	} else if (!strcmp(iface, wl_output_interface.name) && nouts < MAXOUT) {
		struct out *o = &outs[nouts++];
		memset(o, 0, sizeof *o);
		o->name = name;
		o->o = wl_registry_bind(r, name, &wl_output_interface, 1);
		out_watch(o);
		if (connected == 1) { PmwEvent e = { .type = PMW_OUTPUTS }; push(&e); }   /* 插上新螢幕 */
	}
}
/* 螢幕拔掉 */
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r;
	for (int i = 0; i < nouts; i++)
		if (outs[i].name == name) {
			if (outs[i].xo) zxdg_output_v1_destroy(outs[i].xo);
			wl_output_destroy(outs[i].o);
			for (PmWl *w = wins; w; w = w->next) if (w->out == outs[i].o) w->out = NULL;
			memmove(&outs[i], &outs[i + 1], (size_t)(nouts - i - 1) * sizeof outs[0]);
			nouts--;
			PmwEvent e = { .type = PMW_OUTPUTS };
			push(&e);
			break;
		}
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* 設定檔所在的資料夾（跟 settings.c 一樣的規則） */
static void watch_settings(void)
{
	const char *c = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	char dir[PATH_MAX];
	if (c && *c) snprintf(dir, sizeof dir, "%s/pomelo", c);
	else snprintf(dir, sizeof dir, "%s/.config/pomelo", h ? h : "/tmp");
	ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino >= 0 && inotify_add_watch(ino, dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) { close(ino); ino = -1; }
}

/* ---- 視窗清單（wlr-foreign-toplevel）：Dock、小柚用 ---- */

struct tl {
	struct zwlr_foreign_toplevel_handle_v1 *h;
	unsigned id;
	char title[256], app_id[128];
	int minimized, maximized, fullscreen, activated, output, workspace;
	struct zwlr_foreign_toplevel_handle_v1 *parent_h;
	struct tl *next;
};
static struct tl *tls;
static unsigned tl_next_id;

static struct tl *tl_find(struct zwlr_foreign_toplevel_handle_v1 *h)
{
	for (struct tl *t = tls; t; t = t->next) if (t->h == h) return t;
	return NULL;
}
static void tl_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title) { (void)h; snprintf(((struct tl *)d)->title, sizeof ((struct tl *)d)->title, "%s", title ? title : ""); }
static void tl_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *app) { (void)h; snprintf(((struct tl *)d)->app_id, sizeof ((struct tl *)d)->app_id, "%s", app ? app : ""); }
static void tl_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
	(void)h;
	struct tl *t = d;
	for (int i = 0; i < nouts; i++) if (outs[i].o == o) t->output = i;
}
static void tl_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o)
{
	(void)h;
	struct tl *t = d;
	for (int i = 0; i < nouts; i++) if (outs[i].o == o && t->output == i) t->output = -1;
}
static void tl_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *a)
{
	(void)h;
	struct tl *t = d;
	t->minimized = t->maximized = t->fullscreen = t->activated = 0;
	uint32_t *st;
	wl_array_for_each(st, a) {
		if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) t->minimized = 1;
		else if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) t->maximized = 1;
		else if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN) t->fullscreen = 1;
		else if (*st == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) t->activated = 1;
	}
}
static void tl_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { (void)d; (void)h; tl_changed = 1; }
static void tl_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	struct tl *t = d;
	for (struct tl **p = &tls; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
	zwlr_foreign_toplevel_handle_v1_destroy(h);
	free(t);
	tl_changed = 1;
}
static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct zwlr_foreign_toplevel_handle_v1 *parent) { (void)h; ((struct tl *)d)->parent_h = parent; }
static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
	tl_title, tl_app_id, tl_output_enter, tl_output_leave, tl_state, tl_done, tl_closed, tl_parent,
};
static void ftl_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m, struct zwlr_foreign_toplevel_handle_v1 *h)
{
	(void)d; (void)m;
	struct tl *t = calloc(1, sizeof *t);
	if (!t) return;
	t->h = h; t->id = ++tl_next_id; t->output = -1; t->workspace = -2;
	/* 接在最後：清單順序＝開啟順序 */
	struct tl **p = &tls;
	while (*p) p = &(*p)->next;
	*p = t;
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
}
static void ftl_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) { (void)d; (void)m; }
static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_listener = { ftl_toplevel, ftl_finished };

/* ---- 多桌面 ---- */
static void ws_state(void *d, struct pomelo_workspace_manager_v1 *m, uint32_t count, uint32_t current)
{
	(void)d; (void)m;
	ws_count = (int)count; ws_cur = (int)current;
	ws_changed = 1;
}
/* 事件裡的 id 是這個連線裡視窗清單 handle 的物件編號 */
static void ws_window(void *d, struct pomelo_workspace_manager_v1 *m, uint32_t id, uint32_t workspace)
{
	(void)d; (void)m;
	for (struct tl *t = tls; t; t = t->next)
		if (wl_proxy_get_id((struct wl_proxy *)t->h) == id) {
			t->workspace = workspace == 0xffffffffu ? -1 : (int)workspace;
			ws_changed = 1;
			tl_changed = 1;
			return;
		}
}
static const struct pomelo_workspace_manager_v1_listener ws_listener = { ws_state, ws_window };

int pmwl_workspace_start(void)
{
	if (!pmwl_connect() || !ws_name) return 0;
	if (ws_mgr) return 1;
	ws_mgr = wl_registry_bind(reg, ws_name, &pomelo_workspace_manager_v1_interface, 1);
	pomelo_workspace_manager_v1_add_listener(ws_mgr, &ws_listener, NULL);
	pomelo_workspace_manager_v1_sync(ws_mgr);   /* 視窗清單已經先綁好：現有的視窗一次補上 */
	wl_display_roundtrip(wd);
	return 1;
}
int pmwl_workspace_info(int *count, int *current)
{
	if (!ws_mgr || ws_count <= 0) return 0;
	if (count) *count = ws_count;
	if (current) *current = ws_cur;
	return 1;
}
void pmwl_workspace_goto(int n) { if (ws_mgr && n >= 0) { pomelo_workspace_manager_v1_switch_to(ws_mgr, (uint32_t)n); wl_display_flush(wd); } }
void pmwl_workspace_step(int d) { if (ws_mgr) { pomelo_workspace_manager_v1_step(ws_mgr, d); wl_display_flush(wd); } }
void pmwl_workspace_move_window(unsigned id, int n, int follow)
{
	if (!ws_mgr || n < 0) return;
	/* id 是這個程式自己的視窗編號（tl.id），換成協定要的 handle 物件編號 */
	uint32_t pid = 0;
	for (struct tl *t = tls; id && t; t = t->next) if (t->id == id) pid = wl_proxy_get_id((struct wl_proxy *)t->h);
	if (id && !pid) return;
	pomelo_workspace_manager_v1_move_window(ws_mgr, pid, (uint32_t)n, follow ? 1 : 0);
	wl_display_flush(wd);
}
void pmwl_workspace_sticky(unsigned id, int mode)
{
	if (!ws_mgr) return;
	uint32_t pid = 0;
	for (struct tl *t = tls; id && t; t = t->next) if (t->id == id) pid = wl_proxy_get_id((struct wl_proxy *)t->h);
	if (id && !pid) return;
	pomelo_workspace_manager_v1_set_sticky(ws_mgr, pid, (uint32_t)mode);
	wl_display_flush(wd);
}
void pmwl_workspace_add(void) { if (ws_mgr) { pomelo_workspace_manager_v1_add(ws_mgr); wl_display_flush(wd); } }
void pmwl_workspace_remove(int n) { if (ws_mgr && n >= 0) { pomelo_workspace_manager_v1_remove(ws_mgr, (uint32_t)n); wl_display_flush(wd); } }
void pmwl_workspace_overview(void) { if (ws_mgr) { pomelo_workspace_manager_v1_overview(ws_mgr); wl_display_flush(wd); } }

/* 清單有變就送一個 PMW_TOPLEVELS（很多視窗同時更新也只送一個） */
static void tl_flush(void)
{
	if (ws_changed) {
		ws_changed = 0;
		PmwEvent e = { .type = PMW_WORKSPACE };
		push(&e);
	}
	if (!tl_changed) return;
	tl_changed = 0;
	PmwEvent e = { .type = PMW_TOPLEVELS };
	push(&e);
}

int pmwl_toplevels_start(void)
{
	if (!pmwl_connect() || !ftl_name) return 0;
	if (ftl_mgr) return 1;
	ftl_mgr = wl_registry_bind(reg, ftl_name, &zwlr_foreign_toplevel_manager_v1_interface, ftl_ver);
	zwlr_foreign_toplevel_manager_v1_add_listener(ftl_mgr, &ftl_listener, NULL);
	wl_display_roundtrip(wd);   /* 現有的視窗一次進來 */
	tl_changed = 1;
	return 1;
}

int pmwl_toplevels(PmwToplevel *out, int max)
{
	int n = 0;
	for (struct tl *t = tls; t && n < max; t = t->next) {
		PmwToplevel *o = &out[n++];
		memset(o, 0, sizeof *o);
		o->id = t->id;
		snprintf(o->title, sizeof o->title, "%s", t->title);
		snprintf(o->app_id, sizeof o->app_id, "%s", t->app_id);
		o->minimized = t->minimized; o->maximized = t->maximized; o->fullscreen = t->fullscreen; o->activated = t->activated;
		o->output = t->output;
		o->workspace = t->workspace;
		struct tl *pt = t->parent_h ? tl_find(t->parent_h) : NULL;
		o->parent = pt ? pt->id : 0;
	}
	return n;
}

void pmwl_toplevel_do(unsigned id, int what)
{
	struct tl *t = tls;
	while (t && t->id != id) t = t->next;
	if (!t) return;
	switch (what) {
	case PMWL_TL_ACTIVATE: zwlr_foreign_toplevel_handle_v1_unset_minimized(t->h); zwlr_foreign_toplevel_handle_v1_activate(t->h, seat); break;
	case PMWL_TL_MINIMIZE: zwlr_foreign_toplevel_handle_v1_set_minimized(t->h); break;
	case PMWL_TL_UNMINIMIZE: zwlr_foreign_toplevel_handle_v1_unset_minimized(t->h); break;
	case PMWL_TL_MAXIMIZE: zwlr_foreign_toplevel_handle_v1_set_maximized(t->h); break;
	case PMWL_TL_UNMAXIMIZE: zwlr_foreign_toplevel_handle_v1_unset_maximized(t->h); break;
	case PMWL_TL_FULLSCREEN: zwlr_foreign_toplevel_handle_v1_set_fullscreen(t->h, NULL); break;
	case PMWL_TL_UNFULLSCREEN: zwlr_foreign_toplevel_handle_v1_unset_fullscreen(t->h); break;
	case PMWL_TL_CLOSE: zwlr_foreign_toplevel_handle_v1_close(t->h); break;
	}
	wl_display_flush(wd);
}

int pmwl_connect(void)
{
	if (connected >= 0) return connected;
	connected = 0;
	const char *be = getenv("POMELO_BACKEND");
	if (be && !strcmp(be, "x11")) return 0;
	if (!getenv("WAYLAND_DISPLAY") || !(wd = wl_display_connect(NULL))) return 0;
	struct wl_registry *r = reg = wl_display_get_registry(wd);
	wl_registry_add_listener(r, &reg_listener, NULL);
	wl_display_roundtrip(wd);
	if (!comp || !shm || !wm || !seat) {   /* 不夠用：退回 X11 */
		wl_display_disconnect(wd);
		wd = NULL;
		return 0;
	}
	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (ti_mgr) {
		ti = zwp_text_input_manager_v3_get_text_input(ti_mgr, seat);
		zwp_text_input_v3_add_listener(ti, &ti_listener, NULL);
	}
	if (ddm) {
		ddev = wl_data_device_manager_get_data_device(ddm, seat);
		wl_data_device_add_listener(ddev, &dd_listener, NULL);
	}
	if (psm) {
		pdev = zwp_primary_selection_device_manager_v1_get_device(psm, seat);
		zwp_primary_selection_device_v1_add_listener(pdev, &pd_listener, NULL);
	}
	wl_display_roundtrip(wd);   /* 拿到滑鼠、鍵盤、每個螢幕的位置與大小 */
	watch_settings();
	connected = 1;
	return 1;
}

/* ---- 事件 ---- */

/* 程式自己的 fd（例如 D-Bus）：可以讀的時候 pmwl_next 回傳 PMW_FD（x 是那個 fd），程式要把資料讀掉 */
#define NWATCH 4
#define NXF 48   /* pmwl_next_fds 最多幫程式等幾個自己的 fd（終端機每個分頁一個 pty） */
static int watch[NWATCH], nwatch;
void pmwl_watch_fd(int fd) { if (fd >= 0 && nwatch < NWATCH) watch[nwatch++] = fd; }

static int settings_changed(void)
{
	if (ino < 0) return 0;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	int hit = 0;
	ssize_t n;
	while ((n = read(ino, buf, sizeof buf)) > 0)
		for (char *p = buf; p < buf + n; ) {
			struct inotify_event *e = (struct inotify_event *)p;
			if (e->len && !strcmp(e->name, "settings.conf")) hit = 1;
			p += sizeof *e + e->len;
		}
	return hit;
}

/* 閒置放掉備用緩衝（0.41 記憶體）：畫面連畫幾次（開啟時的 configure、expose）會配出 2～3 張整個視窗大的 shm 緩衝，
 * 之後不動了卻一直留著（桌布、啟動器 3.6 MB × 3）。一個視窗 1.5 秒沒有新的一幀，就把「不是目前這張、compositor 也已經還回來」
 * 的緩衝放掉，只留正在顯示的那張；下次要畫再配（memfd＋mmap 約 1 ms）。回傳還要等幾毫秒才該再檢查（-1 = 沒有要檢查的）。 */
#define TRIM_IDLE_MS 1500
static long trim_idle_buffers(long now)
{
	long next = -1;
	for (PmWl *w = wins; w; w = w->next) {
		int spare = 0;
		for (int i = 0; i < 3; i++) if (w->bufs[i].b && !w->bufs[i].busy && &w->bufs[i] != w->cur) spare++;
		if (!spare || w->cs) continue;   /* 正在畫（pmwl_begin 到 pmwl_end 之間）的不動 */
		long due = w->last_commit + TRIM_IDLE_MS;
		if (now >= due) {
			for (int i = 0; i < 3; i++) if (w->bufs[i].b && !w->bufs[i].busy && &w->bufs[i] != w->cur) buf_free(&w->bufs[i]);
		} else if (next < 0 || due - now < next) next = due - now;
	}
	return next;
}

static int next_impl(PmwEvent *ev, long timeout_ms, struct pollfd *xf, int nxf)
{
	if (nxf > NXF) nxf = NXF;
	if (nxf < 0) nxf = 0;
	long deadline = timeout_ms >= 0 ? now_ms() + timeout_ms : -1;
	if (drop_event_given) { drop_event_given = 0; drop_finish(); }   /* 程式處理完上一個放下事件了：告訴來源收好了 */
	for (;;) {
		while (qh != qt) {
			*ev = q[qh];
			qh = (qh + 1) % QN;
			if (ev->type == PMW_DND_DROP) drop_event_given = 1;
			if (ev->type != PMW_NONE) return 1;
		}
		/* 放下之後來源 10 秒都沒有說完成（對方當掉、X 程式不回 XdndFinished）：自己結束 */
		if (drag_src && drag_dropped_at && now_ms() - drag_dropped_at > 10000) drag_end(drag_action ? (int)drag_action : 0);
		long now = now_ms();
		if (rep_key && kb_focus && now >= rep_next) {   /* 按住不放：自己重複 */
			rep_next = now + 1000 / MAX(rep_rate, 1);
			key_event(rep_key - 1, now, 0, 1);
			continue;
		}
		if (wl_display_dispatch_pending(wd) > 0) { tl_flush(); if (qh != qt) continue; }
		wl_display_flush(wd);
		long wait = deadline >= 0 ? MAX(0, deadline - now) : -1;
		long trim = trim_idle_buffers(now);
		if (trim >= 0) wait = wait < 0 ? trim + 1 : MIN(wait, trim + 1);
		if (rep_key && kb_focus) { long r = MAX(0, rep_next - now); wait = wait < 0 ? r : MIN(wait, r); }
		while (wl_display_prepare_read(wd) != 0) wl_display_dispatch_pending(wd);
		struct pollfd pf[2 + NWATCH + NXF] = { { wl_display_get_fd(wd), POLLIN, 0 }, { ino, POLLIN, 0 } };   /* fd 是 -1 的 poll 會略過 */
		for (int i = 0; i < nwatch; i++) pf[2 + i] = (struct pollfd){ watch[i], POLLIN, 0 };
		for (int i = 0; i < nxf; i++) pf[2 + nwatch + i] = (struct pollfd){ xf[i].fd, xf[i].events, 0 };
		int r = poll(pf, 2 + nwatch + nxf, (int)wait);
		if (r > 0 && (pf[0].revents & POLLIN)) {
			/* 先讀；compositor 斷線時讀到 EOF 會回 -1。以前這裡直接 return 0，後面的斷線檢查執行不到，
			 * 呼叫端把 0 當逾時再呼叫，poll 因為 POLLHUP 立刻返回 → 無限空轉吃滿一顆核心（pomelo-notify 實測 98%） */
			if (wl_display_read_events(wd) < 0) { fprintf(stderr, "pomelo：跟 compositor 的連線斷了\n"); exit(1); }
		} else wl_display_cancel_read(wd);
		if (r < 0 && errno != EINTR) return 0;
		if (pf[0].revents & (POLLERR | POLLHUP)) { fprintf(stderr, "pomelo：跟 compositor 的連線斷了\n"); exit(1); }
		wl_display_dispatch_pending(wd);
		if (ino >= 0 && (pf[1].revents & POLLIN) && settings_changed()) {
			pm_conf_load();
			PmwEvent e = { .type = PMW_CONF };
			push(&e);
		}
		tl_flush();
		for (int i = 0; i < nwatch; i++)
			if (pf[2 + i].revents & (POLLIN | POLLHUP | POLLERR)) { PmwEvent e = { .type = PMW_FD, .x = watch[i] }; push(&e); }
		int ext = 0;
		for (int i = 0; i < nxf; i++) { xf[i].revents = pf[2 + nwatch + i].revents; if (xf[i].revents) ext = 1; }
		if (ext) return 0;   /* 程式自己的 fd 有動靜：先讓它處理，佇列裡的事件下次再給 */
		if (qh == qt && deadline >= 0 && now_ms() >= deadline && !(rep_key && kb_focus && now_ms() >= rep_next)) return 0;
	}
}

int pmwl_next(PmwEvent *ev, long timeout_ms) { return next_impl(ev, timeout_ms, NULL, 0); }
int pmwl_next_fds(PmwEvent *ev, long timeout_ms, struct pollfd *fds, int nfds) { return next_impl(ev, timeout_ms, fds, nfds); }

/* ---- 選單（xdg_popup） ---- */

void pmwl_menu(PmWl *parent, const MenuItem *m, int n, int x, int y)
{
	if (!parent || !n) return;
	PmMenuGeo g;
	pm__menu_measure(m, n, &g);
	struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm);
	xdg_positioner_set_size(pos, g.W, g.H);
	xdg_positioner_set_anchor_rect(pos, x, y, 1, 1);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP_LEFT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
	                                         XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
	                                         XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y);
	PmWl *pw = new_surface(g.W, g.H, 1);
	if (!pw) { xdg_positioner_destroy(pos); return; }
	pw->popup = xdg_surface_get_popup(pw->xs, parent->xs, pos);   /* parent 是 layer 視窗時 xs 是 NULL，下面再掛上去 */
	if (parent->layer) zwlr_layer_surface_v1_get_popup(parent->layer, pw->popup);
	xdg_popup_add_listener(pw->popup, &popup_listener, pw);
	xdg_positioner_destroy(pos);
	xdg_popup_grab(pw->popup, seat, last_serial);   /* 點外面 compositor 會關掉它（popup_done） */
	wl_surface_commit(pw->surf);
	wait_configured(pw);

	int hot = -1, chosen = -1, done = 0, redraw = 1;
	PmwEvent later[32];   /* 選單開著時其他視窗的重畫、改大小、關閉：選單關掉後再交回去 */
	int nlater = 0;
	/* 打開選單的那一下還沒放開：放開時游標剛好在第一項上（例如「安裝」），不能算選了它。
	 * 在選單上按過、或按住超過 0.3 秒拖過來再放開（按住拖曳選取）才算 */
	long opened = now_ms();
	int pressed = 0;
	pm_popup_dir = 0;
	while (!done) {
		if (redraw) {
			cairo_t *cr = pmwl_begin(pw, NULL, NULL);
			/* 圓角外面透明（X 版是 Shape 裁的） */
			cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
			cairo_paint(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
			pm_rrect(cr, 0, 0, g.W, g.H, 7, 7);
			cairo_clip(cr);
			pm__menu_draw(cr, m, n, &g, hot);
			pmwl_end(pw, cr);
			redraw = 0;
		}
		PmwEvent ev;
		if (!pmwl_next(&ev, -1)) break;
		if (ev.win != pw && ev.type != PMW_KEY) {
			/* 點到自己別的視窗（例如面板的另一條選單列、桌布）：xdg_popup 的 grab 不會因此結束（同一個程式的視窗算在 grab 裡），
			 * 所以自己把選單關掉，那一下不算（跟 X 版搶住滑鼠、點外面只關選單一樣）；其他滑鼠事件不處理 */
			if (ev.type == PMW_DOWN) { done = 1; continue; }
			if (ev.type != PMW_UP && ev.type != PMW_MOTION && ev.type != PMW_SCROLL && ev.type != PMW_ENTER && ev.type != PMW_LEAVE &&
			    nlater < (int)LEN(later))
				later[nlater++] = ev;
			continue;
		}
		int idx = pm__menu_hit(m, n, &g, ev.x, ev.y);
		switch (ev.type) {
		case PMW_CLOSE: done = 1; break;
		case PMW_EXPOSE: redraw = 1; break;
		case PMW_MOTION: if (idx != hot) { hot = idx; redraw = 1; } break;
		case PMW_DOWN: pressed = 1; break;
		case PMW_UP: if (idx >= 0 && (pressed || now_ms() - opened > 300)) { chosen = idx; done = 1; } break;
		case PMW_KEY:
			if (ev.key == XK_Escape) done = 1;
			else if (ev.key == XK_Down || ev.key == XK_Up) { hot = pm__menu_step(m, n, hot, ev.key == XK_Down ? 1 : -1); redraw = 1; }
			else if ((ev.key == XK_Return || ev.key == XK_KP_Enter) && hot >= 0) { chosen = hot; done = 1; }
			else if (ev.key == XK_Left || ev.key == XK_Right) { pm_popup_dir = ev.key == XK_Left ? -1 : 1; done = 1; }
			break;
		}
	}
	pmwl_destroy(pw);
	for (int i = 0; i < nlater; i++) push(&later[i]);
	if (chosen >= 0 && m[chosen].fn) m[chosen].fn(&m[chosen].arg);
}

/* ---- 雙後端程式的共用薄層（階段 4）：原生事件轉成 XEvent、視窗改大小 ----
 * 辦公、繪圖、PDF 這類程式的事件處理函式本來就吃 XKeyEvent／XButtonEvent：Wayland 路徑的 pmwl_next 取到的事件
 * 用 pmwl_to_xevent 轉過去，同一份處理程式碼兩個後端共用。按鍵的 keysym 與文字存在函式庫裡，用 pmwl_xkey 取回
 * （display 是 NULL 的就是轉出來的事件）。 */
static unsigned shim_btn;
static int shim_px, shim_py;   /* 最後的滑鼠位置（視窗座標）：按鍵事件的 x_root／y_root（Menu 鍵開選單用） */   /* 目前按住的滑鼠鍵（Button1Mask…），X 事件的 state 會帶 */
static struct { KeySym ks, ks0; char text[64]; int len; } shim_key;

void pmwl_resize(PmWl *win, int w, int h)
{
	if (!win || w < 1 || h < 1 || (win->w == w && win->h == h)) return;
	if (win->fullscreen || win->maximized) return;
	win->w = w; win->h = h;
}

int pmwl_to_xevent(const PmwEvent *e, XEvent *x)
{
	memset(x, 0, sizeof *x);
	Window id = (Window)(uintptr_t)e->win;
	if (e->type == PMW_DOWN || e->type == PMW_UP || e->type == PMW_MOTION || e->type == PMW_ENTER) { shim_px = e->x; shim_py = e->y; }
	static const unsigned bm[] = { 0, Button1Mask, Button2Mask, Button3Mask };
	switch (e->type) {
	case PMW_EXPOSE: x->type = Expose; x->xexpose.window = id; return 1;
	case PMW_SIZE: x->type = ConfigureNotify; x->xconfigure.window = id; x->xconfigure.width = e->w; x->xconfigure.height = e->h; return 1;
	case PMW_DOWN: case PMW_UP: {
		int dn = e->type == PMW_DOWN;
		unsigned prev = shim_btn;   /* X 的 state 是事件發生之前的狀態 */
		if (e->button >= 1 && e->button <= 3) { if (dn) shim_btn |= bm[e->button]; else shim_btn &= ~bm[e->button]; }
		x->type = dn ? ButtonPress : ButtonRelease;
		x->xbutton.window = id; x->xbutton.x = e->x; x->xbutton.y = e->y; x->xbutton.button = (unsigned)e->button;
		x->xbutton.time = (Time)e->time; x->xbutton.state = e->state | prev;
		x->xbutton.x_root = e->x; x->xbutton.y_root = e->y;   /* 視窗座標：pmwl_menu 要的就是這個 */
		return 1;
	}
	case PMW_MOTION:
		x->type = MotionNotify; x->xmotion.window = id; x->xmotion.x = e->x; x->xmotion.y = e->y;
		x->xmotion.time = (Time)e->time; x->xmotion.state = e->state | shim_btn;
		x->xmotion.x_root = e->x; x->xmotion.y_root = e->y;
		return 1;
	case PMW_SCROLL:   /* X 的滾輪是按鍵 4～7 */
		x->type = ButtonPress; x->xbutton.window = id; x->xbutton.x = e->x; x->xbutton.y = e->y;
		x->xbutton.button = (unsigned)e->button; x->xbutton.time = (Time)e->time; x->xbutton.state = e->state | shim_btn;
		x->xbutton.x_root = e->x; x->xbutton.y_root = e->y;
		return 1;
	case PMW_KEY: case PMW_KEYUP: {
		x->type = e->type == PMW_KEY ? KeyPress : KeyRelease;
		x->xkey.window = id; x->xkey.state = e->state | shim_btn; x->xkey.time = (Time)e->time;
		x->xkey.x = x->xkey.x_root = shim_px; x->xkey.y = x->xkey.y_root = shim_py;
		x->xkey.keycode = (unsigned)e->button + 8;
		shim_key.ks = e->key;
		shim_key.len = e->textlen > 0 && e->textlen < (int)sizeof shim_key.text ? e->textlen : 0;
		memcpy(shim_key.text, e->text, (size_t)shim_key.len);
		shim_key.text[shim_key.len] = 0;
		/* 沒修飾鍵的 keysym（X 的 XLookupKeysym(e, 0)）：Ctrl+Shift+= 時是 =，不是 + */
		shim_key.ks0 = e->key;
		km_ensure();
		if (keymap && xstate) {
			xkb_keycode_t kc = (xkb_keycode_t)e->button + 8;
			const xkb_keysym_t *syms;
			if (xkb_keymap_key_get_syms_by_level(keymap, kc, xkb_state_key_get_layout(xstate, kc), 0, &syms) > 0) shim_key.ks0 = syms[0];
		}
		return 1;
	}
	case PMW_CLOSE: x->type = ClientMessage; x->xclient.window = id; x->xclient.format = 32; x->xclient.data.l[0] = PMWL_XCLOSE; return 1;
	case PMW_FOCUS_IN: x->type = FocusIn; x->xfocus.window = id; return 1;
	case PMW_FOCUS_OUT: x->type = FocusOut; x->xfocus.window = id; return 1;
	case PMW_ENTER: x->type = EnterNotify; x->xcrossing.window = id; x->xcrossing.x = e->x; x->xcrossing.y = e->y; return 1;
	case PMW_LEAVE: x->type = LeaveNotify; x->xcrossing.window = id; x->xcrossing.x = e->x; x->xcrossing.y = e->y; return 1;
	}
	return 0;
}

int pmwl_xkey(const XKeyEvent *e, char *buf, int n, KeySym *ks, KeySym *ks0)
{
	if (e->display) return -1;
	int len = MIN(shim_key.len, n > 0 ? n - 1 : 0);
	if (len > 0) memcpy(buf, shim_key.text, (size_t)len);
	if (n > 0) buf[len > 0 ? len : 0] = 0;
	if (ks) *ks = shim_key.ks;
	if (ks0) *ks0 = shim_key.ks0;
	return len;
}

/* evdev 按鍵碼 → PC 掃描碼（set 1，遠端桌面、虛擬機器用）：0xe0 開頭的延伸鍵加 0x100；Pause 回傳 0xffff；不知道回傳 0 */
unsigned pmwl_scancode(unsigned code)
{
	if (code >= 1 && code <= 88) return code == 85 ? 0x29 : code;
	switch (code) {
	case 89: return 0x73; case 92: return 0x79; case 93: return 0x70; case 94: return 0x7b;
	case 96: return 0x11c; case 97: return 0x11d; case 98: return 0x135; case 99: return 0x137; case 100: return 0x138;
	case 102: return 0x147; case 103: return 0x148; case 104: return 0x149; case 105: return 0x14b; case 106: return 0x14d;
	case 107: return 0x14f; case 108: return 0x150; case 109: return 0x151; case 110: return 0x152; case 111: return 0x153;
	case 113: return 0x120; case 114: return 0x12e; case 115: return 0x130; case 116: return 0x15e;
	case 117: return 0x59; case 119: return 0xffff;
	case 121: return 0x7e; case 122: return 0x72; case 123: return 0x71; case 124: return 0x7d;
	case 125: return 0x15b; case 126: return 0x15c; case 127: return 0x15d; case 142: return 0x15f; case 143: return 0x163;
	}
	if (code >= 183 && code <= 193) return 0x64 + (code - 183);
	if (code == 194) return 0x76;
	return 0;
}

/* 程式要自己等 Wayland 連線的 fd（跟別的事件來源一起等，例如 FreeRDP 的 WaitForMultipleObjects）；可讀時呼叫 pmwl_next(&ev, 0) 到沒有事件為止 */
int pmwl_fd(void) { return pmwl_connect() ? wl_display_get_fd(wd) : -1; }
