/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 圖片載入（PNG 用 cairo，JPEG 用 libjpeg）
 * JPEG 會依 EXIF 的方向自動轉正（手機直拍的照片），CMYK 的 JPEG 會換成 RGB。 */
/* 上限：單邊 16384 像素、總共 4000 萬像素（約 160 MB）。8K 桌布是 3300 萬像素；
 * 更大的 JPEG（例如 5000 萬、2 億像素的手機照片）會在解碼時直接縮成 1/2、1/4… 放得下的大小 */
#define PM_IMAGE_MAX_DIM 16384
#define PM_IMAGE_MAX_PIXELS 40000000ULL
#define _GNU_SOURCE
#include "pomelo.h"
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <jpeglib.h>
#include <setjmp.h>

struct jerr { struct jpeg_error_mgr mgr; jmp_buf jb; };

static void jpeg_fail(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jpeg_quiet(j_common_ptr c, int level) { (void)c; (void)level; }   /* 不要在終端機印警告 */

/* EXIF（APP1）裡的方向：1 = 正常，2～8 = 要翻轉／旋轉；讀不到回傳 1 */
static int exif_orientation(const unsigned char *d, unsigned len)
{
	if (len < 14 || memcmp(d, "Exif\0\0", 6)) return 1;
	const unsigned char *t = d + 6;
	unsigned n = len - 6;
	int le = t[0] == 'I';
	if (!(t[0] == 'I' && t[1] == 'I') && !(t[0] == 'M' && t[1] == 'M')) return 1;
#define U16(p) (le ? (unsigned)(p)[0] | (unsigned)(p)[1] << 8 : (unsigned)(p)[0] << 8 | (unsigned)(p)[1])
#define U32(p) (le ? (unsigned)(p)[0] | (unsigned)(p)[1] << 8 | (unsigned)(p)[2] << 16 | (unsigned)(p)[3] << 24 \
                   : (unsigned)(p)[0] << 24 | (unsigned)(p)[1] << 16 | (unsigned)(p)[2] << 8 | (unsigned)(p)[3])
	unsigned ifd = U32(t + 4);
	if (ifd + 2 > n) return 1;
	unsigned cnt = U16(t + ifd);
	for (unsigned i = 0; i < cnt && ifd + 2 + i * 12 + 12 <= n; i++) {
		const unsigned char *e = t + ifd + 2 + i * 12;
		if (U16(e) == 0x0112) {
			unsigned v = U16(e + 8);
			return v >= 1 && v <= 8 ? (int)v : 1;
		}
	}
#undef U16
#undef U32
	return 1;
}

/* 依 EXIF 方向轉正：回傳新的圖（原本的會釋放） */
static cairo_surface_t *orient(cairo_surface_t *s, int o)
{
	if (o <= 1 || !s) return s;
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int swap = o >= 5;
	int nw = swap ? h : w, nh = swap ? w : h;
	cairo_surface_t *d = cairo_image_surface_create(cairo_image_surface_get_format(s), nw, nh);
	if (cairo_surface_status(d) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(d); return s; }
	cairo_surface_flush(s);
	const unsigned char *sp = cairo_image_surface_get_data(s);
	unsigned char *dp = cairo_image_surface_get_data(d);
	int ss = cairo_image_surface_get_stride(s), ds = cairo_image_surface_get_stride(d);
	for (int y = 0; y < nh; y++) {
		uint32_t *row = (uint32_t *)(dp + (size_t)y * ds);
		for (int x = 0; x < nw; x++) {
			int sx, sy;
			switch (o) {
			case 2: sx = w - 1 - x; sy = y; break;               /* 左右翻轉 */
			case 3: sx = w - 1 - x; sy = h - 1 - y; break;       /* 轉 180° */
			case 4: sx = x; sy = h - 1 - y; break;               /* 上下翻轉 */
			case 5: sx = y; sy = x; break;                       /* 沿左上到右下的對角線翻轉 */
			case 6: sx = y; sy = h - 1 - x; break;               /* 順時針轉 90° */
			case 7: sx = w - 1 - y; sy = h - 1 - x; break;
			default: sx = w - 1 - y; sy = x; break;              /* 8：逆時針轉 90° */
			}
			row[x] = ((const uint32_t *)(sp + (size_t)sy * ss))[sx];
		}
	}
	cairo_surface_mark_dirty(d);
	cairo_surface_destroy(s);
	return d;
}

