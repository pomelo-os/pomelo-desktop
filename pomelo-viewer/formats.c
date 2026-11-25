/* SPDX-License-Identifier: GPL-3.0-or-later */
/* formats.c — 看圖：各種圖片格式的解碼與 EXIF 資訊
 *
 * PNG、JPEG 用 libpomelo（JPEG 會依 EXIF 轉正）；GIF 用 giflib（動畫）、WebP 用 libwebp（動畫）；
 * BMP、PNM 自己解。HEIC、AVIF 用 libheif，TIFF 用 libtiff，都不列為相依：執行時才載入（dlopen），
 * 沒裝的話畫面上有「安裝」按鈕。SVG 用內嵌的 nanosvg（libpomelo/third-party/，zlib 授權）畫成點陣圖
 * （不用 librsvg：它會帶進 pango、gdk-pixbuf 等 46 個套件；nanosvg 不支援文字與濾鏡）。
 * PSD 認得出來但還不支援，會回傳格式名稱讓畫面說明。
 * 這個檔案只在背景執行緒用，不能呼叫 Xlib 或 pm_tr。
 */
#include <dlfcn.h>
#include <pthread.h>
#include <gif_lib.h>
#include <webp/decode.h>
#include <webp/demux.h>
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../libpomelo/third-party/nanosvg.h"
#include "../libpomelo/third-party/nanosvgrast.h"
#pragma GCC diagnostic pop

#define MAX_DIM 16384
#define MAX_PIXELS 40000000ULL
#define MAX_ANIM_BYTES (600ULL * 1024 * 1024)   /* 動畫所有畫格加起來最多這麼大，超過就只放前面的 */

enum { NEED_NONE, NEED_LIBHEIF, NEED_HEVC, NEED_LIBTIFF };

typedef struct {
	cairo_surface_t **frames;
	int *delays;              /* 毫秒 */
	int n;
	int w, h;                 /* 畫格大小（解碼後） */
	int ow, oh;               /* 原始大小（太大的 JPEG 解碼時會縮小，縮放比例要照原始大小算） */
	char fmt[16];             /* 「JPEG」「PNG」…；err = 2 時是不支援的格式名稱 */
	int err;                  /* 0 = 好、1 = 打不開或壞掉、2 = 不支援的格式 */
	int need;                 /* err = 2 時，裝了什麼就能看：NEED_LIBHEIF、NEED_HEVC、NEED_LIBTIFF */
	size_t bytes;
} Image;

static void image_free(Image *im)
{
	if (!im) return;
	for (int i = 0; i < im->n; i++) cairo_surface_destroy(im->frames[i]);
	free(im->frames);
	free(im->delays);
	free(im);
}

static int image_add(Image *im, cairo_surface_t *s, int delay)
{
	cairo_surface_t **f = realloc(im->frames, (size_t)(im->n + 1) * sizeof *f);
	if (!f) return 0;
	im->frames = f;
	int *d = realloc(im->delays, (size_t)(im->n + 1) * sizeof *d);
	if (!d) return 0;
	im->delays = d;
	im->frames[im->n] = s;
	im->delays[im->n] = delay;
	im->n++;
	im->w = cairo_image_surface_get_width(s);
	im->h = cairo_image_surface_get_height(s);
	im->bytes += (size_t)cairo_image_surface_get_stride(s) * im->h;
	return 1;
}

static cairo_surface_t *new_surface(int w, int h, int alpha)
{
	if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM || (unsigned long long)w * h > MAX_PIXELS) return NULL;
	cairo_surface_t *s = cairo_image_surface_create(alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
	return s;
}

/* ---------------- GIF ---------------- */

