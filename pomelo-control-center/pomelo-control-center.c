/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-control-center — Pomelo 控制中心
 *
 * 從選單列右上角彈出的快速設定：Wi-Fi、深色模式、Dock 自動隱藏、
 * 音量、螢幕亮度，以及開啟完整設定程式的捷徑。點外面或按 Esc 關閉。
 *
 * 用法：pomelo-control-center [--at 右邊界X 上緣Y] [--sound]
 *   --sound：只顯示聲音（點選單列的喇叭圖示時用）
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <dirent.h>

static int wifi = -1, bt = -1, dark, dock_hide, dnd, mute, have_pactl, have_bl, sound_only;

/* 輸出裝置（--sound 模式用） */
static struct { char name[256], desc[256]; } sinks[16];
static int nsinks;
static char def_sink[256];

static void read_sinks(void)
{
	nsinks = 0;
	char *t = pm_run("pactl list sinks");
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save)) {
		while (*l == '\t' || *l == ' ') l++;
		if (!strncmp(l, "Name: ", 6) && nsinks < (int)LEN(sinks)) {
			snprintf(sinks[nsinks].name, sizeof sinks[0].name, "%s", l + 6);
			snprintf(sinks[nsinks].desc, sizeof sinks[0].desc, "%s", l + 6);
			nsinks++;
		} else if (!strncmp(l, "Description: ", 13) && nsinks) {
			snprintf(sinks[nsinks - 1].desc, sizeof sinks[0].desc, "%s", l + 13);
		}
	}
	free(t);
	t = pm_run("pactl get-default-sink");
	snprintf(def_sink, sizeof def_sink, "%s", t ? t : "");
	def_sink[strcspn(def_sink, "\n")] = 0;
	free(t);
}
static double volume, bright;
static char ssid[128], sink[256];
static char bl_path[PATH_MAX];
static int bl_max;
static long last_vol;

static void read_state(void)
{
	dark = pm_conf_int("appearance.dark", 0);
	dock_hide = pm_conf_int("dock.autohide", 0);
	dnd = pm_conf_int("notify.dnd", 0);
	if (pm_have("nmcli")) {
		char *t = pm_run("nmcli radio wifi");
		/* 有無線網卡才顯示 Wi-Fi 開關 */
		char *d = pm_run("nmcli -t -f TYPE device");
		wifi = d && strstr(d, "wifi\n") ? (t && !strncmp(t, "enabled", 7)) : -1;
		free(t);
		free(d);
		t = pm_run("nmcli -t -f ACTIVE,SSID device wifi list --rescan no");
		char *save = NULL;
		for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l; l = strtok_r(NULL, "\n", &save))
			if (!strncmp(l, "yes:", 4)) snprintf(ssid, sizeof ssid, "%s", l + 4);
		free(t);
	}
	if (pm_have("bluetoothctl")) {
		char *t = pm_run("bluetoothctl show");
		bt = t && strstr(t, "Controller ") ? (strstr(t, "Powered: yes") != NULL) : -1;
		free(t);
	}
	have_pactl = pm_have("pactl");
	if (have_pactl) {
		char *t = pm_run("pactl get-default-sink");
		snprintf(sink, sizeof sink, "%s", t ? t : "");
		sink[strcspn(sink, "\n")] = 0;
		free(t);
		t = pm_run("pactl get-sink-volume @DEFAULT_SINK@");
		char *p = t ? strchr(t, '%') : NULL;
		if (p) { while (p > t && p[-1] >= '0' && p[-1] <= '9') p--; volume = atoi(p); }
		free(t);
		t = pm_run("pactl get-sink-mute @DEFAULT_SINK@");
		mute = t && strstr(t, "yes");
		free(t);
	}
	/* 螢幕亮度：/sys/class/backlight（寫入需要權限，使用 brightnessctl） */
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		snprintf(bl_path, sizeof bl_path, "/sys/class/backlight/%s", e->d_name);
		char p[PATH_MAX + 32];
		snprintf(p, sizeof p, "%s/max_brightness", bl_path);
		FILE *f = fopen(p, "r");
		if (f) { if (fscanf(f, "%d", &bl_max) != 1) bl_max = 0; fclose(f); }
		snprintf(p, sizeof p, "%s/brightness", bl_path);
		f = fopen(p, "r");
		int cur = 0;
		if (f) { if (fscanf(f, "%d", &cur) != 1) cur = 0; fclose(f); }
		if (bl_max > 0) { bright = 100.0 * cur / bl_max; have_bl = pm_have("brightnessctl"); }
		break;
	}
	if (d) closedir(d);
}

static void open_app(const char *cmd)
{
	pm_spawn(cmd);
	exit(0);
}

