/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-wl.h — 原生 Wayland 視窗（libpomelo 的 wl.c）：給桌面外殼（控制中心、通知、啟動器、選單列…）與
 * pm_ui_run 用。一般設定程式不用直接呼叫，設 pm_ui_native = 1 就好。
 *
 * 流程：pmwl_connect() 確認連得上而且協定齊全 → pmwl_window()（一般視窗）或 pmwl_layer()（桌面外殼）→
 * pmwl_begin() 拿 cairo 畫、pmwl_end() 送出 → 迴圈呼叫 pmwl_next() 收事件。按鍵的 keysym 與修飾鍵位元跟 X 一樣。 */
#ifndef POMELO_WL_H
#define POMELO_WL_H
#include "pomelo.h"
#include <poll.h>

typedef struct PmWl PmWl;
enum { PMW_NONE, PMW_EXPOSE, PMW_SIZE, PMW_DOWN, PMW_UP, PMW_MOTION, PMW_SCROLL, PMW_KEY, PMW_CLOSE,
       PMW_FOCUS_IN, PMW_FOCUS_OUT, PMW_CONF, PMW_FD, PMW_OUTPUTS,   /* PMW_OUTPUTS：螢幕插拔 */
       PMW_ENTER, PMW_LEAVE, PMW_TOPLEVELS, PMW_WORKSPACE,   /* 滑鼠進入／離開這個視窗；視窗清單變了（pmwl_toplevels） */
       /* 以下是階段 4 加的（有的要先 pmwl_want 才會收到，見下面） */
       PMW_DND_ENTER, PMW_DND_MOTION, PMW_DND_LEAVE, PMW_DND_DROP, PMW_DND_END,   /* 拖放，見「拖放」一節 */
       PMW_KEYUP, PMW_RELMOTION, PMW_PREEDIT, PMW_SCROLL_FINE, PMW_POINTER_LOCK,
       PMW_IDLE,      /* PMW_IDLE：閒置通知，見 pmwl_idle_watch */
       PMW_FRAME,
       PMW_CLIP };    /* PMW_CLIP：剪貼簿換了（x＝1 有內容）。自己放的也會收到，用 pmwl_clip_owned() 分辨；視窗拿到鍵盤焦點時也會來一次 */   /* PMW_FRAME：frame callback，見 pmwl_frame_request（time＝compositor 的時間戳，毫秒） */
typedef struct {
	int type;
	PmWl *win;
	int x, y, button;          /* DOWN／UP：1 左、2 中、3 右；SCROLL：4 上、5 下 */
	long time;                 /* 毫秒 */
	KeySym key;                /* 跟 X 的 keysym 數值一樣（xkbcommon） */
	unsigned state;            /* X 的修飾鍵位元（ShiftMask、ControlMask、Mod1Mask、Mod4Mask…） */
	char text[64];
	int textlen;
	int w, h;                  /* SIZE */
} PmwEvent;
/* 各事件用到的欄位（沒寫的欄位不要用）：
 *   KEY／KEYUP：key、state、text（只有 KEY）；button＝鍵盤的 evdev 代碼（KEY_*，虛擬機器、遠端桌面送掃描碼用）；x＝1 是按住不放的重複
 *   SCROLL：button 4 上、5 下、6 左、7 右（一格一次）；SCROLL_FINE：w、h＝水平、垂直捲動量（1/256 像素，滾輪一格 15 像素）
 *   DOWN／UP：button 1 左、2 中、3 右、8 側邊上一頁、9 側邊下一頁
 *   RELMOTION：x、y＝滑鼠相對移動（沒有加速度的原始值，pmwl_pointer_lock 之後才有）；POINTER_LOCK：x＝1 鎖住了、0 放開了
 *   PREEDIT：text＝輸入法還沒確定的字（空字串＝清掉）、x、y＝游標在字串裡的位元組範圍（-1＝沒有）
 *   DND_ENTER／MOTION／DROP：x、y＝在視窗裡的位置，button＝目前協商好的動作（PMWL_DND_*，0＝還沒有）；DND_END（我們是來源）：button＝結果，0＝取消 */