static void load_gif(const char *path, Image *im)
{
	int err = 0;
	GifFileType *g = DGifOpenFileName(path, &err);
	if (!g) { im->err = 1; return; }
	/* 先看畫布大小，太大的不解（DGifSlurp 會把每一格都讀進記憶體） */
	if ((unsigned long long)g->SWidth * g->SHeight > MAX_PIXELS || g->SWidth > MAX_DIM || g->SHeight > MAX_DIM) { DGifCloseFile(g, &err); im->err = 1; return; }
	if (DGifSlurp(g) != GIF_OK && g->ImageCount == 0) { DGifCloseFile(g, &err); im->err = 1; return; }
	int W = g->SWidth, H = g->SHeight;
	if ((W <= 0 || H <= 0) && g->ImageCount) { W = g->SavedImages[0].ImageDesc.Width; H = g->SavedImages[0].ImageDesc.Height; }
	cairo_surface_t *canvas = new_surface(W, H, 1);
	if (!canvas) { DGifCloseFile(g, &err); im->err = 1; return; }
	uint32_t *cv = (uint32_t *)cairo_image_surface_get_data(canvas);
	int cs = cairo_image_surface_get_stride(canvas) / 4;
	uint32_t *saved = NULL;
	for (int i = 0; i < g->ImageCount; i++) {
		SavedImage *si = &g->SavedImages[i];
		GraphicsControlBlock gcb = { DISPOSAL_UNSPECIFIED, false, 0, NO_TRANSPARENT_COLOR };
		DGifSavedExtensionToGCB(g, i, &gcb);
		ColorMapObject *cm = si->ImageDesc.ColorMap ? si->ImageDesc.ColorMap : g->SColorMap;
		if (!cm || !si->RasterBits) continue;
		if (gcb.DisposalMode == DISPOSE_PREVIOUS) {
			if (!saved) saved = malloc((size_t)cs * H * 4);
			if (saved) memcpy(saved, cv, (size_t)cs * H * 4);
		}
		int fx = si->ImageDesc.Left, fy = si->ImageDesc.Top, fw = si->ImageDesc.Width, fh = si->ImageDesc.Height;
		for (int y = 0; y < fh; y++) {
			int cy = fy + y;
			if (cy < 0 || cy >= H) continue;
			for (int x = 0; x < fw; x++) {
				int cx = fx + x;
				if (cx < 0 || cx >= W) continue;
				int c = si->RasterBits[(size_t)y * fw + x];
				if (c == gcb.TransparentColor || c >= cm->ColorCount) continue;
				GifColorType *col = &cm->Colors[c];
				cv[(size_t)cy * cs + cx] = 0xff000000u | (unsigned)col->Red << 16 | (unsigned)col->Green << 8 | col->Blue;
			}
		}
		cairo_surface_mark_dirty(canvas);
		cairo_surface_t *f = new_surface(W, H, 1);
		if (!f) break;
		memcpy(cairo_image_surface_get_data(f), cv, (size_t)cs * H * 4);
		cairo_surface_mark_dirty(f);
		int delay = gcb.DelayTime < 2 ? 100 : gcb.DelayTime * 10;   /* 跟瀏覽器一樣：太短的當成 0.1 秒 */
		if (!image_add(im, f, delay)) { cairo_surface_destroy(f); break; }
		if (im->bytes > MAX_ANIM_BYTES) break;
		/* 這一格之後要怎麼處理畫布 */
		if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
			for (int y = MAX(0, fy); y < MIN(H, fy + fh); y++)
				for (int x = MAX(0, fx); x < MIN(W, fx + fw); x++) cv[(size_t)y * cs + x] = 0;
		} else if (gcb.DisposalMode == DISPOSE_PREVIOUS && saved) {
			memcpy(cv, saved, (size_t)cs * H * 4);
		}
	}
	free(saved);
	cairo_surface_destroy(canvas);
	DGifCloseFile(g, &err);
	if (!im->n) im->err = 1;
}

/* ---------------- WebP ---------------- */

static unsigned char *read_all(const char *path, size_t *len, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 1 << 16, n = 0;
	unsigned char *d = malloc(cap);
	size_t k;
	while (d && (k = fread(d + n, 1, cap - n, f)) > 0) {
		n += k;
		if (n > max) { free(d); d = NULL; break; }
		if (n == cap) { unsigned char *p = realloc(d, cap *= 2); if (!p) { free(d); d = NULL; break; } d = p; }
	}
	fclose(f);
	*len = n;
	return d;
}

static void load_webp(const char *path, Image *im)
{
	size_t len;
	unsigned char *data = read_all(path, &len, 256u * 1024 * 1024);
	if (!data) { im->err = 1; return; }
	WebPAnimDecoderOptions opt;
	WebPAnimDecoderOptionsInit(&opt);
	opt.color_mode = MODE_bgrA;   /* 預乘 alpha 的 BGRA，跟 cairo ARGB32 一樣 */
	opt.use_threads = 0;
	WebPData wd = { data, len };
	WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
	if (!dec) { free(data); im->err = 1; return; }
	WebPAnimInfo info;
	WebPAnimDecoderGetInfo(dec, &info);
	int prev_ts = 0;
	while (WebPAnimDecoderHasMoreFrames(dec)) {
		uint8_t *buf;
		int ts;
		if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) break;
		cairo_surface_t *f = new_surface((int)info.canvas_width, (int)info.canvas_height, 1);
		if (!f) break;
		int st = cairo_image_surface_get_stride(f);
		unsigned char *dst = cairo_image_surface_get_data(f);
		for (unsigned y = 0; y < info.canvas_height; y++) memcpy(dst + (size_t)y * st, buf + (size_t)y * info.canvas_width * 4, info.canvas_width * 4);
		cairo_surface_mark_dirty(f);
		int delay = ts - prev_ts;
		prev_ts = ts;
		if (!image_add(im, f, delay < 20 ? 100 : delay)) { cairo_surface_destroy(f); break; }
		if (im->bytes > MAX_ANIM_BYTES) break;
	}
	WebPAnimDecoderDelete(dec);
	free(data);
	if (!im->n) im->err = 1;
}

/* ---------------- BMP ---------------- */

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* 遮罩（例如 0x00ff0000）→ 取出 0～255 */
static unsigned mask_val(unsigned v, unsigned mask)
{
	if (!mask) return 255;
	int shift = 0;
	while (!(mask & 1)) { mask >>= 1; shift++; }
	unsigned bits = 0;
	for (unsigned m = mask; m & 1; m >>= 1) bits++;
	unsigned x = (v >> shift) & mask;
	return bits >= 8 ? x >> (bits - 8) : (x * 255) / ((1u << bits) - 1);
}

