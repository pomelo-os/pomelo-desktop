/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pomelo 終端機設定檔 — 修改後執行 make 重新編譯 */

static const char  *tfont        = "monospace:size=11";  /* fontconfig 格式，中文會自動找備用字型 */
static const double line_spacing = 1.12;   /* 行高倍數 */
static const int    tpad         = 10;     /* 文字與視窗邊緣的距離 */
static const int    tcols        = 90;     /* 預設寬度（字元） */
static const int    trows        = 28;     /* 預設高度（行） */
static const int    scrollback   = 5000;   /* 捲動緩衝行數 */
static const int    login_shell  = 0;      /* 1 = 以登入 shell 啟動（像 macOS 終端機） */
static const int    dark_theme   = 1;      /* 沒有系統設定時的預設：0 = 淺色主題（可用 terminal.theme 覆寫） */

/* 深色主題（參考 macOS 系統色） */
static const unsigned int dark_fg = 0xe6e6e6, dark_bg = 0x1e1e1e, dark_cursor = 0xd0d0d0, dark_sel = 0x3e5f8f;
static const unsigned int dark_pal[16] = {
	0x2b2b2b, 0xff6159, 0x3ec95b, 0xf5c242, 0x3e8df7, 0xc06ae6, 0x5ac8e0, 0xd8d8d8,
	0x7f7f84, 0xff8079, 0x6fdc84, 0xffd866, 0x6aaaff, 0xd796f0, 0x86dcf0, 0xffffff,
};

/* 淺色主題（類似 macOS 終端機「基本」） */
static const unsigned int light_fg = 0x1d1d1f, light_bg = 0xffffff, light_cursor = 0x6e6e73, light_sel = 0xb3d7ff;
static const unsigned int light_pal[16] = {
	0x000000, 0xc91b00, 0x00a600, 0xa5a200, 0x0225c7, 0xb000b0, 0x00a5b2, 0xbfbfbf,
	0x686868, 0xff6e67, 0x2bc93a, 0xc7c400, 0x6871ff, 0xff77ff, 0x22c8d3, 0xffffff,
};
