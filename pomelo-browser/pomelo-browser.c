/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-browser.c — 小柚瀏覽器：Pomelo 的輕量網頁瀏覽器
 *
 * 網頁引擎用系統的 WebKitGTK 6.0（排版、JavaScript、影音、安全更新都由發行版負責），這裡只寫外殼：
 * 分頁、網址列、上一頁／下一頁、尋找、縮放、下載、權限詢問、開始頁、錯誤頁。
 * 工具列按鈕、分頁列、選單用 libpomelo 的 cairo 繪圖函式自己畫（跟其他 Pomelo 程式同樣的顏色、字型）；
 * 網址列、尋找欄用 GTK 的文字框（中文輸入、選取、剪貼簿都現成），用 CSS 套上 Pomelo 的樣子。
 *
 * 省資源、開得快：同一個使用者只跑一個行程（GtkApplication），再開一次只是叫出視窗或多開分頁；
 * WebKit 用較小的快取模式；只跑 X11（跟其他 Pomelo 程式一樣經由 pomelo-compositor 的 XWayland）。
 * 輸入法：GTK4 沒有 XIM，用 Pomelo 輸入法時改用 IBus 模組（pomelo-input 有 IBus 相容的介面）。
 */
#include <gtk/gtk.h>
#include <gdk/x11/gdkx.h>
#include <gdk/wayland/gdkwayland.h>
#include <webkit/webkit.h>
#undef MAX
#undef MIN
#undef CLAMP
#include "pomelo.h"
#include "../pomelo-assistant/robot.h"

#define APP_ID   "tw.fedora.PomeloBrowser"
#define START    "pomelo://start"
#define TOOL_H   44
#define TAB_H    34
#define MAXTAB   64
#define NCLOSED  20

typedef struct Win Win;

typedef struct {
	Win *w;
	WebKitWebView *view;
	cairo_surface_t *icon;            /* 網站圖示 */
	WebKitPermissionRequest *ask;     /* 等使用者回答的權限詢問 */
	char ask_text[512];
	GTlsCertificate *tls_cert;        /* 憑證有問題的網站：使用者按「仍要前往」時用 */
	char tls_host[256], tls_uri[2048], token[40];
	char upgraded[2048];               /* 網址列打「example.com」時自動補成的 https 網址：連不上就改用 http 再試 */
} Tab;

struct Win {
	GtkWindow *win;
	GtkWidget *tools, *nav, *entry, *right, *tabbar, *findbar, *findentry, *findtools;
	GtkWidget *askbar, *asklabel, *askno, *askyes, *overlay, *stack, *progress, *status;
	GtkWidget *menu, *menuarea;
	GtkWidget *certpop;                /* 點網址列鎖頭出現的憑證資訊 */
	Tab *tabs[MAXTAB];
	int n, cur;
	int hover_nav, hover_right, hover_tab, hover_close, hover_find, hover_menu;
	int editing;                      /* 網址列正在打字：網頁網址變了也不要蓋掉 */
	int setting;                      /* 程式自己在改網址列的字（不算使用者打字） */
	int fullscreen;
	int confirm_clear;                /* 詢問列正在問「要清除瀏覽資料嗎」 */
	unsigned find_count;
};

static GtkApplication *app;
static WebKitNetworkSession *session;
static WebKitWebContext *ctx;
static WebKitSettings *wsettings;
static GList *wins;
static char closed[NCLOSED][2048];
static int nclosed;
static int engine;
static GPtrArray *downloads;
static char *robot_uri;
static GdkTexture *tex_lock, *tex_warn;
static unsigned col_bar, col_tabbar, col_tab, col_field, col_text, col_dim, col_sep, col_accent, col_hover, col_menu;

static const struct { const char *name, *query, *action; } engines[] = {
	{ "Google", "https://www.google.com/search?q=%s", "https://www.google.com/search" },
	{ "DuckDuckGo", "https://duckduckgo.com/?q=%s", "https://duckduckgo.com/" },
	{ "Bing", "https://www.bing.com/search?q=%s", "https://www.bing.com/search" },
};

static Tab *cur_tab(Win *w) { return w->n ? w->tabs[w->cur] : NULL; }

/* 有沒有真正的顯示卡（Intel、AMD、NVIDIA 的驅動程式）。沒有的話（虛擬機、只有軟體 OpenGL 的電腦）
 * 介面與網頁都改用 CPU 畫：軟體 OpenGL（llvmpipe）每個行程要多佔一百多 MB，也不會比較快 */
static int have_gpu(void)
{
	static const char *drivers[] = { "i915", "xe", "amdgpu", "radeon", "nouveau", "nvidia" };
	char path[64], link[PATH_MAX];
	for (int i = 0; i < 8; i++) {
		snprintf(path, sizeof path, "/sys/class/drm/card%d/device/driver", i);
		ssize_t n = readlink(path, link, sizeof link - 1);
		if (n <= 0) continue;
		link[n] = 0;
		const char *base = strrchr(link, '/');
		base = base ? base + 1 : link;
		for (unsigned k = 0; k < LEN(drivers); k++) if (!strcmp(base, drivers[k])) return 1;
	}
	return 0;
}
static int gpu;

/* Ubuntu 24.04 起 AppArmor 不讓一般程式建立使用者命名空間，WebKit 的網頁沙箱（bwrap）起不來，
 * 第一個網頁行程啟動時整個程式就結束。套件附 /etc/apparmor.d/pomelo-browser 允許（跟 Ubuntu 的 Epiphany 一樣）；
 * 那份設定沒有載入時（程式裡看到自己是 unconfined），先說清楚，不要一聲不響就關掉。 */
static int sandbox_blocked(void)
{
	if (getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) return 0;
	FILE *f = fopen("/proc/sys/kernel/apparmor_restrict_unprivileged_userns", "r");
	if (!f) return 0;
	int on = fgetc(f) == '1';
	fclose(f);
	if (!on) return 0;
	char label[64] = "";
	f = fopen("/proc/self/attr/apparmor/current", "r");
	if (!f) f = fopen("/proc/self/attr/current", "r");
	if (!f) return 0;
	if (!fgets(label, sizeof label, f)) label[0] = 0;
	fclose(f);
	return !strncmp(label, "unconfined", 10);
}

static int sandbox_explain(void)
{
	static const char cmd[] = "sudo apparmor_parser -r /etc/apparmor.d/pomelo-browser";
	fprintf(stderr, "pomelo-browser: %s\n  %s\n", _("這台電腦限制程式使用網頁沙箱（AppArmor），小柚瀏覽器的 AppArmor 設定沒有載入。"
	        "請重新安裝 pomelo-browser 套件，或在終端機執行下面的指令，再開一次小柚瀏覽器："), cmd);
	gtk_init();
	GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", _("小柚瀏覽器打不開"));
	gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
		_("這台電腦限制程式使用網頁沙箱（AppArmor），小柚瀏覽器的 AppArmor 設定沒有載入。"
		  "請重新安裝 pomelo-browser 套件，或在終端機執行下面的指令，再開一次小柚瀏覽器："));
	GtkWidget *l = gtk_label_new(cmd);
	gtk_label_set_selectable(GTK_LABEL(l), TRUE);
	gtk_widget_add_css_class(l, "monospace");
	gtk_box_append(GTK_BOX(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(d))), l);
	gtk_window_set_title(GTK_WINDOW(d), _("小柚瀏覽器"));
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	g_signal_connect_swapped(d, "response", G_CALLBACK(g_main_loop_quit), loop);
	GtkWidget *close = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE);
	if (close) gtk_window_set_focus(GTK_WINDOW(d), close);   /* 不然焦點在指令那行，一開始就有文字游標 */
	gtk_window_present(GTK_WINDOW(d));
	g_main_loop_run(loop);
	return 1;
}

static void tab_select(Win *w, int i);
static Tab *tab_new(Win *w, const char *uri, int front);
static void tab_close(Win *w, int i);
/* 原生 Wayland 時 GTK 拿 GtkApplication 的 ID 當 app_id；Dock 要用 pomelo-browser 才對得到 .desktop（X11 的 WM_CLASS 也是這個） */
static void set_app_id(GtkWidget *win, gpointer unused)
{
	(void)unused;
#if GTK_CHECK_VERSION(4, 10, 0)
	GdkSurface *sf = gtk_native_get_surface(GTK_NATIVE(win));
	if (GDK_IS_WAYLAND_TOPLEVEL(sf)) gdk_wayland_toplevel_set_application_id(GDK_WAYLAND_TOPLEVEL(sf), "pomelo-browser");
#else
	(void)win;
#endif
}

static Win *win_new(void);
static void update_chrome(Win *w);
static void show_find(Win *w, int on);

/* ---------- 小工具 ---------- */

static void colors(void)
{
	col_bar = T->bar;
	col_text = T->bar_text;
	col_dim = T->dim;
	col_accent = T->sel;
	col_tabbar = pm_dark ? 0x222124 : 0xe4e4e8;
	col_tab = pm_dark ? 0x3a393d : 0xffffff;
	col_field = pm_dark ? 0x3a393d : 0xe3e3e8;
	col_sep = pm_dark ? 0x161616 : 0xd1d1d6;
	col_hover = pm_dark ? 0x403f44 : 0xdcdce1;
	col_menu = T->menu;
}

static void notify(const char *title, const char *body)
{
	if (!pm_have("notify-send")) return;
	char *argv[] = { "notify-send", "-a", (char *)_("小柚瀏覽器"), "-i", "pomelo-browser", (char *)title, (char *)body, NULL };
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void open_external(const char *uri)
{
	g_app_info_launch_default_for_uri(uri, NULL, NULL);
}

static const char *download_dir(void)
{
	const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
	static char fallback[PATH_MAX];
	if (d) return d;
	snprintf(fallback, sizeof fallback, "%s/Downloads", g_get_home_dir());
	return fallback;
}

/* HTML 跳脫（標題、網址放進開始頁、錯誤頁時用）；呼叫者 g_free */
static char *esc(const char *s)
{
	return g_markup_escape_text(s ? s : "", -1);
}

static GdkTexture *texture_from(cairo_surface_t *s)
{
	cairo_surface_flush(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int stride = cairo_image_surface_get_stride(s);
	GBytes *b = g_bytes_new(cairo_image_surface_get_data(s), (gsize)stride * h);
	GdkTexture *t = gdk_memory_texture_new(w, h, GDK_MEMORY_DEFAULT, b, stride);
	g_bytes_unref(b);
	return t;
}

static cairo_surface_t *surface_from(GdkTexture *t)
{
	int w = gdk_texture_get_width(t), h = gdk_texture_get_height(t);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_surface_flush(s);
	gdk_texture_download(t, cairo_image_surface_get_data(s), cairo_image_surface_get_stride(s));
	cairo_surface_mark_dirty(s);
	return s;
}

/* 網址列打的字 → 要開的網址：看起來像網址就直接開（沒寫 https:// 自動補上），不然交給搜尋引擎 */
static char *to_uri(const char *in)
{
	while (*in == ' ') in++;
	char *t = g_strstrip(g_strdup(in));
	if (!*t) { g_free(t); return NULL; }
	char *r = NULL;
	if (strstr(t, "://") || !strncmp(t, "about:", 6) || !strncmp(t, "data:", 5) || !strncmp(t, "mailto:", 7))
		r = g_strdup(t);
	else if (t[0] == '/' || !strncmp(t, "~/", 2)) {
		char *p = t[0] == '~' ? g_build_filename(g_get_home_dir(), t + 2, NULL) : g_strdup(t);
		r = g_filename_to_uri(p, NULL, NULL);
		g_free(p);
	} else if (!strchr(t, ' ')) {
		const char *host_end = t + strcspn(t, "/?#");
		char host[256];
		snprintf(host, sizeof host, "%.*s", (int)(host_end - t), t);
		char *colon = strrchr(host, ':');
		int port = colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1);
		if (port) *colon = 0;
		char *dot = strrchr(host, '.');
		int tld = dot && dot[1] && isalpha((unsigned char)dot[1]) && strlen(dot + 1) >= 2;
		int ip = host[0] && strspn(host, "0123456789.") == strlen(host) && strchr(host, '.');
		if (!strcmp(host, "localhost") || ip)
			r = g_strdup_printf("http://%s", t);
		else if (tld || port)
			r = g_strdup_printf("https://%s", t);
	}
	if (!r) {
		char *q = g_uri_escape_string(t, NULL, TRUE);
		r = g_strdup_printf(engines[engine].query, q);
		g_free(q);
	}
	g_free(t);
	return r;
}

/* ---------- 瀏覽紀錄（開始頁的「常去的網站」） ---------- */

typedef struct { char *url, *title; int count; gint64 last; } Hist;
static GHashTable *hist;
static guint hist_save_id;

static char *hist_path(void)
{
	return g_build_filename(g_get_user_data_dir(), "pomelo-browser", "history.tsv", NULL);
}

static void hist_free(gpointer p)
{
	Hist *h = p;
	g_free(h->url);
	g_free(h->title);
	g_free(h);
}

static void hist_load(void)
{
	hist = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, hist_free);
	char *path = hist_path(), *data = NULL;
	if (g_file_get_contents(path, &data, NULL, NULL)) {
		for (char *line = strtok(data, "\n"); line; line = strtok(NULL, "\n")) {
			char **f = g_strsplit(line, "\t", 4);
			if (g_strv_length(f) == 4) {
				Hist *h = g_new0(Hist, 1);
				h->count = atoi(f[0]);
				h->last = g_ascii_strtoll(f[1], NULL, 10);
				h->url = g_strdup(f[2]);
				h->title = g_strdup(f[3]);
				g_hash_table_replace(hist, h->url, h);
			}
			g_strfreev(f);
		}
	}
	g_free(data);
	g_free(path);
}

static int hist_by_last(const void *a, const void *b)
{
	const Hist *x = *(Hist *const *)a, *y = *(Hist *const *)b;
	return x->last < y->last ? 1 : x->last > y->last ? -1 : 0;
}

static gboolean hist_save(gpointer unused)
{
	hist_save_id = 0;
	GPtrArray *all = g_ptr_array_new();   /* （g_hash_table_get_values_as_ptr_array 要 GLib 2.76，Ubuntu 22.04 沒有） */
	GList *vals = g_hash_table_get_values(hist);
	for (GList *l = vals; l; l = l->next) g_ptr_array_add(all, l->data);
	g_list_free(vals);
	g_ptr_array_sort(all, hist_by_last);
	GString *out = g_string_new(NULL);
	for (guint i = 0; i < all->len && i < 3000; i++) {   /* 只留最近的 3000 筆 */
		Hist *h = all->pdata[i];
		g_string_append_printf(out, "%d\t%" G_GINT64_FORMAT "\t%s\t%s\n", h->count, h->last, h->url, h->title);
	}
	char *path = hist_path(), *dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_file_set_contents(path, out->str, out->len, NULL);
	g_free(dir);
	g_free(path);
	g_string_free(out, TRUE);
	g_ptr_array_unref(all);
	return G_SOURCE_REMOVE;
}

static void hist_add(const char *url, const char *title)
{
	if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) return;
	Hist *h = g_hash_table_lookup(hist, url);
	if (!h) {
		h = g_new0(Hist, 1);
		h->url = g_strdup(url);
		h->title = g_strdup("");
		g_hash_table_replace(hist, h->url, h);
	}
	h->count++;
	h->last = g_get_real_time() / 1000000;
	if (title && *title) {
		g_free(h->title);
		h->title = g_strdelimit(g_strdup(title), "\t\n", ' ');
	}
	if (!hist_save_id) hist_save_id = g_timeout_add_seconds(3, hist_save, NULL);
}