static void load_bmp(const char *path, Image *im)
{
	size_t len;
	unsigned char *d = read_all(path, &len, 512u * 1024 * 1024);
	if (!d || len < 26 || d[0] != 'B' || d[1] != 'M') { free(d); im->err = 1; return; }
	unsigned off = rd32(d + 10), hs = rd32(d + 14);
	if (hs > len - 14) { free(d); im->err = 1; return; }   /* 壞掉的檔頭：所有位移都要在檔案裡面 */
	int w, h, bpp, comp = 0;
	unsigned rm = 0, gm = 0, bm = 0, am = 0, ncol = 0;
	if (hs == 12) { w = (int)rd16(d + 18); h = (int)(int16_t)rd16(d + 20); bpp = (int)rd16(d + 24); }
	else if (hs >= 40 && len >= 14 + 40) {
		w = (int)rd32(d + 18); h = (int)rd32(d + 22); bpp = (int)rd16(d + 28); comp = (int)rd32(d + 30); ncol = rd32(d + 46);
		if (comp == 3 || comp == 6) {
			size_t mk = hs >= 52 ? 54 : 14 + (size_t)hs;   /* 40 位元組的檔頭：遮罩接在後面 */
			if (mk + 12 <= len) { rm = rd32(d + mk); gm = rd32(d + mk + 4); bm = rd32(d + mk + 8); }
			if ((hs >= 56 || comp == 6) && mk + 16 <= len) am = rd32(d + mk + 12);
		}
	} else { free(d); im->err = 1; return; }
	int topdown = h < 0;
	if (topdown) h = h == INT_MIN ? 0 : -h;
	if (w <= 0 || h <= 0 || w > MAX_DIM || h > MAX_DIM) { free(d); im->err = 1; return; }
	if (comp != 0 && comp != 3 && comp != 6) { free(d); im->err = 2; snprintf(im->fmt, sizeof im->fmt, "BMP RLE"); return; }
	if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) { free(d); im->err = 1; return; }
	if (bpp == 16 && comp == 0) { rm = 0x7c00; gm = 0x03e0; bm = 0x001f; }
	if (bpp == 32 && comp == 0) { rm = 0xff0000; gm = 0xff00; bm = 0xff; am = 0xff000000; }
	size_t pal_off = 14 + (size_t)hs;
	int pal_sz = hs == 12 ? 3 : 4;
	if (!ncol && bpp <= 8) ncol = 1u << bpp;
	size_t pal_avail = pal_off < len ? (len - pal_off) / (size_t)pal_sz : 0;
	if (ncol > pal_avail) ncol = (unsigned)pal_avail;
	size_t stride = ((size_t)w * bpp + 31) / 32 * 4;
	if (off >= len || (size_t)off + stride * (size_t)(h - 1) + ((size_t)w * bpp + 7) / 8 > len) { free(d); im->err = 1; return; }
	cairo_surface_t *s = new_surface(w, h, 1);
	if (!s) { free(d); im->err = 1; return; }
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
	int ss = cairo_image_surface_get_stride(s) / 4;
	int any_alpha = 0;
	for (int y = 0; y < h; y++) {
		const unsigned char *row = d + off + stride * (size_t)(topdown ? y : h - 1 - y);
		uint32_t *o = px + (size_t)y * ss;
		for (int x = 0; x < w; x++) {
			unsigned r, g, b, a = 255;
			if (bpp <= 8) {
				unsigned idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> (x % 2 ? 0 : 4)) & 15 : (row[x / 8] >> (7 - x % 8)) & 1;
				if (idx < ncol) { const unsigned char *c = d + pal_off + (size_t)idx * pal_sz; b = c[0]; g = c[1]; r = c[2]; }
				else r = g = b = 0;
			} else if (bpp == 24) { b = row[x * 3]; g = row[x * 3 + 1]; r = row[x * 3 + 2]; }
			else {
				unsigned v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
				r = mask_val(v, rm); g = mask_val(v, gm); b = mask_val(v, bm);
				a = am ? mask_val(v, am) : 255;
				if (am && a) any_alpha = 1;
			}
			/* 預乘 alpha */
			o[x] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
		}
	}
	/* 很多程式存 32 位元 BMP 時 alpha 全是 0：當成不透明 */
	if (am && !any_alpha)
		for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
			const unsigned char *row = d + off + stride * (size_t)(topdown ? y : h - 1 - y);
			unsigned v = bpp == 16 ? rd16(row + x * 2) : rd32(row + x * 4);
			px[(size_t)y * ss + x] = 0xff000000u | mask_val(v, rm) << 16 | mask_val(v, gm) << 8 | mask_val(v, bm);
		}
	cairo_surface_mark_dirty(s);
	free(d);
	image_add(im, s, 0);
}

/* ---------------- PNM（P4、P5、P6） ---------------- */

