/* SPDX-License-Identifier: GPL-3.0-or-later */
/* internal.h — libpomelo 內部共用（不裝進 /usr/include）：X11 與 Wayland 兩種後端共用的選單畫法等。
 * 原生 Wayland 視窗的公開介面在 pomelo-wl.h */
#ifndef POMELO_INTERNAL_H
#define POMELO_INTERNAL_H
#include "pomelo.h"
#include "pomelo-wl.h"

#pragma GCC visibility push(hidden)   /* 只給 libpomelo 自己用，不算公開 ABI */

void pm__init_x(void);   /* pm_init 的 X 部分（dpy 開好之後） */

/* 彈出選單（pm_popup）的版面：X 與 Wayland 畫起來一模一樣 */
typedef struct { int W, H, tx, ih, sh, pad; } PmMenuGeo;
void pm__menu_measure(const MenuItem *m, int n, PmMenuGeo *g);
void pm__menu_draw(cairo_t *cr, const MenuItem *m, int n, const PmMenuGeo *g, int hot);
int pm__menu_hit(const MenuItem *m, int n, const PmMenuGeo *g, int px, int py);   /* 項目編號，-1 = 不能選或在外面 */
int pm__menu_step(const MenuItem *m, int n, int hot, int dir);                      /* ↑↓ 移到下一個能選的 */

#pragma GCC visibility pop
#endif
