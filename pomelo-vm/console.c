/* SPDX-License-Identifier: GPL-3.0-or-later */
/* console.c — 虛擬機器的畫面視窗（被 pomelo-vm.c #include）
 *
 * 一台虛擬機器一個視窗（一個行程）：pomelo-vm --console <id>。
 * 畫面走 QEMU 內建的 VNC 伺服器（UNIX socket，只有自己能連）；這裡是自己寫的 VNC 用戶端：
 *   畫面：Raw、CopyRect（本機 socket 不用壓縮），游標形狀、解析度改變、ExtendedDesktopSize（解析度跟著視窗）
 *   鍵盤：QEMU 的延伸按鍵事件，直接送 PC 掃描碼，不管本機與虛擬機器的鍵盤配置
 *   滑鼠：USB 繪圖板，絕對座標，不用抓住滑鼠
 *   聲音：QEMU 沒有 PulseAudio 模組時（Ubuntu 預設）改走 VNC 的聲音通道，用 libpulse-simple 播
 * 開關機、暫停、光碟、快照透過 QMP。
 * 快捷鍵：Ctrl+Alt+Enter 全螢幕、Ctrl+Alt+End 送 Ctrl+Alt+Del、Ctrl+Alt+Home 全螢幕時顯示工具列
 */

#define BAR_H 40

static Vm *CV;                       /* 這個視窗的虛擬機器 */
static Window cwin;
static PmWl *cwl;                    /* 原生 Wayland 的視窗（NULL＝X11）。原生時 dpy 是 NULL，xsurf 是記憶體影像，主迴圈整張送給 compositor */
static int wl_dirty;                 /* xsurf 有新內容還沒送 */
static unsigned last_mods;           /* 最近一次鍵盤滑鼠事件的修飾鍵（原生：切回視窗時對 Caps／Num Lock 用） */

static cairo_surface_t *dot_img;     /* 原生：本機的小圓點游標（虛擬機器的游標畫在畫面裡時用） */
static char *cut_pending;            /* 原生：虛擬機器傳來要放上本機剪貼簿的文字（要在主迴圈放，序號才新） */
static char *cut_last;               /* 最近一次放上去的文字；放的當下還沒有輸入（序號太舊）時 compositor 會忽略，下一次按鍵或按滑鼠時再放一次 */
static int cut_retry;
#define FLUSH_X() do { if (dpy) XFlush(dpy); else wl_dirty = 1; } while (0)
static cairo_surface_t *xsurf;
static int ww, wh;                   /* 視窗大小 */
static int fbw, fbh;                 /* 虛擬機器的畫面大小 */
static cairo_surface_t *fb;          /* 畫面（RGB24，VNC 直接寫進來） */
static double sc = 1;
static int ox, oy;                   /* 畫面在畫面區裡的位置（置中） */
static int bar_h = BAR_H;            /* 工具列高度（全螢幕時 0） */
static int vfd = -1, qfd = -1;       /* VNC、QMP（事件） */
static char qbuf[65536];
static size_t qhave;
static int fullscreen, paused, quit_req, vm_gone;
static int ext_keys, ext_desktop, follow_size = 1, user_sized, too_big;
static int qemu_major;               /* QEMU 7 起可以請虛擬機器改解析度（ExtendedDesktopSize）；剛連上時 QEMU 不會先告訴我們 */
static int want_w, want_h;           /* 上次自己要求的視窗大小 */
static long size_due, thumb_due, last_req_ms;
static int sent_w, sent_h;           /* 上次請虛擬機器改成的解析度 */
static int guest_leds = -1;          /* 虛擬機器的 Caps／Num／Scroll Lock 燈（-1 = 不知道） */
static unsigned btn_mask;
static int pending_motion, mx_, my_;
static int fb_dirty_thumb, no_thumb;
static Atom a_del, a_netstate, a_netfull, a_netname, a_utf8, a_active;
static Cursor dot_cursor, guest_cursor;
static unsigned keymap[256];
static unsigned char pressed[256];
static char busy_msg[160];

/* 工具列 */
enum { TB_POWER, TB_PAUSE, TB_CD, TB_USB, TB_SNAP, TB_KEYS, TB_VIEW, TB_EXITFULL, TB_N };
static Rect tb_r[TB_N];
static int tb_hover = -1, tb_float;      /* tb_float：全螢幕時浮在上面的工具列 */
static long tb_show_at, tb_hide_at;
static Rect tb_box;

static void paint_all(void);
static void xsurf_resize(void);
static void wl_present(int force);
static void tb_draw(void);
static void save_thumb(void);

/* ------------------------------------------------------------------ */
/* 聲音（VNC 的聲音通道 → libpulse-simple）                               */
/* ------------------------------------------------------------------ */

#define AU_RATE 44100
#define AU_RING (AU_RATE * 4)        /* 一秒 */
static unsigned char au_ring[AU_RING];
static size_t au_r, au_w, au_n;
static pthread_mutex_t au_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t au_cv = PTHREAD_COND_INITIALIZER;
static int au_on, au_thread_ok, au_vnc;

static void *au_main(void *arg)
{
	pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = AU_RATE, .channels = 2 };
	pa_buffer_attr ba = { .maxlength = (uint32_t)-1, .tlength = AU_RATE * 4 / 10, .prebuf = (uint32_t)-1,
	                      .minreq = (uint32_t)-1, .fragsize = (uint32_t)-1 };
	int err;
	char name[200];
	snprintf(name, sizeof name, "%s", CV->name);
	pa_simple *pa = pa_simple_new(NULL, _("虛擬機器"), PA_STREAM_PLAYBACK, NULL, name, &ss, NULL, &ba, &err);
	if (!pa) { au_on = 0; return NULL; }
	unsigned char chunk[4096];
	for (;;) {
		pthread_mutex_lock(&au_mx);
		while (!au_n) pthread_cond_wait(&au_cv, &au_mx);
		size_t k = MIN(au_n, sizeof chunk);
		for (size_t i = 0; i < k; i++) chunk[i] = au_ring[(au_r + i) % AU_RING];
		au_r = (au_r + k) % AU_RING;
		au_n -= k;
		pthread_mutex_unlock(&au_mx);
		if (pa_simple_write(pa, chunk, k, &err) < 0) break;
	}
	pa_simple_free(pa);
	return NULL;
}

static void au_push(const unsigned char *d, size_t n)
{
	if (getenv("POMELO_VM_DEBUG")) {
		static size_t total;
		static long last;
		total += n;
		if (pm_now_ms() - last > 1000) { fprintf(stderr, "pomelo-vm: audio %zu bytes\n", total); last = pm_now_ms(); }
	}
	if (!au_on) return;
	if (!au_thread_ok) {
		pthread_t t;
		au_thread_ok = 1;
		if (pthread_create(&t, NULL, au_main, NULL)) { au_on = 0; return; }
		pthread_detach(t);
	}
	pthread_mutex_lock(&au_mx);
	for (size_t i = 0; i < n; i++) {
		au_ring[au_w] = d[i];
		au_w = (au_w + 1) % AU_RING;
		if (au_n < AU_RING) au_n++;
		else au_r = (au_r + 1) % AU_RING;   /* 滿了：丟掉最舊的 */
	}
	pthread_cond_signal(&au_cv);
	pthread_mutex_unlock(&au_mx);
}

/* ------------------------------------------------------------------ */
/* VNC 連線（本機 socket：一則訊息開始後就整則讀完）                        */
/* ------------------------------------------------------------------ */

static int vread(void *buf, size_t n)
{
	unsigned char *p = buf;
	while (n) {
		ssize_t r = read(vfd, p, n);
		if (r > 0) { p += r; n -= (size_t)r; continue; }
		if (r < 0 && errno == EINTR) continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pf = { vfd, POLLIN, 0 };
			if (poll(&pf, 1, 5000) > 0) continue;
		}
		return -1;
	}
	return 0;
}

static int vskip(size_t n)
{
	unsigned char tmp[4096];
	while (n) {
		size_t k = MIN(n, sizeof tmp);
		if (vread(tmp, k)) return -1;
		n -= k;
	}
	return 0;
}

static int vwrite(const void *buf, size_t n)
{
	const unsigned char *p = buf;
	while (n && vfd >= 0) {
		ssize_t r = write(vfd, p, n);
		if (r > 0) { p += r; n -= (size_t)r; continue; }
		if (r < 0 && errno == EINTR) continue;
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pf = { vfd, POLLOUT, 0 };
			if (poll(&pf, 1, 5000) > 0) continue;
		}
		return -1;
	}
	return 0;
}

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rd32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void wr32(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static void fb_resize(int w, int h)
{
	if (w < 1 || h < 1 || w > 16384 || h > 16384) return;
	if (fb && w == fbw && h == fbh) return;
	if (fb) cairo_surface_destroy(fb);
	fb = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	fbw = w; fbh = h;
}

static void request_update(int incremental)
{
	unsigned char m[10] = { 3, (unsigned char)incremental };
	wr16(m + 6, (unsigned)fbw);
	wr16(m + 8, (unsigned)fbh);
	vwrite(m, sizeof m);
	last_req_ms = pm_now_ms();
}

static int vnc_connect(void)
{
	char sock[PATH_MAX];
	vm_run(CV, "vnc.sock", sock, sizeof sock);
	vfd = -1;
	for (int i = 0; i < 50 && vfd < 0; i++) {   /* 剛開機時 socket 可能還沒好 */
		vfd = unix_connect(sock);
		if (vfd < 0) usleep(100000);
	}
	if (vfd < 0) return -1;
	unsigned char b[64];
	if (vread(b, 12) || memcmp(b, "RFB 003.", 8)) goto fail;
	if (vwrite("RFB 003.008\n", 12)) goto fail;
	if (vread(b, 1) || !b[0]) goto fail;
	unsigned n = b[0];
	unsigned char types[256];
	if (vread(types, n)) goto fail;
	int none = 0;
	for (unsigned i = 0; i < n; i++) if (types[i] == 1) none = 1;
	if (!none) goto fail;
	b[0] = 1;
	if (vwrite(b, 1) || vread(b, 4) || rd32(b)) goto fail;
	b[0] = 1;   /* 共用：管理視窗再開一個也不會把這個踢掉 */
	if (vwrite(b, 1) || vread(b, 24)) goto fail;
	fb_resize((int)rd16(b), (int)rd16(b + 2));
	if (vskip(rd32(b + 20))) goto fail;
	/* 像素格式：32 位元、小端序、紅 16 綠 8 藍 0 ＝ cairo 的 RGB24，直接寫進畫面 */
	unsigned char pf[20] = { 0, 0, 0, 0, 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0 };
	if (vwrite(pf, sizeof pf)) goto fail;
	static const int enc[] = { 1, 0, -223, -308, -239, -258, -257, -261, -259 };
	unsigned char se[4 + 4 * LEN(enc)] = { 2, 0 };
	wr16(se + 2, LEN(enc));
	for (size_t i = 0; i < LEN(enc); i++) wr32(se + 4 + 4 * i, (unsigned)enc[i]);
	if (vwrite(se, sizeof se)) goto fail;
	fcntl(vfd, F_SETFL, O_NONBLOCK);
	request_update(0);
	return 0;
fail:
	close(vfd);
	vfd = -1;
	return -1;
}

/* ------------------------------------------------------------------ */
/* 畫面                                                                 */
/* ------------------------------------------------------------------ */

static void layout(void)
{
	int aw = ww, ah = wh - bar_h;
	if (!fbw || !fbh || aw < 1 || ah < 1) { sc = 1; ox = oy = 0; return; }
	sc = MIN((double)aw / fbw, (double)ah / fbh);
	if (sc > 1 && !fullscreen) sc = 1;   /* 視窗模式不放大（會糊），置中就好 */
	ox = (aw - (int)(fbw * sc)) / 2;
	oy = (ah - (int)(fbh * sc)) / 2;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;
}

/* 把畫面的一塊畫到視窗上。縮放時邊緣多拷 2 個像素，免得相鄰兩塊中間出現細縫 */
static void paint_fb(int x, int y, int w, int h)
{
	if (!xsurf || !fb) return;
	int x0 = CLAMP(x, 0, fbw), y0 = CLAMP(y, 0, fbh), x1 = CLAMP(x + w, x0, fbw), y1 = CLAMP(y + h, y0, fbh);
	if (x1 <= x0 || y1 <= y0) return;
	int m = sc == 1 ? 0 : 2;
	int cx0 = MAX(0, x0 - m), cy0 = MAX(0, y0 - m), cx1 = MIN(fbw, x1 + m), cy1 = MIN(fbh, y1 + m);
	cairo_surface_flush(fb);
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_RGB24, cx1 - cx0, cy1 - cy0);
	unsigned char *dst = cairo_image_surface_get_data(tmp), *src = cairo_image_surface_get_data(fb);
	int ds = cairo_image_surface_get_stride(tmp), ss = cairo_image_surface_get_stride(fb);
	for (int r = cy0; r < cy1; r++)
		memcpy(dst + (size_t)(r - cy0) * ds, src + (size_t)r * ss + (size_t)cx0 * 4, (size_t)(cx1 - cx0) * 4);
	cairo_surface_mark_dirty(tmp);
	cairo_t *cr = cairo_create(xsurf);
	double X0 = floor(ox + x0 * sc), Y0 = floor(bar_h + oy + y0 * sc), X1 = ceil(ox + x1 * sc), Y1 = ceil(bar_h + oy + y1 * sc);
	cairo_rectangle(cr, X0, MAX(Y0, bar_h), X1 - X0, Y1 - MAX(Y0, bar_h));
	cairo_clip(cr);
	cairo_translate(cr, ox, bar_h + oy);
	cairo_scale(cr, sc, sc);
	cairo_set_source_surface(cr, tmp, cx0, cy0);
	cairo_pattern_set_filter(cairo_get_source(cr), sc == 1 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);
}