static void load_pnm(const char *path, Image *im)
{
	size_t len;
	unsigned char *d = read_all(path, &len, 512u * 1024 * 1024);
	if (!d || len < 3) { free(d); im->err = 1; return; }
	int type = d[1] - '0';
	size_t p = 2;
	long v[3] = { 0, 0, 1 };
	int need = type == 4 ? 2 : 3;
	for (int i = 0; i < need; i++) {
		while (p < len && (isspace(d[p]) || d[p] == '#')) { if (d[p] == '#') while (p < len && d[p] != '\n') p++; else p++; }
		long x = 0;
		while (p < len && isdigit(d[p]) && x < 1000000) x = x * 10 + (d[p++] - '0');
		v[i] = x;
	}
	p++;   /* 一個空白之後就是資料 */
	int w = (int)v[0], h = (int)v[1], maxv = (int)v[2];
	int bps = maxv > 255 ? 2 : 1;
	size_t need_bytes = type == 4 ? (size_t)(w + 7) / 8 * h : (size_t)w * h * bps * (type == 6 ? 3 : 1);
	cairo_surface_t *s = maxv > 0 && p + need_bytes <= len ? new_surface(w, h, 0) : NULL;
	if (!s) { free(d); im->err = 1; return; }
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
	int ss = cairo_image_surface_get_stride(s) / 4;
	const unsigned char *q = d + p;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			unsigned r, g, b;
			if (type == 4) { int bit = (q[(size_t)y * ((w + 7) / 8) + x / 8] >> (7 - x % 8)) & 1; r = g = b = bit ? 0 : 255; }
			else if (type == 5) { size_t i = ((size_t)y * w + x) * bps; r = g = b = q[i] * 255u / (bps == 2 ? (unsigned)maxv >> 8 | 1 : (unsigned)maxv); if (r > 255) r = g = b = 255; }
			else {
				size_t i = ((size_t)y * w + x) * 3 * bps;
				unsigned mv = bps == 2 ? ((unsigned)maxv >> 8) | 1 : (unsigned)maxv;
				r = MIN(255u, q[i] * 255u / mv); g = MIN(255u, q[i + bps] * 255u / mv); b = MIN(255u, q[i + 2 * bps] * 255u / mv);
			}
			px[(size_t)y * ss + x] = r << 16 | g << 8 | b;
		}
	cairo_surface_mark_dirty(s);
	free(d);
	image_add(im, s, 0);
}

/* ---------------- HEIC、AVIF（libheif，執行時才載入） ---------------- */

/* 照 libheif 的 heif.h（這些數值是固定的 API） */
struct heif_err { int code, subcode; const char *message; };
enum { HEIF_UNSUPPORTED_FEATURE = 4, HEIF_DECODER_PLUGIN_ERROR = 7, HEIF_UNSUPPORTED_CODEC = 3000 };
enum { HEIF_COLORSPACE_RGB = 1, HEIF_CHROMA_RGBA = 11, HEIF_CHANNEL_INTERLEAVED = 10 };

static struct {
	int state;   /* 0 還沒試、1 可以用、-1 沒有裝 */
	void *lib;
	struct heif_err (*init)(void *);
	void *(*ctx_alloc)(void);
	void (*ctx_free)(void *);
	struct heif_err (*read_file)(void *, const char *, const void *);
	struct heif_err (*primary)(void *, void **);
	void (*handle_release)(const void *);
	int (*handle_w)(const void *), (*handle_h)(const void *);
	struct heif_err (*decode)(const void *, void **, int, int, const void *);
	const uint8_t *(*plane)(const void *, int, int *);
	void (*img_release)(const void *);
	int (*img_w)(const void *, int), (*img_h)(const void *, int);
} HF;
static pthread_mutex_t heif_mu = PTHREAD_MUTEX_INITIALIZER;

/* 載入 libheif；retry = 剛裝好，再試一次 */
static int heif_ready(int retry)
{
	pthread_mutex_lock(&heif_mu);
	if (retry && HF.state < 0) HF.state = 0;
	if (!HF.state) {
		HF.state = -1;
		void *h = getenv("POMELO_NO_LIBHEIF") ? NULL : dlopen("libheif.so.1", RTLD_NOW | RTLD_LOCAL);   /* 環境變數：測試「沒裝」的畫面 */
		if (h) {
#define SYM(f, n) (*(void **)&HF.f = dlsym(h, n))
			int ok = SYM(ctx_alloc, "heif_context_alloc") && SYM(ctx_free, "heif_context_free") &&
			         SYM(read_file, "heif_context_read_from_file") && SYM(primary, "heif_context_get_primary_image_handle") &&
			         SYM(handle_release, "heif_image_handle_release") && SYM(handle_w, "heif_image_handle_get_width") &&
			         SYM(handle_h, "heif_image_handle_get_height") && SYM(decode, "heif_decode_image") &&
			         SYM(plane, "heif_image_get_plane_readonly") && SYM(img_release, "heif_image_release") &&
			         SYM(img_w, "heif_image_get_width") && SYM(img_h, "heif_image_get_height");
			SYM(init, "heif_init");   /* 1.13 以後才有；要先呼叫才會載入解碼外掛 */
#undef SYM
			if (ok) {
				if (HF.init) HF.init(NULL);
				HF.lib = h;
				HF.state = 1;
			} else dlclose(h);
		}
	}
	int r = HF.state > 0;
	pthread_mutex_unlock(&heif_mu);
	return r;
}

