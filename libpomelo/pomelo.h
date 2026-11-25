/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo.h — libpomelo：Pomelo 各程式共用的繪圖、圖示、選單、螢幕、設定與介面元件
 *
 * 編譯：pkg-config --cflags --libs pomelo
 * 使用前先 XOpenDisplay 指定給 dpy，再呼叫 pm_init()。
 */
#ifndef POMELO_H
#define POMELO_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrandr.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pomelo-settings.h"

#define POMELO_VERSION "0.42"
#define POMELO_DATE    "2026-11-07"   /* 發行日期（跟 CHANGELOG.txt 這一版的日期一樣） */

#define LEN(a)         (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b)      ((a) > (b) ? (a) : (b))
#define MIN(a, b)      ((a) < (b) ? (a) : (b))
#define CLAMP(v, a, b) ((v) < (a) ? (a) : (v) > (b) ? (b) : (v))
#define MOUSEMASK      (ButtonPressMask | ButtonReleaseMask | PointerMotionMask)

typedef union { int i; const char *s; } Arg;
typedef struct { const char *label; void (*fn)(const Arg *); Arg arg; } MenuItem;
typedef struct { int x, y, w, h; } Rect;
typedef struct { Window w; Pixmap pm; cairo_surface_t *s; int width, height; } Paint;

typedef struct {
	unsigned int border, title, title_in, text, text_in, sep, bar, bar_text,
	             dock, dock_line, menu, menu_text, sel, sel_text, dim, btn_in;
} Theme;

/* 共用狀態（程式中用短名稱存取） */
#define dpy      pm_dpy
#define scr      pm_scr
#define sw       pm_sw
#define sh       pm_sh
#define shape_ok pm_shape_ok
#define rr_base  pm_rr_base
#define gc       pm_gc
#define T        pm_T
#define mons     pm_mons
#define nmons    pm_nmons
#define pmon     pm_pmon

extern Display *dpy;
extern int scr, sw, sh, shape_ok, rr_base;
extern Window pm_root;
extern GC gc;
extern const Theme *T;
extern Cursor pm_cursor;
extern const char *pm_family;
extern double pm_size;
extern int pm_dark;
extern const char *pm_icon_theme;
extern void (*pm_dispatch)(XEvent *);   /* 模態迴圈中，交給主程式處理其他事件 */
extern Rect mons[16];
extern int nmons, pmon;

/* 主題：dark = 深色；accent = 強調色（0 = 預設藍） */
void pm_set_theme(int dark, unsigned accent);

void pm_init(const char *family, double size, int dark, const char *icon_theme);
void pm_spawn(const char *cmd);
int pm_getatoms(Window w, Atom prop, Atom *out, int max);
void pm_send(Window to, Window about, Atom type, long d0, long d1, long d2);
/* 登出：請 pomelo-session 結束工作階段（跟 pomelo-session --logout 一樣）；
 * 不是在 pomelo-session 底下（例如單獨跑 pomelo-wm）才改請視窗管理器結束 */
void pm_logout(void);
/* 是不是在 pomelo-compositor 底下（它在 root 掛 _POMELO_COMPOSITOR）：桌布、陰影與動畫改由誰負責不一樣 */
int pm_under_compositor(void);
void pm_title(Window w, char *out, size_t n);
cairo_surface_t *pm_net_icon(Window win, int size);
long pm_now_ms(void);
int pm_mon_at(int x, int y);
int pm_pointer_mon(void);
void pm_update_monitors(void);
void pm_rgb(cairo_t *cr, unsigned int c);
void pm_rgba(cairo_t *cr, unsigned int c, double a);
void pm_rrect(cairo_t *cr, double x, double y, double w, double h, double rt, double rb);
void pm_font(cairo_t *cr, double size, int bold);
double pm_textw(cairo_t *cr, const char *s);
/* 文字繪製與量測的字型後援（泰文：Noto Sans CJK 沒有泰文字形，cairo 的 toy API 又不會自己換字型；見 textfb.c）。
 * 含有泰文的字串會自動換用泰文字型畫，其他字串照舊；程式照常呼叫 cairo_show_text／cairo_text_extents 即可。 */