enum { PMWL_IN_KEYUP = 1, PMWL_IN_SCROLL_FINE = 2, PMWL_IN_PREEDIT = 4 };
void pmwl_want(PmWl *win, unsigned flags);   /* 這個視窗要多收哪些事件（預設都不給，免得舊程式收到不認得的事件） */

int pmwl_connect(void);        /* 連得上 compositor 而且協定齊全：1 */
PmWl *pmwl_window(const char *title, const char *app_id, int w, int h, int minw, int minh, PmWl *parent);
void pmwl_destroy(PmWl *win);
cairo_t *pmwl_begin(PmWl *win, int *w, int *h);
void pmwl_end(PmWl *win, cairo_t *cr);       /* 送給 compositor 顯示 */
void pmwl_cancel(PmWl *win, cairo_t *cr);    /* 不送（還要再畫一次） */
int pmwl_next(PmwEvent *ev, long timeout_ms);        /* 1 = 有事件，0 = 時間到 */
void pmwl_ime(PmWl *win, int on, int x, int y, int h);   /* 文字輸入框有沒有焦點、游標在哪（給輸入法擺選字列） */
void pmwl_menu(PmWl *parent, const MenuItem *m, int n, int x, int y);   /* x、y 是 parent 裡的座標 */
/* 桌面外殼（layer-shell）：layer 0 背景、1 bottom、2 top、3 overlay；anchor 是 1 上、2 下、4 左、8 右的組合；
 * 邊距 上右下左；exclusive：保留給它的空間（-1 = 不管別人保留的空間，0 = 不保留）；kb：0 不要鍵盤、1 獨佔、2 點了才給；
 * output：pmwl_output_at 的編號，-1 = compositor 決定 */
enum { PMWL_ANCHOR_TOP = 1, PMWL_ANCHOR_BOTTOM = 2, PMWL_ANCHOR_LEFT = 4, PMWL_ANCHOR_RIGHT = 8 };
int pmwl_has_layer(void);
PmWl *pmwl_layer(const char *ns, int layer, int output, unsigned anchor, int w, int h,
                 int mt, int mr, int mb, int ml, int exclusive, int kb);
void pmwl_layer_resize(PmWl *win, int w, int h);
void pmwl_size(PmWl *win, int *w, int *h);   /* 目前的大小（layer 視窗貼滿螢幕時就是螢幕大小） */
void pmwl_layer_margin(PmWl *win, int mt, int mr, int mb, int ml);
int pmwl_outputs(Rect *r, int max);       /* 所有螢幕（全域座標），回傳個數；編號跟 pmwl_layer 的 output 一樣 */
void pmwl_watch_fd(int fd);
int pmwl_fd(void);   /* Wayland 連線的 fd：程式要自己跟別的事件一起等時用（可讀就 pmwl_next(&ev, 0) 到沒有事件為止） */

/* 螢幕鎖定（ext-session-lock）：compositor 保證鎖住，鎖定程式當掉畫面也還是鎖著（compositor 會重開鎖定程式）。
 * pmwl_lock() 成功後幫每個螢幕開 pmwl_lock_surface()，收到 PMW_OUTPUTS 時補開新螢幕的；驗證成功後 pmwl_unlock() */
int pmwl_has_lock(void);
int pmwl_lock(void);                       /* 1 = 鎖好了；0 = 不支援或已經有別的鎖定程式 */
PmWl *pmwl_lock_surface(int output);
int pmwl_window_output(PmWl *win);         /* 在哪個螢幕（-1 = 螢幕拔掉了） */
void pmwl_unlock(void);

/* 擷取畫面（wlr-screencopy）：全域座標的一塊，原生視窗、layer 視窗、X 視窗都拍得到；不支援時回傳 NULL */
int pmwl_can_capture(void);
cairo_surface_t *pmwl_capture(int x, int y, int w, int h, int cursor);