static void load_heif(const char *path, Image *im)
{
	if (!heif_ready(0)) { im->err = 2; im->need = NEED_LIBHEIF; return; }
	void *ctx = HF.ctx_alloc(), *hd = NULL, *img = NULL;
	if (!ctx) { im->err = 1; return; }
	struct heif_err e = HF.read_file(ctx, path, NULL);
	if (!e.code) e = HF.primary(ctx, &hd);
	if (e.code || !hd) { HF.ctx_free(ctx); im->err = 1; return; }
	long long hw = HF.handle_w(hd), hh = HF.handle_h(hd);
	if (hw <= 0 || hh <= 0 || hw * hh > 400000000LL) { HF.handle_release(hd); HF.ctx_free(ctx); im->err = 1; return; }
	e = HF.decode(hd, &img, HEIF_COLORSPACE_RGB, HEIF_CHROMA_RGBA, NULL);   /* 旋轉、鏡像 libheif 會自己處理 */
	if (e.code || !img) {
		/* 有 libheif 但沒有 HEVC 解碼器（Fedora、Rocky 官方的 libheif；Ubuntu 少裝了 libheif-plugin-libde265） */
		if (e.code == HEIF_UNSUPPORTED_FEATURE || e.code == HEIF_DECODER_PLUGIN_ERROR || e.subcode == HEIF_UNSUPPORTED_CODEC) {
			im->err = 2;
			im->need = strcmp(im->fmt, "AVIF") ? NEED_HEVC : NEED_LIBHEIF;
		} else im->err = 1;
		HF.handle_release(hd);
		HF.ctx_free(ctx);
		return;
	}
	int stride = 0;
	const uint8_t *px = HF.plane(img, HEIF_CHANNEL_INTERLEAVED, &stride);
	int w = HF.img_w(img, HEIF_CHANNEL_INTERLEAVED), h = HF.img_h(img, HEIF_CHANNEL_INTERLEAVED);
	/* 手機的 4800 萬像素照片超過上限：每 k×k 個像素平均成一個 */
	int k = 1;
	while ((unsigned long long)(w / k) * (h / k) > MAX_PIXELS || w / k > MAX_DIM || h / k > MAX_DIM) k++;
	cairo_surface_t *s = px && w > 0 && h > 0 ? new_surface(w / k, h / k, 1) : NULL;
	if (s) {
		uint32_t *out = (uint32_t *)cairo_image_surface_get_data(s);
		int os = cairo_image_surface_get_stride(s) / 4, ow = w / k, oh = h / k;
		for (int y = 0; y < oh; y++)
			for (int x = 0; x < ow; x++) {
				unsigned r = 0, g = 0, b = 0, a = 0;
				for (int dy = 0; dy < k; dy++) {
					const uint8_t *p = px + (size_t)(y * k + dy) * stride + (size_t)x * k * 4;
					for (int dx = 0; dx < k; dx++, p += 4) { r += p[0]; g += p[1]; b += p[2]; a += p[3]; }
				}
				unsigned n = (unsigned)(k * k);
				r /= n; g /= n; b /= n; a /= n;
				out[(size_t)y * os + x] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
			}
		cairo_surface_mark_dirty(s);
		if (!image_add(im, s, 0)) cairo_surface_destroy(s);
		im->ow = w; im->oh = h;
	} else im->err = 1;
	HF.img_release(img);
	HF.handle_release(hd);
	HF.ctx_free(ctx);
}

/* ---------------- TIFF（libtiff，執行時才載入） ---------------- */

typedef struct tiff TIFF;
static struct {
	int state;   /* 0 還沒試、1 可以、-1 找不到 */
	TIFF *(*open)(const char *, const char *);
	void (*close)(TIFF *);
	int (*getfield)(TIFF *, uint32_t, ...);
	int (*read_rgba)(TIFF *, uint32_t, uint32_t, uint32_t *, int, int);
	void *(*set_warning)(void *);
	void *(*set_error)(void *);
} TF;
static pthread_mutex_t tiff_mu = PTHREAD_MUTEX_INITIALIZER;
#define TIFFTAG_IMAGEWIDTH 256
#define TIFFTAG_IMAGELENGTH 257
#define ORIENTATION_TOPLEFT 1

static int tiff_ready(int retry)
{
	pthread_mutex_lock(&tiff_mu);
	if (retry && TF.state < 0) TF.state = 0;
	if (!TF.state) {
		TF.state = -1;
		void *h = NULL;
		if (!getenv("POMELO_NO_LIBTIFF")) {   /* 環境變數：測試「沒裝」的畫面 */
			h = dlopen("libtiff.so.6", RTLD_NOW | RTLD_LOCAL);
			if (!h) h = dlopen("libtiff.so.5", RTLD_NOW | RTLD_LOCAL);
		}
		if (h) {
			*(void **)&TF.open = dlsym(h, "TIFFOpen");
			*(void **)&TF.close = dlsym(h, "TIFFClose");
			*(void **)&TF.getfield = dlsym(h, "TIFFGetField");
			*(void **)&TF.read_rgba = dlsym(h, "TIFFReadRGBAImageOriented");
			*(void **)&TF.set_warning = dlsym(h, "TIFFSetWarningHandler");
			*(void **)&TF.set_error = dlsym(h, "TIFFSetErrorHandler");
			if (TF.open && TF.close && TF.getfield && TF.read_rgba) {
				if (TF.set_warning) TF.set_warning(NULL);   /* 不要把警告印到終端機 */
				if (TF.set_error) TF.set_error(NULL);
				TF.state = 1;
			} else dlclose(h);
		}
	}
	int r = TF.state > 0;
	pthread_mutex_unlock(&tiff_mu);
	return r;
}