static void build_sound(PmUI *u)
{
	char buf[320];
	pm_ui_group(u, _("聲音"));
	if (!have_pactl) {
		pm_ui_info(u, _("需要 pactl（pulseaudio-utils）"), NULL);
	} else {
		if (pm_ui_slider(u, NULL, &volume, 0, 100) && pm_now_ms() - last_vol > 80) {
			snprintf(buf, sizeof buf, "pactl set-sink-volume @DEFAULT_SINK@ %d%%", (int)(volume + 0.5));
			pm_spawn(buf);
			last_vol = pm_now_ms();
		}
		int on = !mute;
		if (pm_ui_switch(u, _("聲音"), &on)) {
			mute = !on;
			pm_spawn(mute ? "pactl set-sink-mute @DEFAULT_SINK@ 1" : "pactl set-sink-mute @DEFAULT_SINK@ 0");
		}
	}
	pm_ui_end(u);
	if (nsinks > 1) {
		pm_ui_group(u, _("輸出裝置"));
		for (int i = 0; i < nsinks; i++)
			if (pm_ui_item(u, NULL, sinks[i].desc, NULL, NULL, !strcmp(sinks[i].name, def_sink)) && !strchr(sinks[i].name, '\'')) {
				snprintf(def_sink, sizeof def_sink, "%s", sinks[i].name);
				snprintf(buf, sizeof buf, "pactl set-default-sink '%s'", def_sink);
				pm_spawn(buf);
			}
		pm_ui_end(u);
	}
	pm_ui_group(u, NULL);
	if (pm_ui_item(u, NULL, _("音效設定…"), NULL, NULL, 0)) open_app("pomelo-sound");
	pm_ui_end(u);
}

static void build(PmUI *u)
{
	char buf[64];
	if (sound_only) { build_sound(u); return; }
	pm_ui_group(u, NULL);
	if (wifi >= 0 && pm_ui_switch(u, ssid[0] && wifi ? ssid : "Wi-Fi", &wifi))
		pm_spawn(wifi ? "nmcli radio wifi on" : "nmcli radio wifi off");
	if (bt >= 0 && pm_ui_switch(u, _("藍牙"), &bt))
		pm_spawn(bt ? "bluetoothctl power on" : "bluetoothctl power off");
	if (pm_ui_switch(u, _("深色模式"), &dark)) pm_conf_set_int("appearance.dark", dark);
	if (pm_ui_switch(u, _("勿擾模式"), &dnd)) pm_conf_set_int("notify.dnd", dnd);
	if (pm_ui_switch(u, _("自動隱藏 Dock"), &dock_hide)) pm_conf_set_int("dock.autohide", dock_hide);
	pm_ui_end(u);

	if (have_pactl) {
		pm_ui_group(u, _("聲音"));
		if (pm_ui_slider(u, NULL, &volume, 0, 100) && pm_now_ms() - last_vol > 80) {
			snprintf(buf, sizeof buf, "pactl set-sink-volume @DEFAULT_SINK@ %d%%", (int)(volume + 0.5));
			pm_spawn(buf);
			last_vol = pm_now_ms();
		}
		int on = !mute;
		if (pm_ui_switch(u, _("聲音"), &on)) {
			mute = !on;
			pm_spawn(mute ? "pactl set-sink-mute @DEFAULT_SINK@ 1" : "pactl set-sink-mute @DEFAULT_SINK@ 0");
		}
		pm_ui_end(u);
	}
	if (have_bl) {
		pm_ui_group(u, _("顯示器"));
		if (pm_ui_slider(u, NULL, &bright, 5, 100)) {
			snprintf(buf, sizeof buf, "brightnessctl -q set %d%%", (int)(bright + 0.5));
			pm_spawn(buf);
		}
		pm_ui_end(u);
	}

	pm_ui_group(u, NULL);
	if (pm_ui_item(u, NULL, _("鎖定螢幕"), NULL, NULL, 0)) open_app("pomelo-lock");
	if (pm_ui_item(u, NULL, _("網路設定…"), NULL, NULL, 0)) open_app("pomelo-network");
	if (pm_ui_item(u, NULL, _("藍牙設定…"), NULL, NULL, 0)) open_app("pomelo-bluetooth");
	if (pm_ui_item(u, NULL, _("顯示器設定…"), NULL, NULL, 0)) open_app("pomelo-display");
	if (pm_ui_item(u, NULL, _("音效設定…"), NULL, NULL, 0)) open_app("pomelo-sound");
	if (pm_ui_item(u, NULL, _("外觀設定…"), NULL, NULL, 0)) open_app("pomelo-theme");
	if (pm_ui_item(u, NULL, _("系統設定…"), NULL, NULL, 0)) open_app("pomelo-system");
	pm_ui_end(u);
}

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_set_source_rgb(cr, 0.22, 0.22, 0.25);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 7);
	pm_rrect(cr, 30, 36, 68, 24, 12, 12);
	cairo_stroke(cr);
	pm_rrect(cr, 30, 70, 68, 24, 12, 12);
	cairo_stroke(cr);
	cairo_arc(cr, 86, 48, 8, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_arc(cr, 42, 82, 8, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	int right = -1, top = 32;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--at") && i + 2 < argc) { right = atoi(argv[i + 1]); top = atoi(argv[i + 2]); i += 2; }
		else if (!strcmp(argv[i], "--sound")) sound_only = 1;
	}
	pm_conf_load();
	read_state();
	if (sound_only && have_pactl) read_sinks();
	if (right < 0) {
		/* 沒指定位置：放在螢幕右上角 */
		Display *d = XOpenDisplay(NULL);
		if (d) { right = DisplayWidth(d, DefaultScreen(d)) - 8; XCloseDisplay(d); }
		else right = 800;
	}
	pm_ui_popover(right, top);
	pm_ui_native = 1;   /* Wayland 工作階段：layer-shell 的 overlay（沒有 layer-shell 時照舊走 X11） */
	return pm_ui_run(_("控制中心"), "pomelo-control-center", sound_only ? 300 : 340, 300, build, NULL, 0, icon);
}