/* 顯示器設定（wlr-output-management）：每個螢幕的名稱、可以用的解析度、目前的設定 */
typedef struct { int w, h, refresh, preferred; } PmwMode;   /* refresh 是 mHz（60000 = 60 Hz） */
typedef struct {
	char name[32], desc[128];
	int enabled, x, y;           /* 位置：全域的邏輯座標 */
	int w, h, refresh;           /* 目前的解析度 */
	int transform;               /* 0 正常、1 轉 90°、2 轉 180°、3 轉 270°（wl_output_transform） */
	double scale;
	int nmodes;
	PmwMode modes[64];
} PmwHead;
int pmwl_heads(PmwHead *h, int max);                         /* 回傳螢幕個數；-1 = compositor 不支援 */
int pmwl_heads_apply(const PmwHead *h, int n, int test);     /* 用名稱對到螢幕；test = 只測試。1 = 成功 */
void pmwl_hide_cursor(int hide);                              /* 游標在我們的視窗上時隱藏（螢幕保護程式用） */
int pmwl_screens_power(int on);                              /* 所有螢幕關掉／打開（wlr-output-power-management） */               /* 程式自己的 fd 可以讀時，pmwl_next 回傳 PMW_FD（x 是 fd） */
void pmwl_clear(PmWl *win);   /* 整個透明（只用來接滑鼠） */
void pmwl_input_region(PmWl *win, int x, int y, int w, int h);
int pmwl_output_at(int x, int y, Rect *r);
char *pmwl_clip_get(void);              /* 剪貼簿的文字（呼叫者 free；沒有文字 = NULL） */
void pmwl_clip_set(const char *text);   /* 把文字放上剪貼簿（要在按鍵或按滑鼠之後呼叫） */
/* 建立後改保留空間（例如 Dock 自動隱藏時不佔位）、改鍵盤模式 */
void pmwl_layer_exclusive(PmWl *win, int exclusive);
void pmwl_layer_margin_defer(PmWl *win, int mt, int mr, int mb, int ml);   /* 邊距下一次 pmwl_end 才一起送 */
void pmwl_input_mask(PmWl *win, const unsigned char *bits, int w, int h);  /* 可點的範圍：1 位元遮罩（X Shape 那種） */
void pmwl_layer_kb(PmWl *win, int kb);
/* 跟 pmwl_next 一樣，但順便等程式自己的 fd（例如 D-Bus、inotify、/proc/self/mounts 的 POLLPRI）：
 * 有任何一個 fd 有動靜就回傳 0 並把 revents 填好（這時 *ev 沒有事件；佇列裡的事件下次呼叫再給）；最多 48 個 */
int pmwl_next_fds(PmwEvent *ev, long timeout_ms, struct pollfd *fds, int nfds);

/* 視窗清單（wlr-foreign-toplevel）：給 Dock、小柚用。pmwl_toplevels_start() 開始追蹤（回傳 0 = compositor 不給），
 * 之後清單一變就收到 PMW_TOPLEVELS，用 pmwl_toplevels() 拿目前的清單；id 在這次連線裡不會重複 */
typedef struct {
	unsigned id, parent;         /* parent：對話框的主視窗 id，0 = 沒有 */
	char title[256], app_id[128];
	int minimized, maximized, fullscreen, activated;
	int output;                  /* 在哪個螢幕（pmwl_outputs 的編號），-1 = 不知道 */
	int workspace;               /* 在哪個桌面（0 起算）；-1 = 在所有桌面顯示、-2 = 還不知道（compositor 沒有多桌面） */
} PmwToplevel;
enum { PMWL_TL_ACTIVATE, PMWL_TL_MINIMIZE, PMWL_TL_UNMINIMIZE, PMWL_TL_MAXIMIZE, PMWL_TL_UNMAXIMIZE,
       PMWL_TL_FULLSCREEN, PMWL_TL_UNFULLSCREEN, PMWL_TL_CLOSE };
int pmwl_toplevels_start(void);
int pmwl_toplevels(PmwToplevel *out, int max);   /* 回傳個數（開啟的順序） */
void pmwl_toplevel_do(unsigned id, int what);

/* 多桌面（pomelo_workspace_v1，pomelo-compositor 的自訂協定）：要先 pmwl_toplevels_start()，再 pmwl_workspace_start()
 * （回傳 0 = compositor 沒有多桌面）。桌面數量或目前桌面變了、或有視窗換了桌面，會收到 PMW_WORKSPACE。
 * 視窗在哪個桌面看 PmwToplevel.workspace。桌面編號都從 0 開始。 */
