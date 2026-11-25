/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gui.h — pomelo-archive 的介面層共用（需要 pomelo.h；核心 ac-*.c 不會看到它） */
#ifndef PA_GUI_H
#define PA_GUI_H

#include <pomelo.h>
#include "ac.h"

cairo_surface_t *pa_icon(int size);               /* 程式圖示：紙箱＋拉鍊＋柚子切片 */
const char *pa_exe(void);                         /* 自己的執行檔路徑（程式更新過也找得到）*/
/* 另外執行 pomelo-archive 的某個模式（解壓、建立、密碼視窗）；wait = 0 立刻回傳 pid；
 * pw 非 NULL 時從標準輸入送進去（一行密碼）；回傳 -1 = 失敗 */
int pa_spawn(char *const argv[], const char *pw, int *out_fd);

/* 把對話視窗移到螢幕中央（parent 非 0 就置中在那個視窗上）；原生 Wayland 由合成器擺，這裡什麼都不做。每個畫面呼叫沒關係，只會動一次 */
void pa_center(PmUI *u, unsigned long parent);

/* 各個以 pm_ui_run 為主體的模式（gui-dialogs.c）：回傳結束碼 */
int gui_password(int argc, char **argv);          /* --ui-password 檔案 [--wrong] */
int gui_extract(int argc, char **argv);           /* --extract-here 檔案… ｜ --extract-to 檔案 資料夾 [--only 路徑]… [--password-stdin] */
int gui_compress(int argc, char **argv);          /* --compress 檔案或資料夾… */
int gui_add(int argc, char **argv);               /* --add-gui 封存檔 檔案… [--password-stdin] */
/* 瀏覽視窗（gui-browse.c） */
int gui_browse(int argc, char **argv);

#endif