/* 常去的網站：同一個網站（scheme＋主機）的造訪次數加總，取前 n 個 */
typedef struct { char origin[300], host[256]; int count; } Site;
static int site_cmp(const void *a, const void *b) { return ((const Site *)b)->count - ((const Site *)a)->count; }

static int top_sites(Site *out, int max)
{
	GHashTable *by = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, hist);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Hist *h = v;
		GUri *u = g_uri_parse(h->url, G_URI_FLAGS_NONE, NULL);
		if (!u) continue;
		if (g_uri_get_host(u)) {
			char *origin = g_strdup_printf("%s://%s/", g_uri_get_scheme(u), g_uri_get_host(u));
			Site *s = g_hash_table_lookup(by, origin);
			if (!s) {
				s = g_new0(Site, 1);
				snprintf(s->origin, sizeof s->origin, "%s", origin);
				const char *host = g_uri_get_host(u);
				snprintf(s->host, sizeof s->host, "%s", strncmp(host, "www.", 4) ? host : host + 4);
				g_hash_table_insert(by, g_strdup(origin), s);
			}
			s->count += h->count;
			g_free(origin);
		}
		g_uri_unref(u);
	}
	int n = 0, cap = g_hash_table_size(by);
	Site *all = g_new0(Site, cap ? cap : 1);
	g_hash_table_iter_init(&it, by);
	while (g_hash_table_iter_next(&it, &k, &v)) all[n++] = *(Site *)v;
	qsort(all, n, sizeof *all, site_cmp);
	if (n > max) n = max;
	memcpy(out, all, n * sizeof *out);
	g_free(all);
	g_hash_table_destroy(by);
	return n;
}

/* ---------- 開始頁、錯誤頁（內建的 pomelo:// 網頁） ---------- */

static cairo_status_t png_write(void *closure, const unsigned char *data, unsigned int len)
{
	g_byte_array_append(closure, data, len);
	return CAIRO_STATUS_SUCCESS;
}

static const char *robot_png(void)
{
	if (robot_uri) return robot_uri;
	const double scale = 150;
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, ROBOT_W(scale), ROBOT_H(scale));
	RobotPose p = { 0 };
	p.eye_open = 1;
	p.glow = 1;
	p.expr = 1;
	p.arm_r = 0.9;
	p.head_roll = -0.08;
	p.accent = col_accent;
	robot_skin(0, pm_dark, &p.skin);
	robot_render(s, scale, &p);
	GByteArray *png = g_byte_array_new();
	cairo_surface_write_to_png_stream(s, png_write, png);
	cairo_surface_destroy(s);
	char *b64 = g_base64_encode(png->data, png->len);
	robot_uri = g_strdup_printf("data:image/png;base64,%s", b64);
	g_free(b64);
	g_byte_array_unref(png);
	return robot_uri;
}

/* 所有內建網頁共用的樣式（跟著系統的淺色／深色） */
static void page_head(GString *h, const char *title)
{
	char *t = esc(title);
	g_string_append_printf(h,
		"<!doctype html><html lang=\"%s\"><head><meta charset=\"utf-8\"><title>%s</title><style>"
		":root{color-scheme:light dark;--bg:#f2f2f7;--card:#fff;--text:#1d1d1f;--dim:#8e8e93;--accent:#%06x}"
		"@media (prefers-color-scheme:dark){:root{--bg:#1c1c1e;--card:#2c2c2e;--text:#f2f2f2;--dim:#98989d}}"
		"html,body{margin:0;min-height:100vh;background:var(--bg);color:var(--text);"
		"font-family:\"%s\",\"Noto Sans CJK TC\",sans-serif}"
		"body{display:flex;flex-direction:column;align-items:center;justify-content:center;box-sizing:border-box;padding:24px}"
		"img.robot{width:130px;height:auto;margin-bottom:4px}"
		"h1{font-size:22px;font-weight:600;margin:6px 0 18px;text-align:center}"
		"p{color:var(--dim);max-width:560px;text-align:center;line-height:1.6;margin:4px 0}"
		"form{width:min(600px,86vw)}"
		"input{width:100%%;box-sizing:border-box;font:inherit;font-size:17px;padding:14px 22px;border-radius:26px;"
		"border:1px solid transparent;background:var(--card);color:var(--text);box-shadow:0 2px 14px rgba(0,0,0,.08);outline:none}"
		"input:focus{border-color:var(--accent)}"
		".sites{display:flex;flex-wrap:wrap;justify-content:center;max-width:454px;gap:14px 18px;margin-top:34px}"   /* 一排最多 4 個、置中 */
		".site{width:100px;display:flex;flex-direction:column;align-items:center;text-decoration:none;color:var(--text);font-size:12px}"
		".site .l{width:50px;height:50px;border-radius:15px;display:flex;align-items:center;justify-content:center;"
		"color:#fff;font-size:22px;font-weight:600;margin-bottom:7px;box-shadow:0 2px 8px rgba(0,0,0,.12)}"
		".site .n{max-width:100px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
		".btn{display:inline-block;margin-top:22px;padding:9px 22px;border-radius:9px;background:var(--accent);color:#fff;"
		"text-decoration:none;font-size:14px;border:none;font:inherit;cursor:pointer}"
		".small{margin-top:26px;font-size:12px}.small a{color:var(--dim)}"
		"code{font-size:12px;color:var(--dim);word-break:break-all}"
		"</style></head><body>",
		pm_lang(), t, col_accent, pm_family);
	g_free(t);
}

static char *start_page(void)
{
	GString *h = g_string_new(NULL);
	page_head(h, _("新分頁"));
	g_string_append_printf(h, "<img class=\"robot\" src=\"%s\" alt=\"\"><h1>%s</h1>", robot_png(), _("今天想去哪裡？"));
	char *ph = esc(_("搜尋或輸入網址"));
	/* 搜尋框：打網址就直接開（送出前檢查），其他交給搜尋引擎 */
	g_string_append_printf(h,
		"<form action=\"%s\" method=\"get\" onsubmit=\"var v=this.q.value.trim();"
		"if(/^[^\\s\\/]+\\.[a-z]{2,}(\\/\\S*)?$/i.test(v)&&!/\\s/.test(v)){location.href='https://'+v;return false}\">"
		"<input name=\"q\" placeholder=\"%s\" autocomplete=\"off\"></form>",
		engines[engine].action, ph);
	g_free(ph);
	Site sites[8];
	int n = top_sites(sites, 8);
	static const unsigned palette[] = { 0x0a84ff, 0x30b158, 0xff9f0a, 0xff453a, 0xbf5af2, 0x64d2ff, 0xff375f, 0x8e8e93 };
	if (n) g_string_append(h, "<div class=\"sites\">");
	for (int i = 0; i < n; i++) {
		char *o = esc(sites[i].origin), *name = esc(sites[i].host);
		gunichar c = g_unichar_toupper(g_utf8_get_char(sites[i].host));
		char letter[8] = { 0 };
		g_unichar_to_utf8(c, letter);
		g_string_append_printf(h, "<a class=\"site\" href=\"%s\"><span class=\"l\" style=\"background:#%06x\">%s</span>"
		                       "<span class=\"n\">%s</span></a>", o, palette[g_str_hash(sites[i].host) % LEN(palette)], letter, name);
		g_free(o);
		g_free(name);
	}
	if (n) g_string_append(h, "</div>");
	g_string_append(h, "</body></html>");
	return g_string_free(h, FALSE);
}

static char *about_page(void)
{
	GString *h = g_string_new(NULL);
	page_head(h, _("關於小柚瀏覽器"));
	g_string_append_printf(h, "<img class=\"robot\" src=\"%s\" alt=\"\"><h1>%s</h1>"
		"<p>Pomelo %s</p><p>%s WebKitGTK %u.%u.%u</p><p>%s</p>"
		"<p class=\"small\">GPL-3.0-or-later　<a href=\"https://fedora.tw\">fedora.tw</a></p></body></html>",
		robot_png(), _("小柚瀏覽器"), POMELO_VERSION, _("網頁引擎："),
		webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(),
		_("輕巧、開得快的網頁瀏覽器，網頁引擎與安全性更新由系統的 WebKitGTK 提供。"));
	return g_string_free(h, FALSE);
}

static void scheme_cb(WebKitURISchemeRequest *req, gpointer unused)
{
	const char *uri = webkit_uri_scheme_request_get_uri(req);
	char *html = NULL;
	if (g_str_has_prefix(uri, "pomelo://start")) html = start_page();
	else if (g_str_has_prefix(uri, "pomelo://about")) html = about_page();
	if (!html) {
		GError *e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
		webkit_uri_scheme_request_finish_error(req, e);
		g_error_free(e);
		return;
	}
	gsize len = strlen(html);
	GInputStream *in = g_memory_input_stream_new_from_data(html, len, g_free);
	webkit_uri_scheme_request_finish(req, in, len, "text/html");
	g_object_unref(in);
}

/* 錯誤頁：大標題、說明、按鈕（重新載入或回上一頁），可以再加一行小字連結 */
static void show_error(Tab *t, const char *uri, const char *title, const char *msg, const char *button,
                       const char *button_js, const char *extra)
{
	GString *h = g_string_new(NULL);
	page_head(h, title);
	char *et = esc(title), *em = esc(msg), *eu = esc(uri), *eb = esc(button);
	g_string_append_printf(h, "<img class=\"robot\" src=\"%s\" alt=\"\" style=\"width:100px;opacity:.85\">"
		"<h1>%s</h1><p>%s</p><p><code>%s</code></p><button class=\"btn\" onclick=\"%s\">%s</button>%s</body></html>",
		robot_png(), et, em, eu, button_js, eb, extra ? extra : "");
	g_free(et);
	g_free(em);
	g_free(eu);
	g_free(eb);
	webkit_web_view_load_alternate_html(t->view, h->str, uri, NULL);
	g_string_free(h, TRUE);
}

/* ---------- 畫圖示（工具列、分頁、選單共用） ---------- */

enum { IC_BACK, IC_FWD, IC_RELOAD, IC_STOP, IC_PLUS, IC_MENU, IC_UP, IC_DOWN, IC_CLOSE, IC_LOCK, IC_WARN, IC_DOWNLOAD, IC_GLOBE };

static void draw_icon(cairo_t *cr, int which, double cx, double cy, unsigned color, double alpha)
{
	cairo_save(cr);
	cairo_new_path(cr);
	pm_rgba(cr, color, alpha);
	cairo_set_line_width(cr, 1.7);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (which) {
	case IC_BACK:
		cairo_move_to(cr, cx + 3, cy - 7); cairo_line_to(cr, cx - 4, cy); cairo_line_to(cr, cx + 3, cy + 7);
		cairo_stroke(cr);
		break;
	case IC_FWD:
		cairo_move_to(cr, cx - 3, cy - 7); cairo_line_to(cr, cx + 4, cy); cairo_line_to(cr, cx - 3, cy + 7);
		cairo_stroke(cr);
		break;
	case IC_RELOAD:
		cairo_arc(cr, cx, cy, 6.5, -M_PI * 0.35, M_PI * 1.45);
		cairo_stroke(cr);
		cairo_move_to(cr, cx + 6.5 * cos(-M_PI * 0.35) - 4.2, cy + 6.5 * sin(-M_PI * 0.35) - 1.2);
		cairo_line_to(cr, cx + 6.5 * cos(-M_PI * 0.35) + 0.6, cy + 6.5 * sin(-M_PI * 0.35) - 0.2);
		cairo_line_to(cr, cx + 6.5 * cos(-M_PI * 0.35) + 0.4, cy + 6.5 * sin(-M_PI * 0.35) - 5.0);
		cairo_stroke(cr);
		break;
	case IC_STOP:
	case IC_CLOSE: {
		double r = which == IC_STOP ? 6 : 4.5;
		cairo_move_to(cr, cx - r, cy - r); cairo_line_to(cr, cx + r, cy + r);
		cairo_move_to(cr, cx + r, cy - r); cairo_line_to(cr, cx - r, cy + r);
		cairo_stroke(cr);
		break;
	}
	case IC_PLUS:
		cairo_move_to(cr, cx - 7, cy); cairo_line_to(cr, cx + 7, cy);
		cairo_move_to(cr, cx, cy - 7); cairo_line_to(cr, cx, cy + 7);
		cairo_stroke(cr);
		break;
	case IC_MENU:
		for (int i = -1; i <= 1; i++) { cairo_arc(cr, cx + i * 6, cy, 1.8, 0, 2 * M_PI); cairo_fill(cr); }
		break;
	case IC_UP:
		cairo_move_to(cr, cx - 6, cy + 3); cairo_line_to(cr, cx, cy - 3); cairo_line_to(cr, cx + 6, cy + 3);
		cairo_stroke(cr);
		break;
	case IC_DOWN:
		cairo_move_to(cr, cx - 6, cy - 3); cairo_line_to(cr, cx, cy + 3); cairo_line_to(cr, cx + 6, cy - 3);
		cairo_stroke(cr);
		break;
	case IC_LOCK:
		cairo_set_line_width(cr, 1.5);
		cairo_arc(cr, cx, cy - 2.5, 3.2, M_PI, 2 * M_PI);
		cairo_line_to(cr, cx + 3.2, cy);
		cairo_move_to(cr, cx - 3.2, cy);
		cairo_line_to(cr, cx - 3.2, cy - 2.5);
		cairo_stroke(cr);
		pm_rrect(cr, cx - 5, cy - 0.5, 10, 7.5, 1.6, 1.6);
		cairo_fill(cr);
		break;
	case IC_WARN:
		cairo_set_line_width(cr, 1.5);
		cairo_move_to(cr, cx, cy - 6.5); cairo_line_to(cr, cx + 6.8, cy + 5.5); cairo_line_to(cr, cx - 6.8, cy + 5.5);
		cairo_close_path(cr);
		cairo_stroke(cr);
		cairo_move_to(cr, cx, cy - 2.2); cairo_line_to(cr, cx, cy + 1.6);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy + 3.7, 0.9, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case IC_DOWNLOAD:
		cairo_move_to(cr, cx, cy - 7); cairo_line_to(cr, cx, cy + 3);
		cairo_move_to(cr, cx - 4.5, cy - 1.5); cairo_line_to(cr, cx, cy + 3); cairo_line_to(cr, cx + 4.5, cy - 1.5);
		cairo_move_to(cr, cx - 7, cy + 7); cairo_line_to(cr, cx + 7, cy + 7);
		cairo_stroke(cr);
		break;
	case IC_GLOBE:
		cairo_set_line_width(cr, 1.3);
		cairo_arc(cr, cx, cy, 7, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_save(cr);
		cairo_translate(cr, cx, cy);
		cairo_scale(cr, 0.45, 1);
		cairo_arc(cr, 0, 0, 7, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 7, cy); cairo_line_to(cr, cx + 7, cy);
		cairo_stroke(cr);
		break;
	}
	cairo_restore(cr);
}

static GdkTexture *icon_texture(int which, unsigned color)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 18, 18);
	cairo_t *cr = cairo_create(s);
	draw_icon(cr, which, 9, 9, color, 1);
	cairo_destroy(cr);
	GdkTexture *t = texture_from(s);
	cairo_surface_destroy(s);
	return t;
}

