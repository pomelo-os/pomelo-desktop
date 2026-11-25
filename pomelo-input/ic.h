/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ic.h — 輸入環境（一個程式裡的一個打字的地方）：XIM 的或 IBus 的（Qt 程式），共用同一套引擎與選字列 */
#ifndef POMELO_INPUT_IC_H
#define POMELO_INPUT_IC_H

#define MAXENG 16

typedef struct IC {
	int ibus;                         /* 0 = XIM、1 = IBus 協定（Qt 程式） */
	int wl;                           /* 1 = 原生 Wayland 程式（input-method-v2，見 wl.c） */
	void *xic;                        /* XIM：xcb_im_input_context_t */
	void *conn;                       /* IBus：DBusConnection */
	char path[64];                    /* IBus：物件路徑 /org/freedesktop/IBus/InputContext_N */
	int cx, cy, cw, ch, has_cursor;   /* IBus：游標的位置（程式用 SetCursorLocation 告訴我們） */
	int rel_cursor;                   /* IBus：座標是相對視窗的（SetCursorLocationRelative） */
	void *ctx[MAXENG];                /* 每個引擎的狀態（用到才建立） */
	struct IC *next;                  /* IBus：同一條連線的輸入環境串起來 */
} IC;

/* pomelo-input.c 提供給 ibus.c */
IC *ic_new(void);
void ic_destroy(IC *ic);
void ic_focus(IC *ic, int in);
void ic_reset(IC *ic);
int ic_key(IC *ic, unsigned keysym, unsigned state, int press);   /* state 是 X 的修飾鍵；回傳 1 = 吃掉 */
void ic_cursor_moved(IC *ic);

/* ibus.c */
int ibus_start(const char *display);                 /* 開好伺服器、寫位址檔；失敗回傳 -1（照樣可以用 XIM） */
void ibus_stop(void);                                /* 刪掉位址檔 */
int ibus_fds(void *rfds, void *wfds, int maxfd);     /* 把要等的 fd 加進 fd_set，回傳新的 maxfd */
void ibus_handle(void *rfds, void *wfds);            /* select 之後：處理收到的訊息 */
void ibus_commit(IC *ic, const char *utf8);          /* 送出打好的字（CommitText） */

/* wl.c：原生 Wayland 程式（compositor 的 input-method-v2；沒有 WAYLAND_DISPLAY 或 compositor 不支援就不用） */
int wl_start(void);                                  /* 連上就回傳 0 */
int wl_fds(void *rfds, int maxfd);
void wl_handle(void *rfds);
int wl_timeout(void);                                /* 下一次按鍵重複還要幾毫秒（-1 = 沒有） */
void wl_tick(void);
void wl_commit(IC *ic, const char *utf8);
void wl_show(int w, int h);                          /* 選字列：大小算好了，畫的時候呼叫 ui_draw */
void wl_hide(void);

/* pomelo-input.c 提供給 wl.c：選字列的畫法與滑鼠操作（跟 X 的選字列共用） */
void ui_draw(void *cairo);
void ui_click(int x);
void ui_scroll(int down);
void ui_hover(int x, int y);

#endif
