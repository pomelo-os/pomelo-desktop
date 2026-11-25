/* 測試用：ext-idle-notify 探針。idleprobe 毫秒 秒數：每有 idle／resume 就印一行，跑到秒數結束（用來確認程式的 idle inhibit 有效） */
#include <pomelo-wl.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	long ms = argc > 1 ? atol(argv[1]) : 1500, secs = argc > 2 ? atol(argv[2]) : 5;
	if (!pmwl_connect() || !pmwl_can_idle()) { puts("no idle-notify"); return 1; }
	pmwl_idle_watch(ms);
	long end = pm_now_ms() + secs * 1000;
	PmwEvent e;
	while (pm_now_ms() < end) {
		if (pmwl_next(&e, 200) && e.type == PMW_IDLE) { printf("%s\n", e.x ? "IDLE" : "RESUMED"); fflush(stdout); }
	}
	puts("done");
	return 0;
}