/* 程式圖示：柚子色的圓（地球的經緯線）＋頂端一片綠葉 */
static cairo_surface_t *app_icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	cairo_pattern_t *g = cairo_pattern_create_linear(28, 22, 100, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 1.00, 0.80, 0.30);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.97, 0.47, 0.15);
	cairo_arc(cr, 64, 70, 50, 0, 2 * M_PI);
	cairo_set_source(cr, g);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(g);
	cairo_clip(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
	cairo_set_line_width(cr, 4.2);
	cairo_save(cr);
	cairo_translate(cr, 64, 70);
	cairo_scale(cr, 0.46, 1);
	cairo_arc(cr, 0, 0, 50, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_stroke(cr);
	cairo_move_to(cr, 64, 18); cairo_line_to(cr, 64, 122);
	cairo_move_to(cr, 12, 70); cairo_line_to(cr, 116, 70);
	cairo_stroke(cr);
	for (int k = -1; k <= 1; k += 2) {
		cairo_save(cr);
		cairo_translate(cr, 64, 70 + k * 74);
		cairo_scale(cr, 1, 0.62);
		cairo_arc(cr, 0, 0, 80, 0, 2 * M_PI);
		cairo_restore(cr);
		cairo_stroke(cr);
	}
	cairo_reset_clip(cr);
	cairo_set_source_rgba(cr, 0.55, 0.25, 0.05, 0.25);   /* 圓周一圈淡淡的邊 */
	cairo_set_line_width(cr, 1.5);
	cairo_arc(cr, 64, 70, 49.5, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_save(cr);                                        /* 葉子 */
	cairo_translate(cr, 80, 20);
	cairo_rotate(cr, -0.55);
	cairo_scale(cr, 1, 0.48);
	cairo_arc(cr, 0, 0, 19, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_set_source_rgb(cr, 0.27, 0.69, 0.31);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.20, 0.52, 0.24);
	cairo_set_line_width(cr, 2);
	cairo_move_to(cr, 66, 28); cairo_line_to(cr, 95, 12);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

/* ---------- 工具列（左：上一頁／下一頁／重新載入；右：下載、新分頁、選單） ---------- */

#define NAV_BW 34

static void button_bg(cairo_t *cr, double x, double y, double w, double h, int hover)
{
	if (!hover) return;
	pm_rrect(cr, x, y, w, h, 7, 7);
	pm_rgb(cr, col_hover);
	cairo_fill(cr);
}

static void draw_nav(GtkDrawingArea *a, cairo_t *cr, int wd, int ht, gpointer data)
{
	Win *w = data;
	Tab *t = cur_tab(w);
	int en[3] = {
		t && webkit_web_view_can_go_back(t->view),
		t && webkit_web_view_can_go_forward(t->view),
		1,
	};
	int loading = t && webkit_web_view_is_loading(t->view);
	for (int i = 0; i < 3; i++) {
		double x = 2 + i * NAV_BW, y = (ht - 30) / 2.0;
		button_bg(cr, x, y, NAV_BW - 4, 30, w->hover_nav == i && en[i]);
		int ic = i == 0 ? IC_BACK : i == 1 ? IC_FWD : loading ? IC_STOP : IC_RELOAD;
		draw_icon(cr, ic, x + (NAV_BW - 4) / 2.0, y + 15, col_text, en[i] ? 0.85 : 0.28);
	}
}

static int nav_hit(double x) { int i = (int)((x - 2) / NAV_BW); return x < 2 || i > 2 ? -1 : i; }

static void nav_activate(Win *w, int i)
{
	Tab *t = cur_tab(w);
	if (!t) return;
	if (i == 0) webkit_web_view_go_back(t->view);
	else if (i == 1) webkit_web_view_go_forward(t->view);
	else if (i == 2) {
		if (webkit_web_view_is_loading(t->view)) webkit_web_view_stop_loading(t->view);
		else webkit_web_view_reload(t->view);
	}
}

/* 右邊的按鈕：有下載時多一個下載按鈕 */
static int right_count(void) { return downloads && downloads->len ? 3 : 2; }
static int right_kind(int i) { return right_count() == 3 ? i : i + 1; }   /* 0 下載、1 新分頁、2 選單 */

static void draw_right(GtkDrawingArea *a, cairo_t *cr, int wd, int ht, gpointer data)
{
	Win *w = data;
	int n = right_count();
	for (int i = 0; i < n; i++) {
		double x = wd - (n - i) * NAV_BW, y = (ht - 30) / 2.0;
		button_bg(cr, x, y, NAV_BW - 4, 30, w->hover_right == i);
		double cx = x + (NAV_BW - 4) / 2.0, cy = y + 15;
		int k = right_kind(i);
		if (k == 0) {
			/* 下載進度：圓環（全部完成就是滿的） */
			double done = 0;
			int active = 0;
			for (guint d = 0; d < downloads->len; d++) {
				WebKitDownload *dl = downloads->pdata[d];
				if (!g_object_get_data(G_OBJECT(dl), "pb-done")) { active++; done += webkit_download_get_estimated_progress(dl); }
			}
			draw_icon(cr, IC_DOWNLOAD, cx, cy - 0.5, active ? col_accent : col_text, 0.85);
			if (active) {
				cairo_set_line_width(cr, 2);
				pm_rgba(cr, col_text, 0.15);
				cairo_arc(cr, cx, cy, 12, 0, 2 * M_PI);
				cairo_stroke(cr);
				pm_rgb(cr, col_accent);
				cairo_arc(cr, cx, cy, 12, -M_PI / 2, -M_PI / 2 + 2 * M_PI * MAX(0.04, done / active));
				cairo_stroke(cr);
			}
		} else
			draw_icon(cr, k == 1 ? IC_PLUS : IC_MENU, cx, cy, col_text, 0.85);
	}
}

static int right_hit(Win *w, double x)
{
	int wd = gtk_widget_get_width(w->right), n = right_count();
	for (int i = 0; i < n; i++) {
		double bx = wd - (n - i) * NAV_BW;
		if (x >= bx && x < bx + NAV_BW - 4) return i;
	}
	return -1;
}

static void menu_open(Win *w);
static void act_downloads(Win *w);

static void right_activate(Win *w, int i)
{
	int k = right_kind(i);
	if (k == 0) {
		act_downloads(w);
	} else if (k == 1) {
		tab_new(w, START, 1);
	} else
		menu_open(w);
}

/* 讀取進度：內容最上面一條細線 */
static void draw_progress(GtkDrawingArea *a, cairo_t *cr, int wd, int ht, gpointer data)
{
	Win *w = data;
	Tab *t = cur_tab(w);
	if (!t || !webkit_web_view_is_loading(t->view)) return;
	double p = MAX(0.08, webkit_web_view_get_estimated_load_progress(t->view));
	pm_rgb(cr, col_accent);
	cairo_rectangle(cr, 0, 0, wd * p, ht);
	cairo_fill(cr);
}

/* ---------- 分頁列（兩個分頁以上才出現） ---------- */

static void tab_geom(Win *w, int i, double *x, double *wd)
{
	double total = gtk_widget_get_width(w->tabbar) - 12;
	double tw = MIN(240, total / MAX(1, w->n));
	*x = 6 + i * tw;
	*wd = tw;
}

static void draw_tabs(GtkDrawingArea *a, cairo_t *cr, int wd, int ht, gpointer data)
{
	Win *w = data;
	pm_rgb(cr, col_tabbar);
	cairo_paint(cr);
	for (int i = 0; i < w->n; i++) {
		Tab *t = w->tabs[i];
		double x, tw;
		tab_geom(w, i, &x, &tw);
		int act = i == w->cur, hov = i == w->hover_tab;
		if (act || hov) {
			pm_rrect(cr, x + 1, 4, tw - 2, ht - 8, 8, 8);
			pm_rgba(cr, act ? col_tab : col_hover, act ? 1 : 0.6);
			cairo_fill(cr);
		} else if (i + 1 != w->cur && i + 1 < w->n && i + 1 != w->hover_tab && !hov) {
			pm_rgba(cr, col_text, 0.15);   /* 沒選到的分頁之間一條短線 */
			cairo_rectangle(cr, x + tw - 0.5, 11, 1, ht - 22);
			cairo_fill(cr);
		}
		double cy = ht / 2.0, ix = x + 12;
		int loading = webkit_web_view_is_loading(t->view);
		if (loading) {
			cairo_set_line_width(cr, 2);
			pm_rgba(cr, col_text, 0.15);
			cairo_arc(cr, ix + 7, cy, 6, 0, 2 * M_PI);
			cairo_stroke(cr);
			pm_rgb(cr, col_accent);
			cairo_arc(cr, ix + 7, cy, 6, -M_PI / 2, -M_PI / 2 + 2 * M_PI * MAX(0.1, webkit_web_view_get_estimated_load_progress(t->view)));
			cairo_stroke(cr);
		} else if (t->icon) {
			int iw = cairo_image_surface_get_width(t->icon), ih = cairo_image_surface_get_height(t->icon);
			cairo_save(cr);
			cairo_translate(cr, ix, cy - 8);
			cairo_scale(cr, 16.0 / MAX(1, iw), 16.0 / MAX(1, ih));
			cairo_set_source_surface(cr, t->icon, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		} else
			draw_icon(cr, IC_GLOBE, ix + 8, cy, col_text, 0.55);
		const char *title = webkit_web_view_get_title(t->view);
		const char *uri = webkit_web_view_get_uri(t->view);
		if (!title || !*title) title = uri && !g_str_has_prefix(uri, START) ? uri : _("新分頁");
		char buf[512];
		pm_font(cr, 12, 0);
		int show_close = act || hov;
		pm_ellipsize(cr, title, tw - 50 - (show_close ? 18 : 0), buf, sizeof buf);
		pm_rgba(cr, col_text, act ? 1 : 0.75);
		pm_text(cr, ix + 24, cy, buf);
		if (show_close) {
			double cx = x + tw - 18;
			if (hov && w->hover_close) {
				cairo_arc(cr, cx, cy, 9, 0, 2 * M_PI);
				pm_rgba(cr, col_text, 0.12);
				cairo_fill(cr);
			}
			draw_icon(cr, IC_CLOSE, cx, cy, col_text, 0.7);
		}
	}
}

static int tab_hit(Win *w, double x, double y, int *on_close)
{
	for (int i = 0; i < w->n; i++) {
		double tx, tw;
		tab_geom(w, i, &tx, &tw);
		if (x >= tx && x < tx + tw) {
			if (on_close) *on_close = fabs(x - (tx + tw - 18)) <= 9;
			return i;
		}
	}
	return -1;
}

/* ---------- 尋找列 ---------- */

static void draw_findtools(GtkDrawingArea *a, cairo_t *cr, int wd, int ht, gpointer data)
{
	Win *w = data;
	const char *q = gtk_editable_get_text(GTK_EDITABLE(w->findentry));
	char buf[64] = "";
	if (*q) {
		if (w->find_count) snprintf(buf, sizeof buf, _("%u 個符合"), w->find_count);
		else snprintf(buf, sizeof buf, "%s", _("找不到"));
	}
	pm_font(cr, 12, 0);
	pm_rgb(cr, col_dim);
	pm_text(cr, 8, ht / 2.0, buf);
	for (int i = 0; i < 3; i++) {
		double x = wd - (3 - i) * 32, y = (ht - 26) / 2.0;
		button_bg(cr, x, y, 28, 26, w->hover_find == i);
		draw_icon(cr, i == 0 ? IC_UP : i == 1 ? IC_DOWN : IC_CLOSE, x + 14, y + 13, col_text, 0.8);
	}
}

static int find_hit(Win *w, double x)
{
	int wd = gtk_widget_get_width(w->findtools);
	for (int i = 0; i < 3; i++) {
		double bx = wd - (3 - i) * 32;
		if (x >= bx && x < bx + 28) return i;
	}
	return -1;
}

static void find_run(Win *w, int dir)
{
	Tab *t = cur_tab(w);
	if (!t) return;
	WebKitFindController *fc = webkit_web_view_get_find_controller(t->view);
	const char *q = gtk_editable_get_text(GTK_EDITABLE(w->findentry));
	if (!*q) { webkit_find_controller_search_finish(fc); w->find_count = 0; gtk_widget_queue_draw(w->findtools); return; }
	if (dir == 0) {
		guint32 opt = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
		webkit_find_controller_search(fc, q, opt, 1000);
		webkit_find_controller_count_matches(fc, q, opt, 1000);
	} else if (dir > 0)
		webkit_find_controller_search_next(fc);
	else
		webkit_find_controller_search_previous(fc);
}

static void on_counted(WebKitFindController *fc, guint count, Win *w)
{
	w->find_count = count;
	gtk_widget_queue_draw(w->findtools);
}

static void on_find_failed(WebKitFindController *fc, Win *w)
{
	w->find_count = 0;
	gtk_widget_queue_draw(w->findtools);
}

static void show_find(Win *w, int on)
{
	Tab *t = cur_tab(w);
	gtk_widget_set_visible(w->findbar, on);
	if (on) {
		gtk_widget_grab_focus(w->findentry);
		gtk_editable_select_region(GTK_EDITABLE(w->findentry), 0, -1);
		find_run(w, 0);
	} else if (t) {
		webkit_find_controller_search_finish(webkit_web_view_get_find_controller(t->view));
		gtk_widget_grab_focus(GTK_WIDGET(t->view));
	}
}

/* ---------- 詢問列（網站要權限、清除瀏覽資料的確認） ---------- */

static void ask_update(Win *w)
{
	Tab *t = cur_tab(w);
	if (w->confirm_clear) {
		gtk_label_set_text(GTK_LABEL(w->asklabel), _("要清除所有瀏覽紀錄、Cookie 與快取嗎？網站會需要重新登入。"));
		gtk_button_set_label(GTK_BUTTON(w->askno), _("取消"));
		gtk_button_set_label(GTK_BUTTON(w->askyes), _("清除"));
		gtk_widget_set_visible(w->askbar, TRUE);
	} else if (t && t->ask) {
		gtk_label_set_text(GTK_LABEL(w->asklabel), t->ask_text);
		gtk_button_set_label(GTK_BUTTON(w->askno), _("不允許"));
		gtk_button_set_label(GTK_BUTTON(w->askyes), _("允許"));
		gtk_widget_set_visible(w->askbar, TRUE);
	} else
		gtk_widget_set_visible(w->askbar, FALSE);
}

static void clear_done(GObject *src, GAsyncResult *res, gpointer unused)
{
	webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(src), res, NULL);
}

static void ask_answer(Win *w, int yes)
{
	Tab *t = cur_tab(w);
	if (w->confirm_clear) {
		w->confirm_clear = 0;
		if (yes) {
			WebKitWebsiteDataManager *m = webkit_network_session_get_website_data_manager(session);
			webkit_website_data_manager_clear(m, WEBKIT_WEBSITE_DATA_ALL, 0, NULL, clear_done, NULL);
			g_hash_table_remove_all(hist);
			hist_save(NULL);
			nclosed = 0;
		}
	} else if (t && t->ask) {
		if (yes) webkit_permission_request_allow(t->ask);
		else webkit_permission_request_deny(t->ask);
		g_clear_object(&t->ask);
	}
	ask_update(w);
}

static void on_ask_no(GtkButton *b, Win *w) { ask_answer(w, 0); }
static void on_ask_yes(GtkButton *b, Win *w) { ask_answer(w, 1); }

/* ---------- 選單（右上角「⋯」） ---------- */

typedef struct { const char *label, *accel; void (*fn)(Win *); int sep; int engine; } MItem;

static void act_newtab(Win *w) { tab_new(w, START, 1); }
static void act_newwin(Win *w) { Win *n = win_new(); tab_new(n, START, 1); gtk_window_present(n->win); }
static void act_reopen(Win *w)
{
	if (!nclosed) return;
	tab_new(w, closed[--nclosed], 1);
}
static void zoom_by(Win *w, double f)
{
	Tab *t = cur_tab(w);
	if (!t) return;
	double z = f ? CLAMP(webkit_web_view_get_zoom_level(t->view) * f, 0.3, 5.0) : 1.0;
	webkit_web_view_set_zoom_level(t->view, z);
}
static void act_zoomin(Win *w) { zoom_by(w, 1.1); }
static void act_zoomout(Win *w) { zoom_by(w, 1 / 1.1); }
static void act_zoom0(Win *w) { zoom_by(w, 0); }
static void act_find(Win *w) { show_find(w, 1); }
static void act_print(Win *w)
{
	Tab *t = cur_tab(w);
	if (!t) return;
	WebKitPrintOperation *op = webkit_print_operation_new(t->view);
	webkit_print_operation_run_dialog(op, w->win);
	g_object_unref(op);
}
static void set_fullscreen(Win *w, int on)
{
	w->fullscreen = on;
	gtk_widget_set_visible(w->tools, !on);
	update_chrome(w);
	if (on) gtk_window_fullscreen(w->win);
	else gtk_window_unfullscreen(w->win);
}
static void act_fullscreen(Win *w) { set_fullscreen(w, !w->fullscreen); }
static void act_downloads(Win *w)
{
	char *u = g_filename_to_uri(download_dir(), NULL, NULL);
	open_external(u);
	g_free(u);
}
static void act_clear(Win *w) { w->confirm_clear = 1; ask_update(w); }
static void act_about(Win *w) { tab_new(w, "pomelo://about", 1); }

static const MItem menu_items[] = {
	{ N_("新分頁"), "Ctrl+T", act_newtab, 0, -1 },
	{ N_("新視窗"), "Ctrl+N", act_newwin, 0, -1 },
	{ N_("重新開啟關閉的分頁"), "Ctrl+Shift+T", act_reopen, 0, -1 },
	{ N_("放大"), "Ctrl++", act_zoomin, 1, -1 },
	{ N_("縮小"), "Ctrl+-", act_zoomout, 0, -1 },
	{ N_("實際大小"), "Ctrl+0", act_zoom0, 0, -1 },
	{ N_("在網頁中尋找…"), "Ctrl+F", act_find, 1, -1 },
	{ N_("列印…"), "Ctrl+P", act_print, 0, -1 },
	{ N_("全螢幕"), "F11", act_fullscreen, 0, -1 },
	{ N_("下載的檔案"), NULL, act_downloads, 1, -1 },
	{ "Google", NULL, NULL, 1, 0 },
	{ "DuckDuckGo", NULL, NULL, 0, 1 },
	{ "Bing", NULL, NULL, 0, 2 },
	{ N_("清除瀏覽資料…"), NULL, act_clear, 1, -1 },
	{ N_("關於小柚瀏覽器"), NULL, act_about, 0, -1 },
};
#define MENU_W   280
#define ITEM_H   30
#define SEP_H    9
#define CAPTION_H 24

static double item_y(int i)
{
	double y = 4;
	for (int k = 0; k <= i; k++) {
		if (menu_items[k].sep) y += SEP_H;
		if (menu_items[k].engine == 0) y += CAPTION_H;   /* 「搜尋引擎」小標 */
		if (k < i) y += ITEM_H;
	}
	return y;
}

static void draw_menu(GtkDrawingArea *a, cairo_t *cr, int wd, int ht, gpointer data)
{
	Win *w = data;
	for (int i = 0; i < (int)LEN(menu_items); i++) {
		const MItem *m = &menu_items[i];
		double y = item_y(i);
		if (m->sep) {
			pm_rgba(cr, col_text, 0.12);
			cairo_rectangle(cr, 10, y - SEP_H / 2.0 - 0.5, wd - 20, 1);
			cairo_fill(cr);
		}
		if (m->engine == 0) {
			pm_font(cr, 11, 0);
			pm_rgb(cr, col_dim);
			pm_text(cr, 12, y - CAPTION_H / 2.0, _("搜尋引擎"));
		}
		int hov = w->hover_menu == i;
		if (hov) {
			pm_rrect(cr, 4, y, wd - 8, ITEM_H, 6, 6);
			pm_rgb(cr, col_accent);
			cairo_fill(cr);
		}
		unsigned fg = hov ? 0xffffff : col_text;
		pm_font(cr, 13, 0);
		pm_rgb(cr, fg);
		pm_text(cr, m->engine >= 0 ? 32 : 14, y + ITEM_H / 2.0, m->engine >= 0 ? m->label : _(m->label));
		if (m->engine >= 0 && m->engine == engine) {
			cairo_set_line_width(cr, 1.8);
			cairo_move_to(cr, 13, y + 15); cairo_line_to(cr, 17, y + 19); cairo_line_to(cr, 24, y + 10);
			cairo_stroke(cr);
		}
		if (m->accel) {
			pm_font(cr, 12, 0);
			pm_rgba(cr, fg, hov ? 0.85 : 0.5);
			pm_text(cr, wd - 14 - pm_textw(cr, m->accel), y + ITEM_H / 2.0, m->accel);
		}
	}
}

static int menu_hit(double y)
{
	for (int i = 0; i < (int)LEN(menu_items); i++) {
		double iy = item_y(i);
		if (y >= iy && y < iy + ITEM_H) return i;
	}
	return -1;
}

static void menu_motion(GtkEventControllerMotion *c, double x, double y, Win *w)
{
	int h = menu_hit(y);
	if (h != w->hover_menu) { w->hover_menu = h; gtk_widget_queue_draw(w->menuarea); }
}

static void menu_leave(GtkEventControllerMotion *c, Win *w)
{
	w->hover_menu = -1;
	gtk_widget_queue_draw(w->menuarea);
}

static void menu_click(GtkGestureClick *g, int n, double x, double y, Win *w)
{
	int i = menu_hit(y);
	if (i < 0) return;
	gtk_popover_popdown(GTK_POPOVER(w->menu));
	const MItem *m = &menu_items[i];
	if (m->engine >= 0) {
		engine = m->engine;
		pm_conf_set("browser.search", engines[engine].name);
	} else if (m->fn)
		m->fn(w);
}

static void menu_open(Win *w)
{
	if (!w->menu) {
		w->menu = gtk_popover_new();
		gtk_widget_add_css_class(w->menu, "pb-menu");
		gtk_popover_set_has_arrow(GTK_POPOVER(w->menu), FALSE);
		gtk_widget_set_parent(w->menu, w->right);
		w->menuarea = gtk_drawing_area_new();
		gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(w->menuarea), MENU_W);
		gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(w->menuarea), (int)(item_y(LEN(menu_items) - 1) + ITEM_H + 4));
		gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(w->menuarea), draw_menu, w, NULL);
		GtkEventController *mc = gtk_event_controller_motion_new();
		g_signal_connect(mc, "motion", G_CALLBACK(menu_motion), w);
		g_signal_connect(mc, "leave", G_CALLBACK(menu_leave), w);
		gtk_widget_add_controller(w->menuarea, mc);
		GtkGesture *click = gtk_gesture_click_new();
		g_signal_connect(click, "released", G_CALLBACK(menu_click), w);
		gtk_widget_add_controller(w->menuarea, GTK_EVENT_CONTROLLER(click));
		gtk_popover_set_child(GTK_POPOVER(w->menu), w->menuarea);
		gtk_popover_set_position(GTK_POPOVER(w->menu), GTK_POS_BOTTOM);
	}
	int wd = gtk_widget_get_width(w->right);
	GdkRectangle r = { wd - NAV_BW, 4, NAV_BW - 4, 34 };
	gtk_popover_set_pointing_to(GTK_POPOVER(w->menu), &r);
	w->hover_menu = -1;
	gtk_popover_popup(GTK_POPOVER(w->menu));
}