int pmwl_workspace_start(void);
int pmwl_workspace_info(int *count, int *current);    /* 回傳 1 = 有多桌面 */
void pmwl_workspace_goto(int n);
void pmwl_workspace_step(int delta);                  /* -1／+1，到頭不繞回 */
void pmwl_workspace_move_window(unsigned toplevel_id, int n, int follow);   /* toplevel_id 0 = 目前有焦點的視窗 */
void pmwl_workspace_sticky(unsigned toplevel_id, int mode);   /* 0 取消、1 在所有桌面顯示、2 切換 */
void pmwl_workspace_add(void);
void pmwl_workspace_remove(int n);
void pmwl_workspace_overview(void);

/* ---- 階段 4：應用程式（檔案管理員、終端機、文件編輯…）需要的 ---- */

/* 視窗屬性 */
void pmwl_set_title(PmWl *win, const char *title);
void pmwl_set_app_id(PmWl *win, const char *app_id);
void pmwl_set_min_size(PmWl *win, int w, int h);
void pmwl_set_max_size(PmWl *win, int w, int h);       /* 0 = 不限 */
void pmwl_fullscreen(PmWl *win, int on, int output);   /* output：pmwl_outputs 的編號，-1 = 目前的螢幕 */
void pmwl_maximize(PmWl *win, int on);
void pmwl_minimize(PmWl *win);
int pmwl_is_fullscreen(PmWl *win);                     /* compositor 最近一次 configure 的狀態（PMW_SIZE 時看） */
int pmwl_is_maximized(PmWl *win);

/* 游標形狀（滑鼠在這個視窗上的時候）。cursor-shape 協定，形狀由 compositor 的佈景畫 */
enum { PMWL_CURSOR_DEFAULT, PMWL_CURSOR_TEXT, PMWL_CURSOR_POINTER, PMWL_CURSOR_GRAB, PMWL_CURSOR_GRABBING, PMWL_CURSOR_CROSSHAIR,
       PMWL_CURSOR_RESIZE_EW, PMWL_CURSOR_RESIZE_NS, PMWL_CURSOR_RESIZE_NESW, PMWL_CURSOR_RESIZE_NWSE, PMWL_CURSOR_MOVE,
       PMWL_CURSOR_NOT_ALLOWED, PMWL_CURSOR_WAIT, PMWL_CURSOR_COPY, PMWL_CURSOR_HIDDEN };
void pmwl_cursor(PmWl *win, int shape);
/* 自訂游標圖（虛擬機器、遠端桌面的來賓游標）：img＝ARGB32 的 cairo 影像（會複製，最大 256×256）、熱點是圖上的位置；
 * 滑鼠在這個視窗上時用這張，img＝NULL 或之後呼叫 pmwl_cursor 就回到形狀 */
void pmwl_cursor_image(PmWl *win, cairo_surface_t *img, int hot_x, int hot_y);
/* frame callback：pmwl_frame_request 之後的下一次 pmwl_end 會掛一個，compositor 準備好接下一張時收到 PMW_FRAME（win）。
 * pmwl_frame_wait：1＝上一個還沒回來（影片先別畫，免得白白丟格；超過 250 毫秒當作回來了） */
void pmwl_frame_request(PmWl *win);
int pmwl_frame_wait(PmWl *win);

/* 輸入法：pmwl_ime 之前設內容用途（zwp_text_input_v3 的 content purpose：0 一般、8 密碼、13 終端機…），
 * 有 pmwl_want(PMWL_IN_PREEDIT) 的視窗會收到 PMW_PREEDIT（輸入法還沒確定的字，要自己畫在游標處），確定的字照舊是只有 text 的 PMW_KEY */
enum { PMWL_IME_NORMAL = 0, PMWL_IME_PASSWORD = 8, PMWL_IME_TERMINAL = 13 };
void pmwl_ime_purpose(int purpose);

/* 剪貼簿（任何格式）：pmwl_clip_get／pmwl_clip_set 只管文字。mime 例如 "image/png"、"text/uri-list"。
 * 放東西要在按鍵或按滑鼠之後；資料會複製。拿到的要 free，最多 64 MB */