static void paint_busy(void)
{
	if (!busy_msg[0] || !xsurf) return;
	cairo_t *cr = cairo_create(xsurf);
	pm_font(cr, 15, 0);
	double tw = pm_textw(cr, busy_msg), bw = tw + 48, bh = 54;
	double bx = (ww - bw) / 2, by = bar_h + (wh - bar_h - bh) / 2;
	pm_rrect(cr, bx, by, bw, bh, 12, 12);
	cairo_set_source_rgba(cr, 0.1, 0.1, 0.12, 0.88);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.96, 0.96, 0.97);
	pm_text(cr, bx + 24, by + bh / 2, busy_msg);
	cairo_destroy(cr);
}

static void paint_all(void)
{
	if (!xsurf) return;
	cairo_t *cr = cairo_create(xsurf);
	cairo_rectangle(cr, 0, bar_h, ww, wh - bar_h);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_fill(cr);
	if (!fb || vfd < 0) {
		pm_font(cr, 15, 0);
		cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
		const char *m = vm_gone ? _("虛擬機器已關機") : _("正在連到虛擬機器的畫面…");
		pm_text(cr, (ww - pm_textw(cr, m)) / 2, bar_h + (wh - bar_h) / 2.0, m);
	}
	cairo_destroy(cr);
	if (fb && vfd >= 0) paint_fb(0, 0, fbw, fbh);
	tb_draw();
	paint_busy();
	cairo_surface_flush(xsurf);
	wl_dirty |= cwl != NULL;
}

/* 一段時間要做很久的事（儲存狀態、拍快照）：先在畫面中間顯示訊息 */
static void busy(const char *msg)
{
	snprintf(busy_msg, sizeof busy_msg, "%s", msg ? msg : "");
	if (msg) { paint_busy(); cairo_surface_flush(xsurf); FLUSH_X(); wl_present(1); }
	else { paint_all(); wl_present(1); }
}

static void set_title(void)
{
	char t[300];
	if (paused) snprintf(t, sizeof t, "%s（%s）— %s", CV->name, _("已暫停"), _("虛擬機器"));
	else snprintf(t, sizeof t, "%s — %s", CV->name, _("虛擬機器"));
	if (cwl) { pmwl_set_title(cwl, t); return; }
	XStoreName(dpy, cwin, t);
	XChangeProperty(dpy, cwin, a_netname, a_utf8, 8, PropModeReplace, (unsigned char *)t, (int)strlen(t));
}

/* 視窗大小跟著虛擬機器的解析度（使用者自己拉過大小、全螢幕時就不動），保持在螢幕裡。
 * 畫面比螢幕還大時，視窗只能開到螢幕的九成：可以的話請虛擬機器改用放得下的解析度 */
static void fit_window(void)
{
	if (fullscreen || user_sized || !fbw || !fbh) return;
	int rx = 0, ry = 0;
	Window child;
	Rect mon;
	if (cwl) {   /* Wayland 程式不知道也不能指定視窗位置：用第一個螢幕的大小估，視窗由 compositor 擺 */
		Rect o[8];
		mon = pmwl_outputs(o, 8) > 0 ? o[0] : (Rect){ 0, 0, 1280, 720 };
	} else {
		pm_update_monitors();
		XTranslateCoordinates(dpy, cwin, pm_root, 0, 0, &rx, &ry, &child);
		int m = pm_mon_at(rx + ww / 2, ry + wh / 2);
		if (m < 0) m = pm_pointer_mon();
		mon = mons[m < 0 ? 0 : m];
	}
	if (mon.w < 200 || mon.h < 200) mon = (Rect){ 0, 0, sw, sh };
	int maxw = mon.w * 92 / 100, maxh = mon.h * 88 / 100;
	int w = MIN(fbw, maxw), h = MIN(fbh + BAR_H, maxh);
	too_big = fbw > maxw || fbh + BAR_H > maxh;
	if (too_big) size_due = pm_now_ms() + 300;
	if (w == ww && h == wh) return;
	want_w = w; want_h = h;
	if (cwl) { pmwl_resize(cwl, w, h); ww = w; wh = h; xsurf_resize(); layout(); paint_all(); return; }
	int cx = rx + ww / 2, cy = ry + wh / 2;
	int x = CLAMP(cx - w / 2, mon.x, mon.x + mon.w - w), y = CLAMP(cy - h / 2, mon.y + 32, MAX(mon.y + 32, mon.y + mon.h - h));
	XMoveResizeWindow(dpy, cwin, x, y, (unsigned)w, (unsigned)h);
}

/* ------------------------------------------------------------------ */
/* 滑鼠游標                                                              */
/* ------------------------------------------------------------------ */

/* 虛擬機器沒有硬體游標（標準 VGA）時游標是畫在畫面裡的：本機只顯示一個小點，免得兩個箭頭疊在一起 */
static Cursor make_dot_cursor(void)
{
	if (cwl) {
		dot_img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 6, 6);
		unsigned *px = (unsigned *)cairo_image_surface_get_data(dot_img);
		for (int y = 0; y < 6; y++)
			for (int x = 0; x < 6; x++) {
				double d = hypot(x - 2.5, y - 2.5);
				px[y * 6 + x] = d < 1.6 ? 0xff000000 : d < 2.7 ? 0xffffffff : 0;
			}
		cairo_surface_mark_dirty(dot_img);
		pmwl_cursor_image(cwl, dot_img, 2, 2);
		return None;
	}
	XcursorImage *img = XcursorImageCreate(6, 6);
	if (!img) return None;
	img->xhot = img->yhot = 2;
	for (int y = 0; y < 6; y++)
		for (int x = 0; x < 6; x++) {
			double d = hypot(x - 2.5, y - 2.5);
			img->pixels[y * 6 + x] = d < 1.6 ? 0xff000000 : d < 2.7 ? 0xffffffff : 0;
		}
	Cursor c = XcursorImageLoadCursor(dpy, img);
	XcursorImageDestroy(img);
	return c;
}

static int read_cursor(int hx, int hy, int w, int h)
{
	size_t npx = (size_t)w * h * 4, mlen = (size_t)((w + 7) / 8) * h;
	unsigned char *px = malloc(npx + 1), *mask = malloc(mlen + 1);
	if (vread(px, npx) || vread(mask, mlen)) { free(px); free(mask); return -1; }
	if (cwl) {   /* 原生：游標圖直接給 compositor（ARGB32 預乘），0×0＝虛擬機器把游標藏起來 */
		if (w > 0 && h > 0 && w <= 256 && h <= 256) {
			cairo_surface_t *im = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
			unsigned *dst = (unsigned *)cairo_image_surface_get_data(im);
			int stride = (w + 7) / 8;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					const unsigned char *p = px + ((size_t)y * w + x) * 4;
					int on = mask[y * stride + x / 8] & (0x80 >> (x % 8));
					dst[y * w + x] = on ? 0xff000000u | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0] : 0;
				}
			cairo_surface_mark_dirty(im);
			pmwl_cursor_image(cwl, im, CLAMP(hx, 0, w - 1), CLAMP(hy, 0, h - 1));
			cairo_surface_destroy(im);
		} else pmwl_cursor(cwl, PMWL_CURSOR_HIDDEN);
		free(px);
		free(mask);
		return 0;
	}
	if (guest_cursor) { XFreeCursor(dpy, guest_cursor); guest_cursor = None; }
	XcursorImage *img = w > 0 && h > 0 ? XcursorImageCreate(w, h) : NULL;
	if (img) {
		img->xhot = (XcursorDim)CLAMP(hx, 0, w - 1);
		img->yhot = (XcursorDim)CLAMP(hy, 0, h - 1);
		int stride = (w + 7) / 8;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const unsigned char *p = px + ((size_t)y * w + x) * 4;
				int on = mask[y * stride + x / 8] & (0x80 >> (x % 8));
				img->pixels[y * w + x] = on ? 0xff000000u | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0] : 0;
			}
		guest_cursor = XcursorImageLoadCursor(dpy, img);
		XcursorImageDestroy(img);
	} else {   /* 0×0：虛擬機器把游標藏起來 */
		char z = 0;
		Pixmap pm = XCreateBitmapFromData(dpy, cwin, &z, 1, 1);
		XColor col = { 0 };
		guest_cursor = XCreatePixmapCursor(dpy, pm, pm, &col, &col, 0, 0);
		XFreePixmap(dpy, pm);
	}
	XDefineCursor(dpy, cwin, guest_cursor ? guest_cursor : dot_cursor);
	free(px);
	free(mask);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 收 VNC 訊息                                                           */
/* ------------------------------------------------------------------ */

typedef struct { int x, y, w, h; } Dmg;
static Dmg dmg[64];
static int ndmg;

static void damage(int x, int y, int w, int h)
{
	if (ndmg < (int)LEN(dmg)) { dmg[ndmg++] = (Dmg){ x, y, w, h }; return; }
	/* 太多塊：合成一塊 */
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	for (int i = 0; i < ndmg; i++) {
		x0 = MIN(x0, dmg[i].x); y0 = MIN(y0, dmg[i].y);
		x1 = MAX(x1, dmg[i].x + dmg[i].w); y1 = MAX(y1, dmg[i].y + dmg[i].h);
	}
	ndmg = 1;
	dmg[0] = (Dmg){ x0, y0, x1 - x0, y1 - y0 };
}

static void on_desktop_size(int w, int h)
{
	fb_resize(w, h);
	layout();
	fit_window();
	paint_all();
	ndmg = 0;
}