void pm_show_text(cairo_t *cr, const char *s);
void pm_text_extents(cairo_t *cr, const char *s, cairo_text_extents_t *e);
#ifndef POMELO_NO_TEXT_FALLBACK
#define cairo_show_text(cr, s) pm_show_text((cr), (s))
#define cairo_text_extents(cr, s, e) pm_text_extents((cr), (s), (e))
#endif
void pm_ellipsize(cairo_t *cr, const char *s, double maxw, char *out, size_t n);
/* s 的前 len 個位元組，一行（寬 maxw）放得下多少位元組：盡量在能換行的地方斷——空白後面（英文、韓文、越南文…）、
 * 中日文任何字之間（「，。」不放行首）、泰文字與字之間（不拆開上下的母音與聲調符號）；一個詞比整行還長才硬切。
 * 至少回傳一個字元的長度。下一行開頭的空白由呼叫者跳過。 */
size_t pm_text_fit(cairo_t *cr, const char *s, size_t len, double maxw);
void pm_text(cairo_t *cr, double x, double yc, const char *s);
cairo_t *pm_paint_begin(Paint *p, Window w, int width, int height);
void pm_paint_end(Paint *p, cairo_t *cr, int dx, int dy);
void pm_shape_round(Window w, int width, int height, int rt, int rb);
/* 狀態列右邊的縮放控制（−  ━━●━━  ＋  100%），文書、試算表、簡報共用。lo、hi 是縮放範圍（1＝100%，
 * 滑桿中間是 100%），呼叫端先填好；pm_zoom_draw 畫在 (x, y) 高 h 的列裡，寬度是 pm_zoom_width()。
 * pm_zoom_click 回傳 PM_ZOOM_SET（*out 是新的比例）、PM_ZOOM_DRAG（同上，而且開始拖滑桿，之後拖動用
 * pm_zoom_slider_at）、PM_ZOOM_MENU（點了百分比，呼叫端自己開比例選單）或 PM_ZOOM_NONE。 */
typedef struct { Rect minus, plus, slider, pct; double lo, hi; } PmZoomBar;
enum { PM_ZOOM_NONE, PM_ZOOM_SET, PM_ZOOM_DRAG, PM_ZOOM_MENU };
double pm_zoom_width(cairo_t *cr);
void pm_zoom_draw(cairo_t *cr, PmZoomBar *zb, double x, double y, double h, double zoom);
int pm_zoom_click(PmZoomBar *zb, int x, int y, double zoom, double *out);
double pm_zoom_slider_at(const PmZoomBar *zb, int x);
/* 要用 pm_shape_round 裁圓角的 pm_root 子視窗：有合成器時建成 ARGB 視窗，角落會是透明的
 * （只靠 Shape 的話 XWayland 底下會露出黑角）。要用 pm_paint_begin／pm_paint_end 畫，
 * 用 pm_destroy_window 關掉。 */
Window pm_round_window(int x, int y, int w, int h, unsigned long mask, XSetWindowAttributes *wa);
void pm_destroy_window(Window w);
int pm_icon_find(const char *name, const char *ext, char *out, size_t n, int want);
cairo_surface_t *pm_icon_scale(cairo_surface_t *src, int w, int h, int size);
cairo_surface_t *pm_icon_file(const char *path, int size);
cairo_surface_t *pm_icon_load(const char *name, int size);
int pm_modal_next(XEvent *ev);
/* 彈出選單。label 可以寫成「名稱\t快捷鍵」（快捷鍵靠右、淡色顯示）；開頭加 \1 是打勾的項目、\2 是沒勾的；
 * 名稱的部分會用 pm_tr 翻譯。fn 設成 pm_menu_off 的項目是灰色、不能選。
 * 按 ←／→ 會關掉選單並把 pm_popup_dir 設成 -1／1（選單列用來換到旁邊的選單；呼叫前先設成 0） */
void pm_popup(const MenuItem *m, int n, int x, int y);
void pm_menu_off(const Arg *a);
extern int pm_popup_dir;


/* 圖片：PNG、JPEG（其他格式回傳 NULL） */
cairo_surface_t *pm_image_load(const char *path);
/* 縮放：mode 0 = 填滿裁切，1 = 完整顯示，2 = 拉伸 */
cairo_surface_t *pm_image_fit(cairo_surface_t *src, int w, int h, int mode);
cairo_surface_t *pm_image_load_fit(const char *path, int w, int h, int mode);
/* 桌布：同上，但縮好的結果存在 ~/.cache/pomelo/wallpaper/，下次登入直接讀（大圖解壓很慢） */
cairo_surface_t *pm_wallpaper_load(const char *path, int w, int h, int mode);
/* 縮圖快取（~/.cache/pomelo/wpthumb/，鍵含路徑、修改時間、大小、尺寸）：cached 只讀快取（沒有回傳 NULL，很快），
 * load 沒有就解碼並存起來（快取目錄不能寫時等於 pm_image_load_fit）。多程式同時寫入安全，損壞自動重建，超過上限清最舊的 */