void pmwl_clip_set_type(const char *mime, const void *data, size_t len);
void pmwl_clip_set_types(const char *const *mimes, const void *const *datas, const size_t *lens, int n);
int pmwl_clip_types(const char **out, int max);       /* 目前剪貼簿有的格式（指標到下次處理事件前有效） */
int pmwl_clip_has_type(const char *mime);
int pmwl_clip_owned(void);                            /* 剪貼簿現在還是我們放的（別人放了新東西就變 0） */
char *pmwl_clip_get_type(const char *mime, size_t *len);
/* primary selection（選取文字就放上去、中鍵貼上；只有文字） */
void pmwl_primary_set(const char *text);              /* NULL 或空字串＝清掉 */
char *pmwl_primary_get(void);

/* 滑鼠與鍵盤獨佔（遠端桌面、虛擬機器的畫面視窗）：鎖住滑鼠後游標不動、只收 PMW_RELMOTION；
 * 要有焦點才會成功（compositor 同意後送 PMW_POINTER_LOCK）。快捷鍵抑制：Alt+Tab、Super 等系統快捷鍵也送給這個視窗 */
int pmwl_pointer_lock(PmWl *win, int on);
void pmwl_shortcuts_inhibit(PmWl *win, int on);
void pmwl_pointer_hint(PmWl *win, double x, double y);   /* 鎖住期間告訴 compositor 游標在視窗裡「應該」在哪（來賓游標的位置）；放開鎖定時游標會出現在那裡 */
/* 閒置通知（ext-idle-notify-v1；compositor 把輸入、X 程式與原生程式的防止閒置都算進去）：電源管理（pomelo-power）用，不需要 X 連線。
 * pmwl_idle_watch(毫秒) 回傳編號（0＝compositor 不支援）；那麼久沒有輸入時 pmwl_next 回傳 PMW_IDLE（x＝1，button＝編號），
 * 有輸入回來時 PMW_IDLE（x＝0）。同時最多 8 個；設定改了先 pmwl_idle_clear() 再重新登記 */
int pmwl_can_idle(void);
int pmwl_idle_watch(long timeout_ms);
void pmwl_idle_clear(void);
void pmwl_idle_inhibit(PmWl *win, int on);   /* 這個視窗在畫面上時不要變暗、鎖定、休眠（影片、簡報、遠端桌面）；取代 X 的 XResetScreenSaver 定時戳 */

/* 子畫面：貼在別的視窗上、自己畫自己送的小畫面（影片、虛擬機器的畫面），位置相對 parent 左上角 */
PmWl *pmwl_subsurface(PmWl *parent, int x, int y, int w, int h);
void pmwl_sub_move(PmWl *sub, PmWl *parent, int x, int y);   /* 位置要 parent 下一次送出才生效，這裡會幫它送 */
void pmwl_sub_resize(PmWl *sub, int w, int h);               /* 之後的 pmwl_begin 就用新大小 */

/* ---- 拖放（wl_data_device；X 程式之間、X 與原生之間 compositor 會接橋） ----
 *
 * 目標（接別人拖進來的）：建立視窗後 pmwl_dnd_target(win, 收得下的格式, n, 動作)——有登記的格式拖進來就自動接受（動作優先複製）；
 * 事件 PMW_DND_ENTER／MOTION（x、y）／LEAVE／DROP。MOTION 裡可以用 pmwl_dnd_accept 換掉預設反應（例如游標在不能放的地方就
 * pmwl_dnd_accept(NULL, 0, 0)）。DROP 事件之後（下一次 pmwl_next 之前）用 pmwl_dnd_fetch／pmwl_dnd_files 拿資料；
 * 程式處理完回到 pmwl_next，函式庫自動告訴來源「收好了」。
 * 來源（把東西拖出去的）：在 PMW_DOWN 之後、確定開始拖的時候（例如移動超過 5 像素）呼叫 pmwl_drag_start。拖的期間
 * 這個視窗收不到滑鼠事件（會收到 PMW_LEAVE）；結束時收到 PMW_DND_END（button＝實際動作，0＝取消或沒放成功）。Esc 取消由 compositor 處理。 */