/* ---------- 網址列 ---------- */

static void entry_show(Win *w)
{
	Tab *t = cur_tab(w);
	if (!t || w->editing) return;
	const char *uri = webkit_web_view_get_uri(t->view);
	if (!uri || g_str_has_prefix(uri, START) || !strcmp(uri, "about:blank")) uri = "";
	/* 網址裡的 %E9%A6%96 之類解回看得懂的字（例如中文）。只解 0x80 以上的位元組：%20、%2F、%3F 這些
	 * 英文符號解開會改變網址的意思（使用者再按 Enter 就開錯），照原樣留著；解出來不是正常的 UTF-8 就整串不解 */
	size_t len = strlen(uri);
	char *show = g_malloc(len + 1), *o = show;
	for (const char *c = uri; *c; ) {
		int hi = c[0] == '%' && g_ascii_isxdigit(c[1]) && g_ascii_isxdigit(c[2]) ? g_ascii_xdigit_value(c[1]) * 16 + g_ascii_xdigit_value(c[2]) : -1;
		if (hi >= 0x80) { *o++ = (char)hi; c += 3; }
		else *o++ = *c++;
	}
	*o = 0;
	w->setting = 1;
	gtk_editable_set_text(GTK_EDITABLE(w->entry), g_utf8_validate(show, -1, NULL) ? show : uri);
	w->setting = 0;
	g_free(show);
	GTlsCertificateFlags errors = 0;
	GTlsCertificate *cert = NULL;
	GdkTexture *icon = NULL;
	if (g_str_has_prefix(uri, "https://") && webkit_web_view_get_tls_info(t->view, &cert, &errors) && !errors) icon = tex_lock;
	else if (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")) icon = tex_warn;
	gtk_entry_set_icon_from_paintable(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY, icon ? GDK_PAINTABLE(icon) : NULL);
	gtk_entry_set_icon_tooltip_text(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY,
		icon == tex_lock ? _("連線是加密的（按一下看憑證）") : icon ? _("連線沒有加密，請不要在這個網站輸入密碼或信用卡") : NULL);
}

/* ---------- 憑證資訊（點網址列的鎖頭） ---------- */

/* 從 RFC 4514 的名稱（"CN=*.example.org,O=Example\, Inc.,C=US"）取出一項，沒有回傳 NULL */
static char *dn_part(const char *dn, const char *key)
{
	size_t kl = strlen(key);
	for (const char *p = dn; p && *p; ) {
		while (*p == ' ' || *p == ',') p++;
		int hit = !g_ascii_strncasecmp(p, key, kl) && p[kl] == '=';
		GString *v = hit ? g_string_new(NULL) : NULL;
		if (hit) p += kl + 1;
		for (; *p && *p != ','; p++) {
			if (*p == '\\' && p[1]) p++;
			if (v) g_string_append_c(v, *p);
		}
		if (v) return g_string_free(v, FALSE);
	}
	return NULL;
}

/* 名稱裡最能代表的一項：一般名稱（CN），沒有就用組織（O），都沒有就整串 */
static char *dn_short(const char *dn)
{
	char *v = dn ? dn_part(dn, "CN") : NULL;
	if (!v && dn) v = dn_part(dn, "O");
	return v ? v : g_strdup(dn ? dn : "");
}

static void cert_row(GtkWidget *grid, int *row, const char *key, const char *value, int mono)
{
	if (!value || !*value) return;
	GtkWidget *k = gtk_label_new(key), *v = gtk_label_new(value);
	gtk_widget_add_css_class(k, "pb-dim");
	gtk_label_set_xalign(GTK_LABEL(k), 0);
	gtk_label_set_yalign(GTK_LABEL(k), 0);
	gtk_label_set_xalign(GTK_LABEL(v), 0);
	gtk_label_set_wrap(GTK_LABEL(v), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 46);
	gtk_label_set_selectable(GTK_LABEL(v), TRUE);
	if (mono) gtk_widget_add_css_class(v, "monospace");
	if (!g_object_get_data(G_OBJECT(grid), "first")) g_object_set_data(G_OBJECT(grid), "first", v);
	gtk_grid_attach(GTK_GRID(grid), k, 0, *row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), v, 1, *row, 1, 1);
	(*row)++;
}

static char *cert_date(GDateTime *d)
{
	return d ? g_date_time_format(d, "%Y-%m-%d") : NULL;
}

static void cert_copy(GtkButton *b, gpointer unused)
{
	const char *pem = g_object_get_data(G_OBJECT(b), "pem");
	if (pem) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(b)), pem);
	gtk_button_set_label(b, _("已複製"));
}