/* 多頁的 TIFF 只看第一頁 */
static void load_tiff(const char *path, Image *im)
{
	if (!tiff_ready(0)) { im->err = 2; im->need = NEED_LIBTIFF; return; }
	TIFF *t = TF.open(path, "r");
	if (!t) { im->err = 1; return; }
	uint32_t w = 0, h = 0;
	TF.getfield(t, TIFFTAG_IMAGEWIDTH, &w);
	TF.getfield(t, TIFFTAG_IMAGELENGTH, &h);
	cairo_surface_t *s = new_surface((int)w, (int)h, 1);
	uint32_t *ras = s ? malloc((size_t)w * h * 4) : NULL;
	if (ras && TF.read_rgba(t, w, h, ras, ORIENTATION_TOPLEFT, 0)) {
		/* libtiff 給的是 ABGR（R 在最低位元組），透明度已經乘過了：換成 cairo 的 ARGB */
		cairo_surface_flush(s);
		unsigned char *d = cairo_image_surface_get_data(s);
		int st = cairo_image_surface_get_stride(s);
		for (uint32_t y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(d + (size_t)y * st);
			for (uint32_t x = 0; x < w; x++) {
				uint32_t p = ras[(size_t)y * w + x];
				row[x] = (p & 0xff000000u) | (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff);
			}
		}
		cairo_surface_mark_dirty(s);
		image_add(im, s, 0);
		s = NULL;
	} else im->err = 1;
	if (s) cairo_surface_destroy(s);
	free(ras);
	TF.close(t);
}

static void decoders_retry(void) { heif_ready(1); tiff_ready(1); }   /* 剛裝好解碼程式：再試一次 */

/* ---------------- SVG（nanosvg） ---------------- */

/* 開頭看起來像 XML／SVG：再讀前 4 KB 找 <svg */
static int sniff_svg(const char *path)
{
	char b[4097];
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	size_t n = fread(b, 1, sizeof b - 1, f);
	fclose(f);
	b[n] = 0;
	return strstr(b, "<svg") != NULL;
}

static void load_svg(const char *path, Image *im)
{
	size_t len = 0;
	unsigned char *raw = read_all(path, &len, 32u << 20);
	char *txt = raw ? malloc(len + 1) : NULL;
	if (!txt) { free(raw); im->err = 1; return; }
	memcpy(txt, raw, len);
	txt[len] = 0;
	free(raw);
	NSVGimage *svg = nsvgParse(txt, "px", 96.0f);   /* nsvgParse 會改 txt */
	free(txt);
	if (!svg || !(svg->width > 0) || !(svg->height > 0)) { if (svg) nsvgDelete(svg); im->err = 1; return; }
	/* 小圖示畫大一點（放大看也清楚），大的照原尺寸，最長 8192 像素、總像素有上限 */
	double sw = svg->width, sh = svg->height, longer = MAX(sw, sh), scale = longer < 1024 ? 1024 / longer : 1;
	if (longer * scale > 8192) scale = 8192 / longer;
	while (sw * scale * sh * scale > (double)MAX_PIXELS) scale *= 0.9;
	int w = MAX(1, (int)lround(sw * scale)), h = MAX(1, (int)lround(sh * scale));
	cairo_surface_t *s = new_surface(w, h, 1);
	unsigned char *rgba = s ? calloc((size_t)w * h, 4) : NULL;
	NSVGrasterizer *r = rgba ? nsvgCreateRasterizer() : NULL;
	if (r) {
		nsvgRasterize(r, svg, 0, 0, (float)scale, rgba, w, h, w * 4);
		/* nanosvg 給的是沒乘透明度的 RGBA：換成 cairo 的（乘過的）ARGB */
		cairo_surface_flush(s);
		unsigned char *d = cairo_image_surface_get_data(s);
		int st = cairo_image_surface_get_stride(s);
		for (int y = 0; y < h; y++) {
			uint32_t *row = (uint32_t *)(d + (size_t)y * st);
			const unsigned char *p = rgba + (size_t)y * w * 4;
			for (int x = 0; x < w; x++, p += 4) {
				unsigned a = p[3];
				row[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
			}
		}
		cairo_surface_mark_dirty(s);
		image_add(im, s, 0);
		s = NULL;
		nsvgDeleteRasterizer(r);
	} else im->err = 1;
	if (s) cairo_surface_destroy(s);
	free(rgba);
	nsvgDelete(svg);
}

/* ---------------- 判斷格式 ---------------- */

static const char *const image_exts[] = { "png", "jpg", "jpeg", "jpe", "jfif", "gif", "webp", "bmp", "dib", "pnm", "ppm", "pgm", "pbm",
                                          "heic", "heif", "avif", "tif", "tiff", "svg" };

static int is_image_name(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || name[0] == '.') return 0;
	for (unsigned i = 0; i < LEN(image_exts); i++)
		if (!strcasecmp(dot + 1, image_exts[i])) return 1;
	return 0;
}