static int read_update(void)
{
	unsigned char h[3];
	if (vread(h, 3)) return -1;
	unsigned n = rd16(h + 1);
	int resized = 0;
	for (unsigned i = 0; i < n; i++) {
		unsigned char r[12];
		if (vread(r, 12)) return -1;
		int x = (int)rd16(r), y = (int)rd16(r + 2), w = (int)rd16(r + 4), hh = (int)rd16(r + 6);
		int enc = (int)rd32(r + 8);
		switch (enc) {
		case 0: {   /* Raw */
			size_t row = (size_t)w * 4;
			if (!fb || x + w > fbw || y + hh > fbh) { if (vskip(row * hh)) return -1; break; }
			unsigned char *d = cairo_image_surface_get_data(fb);
			int st = cairo_image_surface_get_stride(fb);
			cairo_surface_flush(fb);
			if (w == fbw && st == w * 4) { if (vread(d + (size_t)y * st, row * hh)) return -1; }
			else for (int k = 0; k < hh; k++) if (vread(d + (size_t)(y + k) * st + (size_t)x * 4, row)) return -1;
			cairo_surface_mark_dirty_rectangle(fb, x, y, w, hh);
			damage(x, y, w, hh);
			fb_dirty_thumb = 1;
			break;
		}
		case 1: {   /* CopyRect */
			unsigned char s[4];
			if (vread(s, 4)) return -1;
			int sx = (int)rd16(s), sy = (int)rd16(s + 2);
			if (!fb || x + w > fbw || y + hh > fbh || sx + w > fbw || sy + hh > fbh) break;
			unsigned char *d = cairo_image_surface_get_data(fb);
			int st = cairo_image_surface_get_stride(fb);
			cairo_surface_flush(fb);
			if (sy < y) for (int k = hh - 1; k >= 0; k--) memmove(d + (size_t)(y + k) * st + x * 4, d + (size_t)(sy + k) * st + sx * 4, (size_t)w * 4);
			else for (int k = 0; k < hh; k++) memmove(d + (size_t)(y + k) * st + x * 4, d + (size_t)(sy + k) * st + sx * 4, (size_t)w * 4);
			cairo_surface_mark_dirty_rectangle(fb, x, y, w, hh);
			damage(x, y, w, hh);
			fb_dirty_thumb = 1;
			break;
		}
		case -223:  /* DesktopSize */
			on_desktop_size(w, hh);
			resized = 1;
			break;
		case -308: {   /* ExtendedDesktopSize：x = 原因、y = 結果 */
			unsigned char s[4];
			if (vread(s, 4) || vskip((size_t)s[0] * 16)) return -1;
			ext_desktop = 1;
			if (w != fbw || hh != fbh) { on_desktop_size(w, hh); resized = 1; }
			break;
		}
		case -239:  /* Cursor */
			if (read_cursor(x, y, w, hh)) return -1;
			break;
		case -258: ext_keys = 1; break;          /* QEMU 延伸按鍵事件：伺服器支援 */
		case -257: break;                         /* 滑鼠模式（絕對／相對）：一律用 USB 繪圖板 */
		case -261: {                              /* 鍵盤燈 */
			unsigned char l;
			if (vread(&l, 1)) return -1;
			guest_leds = l;
			break;
		}
		case -259:                                /* QEMU 聲音：伺服器支援 */
			if (au_vnc) {
				unsigned char m[10] = { 255, 1, 0, 2, 3, 2 };   /* 設定格式：S16、雙聲道、44100 Hz */
				wr32(m + 6, AU_RATE);
				unsigned char en[4] = { 255, 1, 0, 0 };          /* 開始送聲音 */
				vwrite(m, sizeof m);
				vwrite(en, sizeof en);
				au_on = 1;
			}
			break;
		default:
			fprintf(stderr, "pomelo-vm: VNC: unknown encoding %d\n", enc);
			return -1;
		}
	}
	if (!resized || ndmg) {
		for (int i = 0; i < ndmg; i++) paint_fb(dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
		if (tb_float) tb_draw();
		paint_busy();
		cairo_surface_flush(xsurf);
		wl_dirty |= cwl != NULL;
	}
	ndmg = 0;
	request_update(1);
	return 0;
}

static int read_message(void)
{
	unsigned char t;
	ssize_t r = read(vfd, &t, 1);
	if (r == 0) return -1;
	if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 1 : -1;
	unsigned char b[8];
	switch (t) {
	case 0: return read_update();
	case 1:   /* SetColourMapEntries（真彩色用不到，讀掉） */
		if (vread(b, 5)) return -1;
		return vskip((size_t)rd16(b + 3) * 6);
	case 2: return 0;   /* Bell */
	case 3: {           /* ServerCutText（原生 Wayland：放上本機剪貼簿，Latin-1 文字；X11 版沒有剪貼簿同步） */
		if (vread(b, 7)) return -1;
		int len = (int)rd32(b + 3);
		if (cwl && len > 0 && len < (1 << 20)) {
			unsigned char *t = malloc((size_t)len);
			if (!t) return -1;
			if (vread(t, (size_t)len)) { free(t); return -1; }
			char *u = malloc((size_t)len * 2 + 1);
			size_t k = 0;
			for (int i = 0; i < len; i++) { unsigned c = t[i]; if (c < 0x80) u[k++] = (char)c; else { u[k++] = (char)(0xc0 | (c >> 6)); u[k++] = (char)(0x80 | (c & 0x3f)); } }
			u[k] = 0;
			free(t);
			free(cut_pending);
			cut_pending = u;
			return 0;
		}
		return vskip((size_t)(len < 0 ? -len : len));
	}
	case 255: {         /* QEMU 自訂訊息：聲音 */
		if (vread(b, 3)) return -1;
		if (b[0] != 1) return -1;
		if (rd16(b + 1) == 2) {
			unsigned char l[4];
			if (vread(l, 4)) return -1;
			size_t n = rd32(l);
			if (n > (1 << 24)) return -1;
			unsigned char *d = malloc(n + 1);
			if (vread(d, n)) { free(d); return -1; }
			au_push(d, n);
			free(d);
		}
		return 0;
	}
	default:
		fprintf(stderr, "pomelo-vm: VNC: unknown message %d\n", t);
		return -1;
	}
}

/* ------------------------------------------------------------------ */
/* 鍵盤：X keycode → PC 掃描碼（QEMU 的編號：0xe0 開頭的延伸鍵 = 0x80 | 第二碼）  */
/* ------------------------------------------------------------------ */

static const struct { const char *n; unsigned sc; } keynames[] = {
	{ "ESC", 0x01 }, { "AE01", 0x02 }, { "AE02", 0x03 }, { "AE03", 0x04 }, { "AE04", 0x05 }, { "AE05", 0x06 },
	{ "AE06", 0x07 }, { "AE07", 0x08 }, { "AE08", 0x09 }, { "AE09", 0x0a }, { "AE10", 0x0b }, { "AE11", 0x0c },
	{ "AE12", 0x0d }, { "BKSP", 0x0e }, { "TAB", 0x0f }, { "AD01", 0x10 }, { "AD02", 0x11 }, { "AD03", 0x12 },
	{ "AD04", 0x13 }, { "AD05", 0x14 }, { "AD06", 0x15 }, { "AD07", 0x16 }, { "AD08", 0x17 }, { "AD09", 0x18 },
	{ "AD10", 0x19 }, { "AD11", 0x1a }, { "AD12", 0x1b }, { "RTRN", 0x1c }, { "LCTL", 0x1d }, { "AC01", 0x1e },
	{ "AC02", 0x1f }, { "AC03", 0x20 }, { "AC04", 0x21 }, { "AC05", 0x22 }, { "AC06", 0x23 }, { "AC07", 0x24 },
	{ "AC08", 0x25 }, { "AC09", 0x26 }, { "AC10", 0x27 }, { "AC11", 0x28 }, { "TLDE", 0x29 }, { "LFSH", 0x2a },
	{ "BKSL", 0x2b }, { "AC12", 0x2b }, { "AB01", 0x2c }, { "AB02", 0x2d }, { "AB03", 0x2e }, { "AB04", 0x2f },
	{ "AB05", 0x30 }, { "AB06", 0x31 }, { "AB07", 0x32 }, { "AB08", 0x33 }, { "AB09", 0x34 }, { "AB10", 0x35 },
	{ "RTSH", 0x36 }, { "KPMU", 0x37 }, { "LALT", 0x38 }, { "SPCE", 0x39 }, { "CAPS", 0x3a }, { "FK01", 0x3b },
	{ "FK02", 0x3c }, { "FK03", 0x3d }, { "FK04", 0x3e }, { "FK05", 0x3f }, { "FK06", 0x40 }, { "FK07", 0x41 },
	{ "FK08", 0x42 }, { "FK09", 0x43 }, { "FK10", 0x44 }, { "NMLK", 0x45 }, { "SCLK", 0x46 }, { "KP7", 0x47 },
	{ "KP8", 0x48 }, { "KP9", 0x49 }, { "KPSU", 0x4a }, { "KP4", 0x4b }, { "KP5", 0x4c }, { "KP6", 0x4d },
	{ "KPAD", 0x4e }, { "KP1", 0x4f }, { "KP2", 0x50 }, { "KP3", 0x51 }, { "KP0", 0x52 }, { "KPDL", 0x53 },
	{ "LSGT", 0x56 }, { "FK11", 0x57 }, { "FK12", 0x58 }, { "KPEQ", 0x59 }, { "FK13", 0x64 }, { "FK14", 0x65 },
	{ "FK15", 0x66 }, { "FK16", 0x67 }, { "FK17", 0x68 }, { "FK18", 0x69 }, { "FK19", 0x6a }, { "FK20", 0x6b },
	{ "HKTG", 0x70 }, { "HJCV", 0x71 }, { "HNGL", 0x72 }, { "AB11", 0x73 }, { "HENK", 0x79 }, { "MUHE", 0x7b },
	{ "AE13", 0x7d }, { "KPPT", 0x7e },
	{ "KPEN", 0x9c }, { "RCTL", 0x9d }, { "KPDV", 0xb5 }, { "PRSC", 0xb7 }, { "SYRQ", 0x54 },
	{ "RALT", 0xb8 }, { "ALGR", 0xb8 }, { "LVL3", 0xb8 }, { "HOME", 0xc7 }, { "UP", 0xc8 },
	{ "PGUP", 0xc9 }, { "LEFT", 0xcb }, { "RGHT", 0xcd }, { "END", 0xcf }, { "DOWN", 0xd0 },
	{ "PGDN", 0xd1 }, { "INS", 0xd2 }, { "DELE", 0xd3 }, { "LWIN", 0xdb }, { "LMTA", 0xdb },
	{ "RWIN", 0xdc }, { "RMTA", 0xdc }, { "COMP", 0xdd }, { "MENU", 0xdd }, { "MUTE", 0xa0 },
	{ "VOL-", 0xae }, { "VOL+", 0xb0 }, { "POWR", 0xde }, { "SLEP", 0xdf }, { "WAKE", 0xe3 },
	{ "PAUS", 0xc6 },
};

static void keymap_init(void)
{
	memset(keymap, 0, sizeof keymap);
	if (cwl) {   /* 原生：PMW_KEY 帶 evdev 按鍵碼（xkb 的 keycode＝evdev＋8），換成 QEMU 的編號（0xe0 開頭的延伸鍵＝0x80｜第二碼；Pause＝0xc6） */
		for (unsigned code = 1; code < 248; code++) {
			unsigned sc = pmwl_scancode(code);
			keymap[code + 8] = sc == 0xffff ? 0xc6 : (sc & 0x100) ? 0x80 | (sc & 0xff) : sc;
		}
		return;
	}
	XkbDescPtr xkb = XkbGetMap(dpy, 0, XkbUseCoreKbd);
	if (xkb && XkbGetNames(dpy, XkbKeyNamesMask, xkb) == Success && xkb->names && xkb->names->keys) {
		for (int kc = xkb->min_key_code; kc <= xkb->max_key_code && kc < 256; kc++) {
			char n[XkbKeyNameLength + 1];
			memcpy(n, xkb->names->keys[kc].name, XkbKeyNameLength);
			n[XkbKeyNameLength] = 0;
			if (!n[0]) continue;
			for (size_t i = 0; i < LEN(keynames); i++)
				if (!strcmp(n, keynames[i].n)) { keymap[kc] = keynames[i].sc; break; }
		}
	}
	if (xkb) XkbFreeKeyboard(xkb, 0, True);
	for (int kc = 9; kc < 97; kc++)   /* 沒名字的按鍵：當作 evdev（Linux 按鍵碼 + 8），前 88 個跟 PC 掃描碼一樣 */
		if (!keymap[kc]) keymap[kc] = (unsigned)(kc - 8);
	int ok;
	XkbSetDetectableAutoRepeat(dpy, True, &ok);
}

static void send_key(unsigned sc, KeySym ks, int down)
{
	if (vfd < 0) return;
	if (ext_keys && sc) {
		unsigned char m[12] = { 255, 0 };
		wr16(m + 2, (unsigned)down);
		wr32(m + 4, (unsigned)ks);
		wr32(m + 8, sc);
		vwrite(m, sizeof m);
	} else if (ks) {
		unsigned char m[8] = { 4, (unsigned char)down };
		wr32(m + 4, (unsigned)ks);
		vwrite(m, sizeof m);
	}
}

static void tap(unsigned sc, int down) { send_key(sc, 0, down); }

/* 一組組合鍵：依序按下、反序放開 */
static void send_combo(const unsigned *sc, int n)
{
	for (int i = 0; i < n; i++) tap(sc[i], 1);
	for (int i = n - 1; i >= 0; i--) tap(sc[i], 0);
}

static void release_all_keys(void)
{
	for (int kc = 0; kc < 256; kc++)
		if (pressed[kc]) { pressed[kc] = 0; tap(keymap[kc], 0); }
}

/* 切回視窗時，把虛擬機器的 Caps Lock、Num Lock 對成跟本機一樣 */
static void sync_locks(void)
{
	if (guest_leds < 0 || vfd < 0 || !ext_keys) return;
	int caps, nl;
	if (cwl) { caps = (last_mods & LockMask) != 0; nl = (last_mods & Mod2Mask) != 0; }   /* Wayland 沒有查鎖定鍵的 API：用最近一次事件的修飾鍵 */
	else {
		XkbStateRec st;
		if (XkbGetState(dpy, XkbUseCoreKbd, &st) != Success) return;
		unsigned num = XkbKeysymToModifiers(dpy, XK_Num_Lock);
		caps = (st.locked_mods & LockMask) != 0; nl = num && (st.locked_mods & num);
	}
	if (caps != ((guest_leds & 4) != 0)) { tap(0x3a, 1); tap(0x3a, 0); }
	if (nl != ((guest_leds & 2) != 0)) { tap(0x45, 1); tap(0x45, 0); }
}

/* ------------------------------------------------------------------ */
/* 滑鼠                                                                 */
/* ------------------------------------------------------------------ */

static int in_screen(int x, int y)
{
	if (tb_float && x >= tb_box.x && x < tb_box.x + tb_box.w && y >= tb_box.y && y < tb_box.y + tb_box.h) return 0;
	return y >= bar_h;
}

static void send_pointer(int x, int y)
{
	if (vfd < 0 || !fbw) return;
	int X = (int)((x - ox) / sc), Y = (int)((y - bar_h - oy) / sc);
	X = CLAMP(X, 0, fbw - 1);
	Y = CLAMP(Y, 0, fbh - 1);
	unsigned char m[6] = { 5, (unsigned char)btn_mask };
	wr16(m + 2, (unsigned)X);
	wr16(m + 4, (unsigned)Y);
	vwrite(m, sizeof m);
}

/* ------------------------------------------------------------------ */
/* 控制（QMP）                                                           */
/* ------------------------------------------------------------------ */

static void qmp_events(void)
{
	for (;;) {
		ssize_t r = read(qfd, qbuf + qhave, sizeof qbuf - 1 - qhave);
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) { close(qfd); qfd = -1; return; }
		if (r < 0) break;
		qhave += (size_t)r;
		if (qhave >= sizeof qbuf - 1) qhave = 0;
	}
	char *nl;
	while ((nl = memchr(qbuf, '\n', qhave))) {
		*nl = 0;
		Js *j = js_parse(qbuf);
		const char *ev = js_str(js_get(j, "event"), "");
		if (!strcmp(ev, "STOP")) { paused = 1; set_title(); tb_draw(); cairo_surface_flush(xsurf); wl_dirty |= cwl != NULL; }
		else if (!strcmp(ev, "RESUME")) { paused = 0; set_title(); tb_draw(); cairo_surface_flush(xsurf); wl_dirty |= cwl != NULL; }
		else if (!strcmp(ev, "SHUTDOWN")) vm_gone = no_thumb = 1;
		else if (!strcmp(ev, "POWERDOWN")) no_thumb = 1;   /* 關機中的畫面不要拿來當縮圖 */
		js_free(j);
		size_t used = (size_t)(nl - qbuf) + 1;
		memmove(qbuf, nl + 1, qhave - used);
		qhave -= used;
	}
}