static void cert_open(Win *w)
{
	Tab *t = cur_tab(w);
	if (!t) return;
	const char *uri = webkit_web_view_get_uri(t->view);
	if (!uri || !(g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "http://"))) return;
	GTlsCertificate *cert = NULL;
	GTlsCertificateFlags errors = 0;
	int tls = g_str_has_prefix(uri, "https://") && webkit_web_view_get_tls_info(t->view, &cert, &errors) && cert;
	GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	char *host = g_strdup(u && g_uri_get_host(u) ? g_uri_get_host(u) : "");
	if (u) g_uri_unref(u);

	if (w->certpop) gtk_widget_unparent(w->certpop);
	GtkWidget *pop = w->certpop = gtk_popover_new();
	gtk_widget_add_css_class(pop, "pb-menu");
	gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
	gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
	gtk_widget_set_parent(pop, w->entry);
	GdkRectangle r;
	gtk_entry_get_icon_area(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY, &r);
	r.x -= 8; r.width = 440;   /* 彈出視窗以指的範圍置中：給一段跟它差不多寬的範圍，看起來就是從鎖頭往右展開 */
	gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
	gtk_widget_set_margin_top(box, 14); gtk_widget_set_margin_bottom(box, 14);
	const char *title, *desc;
	if (!tls) {
		title = _("連線沒有加密");
		desc = _("你跟這個網站之間傳送的資料（包括密碼、信用卡號碼）可能被別人看到或竄改。");
	} else if (errors) {
		title = _("這個網站的憑證有問題");
		desc = _("你已經選擇仍要前往。請不要在這個網站輸入密碼或信用卡。");
	} else {
		title = _("連線是加密的");
		desc = _("你跟這個網站之間傳送的資料是加密的，別人看不到。網站的身分由下面的憑證證明。");
	}
	GtkWidget *l = gtk_label_new(title);
	gtk_widget_add_css_class(l, "pb-title");
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_box_append(GTK_BOX(box), l);
	l = gtk_label_new(desc);
	gtk_label_set_xalign(GTK_LABEL(l), 0);
	gtk_label_set_wrap(GTK_LABEL(l), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(l), 52);
	gtk_box_append(GTK_BOX(box), l);

	if (tls) {
		GtkWidget *grid = gtk_grid_new();
		gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
		gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
		gtk_widget_set_margin_top(grid, 6);
		int row = 0;
		cert_row(grid, &row, _("網站"), host, 0);
		if (errors) {
			static const struct { GTlsCertificateFlags f; const char *s; } why[] = {
				{ G_TLS_CERTIFICATE_UNKNOWN_CA, N_("不是受信任的憑證單位發的") },
				{ G_TLS_CERTIFICATE_BAD_IDENTITY, N_("憑證上的網址跟這個網站不符") },
				{ G_TLS_CERTIFICATE_NOT_ACTIVATED, N_("憑證還沒生效") },
				{ G_TLS_CERTIFICATE_EXPIRED, N_("憑證已經過期") },
				{ G_TLS_CERTIFICATE_REVOKED, N_("憑證已經被撤銷") },
				{ G_TLS_CERTIFICATE_INSECURE, N_("使用不安全的加密方式") },
				{ G_TLS_CERTIFICATE_GENERIC_ERROR, N_("其他錯誤") },
			};
			GString *e = g_string_new(NULL);
			for (unsigned i = 0; i < LEN(why); i++)
				if (errors & why[i].f) g_string_append_printf(e, "%s%s", e->len ? "\n" : "", _(why[i].s));
			cert_row(grid, &row, _("問題"), e->str, 0);
			g_string_free(e, TRUE);
		}
		char *subj = g_tls_certificate_get_subject_name(cert), *iss = g_tls_certificate_get_issuer_name(cert);
		char *cn = subj ? dn_part(subj, "CN") : NULL, *org = subj ? dn_part(subj, "O") : NULL;
		/* 發證單位：組織加上代號，例如「Let's Encrypt（YE2）」；只有代號看不出是誰 */
		char *iss_o = iss ? dn_part(iss, "O") : NULL, *iss_cn = iss ? dn_part(iss, "CN") : NULL;
		char *by = iss_o && iss_cn && strcmp(iss_o, iss_cn) ? g_strdup_printf("%s（%s）", iss_o, iss_cn) : dn_short(iss);
		g_free(iss_o); g_free(iss_cn);
		cert_row(grid, &row, _("發給"), cn, 0);
		cert_row(grid, &row, _("組織"), org, 0);
		cert_row(grid, &row, _("發證單位"), by, 0);
		GDateTime *d0 = g_tls_certificate_get_not_valid_before(cert), *d1 = g_tls_certificate_get_not_valid_after(cert);
		char *s0 = cert_date(d0), *s1 = cert_date(d1);
		if (s0 && s1) {
			char *span = g_strdup_printf(_("%s 到 %s"), s0, s1);
			cert_row(grid, &row, _("有效期間"), span, 0);
			g_free(span);
		}
		/* 這張憑證也適用的網址（最多列 6 個） */
		GPtrArray *names = g_tls_certificate_get_dns_names(cert);
		if (names && names->len) {
			GString *n = g_string_new(NULL);
			for (unsigned i = 0; i < names->len && i < 6; i++) {
				gsize sz;
				const char *d = g_bytes_get_data(names->pdata[i], &sz);
				g_string_append_printf(n, "%s%.*s", i ? "、" : "", (int)sz, d);
			}
			if (names->len > 6) g_string_append_printf(n, _("…等 %u 個"), names->len);
			cert_row(grid, &row, _("適用網址"), n->str, 0);
			g_string_free(n, TRUE);
		}
		if (names) g_ptr_array_unref(names);
		/* 憑證鏈：網站 → 中繼 → 根憑證；PEM 一起收集起來給「複製憑證」 */
		GString *chain = g_string_new(NULL), *pem = g_string_new(NULL);
		for (GTlsCertificate *c = cert; c; c = g_tls_certificate_get_issuer(c)) {
			char *sn = g_tls_certificate_get_subject_name(c), *one = dn_short(sn), *p = NULL;
			g_string_append_printf(chain, "%s%s", chain->len ? "\n→ " : "", one);
			g_object_get(c, "certificate-pem", &p, NULL);
			if (p) g_string_append(pem, p);
			g_free(sn); g_free(one); g_free(p);
		}
		cert_row(grid, &row, _("憑證鏈"), chain->str, 0);
		GByteArray *der = NULL;
		g_object_get(cert, "certificate", &der, NULL);
		if (der) {
			char *hex = g_compute_checksum_for_data(G_CHECKSUM_SHA256, der->data, der->len);
			GString *fp = g_string_new(NULL);
			for (int i = 0; hex[i] && hex[i + 1]; i += 2)
				g_string_append_printf(fp, "%s%c%c", i ? ":" : "", g_ascii_toupper(hex[i]), g_ascii_toupper(hex[i + 1]));
			cert_row(grid, &row, _("SHA-256 指紋"), fp->str, 1);
			g_string_free(fp, TRUE);
			g_free(hex);
			g_byte_array_unref(der);
		}
		gtk_box_append(GTK_BOX(box), grid);
		GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_halign(btns, GTK_ALIGN_END);
		gtk_widget_set_margin_top(btns, 6);
		GtkWidget *b = gtk_button_new_with_label(_("複製憑證"));
		gtk_widget_add_css_class(b, "pb-btn");
		gtk_widget_set_tooltip_text(b, _("把整條憑證鏈以 PEM 格式複製到剪貼簿"));
		g_object_set_data_full(G_OBJECT(b), "pem", g_string_free(pem, FALSE), g_free);
		g_signal_connect(b, "clicked", G_CALLBACK(cert_copy), NULL);
		gtk_box_append(GTK_BOX(btns), b);
		gtk_box_append(GTK_BOX(box), btns);
		g_object_set_data(G_OBJECT(pop), "focus", b);
		g_object_set_data(G_OBJECT(pop), "first", g_object_get_data(G_OBJECT(grid), "first"));
		g_string_free(chain, TRUE);
		g_free(subj); g_free(iss); g_free(cn); g_free(org); g_free(by); g_free(s0); g_free(s1);
	}
	gtk_popover_set_child(GTK_POPOVER(pop), box);
	gtk_popover_popup(GTK_POPOVER(pop));
	/* 焦點放在按鈕上：不然第一個可以選取的欄位（網站）一打開就整個反白 */
	GtkWidget *f = g_object_get_data(G_OBJECT(pop), "focus");
	if (f) gtk_widget_grab_focus(f);
	GtkWidget *first = g_object_get_data(G_OBJECT(pop), "first");   /* 一打開時拿到焦點的欄位會整個選起來 */
	if (first) gtk_label_select_region(GTK_LABEL(first), 0, 0);
	g_free(host);
}

/* 點網址列前面的鎖頭／三角形（左鍵或右鍵）：看憑證。要比輸入框自己的點擊（全選、右鍵選單）先處理 */
static int on_lock(Win *w, double x, double y)
{
	if (!gtk_entry_get_icon_paintable(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY)) return 0;
	GdkRectangle r;
	gtk_entry_get_icon_area(GTK_ENTRY(w->entry), GTK_ENTRY_ICON_PRIMARY, &r);
	return x >= r.x - 4 && x < r.x + r.width + 4 && y >= r.y - 4 && y < r.y + r.height + 4;
}

static void on_entry_icon_press(GtkGestureClick *g, int n, double x, double y, Win *w)   /* 右鍵 */
{
	if (!on_lock(w, x, y)) return;
	gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
	cert_open(w);
}

static void on_entry_activate(GtkEntry *e, Win *w)
{
	Tab *t = cur_tab(w);
	const char *typed = gtk_editable_get_text(GTK_EDITABLE(e));
	int bare = !strstr(typed, "://");   /* 要在載入前看：一開始載入網址列就會換成完整網址 */
	char *uri = to_uri(typed);
	if (!uri) return;
	w->editing = 0;
	if (!t) t = tab_new(w, uri, 1);
	else webkit_web_view_load_uri(t->view, uri);
	/* 沒打 http:// 或 https:// 的網址先用 https；網站不支援的話 on_load_failed 改用 http */
	snprintf(t->upgraded, sizeof t->upgraded, "%s",
	         bare && g_str_has_prefix(uri, "https://") ? uri : "");
	g_free(uri);
	gtk_widget_grab_focus(GTK_WIDGET(t->view));
	entry_show(w);
}

static void on_entry_changed(GtkEditable *e, Win *w)
{
	if (!w->setting && gtk_widget_get_focus_child(w->entry)) w->editing = 1;
}

static void on_entry_focus_leave(GtkEventControllerFocus *f, Win *w)
{
	w->editing = 0;
	entry_show(w);
}

/* 第一次點網址列就全選（再點才是移動游標），跟一般的瀏覽器一樣 */
static void on_entry_press(GtkGestureClick *g, int n, double x, double y, Win *w)
{
	if (on_lock(w, x, y)) { gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED); cert_open(w); return; }
	if (gtk_widget_get_focus_child(w->entry)) return;
	gtk_widget_grab_focus(w->entry);
	gtk_editable_select_region(GTK_EDITABLE(w->entry), 0, -1);
	gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void focus_entry(Win *w)
{
	gtk_widget_grab_focus(w->entry);
	gtk_editable_select_region(GTK_EDITABLE(w->entry), 0, -1);
}

/* ---------- 分頁的 WebKit 事件 ---------- */

static Win *win_of(Tab *t) { return t->w; }
static int tab_index(Tab *t) { Win *w = t->w; for (int i = 0; i < w->n; i++) if (w->tabs[i] == t) return i; return -1; }

static void update_chrome(Win *w)
{
	Tab *t = cur_tab(w);
	gtk_widget_set_visible(w->tabbar, w->n > 1 && !w->fullscreen);
	gtk_widget_queue_draw(w->nav);
	gtk_widget_queue_draw(w->right);
	gtk_widget_queue_draw(w->tabbar);
	gtk_widget_queue_draw(w->progress);
	const char *title = t ? webkit_web_view_get_title(t->view) : NULL;
	gtk_window_set_title(w->win, title && *title ? title : _("小柚瀏覽器"));
}

static void on_title(WebKitWebView *v, GParamSpec *p, Tab *t)
{
	update_chrome(win_of(t));
}

static void on_uri(WebKitWebView *v, GParamSpec *p, Tab *t)
{
	if (t == cur_tab(t->w)) entry_show(t->w);
	update_chrome(t->w);
}

static void on_progress(WebKitWebView *v, GParamSpec *p, Tab *t)
{
	Win *w = t->w;
	gtk_widget_queue_draw(w->tabbar);
	if (t == cur_tab(w)) { gtk_widget_queue_draw(w->progress); gtk_widget_queue_draw(w->nav); }
}

static void on_favicon(WebKitWebView *v, GParamSpec *p, Tab *t)
{
	g_clear_pointer(&t->icon, cairo_surface_destroy);
	GdkTexture *tex = webkit_web_view_get_favicon(v);
	if (tex) t->icon = surface_from(tex);
	gtk_widget_queue_draw(t->w->tabbar);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, Tab *t)
{
	Win *w = t->w;
	if (ev == WEBKIT_LOAD_STARTED) {
		g_clear_pointer(&t->icon, cairo_surface_destroy);
		if (t->ask) { webkit_permission_request_deny(t->ask); g_clear_object(&t->ask); if (t == cur_tab(w)) ask_update(w); }
	}
	if (ev == WEBKIT_LOAD_COMMITTED) t->upgraded[0] = 0;   /* 已經連上了 */
	if (ev == WEBKIT_LOAD_COMMITTED && t == cur_tab(w)) entry_show(w);
	if (ev == WEBKIT_LOAD_FINISHED) hist_add(webkit_web_view_get_uri(v), webkit_web_view_get_title(v));
	update_chrome(w);
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent ev, const char *uri, GError *e, Tab *t)
{
	if (g_error_matches(e, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
	    g_error_matches(e, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
		return FALSE;   /* 使用者按了停止、或改成下載 */
	/* 自動補的 https 連不上（網站沒有 https）：改用 http 再試一次。憑證有問題不會到這裡（on_tls_error），不會偷偷降級 */
	if (t->upgraded[0] && g_str_has_prefix(uri, "https://") && !g_error_matches(e, G_RESOLVER_ERROR, G_RESOLVER_ERROR_NOT_FOUND)) {
		char *http = g_strdup_printf("http://%s", t->upgraded + strlen("https://"));
		t->upgraded[0] = 0;
		webkit_web_view_load_uri(v, http);
		g_free(http);
		return TRUE;
	}
	t->upgraded[0] = 0;
	/* 網路函式庫的錯誤訊息是英文：常見的換成看得懂的說明，原文放在下面小字 */
	const char *why = _("網站沒有正常回應，連線中斷了。");
	if (e->domain == G_RESOLVER_ERROR) why = _("找不到這個網站。請檢查網址有沒有打錯，或電腦有沒有連上網路。");
	else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED)) why = _("這個網站拒絕連線。");
	else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) why = _("網站太久沒有回應。");
	else if (g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) || g_error_matches(e, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE))
		why = _("連不上網路。請檢查網路連線。");
	char *raw = esc(e->message), *more = g_strdup_printf("<p class=\"small\">%s</p>", raw);
	show_error(t, uri, _("無法開啟這個網頁"), why, _("重新載入"), "location.reload()", more);
	g_free(raw);
	g_free(more);
	return TRUE;
}

static gboolean on_tls_error(WebKitWebView *v, const char *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, Tab *t)
{
	GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	snprintf(t->tls_host, sizeof t->tls_host, "%s", u && g_uri_get_host(u) ? g_uri_get_host(u) : "");
	if (u) g_uri_unref(u);
	snprintf(t->tls_uri, sizeof t->tls_uri, "%s", uri);
	g_set_object(&t->tls_cert, cert);
	char *tok = g_uuid_string_random();
	snprintf(t->token, sizeof t->token, "%s", tok);
	g_free(tok);
	char *more = g_strdup_printf("<p class=\"small\"><a href=\"pomelo://proceed?t=%s\">%s</a></p>", t->token, _("我了解風險，仍要前往（不建議）"));
	show_error(t, uri, _("這個連線不安全"),
		_("這個網站的安全憑證有問題，可能有人在冒充這個網站、竊取你輸入的資料（例如密碼、信用卡）。"),
		_("回上一頁"), "history.back()", more);
	g_free(more);
	return TRUE;
}

static int web_scheme(const char *uri)
{
	static const char *ok[] = { "http:", "https:", "about:", "data:", "blob:", "file:", "pomelo:", "javascript:" };
	for (unsigned i = 0; i < LEN(ok); i++) if (g_str_has_prefix(uri, ok[i])) return 1;
	return 0;
}

static gboolean on_decide(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, Tab *t)
{
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
		WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(d);
		if (webkit_response_policy_decision_is_main_frame_main_resource(r) && !webkit_response_policy_decision_is_mime_type_supported(r)) {
			webkit_policy_decision_download(d);
			return TRUE;
		}
		return FALSE;
	}
	WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
	const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
	if (!uri) return FALSE;
	if (g_str_has_prefix(uri, "pomelo://proceed?t=")) {   /* 錯誤頁的「仍要前往」 */
		if (t->token[0] && !strcmp(uri + strlen("pomelo://proceed?t="), t->token) && t->tls_cert) {
			webkit_network_session_allow_tls_certificate_for_host(session, t->tls_cert, t->tls_host);
			t->token[0] = 0;
			webkit_web_view_load_uri(t->view, t->tls_uri);
		}
		webkit_policy_decision_ignore(d);
		return TRUE;
	}
	if (!web_scheme(uri)) {   /* mailto:、tel:、magnet: 之類交給系統預設的程式 */
		open_external(uri);
		webkit_policy_decision_ignore(d);
		return TRUE;
	}
	if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
	    webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
		guint btn = webkit_navigation_action_get_mouse_button(a), mods = webkit_navigation_action_get_modifiers(a);
		if (btn == 2 || (btn == 1 && (mods & GDK_CONTROL_MASK))) {   /* 中鍵、Ctrl＋點：在背景開新分頁 */
			tab_new(t->w, uri, 0);
			webkit_policy_decision_ignore(d);
			return TRUE;
		}
	}
	return FALSE;
}

static WebKitWebView *view_new(WebKitWebView *related);
static Tab *tab_add(Win *w, WebKitWebView *v, int front);

static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, Tab *t)
{
	Tab *n = tab_add(t->w, view_new(v), 1);
	return GTK_WIDGET(n->view);
}

/* 網頁的右鍵選單：WebKit 內建項目的文字用的是 WebKit 自己的翻譯（Ubuntu 要另外裝 GNOME 的語言包才有，Pomelo 沒裝，
 * 所以常常是英文），換成我們的翻譯；GTK 的輸入法子選單（Pomelo 輸入法用不到）拿掉。
 * 開關型的項目（迴圈播放、顯示控制列、靜音）保留 WebKit 原本的，換掉會失去打勾的狀態 */
