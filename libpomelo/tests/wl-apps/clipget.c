/* 測試工具：開一個小視窗取得焦點，印出剪貼簿的格式與文字（或把某格式存檔）。
 * 用法：clipget [mime outfile] / clipget --set 文字 / clipget --primary（印 primary selection）。
 * 編譯：cc -o clipget clipget.c -I<libpomelo> -L<libpomelo> -lpomelo $(pkg-config --cflags --libs cairo x11) */
#include <pomelo-wl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	if (!pmwl_connect()) { puts("no wayland"); return 1; }
	PmWl *w = pmwl_window("clipget", "clipget", 200, 100, 0, 0, NULL);
	PmwEvent e; int focused = 0;
	cairo_t *cr = pmwl_begin(w, NULL, NULL); cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr); pmwl_end(w, cr);
	for (int i = 0; i < 40 && !focused; i++) if (pmwl_next(&e, 100) && e.type == PMW_FOCUS_IN) focused = 1;
	for (int i = 0; i < 5; i++) pmwl_next(&e, 100);
	if (argc > 2 && !strcmp(argv[1], "--set")) {
		/* 要先有按鍵或按滑鼠事件才能設剪貼簿：等一個 */
		puts("press a key"); fflush(stdout);
		for (int i = 0; i < 100; i++) if (pmwl_next(&e, 100) && (e.type == PMW_KEY || e.type == PMW_DOWN)) break;
		pmwl_clip_set(argv[2]); for (int i = 0; i < 50; i++) pmwl_next(&e, 100);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--primary")) { char *s = pmwl_primary_get(); printf("primary=[%s]\n", s ? s : "(null)"); return 0; }
	const char *t[32]; int n = pmwl_clip_types(t, 32);
	printf("focused=%d types=%d:", focused, n); for (int i = 0; i < n; i++) printf(" %s", t[i]); puts("");
	if (argc > 2) {
		size_t len; char *d = pmwl_clip_get_type(argv[1], &len);
		if (d) { FILE *f = fopen(argv[2], "wb"); fwrite(d, 1, len, f); fclose(f); printf("saved %zu bytes\n", len); }
		else puts("no data");
	} else { char *s = pmwl_clip_get(); printf("text=[%s]\n", s ? s : "(null)"); }
	return 0;
}