enum { PMWL_DND_COPY = 1, PMWL_DND_MOVE = 2, PMWL_DND_ASK = 4 };
void pmwl_dnd_target(PmWl *win, const char *const *mimes, int n, unsigned actions);   /* n = 0：不收 */
void pmwl_dnd_accept(const char *mime, unsigned actions, unsigned preferred);          /* mime = NULL：這裡不能放 */
unsigned pmwl_dnd_action(void);                    /* 目前協商好的動作（PMWL_DND_*），0 = 還沒有／不能放 */
int pmwl_dnd_has(const char *mime);                /* 拖進來的東西有沒有這個格式 */
int pmwl_dnd_mimes(const char **out, int max);
char *pmwl_dnd_fetch(const char *mime, size_t *len);   /* 呼叫者 free；最多 64 MB；同一個程式內拖放也行 */
int pmwl_dnd_files(char ***paths);                 /* text/uri-list 的本機檔案，回傳個數；paths 用 pm_dnd_free（pomelo-dnd.h）釋放 */

typedef struct {
	const char *const *mimes; int nmimes;          /* 提供的格式（第一個最好） */
	unsigned actions;                              /* 允許的動作 PMWL_DND_*；0 = 複製 */
	char *(*data)(const char *mime, size_t *len, void *user);   /* 目標來拿資料時呼叫；回傳 malloc 的內容（函式庫 free）；*len 填長度 */
	void *user;
	cairo_surface_t *icon; int hot_x, hot_y;       /* 拖曳圖示（可 NULL）與游標在圖裡的位置；最大 512×512 */
} PmwDragSrc;
int pmwl_drag_start(PmWl *origin, const PmwDragSrc *src);   /* 1 = 開始了。要在剛按下滑鼠之後（用最近一次按下的序號） */
int pmwl_drag_start_files(PmWl *origin, const char *const *paths, int n, unsigned actions, cairo_surface_t *icon, int hot_x, int hot_y);
int pmwl_drag_active(void);                        /* 我們有一個拖放正在進行 */
unsigned pmwl_drag_action(void);                   /* 目前目標接受的動作（來源用來換游標或圖示），0 = 沒有 */

/* ---- 雙後端程式（辦公、繪圖、PDF 等：事件處理函式吃 XEvent）的共用薄層 ----
 * pmwl_to_xevent：把 pmwl_next 取到的事件轉成 XEvent（回傳 0＝沒有對應的，例如拖放、預編輯、PMW_CONF，程式自己處理）。
 * xany.window 是 PmWl 指標（用 PMWL_XID 轉），按下去的滑鼠鍵會帶在 state（Button1Mask…），滾輪是按鍵 4～7，關閉是
 * ClientMessage 而且 data.l[0] == PMWL_XCLOSE。按鍵的 keysym 與文字用 pmwl_xkey 取：回傳文字長度，-1＝不是轉出來的事件
 * （display 不是 NULL，呼叫者改用 XLookupString）；ks0 是沒修飾鍵的 keysym（XLookupKeysym(e, 0)）。
 * pmwl_resize：程式自己改視窗大小（例如計算機切換模式）；下一次 pmwl_begin 用新大小，compositor 跟著調外框。 */
/* evdev 按鍵碼（PMW_KEY 的 button）→ PC 掃描碼 set 1：延伸鍵（0xe0 開頭）加 0x100，Pause 是 0xffff，不認得是 0 */
unsigned pmwl_scancode(unsigned evdev_code);
#define PMWL_XCLOSE 0x504d57L
#define PMWL_XID(w) ((Window)(uintptr_t)(w))
int pmwl_to_xevent(const PmwEvent *e, XEvent *x);
int pmwl_xkey(const XKeyEvent *e, char *buf, int n, KeySym *ks, KeySym *ks0);
void pmwl_resize(PmWl *win, int w, int h);
/* 彈出選單：原生視窗（wl 不是 NULL）用 pmwl_menu（x、y 是視窗裡的座標，轉出來的事件的 x_root／y_root 就是），X 視窗用 pm_popup（螢幕座標） */
#define PMWL_POPUP(wl, m, n, x, y) do { if (wl) pmwl_menu((wl), (m), (n), (x), (y)); else pm_popup((m), (n), (x), (y)); } while (0)
/* 視窗座標 → 彈出選單用的座標：原生視窗就是自己（pmwl_menu 用視窗座標），X 視窗要換成螢幕座標 */
#define PMWL_XLATE(wl, d, w, r, x, y, rx, ry, ch) do { if (wl) { *(rx) = (x); *(ry) = (y); } else XTranslateCoordinates(d, w, r, x, y, rx, ry, ch); } while (0)
#endif