/* 從檔案（f）或記憶體（mem、len）解碼 JPEG */
static cairo_surface_t *decode_jpeg(FILE *f, const void *mem, size_t len, int maxdim)
{
	struct jpeg_decompress_struct ci;
	struct jerr je;
	cairo_surface_t *s = NULL;
	ci.err = jpeg_std_error(&je.mgr);
	je.mgr.error_exit = jpeg_fail;
	je.mgr.emit_message = jpeg_quiet;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		if (s) cairo_surface_destroy(s);
		return NULL;
	}
	jpeg_create_decompress(&ci);
	if (f) jpeg_stdio_src(&ci, f);
	else jpeg_mem_src(&ci, (const unsigned char *)mem, (unsigned long)len);
	jpeg_save_markers(&ci, JPEG_APP0 + 1, 0xffff);
	jpeg_read_header(&ci, TRUE);
	if (ci.image_width == 0 || ci.image_height == 0) { jpeg_destroy_decompress(&ci); return NULL; }
	/* 只要小圖時讓解碼器直接縮小，快很多；太大的圖也縮到放得下（最多縮成 1/8） */
	unsigned d = 1;
	if (maxdim > 0)
		while (d < 8 && ci.image_width / (d * 2) >= (unsigned)maxdim && ci.image_height / (d * 2) >= (unsigned)maxdim)
			d *= 2;
	while (d < 8 && (ci.image_width / d > PM_IMAGE_MAX_DIM || ci.image_height / d > PM_IMAGE_MAX_DIM ||
	                 (unsigned long long)(ci.image_width / d) * (ci.image_height / d) > PM_IMAGE_MAX_PIXELS))
		d *= 2;
	ci.scale_num = 1;
	ci.scale_denom = d;
	volatile int orientation = 1;
	for (jpeg_saved_marker_ptr m = ci.marker_list; m; m = m->next)
		if (m->marker == JPEG_APP0 + 1 && (orientation = exif_orientation(m->data, m->data_length)) != 1) break;
	/* CMYK（印刷用，例如 Photoshop 存的）：libjpeg 不會轉 RGB，自己算 */
	int cmyk = ci.jpeg_color_space == JCS_CMYK || ci.jpeg_color_space == JCS_YCCK;
	int adobe = ci.saw_Adobe_marker;
	if (cmyk) ci.out_color_space = JCS_CMYK;
	else {
#ifdef JCS_EXT_BGRX
		ci.out_color_space = JCS_EXT_BGRX;   /* 和 cairo 的 RGB24 排列相同 */
#else
		ci.out_color_space = JCS_RGB;
#endif
	}
	jpeg_start_decompress(&ci);
	int w = (int)ci.output_width, h = (int)ci.output_height;
	if (w <= 0 || h <= 0 || w > PM_IMAGE_MAX_DIM || h > PM_IMAGE_MAX_DIM || (unsigned long long)w * h > PM_IMAGE_MAX_PIXELS) {
		jpeg_destroy_decompress(&ci);
		return NULL;
	}
	s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {   /* 記憶體不夠 */
		cairo_surface_destroy(s);
		jpeg_destroy_decompress(&ci);
		return NULL;
	}
	unsigned char *data = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	unsigned char *volatile row = NULL;   /* volatile：longjmp 回來時才能正確釋放 */
#ifdef JCS_EXT_BGRX
	if (cmyk)
#endif
		row = malloc((size_t)w * 4);
	if (setjmp(je.jb)) {   /* 解到一半壞掉：已經解出來的部分照樣顯示（下載到一半的照片） */
		free(row);
		row = NULL;
		jpeg_destroy_decompress(&ci);
		cairo_surface_mark_dirty(s);
		return orient(s, orientation);
	}
	while (ci.output_scanline < ci.output_height) {
		unsigned char *dst = data + (size_t)ci.output_scanline * stride;
		if (!row) { jpeg_read_scanlines(&ci, &dst, 1); continue; }
		unsigned char *r = row;
		jpeg_read_scanlines(&ci, &r, 1);
		for (int x = 0; x < w; x++) {
			unsigned R, G, B;
			if (cmyk) {
				unsigned c = row[x * 4], m = row[x * 4 + 1], yy = row[x * 4 + 2], k = row[x * 4 + 3];
				if (!adobe) { c = 255 - c; m = 255 - m; yy = 255 - yy; k = 255 - k; }   /* Adobe 存的是反過來的值 */
				R = c * k / 255; G = m * k / 255; B = yy * k / 255;
			} else { R = row[x * 3]; G = row[x * 3 + 1]; B = row[x * 3 + 2]; }
			((uint32_t *)dst)[x] = (R << 16) | (G << 8) | B;
		}
	}
	free(row);
	row = NULL;
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	cairo_surface_mark_dirty(s);
	return orient(s, orientation);
}