static void qmp_connect_events(void)
{
	char sock[PATH_MAX];
	vm_run(CV, "qmp.sock", sock, sizeof sock);
	qfd = unix_connect(sock);
	if (qfd < 0) return;
	const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
	if (write(qfd, caps, strlen(caps)) < 0) { close(qfd); qfd = -1; return; }
	fcntl(qfd, F_SETFL, O_NONBLOCK);
}

static void console_dispatch(XEvent *ev);

/* pm_ask 會把模態迴圈的事件處理換成它自己的：問完換回來 */
static int ask(const char *msg, const char *const *b, int n, int def, int cancel)
{
	release_all_keys();
	int r = pm_ask(_("虛擬機器"), "pomelo-vm", icon, msg, b, n, def, cancel);
	if (cwin) { pm_dispatch = console_dispatch; paint_all(); wl_present(1); }
	return r;
}

static void show_error(const char *title, const char *msg)
{
	char full[700];
	snprintf(full, sizeof full, "%s：%s", title, msg);
	const char *ok[] = { _("好") };
	ask(full, ok, 1, 0, 0);
}

static int confirm(const char *msg, const char *yes)
{
	const char *b[] = { _("取消"), yes };
	return ask(msg, b, 2, 1, 0) == 1;
}

static void do_save_state(void)
{
	char err[512];
	if (!vm_can_snapshot(CV)) { show_error(_("無法儲存狀態"), _("硬碟不是 qcow2 格式")); return; }
	busy(_("正在儲存虛擬機器的狀態…"));
	int rc = hmp_ok(CV, "savevm " RESUME_TAG, 600000, err, sizeof err);
	busy(NULL);
	if (rc) { show_error(_("無法儲存狀態"), err); return; }
	snprintf(CV->resume, sizeof CV->resume, "%s", RESUME_TAG);
	vm_save(CV);
	qmp_simple(CV, "quit");
	quit_req = 1;
}

static void do_snapshot(void)
{
	char err[512], tag[64], cmd[96];
	if (!vm_can_snapshot(CV)) { show_error(_("無法拍快照"), _("硬碟不是 qcow2 格式")); return; }
	time_t now = time(NULL);
	snprintf(tag, sizeof tag, "s%lld", (long long)now);
	snprintf(cmd, sizeof cmd, "savevm %s", tag);
	busy(_("正在拍快照…"));
	int rc = hmp_ok(CV, cmd, 600000, err, sizeof err);
	busy(NULL);
	if (rc) { show_error(_("無法拍快照"), err); return; }
	Snap s[MAX_SNAPS];
	int n = snaps_load(CV, s, MAX_SNAPS);
	if (n >= MAX_SNAPS) n = MAX_SNAPS - 1;
	memset(&s[n], 0, sizeof s[n]);
	snprintf(s[n].tag, sizeof s[n].tag, "%s", tag);
	s[n].t = now;
	s[n].ram = 1;
	char when[64];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&now));
	snprintf(s[n].name, sizeof s[n].name, "%s %s", _("快照"), when);
	snaps_save(CV, s, n + 1);
}

static void do_restore(const Snap *s)
{
	char msg[300], err[512], cmd[96];
	snprintf(msg, sizeof msg, _("回到「%s」？現在的狀態沒拍快照的話就會不見。"), s->name);
	if (!confirm(msg, _("回到快照"))) return;
	snprintf(cmd, sizeof cmd, "loadvm %s", s->tag);
	busy(_("正在回到快照…"));
	int rc = hmp_ok(CV, cmd, 600000, err, sizeof err);
	busy(NULL);
	if (rc) show_error(_("無法回到快照"), err);
	qmp_simple(CV, "cont");
}

static void do_insert_cd(void)
{
	static const PmFileType types[] = { { N_("光碟映像（.iso）"), "iso;img" } };
	char path[PATH_MAX], dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", CV->iso);
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0; else dir[0] = 0;
	int type = 0;
	int ok = pm_file_dialog(_("選擇光碟映像"), "pomelo-vm", icon, 0, dir[0] ? dir : NULL, NULL, types, 1, &type, path, sizeof path);
	paint_all();
	if (!ok) return;
	char q[PATH_MAX + 16], cmd[PATH_MAX + 200];
	js_quote(q, sizeof q, path);
	snprintf(cmd, sizeof cmd, "{\"execute\":\"blockdev-change-medium\",\"arguments\":{\"id\":\"cd\",\"filename\":%s,\"format\":\"raw\",\"read-only-mode\":\"read-only\"}}", q);
	char *r = qmp_call(CV, cmd, 10000);
	if (r && strstr(r, "\"return\"")) {
		snprintf(CV->iso, sizeof CV->iso, "%s", path);
		vm_save(CV);
	} else {
		Js *j = js_parse(r);
		show_error(_("無法放入光碟"), js_str(js_get(js_get(j, "error"), "desc"), _("跟虛擬機器的連線中斷了")));
		js_free(j);
	}
	free(r);
}

static void do_eject_cd(void)
{
	char *r = qmp_call(CV, "{\"execute\":\"eject\",\"arguments\":{\"id\":\"cd\",\"force\":true}}", 5000);
	free(r);
	CV->iso[0] = 0;
	vm_save(CV);
}

/* ------------------------------------------------------------------ */
/* 全螢幕與工具列                                                         */
/* ------------------------------------------------------------------ */

/* 全螢幕時系統快捷鍵（Alt+Tab、Super…）要交給虛擬機器：X 用 XGrabKeyboard，Wayland 用 keyboard-shortcuts-inhibit；
 * 同時不讓螢幕變暗、鎖定（原生用 idle-inhibit） */
static void grab_keys(int on)
{
	if (cwl) { pmwl_shortcuts_inhibit(cwl, on); pmwl_idle_inhibit(cwl, on); }
	else if (on) XGrabKeyboard(dpy, cwin, True, GrabModeAsync, GrabModeAsync, CurrentTime);
	else XUngrabKeyboard(dpy, CurrentTime);
}

