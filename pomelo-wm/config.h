/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pomelo 視窗管理器設定檔 — 修改後執行 make 重新編譯
 * （選單列與 Dock 的設定在 panel-config.h） */

/* 主修飾鍵：Mod4Mask = Super(Win) 鍵，相當於 Mac 的 ⌘ */
#define MODKEY Mod4Mask

/* 外觀（預設值；pomelo-theme 改的設定存在 ~/.config/pomelo/settings.conf，會優先使用） */
static const char  *font_family = "Noto Sans CJK TC";
static const double font_size   = 13.0;
static const int    dark_mode   = 0;     /* 1 = 深色模式 */
static const int    title_h     = 28;    /* 標題列高度 */
static const int    border_w    = 1;     /* 視窗外框 */
static const int    corner_r    = 10;    /* 圓角半徑 */
static const int    wallpaper   = 1;     /* 1 = 繪製漸層桌布；0 = 交給 feh 等工具 */
static const char  *terminal    = "pomelo-terminal";
static const char  *terminal_exec = "pomelo-terminal -e";  /* 執行需要終端機的程式時使用 */
static const char  *icon_theme  = "Yaru";  /* 優先使用的圖示主題 */

/* 快捷鍵 */
static const Key keys[] = {
	/* 修飾鍵                  按鍵        功能          參數 */
	{ MODKEY,                 XK_Return,  spawn,        {.s = NULL} }, /* NULL = terminal */
	{ MODKEY,                 XK_space,   spotlight,    {0} },
	{ MODKEY|ShiftMask,       XK_space,   spawn,        {.s = "pomelo-assistant"} },   /* 小幫手 */
	{ MODKEY|ShiftMask,       XK_v,       spawn,        {.s = "pomelo-assistant --listen"} },   /* 跟小幫手用說的 */
	{ MODKEY,                 XK_d,       spotlight,    {0} },
	{ MODKEY,                 XK_q,       killclient,   {0} },
	{ MODKEY,                 XK_w,       killclient,   {0} },
	{ MODKEY,                 XK_m,       minimize,     {0} },
	{ MODKEY,                 XK_h,       minimize,     {0} },
	{ MODKEY,                 XK_f,       zoom,         {0} },
	{ MODKEY,                 XK_Up,      zoom,         {0} },
	{ MODKEY,                 XK_Down,    unzoom,       {0} },
	{ MODKEY,                 XK_Left,    tile,         {.i = -1} },
	{ MODKEY,                 XK_Right,   tile,         {.i = +1} },
	{ MODKEY|ControlMask,     XK_f,       togglefs,     {0} },
	{ Mod1Mask,               XK_Tab,     cycle,        {0} },
	{ MODKEY,                 XK_Tab,     overview,     {0} },
	{ MODKEY,                 XK_e,       overview,     {0} },
	{ MODKEY,                 XK_a,       spawn,        {.s = "pomelo-launcher"} },   /* 啟動台 */
	{ 0,                      XF86XK_LaunchA, spawn,    {.s = "pomelo-launcher"} },
	{ MODKEY,                 XK_l,       spawn,        {.s = "pomelo-lock"} },
	{ MODKEY|ControlMask,     XK_q,       spawn,        {.s = "pomelo-lock"} },   /* 跟 Mac 一樣 */
	{ 0,                      XF86XK_ScreenSaver, spawn, {.s = "pomelo-lock"} },
	/* 螢幕截圖與錄影（跟 Mac 一樣；加 Ctrl = 只拷貝到剪貼簿） */
	{ MODKEY|ShiftMask,             XK_3,      spawn, {.s = "pomelo-screenshot --full"} },
	{ MODKEY|ShiftMask,             XK_4,      spawn, {.s = "pomelo-screenshot --area"} },
	{ MODKEY|ShiftMask,             XK_5,      spawn, {.s = "pomelo-screenshot"} },
	{ MODKEY|ControlMask|ShiftMask, XK_3,      spawn, {.s = "pomelo-screenshot --full --clipboard"} },
	{ MODKEY|ControlMask|ShiftMask, XK_4,      spawn, {.s = "pomelo-screenshot --area --clipboard"} },
	{ 0,                            XK_Print,  spawn, {.s = "pomelo-screenshot --full"} },
	{ ShiftMask,                    XK_Print,  spawn, {.s = "pomelo-screenshot --area"} },
	{ Mod1Mask,                     XK_Print,  spawn, {.s = "pomelo-screenshot --window"} },
	{ ControlMask,                  XK_Print,  spawn, {.s = "pomelo-screenshot --full --clipboard"} },
	{ MODKEY|ControlMask,           XK_Escape, spawn, {.s = "pomelo-screenshot --stop"} },   /* 停止錄影 */
	{ MODKEY|ShiftMask,       XK_r,       restart,      {0} },
	{ MODKEY|ShiftMask,       XK_q,       quit,         {0} },
	/* 鍵盤上的音量鍵 */
	{ 0, XF86XK_AudioRaiseVolume, spawn, {.s = "pactl set-sink-mute @DEFAULT_SINK@ 0; pactl set-sink-volume @DEFAULT_SINK@ +5%"} },
	{ 0, XF86XK_AudioLowerVolume, spawn, {.s = "pactl set-sink-volume @DEFAULT_SINK@ -5%"} },
	{ 0, XF86XK_AudioMute,        spawn, {.s = "pactl set-sink-mute @DEFAULT_SINK@ toggle"} },
	{ 0, XF86XK_AudioMicMute,     spawn, {.s = "pactl set-source-mute @DEFAULT_SOURCE@ toggle"} },
	/* 媒體鍵：控制 Pomelo 影音播放器（沒有在播就不理） */
	{ 0, XF86XK_AudioPlay,        spawn, {.s = "pomelo-player --toggle"} },
	{ 0, XF86XK_AudioPause,       spawn, {.s = "pomelo-player --pause"} },
	{ 0, XF86XK_AudioStop,        spawn, {.s = "pomelo-player --stop"} },
	{ 0, XF86XK_AudioNext,        spawn, {.s = "pomelo-player --next"} },
	{ 0, XF86XK_AudioPrev,        spawn, {.s = "pomelo-player --prev"} },
	{ 0, XF86XK_AudioMedia,       spawn, {.s = "pomelo-player"} },
	{ 0, XF86XK_Calculator,       spawn, {.s = "pomelo-calc"} },
};

/* 在標題列按右鍵時出現的選單 */
static const MenuItem app_menu[] = {
	{ N_("縮到最小"),      minimize,   {0} },
	{ N_("縮放"),          zoom,       {0} },
	{ N_("全螢幕"),        togglefs,   {0} },
	{ NULL,            NULL,       {0} },
	{ N_("關閉視窗"),      killclient, {0} },
};