cairo_surface_t *pm_thumb_cached(const char *path, int w, int h, int mode);
cairo_surface_t *pm_thumb_load(const char *path, int w, int h, int mode);
/* 一個螢幕（w×h）的桌布：設定的圖片或 Pomelo 漸層（跟桌面上畫的一樣）；設定成不要桌布回傳 NULL */
cairo_surface_t *pm_desktop_image(int w, int h);
cairo_surface_t *pm_image_jpeg_mem(const void *data, size_t len, int maxdim);   /* 例如攝影機的 MJPEG 畫面 */

/* 已安裝的應用程式（.desktop 檔），依名稱排序；第一次呼叫時掃描 */
typedef struct {
	char *id, *path, *name, *name_en, *generic, *comment, *exec, *icon, *keywords, *wmclass, *categories;
	int terminal, hidden;
} PmApp;
PmApp *pm_apps(int *count);
void pm_apps_free(void);                                  /* 清掉，下次重新掃描 */
PmApp *pm_app_by_id(const char *id);                      /* 例如 "firefox"、"org.gnome.Nautilus" */
PmApp *pm_app_by_class(const char *cls, const char *inst); /* 用視窗的 WM_CLASS 找 */
int pm_app_match(const PmApp *a, const char *query);      /* 搜尋分數，0 = 不符合 */
/* 應用程式分類（啟動器的頁籤）：工具、辦公、文書、娛樂、生產力、其他。
 * 依 .desktop 的 Categories 自動對應（Pomelo 自己的程式可加 X-Pomelo-Tools／Office／Docs／Fun／Productivity／Other 蓋過去）；
 * 使用者在啟動器右鍵「移到分類」改過的記在設定 launcher.cat.<程式 ID>（0～5），優先於自動對應。 */
enum { PM_CAT_TOOLS, PM_CAT_OFFICE, PM_CAT_DOCS, PM_CAT_FUN, PM_CAT_PROD, PM_CAT_OTHER, PM_CAT_N };
const char *pm_category_name(int cat);                    /* 顯示用的名稱（已翻譯）；cat = -1 是「全部」 */
int pm_app_auto_category(const PmApp *a);                 /* 只看 Categories 的自動分類 */
int pm_app_category(const PmApp *a);                      /* 實際分類（先看使用者改過的） */
/* 某個分類（cat = -1 是全部）的程式，依使用者排的順序（設定 launcher.order.<分類>），沒排過的依名稱接在後面；
 * hidden_mode：0 只列顯示的、1 全部、2 只列隱藏的。結果指到 pm_apps() 的陣列裡，回傳個數 */
int pm_apps_in_category(int cat, int hidden_mode, PmApp **out, int max);
/* 存下某個分類的順序（n = 0 或 list = NULL 就清掉，回到依名稱排） */
void pm_app_order_set(int cat, PmApp *const *list, int n);
void pm_app_launch(const PmApp *a);
/* 用預設的程式開啟檔案、資料夾或網址（open.c，取代 xdg-open）；0 = 交給某個程式了 */
int pm_open(const char *target);
/* 某個類型（image/png、x-scheme-handler/https…）的預設程式 ID（不含 .desktop）；1 = 找到 */
int pm_default_app(const char *mime, char *out, size_t n);

/* 套件管理（pkg.c）：執行時判斷用 apt（Debian、Ubuntu）還是 dnf（Fedora、Rocky、RHEL）。
 * 參數一律寫 Debian 的套件名稱，dnf 的系統上會自動換成那邊的名稱（那邊沒有的就略過）。 */
enum { PM_PKG_NONE, PM_PKG_APT, PM_PKG_DNF };
int pm_pkg_kind(void);
int pm_os_like(const char *id);                     /* /etc/os-release 的 ID 或 ID_LIKE 有沒有 id，例如 "rhel" */
const char *pm_pkg_name(const char *deb);           /* 這個系統上的套件名稱；沒有這個套件回傳 NULL */
/* 給 pkexec 執行的指令：action 是 install、install-min（不裝建議套件）、remove、refresh、upgrade、autoremove、clean。
 * argv 的字串是靜態的或來自 debs；回傳參數個數，0 = 這個系統做不到 */