static cairo_surface_t *load_jpeg(const char *path, int maxdim)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	cairo_surface_t *s = decode_jpeg(f, NULL, 0, maxdim);
	fclose(f);
	return s;
}

cairo_surface_t *pm_image_jpeg_mem(const void *data, size_t len, int maxdim)
{
	if (!data || len < 4) return NULL;
	return decode_jpeg(NULL, data, len, maxdim);
}

static cairo_surface_t *load_any(const char *path, int maxdim)
{
	unsigned char magic[24] = { 0 };
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t n = fread(magic, 1, sizeof magic, f);
	fclose(f);
	if (n >= 24 && magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') {
		/* 先看 IHDR 的寬高（第 16～23 位元組），太大的不解 */
		unsigned long long pw = ((unsigned)magic[16] << 24) | ((unsigned)magic[17] << 16) | ((unsigned)magic[18] << 8) | magic[19];
		unsigned long long ph = ((unsigned)magic[20] << 24) | ((unsigned)magic[21] << 16) | ((unsigned)magic[22] << 8) | magic[23];
		if (!pw || !ph || pw > PM_IMAGE_MAX_DIM || ph > PM_IMAGE_MAX_DIM || pw * ph > PM_IMAGE_MAX_PIXELS) return NULL;
		cairo_surface_t *s = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) return s;
		cairo_surface_destroy(s);
		return NULL;
	}
	if (n >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
		return load_jpeg(path, maxdim);
	return NULL;
}

cairo_surface_t *pm_image_load(const char *path) { return load_any(path, 0); }

/* 載入並縮成 w×h（解 JPEG 時直接用較小的解析度，省時間也省記憶體） */
cairo_surface_t *pm_image_load_fit(const char *path, int w, int h, int mode)
{
	cairo_surface_t *src = load_any(path, MIN(w, h));
	if (!src) return NULL;
	cairo_surface_t *out = pm_image_fit(src, w, h, mode);
	cairo_surface_destroy(src);
	return out;
}

/* 桌布：面板（與 pomelo-wm）每次登入都要把桌布從原圖解壓再縮成螢幕大小——4K 的 PNG 要一兩百毫秒，
 * 是登入時桌面晚出來的大宗。縮好的結果存成原始像素（~/.cache/pomelo/wallpaper/寬x高-模式.raw），
 * 檔頭記著原圖的路徑、修改時間、大小，任何一個不同就重做（0.40）。 */
#define WP_MAGIC "POMELOWP1"
static void wp_cache_path(char *out, size_t n, int w, int h, int mode)
{
	const char *c = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	if (c && *c) snprintf(out, n, "%s/pomelo/wallpaper/%dx%d-%d.raw", c, w, h, mode);
	else snprintf(out, n, "%s/.cache/pomelo/wallpaper/%dx%d-%d.raw", home ? home : "/tmp", w, h, mode);
}

static void mkdirs(const char *file)
{
	char d[PATH_MAX];
	snprintf(d, sizeof d, "%s", file);
	for (char *p = d + 1; *p; p++)
		if (*p == '/') { *p = 0; mkdir(d, 0700); *p = '/'; }
}

