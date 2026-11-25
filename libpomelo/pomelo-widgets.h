/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-widgets.h — 設定程式用的簡易介面元件（立即模式）
 *
 * 每次重畫都呼叫一次 build()，在裡面依序宣告元件；元件回傳 1 代表使用者剛剛改變了它。
 *
 *   static void build(PmUI *u) {
 *       pm_ui_title(u, "外觀", NULL);
 *       pm_ui_group(u, NULL);
 *       if (pm_ui_switch(u, "深色模式", &dark)) pm_conf_set_int("appearance.dark", dark);
 *       pm_ui_end(u);
 *   }
 *   int main(void) { return pm_ui_run("外觀", "pomelo-theme", 640, 560, build, NULL, 0, NULL); }
 */
#ifndef POMELO_WIDGETS_H
#define POMELO_WIDGETS_H

typedef struct PmUI PmUI;
typedef void (*PmBuild)(PmUI *u);

struct PmUI {
	Window win;
	int w, h;
	cairo_t *cr;
	int mx, my, down, clicked, released, dbl;
	int id, active, focus, claimed;
	int x, y, cw;                 /* 版面：目前位置與欄寬 */
	int scroll, content_h;
	int group_open, group_rows;
	KeySym key;
	char keybuf[64];
	int keylen;
	unsigned keystate;
	XIC xic;
	int again;                    /* 元件狀態改變，需要再畫一次 */
	Time last_click;
	void *user;
	void *wl;                     /* 原生 Wayland 視窗（內部用；X11 時是 NULL，這時 win 才有值；pm_ui_hide 收起來的時候也是 NULL） */
	int native;                   /* 1＝這個視窗走原生 Wayland（含 pm_ui_hide 收起來的時候）；X11 是 0 */
};

/* 設成 1（在 pm_ui_run 之前）＝這個程式可以直接當原生 Wayland 程式：在 Wayland 工作階段不經過 XWayland。
 * 只有 build()／tick() 裡完全沒有直接呼叫 Xlib（dpy、u->win…）的程式才能設；fork 出去的子程序要關 X 連線時
 * 先判斷 dpy 不是 NULL。沒有 WAYLAND_DISPLAY、compositor 不支援、POMELO_BACKEND=x11、或 pm_ui_popover 時照舊走 X11。 */
extern int pm_ui_native;

/* 建立視窗並執行事件迴圈；tick 每 tick_ms 毫秒呼叫一次（0 = 不需要） */
int pm_ui_run(const char *title, const char *wmclass, int w, int h, PmBuild build,
              void (*tick)(PmUI *u), int tick_ms, cairo_surface_t *(*icon)(int size));
/* 在 tick() 裡呼叫：把下一次 tick 的間隔改成 ms（0 = 回到 pm_ui_run 給的 tick_ms）。程式閒置時拉長（例如 66 → 250 ms），
 * 有動畫、背景工作時再縮短，閒置的喚醒次數就不用跟動畫一樣高 */
void pm_ui_tick_ms(int ms);
void pm_ui_redraw(PmUI *u);

void pm_ui_title(PmUI *u, const char *title, const char *subtitle);
/* 頁面左上角的「‹ 上一頁」；按下（或 Backspace、Alt+←）回傳 1 */
int pm_ui_back(PmUI *u, const char *label);
void pm_ui_group(PmUI *u, const char *caption);
void pm_ui_end(PmUI *u);
void pm_ui_note(PmUI *u, const char *text);
void pm_ui_space(PmUI *u, int h);

int pm_ui_switch(PmUI *u, const char *label, int *v);
int pm_ui_slider(PmUI *u, const char *label, double *v, double lo, double hi);
int pm_ui_segment(PmUI *u, const char *label, const char **opts, int n, int *v);
int pm_ui_choice(PmUI *u, const char *label, const char **opts, int n, int *v);
int pm_ui_swatches(PmUI *u, const char *label, const unsigned *colors, int n, int *v);
int pm_ui_button(PmUI *u, const char *label, const char *button);
/* 對話窗底下一排橫的按鈕（靠右，def 是藍色的預設按鈕、按 Enter 也算），回傳按下的 index，沒按回傳 -1 */
int pm_ui_buttons(PmUI *u, const char *const *labels, int n, int def);
/* 在剛點的位置開選單（例如點清單裡的一項跳出「連線／移除」）；選了就呼叫那一項的 fn。X11 與原生 Wayland 都能用 */
void pm_ui_menu(PmUI *u, const MenuItem *m, int n);
/* 常見的詢問對話窗：一句問題，buttons 由左到右橫排靠右；Enter＝def、Esc 或關掉視窗＝cancel。
 * 回傳按下的按鈕 index。例如還沒儲存就要關閉：{ "不要儲存", "取消", "儲存" }，def 2、cancel 1 */
int pm_ask(const char *title, const char *wmclass, cairo_surface_t *(*icon)(int size),
           const char *msg, const char *const *buttons, int n, int def, int cancel);

/* 開啟／另存新檔對話窗（常見的排法：上面是目前的資料夾、左邊常用位置、中間檔案清單、下面名稱與類型）。
 * save：1＝另存新檔（有名稱欄，檔案已經存在會先問要不要取代），0＝開啟。dir：一開始的資料夾（NULL＝家目錄）；
 * name：另存新檔預設的檔名。types：檔案類型（label 顯示用、ext 是副檔名，不含點，多個用分號隔開，例如
 * "docx;odt"），清單只列出符合目前類型的檔案；*type 進來是預設選哪個、出去是使用者選的（可以是 NULL）。
 * 選好的完整路徑寫到 out（另存新檔沒打副檔名會自動補上目前類型的）。回傳 1＝選好了、0＝取消。 */