static const struct { WebKitContextMenuAction a; const char *label; } ctx_labels[] = {
	{ WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK, N_("開啟連結") },
	{ WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW, N_("在新分頁開啟連結") },
	{ WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_LINK_TO_DISK, N_("下載連結的檔案") },
	{ WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD, N_("複製連結") },
	{ WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW, N_("在新分頁開啟圖片") },
	{ WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK, N_("下載圖片") },
	{ WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD, N_("複製圖片") },
	{ WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD, N_("複製圖片網址") },
	{ WEBKIT_CONTEXT_MENU_ACTION_OPEN_FRAME_IN_NEW_WINDOW, N_("在新分頁開啟框架") },
	{ WEBKIT_CONTEXT_MENU_ACTION_GO_BACK, N_("上一頁") },
	{ WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD, N_("下一頁") },
	{ WEBKIT_CONTEXT_MENU_ACTION_STOP, N_("停止") },
	{ WEBKIT_CONTEXT_MENU_ACTION_RELOAD, N_("重新載入") },
	{ WEBKIT_CONTEXT_MENU_ACTION_COPY, N_("複製") },
	{ WEBKIT_CONTEXT_MENU_ACTION_CUT, N_("剪下") },
	{ WEBKIT_CONTEXT_MENU_ACTION_PASTE, N_("貼上") },
	{ WEBKIT_CONTEXT_MENU_ACTION_DELETE, N_("刪除") },
	{ WEBKIT_CONTEXT_MENU_ACTION_SELECT_ALL, N_("全選") },
	{ WEBKIT_CONTEXT_MENU_ACTION_OPEN_VIDEO_IN_NEW_WINDOW, N_("在新分頁開啟影片") },
	{ WEBKIT_CONTEXT_MENU_ACTION_OPEN_AUDIO_IN_NEW_WINDOW, N_("在新分頁開啟音訊") },
	{ WEBKIT_CONTEXT_MENU_ACTION_COPY_VIDEO_LINK_TO_CLIPBOARD, N_("複製影片網址") },
	{ WEBKIT_CONTEXT_MENU_ACTION_COPY_AUDIO_LINK_TO_CLIPBOARD, N_("複製音訊網址") },
	{ WEBKIT_CONTEXT_MENU_ACTION_ENTER_VIDEO_FULLSCREEN, N_("全螢幕播放") },
	{ WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PLAY, N_("播放") },
	{ WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PAUSE, N_("暫停") },
	{ WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_VIDEO_TO_DISK, N_("下載影片") },
	{ WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_AUDIO_TO_DISK, N_("下載音訊") },
	{ WEBKIT_CONTEXT_MENU_ACTION_INSERT_EMOJI, N_("插入表情符號") },
	{ WEBKIT_CONTEXT_MENU_ACTION_PASTE_AS_PLAIN_TEXT, N_("貼上為純文字") },
};

/* 右鍵選單裡我們自己加的項目（WebKit 內建的只有網頁本身的功能）：複製網址、尋找、列印、縮放、全螢幕、搜尋選取的文字 */
enum { CA_COPY_URL, CA_FIND, CA_PRINT, CA_ZOOMIN, CA_ZOOMOUT, CA_ZOOM0, CA_FULLSCREEN, CA_SEARCH, CA_N };
static GSimpleAction *ctx_act[CA_N];
static Tab *ctx_tab;   /* 按右鍵的那個分頁 */

/* 選單出來到選好之間分頁可能已經關掉：確認它還在 */
static int tab_alive(Tab *t)
{
	for (GList *l = wins; l; l = l->next) {
		Win *w = l->data;
		for (int i = 0; i < w->n; i++) if (w->tabs[i] == t) return 1;
	}
	return 0;
}

static void ctx_search_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Tab *t = data;
	JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, NULL);
	if (!v) return;
	char *sel = jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
	g_object_unref(v);
	if (sel && tab_alive(t)) {
		g_strstrip(sel);
		if (*sel) {
			char *q = g_uri_escape_string(sel, NULL, TRUE);
			char *url = g_strdup_printf(engines[engine].query, q);
			tab_new(t->w, url, 1);
			g_free(url);
			g_free(q);
		}
	}
	g_free(sel);
}

static void ctx_activate(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a; (void)p;
	Tab *t = ctx_tab;
	if (!t || !tab_alive(t)) return;
	Win *w = t->w;
	switch (GPOINTER_TO_INT(data)) {
	case CA_COPY_URL: {
		const char *u = webkit_web_view_get_uri(t->view);
		if (u) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t->view)), u);
		break;
	}
	case CA_FIND: act_find(w); break;
	case CA_PRINT: act_print(w); break;
	case CA_ZOOMIN: act_zoomin(w); break;
	case CA_ZOOMOUT: act_zoomout(w); break;
	case CA_ZOOM0: act_zoom0(w); break;
	case CA_FULLSCREEN: act_fullscreen(w); break;
	case CA_SEARCH:   /* 選取的文字在網頁那邊：用 JavaScript 拿回來 */
		webkit_web_view_evaluate_javascript(t->view, "window.getSelection().toString()", -1, NULL, NULL, NULL, ctx_search_done, t);
		break;
	}
}

static void ctx_add(WebKitContextMenu *menu, int which, const char *label)
{
	if (!ctx_act[which]) {
		char name[16];
		snprintf(name, sizeof name, "ctx%d", which);
		ctx_act[which] = g_simple_action_new(name, NULL);
		g_signal_connect(ctx_act[which], "activate", G_CALLBACK(ctx_activate), GINT_TO_POINTER(which));
	}
	webkit_context_menu_append(menu, webkit_context_menu_item_new_from_gaction(G_ACTION(ctx_act[which]), label, NULL));
}

static gboolean on_context_menu(WebKitWebView *v, WebKitContextMenu *menu, WebKitHitTestResult *hit, Tab *t)
{
	GList *items = g_list_copy(webkit_context_menu_get_items(menu));
	int pos = 0;
	for (GList *l = items; l; l = l->next) {
		WebKitContextMenuItem *it = l->data;
		WebKitContextMenuAction a = webkit_context_menu_item_get_stock_action(it);
		/* 上一頁／下一頁／停止：換成我們的文字後 WebKit 不會再幫忙變灰，用不到的就不要列出來 */
		int useless = (a == WEBKIT_CONTEXT_MENU_ACTION_GO_BACK && !webkit_web_view_can_go_back(v)) ||
		              (a == WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD && !webkit_web_view_can_go_forward(v)) ||
		              (a == WEBKIT_CONTEXT_MENU_ACTION_STOP && !webkit_web_view_is_loading(v));
		if (useless || a == WEBKIT_CONTEXT_MENU_ACTION_INPUT_METHODS || a == WEBKIT_CONTEXT_MENU_ACTION_UNICODE) {
			webkit_context_menu_remove(menu, it);
			continue;
		}
		for (unsigned k = 0; k < LEN(ctx_labels); k++)
			if (ctx_labels[k].a == a) {
				webkit_context_menu_remove(menu, it);
				webkit_context_menu_insert(menu, webkit_context_menu_item_new_from_stock_action_with_label(a, _(ctx_labels[k].label)), pos);
				break;
			}
		pos++;
	}
	g_list_free(items);
	/* 網頁空白處或選取的文字（不是連結、圖片、影片、輸入框）：加上瀏覽器本身的常用功能 */
	ctx_tab = t;
	if (!webkit_hit_test_result_context_is_link(hit) && !webkit_hit_test_result_context_is_image(hit) &&
	    !webkit_hit_test_result_context_is_media(hit) && !webkit_hit_test_result_context_is_editable(hit)) {
		if (webkit_hit_test_result_context_is_selection(hit)) {
			char label[128];
			snprintf(label, sizeof label, _("用 %s 搜尋選取的文字"), engines[engine].name);
			ctx_add(menu, CA_SEARCH, label);
		} else {
			webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
			ctx_add(menu, CA_COPY_URL, _("複製網址"));
		}
		webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
		ctx_add(menu, CA_FIND, _("在網頁中尋找…"));
		ctx_add(menu, CA_PRINT, _("列印…"));
		webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
		ctx_add(menu, CA_ZOOMIN, _("放大"));
		ctx_add(menu, CA_ZOOMOUT, _("縮小"));
		ctx_add(menu, CA_ZOOM0, _("實際大小"));
		webkit_context_menu_append(menu, webkit_context_menu_item_new_separator());
		ctx_add(menu, CA_FULLSCREEN, t->w->fullscreen ? _("結束全螢幕") : _("全螢幕"));
	}
	/* 拿掉項目後留下的分隔線：開頭、結尾、連續兩條 */
	items = g_list_copy(webkit_context_menu_get_items(menu));
	int prev_sep = 1;
	for (GList *l = items; l; l = l->next) {
		int sep = webkit_context_menu_item_is_separator(l->data);
		if (sep && (prev_sep || !l->next)) { webkit_context_menu_remove(menu, l->data); continue; }
		prev_sep = sep;
	}
	g_list_free(items);
	GList *last = g_list_last(webkit_context_menu_get_items(menu));
	if (last && webkit_context_menu_item_is_separator(last->data)) webkit_context_menu_remove(menu, last->data);
	return !webkit_context_menu_get_n_items(menu);   /* 沒有項目就不要出現空的選單 */
}

static void on_close(WebKitWebView *v, Tab *t)
{
	int i = tab_index(t);
	if (i >= 0) tab_close(t->w, i);
}

static void on_hover(WebKitWebView *v, WebKitHitTestResult *h, guint mods, Tab *t)
{
	Win *w = t->w;
	if (t != cur_tab(w)) return;
	const char *link = webkit_hit_test_result_context_is_link(h) ? webkit_hit_test_result_get_link_uri(h) : NULL;
	if (link && !g_str_has_prefix(link, "javascript:")) {
		gtk_label_set_text(GTK_LABEL(w->status), link);
		gtk_widget_set_visible(w->status, TRUE);
	} else
		gtk_widget_set_visible(w->status, FALSE);
}

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *req, Tab *t)
{
	GUri *u = g_uri_parse(webkit_web_view_get_uri(v), G_URI_FLAGS_NONE, NULL);
	const char *host = u && g_uri_get_host(u) ? g_uri_get_host(u) : _("這個網站");
	const char *what = NULL;
	if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req)) what = _("%s 想知道你的位置");
	else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req)) what = _("%s 想要顯示通知");
	else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req)) {
		WebKitUserMediaPermissionRequest *m = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
		int vid = webkit_user_media_permission_is_for_video_device(m), aud = webkit_user_media_permission_is_for_audio_device(m);
		what = vid && aud ? _("%s 想要使用攝影機和麥克風") : vid ? _("%s 想要使用攝影機") : _("%s 想要使用麥克風");
	} else if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req)) {
		webkit_permission_request_allow(req);   /* 只是列出有哪些裝置（不會開啟），視訊會議網站都需要 */
		if (u) g_uri_unref(u);
		return TRUE;
	}
	if (!what) {
		webkit_permission_request_deny(req);
		if (u) g_uri_unref(u);
		return TRUE;
	}
	if (t->ask) webkit_permission_request_deny(t->ask);
	g_set_object(&t->ask, req);
	snprintf(t->ask_text, sizeof t->ask_text, what, host);
	if (u) g_uri_unref(u);
	if (t == cur_tab(t->w)) ask_update(t->w);
	return TRUE;
}

static void on_enter_fullscreen(WebKitWebView *v, Tab *t) { if (t == cur_tab(t->w)) set_fullscreen(t->w, 1); }
static gboolean on_fullscreen_signal(WebKitWebView *v, Tab *t) { on_enter_fullscreen(v, t); return FALSE; }
static gboolean on_leave_fullscreen(WebKitWebView *v, Tab *t) { if (t->w->fullscreen) set_fullscreen(t->w, 0); return FALSE; }

static void on_crash(WebKitWebView *v, WebKitWebProcessTerminationReason why, Tab *t)
{
	const char *uri = webkit_web_view_get_uri(v);
	show_error(t, uri ? uri : "", _("這個分頁停止運作了"),
		why == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? _("這個網頁用了太多記憶體。") : _("顯示這個網頁的程式意外結束。"),
		_("重新載入"), "location.reload()", NULL);
}

/* ---------- 下載 ---------- */

static void downloads_changed(void)
{
	for (GList *l = wins; l; l = l->next) gtk_widget_queue_draw(((Win *)l->data)->right);
}

static gboolean on_dl_dest(WebKitDownload *dl, const char *name, gpointer u)
{
	const char *dir = download_dir();
	g_mkdir_with_parents(dir, 0755);
	char *base = g_path_get_basename(name && *name ? name : "download");
	char *path = g_build_filename(dir, base, NULL);
	for (int i = 1; g_file_test(path, G_FILE_TEST_EXISTS) && i < 1000; i++) {
		g_free(path);
		const char *dot = strrchr(base, '.');
		int stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
		char *alt = g_strdup_printf("%.*s (%d)%s", stem, base, i, base + stem);
		path = g_build_filename(dir, alt, NULL);
		g_free(alt);
	}
	webkit_download_set_destination(dl, path);
	g_free(path);
	g_free(base);
	return TRUE;
}

static void on_dl_progress(WebKitDownload *dl, GParamSpec *p, gpointer u) { downloads_changed(); }

static void on_dl_finished(WebKitDownload *dl, gpointer u)
{
	if (g_object_get_data(G_OBJECT(dl), "pb-done")) return;   /* 失敗時 finished 也會來 */
	g_object_set_data(G_OBJECT(dl), "pb-done", GINT_TO_POINTER(1));
	const char *dest = webkit_download_get_destination(dl);
	char *name = dest ? g_path_get_basename(dest) : g_strdup("");
	notify(_("下載完成"), name);
	g_free(name);
	downloads_changed();
}

static void on_dl_failed(WebKitDownload *dl, GError *e, gpointer u)
{
	g_object_set_data(G_OBJECT(dl), "pb-done", GINT_TO_POINTER(1));
	if (!g_error_matches(e, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
		notify(_("下載失敗"), e->message);
	downloads_changed();
}

static void on_download(WebKitNetworkSession *s, WebKitDownload *dl, gpointer u)
{
	g_ptr_array_add(downloads, g_object_ref(dl));
	g_signal_connect(dl, "decide-destination", G_CALLBACK(on_dl_dest), NULL);
	g_signal_connect(dl, "notify::estimated-progress", G_CALLBACK(on_dl_progress), NULL);
	g_signal_connect(dl, "finished", G_CALLBACK(on_dl_finished), NULL);
	g_signal_connect(dl, "failed", G_CALLBACK(on_dl_failed), NULL);
	downloads_changed();
}

/* ---------- 分頁 ---------- */

static WebKitWebView *view_new(WebKitWebView *related)
{
	if (related) return WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, NULL));
	return WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", ctx, "network-session", session,
	                                    "settings", wsettings, NULL));
}

static gboolean on_view_scroll(GtkEventControllerScroll *c, double dx, double dy, Win *w)
{
	GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
	if (!(st & GDK_CONTROL_MASK) || dy == 0) return FALSE;
	zoom_by(w, dy < 0 ? 1.1 : 1 / 1.1);   /* Ctrl＋滾輪：縮放 */
	return TRUE;
}