cairo_surface_t *pm_wallpaper_load(const char *path, int w, int h, int mode)
{
	struct stat st;
	if (!path || stat(path, &st) || w <= 0 || h <= 0) return NULL;
	char key[PATH_MAX + 64], cache[PATH_MAX];
	int kl = snprintf(key, sizeof key, "%s\n%lld\n%lld\n", path, (long long)st.st_mtime, (long long)st.st_size);
	wp_cache_path(cache, sizeof cache, w, h, mode);
	FILE *f = fopen(cache, "rb");
	if (f) {
		char magic[sizeof WP_MAGIC], k2[sizeof key];
		int hdr[3];   /* 寬、高、key 長度 */
		cairo_surface_t *s = NULL;
		if (fread(magic, 1, sizeof magic, f) == sizeof magic && !memcmp(magic, WP_MAGIC, sizeof magic) &&
		    fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == w && hdr[1] == h && hdr[2] == kl &&
		    fread(k2, 1, (size_t)kl, f) == (size_t)kl && !memcmp(k2, key, (size_t)kl)) {
			s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
			unsigned char *d = cairo_image_surface_get_data(s);
			int stride = cairo_image_surface_get_stride(s), ok = 1;
			cairo_surface_flush(s);
			for (int y = 0; y < h && ok; y++) ok = fread(d + (size_t)y * stride, 4, (size_t)w, f) == (size_t)w;
			cairo_surface_mark_dirty(s);
			if (!ok) { cairo_surface_destroy(s); s = NULL; }
		}
		fclose(f);
		if (s) return s;
	}
	cairo_surface_t *s = pm_image_load_fit(path, w, h, mode);
	if (!s) return NULL;
	/* 寫到暫存檔再改名：同時有兩個程式在寫（面板、登入畫面）也不會讀到寫一半的 */
	mkdirs(cache);
	char tmp[PATH_MAX + 16];
	snprintf(tmp, sizeof tmp, "%s.%d", cache, (int)getpid());
	f = fopen(tmp, "wb");
	if (f) {
		int hdr[3] = { w, h, kl }, ok = 1;
		cairo_surface_flush(s);
		const unsigned char *d = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s);
		ok = fwrite(WP_MAGIC, 1, sizeof WP_MAGIC, f) == sizeof WP_MAGIC && fwrite(hdr, sizeof hdr, 1, f) == 1 &&
		     fwrite(key, 1, (size_t)kl, f) == (size_t)kl;
		for (int y = 0; y < h && ok; y++) ok = fwrite(d + (size_t)y * stride, 4, (size_t)w, f) == (size_t)w;
		if (fclose(f) || !ok || rename(tmp, cache)) unlink(tmp);
	}
	return s;
}

/* 縮圖快取（0.41 效能）：~/.cache/pomelo/wpthumb/<雜湊>.raw。外觀、登入畫面、鎖定畫面等「一排桌布預覽」的頁用，
 * 原本每次開頁都要把 /usr/share/backgrounds 的大圖全部解碼（24 張約 1～2 秒）。
 * 鍵 = 路徑＋修改時間＋大小＋縮圖尺寸與模式，檔頭整段比對，任何一個不同就當作沒有；檔案長度不對（寫一半、損壞）
 * 就刪掉重做。寫入先寫暫存檔再 rename，多個程式同時寫也安全；目錄不能寫就照舊直接解碼。
 * 上限：超過 2000 個檔或 200 MB 時，清掉最舊的（修改時間）到剩 75%。 */
#define TH_MAGIC "POMELOTH1"
#define TH_MAX_FILES 2000
#define TH_MAX_BYTES (200LL << 20)

static int thumb_path(const char *path, int w, int h, int mode, char *key, size_t kn, char *out, size_t n)
{
	struct stat st;
	if (!path || w <= 0 || h <= 0 || w > 4096 || h > 4096 || stat(path, &st)) return 0;
	int kl = snprintf(key, kn, "%s\n%lld\n%lld\n%dx%d-%d\n", path, (long long)st.st_mtime, (long long)st.st_size, w, h, mode);
	if (kl <= 0 || (size_t)kl >= kn) return 0;
	unsigned long long hv = 1469598103934665603ULL;
	for (const char *c = key; *c; c++) hv = (hv ^ (unsigned char)*c) * 1099511628211ULL;
	const char *cd = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
	if (cd && *cd) snprintf(out, n, "%s/pomelo/wpthumb/%016llx.raw", cd, hv);
	else snprintf(out, n, "%s/.cache/pomelo/wpthumb/%016llx.raw", home ? home : "/tmp", hv);
	return kl;
}