typedef struct { const char *label; const char *ext; } PmFileType;
int pm_file_dialog(const char *title, const char *wmclass, cairo_surface_t *(*icon)(int size), int save,
                   const char *dir, const char *name, const PmFileType *types, int ntype, int *type,
                   char *out, size_t outn);
void pm_ui_info(PmUI *u, const char *label, const char *value);
/* 可點選的一行：icon 是圖示主題中的名稱（可為 NULL） */
int pm_ui_item(PmUI *u, const char *icon, const char *title, const char *sub, const char *right, int checked);
/* 剛畫的那一列（pm_ui_item）上有沒有人按右鍵：有的話回傳 1（一次），這時可以馬上 pm_ui_menu 開右鍵選單 */
int pm_ui_item_rclick(PmUI *u);
/* 自己畫整個畫面的程式（例如行事曆）用：按右鍵的位置（視窗座標；每次右鍵回傳 1 一次），以及滾輪累計的格數
 * （往上 −1、往下 +1；程式自己讀完歸零） */
int pm_ui_rclick_at(PmUI *u, int *x, int *y);
/* 從 build()／tick() 裡面關掉這個視窗，pm_ui_run 隨後回傳（對話窗按了「儲存」之後用） */
void pm_ui_close(PmUI *u);
extern int pm_ui_wheel_steps;
/* 設了之後（在 pm_ui_run 之前），別的程式把檔案拖進視窗就呼叫 fn(路徑)，每個檔案一次（X11 與原生 Wayland 都行） */
extern void (*pm_ui_on_drop)(const char *path);
/* 把檔案從這個視窗拖出去（text/uri-list；動作允許複製與移動）。icon 是拖曳時跟著游標的圖（可 NULL）。
 * 只有原生 Wayland 視窗做得到（回傳 1）；X11 視窗回傳 0。要在剛按下滑鼠之後（例如 pm_ui_item 偵測到按住拖曳）呼叫 */
int pm_ui_drag_files(PmUI *u, const char *const *paths, int n, cairo_surface_t *icon, int hot_x, int hot_y);
/* 顯示一張圖（例如攝影機預覽），等比例填滿 w×h；img 為 NULL 時顯示黑色方塊 */
void pm_ui_image(PmUI *u, cairo_surface_t *img, int w, int h);
/* 讓 pm_ui_item 的 icon 名稱對應到自己畫的圖（56×56，例如 "@general"） */
void pm_ui_add_icon(const char *name, cairo_surface_t *s);
/* 文字輸入框：按 Enter 時回傳 1；secret = 密碼 */
int pm_ui_entry(PmUI *u, const char *label, char *buf, size_t n, int secret);
/* 即時折線圖：nseries 條資料，每條 n 個點（舊→新），max <= 0 代表自動縮放；
 * label 在左上，legend[i]（可為 NULL）是每條線的說明，帶色點顯示在右上 */
void pm_ui_graph(PmUI *u, const char *label, const char *const *legend, const double *const *data,
                 const unsigned *colors, int nseries, int n, double max);
/* 比例條：frac 0～1，例如記憶體、各核心使用率 */
void pm_ui_meter(PmUI *u, const char *label, const char *value, double frac, unsigned color);
/* 一排直條：v[i] 為 0～1，超過 0.85 用 hot 顏色（例如各 CPU 核心）；labels 為 NULL 時標 1、2、3… */
void pm_ui_bars(PmUI *u, const char *label, const double *v, const char *const *labels, int n, unsigned color, unsigned hot);
/* 全寬的主要輸入框（一個視窗只放一個），空的時候顯示 hint，一律保持鍵盤焦點。按 Enter 回傳 1 */
int pm_ui_input(PmUI *u, const char *hint, char *buf, size_t n);
/* 對話泡泡（自動換行）：mine = 1 靠右、用強調色 */
void pm_ui_bubble(PmUI *u, const char *text, int mine);
/* 圖片格（例如桌布），點選時回傳 1 並設定 *v */
int pm_ui_images(PmUI *u, cairo_surface_t **imgs, const char **names, int n, int *v, int tw, int th);

/* 彈出式視窗（例如控制中心）：在 pm_ui_run 之前呼叫。視窗右上角對齊 (right, top)，
 * 高度自動依內容調整，點視窗外或按 Esc 就結束程式。 */
void pm_ui_popover(int right, int top);
void pm_ui_hide(int hide);   /* 原生 Wayland 的彈出式視窗暫時收起來（1）／叫回來（0）；X 版沒有作用 */
/* 把圖示函式畫成 PNG（安裝時產生圖示檔用） */
int pm_ui_write_icon(cairo_surface_t *(*icon)(int size), const char *path);

/* 執行指令並取回標準輸出（以 LC_ALL=C.UTF-8 執行：英文欄位方便解析，內容保留 UTF-8）；呼叫者要 free() */
char *pm_run(const char *cmd);
/* 指令是否在 PATH 中 */
int pm_have(const char *cmd);

/* 載入並縮放圖片（mode：0 填滿裁切、1 完整顯示、2 拉伸） */
cairo_surface_t *pm_image_load_fit(const char *path, int w, int h, int mode);

#endif