static Tab *tab_add(Win *w, WebKitWebView *v, int front)
{
	if (w->n >= MAXTAB) { g_object_ref_sink(v); g_object_unref(v); return w->tabs[w->cur]; }
	Tab *t = g_new0(Tab, 1);
	t->w = w;
	t->view = v;
	int at = w->n ? w->cur + 1 : 0;
	memmove(&w->tabs[at + 1], &w->tabs[at], (w->n - at) * sizeof *w->tabs);
	w->tabs[at] = t;
	w->n++;
	if (at <= w->cur && w->n > 1) w->cur++;
	g_signal_connect(v, "notify::title", G_CALLBACK(on_title), t);
	g_signal_connect(v, "notify::uri", G_CALLBACK(on_uri), t);
	g_signal_connect(v, "notify::estimated-load-progress", G_CALLBACK(on_progress), t);
	g_signal_connect(v, "notify::is-loading", G_CALLBACK(on_progress), t);
	g_signal_connect(v, "notify::favicon", G_CALLBACK(on_favicon), t);
	g_signal_connect(v, "load-changed", G_CALLBACK(on_load_changed), t);
	g_signal_connect(v, "load-failed", G_CALLBACK(on_load_failed), t);
	g_signal_connect(v, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), t);
	g_signal_connect(v, "decide-policy", G_CALLBACK(on_decide), t);
	g_signal_connect(v, "create", G_CALLBACK(on_create), t);
	g_signal_connect(v, "context-menu", G_CALLBACK(on_context_menu), t);
	g_signal_connect(v, "close", G_CALLBACK(on_close), t);
	g_signal_connect(v, "mouse-target-changed", G_CALLBACK(on_hover), t);
	g_signal_connect(v, "permission-request", G_CALLBACK(on_permission), t);
	g_signal_connect(v, "enter-fullscreen", G_CALLBACK(on_fullscreen_signal), t);
	g_signal_connect(v, "leave-fullscreen", G_CALLBACK(on_leave_fullscreen), t);
	g_signal_connect(v, "web-process-terminated", G_CALLBACK(on_crash), t);
	WebKitFindController *fc = webkit_web_view_get_find_controller(v);
	g_signal_connect(fc, "counted-matches", G_CALLBACK(on_counted), w);
	g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_find_failed), w);
	GtkEventController *sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	gtk_event_controller_set_propagation_phase(sc, GTK_PHASE_CAPTURE);
	g_signal_connect(sc, "scroll", G_CALLBACK(on_view_scroll), w);
	gtk_widget_add_controller(GTK_WIDGET(v), sc);
	gtk_widget_set_vexpand(GTK_WIDGET(v), TRUE);
	gtk_widget_set_hexpand(GTK_WIDGET(v), TRUE);
	gtk_stack_add_child(GTK_STACK(w->stack), GTK_WIDGET(v));
	if (front || w->n == 1) tab_select(w, at);
	else update_chrome(w);
	return t;
}

static Tab *tab_new(Win *w, const char *uri, int front)
{
	Tab *t = tab_add(w, view_new(NULL), front);
	webkit_web_view_load_uri(t->view, uri ? uri : START);
	if (front && (!uri || !strcmp(uri, START))) focus_entry(w);
	return t;
}

static void tab_select(Win *w, int i)
{
	if (i < 0 || i >= w->n) return;
	Tab *old = cur_tab(w);
	if (old && old != w->tabs[i] && gtk_widget_get_visible(w->findbar)) show_find(w, 0);
	w->cur = i;
	Tab *t = w->tabs[i];
	gtk_stack_set_visible_child(GTK_STACK(w->stack), GTK_WIDGET(t->view));
	w->editing = 0;
	entry_show(w);
	ask_update(w);
	gtk_widget_set_visible(w->status, FALSE);
	update_chrome(w);
	const char *uri = webkit_web_view_get_uri(t->view);
	if (uri && !g_str_has_prefix(uri, START)) gtk_widget_grab_focus(GTK_WIDGET(t->view));
}

static void tab_free(Tab *t)
{
	if (t->ask) { webkit_permission_request_deny(t->ask); g_object_unref(t->ask); }
	g_clear_object(&t->tls_cert);
	g_clear_pointer(&t->icon, cairo_surface_destroy);
	g_signal_handlers_disconnect_by_data(t->view, t);
	g_signal_handlers_disconnect_by_data(webkit_web_view_get_find_controller(t->view), t->w);
	g_free(t);
}

static void tab_close(Win *w, int i)
{
	if (i < 0 || i >= w->n) return;
	Tab *t = w->tabs[i];
	const char *uri = webkit_web_view_get_uri(t->view);
	if (uri && *uri && !g_str_has_prefix(uri, START)) {
		if (nclosed == NCLOSED) { memmove(closed[0], closed[1], sizeof closed - sizeof closed[0]); nclosed--; }
		snprintf(closed[nclosed++], sizeof closed[0], "%s", uri);
	}
	gtk_stack_remove(GTK_STACK(w->stack), GTK_WIDGET(t->view));
	tab_free(t);
	memmove(&w->tabs[i], &w->tabs[i + 1], (w->n - i - 1) * sizeof *w->tabs);
	w->n--;
	if (!w->n) { gtk_window_destroy(w->win); return; }
	if (w->cur > i || w->cur >= w->n) w->cur = MAX(0, w->cur - 1);
	tab_select(w, w->cur);
}

/* ---------- 視窗：滑鼠 ---------- */

static void nav_motion(GtkEventControllerMotion *c, double x, double y, Win *w)
{
	int h = nav_hit(x);
	if (h != w->hover_nav) { w->hover_nav = h; gtk_widget_queue_draw(w->nav); }
}
static void nav_leave(GtkEventControllerMotion *c, Win *w) { w->hover_nav = -1; gtk_widget_queue_draw(w->nav); }
static void nav_click(GtkGestureClick *g, int n, double x, double y, Win *w)
{
	int i = nav_hit(x);
	if (i >= 0) nav_activate(w, i);
}

static void right_motion(GtkEventControllerMotion *c, double x, double y, Win *w)
{
	int h = right_hit(w, x);
	if (h != w->hover_right) { w->hover_right = h; gtk_widget_queue_draw(w->right); }
}
static void right_leave(GtkEventControllerMotion *c, Win *w) { w->hover_right = -1; gtk_widget_queue_draw(w->right); }
static void right_click(GtkGestureClick *g, int n, double x, double y, Win *w)
{
	int i = right_hit(w, x);
	if (i >= 0) right_activate(w, i);
}

static void tabs_motion(GtkEventControllerMotion *c, double x, double y, Win *w)
{
	int on_close = 0, h = tab_hit(w, x, y, &on_close);
	if (h != w->hover_tab || on_close != w->hover_close) { w->hover_tab = h; w->hover_close = on_close; gtk_widget_queue_draw(w->tabbar); }
}
static void tabs_leave(GtkEventControllerMotion *c, Win *w) { w->hover_tab = -1; gtk_widget_queue_draw(w->tabbar); }
static void tabs_click(GtkGestureClick *g, int n, double x, double y, Win *w)
{
	guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
	int on_close = 0, i = tab_hit(w, x, y, &on_close);
	if (i < 0) { if (btn == 1 && n == 2) tab_new(w, START, 1); return; }   /* 空白處按兩下：新分頁 */
	if (btn == 2 || (btn == 1 && on_close)) tab_close(w, i);
	else if (btn == 1) tab_select(w, i);
}
static gboolean tabs_scroll(GtkEventControllerScroll *c, double dx, double dy, Win *w)
{
	if (dy > 0) tab_select(w, MIN(w->n - 1, w->cur + 1));
	else if (dy < 0) tab_select(w, MAX(0, w->cur - 1));
	return TRUE;
}

static void find_motion(GtkEventControllerMotion *c, double x, double y, Win *w)
{
	int h = find_hit(w, x);
	if (h != w->hover_find) { w->hover_find = h; gtk_widget_queue_draw(w->findtools); }
}
static void find_leave(GtkEventControllerMotion *c, Win *w) { w->hover_find = -1; gtk_widget_queue_draw(w->findtools); }
static void find_click(GtkGestureClick *g, int n, double x, double y, Win *w)
{
	int i = find_hit(w, x);
	if (i == 0) find_run(w, -1);
	else if (i == 1) find_run(w, 1);
	else if (i == 2) show_find(w, 0);
}
static void on_find_changed(GtkEditable *e, Win *w) { find_run(w, 0); gtk_widget_queue_draw(w->findtools); }
static void on_find_activate(GtkEntry *e, Win *w) { find_run(w, 1); }

/* ---------- 鍵盤快速鍵 ---------- */

static gboolean on_key(GtkEventControllerKey *k, guint key, guint code, GdkModifierType st, Win *w)
{
	st &= gtk_accelerator_get_default_mod_mask();
	int ctrl = (st & GDK_CONTROL_MASK) != 0, shift = (st & GDK_SHIFT_MASK) != 0, alt = (st & GDK_ALT_MASK) != 0;
	Tab *t = cur_tab(w);
	guint lk = gdk_keyval_to_lower(key);
	if (ctrl && !alt) {
		switch (lk) {
		case GDK_KEY_t: if (shift) act_reopen(w); else act_newtab(w); return TRUE;
		case GDK_KEY_n: if (!shift) { act_newwin(w); return TRUE; } break;
		case GDK_KEY_w: case GDK_KEY_F4: tab_close(w, w->cur); return TRUE;
		case GDK_KEY_l: focus_entry(w); return TRUE;
		case GDK_KEY_r: if (t) { if (shift) webkit_web_view_reload_bypass_cache(t->view); else webkit_web_view_reload(t->view); } return TRUE;
		case GDK_KEY_f: show_find(w, 1); return TRUE;
		case GDK_KEY_g: find_run(w, shift ? -1 : 1); return TRUE;
		case GDK_KEY_p: act_print(w); return TRUE;
		case GDK_KEY_q: for (GList *l = wins; l; l = wins) gtk_window_destroy(((Win *)l->data)->win); return TRUE;
		case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: act_zoomin(w); return TRUE;
		case GDK_KEY_minus: case GDK_KEY_KP_Subtract: act_zoomout(w); return TRUE;
		case GDK_KEY_0: case GDK_KEY_KP_0: act_zoom0(w); return TRUE;
		case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: case GDK_KEY_Page_Down: case GDK_KEY_Page_Up: {
			int back = lk == GDK_KEY_ISO_Left_Tab || lk == GDK_KEY_Page_Up || (lk == GDK_KEY_Tab && shift);
			if (w->n > 1) tab_select(w, (w->cur + (back ? w->n - 1 : 1)) % w->n);
			return TRUE;
		}
		}
		if (lk >= GDK_KEY_1 && lk <= GDK_KEY_9) {
			tab_select(w, lk == GDK_KEY_9 ? w->n - 1 : (int)(lk - GDK_KEY_1));
			return TRUE;
		}
	}
	if (alt && !ctrl && t) {
		if (key == GDK_KEY_Left) { webkit_web_view_go_back(t->view); return TRUE; }
		if (key == GDK_KEY_Right) { webkit_web_view_go_forward(t->view); return TRUE; }
		if (key == GDK_KEY_Home) { webkit_web_view_load_uri(t->view, START); return TRUE; }
		if (lk == GDK_KEY_d) { focus_entry(w); return TRUE; }
	}
	if (!ctrl && !alt) {
		switch (key) {
		case GDK_KEY_F5: if (t) { if (shift) webkit_web_view_reload_bypass_cache(t->view); else webkit_web_view_reload(t->view); } return TRUE;
		case GDK_KEY_F6: focus_entry(w); return TRUE;
		case GDK_KEY_F11: act_fullscreen(w); return TRUE;
		case GDK_KEY_F3: find_run(w, shift ? -1 : 1); return TRUE;
		case GDK_KEY_Escape:
			if (gtk_widget_get_visible(w->findbar)) { show_find(w, 0); return TRUE; }
			if (gtk_widget_get_focus_child(w->entry)) { w->editing = 0; entry_show(w); if (t) gtk_widget_grab_focus(GTK_WIDGET(t->view)); return TRUE; }
			if (t && webkit_web_view_is_loading(t->view)) { webkit_web_view_stop_loading(t->view); return TRUE; }
			if (w->fullscreen && !shift) { set_fullscreen(w, 0); return FALSE; }
			break;
		}
	}
	return FALSE;
}

/* ---------- 視窗 ---------- */

static void apply_css(void)
{
	char css[6000];
	snprintf(css, sizeof css,
		"window.pomelo-browser{background:#%06x;}"
		".pb-tools{background:#%06x;border-bottom:1px solid #%06x;padding:0 6px;}"
		"entry.pb-url{min-height:30px;margin:7px 4px;border-radius:9px;border:1px solid transparent;outline:none;"
		"background:#%06x;color:#%06x;box-shadow:none;padding:0 10px;font-size:13px;}"
		"entry.pb-url:focus-within{border-color:#%06x;box-shadow:0 0 0 3px alpha(#%06x,0.25);background:#%06x;}"
		"entry.pb-url image{margin-right:4px;}"
		".pb-bar{background:#%06x;border-bottom:1px solid #%06x;padding:4px 10px;}"
		"entry.pb-findentry{min-height:26px;border-radius:7px;border:1px solid transparent;outline:none;background:#%06x;"
		"color:#%06x;box-shadow:none;padding:0 8px;font-size:13px;}"
		"entry.pb-findentry:focus-within{border-color:#%06x;}"
		".pb-bar label{color:#%06x;font-size:13px;}"
		"button.pb-btn{min-height:26px;padding:0 14px;margin-left:8px;border-radius:7px;border:none;outline:none;"
		"background:#%06x;color:#%06x;box-shadow:none;font-size:13px;}"
		"button.pb-btn:hover{background:#%06x;}"
		"button.pb-btn.pb-default{background:#%06x;color:#ffffff;}"
		"label.pb-status{background:alpha(#%06x,0.95);color:#%06x;border:1px solid #%06x;border-radius:7px;"
		"padding:3px 9px;margin:6px;font-size:12px;}"
		"popover.pb-menu>contents{padding:0;border-radius:11px;background:#%06x;border:1px solid alpha(#%06x,0.12);"
		"box-shadow:0 8px 28px alpha(black,0.22);}"
		"label.pb-title{font-weight:600;font-size:15px;}label.pb-dim{opacity:0.62;}",
		col_bar, col_bar, col_sep, col_field, col_text, col_accent, col_accent, col_tab,
		col_bar, col_sep, col_field, col_text, col_accent, col_text,
		col_field, col_text, col_hover, col_accent,
		col_tab, col_text, col_sep, col_menu, col_text);
	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_data(p, css, -1);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
	                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static GtkWidget *area(int width, int height, GtkDrawingAreaDrawFunc draw, Win *w,
                       GCallback motion, GCallback leave, GCallback click)
{
	GtkWidget *a = gtk_drawing_area_new();
	if (width > 0) gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(a), width);
	if (height > 0) gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(a), height);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(a), draw, w, NULL);
	if (motion) {
		GtkEventController *mc = gtk_event_controller_motion_new();
		g_signal_connect(mc, "motion", motion, w);
		g_signal_connect(mc, "leave", leave, w);
		gtk_widget_add_controller(a, mc);
	}
	if (click) {
		GtkGesture *g = gtk_gesture_click_new();
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
		g_signal_connect(g, "released", click, w);
		gtk_widget_add_controller(a, GTK_EVENT_CONTROLLER(g));
	}
	return a;
}

