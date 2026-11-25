/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-settings.h — 讀寫 Pomelo 共用設定檔
 *
 * 設定檔：$XDG_CONFIG_HOME/pomelo/settings.conf（預設 ~/.config/pomelo/settings.conf）
 * 格式：每行「鍵 = 值」，# 開頭為註解。
 *
 * 設定改變時，根視窗的 _POMELO_SETTINGS 屬性會更新；各程式收到 PropertyNotify
 * 後用 pm_conf_changed() 判斷並自動重新載入，畫面就能即時套用新設定。
 */
#ifndef POMELO_SETTINGS_H
#define POMELO_SETTINGS_H

#include <X11/Xlib.h>

void pm_conf_load(void);
/* 每次重新載入就加一，用來判斷設定是否被別的程式改過 */
int pm_conf_serial(void);
const char *pm_conf_get(const char *key, const char *def);
int pm_conf_int(const char *key, int def);
double pm_conf_num(const char *key, double def);
unsigned pm_conf_color(const char *key, unsigned def);   /* #rrggbb */

/* 寫入設定檔並通知其他程式 */
void pm_conf_set(const char *key, const char *value);
void pm_conf_set_int(const char *key, int value);

/* 開始接收設定變更通知（會保留根視窗上原本的事件遮罩） */
void pm_conf_watch(Display *d);
/* 這個事件是設定變更嗎？是的話會自動重新載入，回傳 1 */
int pm_conf_changed(Display *d, XEvent *ev);

/* 輸入法（XIM）連線。輸入法伺服器（pomelo-input、fcitx5…）結束或重新啟動時，舊的連線會失效，
 * 繼續用（XSetICValues、XSetICFocus…）就會當掉。pm_xim_attach 建立連線並向 Xlib 登記通知：
 * 伺服器消失時把 *im、*ic 設成 NULL（程式裡照常用 if (xic) 判斷即可），伺服器回來時自動重新連上
 * （也包括程式比輸入法先啟動的情況）。w 的事件遮罩會自動加上輸入法需要的事件。 */
#include <X11/Xutil.h>
void pm_xim_attach(Display *d, XIM *im, XIC *ic, Window w);
/* 只監看 XIM 本身（自己建 IC 的程式，例如視窗管理員的 Spotlight）；每次連線消失，世代編號加一，
 * 用舊世代建的 IC 不可以再碰（包括 XDestroyIC） */
void pm_xim_watch(Display *d, XIM *im);
unsigned pm_xim_generation(void);

/* 介面翻譯：原文是繁體中文，依 LANGUAGE / LANG 載入 /usr/share/pomelo/i18n/<語言>.tsv
 * （每行「原文<Tab>譯文」）。找不到譯文就傳回原文。原文寫成「{情境}原文」可以區分同字不同義（顯示時去掉 {情境}）。 */
const char *pm_tr(const char *s);
/* 目前介面語言代碼，例如 "zh_TW"、"en" */
const char *pm_lang(void);
#ifndef _
#define _(s) pm_tr(s)
#endif
#define N_(s) (s)

#endif