typedef struct Exif Exif;
static int exif_dims(const char *path, int *w, int *h);

static Image *image_load(const char *path)
{
	Image *im = calloc(1, sizeof *im);
	if (!im) return NULL;
	unsigned char m[32] = { 0 };
	FILE *f = fopen(path, "rb");
	if (!f) { im->err = 1; return im; }
	size_t n = fread(m, 1, sizeof m, f);
	fclose(f);
	if (n >= 8 && !memcmp(m, "\x89PNG", 4)) {
		snprintf(im->fmt, sizeof im->fmt, "PNG");
		cairo_surface_t *s = pm_image_load(path);
		if (s) image_add(im, s, 0); else im->err = 1;
	} else if (n >= 3 && m[0] == 0xff && m[1] == 0xd8 && m[2] == 0xff) {
		snprintf(im->fmt, sizeof im->fmt, "JPEG");
		cairo_surface_t *s = pm_image_load(path);
		if (s) image_add(im, s, 0); else im->err = 1;
	} else if (n >= 6 && (!memcmp(m, "GIF87a", 6) || !memcmp(m, "GIF89a", 6))) {
		snprintf(im->fmt, sizeof im->fmt, "GIF");
		load_gif(path, im);
	} else if (n >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WEBP", 4)) {
		snprintf(im->fmt, sizeof im->fmt, "WebP");
		load_webp(path, im);
	} else if (n >= 2 && m[0] == 'B' && m[1] == 'M') {
		snprintf(im->fmt, sizeof im->fmt, "BMP");
		load_bmp(path, im);
	} else if (n >= 3 && m[0] == 'P' && (m[1] == '4' || m[1] == '5' || m[1] == '6') && isspace(m[2])) {
		snprintf(im->fmt, sizeof im->fmt, "PNM");
		load_pnm(path, im);
	} else if (n >= 12 && !memcmp(m + 4, "ftyp", 4) && (!memcmp(m + 8, "avif", 4) || !memcmp(m + 8, "avis", 4) ||
	           !memcmp(m + 8, "heic", 4) || !memcmp(m + 8, "heix", 4) || !memcmp(m + 8, "mif1", 4) || !memcmp(m + 8, "msf1", 4) ||
	           !memcmp(m + 8, "hevc", 4) || !memcmp(m + 8, "heim", 4) || !memcmp(m + 8, "heis", 4))) {
		/* mif1 是通用的 HEIF 容器：看相容品牌裡有沒有 avif */
		int avif = !memcmp(m + 8, "avif", 4) || !memcmp(m + 8, "avis", 4) || (n >= 24 && (!memcmp(m + 16, "avif", 4) || !memcmp(m + 20, "avif", 4)));
		snprintf(im->fmt, sizeof im->fmt, "%s", avif ? "AVIF" : "HEIC");
		load_heif(path, im);
	} else if (n >= 4 && (!memcmp(m, "II*\0", 4) || !memcmp(m, "MM\0*", 4))) {
		snprintf(im->fmt, sizeof im->fmt, "TIFF");
		load_tiff(path, im);
	} else if (n >= 4 && (!memcmp(m, "<svg", 4) || !memcmp(m, "<?xm", 4) || !memcmp(m, "<!--", 4) || !memcmp(m, "<!DO", 4) ||
	           (n >= 3 && m[0] == 0xef && m[1] == 0xbb && m[2] == 0xbf)) && sniff_svg(path)) {
		snprintf(im->fmt, sizeof im->fmt, "SVG");
		load_svg(path, im);
	} else {
		/* 認得但還不支援的格式 */
		const char *u = NULL;
		if (n >= 4 && !memcmp(m, "8BPS", 4)) u = "PSD";
		im->err = u ? 2 : 1;
		snprintf(im->fmt, sizeof im->fmt, "%s", u ? u : "");
	}
	if (!im->err && !im->n) im->err = 1;
	if (!im->ow) { im->ow = im->w; im->oh = im->h; }
	if (!im->err && !strcmp(im->fmt, "JPEG")) {
		int w, h;
		if (exif_dims(path, &w, &h) && w >= im->w && h >= im->h) { im->ow = w; im->oh = h; }
	}
	return im;
}

/* ---------------- EXIF 資訊（資訊面板用） ---------------- */

struct Exif {
	char make[64], model[64], taken[32], exposure[24], fnumber[16], iso[16], focal[16];
	int ow, oh;          /* 原始尺寸（JPEG 的 SOF；大照片解碼時可能縮小過） */
	int orientation;
};

typedef struct { const unsigned char *t; unsigned n; int le; } Tiff;

