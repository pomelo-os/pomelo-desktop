/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-archive — Pomelo 壓縮管理員：壓縮、解壓縮、加密（zip 7z tar gz xz zst cpio iso squashfs img …）
 *
 *   pomelo-archive [檔案]                     瀏覽壓縮檔
 *   pomelo-archive --extract-here 檔案…       解壓到壓縮檔旁邊（有進度視窗）
 *   pomelo-archive --extract-to 檔案 資料夾    解壓到指定資料夾
 *   pomelo-archive --compress 檔案或資料夾…     建立壓縮檔（選格式、等級、密碼）
 *   無介面：--list --test --extract --create --add --formats（見 cli.c）
 * 後端：libarchive、外部的 7-Zip／gpg（見 ac.h）。 */
#define _GNU_SOURCE
#include "gui.h"
#include "cli.h"

int main(int argc, char **argv)
{
	ac_init_locale();
	int r = cli_main(argc, argv);
	if (r >= 0) return r;
	if (argc > 2 && !strcmp(argv[1], "--icon")) {
		cairo_surface_t *s = pa_icon(256);
		int ok = cairo_surface_write_to_png(s, argv[2]) == CAIRO_STATUS_SUCCESS;
		cairo_surface_destroy(s);
		return !ok;
	}
	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		puts(_("用法：pomelo-archive [壓縮檔]\n"
		       "      pomelo-archive --extract-here 檔案…\n"
		       "      pomelo-archive --extract-to 檔案 資料夾\n"
		       "      pomelo-archive --compress 檔案或資料夾…\n"
		       "      pomelo-archive --formats | --list | --test | --extract | --create | --add （命令列，見 README）"));
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "--ui-password")) return gui_password(argc, argv);
	if (argc > 2 && (!strcmp(argv[1], "--extract-here") || !strcmp(argv[1], "--extract-to"))) return gui_extract(argc, argv);
	if (argc > 2 && !strcmp(argv[1], "--compress")) return gui_compress(argc, argv);
	if (argc > 2 && !strcmp(argv[1], "--add-gui")) return gui_add(argc, argv);
	return gui_browse(argc, argv);
}