cairo_surface_t *pm_thumb_cached(const char *path, int w, int h, int mode)
{
	char key[PATH_MAX + 80], cp[PATH_MAX + 64];
	int kl = thumb_path(path, w, h, mode, key, sizeof key, cp, sizeof cp);
	if (!kl) return NULL;
	FILE *f = fopen(cp, "rb");
	if (!f) return NULL;
	struct stat st;
	int hdr[3], bad = 0;
	char magic[sizeof TH_MAGIC], k2[sizeof key];
	cairo_surface_t *s = NULL;
	long long want = (long long)sizeof TH_MAGIC + sizeof hdr + kl + (long long)w * h * 4;
	if (fstat(fileno(f), &st) || st.st_size != want) bad = 1;   /* 寫一半或損壞：刪掉重做 */
	else if (fread(magic, 1, sizeof magic, f) != sizeof magic || memcmp(magic, TH_MAGIC, sizeof magic) ||
	         fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != w || hdr[1] != h || hdr[2] != kl ||
	         fread(k2, 1, (size_t)kl, f) != (size_t)kl) bad = 1;
	else if (memcmp(k2, key, (size_t)kl)) bad = 0;   /* 雜湊撞到別張：不是損壞，只是不是這張 */
	else {
		s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		unsigned char *d = cairo_image_surface_get_data(s);
		int stride = cairo_image_surface_get_stride(s), ok = cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
		if (ok) cairo_surface_flush(s);
		for (int y = 0; y < h && ok; y++) ok = fread(d + (size_t)y * stride, 4, (size_t)w, f) == (size_t)w;
		if (ok) cairo_surface_mark_dirty(s);
		else { cairo_surface_destroy(s); s = NULL; bad = 1; }
	}
	fclose(f);
	if (bad) unlink(cp);
	return s;
}

struct tfile { time_t t; off_t sz; char name[64]; };
static int thumb_cmp(const void *a, const void *b)
{
	time_t x = ((const struct tfile *)a)->t, y = ((const struct tfile *)b)->t;
	return x < y ? -1 : x > y;
}

/* 清舊檔：只看 *.raw 與遺留的暫存檔 */
static void thumb_prune(const char *dir)
{
	DIR *dp = opendir(dir);
	if (!dp) return;
	struct tfile *v = NULL;
	size_t n = 0, cap = 0;
	long long total = 0;
	time_t now = time(NULL);
	struct dirent *e;
	while ((e = readdir(dp))) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= 64) continue;
		char p[PATH_MAX + 80];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
		const char *x = strstr(e->d_name, ".raw");
		if (!x) continue;
		if (x[4] && now - st.st_mtime > 3600) { unlink(p); continue; }   /* 當機留下的暫存檔 */
		if (x[4]) continue;
		if (n == cap) {
			struct tfile *nv = realloc(v, (cap = cap ? cap * 2 : 256) * sizeof *v);
			if (!nv) break;
			v = nv;
		}
		v[n].t = st.st_mtime; v[n].sz = st.st_size;
		snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
		total += st.st_size;
		n++;
	}
	closedir(dp);
	if (n > TH_MAX_FILES || total > TH_MAX_BYTES) {
		size_t keep = n;
		long long left = total;
		qsort(v, n, sizeof *v, thumb_cmp);
		for (size_t i = 0; i < n && (keep > TH_MAX_FILES * 3 / 4 || left > TH_MAX_BYTES * 3 / 4); i++) {
			char p[PATH_MAX + 80];
			snprintf(p, sizeof p, "%s/%s", dir, v[i].name);
			unlink(p);
			keep--; left -= v[i].sz;
		}
	}
	free(v);
}

static void thumb_store(const char *path, cairo_surface_t *s, int w, int h, int mode)
{
	char key[PATH_MAX + 80], cp[PATH_MAX + 64], tmp[PATH_MAX + 96];
	int kl = thumb_path(path, w, h, mode, key, sizeof key, cp, sizeof cp);
	if (!kl || cairo_image_surface_get_width(s) != w || cairo_image_surface_get_height(s) != h) return;
	mkdirs(cp);
	snprintf(tmp, sizeof tmp, "%s.%d", cp, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) return;   /* 目錄不能寫：沒有快取，照舊 */
	int hdr[3] = { w, h, kl };
	cairo_surface_flush(s);
	const unsigned char *d = cairo_image_surface_get_data(s);
	int stride = cairo_image_surface_get_stride(s);
	int ok = fwrite(TH_MAGIC, 1, sizeof TH_MAGIC, f) == sizeof TH_MAGIC && fwrite(hdr, sizeof hdr, 1, f) == 1 &&
	         fwrite(key, 1, (size_t)kl, f) == (size_t)kl;
	for (int y = 0; y < h && ok; y++) ok = fwrite(d + (size_t)y * stride, 4, (size_t)w, f) == (size_t)w;
	if (fclose(f) || !ok || rename(tmp, cp)) { unlink(tmp); return; }
	char dir[PATH_MAX + 64];
	snprintf(dir, sizeof dir, "%s", cp);
	*strrchr(dir, '/') = 0;
	thumb_prune(dir);
}