static void set_fullscreen(int on)
{
	if (cwl) { pmwl_fullscreen(cwl, on, -1); return; }
	XEvent e = { 0 };
	e.xclient.type = ClientMessage;
	e.xclient.window = cwin;
	e.xclient.message_type = a_netstate;
	e.xclient.format = 32;
	e.xclient.data.l[0] = on ? 1 : 0;
	e.xclient.data.l[1] = (long)a_netfull;
	e.xclient.data.l[3] = 1;
	XSendEvent(dpy, pm_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

/* 視窗管理員改了全螢幕狀態（自己要求的或使用者用快捷鍵） */
static void fullscreen_changed(int on)
{
	if (on == fullscreen) return;
	fullscreen = on;
	bar_h = on ? 0 : BAR_H;
	tb_float = 0;
	grab_keys(on);
	layout();
	paint_all();
}

static const char *tb_label(int i)
{
	switch (i) {
	case TB_POWER: return _("電源");
	case TB_PAUSE: return paused ? _("{執行}繼續") : _("暫停");
	case TB_CD: return _("光碟");
	case TB_USB: return "USB";
	case TB_SNAP: return _("快照");
	case TB_KEYS: return _("按鍵");
	case TB_VIEW: return _("檢視");
	default: return _("離開全螢幕");
	}
}

/* 小圖示（16×16，以 (x, y) 為左上角） */
static void tb_icon(cairo_t *cr, int i, double x, double y)
{
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_set_line_width(cr, 1.7);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_new_path(cr);
	switch (i) {
	case TB_POWER:
		cairo_arc(cr, 8, 9, 6, -M_PI / 2 + 0.75, 3 * M_PI / 2 - 0.75);
		cairo_stroke(cr);
		cairo_move_to(cr, 8, 1.5); cairo_line_to(cr, 8, 8); cairo_stroke(cr);
		break;
	case TB_PAUSE:
		if (paused) { cairo_move_to(cr, 4, 2); cairo_line_to(cr, 14, 8); cairo_line_to(cr, 4, 14); cairo_close_path(cr); cairo_fill(cr); }
		else { cairo_rectangle(cr, 3.5, 2.5, 3, 11); cairo_rectangle(cr, 9.5, 2.5, 3, 11); cairo_fill(cr); }
		break;
	case TB_CD:
		cairo_arc(cr, 8, 8, 6.5, 0, 2 * M_PI); cairo_stroke(cr);
		cairo_arc(cr, 8, 8, 1.8, 0, 2 * M_PI); cairo_fill(cr);
		break;
	case TB_USB:   /* USB 的三叉符號 */
		cairo_move_to(cr, 8, 15); cairo_line_to(cr, 8, 2.5); cairo_stroke(cr);
		cairo_move_to(cr, 5.5, 4.5); cairo_line_to(cr, 8, 1.5); cairo_line_to(cr, 10.5, 4.5); cairo_close_path(cr); cairo_fill(cr);
		cairo_move_to(cr, 8, 11); cairo_line_to(cr, 3.5, 8); cairo_line_to(cr, 3.5, 6.5); cairo_stroke(cr);
		cairo_arc(cr, 3.5, 5.8, 1.5, 0, 2 * M_PI); cairo_fill(cr);
		cairo_move_to(cr, 8, 9.5); cairo_line_to(cr, 12.5, 7); cairo_line_to(cr, 12.5, 5.5); cairo_stroke(cr);
		cairo_rectangle(cr, 11.2, 3.2, 2.6, 2.6); cairo_fill(cr);
		break;
	case TB_SNAP:
		pm_rrect(cr, 1.5, 4, 13, 10, 2, 2); cairo_stroke(cr);
		cairo_move_to(cr, 5.5, 4); cairo_line_to(cr, 6.5, 2); cairo_line_to(cr, 9.5, 2); cairo_line_to(cr, 10.5, 4); cairo_stroke(cr);
		cairo_arc(cr, 8, 9, 2.6, 0, 2 * M_PI); cairo_stroke(cr);
		break;
	case TB_KEYS:
		pm_rrect(cr, 1, 3.5, 14, 9.5, 2, 2); cairo_stroke(cr);
		for (int k = 0; k < 4; k++) { cairo_rectangle(cr, 3.4 + k * 2.6, 6, 1.4, 1.4); }
		cairo_fill(cr);
		cairo_move_to(cr, 5, 10.3); cairo_line_to(cr, 11, 10.3); cairo_stroke(cr);
		break;
	case TB_VIEW:
		pm_rrect(cr, 1, 2.5, 14, 10, 1.5, 1.5); cairo_stroke(cr);
		cairo_move_to(cr, 5, 15); cairo_line_to(cr, 11, 15); cairo_stroke(cr);
		break;
	default:   /* 離開全螢幕：四個往內的角 */
		cairo_move_to(cr, 5, 1.5); cairo_line_to(cr, 5, 5); cairo_line_to(cr, 1.5, 5);
		cairo_move_to(cr, 11, 1.5); cairo_line_to(cr, 11, 5); cairo_line_to(cr, 14.5, 5);
		cairo_move_to(cr, 5, 14.5); cairo_line_to(cr, 5, 11); cairo_line_to(cr, 1.5, 11);
		cairo_move_to(cr, 11, 14.5); cairo_line_to(cr, 11, 11); cairo_line_to(cr, 14.5, 11);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
}

static int tb_has_menu(int i) { return i != TB_PAUSE && i != TB_EXITFULL; }

static void tb_draw(void)
{
	if (!xsurf || (fullscreen && !tb_float)) return;
	cairo_t *cr = cairo_create(xsurf);
	int nb = fullscreen ? TB_N : TB_N - 1, dark = fullscreen || pm_dark;
	unsigned bg = dark ? 0x2c2c2e : 0xf6f6f8, fg = dark ? 0xf2f2f2 : 0x1d1d1f;
	pm_font(cr, 13, 0);
	double total = 12;
	double bw[TB_N];
	for (int i = 0; i < nb; i++) { bw[i] = 16 + 6 + pm_textw(cr, tb_label(i)) + (tb_has_menu(i) ? 14 : 0) + 20; total += bw[i] + 2; }
	int compact = total > ww - 16;   /* 視窗太窄：只畫圖示 */
	if (compact) {
		total = 12;
		for (int i = 0; i < nb; i++) { bw[i] = 16 + 20 + (tb_has_menu(i) ? 12 : 0); total += bw[i] + 2; }
	}
	double x = 8, y = 0, h = BAR_H;
	if (fullscreen) {
		tb_box = (Rect){ (int)((ww - total) / 2), 0, (int)total, BAR_H };
		pm_rrect(cr, tb_box.x, -10, tb_box.w, BAR_H + 10, 10, 10);
		pm_rgba(cr, bg, 0.94);
		cairo_fill(cr);
		x = tb_box.x + 6;
	} else {
		cairo_rectangle(cr, 0, 0, ww, BAR_H);
		pm_rgb(cr, bg);
		cairo_fill(cr);
		cairo_rectangle(cr, 0, BAR_H - 1, ww, 1);
		pm_rgba(cr, 0x000000, dark ? 0.5 : 0.12);
		cairo_fill(cr);
	}
	for (int i = 0; i < TB_N; i++) tb_r[i] = (Rect){ 0, 0, 0, 0 };
	for (int i = 0; i < nb; i++) {
		tb_r[i] = (Rect){ (int)x, (int)y + 5, (int)bw[i], (int)h - 10 };
		if (tb_hover == i) {
			pm_rrect(cr, x, y + 5, bw[i], h - 10, 7, 7);
			pm_rgba(cr, dark ? 0xffffff : 0x000000, dark ? 0.16 : 0.08);
			cairo_fill(cr);
		}
		pm_rgb(cr, i == TB_PAUSE && paused ? 0xff9f0a : fg);
		tb_icon(cr, i, x + 10, y + (h - 16) / 2);
		pm_rgb(cr, fg);
		if (!compact) pm_text(cr, x + 32, y + h / 2, tb_label(i));
		if (tb_has_menu(i)) {
			double cx = x + bw[i] - (compact ? 10 : 16), cy = y + h / 2;
			pm_rgba(cr, fg, 0.6);
			cairo_set_line_width(cr, 1.4);
			cairo_move_to(cr, cx - 3.5, cy - 1.5); cairo_line_to(cr, cx, cy + 2); cairo_line_to(cr, cx + 3.5, cy - 1.5);
			cairo_stroke(cr);
		}
		x += bw[i] + 2;
	}
	if (!fullscreen && paused) {   /* 右邊的狀態 */
		const char *s = _("已暫停");
		pm_rgb(cr, 0xff9f0a);
		double tw = pm_textw(cr, s);
		if (x + tw + 24 < ww) pm_text(cr, ww - tw - 16, h / 2, s);
	}
	cairo_destroy(cr);
}

static void tb_set_float(int on)
{
	if (tb_float == on) return;
	tb_float = on;
	tb_hover = -1;
	if (on) tb_draw();
	else paint_all();
	cairo_surface_flush(xsurf);
	FLUSH_X();
}

static int tb_hit(int x, int y)
{
	if (fullscreen && !tb_float) return -1;
	for (int i = 0; i < TB_N; i++)
		if (tb_r[i].w && x >= tb_r[i].x && x < tb_r[i].x + tb_r[i].w && y >= tb_r[i].y && y < tb_r[i].y + tb_r[i].h) return i;
	return -1;
}

/* 選單 */
static int menu_pick;
static void menu_cb(const Arg *a) { menu_pick = a->i; }

static int popup_at(int b, MenuItem *m, int n)
{
	int rx, ry;
	Window child;
	PMWL_XLATE(cwl, dpy, cwin, pm_root, tb_r[b].x, tb_r[b].y + tb_r[b].h + 2, &rx, &ry, &child);
	menu_pick = -1;
	release_all_keys();
	PMWL_POPUP(cwl, m, n, rx, ry);
	tb_hover = -1;
	paint_all();
	wl_present(1);
	return menu_pick;
}

#define MI(label, id) { label, menu_cb, { .i = id } }
#define MOFF(label) { label, pm_menu_off, { 0 } }
#define MSEP { NULL, NULL, { 0 } }

static void menu_power(void)
{
	MenuItem m[] = {
		MI(N_("關機"), 1), MI(N_("重新開機"), 2), MSEP,
		MI(N_("儲存狀態並關閉"), 3), MSEP,
		MI(N_("強制關機"), 4),
	};
	switch (popup_at(TB_POWER, m, LEN(m))) {
	case 1:
		if (qmp_simple(CV, "system_powerdown")) show_error(_("無法關機"), _("跟虛擬機器的連線中斷了"));
		break;
	case 2:
		if (confirm(_("重新開機就像按下電腦的重置按鈕，沒存的資料會不見。"), _("重新開機"))) qmp_simple(CV, "system_reset");
		break;
	case 3: do_save_state(); break;
	case 4:
		if (confirm(_("強制關機就像拔掉電源，沒存的資料會不見。"), _("強制關機"))) { save_thumb(); qmp_simple(CV, "quit"); quit_req = 1; }
		break;
	}
}

static void menu_cd(void)
{
	char cur[300];
	snprintf(cur, sizeof cur, "%s", CV->iso[0] ? base_name(CV->iso) : _("（沒有光碟）"));
	MenuItem m[] = { MOFF(cur), MSEP, MI(N_("放入光碟映像…"), 1), CV->iso[0] ? (MenuItem)MI(N_("退出光碟"), 2) : (MenuItem)MOFF(N_("退出光碟")) };
	switch (popup_at(TB_CD, m, LEN(m))) {
	case 1: do_insert_cd(); break;
	case 2: do_eject_cd(); break;
	}
}

/* ------------------------------------------------------------------ */
/* USB 穿透：把這台電腦插著的 USB 裝置（隨身碟、外接硬碟、手機、加密鎖…）交給虛擬機器 */
/* ------------------------------------------------------------------ */
/* QEMU 的 usb-host 用 libusb 直接開 /dev/bus/usb/BBB/DDD：一般使用者只能讀，要寫入權限才能用，
 * 所以交出去之前用 pkexec 把那一個裝置節點的擁有者改成自己（拔掉重插就恢復）。
 * 這台電腦掛載著的外接硬碟先退出，免得兩邊同時寫入把資料弄壞；掛著系統（/、/home…）的不給。 */

typedef struct { char sys[32], name[160]; int bus, addr, hid, storage, attached; } UsbDev;

static int sys_str(const char *dev, const char *f, char *out, size_t n)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/%s", dev, f);
	FILE *fp = fopen(p, "r");
	if (!fp) { out[0] = 0; return -1; }
	if (!fgets(out, (int)n, fp)) out[0] = 0;
	fclose(fp);
	out[strcspn(out, "\n")] = 0;
	return 0;
}

static int usb_scan(UsbDev *d, int max)
{
	char *attached = hmp_call(CV, "info usb", 2000);   /* 已經交給虛擬機器的：ID: usbhost-匯流排-位址 */
	DIR *dir = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	int n = 0;
	while (dir && (e = readdir(dir)) && n < max) {
		const char *nm = e->d_name;
		if (nm[0] == '.' || strchr(nm, ':') || !strncmp(nm, "usb", 3) || strlen(nm) >= sizeof d->sys) continue;
		char b[160], cls[8];
		UsbDev *u = &d[n];
		memset(u, 0, sizeof *u);
		snprintf(u->sys, sizeof u->sys, "%s", nm);
		if (sys_str(nm, "busnum", b, sizeof b)) continue;
		u->bus = atoi(b);
		sys_str(nm, "devnum", b, sizeof b);
		u->addr = atoi(b);
		sys_str(nm, "bDeviceClass", cls, sizeof cls);
		if (!strcmp(cls, "09")) continue;   /* USB 集線器 */
		/* 各介面的類別：03 = 鍵盤滑鼠、08 = 儲存裝置 */
		char ifp[PATH_MAX];
		snprintf(ifp, sizeof ifp, "/sys/bus/usb/devices/%s:", nm);
		DIR *d2 = opendir("/sys/bus/usb/devices");
		struct dirent *e2;
		while (d2 && (e2 = readdir(d2))) {
			if (strncmp(e2->d_name, ifp + strlen("/sys/bus/usb/devices/"), strlen(nm) + 1)) continue;
			char c[8];
			sys_str(e2->d_name, "bInterfaceClass", c, sizeof c);
			if (!strcmp(c, "03")) u->hid = 1;
			if (!strcmp(c, "08")) u->storage = 1;
		}
		if (d2) closedir(d2);
		char man[80], prod[80], vid[8], pid[8];
		sys_str(nm, "manufacturer", man, sizeof man);
		sys_str(nm, "product", prod, sizeof prod);
		sys_str(nm, "idVendor", vid, sizeof vid);
		sys_str(nm, "idProduct", pid, sizeof pid);
		if (prod[0] && man[0] && !strstr(prod, man)) snprintf(u->name, sizeof u->name, "%s %s", man, prod);
		else if (prod[0]) snprintf(u->name, sizeof u->name, "%s", prod);
		else snprintf(u->name, sizeof u->name, "%s %s:%s", _("USB 裝置"), vid, pid);
		char id[48];
		snprintf(id, sizeof id, "usbhost-%d-%d", u->bus, u->addr);
		if (attached) {
			char *p = attached;
			size_t l = strlen(id);
			while ((p = strstr(p, id))) { if (!isdigit((unsigned char)p[l])) { u->attached = 1; break; } p += l; }
		}
		n++;
	}
	if (dir) closedir(dir);
	free(attached);
	return n;
}

/* 這個 USB 裝置上掛載著的分割區退出；掛著系統的（/、/home…）回傳 -1 */
static int usb_unmount(const UsbDev *u, char *err, size_t en)
{
	char key[48], key2[48];
	snprintf(key, sizeof key, "/%s/", u->sys);
	snprintf(key2, sizeof key2, "/%s:", u->sys);
	DIR *d = opendir("/sys/block");
	struct dirent *e;
	int rc = 0;
	while (d && (e = readdir(d)) && !rc) {
		if (e->d_name[0] == '.') continue;
		char p[PATH_MAX], real[PATH_MAX];
		snprintf(p, sizeof p, "/sys/block/%s", e->d_name);
		if (!realpath(p, real) || (!strstr(real, key) && !strstr(real, key2))) continue;
		/* 這顆碟（sdX）的分割區有沒有掛載 */
		FILE *m = fopen("/proc/self/mounts", "r");
		char line[1024], dev[PATH_MAX], mnt[PATH_MAX], disk[64];
		snprintf(disk, sizeof disk, "/dev/%s", e->d_name);
		char todo[16][PATH_MAX];
		int nt = 0;
		while (m && fgets(line, sizeof line, m) && nt < 16) {
			if (sscanf(line, "%4095s %4095s", dev, mnt) != 2 || strncmp(dev, disk, strlen(disk))) continue;
			if (!strcmp(mnt, "/") || !strncmp(mnt, "/boot", 5) || !strcmp(mnt, "/home") || !strcmp(mnt, "/usr") || !strcmp(mnt, "/var")) {
				snprintf(err, en, _("%s 上面是這台電腦正在用的系統，不能交給虛擬機器"), disk);
				rc = -1;
				break;
			}
			snprintf(todo[nt++], PATH_MAX, "%s", dev);
		}
		if (m) fclose(m);
		for (int i = 0; i < nt && !rc; i++) {
			char out[512];
			char *argv[] = { "udisksctl", "unmount", "-b", todo[i], NULL };
			if (run_argv(argv, out, sizeof out)) {
				last_line(out);
				snprintf(err, en, "%s %s：%s", _("無法退出"), todo[i], out[0] ? out : _("可能有程式正在用它"));
				rc = -1;
			}
		}
	}
	if (d) closedir(d);
	return rc;
}

static void usb_attach(const UsbDev *u)
{
	char msg[400], err[600], node[64];
	if (u->hid) {
		snprintf(msg, sizeof msg, _("「%s」是鍵盤或滑鼠，交給虛擬機器後這台電腦就不能用它了。"), u->name);
		if (!confirm(msg, _("交給虛擬機器"))) return;
	}
	if (u->storage) {
		busy(_("正在退出這台電腦上的磁碟…"));
		int rc = usb_unmount(u, err, sizeof err);
		busy(NULL);
		if (rc) { show_error(_("無法交給虛擬機器"), err); return; }
	}
	snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", u->bus, u->addr);
	if (access(node, R_OK | W_OK)) {
		char uid[16], out[512];
		snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
		char *argv[] = { "pkexec", "chown", uid, node, NULL };
		busy(_("等待授權…"));
		run_argv(argv, out, sizeof out);
		busy(NULL);
		if (access(node, R_OK | W_OK)) { show_error(_("無法交給虛擬機器"), _("沒有取得這個 USB 裝置的使用權限")); return; }
	}
	char cmd[300];
	snprintf(cmd, sizeof cmd, "{\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-host\",\"bus\":\"xhci.0\","
	         "\"hostbus\":%d,\"hostaddr\":%d,\"id\":\"usbhost-%d-%d\"}}", u->bus, u->addr, u->bus, u->addr);
	char *r = qmp_call(CV, cmd, 10000);
	if (!r || !strstr(r, "\"return\"")) {
		Js *j = js_parse(r);
		show_error(_("無法交給虛擬機器"), js_str(js_get(js_get(j, "error"), "desc"), _("跟虛擬機器的連線中斷了")));
		js_free(j);
	}
	free(r);
}

static void usb_detach(const UsbDev *u)
{
	char cmd[160];
	snprintf(cmd, sizeof cmd, "{\"execute\":\"device_del\",\"arguments\":{\"id\":\"usbhost-%d-%d\"}}", u->bus, u->addr);
	free(qmp_call(CV, cmd, 5000));
}

static void menu_usb(void)
{
	UsbDev d[24];
	int n = qemu_has_device("usb-host") ? usb_scan(d, LEN(d)) : -1;
	MenuItem m[LEN(d) + 2];
	char labels[LEN(d)][200];
	int k = 0;
	if (n < 0) m[k++] = (MenuItem)MOFF(N_("QEMU 沒有 USB 穿透的模組"));
	else if (!n) m[k++] = (MenuItem)MOFF(N_("沒有插著 USB 裝置"));
	for (int i = 0; i < n; i++) {
		snprintf(labels[i], sizeof labels[i], "%s%s%s", d[i].attached ? "\1" : "\2", d[i].name,
		         d[i].storage ? _("（儲存裝置）") : d[i].hid ? _("（鍵盤／滑鼠）") : "");
		m[k++] = (MenuItem)MI(labels[i], i);
	}
	int r = popup_at(TB_USB, m, k);
	if (r < 0 || r >= n) return;
	if (d[r].attached) usb_detach(&d[r]);
	else usb_attach(&d[r]);
}

static void spawn_self(const char *args);

static void menu_snap(void)
{
	Snap s[MAX_SNAPS];
	int n = snaps_load(CV, s, MAX_SNAPS);
	MenuItem m[16];
	char labels[8][200];
	int k = 0;
	m[k++] = (MenuItem)MI(N_("拍快照"), 1000);
	int shown = 0;
	for (int i = n - 1; i >= 0 && shown < 8; i--, shown++) {
		if (!shown) m[k++] = (MenuItem)MSEP;
		snprintf(labels[shown], sizeof labels[0], _("回到「%s」"), s[i].name);
		m[k++] = s[i].ram ? (MenuItem)MI(labels[shown], i) : (MenuItem)MOFF(labels[shown]);
	}
	m[k++] = (MenuItem)MSEP;
	m[k++] = (MenuItem)MI(N_("管理快照…"), 1001);
	int r = popup_at(TB_SNAP, m, k);
	if (r == 1000) do_snapshot();
	else if (r == 1001) {
		char a[64];
		snprintf(a, sizeof a, "--show %s", CV->id);
		spawn_self(a);
	} else if (r >= 0 && r < n) do_restore(&s[r]);
}

static void menu_keys(void)
{
	MenuItem m[] = {
		MI("Ctrl+Alt+Del\tCtrl+Alt+End", 1), MI("Ctrl+Alt+Backspace", 2), MSEP,
		MI("Ctrl+Alt+F1", 3), MI("Ctrl+Alt+F2", 4), MI("Ctrl+Alt+F3", 5), MI("Ctrl+Alt+F7", 6), MSEP,
		MI(N_("Windows 鍵／Super 鍵"), 7), MI("Print Screen", 8),
	};
	int r = popup_at(TB_KEYS, m, LEN(m));
	unsigned cad[] = { 0x1d, 0x38, 0xd3 }, cab[] = { 0x1d, 0x38, 0x0e }, caf[] = { 0x1d, 0x38, 0 };
	switch (r) {
	case 1: send_combo(cad, 3); break;
	case 2: send_combo(cab, 3); break;
	case 3: case 4: case 5: case 6:
		caf[2] = r == 6 ? 0x41 : (unsigned)(0x3b + r - 3);
		send_combo(caf, 3);
		break;
	case 7: tap(0xdb, 1); tap(0xdb, 0); break;
	case 8: tap(0xb7, 1); tap(0xb7, 0); break;
	}
}

static void menu_view(void)
{
	int can_follow = (ext_desktop || qemu_major >= 7) && CV->display == DISP_VIRTIO;
	char follow[200];
	snprintf(follow, sizeof follow, "%s%s", follow_size ? "\1" : "\2", _("解析度跟著視窗"));
	MenuItem m[] = {
		MI(N_("全螢幕\tCtrl+Alt+Enter"), 1),
		MI(N_("視窗符合畫面大小"), 2),
		can_follow ? (MenuItem)MI(follow, 3) : (MenuItem)MOFF(N_("解析度跟著視窗（要 VirtIO 顯示卡）")),
	};
	switch (popup_at(TB_VIEW, m, LEN(m))) {
	case 1: set_fullscreen(!fullscreen); break;
	case 2: user_sized = 0; fit_window(); break;
	case 3: follow_size = !follow_size; if (follow_size) size_due = pm_now_ms() + 100; break;
	}
}

static void tb_action(int i)
{
	switch (i) {
	case TB_POWER: menu_power(); break;
	case TB_PAUSE: qmp_simple(CV, paused ? "cont" : "stop"); break;
	case TB_CD: menu_cd(); break;
	case TB_USB: menu_usb(); break;
	case TB_SNAP: menu_snap(); break;
	case TB_KEYS: menu_keys(); break;
	case TB_VIEW: menu_view(); break;
	case TB_EXITFULL: set_fullscreen(0); break;
	}
}

/* 解析度跟著視窗：視窗大小穩定下來後，請虛擬機器改成畫面區的大小（要虛擬機器的顯示卡驅動支援） */
static void send_desktop_size(void)
{
	size_due = 0;
	if (!follow_size || !(ext_desktop || qemu_major >= 7) || CV->display != DISP_VIRTIO || vfd < 0) return;
	if (!fullscreen && !user_sized && !too_big) return;
	int w = ww & ~7, h = (wh - bar_h) & ~1;
	if (w < 320 || h < 200 || (w == fbw && h == fbh) || (w == sent_w && h == sent_h)) return;
	unsigned char m[24] = { 251, 0 };
	wr16(m + 2, (unsigned)w);
	wr16(m + 4, (unsigned)h);
	m[6] = 1;
	wr32(m + 8, 0);
	wr16(m + 12, 0); wr16(m + 14, 0);
	wr16(m + 16, (unsigned)w); wr16(m + 18, (unsigned)h);
	wr32(m + 20, 0);
	vwrite(m, sizeof m);
	sent_w = w; sent_h = h;
	if (getenv("POMELO_VM_DEBUG")) fprintf(stderr, "pomelo-vm: SetDesktopSize %dx%d\n", w, h);
}

/* ------------------------------------------------------------------ */
/* 縮圖（管理視窗的清單用）                                                */
/* ------------------------------------------------------------------ */

static void save_thumb(void)
{
	if (!fb || fbw < 64 || fbh < 48 || no_thumb) return;
	fb_dirty_thumb = 0;
	/* 全黑（關機中、螢幕保護）就不存，留著上一張 */
	cairo_surface_flush(fb);
	const unsigned char *d = cairo_image_surface_get_data(fb);
	int st = cairo_image_surface_get_stride(fb), lit = 0;
	for (int y = 0; y < fbh; y += fbh / 16)
		for (int x = 0; x < fbw; x += fbw / 16) {
			const unsigned char *p = d + (size_t)y * st + (size_t)x * 4;
			if (p[0] + p[1] + p[2] > 60) lit++;
		}
	if (lit < 20) return;   /* 256 個取樣點不到 8% 有亮：大概是黑畫面或關機訊息 */
	int tw = MIN(480, fbw), th = fbh * tw / fbw;
	cairo_surface_t *t = cairo_image_surface_create(CAIRO_FORMAT_RGB24, tw, th);
	cairo_t *cr = cairo_create(t);
	cairo_scale(cr, (double)tw / fbw, (double)th / fbh);
	cairo_set_source_surface(cr, fb, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	vm_file(CV, "screen.png", path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (cairo_surface_write_to_png(t, tmp) == CAIRO_STATUS_SUCCESS) rename(tmp, path);
	else unlink(tmp);
	cairo_surface_destroy(t);
}

/* ------------------------------------------------------------------ */
/* X 事件                                                                */
/* ------------------------------------------------------------------ */

static void close_window(void)
{
	if (vm_gone || !vm_pid(CV)) { quit_req = 1; return; }
	const char *b[] = { _("關機"), _("取消"), _("在背景繼續執行") };
	char msg[300];
	snprintf(msg, sizeof msg, _("「%s」還在執行，關掉視窗後要怎麼做？"), CV->name);
	int r = ask(msg, b, 3, 2, 1);
	if (r == 0) { save_thumb(); qmp_simple(CV, "system_powerdown"); quit_req = 1; }
	else if (r == 2) { save_thumb(); quit_req = 1; }
}

static void on_key(XKeyEvent *e, int down)
{
	KeySym ks, ks0;
	if (pmwl_xkey(e, NULL, 0, &ks, &ks0) < 0) ks = XLookupKeysym(e, 0); else ks = ks0;
	last_mods = e->state;
	unsigned mods = e->state & (ControlMask | Mod1Mask);
	if (down && mods == (ControlMask | Mod1Mask)) {   /* 本機的快捷鍵 */
		if (ks == XK_Return || ks == XK_KP_Enter) { release_all_keys(); set_fullscreen(!fullscreen); return; }
		if (ks == XK_End || ks == XK_KP_End) { unsigned cad[] = { 0x1d, 0x38, 0xd3 }; release_all_keys(); send_combo(cad, 3); return; }
		if (ks == XK_Home || ks == XK_KP_Home) { if (fullscreen) { release_all_keys(); tb_set_float(1); tb_hide_at = pm_now_ms() + 3000; } return; }
	}
	unsigned kc = e->keycode & 0xff;
	if (!down && !pressed[kc]) return;   /* 按下時不在這個視窗（例如剛關掉選單）的鍵：放開不用送 */
	pressed[kc] = (unsigned char)down;
	send_key(keymap[kc], ks, down);
}

static void on_button(XButtonEvent *e, int down)
{
	last_mods = e->state;
	int hit = tb_hit(e->x, e->y);
	if (hit >= 0 || (!in_screen(e->x, e->y) && !(btn_mask && !down))) {
		if (down && hit >= 0) tb_action(hit);
		return;
	}
	static const unsigned bits[] = { 0, 1, 2, 4, 8, 16, 32, 64 };
	if (e->button < 1 || e->button > 7) return;
	unsigned b = bits[e->button];
	if (pending_motion) { pending_motion = 0; send_pointer(mx_, my_); }
	if (e->button >= 4) {   /* 滾輪：按下＋放開 */
		if (!down) return;
		btn_mask |= b; send_pointer(e->x, e->y);
		btn_mask &= ~b; send_pointer(e->x, e->y);
		return;
	}
	if (down) btn_mask |= b; else btn_mask &= ~b;
	send_pointer(e->x, e->y);
}

static void on_motion(XMotionEvent *e)
{
	long now = pm_now_ms();
	if (fullscreen) {
		if (e->y <= 1 && !tb_float) { if (!tb_show_at) tb_show_at = now + 350; }
		else tb_show_at = 0;
		if (tb_float) {
			if (e->y < BAR_H + 10 && e->x >= tb_box.x - 10 && e->x < tb_box.x + tb_box.w + 10) tb_hide_at = 0;
			else if (!tb_hide_at) tb_hide_at = now + 1200;
		}
	}
	int h = tb_hit(e->x, e->y);
	if (h != tb_hover) { tb_hover = h; tb_draw(); cairo_surface_flush(xsurf); wl_dirty |= cwl != NULL; }
	if (!in_screen(e->x, e->y) && !btn_mask) return;
	pending_motion = 1;
	mx_ = e->x; my_ = e->y;
}

static void handle_x(XEvent *ev)
{
	switch (ev->type) {
	case Expose:
		if (cwl) { wl_dirty = 1; break; }   /* xsurf 一直保著內容 */
		if (!ev->xexpose.count) paint_all();
		break;
	case ConfigureNotify:
		if (ev->xconfigure.width != ww || ev->xconfigure.height != wh) {
			ww = ev->xconfigure.width; wh = ev->xconfigure.height;
			if (cwl) xsurf_resize(); else cairo_xlib_surface_set_size(xsurf, ww, wh);
			if (!fullscreen && (ww != want_w || wh != want_h) && want_w) user_sized = 1;
			layout();
			paint_all();
			size_due = pm_now_ms() + 500;
		}
		break;
	case KeyPress: on_key(&ev->xkey, 1); break;
	case KeyRelease: on_key(&ev->xkey, 0); break;
	case ButtonPress: on_button(&ev->xbutton, 1); break;
	case ButtonRelease: on_button(&ev->xbutton, 0); break;
	case MotionNotify: on_motion(&ev->xmotion); break;
	case LeaveNotify:
		tb_show_at = 0;
		if (tb_hover >= 0) { tb_hover = -1; tb_draw(); cairo_surface_flush(xsurf); wl_dirty |= cwl != NULL; }
		break;
	case FocusIn: sync_locks(); break;
	case FocusOut:
		release_all_keys();
		if (btn_mask) { btn_mask = 0; send_pointer(mx_, my_); }
		break;
	case ClientMessage:
		if ((Atom)ev->xclient.data.l[0] == a_del) close_window();
		break;
	case MappingNotify:
		if (cwl) break;
		XRefreshKeyboardMapping(&ev->xmapping);
		keymap_init();
		break;
	case PropertyNotify:
		if (ev->xproperty.window == cwin && ev->xproperty.atom == a_netstate) {
			Atom st[16];
			int n = pm_getatoms(cwin, a_netstate, st, 16), on = 0;
			for (int i = 0; i < n; i++) if (st[i] == a_netfull) on = 1;
			fullscreen_changed(on);
		}
		break;
	}
}

static void console_dispatch(XEvent *ev) { if (ev->type == Expose && ev->xany.window == cwin) paint_all(); }

/* 原生 Wayland 的事件：大部分轉成 X 事件交給 handle_x；全螢幕狀態、剪貼簿自己處理 */
static void send_clip_to_guest(void)
{
	if (vfd < 0 || pmwl_clip_owned()) return;
	char *t = pmwl_clip_get();
	if (!t) return;
	/* 傳統的 ClientCutText 只有 Latin-1：非 Latin-1 的字換成 ? */
	size_t n = strlen(t), k = 0;
	unsigned char *o = malloc(n + 9);
	if (!o) { free(t); return; }
	for (size_t i = 0; i < n && k < (1 << 20);) {
		unsigned c = (unsigned char)t[i];
		int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
		unsigned cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
		for (int j = 1; j < len && i + (size_t)j < n; j++) cp = (cp << 6) | ((unsigned char)t[i + (size_t)j] & 0x3f);
		i += (size_t)len;
		o[8 + k++] = cp < 0x100 ? (unsigned char)cp : '?';
	}
	o[0] = 6; o[1] = o[2] = o[3] = 0;
	wr32(o + 4, (unsigned)k);
	vwrite(o, 8 + k);
	free(o);
	free(t);
}

static void handle_pe(const PmwEvent *pe)
{
	XEvent ev;
	switch (pe->type) {
	case PMW_CLIP: send_clip_to_guest(); return;   /* 本機剪貼簿換了（視窗拿到焦點時也會來一次）：傳給虛擬機器 */
	case PMW_SIZE: {
		int fs = pmwl_is_fullscreen(cwl);
		if (fs != fullscreen) {   /* 自己要求的、或使用者用視窗的按鈕／快捷鍵換了：先把新大小記好，再換全螢幕狀態（版面會用到） */
			ww = pe->w; wh = pe->h;
			xsurf_resize();
			fullscreen_changed(fs);
		}
		break;
	}
	case PMW_MOTION: last_mods = pe->state; break;
	case PMW_DOWN: case PMW_KEY:
		if (cut_retry && cut_last && pmwl_clip_owned()) { pmwl_clip_set(cut_last); cut_retry = 0; }   /* 這時序號是新的 */
		break;
	}
	if (pmwl_to_xevent(pe, &ev)) handle_x(&ev);
}

/* ------------------------------------------------------------------ */
/* 視窗與主迴圈                                                           */
/* ------------------------------------------------------------------ */

static void xsurf_resize(void)
{
	if (!cwl) return;
	if (xsurf) cairo_surface_destroy(xsurf);
	xsurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, MAX(ww, 1), MAX(wh, 1));
}

/* 原生：xsurf（所有繪圖都畫在這裡）整張送給 compositor；上一張還沒顯示出去（frame callback 沒回來）就先等，不堆積 */
static void wl_present(int force)
{
	if (!cwl || !wl_dirty || !xsurf) return;
	if (!force && pmwl_frame_wait(cwl)) return;
	int w, h;
	cairo_t *cr = pmwl_begin(cwl, &w, &h);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_source_surface(cr, xsurf, 0, 0);
	cairo_paint(cr);
	pmwl_frame_request(cwl);
	pmwl_end(cwl, cr);
	wl_dirty = 0;
}

static void create_window_wl(void)
{
	a_del = PMWL_XCLOSE;
	Rect mon = { 0, 0, 1280, 720 };
	Rect o[8];
	if (pmwl_outputs(o, 8) > 0) mon = o[0];
	ww = MIN(MAX(fbw, 640), mon.w * 92 / 100);
	wh = MIN(MAX(fbh, 400) + BAR_H, mon.h * 88 / 100);
	want_w = ww; want_h = wh;
	cwl = pmwl_window("", "pomelo-vm", ww, wh, 480, 300, NULL);
	if (!cwl) { fprintf(stderr, "pomelo-vm: %s\n", _("無法開啟 Wayland 視窗")); exit(1); }
	cwin = PMWL_XID(cwl);
	pmwl_want(cwl, PMWL_IN_KEYUP);
	pmwl_size(cwl, &ww, &wh);
	xsurf_resize();
	set_title();
	dot_cursor = make_dot_cursor();
	wl_dirty = 1;
}

static void create_window(void)
{
	if (!dpy) { create_window_wl(); return; }
	a_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	a_netstate = XInternAtom(dpy, "_NET_WM_STATE", False);
	a_netfull = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	a_netname = XInternAtom(dpy, "_NET_WM_NAME", False);
	a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
	a_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	pm_update_monitors();
	int m = pm_pointer_mon();
	Rect mon = mons[m < 0 ? 0 : m];
	if (mon.w < 200 || mon.h < 200) { mon = (Rect){ 0, 0, sw, sh }; }
	ww = MIN(MAX(fbw, 640), mon.w * 92 / 100);
	wh = MIN(MAX(fbh, 400) + BAR_H, mon.h * 88 / 100);
	want_w = ww; want_h = wh;
	XSetWindowAttributes wa = { .background_pixel = 0, .bit_gravity = NorthWestGravity,
	                            .event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
	                                          PointerMotionMask | LeaveWindowMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask };
	cwin = XCreateWindow(dpy, pm_root, mon.x + (mon.w - ww) / 2, mon.y + (mon.h - wh) / 2, (unsigned)ww, (unsigned)wh, 0,
	                     CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWBitGravity | CWEventMask, &wa);
	XClassHint ch = { "pomelo-vm", "pomelo-vm" };
	XSizeHints *sz = XAllocSizeHints();
	sz->flags = PMinSize;
	sz->min_width = 480; sz->min_height = 300;
	XWMHints wm = { .flags = InputHint, .input = True };
	XSetWMProperties(dpy, cwin, NULL, NULL, NULL, 0, sz, &wm, &ch);
	XFree(sz);
	XSetWMProtocols(dpy, cwin, &a_del, 1);
	long pid = getpid();
	XChangeProperty(dpy, cwin, XInternAtom(dpy, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);
	cairo_surface_t *is = icon(64);
	cairo_surface_flush(is);
	unsigned long *d = malloc((2 + 64 * 64) * sizeof(unsigned long));
	d[0] = 64; d[1] = 64;
	unsigned *px = (unsigned *)cairo_image_surface_get_data(is);
	for (int i = 0; i < 64 * 64; i++) d[2 + i] = px[i];
	XChangeProperty(dpy, cwin, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)d, 2 + 64 * 64);
	free(d);
	cairo_surface_destroy(is);
	set_title();
	dot_cursor = make_dot_cursor();
	XDefineCursor(dpy, cwin, dot_cursor);
	xsurf = cairo_xlib_surface_create(dpy, cwin, DefaultVisual(dpy, scr), ww, wh);
	XMapWindow(dpy, cwin);
	XFlush(dpy);
}

/* 同一台虛擬機器只開一個畫面視窗：已經開了就把那個叫到前面 */
static int console_single(void)
{
	char path[PATH_MAX];
	vm_run(CV, "console.lock", path, sizeof path);
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) return 0;
	if (!flock(fd, LOCK_EX | LOCK_NB)) return 0;   /* 拿到鎖：我是第一個（fd 留著直到結束） */
	char buf[32] = { 0 };
	ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
	Window w = n > 0 ? (Window)strtoul(buf, NULL, 10) : None;
	if (!dpy) {   /* 原生：用視窗清單找標題以這台名字開頭的視窗，叫到前面 */
		if (pmwl_toplevels_start()) {
			PmwEvent pe;
			for (int i = 0; i < 10; i++) pmwl_next(&pe, 50);
			PmwToplevel t[64];
			int nt = pmwl_toplevels(t, 64);
			char pre[200];
			snprintf(pre, sizeof pre, "%s", CV->name);
			for (int i = 0; i < nt; i++)
				if (!strcmp(t[i].app_id, "pomelo-vm") && !strncmp(t[i].title, pre, strlen(pre))) { pmwl_toplevel_do(t[i].id, PMWL_TL_ACTIVATE); break; }
		}
		close(fd);
		return 1;
	}
	if (w) {
		XEvent e = { 0 };
		e.xclient.type = ClientMessage;
		e.xclient.window = w;
		e.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
		e.xclient.format = 32;
		e.xclient.data.l[0] = 2;
		XSendEvent(dpy, pm_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XMapRaised(dpy, w);
		XFlush(dpy);
	}
	close(fd);
	return 1;
}

static void console_publish(void)
{
	char path[PATH_MAX], buf[32];
	vm_run(CV, "console.lock", path, sizeof path);
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return;
	int n = snprintf(buf, sizeof buf, "%lu\n", cwl ? 0UL : (unsigned long)cwin);
	if (ftruncate(fd, 0) == 0 && pwrite(fd, buf, (size_t)n, 0) < 0) { /* 只是叫回視窗用，失敗沒關係 */ }
	close(fd);
}

static int console_main(Vm *v)
{
	CV = v;
	/* Wayland 工作階段（POMELO_BACKEND=x11 可強制走 X）：原生視窗，不開 X 連線 */
	int native = pmwl_connect();
	if (native) pm_ui_native = 1;
	else if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "pomelo-vm: %s\n", _("無法開啟 X display")); return 1; }
	pm_conf_load();
	pm_init(pm_conf_get("appearance.font", "Noto Sans CJK TC"), pm_conf_num("appearance.font_size", 13),
	        pm_conf_int("appearance.dark", 0), pm_conf_get("appearance.icon_theme", "Yaru"));
	pm_set_theme(pm_conf_int("appearance.dark", 0), pm_conf_color("appearance.accent", 0));
	signal(SIGPIPE, SIG_IGN);
	if (console_single()) return 0;
	if (!vm_pid(v)) {
		const char *ok[] = { _("好") };
		char msg[300];
		snprintf(msg, sizeof msg, _("「%s」沒有在執行"), v->name);
		pm_ask(_("虛擬機器"), "pomelo-vm", icon, msg, ok, 1, 0, 0);
		return 1;
	}
	char ap[PATH_MAX], am[16] = "";
	vm_run(v, "audio", ap, sizeof ap);
	FILE *f = fopen(ap, "r");
	if (f) { if (!fgets(am, sizeof am, f)) am[0] = 0; fclose(f); }
	au_vnc = !strncmp(am, "vnc", 3);
	if (vnc_connect()) {
		const char *ok[] = { _("好") };
		pm_ask(_("虛擬機器"), "pomelo-vm", icon, _("連不上虛擬機器的畫面"), ok, 1, 0, 0);
		return 1;
	}
	qmp_connect_events();
	char *ver = qmp_call(v, "{\"execute\":\"query-version\"}", 3000);
	Js *vj = js_parse(ver);
	qemu_major = (int)js_num(js_get(js_get(js_get(vj, "return"), "qemu"), "major"), 0);
	js_free(vj);
	free(ver);
	vm_refresh(v);
	paused = v->state == ST_PAUSED;
	create_window();
	if (cwl) paint_all();   /* 原生：xsurf 一開始是空的，先把工具列與畫面畫上去 */
	console_publish();
	keymap_init();
	layout();
	fit_window();   /* 畫面比螢幕大時，請虛擬機器改用放得下的解析度 */
	pm_dispatch = console_dispatch;
	thumb_due = pm_now_ms() + 5000;

	int xfd = dpy ? ConnectionNumber(dpy) : pmwl_fd();
	while (!quit_req) {
		while (dpy && XPending(dpy) && !quit_req) {
			XEvent ev;
			XNextEvent(dpy, &ev);
			handle_x(&ev);
		}
		if (cwl) {
			PmwEvent pe;
			while (!quit_req && pmwl_next(&pe, 0)) handle_pe(&pe);
			if (cut_pending) {   /* 虛擬機器傳來的剪貼簿 */
				free(cut_last); cut_last = cut_pending; cut_pending = NULL;
				pmwl_clip_set(cut_last);
				cut_retry = 1;
			}
		}
		if (quit_req) break;
		if (pending_motion) { pending_motion = 0; send_pointer(mx_, my_); }
		long now = pm_now_ms();
		if (size_due && now >= size_due) send_desktop_size();
		if (tb_show_at && now >= tb_show_at) { tb_show_at = 0; tb_set_float(1); }
		if (tb_hide_at && now >= tb_hide_at) { tb_hide_at = 0; tb_set_float(0); }
		if (thumb_due && now >= thumb_due) { thumb_due = now + 20000; if (fb_dirty_thumb && !paused) save_thumb(); }
		/* 很久沒收到畫面更新（虛擬機器暫停時 QEMU 不回應）：再要一次，免得卡住 */
		if (vfd >= 0 && now - last_req_ms > 3000) request_update(1);
		if (dpy) { XFlush(dpy); if (XPending(dpy)) continue; }
		else wl_present(0);
		long wake = 1000;
		if (cwl && wl_dirty) wake = 16;   /* 等 frame callback（逾時了就直接送） */
		long cands[] = { size_due, tb_show_at, tb_hide_at, thumb_due };
		for (size_t i = 0; i < LEN(cands); i++) if (cands[i]) wake = MIN(wake, MAX(0, cands[i] - now));
		struct pollfd pf[3] = { { xfd, POLLIN, 0 }, { vfd, POLLIN, 0 }, { qfd, POLLIN, 0 } };
		if (poll(pf, 3, (int)wake) < 0 && errno != EINTR) break;
		if (vfd >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			/* 一次最多處理 30 毫秒的訊息，別讓鍵盤滑鼠等太久 */
			long until = pm_now_ms() + 30;
			int r;
			while ((r = read_message()) == 0 && pm_now_ms() < until) {}
			if (r < 0) {
				close(vfd);
				vfd = -1;
				if (!vm_pid(v)) break;   /* 關機了：視窗跟著關 */
				usleep(300000);
				if (vnc_connect()) break;
			}
		}
		if (qfd >= 0 && (pf[2].revents & (POLLIN | POLLHUP | POLLERR))) qmp_events();
	}
	if (vfd >= 0) close(vfd);
	if (qfd >= 0) close(qfd);
	return 0;
}