int pm_pkg_argv(const char *action, const char *const *debs, int n, char **argv, int max);
char *pm_pkg_hint(const char *debs);                /* 「sudo apt install a b」或「sudo dnf install …」（空白分隔）；呼叫者 free */
char *pm_pkg_cmdline(const char *action, const char *debs);   /* 給終端機執行的完整指令（含 -y）；呼叫者 free */
void pm_pkg_check(const char *const *debs, int n, int *installed);
int pm_pkg_installed(const char *deb);
char *pm_pkg_upgradable(void);                      /* 每行「名稱<Tab>新版本」；呼叫者 free */
char *pm_pkg_owners(const char *const *paths, int n);   /* 每行「檔案路徑<Tab>套件」；呼叫者 free */

/* 選單列左上角的 Pomelo 選單（使用者可以編輯，存在 ~/.config/pomelo/menu.conf；見 logomenu.c） */
enum { PM_MENU_ITEM, PM_MENU_APP, PM_MENU_CMD, PM_MENU_SEP };
typedef struct { int kind; char arg[512]; char label[128]; } PmMenuEntry;   /* label 空的 = 用預設名稱 */
typedef struct { const char *id, *label; } PmMenuBuiltin;                   /* label 是還沒翻譯的原文 */
#define PM_MENU_MAX 64
extern const PmMenuBuiltin pm_menu_builtins[];
extern const int pm_menu_nbuiltins;
int pm_menu_load(PmMenuEntry *e, int max);          /* 沒有 menu.conf 就給預設選單；回傳項目數 */
int pm_menu_save(const PmMenuEntry *e, int n);      /* 0 = 成功 */
int pm_menu_customized(void);                       /* 使用者改過（有 menu.conf） */
void pm_menu_reset(void);                           /* 回到預設選單 */
const char *pm_menu_builtin_label(const char *id);  /* 內建項目的原文名稱，不認得回傳 NULL */
const char *pm_menu_label(const PmMenuEntry *e);    /* 顯示用的名稱（已翻譯） */
/* 在桌面（桌布上）按右鍵的選單：換桌布、外觀、顯示器、開終端機…（x、y 是螢幕座標） */
void pm_desktop_menu(int x, int y);
const MenuItem *pm_desktop_menu_items(int *n);   /* 同一份項目（原生面板用 pmwl_menu 開） */

/* 磁碟與外接裝置（透過 udisks2，見 disks.c） */
typedef struct {
	char obj[192], dev[64], model[128], vendor[64], bus[16];
	unsigned long long size;
	int removable, ejectable, can_power_off, rotation, external, in_use_by_system;
	int smart, failing, attrs_failing, nvme;      /* 健康狀態（SMART） */
	long bad_sectors, power_on_hours;              /* -1 = 不知道 */
	double temp_c;                                 /* < -100 = 不知道 */
} PmDrive;
typedef struct {
	char obj[192], dev[64], drive[192], table[192];
	char label[128], fstype[32], uuid[64], mount[512];
	unsigned long long size;
	int is_fs, is_part, has_table, is_loop, whole, crypto;
	int ignore, system, hint_auto, external;
	PmDrive *d;
} PmVolume;
int pm_disks_scan(void);                 /* 重新讀取；失敗（沒有 udisks2）回傳 -1 */
PmDrive *pm_drives(int *n);
PmVolume *pm_volumes(int *n);
PmVolume *pm_volume_by_dev(const char *dev);
const char *pm_volume_name(const PmVolume *v, char *buf, size_t n);
void pm_human_size(char *out, size_t n, double bytes);   /* 1000 進位，例如 "32 GB" */
int pm_volume_mount(const char *dev, char *mountpoint, size_t n, char *err, size_t en);
int pm_volume_unmount(const char *dev, char *err, size_t en);
int pm_drive_eject(const char *drive_obj, char *err, size_t en);
int pm_drive_format(const char *drive_obj, const char *fstype, const char *label, char *err, size_t en);

#include "pomelo-dnd.h"     /* 收拖進來的檔案 */
#include "pomelo-widgets.h"

#endif