cairo_surface_t *pm_thumb_load(const char *path, int w, int h, int mode)
{
	cairo_surface_t *s = pm_thumb_cached(path, w, h, mode);
	if (s) return s;
	s = pm_image_load_fit(path, w, h, mode);
	if (s && cairo_surface_status(s) == CAIRO_STATUS_SUCCESS) thumb_store(path, s, w, h, mode);
	return s;
}

cairo_surface_t *pm_image_fit(cairo_surface_t *src, int w, int h, int mode)
{
	int iw = cairo_image_surface_get_width(src), ih = cairo_image_surface_get_height(src);
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	if (iw <= 0 || ih <= 0 || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS || cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) return out;
	cairo_t *cr = cairo_create(out);
	double sx = (double)w / iw, sy = (double)h / ih;
	double k = mode == 0 ? MAX(sx, sy) : MIN(sx, sy);
	if (mode == 2) {
		cairo_scale(cr, sx, sy);
	} else {
		cairo_translate(cr, (w - iw * k) / 2, (h - ih * k) / 2);
		cairo_scale(cr, k, k);
	}
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return out;
}

/* 一個螢幕（w×h）的桌布：設定的圖片（照顯示方式縮放），沒設或讀不到就是 Pomelo 的漸層。
 * 桌布視窗、啟動器的模糊背景都用這個，畫起來才會一樣。設定成不要桌布（none）回傳 NULL */
cairo_surface_t *pm_desktop_image(int w, int h)
{
	const char *wp = pm_conf_get("desktop.wallpaper", "gradient");
	if (!strcmp(wp, "none") || w <= 0 || h <= 0) return NULL;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	cairo_t *cr = cairo_create(s);
	pm_rgb(cr, 0x000000);
	cairo_paint(cr);
	cairo_surface_t *img = strcmp(wp, "gradient") ? pm_wallpaper_load(wp, w, h, pm_conf_int("desktop.wallpaper_mode", 0)) : NULL;
	if (img) {
		cairo_set_source_surface(cr, img, (w - cairo_image_surface_get_width(img)) / 2, (h - cairo_image_surface_get_height(img)) / 2);
		cairo_paint(cr);
		cairo_surface_destroy(img);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, w, h);
		if (pm_dark) {
			cairo_pattern_add_color_stop_rgb(g, 0.0, 0.07, 0.09, 0.20);
			cairo_pattern_add_color_stop_rgb(g, 0.5, 0.20, 0.13, 0.32);
			cairo_pattern_add_color_stop_rgb(g, 1.0, 0.42, 0.20, 0.26);
		} else {
			cairo_pattern_add_color_stop_rgb(g, 0.0, 0.20, 0.36, 0.66);
			cairo_pattern_add_color_stop_rgb(g, 0.5, 0.52, 0.42, 0.74);
			cairo_pattern_add_color_stop_rgb(g, 1.0, 0.98, 0.62, 0.48);
		}
		cairo_set_source(cr, g);
		cairo_paint(cr);
		cairo_pattern_destroy(g);
		/* 柔和的光暈 */
		cairo_pattern_t *r = cairo_pattern_create_radial(w * 0.7, h * 0.35, 0, w * 0.7, h * 0.35, w * 0.55);
		cairo_pattern_add_color_stop_rgba(r, 0, 1, 1, 1, pm_dark ? 0.10 : 0.22);
		cairo_pattern_add_color_stop_rgba(r, 1, 1, 1, 1, 0);
		cairo_set_source(cr, r);
		cairo_paint(cr);
		cairo_pattern_destroy(r);
	}
	cairo_destroy(cr);
	cairo_surface_flush(s);
	return s;
}