static unsigned t16(Tiff *t, unsigned o) { if (o + 2 > t->n) return 0; const unsigned char *p = t->t + o; return t->le ? (unsigned)(p[0] | p[1] << 8) : (unsigned)(p[0] << 8 | p[1]); }
static unsigned t32(Tiff *t, unsigned o)
{
	if (o + 4 > t->n) return 0;
	const unsigned char *p = t->t + o;
	return t->le ? p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24 : (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void t_str(Tiff *t, unsigned e, char *out, size_t n)
{
	unsigned cnt = t32(t, e + 4), off = cnt <= 4 ? e + 8 : t32(t, e + 8);
	if (off >= t->n || !cnt) return;
	size_t k = MIN((size_t)cnt, n - 1);
	if (off + k > t->n) k = t->n - off;
	memcpy(out, t->t + off, k);
	out[k] = 0;
	/* 去掉結尾空白 */
	for (int i = (int)strlen(out) - 1; i >= 0 && (out[i] == ' ' || !out[i]); i--) out[i] = 0;
}

static double t_rat(Tiff *t, unsigned e)
{
	unsigned off = t32(t, e + 8), a = t32(t, off), b = t32(t, off + 4);
	return b ? (double)a / b : 0;
}

static void exif_ifd(Tiff *t, unsigned ifd, Exif *x, int depth)
{
	unsigned cnt = t16(t, ifd);
	for (unsigned i = 0; i < cnt && i < 512; i++) {
		unsigned e = ifd + 2 + i * 12;
		if (e + 12 > t->n) break;
		unsigned tag = t16(t, e);
		switch (tag) {
		case 0x010f: t_str(t, e, x->make, sizeof x->make); break;
		case 0x0110: t_str(t, e, x->model, sizeof x->model); break;
		case 0x0112: x->orientation = (int)t16(t, e + 8); break;
		case 0x0132: if (!x->taken[0]) t_str(t, e, x->taken, sizeof x->taken); break;
		case 0x9003: t_str(t, e, x->taken, sizeof x->taken); break;
		case 0x829a: { double v = t_rat(t, e); if (v > 0 && v < 1) snprintf(x->exposure, sizeof x->exposure, "1/%.0f s", 1 / v); else if (v >= 1) snprintf(x->exposure, sizeof x->exposure, "%.1f s", v); break; }
		case 0x829d: { double v = t_rat(t, e); if (v > 0) snprintf(x->fnumber, sizeof x->fnumber, "f/%.1f", v); break; }
		case 0x8827: snprintf(x->iso, sizeof x->iso, "ISO %u", t16(t, e + 8)); break;
		case 0x920a: { double v = t_rat(t, e); if (v > 0) snprintf(x->focal, sizeof x->focal, "%.1f mm", v); break; }
		case 0x8769: if (depth < 2) exif_ifd(t, t32(t, e + 8), x, depth + 1); break;
		}
	}
}

static int exif_read(const char *path, Exif *x)
{
	memset(x, 0, sizeof *x);
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	unsigned char hdr[4];
	if (fread(hdr, 1, 2, f) != 2 || hdr[0] != 0xff || hdr[1] != 0xd8) { fclose(f); return 0; }
	int found = 0;
	for (int guard = 0; guard < 64; guard++) {
		int c;
		do c = fgetc(f); while (c == 0xff);
		if (c == EOF || fread(hdr, 1, 2, f) != 2) break;
		unsigned len = (unsigned)(hdr[0] << 8 | hdr[1]);
		if (len < 2) break;
		if (c == 0xe1 && !found) {
			unsigned char *seg = malloc(len);
			if (seg && fread(seg, 1, len - 2, f) == len - 2 && len > 16 && !memcmp(seg, "Exif\0\0", 6)) {
				Tiff t = { seg + 6, len - 8, seg[6] == 'I' };
				exif_ifd(&t, t32(&t, 4), x, 0);
				found = 1;
			}
			free(seg);
			continue;
		}
		if ((c >= 0xc0 && c <= 0xcf) && c != 0xc4 && c != 0xc8 && c != 0xcc) {   /* SOFn：寬高 */
			unsigned char sof[5];
			if (fread(sof, 1, 5, f) == 5) { x->oh = sof[1] << 8 | sof[2]; x->ow = sof[3] << 8 | sof[4]; }
			break;
		}
		if (c == 0xda) break;
		fseek(f, len - 2, SEEK_CUR);
	}
	fclose(f);
	if (x->orientation >= 5 && x->orientation <= 8) { int t = x->ow; x->ow = x->oh; x->oh = t; }
	/* 2026:10:19 08:30:00 → 2026-10-19 08:30 */
	if (strlen(x->taken) >= 16 && x->taken[4] == ':') { x->taken[4] = x->taken[7] = '-'; x->taken[16] = 0; }
	/* 型號裡常常已經有廠牌（Canon Canon EOS R6） */
	if (x->make[0] && !strncasecmp(x->model, x->make, strlen(x->make))) x->make[0] = 0;
	return found || x->ow;
}

static int exif_dims(const char *path, int *w, int *h)
{
	Exif x;
	exif_read(path, &x);
	*w = x.ow; *h = x.oh;
	return x.ow > 0;
}
