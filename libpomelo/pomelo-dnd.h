/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-dnd.h — 收別的程式拖進來的檔案（XDND，見 dnd.c）。不需要 pm_init，自己有 X 連線的程式（終端機）也能用。
 *
 * 建立視窗後 pm_dnd_accept(d, win)；每個事件先給 pm_dnd_event：
 * -1 = 不是拖放的事件；0 = 處理掉了；> 0 = 放下了幾個檔案（*paths，用 pm_dnd_free 釋放） */
#ifndef POMELO_DND_H
#define POMELO_DND_H

#include <X11/Xlib.h>

void pm_dnd_accept(Display *d, Window w);
int pm_dnd_event(Display *d, XEvent *ev, char ***paths);
void pm_dnd_free(char **paths, int n);
int pm_parse_uris(char *data, char **paths, int max);   /* text/uri-list → 本機路徑（只收 file://，每個要 free） */

#endif
