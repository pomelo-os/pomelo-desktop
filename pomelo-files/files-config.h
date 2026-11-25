/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pomelo 檔案管理員設定檔 — 修改後執行 make 重新編譯
 * （外觀等設定以 ~/.config/pomelo/settings.conf 為優先，這裡是預設值） */

static const char  *def_font_family = "Noto Sans CJK TC";
static const double def_font_size   = 13.0;
static const int    def_dark_mode   = 0;      /* 1 = 深色模式 */
static const char  *def_icon_theme  = "Yaru";
static const int    def_start_view  = 0;      /* 0 = 圖示，1 = 列表 */
static const int    def_show_hidden = 0;      /* 1 = 預設顯示隱藏檔 */
static const char  *terminal    = "pomelo-terminal";   /* 「在終端機中打開」用，會加上 -d 目錄 */
static const char  *opener      = "pomelo-open";       /* 開啟檔案用的程式（libpomelo 附的，照 mimeapps.list 找預設程式） */
