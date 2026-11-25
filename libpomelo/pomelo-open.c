/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-open — 用預設的程式開啟檔案、資料夾或網址（跟 xdg-open 一樣的用法，見 open.c） */
#include "pomelo.h"

int main(int argc, char *argv[])
{
	if (argc != 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
		fprintf(stderr, "用法：pomelo-open 檔案或網址\n");
		return argc == 2 ? 0 : 1;
	}
	if (!strcmp(argv[1], "--version")) { puts("pomelo-open " POMELO_VERSION); return 0; }
	if (pm_open(argv[1])) {
		fprintf(stderr, "pomelo-open：找不到可以開啟 %s 的程式\n", argv[1]);
		return 3;   /* 跟 xdg-open 一樣：3 = 找不到程式 */
	}
	return 0;
}