static gboolean on_close_request(GtkWindow *gw, Win *w)
{
	int wd, ht;
	gtk_window_get_default_size(gw, &wd, &ht);
	if (!w->fullscreen && !gtk_window_is_maximized(gw) && wd > 200 && ht > 200) {
		pm_conf_set_int("browser.width", wd);
		pm_conf_set_int("browser.height", ht);
	}
	while (w->n) {
		Tab *t = w->tabs[--w->n];
		gtk_stack_remove(GTK_STACK(w->stack), GTK_WIDGET(t->view));
		tab_free(t);
	}
	return FALSE;
}

static void on_destroy(GtkWidget *gw, Win *w)
{
	if (w->menu) gtk_widget_unparent(w->menu);
	if (w->certpop) gtk_widget_unparent(w->certpop);
	wins = g_list_remove(wins, w);
	g_free(w);
}

static Win *win_new(void)
{
	Win *w = g_new0(Win, 1);
	w->hover_nav = w->hover_right = w->hover_tab = w->hover_find = w->hover_menu = -1;
	w->win = GTK_WINDOW(gtk_application_window_new(app));
	g_signal_connect(w->win, "realize", G_CALLBACK(set_app_id), NULL);
	gtk_widget_add_css_class(GTK_WIDGET(w->win), "pomelo-browser");
	gtk_window_set_icon_name(w->win, "pomelo-browser");
	gtk_window_set_title(w->win, _("小柚瀏覽器"));
	gtk_window_set_default_size(w->win, pm_conf_int("browser.width", 1180), pm_conf_int("browser.height", 800));

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(w->win, box);

	/* 工具列：上一頁 下一頁 重新載入 ｜ 網址列 ｜（下載）新分頁 選單 */
	w->tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(w->tools, "pb-tools");
	w->nav = area(3 * NAV_BW + 2, TOOL_H, draw_nav, w, G_CALLBACK(nav_motion), G_CALLBACK(nav_leave), G_CALLBACK(nav_click));
	w->entry = gtk_entry_new();
	gtk_widget_add_css_class(w->entry, "pb-url");
	gtk_widget_set_hexpand(w->entry, TRUE);
	gtk_entry_set_placeholder_text(GTK_ENTRY(w->entry), _("搜尋或輸入網址"));
	gtk_entry_set_input_hints(GTK_ENTRY(w->entry), GTK_INPUT_HINT_NO_SPELLCHECK);
	g_signal_connect(w->entry, "activate", G_CALLBACK(on_entry_activate), w);
	g_signal_connect(w->entry, "changed", G_CALLBACK(on_entry_changed), w);
	GtkEventController *fc = gtk_event_controller_focus_new();
	g_signal_connect(fc, "leave", G_CALLBACK(on_entry_focus_leave), w);
	gtk_widget_add_controller(w->entry, fc);
	GtkGesture *icon = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(icon), GDK_BUTTON_SECONDARY);   /* 右鍵；左鍵在 on_entry_press */
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(icon), GTK_PHASE_CAPTURE);
	g_signal_connect(icon, "pressed", G_CALLBACK(on_entry_icon_press), w);
	gtk_widget_add_controller(w->entry, GTK_EVENT_CONTROLLER(icon));
	GtkGesture *press = gtk_gesture_click_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
	g_signal_connect(press, "pressed", G_CALLBACK(on_entry_press), w);
	gtk_widget_add_controller(w->entry, GTK_EVENT_CONTROLLER(press));
	w->right = area(3 * NAV_BW + 2, TOOL_H, draw_right, w, G_CALLBACK(right_motion), G_CALLBACK(right_leave), G_CALLBACK(right_click));
	gtk_box_append(GTK_BOX(w->tools), w->nav);
	gtk_box_append(GTK_BOX(w->tools), w->entry);
	gtk_box_append(GTK_BOX(w->tools), w->right);
	gtk_box_append(GTK_BOX(box), w->tools);

	w->tabbar = area(-1, TAB_H, draw_tabs, w, G_CALLBACK(tabs_motion), G_CALLBACK(tabs_leave), G_CALLBACK(tabs_click));
	GtkEventController *ts = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
	g_signal_connect(ts, "scroll", G_CALLBACK(tabs_scroll), w);
	gtk_widget_add_controller(w->tabbar, ts);
	gtk_widget_set_visible(w->tabbar, FALSE);
	gtk_box_append(GTK_BOX(box), w->tabbar);

	/* 尋找列 */
	w->findbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(w->findbar, "pb-bar");
	w->findentry = gtk_entry_new();
	gtk_widget_add_css_class(w->findentry, "pb-findentry");
	gtk_entry_set_placeholder_text(GTK_ENTRY(w->findentry), _("在網頁中尋找"));
	gtk_widget_set_size_request(w->findentry, 260, -1);
	g_signal_connect(w->findentry, "changed", G_CALLBACK(on_find_changed), w);
	g_signal_connect(w->findentry, "activate", G_CALLBACK(on_find_activate), w);
	w->findtools = area(-1, 30, draw_findtools, w, G_CALLBACK(find_motion), G_CALLBACK(find_leave), G_CALLBACK(find_click));
	gtk_widget_set_hexpand(w->findtools, TRUE);
	gtk_box_append(GTK_BOX(w->findbar), w->findentry);
	gtk_box_append(GTK_BOX(w->findbar), w->findtools);
	gtk_widget_set_visible(w->findbar, FALSE);
	gtk_box_append(GTK_BOX(box), w->findbar);

	/* 詢問列 */
	w->askbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(w->askbar, "pb-bar");
	w->asklabel = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(w->asklabel), 0);
	gtk_label_set_wrap(GTK_LABEL(w->asklabel), TRUE);
	gtk_widget_set_hexpand(w->asklabel, TRUE);
	w->askno = gtk_button_new_with_label("");
	w->askyes = gtk_button_new_with_label("");
	gtk_widget_add_css_class(w->askno, "pb-btn");
	gtk_widget_add_css_class(w->askyes, "pb-btn");
	gtk_widget_add_css_class(w->askyes, "pb-default");
	g_signal_connect(w->askno, "clicked", G_CALLBACK(on_ask_no), w);
	g_signal_connect(w->askyes, "clicked", G_CALLBACK(on_ask_yes), w);
	gtk_box_append(GTK_BOX(w->askbar), w->asklabel);
	gtk_box_append(GTK_BOX(w->askbar), w->askno);
	gtk_box_append(GTK_BOX(w->askbar), w->askyes);
	gtk_widget_set_visible(w->askbar, FALSE);
	gtk_box_append(GTK_BOX(box), w->askbar);

	/* 網頁：疊上讀取進度線、滑鼠移到連結時左下角的網址 */
	w->overlay = gtk_overlay_new();
	gtk_widget_set_vexpand(w->overlay, TRUE);
	w->stack = gtk_stack_new();
	gtk_overlay_set_child(GTK_OVERLAY(w->overlay), w->stack);
	w->progress = area(-1, 2, draw_progress, w, NULL, NULL, NULL);
	gtk_widget_set_valign(w->progress, GTK_ALIGN_START);
	gtk_widget_set_can_target(w->progress, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(w->overlay), w->progress);
	w->status = gtk_label_new("");
	gtk_widget_add_css_class(w->status, "pb-status");
	gtk_label_set_ellipsize(GTK_LABEL(w->status), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(w->status), 80);
	gtk_widget_set_halign(w->status, GTK_ALIGN_START);
	gtk_widget_set_valign(w->status, GTK_ALIGN_END);
	gtk_widget_set_can_target(w->status, FALSE);
	gtk_widget_set_visible(w->status, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(w->overlay), w->status);
	gtk_box_append(GTK_BOX(box), w->overlay);

	GtkEventController *kc = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
	g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), w);
	gtk_widget_add_controller(GTK_WIDGET(w->win), kc);
	g_signal_connect(w->win, "close-request", G_CALLBACK(on_close_request), w);
	g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);
	wins = g_list_append(wins, w);
	return w;
}

/* ---------- 程式啟動 ---------- */

static void on_startup(GApplication *a, gpointer unused)
{
	GdkDisplay *d = gdk_display_get_default();
	if (GDK_IS_X11_DISPLAY(d)) gdk_x11_display_set_program_class(d, "pomelo-browser");
	/* Pomelo 輸入法：原生 Wayland 時 GTK4 自己用 text-input（compositor 轉給 pomelo-input）；
	 * 走 X11 時 GTK4 沒有 XIM，改用 IBus 模組跟 pomelo-input 溝通。要等顯示開好才知道是哪一種 */
	const char *xm = getenv("XMODIFIERS");
	if (GDK_IS_X11_DISPLAY(d) && xm && strstr(xm, "@im=pomelo")) {
		setenv("GTK_IM_MODULE", "ibus", 1);
		/* 一定要同步模式：非同步模式下輸入法沒吃掉的鍵（Enter、Backspace、Shift＋字母、Ctrl+A…）
		 * GTK4 送不回輸入框，網址列只打得出小寫字母（Debian 13 的 ibus-gtk4 預設是非同步） */
		setenv("IBUS_ENABLE_SYNC_MODE", "1", 1);
	}
	pm_conf_load();
	pm_family = g_strdup(pm_conf_get("appearance.font", "Noto Sans CJK TC"));
	pm_size = pm_conf_num("appearance.font_size", 13);
	pm_icon_theme = g_strdup(pm_conf_get("appearance.icon_theme", "Yaru"));
	pm_set_theme(pm_conf_int("appearance.dark", 0), (unsigned)strtoul(pm_conf_get("appearance.accent", "#0a64d8") + 1, NULL, 16));
	colors();
	const char *e = pm_conf_get("browser.search", "Google");
	for (unsigned i = 0; i < LEN(engines); i++) if (!g_ascii_strcasecmp(e, engines[i].name)) engine = i;

	char font[200];
	snprintf(font, sizeof font, "%s %.0f", pm_family, pm_size * 0.75 + 0.5);   /* pm_size 是像素，GTK 用點 */
	g_object_set(gtk_settings_get_default(), "gtk-font-name", font, "gtk-application-prefer-dark-theme", pm_dark, NULL);
	apply_css();
	tex_lock = icon_texture(IC_LOCK, col_dim);
	tex_warn = icon_texture(IC_WARN, col_dim);

	char *data = g_build_filename(g_get_user_data_dir(), "pomelo-browser", NULL);
	char *cache = g_build_filename(g_get_user_cache_dir(), "pomelo-browser", NULL);
	session = webkit_network_session_new(data, cache);
	char *cookies = g_build_filename(data, "cookies.sqlite", NULL);
	webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(session), cookies,
	                                             WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	webkit_website_data_manager_set_favicons_enabled(webkit_network_session_get_website_data_manager(session), TRUE);
	g_signal_connect(session, "download-started", G_CALLBACK(on_download), NULL);
	g_free(cookies);
	g_free(data);
	g_free(cache);

	/* 網頁行程用超過約 340 MB 時開始釋放快取（圖片、JavaScript 編譯結果…），超過 512 MB 時釋放得更積極
	 * （上限 1024 MB 的 1/3 與 1/2；沒設的話上限是整台電腦的記憶體，幾乎不會主動釋放） */
	WebKitMemoryPressureSettings *mps = webkit_memory_pressure_settings_new();
	webkit_memory_pressure_settings_set_memory_limit(mps, 1024);
	ctx = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mps, NULL));
	webkit_memory_pressure_settings_free(mps);
	webkit_web_context_set_cache_model(ctx, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);   /* 快取小一點，省記憶體 */
	webkit_web_context_register_uri_scheme(ctx, "pomelo", scheme_cb, NULL, NULL);
	webkit_security_manager_register_uri_scheme_as_secure(webkit_web_context_get_security_manager(ctx), "pomelo");
	/* 網頁語言：跟 Pomelo 的介面語言（例如 zh_TW → zh-TW、zh、en） */
	char lang[32];
	snprintf(lang, sizeof lang, "%s", pm_lang());
	for (char *p = lang; *p; p++) if (*p == '_') *p = '-';
	char shortl[32];
	snprintf(shortl, sizeof shortl, "%.*s", (int)strcspn(lang, "-"), lang);
	const char *langs[] = { lang, strcmp(lang, shortl) ? shortl : "en", "en", NULL };
	webkit_web_context_set_preferred_languages(ctx, langs);

	wsettings = webkit_settings_new();
	/* 開發者工具（右鍵「檢查元素」）：一般使用者用不到，設定檔 browser.devtools=1 才開 */
	webkit_settings_set_enable_developer_extras(wsettings, pm_conf_int("browser.devtools", 0));
	webkit_settings_set_enable_smooth_scrolling(wsettings, TRUE);
	webkit_settings_set_enable_back_forward_navigation_gestures(wsettings, TRUE);
	webkit_settings_set_javascript_can_open_windows_automatically(wsettings, FALSE);
	/* 省記憶體：換網站時不留著舊網頁（「上一頁」改成從快取重新顯示，不用多一個行程等著）；沒有顯示卡就不用 GPU 合成 */
	webkit_settings_set_enable_page_cache(wsettings, FALSE);
	if (!gpu) webkit_settings_set_hardware_acceleration_policy(wsettings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);

	downloads = g_ptr_array_new_with_free_func(g_object_unref);
	hist_load();
}

static void on_shutdown(GApplication *a, gpointer unused)
{
	if (hist_save_id) { g_source_remove(hist_save_id); hist_save(NULL); }
}

static Win *active_win(void)
{
	GtkWindow *aw = gtk_application_get_active_window(app);
	for (GList *l = wins; l; l = l->next) if (((Win *)l->data)->win == aw) return l->data;
	return wins ? wins->data : NULL;
}

/* 每次執行 pomelo-browser（包括已經在跑時再執行一次）都到這裡：參數是網址或檔案就開成分頁 */
static int on_command_line(GApplication *a, GApplicationCommandLine *cl, gpointer unused)
{
	int argc;
	char **argv = g_application_command_line_get_arguments(cl, &argc);
	int newwin = 0;
	for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--new-window")) newwin = 1;
	Win *w = newwin ? NULL : active_win();
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1] == '-') continue;
		char *uri = NULL;
		GFile *f = g_application_command_line_create_file_for_arg(cl, argv[i]);
		if (!strstr(argv[i], "://") && g_file_query_exists(f, NULL)) uri = g_file_get_uri(f);
		else uri = to_uri(argv[i]);
		g_object_unref(f);
		if (!uri) continue;
		if (!w) w = win_new();
		tab_new(w, uri, 1);
		g_free(uri);
	}
	if (!w) w = win_new();
	if (!w->n) tab_new(w, START, 1);
	gtk_window_present(w->win);
	g_strfreev(argv);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) {
		cairo_surface_t *s = app_icon(256);
		int ok = cairo_surface_write_to_png(s, argv[2]) == CAIRO_STATUS_SUCCESS;
		cairo_surface_destroy(s);
		return ok ? 0 : 1;
	}
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		printf("%s: pomelo-browser [--new-window] [%s…]\n", _("用法"), _("網址"));
		return 0;
	}
	setlocale(LC_ALL, "");
	g_set_prgname("pomelo-browser");
	g_set_application_name(_("小柚瀏覽器"));
	/* 0.40 起 Pomelo 的工作階段有原生 Wayland（compositor 支援 xdg-shell 與輸入法）就用 Wayland */
	gdk_set_allowed_backends(getenv("WAYLAND_DISPLAY") ? "wayland,x11,*" : "x11,*");
	if (sandbox_blocked()) return sandbox_explain();
	gpu = have_gpu() && !getenv("POMELO_BROWSER_NO_GPU");
	if (!gpu) setenv("GSK_RENDERER", "cairo", 0);
	app = gtk_application_new(APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
	g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
	g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
	g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
	int r = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return r;
}
